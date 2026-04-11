#pragma once

#include "cockscreen/runtime/Scene.hpp"

#include <filesystem>

class QJsonObject;

namespace cockscreen::runtime::scene_parser
{

SceneDefinition parse_scene_definition(const QJsonObject &root, const std::filesystem::path &source_path);

} // namespace cockscreen::runtime::scene_parser