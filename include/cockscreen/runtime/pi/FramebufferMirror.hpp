#pragma once

#ifndef _WIN32

#include <cstddef>
#include <cstdint>
#include <string>

#include <QImage>

namespace cockscreen::runtime::pi
{

class FramebufferMirror
{
  public:
    explicit FramebufferMirror(std::string device_path = "/dev/fb1");
    ~FramebufferMirror();

    FramebufferMirror(const FramebufferMirror &) = delete;
    FramebufferMirror &operator=(const FramebufferMirror &) = delete;

    [[nodiscard]] bool ready() const;
    [[nodiscard]] bool device_present() const;
    [[nodiscard]] int width() const;
    [[nodiscard]] int height() const;
    [[nodiscard]] int bits_per_pixel() const;
    [[nodiscard]] std::string_view device_path() const;
    [[nodiscard]] std::string_view status_message() const;

    bool present_video_frame(const QImage &source);
    void clear();

  private:
    void close_device();
    std::uint32_t pack_pixel(std::uint8_t red, std::uint8_t green, std::uint8_t blue) const;

    std::string device_path_;
    std::string status_message_;
    int fd_{-1};
    void *mapped_{nullptr};
    std::size_t mapped_size_{0};
    int width_{0};
    int height_{0};
    int bits_per_pixel_{0};
    int line_length_{0};
    std::uint32_t red_offset_{0};
    std::uint32_t red_length_{0};
    std::uint32_t green_offset_{0};
    std::uint32_t green_length_{0};
    std::uint32_t blue_offset_{0};
    std::uint32_t blue_length_{0};
    bool device_present_{false};
};

} // namespace cockscreen::runtime::pi

#endif // !_WIN32
