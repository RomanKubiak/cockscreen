#pragma once

#ifndef _WIN32

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <QStringList>
#include <QImage>

#include "cockscreen/core/ControlFrame.hpp"
#include "cockscreen/runtime/RuntimeHelpers.hpp"
#include "cockscreen/runtime/Scene.hpp"

namespace cockscreen::runtime::pi
{

class FramebufferMirror
{
  public:
    explicit FramebufferMirror(std::string device_path = "/dev/fb1");
    explicit FramebufferMirror(SceneSecondaryDisplay display, bool verbose_debug = false);
    FramebufferMirror(std::string device_path, SceneSecondaryDisplay display, bool verbose_debug = false);
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
    bool present_frame(const QImage &source, const core::ControlFrame &frame, const QStringList &system_lines,
                       const QStringList &app_lines);
    void clear();

  private:
    struct GpioControl
    {
        SecondaryDisplayControlMapping mapping;
        int sysfs_gpio{-1};
        bool available{false};
        bool last_pressed{false};
    };

    void close_device();
    void initialize_controls();
    void poll_controls();
    [[nodiscard]] bool read_gpio_pressed(const GpioControl &control) const;
    void handle_control_press(const SecondaryDisplayControlMapping &mapping);
    [[nodiscard]] SecondaryDisplayPage adjacent_page(int delta) const;
    [[nodiscard]] QImage render_page(const QImage &source, const core::ControlFrame &frame,
                                     const QStringList &system_lines, const QStringList &app_lines) const;
    [[nodiscard]] QImage orient_for_framebuffer(const QImage &image) const;
    bool write_canvas(const QImage &canvas);
    std::uint32_t pack_pixel(std::uint8_t red, std::uint8_t green, std::uint8_t blue) const;

    std::string device_path_;
    std::string status_message_;
    SceneSecondaryDisplay display_;
    SecondaryDisplayPage current_page_{SecondaryDisplayPage::VideoInput};
    std::vector<GpioControl> controls_;
    mutable SystemMetricsSampler system_metrics_;
    bool verbose_debug_{false};
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
