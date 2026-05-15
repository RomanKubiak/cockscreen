#include "cockscreen/runtime/V4l2Capture.hpp"

#include "cockscreen/runtime/v4l2/Support.hpp"

#include <linux/videodev2.h>

#include <cerrno>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace cockscreen::runtime::v4l2
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

} // namespace cockscreen::runtime::v4l2

namespace cockscreen::runtime
{

V4l2Capture::~V4l2Capture()
{
    close_device();
}

bool V4l2Capture::open(std::string_view device_path, int requested_width, int requested_height,
                       bool prefer_rgb_capture)
{
    close_device();
    prefer_rgb_capture_ = prefer_rgb_capture;
    device_path_ = std::string(device_path);

    fd_ = ::open(device_path_.c_str(), O_RDWR | O_NONBLOCK);
    if (fd_ < 0)
    {
        error_message_ = "Failed to open V4L2 device";
        return false;
    }

    v4l2_capability capability{};
    if (v4l2::ioctl_retry(fd_, VIDIOC_QUERYCAP, &capability) != 0)
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

    is_mc_device_ = (capability.capabilities & V4L2_CAP_IO_MC) != 0;

    std::string mc_error;
    std::uint32_t mc_mbus_code = 0;
    bool mc_configured = false;
    if (is_mc_device_)
    {
        mc_configured = v4l2::mc_setup_pipeline(device_path_, requested_width, requested_height,
                                                &mc_error, &mc_mbus_code);
    }

    bool format_ready = configure_format(requested_width, requested_height, mc_mbus_code);
    if (!format_ready && is_mc_device_)
    {
        const std::string direct_error = error_message_;
        if (!mc_configured)
        {
            error_message_ = "Direct MC capture negotiation failed: " + direct_error +
                             " | Media Controller pipeline setup failed: " + mc_error;
            close_device();
            return false;
        }
        error_message_ = "Direct MC capture negotiation failed: " + direct_error +
                         " | After Media Controller setup: " + error_message_;
    }

    if (!format_ready || !request_buffers())
    {
        close_device();
        return false;
    }

    // Override pixel_format_ with the exact pattern from the sensor's mbus code
    // so debayering uses the correct channel layout (OV5647=GBRG, IMX219=RGGB, etc.).
    if (is_mc_device_ && mc_mbus_code != 0)
    {
        const auto pattern = v4l2::mbus_code_to_pixel_format(mc_mbus_code);
        if (pattern != V4l2PixelFormat::unsupported)
        {
            pixel_format_ = pattern;
            format_label_ = v4l2::to_format_label(pixel_format_, width_, height_);
        }
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
    if (v4l2::ioctl_retry(fd_, VIDIOC_STREAMON, &type) != 0)
    {
        if (is_mc_device_)
        {
            std::string mc_error;
            std::uint32_t retry_mbus_code = 0;
            if (v4l2::mc_setup_pipeline(device_path_, width_, height_, &mc_error, &retry_mbus_code) &&
                v4l2::ioctl_retry(fd_, VIDIOC_STREAMON, &type) == 0)
            {
                if (retry_mbus_code != 0)
                {
                    const auto pattern = v4l2::mbus_code_to_pixel_format(retry_mbus_code);
                    if (pattern != V4l2PixelFormat::unsupported)
                    {
                        pixel_format_ = pattern;
                        format_label_ = v4l2::to_format_label(pixel_format_, width_, height_);
                    }
                }
                streaming_ = true;
                return true;
            }
            if (!mc_error.empty())
            {
                error_message_ = "VIDIOC_STREAMON failed after MC retry: " + mc_error;
                return false;
            }
        }
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
    if (v4l2::ioctl_retry(fd_, VIDIOC_DQBUF, &buffer) != 0)
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
    if (v4l2::ioctl_retry(fd_, VIDIOC_QBUF, &buffer) != 0)
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

} // namespace cockscreen::runtime
