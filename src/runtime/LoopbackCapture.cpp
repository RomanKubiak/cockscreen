#include "cockscreen/runtime/LoopbackCapture.hpp"

#ifndef _WIN32

#include "cockscreen/runtime/V4l2Capture.hpp"

#include <QSize>
#include <QVideoFrame>
#include <QVideoFrameFormat>

#include <algorithm>
#include <cstring>

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace cockscreen::runtime
{

LoopbackCapture::~LoopbackCapture()
{
    stop();
}

bool LoopbackCapture::start(const std::string &device_path, QVideoSink *sink)
{
    stop();

    // Query the format the GStreamer receiver has already negotiated so we can
    // open V4l2Capture with the exact dimensions — v4l2loopback may reject
    // VIDIOC_S_FMT requests that differ from what the writer set.
    int actual_width = 0;
    int actual_height = 0;
    {
        const int probe_fd = ::open(device_path.c_str(), O_RDONLY | O_NONBLOCK);
        if (probe_fd >= 0)
        {
            v4l2_format fmt{};
            fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            if (::ioctl(probe_fd, VIDIOC_G_FMT, &fmt) == 0)
            {
                actual_width = static_cast<int>(fmt.fmt.pix.width);
                actual_height = static_cast<int>(fmt.fmt.pix.height);
            }
            ::close(probe_fd);
        }
    }

    running_ = true;

    thread_ = QThread::create([this, device_path, sink, actual_width, actual_height]() {
        V4l2Capture capture;
        if (!capture.open(device_path, actual_width, actual_height))
        {
            status_message_ = QString::fromStdString(capture.error_message());
            running_ = false;
            return;
        }

        if (!capture.start())
        {
            status_message_ = QString::fromStdString(capture.error_message());
            running_ = false;
            return;
        }

        const int width = capture.width();
        const int height = capture.height();

        // Map our V4l2PixelFormat to QVideoFrameFormat. We only handle YUYV/UYVY
        // because the GStreamer receiver pipeline forces YUY2 output. RGB24/BGR24
        // have no direct QVideoFrameFormat equivalent in Qt6 and are not emitted
        // by the loopback pipeline.
        QVideoFrameFormat::PixelFormat qt_fmt = QVideoFrameFormat::Format_Invalid;
        switch (capture.pixel_format())
        {
        case V4l2PixelFormat::yuyv:
            qt_fmt = QVideoFrameFormat::Format_YUYV;
            break;
        case V4l2PixelFormat::uyvy:
            qt_fmt = QVideoFrameFormat::Format_UYVY;
            break;
        default:
            status_message_ = QStringLiteral("LoopbackCapture: unsupported pixel format from V4L2 device");
            running_ = false;
            return;
        }

        const QVideoFrameFormat frame_format(QSize(width, height), qt_fmt);

        while (running_)
        {
            auto frame_view = capture.dequeue();
            if (!frame_view.has_value())
            {
                // Non-blocking: no frame ready yet, yield briefly.
                QThread::usleep(1000);
                continue;
            }

            // Create a system-memory QVideoFrame and copy the MMAP data into it.
            // QVideoSink::setVideoFrame() is thread-safe; the videoFrameChanged
            // signal will be queued to ShaderVideoWindow's handle_playback_frame
            // on the main thread.
            QVideoFrame video_frame(frame_format);
            if (video_frame.map(QVideoFrame::WriteOnly))
            {
                const auto bytes_to_copy =
                    std::min(frame_view->size,
                             static_cast<std::size_t>(video_frame.mappedBytes(0)));
                std::memcpy(video_frame.bits(0), frame_view->data, bytes_to_copy);
                video_frame.unmap();
                sink->setVideoFrame(video_frame);
            }

            capture.release();
        }
    });

    thread_->start();
    return true;
}

void LoopbackCapture::stop()
{
    running_ = false;
    if (thread_ != nullptr)
    {
        thread_->wait();
        delete thread_;
        thread_ = nullptr;
    }
}

bool LoopbackCapture::is_running() const
{
    return running_.load();
}

const QString &LoopbackCapture::status_message() const
{
    return status_message_;
}

} // namespace cockscreen::runtime

#endif // _WIN32
