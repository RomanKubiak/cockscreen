#pragma once

#include "cockscreen/core/ControlFrame.hpp"
#include "cockscreen/runtime/Application.hpp"
#include "cockscreen/runtime/RuntimeHelpers.hpp"
#include "cockscreen/runtime/Scene.hpp"

#include <filesystem>
#include <optional>
#include <utility>

#include <QColor>
#include <QImage>
#include <QRectF>
#include <QSize>
#include <QString>

class QOpenGLShaderProgram;

namespace cockscreen::runtime::shader_window
{

inline constexpr int kNoteAtlasColumns{16};
inline constexpr int kNoteAtlasRows{8};
inline constexpr int kNoteAtlasCellSize{96};

const char *vertex_shader_source();
const char *fullscreen_vertex_shader_source();
const char *passthrough_fragment_shader_source();

QString shader_source_for_current_context(QString source);
void set_midi_uniforms(QOpenGLShaderProgram *program, const core::ControlFrame &frame);
QString read_text_file_qstring(const std::filesystem::path &path);
std::optional<std::filesystem::path> resolve_relative_path(const std::filesystem::path &relative_path);
QColor scene_clear_color(const SceneColor &color);
std::pair<int, int> requested_video_dimensions(const SceneDefinition &scene, const ApplicationSettings &settings);
QRectF video_display_rect(const SceneInput &video_input, const QSize &viewport_size);
std::optional<std::filesystem::path> resolve_scene_resource_path(const std::filesystem::path &resources_directory,
                                                                 const std::string &resource_file);
QString note_font_family_for_scene(const SceneDefinition &scene);
bool image_has_opaque_pixels(const QImage &image);
float image_opaque_coverage(const QImage &image);
QImage build_note_label_atlas_image(const QString &font_family);
QImage vertically_flipped_image(const QImage &image);

} // namespace cockscreen::runtime::shader_window