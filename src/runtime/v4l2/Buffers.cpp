#include "cockscreen/runtime/V4l2Capture.hpp"

#include "cockscreen/runtime/v4l2/Support.hpp"

#include <linux/videodev2.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace cockscreen::runtime
{

bool V4l2Capture::request_buffers()
{
    v4l2_requestbuffers request{};
    request.count = 4;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;
    if (v4l2::ioctl_retry(fd_, VIDIOC_REQBUFS, &request) != 0 || request.count < 2)
    {
        error_message_ = "VIDIOC_REQBUFS failed";
        return false;
    }

    buffers_.resize(request.count);
    dmabuf_export_supported_ = true;

    for (unsigned int index = 0; index < request.count; ++index)
    {
        v4l2_buffer buffer{};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = index;
        if (v4l2::ioctl_retry(fd_, VIDIOC_QUERYBUF, &buffer) != 0)
        {
            error_message_ = "VIDIOC_QUERYBUF failed";
            return false;
        }

        buffers_[index].length = buffer.length;
        buffers_[index].data = mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, buffer.m.offset);
        if (buffers_[index].data == MAP_FAILED)
        {
            buffers_[index].data = nullptr;
            error_message_ = "mmap failed for V4L2 buffer";
            return false;
        }

        v4l2_exportbuffer export_buffer{};
        export_buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        export_buffer.index = index;
        export_buffer.flags = O_CLOEXEC;
        if (v4l2::ioctl_retry(fd_, VIDIOC_EXPBUF, &export_buffer) == 0)
        {
            buffers_[index].dmabuf_fd = export_buffer.fd;
        }
        else
        {
            dmabuf_export_supported_ = false;
        }
    }

    return true;
}

bool V4l2Capture::queue_all_buffers()
{
    for (unsigned int index = 0; index < buffers_.size(); ++index)
    {
        v4l2_buffer buffer{};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = index;
        if (v4l2::ioctl_retry(fd_, VIDIOC_QBUF, &buffer) != 0)
        {
            error_message_ = "VIDIOC_QBUF failed while priming buffers";
            return false;
        }
    }

    return true;
}

void V4l2Capture::stop_stream()
{
    if (fd_ < 0 || !streaming_)
    {
        return;
    }

    if (active_buffer_index_ >= 0)
    {
        release();
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    v4l2::ioctl_retry(fd_, VIDIOC_STREAMOFF, &type);
    streaming_ = false;
}

void V4l2Capture::close_device()
{
    stop_stream();

    for (auto &buffer : buffers_)
    {
        if (buffer.dmabuf_fd >= 0)
        {
            ::close(buffer.dmabuf_fd);
            buffer.dmabuf_fd = -1;
        }

        if (buffer.data != nullptr)
        {
            munmap(buffer.data, buffer.length);
            buffer.data = nullptr;
        }
    }

    buffers_.clear();

    if (fd_ >= 0)
    {
        ::close(fd_);
        fd_ = -1;
    }

    active_buffer_index_ = -1;
    width_ = 0;
    height_ = 0;
    stride_ = 0;
    pixel_format_ = V4l2PixelFormat::unsupported;
    format_label_ = "unknown";
    dmabuf_export_supported_ = false;
    prefer_rgb_capture_ = false;
}

} // namespace cockscreen::runtime