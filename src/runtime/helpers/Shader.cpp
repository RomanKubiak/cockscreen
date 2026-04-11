#include "cockscreen/runtime/RuntimeHelpers.hpp"

#include <filesystem>

namespace cockscreen::runtime
{

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