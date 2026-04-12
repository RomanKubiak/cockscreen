#include "cockscreen/app/CliSupport.hpp"

#include "cockscreen/runtime/RuntimeHelpers.hpp"

#include <QSettings>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h> // CSIDL_APPDATA / SHGetFolderPathW
#endif

namespace cockscreen::app
{

namespace
{

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

} // namespace

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

ConfigFileSelection select_config_file(int argc, char *argv[],
                                       const std::filesystem::path &executable_dir)
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

        selection.path = resolve_relative_path(std::filesystem::path{argv[index + 1]});
        return selection;
    }

    ConfigFileSelection selection;

    // Highest priority: cockscreen.ini next to the executable.
    // This makes double-clicking cockscreen.exe work out of the box.
    if (!executable_dir.empty())
    {
        const std::filesystem::path beside_exe = executable_dir / "cockscreen.ini";
        if (std::filesystem::exists(beside_exe) && std::filesystem::is_regular_file(beside_exe))
        {
            selection.path = beside_exe;
            return selection;
        }
    }

#ifdef _WIN32
    // On Windows prefer %APPDATA%\cockscreen\config.ini, fall back to USERPROFILE.
    auto try_config_dir = [&selection](const wchar_t *base_w) {
        if (base_w == nullptr || base_w[0] == L'\0') { return; }
        const std::filesystem::path fallback_directory = std::filesystem::path{base_w} / "cockscreen";
        const std::filesystem::path fallback_file = fallback_directory / "config.ini";
        if (std::filesystem::exists(fallback_file) && std::filesystem::is_regular_file(fallback_file))
        {
            selection.path = fallback_file;
        }
    };

    wchar_t appdata_path[MAX_PATH]{};
    if (SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata_path) == S_OK)
    {
        try_config_dir(appdata_path);
    }
    if (!selection.path.has_value())
    {
        try_config_dir(_wgetenv(L"USERPROFILE"));
    }
#else
    if (const char *home = std::getenv("HOME"); home != nullptr && *home != '\0')
    {
        const std::filesystem::path fallback_directory = std::filesystem::path{home} / ".config" / "cockscreen";
        const std::filesystem::path fallback_file = fallback_directory / "config.ini";
        if (std::filesystem::exists(fallback_directory) && std::filesystem::is_directory(fallback_directory) &&
            std::filesystem::exists(fallback_file) && std::filesystem::is_regular_file(fallback_file))
        {
            selection.path = fallback_file;
        }
    }
#endif

    return selection;
}

bool load_config_file(const std::filesystem::path &path, runtime::ApplicationSettings *settings)
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

    apply_string_setting(config, "render_path", &settings->render_path);
    apply_string_setting(config, "window_title", &settings->window_title);

    if (!apply_int_setting(config, "width", &settings->width) || !apply_int_setting(config, "height", &settings->height) ||
        !apply_int_setting(config, "frame_rate", &settings->frame_rate))
    {
        return false;
    }

    return true;
}

bool apply_capture_mode(std::string_view mode, runtime::ApplicationSettings *settings)
{
    const auto dimensions = runtime::parse_capture_mode_dimensions(mode);
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

bool apply_top_layer(std::string_view layer, runtime::ApplicationSettings *settings)
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

bool apply_top_layer_opacity(std::string_view value, runtime::ApplicationSettings *settings)
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

void enable_shader_render_path_if_requested(runtime::ApplicationSettings *settings)
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

} // namespace cockscreen::app