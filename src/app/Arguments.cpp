#include "cockscreen/app/CliSupport.hpp"

#include "cockscreen/runtime/V4l2Capture.hpp"

#include <QAudioDevice>
#include <QCameraDevice>
#include <QMediaDevices>

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

bool has_list_devices_argument(int argc, char *argv[])
{
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument{argv[index]};
        if (argument == "--list-devices")
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
        else if (argument == "--scene-file")
        {
            result.settings.scene_file = std::string(next_value(index, argc, argv));
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
              << "  --config-file PATH\n"
              << "  --list-devices\n"
              << "  --list-capture-modes\n"
              << "  --window-title TEXT\n"
              << "  --scene-file FILE\n"
              << "  --render-path qt|qt-shader|v4l2-dmabuf-egl\n"
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

void print_device_list()
{
    const auto audio_inputs = QMediaDevices::audioInputs();
    const auto video_inputs = QMediaDevices::videoInputs();
    const auto midi_devices = detect_midi_devices();
    std::cout << "Available audio input devices\n";
    if (audio_inputs.isEmpty())
    {
        std::cout << "  <none>\n";
    }
    else
    {
        for (const auto &device : audio_inputs)
        {
            std::cout << "  - " << device.description().toStdString();
            if (!device.id().isEmpty())
            {
                std::cout << " (id=" << device.id().toHex().toStdString() << ")";
            }
            std::cout << "\n";
        }
    }

    std::cout << "Available video input devices\n";
    if (video_inputs.isEmpty())
    {
        std::cout << "  <none>\n";
    }
    else
    {
        for (const auto &device : video_inputs)
        {
            std::cout << "  - " << device.description().toStdString();
            if (!device.id().isEmpty())
            {
                std::cout << " (id=" << device.id().toHex().toStdString() << ")";
            }
            std::cout << "\n";
        }
    }

    std::cout << "Available MIDI devices\n";
    if (midi_devices.empty())
    {
        std::cout << "  <none>\n";
    }
    else
    {
        for (const auto &device : midi_devices)
        {
            std::cout << "  - " << device << "\n";
        }
    }
}

} // namespace cockscreen::app