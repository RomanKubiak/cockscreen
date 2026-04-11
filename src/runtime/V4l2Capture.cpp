#include "../../include/cockscreen/runtime/V4l2Capture.hpp"

#include <linux/videodev2.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

namespace cockscreen::runtime
{

namespace
{

int ioctl_retry(int fd, unsigned long request, void *arg)
{
    int result = -1;
    do
    {
        result = ioctl(fd, request, arg);
    } while (result == -1 && errno == EINTR);

    return result;
}

V4l2PixelFormat to_pixel_format(std::uint32_t value)
{
    switch (value)
    {
    case V4L2_PIX_FMT_YUYV:
        return V4l2PixelFormat::yuyv;
    case V4L2_PIX_FMT_UYVY:
        return V4l2PixelFormat::uyvy;
    case V4L2_PIX_FMT_RGB24:
        return V4l2PixelFormat::rgb24;
    case V4L2_PIX_FMT_BGR24:
        return V4l2PixelFormat::bgr24;
    default:
        return V4l2PixelFormat::unsupported;
    }
}

std::string to_format_label(V4l2PixelFormat pixel_format, int width, int height)
{
    std::string label;
    switch (pixel_format)
    {
    case V4l2PixelFormat::yuyv:
        label = "YUYV";
        break;
    case V4l2PixelFormat::uyvy:
        label = "UYVY";
        break;
    case V4l2PixelFormat::rgb24:
        label = "RGB24";
        break;
    case V4l2PixelFormat::bgr24:
        label = "BGR24";
        break;
    default:
        label = "unknown";
        break;
    }

    label += " ";
    label += std::to_string(width);
    label += "x";
    label += std::to_string(height);
    return label;
}

std::string fourcc_to_string(std::uint32_t fourcc)
{
    std::array<char, 5> buffer{};
    buffer[0] = static_cast<char>(fourcc & 0xFFU);
    buffer[1] = static_cast<char>((fourcc >> 8U) & 0xFFU);
    buffer[2] = static_cast<char>((fourcc >> 16U) & 0xFFU);
    buffer[3] = static_cast<char>((fourcc >> 24U) & 0xFFU);
    return std::string{buffer.data()};
}

} // namespace

std::vector<std::string> V4l2Capture::enumerate_supported_modes(std::string_view device_path)
{
    std::vector<std::string> result;

    const std::string device_path_text{device_path};
    const int fd = ::open(device_path_text.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0)
    {
        return result;
    }

    v4l2_capability capability{};
    if (ioctl_retry(fd, VIDIOC_QUERYCAP, &capability) != 0 || (capability.capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0)
    {
        ::close(fd);
        return result;
    }

    for (unsigned int format_index = 0;; ++format_index)
    {
        v4l2_fmtdesc format{};
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        format.index = format_index;
        if (ioctl_retry(fd, VIDIOC_ENUM_FMT, &format) != 0)
        {
            break;
        }

        const auto fourcc = fourcc_to_string(format.pixelformat);
        const std::string description{reinterpret_cast<const char *>(format.description)};

        bool found_size = false;
        for (unsigned int size_index = 0;; ++size_index)
        {
            v4l2_frmsizeenum frame_size{};
            frame_size.pixel_format = format.pixelformat;
            frame_size.index = size_index;
            if (ioctl_retry(fd, VIDIOC_ENUM_FRAMESIZES, &frame_size) != 0)
            {
                break;
            }

            found_size = true;
            if (frame_size.type == V4L2_FRMSIZE_TYPE_DISCRETE)
            {
                result.emplace_back(description + " (" + fourcc + ") " +
                                    std::to_string(frame_size.discrete.width) + "x" +
                                    std::to_string(frame_size.discrete.height));
            }
            else if (frame_size.type == V4L2_FRMSIZE_TYPE_STEPWISE || frame_size.type == V4L2_FRMSIZE_TYPE_CONTINUOUS)
            {
                result.emplace_back(description + " (" + fourcc + ") " +
                                    std::to_string(frame_size.stepwise.min_width) + "x" +
                                    std::to_string(frame_size.stepwise.min_height) + " .. " +
                                    std::to_string(frame_size.stepwise.max_width) + "x" +
                                    std::to_string(frame_size.stepwise.max_height));
                break;
            }
        }

        if (!found_size)
        {
            result.emplace_back(description + " (" + fourcc + ") <size enumeration unavailable>");
        }
    }

    ::close(fd);

    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

V4l2Capture::~V4l2Capture()
{
    close_device();
}

bool V4l2Capture::open(std::string_view device_path, int requested_width, int requested_height,
                       bool prefer_rgb_capture)
{
    close_device();
    prefer_rgb_capture_ = prefer_rgb_capture;

    fd_ = ::open(std::string(device_path).c_str(), O_RDWR | O_NONBLOCK);
    if (fd_ < 0)
    {
        error_message_ = "Failed to open V4L2 device";
        return false;
    }

    v4l2_capability capability{};
    if (ioctl_retry(fd_, VIDIOC_QUERYCAP, &capability) != 0)
    {
        error_message_ = "VIDIOC_QUERYCAP failed";
        close_device();
        return false;
    }

    if ((capability.capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0 ||
        (capability.capabilities & V4L2_CAP_STREAMING) == 0)
    {
        error_message_ = "Device does not support V4L2 video capture streaming";
        close_device();
        return false;
    }

    if (!configure_format(requested_width, requested_height) || !request_buffers())
    {
        close_device();
        return false;
    }

    return true;
}

bool V4l2Capture::start()
{
    if (fd_ < 0)
    {
        error_message_ = "V4L2 device is not open";
        return false;
    }

    if (!queue_all_buffers())
    {
        return false;
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl_retry(fd_, VIDIOC_STREAMON, &type) != 0)
    {
        error_message_ = "VIDIOC_STREAMON failed";
        return false;
    }

    streaming_ = true;
    return true;
}

std::optional<V4l2FrameView> V4l2Capture::dequeue()
{
    if (fd_ < 0 || !streaming_)
    {
        return std::nullopt;
    }

    if (active_buffer_index_ >= 0)
    {
        release();
    }

    v4l2_buffer buffer{};
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;
    if (ioctl_retry(fd_, VIDIOC_DQBUF, &buffer) != 0)
    {
        if (errno == EAGAIN)
        {
            return std::nullopt;
        }

        error_message_ = "VIDIOC_DQBUF failed";
        return std::nullopt;
    }

    if (buffer.index >= buffers_.size())
    {
        error_message_ = "V4L2 returned an invalid buffer index";
        return std::nullopt;
    }

    active_buffer_index_ = static_cast<int>(buffer.index);
    return V4l2FrameView{static_cast<const std::uint8_t *>(buffers_[buffer.index].data), buffer.bytesused, width_,
                         height_, stride_, static_cast<int>(buffer.index), buffers_[buffer.index].dmabuf_fd,
                         pixel_format_};
}

void V4l2Capture::release()
{
    if (fd_ < 0 || active_buffer_index_ < 0)
    {
        return;
    }

    v4l2_buffer buffer{};
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.index = static_cast<unsigned int>(active_buffer_index_);
    if (ioctl_retry(fd_, VIDIOC_QBUF, &buffer) != 0)
    {
        error_message_ = "VIDIOC_QBUF failed";
    }

    active_buffer_index_ = -1;
}

const std::string &V4l2Capture::error_message() const
{
    return error_message_;
}

const std::string &V4l2Capture::format_label() const
{
    return format_label_;
}

bool V4l2Capture::dmabuf_export_supported() const
{
    return dmabuf_export_supported_;
}

int V4l2Capture::width() const
{
    return width_;
}

int V4l2Capture::height() const
{
    return height_;
}

V4l2PixelFormat V4l2Capture::pixel_format() const
{
    return pixel_format_;
}

bool V4l2Capture::configure_format(int requested_width, int requested_height)
{
    const std::array<std::uint32_t, 4> candidate_formats = prefer_rgb_capture_
                                                               ? std::array<std::uint32_t, 4>{V4L2_PIX_FMT_RGB24,
                                                                                               V4L2_PIX_FMT_BGR24,
                                                                                               V4L2_PIX_FMT_YUYV,
                                                                                               V4L2_PIX_FMT_UYVY}
                                                               : std::array<std::uint32_t, 4>{V4L2_PIX_FMT_YUYV,
                                                                                               V4L2_PIX_FMT_UYVY,
                                                                                               V4L2_PIX_FMT_RGB24,
                                                                                               V4L2_PIX_FMT_BGR24};

    for (const auto candidate : candidate_formats)
    {
        v4l2_format format{};
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        format.fmt.pix.width = requested_width;
        format.fmt.pix.height = requested_height;
        format.fmt.pix.pixelformat = candidate;
        format.fmt.pix.field = V4L2_FIELD_NONE;

        if (ioctl_retry(fd_, VIDIOC_S_FMT, &format) != 0)
        {
            continue;
        }

        pixel_format_ = to_pixel_format(format.fmt.pix.pixelformat);
        if (pixel_format_ == V4l2PixelFormat::unsupported)
        {
            continue;
        }

        width_ = static_cast<int>(format.fmt.pix.width);
        height_ = static_cast<int>(format.fmt.pix.height);
        stride_ = static_cast<int>(format.fmt.pix.bytesperline);
        format_label_ = to_format_label(pixel_format_, width_, height_);
        return true;
    }

    error_message_ = "Failed to negotiate a supported V4L2 pixel format";
    return false;
}

bool V4l2Capture::request_buffers()
{
    v4l2_requestbuffers request{};
    request.count = 4;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;
    if (ioctl_retry(fd_, VIDIOC_REQBUFS, &request) != 0 || request.count < 2)
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
        if (ioctl_retry(fd_, VIDIOC_QUERYBUF, &buffer) != 0)
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
        if (ioctl_retry(fd_, VIDIOC_EXPBUF, &export_buffer) == 0)
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
        if (ioctl_retry(fd_, VIDIOC_QBUF, &buffer) != 0)
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
    ioctl_retry(fd_, VIDIOC_STREAMOFF, &type);
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