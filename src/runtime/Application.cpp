#include "../../include/cockscreen/runtime/Application.hpp"
#include "../../include/cockscreen/runtime/AudioAnalysisWindow.hpp"
#include "../../include/cockscreen/runtime/DirectVideoWindow.hpp"
#include "../../include/cockscreen/runtime/MidiInputMonitor.hpp"
#include "../../include/cockscreen/runtime/Scene.hpp"
#include "../../include/cockscreen/runtime/ShaderVideoWindow.hpp"
#include "../../include/cockscreen/runtime/RuntimeHelpers.hpp"
#include "../../include/cockscreen/runtime/application/Support.hpp"
#include "../../include/cockscreen/runtime/VideoWindow.hpp"

#include <QApplication>
#include <QTimer>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <utility>

namespace cockscreen::runtime
{

namespace support = application_support;

Application::Application(ApplicationSettings settings) : settings_{std::move(settings)} {}

int Application::run(int argc, char *argv[])
{
    if (!print_startup_preflight())
    {
        return 1;
    }

    if (!validate_render_path(settings_))
    {
        return 2;
    }

    if (is_pi_target())
    {
        qputenv("QT_QPA_PLATFORM", "eglfs");
    }
    else
    {
        qputenv("QT_QPA_PLATFORM", "wayland-egl");
    }

    QApplication application{argc, argv};
    application.setApplicationName(QString::fromStdString(settings_.window_title));

    SceneDefinition scene = support::default_scene_for_settings(settings_);
    if (!settings_.scene_file.empty())
    {
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

        scene = std::move(*loaded_scene);
        settings_.scene_file = scene.source_path.string();
    }

    support::apply_scene_to_settings(scene, &settings_);
    settings_.shader_directory = support::effective_shader_directory(scene, settings_);

    QString audio_label;
    select_audio_input(settings_, &audio_label);

    AudioAnalysisWindow audio_analysis{settings_};
    MidiInputMonitor midi_input{settings_.midi_input, &scene.midi_cc_mappings};

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
        frame->audio_fft_bands = audio_analysis.fft_bands();
        midi_input.advance(frame_step_seconds);
        midi_input.poll();
        midi_input.populate_frame(frame);
    };

    if (settings_.render_path == "v4l2-dmabuf-egl")
    {
        const QString shader_label = shader_label_for(settings_);
        DirectVideoWindow window{settings_, shader_label, scene.show_status_overlay};
        window.show();

        QObject::connect(&timer, &QTimer::timeout, [&]() {
            auto frame = modulation_bus_.snapshot();
            refresh_frame(&frame);
            modulation_bus_.update(frame);
            window.set_frame(frame);
        });

        auto live_frame = modulation_bus_.snapshot();
        refresh_frame(&live_frame);
        modulation_bus_.update(live_frame);
        window.set_frame(live_frame);
        timer.start(static_cast<int>(frame_time.count()));

        std::cout << "Cockscreen initial scaffold" << '\n';
        std::cout << "Target platform: " << COCKSCREEN_TARGET_PLATFORM << '\n';
        std::cout << "Video input: " << settings_.video_device << '\n';
        std::cout << "Video device: " << settings_.video_device << '\n';
        std::cout << "Video format: " << window.capture_format_label().toStdString() << '\n';
        std::cout << "Audio device: " << settings_.audio_device << '\n';
        std::cout << "Audio analysis: " << (audio_label.isEmpty() ? "<none>" : audio_label.toStdString()) << '\n';
        std::cout << "OSC endpoint: " << settings_.osc_endpoint << '\n';
        std::cout << "MIDI input: " << settings_.midi_input << '\n';
        std::cout << "Shader directory: " << settings_.shader_directory << '\n';
          std::cout << "Resources directory: " << scene.resources_directory.string() << '\n';
        std::cout << "Shader file: " << (settings_.shader_file.empty() ? "<default>" : settings_.shader_file) << '\n';
        std::cout << "Screen shader file: "
              << (settings_.screen_shader_file.empty() ? "<default>" : settings_.screen_shader_file) << '\n';
        std::cout << "Shader loaded: " << shader_label.toStdString() << '\n';
        std::cout << "Render path: " << settings_.render_path << '\n';
        std::cout << "DMABUF export: " << (window.dmabuf_export_supported() ? "available" : "unavailable") << '\n';
        std::cout << "Window mode: direct V4L2/EGL" << '\n';
        std::cout << "Qt platform: " << (is_pi_target() ? "eglfs" : "wayland-egl") << '\n';
        std::cout << "Frame size: " << settings_.width << 'x' << settings_.height << " @ " << settings_.frame_rate
                  << " fps" << '\n';

        if (!window.status_message().isEmpty())
        {
            std::cerr << "Direct backend status: " << window.status_message().toStdString() << '\n';
        }

        if (!audio_analysis.status_message().isEmpty())
        {
            std::cerr << "Audio analysis status: " << audio_analysis.status_message().toStdString() << '\n';
        }

        if (!midi_input.status_message().isEmpty())
        {
            std::cerr << "MIDI input status: " << midi_input.status_message().toStdString() << '\n';
        }

        const auto startup_frame = modulation_bus_.snapshot();
        std::cout << "Initial modulation state: audio=" << startup_frame.audio_level << ", gain=" << startup_frame.gain << '\n';

        return application.exec();
    }

    if (settings_.render_path == "qt-shader")
    {
        QString selected_video_label;
        const auto video_device = select_video_input(settings_, &selected_video_label);
        const auto [requested_width, requested_height] = support::requested_video_dimensions(scene, settings_);
        const auto selected_format = video_device.has_value()
                                         ? select_camera_format(*video_device, requested_width, requested_height)
                                         : std::nullopt;
        const QString camera_format_text = selected_format.has_value() ? camera_format_label(*selected_format)
                                                                      : QStringLiteral("unknown");
        const bool video_on_top = scene.video_input.on_top.value_or(settings_.top_layer == "video");
        const bool show_status_overlay = scene.show_status_overlay;

        ShaderVideoWindow window{settings_, scene, video_device.value_or(QCameraDevice{}), selected_video_label,
                     camera_format_text, video_on_top, show_status_overlay};
        window.show();

        QObject::connect(&timer, &QTimer::timeout, [&]() {
            auto frame = modulation_bus_.snapshot();
            refresh_frame(&frame);
            modulation_bus_.update(frame);
            window.set_frame(frame);
        });

        auto live_frame = modulation_bus_.snapshot();
        refresh_frame(&live_frame);
        modulation_bus_.update(live_frame);
        window.set_frame(live_frame);
        timer.start(static_cast<int>(frame_time.count()));

        std::cout << "Cockscreen initial scaffold" << '\n';
        std::cout << "Target platform: " << COCKSCREEN_TARGET_PLATFORM << '\n';
        std::cout << "Video input: " << (selected_video_label.isEmpty() ? "<none>" : selected_video_label.toStdString())
                  << '\n';
        std::cout << "Video device: " << settings_.video_device << '\n';
        std::cout << "Video format: " << camera_format_text.toStdString() << '\n';
        std::cout << "Audio device: " << settings_.audio_device << '\n';
        std::cout << "Audio analysis: " << (audio_label.isEmpty() ? "<none>" : audio_label.toStdString()) << '\n';
        std::cout << "OSC endpoint: " << settings_.osc_endpoint << '\n';
        std::cout << "MIDI input: " << settings_.midi_input << '\n';
        std::cout << "Shader directory: " << settings_.shader_directory << '\n';
        std::cout << "Shader file: " << (settings_.shader_file.empty() ? "<default>" : settings_.shader_file) << '\n';
        std::cout << "Screen shader file: "
                  << (settings_.screen_shader_file.empty() ? "<default>" : settings_.screen_shader_file) << '\n';
        std::cout << "Scene file: " << (settings_.scene_file.empty() ? "<none>" : settings_.scene_file) << '\n';
        std::cout << "Top layer: " << (video_on_top ? "video" : "screen") << '\n';
        std::cout << "Top layer opacity: " << settings_.top_layer_opacity << '\n';
        std::cout << "Render path: " << settings_.render_path << '\n';
        std::cout << "Window mode: Qt6 windowed" << '\n';
        std::cout << "Qt platform: " << (is_pi_target() ? "eglfs" : "wayland-egl") << '\n';
        std::cout << "Frame size: " << settings_.width << 'x' << settings_.height << " @ " << settings_.frame_rate
                  << " fps" << '\n';

        if (!audio_analysis.status_message().isEmpty())
        {
            std::cerr << "Audio analysis status: " << audio_analysis.status_message().toStdString() << '\n';
        }

        if (!midi_input.status_message().isEmpty())
        {
            std::cerr << "MIDI input status: " << midi_input.status_message().toStdString() << '\n';
        }

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
    window.show();

    QObject::connect(&timer, &QTimer::timeout, [&]() {
        auto initial_frame = modulation_bus_.snapshot();
        refresh_frame(&initial_frame);
        modulation_bus_.update(initial_frame);
        window.set_frame(initial_frame);
    });

    auto live_frame = modulation_bus_.snapshot();
    refresh_frame(&live_frame);
    modulation_bus_.update(live_frame);
    window.set_frame(live_frame);
    timer.start(static_cast<int>(frame_time.count()));

    std::cout << "Cockscreen initial scaffold" << '\n';
    std::cout << "Target platform: " << COCKSCREEN_TARGET_PLATFORM << '\n';
    std::cout << "Video input: " << (selected_video_label.isEmpty() ? "<none>" : selected_video_label.toStdString()) << '\n';
    std::cout << "Video device: " << settings_.video_device << '\n';
    std::cout << "Video format: " << camera_format_text.toStdString() << '\n';
    std::cout << "Audio device: " << settings_.audio_device << '\n';
    std::cout << "Audio analysis: " << (audio_label.isEmpty() ? "<none>" : audio_label.toStdString()) << '\n';
    std::cout << "OSC endpoint: " << settings_.osc_endpoint << '\n';
    std::cout << "MIDI input: " << settings_.midi_input << '\n';
    std::cout << "Shader directory: " << settings_.shader_directory << '\n';
      std::cout << "Resources directory: " << scene.resources_directory.string() << '\n';
    std::cout << "Shader file: " << (settings_.shader_file.empty() ? "<default>" : settings_.shader_file) << '\n';
    std::cout << "Playback file: " << (scene.playback_input.file.empty() ? "<none>" : scene.playback_input.file) << '\n';
    std::cout << "Screen shader file: "
              << (settings_.screen_shader_file.empty() ? "<default>" : settings_.screen_shader_file) << '\n';
    std::cout << "Video shader loaded: " << video_shader_label.toStdString() << '\n';
    std::cout << "Screen shader loaded: " << screen_shader_label.toStdString() << '\n';
    std::cout << "Top layer: " << (video_on_top ? "video" : "screen") << '\n';
    std::cout << "Render path: " << settings_.render_path << '\n';
    std::cout << "Window mode: Qt6 windowed" << '\n';
    std::cout << "Qt platform: " << (is_pi_target() ? "eglfs" : "wayland-egl") << '\n';
    std::cout << "Frame size: " << settings_.width << 'x' << settings_.height << " @ " << settings_.frame_rate << " fps"
              << '\n';

    const auto startup_frame = modulation_bus_.snapshot();
    std::cout << "Initial modulation state: audio=" << startup_frame.audio_level << ", gain=" << startup_frame.gain << '\n';

    if (!midi_input.status_message().isEmpty())
    {
        std::cerr << "MIDI input status: " << midi_input.status_message().toStdString() << '\n';
    }

    return application.exec();
}

} // namespace cockscreen::runtime