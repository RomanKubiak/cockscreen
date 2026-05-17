#pragma once

#ifndef _WIN32

#include <QString>
#include <QVideoSink>

#include <atomic>
#include <pthread.h>
#include <string>

namespace cockscreen::runtime
{

// ---------------------------------------------------------------------------
// AppsinkCapture
//
// Runs a GStreamer pipeline on a POSIX thread and pushes decoded frames to
// a QVideoSink via QVideoFrame.
//
// Two source types are supported:
//
//   UDP loopback (RTP) — start(udp_port, sink, use_h264)
//     udpsrc → rtph264depay/rtpjpegdepay → avdec_h264/avdec_mjpeg
//       → videoconvert → BGRx → appsink
//
//   Network camera — start_network(url, sink)
//     RTSP (H.264):  rtspsrc → rtph264depay → h264parse → avdec_h264
//                      → videoconvert → BGRx → appsink
//     HTTP MJPEG:    souphttpsrc → multipartdemux → jpegdec
//                      → videoconvert → BGRx → appsink
// ---------------------------------------------------------------------------
class AppsinkCapture
{
  public:
    AppsinkCapture() = default;
    ~AppsinkCapture();

    AppsinkCapture(const AppsinkCapture &) = delete;
    AppsinkCapture &operator=(const AppsinkCapture &) = delete;

    // RTP-over-UDP loopback path (existing behaviour).
    // use_h264: true → H.264 RTP (pt=96); false → MJPEG RTP (pt=26).
    // sink must remain valid for the lifetime of AppsinkCapture.
    [[nodiscard]] bool start(int udp_port, QVideoSink *sink, bool use_h264 = true,
                             bool verbose_debug = false);

    // Network camera path.
    // url must be an rtsp://, http://, or https:// address.
    //   rtsp://  — H.264 stream via rtspsrc (covers most IP cameras).
    //   http://  — MJPEG stream via souphttpsrc + multipartdemux.
    //   https:// — same as http://, TLS.
    // sink must remain valid for the lifetime of AppsinkCapture.
    [[nodiscard]] bool start_network(const std::string &url, QVideoSink *sink,
                                     bool verbose_debug = false);

    void stop();

    [[nodiscard]] bool is_running() const;
    [[nodiscard]] const QString &status_message() const;

  private:
    [[nodiscard]] bool start_pipeline(std::string pipeline_str, QVideoSink *sink,
                                      bool verbose_debug);

    pthread_t thread_{};
    bool thread_started_{false};
    std::atomic<bool> running_{false};
    QString status_message_;
    bool verbose_debug_{false};
};

} // namespace cockscreen::runtime

#endif // _WIN32
