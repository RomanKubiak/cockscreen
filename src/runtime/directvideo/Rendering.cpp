#include "cockscreen/runtime/DirectVideoWindow.hpp"

#include "cockscreen/runtime/StatusOverlay.hpp"
#include "cockscreen/runtime/directvideo/Support.hpp"
#include "cockscreen/runtime/shadervideo/Support.hpp"

#include <QElapsedTimer>
#include <QRectF>
#include <QSize>
#include <QVector2D>

#include <chrono>

namespace cockscreen::runtime
{

namespace helper = shader_window;

void DirectVideoWindow::paintGL()
{
    QElapsedTimer render_timer;
    render_timer.start();

    glClear(GL_COLOR_BUFFER_BIT);

    upload_latest_frame();

    const auto now = std::chrono::steady_clock::now();
    const float elapsed_seconds = std::chrono::duration<float>(now - start_time_).count();

    if (texture_id_ != 0)
    {
        static constexpr GLfloat kVertices[] = {
            0.0F, 0.0F, 0.0F, 0.0F,
            1.0F, 0.0F, 1.0F, 0.0F,
            0.0F, 1.0F, 0.0F, 1.0F,
            1.0F, 1.0F, 1.0F, 1.0F,
        };

        QOpenGLShaderProgram *program = &cpu_program_;
        GLenum texture_target = GL_TEXTURE_2D;
        if (current_texture_is_external_ && render_mode_ == RenderMode::dmabuf_egl)
        {
            program = &dmabuf_program_;
            texture_target = direct_video::texture_external_oes();
        }

        if (program->isLinked())
        {
            const auto transform = helper::evaluate_video_transform(video_input_, QSize{width(), height()}, elapsed_seconds);
            const QRectF draw_rect = transform.rect;

            program->bind();
            program->setUniformValue("u_viewport_size", QVector2D{static_cast<float>(width()), static_cast<float>(height())});
            program->setUniformValue("u_video_size", QVector2D{static_cast<float>(video_width_), static_cast<float>(video_height_)});
            program->setUniformValue("u_draw_origin",
                                     QVector2D{static_cast<float>(draw_rect.left()),
                                               static_cast<float>(draw_rect.top())});
            program->setUniformValue("u_draw_size",
                                     QVector2D{static_cast<float>(draw_rect.width()),
                                               static_cast<float>(draw_rect.height())});
            program->setUniformValue("u_rotation_radians",
                                     transform.rotation_degrees * 3.14159265358979323846F / 180.0F);
            program->setUniformValue("u_layout", texture_layout_);
            program->setUniformValue("u_texture", 0);

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(texture_target, texture_id_);

            program->enableAttributeArray("a_position");
            program->enableAttributeArray("a_texcoord");
            program->setAttributeArray("a_position", GL_FLOAT, kVertices, 2, 4 * sizeof(GLfloat));
            program->setAttributeArray("a_texcoord", GL_FLOAT, kVertices + 2, 2, 4 * sizeof(GLfloat));
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            program->disableAttributeArray("a_position");
            program->disableAttributeArray("a_texcoord");
            glBindTexture(texture_target, 0);
            program->release();
        }
    }

    if (status_overlay_ != nullptr)
    {
        status_overlay_->set_status_overlay_text(status_overlay_text_);
        status_overlay_->raise();
    }
    if (last_render_time_ != std::chrono::steady_clock::time_point{})
    {
        const double delta_seconds = std::chrono::duration<double>(now - last_render_time_).count();
        if (delta_seconds > 0.0)
        {
            const double instant_fps = 1.0 / delta_seconds;
            render_fps_ = render_fps_ <= 0.0 ? instant_fps : render_fps_ * 0.85 + instant_fps * 0.15;
        }
    }
    last_render_time_ = now;

    render_ms_ = render_timer.nsecsElapsed() / 1.0e6;
}

} // namespace cockscreen::runtime
