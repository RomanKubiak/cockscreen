#include "cockscreen/runtime/ShaderVideoWindow.hpp"

#include "cockscreen/runtime/StatusOverlay.hpp"
#include "cockscreen/runtime/shadervideo/Support.hpp"

#include <QColor>
#include <QOpenGLShader>
#include <QResizeEvent>
#include <QUrl>

#include <filesystem>
#include <utility>

namespace cockscreen::runtime
{

namespace
{

void place_status_overlay(QWidget *widget, StatusOverlay *overlay)
{
    if (widget == nullptr || overlay == nullptr)
    {
        return;
    }

    const int overlay_width = std::max(widget->width() * 9 / 20, 1);
    const int overlay_x = std::max(widget->width() - overlay_width, 0);
    const int overlay_y = widget->height() / 12;
    const int overlay_height = widget->height() * 10 / 12;
    overlay->setGeometry(overlay_x, overlay_y, overlay_width, overlay_height);
}

} // namespace

namespace helper = shader_window;

ShaderVideoWindow::ShaderVideoWindow(const ApplicationSettings &settings, SceneDefinition scene, QCameraDevice video_device,
                                     QString video_label, QString format_label, bool video_on_top,
                                     bool show_status_overlay, QWidget *parent)
    : QOpenGLWidget{parent}, settings_{settings}, scene_{std::move(scene)}, video_label_{std::move(video_label)},
      video_on_top_{video_on_top}, show_status_overlay_{show_status_overlay}, camera_format_label_{std::move(format_label)}
{
    resize(settings_.width, settings_.height);
    setMinimumSize(900, 540);
    setAutoFillBackground(false);
    setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);
    setAttribute(Qt::WA_AcceptTouchEvents, true);
    setCursor(Qt::BlankCursor);

    if (show_status_overlay_)
    {
        status_overlay_ = new StatusOverlay{this};
        place_status_overlay(this, status_overlay_);
        status_overlay_->raise();
    }

    capture_session_.setVideoSink(&video_sink_);
    QObject::connect(&video_sink_, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame &frame) {
        handle_frame(frame);
    });
    QObject::connect(&playback_sink_, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame &frame) {
        handle_playback_frame(frame);
    });

    if (!video_device.isNull())
    {
        camera_ = new QCamera{video_device, this};
        const auto [requested_width, requested_height] = helper::requested_video_dimensions(scene_, settings_);
        if (const auto selected_format = select_camera_format(video_device, requested_width, requested_height);
            selected_format.has_value())
        {
            camera_->setCameraFormat(*selected_format);
            camera_format_label_ = camera_format_label(*selected_format);
        }
        else
        {
            camera_format_label_ = QStringLiteral("unknown");
        }
        capture_session_.setCamera(camera_);
    }

    if (scene_.playback_input.enabled && !scene_.playback_input.file.empty())
    {
        const auto playback_path = helper::resolve_scene_resource_path(scene_.resources_directory, scene_.playback_input.file);
        if (playback_path.has_value())
        {
            playback_player_.setVideoSink(&playback_sink_);
            playback_player_.setLoops(QMediaPlayer::Infinite);
            playback_player_.setSource(QUrl::fromLocalFile(QString::fromStdString(playback_path->string())));
            playback_player_.play();
        }
        else
        {
            status_message_ = QStringLiteral("Playback file not found");
        }
    }

    if (camera_ != nullptr)
    {
        camera_->start();
        if (!camera_->isActive())
        {
            status_message_ = QStringLiteral("Video capture could not start");
        }
    }
    else if (!(scene_.playback_input.enabled && !scene_.playback_input.file.empty()))
    {
        status_message_ = QStringLiteral("No video capture device was found");
    }
}

ShaderVideoWindow::~ShaderVideoWindow()
{
    if (context() == nullptr)
    {
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
        return;
    }

    makeCurrent();
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
    if (texture_id_ != 0)
    {
        glDeleteTextures(1, &texture_id_);
        texture_id_ = 0;
    }
    if (playback_texture_id_ != 0)
    {
        glDeleteTextures(1, &playback_texture_id_);
        playback_texture_id_ = 0;
    }
    if (blank_texture_id_ != 0)
    {
        glDeleteTextures(1, &blank_texture_id_);
        blank_texture_id_ = 0;
    }
    if (background_texture_id_ != 0)
    {
        glDeleteTextures(1, &background_texture_id_);
        background_texture_id_ = 0;
    }
    if (background_image_texture_id_ != 0)
    {
        glDeleteTextures(1, &background_image_texture_id_);
        background_image_texture_id_ = 0;
    }
    if (note_label_atlas_texture_id_ != 0)
    {
        glDeleteTextures(1, &note_label_atlas_texture_id_);
        note_label_atlas_texture_id_ = 0;
    }
    if (icon_atlas_texture_id_ != 0)
    {
        glDeleteTextures(1, &icon_atlas_texture_id_);
        icon_atlas_texture_id_ = 0;
    }
    if (quad_vertex_buffer_.isCreated())
    {
        quad_vertex_buffer_.destroy();
    }
    doneCurrent();
}

double ShaderVideoWindow::processing_fps() const
{
    return processing_fps_;
}

double ShaderVideoWindow::render_fps() const
{
    return render_fps_;
}

QString ShaderVideoWindow::status_message() const
{
    return status_message_;
}

QString ShaderVideoWindow::fatal_render_error() const
{
    return fatal_render_error_;
}

void ShaderVideoWindow::apply_scene_update(SceneDefinition scene)
{
    scene_ = std::move(scene);
    status_message_.clear();
    fatal_render_error_.clear();

    if (fatal_error_overlay_ != nullptr)
    {
        fatal_error_overlay_->hide();
    }

    if (status_overlay_ != nullptr)
    {
        status_overlay_->show();
        status_overlay_->raise();
    }

    background_texture_dirty_ = true;
    background_image_texture_dirty_ = true;
    note_label_atlas_texture_dirty_ = true;
    icon_atlas_texture_dirty_ = true;
    scene_fbo_dirty_ = true;

    if (context() != nullptr)
    {
        makeCurrent();
        build_render_stages();
        doneCurrent();
    }

    update();
}

void ShaderVideoWindow::record_fatal_render_error(QString text)
{
    if (text.isEmpty())
    {
        return;
    }

    if (fatal_render_error_.isEmpty())
    {
        fatal_render_error_ = std::move(text);
        return;
    }

    fatal_render_error_ += QStringLiteral("\n");
    fatal_render_error_ += text;

    if (status_overlay_ != nullptr)
    {
        status_overlay_->hide();
    }

    if (fatal_error_overlay_ == nullptr)
    {
        fatal_error_overlay_ = new StatusOverlay{this};
        fatal_error_overlay_->setGeometry(rect());
    }

    fatal_error_overlay_->set_status_overlay_text(fatal_render_error_);
    fatal_error_overlay_->show();
    fatal_error_overlay_->raise();

    update();
}

void ShaderVideoWindow::set_status_overlay_text(QString text)
{
    status_overlay_text_ = std::move(text);
    if (!fatal_render_error_.isEmpty())
    {
        return;
    }

    if (status_overlay_ != nullptr)
    {
        status_overlay_->set_status_overlay_text(status_overlay_text_);
        status_overlay_->raise();
    }
}

void ShaderVideoWindow::set_frame(const core::ControlFrame &frame)
{
    frame_ = frame;
    update();
}

void ShaderVideoWindow::initializeGL()
{
    initializeOpenGLFunctions();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    if (!quad_vertex_buffer_.isCreated())
    {
        quad_vertex_buffer_.create();
        quad_vertex_buffer_.setUsagePattern(QOpenGLBuffer::DynamicDraw);
    }
    const QColor clear_color = helper::scene_clear_color(scene_.background_color);
    glClearColor(clear_color.redF(), clear_color.greenF(), clear_color.blueF(), clear_color.alphaF());

    build_render_stages();

    const QString fullscreen_vertex_shader_source =
        helper::shader_source_for_current_context(QString::fromUtf8(helper::fullscreen_vertex_shader_source()));

    if (!blit_program_.addShaderFromSourceCode(QOpenGLShader::Vertex, fullscreen_vertex_shader_source) ||
        !blit_program_.addShaderFromSourceCode(
            QOpenGLShader::Fragment,
            helper::shader_source_for_current_context(QString::fromUtf8(helper::passthrough_fragment_shader_source()))) ||
        !blit_program_.link())
    {
        status_message_ = blit_program_.log();
        record_fatal_render_error(QStringLiteral("Blit shader initialization failed:\n%1").arg(blit_program_.log()));
    }

    ensure_blank_texture();
    ensure_background_texture();
    background_image_texture_dirty_ = true;
    ensure_background_image_texture();
    note_label_atlas_texture_dirty_ = true;
    ensure_note_label_atlas_texture();
    icon_atlas_texture_dirty_ = true;
    ensure_icon_atlas_texture();
    scene_fbo_dirty_ = true;
}

void ShaderVideoWindow::resizeEvent(QResizeEvent *event)
{
    QOpenGLWidget::resizeEvent(event);
    scene_fbo_dirty_ = true;
    background_image_texture_dirty_ = true;
    if (status_overlay_ != nullptr)
    {
        place_status_overlay(this, status_overlay_);
        status_overlay_->raise();
    }
    if (fatal_error_overlay_ != nullptr)
    {
        fatal_error_overlay_->setGeometry(rect());
        fatal_error_overlay_->raise();
    }
}

QString ShaderVideoWindow::load_fragment_shader_source(std::string_view shader_file, bool allow_directory_scan) const
{
    if (!shader_file.empty())
    {
        std::filesystem::path shader_path{shader_file};
        if (!shader_path.is_absolute())
        {
            shader_path = std::filesystem::path{settings_.shader_directory} / shader_path;
        }

        const auto resolved_shader_path = helper::resolve_relative_path(shader_path);
        if (!resolved_shader_path.has_value())
        {
            const_cast<ShaderVideoWindow *>(this)->record_fatal_render_error(
                QStringLiteral("Shader import failed: could not resolve '%1'").arg(QString::fromStdString(shader_path.string())));
            return {};
        }

        const auto source = helper::read_text_file_qstring(*resolved_shader_path);
        if (source.isEmpty())
        {
            const_cast<ShaderVideoWindow *>(this)->record_fatal_render_error(
                QStringLiteral("Shader import failed: could not read '%1'")
                    .arg(QString::fromStdString(resolved_shader_path->string())));
        }
        return source;
    }

    if (!allow_directory_scan)
    {
        return {};
    }

    const auto resolved_shader_directory = helper::resolve_relative_path(std::filesystem::path{settings_.shader_directory});
    if (!resolved_shader_directory.has_value())
    {
        return {};
    }

    for (const auto &entry : std::filesystem::directory_iterator{*resolved_shader_directory})
    {
        if (!entry.is_regular_file())
        {
            continue;
        }

        const auto extension = entry.path().extension().string();
        if (extension == ".frag" || extension == ".glsl" || extension == ".vert" || extension == ".comp")
        {
            return helper::read_text_file_qstring(entry.path());
        }
    }

    return {};
}

} // namespace cockscreen::runtime