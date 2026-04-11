#pragma once

#include "cockscreen/runtime/Application.hpp"
#include "cockscreen/runtime/Scene.hpp"

#include <filesystem>
#include <optional>
#include <utility>

namespace cockscreen::runtime::application_support
{

std::optional<std::filesystem::path> resolve_relative_path(const std::filesystem::path &path);
SceneDefinition default_scene_for_settings(const ApplicationSettings &settings);
void apply_scene_to_settings(const SceneDefinition &scene, ApplicationSettings *settings);
std::string effective_shader_directory(const SceneDefinition &scene, const ApplicationSettings &settings);
std::pair<int, int> requested_video_dimensions(const SceneDefinition &scene, const ApplicationSettings &settings);

} // namespace cockscreen::runtime::application_support