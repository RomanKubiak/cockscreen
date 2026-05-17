# Pi Hardware ISP Pipeline — DONE (commit 0da5cb9)

~~Replace CPU Bayer debayer with the bcm2835-isp hardware ISP: Bayer->YUYV in
dedicated hardware, zero CPU involvement in pixel conversion.~~

Implemented and working. Camera image confirmed on Pi display.

## Remaining open questions / follow-up

## Why

rpicam-hello runs realtime at 1296x972 because libcamera routes unicam raw
frames through the hardware ISP (/dev/media1). Cockscreen debayers on A53 cores;
even with the tile-NN optimisation this is the bottleneck for shader layers.

## Device topology (Pi Zero 2W, kernel 6.12, confirmed on device)

```
unicam  /dev/video2  /dev/media0
  | raw Bayer GB10 (SGBRG10 unpacked, 2B/px) - what our V4l2Capture produces
  | V4L2_CAP_VIDEO_CAPTURE
  |
  v  (app reads frame, queues pointer to ISP - USERPTR = zero copy)
bcm2835-isp input   /dev/video13  (bcm2835-isp0-output0)
  V4L2_CAP_VIDEO_OUTPUT
  Accepts GB10 (SGBRG10 unpacked) confirmed in VIDIOC_ENUM_FMT output
  All /dev/media1 links IMMUTABLE - no media-ctl setup needed
  |
  v  (hardware ISP: debayer + colour matrix, no CPU)
bcm2835-isp output  /dev/video14  (bcm2835-isp0-capture1)
  V4L2_CAP_VIDEO_CAPTURE
  Outputs YUYV, UYVY, RGB24, BGR24, NV12 etc.
  -> cockscreen already handles YUYV in the existing shader path
```

Not needed now: /dev/video15 (low-res), /dev/video16 (stats).

## Buffer strategy

ISP input  /dev/video13: V4L2_MEMORY_USERPTR
  Pass unicam mmap buffer pointers directly - zero copy.
  Fallback: V4L2_MEMORY_MMAP + memcpy (still cheaper than CPU debayer).

ISP output /dev/video14: V4L2_MEMORY_MMAP
  Standard mmap capture queue.

## New class: IspPipeline

Files:
  include/cockscreen/runtime/v4l2/IspPipeline.hpp
  src/runtime/v4l2/IspPipeline.cpp

Interface sketch:
  static bool is_available()
  bool open(int w, int h, uint32_t in_fourcc, uint32_t out_fourcc, string *err)
  bool start()
  bool queue_input_frame(const void *data, size_t size, int stride)
  optional<V4l2FrameView> dequeue_output()
  void release_output()
  void stop() / close()
  V4l2PixelFormat output_pixel_format() / output_width() / output_height()

Device discovery: scan /dev/video* for V4L2_CAP_VIDEO_OUTPUT with driver
"bcm2835-isp" for input node, V4L2_CAP_VIDEO_CAPTURE with same driver for
output node. Fall back to /dev/video13 and /dev/video14 as first attempt.

ISP input setup (V4L2_BUF_TYPE_VIDEO_OUTPUT):
  VIDIOC_S_FMT: width/height, pixelformat=GB10, colorspace=RAW
  VIDIOC_REQBUFS: memory=USERPTR, count=4
  VIDIOC_STREAMON

ISP output setup (V4L2_BUF_TYPE_VIDEO_CAPTURE):
  VIDIOC_S_FMT: width/height, pixelformat=YUYV
  VIDIOC_REQBUFS: memory=MMAP, count=4, mmap+queue all buffers
  VIDIOC_STREAMON

Per-frame loop - queue output buffer BEFORE input (ISP starts when both ready):
  unicam DQBUF -> raw GB10 frame
    ISP output QBUF (pre-queued on startup, recycle after each frame)
    ISP input  QBUF (USERPTR = unicam mmap ptr)
    ISP output DQBUF -> YUYV frame
    existing YUYV texture upload (no CPU debayer)
    unicam QBUF (release)

## Integration points

Lifecycle.cpp - after raw_video_capture_.open() succeeds with Bayer format:
  if (IspPipeline::is_available())
    isp_pipeline_.open(w, h, V4L2_PIX_FMT_SGBRG10, V4L2_PIX_FMT_YUYV)
    isp_pipeline_.start()
    use_isp_ = true

Assets.cpp frame loop:
  if (use_isp_): unicam dequeue -> queue to ISP -> YUYV path (skip debayer)
  else:          existing tile-NN CPU debayer path (unchanged)

Files to modify:
  CMakeLists.txt                              add IspPipeline.cpp
  src/runtime/shadervideo/Lifecycle.cpp       try ISP after raw capture opens
  src/runtime/shadervideo/Assets.cpp          branch on use_isp_ flag
  include/cockscreen/runtime/V4l2Capture.hpp  check if pixel_format() already public

## Testing

Step 1 (do first, before touching cockscreen):
  Standalone C++ test on the Pi: open /dev/video2 + /dev/video13 + /dev/video14,
  run 30 frames, save one YUYV frame, inspect with:
    ffmpeg -f rawvideo -pix_fmt yuyv422 -s 640x480 -i out.raw out.png
  This validates USERPTR works and ISP accepts GB10.

Step 2: Integrate behind use_isp_ flag, auto-enabled on Pi when ISP detected.
Step 3: Measure CPU% with top vs tile-NN baseline.

## Open questions

Q1: Does bcm2835-isp accept USERPTR pointing at unicam CMA mmap regions?
    If not: fall back to MMAP input + memcpy (still net win over CPU debayer).

Q2: Does STREAMON on both fds need to happen before queuing, or just output?

Q3: Colour quality without libcamera IPA: ISP applies fixed default colour matrix.
    May look slightly off-white-balance but acceptable for cockpit use.
    Can tune later via V4L2 controls (colour gains) on /dev/video13.
