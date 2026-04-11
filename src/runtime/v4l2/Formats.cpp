#include "cockscreen/runtime/V4l2Capture.hpp"

#include "cockscreen/runtime/v4l2/Support.hpp"

#include <linux/videodev2.h>

#include <algorithm>
#include <array>
#include <fcntl.h>
#include <unistd.h>

namespace cockscreen::runtime::v4l2
{

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

} // namespace cockscreen::runtime::v4l2

namespace cockscreen::runtime
{

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
    if (v4l2::ioctl_retry(fd, VIDIOC_QUERYCAP, &capability) != 0 || (capability.capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0)
    {
        ::close(fd);
        return result;
    }

    for (unsigned int format_index = 0;; ++format_index)
    {
        v4l2_fmtdesc format{};
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        format.index = format_index;
        if (v4l2::ioctl_retry(fd, VIDIOC_ENUM_FMT, &format) != 0)
        {
            break;
        }

        const auto fourcc = v4l2::fourcc_to_string(format.pixelformat);
        const std::string description{reinterpret_cast<const char *>(format.description)};

        bool found_size = false;
        for (unsigned int size_index = 0;; ++size_index)
        {
            v4l2_frmsizeenum frame_size{};
            frame_size.pixel_format = format.pixelformat;
            frame_size.index = size_index;
            if (v4l2::ioctl_retry(fd, VIDIOC_ENUM_FRAMESIZES, &frame_size) != 0)
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

        if (v4l2::ioctl_retry(fd_, VIDIOC_S_FMT, &format) != 0)
        {
            continue;
        }

        pixel_format_ = v4l2::to_pixel_format(format.fmt.pix.pixelformat);
        if (pixel_format_ == V4l2PixelFormat::unsupported)
        {
            continue;
        }

        width_ = static_cast<int>(format.fmt.pix.width);
        height_ = static_cast<int>(format.fmt.pix.height);
        stride_ = static_cast<int>(format.fmt.pix.bytesperline);
        format_label_ = v4l2::to_format_label(pixel_format_, width_, height_);
        return true;
    }

    error_message_ = "Failed to negotiate a supported V4L2 pixel format";
    return false;
}

} // namespace cockscreen::runtime