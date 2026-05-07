#include "cockscreen/runtime/ShaderVideoWindow.hpp"

#include "cockscreen/runtime/ArtifactInjector.hpp"
#include "cockscreen/runtime/V4l2Capture.hpp"
#include "cockscreen/runtime/shadervideo/Support.hpp"

#include <QColor>
#include <QFont>
#include <QFontDatabase>
#include <QImage>
#include <QVideoFrameFormat>
#include <QOpenGLFramebufferObjectFormat>
#include <QPainter>
#include <QPoint>
#include <QRect>

#include <algorithm>
#include <cmath>

namespace cockscreen::runtime
{

namespace helper = shader_window;

namespace
{

inline unsigned char clamp_byte(int value)
{
    return static_cast<unsigned char>(std::clamp(value, 0, 255));
}

inline void write_rgba_pixel(uchar *pixel, int red, int green, int blue)
{
    pixel[0] = clamp_byte(red);
    pixel[1] = clamp_byte(green);
    pixel[2] = clamp_byte(blue);
    pixel[3] = 255;
}

inline void yuv_to_rgb(int y, int u, int v, int *red, int *green, int *blue)
{
    const int c = y - 16;
    const int d = u - 128;
    const int e = v - 128;
    *red = (298 * c + 409 * e + 128) >> 8;
    *green = (298 * c - 100 * d - 208 * e + 128) >> 8;
    *blue = (298 * c + 516 * d + 128) >> 8;
}

bool image_looks_blank(const QImage &image)
{
    if (image.isNull())
    {
        return true;
    }

    const QImage rgba = image.format() == QImage::Format_RGBA8888 ? image : image.convertToFormat(QImage::Format_RGBA8888);
    if (rgba.isNull())
    {
        return true;
    }

    const int sample_step_x = std::max(rgba.width() / 64, 1);
    const int sample_step_y = std::max(rgba.height() / 36, 1);
    int sample_count = 0;
    int bright_samples = 0;
    int total_luma = 0;

    for (int y = 0; y < rgba.height(); y += sample_step_y)
    {
        const uchar *row = rgba.constScanLine(y);
        for (int x = 0; x < rgba.width(); x += sample_step_x)
        {
            const uchar *pixel = row + x * 4;
            const int luma = (pixel[0] * 2126 + pixel[1] * 7152 + pixel[2] * 722) / 10000;
            total_luma += luma;
            if (luma > 24)
            {
                ++bright_samples;
            }
            ++sample_count;
        }
    }

    if (sample_count == 0)
    {
        return true;
    }

    const double average_luma = static_cast<double>(total_luma) / static_cast<double>(sample_count);
    return average_luma < 12.0 && bright_samples == 0;
}

QImage frame_to_rgba8888(const QVideoFrame &frame)
{
    if (!frame.isValid())
    {
        return {};
    }

    if (const QImage direct = frame.toImage(); !direct.isNull())
    {
        return direct.convertToFormat(QImage::Format_RGBA8888);
    }

    QVideoFrame mapped{frame};
    if (!mapped.map(QVideoFrame::ReadOnly))
    {
        return {};
    }

    const auto pixel_format = mapped.pixelFormat();
    const auto image_format = QVideoFrameFormat::imageFormatFromPixelFormat(pixel_format);
    if (image_format != QImage::Format_Invalid)
    {
        const QImage wrapped{mapped.bits(0), mapped.width(), mapped.height(), mapped.bytesPerLine(0), image_format};
        const QImage converted = wrapped.copy().convertToFormat(QImage::Format_RGBA8888);
        mapped.unmap();
        return converted;
    }

    if (pixel_format == QVideoFrameFormat::Format_YUYV || pixel_format == QVideoFrameFormat::Format_UYVY)
    {
        QImage converted{mapped.width(), mapped.height(), QImage::Format_RGBA8888};
        if (converted.isNull())
        {
            mapped.unmap();
            return {};
        }

        const int width = mapped.width();
        const int height = mapped.height();
        const int stride = mapped.bytesPerLine(0);
        const uchar *source = mapped.bits(0);

        for (int y = 0; y < height; ++y)
        {
            const uchar *row = source + y * stride;
            uchar *dst = converted.scanLine(y);
            for (int x = 0; x < width; x += 2)
            {
                int y0 = 0;
                int y1 = 0;
                int u = 0;
                int v = 0;

                if (pixel_format == QVideoFrameFormat::Format_YUYV)
                {
                    y0 = row[0];
                    u = row[1];
                    y1 = row[2];
                    v = row[3];
                }
                else
                {
                    u = row[0];
                    y0 = row[1];
                    v = row[2];
                    y1 = row[3];
                }

                int red = 0;
                int green = 0;
                int blue = 0;
                yuv_to_rgb(y0, u, v, &red, &green, &blue);
                write_rgba_pixel(dst, red, green, blue);

                if (x + 1 < width)
                {
                    yuv_to_rgb(y1, u, v, &red, &green, &blue);
                    write_rgba_pixel(dst + 4, red, green, blue);
                }

                row += 4;
                dst += 8;
            }
        }

        mapped.unmap();
        return converted;
    }

    mapped.unmap();
    return {};
}

#ifndef _WIN32
QImage frame_to_rgba8888(const V4l2FrameView &frame)
{
    if (frame.data == nullptr || frame.width <= 0 || frame.height <= 0)
    {
        return {};
    }

    if (frame.pixel_format == V4l2PixelFormat::rgb24)
    {
        const QImage wrapped{frame.data, frame.width, frame.height, frame.stride, QImage::Format_RGB888};
        return wrapped.copy().convertToFormat(QImage::Format_RGBA8888);
    }

    if (frame.pixel_format == V4l2PixelFormat::bgr24)
    {
        const QImage wrapped{frame.data, frame.width, frame.height, frame.stride, QImage::Format_BGR888};
        return wrapped.copy().convertToFormat(QImage::Format_RGBA8888);
    }

    if (frame.pixel_format == V4l2PixelFormat::yuyv || frame.pixel_format == V4l2PixelFormat::uyvy)
    {
        QImage converted{frame.width, frame.height, QImage::Format_RGBA8888};
        if (converted.isNull())
        {
            return {};
        }

        const int width = frame.width;
        const int height = frame.height;
        const int stride = frame.stride;
        const auto *source = reinterpret_cast<const uchar *>(frame.data);

        for (int y = 0; y < height; ++y)
        {
            const uchar *row = source + y * stride;
            uchar *dst = converted.scanLine(y);
            for (int x = 0; x < width; x += 2)
            {
                int y0 = 0;
                int y1 = 0;
                int u = 0;
                int v = 0;

                if (frame.pixel_format == V4l2PixelFormat::yuyv)
                {
                    y0 = row[0];
                    u = row[1];
                    y1 = row[2];
                    v = row[3];
                }
                else
                {
                    u = row[0];
                    y0 = row[1];
                    v = row[2];
                    y1 = row[3];
                }

                int red = 0;
                int green = 0;
                int blue = 0;
                yuv_to_rgb(y0, u, v, &red, &green, &blue);
                write_rgba_pixel(dst, red, green, blue);

                if (x + 1 < width)
                {
                    yuv_to_rgb(y1, u, v, &red, &green, &blue);
                    write_rgba_pixel(dst + 4, red, green, blue);
                }

                row += 4;
                dst += 8;
            }
        }

        return converted;
    }

    return {};
}
#endif

} // namespace

QImage ShaderVideoWindow::build_no_signal_frame(int width, int height)
{
    QImage image{width, height, QImage::Format_RGBA8888};
    if (image.isNull())
    {
        return {};
    }

    image.fill(QColor{0, 0, 0});

    QPainter painter{&image};
    painter.setRenderHint(QPainter::Antialiasing, false);

    const int bar_height = std::max(height * 2 / 3, 1);
    const int bar_width = std::max(width / 7, 1);
    const QColor bars[] = {
        QColor{235, 235, 235},
        QColor{235, 235, 0},
        QColor{0, 235, 235},
        QColor{0, 235, 0},
        QColor{235, 0, 235},
        QColor{235, 0, 0},
        QColor{0, 0, 235},
    };

    for (int index = 0; index < 7; ++index)
    {
        painter.fillRect(index * bar_width, 0, index == 6 ? width - index * bar_width : bar_width, bar_height,
                         bars[index]);
    }

    painter.fillRect(0, bar_height, width, std::max(height / 12, 1), QColor{32, 32, 32});
    painter.fillRect(0, bar_height + std::max(height / 12, 1), width, height - bar_height,
                     QColor{8, 8, 8});

    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setBold(true);
    font.setPointSize(std::max(height / 12, 18));
    painter.setFont(font);
    painter.setPen(QColor{255, 255, 255});
    painter.drawText(image.rect(), Qt::AlignCenter, QStringLiteral("NO SIGNAL"));

    font.setBold(false);
    font.setPointSize(std::max(height / 28, 11));
    painter.setFont(font);
    painter.setPen(QColor{220, 220, 220});
    const QRect info_rect{0, bar_height + std::max(height / 16, 1), width, std::max(height / 4, 1)};
    painter.drawText(info_rect, Qt::AlignHCenter | Qt::AlignTop,
                     QStringLiteral("raw V4L2 capture is active, but no frames have arrived yet"));
    return image;
}

void ShaderVideoWindow::handle_frame(const QVideoFrame &frame)
{
    const QImage image = frame_to_rgba8888(frame);
    if (image.isNull())
    {
        return;
    }

    latest_frame_ = image;
    texture_dirty_ = true;
    update();
}

void ShaderVideoWindow::handle_playback_frame(const QVideoFrame &frame)
{
    const QImage image = frame_to_rgba8888(frame);
    if (image.isNull())
    {
        return;
    }

    latest_playback_frame_ = image;
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
        background_image_texture_width_ = 0;
        background_image_texture_height_ = 0;
        background_image_texture_dirty_ = false;
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
        background_image_texture_width_ = 0;
        background_image_texture_height_ = 0;
        background_image_texture_dirty_ = false;
        return;
    }

    QImage image(QString::fromStdString(background_path->string()));
    if (image.isNull())
    {
        status_message_ = QStringLiteral("Background image could not be loaded");
        background_image_texture_width_ = 0;
        background_image_texture_height_ = 0;
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
#ifndef _WIN32
    if (raw_video_capture_active_)
    {
        bool received_frame = false;
        const auto raw_frame = raw_video_capture_.dequeue();
        if (raw_frame.has_value())
        {
            const QImage image = frame_to_rgba8888(*raw_frame);
            raw_video_capture_.release();
            if (!image.isNull())
            {
                if (image_looks_blank(image))
                {
                    ++raw_video_blank_frame_count_;
                    if (raw_video_blank_frame_count_ >= 12)
                    {
                        latest_frame_ = build_no_signal_frame(std::max(settings_.width, 640), std::max(settings_.height, 360));
                        texture_dirty_ = true;
                        raw_video_placeholder_shown_ = true;
                        if (status_message_.isEmpty())
                        {
                            status_message_ = QStringLiteral("Analog input appears blank");
                        }
                        received_frame = true;
                    }
                }
                else
                {
                    raw_video_blank_frame_count_ = 0;
                    latest_frame_ = image;
                    texture_dirty_ = true;
                    raw_video_frame_received_ = true;
                    raw_video_placeholder_shown_ = false;
                    received_frame = true;
                    if (status_message_ == QStringLiteral("No raw video frames yet") ||
                        status_message_ == QStringLiteral("Analog input appears blank"))
                    {
                        status_message_.clear();
                    }
                }
            }
        }

        if (!received_frame && !raw_video_frame_received_ &&
            std::chrono::steady_clock::now() - raw_video_capture_start_time_ > std::chrono::milliseconds{1500} &&
            latest_frame_.isNull() && !raw_video_placeholder_shown_)
        {
            latest_frame_ = build_no_signal_frame(std::max(settings_.width, 640), std::max(settings_.height, 360));
            texture_dirty_ = true;
            raw_video_placeholder_shown_ = true;
            if (status_message_.isEmpty())
            {
                status_message_ = QStringLiteral("No raw video frames yet");
            }
        }
    }
#endif

    if (latest_frame_.isNull() || !texture_dirty_)
    {
        return;
    }

    ensure_texture();

    // Artifact injection (Step 1) — video layer.
    if (scene_.video_input.artifact.enabled)
    {
        // QImage::Format_RGBA8888 is packed; bits() returns a mutable pointer when the
        // image is not shared, so detach first to guarantee exclusive ownership.
        latest_frame_.detach();
        const int width_bytes = latest_frame_.width() * 4;
        apply_artifact(latest_frame_.bits(), width_bytes, latest_frame_.height(),
                       latest_frame_.bytesPerLine(), scene_.video_input.artifact,
                       video_artifact_frame_counter_++);
    }

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

    // Artifact injection (Step 1) — playback layer.
    if (scene_.playback_input.artifact.enabled)
    {
        latest_playback_frame_.detach();
        const int width_bytes = latest_playback_frame_.width() * 4;
        apply_artifact(latest_playback_frame_.bits(), width_bytes, latest_playback_frame_.height(),
                       latest_playback_frame_.bytesPerLine(), scene_.playback_input.artifact,
                       playback_artifact_frame_counter_++);
    }

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