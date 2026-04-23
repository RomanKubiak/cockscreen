#include "cockscreen/app/CliSupport.hpp"

#include "cockscreen/runtime/RuntimeHelpers.hpp"

#include <filesystem>

namespace cockscreen::app
{

namespace
{
std::vector<std::string> default_scene_names()
{
#ifdef _WIN32
    return {"windows.scene.jsonc", "windows.scene.json"};
#else
    if (runtime::is_pi_target())
    {
        return {"pizero-linux.scene.jsonc", "pizero-linux.scene.json"};
    }

    return {"x86_64-linux.scene.jsonc", "x86_64-linux.scene.json"};
#endif
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

std::optional<std::filesystem::path> default_scene_file(const std::filesystem::path &executable_dir)
{
    const auto scene_names = default_scene_names();

    if (!executable_dir.empty())
    {
        for (const auto &scene_name : scene_names)
        {
            const auto beside_executable = executable_dir / "scenes" / scene_name;
            if (std::filesystem::exists(beside_executable))
            {
                return beside_executable;
            }
        }
    }

    for (const auto &scene_name : scene_names)
    {
        if (const auto current = resolve_relative_path(std::filesystem::path{"scenes"} / scene_name); current.has_value())
        {
            return current;
        }
    }

    return std::nullopt;
}

} // namespace cockscreen::app