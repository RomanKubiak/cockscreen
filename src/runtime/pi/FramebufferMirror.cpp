#include "cockscreen/runtime/pi/FramebufferMirror.hpp"

#ifndef _WIN32

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

#include <QColor>
#include <QPainter>
#include <QRect>
#include <QSize>

#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace cockscreen::runtime::pi
{

namespace
{

std::uint32_t pack_channel(std::uint8_t channel, std::uint32_t length, std::uint32_t offset)
{
    if (length == 0)
    {
        return 0;
    }

    const std::uint32_t max_value = (1U << length) - 1U;
    const std::uint32_t scaled = (static_cast<std::uint32_t>(channel) * max_value + 127U) / 255U;
    return scaled << offset;
}

} // namespace

FramebufferMirror::FramebufferMirror(std::string device_path) : device_path_{std::move(device_path)}
{
    device_present_ = (::access(device_path_.c_str(), F_OK) == 0);
    if (!device_present_)
    {
        status_message_ = "secondary framebuffer not present";
        return;
    }

    fd_ = ::open(device_path_.c_str(), O_RDWR | O_CLOEXEC);
    if (fd_ < 0)
    {
        status_message_ = std::string{"open failed: "} + std::strerror(errno);
        return;
    }

    fb_fix_screeninfo fix_info{};
    fb_var_screeninfo var_info{};
    if (::ioctl(fd_, FBIOGET_FSCREENINFO, &fix_info) != 0 || ::ioctl(fd_, FBIOGET_VSCREENINFO, &var_info) != 0)
    {
        status_message_ = std::string{"framebuffer ioctl failed: "} + std::strerror(errno);
        close_device();
        return;
    }

    width_ = static_cast<int>(var_info.xres);
    height_ = static_cast<int>(var_info.yres);
    bits_per_pixel_ = static_cast<int>(var_info.bits_per_pixel);
    line_length_ = static_cast<int>(fix_info.line_length);
    red_offset_ = var_info.red.offset;
    red_length_ = var_info.red.length;
    green_offset_ = var_info.green.offset;
    green_length_ = var_info.green.length;
    blue_offset_ = var_info.blue.offset;
    blue_length_ = var_info.blue.length;
    mapped_size_ = static_cast<std::size_t>(fix_info.line_length) * static_cast<std::size_t>(var_info.yres_virtual);

    if (width_ <= 0 || height_ <= 0 || bits_per_pixel_ <= 0 || line_length_ <= 0 || mapped_size_ == 0)
    {
        status_message_ = "framebuffer reported invalid geometry";
        close_device();
        return;
    }

    mapped_ = ::mmap(nullptr, mapped_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (mapped_ == MAP_FAILED)
    {
        mapped_ = nullptr;
        status_message_ = std::string{"mmap failed: "} + std::strerror(errno);
        close_device();
        return;
    }

    status_message_ = "ready";
}

FramebufferMirror::~FramebufferMirror()
{
    close_device();
}

bool FramebufferMirror::ready() const
{
    return mapped_ != nullptr && fd_ >= 0;
}

bool FramebufferMirror::device_present() const
{
    return device_present_;
}

int FramebufferMirror::width() const
{
    return width_;
}

int FramebufferMirror::height() const
{
    return height_;
}

int FramebufferMirror::bits_per_pixel() const
{
    return bits_per_pixel_;
}

std::string_view FramebufferMirror::device_path() const
{
    return device_path_;
}

std::string_view FramebufferMirror::status_message() const
{
    return status_message_;
}

bool FramebufferMirror::present_video_frame(const QImage &source)
{
    if (!ready())
    {
        return false;
    }

    QImage canvas{std::max(width_, 1), std::max(height_, 1), QImage::Format_RGB32};
    canvas.fill(QColor{0, 0, 0});

    if (!source.isNull())
    {
        const QImage scaled =
            source.convertToFormat(QImage::Format_RGB32)
                .scaled(canvas.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        const QRect target_rect{(canvas.width() - scaled.width()) / 2, (canvas.height() - scaled.height()) / 2,
                                scaled.width(), scaled.height()};
        QPainter painter{&canvas};
        painter.drawImage(target_rect, scaled);
    }

    auto *dst_bytes = static_cast<std::uint8_t *>(mapped_);
    for (int y = 0; y < canvas.height(); ++y)
    {
        const QRgb *src_line = reinterpret_cast<const QRgb *>(canvas.constScanLine(y));
        std::uint8_t *dst_line = dst_bytes + static_cast<std::size_t>(y) * static_cast<std::size_t>(line_length_);

        if (bits_per_pixel_ == 16)
        {
            auto *dst_pixels = reinterpret_cast<std::uint16_t *>(dst_line);
            for (int x = 0; x < canvas.width(); ++x)
            {
                dst_pixels[x] = static_cast<std::uint16_t>(
                    pack_pixel(qRed(src_line[x]), qGreen(src_line[x]), qBlue(src_line[x])));
            }
        }
        else if (bits_per_pixel_ == 24)
        {
            for (int x = 0; x < canvas.width(); ++x)
            {
                const std::uint32_t packed = pack_pixel(qRed(src_line[x]), qGreen(src_line[x]), qBlue(src_line[x]));
                dst_line[x * 3 + 0] = static_cast<std::uint8_t>(packed & 0xffU);
                dst_line[x * 3 + 1] = static_cast<std::uint8_t>((packed >> 8U) & 0xffU);
                dst_line[x * 3 + 2] = static_cast<std::uint8_t>((packed >> 16U) & 0xffU);
            }
        }
        else if (bits_per_pixel_ == 32)
        {
            auto *dst_pixels = reinterpret_cast<std::uint32_t *>(dst_line);
            for (int x = 0; x < canvas.width(); ++x)
            {
                dst_pixels[x] = pack_pixel(qRed(src_line[x]), qGreen(src_line[x]), qBlue(src_line[x]));
            }
        }
        else
        {
            status_message_ = "unsupported framebuffer format";
            return false;
        }
    }

    return true;
}

void FramebufferMirror::clear()
{
    if (ready())
    {
        std::memset(mapped_, 0, mapped_size_);
    }
}

void FramebufferMirror::close_device()
{
    if (mapped_ != nullptr)
    {
        ::munmap(mapped_, mapped_size_);
        mapped_ = nullptr;
    }
    if (fd_ >= 0)
    {
        ::close(fd_);
        fd_ = -1;
    }
}

std::uint32_t FramebufferMirror::pack_pixel(std::uint8_t red, std::uint8_t green, std::uint8_t blue) const
{
    return pack_channel(red, red_length_, red_offset_) | pack_channel(green, green_length_, green_offset_)
        | pack_channel(blue, blue_length_, blue_offset_);
}

} // namespace cockscreen::runtime::pi

#endif // !_WIN32
