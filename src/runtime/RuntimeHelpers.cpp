#include "../../include/cockscreen/runtime/RuntimeHelpers.hpp"

#include <QMediaDevices>
#include <QProcess>

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>

namespace cockscreen::runtime
{

namespace
{

constexpr std::string_view kPiZero2W{"raspberry-pi-zero-2w"};

std::optional<QString> detect_default_output_monitor_source_name()
{
    QProcess info_process;
    info_process.start(QStringLiteral("pactl"), QStringList{QStringLiteral("info")});
    if (!info_process.waitForFinished(1000) || info_process.exitStatus() != QProcess::NormalExit ||
        info_process.exitCode() != 0)
    {
        return std::nullopt;
    }

    const QString info_output = QString::fromUtf8(info_process.readAllStandardOutput());
    QString default_sink;
    for (const auto &line : info_output.split('\n', Qt::SkipEmptyParts))
    {
        if (line.startsWith(QStringLiteral("Default Sink: ")))
        {
            default_sink = line.mid(QStringLiteral("Default Sink: ").size()).trimmed();
            break;
        }
    }

    if (default_sink.isEmpty())
    {
        return std::nullopt;
    }

    QProcess sources_process;
    sources_process.start(QStringLiteral("pactl"),
                          QStringList{QStringLiteral("list"), QStringLiteral("short"), QStringLiteral("sources")});
    if (!sources_process.waitForFinished(1000) || sources_process.exitStatus() != QProcess::NormalExit ||
        sources_process.exitCode() != 0)
    {
        return std::nullopt;
    }

    const QString sources_output = QString::fromUtf8(sources_process.readAllStandardOutput());
    const QString monitor_prefix = default_sink + QStringLiteral(".monitor");
    for (const auto &line : sources_output.split('\n', Qt::SkipEmptyParts))
    {
        const auto fields = line.simplified().split(' ', Qt::SkipEmptyParts);
        if (fields.size() < 2)
        {
            continue;
        }

        const QString source_name = fields[1];
        if (source_name == monitor_prefix || source_name.startsWith(monitor_prefix) ||
            (source_name.contains(default_sink, Qt::CaseInsensitive) &&
             source_name.contains(QStringLiteral("monitor"), Qt::CaseInsensitive)))
        {
            return source_name;
        }
    }

    return std::nullopt;
}

int camera_format_priority(QVideoFrameFormat::PixelFormat pixel_format)
{
    switch (pixel_format)
    {
    case QVideoFrameFormat::Format_RGBA8888:
    case QVideoFrameFormat::Format_RGBX8888:
        return 0;
    case QVideoFrameFormat::Format_BGRA8888:
    case QVideoFrameFormat::Format_BGRX8888:
        return 1;
    case QVideoFrameFormat::Format_ARGB8888:
    case QVideoFrameFormat::Format_XRGB8888:
    case QVideoFrameFormat::Format_ABGR8888:
    case QVideoFrameFormat::Format_XBGR8888:
        return 2;
    case QVideoFrameFormat::Format_UYVY:
    case QVideoFrameFormat::Format_YUYV:
        return 3;
    default:
        return 4;
    }
}

bool is_pi_target_impl()
{
    return std::string_view{COCKSCREEN_TARGET_PLATFORM} == kPiZero2W;
}

bool has_direct_drm_access_impl()
{
    return std::filesystem::exists("/dev/dri/card0") || std::filesystem::exists("/dev/dri/renderD128");
}

} // namespace

bool is_pi_target()
{
    return is_pi_target_impl();
}

bool has_direct_drm_access()
{
    return has_direct_drm_access_impl();
}

std::optional<std::string> read_text_file(const std::filesystem::path &path)
{
    std::ifstream file{path};
    if (!file.is_open())
    {
        return std::nullopt;
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

bool looks_like_touch_input(const std::string &name)
{
    const auto lower = [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); };
    std::string normalized;
    normalized.reserve(name.size());
    for (unsigned char ch : name)
    {
        normalized.push_back(lower(ch));
    }

    return normalized.find("touch") != std::string::npos || normalized.find("tablet") != std::string::npos ||
           normalized.find("ts") != std::string::npos;
}

std::optional<std::string> find_usb_touchscreen()
{
    const std::filesystem::path input_root{"/sys/class/input"};
    if (!std::filesystem::exists(input_root))
    {
        return std::nullopt;
    }

    for (const auto &entry : std::filesystem::directory_iterator{input_root})
    {
        const auto device_dir = entry.path() / "device";
        const auto name = read_text_file(device_dir / "name");
        if (!name.has_value() || !looks_like_touch_input(*name))
        {
            continue;
        }

        std::error_code error;
        const auto resolved = std::filesystem::canonical(device_dir, error);
        if (error)
        {
            continue;
        }

        const auto resolved_text = resolved.string();
        if (resolved_text.find("/usb") != std::string::npos || resolved_text.find("usb") != std::string::npos)
        {
            return *name;
        }
    }

    return std::nullopt;
}

bool print_startup_preflight()
{
    if (!is_pi_target())
    {
        return true;
    }

    if (!has_direct_drm_access())
    {
        std::cerr << "Qt startup check failed: no direct DRM/KMS device was found."
                  << "\n";
        std::cerr << "eglfs runs without a compositor, but it still needs access to /dev/dri/card0"
                  << " or /dev/dri/renderD128."
                  << "\n";
        std::cerr << "If you are starting from SSH, make sure the Pi is on a local VT with DRM access"
                  << " and that the device nodes are present and readable."
                  << "\n";
        return false;
    }

    if (std::getenv("DISPLAY") != nullptr || std::getenv("WAYLAND_DISPLAY") != nullptr)
    {
        std::cerr << "Qt startup note: DISPLAY/WAYLAND_DISPLAY are set, but the Pi build uses eglfs"
                  << " and ignores any compositor session."
                  << "\n";
    }

    if (const auto touchscreen = find_usb_touchscreen(); touchscreen.has_value())
    {
        std::cerr << "Qt startup note: USB touch input detected: " << *touchscreen << "\n";
    }
    else
    {
        std::cerr << "Qt startup note: no USB touchscreen-like input device was detected under /sys/class/input."
                  << "\n";
    }

    return true;
}

bool validate_render_path(const ApplicationSettings &settings)
{
    if (settings.render_path == "qt")
    {
        return true;
    }

    if (settings.render_path == "qt-shader")
    {
        return true;
    }

    if (settings.render_path == "v4l2-dmabuf-egl")
    {
        return true;
    }

    std::cerr << "Unknown render path: " << settings.render_path << "\n";
    std::cerr << "Supported render paths: qt, qt-shader, v4l2-dmabuf-egl\n";
    return false;
}

std::optional<std::pair<int, int>> parse_capture_mode_dimensions(std::string_view mode)
{
    if (mode == "qvga")
    {
        return std::pair{320, 240};
    }

    if (mode == "vga")
    {
        return std::pair{640, 480};
    }

    if (mode == "svga")
    {
        return std::pair{800, 600};
    }

    if (mode == "xga")
    {
        return std::pair{1024, 768};
    }

    if (mode == "720p")
    {
        return std::pair{1280, 720};
    }

    if (mode == "1080p")
    {
        return std::pair{1920, 1080};
    }

    const auto separator = mode.find('x');
    if (separator != std::string_view::npos)
    {
        try
        {
            const int width = std::stoi(std::string{mode.substr(0, separator)});
            const int height = std::stoi(std::string{mode.substr(separator + 1)});
            if (width > 0 && height > 0)
            {
                return std::pair{width, height};
            }
        }
        catch (...)
        {
        }
    }

    return std::nullopt;
}

std::optional<QAudioDevice> select_audio_input(const ApplicationSettings &settings, QString *selected_label)
{
    const auto audio_inputs = QMediaDevices::audioInputs();
    if (audio_inputs.isEmpty())
    {
        return std::nullopt;
    }

    QString requested = QString::fromStdString(settings.audio_device);
    if (requested == QStringLiteral("@disabled@") || requested == QStringLiteral("@DISABLED@"))
    {
        if (selected_label != nullptr)
        {
            *selected_label = QStringLiteral("disabled");
        }

        return std::nullopt;
    }

    if (requested == QStringLiteral("@default_input@") || requested == QStringLiteral("@DEFAULT_INPUT@"))
    {
        requested.clear();
    }

    if (requested == QStringLiteral("@default_output_monitor@") ||
        requested == QStringLiteral("@DEFAULT_OUTPUT_MONITOR@") || requested == QStringLiteral("@DEFAULT_MONITOR@"))
    {
        if (selected_label != nullptr)
        {
            *selected_label = QStringLiteral("default output monitor");
        }

        return std::nullopt;
    }

    if (!requested.isEmpty())
    {
        const QByteArray requested_id = requested.toUtf8();
        for (const auto &device : audio_inputs)
        {
            if (device.id() == requested_id || device.id().contains(requested_id) ||
                device.description().contains(requested, Qt::CaseInsensitive) ||
                requested.contains(device.description(), Qt::CaseInsensitive))
            {
                if (selected_label != nullptr)
                {
                    *selected_label = device.description();
                }

                return device;
            }
        }
    }

    if (!is_pi_target())
    {
        if (const auto monitor_source_name = detect_default_output_monitor_source_name(); monitor_source_name.has_value())
        {
            const QByteArray monitor_id = monitor_source_name->toUtf8();
            for (const auto &device : audio_inputs)
            {
                if (device.id() == monitor_id || device.id().contains(monitor_id))
                {
                    if (selected_label != nullptr)
                    {
                        *selected_label = device.description();
                    }

                    return device;
                }
            }
        }
    }

    for (const auto &device : audio_inputs)
    {
        const auto description = device.description();
        if (description.contains(QStringLiteral("hdmi"), Qt::CaseInsensitive) ||
            description.contains(QStringLiteral("iec958"), Qt::CaseInsensitive) ||
            description.contains(QStringLiteral("spdif"), Qt::CaseInsensitive))
        {
            continue;
        }

        if (selected_label != nullptr)
        {
            *selected_label = description;
        }

        return device;
    }

    if (selected_label != nullptr)
    {
        *selected_label = audio_inputs.front().description();
    }

    return audio_inputs.front();
}

std::optional<QCameraDevice> select_video_input(const ApplicationSettings &settings, QString *selected_label)
{
    const auto video_inputs = QMediaDevices::videoInputs();
    if (video_inputs.isEmpty())
    {
        return std::nullopt;
    }

    const QString requested = QString::fromStdString(settings.video_device);
    if (requested == QStringLiteral("@disabled@") || requested == QStringLiteral("@DISABLED@"))
    {
        if (selected_label != nullptr)
        {
            *selected_label = QStringLiteral("disabled");
        }

        return std::nullopt;
    }

    if (!requested.isEmpty())
    {
        const QByteArray requested_id = requested.toUtf8();
        for (const auto &device : video_inputs)
        {
            if (device.id() == requested_id || device.id().contains(requested_id) ||
                device.description().contains(requested, Qt::CaseInsensitive) ||
                requested.contains(device.description(), Qt::CaseInsensitive))
            {
                if (selected_label != nullptr)
                {
                    *selected_label = device.description();
                }

                return device;
            }
        }

        if (requested.startsWith(QStringLiteral("/dev/video")))
        {
            bool ok = false;
            const int requested_index = requested.sliced(QStringLiteral("/dev/video").size()).toInt(&ok);
            if (ok && requested_index >= 0 && requested_index < video_inputs.size())
            {
                const auto &device = video_inputs[requested_index];
                if (selected_label != nullptr)
                {
                    *selected_label = device.description();
                }

                return device;
            }
        }
    }

    const auto &device = video_inputs.front();
    if (selected_label != nullptr)
    {
        *selected_label = device.description();
    }

    return device;
}

std::optional<QCameraFormat> select_camera_format(const QCameraDevice &device, int requested_width, int requested_height)
{
    const auto formats = device.videoFormats();
    if (formats.isEmpty())
    {
        return std::nullopt;
    }

    auto score_format = [requested_width, requested_height](const QCameraFormat &format) {
        const QSize resolution = format.resolution();
        const int width_error = std::abs(resolution.width() - requested_width);
        const int height_error = std::abs(resolution.height() - requested_height);
        const qint64 requested_area = static_cast<qint64>(requested_width) * static_cast<qint64>(requested_height);
        const qint64 area = static_cast<qint64>(resolution.width()) * static_cast<qint64>(resolution.height());
        const int fps = static_cast<int>(format.maxFrameRate() * 100.0F);
        const int pixel_priority = camera_format_priority(format.pixelFormat());
        return std::tuple{pixel_priority, width_error + height_error, std::llabs(area - requested_area), -fps};
    };

    auto best = formats.front();
    auto best_score = score_format(best);

    for (const auto &format : formats)
    {
        const auto score = score_format(format);
        if (score < best_score)
        {
            best = format;
            best_score = score;
        }

        if (format.resolution().width() == requested_width && format.resolution().height() == requested_height)
        {
            return format;
        }
    }

    return best;
}

QString camera_format_label(const QCameraFormat &format)
{
    if (format.isNull())
    {
        return QStringLiteral("unknown");
    }

    return QStringLiteral("%1x%2 @ %3-%4 fps")
        .arg(format.resolution().width())
        .arg(format.resolution().height())
        .arg(format.minFrameRate(), 0, 'f', 1)
        .arg(format.maxFrameRate(), 0, 'f', 1);
}

QString shader_label_for(const ApplicationSettings &settings, std::string_view shader_file)
{
    static_cast<void>(settings);

    if (!shader_file.empty())
    {
        return QString::fromStdString(std::filesystem::path{shader_file}.filename().string());
    }
    return QStringLiteral("none");
}

QString shader_label_for(const ApplicationSettings &settings)
{
        if (!settings.shader_file.empty())
        {
            return QString::fromStdString(std::filesystem::path{settings.shader_file}.filename().string());
        }

        const std::filesystem::path shader_directory{settings.shader_directory};
        if (!std::filesystem::exists(shader_directory))
        {
            return QStringLiteral("none");
        }

        for (const auto &entry : std::filesystem::directory_iterator{shader_directory})
        {
            if (!entry.is_regular_file())
            {
                continue;
            }

            const auto extension = entry.path().extension().string();
            if (extension == ".frag" || extension == ".glsl" || extension == ".vert" || extension == ".comp")
            {
                return QString::fromStdString(entry.path().filename().string());
            }
        }

        return QStringLiteral("none");
}

} // namespace cockscreen::runtime