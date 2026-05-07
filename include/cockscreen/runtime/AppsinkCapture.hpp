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
// Runs a GStreamer pipeline on a POSIX thread:
//   udpsrc → rtpjitterbuffer → rtph264depay → h264parse → avdec_h264
//     → videoconvert → BGRx → appsink
//
// Each decoded frame is pushed directly to the supplied QVideoSink via
// QVideoFrame, bypassing v4l2loopback entirely.
// ---------------------------------------------------------------------------
class AppsinkCapture
{
  public:
    AppsinkCapture() = default;
    ~AppsinkCapture();

    AppsinkCapture(const AppsinkCapture &) = delete;
    AppsinkCapture &operator=(const AppsinkCapture &) = delete;

    // Start the GStreamer appsink pipeline listening on udp_port.
    // use_h264: true → expect H.264 RTP (pt=96); false → expect MJPEG RTP (pt=26).
    // sink must remain valid for the lifetime of AppsinkCapture.
    [[nodiscard]] bool start(int udp_port, QVideoSink *sink, bool use_h264 = true,
                             bool verbose_debug = false);

    void stop();

    [[nodiscard]] bool is_running() const;
    [[nodiscard]] const QString &status_message() const;

  private:
    pthread_t thread_{};
    bool thread_started_{false};
    std::atomic<bool> running_{false};
    QString status_message_;
    bool verbose_debug_{false};
};

} // namespace cockscreen::runtime

#endif // _WIN32
