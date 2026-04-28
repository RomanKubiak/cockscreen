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
#include <poll.h>
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

        // --- 1. Quick format probe (temporary fd, closed immediately) --------
        {
            const int pfd = ::open(device_path.c_str(), O_RDONLY | O_NONBLOCK);
            if (pfd >= 0)
            {
                v4l2_format fmtcheck{};
                fmtcheck.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                if (::ioctl(pfd, VIDIOC_G_FMT, &fmtcheck) == 0)
                {
                    char cc[5] = {};
                    std::memcpy(cc, &fmtcheck.fmt.pix.pixelformat, 4);
                    std::cerr << "[LoopbackCapture] format probe: "
                              << fmtcheck.fmt.pix.width << "x" << fmtcheck.fmt.pix.height
                              << " fourcc=" << cc << "\n";
                }
                ::close(pfd);
            }
            else
            {
                status_message_ = QStringLiteral("LoopbackCapture: cannot open device");
                running_ = false;
                return;
            }
        }

        // --- 2. Wait for the first frame from v4l2sink (writer) via poll() --------
        // poll(POLLIN) on a reader fd is the only reliable synchronisation point:
        // v4l2loopback sets POLLIN only when ready_for_capture==1 (writer called
        // STREAMON_OUTPUT) AND at least one frame has been written.
        // - QUERYCAP(V4L2_CAP_VIDEO_CAPTURE) has false positives: stale format or
        //   the writer opening in READY state (before STREAMON) triggers it.
        // - STREAMON probe without REQBUFS returns EINVAL (from vb2_streamon for
        //   "no buffers") before the ready_for_capture check — always fires.
        // - Only poll() is unconditionally correct and requires no REQBUFS.
        //
        // We hold a single reader fd open during polling; because the poll fd is
        // only READING (no REQBUFS) it does not interfere with v4l2sink's output
        // buffer pool setup (which uses a separate vb2 queue).
        {
            const int poll_fd = ::open(device_path.c_str(), O_RDONLY | O_NONBLOCK);
            if (poll_fd < 0)
            {
                status_message_ = QStringLiteral("LoopbackCapture: cannot open device for poll");
                running_ = false;
                return;
            }

            constexpr int kPoll_max_ms = 30000; // 30 s total
            constexpr int kPoll_step   = 100;   // 100 ms per iteration
            bool got_frame = false;
            for (int elapsed = 0; running_ && elapsed < kPoll_max_ms; elapsed += kPoll_step)
            {
                struct pollfd pfd{poll_fd, POLLIN, 0};
                const int ret = ::poll(&pfd, 1, kPoll_step);
                if (ret > 0 && (pfd.revents & POLLIN))
                {
                    std::cerr << "[LoopbackCapture] first frame detected via poll after "
                              << elapsed + kPoll_step << " ms\n";
                    got_frame = true;
                    break;
                }
                if ((elapsed / kPoll_step) % 20 == 0)
                    std::cerr << "[LoopbackCapture] poll: waiting for first frame ("
                              << elapsed << " ms elapsed)\n";
            }
            ::close(poll_fd);

            if (!got_frame)
            {
                std::cerr << "[LoopbackCapture] timed out waiting for first frame\n";
                status_message_ = QStringLiteral("LoopbackCapture: timed out waiting for writer");
                running_ = false;
                return;
            }
        }

        // --- 3. Open capture fd and read negotiated format -------------------
        const int fd = ::open(device_path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0)
        {
            status_message_ = QStringLiteral("LoopbackCapture: cannot open device for capture");
            running_ = false;
            return;
        }

        v4l2_format fmt{};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (::ioctl(fd, VIDIOC_G_FMT, &fmt) != 0)
        {
            std::cerr << "[LoopbackCapture] VIDIOC_G_FMT failed: " << strerror(errno) << "\n";
            status_message_ = QStringLiteral("LoopbackCapture: VIDIOC_G_FMT failed");
            ::close(fd);
            running_ = false;
            return;
        }

        const int width  = static_cast<int>(fmt.fmt.pix.width);
        const int height = static_cast<int>(fmt.fmt.pix.height);
        const int stride = static_cast<int>(fmt.fmt.pix.bytesperline);
        const std::uint32_t fourcc = fmt.fmt.pix.pixelformat;
        const bool is_rgb24 = (fourcc == V4L2_PIX_FMT_RGB24);
        const bool is_yuyv  = (fourcc == V4L2_PIX_FMT_YUYV);

        {
            char cc[5] = {};
            std::memcpy(cc, &fourcc, 4);
            std::cerr << "[LoopbackCapture] opened " << device_path
                      << " " << width << "x" << height
                      << " stride=" << stride << " fourcc=" << cc
                      << " is_rgb24=" << is_rgb24 << " is_yuyv=" << is_yuyv << "\n";
        }

        if (!is_rgb24 && !is_yuyv)
        {
            char cc[5] = {};
            std::memcpy(cc, &fourcc, 4);
            status_message_ = QStringLiteral("LoopbackCapture: unsupported format '%1'")
                                  .arg(QString::fromLatin1(cc));
            ::close(fd);
            running_ = false;
            return;
        }

        // --- 4. Allocate MMAP capture buffers --------------------------------
        v4l2_requestbuffers req{};
        req.count  = 4;
        req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        if (::ioctl(fd, VIDIOC_REQBUFS, &req) != 0 || req.count == 0)
        {
            std::cerr << "[LoopbackCapture] VIDIOC_REQBUFS failed: " << strerror(errno) << "\n";
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
                for (auto &b : bufs) if (b.ptr) ::munmap(b.ptr, b.len);
                ::close(fd);
                running_ = false;
                return;
            }
            bufs[i].len = qbuf.length;
            bufs[i].ptr = ::mmap(nullptr, qbuf.length, PROT_READ, MAP_SHARED, fd, qbuf.m.offset);
            if (bufs[i].ptr == MAP_FAILED)
            {
                bufs[i].ptr = nullptr;
                status_message_ = QStringLiteral("LoopbackCapture: mmap failed");
                for (auto &b : bufs) if (b.ptr) ::munmap(b.ptr, b.len);
                ::close(fd);
                running_ = false;
                return;
            }
            if (::ioctl(fd, VIDIOC_QBUF, &qbuf) != 0)
            {
                status_message_ = QStringLiteral("LoopbackCapture: VIDIOC_QBUF priming failed");
                for (auto &b : bufs) if (b.ptr) ::munmap(b.ptr, b.len);
                ::close(fd);
                running_ = false;
                return;
            }
        }

        // --- 5. Start capture streaming --------------------------------------
        int stream_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (::ioctl(fd, VIDIOC_STREAMON, &stream_type) != 0)
        {
            std::cerr << "[LoopbackCapture] VIDIOC_STREAMON failed: " << strerror(errno) << "\n";
            status_message_ = QStringLiteral("LoopbackCapture: VIDIOC_STREAMON failed");
            for (auto &b : bufs) if (b.ptr) ::munmap(b.ptr, b.len);
            ::close(fd);
            running_ = false;
            return;
        }
        std::cerr << "[LoopbackCapture] STREAMON ok, entering capture loop\n";

        // --- 6. Capture loop ------------------------------------------------
        const QVideoFrameFormat frame_format(
            QSize(width, height), QVideoFrameFormat::Format_RGBA8888);
        int frame_count = 0;

        while (running_)
        {
            v4l2_buffer dqbuf{};
            dqbuf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            dqbuf.memory = V4L2_MEMORY_MMAP;

            if (::ioctl(fd, VIDIOC_DQBUF, &dqbuf) != 0)
            {
                if (errno == EAGAIN) { QThread::usleep(2000); continue; }
                std::cerr << "[LoopbackCapture] VIDIOC_DQBUF error after "
                          << frame_count << " frames: " << strerror(errno) << "\n";
                status_message_ = QStringLiteral("LoopbackCapture: VIDIOC_DQBUF error");
                break;
            }

            ++frame_count;
            if (frame_count <= 3 || frame_count % 60 == 0)
                std::cerr << "[LoopbackCapture] frame " << frame_count
                          << " bytesused=" << dqbuf.bytesused
                          << " buf_idx=" << dqbuf.index << "\n";

            const auto *src       = static_cast<const std::uint8_t *>(bufs[dqbuf.index].ptr);
            const int   bytes_used = static_cast<int>(dqbuf.bytesused);

            if (src != nullptr && bytes_used > 0)
            {
                QVideoFrame video_frame(frame_format);
                if (video_frame.map(QVideoFrame::WriteOnly))
                {
                    auto *dst = video_frame.bits(0);
                    if (is_rgb24)
                    {
                        for (int row = 0; row < height; ++row)
                        {
                            const auto *s = src + row * stride;
                            auto *d = dst + row * width * 4;
                            for (int col = 0; col < width; ++col, s += 3, d += 4)
                            { d[0]=s[0]; d[1]=s[1]; d[2]=s[2]; d[3]=0xFF; }
                        }
                    }
                    else
                    {
                        auto clamp = [](int v) -> std::uint8_t {
                            return static_cast<std::uint8_t>(v<0?0:v>255?255:v);
                        };
                        const int mc = width / 2;
                        for (int row = 0; row < height; ++row)
                        {
                            const auto *s = src + row * stride;
                            auto *d = dst + row * width * 4;
                            for (int c = 0; c < mc; ++c, s += 4, d += 8)
                            {
                                const int y0=s[0],u=s[1],y1=s[2],v=s[3];
                                const int pu=u-128,pv=v-128;
                                const int r=1402*pv/1000, g=344*pu/1000+714*pv/1000, b=1772*pu/1000;
                                d[0]=clamp(y0+r);d[1]=clamp(y0-g);d[2]=clamp(y0+b);d[3]=0xFF;
                                d[4]=clamp(y1+r);d[5]=clamp(y1-g);d[6]=clamp(y1+b);d[7]=0xFF;
                            }
                        }
                    }
                    video_frame.unmap();
                    sink->setVideoFrame(video_frame);
                }
            }

            if (::ioctl(fd, VIDIOC_QBUF, &dqbuf) != 0)
            {
                std::cerr << "[LoopbackCapture] VIDIOC_QBUF failed after "
                          << frame_count << " frames: " << strerror(errno) << "\n";
                status_message_ = QStringLiteral("LoopbackCapture: VIDIOC_QBUF failed");
                break;
            }
        }

        // --- 7. Cleanup ----------------------------------------------------
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

bool LoopbackCapture::is_running() const { return running_.load(); }

const QString &LoopbackCapture::status_message() const { return status_message_; }

} // namespace cockscreen::runtime

#endif // _WIN32
