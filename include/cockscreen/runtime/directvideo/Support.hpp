#pragma once

#include "cockscreen/runtime/V4l2Capture.hpp"

#include <cstdint>
#include <string_view>

#include <qopengl.h>

namespace cockscreen::runtime::direct_video
{

GLenum texture_external_oes();
const char *cpu_vertex_shader();
const char *cpu_fragment_shader();
const char *external_fragment_shader();
bool contains_extension(const char *extensions, std::string_view needle);
std::uint32_t drm_fourcc_value(V4l2PixelFormat pixel_format);

} // namespace cockscreen::runtime::direct_video