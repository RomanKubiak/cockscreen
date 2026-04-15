#include "cockscreen/runtime/application/Support.hpp"

#include "cockscreen/runtime/RuntimeHelpers.hpp"

#include <filesystem>

namespace cockscreen::runtime::application_support
{

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

void apply_scene_to_settings(const SceneDefinition &scene, ApplicationSettings *settings)
{
    if (settings == nullptr)
    {
        return;
    }

    settings->video_device = scene.video_input.enabled ? scene.video_input.device : "@disabled@";
    settings->audio_device = scene.audio_input.enabled ? scene.audio_input.device : "@disabled@";
    settings->midi_input = scene.midi_input.enabled ? scene.midi_input.device : "@disabled@";
    settings->render_path = scene.render_path;
    settings->width = scene.geometry.width;
    settings->height = scene.geometry.height;
    settings->shader_file = scene.video_layer.enabled && !scene.video_layer.shaders.empty()
                                ? scene.video_layer.shaders.front()
                                : std::string{};
    settings->screen_shader_file = scene.screen_layer.enabled && !scene.screen_layer.shaders.empty()
                                       ? scene.screen_layer.shaders.front()
                                       : std::string{};
}

std::string effective_shader_directory(const SceneDefinition &scene, const ApplicationSettings &settings)
{
    if (!scene.shader_directory.empty())
    {
        return scene.shader_directory;
    }

    if (!settings.shader_directory.empty())
    {
        return settings.shader_directory;
    }

    return settings.executable_directory;
}

std::pair<int, int> requested_video_dimensions(const SceneDefinition &scene)
{
    if (const auto requested = parse_capture_mode_dimensions(scene.video_input.format); requested.has_value())
    {
        return *requested;
    }

    return {scene.geometry.width, scene.geometry.height};
}

} // namespace cockscreen::runtime::application_support