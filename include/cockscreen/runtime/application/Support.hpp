#pragma once

#include "../Application.hpp"
#include "../Scene.hpp"

#include <filesystem>
#include <optional>
#include <utility>

namespace cockscreen::runtime::application_support
{

std::optional<std::filesystem::path> resolve_relative_path(const std::filesystem::path &path);
void apply_scene_to_settings(const SceneDefinition &scene, ApplicationSettings *settings);
std::string effective_shader_directory(const SceneDefinition &scene, const ApplicationSettings &settings);
std::pair<int, int> requested_video_dimensions(const SceneDefinition &scene);

} // namespace cockscreen::runtime::application_support