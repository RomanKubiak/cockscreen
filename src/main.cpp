#include "../include/cockscreen/runtime/Application.hpp"
#include "../include/cockscreen/runtime/V4l2Capture.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <QSettings>

namespace
{

struct CommandLine
{
    cockscreen::runtime::ApplicationSettings settings;
    std::optional<std::string> shader_directory_override;
    bool help_requested{false};
    bool list_devices_requested{false};
    bool list_capture_modes_requested{false};
};

struct ConfigFileSelection
{
    std::optional<std::filesystem::path> path;
    bool explicit_requested{false};
};

bool apply_top_layer(std::string_view layer, cockscreen::runtime::ApplicationSettings *settings);

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

std::optional<std::filesystem::path> resolve_relative_path(const std::filesystem::path &path)
{
    if (path.is_absolute())
    {
        return std::filesystem::exists(path) ? std::optional{path} : std::nullopt;
    }

    auto current = std::filesystem::current_path();
    while (true)
    {
        const auto candidate = current / path;
        if (std::filesystem::exists(candidate))
        {
            return candidate;
        }

        if (!current.has_parent_path() || current == current.parent_path())
        {
            break;
        }

        current = current.parent_path();
    }

    return std::nullopt;
}

ConfigFileSelection select_config_file(int argc, char *argv[])
{
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument{argv[index]};
        if (argument != "--config-file")
        {
            continue;
        }

        ConfigFileSelection selection;
        selection.explicit_requested = true;
        if (index + 1 >= argc)
        {
            return selection;
        }

        const std::filesystem::path requested{argv[index + 1]};
        selection.path = resolve_relative_path(requested);
        return selection;
    }

    ConfigFileSelection selection;
    selection.path = resolve_relative_path(std::filesystem::path{"config/default.ini"});
    return selection;
}

void apply_string_setting(const QSettings &settings, const char *key, std::string *target)
{
    if (settings.contains(key))
    {
        *target = settings.value(key).toString().toStdString();
    }
}

bool apply_int_setting(const QSettings &settings, const char *key, int *target)
{
    if (!settings.contains(key))
    {
        return true;
    }

    bool ok = false;
    const int value = settings.value(key).toInt(&ok);
    if (!ok)
    {
        std::cerr << "Invalid integer value for " << key << " in config file\n";
        return false;
    }

    *target = value;
    return true;
}

bool apply_double_setting(const QSettings &settings, const char *key, double *target)
{
    if (!settings.contains(key))
    {
        return true;
    }

    bool ok = false;
    const double value = settings.value(key).toDouble(&ok);
    if (!ok || !std::isfinite(value))
    {
        std::cerr << "Invalid numeric value for " << key << " in config file\n";
        return false;
    }

    *target = value;
    return true;
}

std::optional<std::filesystem::path> resolve_relative_path_from_base(const std::filesystem::path &base_dir,
                                                                     const std::filesystem::path &path)
{
    if (path.is_absolute())
    {
        return std::filesystem::exists(path) ? std::optional{path} : std::nullopt;
    }

    const auto candidate = base_dir / path;
    if (std::filesystem::exists(candidate))
    {
        return candidate;
    }

    return std::nullopt;
}

bool load_config_file(const std::filesystem::path &path, cockscreen::runtime::ApplicationSettings *settings)
{
    if (path.empty())
    {
        return true;
    }

    if (!std::filesystem::exists(path))
    {
        std::cerr << "Config file not found: " << path.string() << '\n';
        return false;
    }

    QSettings config{QString::fromStdString(path.string()), QSettings::IniFormat};
    const auto base_dir = path.parent_path();
    config.beginGroup(QStringLiteral("runtime"));

    apply_string_setting(config, "video_device", &settings->video_device);
    apply_string_setting(config, "audio_device", &settings->audio_device);
    apply_string_setting(config, "osc_endpoint", &settings->osc_endpoint);
    apply_string_setting(config, "midi_input", &settings->midi_input);
    if (config.contains("scene_file"))
    {
        const auto scene_file = config.value("scene_file").toString().toStdString();
        const auto resolved_scene_file = resolve_relative_path_from_base(base_dir, std::filesystem::path{scene_file});
        if (!resolved_scene_file.has_value())
        {
            std::cerr << "Scene file not found: " << scene_file << '\n';
            return false;
        }

        settings->scene_file = resolved_scene_file->string();
    }

    if (config.contains("shader_directory"))
    {
        const auto shader_directory = config.value("shader_directory").toString().toStdString();
        const auto resolved_shader_directory =
            resolve_relative_path_from_base(base_dir, std::filesystem::path{shader_directory});
        if (!resolved_shader_directory.has_value())
        {
            std::cerr << "Shader directory not found: " << shader_directory << '\n';
            return false;
        }

        settings->shader_directory = resolved_shader_directory->string();
    }

    apply_string_setting(config, "shader_file", &settings->shader_file);
    apply_string_setting(config, "screen_shader_file", &settings->screen_shader_file);
    apply_string_setting(config, "render_path", &settings->render_path);
    apply_string_setting(config, "window_title", &settings->window_title);

    if (!apply_int_setting(config, "width", &settings->width) || !apply_int_setting(config, "height", &settings->height) ||
        !apply_int_setting(config, "frame_rate", &settings->frame_rate) ||
        !apply_double_setting(config, "top_layer_opacity", &settings->top_layer_opacity))
    {
        return false;
    }

    if (config.contains("top_layer"))
    {
        if (!apply_top_layer(config.value("top_layer").toString().toStdString(), settings))
        {
            return false;
        }
    }

    return true;
}

std::optional<std::pair<int, int>> capture_mode_dimensions(std::string_view mode)
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

bool apply_capture_mode(std::string_view mode, cockscreen::runtime::ApplicationSettings *settings)
{
    const auto dimensions = capture_mode_dimensions(mode);
    if (!dimensions.has_value())
    {
        std::cerr << "Unknown capture mode: " << mode << '\n';
        std::cerr << "Supported presets: qvga, vga, svga, xga, 720p, 1080p, or WIDTHxHEIGHT\n";
        return false;
    }

    settings->width = dimensions->first;
    settings->height = dimensions->second;
    return true;
}

bool apply_top_layer(std::string_view layer, cockscreen::runtime::ApplicationSettings *settings)
{
    if (layer == "video" || layer == "screen")
    {
        settings->top_layer = std::string(layer);
        return true;
    }

    std::cerr << "Unknown top layer: " << layer << '\n';
    std::cerr << "Supported top layers: video, screen\n";
    return false;
}

bool apply_top_layer_opacity(std::string_view value, cockscreen::runtime::ApplicationSettings *settings)
{
    try
    {
        const double opacity = std::stod(std::string{value});
        if (!std::isfinite(opacity) || opacity < 0.0 || opacity > 1.0)
        {
            std::cerr << "Unknown top layer opacity: " << value << '\n';
            std::cerr << "Supported top layer opacity values: 0.0 to 1.0\n";
            return false;
        }

        settings->top_layer_opacity = opacity;
        return true;
    }
    catch (...)
    {
        std::cerr << "Unknown top layer opacity: " << value << '\n';
        std::cerr << "Supported top layer opacity values: 0.0 to 1.0\n";
        return false;
    }
}

void enable_shader_render_path_if_requested(cockscreen::runtime::ApplicationSettings *settings)
{
    if (settings == nullptr)
    {
        return;
    }

    const bool shader_requested = !settings->shader_file.empty() || !settings->screen_shader_file.empty() ||
                                  !settings->scene_file.empty();
    if (shader_requested && settings->render_path != "qt-shader")
    {
        settings->render_path = "qt-shader";
        std::cerr << "Shader files requested; switching render path to qt-shader.\n";
    }
}

std::string_view next_value(int &index, int argc, char *argv[])
{
    if (index + 1 >= argc)
    {
        return {};
    }

    ++index;
    return argv[index];
}

std::optional<std::string> read_text_file(const std::filesystem::path &path)
{
    std::ifstream file{path};
    if (!file.is_open())
    {
        return std::nullopt;
    }

    std::string value;
    std::getline(file, value);
    return value;
}

bool contains_case_insensitive(std::string_view text, std::string_view needle)
{
    if (needle.empty() || text.size() < needle.size())
    {
        return false;
    }

    for (std::size_t offset = 0; offset + needle.size() <= text.size(); ++offset)
    {
        bool match = true;
        for (std::size_t index = 0; index < needle.size(); ++index)
        {
            const auto left = static_cast<unsigned char>(text[offset + index]);
            const auto right = static_cast<unsigned char>(needle[index]);
            if (std::tolower(left) != std::tolower(right))
            {
                match = false;
                break;
            }
        }

        if (match)
        {
            return true;
        }
    }

    return false;
}

bool card_has_capture_pcm(const std::filesystem::path &card_dir)
{
    std::error_code error;
    for (const auto &entry : std::filesystem::directory_iterator{card_dir, error})
    {
        if (error)
        {
            break;
        }

        if (!entry.is_directory())
        {
            continue;
        }

        const auto name = entry.path().filename().string();
        if (name.rfind("pcm", 0) == 0 && !name.empty() && name.back() == 'c')
        {
            return true;
        }
    }

    return false;
}

std::optional<std::string> detect_default_audio_device()
{
    const std::filesystem::path asound_root{"/proc/asound"};
    if (!std::filesystem::exists(asound_root))
    {
        return std::nullopt;
    }

    std::vector<std::filesystem::path> card_dirs;
    std::error_code error;
    for (const auto &entry : std::filesystem::directory_iterator{asound_root, error})
    {
        if (error)
        {
            break;
        }

        if (!entry.is_directory())
        {
            continue;
        }

        const auto name = entry.path().filename().string();
        if (name.rfind("card", 0) == 0)
        {
            card_dirs.push_back(entry.path());
        }
    }

    std::sort(card_dirs.begin(), card_dirs.end());

    for (const auto &card_dir : card_dirs)
    {
        const auto id = read_text_file(card_dir / "id");
        const auto name = read_text_file(card_dir / "name");
        const auto id_text = id.value_or("");
        const auto name_text = name.value_or("");

        if (contains_case_insensitive(id_text, "hdmi") || contains_case_insensitive(name_text, "hdmi") ||
            contains_case_insensitive(id_text, "vc4hdmi") || contains_case_insensitive(name_text, "vc4-hdmi"))
        {
            continue;
        }

        if (!card_has_capture_pcm(card_dir))
        {
            continue;
        }

        if (!id_text.empty())
        {
            return id_text;
        }

        if (!name_text.empty())
        {
            return name_text;
        }
    }

    return std::nullopt;
}

std::vector<std::string> detect_midi_devices()
{
    std::vector<std::string> result;
    const std::filesystem::path seq_clients{"/proc/asound/seq/clients"};
    std::ifstream file{seq_clients};
    if (!file.is_open())
    {
        return result;
    }

    std::string line;
    std::string current_client;
    while (std::getline(file, line))
    {
        if (line.rfind("Client ", 0) == 0)
        {
            const auto first_quote = line.find('"');
            const auto second_quote = first_quote == std::string::npos ? std::string::npos : line.find('"', first_quote + 1);
            if (first_quote != std::string::npos && second_quote != std::string::npos)
            {
                current_client = line.substr(first_quote + 1, second_quote - first_quote - 1);
                if (current_client == "System" || current_client == "Midi Through" ||
                    contains_case_insensitive(current_client, "pipewire"))
                {
                    current_client.clear();
                }
            }
            else
            {
                current_client.clear();
            }
            continue;
        }

        if (current_client.empty() || line.rfind("  Port ", 0) != 0)
        {
            continue;
        }

        const auto first_quote = line.find('"');
        const auto second_quote = first_quote == std::string::npos ? std::string::npos : line.find('"', first_quote + 1);
        if (first_quote == std::string::npos || second_quote == std::string::npos)
        {
            continue;
        }

        const auto port_name = line.substr(first_quote + 1, second_quote - first_quote - 1);
        if (port_name.empty())
        {
            continue;
        }

        result.push_back(current_client + " / " + port_name);
        current_client.clear();
    }

    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::optional<std::string> detect_default_midi_device()
{
    const auto midi_devices = detect_midi_devices();
    if (midi_devices.empty())
    {
        return std::nullopt;
    }

    return midi_devices.front();
}

CommandLine parse_arguments(int argc, char *argv[], cockscreen::runtime::ApplicationSettings settings)
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

void print_capture_modes(const cockscreen::runtime::ApplicationSettings &settings)
{
    const auto modes = cockscreen::runtime::V4l2Capture::enumerate_supported_modes(settings.video_device);
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

void print_device_list(const cockscreen::runtime::ApplicationSettings &settings)
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

} // namespace

int main(int argc, char *argv[])
{
    const std::filesystem::path executable_directory = [argv]() {
        std::error_code error;
        const auto resolved = std::filesystem::read_symlink("/proc/self/exe", error);
        if (!error)
        {
            return resolved.parent_path();
        }

        const auto fallback = std::filesystem::absolute(std::filesystem::path{argv[0]}, error);
        if (!error)
        {
            return fallback.parent_path();
        }

        return std::filesystem::current_path();
    }();

    if (has_help_argument(argc, argv))
    {
        print_help();
        return 0;
    }

    const auto config_selection = select_config_file(argc, argv);
    if (config_selection.explicit_requested && !config_selection.path.has_value())
    {
        std::cerr << "Config file not found\n";
        return 2;
    }

    cockscreen::runtime::ApplicationSettings initial_settings;
    if (config_selection.path.has_value() && !load_config_file(*config_selection.path, &initial_settings))
    {
        return 2;
    }

    auto command_line = parse_arguments(argc, argv, std::move(initial_settings));
    if (command_line.settings.audio_device.empty())
    {
        if (const auto default_audio = detect_default_audio_device(); default_audio.has_value())
        {
            command_line.settings.audio_device = *default_audio;
        }
    }

    if (command_line.settings.midi_input.empty())
    {
        if (const auto default_midi = detect_default_midi_device(); default_midi.has_value())
        {
            command_line.settings.midi_input = *default_midi;
        }
    }

    command_line.settings.executable_directory = executable_directory.string();

    if (command_line.settings.shader_directory.empty())
    {
        if (command_line.shader_directory_override.has_value())
        {
            const std::filesystem::path requested{*command_line.shader_directory_override};
            const auto resolved_override = resolve_relative_path(requested);
            command_line.settings.shader_directory = resolved_override.has_value() ? resolved_override->string()
                                                                                   : *command_line.shader_directory_override;
        }
        else
        {
            command_line.settings.shader_directory = command_line.settings.executable_directory;
        }
    }

    enable_shader_render_path_if_requested(&command_line.settings);

    if (command_line.help_requested)
    {
        print_help();
        return 0;
    }

    if (command_line.list_devices_requested)
    {
        print_device_list(command_line.settings);
    }

    if (command_line.list_capture_modes_requested)
    {
        print_capture_modes(command_line.settings);
    }

    if (command_line.list_devices_requested || command_line.list_capture_modes_requested)
    {
        return 0;
    }

    cockscreen::runtime::Application application{command_line.settings};
    return application.run(argc, argv);
}
