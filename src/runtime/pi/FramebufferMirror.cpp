#include "cockscreen/runtime/pi/FramebufferMirror.hpp"

#ifndef _WIN32

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QRect>
#include <QSize>
#include <QString>

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

QColor scene_color_to_qcolor(const SceneColor &color)
{
    return QColor::fromRgbF(color.red, color.green, color.blue, color.alpha);
}

QString page_label(SecondaryDisplayPage page)
{
    switch (page)
    {
    case SecondaryDisplayPage::Mode:
        return QStringLiteral("mode");
    case SecondaryDisplayPage::VideoInput:
        return QStringLiteral("video input");
    case SecondaryDisplayPage::SystemPerformance:
        return QStringLiteral("system performance");
    case SecondaryDisplayPage::AppStatusModulation:
        return QStringLiteral("app status");
    }
    return QStringLiteral("video input");
}

QStringList modulation_lines(const core::ControlFrame &frame)
{
    QStringList lines;
    lines << QStringLiteral("gain %1").arg(frame.gain, 0, 'f', 2);
    lines << QStringLiteral("audio rms %1 peak %2 beat %3")
                 .arg(frame.audio_rms, 0, 'f', 2)
                 .arg(frame.audio_peak, 0, 'f', 2)
                 .arg(frame.audio_beat, 0, 'f', 2);
    lines << QStringLiteral("midi primary %1 secondary %2")
                 .arg(frame.midi_primary, 0, 'f', 2)
                 .arg(frame.midi_secondary, 0, 'f', 2);
    lines << QStringLiteral("osc x %1 y %2 values %3")
                 .arg(frame.osc_x, 0, 'f', 2)
                 .arg(frame.osc_y, 0, 'f', 2)
                 .arg(frame.osc_values.size());

    int shown_values = 0;
    for (const auto &[name, value] : frame.osc_values)
    {
        if (shown_values >= 4)
        {
            break;
        }
        lines << QStringLiteral("%1 %2").arg(QString::fromStdString(name)).arg(value, 0, 'f', 2);
        ++shown_values;
    }
    return lines;
}

void draw_text_page(QPainter &painter, const QRect &bounds, const QString &title, const QStringList &lines)
{
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    QFont title_font = painter.font();
    title_font.setPixelSize(16);
    title_font.setBold(true);
    painter.setFont(title_font);
    painter.setPen(QColor{240, 246, 255});
    painter.drawText(QRect{bounds.left() + 8, bounds.top() + 7, bounds.width() - 16, 20}, Qt::AlignLeft | Qt::AlignVCenter,
                     title);

    QFont body_font = painter.font();
    body_font.setPixelSize(11);
    body_font.setBold(false);
    painter.setFont(body_font);
    painter.setPen(QColor{190, 205, 218});

    const QFontMetrics metrics{body_font};
    int y = bounds.top() + 34;
    for (const QString &line : lines)
    {
        if (y + metrics.height() > bounds.bottom() - 6)
        {
            break;
        }
        const QString elided = metrics.elidedText(line, Qt::ElideRight, bounds.width() - 16);
        painter.drawText(QRect{bounds.left() + 8, y, bounds.width() - 16, metrics.height()}, Qt::AlignLeft | Qt::AlignVCenter,
                         elided);
        y += metrics.height() + 2;
    }
}

std::filesystem::path gpio_path(int gpio, const char *entry)
{
    return std::filesystem::path{"/sys/class/gpio"} / ("gpio" + std::to_string(gpio)) / entry;
}

bool write_text_file(const std::filesystem::path &path, const std::string &text)
{
    std::ofstream stream{path};
    if (!stream)
    {
        return false;
    }
    stream << text;
    return static_cast<bool>(stream);
}

} // namespace

FramebufferMirror::FramebufferMirror(std::string device_path)
    : FramebufferMirror{std::move(device_path), SceneSecondaryDisplay{}, false}
{
}

FramebufferMirror::FramebufferMirror(SceneSecondaryDisplay display, bool verbose_debug)
    : FramebufferMirror{display.device, std::move(display), verbose_debug}
{
}

FramebufferMirror::FramebufferMirror(std::string device_path, SceneSecondaryDisplay display, bool verbose_debug)
    : device_path_{std::move(device_path)}, display_{std::move(display)}, current_page_{display_.default_page},
      verbose_debug_{verbose_debug}
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
    initialize_controls();
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
    return present_frame(source, core::ControlFrame{}, {}, {});
}

bool FramebufferMirror::present_frame(const QImage &source, const core::ControlFrame &frame, const QStringList &system_lines,
                                      const QStringList &app_lines)
{
    if (!ready())
    {
        return false;
    }

    poll_controls();
    return write_canvas(orient_for_framebuffer(render_page(source, frame, system_lines, app_lines)));
}

void FramebufferMirror::clear()
{
    if (ready())
    {
        std::memset(mapped_, 0, mapped_size_);
    }
}

void FramebufferMirror::initialize_controls()
{
    controls_.clear();
    controls_.reserve(display_.controls.size());

    for (const auto &mapping : display_.controls)
    {
        if (mapping.gpio < 0)
        {
            continue;
        }

        GpioControl control{mapping};
        const auto value_path = gpio_path(mapping.gpio, "value");
        if (!std::filesystem::exists(value_path))
        {
            write_text_file("/sys/class/gpio/export", std::to_string(mapping.gpio));
        }
        write_text_file(gpio_path(mapping.gpio, "direction"), "in");
        control.available = std::filesystem::exists(value_path);
        control.last_pressed = control.available && read_gpio_pressed(control);
        controls_.push_back(std::move(control));
    }
}

void FramebufferMirror::poll_controls()
{
    for (auto &control : controls_)
    {
        if (!control.available)
        {
            continue;
        }
        const bool pressed = read_gpio_pressed(control);
        if (pressed && !control.last_pressed)
        {
            handle_control_press(control.mapping);
        }
        control.last_pressed = pressed;
    }
}

bool FramebufferMirror::read_gpio_pressed(const GpioControl &control) const
{
    std::ifstream stream{gpio_path(control.mapping.gpio, "value")};
    char value{'1'};
    stream >> value;
    if (!stream)
    {
        return false;
    }
    return control.mapping.active_low ? value == '0' : value != '0';
}

void FramebufferMirror::handle_control_press(const SecondaryDisplayControlMapping &mapping)
{
    const QString action = QString::fromStdString(mapping.action).trimmed().toLower().replace(QChar{'-'}, QChar{'_'});
    if (action == QStringLiteral("next_page") || action == QStringLiteral("next"))
    {
        current_page_ = adjacent_page(1);
    }
    else if (action == QStringLiteral("previous_page") || action == QStringLiteral("previous") ||
             action == QStringLiteral("prev"))
    {
        current_page_ = adjacent_page(-1);
    }
    else if (action == QStringLiteral("cycle_page") || action == QStringLiteral("cycle"))
    {
        current_page_ = adjacent_page(1);
    }
    else
    {
        current_page_ = mapping.page;
    }

    if (verbose_debug_)
    {
        std::cerr << "[secondary-display] key=" << mapping.control << " gpio=" << mapping.gpio
                  << " action=" << mapping.action << " page=" << page_label(mapping.page).toStdString()
                  << " selected=" << page_label(current_page_).toStdString() << '\n';
    }
}

SecondaryDisplayPage FramebufferMirror::adjacent_page(int delta) const
{
    const std::vector<SecondaryDisplayPage> pages{SecondaryDisplayPage::Mode, SecondaryDisplayPage::VideoInput,
                                                  SecondaryDisplayPage::SystemPerformance,
                                                  SecondaryDisplayPage::AppStatusModulation};
    auto it = std::find(pages.begin(), pages.end(), current_page_);
    const int current = it == pages.end() ? 0 : static_cast<int>(std::distance(pages.begin(), it));
    const int next = (current + delta + static_cast<int>(pages.size())) % static_cast<int>(pages.size());
    return pages[static_cast<std::size_t>(next)];
}

QImage FramebufferMirror::render_page(const QImage &source, const core::ControlFrame &frame,
                                      const QStringList &system_lines, const QStringList &app_lines) const
{
    const int logical_width =
        display_.render_target.enabled ? std::max(1, display_.render_target.width) : std::max(width_, 1);
    const int logical_height =
        display_.render_target.enabled ? std::max(1, display_.render_target.height) : std::max(height_, 1);

    QImage page{logical_width, logical_height, QImage::Format_RGB32};
    page.fill(scene_color_to_qcolor(display_.background_color));

    QPainter painter{&page};
    const QRect bounds{0, 0, page.width(), page.height()};

    if (current_page_ == SecondaryDisplayPage::VideoInput)
    {
        if (!source.isNull())
        {
            const Qt::TransformationMode filter =
                display_.render_target.filter == RenderTargetFilter::Nearest ? Qt::FastTransformation
                                                                              : Qt::SmoothTransformation;
            const QImage scaled = source.convertToFormat(QImage::Format_RGB32).scaled(page.size(), Qt::KeepAspectRatio, filter);
            const QRect target_rect{(page.width() - scaled.width()) / 2, (page.height() - scaled.height()) / 2,
                                    scaled.width(), scaled.height()};
            painter.setOpacity(display_.video_layer.enabled ? display_.video_layer.opacity : 0.0);
            painter.drawImage(target_rect, scaled);
            painter.setOpacity(1.0);
        }
        draw_text_page(painter, bounds, QStringLiteral("video input"), {});
        return page;
    }

    if (current_page_ == SecondaryDisplayPage::SystemPerformance)
    {
        draw_text_page(painter, bounds, QStringLiteral("system performance"), system_lines);
        return page;
    }

    if (current_page_ == SecondaryDisplayPage::AppStatusModulation)
    {
        QStringList lines = app_lines;
        lines << modulation_lines(frame);
        draw_text_page(painter, bounds, QStringLiteral("modulation status"), lines);
        return page;
    }

    QStringList mode_lines;
    mode_lines << QStringLiteral("current: %1").arg(page_label(current_page_));
    for (const auto &control : display_.controls)
    {
        const QString action = QString::fromStdString(control.action);
        mode_lines << QStringLiteral("%1 gpio%2 %3 %4")
                          .arg(QString::fromStdString(control.control))
                          .arg(control.gpio)
                          .arg(action)
                          .arg(page_label(control.page));
    }
    draw_text_page(painter, bounds, QStringLiteral("mode"), mode_lines);
    return page;
}

QImage FramebufferMirror::orient_for_framebuffer(const QImage &image) const
{
    QImage canvas{std::max(width_, 1), std::max(height_, 1), QImage::Format_RGB32};
    canvas.fill(scene_color_to_qcolor(display_.background_color));

    if (image.isNull())
    {
        return canvas;
    }

    const int rotation = ((display_.rotation_degrees % 360) + 360) % 360;
    const QSize staging_size = (rotation == 90 || rotation == 270) ? QSize{canvas.height(), canvas.width()} : canvas.size();
    QImage staging{staging_size, QImage::Format_RGB32};
    staging.fill(scene_color_to_qcolor(display_.background_color));

    const QImage scaled = image.convertToFormat(QImage::Format_RGB32).scaled(staging.size(), Qt::KeepAspectRatio, Qt::FastTransformation);
    {
        QPainter painter{&staging};
        const QRect target_rect{(staging.width() - scaled.width()) / 2, (staging.height() - scaled.height()) / 2,
                                scaled.width(), scaled.height()};
        painter.drawImage(target_rect, scaled);
    }

    QPainter painter{&canvas};
    switch (rotation)
    {
    case 90:
        painter.translate(canvas.width(), 0);
        painter.rotate(90.0);
        break;
    case 180:
        painter.translate(canvas.width(), canvas.height());
        painter.rotate(180.0);
        break;
    case 270:
        painter.translate(0, canvas.height());
        painter.rotate(270.0);
        break;
    default:
        break;
    }
    painter.drawImage(QPoint{0, 0}, staging);
    return canvas;
}

bool FramebufferMirror::write_canvas(const QImage &canvas)
{
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
