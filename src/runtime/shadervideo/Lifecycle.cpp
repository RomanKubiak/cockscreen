#include "cockscreen/runtime/ShaderVideoWindow.hpp"

#include "cockscreen/runtime/StatusOverlay.hpp"
#include "cockscreen/runtime/shadervideo/Support.hpp"
#include "cockscreen/runtime/audioanalysis/Support.hpp"

#include <QAudioBuffer>
#include <QAudioFormat>
#include <QColor>
#include <QOpenGLShader>
#include <QResizeEvent>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
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
                                     QString video_device_path, QString video_label, QString format_label, bool video_on_top,
                                     bool show_status_overlay, QWidget *parent)
    : QOpenGLWidget{parent}, settings_{settings}, scene_{std::move(scene)}, video_label_{std::move(video_label)},
      video_on_top_{video_on_top}, show_status_overlay_{show_status_overlay}, camera_format_label_{std::move(format_label)}
{
    Q_UNUSED(video_device_path);
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

    const auto [requested_width, requested_height] = helper::requested_video_dimensions(scene_, settings_);
    const bool use_camera_capture = !video_label_.startsWith(QStringLiteral("appsink:")) && !video_device.isNull();
    if (use_camera_capture)
    {
        camera_ = new QCamera{video_device, this};
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

#ifndef _WIN32
    // --- Step 2 (playback layer): start the loopback pipeline if configured.
    if (scene_.playback_input.loopback.enabled && !scene_.playback_input.file.empty())
    {
        if (scene_.playback_input.loopback.use_appsink)
        {
            // Receiver first: bind the UDP socket before the sender starts
            // transmitting so the receiver catches the very first IDR frame.
            playback_appsink_capture_ = new AppsinkCapture;
            const bool use_h264 = LoopbackPipeline::uses_h264();
            if (!playback_appsink_capture_->start(
                    scene_.playback_input.loopback.udp_port, &playback_sink_, use_h264))
            {
                const QString error_text =
                    QStringLiteral("Playback appsink capture failed: %1")
                        .arg(playback_appsink_capture_->status_message());
                fatal_render_error_ = error_text;
                status_message_ = error_text;
                delete playback_appsink_capture_;
                playback_appsink_capture_ = nullptr;
            }
            else
            {
                std::cerr << "[appsink-playback] "
                          << playback_appsink_capture_->status_message().toStdString() << "\n";
            }

            // Sender-only: GStreamer encodes the file → RTP → UDP.
            // Starts after the receiver is listening so the first IDR is received.
            const std::int64_t loop_end_ms = scene_.playback_input.loop_end_ms.has_value()
                                                 ? *scene_.playback_input.loop_end_ms
                                                 : -1;
            const bool started = playback_loopback_.start_sender_only_for_file(
                scene_.playback_input.file,
                scene_.playback_input.loopback,
                scene_.playback_input.start_ms,
                scene_.playback_input.loop_start_ms,
                loop_end_ms);
            if (!started)
            {
                fatal_render_error_ = QStringLiteral("Playback loopback sender failed to start: ") +
                                      playback_loopback_.status_message();
                status_message_ = fatal_render_error_;
                if (playback_appsink_capture_ != nullptr)
                {
                    playback_appsink_capture_->stop();
                    delete playback_appsink_capture_;
                    playback_appsink_capture_ = nullptr;
                }
            }
        }
        else
        {
            // Classic v4l2loopback path.
            QString prereq_error;
            if (!LoopbackPipeline::check_prerequisites(scene_.playback_input.loopback, &prereq_error))
            {
                fatal_render_error_ = prereq_error;
                status_message_ = prereq_error;
            }
            else
            {
                const bool started = playback_loopback_.start_for_file(
                    scene_.playback_input.file, scene_.playback_input.loopback);

                if (started)
                {
                    if (!playback_loopback_.wait_for_device_ready(8000))
                    {
                        fatal_render_error_ = QStringLiteral("Playback loopback device not ready: ") +
                                              playback_loopback_.status_message();
                        status_message_ = fatal_render_error_;
                        playback_loopback_.stop();
                    }
                    else
                    {
                        playback_loopback_capture_ = new LoopbackCapture;
                        if (!playback_loopback_capture_->start(
                                scene_.playback_input.loopback.loopback_device, &playback_sink_,
                                settings_.verbose_debug))
                        {
                            const QString error_text =
                                QStringLiteral("Playback loopback capture failed: %1")
                                    .arg(playback_loopback_capture_->status_message());
                            fatal_render_error_ = error_text;
                            status_message_ = error_text;
                            delete playback_loopback_capture_;
                            playback_loopback_capture_ = nullptr;
                            playback_loopback_.stop();
                        }
                    }
                }
                else
                {
                    fatal_render_error_ = QStringLiteral("Playback loopback failed to start: ") +
                                          playback_loopback_.status_message();
                    status_message_ = fatal_render_error_;
                }
            }
        }
    }
    else
#endif
    {
        restart_playback_source(true);
    }

    audio_playback_player_.setAudioOutput(&audio_playback_audio_output_);
    audio_playback_player_.setAudioBufferOutput(&audio_playback_buffer_output_);
    QObject::connect(&audio_playback_buffer_output_, &QAudioBufferOutput::audioBufferReceived, this,
                     [this](const QAudioBuffer &buffer) {
                         process_audio_playback_buffer(buffer);
                     });
    QObject::connect(&audio_playback_player_, &QMediaPlayer::positionChanged, this, [this](qint64 position) {
        handle_audio_playback_position_changed(static_cast<std::int64_t>(position));
    });
    QObject::connect(&audio_playback_player_, &QMediaPlayer::durationChanged, this, [this](qint64 duration) {
        audio_playback_duration_ms_ = std::max<std::int64_t>(0, static_cast<std::int64_t>(duration));
    });
    QObject::connect(&audio_playback_player_, &QMediaPlayer::mediaStatusChanged, this,
                     [this](QMediaPlayer::MediaStatus status) {
                         audio_playback_status_text_ = playback_media_status_text(status);
                         if (status == QMediaPlayer::EndOfMedia && !audio_playback_outro_active_)
                         {
                             audio_playback_outro_active_ = true;
                         }
                         if (!audio_playback_transport_pending_seek_)
                         {
                             return;
                         }
                         if (status == QMediaPlayer::LoadedMedia || status == QMediaPlayer::BufferedMedia ||
                             status == QMediaPlayer::BufferingMedia)
                         {
                             audio_playback_transport_pending_seek_ = false;
                             configure_audio_playback_transport(true, true);
                         }
                     });
    QObject::connect(&audio_playback_player_, &QMediaPlayer::errorOccurred, this,
                     [this](QMediaPlayer::Error error, const QString &error_string) {
                         if (error == QMediaPlayer::NoError)
                         {
                             audio_playback_error_text_.clear();
                             return;
                         }
                         audio_playback_error_text_ = error_string.trimmed().isEmpty()
                                                          ? QStringLiteral("Audio playback error %1").arg(static_cast<int>(error))
                                                          : error_string.trimmed();
                     });
    restart_audio_playback_source(true);

    // Volume-fade timer: fires every 50 ms, calls tick_audio_playback_volume().
    audio_playback_volume_timer_.setInterval(50);
    QObject::connect(&audio_playback_volume_timer_, &QTimer::timeout, this, [this]() {
        tick_audio_playback_volume();
    });
    audio_playback_volume_timer_.start();

    // Build Hann window for the playback FFT analysis.
    for (int i = 0; i < kAudioPlaybackFftSize; ++i)
    {
        const float phase = static_cast<float>(i) / static_cast<float>(kAudioPlaybackFftSize - 1);
        audio_playback_fft_window_[static_cast<std::size_t>(i)] =
            0.5F - 0.5F * std::cos(2.0F * audio_analysis::kPi * phase);
    }

    if (camera_ != nullptr)
    {
        camera_->start();
        if (!camera_->isActive())
        {
            status_message_ = QStringLiteral("Video capture could not start");
        }
    }
    else if (status_message_.isEmpty() && !video_label_.startsWith(QStringLiteral("appsink:")) &&
             !(scene_.playback_input.enabled && !scene_.playback_input.file.empty()))
    {
        status_message_ = QStringLiteral("No video capture device was found");
    }
}

ShaderVideoWindow::~ShaderVideoWindow()
{
#ifndef _WIN32
    // Stop the loopback capture thread and GStreamer subprocesses first so the
    // V4L2 file descriptors are closed before the GL context is torn down.
    // If this is skipped, the fd stays open across runs and the next run's
    // GStreamer receiver gets EBUSY when trying to open the same device.
    if (playback_loopback_capture_ != nullptr)
    {
        playback_loopback_capture_->stop();
        delete playback_loopback_capture_;
        playback_loopback_capture_ = nullptr;
    }
    if (playback_appsink_capture_ != nullptr)
    {
        playback_appsink_capture_->stop();
        delete playback_appsink_capture_;
        playback_appsink_capture_ = nullptr;
    }
    playback_loopback_.stop();
#endif

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

QImage ShaderVideoWindow::latest_video_frame_image() const
{
    return latest_frame_.copy();
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

    const bool audio_source_changed = scene_.audio_playback_input.enabled != scene.audio_playback_input.enabled ||
                                      scene_.audio_playback_input.file != scene.audio_playback_input.file;
    const bool audio_transport_changed = scene_.audio_playback_input.start_ms != scene.audio_playback_input.start_ms ||
                                         scene_.audio_playback_input.loop_start_ms != scene.audio_playback_input.loop_start_ms ||
                                         scene_.audio_playback_input.loop_end_ms != scene.audio_playback_input.loop_end_ms ||
                                         scene_.audio_playback_input.loop_repeat != scene.audio_playback_input.loop_repeat ||
                                         scene_.audio_playback_input.playback_rate != scene.audio_playback_input.playback_rate ||
                                         scene_.audio_playback_input.playback_rate_looping !=
                                             scene.audio_playback_input.playback_rate_looping;
    const bool audio_start_changed = scene_.audio_playback_input.start_ms != scene.audio_playback_input.start_ms;

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

    if (audio_source_changed)
    {
        restart_audio_playback_source(true);
    }
    else if (audio_transport_changed)
    {
        configure_audio_playback_transport(audio_start_changed, true);
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

// ---- Audio-only playback ---------------------------------------------------

void ShaderVideoWindow::stop_audio_playback_source()
{
    audio_playback_player_.stop();
    audio_playback_player_.setSource(QUrl{});
    audio_playback_position_ms_ = 0;
    audio_playback_duration_ms_ = 0;
    audio_playback_loops_completed_ = 0;
    audio_playback_transport_pending_seek_ = false;
    audio_playback_error_text_.clear();
    audio_playback_status_text_ = QStringLiteral("idle");
    audio_playback_current_volume_ = 0.0F;
    audio_playback_outro_active_ = false;
    audio_playback_audio_output_.setVolume(0.0F);
    audio_playback_fft_bands_.fill(0.0F);
    audio_playback_waveform_.fill(0.0F);
    audio_playback_fft_sample_count_ = 0;
    audio_playback_analysis_rms_ = 0.0F;
    audio_playback_analysis_peak_ = 0.0F;
}

void ShaderVideoWindow::restart_audio_playback_source(bool seek_to_start)
{
    audio_playback_position_ms_ = 0;
    audio_playback_duration_ms_ = 0;
    audio_playback_loops_completed_ = 0;
    audio_playback_error_text_.clear();
    audio_playback_status_text_ = QStringLiteral("idle");
    audio_playback_outro_active_ = false;
    // Start at the configured initial volume.
    audio_playback_current_volume_ = std::clamp(scene_.audio_playback_input.volume_initial, 0.0F, 1.0F);
    audio_playback_audio_output_.setVolume(audio_playback_current_volume_);

    if (!scene_.audio_playback_input.enabled || scene_.audio_playback_input.file.empty())
    {
        stop_audio_playback_source();
        return;
    }

    const auto audio_path = helper::resolve_scene_resource_path(scene_.resources_directory,
                                                                scene_.audio_playback_input.file);
    if (!audio_path.has_value())
    {
        stop_audio_playback_source();
        status_message_ = QStringLiteral("Audio playback file not found");
        return;
    }

    audio_playback_transport_pending_seek_ = seek_to_start;
    audio_playback_status_text_ = QStringLiteral("loading");
    audio_playback_player_.stop();
    audio_playback_player_.setSource(QUrl::fromLocalFile(QString::fromStdString(audio_path->string())));
    audio_playback_player_.play();
    configure_audio_playback_transport(seek_to_start, true);
}

void ShaderVideoWindow::configure_audio_playback_transport(bool seek_to_start, bool reset_loop_count)
{
    if (audio_playback_player_.source().isEmpty())
    {
        return;
    }

    if (reset_loop_count)
    {
        audio_playback_loops_completed_ = 0;
    }

    if (seek_to_start)
    {
        audio_playback_position_ms_ = std::max<std::int64_t>(0, scene_.audio_playback_input.start_ms);
        audio_playback_player_.setPosition(audio_playback_position_ms_);
    }

    apply_audio_playback_rate_for_position(audio_playback_position_ms_);
}

bool ShaderVideoWindow::audio_playback_loop_enabled() const
{
    return audio_playback_effective_loop_end_ms().has_value();
}

std::optional<std::int64_t> ShaderVideoWindow::audio_playback_effective_loop_end_ms() const
{
    const auto loop_start_ms = std::max<std::int64_t>(0, scene_.audio_playback_input.loop_start_ms);
    if (scene_.audio_playback_input.loop_end_ms.has_value())
    {
        return *scene_.audio_playback_input.loop_end_ms > loop_start_ms
                   ? std::optional<std::int64_t>{*scene_.audio_playback_input.loop_end_ms}
                   : std::nullopt;
    }

    if (audio_playback_duration_ms_ > loop_start_ms)
    {
        return audio_playback_duration_ms_;
    }

    return std::nullopt;
}

void ShaderVideoWindow::apply_audio_playback_rate_for_position(std::int64_t position_ms)
{
    if (audio_playback_player_.source().isEmpty())
    {
        return;
    }

    const float base_rate = sanitized_playback_rate(scene_.audio_playback_input.playback_rate);
    float target_rate = base_rate;
    if (const auto loop_end_ms = audio_playback_effective_loop_end_ms(); loop_end_ms.has_value())
    {
        const auto loop_start_ms = std::max<std::int64_t>(0, scene_.audio_playback_input.loop_start_ms);
        const bool loop_has_budget = scene_.audio_playback_input.loop_repeat == 0 ||
                                     audio_playback_loops_completed_ < scene_.audio_playback_input.loop_repeat;
        if (loop_has_budget && position_ms >= loop_start_ms && position_ms < *loop_end_ms)
        {
            target_rate = sanitized_playback_rate(scene_.audio_playback_input.playback_rate_looping, base_rate);
        }
    }

    if (std::fabs(audio_playback_player_.playbackRate() - target_rate) > 0.0001F)
    {
        audio_playback_player_.setPlaybackRate(target_rate);
    }
}

void ShaderVideoWindow::handle_audio_playback_position_changed(std::int64_t position_ms)
{
    audio_playback_position_ms_ = std::max<std::int64_t>(0, position_ms);

    if (const auto loop_end_ms = audio_playback_effective_loop_end_ms(); loop_end_ms.has_value())
    {
        const auto loop_start_ms = std::max<std::int64_t>(0, scene_.audio_playback_input.loop_start_ms);
        const bool loop_has_budget = scene_.audio_playback_input.loop_repeat == 0 ||
                                     audio_playback_loops_completed_ < scene_.audio_playback_input.loop_repeat;
        if (loop_has_budget && audio_playback_position_ms_ >= *loop_end_ms)
        {
            ++audio_playback_loops_completed_;
            audio_playback_position_ms_ = loop_start_ms;
            audio_playback_player_.setPosition(loop_start_ms);
            apply_audio_playback_rate_for_position(loop_start_ms);
            return;
        }

        // Budget exhausted — begin outro fade if configured.
        if (!loop_has_budget && !audio_playback_outro_active_ && audio_playback_position_ms_ >= *loop_end_ms)
        {
            audio_playback_outro_active_ = true;
        }
    }

    apply_audio_playback_rate_for_position(audio_playback_position_ms_);
}

void ShaderVideoWindow::tick_audio_playback_volume()
{
    const auto &cfg = scene_.audio_playback_input;
    if (!cfg.enabled || cfg.file.empty() || audio_playback_player_.source().isEmpty())
    {
        return;
    }

    const float peak   = std::clamp(cfg.volume, 0.0F, 1.0F);
    const float init_v = std::clamp(cfg.volume_initial, 0.0F, 1.0F);
    const std::int64_t pos = audio_playback_position_ms_;

    float target = peak;

    // ---- outro (after final loop or end-of-media) -------------------------
    if (audio_playback_outro_active_)
    {
        if (cfg.volume_fade_out_ms > 0)
        {
            // outro_active_ is set when we detect the final loop end / EOM.
            // We simply keep fading toward 0 at the configured rate each tick.
            const float step = static_cast<float>(50) / static_cast<float>(cfg.volume_fade_out_ms);
            audio_playback_current_volume_ = std::max(0.0F, audio_playback_current_volume_ - step * peak);
            audio_playback_audio_output_.setVolume(audio_playback_current_volume_);
        }
        else
        {
            audio_playback_audio_output_.setVolume(0.0F);
            audio_playback_current_volume_ = 0.0F;
        }
        return;
    }

    // ---- intro fade-in (from start_ms) ------------------------------------
    const std::int64_t rel = pos - std::max<std::int64_t>(0, cfg.start_ms);
    if (cfg.volume_fade_in_ms > 0 && rel >= 0 && rel < cfg.volume_fade_in_ms)
    {
        const float t = static_cast<float>(rel) / static_cast<float>(cfg.volume_fade_in_ms);
        target = init_v + t * (peak - init_v);
    }
    // ---- per-loop fade-in/out ---------------------------------------------
    else if (const auto loop_end_opt = audio_playback_effective_loop_end_ms(); loop_end_opt.has_value())
    {
        const std::int64_t loop_start = std::max<std::int64_t>(0, cfg.loop_start_ms);
        const std::int64_t loop_end   = *loop_end_opt;
        const std::int64_t loop_len   = loop_end - loop_start;

        if (pos >= loop_start && pos < loop_end && loop_len > 0)
        {
            const std::int64_t rel_loop = pos - loop_start;
            const std::int64_t rem_loop = loop_end - pos;

            // Loop fade-in
            if (cfg.volume_loop_fade_in_ms > 0 && rel_loop < cfg.volume_loop_fade_in_ms)
            {
                const float t = static_cast<float>(rel_loop) / static_cast<float>(cfg.volume_loop_fade_in_ms);
                target = t * peak;
            }
            // Loop fade-out (takes priority if both overlap)
            if (cfg.volume_loop_fade_out_ms > 0 && rem_loop < cfg.volume_loop_fade_out_ms)
            {
                const float t = static_cast<float>(rem_loop) / static_cast<float>(cfg.volume_loop_fade_out_ms);
                target = std::min(target, t * peak);
            }
        }
    }

    // Smooth the volume toward target: step ≤ 50 ms worth of full-range change.
    constexpr float kMaxStep = 0.02F; // at most 2% per 50 ms tick if no fade configured
    const float delta = target - audio_playback_current_volume_;
    if (std::fabs(delta) > kMaxStep)
    {
        audio_playback_current_volume_ += (delta > 0.0F ? kMaxStep : -kMaxStep);
    }
    else
    {
        audio_playback_current_volume_ = target;
    }

    audio_playback_audio_output_.setVolume(std::clamp(audio_playback_current_volume_, 0.0F, 1.0F));
}

void ShaderVideoWindow::process_audio_playback_buffer(const QAudioBuffer &buffer)
{
    if (!buffer.isValid() || buffer.frameCount() <= 0)
    {
        return;
    }

    const QAudioFormat fmt = buffer.format();
    const int channel_count = std::max(fmt.channelCount(), 1);
    const int frame_count = static_cast<int>(buffer.frameCount());
    const QAudioFormat::SampleFormat sample_fmt = fmt.sampleFormat();
    const auto *bytes = reinterpret_cast<const unsigned char *>(buffer.constData<char>());
    const int sample_bytes = fmt.bytesPerSample();

    if (sample_bytes <= 0)
    {
        return;
    }

    const int frame_bytes = sample_bytes * channel_count;
    double sum_squares = 0.0;
    float chunk_peak = 0.0F;

    for (int frame = 0; frame < frame_count; ++frame)
    {
        double mono_sum = 0.0;
        for (int ch = 0; ch < std::min(channel_count, 2); ++ch)
        {
            const int offset = frame * frame_bytes + ch * sample_bytes;
            double value = 0.0;
            switch (sample_fmt)
            {
            case QAudioFormat::UInt8:
                value = audio_analysis::sample_to_float(bytes[offset]);
                break;
            case QAudioFormat::Int16:
            {
                qint16 s = 0;
                std::memcpy(&s, bytes + offset, sizeof(s));
                value = audio_analysis::sample_to_float(s);
                break;
            }
            case QAudioFormat::Int32:
            {
                qint32 s = 0;
                std::memcpy(&s, bytes + offset, sizeof(s));
                value = audio_analysis::sample_to_float(s);
                break;
            }
            case QAudioFormat::Float:
            {
                float s = 0.0F;
                std::memcpy(&s, bytes + offset, sizeof(s));
                value = audio_analysis::sample_to_float(s);
                break;
            }
            default:
                return;
            }
            mono_sum += value;
        }

        const float mono = static_cast<float>(mono_sum / static_cast<double>(std::min(channel_count, 2)));
        sum_squares += static_cast<double>(mono) * static_cast<double>(mono);
        chunk_peak = std::max(chunk_peak, std::fabs(mono));

        // Feed FFT ring buffer.
        const auto idx = static_cast<std::size_t>(audio_playback_fft_sample_count_);
        if (idx < audio_playback_fft_sample_buffer_.size())
        {
            audio_playback_fft_sample_buffer_[idx] = mono;
            ++audio_playback_fft_sample_count_;
        }

        if (audio_playback_fft_sample_count_ >= audio_playback_fft_sample_buffer_.size())
        {
            // Run FFT.
            if (audio_playback_fft_.isValid())
            {
                for (std::size_t k = 0; k < audio_playback_fft_sample_buffer_.size(); ++k)
                {
                    audio_playback_fft_input_[k] = audio_playback_fft_sample_buffer_[k] * audio_playback_fft_window_[k];
                }
                audio_playback_fft_.forward(audio_playback_fft_input_, audio_playback_fft_spectrum_);

                constexpr float kBandCurve = 2.4F;
                const int spectrum_bins = static_cast<int>(audio_playback_fft_spectrum_.size());
                const int positive_bins = std::max(spectrum_bins - 1, 1);

                for (std::size_t band = 0; band < audio_playback_fft_bands_.size(); ++band)
                {
                    const float start_ratio = static_cast<float>(band) / static_cast<float>(audio_playback_fft_bands_.size());
                    const float end_ratio = static_cast<float>(band + 1) / static_cast<float>(audio_playback_fft_bands_.size());

                    int start_bin = static_cast<int>(std::pow(start_ratio, kBandCurve) * static_cast<float>(positive_bins));
                    int end_bin = static_cast<int>(std::pow(end_ratio, kBandCurve) * static_cast<float>(positive_bins));
                    start_bin = std::clamp(start_bin, 0, spectrum_bins - 1);
                    end_bin = std::clamp(end_bin, start_bin + 1, spectrum_bins);

                    float sum = 0.0F;
                    int bin_count = 0;
                    for (int bin = start_bin; bin < end_bin; ++bin)
                    {
                        const float magnitude = std::abs(audio_playback_fft_spectrum_[static_cast<std::size_t>(bin)])
                                                / static_cast<float>(kAudioPlaybackFftSize);
                        sum += std::sqrt(std::max(magnitude, 0.0F));
                        ++bin_count;
                    }

                    const float normalized = bin_count > 0
                                                 ? std::clamp((sum / static_cast<float>(bin_count)) * 1.8F, 0.0F, 1.0F)
                                                 : 0.0F;
                    audio_playback_fft_bands_[band] = audio_playback_fft_bands_[band] * 0.82F + normalized * 0.18F;
                }
            }

            // Slide the ring buffer by half.
            audio_playback_fft_sample_count_ = audio_playback_fft_sample_buffer_.size() / 2;
            std::move(audio_playback_fft_sample_buffer_.begin()
                          + static_cast<std::ptrdiff_t>(audio_playback_fft_sample_buffer_.size() / 2),
                      audio_playback_fft_sample_buffer_.end(),
                      audio_playback_fft_sample_buffer_.begin());
        }
    }

    const double rms = frame_count > 0 ? std::sqrt(sum_squares / static_cast<double>(frame_count)) : 0.0;
    audio_playback_analysis_rms_ = audio_playback_analysis_rms_ * 0.85F + static_cast<float>(rms) * 0.15F;
    audio_playback_analysis_peak_ = std::max(chunk_peak, audio_playback_analysis_peak_ * 0.92F);

    // Downsample the FFT ring buffer into a 64-sample waveform, matching
    // the approach used by AudioAnalysisWindow::refresh_waveform_samples().
    for (std::size_t i = 0; i < audio_playback_waveform_.size(); ++i)
    {
        const float ratio = audio_playback_waveform_.size() > 1
                                ? static_cast<float>(i) / static_cast<float>(audio_playback_waveform_.size() - 1)
                                : 0.0F;
        const auto source_index = static_cast<std::size_t>(ratio * static_cast<float>(audio_playback_fft_sample_buffer_.size() - 1));
        audio_playback_waveform_[i] = std::clamp(audio_playback_fft_sample_buffer_[source_index], -1.0F, 1.0F);
    }
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