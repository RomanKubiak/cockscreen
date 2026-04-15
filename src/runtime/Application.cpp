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
#include "../../include/cockscreen/runtime/VideoWindow.hpp"

#include <QApplication>
#include <QCursor>
#include <QTimer>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <utility>

namespace cockscreen::runtime
{

namespace support = application_support;

namespace
{

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
                           const QString &extra_line = QString{})
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

    return lines.join('\n');
}

} // namespace

Application::Application(ApplicationSettings settings) : settings_{std::move(settings)} {}

int Application::run(int argc, char *argv[])
{
    if (!print_startup_preflight())
    {
        return 1;
    }

    if (settings_.scene_file.empty())
    {
        std::cerr << "Scene file not specified. Pass --scene-file PATH or place a default scene beside the executable.\n";
        return 2;
    }

    const auto scene_path = support::resolve_relative_path(std::filesystem::path{settings_.scene_file});
    if (!scene_path.has_value())
    {
        std::cerr << "Scene file not found: " << settings_.scene_file << '\n';
        return 2;
    }

    std::string scene_error;
    const auto loaded_scene = load_scene_definition(*scene_path, &scene_error);
    if (!loaded_scene.has_value())
    {
        std::cerr << scene_error << '\n';
        return 2;
    }

    SceneDefinition scene = *loaded_scene;
    settings_.scene_file = scene.source_path.string();
    support::apply_scene_to_settings(scene, &settings_);
    settings_.shader_directory = support::effective_shader_directory(scene, settings_);

    if (const auto missing_shaders = support::missing_scene_shaders(scene, settings_); !missing_shaders.empty())
    {
        for (const auto &message : missing_shaders)
        {
            std::cerr << message << '\n';
        }
        std::cerr << "Refusing to start due to missing scene shader files.\n";
        return 2;
    }

    if (!validate_render_path(settings_))
    {
        return 2;
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

    QApplication application{argc, argv};
    application.setApplicationName(QStringLiteral("cockscreen"));
    const auto qt_platform_name = application.platformName().toStdString();
    if (is_pi_target())
    {
        QApplication::setOverrideCursor(Qt::BlankCursor);
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
        const bool show_status_overlay = scene.show_status_overlay;

        ShaderVideoWindow window{settings_, scene, video_device.value_or(QCameraDevice{}), selected_video_label,
                     camera_format_text, video_on_top, show_status_overlay};
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
            const QString fps_line = QStringLiteral("FPS process %1 | render %2 | gain %3")
                                         .arg(window.processing_fps(), 0, 'f', 1)
                                         .arg(window.processing_fps(), 0, 'f', 1)
                                         .arg(frame.gain, 0, 'f', 2);
            const QString device_line = QStringLiteral("Video %1 | format %2 | top layer %3 | opacity %4")
                                            .arg(selected_video_label.isEmpty() ? QStringLiteral("<none>") : selected_video_label)
                                            .arg(camera_format_text)
                                            .arg(video_on_top ? QStringLiteral("video") : QStringLiteral("screen"))
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
                                            .arg(video_on_top ? QStringLiteral("video") : QStringLiteral("screen"))
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
        std::cout << "Top layer: " << (video_on_top ? "video" : "screen") << '\n';
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
    std::cout << "Playback file: " << (scene.playback_input.file.empty() ? "<none>" : scene.playback_input.file) << '\n';
    std::cout << "Video shader loaded: " << video_shader_label.toStdString() << '\n';
    std::cout << "Screen shader loaded: " << screen_shader_label.toStdString() << '\n';
    std::cout << "Top layer: " << (video_on_top ? "video" : "screen") << '\n';
    std::cout << "Render path: " << settings_.render_path << '\n';
    std::cout << "Window mode: Qt6 windowed" << '\n';
    std::cout << "Qt platform: " << qt_platform_name << '\n';

    const auto startup_frame = modulation_bus_.snapshot();
    std::cout << "Initial modulation state: audio=" << startup_frame.audio_level << ", gain=" << startup_frame.gain << '\n';

    return application.exec();
}

} // namespace cockscreen::runtime