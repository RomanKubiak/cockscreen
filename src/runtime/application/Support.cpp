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

SceneDefinition default_scene_for_settings(const ApplicationSettings &settings)
{
    SceneDefinition scene;
    scene.video_input.enabled = true;
    scene.video_input.device = settings.video_device;
    scene.video_input.on_top = settings.top_layer == "video";
    scene.shader_directory = settings.shader_directory;
    scene.resources_directory = settings.executable_directory.empty() ? std::filesystem::current_path()
                                                                      : std::filesystem::path{settings.executable_directory};
    scene.playback_input.enabled = false;
    scene.audio_input.enabled = true;
    scene.audio_input.device = settings.audio_device;
    scene.midi_input.enabled = true;
    scene.midi_input.device = settings.midi_input;
    scene.video_layer.enabled = true;
    if (!settings.shader_file.empty())
    {
        scene.video_layer.shaders.push_back(settings.shader_file);
    }
    scene.playback_layer.enabled = false;
    scene.screen_layer.enabled = true;
    if (!settings.screen_shader_file.empty())
    {
        scene.screen_layer.shaders.push_back(settings.screen_shader_file);
    }
    return scene;
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

std::pair<int, int> requested_video_dimensions(const SceneDefinition &scene, const ApplicationSettings &settings)
{
    if (const auto requested = parse_capture_mode_dimensions(scene.video_input.format); requested.has_value())
    {
        return *requested;
    }

    return {settings.width, settings.height};
}

} // namespace cockscreen::runtime::application_support