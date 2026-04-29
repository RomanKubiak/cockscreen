#include "cockscreen/runtime/ShaderVideoWindow.hpp"

#include "cockscreen/runtime/StatusOverlay.hpp"
#include "cockscreen/runtime/shadervideo/Support.hpp"

#include <QColor>
#include <QOpenGLShader>
#include <QResizeEvent>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <utility>

namespace cockscreen::runtime
{

namespace
{

float sanitized_playback_rate(float requested_rate, float fallback_rate = 1.0F)
{
    const float candidate = std::isfinite(requested_rate) ? requested_rate : fallback_rate;
    return std::max(candidate, 0.01F);
}

QString playback_media_status_text(QMediaPlayer::MediaStatus status)
{
    switch (status)
    {
        case QMediaPlayer::NoMedia:
            return QStringLiteral("no media");
        case QMediaPlayer::LoadingMedia:
            return QStringLiteral("loading");
        case QMediaPlayer::LoadedMedia:
            return QStringLiteral("loaded");
        case QMediaPlayer::StalledMedia:
            return QStringLiteral("stalled");
        case QMediaPlayer::BufferingMedia:
            return QStringLiteral("buffering");
        case QMediaPlayer::BufferedMedia:
            return QStringLiteral("buffered");
        case QMediaPlayer::EndOfMedia:
            return QStringLiteral("end");
        case QMediaPlayer::InvalidMedia:
            return QStringLiteral("invalid");
    }

    return QStringLiteral("unknown");
}

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
    QObject::connect(&playback_player_, &QMediaPlayer::positionChanged, this, [this](qint64 position) {
        handle_playback_position_changed(static_cast<std::int64_t>(position));
    });
    QObject::connect(&playback_player_, &QMediaPlayer::durationChanged, this, [this](qint64 duration) {
        playback_duration_ms_ = std::max<std::int64_t>(0, static_cast<std::int64_t>(duration));
    });
    QObject::connect(&playback_player_, &QMediaPlayer::mediaStatusChanged, this,
                     [this](QMediaPlayer::MediaStatus status) {
                         playback_status_text_ = playback_media_status_text(status);
                         if (!playback_transport_pending_seek_)
                         {
                             return;
                         }

                         if (status == QMediaPlayer::LoadedMedia || status == QMediaPlayer::BufferedMedia ||
                             status == QMediaPlayer::BufferingMedia)
                         {
                             playback_transport_pending_seek_ = false;
                             configure_playback_transport(true, true);
                         }
                     });
    QObject::connect(&playback_player_, &QMediaPlayer::errorOccurred, this,
                     [this](QMediaPlayer::Error error, const QString &error_string) {
                         if (error == QMediaPlayer::NoError)
                         {
                             playback_error_text_.clear();
                             return;
                         }

                         playback_error_text_ = error_string.trimmed().isEmpty()
                                                    ? QStringLiteral("Qt playback error %1").arg(static_cast<int>(error))
                                                    : error_string.trimmed();
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

    playback_player_.setVideoSink(&playback_sink_);
    restart_playback_source(true);

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

std::int64_t ShaderVideoWindow::playback_position_ms() const
{
    return playback_position_ms_;
}

std::int64_t ShaderVideoWindow::playback_duration_ms() const
{
    return playback_duration_ms_;
}

int ShaderVideoWindow::playback_loops_completed() const
{
    return playback_loops_completed_;
}

double ShaderVideoWindow::playback_current_rate() const
{
    return playback_player_.playbackRate();
}

QString ShaderVideoWindow::playback_error_text() const
{
    return playback_error_text_;
}

QString ShaderVideoWindow::playback_status_text() const
{
    return playback_status_text_;
}

std::optional<std::uintmax_t> ShaderVideoWindow::playback_file_size_bytes() const
{
    return playback_file_size_bytes_;
}

void ShaderVideoWindow::apply_scene_update(SceneDefinition scene)
{
    const bool playback_source_changed = scene_.playback_input.enabled != scene.playback_input.enabled ||
                                         scene_.playback_input.file != scene.playback_input.file;
    const bool playback_transport_changed = scene_.playback_input.start_ms != scene.playback_input.start_ms ||
                                            scene_.playback_input.loop_start_ms != scene.playback_input.loop_start_ms ||
                                            scene_.playback_input.loop_end_ms != scene.playback_input.loop_end_ms ||
                                            scene_.playback_input.loop_repeat != scene.playback_input.loop_repeat ||
                                            scene_.playback_input.playback_rate != scene.playback_input.playback_rate ||
                                            scene_.playback_input.playback_rate_looping !=
                                                scene.playback_input.playback_rate_looping;
    const bool playback_start_changed = scene_.playback_input.start_ms != scene.playback_input.start_ms;

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

    if (playback_source_changed)
    {
        restart_playback_source(true);
    }
    else if (playback_transport_changed)
    {
        configure_playback_transport(playback_start_changed, true);
    }

    update();
}

void ShaderVideoWindow::stop_playback_source()
{
    playback_player_.stop();
    playback_player_.setSource(QUrl{});
    playback_position_ms_ = 0;
    playback_duration_ms_ = 0;
    playback_loops_completed_ = 0;
    playback_transport_pending_seek_ = false;
    playback_error_text_.clear();
    playback_status_text_ = QStringLiteral("idle");
    playback_file_size_bytes_.reset();
    latest_playback_frame_ = QImage{};
    playback_texture_dirty_ = false;
}

void ShaderVideoWindow::restart_playback_source(bool seek_to_start)
{
    latest_playback_frame_ = QImage{};
    playback_texture_dirty_ = false;
    playback_position_ms_ = 0;
    playback_duration_ms_ = 0;
    playback_loops_completed_ = 0;
    playback_error_text_.clear();
    playback_status_text_ = QStringLiteral("idle");
    playback_file_size_bytes_.reset();

    if (!scene_.playback_input.enabled || scene_.playback_input.file.empty())
    {
        stop_playback_source();
        return;
    }

    const auto playback_path = helper::resolve_scene_resource_path(scene_.resources_directory, scene_.playback_input.file);
    if (!playback_path.has_value())
    {
        stop_playback_source();
        status_message_ = QStringLiteral("Playback file not found");
        return;
    }

    std::error_code file_error;
    const auto file_size = std::filesystem::file_size(*playback_path, file_error);
    if (!file_error)
    {
        playback_file_size_bytes_ = file_size;
    }

    playback_transport_pending_seek_ = seek_to_start;
    playback_status_text_ = QStringLiteral("loading");
    playback_player_.stop();
    playback_player_.setSource(QUrl::fromLocalFile(QString::fromStdString(playback_path->string())));
    playback_player_.play();
    configure_playback_transport(seek_to_start, true);
}

void ShaderVideoWindow::configure_playback_transport(bool seek_to_start, bool reset_loop_count)
{
    if (playback_player_.source().isEmpty())
    {
        return;
    }

    if (reset_loop_count)
    {
        playback_loops_completed_ = 0;
    }

    if (seek_to_start)
    {
        playback_position_ms_ = std::max<std::int64_t>(0, scene_.playback_input.start_ms);
        playback_player_.setPosition(playback_position_ms_);
    }

    apply_playback_rate_for_position(playback_position_ms_);
}

bool ShaderVideoWindow::playback_loop_enabled() const
{
    return playback_effective_loop_end_ms().has_value();
}

std::optional<std::int64_t> ShaderVideoWindow::playback_effective_loop_end_ms() const
{
    const auto loop_start_ms = std::max<std::int64_t>(0, scene_.playback_input.loop_start_ms);
    if (scene_.playback_input.loop_end_ms.has_value())
    {
        return *scene_.playback_input.loop_end_ms > loop_start_ms
                   ? std::optional<std::int64_t>{*scene_.playback_input.loop_end_ms}
                   : std::nullopt;
    }

    if (playback_duration_ms_ > loop_start_ms)
    {
        return playback_duration_ms_;
    }

    return std::nullopt;
}

void ShaderVideoWindow::apply_playback_rate_for_position(std::int64_t position_ms)
{
    if (playback_player_.source().isEmpty())
    {
        return;
    }

    const float base_rate = sanitized_playback_rate(scene_.playback_input.playback_rate);
    float target_rate = base_rate;
    if (const auto loop_end_ms = playback_effective_loop_end_ms(); loop_end_ms.has_value())
    {
        const auto loop_start_ms = std::max<std::int64_t>(0, scene_.playback_input.loop_start_ms);
        const bool loop_has_budget = scene_.playback_input.loop_repeat == 0 ||
                                     playback_loops_completed_ < scene_.playback_input.loop_repeat;
        if (loop_has_budget && position_ms >= loop_start_ms && position_ms < *loop_end_ms)
        {
            target_rate = sanitized_playback_rate(scene_.playback_input.playback_rate_looping, base_rate);
        }
    }

    if (std::fabs(playback_player_.playbackRate() - target_rate) > 0.0001F)
    {
        playback_player_.setPlaybackRate(target_rate);
    }
}

void ShaderVideoWindow::handle_playback_position_changed(std::int64_t position_ms)
{
    playback_position_ms_ = std::max<std::int64_t>(0, position_ms);

    if (const auto loop_end_ms = playback_effective_loop_end_ms(); loop_end_ms.has_value())
    {
        const auto loop_start_ms = std::max<std::int64_t>(0, scene_.playback_input.loop_start_ms);
        const bool loop_has_budget = scene_.playback_input.loop_repeat == 0 ||
                                     playback_loops_completed_ < scene_.playback_input.loop_repeat;
        if (loop_has_budget && playback_position_ms_ >= *loop_end_ms)
        {
            ++playback_loops_completed_;
            playback_position_ms_ = loop_start_ms;
            playback_player_.setPosition(loop_start_ms);
            apply_playback_rate_for_position(loop_start_ms);
            return;
        }
    }

    apply_playback_rate_for_position(playback_position_ms_);
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