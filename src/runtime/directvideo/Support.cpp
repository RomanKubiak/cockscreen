#include "cockscreen/runtime/directvideo/Support.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <cstring>
#include <string>

#include <drm/drm_fourcc.h>

namespace cockscreen::runtime::direct_video
{

GLenum texture_external_oes()
{
    return 0x8D65;
}

const char *cpu_vertex_shader()
{
    return R"(
    precision mediump float;
    attribute vec2 a_position;
    attribute vec2 a_texcoord;
    varying vec2 v_texcoord;
    uniform vec2 u_viewport_size;
    uniform vec2 u_video_size;
    uniform float u_status_bar_height;
    void main()
    {
        vec2 usable = vec2(u_viewport_size.x, max(u_viewport_size.y - u_status_bar_height, 1.0));
        vec2 origin = floor((usable - u_video_size) * 0.5);
        vec2 pixel = origin + a_position * u_video_size;
        vec2 ndc = vec2((pixel.x / u_viewport_size.x) * 2.0 - 1.0,
                        1.0 - (pixel.y / u_viewport_size.y) * 2.0);
        gl_Position = vec4(ndc, 0.0, 1.0);
        v_texcoord = vec2(a_texcoord.x, 1.0 - a_texcoord.y);
    }
)";
}

const char *cpu_fragment_shader()
{
    return R"(
    precision mediump float;
    varying vec2 v_texcoord;
    uniform sampler2D u_texture;
    uniform vec2 u_video_size;
    uniform float u_layout;

    vec3 yuv_to_rgb(float y, float u, float v)
    {
        float r = y + 1.402 * v;
        float g = y - 0.344136 * u - 0.714136 * v;
        float b = y + 1.772 * u;
        return clamp(vec3(r, g, b), 0.0, 1.0);
    }

    void main()
    {
        if (u_layout < 1.5)
        {
            float pixel_x = floor(v_texcoord.x * u_video_size.x);
            float pair_width = max(u_video_size.x * 0.5, 1.0);
            vec2 pair_coord = vec2((floor(pixel_x * 0.5) + 0.5) / pair_width, v_texcoord.y);
            vec4 pair = texture2D(u_texture, pair_coord);
            float y = 0.0;
            float u = 0.0;
            float v = 0.0;
            if (u_layout < 0.5)
            {
                y = mod(pixel_x, 2.0) < 0.5 ? pair.r : pair.b;
                u = pair.g - 0.5;
                v = pair.a - 0.5;
            }
            else
            {
                y = mod(pixel_x, 2.0) < 0.5 ? pair.g : pair.a;
                u = pair.r - 0.5;
                v = pair.b - 0.5;
            }
            gl_FragColor = vec4(yuv_to_rgb(y, u, v), 1.0);
            return;
        }

        vec3 rgb = texture2D(u_texture, v_texcoord).rgb;
        if (u_layout > 2.5)
        {
            rgb = rgb.bgr;
        }
        gl_FragColor = vec4(rgb, 1.0);
    }
)";
}

const char *external_fragment_shader()
{
    return R"(
    #extension GL_OES_EGL_image_external : require
    precision mediump float;
    varying vec2 v_texcoord;
    uniform samplerExternalOES u_texture;
    void main()
    {
        gl_FragColor = texture2D(u_texture, v_texcoord);
    }
)";
}

bool contains_extension(const char *extensions, std::string_view needle)
{
    if (extensions == nullptr || needle.empty())
    {
        return false;
    }

    return std::strstr(extensions, std::string{needle}.c_str()) != nullptr;
}

std::uint32_t drm_fourcc_value(V4l2PixelFormat pixel_format)
{
    switch (pixel_format)
    {
    case V4l2PixelFormat::yuyv:
        return DRM_FORMAT_YUYV;
    case V4l2PixelFormat::uyvy:
        return DRM_FORMAT_UYVY;
    case V4l2PixelFormat::rgb24:
        return DRM_FORMAT_RGB888;
    case V4l2PixelFormat::bgr24:
        return DRM_FORMAT_BGR888;
    default:
        return 0;
    }
}

} // namespace cockscreen::runtime::direct_video