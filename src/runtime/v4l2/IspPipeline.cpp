#include "cockscreen/runtime/v4l2/IspPipeline.hpp"

#include "cockscreen/runtime/v4l2/Support.hpp"

#include <linux/videodev2.h>
#include <sys/mman.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <unistd.h>

namespace cockscreen::runtime::v4l2
{

static bool mmap_buffers(int fd, std::uint32_t buf_type, std::vector<IspBuffer> &out,
                         std::string *error_out)
{
    v4l2_requestbuffers req{};
    req.type   = buf_type;
    req.memory = V4L2_MEMORY_MMAP;
    req.count  = 4;
    if (ioctl_retry(fd, VIDIOC_REQBUFS, &req) != 0 || req.count == 0)
    {
        if (error_out) *error_out = "VIDIOC_REQBUFS failed";
        return false;
    }

    out.resize(req.count);
    for (unsigned int i = 0; i < req.count; ++i)
    {
        v4l2_buffer buf{};
        buf.type   = buf_type;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (ioctl_retry(fd, VIDIOC_QUERYBUF, &buf) != 0)
        {
            if (error_out) *error_out = "VIDIOC_QUERYBUF failed";
            return false;
        }

        out[i].size = buf.length;
        out[i].data = ::mmap(nullptr, buf.length, PROT_READ | PROT_WRITE,
                             MAP_SHARED, fd, buf.m.offset);
        if (out[i].data == MAP_FAILED)
        {
            out[i].data = nullptr;
            if (error_out) *error_out = "mmap failed";
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------

IspPipeline::~IspPipeline()
{
    close();
}

bool IspPipeline::find_devices(std::string *input_dev, std::string *output_dev)
{
    std::string found_input;
    std::string found_output;

    for (int n = 0; n <= 31 && (found_input.empty() || found_output.empty()); ++n)
    {
        const std::string path = "/dev/video" + std::to_string(n);
        const int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK);
        if (fd < 0)
            continue;

        v4l2_capability cap{};
        const bool ok = ioctl_retry(fd, VIDIOC_QUERYCAP, &cap) == 0;
        ::close(fd);
        if (!ok)
            continue;

        const std::string driver{reinterpret_cast<const char *>(cap.driver)};
        if (driver != "bcm2835-isp")
            continue;

        const bool is_output  = (cap.capabilities & V4L2_CAP_VIDEO_OUTPUT)  != 0;
        const bool is_capture = (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) != 0;

        if (is_output && found_input.empty())
            found_input = path;
        else if (is_capture && found_output.empty())
            found_output = path;
    }

    if (found_input.empty() || found_output.empty())
        return false;

    if (input_dev)  *input_dev  = found_input;
    if (output_dev) *output_dev = found_output;
    return true;
}

// ---------------------------------------------------------------------------

bool IspPipeline::open(const std::string &input_dev, const std::string &output_dev,
                       int width, int height,
                       std::uint32_t input_fourcc, std::uint32_t output_fourcc,
                       std::string *error_out)
{
    auto fail = [&](const std::string &msg) -> bool {
        error_ = msg;
        if (error_out) *error_out = msg;
        close();
        return false;
    };

    // --- ISP input ---
    input_fd_ = ::open(input_dev.c_str(), O_RDWR | O_NONBLOCK);
    if (input_fd_ < 0)
        return fail("Cannot open ISP input " + input_dev);

    v4l2_format in_fmt{};
    in_fmt.type                = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    in_fmt.fmt.pix.width       = static_cast<std::uint32_t>(width);
    in_fmt.fmt.pix.height      = static_cast<std::uint32_t>(height);
    in_fmt.fmt.pix.pixelformat = input_fourcc;
    in_fmt.fmt.pix.field       = V4L2_FIELD_NONE;
    in_fmt.fmt.pix.colorspace  = V4L2_COLORSPACE_RAW;
    if (ioctl_retry(input_fd_, VIDIOC_S_FMT, &in_fmt) != 0)
        return fail("ISP input VIDIOC_S_FMT failed");

    std::string mmap_err;
    if (!mmap_buffers(input_fd_, V4L2_BUF_TYPE_VIDEO_OUTPUT, input_buffers_, &mmap_err))
        return fail("ISP input buffers: " + mmap_err);

    input_buffer_free_.assign(input_buffers_.size(), true);

    // --- ISP output ---
    output_fd_ = ::open(output_dev.c_str(), O_RDWR | O_NONBLOCK);
    if (output_fd_ < 0)
        return fail("Cannot open ISP output " + output_dev);

    v4l2_format out_fmt{};
    out_fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    out_fmt.fmt.pix.width       = static_cast<std::uint32_t>(width);
    out_fmt.fmt.pix.height      = static_cast<std::uint32_t>(height);
    out_fmt.fmt.pix.pixelformat = output_fourcc;
    out_fmt.fmt.pix.field       = V4L2_FIELD_NONE;
    if (ioctl_retry(output_fd_, VIDIOC_S_FMT, &out_fmt) != 0)
        return fail("ISP output VIDIOC_S_FMT failed");

    output_width_  = static_cast<int>(out_fmt.fmt.pix.width);
    output_height_ = static_cast<int>(out_fmt.fmt.pix.height);
    output_pixel_format_ = to_pixel_format(out_fmt.fmt.pix.pixelformat);
    if (output_pixel_format_ == V4l2PixelFormat::unsupported)
        return fail("ISP output pixel format not recognised");

    if (!mmap_buffers(output_fd_, V4L2_BUF_TYPE_VIDEO_CAPTURE, output_buffers_, &mmap_err))
        return fail("ISP output buffers: " + mmap_err);

    return true;
}

// ---------------------------------------------------------------------------

bool IspPipeline::start()
{
    if (input_fd_ < 0 || output_fd_ < 0)
    {
        error_ = "ISP pipeline not open";
        return false;
    }

    // Queue all output buffers so the ISP has somewhere to write.
    for (unsigned int i = 0; i < output_buffers_.size(); ++i)
    {
        v4l2_buffer buf{};
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (ioctl_retry(output_fd_, VIDIOC_QBUF, &buf) != 0)
        {
            error_ = "ISP output VIDIOC_QBUF failed during start";
            return false;
        }
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl_retry(output_fd_, VIDIOC_STREAMON, &type) != 0)
    {
        error_ = "ISP output VIDIOC_STREAMON failed";
        return false;
    }

    type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    if (ioctl_retry(input_fd_, VIDIOC_STREAMON, &type) != 0)
    {
        error_ = "ISP input VIDIOC_STREAMON failed";
        return false;
    }

    streaming_ = true;
    return true;
}

void IspPipeline::stop()
{
    if (!streaming_)
        return;

    int type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    ioctl_retry(input_fd_, VIDIOC_STREAMOFF, &type);
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl_retry(output_fd_, VIDIOC_STREAMOFF, &type);

    streaming_           = false;
    active_input_index_  = -1;
    active_output_index_ = -1;
    input_buffer_free_.assign(input_buffers_.size(), true);
}

void IspPipeline::close()
{
    stop();

    auto unmap = [](std::vector<IspBuffer> &bufs) {
        for (auto &b : bufs)
            if (b.data) { ::munmap(b.data, b.size); b.data = nullptr; }
        bufs.clear();
    };
    unmap(input_buffers_);
    unmap(output_buffers_);
    input_buffer_free_.clear();

    if (input_fd_  >= 0) { ::close(input_fd_);  input_fd_  = -1; }
    if (output_fd_ >= 0) { ::close(output_fd_); output_fd_ = -1; }
}

// ---------------------------------------------------------------------------

bool IspPipeline::queue_input(const void *data, std::size_t size)
{
    if (!streaming_)
        return false;

    // Find a free input buffer.
    int idx = -1;
    for (int i = 0; i < static_cast<int>(input_buffer_free_.size()); ++i)
    {
        if (input_buffer_free_[i]) { idx = i; break; }
    }
    if (idx < 0)
        return false; // all input slots in use

    // Copy raw frame data — caller can release the source buffer immediately.
    const std::size_t copy_size = std::min(size, input_buffers_[idx].size);
    std::memcpy(input_buffers_[idx].data, data, copy_size);

    v4l2_buffer buf{};
    buf.type      = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    buf.memory    = V4L2_MEMORY_MMAP;
    buf.index     = static_cast<std::uint32_t>(idx);
    buf.bytesused = static_cast<std::uint32_t>(copy_size);
    if (ioctl_retry(input_fd_, VIDIOC_QBUF, &buf) != 0)
    {
        error_ = "ISP input VIDIOC_QBUF failed: " + std::to_string(errno);
        std::cerr << "[isp-pipeline] " << error_ << '\n';
        return false;
    }

    input_buffer_free_[idx] = false;
    return true;
}

std::optional<V4l2FrameView> IspPipeline::dequeue_output()
{
    if (!streaming_)
        return std::nullopt;

    if (active_output_index_ >= 0)
        release_output();

    // Dequeue a processed frame from the ISP output.
    v4l2_buffer out_buf{};
    out_buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    out_buf.memory = V4L2_MEMORY_MMAP;
    if (ioctl_retry(output_fd_, VIDIOC_DQBUF, &out_buf) != 0)
    {
        if (errno != EAGAIN)
            error_ = "ISP output VIDIOC_DQBUF failed: " + std::to_string(errno);
        return std::nullopt;
    }

    active_output_index_ = static_cast<int>(out_buf.index);

    // Recycle the corresponding input buffer — the ISP is done with it.
    v4l2_buffer in_buf{};
    in_buf.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    in_buf.memory = V4L2_MEMORY_MMAP;
    if (ioctl_retry(input_fd_, VIDIOC_DQBUF, &in_buf) == 0)
        input_buffer_free_[in_buf.index] = true;

    const auto &b = output_buffers_[active_output_index_];
    const int stride = output_width_ *
        (output_pixel_format_ == V4l2PixelFormat::yuyv ||
         output_pixel_format_ == V4l2PixelFormat::uyvy ? 2 : 3);

    return V4l2FrameView{
        static_cast<const std::uint8_t *>(b.data),
        out_buf.bytesused,
        output_width_,
        output_height_,
        stride,
        active_output_index_,
        /*dmabuf_fd=*/-1,
        output_pixel_format_
    };
}

void IspPipeline::release_output()
{
    if (!streaming_ || active_output_index_ < 0)
        return;

    v4l2_buffer buf{};
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index  = static_cast<std::uint32_t>(active_output_index_);
    ioctl_retry(output_fd_, VIDIOC_QBUF, &buf);
    active_output_index_ = -1;
}

} // namespace cockscreen::runtime::v4l2
