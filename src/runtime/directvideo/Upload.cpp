#include "cockscreen/runtime/DirectVideoWindow.hpp"

#include "cockscreen/runtime/directvideo/Support.hpp"

#include <QElapsedTimer>
#include <QOpenGLContext>

#include <cstring>

#include <libdrm/drm_fourcc.h>

namespace cockscreen::runtime
{

void DirectVideoWindow::upload_latest_frame()
{
    const auto frame = capture_.dequeue();
    if (!frame.has_value())
    {
        return;
    }

    QElapsedTimer upload_timer;
    upload_timer.start();

    const auto expected_stride = bytes_per_line(*frame);
    bool imported_frame = false;

    if (render_mode_ == RenderMode::dmabuf_egl && frame->dmabuf_fd >= 0)
    {
        imported_frame = ensure_dmabuf_texture(*frame);
        if (!imported_frame)
        {
            render_mode_ = RenderMode::cpu_upload;
            current_texture_is_external_ = false;
            texture_id_ = 0;
            status_message_ = QStringLiteral("DMABUF import unavailable, using CPU upload");
        }
    }

    if (!imported_frame)
    {
        ensure_cpu_texture(*frame);
        const std::uint8_t *source = frame->data;
        if (frame->stride != expected_stride)
        {
            staging_.resize(static_cast<std::size_t>(expected_stride) * static_cast<std::size_t>(frame->height));
            for (int row = 0; row < frame->height; ++row)
            {
                std::memcpy(staging_.data() + static_cast<std::size_t>(row) * expected_stride,
                            frame->data + static_cast<std::size_t>(row) * frame->stride,
                            static_cast<std::size_t>(expected_stride));
            }
            source = staging_.data();
        }

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture_id_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

        if (frame->pixel_format == V4l2PixelFormat::yuyv || frame->pixel_format == V4l2PixelFormat::uyvy)
        {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame->width / 2, frame->height, GL_RGBA, GL_UNSIGNED_BYTE, source);
            texture_layout_ = frame->pixel_format == V4l2PixelFormat::yuyv ? 0.0F : 1.0F;
        }
        else if (frame->pixel_format == V4l2PixelFormat::rgb24 || frame->pixel_format == V4l2PixelFormat::bgr24)
        {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame->width, frame->height, GL_RGB, GL_UNSIGNED_BYTE, source);
            texture_layout_ = frame->pixel_format == V4l2PixelFormat::rgb24 ? 2.0F : 3.0F;
        }

        glBindTexture(GL_TEXTURE_2D, 0);
        current_texture_is_external_ = false;
    }

    upload_ms_ = upload_timer.nsecsElapsed() / 1.0e6;

    const auto now = std::chrono::steady_clock::now();
    if (last_capture_time_ != std::chrono::steady_clock::time_point{})
    {
        const double delta_seconds = std::chrono::duration<double>(now - last_capture_time_).count();
        if (delta_seconds > 0.0)
        {
            const double instant_fps = 1.0 / delta_seconds;
            capture_fps_ = capture_fps_ <= 0.0 ? instant_fps : capture_fps_ * 0.85 + instant_fps * 0.15;
        }
    }
    last_capture_time_ = now;

    if (imported_frame || status_message_ != QStringLiteral("DMABUF import unavailable, using CPU upload"))
    {
        status_message_.clear();
    }

    capture_.release();
}

void DirectVideoWindow::ensure_cpu_texture(const V4l2FrameView &frame)
{
    const int target_width = frame.pixel_format == V4l2PixelFormat::yuyv || frame.pixel_format == V4l2PixelFormat::uyvy
                                 ? frame.width / 2
                                 : frame.width;
    const int target_height = frame.height;

    if (texture_id_ == 0 || current_texture_is_external_)
    {
        if (texture_id_ != 0 && current_texture_is_external_)
        {
            texture_id_ = 0;
        }

        glGenTextures(1, &texture_id_);
        current_texture_is_external_ = false;
    }

    if (target_width == texture_width_ && target_height == texture_height_ && frame.pixel_format == current_pixel_format_)
    {
        return;
    }

    video_width_ = frame.width;
    video_height_ = frame.height;
    texture_width_ = target_width;
    texture_height_ = target_height;
    current_pixel_format_ = frame.pixel_format;

    glBindTexture(GL_TEXTURE_2D, texture_id_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    if (frame.pixel_format == V4l2PixelFormat::yuyv || frame.pixel_format == V4l2PixelFormat::uyvy)
    {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, target_width, target_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    }
    else
    {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, target_width, target_height, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
    }

    glBindTexture(GL_TEXTURE_2D, 0);
}

bool DirectVideoWindow::ensure_dmabuf_texture(const V4l2FrameView &frame)
{
    if (!dmabuf_import_ready_ || egl_create_image_ == nullptr || egl_destroy_image_ == nullptr ||
        gl_egl_image_target_texture_2d_ == nullptr)
    {
        return false;
    }

    const auto fourcc = direct_video::drm_fourcc_value(frame.pixel_format);
    if (fourcc == 0 || frame.buffer_index < 0 || frame.dmabuf_fd < 0)
    {
        return false;
    }

    auto &entry = imported_textures_[frame.buffer_index];
    if (entry.texture != 0 && entry.width == frame.width && entry.height == frame.height &&
        entry.pixel_format == frame.pixel_format)
    {
        texture_id_ = entry.texture;
        texture_width_ = frame.width;
        texture_height_ = frame.height;
        video_width_ = frame.width;
        video_height_ = frame.height;
        current_pixel_format_ = frame.pixel_format;
        current_texture_is_external_ = true;
        return true;
    }

    if (entry.image != EGL_NO_IMAGE_KHR)
    {
        egl_destroy_image_(egl_display_, entry.image);
        entry.image = EGL_NO_IMAGE_KHR;
    }

    if (entry.texture != 0)
    {
        glDeleteTextures(1, &entry.texture);
        entry.texture = 0;
    }

    const auto modifier = static_cast<GLuint64>(DRM_FORMAT_MOD_LINEAR);
    std::vector<EGLint> attributes{
        EGL_WIDTH, frame.width,
        EGL_HEIGHT, frame.height,
        EGL_LINUX_DRM_FOURCC_EXT, static_cast<EGLint>(fourcc),
        EGL_DMA_BUF_PLANE0_FD_EXT, frame.dmabuf_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, frame.stride,
    };

    if (egl_import_modifiers_supported_)
    {
        attributes.push_back(EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT);
        attributes.push_back(static_cast<EGLint>(modifier & 0xffffffffULL));
        attributes.push_back(EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT);
        attributes.push_back(static_cast<EGLint>(modifier >> 32));
    }

    attributes.push_back(EGL_NONE);

    entry.image = egl_create_image_(egl_display_, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                                    static_cast<EGLClientBuffer>(nullptr), attributes.data());
    if (entry.image == EGL_NO_IMAGE_KHR)
    {
        return false;
    }

    glGenTextures(1, &entry.texture);
    glBindTexture(direct_video::texture_external_oes(), entry.texture);
    glTexParameteri(direct_video::texture_external_oes(), GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(direct_video::texture_external_oes(), GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(direct_video::texture_external_oes(), GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(direct_video::texture_external_oes(), GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl_egl_image_target_texture_2d_(direct_video::texture_external_oes(), reinterpret_cast<GLeglImageOES>(entry.image));
    glBindTexture(direct_video::texture_external_oes(), 0);

    entry.width = frame.width;
    entry.height = frame.height;
    entry.pixel_format = frame.pixel_format;

    texture_id_ = entry.texture;
    texture_width_ = frame.width;
    texture_height_ = frame.height;
    video_width_ = frame.width;
    video_height_ = frame.height;
    current_pixel_format_ = frame.pixel_format;
    current_texture_is_external_ = true;
    return true;
}

void DirectVideoWindow::destroy_imported_textures()
{
    for (auto &item : imported_textures_)
    {
        auto &entry = item.second;
        if (entry.image != EGL_NO_IMAGE_KHR && egl_destroy_image_ != nullptr)
        {
            egl_destroy_image_(egl_display_, entry.image);
            entry.image = EGL_NO_IMAGE_KHR;
        }

        if (entry.texture != 0)
        {
            if (entry.texture == texture_id_)
            {
                texture_id_ = 0;
            }

            glDeleteTextures(1, &entry.texture);
            entry.texture = 0;
        }
    }

    imported_textures_.clear();
}

bool DirectVideoWindow::initialize_dmabuf_import()
{
    if (!capture_.dmabuf_export_supported())
    {
        return false;
    }

    const auto *context = QOpenGLContext::currentContext();
    if (context == nullptr || !context->isOpenGLES())
    {
        return false;
    }

    egl_display_ = eglGetCurrentDisplay();
    if (egl_display_ == EGL_NO_DISPLAY)
    {
        return false;
    }

    const char *egl_extensions = eglQueryString(egl_display_, EGL_EXTENSIONS);
    if (!direct_video::contains_extension(egl_extensions, "EGL_EXT_image_dma_buf_import"))
    {
        return false;
    }

    egl_import_modifiers_supported_ = direct_video::contains_extension(egl_extensions, "EGL_EXT_image_dma_buf_import_modifiers");

    egl_create_image_ = reinterpret_cast<EglCreateImageProc>(eglGetProcAddress("eglCreateImageKHR"));
    egl_destroy_image_ = reinterpret_cast<EglDestroyImageProc>(eglGetProcAddress("eglDestroyImageKHR"));
    gl_egl_image_target_texture_2d_ = reinterpret_cast<GlEglImageTargetTexture2DProc>(
        context->getProcAddress("glEGLImageTargetTexture2DOES"));

    dmabuf_import_ready_ = egl_create_image_ != nullptr && egl_destroy_image_ != nullptr &&
                           gl_egl_image_target_texture_2d_ != nullptr;
    return dmabuf_import_ready_;
}

int DirectVideoWindow::bytes_per_line(const V4l2FrameView &frame)
{
    switch (frame.pixel_format)
    {
    case V4l2PixelFormat::yuyv:
    case V4l2PixelFormat::uyvy:
        return frame.width * 2;
    case V4l2PixelFormat::rgb24:
    case V4l2PixelFormat::bgr24:
        return frame.width * 3;
    default:
        return frame.stride;
    }
}

std::uint32_t DirectVideoWindow::drm_fourcc_for(V4l2PixelFormat pixel_format)
{
    return direct_video::drm_fourcc_value(pixel_format);
}

} // namespace cockscreen::runtime