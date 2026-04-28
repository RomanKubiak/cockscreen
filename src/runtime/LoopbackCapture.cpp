#include "cockscreen/runtime/LoopbackCapture.hpp"

#ifndef _WIN32

#include <QSize>
#include <QVideoFrame>
#include <QVideoFrameFormat>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <vector>

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
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
    running_ = true;

    thread_ = QThread::create([this, device_path, sink]() {

        // --- 1. Open the device -----------------------------------------
        const int fd = ::open(device_path.c_str(), O_RDWR | O_NONBLOCK);
        if (fd < 0)
        {
            status_message_ = QStringLiteral("LoopbackCapture: failed to open '%1'")
                                  .arg(QString::fromStdString(device_path));
            running_ = false;
            return;
        }

        // --- 2. Read the format GStreamer already set — NO VIDIOC_S_FMT -----
        // Calling VIDIOC_S_FMT on a v4l2loopback device where a writer is
        // already active (exclusive_caps=0) returns EBUSY and confuses the
        // writer. We accept whatever format is already negotiated.
        v4l2_format fmt{};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (::ioctl(fd, VIDIOC_G_FMT, &fmt) != 0)
        {
            status_message_ = QStringLiteral("LoopbackCapture: VIDIOC_G_FMT failed");
            ::close(fd);
            running_ = false;
            return;
        }

        const int width  = static_cast<int>(fmt.fmt.pix.width);
        const int height = static_cast<int>(fmt.fmt.pix.height);
        const int stride = static_cast<int>(fmt.fmt.pix.bytesperline);
        const std::uint32_t fourcc = fmt.fmt.pix.pixelformat;

        // We only handle the two formats the receiver pipeline emits:
        //   V4L2_PIX_FMT_RGB24  → 3 bytes per pixel, R G B
        //   V4L2_PIX_FMT_YUYV   → 2 bytes per macro-pixel pair
        const bool is_rgb24 = (fourcc == V4L2_PIX_FMT_RGB24);
        const bool is_yuyv  = (fourcc == V4L2_PIX_FMT_YUYV);

        char cc[5] = {};
        std::memcpy(cc, &fourcc, 4);
        std::cerr << "[LoopbackCapture] opened " << device_path
                  << " " << width << "x" << height
                  << " stride=" << stride
                  << " fourcc=" << cc
                  << " is_rgb24=" << is_rgb24
                  << " is_yuyv=" << is_yuyv << "\n";

        if (!is_rgb24 && !is_yuyv)
        {
            char cc[5] = {};
            std::memcpy(cc, &fourcc, 4);
            status_message_ = QStringLiteral("LoopbackCapture: unsupported pixel format '%1'")
                                  .arg(QString::fromLatin1(cc));
            ::close(fd);
            running_ = false;
            return;
        }

        // --- 3. Allocate MMAP buffers ------------------------------------
        v4l2_requestbuffers req{};
        req.count  = 4;
        req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        if (::ioctl(fd, VIDIOC_REQBUFS, &req) != 0 || req.count == 0)
        {
            status_message_ = QStringLiteral("LoopbackCapture: VIDIOC_REQBUFS failed");
            ::close(fd);
            running_ = false;
            return;
        }

        struct Buf { void *ptr{nullptr}; std::size_t len{0}; };
        std::vector<Buf> bufs(req.count);
        for (unsigned i = 0; i < req.count; ++i)
        {
            v4l2_buffer qbuf{};
            qbuf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            qbuf.memory = V4L2_MEMORY_MMAP;
            qbuf.index  = i;
            if (::ioctl(fd, VIDIOC_QUERYBUF, &qbuf) != 0)
            {
                status_message_ = QStringLiteral("LoopbackCapture: VIDIOC_QUERYBUF failed");
                ::close(fd);
                running_ = false;
                return;
            }
            bufs[i].len = qbuf.length;
            bufs[i].ptr = ::mmap(nullptr, qbuf.length,
                                 PROT_READ | PROT_WRITE, MAP_SHARED,
                                 fd, qbuf.m.offset);
            if (bufs[i].ptr == MAP_FAILED)
            {
                bufs[i].ptr = nullptr;
                status_message_ = QStringLiteral("LoopbackCapture: mmap failed");
                ::close(fd);
                running_ = false;
                return;
            }
            if (::ioctl(fd, VIDIOC_QBUF, &qbuf) != 0)
            {
                status_message_ = QStringLiteral("LoopbackCapture: VIDIOC_QBUF priming failed");
                ::close(fd);
                running_ = false;
                return;
            }
        }

        // --- 4. Start streaming -----------------------------------------
        // With exclusive_caps=1, STREAMON on the capture side requires the
        // output side (v4l2sink) to have already called STREAMON.  That happens
        // when the GStreamer pipeline transitions to PLAYING, which is slightly
        // after the format is negotiated (PREROLLING).  Retry on EIO until the
        // writer is ready, or until we are asked to stop.
        int stream_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        constexpr int kStreamon_retries = 75;  // 75 × 100 ms = 7.5 s
        {
            int attempt = 0;
            while (running_)
            {
                if (::ioctl(fd, VIDIOC_STREAMON, &stream_type) == 0)
                    break;

                const int err = errno;
                if (err != EIO || attempt >= kStreamon_retries)
                {
                    std::cerr << "[LoopbackCapture] VIDIOC_STREAMON failed: "
                              << strerror(err) << "\n";
                    status_message_ = QStringLiteral("LoopbackCapture: VIDIOC_STREAMON failed");
                    for (auto &b : bufs) if (b.ptr) ::munmap(b.ptr, b.len);
                    ::close(fd);
                    running_ = false;
                    return;
                }
                ++attempt;
                std::cerr << "[LoopbackCapture] STREAMON not ready (EIO), retry "
                          << attempt << "/" << kStreamon_retries << "\n";
                ::usleep(100000);
            }
            if (!running_)
            {
                for (auto &b : bufs) if (b.ptr) ::munmap(b.ptr, b.len);
                ::close(fd);
                return;
            }
        }
        std::cerr << "[LoopbackCapture] STREAMON ok, entering capture loop\n";

        // QVideoFrameFormat — always use RGBA8888 so Qt6 handles it natively.
        const QVideoFrameFormat frame_format(
            QSize(width, height), QVideoFrameFormat::Format_RGBA8888);

        // --- 5. Capture loop --------------------------------------------
        int frame_count = 0;
        while (running_)
        {
            v4l2_buffer dqbuf{};
            dqbuf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            dqbuf.memory = V4L2_MEMORY_MMAP;

            if (::ioctl(fd, VIDIOC_DQBUF, &dqbuf) != 0)
            {
                if (errno == EAGAIN)
                {
                    QThread::usleep(2000); // 2 ms — ~half a 60fps frame
                    continue;
                }
                // Fatal read error — stop the thread.
                std::cerr << "[LoopbackCapture] VIDIOC_DQBUF error after "
                          << frame_count << " frames: " << strerror(errno) << "\n";
                status_message_ = QStringLiteral("LoopbackCapture: VIDIOC_DQBUF error");
                break;
            }

            ++frame_count;
            if (frame_count <= 3 || frame_count % 60 == 0)
            {
                std::cerr << "[LoopbackCapture] frame " << frame_count
                          << " bytesused=" << dqbuf.bytesused
                          << " buf_idx=" << dqbuf.index << "\n";
            }
            const auto *src = static_cast<const std::uint8_t *>(bufs[dqbuf.index].ptr);
            const int bytes_used = static_cast<int>(dqbuf.bytesused);

            if (src != nullptr && bytes_used > 0)
            {
                QVideoFrame video_frame(frame_format);
                if (video_frame.map(QVideoFrame::WriteOnly))
                {
                    auto *dst = video_frame.bits(0);

                    if (is_rgb24)
                    {
                        // RGB24 → RGBA8888: row-by-row pointer walk, no per-pixel division.
                        for (int row = 0; row < height; ++row)
                        {
                            const auto *s = src + row * stride;
                            auto *d = dst + row * width * 4;
                            for (int col = 0; col < width; ++col, s += 3, d += 4)
                            {
                                d[0] = s[0]; // R
                                d[1] = s[1]; // G
                                d[2] = s[2]; // B
                                d[3] = 0xFF; // A
                            }
                        }
                    }
                    else // YUYV
                    {
                        // YUYV 4:2:2 → RGBA8888: one macro-pixel (4 bytes) → 2 RGBA pixels.
                        // Conversion: R = Y + 1.402*(V-128)
                        //             G = Y - 0.344*(U-128) - 0.714*(V-128)
                        //             B = Y + 1.772*(U-128)
                        auto clamp = [](int v) -> std::uint8_t {
                            return static_cast<std::uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v);
                        };
                        const int macro_cols = width / 2;
                        for (int row = 0; row < height; ++row)
                        {
                            const auto *s = src + row * stride;
                            auto *d = dst + row * width * 4;
                            for (int mc = 0; mc < macro_cols; ++mc, s += 4, d += 8)
                            {
                                const int y0 = s[0], u = s[1], y1 = s[2], v = s[3];
                                const int pu = u - 128, pv = v - 128;
                                const int r_off = 1402 * pv / 1000;
                                const int g_off = 344  * pu / 1000 + 714 * pv / 1000;
                                const int b_off = 1772 * pu / 1000;
                                d[0] = clamp(y0 + r_off); d[1] = clamp(y0 - g_off); d[2] = clamp(y0 + b_off); d[3] = 0xFF;
                                d[4] = clamp(y1 + r_off); d[5] = clamp(y1 - g_off); d[6] = clamp(y1 + b_off); d[7] = 0xFF;
                            }
                        }
                    }

                    video_frame.unmap();
                    sink->setVideoFrame(video_frame);
                }
            }

            // Re-queue the buffer immediately.
            if (::ioctl(fd, VIDIOC_QBUF, &dqbuf) != 0)
            {
                std::cerr << "[LoopbackCapture] VIDIOC_QBUF failed after "
                          << frame_count << " frames: " << strerror(errno) << "\n";
                status_message_ = QStringLiteral("LoopbackCapture: VIDIOC_QBUF failed");
                break;
            }
        }

        // --- 6. Cleanup -------------------------------------------------
        ::ioctl(fd, VIDIOC_STREAMOFF, &stream_type);
        for (auto &b : bufs) if (b.ptr) ::munmap(b.ptr, b.len);
        ::close(fd);
        running_ = false;
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
