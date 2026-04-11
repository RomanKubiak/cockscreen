#include "cockscreen/app/CliSupport.hpp"

#include "cockscreen/runtime/V4l2Capture.hpp"

#include <iostream>

namespace cockscreen::app
{

namespace
{

std::string_view next_value(int &index, int argc, char *argv[])
{
    if (index + 1 >= argc)
    {
        return {};
    }

    ++index;
    return argv[index];
}

} // namespace

bool has_help_argument(int argc, char *argv[])
{
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument{argv[index]};
        if (argument == "--help" || argument == "-h")
        {
            return true;
        }
    }

    return false;
}

CommandLine parse_arguments(int argc, char *argv[], runtime::ApplicationSettings settings)
{
    CommandLine result;
    result.settings = std::move(settings);

    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument{argv[index]};

        if (argument == "--help" || argument == "-h")
        {
            result.help_requested = true;
        }
        else if (argument == "--list-devices")
        {
            result.list_devices_requested = true;
        }
        else if (argument == "--list-capture-modes")
        {
            result.list_capture_modes_requested = true;
        }
        else if (argument == "--window-title")
        {
            result.settings.window_title = std::string(next_value(index, argc, argv));
        }
        else if (argument == "--video-device")
        {
            result.settings.video_device = std::string(next_value(index, argc, argv));
        }
        else if (argument == "--audio-device")
        {
            result.settings.audio_device = std::string(next_value(index, argc, argv));
        }
        else if (argument == "--osc-endpoint")
        {
            result.settings.osc_endpoint = std::string(next_value(index, argc, argv));
        }
        else if (argument == "--midi-input")
        {
            result.settings.midi_input = std::string(next_value(index, argc, argv));
        }
        else if (argument == "--scene-file")
        {
            result.settings.scene_file = std::string(next_value(index, argc, argv));
        }
        else if (argument == "--shader-directory")
        {
            result.shader_directory_override = std::string(next_value(index, argc, argv));
        }
        else if (argument == "--shader-file")
        {
            result.settings.shader_file = std::string(next_value(index, argc, argv));
        }
        else if (argument == "--screen-shader-file")
        {
            result.settings.screen_shader_file = std::string(next_value(index, argc, argv));
        }
        else if (argument == "--top-layer-opacity")
        {
            const auto opacity = next_value(index, argc, argv);
            if (opacity.empty() || !apply_top_layer_opacity(opacity, &result.settings))
            {
                result.help_requested = true;
            }
        }
        else if (argument == "--top-layer")
        {
            const auto layer = next_value(index, argc, argv);
            if (layer.empty() || !apply_top_layer(layer, &result.settings))
            {
                result.help_requested = true;
            }
        }
        else if (argument == "--config-file")
        {
            if (next_value(index, argc, argv).empty())
            {
                result.help_requested = true;
            }
        }
        else if (argument == "--render-path")
        {
            result.settings.render_path = std::string(next_value(index, argc, argv));
        }
        else if (argument == "--width")
        {
            result.settings.width = std::stoi(std::string(next_value(index, argc, argv)));
        }
        else if (argument == "--height")
        {
            result.settings.height = std::stoi(std::string(next_value(index, argc, argv)));
        }
        else if (argument == "--frame-rate")
        {
            result.settings.frame_rate = std::stoi(std::string(next_value(index, argc, argv)));
        }
        else if (argument == "--capture-mode")
        {
            const auto mode = next_value(index, argc, argv);
            if (mode.empty() || !apply_capture_mode(mode, &result.settings))
            {
                result.help_requested = true;
            }
        }
        else
        {
            std::cerr << "Unknown argument: " << argument << '\n';
            result.help_requested = true;
        }
    }

    return result;
}

void print_help()
{
    std::cout << "Usage: cockscreen [options]\n"
              << "  --list-devices\n"
              << "  --list-capture-modes\n"
              << "  --window-title TEXT\n"
              << "  --video-device PATH\n"
              << "  --audio-device NAME\n"
              << "  --osc-endpoint HOST:PORT\n"
              << "  --midi-input NAME\n"
              << "  --scene-file FILE\n"
              << "  --shader-directory PATH\n"
              << "  --shader-file FILE\n"
              << "  --screen-shader-file FILE\n"
              << "  --top-layer-opacity 0.0-1.0\n"
              << "  --top-layer video|screen\n"
              << "  --config-file PATH\n"
              << "  --render-path qt|qt-shader|v4l2-dmabuf-egl\n"
              << "  --capture-mode qvga|vga|svga|xga|720p|1080p|WIDTHxHEIGHT\n"
              << "  --width N\n"
              << "  --height N\n"
              << "  --frame-rate N\n";
}

void print_capture_modes(const runtime::ApplicationSettings &settings)
{
    const auto modes = runtime::V4l2Capture::enumerate_supported_modes(settings.video_device);
    std::cout << "Capture modes for " << settings.video_device << "\n";
    if (modes.empty())
    {
        std::cout << "  <no device enumeration available>\n";
    }
    else
    {
        for (const auto &mode : modes)
        {
            std::cout << "  - " << mode << "\n";
        }
    }

    std::cout << "Static presets:\n";
    std::cout << "  - qvga (320x240)\n";
    std::cout << "  - vga (640x480)\n";
    std::cout << "  - svga (800x600)\n";
    std::cout << "  - xga (1024x768)\n";
    std::cout << "  - 720p (1280x720)\n";
    std::cout << "  - 1080p (1920x1080)\n";
}

void print_device_list(const runtime::ApplicationSettings &settings)
{
    const auto midi_devices = detect_midi_devices();
    std::cout << "Required devices and connections\n";
    std::cout << "  Video Capture: " << settings.video_device << "\n";
    std::cout << "  Audio: " << (settings.audio_device.empty() ? "<not set>" : settings.audio_device) << "\n";
    std::cout << "  MIDI: " << (settings.midi_input.empty() ? "<not set>" : settings.midi_input) << "\n";
    if (midi_devices.empty())
    {
        std::cout << "  MIDI devices detected: <none>\n";
    }
    else
    {
        std::cout << "  MIDI devices detected: " << midi_devices.size() << "\n";
        for (const auto &device : midi_devices)
        {
            std::cout << "    - " << device << "\n";
        }
    }
    std::cout << "  Network: OSC over UDP, endpoint " << settings.osc_endpoint << "\n";
    std::cout << "  Network protocol: OSC/UDP (IP:port)\n";
    std::cout << "  Render path: " << settings.render_path << "\n";
    std::cout << "  Window: " << settings.width << "x" << settings.height << "\n";
}

} // namespace cockscreen::app