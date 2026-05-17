#include "cockscreen/app/CliSupport.hpp"

#include <QAudioDevice>
#include <QCameraDevice>
#include <QCameraFormat>
#include <QMediaDevices>
#include <QSize>
#include <QVideoFrameFormat>

#include <array>
#include <cctype>
#include <iostream>
#include <optional>

namespace cockscreen::app
{

namespace
{

struct NamedResolution
{
    int width;
    int height;
    std::string_view name;
};

constexpr std::array<NamedResolution, 11> kNamedResolutions{{
    {160, 120, "QQVGA"},
    {320, 240, "QVGA"},
    {640, 480, "VGA"},
    {800, 600, "SVGA"},
    {1024, 600, "WSVGA"},
    {1024, 768, "XGA"},
    {1280, 720, "720p"},
    {1280, 1024, "SXGA"},
    {1600, 900, "900p"},
    {1920, 1080, "1080p"},
    {2592, 1944, "5MP"},
}};

std::string_view next_value(int &index, int argc, char *argv[])
{
    if (index + 1 >= argc)
    {
        return {};
    }

    ++index;
    return argv[index];
}

std::optional<std::string_view> named_resolution(int width, int height)
{
    for (const auto &entry : kNamedResolutions)
    {
        if (entry.width == width && entry.height == height)
        {
            return entry.name;
        }
    }

    return std::nullopt;
}

std::string friendly_resolution_text(int width, int height)
{
    if (const auto label = named_resolution(width, height); label.has_value())
    {
        return std::string{*label} + " (" + std::to_string(width) + "x" + std::to_string(height) + ")";
    }

    return std::to_string(width) + "x" + std::to_string(height);
}

#ifndef _WIN32
std::optional<std::pair<int, int>> first_resolution_pair(std::string_view text)
{
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        if (!std::isdigit(static_cast<unsigned char>(text[i])))
        {
            continue;
        }

        std::size_t width_end = i;
        while (width_end < text.size() && std::isdigit(static_cast<unsigned char>(text[width_end])))
        {
            ++width_end;
        }

        if (width_end >= text.size() || text[width_end] != 'x')
        {
            i = width_end;
            continue;
        }

        const std::size_t height_begin = width_end + 1;
        std::size_t height_end = height_begin;
        while (height_end < text.size() && std::isdigit(static_cast<unsigned char>(text[height_end])))
        {
            ++height_end;
        }

        if (height_end == height_begin)
        {
            i = height_end;
            continue;
        }

        try
        {
            return std::pair{std::stoi(std::string{text.substr(i, width_end - i)}),
                             std::stoi(std::string{text.substr(height_begin, height_end - height_begin)})};
        }
        catch (...)
        {
        }

        i = height_end;
    }

    return std::nullopt;
}

std::string common_stepwise_resolutions(std::string_view text)
{
    const auto range_pos = text.find("..");
    if (range_pos == std::string_view::npos)
    {
        return {};
    }

    const auto first = first_resolution_pair(text);
    const auto second = first_resolution_pair(text.substr(range_pos + 2));
    if (!first.has_value() || !second.has_value())
    {
        return {};
    }

    std::string result;
    for (const auto &entry : kNamedResolutions)
    {
        if (entry.width >= first->first && entry.width <= second->first &&
            entry.height >= first->second && entry.height <= second->second)
        {
            if (!result.empty())
            {
                result += ", ";
            }
            result += entry.name;
        }
    }

    return result;
}

std::string humanize_rpi_mode(std::string_view mode)
{
    const auto range_pos = mode.find("..");
    if (range_pos != std::string_view::npos)
    {
        const std::string common = common_stepwise_resolutions(mode);
        if (!common.empty())
        {
            return std::string{mode} + " [common picks: " + common + "]";
        }
        return std::string{mode};
    }

    const auto resolution = first_resolution_pair(mode);
    if (!resolution.has_value())
    {
        return std::string{mode};
    }

    if (const auto label = named_resolution(resolution->first, resolution->second); label.has_value())
    {
        return std::string{*label} + " | " + std::string{mode};
    }

    return std::string{mode};
}
#endif

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
            result.settings.scene_file_is_read_only = !result.settings.scene_file.empty();
        }
        else if (argument == "--enable-web-server")
        {
            result.settings.web_server_bind_url = std::string(next_value(index, argc, argv));
        }
        else if (argument == "--ads-vref")
        {
            const auto text = next_value(index, argc, argv);
            if (!text.empty())
            {
                char *end = nullptr;
                const double v = std::strtod(text.data(), &end);
                if (end != text.data() && v > 0.0)
                    result.settings.ads1256_vref_volts = v;
            }
        }
        else if (argument == "-v" || argument == "--verbose" || argument == "-d" || argument == "--debug")
        {
            result.settings.verbose_debug = true;
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
              << "  --enable-web-server URL\n"
              << "  --ads-vref VOLTS  ADS1256 reference voltage (default 3.3; env COCKSCREEN_ADS1256_VREF_VOLTS)\n"
              << "  -v, --verbose   Enable verbose debug output (sets GST_DEBUG=2, Qt debug logging)\n"
              << "  -d, --debug     Alias for --verbose\n"
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
                std::cout << " (id=" << device.id().toStdString() << ")";
            }
            std::cout << "\n";
            for (const auto &fmt : device.videoFormats())
            {
                const auto res = fmt.resolution();
                std::cout << "      " << friendly_resolution_text(res.width(), res.height())
                          << "  " << fmt.minFrameRate() << "-" << fmt.maxFrameRate() << " fps\n";
            }
        }
    }

#ifndef _WIN32
    const auto rpi_cameras = detect_rpi_cameras();
    std::cout << "RPi camera devices\n";
    if (rpi_cameras.empty())
    {
        std::cout << "  <none>\n";
    }
    else
    {
        for (const auto &cam : rpi_cameras)
        {
            std::cout << "  - " << cam.card << " (" << cam.path << ", driver=" << cam.driver << ")";
            if (cam.requires_media_controller)
            {
                std::cout << " [MC]";
            }
            std::cout << "\n";
            if (cam.requires_media_controller)
            {
                std::cout << "      NOTE: uses hardware ISP (bcm2835-isp) via Media Controller\n"
                          << "      Supported scene formats (\"format\" field in scene inputs.video):\n";
                if (cam.modes.empty())
                {
                    std::cout << "        <sensor modes not enumerable — try media-ctl --print-topology>\n";
                }
                else
                {
                    for (const auto &mode : cam.modes)
                        std::cout << "        " << humanize_rpi_mode(mode) << "\n";
                }
            }
            else
            {
                if (cam.modes.empty())
                {
                    std::cout << "      <no modes enumerated>\n";
                }
                else
                {
                    for (const auto &mode : cam.modes)
                        std::cout << "      " << humanize_rpi_mode(mode) << "\n";
                }
            }
        }
    }
#endif

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
