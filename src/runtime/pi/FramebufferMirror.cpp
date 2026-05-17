#include "cockscreen/runtime/pi/FramebufferMirror.hpp"

#ifndef _WIN32

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
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
#include <linux/i2c-dev.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/statvfs.h>

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

std::optional<int> read_int_file(const std::filesystem::path &path);
std::string read_string_file(const std::filesystem::path &path);

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
    case SecondaryDisplayPage::LinuxOsStats:
        return QStringLiteral("linux/os");
    }
    return QStringLiteral("video input");
}

QString normalized_token(const std::string &value)
{
    return QString::fromStdString(value).trimmed().toLower().replace(QChar{'_'}, QChar{'-'});
}

bool uses_ssd1309_i2c(const SceneSecondaryDisplay &display)
{
    const QString interface = normalized_token(display.interface);
    const QString model = normalized_token(display.model);
    const QString device = QString::fromStdString(display.device).trimmed().toLower();
    return interface == QStringLiteral("i2c") ||
           (device.startsWith(QStringLiteral("/dev/i2c")) && model.contains(QStringLiteral("ssd1309")));
}

bool uses_ssd1309_spidev(const SceneSecondaryDisplay &display)
{
    const QString interface = normalized_token(display.interface);
    const QString model = normalized_token(display.model);
    const QString device = QString::fromStdString(display.device).trimmed().toLower();
    return interface == QStringLiteral("spidev") ||
           (device.startsWith(QStringLiteral("/dev/spidev")) && model.contains(QStringLiteral("ssd1309")));
}

bool uses_ssd1309_direct(const SceneSecondaryDisplay &display)
{
    return uses_ssd1309_i2c(display) || uses_ssd1309_spidev(display);
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

void draw_compact_text_page(QPainter &painter, const QRect &bounds, const QString &title, const QStringList &lines)
{
    painter.setRenderHint(QPainter::TextAntialiasing, false);
    QFont title_font = painter.font();
    title_font.setPixelSize(8);
    title_font.setBold(true);
    painter.setFont(title_font);
    painter.setPen(QColor{255, 255, 255});
    painter.drawText(QRect{bounds.left() + 2, bounds.top(), bounds.width() - 4, 10}, Qt::AlignLeft | Qt::AlignVCenter,
                     title.toUpper());

    QFont body_font = painter.font();
    body_font.setPixelSize(7);
    body_font.setBold(false);
    painter.setFont(body_font);

    const QFontMetrics metrics{body_font};
    int y = bounds.top() + 12;
    for (const QString &line : lines)
    {
        if (y + metrics.height() > bounds.bottom())
        {
            break;
        }
        painter.drawText(QRect{bounds.left() + 2, y, bounds.width() - 4, metrics.height()}, Qt::AlignLeft | Qt::AlignVCenter,
                         metrics.elidedText(line, Qt::ElideRight, bounds.width() - 4));
        y += metrics.height();
    }
}

float clamp01(float value)
{
    return std::clamp(value, 0.0F, 1.0F);
}

QString first_metric_value(const QString &line, const QString &label)
{
    const int start = line.indexOf(label);
    if (start < 0)
    {
        return QStringLiteral("--");
    }
    const int value_start = start + label.size();
    int value_end = line.indexOf(QStringLiteral(" | "), value_start);
    if (value_end < 0)
    {
        value_end = line.size();
    }
    return line.mid(value_start, value_end - value_start).trimmed();
}

void draw_tile(QPainter &painter, const QRect &rect, const QString &label, const QString &value, const QColor &accent)
{
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor{18, 23, 28});
    painter.drawRoundedRect(rect, 5, 5);
    painter.setBrush(accent);
    painter.drawRect(QRect{rect.left(), rect.top(), 4, rect.height()});

    QFont label_font = painter.font();
    label_font.setPixelSize(10);
    label_font.setBold(true);
    painter.setFont(label_font);
    painter.setPen(QColor{155, 168, 180});
    painter.drawText(rect.adjusted(9, 5, -6, -rect.height() / 2), Qt::AlignLeft | Qt::AlignTop, label.toUpper());

    QFont value_font = painter.font();
    value_font.setPixelSize(24);
    value_font.setBold(true);
    painter.setFont(value_font);
    painter.setPen(QColor{245, 250, 255});
    painter.drawText(rect.adjusted(8, 17, -6, -4), Qt::AlignLeft | Qt::AlignVCenter, value);
}

void draw_meter(QPainter &painter, const QRect &rect, const QString &label, float value, const QColor &accent)
{
    const float clamped = clamp01(value);
    QFont font = painter.font();
    font.setPixelSize(11);
    font.setBold(true);
    painter.setFont(font);
    painter.setPen(QColor{215, 224, 232});
    painter.drawText(QRect{rect.left(), rect.top(), 58, rect.height()}, Qt::AlignLeft | Qt::AlignVCenter,
                     label.toUpper());

    const QRect track{rect.left() + 64, rect.top() + 5, rect.width() - 64, rect.height() - 10};
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor{29, 35, 41});
    painter.drawRoundedRect(track, 4, 4);
    QRect fill = track.adjusted(2, 2, -2, -2);
    fill.setWidth(static_cast<int>(static_cast<float>(fill.width()) * clamped));
    painter.setBrush(accent);
    painter.drawRoundedRect(fill, 3, 3);
}

void draw_compact_meter(QPainter &painter, const QRect &rect, const QString &label, float value)
{
    const float clamped = clamp01(value);
    QFont font = painter.font();
    font.setPixelSize(7);
    font.setBold(true);
    painter.setFont(font);
    painter.setPen(QColor{255, 255, 255});
    painter.drawText(QRect{rect.left(), rect.top(), 28, rect.height()}, Qt::AlignLeft | Qt::AlignVCenter,
                     label.toUpper());

    const QRect track{rect.left() + 30, rect.top() + 2, std::max(1, rect.width() - 30), std::max(1, rect.height() - 4)};
    painter.setPen(QColor{255, 255, 255});
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(track);
    QRect fill = track.adjusted(1, 1, -1, -1);
    fill.setWidth(static_cast<int>(static_cast<float>(fill.width()) * clamped));
    painter.fillRect(fill, QColor{255, 255, 255});
}

void draw_compact_kv(QPainter &painter, const QRect &rect, const QString &label, const QString &value)
{
    painter.setPen(QColor{255, 255, 255});
    painter.drawRect(rect);

    QFont label_font = painter.font();
    label_font.setPixelSize(7);
    label_font.setBold(false);
    painter.setFont(label_font);
    painter.drawText(rect.adjusted(2, 1, -2, -rect.height() / 2), Qt::AlignLeft | Qt::AlignTop, label.toUpper());

    QFont value_font = painter.font();
    value_font.setPixelSize(11);
    value_font.setBold(true);
    painter.setFont(value_font);
    painter.drawText(rect.adjusted(2, 10, -2, -1), Qt::AlignLeft | Qt::AlignVCenter,
                     QFontMetrics{value_font}.elidedText(value, Qt::ElideRight, rect.width() - 4));
}

std::optional<double> read_linux_temperature_c()
{
#if defined(__linux__)
    const std::filesystem::path thermal_root{"/sys/class/thermal"};
    std::error_code error;
    if (!std::filesystem::exists(thermal_root, error))
    {
        return std::nullopt;
    }

    for (const auto &entry : std::filesystem::directory_iterator{thermal_root, error})
    {
        if (error || !entry.is_directory())
        {
            break;
        }
        const auto type = read_string_file(entry.path() / "type");
        if (!type.empty() && type.find("cpu") == std::string::npos && type.find("soc") == std::string::npos &&
            type.find("thermal") == std::string::npos)
        {
            continue;
        }
        const auto milli_c = read_int_file(entry.path() / "temp");
        if (milli_c.has_value())
        {
            return static_cast<double>(*milli_c) / 1000.0;
        }
    }
#endif
    return std::nullopt;
}

struct StorageStats
{
    double used_percent{0.0};
    double total_gb{0.0};
    bool available{false};
};

StorageStats read_storage_stats(const char *path)
{
    StorageStats stats;
#if defined(__linux__)
    struct statvfs info
    {
    };
    if (::statvfs(path, &info) != 0 || info.f_blocks == 0)
    {
        return stats;
    }
    const double total = static_cast<double>(info.f_blocks) * static_cast<double>(info.f_frsize);
    const double free = static_cast<double>(info.f_bavail) * static_cast<double>(info.f_frsize);
    const double used = std::max(total - free, 0.0);
    stats.used_percent = std::clamp((used / total) * 100.0, 0.0, 100.0);
    stats.total_gb = total / (1024.0 * 1024.0 * 1024.0);
    stats.available = true;
#else
    (void)path;
#endif
    return stats;
}

std::optional<double> read_mmc_total_gb()
{
#if defined(__linux__)
    const auto sectors = read_int_file("/sys/block/mmcblk0/size");
    if (!sectors.has_value())
    {
        return std::nullopt;
    }
    return static_cast<double>(*sectors) * 512.0 / (1024.0 * 1024.0 * 1024.0);
#endif
    return std::nullopt;
}

void draw_header(QPainter &painter, const QRect &bounds, const QString &title)
{
    QFont title_font = painter.font();
    title_font.setPixelSize(15);
    title_font.setBold(true);
    painter.setFont(title_font);
    painter.setPen(QColor{236, 244, 252});
    painter.drawText(QRect{bounds.left() + 8, bounds.top() + 5, bounds.width() - 16, 20},
                     Qt::AlignLeft | Qt::AlignVCenter, title.toUpper());
}

void draw_performance_page(QPainter &painter, const QRect &bounds, const core::ControlFrame &frame,
                           const QStringList &system_lines)
{
    draw_header(painter, bounds, QStringLiteral("performance"));

    const QString fps_line = system_lines.isEmpty() ? QString{} : system_lines.front();
    QString process_fps = first_metric_value(fps_line, QStringLiteral("FPS process "));
    if (process_fps == QStringLiteral("--"))
    {
        process_fps = first_metric_value(fps_line, QStringLiteral("FPS capture "));
    }
    const QString render_fps = first_metric_value(fps_line, QStringLiteral("render "));

    draw_tile(painter, QRect{8, 31, 108, 55}, QStringLiteral("process"), process_fps, QColor{73, 196, 255});
    draw_tile(painter, QRect{124, 31, 108, 55}, QStringLiteral("render"), render_fps, QColor{92, 231, 165});
    draw_tile(painter, QRect{8, 94, 108, 50}, QStringLiteral("gain"), QString::number(frame.gain, 'f', 2),
              QColor{255, 198, 78});
    draw_tile(painter, QRect{124, 94, 108, 50}, QStringLiteral("beat"), QString::number(frame.audio_beat, 'f', 2),
              QColor{255, 103, 138});

    draw_meter(painter, QRect{8, 153, 224, 22}, QStringLiteral("rms"), frame.audio_rms, QColor{83, 184, 255});
    draw_meter(painter, QRect{8, 178, 224, 22}, QStringLiteral("peak"), frame.audio_peak, QColor{255, 116, 96});

    QFont font = painter.font();
    font.setPixelSize(10);
    font.setBold(false);
    painter.setFont(font);
    painter.setPen(QColor{142, 154, 166});
    const QString device_line = system_lines.size() > 1 ? system_lines.at(1) : QStringLiteral("video <unknown>");
    painter.drawText(QRect{8, 211, 224, 17}, Qt::AlignLeft | Qt::AlignVCenter,
                     QFontMetrics{font}.elidedText(device_line, Qt::ElideRight, 224));
}

void draw_xy_scope(QPainter &painter, const QRect &rect, float x, float y)
{
    painter.setPen(QPen{QColor{70, 82, 92}, 1});
    painter.setBrush(QColor{15, 19, 24});
    painter.drawRoundedRect(rect, 5, 5);
    painter.drawLine(rect.center().x(), rect.top() + 6, rect.center().x(), rect.bottom() - 6);
    painter.drawLine(rect.left() + 6, rect.center().y(), rect.right() - 6, rect.center().y());

    const int dot_x = rect.left() + 8 + static_cast<int>(clamp01(x) * static_cast<float>(rect.width() - 16));
    const int dot_y = rect.bottom() - 8 - static_cast<int>(clamp01(y) * static_cast<float>(rect.height() - 16));
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor{255, 219, 92});
    painter.drawEllipse(QPoint{dot_x, dot_y}, 6, 6);
}

void draw_modulation_page(QPainter &painter, const QRect &bounds, const core::ControlFrame &frame,
                          const QStringList &app_lines)
{
    draw_header(painter, bounds, QStringLiteral("modulation"));

    draw_meter(painter, QRect{8, 31, 224, 24}, QStringLiteral("audio"), frame.audio_level, QColor{83, 184, 255});
    draw_meter(painter, QRect{8, 58, 224, 24}, QStringLiteral("midi 1"), frame.midi_primary, QColor{210, 130, 255});
    draw_meter(painter, QRect{8, 85, 224, 24}, QStringLiteral("midi 2"), frame.midi_secondary, QColor{143, 221, 126});

    draw_xy_scope(painter, QRect{8, 119, 104, 86}, frame.osc_x, frame.osc_y);
    draw_tile(painter, QRect{122, 119, 110, 40}, QStringLiteral("osc x"), QString::number(frame.osc_x, 'f', 2),
              QColor{255, 219, 92});
    draw_tile(painter, QRect{122, 166, 110, 40}, QStringLiteral("osc y"), QString::number(frame.osc_y, 'f', 2),
              QColor{255, 219, 92});

    QFont font = painter.font();
    font.setPixelSize(10);
    font.setBold(true);
    painter.setFont(font);
    painter.setPen(QColor{151, 164, 176});
    const QString app_line = app_lines.isEmpty() ? QStringLiteral("osc/midi/adc status") : app_lines.front();
    const QString count_line = QStringLiteral("OSC values %1").arg(frame.osc_values.size());
    painter.drawText(QRect{8, 211, 111, 17}, Qt::AlignLeft | Qt::AlignVCenter, count_line);
    painter.drawText(QRect{122, 211, 110, 17}, Qt::AlignRight | Qt::AlignVCenter,
                     QFontMetrics{font}.elidedText(app_line, Qt::ElideLeft, 110));
}

void draw_linux_os_page(QPainter &painter, const QRect &bounds, const SystemMetricsSnapshot &metrics,
                        const QStringList &system_lines)
{
    draw_header(painter, bounds, QStringLiteral("linux/os"));

    const QString fps_line = system_lines.isEmpty() ? QString{} : system_lines.front();
    QString process_fps = first_metric_value(fps_line, QStringLiteral("FPS process "));
    if (process_fps == QStringLiteral("--"))
    {
        process_fps = first_metric_value(fps_line, QStringLiteral("FPS capture "));
    }
    const QString render_fps = first_metric_value(fps_line, QStringLiteral("render "));

    const auto temp_c = read_linux_temperature_c();
    const StorageStats root = read_storage_stats("/");
    const auto mmc_total_gb = read_mmc_total_gb();

    draw_tile(painter, QRect{8, 31, 70, 45}, QStringLiteral("cpu"), metrics.available ? QString::number(metrics.cpu_percent, 'f', 0) + QStringLiteral("%")
                                                                                       : QStringLiteral("--"),
              QColor{73, 196, 255});
    draw_tile(painter, QRect{85, 31, 70, 45}, QStringLiteral("mem"), metrics.available ? QString::number(metrics.memory_percent, 'f', 0) + QStringLiteral("%")
                                                                                       : QStringLiteral("--"),
              QColor{92, 231, 165});
    draw_tile(painter, QRect{162, 31, 70, 45}, QStringLiteral("temp"), temp_c.has_value() ? QString::number(*temp_c, 'f', 0) + QStringLiteral("C")
                                                                                         : QStringLiteral("--"),
              QColor{255, 116, 96});

    draw_meter(painter, QRect{8, 86, 224, 24}, QStringLiteral("cpu"), metrics.available ? static_cast<float>(metrics.cpu_percent / 100.0) : 0.0F,
               QColor{73, 196, 255});
    draw_meter(painter, QRect{8, 113, 224, 24}, QStringLiteral("mem"), metrics.available ? static_cast<float>(metrics.memory_percent / 100.0) : 0.0F,
               QColor{92, 231, 165});
    draw_meter(painter, QRect{8, 140, 224, 24}, QStringLiteral("root"), root.available ? static_cast<float>(root.used_percent / 100.0) : 0.0F,
               QColor{255, 198, 78});

    draw_tile(painter, QRect{8, 174, 108, 42}, QStringLiteral("render"), render_fps, QColor{210, 130, 255});
    draw_tile(painter, QRect{124, 174, 108, 42}, QStringLiteral("process"), process_fps, QColor{143, 221, 126});

    QFont font = painter.font();
    font.setPixelSize(10);
    font.setBold(true);
    painter.setFont(font);
    painter.setPen(QColor{151, 164, 176});
    const QString mmc_text = mmc_total_gb.has_value()
                                 ? QStringLiteral("mmc %1GB root %2%")
                                       .arg(*mmc_total_gb, 0, 'f', 1)
                                       .arg(root.used_percent, 0, 'f', 0)
                                 : QStringLiteral("root %1GB %2%")
                                       .arg(root.total_gb, 0, 'f', 1)
                                       .arg(root.used_percent, 0, 'f', 0);
    painter.drawText(QRect{8, 219, 224, 15}, Qt::AlignLeft | Qt::AlignVCenter, mmc_text);
}

void draw_compact_performance_page(QPainter &painter, const QRect &bounds, const core::ControlFrame &frame,
                                   const QStringList &system_lines)
{
    const QString fps_line = system_lines.isEmpty() ? QString{} : system_lines.front();
    QString process_fps = first_metric_value(fps_line, QStringLiteral("FPS process "));
    if (process_fps == QStringLiteral("--"))
    {
        process_fps = first_metric_value(fps_line, QStringLiteral("FPS capture "));
    }
    const QString render_fps = first_metric_value(fps_line, QStringLiteral("render "));

    draw_compact_kv(painter, QRect{2, 1, 40, 25}, QStringLiteral("proc"), process_fps);
    draw_compact_kv(painter, QRect{44, 1, 40, 25}, QStringLiteral("rend"), render_fps);
    draw_compact_kv(painter, QRect{86, 1, 40, 25}, QStringLiteral("gain"), QString::number(frame.gain, 'f', 1));
    draw_compact_meter(painter, QRect{2, 31, bounds.width() - 4, 12}, QStringLiteral("rms"), frame.audio_rms);
    draw_compact_meter(painter, QRect{2, 46, bounds.width() - 4, 12}, QStringLiteral("peak"), frame.audio_peak);
}

void draw_compact_modulation_page(QPainter &painter, const QRect &bounds, const core::ControlFrame &frame)
{
    draw_compact_meter(painter, QRect{2, 3, bounds.width() - 4, 12}, QStringLiteral("aud"), frame.audio_level);
    draw_compact_meter(painter, QRect{2, 18, bounds.width() - 4, 12}, QStringLiteral("mid1"), frame.midi_primary);
    draw_compact_meter(painter, QRect{2, 33, bounds.width() - 4, 12}, QStringLiteral("mid2"), frame.midi_secondary);
    draw_compact_kv(painter, QRect{2, 48, 61, 15}, QStringLiteral("oscx"), QString::number(frame.osc_x, 'f', 2));
    draw_compact_kv(painter, QRect{65, 48, 61, 15}, QStringLiteral("oscy"), QString::number(frame.osc_y, 'f', 2));
}

void draw_compact_linux_os_page(QPainter &painter, const QRect &bounds, const SystemMetricsSnapshot &metrics,
                                const QStringList &system_lines)
{
    const QString fps_line = system_lines.isEmpty() ? QString{} : system_lines.front();
    const QString render_fps = first_metric_value(fps_line, QStringLiteral("render "));
    const auto temp_c = read_linux_temperature_c();
    const StorageStats root = read_storage_stats("/");

    draw_compact_kv(painter, QRect{2, 1, 40, 25}, QStringLiteral("cpu"),
                    metrics.available ? QString::number(metrics.cpu_percent, 'f', 0) + QStringLiteral("%")
                                      : QStringLiteral("--"));
    draw_compact_kv(painter, QRect{44, 1, 40, 25}, QStringLiteral("mem"),
                    metrics.available ? QString::number(metrics.memory_percent, 'f', 0) + QStringLiteral("%")
                                      : QStringLiteral("--"));
    draw_compact_kv(painter, QRect{86, 1, 40, 25}, QStringLiteral("temp"),
                    temp_c.has_value() ? QString::number(*temp_c, 'f', 0) + QStringLiteral("C") : QStringLiteral("--"));
    draw_compact_meter(painter, QRect{2, 31, bounds.width() - 4, 12}, QStringLiteral("root"),
                       root.available ? static_cast<float>(root.used_percent / 100.0) : 0.0F);
    draw_compact_kv(painter, QRect{2, 47, 61, 16}, QStringLiteral("rend"), render_fps);
    draw_compact_kv(painter, QRect{65, 47, 61, 16}, QStringLiteral("disk"),
                    root.available ? QString::number(root.total_gb, 'f', 1) + QStringLiteral("G") : QStringLiteral("--"));
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

std::optional<int> read_int_file(const std::filesystem::path &path)
{
    std::ifstream stream{path};
    int value{0};
    stream >> value;
    if (!stream)
    {
        return std::nullopt;
    }
    return value;
}

std::string read_string_file(const std::filesystem::path &path)
{
    std::ifstream stream{path};
    std::string value;
    std::getline(stream, value);
    return value;
}

int resolve_sysfs_gpio(int bcm_gpio)
{
    if (std::filesystem::exists(gpio_path(bcm_gpio, "value")))
    {
        return bcm_gpio;
    }

    const std::filesystem::path gpio_root{"/sys/class/gpio"};
    for (const auto &entry : std::filesystem::directory_iterator{gpio_root})
    {
        const std::string filename = entry.path().filename().string();
        if (filename.rfind("gpiochip", 0) != 0)
        {
            continue;
        }

        const std::filesystem::path chip_path = entry.path();
        const std::string label = read_string_file(chip_path / "label");
        if (label.find("pinctrl") == std::string::npos && label.find("bcm") == std::string::npos)
        {
            continue;
        }

        const auto base = read_int_file(chip_path / "base");
        const auto ngpio = read_int_file(chip_path / "ngpio");
        if (!base.has_value() || !ngpio.has_value() || bcm_gpio < 0 || bcm_gpio >= *ngpio)
        {
            continue;
        }
        return *base + bcm_gpio;
    }

    return bcm_gpio;
}

void configure_input_pullups(const std::vector<SecondaryDisplayControlMapping> &controls, bool verbose_debug)
{
    std::unordered_set<int> gpio_set;
    for (const auto &control : controls)
    {
        if (control.gpio >= 0 && control.active_low)
        {
            gpio_set.insert(control.gpio);
        }
    }
    if (gpio_set.empty())
    {
        return;
    }

    std::vector<int> gpios{gpio_set.begin(), gpio_set.end()};
    std::sort(gpios.begin(), gpios.end());

    std::ostringstream command;
    command << "command -v pinctrl >/dev/null 2>&1 && pinctrl set ";
    for (std::size_t i = 0; i < gpios.size(); ++i)
    {
        if (i > 0)
        {
            command << ',';
        }
        command << gpios[i];
    }
    command << " ip pu >/dev/null 2>&1";

    const int result = std::system(command.str().c_str());
    if (verbose_debug)
    {
        std::cerr << "[secondary-display] input pull-up setup for BCM GPIOs";
        for (const int gpio : gpios)
        {
            std::cerr << ' ' << gpio;
        }
        std::cerr << (result == 0 ? " succeeded" : " skipped/failed") << '\n';
    }
}

bool configure_output_gpio(int bcm_gpio, bool high)
{
    if (bcm_gpio < 0)
    {
        return true;
    }
    const int sysfs_gpio = resolve_sysfs_gpio(bcm_gpio);
    const auto value_path = gpio_path(sysfs_gpio, "value");
    if (!std::filesystem::exists(value_path))
    {
        write_text_file("/sys/class/gpio/export", std::to_string(sysfs_gpio));
    }
    write_text_file(gpio_path(sysfs_gpio, "direction"), "out");
    return write_text_file(value_path, high ? "1" : "0");
}

bool set_output_gpio(int bcm_gpio, bool high)
{
    if (bcm_gpio < 0)
    {
        return true;
    }
    return write_text_file(gpio_path(resolve_sysfs_gpio(bcm_gpio), "value"), high ? "1" : "0");
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
        status_message_ = uses_ssd1309_direct(display_) ? "secondary SSD1309 device not present"
                                                        : "secondary framebuffer not present";
        return;
    }

    if (uses_ssd1309_direct(display_))
    {
        spidev_backend_ = uses_ssd1309_spidev(display_);
        i2c_backend_ = uses_ssd1309_i2c(display_);
        fd_ = ::open(device_path_.c_str(), O_RDWR | O_CLOEXEC);
        if (fd_ < 0)
        {
            status_message_ = std::string{"open failed: "} + std::strerror(errno);
            return;
        }
        width_ = std::max(1, display_.width);
        height_ = std::max(1, display_.height);
        bits_per_pixel_ = 1;
        line_length_ = (width_ + 7) / 8;
        const bool initialized = spidev_backend_ ? initialize_spidev() : initialize_i2c();
        if (!initialized)
        {
            close_device();
            return;
        }
        status_message_ = "ready";
        initialize_controls();
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
    if (spidev_backend_ || i2c_backend_)
    {
        return fd_ >= 0;
    }
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
    if (ready() && (spidev_backend_ || i2c_backend_))
    {
        QImage canvas{std::max(width_, 1), std::max(height_, 1), QImage::Format_RGB32};
        canvas.fill(Qt::black);
        write_canvas(canvas);
    }
    else if (ready())
    {
        std::memset(mapped_, 0, mapped_size_);
    }
}

void FramebufferMirror::initialize_controls()
{
    controls_.clear();
    controls_.reserve(display_.controls.size());
    configure_input_pullups(display_.controls, verbose_debug_);

    for (const auto &mapping : display_.controls)
    {
        if (mapping.gpio < 0)
        {
            continue;
        }

        GpioControl control;
        control.mapping = mapping;
        control.sysfs_gpio = resolve_sysfs_gpio(mapping.gpio);
        const auto value_path = gpio_path(control.sysfs_gpio, "value");
        if (!std::filesystem::exists(value_path))
        {
            write_text_file("/sys/class/gpio/export", std::to_string(control.sysfs_gpio));
        }
        write_text_file(gpio_path(control.sysfs_gpio, "direction"), "in");
        control.available = std::filesystem::exists(value_path);
        if (verbose_debug_)
        {
            std::cerr << "[secondary-display] control=" << mapping.control << " bcm_gpio=" << mapping.gpio
                      << " sysfs_gpio=" << control.sysfs_gpio
                      << (control.available ? " available" : " unavailable") << '\n';
        }
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
    std::ifstream stream{gpio_path(control.sysfs_gpio, "value")};
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
                                                  SecondaryDisplayPage::AppStatusModulation,
                                                  SecondaryDisplayPage::LinuxOsStats};
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
        if (logical_width > 160 && logical_height > 80)
        {
            draw_text_page(painter, bounds, QStringLiteral("video input"), {});
        }
        return page;
    }

    if (current_page_ == SecondaryDisplayPage::SystemPerformance)
    {
        if (logical_width <= 160 || logical_height <= 80)
        {
            draw_compact_performance_page(painter, bounds, frame, system_lines);
            return page;
        }
        draw_performance_page(painter, bounds, frame, system_lines);
        return page;
    }

    if (current_page_ == SecondaryDisplayPage::AppStatusModulation)
    {
        if (logical_width <= 160 || logical_height <= 80)
        {
            draw_compact_modulation_page(painter, bounds, frame);
            return page;
        }
        draw_modulation_page(painter, bounds, frame, app_lines);
        return page;
    }

    if (current_page_ == SecondaryDisplayPage::LinuxOsStats)
    {
        if (logical_width <= 160 || logical_height <= 80)
        {
            draw_compact_linux_os_page(painter, bounds, system_metrics_.sample(), system_lines);
            return page;
        }
        draw_linux_os_page(painter, bounds, system_metrics_.sample(), system_lines);
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
    if (logical_width <= 160 || logical_height <= 80)
    {
        draw_compact_text_page(painter, bounds, QStringLiteral("mode"), mode_lines);
    }
    else
    {
        draw_text_page(painter, bounds, QStringLiteral("mode"), mode_lines);
    }
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
    if (spidev_backend_)
    {
        return write_spidev_canvas(canvas);
    }
    if (i2c_backend_)
    {
        return write_i2c_canvas(canvas);
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

bool FramebufferMirror::initialize_spidev()
{
    std::uint8_t mode = SPI_MODE_3;
    std::uint8_t bits_per_word = 8;
    std::uint32_t speed = static_cast<std::uint32_t>(std::max(1, display_.spi_speed_hz));
    if (::ioctl(fd_, SPI_IOC_WR_MODE, &mode) != 0 ||
        ::ioctl(fd_, SPI_IOC_WR_BITS_PER_WORD, &bits_per_word) != 0 ||
        ::ioctl(fd_, SPI_IOC_WR_MAX_SPEED_HZ, &speed) != 0)
    {
        status_message_ = std::string{"SPI setup failed: "} + std::strerror(errno);
        return false;
    }

    if (display_.gpio_dc < 0)
    {
        status_message_ = "SSD1309 SPI requires gpio_dc";
        return false;
    }
    if (!configure_output_gpio(display_.gpio_dc, true) || !configure_output_gpio(display_.gpio_reset, true))
    {
        status_message_ = "SSD1309 GPIO setup failed";
        return false;
    }

    if (display_.gpio_reset >= 0)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        set_output_gpio(display_.gpio_reset, false);
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
        set_output_gpio(display_.gpio_reset, true);
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
    }

    QImage blank{std::max(width_, 1), std::max(height_, 1), QImage::Format_RGB32};
    blank.fill(Qt::black);

    // SparkFun UG2856KLBAG01 transparent OLED / SSD1309 recommended setup.
    return write_spidev_command({0xFD, 0x12}) && // command lock off
           write_spidev_command({0xAE}) &&       // display off
           write_spidev_command({0xD5, 0xA0}) &&
           write_spidev_command({0xA8, 0x3F}) &&
           write_spidev_command({0xD3, 0x00}) &&
           write_spidev_command({0x40}) &&
           write_spidev_command({0x20, 0x00}) && // horizontal addressing
           write_spidev_command({0xA1}) &&
           write_spidev_command({0xC8}) &&
           write_spidev_command({0xDA, 0x12}) &&
           write_spidev_command({0x81, 0x8F}) &&
           write_spidev_command({0xD9, 0x25}) &&
           write_spidev_command({0xDB, 0x34}) &&
           write_spidev_command({0xA4}) &&
           write_spidev_command({0xA6}) &&
           write_spidev_canvas(blank) &&
           write_spidev_command({0xAF});
}

bool FramebufferMirror::initialize_i2c()
{
    if (::ioctl(fd_, I2C_SLAVE, display_.i2c_address) != 0)
    {
        status_message_ = std::string{"I2C address setup failed: "} + std::strerror(errno);
        return false;
    }

    if (!configure_output_gpio(display_.gpio_reset, true))
    {
        status_message_ = "SSD1309 reset GPIO setup failed";
        return false;
    }

    if (display_.gpio_reset >= 0)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        set_output_gpio(display_.gpio_reset, false);
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
        set_output_gpio(display_.gpio_reset, true);
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
    }

    QImage blank{std::max(width_, 1), std::max(height_, 1), QImage::Format_RGB32};
    blank.fill(Qt::black);

    // SparkFun UG2856KLBAG01 transparent OLED / SSD1309 recommended setup.
    return write_i2c_command({0xFD, 0x12}) && // command lock off
           write_i2c_command({0xAE}) &&       // display off
           write_i2c_command({0xD5, 0xA0}) &&
           write_i2c_command({0xA8, 0x3F}) &&
           write_i2c_command({0xD3, 0x00}) &&
           write_i2c_command({0x40}) &&
           write_i2c_command({0x20, 0x00}) && // horizontal addressing
           write_i2c_command({0xA1}) &&
           write_i2c_command({0xC8}) &&
           write_i2c_command({0xDA, 0x12}) &&
           write_i2c_command({0x81, 0x8F}) &&
           write_i2c_command({0xD9, 0x25}) &&
           write_i2c_command({0xDB, 0x34}) &&
           write_i2c_command({0xA4}) &&
           write_i2c_command({0xA6}) &&
           write_i2c_canvas(blank) &&
           write_i2c_command({0xAF});
}

bool FramebufferMirror::write_spidev_command(std::initializer_list<std::uint8_t> bytes)
{
    std::vector<std::uint8_t> buffer{bytes};
    set_output_gpio(display_.gpio_dc, false);
    spi_ioc_transfer transfer{};
    transfer.tx_buf = reinterpret_cast<unsigned long>(buffer.data());
    transfer.len = static_cast<__u32>(buffer.size());
    transfer.speed_hz = static_cast<__u32>(std::max(1, display_.spi_speed_hz));
    transfer.bits_per_word = 8;
    if (::ioctl(fd_, SPI_IOC_MESSAGE(1), &transfer) < 0)
    {
        status_message_ = std::string{"SPI command failed: "} + std::strerror(errno);
        return false;
    }
    return true;
}

bool FramebufferMirror::write_spidev_data(const std::vector<std::uint8_t> &bytes)
{
    if (bytes.empty())
    {
        return true;
    }
    set_output_gpio(display_.gpio_dc, true);
    spi_ioc_transfer transfer{};
    transfer.tx_buf = reinterpret_cast<unsigned long>(bytes.data());
    transfer.len = static_cast<__u32>(bytes.size());
    transfer.speed_hz = static_cast<__u32>(std::max(1, display_.spi_speed_hz));
    transfer.bits_per_word = 8;
    if (::ioctl(fd_, SPI_IOC_MESSAGE(1), &transfer) < 0)
    {
        status_message_ = std::string{"SPI data failed: "} + std::strerror(errno);
        return false;
    }
    return true;
}

bool FramebufferMirror::write_spidev_canvas(const QImage &canvas)
{
    if (canvas.isNull())
    {
        return false;
    }

    const QImage source = canvas.convertToFormat(QImage::Format_RGB32).scaled(QSize{width_, height_}, Qt::IgnoreAspectRatio,
                                                                              Qt::FastTransformation);
    std::vector<std::uint8_t> pages(static_cast<std::size_t>(width_) * static_cast<std::size_t>((height_ + 7) / 8), 0);
    for (int y = 0; y < height_; ++y)
    {
        const QRgb *line = reinterpret_cast<const QRgb *>(source.constScanLine(y));
        for (int x = 0; x < width_; ++x)
        {
            const int luma = (qRed(line[x]) * 299 + qGreen(line[x]) * 587 + qBlue(line[x]) * 114) / 1000;
            if (luma >= 96)
            {
                pages[static_cast<std::size_t>((y / 8) * width_ + x)] |= static_cast<std::uint8_t>(1U << (y % 8));
            }
        }
    }

    return write_spidev_command({0x21, 0x00, static_cast<std::uint8_t>(std::max(0, width_ - 1))}) &&
           write_spidev_command({0x22, 0x00, static_cast<std::uint8_t>(std::max(0, ((height_ + 7) / 8) - 1))}) &&
           write_spidev_data(pages);
}

bool FramebufferMirror::write_i2c_command(std::initializer_list<std::uint8_t> bytes)
{
    std::vector<std::uint8_t> buffer;
    buffer.reserve(bytes.size() + 1);
    buffer.push_back(0x00);
    buffer.insert(buffer.end(), bytes.begin(), bytes.end());
    if (::write(fd_, buffer.data(), buffer.size()) != static_cast<ssize_t>(buffer.size()))
    {
        status_message_ = std::string{"I2C command failed: "} + std::strerror(errno);
        return false;
    }
    return true;
}

bool FramebufferMirror::write_i2c_data(const std::vector<std::uint8_t> &bytes)
{
    constexpr std::size_t kChunkBytes = 16;
    std::vector<std::uint8_t> buffer;
    buffer.reserve(kChunkBytes + 1);
    for (std::size_t offset = 0; offset < bytes.size(); offset += kChunkBytes)
    {
        const std::size_t count = std::min(kChunkBytes, bytes.size() - offset);
        buffer.clear();
        buffer.push_back(0x40);
        buffer.insert(buffer.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                      bytes.begin() + static_cast<std::ptrdiff_t>(offset + count));
        if (::write(fd_, buffer.data(), buffer.size()) != static_cast<ssize_t>(buffer.size()))
        {
            status_message_ = std::string{"I2C data failed: "} + std::strerror(errno);
            return false;
        }
    }
    return true;
}

bool FramebufferMirror::write_i2c_canvas(const QImage &canvas)
{
    if (canvas.isNull())
    {
        return false;
    }

    const QImage source = canvas.convertToFormat(QImage::Format_RGB32).scaled(QSize{width_, height_}, Qt::IgnoreAspectRatio,
                                                                              Qt::FastTransformation);
    std::vector<std::uint8_t> pages(static_cast<std::size_t>(width_) * static_cast<std::size_t>((height_ + 7) / 8), 0);
    for (int y = 0; y < height_; ++y)
    {
        const QRgb *line = reinterpret_cast<const QRgb *>(source.constScanLine(y));
        for (int x = 0; x < width_; ++x)
        {
            const int luma = (qRed(line[x]) * 299 + qGreen(line[x]) * 587 + qBlue(line[x]) * 114) / 1000;
            if (luma >= 96)
            {
                pages[static_cast<std::size_t>((y / 8) * width_ + x)] |= static_cast<std::uint8_t>(1U << (y % 8));
            }
        }
    }

    return write_i2c_command({0x21, 0x00, static_cast<std::uint8_t>(std::max(0, width_ - 1))}) &&
           write_i2c_command({0x22, 0x00, static_cast<std::uint8_t>(std::max(0, ((height_ + 7) / 8) - 1))}) &&
           write_i2c_data(pages);
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
