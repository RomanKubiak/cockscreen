#include "cockscreen/app/CliSupport.hpp"

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
        else if (argument == "--scene-file")
        {
            result.settings.scene_file = std::string(next_value(index, argc, argv));
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
              << "  --scene-file FILE\n"
              << "  --help\n";
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