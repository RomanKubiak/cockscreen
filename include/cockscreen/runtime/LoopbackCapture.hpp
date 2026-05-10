#pragma once

#ifndef _WIN32

#include <QString>
#include <QThread>
#include <QVideoSink>

#include <atomic>
#include <string>

namespace cockscreen::runtime
{

// Reads frames from a V4L2 device using V4L2_MEMORY_MMAP on a background thread
// and pushes them to a QVideoSink via QVideoFrame.
//
// This exists because Qt6's FFmpeg multimedia backend tries V4L2_MEMORY_USERPTR
// (and falls back to it when MMAP also fails) when using QCamera to open a
// v4l2loopback device. v4l2loopback only supports V4L2_MEMORY_MMAP, so QCamera
// always fails. LoopbackCapture bypasses QCamera entirely and does the MMAP
// capture itself, exactly as DirectVideoWindow does.
class LoopbackCapture
{
public:
    LoopbackCapture() = default;
    ~LoopbackCapture();

    LoopbackCapture(const LoopbackCapture &) = delete;
    LoopbackCapture &operator=(const LoopbackCapture &) = delete;

    // Opens the V4L2 device and starts the capture thread. The device must
    // already have a format negotiated by the GStreamer receiver (i.e. call
    // LoopbackPipeline::wait_for_device_ready() first). sink must remain valid
    // for the lifetime of the LoopbackCapture.
    [[nodiscard]] bool start(const std::string &device_path, QVideoSink *sink,
                             bool verbose_debug = false);

    void stop();

    [[nodiscard]] bool is_running() const;
    [[nodiscard]] const QString &status_message() const;

private:
    QThread *thread_{nullptr};
    std::atomic<bool> running_{false};
    QString status_message_;
    bool verbose_debug_{false};
};

} // namespace cockscreen::runtime

#endif // _WIN32
