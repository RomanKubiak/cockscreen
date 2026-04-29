#pragma once

#ifndef _WIN32

#include <QString>
#include <QVideoSink>

#include <atomic>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <string>

// Forward-declare GLib type to avoid pulling glib headers into every TU.
typedef struct _GMainLoop GMainLoop;

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
    // sink must remain valid for the lifetime of AppsinkCapture.
    [[nodiscard]] bool start(int udp_port, QVideoSink *sink);

    void stop();

    [[nodiscard]] bool is_running() const;
    [[nodiscard]] const QString &status_message() const;

  private:
    pthread_t thread_{};
    bool thread_started_{false};
    std::atomic<bool> running_{false};
    QString status_message_;
    GMainLoop *loop_{nullptr};
    std::unique_ptr<std::mutex> loop_mutex_;
};

} // namespace cockscreen::runtime

#endif // _WIN32
