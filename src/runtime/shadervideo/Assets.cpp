#include "cockscreen/runtime/ShaderVideoWindow.hpp"

#include "cockscreen/runtime/shadervideo/Support.hpp"

#include <QColor>
#include <QImage>
#include <QOpenGLFramebufferObjectFormat>
#include <QPainter>
#include <QPoint>
#include <QRect>

#include <algorithm>
#include <cmath>

namespace cockscreen::runtime
{

namespace helper = shader_window;

void ShaderVideoWindow::handle_frame(const QVideoFrame &frame)
{
    if (!frame.isValid())
    {
        return;
    }

    const QImage image = frame.toImage();
    if (image.isNull())
    {
        return;
    }

    latest_frame_ = image.convertToFormat(QImage::Format_RGBA8888);
    texture_dirty_ = true;
    update();
}

void ShaderVideoWindow::handle_playback_frame(const QVideoFrame &frame)
{
    if (!frame.isValid())
    {
        return;
    }

    const QImage image = frame.toImage();
    if (image.isNull())
    {
        return;
    }

    latest_playback_frame_ = image.convertToFormat(QImage::Format_RGBA8888);
    playback_texture_dirty_ = true;
    update();
}

void ShaderVideoWindow::ensure_texture()
{
    if (texture_id_ == 0)
    {
        glGenTextures(1, &texture_id_);
        glBindTexture(GL_TEXTURE_2D, texture_id_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
}

void ShaderVideoWindow::ensure_playback_texture()
{
    if (playback_texture_id_ == 0)
    {
        glGenTextures(1, &playback_texture_id_);
        glBindTexture(GL_TEXTURE_2D, playback_texture_id_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
}

void ShaderVideoWindow::ensure_note_label_atlas_texture()
{
    if (note_label_atlas_texture_id_ == 0)
    {
        glGenTextures(1, &note_label_atlas_texture_id_);
        glBindTexture(GL_TEXTURE_2D, note_label_atlas_texture_id_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    if (!note_label_atlas_texture_dirty_)
    {
        return;
    }

    QString font_family = helper::note_font_family_for_scene(scene_);
    QImage atlas = helper::build_note_label_atlas_image(font_family);
    const float atlas_coverage = helper::image_opaque_coverage(atlas);
    if ((!helper::image_has_opaque_pixels(atlas) || atlas_coverage > 0.35F) && font_family != QStringLiteral("Sans Serif"))
    {
        atlas = helper::build_note_label_atlas_image(QStringLiteral("Sans Serif"));
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, note_label_atlas_texture_id_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, atlas.width(), atlas.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 atlas.constBits());
    glBindTexture(GL_TEXTURE_2D, 0);
    note_label_atlas_texture_width_ = atlas.width();
    note_label_atlas_texture_height_ = atlas.height();
    note_label_atlas_texture_dirty_ = false;
}

void ShaderVideoWindow::ensure_icon_atlas_texture()
{
    if (!icon_atlas_texture_dirty_)
    {
        return;
    }

    const auto font_path = helper::resolve_scene_resource_path(scene_.resources_directory,
                                                               "fonts/Font Awesome 7 Free-Solid-900.otf");
    if (!font_path.has_value())
    {
        icon_atlas_texture_dirty_ = false;
        return;
    }

    const QImage atlas = helper::build_icon_atlas_image(*font_path);
    if (atlas.isNull() || !helper::image_has_opaque_pixels(atlas))
    {
        icon_atlas_texture_dirty_ = false;
        return;
    }

    if (icon_atlas_texture_id_ == 0)
    {
        glGenTextures(1, &icon_atlas_texture_id_);
        glBindTexture(GL_TEXTURE_2D, icon_atlas_texture_id_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, icon_atlas_texture_id_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, atlas.width(), atlas.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 atlas.constBits());
    glBindTexture(GL_TEXTURE_2D, 0);
    icon_atlas_texture_dirty_ = false;
}

void ShaderVideoWindow::ensure_blank_texture()
{
    if (blank_texture_id_ != 0)
    {
        return;
    }

    static constexpr unsigned char kBlankPixel[] = {0, 0, 0, 0};
    glGenTextures(1, &blank_texture_id_);
    glBindTexture(GL_TEXTURE_2D, blank_texture_id_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, kBlankPixel);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void ShaderVideoWindow::ensure_background_texture()
{
    if (background_texture_id_ == 0)
    {
        glGenTextures(1, &background_texture_id_);
        glBindTexture(GL_TEXTURE_2D, background_texture_id_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    if (!background_texture_dirty_)
    {
        glBindTexture(GL_TEXTURE_2D, 0);
        return;
    }

    const QColor clear_color = helper::scene_clear_color(scene_.background_color);
    const unsigned char pixel[] = {
        static_cast<unsigned char>(std::clamp(clear_color.redF(), 0.0F, 1.0F) * 255.0F),
        static_cast<unsigned char>(std::clamp(clear_color.greenF(), 0.0F, 1.0F) * 255.0F),
        static_cast<unsigned char>(std::clamp(clear_color.blueF(), 0.0F, 1.0F) * 255.0F),
        static_cast<unsigned char>(std::clamp(clear_color.alphaF(), 0.0F, 1.0F) * 255.0F),
    };

    glBindTexture(GL_TEXTURE_2D, background_texture_id_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    glBindTexture(GL_TEXTURE_2D, 0);
    background_texture_dirty_ = false;
}

void ShaderVideoWindow::ensure_background_image_texture()
{
    if (scene_.background_image.file.empty())
    {
        return;
    }

    if (background_image_texture_id_ == 0)
    {
        glGenTextures(1, &background_image_texture_id_);
    }

    if (!background_image_texture_dirty_)
    {
        return;
    }

    const auto background_path = helper::resolve_scene_resource_path(scene_.resources_directory, scene_.background_image.file);
    if (!background_path.has_value())
    {
        status_message_ = QStringLiteral("Background image not found");
        background_image_texture_dirty_ = false;
        return;
    }

    QImage image(QString::fromStdString(background_path->string()));
    if (image.isNull())
    {
        status_message_ = QStringLiteral("Background image could not be loaded");
        background_image_texture_dirty_ = false;
        return;
    }

    const QImage source = image.convertToFormat(QImage::Format_RGBA8888);
    QImage composed{std::max(width(), 1), std::max(height(), 1), QImage::Format_RGBA8888};
    composed.fill(helper::scene_clear_color(scene_.background_color));

    QPainter painter{&composed};
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);

    const QRect target_rect{0, 0, composed.width(), composed.height()};
    switch (scene_.background_image.placement)
    {
        case BackgroundImagePlacement::Center:
            painter.drawImage(QPoint{(composed.width() - source.width()) / 2, (composed.height() - source.height()) / 2}, source);
            break;
        case BackgroundImagePlacement::Stretched:
            painter.drawImage(target_rect, source);
            break;
        case BackgroundImagePlacement::ProportionalStretch:
        {
            const float scale_x = static_cast<float>(composed.width()) / std::max(source.width(), 1);
            const float scale_y = static_cast<float>(composed.height()) / std::max(source.height(), 1);
            const float scale = std::min(scale_x, scale_y);
            const int scaled_width = std::max(1, static_cast<int>(std::round(static_cast<float>(source.width()) * scale)));
            const int scaled_height = std::max(1, static_cast<int>(std::round(static_cast<float>(source.height()) * scale)));
            const QRect scaled_rect{(composed.width() - scaled_width) / 2, (composed.height() - scaled_height) / 2,
                                    scaled_width, scaled_height};
            painter.drawImage(scaled_rect, source);
            break;
        }
        case BackgroundImagePlacement::Tiled:
            for (int y = 0; y < composed.height(); y += source.height())
            {
                for (int x = 0; x < composed.width(); x += source.width())
                {
                    painter.drawImage(QPoint{x, y}, source);
                }
            }
            break;
    }
    painter.end();

    const QImage flipped = helper::vertically_flipped_image(composed);
    glBindTexture(GL_TEXTURE_2D, background_image_texture_id_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, flipped.width(), flipped.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 flipped.constBits());
    glBindTexture(GL_TEXTURE_2D, 0);
    background_image_texture_width_ = flipped.width();
    background_image_texture_height_ = flipped.height();
    background_image_texture_dirty_ = false;
}

void ShaderVideoWindow::ensure_scene_fbos()
{
    const int target_width = width();
    const int target_height = height();
    if (!scene_fbo_dirty_ && video_scene_fbo_ != nullptr && video_scene_fbo_alt_ != nullptr &&
        playback_scene_fbo_ != nullptr && playback_scene_fbo_alt_ != nullptr && screen_scene_fbo_ != nullptr &&
        screen_scene_fbo_alt_ != nullptr && scene_fbo_width_ == target_width && scene_fbo_height_ == target_height)
    {
        return;
    }

    delete video_scene_fbo_;
    video_scene_fbo_ = nullptr;
    delete video_scene_fbo_alt_;
    video_scene_fbo_alt_ = nullptr;
    delete playback_scene_fbo_;
    playback_scene_fbo_ = nullptr;
    delete playback_scene_fbo_alt_;
    playback_scene_fbo_alt_ = nullptr;
    delete screen_scene_fbo_;
    screen_scene_fbo_ = nullptr;
    delete screen_scene_fbo_alt_;
    screen_scene_fbo_alt_ = nullptr;

    if (target_width <= 0 || target_height <= 0)
    {
        scene_fbo_dirty_ = true;
        return;
    }

    QOpenGLFramebufferObjectFormat format;
    format.setAttachment(QOpenGLFramebufferObject::NoAttachment);
    video_scene_fbo_ = new QOpenGLFramebufferObject(target_width, target_height, format);
    video_scene_fbo_alt_ = new QOpenGLFramebufferObject(target_width, target_height, format);
    playback_scene_fbo_ = new QOpenGLFramebufferObject(target_width, target_height, format);
    playback_scene_fbo_alt_ = new QOpenGLFramebufferObject(target_width, target_height, format);
    screen_scene_fbo_ = new QOpenGLFramebufferObject(target_width, target_height, format);
    screen_scene_fbo_alt_ = new QOpenGLFramebufferObject(target_width, target_height, format);

    if (!video_scene_fbo_->isValid() || !video_scene_fbo_alt_->isValid() || !playback_scene_fbo_->isValid() ||
        !playback_scene_fbo_alt_->isValid() || !screen_scene_fbo_->isValid() || !screen_scene_fbo_alt_->isValid())
    {
        record_fatal_render_error(QStringLiteral("Scene framebuffer initialization failed for viewport %1x%2")
                                      .arg(target_width)
                                      .arg(target_height));
    }

    scene_fbo_width_ = target_width;
    scene_fbo_height_ = target_height;
    scene_fbo_dirty_ = false;
}

void ShaderVideoWindow::upload_latest_frame()
{
    if (latest_frame_.isNull() || !texture_dirty_)
    {
        return;
    }

    ensure_texture();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_id_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    const QImage image = helper::vertically_flipped_image(latest_frame_);
    if (texture_width_ != image.width() || texture_height_ != image.height())
    {
        texture_width_ = image.width();
        texture_height_ = image.height();
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texture_width_, texture_height_, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     image.constBits());
    }
    else
    {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, texture_width_, texture_height_, GL_RGBA, GL_UNSIGNED_BYTE,
                        image.constBits());
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    texture_dirty_ = false;
}

void ShaderVideoWindow::upload_latest_playback_frame()
{
    if (latest_playback_frame_.isNull() || !playback_texture_dirty_)
    {
        return;
    }

    ensure_playback_texture();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, playback_texture_id_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    const QImage image = helper::vertically_flipped_image(latest_playback_frame_);
    if (playback_texture_width_ != image.width() || playback_texture_height_ != image.height())
    {
        playback_texture_width_ = image.width();
        playback_texture_height_ = image.height();
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, playback_texture_width_, playback_texture_height_, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, image.constBits());
    }
    else
    {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, playback_texture_width_, playback_texture_height_, GL_RGBA,
                        GL_UNSIGNED_BYTE, image.constBits());
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    playback_texture_dirty_ = false;
}

} // namespace cockscreen::runtime