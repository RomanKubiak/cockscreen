#include "cockscreen/runtime/application/Support.hpp"

#include "cockscreen/runtime/RuntimeHelpers.hpp"

#include <filesystem>
#include <vector>

namespace cockscreen::runtime::application_support
{

namespace
{

void collect_missing_layer_shaders(const char *layer_name, const SceneLayer &layer, const std::string &shader_directory,
                                   std::vector<std::string> *missing_shaders)
{
    if (missing_shaders == nullptr)
    {
        return;
    }

    for (const auto &shader_file : layer.shaders)
    {
        if (shader_file.empty())
        {
            continue;
        }

        std::filesystem::path shader_path{shader_file};
        if (!shader_path.is_absolute())
        {
            shader_path = std::filesystem::path{shader_directory} / shader_path;
        }

        // Also check the per-shader subdirectory layout: shaders/<stem>/<stem>.glsl
        const auto stem = std::filesystem::path{shader_file}.stem();
        const std::filesystem::path subdir_path = std::filesystem::path{shader_directory} / stem;
        const bool exists_flat = resolve_relative_path(shader_path).has_value();
        const bool exists_subdir = std::filesystem::is_directory(subdir_path) &&
                                   (std::filesystem::is_regular_file(subdir_path / (stem.string() + ".glsl")) ||
                                    [&]() {
                                        for (const auto &e : std::filesystem::directory_iterator{subdir_path})
                                        {
                                            const auto ext = e.path().extension().string();
                                            if (ext == ".glsl" || ext == ".frag")
                                            {
                                                return true;
                                            }
                                        }
                                        return false;
                                    }());

        if (!exists_flat && !exists_subdir)
        {
            missing_shaders->push_back("Missing scene shader [" + std::string(layer_name) + "]: " + shader_file +
                                       " (expected at " + shader_path.string() + ")");
        }
    }
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

std::vector<std::string> missing_scene_shaders(const SceneDefinition &scene, const ApplicationSettings &settings)
{
    std::vector<std::string> missing_shaders;
    const auto shader_directory = effective_shader_directory(scene, settings);

    // Check all named layers (covers built-ins and user-defined layers from the
    // "layers" dict).  The three legacy fields (video_layer, playback_layer,
    // screen_layer) are already mirrored into named_layers during parsing, so
    // iterating named_layers is sufficient.
    for (const auto &[name, named_layer] : scene.named_layers)
    {
        collect_missing_layer_shaders(name.c_str(), named_layer.layer, shader_directory, &missing_shaders);
    }

    return missing_shaders;
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