#include "cockscreen/runtime/DirectVideoWindow.hpp"

#include "cockscreen/runtime/StatusOverlay.hpp"
#include "cockscreen/runtime/directvideo/Support.hpp"

#include <QColor>
#include <QOpenGLShader>
#include <QResizeEvent>
#include <QVector2D>

#include <utility>

namespace cockscreen::runtime
{

DirectVideoWindow::DirectVideoWindow(const ApplicationSettings &settings, QString shader_label, bool show_status_overlay,
                                     QWidget *parent)
    : QOpenGLWidget{parent}, settings_{settings}, shader_label_{std::move(shader_label)},
      show_status_overlay_{show_status_overlay}
{
    setWindowTitle(QString::fromStdString(settings_.window_title));
    resize(settings_.width, settings_.height);
    setMinimumSize(900, 540);
    setAutoFillBackground(false);
    setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAttribute(Qt::WA_AcceptTouchEvents, true);
    setCursor(Qt::BlankCursor);

    if (show_status_overlay_)
    {
        status_overlay_ = new StatusOverlay{this};
        status_overlay_->setGeometry(0, height() - kStatusBarHeight, width(), kStatusBarHeight);
        status_overlay_->raise();
    }

    if (!capture_.open(settings_.video_device, settings_.width, settings_.height, true))
    {
        status_message_ = QString::fromStdString(capture_.error_message());
        return;
    }

    if (!capture_.start())
    {
        status_message_ = QString::fromStdString(capture_.error_message());
        return;
    }

    capture_format_label_ = QString::fromStdString(capture_.format_label());
    render_mode_ = capture_.dmabuf_export_supported() ? RenderMode::dmabuf_egl : RenderMode::cpu_upload;
}

DirectVideoWindow::~DirectVideoWindow()
{
    if (context() == nullptr)
    {
        return;
    }

    makeCurrent();
    destroy_imported_textures();
    if (texture_id_ != 0)
    {
        glDeleteTextures(1, &texture_id_);
        texture_id_ = 0;
    }
    doneCurrent();
}

QString DirectVideoWindow::capture_format_label() const
{
    return capture_format_label_;
}

bool DirectVideoWindow::dmabuf_export_supported() const
{
    return capture_.dmabuf_export_supported();
}

QString DirectVideoWindow::status_message() const
{
    return status_message_;
}

void DirectVideoWindow::set_frame(const core::ControlFrame &frame)
{
    frame_ = frame;
    update();
}

void DirectVideoWindow::initializeGL()
{
    initializeOpenGLFunctions();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glClearColor(0.0F, 0.0F, 0.0F, 1.0F);

    const auto compile_program = [](QOpenGLShaderProgram &program, const char *fragment_shader) {
        if (!program.addShaderFromSourceCode(QOpenGLShader::Vertex, direct_video::cpu_vertex_shader()))
        {
            return false;
        }

        if (!program.addShaderFromSourceCode(QOpenGLShader::Fragment, fragment_shader))
        {
            return false;
        }

        return program.link();
    };

    if (!compile_program(cpu_program_, direct_video::cpu_fragment_shader()))
    {
        status_message_ = cpu_program_.log();
    }

    if (render_mode_ == RenderMode::dmabuf_egl && initialize_dmabuf_import())
    {
        if (!compile_program(dmabuf_program_, direct_video::external_fragment_shader()))
        {
            status_message_ = dmabuf_program_.log();
            render_mode_ = RenderMode::cpu_upload;
        }
    }
    else
    {
        render_mode_ = RenderMode::cpu_upload;
    }
}

void DirectVideoWindow::resizeGL(int, int)
{
}

void DirectVideoWindow::resizeEvent(QResizeEvent *event)
{
    QOpenGLWidget::resizeEvent(event);
    if (status_overlay_ != nullptr)
    {
        status_overlay_->setGeometry(0, height() - kStatusBarHeight, width(), kStatusBarHeight);
        status_overlay_->raise();
    }
}

} // namespace cockscreen::runtime