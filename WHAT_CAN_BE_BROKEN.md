# What Can Be Broken — IMX219 / CSI Camera Pipeline

Symptoms and root causes observed when switching from OV5647 to the official
Pi Camera v2 (IMX219) on Pi 3B+, kernel 6.12.

---

## 1. No signal at 640x480

**Symptom:** Scene shows "NO SIGNAL" text; no frames arrive.

**Cause:** The IMX219 sensor driver does not expose 640x480 as a valid capture
mode via unicam/Media Controller. `mc_setup_pipeline()` fails to set the sensor
subdev to that resolution; `VIDIOC_STREAMON` is never reached.

Supported modes on Pi Camera v2 via raw V4L2:
- 1280×720
- 1640×1232 (full-FOV binned)
- 1920×1080 (cropped)
- 3280×2464 (full native, very slow)

**Fix:** Use `"format": "1280x720"` (or higher) in the scene `inputs.video`
block.

**How to reproduce:** Set `"format": "640x480"` in the scene with `/dev/video2`
pointing at the IMX219. Status message will say "No raw video frames yet" and
then stay on "NO SIGNAL".

---

## 2. Green/blue columns, wrong colours (IMX219 Bayer mismatch in ISP)

**Symptom:** Image arrives but shows vertical columns of green/blue; scene
objects (e.g. a red LED) faintly visible with wrong hue. Image is not black —
data is flowing — but colour is grossly wrong.

**Cause (fixed in commit after 0da5cb9):** `IspPipeline::open()` in
`Lifecycle.cpp` was called with the hardcoded FOURCC `V4L2_PIX_FMT_SGBRG10`
(OV5647's GBRG Bayer pattern). The IMX219 negotiates `SRGGB10` (RGGB pattern).
The bcm2835-isp hardware accepts the wrong format declaration without error and
applies its colour matrix with mismatched channel assignments:

```
IMX219 pixel (0,0) = Red   → ISP told it is Green  → wrong output channel
IMX219 pixel (1,0) = Green → ISP told it is Blue   → wrong output channel
```

This produces a green-dominant YUYV output with blue artifacts and suppressed
red, appearing as regular vertical banding (2-pixel Bayer cell period) from the
misphased demosaicing.

**Fix:** `Lifecycle.cpp` now derives the FOURCC from the actual negotiated pixel
format via `v4l2::pixel_format_to_fourcc(raw_video_capture_.pixel_format())`
instead of the hardcoded constant.

**How to reproduce:** In `Lifecycle.cpp`, change:
```cpp
const std::uint32_t capture_fourcc =
    v4l2::pixel_format_to_fourcc(raw_video_capture_.pixel_format());
```
back to:
```cpp
const std::uint32_t capture_fourcc = V4L2_PIX_FMT_SGBRG10;
```
Rebuild and point at any coloured scene. Blue/green columns appear immediately.
OV5647 is unaffected (its pattern happens to be GBRG, matching the old
constant).

---

## 3. ISP output stride assumed, not read from driver (latent)

**Symptom:** Not currently observed. Could produce vertical column artifacts or
shifted image rows at non-standard resolutions where the driver pads
`bytesperline` beyond `width * 2`.

**Cause (fixed):** `IspPipeline::dequeue_output()` computed stride as
`output_width_ * 2` for YUYV instead of reading `bytesperline` from the
`VIDIOC_S_FMT` response stored in `out_fmt.fmt.pix.bytesperline`.

**Fix:** `output_stride_` is now saved from `out_fmt.fmt.pix.bytesperline`
after `VIDIOC_S_FMT` and used directly in `dequeue_output()`.

**How to reproduce:** Patch `IspPipeline::open()` to force a padded stride:
```cpp
output_stride_ = (output_width_ * 2) + 64;  // artificial padding
```
Any camera input will then show rows shifted left or right by 32 pixels,
accumulating across the image height.

---

---

## 4. NO SIGNAL despite unicam + ISP both running (pipelining race)

**Symptom:** "NO SIGNAL" / "No raw video frames yet" even though strace confirms
both unicam and bcm2835-isp are dequeuing frames with non-zero `bytesused`.
The bug disappears under strace (a classic heisenbug).

**Cause (fixed):** `upload_latest_frame()` called `isp_pipeline_.dequeue_output()`
immediately after `isp_pipeline_.queue_input()` on every iteration. With
`O_NONBLOCK` the ISP has not had time to process the just-submitted frame, so
`VIDIOC_DQBUF` returns `EAGAIN` and `dequeue_output()` returns `nullopt`. The
input ring fills (4 buffers × EAGAIN = all slots occupied after 4 frames);
`queue_input()` then returns false and no further frames are submitted. The
grace period expires and "NO SIGNAL" is shown.

Under strace the overhead of tracing every syscall adds enough wall time between
`VIDIOC_QBUF` and `VIDIOC_DQBUF` that the ISP finishes in that window, so the
bug is invisible.

**Fix:** In `upload_latest_frame()`, dequeue any ready ISP output **before**
submitting the new unicam frame. This introduces one frame of pipeline latency
but decouples consumption from submission — output from frame N is consumed on
the N+1 iteration, by which point the ISP has comfortably finished.

**How to reproduce:** In `Assets.cpp`, swap the order back so `queue_input()`
is called before `dequeue_output()`:
```cpp
const bool queued = isp_pipeline_.queue_input(...);
raw_video_capture_.release();
if (queued) {
    const auto isp_frame = isp_pipeline_.dequeue_output(); // always EAGAIN
    ...
}
```
Run without strace. OV5647 or IMX219 at any resolution will show "NO SIGNAL"
after the 4-second grace period.

---

## Summary table

| # | Symptom                        | Root cause                         | Fixed |
|---|--------------------------------|------------------------------------|-------|
| 1 | No signal at 640×480           | IMX219 does not support that mode  | N/A — use supported resolution |
| 2 | Green/blue columns             | ISP FOURCC hardcoded to OV5647 GBRG | Yes — Lifecycle.cpp |
| 3 | Potential row-shift artifacts  | ISP output stride not read from driver | Yes — IspPipeline.cpp |
| 4 | NO SIGNAL despite pipeline healthy | ISP dequeue immediately after queue → EAGAIN, ring starvation | Yes — Assets.cpp |
