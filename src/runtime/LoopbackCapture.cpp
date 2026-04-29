#include "cockscreen/runtime/LoopbackCapture.hpp"

#ifndef _WIN32

#include <QSize>
#include <QVideoFrame>
#include <QVideoFrameFormat>

#include <cstdio>
#include <string_view>
#include <cerrno>
#include <chrono>
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

        // --- 2. Wait for v4l2loopback to enter "capture" state via sysfs ----------
        // In v4l2loopback 0.15.0, the sysfs file
        //   /sys/devices/virtual/video4linux/videoN/state
        // shows:
        //   "idle"    – no writer open
        //   "output"  – writer open, format set, NOT yet streaming
        //   "capture" – writer called STREAMON(OUTPUT) → ready_for_capture > 0
        //
        // "capture" is the ONLY reliable signal that STREAMON(VIDEO_CAPTURE)
        // will succeed:
        //   - QUERYCAP(V4L2_CAP_VIDEO_CAPTURE) reflects OUR fd's own caps, not
        //     the writer state — always true for an O_RDONLY fd.
        //   - STREAMON probe without REQBUFS returns EINVAL from vb2_streamon
        //     ("no buffers") BEFORE the ready_for_capture check — always fires.
        //   - poll(POLLIN) without REQBUFS is not wired up in v4l2loopback's
        //     vb2_poll unless capture buffers are already allocated.
        //
        // We must NOT call REQBUFS before the writer's STREAMON(OUTPUT).  If we
        // do, the capture-side vb2 queue is allocated first; when v4l2sink then
        // allocates the output-side queue the two conflict and ready_for_capture
        // never becomes > 0.
        // Derive sysfs path once; reused in the capture loop for diagnostics.
        const std::string dev_name = device_path.substr(device_path.rfind('/') + 1);
        const std::string sysfs_state =
            "/sys/devices/virtual/video4linux/" + dev_name + "/state";

        {

            constexpr int kMax_ms   = 30000; // 30 s
            constexpr int kStep_ms  = 50;
            bool writer_streaming = false;
            for (int elapsed = 0; running_ && elapsed < kMax_ms; elapsed += kStep_ms)
            {
                FILE *f = ::fopen(sysfs_state.c_str(), "r");
                if (f != nullptr)
                {
                    char buf[32] = {};
                    if (::fgets(buf, sizeof(buf), f) != nullptr)
                    {
                        const std::string_view sv{buf};
                        if (sv.find("capture") != std::string_view::npos)
                        {
                            std::cerr << "[LoopbackCapture] sysfs state=capture after "
                                      << elapsed << " ms — writer is streaming\n";
                            writer_streaming = true;
                            ::fclose(f);
                            break;
                        }
                    }
                    ::fclose(f);
                }
                if (elapsed % 2000 == 0)
                    std::cerr << "[LoopbackCapture] sysfs: waiting for state=capture ("
                              << elapsed << " ms)\n";
                ::usleep(static_cast<unsigned>(kStep_ms) * 1000);
            }

            if (!writer_streaming)
            {
                std::cerr << "[LoopbackCapture] timed out waiting for state=capture\n";
                status_message_ = QStringLiteral("LoopbackCapture: writer never reached streaming state");
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
        long eagain_count = 0;
        const auto loop_start = std::chrono::steady_clock::now();

        while (running_)
        {
            v4l2_buffer dqbuf{};
            dqbuf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            dqbuf.memory = V4L2_MEMORY_MMAP;

            if (::ioctl(fd, VIDIOC_DQBUF, &dqbuf) != 0)
            {
                if (errno == EAGAIN)
                {
                    ++eagain_count;
                    // Every 5 seconds of EAGAIN, log sysfs state + count.
                    if (eagain_count % 2500 == 0)
                    {
                        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - loop_start).count();
                        // Read sysfs state inline for diagnostics.
                        char sybuf[32] = {};
                        FILE *sf = ::fopen(sysfs_state.c_str(), "r");
                        if (sf) { ::fgets(sybuf, sizeof(sybuf), sf); ::fclose(sf); }
                        const char *nl = ::strchr(sybuf, '\n'); if (nl) const_cast<char*>(nl)[0]=0;
                        std::cerr << "[LoopbackCapture] EAGAIN x" << eagain_count
                                  << " at t=" << ms << "ms sysfs=" << sybuf
                                  << " frames=" << frame_count << "\n";
                    }
                    QThread::usleep(2000);
                    continue;
                }
                std::cerr << "[LoopbackCapture] VIDIOC_DQBUF error after "
                          << frame_count << " frames: " << strerror(errno) << "\n";
                status_message_ = QStringLiteral("LoopbackCapture: VIDIOC_DQBUF error");
                break;
            }

            ++frame_count;
            if (frame_count <= 3 || frame_count % 60 == 0)
            {
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - loop_start).count();
                std::cerr << "[LoopbackCapture] frame " << frame_count
                          << " t=" << ms << "ms"
                          << " bytesused=" << dqbuf.bytesused
                          << " buf_idx=" << dqbuf.index << "\n";
            }

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
