#pragma once

#include "cockscreen/runtime/V4l2Capture.hpp"

namespace cockscreen::runtime::v4l2
{

int ioctl_retry(int fd, unsigned long request, void *arg);
V4l2PixelFormat to_pixel_format(std::uint32_t value);
std::string to_format_label(V4l2PixelFormat pixel_format, int width, int height);
std::string fourcc_to_string(std::uint32_t fourcc);

} // namespace cockscreen::runtime::v4l2