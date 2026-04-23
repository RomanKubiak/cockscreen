#include "../../include/cockscreen/runtime/Application.hpp"
#include "../../include/cockscreen/runtime/AudioAnalysisWindow.hpp"
#include "../../include/cockscreen/runtime/DirectVideoWindow.hpp"
#include "../../include/cockscreen/runtime/MidiInputMonitor.hpp"
#include "../../include/cockscreen/runtime/OscInputMonitor.hpp"
#if defined(__linux__) && defined(__aarch64__)
#include "../../include/cockscreen/runtime/pi/WaveshareAds1256Monitor.hpp"
#endif
#include "../../include/cockscreen/runtime/Scene.hpp"
#include "../../include/cockscreen/runtime/ShaderVideoWindow.hpp"
#include "../../include/cockscreen/runtime/RuntimeHelpers.hpp"
#include "../../include/cockscreen/runtime/application/Support.hpp"
#include "../../include/cockscreen/runtime/shadervideo/Support.hpp"
#include "../../include/cockscreen/runtime/web/SceneControlServer.hpp"
#include "../../include/cockscreen/runtime/VideoWindow.hpp"

#include <QApplication>
#include <QCursor>
#include <QFont>
#include <QFontDatabase>
#include <QHostAddress>
#include <QMimeDatabase>
#include <QPainter>
#include <QSurfaceFormat>
#include <QTimer>
#include <QUrl>

#include <chrono>
#include <filesystem>
#include <optional>
#include <iostream>
#include <memory>
#include <utility>

namespace cockscreen::runtime
{

namespace support = application_support;

namespace
{

class FatalErrorWindow final : public QWidget
{
  public:
    explicit FatalErrorWindow(QString message, QWidget *parent = nullptr)
        : QWidget{parent}, message_{std::move(message)}
    {
        setWindowTitle(QStringLiteral("cockscreen error"));
        resize(1024, 600);
        setMinimumSize(640, 360);
        setAutoFillBackground(false);
    }

  protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter{this};
        painter.fillRect(rect(), Qt::black);
        painter.setRenderHint(QPainter::TextAntialiasing, true);

        QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        font.setPointSize(11);
        painter.setFont(font);
        painter.setPen(Qt::white);
        painter.drawText(rect().adjusted(24, 24, -24, -24), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                         message_);
    }

  private:
    QString message_;
};

int show_fatal_error_window(QApplication *application, QString message)
{
    if (application == nullptr)
    {
        return 2;
    }

    FatalErrorWindow window{std::move(message)};
    if (is_pi_target())
    {
        window.showFullScreen();
    }
    else
    {
        window.show();
    }

    return application->exec();
}

QStringList wrap_overlay_line(const QString &text, int preferred_chars)
{
    if (preferred_chars <= 0 || text.size() <= preferred_chars)
    {
        return {text};
    }

    const QStringList segments = text.split(QStringLiteral(" | "));
    if (segments.size() <= 1)
    {
        return {text};
    }

    QStringList wrapped_lines;
    QString current_line;
    for (const QString &segment : segments)
    {
        const QString candidate = current_line.isEmpty() ? segment : current_line + QStringLiteral(" | ") + segment;
        if (current_line.isEmpty() || candidate.size() <= preferred_chars)
        {
            current_line = candidate;
            continue;
        }

        wrapped_lines << current_line;
        current_line = segment;
    }

    if (!current_line.isEmpty())
    {
        wrapped_lines << current_line;
    }

    return wrapped_lines.isEmpty() ? QStringList{text} : wrapped_lines;
}

QString build_overlay_text(const QString &fps_line, const QString &device_line, const QString &audio_line,
                           const QString &metrics_line, const QString &midi_line, const QString &osc_line,
                           const QString &extra_line = QString{}, const QString &extra_line_two = QString{},
                           const QString &extra_line_three = QString{})
{
    constexpr int kPreferredCharsPerLine{58};

    QStringList lines;
    lines << wrap_overlay_line(fps_line, kPreferredCharsPerLine)
          << wrap_overlay_line(metrics_line, kPreferredCharsPerLine)
          << wrap_overlay_line(device_line, kPreferredCharsPerLine)
          << wrap_overlay_line(audio_line, kPreferredCharsPerLine)
          << wrap_overlay_line(midi_line, kPreferredCharsPerLine)
          << wrap_overlay_line(osc_line, kPreferredCharsPerLine);
    if (!extra_line.isEmpty())
    {
        lines << wrap_overlay_line(extra_line, kPreferredCharsPerLine);
    }
    if (!extra_line_two.isEmpty())
    {
        lines << wrap_overlay_line(extra_line_two, kPreferredCharsPerLine);
    }
    if (!extra_line_three.isEmpty())
    {
        lines << wrap_overlay_line(extra_line_three, kPreferredCharsPerLine);
    }

    return lines.join('\n');
}

QString format_transport_clock(std::int64_t position_ms)
{
    const std::int64_t clamped_ms = std::max<std::int64_t>(0, position_ms);
    const std::int64_t total_seconds = clamped_ms / 1000;
    const std::int64_t milliseconds = clamped_ms % 1000;
    const std::int64_t hours = total_seconds / 3600;
    const std::int64_t minutes = (total_seconds / 60) % 60;
    const std::int64_t seconds = total_seconds % 60;
    return QStringLiteral("%1:%2:%3.%4")
        .arg(hours, 2, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'))
        .arg(milliseconds, 3, 10, QLatin1Char('0'));
}

QString format_file_size(std::optional<std::uintmax_t> bytes)
{
    if (!bytes.has_value())
    {
        return QStringLiteral("?");
    }

    constexpr double kKilobyte{1024.0};
    constexpr double kMegabyte{1024.0 * 1024.0};
    constexpr double kGigabyte{1024.0 * 1024.0 * 1024.0};
    const double value = static_cast<double>(*bytes);
    if (value >= kGigabyte)
    {
        return QStringLiteral("%1 GB").arg(value / kGigabyte, 0, 'f', 2);
    }
    if (value >= kMegabyte)
    {
        return QStringLiteral("%1 MB").arg(value / kMegabyte, 0, 'f', 2);
    }
    if (value >= kKilobyte)
    {
        return QStringLiteral("%1 KB").arg(value / kKilobyte, 0, 'f', 1);
    }
    return QStringLiteral("%1 B").arg(*bytes);
}

QString playback_source_label(const SceneDefinition &scene)
{
    if (scene.playback_input.file.empty())
    {
        return QStringLiteral("<none>");
    }

    const std::filesystem::path source_path{scene.playback_input.file};
    return QString::fromStdString(source_path.filename().empty() ? scene.playback_input.file : source_path.filename().string());
}

QString playback_config_summary(const SceneDefinition &scene)
{
    const auto &playback = scene.playback_input;
    const QString loop_text = playback.loop_end_ms.has_value()
                                  ? QStringLiteral("%1-%2 ms")
                                        .arg(playback.loop_start_ms)
                                        .arg(*playback.loop_end_ms)
                                  : QStringLiteral("full");
    const QString repeat_text = playback.loop_repeat == 0 ? QStringLiteral("inf")
                                                          : QString::number(playback.loop_repeat);
    const QString source_text = playback.file.empty() ? QStringLiteral("<none>")
                                                      : QString::fromStdString(playback.file);

    return QStringLiteral("Playback cfg %1 | source %2 | start %3 ms | loop %4 | repeat %5 | rate %6/%7")
        .arg(playback.enabled ? QStringLiteral("on") : QStringLiteral("off"))
        .arg(source_text)
        .arg(playback.start_ms)
        .arg(loop_text)
        .arg(repeat_text)
        .arg(playback.playback_rate, 0, 'f', 2)
        .arg(playback.playback_rate_looping, 0, 'f', 2);
}

QString playback_runtime_overlay_line(const SceneDefinition &scene, const ShaderVideoWindow &window)
{
    const auto &playback = scene.playback_input;
    const QString duration_text = window.playback_duration_ms() > 0 ? format_transport_clock(window.playback_duration_ms())
                                                                    : QStringLiteral("--:--:--.---");
    const QString loop_text = playback.loop_end_ms.has_value()
                                  ? QStringLiteral("%1-%2")
                                        .arg(format_transport_clock(playback.loop_start_ms))
                                        .arg(format_transport_clock(*playback.loop_end_ms))
                                  : QStringLiteral("full");
    const QString repeat_text = playback.loop_repeat == 0 ? QStringLiteral("inf") : QString::number(playback.loop_repeat);
    const QString status_text = window.playback_status_text().isEmpty() ? QStringLiteral("idle") : window.playback_status_text();
    const QString error_text = window.playback_error_text().isEmpty() ? QStringLiteral("none") : window.playback_error_text();

    return QStringLiteral("Playback %1 | size %2 | pos %3/%4 | ms %5 | start %6 | loop %7 x%8 | rate %9 | status %10 | err %11")
        .arg(playback_source_label(scene))
        .arg(format_file_size(window.playback_file_size_bytes()))
        .arg(format_transport_clock(window.playback_position_ms()))
        .arg(duration_text)
        .arg(window.playback_position_ms())
        .arg(format_transport_clock(playback.start_ms))
        .arg(loop_text)
        .arg(repeat_text)
        .arg(window.playback_current_rate(), 0, 'f', 2)
        .arg(status_text)
        .arg(error_text);
}

QString playback_static_overlay_line(const SceneDefinition &scene)
{
    const auto &playback = scene.playback_input;
    const QString loop_text = playback.loop_end_ms.has_value()
                                  ? QStringLiteral("%1-%2")
                                        .arg(format_transport_clock(playback.loop_start_ms))
                                        .arg(format_transport_clock(*playback.loop_end_ms))
                                  : QStringLiteral("full");
    const QString repeat_text = playback.loop_repeat == 0 ? QStringLiteral("inf") : QString::number(playback.loop_repeat);
    return QStringLiteral("Playback %1 | start %2 | loop %3 x%4 | rate %5/%6")
        .arg(playback_source_label(scene))
        .arg(format_transport_clock(playback.start_ms))
        .arg(loop_text)
        .arg(repeat_text)
        .arg(playback.playback_rate, 0, 'f', 2)
        .arg(playback.playback_rate_looping, 0, 'f', 2);
}

std::optional<QString> validate_playback_source(const SceneDefinition &scene)
{
    const auto &playback = scene.playback_input;
    if (!playback.enabled)
    {
        return std::nullopt;
    }

    const QString source_text = QString::fromStdString(playback.file);
    if (source_text.trimmed().isEmpty())
    {
        return QStringLiteral("Playback is enabled but no playback source is defined.");
    }

    const QUrl source_url{source_text};
    if (!source_url.scheme().isEmpty())
    {
        if (!source_url.isValid())
        {
            return QStringLiteral("Playback source '%1' is not a valid URL or file path.").arg(source_text);
        }

        if (!source_url.isLocalFile())
        {
            return QStringLiteral("Playback source '%1' uses unsupported scheme '%2'. Only local files are supported.")
                .arg(source_text, source_url.scheme());
        }

        const std::filesystem::path local_file{source_url.toLocalFile().toStdString()};
        if (local_file.empty() || !std::filesystem::exists(local_file))
        {
            return QStringLiteral("Playback source '%1' does not exist on disk.").arg(source_text);
        }
        if (std::filesystem::is_directory(local_file))
        {
            return QStringLiteral("Playback source '%1' resolves to a directory, not a media file.").arg(source_text);
        }

        QMimeDatabase mime_database;
        const auto mime = mime_database.mimeTypeForFile(QString::fromStdString(local_file.string()), QMimeDatabase::MatchContent);
        if (mime.isValid() && !mime.name().startsWith(QStringLiteral("video/")) &&
            !mime.name().startsWith(QStringLiteral("audio/")) && mime.name() != QStringLiteral("application/ogg"))
        {
            return QStringLiteral("Playback source '%1' is not a recognized audio/video media file (%2).").arg(source_text,
                                                                                                                   mime.name());
        }

        return std::nullopt;
    }

    const std::filesystem::path raw_path{playback.file};
    if (raw_path.filename().empty())
    {
        return QStringLiteral("Playback source '%1' is not a valid file path.").arg(source_text);
    }

    const auto resolved_path = shader_window::resolve_scene_resource_path(scene.resources_directory, playback.file);
    if (!resolved_path.has_value())
    {
        return QStringLiteral("Playback source '%1' does not exist on disk.").arg(source_text);
    }
    if (std::filesystem::is_directory(*resolved_path))
    {
        return QStringLiteral("Playback source '%1' resolves to a directory, not a media file.").arg(source_text);
    }

    QMimeDatabase mime_database;
    const auto mime = mime_database.mimeTypeForFile(QString::fromStdString(resolved_path->string()), QMimeDatabase::MatchContent);
    if (mime.isValid() && !mime.name().startsWith(QStringLiteral("video/")) &&
        !mime.name().startsWith(QStringLiteral("audio/")) && mime.name() != QStringLiteral("application/ogg"))
    {
        return QStringLiteral("Playback source '%1' is not a recognized audio/video media file (%2).").arg(source_text,
                                                                                                               mime.name());
    }

    return std::nullopt;
}

struct WebServerBindConfig
{
    QHostAddress address;
    quint16 port{0};
    QString display_url;
};

std::optional<WebServerBindConfig> parse_web_server_bind_url(const std::string &bind_url, QString *error_message)
{
    if (bind_url.empty())
    {
        return std::nullopt;
    }

    const QUrl url{QString::fromStdString(bind_url)};
    if (!url.isValid() || url.scheme().isEmpty() || url.host().isEmpty() || url.port() <= 0)
    {
        if (error_message != nullptr)
        {
            *error_message = QStringLiteral("Invalid web server URL. Expected a full URL such as http://127.0.0.1:8080");
        }
        return std::nullopt;
    }

    QHostAddress address;
    const QString host = url.host();
    if (host == QStringLiteral("localhost"))
    {
        address = QHostAddress::LocalHost;
    }
    else if (host == QStringLiteral("0.0.0.0"))
    {
        address = QHostAddress::Any;
    }
    else if (!address.setAddress(host))
    {
        if (error_message != nullptr)
        {
            *error_message = QStringLiteral("Unsupported web server host '%1'. Use localhost, 0.0.0.0, or a numeric IP address.")
                                 .arg(host);
        }
        return std::nullopt;
    }

    return WebServerBindConfig{address, static_cast<quint16>(url.port()), url.toString()};
}

QString effective_top_layer_name(const SceneDefinition &scene, bool video_on_top)
{
    if (!scene.layer_order.empty())
    {
        return QString::fromStdString(scene.layer_order.back());
    }

    return video_on_top ? QStringLiteral("video") : QStringLiteral("screen");
}

} // namespace

Application::Application(ApplicationSettings settings) : settings_{std::move(settings)} {}

int Application::run(int argc, char *argv[])
{
    if (!print_startup_preflight())
    {
        return 1;
    }

#if defined(__linux__) && defined(__aarch64__)
    std::unique_ptr<WaveshareAds1256Monitor> ads1256_monitor;
    if (is_pi_target())
    {
        ads1256_monitor = std::make_unique<WaveshareAds1256Monitor>();
        if (!ads1256_monitor->start())
        {
            std::cerr << "[ads1256] analog monitor disabled" << '\n';
            ads1256_monitor.reset();
        }
    }
#endif

#ifdef _WIN32
    // v4l2-dmabuf-egl is Linux-only; silently fall back to qt-shader which uses
    // Qt Multimedia (Media Foundation on Windows) for camera capture.
    if (settings_.render_path == "v4l2-dmabuf-egl")
    {
        std::cerr << "Render path 'v4l2-dmabuf-egl' is not supported on Windows; using 'qt-shader' instead.\n";
        settings_.render_path = "qt-shader";
    }
#endif

    if (is_pi_target() && qgetenv("QT_QPA_PLATFORM").isEmpty())
    {
        qputenv("QT_QPA_PLATFORM", "eglfs");
    }

    if (!is_pi_target())
    {
        QSurfaceFormat format;
        format.setRenderableType(QSurfaceFormat::OpenGL);
        format.setVersion(2, 1);
        format.setProfile(QSurfaceFormat::CompatibilityProfile);
        QSurfaceFormat::setDefaultFormat(format);
    }

    QApplication application{argc, argv};
    application.setApplicationName(QStringLiteral("cockscreen"));
    const auto qt_platform_name = application.platformName().toStdString();
    if (is_pi_target())
    {
        QApplication::setOverrideCursor(Qt::BlankCursor);
    }

    if (settings_.scene_file.empty())
    {
        return show_fatal_error_window(
            &application,
            QStringLiteral("Scene file not specified. Pass --scene-file PATH or place a default scene beside the executable."));
    }

    const auto scene_path = support::resolve_relative_path(std::filesystem::path{settings_.scene_file});
    if (!scene_path.has_value())
    {
        return show_fatal_error_window(
            &application, QStringLiteral("Scene file not found: %1").arg(QString::fromStdString(settings_.scene_file)));
    }

    std::string scene_error;
    const auto loaded_scene = load_scene_definition(*scene_path, &scene_error);
    if (!loaded_scene.has_value())
    {
        return show_fatal_error_window(&application, QString::fromStdString(scene_error));
    }

    SceneDefinition scene = *loaded_scene;
    settings_.scene_file = scene.source_path.string();
    support::apply_scene_to_settings(scene, &settings_);
    settings_.shader_directory = support::effective_shader_directory(scene, settings_);

    if (const auto missing_shaders = support::missing_scene_shaders(scene, settings_); !missing_shaders.empty())
    {
        QStringList lines;
        for (const auto &message : missing_shaders)
        {
            lines.push_back(QString::fromStdString(message));
        }
        lines.push_back(QStringLiteral("Refusing to start due to missing scene shader files."));
        return show_fatal_error_window(&application, lines.join('\n'));
    }

    if (const auto playback_error = validate_playback_source(scene); playback_error.has_value())
    {
        return show_fatal_error_window(&application, *playback_error);
    }

    if (!validate_render_path(settings_))
    {
        return show_fatal_error_window(
            &application,
            QStringLiteral("Invalid render path: %1").arg(QString::fromStdString(settings_.render_path)));
    }

    QString web_server_error;
    const auto web_server_bind = parse_web_server_bind_url(settings_.web_server_bind_url, &web_server_error);
    if (!settings_.web_server_bind_url.empty() && !web_server_bind.has_value())
    {
        return show_fatal_error_window(&application, web_server_error);
    }

    QString audio_label;
    select_audio_input(settings_, &audio_label);

    AudioAnalysisWindow audio_analysis{settings_};
    MidiInputMonitor midi_input{settings_.midi_input, &scene.midi_cc_mappings};
    OscInputMonitor osc_input{settings_.osc_endpoint, &scene.osc_mappings};
    SystemMetricsSampler system_metrics;

    const auto frame_time = std::chrono::milliseconds(settings_.frame_rate > 0 ? 1000 / settings_.frame_rate : 33);
    const float frame_step_seconds = static_cast<float>(frame_time.count()) / 1000.0F;
    QTimer timer;
    timer.setTimerType(Qt::PreciseTimer);

    const auto refresh_frame = [&](core::ControlFrame *frame) {
        if (frame == nullptr)
        {
            return;
        }

        frame->audio_level = audio_analysis.overall_level_db();
        frame->audio_rms = audio_analysis.rms_level();
        frame->audio_peak = audio_analysis.peak_level();
        frame->audio_fft_bands = audio_analysis.fft_bands();
        frame->audio_waveform = audio_analysis.waveform_samples();
        midi_input.advance(frame_step_seconds);
        midi_input.poll();
        midi_input.populate_frame(frame);
        osc_input.poll();
        osc_input.populate_frame(frame);
    };

    const auto build_metrics_line = [&system_metrics]() {
        const auto metrics = system_metrics.sample();
        if (!metrics.available)
        {
            return QStringLiteral("CPU n/a | MEM n/a");
        }

        return QStringLiteral("CPU %1% | MEM %2% (%3 / %4 MiB)")
            .arg(metrics.cpu_percent, 0, 'f', 1)
            .arg(metrics.memory_percent, 0, 'f', 1)
            .arg(metrics.memory_used_mb, 0, 'f', 0)
            .arg(metrics.memory_total_mb, 0, 'f', 0);
    };

    if (settings_.render_path == "v4l2-dmabuf-egl")
    {
#ifdef _WIN32
        std::cerr << "Render path 'v4l2-dmabuf-egl' is not supported on Windows.\n";
        return 2;
#else
        const QString shader_label = shader_label_for(settings_);
        DirectVideoWindow window{settings_, shader_label, scene.show_status_overlay};
        if (is_pi_target())
        {
            window.showFullScreen();
        }
        else
        {
            window.show();
        }

        QObject::connect(&timer, &QTimer::timeout, [&]() {
            auto frame = modulation_bus_.snapshot();
            refresh_frame(&frame);
            modulation_bus_.update(frame);
            window.set_frame(frame);
            const QString fps_line = QStringLiteral("FPS capture %1 | render %2 | gain %3")
                                         .arg(window.capture_fps(), 0, 'f', 1)
                                         .arg(window.render_fps(), 0, 'f', 1)
                                         .arg(frame.gain, 0, 'f', 2);
            const QString device_line = QStringLiteral("Video %1 | format %2 | shader %3 | render path %4")
                                            .arg(QString::fromStdString(settings_.video_device))
                                            .arg(window.capture_format_label())
                                            .arg(shader_label)
                                            .arg(QString::fromStdString(settings_.render_path));
            const QString audio_line = QStringLiteral("Audio %1 | level %2 dB | rms %3 | peak %4")
                                           .arg(audio_analysis.status_message().isEmpty() ? audio_label : audio_analysis.status_message())
                                           .arg(audio_analysis.overall_level_db(), 0, 'f', 1)
                                           .arg(audio_analysis.rms_level(), 0, 'f', 3)
                                           .arg(audio_analysis.peak_level(), 0, 'f', 3);
            const QString midi_line = QStringLiteral("MIDI %1 | %2")
                                          .arg(midi_input.status_message().isEmpty() ? QStringLiteral("inactive")
                                                                                     : midi_input.status_message())
                                          .arg(midi_input.activity_message().isEmpty() ? QStringLiteral("no incoming data yet")
                                                                                       : midi_input.activity_message());
            const QString osc_line = QStringLiteral("OSC %1 | rx:%2 addr:%3 | %4")
                                         .arg(osc_input.status_message())
                                         .arg(osc_input.message_count())
                                         .arg(osc_input.address_count())
                                         .arg(osc_input.activity_message().isEmpty() ? QStringLiteral("waiting")
                                                                                     : osc_input.activity_message());
            window.set_status_overlay_text(build_overlay_text(fps_line, device_line, audio_line, build_metrics_line(), midi_line, osc_line,
                                                             playback_config_summary(scene),
                                                             QStringLiteral("Pi fullscreen mode | mouse cursor hidden")));
        });

        auto live_frame = modulation_bus_.snapshot();
        refresh_frame(&live_frame);
        modulation_bus_.update(live_frame);
        window.set_frame(live_frame);
        {
            const QString fps_line = QStringLiteral("FPS capture %1 | render %2 | gain %3")
                                         .arg(window.capture_fps(), 0, 'f', 1)
                                         .arg(window.render_fps(), 0, 'f', 1)
                                         .arg(live_frame.gain, 0, 'f', 2);
            const QString device_line = QStringLiteral("Video %1 | format %2 | shader %3 | render path %4")
                                            .arg(QString::fromStdString(settings_.video_device))
                                            .arg(window.capture_format_label())
                                            .arg(shader_label)
                                            .arg(QString::fromStdString(settings_.render_path));
            const QString audio_line = QStringLiteral("Audio %1 | level %2 dB | rms %3 | peak %4")
                                           .arg(audio_analysis.status_message().isEmpty() ? audio_label : audio_analysis.status_message())
                                           .arg(audio_analysis.overall_level_db(), 0, 'f', 1)
                                           .arg(audio_analysis.rms_level(), 0, 'f', 3)
                                           .arg(audio_analysis.peak_level(), 0, 'f', 3);
            const QString midi_line = QStringLiteral("MIDI %1 | %2")
                                          .arg(midi_input.status_message().isEmpty() ? QStringLiteral("inactive")
                                                                                     : midi_input.status_message())
                                          .arg(midi_input.activity_message().isEmpty() ? QStringLiteral("no incoming data yet")
                                                                                       : midi_input.activity_message());
            const QString osc_line = QStringLiteral("OSC %1 | rx:%2 addr:%3 | %4")
                                         .arg(osc_input.status_message())
                                         .arg(osc_input.message_count())
                                         .arg(osc_input.address_count())
                                         .arg(osc_input.activity_message().isEmpty() ? QStringLiteral("waiting")
                                                                                     : osc_input.activity_message());
            window.set_status_overlay_text(build_overlay_text(fps_line, device_line, audio_line, build_metrics_line(), midi_line, osc_line,
                                                             playback_config_summary(scene),
                                                             QStringLiteral("Pi fullscreen mode | mouse cursor hidden")));
        }
        timer.start(static_cast<int>(frame_time.count()));

        std::cout << "Cockscreen initial scaffold" << '\n';
        std::cout << "Target platform: " << COCKSCREEN_TARGET_PLATFORM << '\n';
        std::cout << "Video input: " << settings_.video_device << '\n';
        std::cout << "Video device: " << settings_.video_device << '\n';
        std::cout << "Video format: " << window.capture_format_label().toStdString() << '\n';
        std::cout << "Audio device: " << settings_.audio_device << '\n';
        std::cout << "OSC endpoint: " << settings_.osc_endpoint << '\n';
          std::cout << "Resources directory: " << scene.resources_directory.string() << '\n';
        std::cout << "Shader loaded: " << shader_label.toStdString() << '\n';
        std::cout << "Render path: " << settings_.render_path << '\n';
        std::cout << "DMABUF export: " << (window.dmabuf_export_supported() ? "available" : "unavailable") << '\n';
        std::cout << "Window mode: direct V4L2/EGL" << '\n';
        std::cout << "Qt platform: " << qt_platform_name << '\n';

        if (!window.status_message().isEmpty())
        {
            std::cerr << "Direct backend status: " << window.status_message().toStdString() << '\n';
        }

        const auto startup_frame = modulation_bus_.snapshot();
        std::cout << "Initial modulation state: audio=" << startup_frame.audio_level << ", gain=" << startup_frame.gain << '\n';

        return application.exec();
#endif // !_WIN32
    }

    if (settings_.render_path == "qt-shader")
    {
        QString selected_video_label;
        const auto video_device = select_video_input(settings_, &selected_video_label);
        const auto [requested_width, requested_height] = support::requested_video_dimensions(scene);
        const auto selected_format = video_device.has_value()
                                         ? select_camera_format(*video_device, requested_width, requested_height)
                                         : std::nullopt;
        const QString camera_format_text = selected_format.has_value() ? camera_format_label(*selected_format)
                                                                      : QStringLiteral("unknown");
        const bool video_on_top = scene.video_input.on_top.value_or(settings_.top_layer == "video");
        const QString top_layer_name = effective_top_layer_name(scene, video_on_top);
        const bool show_status_overlay = scene.show_status_overlay;

        ShaderVideoWindow window{settings_, scene, video_device.value_or(QCameraDevice{}), selected_video_label,
                     camera_format_text, video_on_top, show_status_overlay};
        SceneControlDeviceInfo web_device_info;
        web_device_info.opened_video = selected_video_label.isEmpty() ? QStringLiteral("<none>") : selected_video_label;
        web_device_info.opened_audio = audio_label.isEmpty() ? QStringLiteral("<none>") : audio_label;
        web_device_info.opened_midi = settings_.midi_input.empty() ? QStringLiteral("<none>")
                                                                   : QString::fromStdString(settings_.midi_input);
        SceneControlServer control_server{&scene,
                                          &window,
                                          scene.source_path,
                                          scene.resources_directory,
                                          std::filesystem::path{settings_.shader_directory},
                                          web_device_info,
                                          settings_.scene_file_is_read_only};
        if (web_server_bind.has_value())
        {
            if (control_server.start(web_server_bind->address, web_server_bind->port))
            {
                std::cout << "Web control: " << web_server_bind->display_url.toStdString() << '\n';
            }
            else
            {
                std::cerr << "Web control server could not start at " << web_server_bind->display_url.toStdString() << '\n';
            }
        }
        if (is_pi_target())
        {
            window.showFullScreen();
        }
        else
        {
            window.show();
        }

        application.processEvents();
        if (!window.fatal_render_error().isEmpty())
        {
            std::cerr << window.fatal_render_error().toStdString() << '\n';
            return application.exec();
        }

        QObject::connect(&timer, &QTimer::timeout, [&]() {
            if (!window.fatal_render_error().isEmpty())
            {
                return;
            }

            auto frame = modulation_bus_.snapshot();
            refresh_frame(&frame);
            modulation_bus_.update(frame);
            window.set_frame(frame);
            const QString fps_line = QStringLiteral("FPS process %1 | render %2 | gain %3")
                                         .arg(window.processing_fps(), 0, 'f', 1)
                                         .arg(window.processing_fps(), 0, 'f', 1)
                                         .arg(frame.gain, 0, 'f', 2);
            const QString device_line = QStringLiteral("Video %1 | format %2 | top layer %3 | opacity %4")
                                            .arg(selected_video_label.isEmpty() ? QStringLiteral("<none>") : selected_video_label)
                                            .arg(camera_format_text)
                                            .arg(top_layer_name)
                                            .arg(settings_.top_layer_opacity, 0, 'f', 2);
            const QString audio_line = QStringLiteral("Audio %1 | level %2 dB | rms %3 | peak %4")
                                           .arg(audio_analysis.status_message().isEmpty() ? audio_label : audio_analysis.status_message())
                                           .arg(audio_analysis.overall_level_db(), 0, 'f', 1)
                                           .arg(audio_analysis.rms_level(), 0, 'f', 3)
                                           .arg(audio_analysis.peak_level(), 0, 'f', 3);
            const QString midi_line = QStringLiteral("MIDI %1 | %2")
                                          .arg(midi_input.status_message().isEmpty() ? QStringLiteral("inactive")
                                                                                     : midi_input.status_message())
                                          .arg(midi_input.activity_message().isEmpty() ? QStringLiteral("no incoming data yet")
                                                                                       : midi_input.activity_message());
            const QString osc_line = QStringLiteral("OSC %1 | rx:%2 addr:%3 | %4")
                                         .arg(osc_input.status_message())
                                         .arg(osc_input.message_count())
                                         .arg(osc_input.address_count())
                                         .arg(osc_input.activity_message().isEmpty() ? QStringLiteral("waiting")
                                                                                     : osc_input.activity_message());
            window.set_status_overlay_text(build_overlay_text(fps_line, device_line, audio_line, build_metrics_line(), midi_line, osc_line,
                                                             playback_config_summary(scene),
                                                             playback_runtime_overlay_line(scene, window),
                                                             QStringLiteral("Qt6 shader pipeline on Linux")));
        });

        auto live_frame = modulation_bus_.snapshot();
        refresh_frame(&live_frame);
        modulation_bus_.update(live_frame);
        window.set_frame(live_frame);
        {
            const QString fps_line = QStringLiteral("FPS process %1 | render %2 | gain %3")
                                         .arg(window.processing_fps(), 0, 'f', 1)
                                         .arg(window.processing_fps(), 0, 'f', 1)
                                         .arg(live_frame.gain, 0, 'f', 2);
            const QString device_line = QStringLiteral("Video %1 | format %2 | top layer %3 | opacity %4")
                                            .arg(selected_video_label.isEmpty() ? QStringLiteral("<none>") : selected_video_label)
                                            .arg(camera_format_text)
                                            .arg(top_layer_name)
                                            .arg(settings_.top_layer_opacity, 0, 'f', 2);
            const QString audio_line = QStringLiteral("Audio %1 | level %2 dB | rms %3 | peak %4")
                                           .arg(audio_analysis.status_message().isEmpty() ? audio_label : audio_analysis.status_message())
                                           .arg(audio_analysis.overall_level_db(), 0, 'f', 1)
                                           .arg(audio_analysis.rms_level(), 0, 'f', 3)
                                           .arg(audio_analysis.peak_level(), 0, 'f', 3);
            const QString midi_line = QStringLiteral("MIDI %1 | %2")
                                          .arg(midi_input.status_message().isEmpty() ? QStringLiteral("inactive")
                                                                                     : midi_input.status_message())
                                          .arg(midi_input.activity_message().isEmpty() ? QStringLiteral("no incoming data yet")
                                                                                       : midi_input.activity_message());
            const QString osc_line = QStringLiteral("OSC %1 | rx:%2 addr:%3 | %4")
                                         .arg(osc_input.status_message())
                                         .arg(osc_input.message_count())
                                         .arg(osc_input.address_count())
                                         .arg(osc_input.activity_message().isEmpty() ? QStringLiteral("waiting")
                                                                                     : osc_input.activity_message());
            window.set_status_overlay_text(build_overlay_text(fps_line, device_line, audio_line, build_metrics_line(), midi_line, osc_line,
                                                             playback_config_summary(scene),
                                                             playback_runtime_overlay_line(scene, window),
                                                             QStringLiteral("Qt6 shader pipeline on Linux")));
        }
        timer.start(static_cast<int>(frame_time.count()));

        std::cout << "Cockscreen initial scaffold" << '\n';
        std::cout << "Target platform: " << COCKSCREEN_TARGET_PLATFORM << '\n';
        std::cout << "Video input: " << (selected_video_label.isEmpty() ? "<none>" : selected_video_label.toStdString())
                  << '\n';
        std::cout << "Video device: " << settings_.video_device << '\n';
        std::cout << "Video format: " << camera_format_text.toStdString() << '\n';
        std::cout << "Audio device: " << settings_.audio_device << '\n';
        std::cout << "OSC endpoint: " << settings_.osc_endpoint << '\n';
        std::cout << "Scene file: " << (settings_.scene_file.empty() ? "<none>" : settings_.scene_file) << '\n';
        std::cout << "Top layer: " << top_layer_name.toStdString() << '\n';
        std::cout << "Top layer opacity: " << settings_.top_layer_opacity << '\n';
        std::cout << "Render path: " << settings_.render_path << '\n';
        std::cout << "Window mode: Qt6 windowed" << '\n';
        std::cout << "Qt platform: " << qt_platform_name << '\n';

        const auto startup_frame = modulation_bus_.snapshot();
        std::cout << "Initial modulation state: audio=" << startup_frame.audio_level << ", gain=" << startup_frame.gain << '\n';

        return application.exec();
    }

    QString selected_video_label;
    const auto video_device = select_video_input(settings_, &selected_video_label);
    const auto selected_format = video_device.has_value()
                                     ? select_camera_format(*video_device, settings_.width, settings_.height)
                                     : std::nullopt;
    const QString camera_format_text = selected_format.has_value() ? camera_format_label(*selected_format)
                                                                  : QStringLiteral("unknown");
    const QString video_shader_label = settings_.shader_file.empty()
                                           ? shader_label_for(settings_)
                                           : shader_label_for(settings_, settings_.shader_file);
    const QString screen_shader_label = shader_label_for(settings_, settings_.screen_shader_file);
    const bool video_on_top = settings_.top_layer == "video";
    const bool show_status_overlay = scene.show_status_overlay;

    VideoWindow window{settings_, video_device.value_or(QCameraDevice{}), selected_video_label, camera_format_text,
                       video_shader_label, show_status_overlay};
    if (is_pi_target())
    {
        window.showFullScreen();
    }
    else
    {
        window.show();
    }

    QObject::connect(&timer, &QTimer::timeout, [&]() {
        auto initial_frame = modulation_bus_.snapshot();
        refresh_frame(&initial_frame);
        modulation_bus_.update(initial_frame);
        window.set_frame(initial_frame);
        const QString fps_line = QStringLiteral("FPS process %1 | render %2 | gain %3")
                                     .arg(window.processing_fps(), 0, 'f', 1)
                                     .arg(window.processing_fps(), 0, 'f', 1)
                                     .arg(initial_frame.gain, 0, 'f', 2);
        const QString device_line = QStringLiteral("Video %1 | format %2 | shader %3 | playback %4")
                                        .arg(selected_video_label.isEmpty() ? QStringLiteral("<none>") : selected_video_label)
                                        .arg(camera_format_text)
                                        .arg(video_shader_label)
                                        .arg(screen_shader_label);
        const QString audio_line = QStringLiteral("Audio %1 | level %2 dB | rms %3 | peak %4")
                                       .arg(audio_analysis.status_message().isEmpty() ? audio_label : audio_analysis.status_message())
                                       .arg(audio_analysis.overall_level_db(), 0, 'f', 1)
                                       .arg(audio_analysis.rms_level(), 0, 'f', 3)
                                       .arg(audio_analysis.peak_level(), 0, 'f', 3);
        const QString midi_line = QStringLiteral("MIDI %1 | %2")
                                      .arg(midi_input.status_message().isEmpty() ? QStringLiteral("inactive")
                                                                                 : midi_input.status_message())
                                      .arg(midi_input.activity_message().isEmpty() ? QStringLiteral("no incoming data yet")
                                                                                   : midi_input.activity_message());
        const QString osc_line = QStringLiteral("OSC %1 | rx:%2 addr:%3 | %4")
                                     .arg(osc_input.status_message())
                                     .arg(osc_input.message_count())
                                     .arg(osc_input.address_count())
                                     .arg(osc_input.activity_message().isEmpty() ? QStringLiteral("waiting")
                                                                                 : osc_input.activity_message());
        window.set_status_overlay_text(build_overlay_text(fps_line, device_line, audio_line, build_metrics_line(), midi_line, osc_line,
                     playback_config_summary(scene),
                     playback_static_overlay_line(scene),
                                 QStringLiteral("Qt6 windowed mode on Linux")));
    });

    auto live_frame = modulation_bus_.snapshot();
    refresh_frame(&live_frame);
    modulation_bus_.update(live_frame);
    window.set_frame(live_frame);
    {
        const QString fps_line = QStringLiteral("FPS process %1 | render %2 | gain %3")
                                     .arg(window.processing_fps(), 0, 'f', 1)
                                     .arg(window.processing_fps(), 0, 'f', 1)
                                     .arg(live_frame.gain, 0, 'f', 2);
        const QString device_line = QStringLiteral("Video %1 | format %2 | shader %3 | playback %4")
                                        .arg(selected_video_label.isEmpty() ? QStringLiteral("<none>") : selected_video_label)
                                        .arg(camera_format_text)
                                        .arg(video_shader_label)
                                        .arg(screen_shader_label);
        const QString audio_line = QStringLiteral("Audio %1 | level %2 dB | rms %3 | peak %4")
                                       .arg(audio_analysis.status_message().isEmpty() ? audio_label : audio_analysis.status_message())
                                       .arg(audio_analysis.overall_level_db(), 0, 'f', 1)
                                       .arg(audio_analysis.rms_level(), 0, 'f', 3)
                                       .arg(audio_analysis.peak_level(), 0, 'f', 3);
        const QString midi_line = QStringLiteral("MIDI %1 | %2")
                                      .arg(midi_input.status_message().isEmpty() ? QStringLiteral("inactive")
                                                                                 : midi_input.status_message())
                                      .arg(midi_input.activity_message().isEmpty() ? QStringLiteral("no incoming data yet")
                                                                                   : midi_input.activity_message());
        const QString osc_line = QStringLiteral("OSC %1 | rx:%2 addr:%3 | %4")
                                     .arg(osc_input.status_message())
                                     .arg(osc_input.message_count())
                                     .arg(osc_input.address_count())
                                     .arg(osc_input.activity_message().isEmpty() ? QStringLiteral("waiting")
                                                                                 : osc_input.activity_message());
        window.set_status_overlay_text(build_overlay_text(fps_line, device_line, audio_line, build_metrics_line(), midi_line, osc_line,
                 playback_config_summary(scene),
                 playback_static_overlay_line(scene),
                     QStringLiteral("Qt6 windowed mode on Linux")));
    }
    timer.start(static_cast<int>(frame_time.count()));

    std::cout << "Cockscreen initial scaffold" << '\n';
    std::cout << "Target platform: " << COCKSCREEN_TARGET_PLATFORM << '\n';
    std::cout << "Video input: " << (selected_video_label.isEmpty() ? "<none>" : selected_video_label.toStdString()) << '\n';
    std::cout << "Video device: " << settings_.video_device << '\n';
    std::cout << "Video format: " << camera_format_text.toStdString() << '\n';
    std::cout << "Audio device: " << settings_.audio_device << '\n';
    std::cout << "OSC endpoint: " << settings_.osc_endpoint << '\n';
      std::cout << "Resources directory: " << scene.resources_directory.string() << '\n';
    std::cout << playback_config_summary(scene).toStdString() << '\n';
    std::cout << "Video shader loaded: " << video_shader_label.toStdString() << '\n';
    std::cout << "Screen shader loaded: " << screen_shader_label.toStdString() << '\n';
    std::cout << "Top layer: " << effective_top_layer_name(scene, video_on_top).toStdString() << '\n';
    std::cout << "Render path: " << settings_.render_path << '\n';
    std::cout << "Window mode: Qt6 windowed" << '\n';
    std::cout << "Qt platform: " << qt_platform_name << '\n';

    const auto startup_frame = modulation_bus_.snapshot();
    std::cout << "Initial modulation state: audio=" << startup_frame.audio_level << ", gain=" << startup_frame.gain << '\n';

    return application.exec();
}

} // namespace cockscreen::runtime