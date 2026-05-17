#include "cockscreen/runtime/ShaderVideoWindow.hpp"

#include "cockscreen/runtime/StatusOverlay.hpp"
#include "cockscreen/runtime/audioanalysis/Support.hpp"
#include "cockscreen/runtime/shadervideo/Support.hpp"
#include "cockscreen/runtime/v4l2/Support.hpp"

#include <QApplication>
#include <QAudioBuffer>
#include <QAudioFormat>
#include <QColor>
#include <QCursor>
#include <QEvent>
#include <QOpenGLShader>
#include <QResizeEvent>
#include <QTouchEvent>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <linux/videodev2.h>

#include <filesystem>
#include <iostream>
#include <string_view>
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

bool is_network_url(std::string_view value)
{
    return value.rfind("rtsp://", 0) == 0 || value.rfind("rtsps://", 0) == 0 ||
           value.rfind("http://", 0) == 0 || value.rfind("https://", 0) == 0;
}

} // namespace

namespace helper = shader_window;

ShaderVideoWindow::ShaderVideoWindow(const ApplicationSettings &settings, SceneDefinition scene,
                                     QCameraDevice video_device, QString video_device_path, QString video_label,
                                     QString format_label, bool show_status_overlay, QWidget *parent)
    : QOpenGLWidget{parent}, settings_{settings}, scene_{std::move(scene)}, video_label_{std::move(video_label)},
      show_status_overlay_{show_status_overlay}, camera_format_label_{std::move(format_label)}
{
    resize(settings_.width, settings_.height);
    setMinimumSize(900, 540);
    setAutoFillBackground(false);
    setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);
    setMouseTracking(true);
    setAttribute(Qt::WA_AcceptTouchEvents, true);
#if defined(__linux__) && defined(__aarch64__)
    setCursor(Qt::BlankCursor);
#else
    unsetCursor();
#endif

    if (show_status_overlay_)
    {
        status_overlay_ = new StatusOverlay{this};
        place_status_overlay(this, status_overlay_);
        status_overlay_->raise();
    }

    capture_session_.setVideoSink(&video_sink_);
    QObject::connect(&video_sink_, &QVideoSink::videoFrameChanged, this,
                     [this](const QVideoFrame &frame) { handle_frame(frame); });

    // Initialize independent named video feeds. The primary "video" layer is
    // still driven by the constructor arguments because Application owns device
    // discovery, raw V4L2 fallback, and primary network loopback setup.
    for (const auto &[layer_name, named_layer] : scene_.named_layers)
    {
        if (layer_name == "video" || named_layer.type != SceneLayerType::Video ||
            !named_layer.input.enabled || !is_network_url(named_layer.input.device))
        {
            continue;
        }

        auto vs = std::make_unique<VideoLayerState>();
        vs->sink = new QVideoSink();

        const std::string ln = layer_name;
        QObject::connect(vs->sink, &QVideoSink::videoFrameChanged, this,
                         [this, ln](const QVideoFrame &frame) { handle_video_layer_frame(ln, frame); });

#ifndef _WIN32
        vs->appsink_capture = new AppsinkCapture;
        if (!vs->appsink_capture->start_network(named_layer.input.device, vs->sink, settings_.verbose_debug))
        {
            const QString error_text =
                QStringLiteral("Network video layer '%1' failed: %2")
                    .arg(QString::fromStdString(layer_name), vs->appsink_capture->status_message());
            status_message_ = error_text;
            std::cerr << error_text.toStdString() << '\n';
            delete vs->appsink_capture;
            vs->appsink_capture = nullptr;
        }
        else
        {
            std::cout << "[appsink-video:" << layer_name << "] " << named_layer.input.device << '\n';
        }
#endif

        video_layer_states_[layer_name] = std::move(vs);
    }

    // Initialize one PlaybackState per Playback-type named layer.
    for (const auto &[layer_name, named_layer] : scene_.named_layers)
    {
        if (named_layer.type != SceneLayerType::Playback)
        {
            continue;
        }
        auto ps = std::make_unique<PlaybackState>();
        ps->player = new QMediaPlayer();
        ps->audio_output = new QAudioOutput();
        ps->sink = new QVideoSink();
        ps->player->setVideoSink(ps->sink);
        ps->player->setAudioOutput(ps->audio_output);

        const std::string ln = layer_name;
        QObject::connect(ps->sink, &QVideoSink::videoFrameChanged, this,
                         [this, ln](const QVideoFrame &frame) { handle_playback_frame(ln, frame); });
        QObject::connect(ps->player, &QMediaPlayer::positionChanged, this,
                         [this, ln](qint64 position) {
                             handle_playback_position_changed(ln, static_cast<std::int64_t>(position));
                         });
        QObject::connect(ps->player, &QMediaPlayer::durationChanged, this,
                         [this, ln](qint64 duration) {
                             auto it = playback_states_.find(ln);
                             if (it != playback_states_.end() && it->second)
                             {
                                 it->second->duration_ms = std::max<std::int64_t>(0, static_cast<std::int64_t>(duration));
                             }
                         });
        QObject::connect(ps->player, &QMediaPlayer::mediaStatusChanged, this,
                         [this, ln](QMediaPlayer::MediaStatus status) {
                             auto it = playback_states_.find(ln);
                             if (it == playback_states_.end() || !it->second)
                             {
                                 return;
                             }
                             it->second->status_text = playback_media_status_text(status);
                             if (!it->second->transport_pending_seek)
                             {
                                 return;
                             }
                             if (status == QMediaPlayer::LoadedMedia || status == QMediaPlayer::BufferedMedia ||
                                 status == QMediaPlayer::BufferingMedia)
                             {
                                 it->second->transport_pending_seek = false;
                                 configure_playback_transport(ln, true, true);
                             }
                         });
        QObject::connect(ps->player, &QMediaPlayer::errorOccurred, this,
                         [this, ln](QMediaPlayer::Error error, const QString &error_string) {
                             auto it = playback_states_.find(ln);
                             if (it == playback_states_.end() || !it->second)
                             {
                                 return;
                             }
                             if (error == QMediaPlayer::NoError)
                             {
                                 it->second->error_text.clear();
                                 return;
                             }
                             it->second->error_text =
                                 error_string.trimmed().isEmpty()
                                     ? QStringLiteral("Qt playback error %1").arg(static_cast<int>(error))
                                     : error_string.trimmed();
                         });

        playback_states_[layer_name] = std::move(ps);
    }
    render_tick_timer_.setTimerType(Qt::PreciseTimer);
    QObject::connect(&render_tick_timer_, &QTimer::timeout, this, [this]() { update(); });
    render_tick_timer_.start(16);

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

#ifndef _WIN32
    // Raw V4L2 capture path for Media Controller devices (e.g. CSI cameras on Pi)
    // that Qt's multimedia backend cannot enumerate.
    const bool try_raw_capture = !use_camera_capture &&
                                 !video_device_path.isEmpty() &&
                                 !video_device_path.startsWith(QLatin1Char('@')) &&
                                 !video_label_.startsWith(QStringLiteral("appsink:"));
    if (try_raw_capture)
    {
        const std::string dev_path = video_device_path.toStdString();
        if (raw_video_capture_.open(dev_path, requested_width, requested_height))
        {
            if (raw_video_capture_.start())
            {
                raw_video_capture_active_ = true;
                raw_video_capture_start_time_ = std::chrono::steady_clock::now();
                camera_format_label_ = QString::fromStdString(raw_video_capture_.format_label());
                std::cout << "[raw-capture] opened " << dev_path
                          << " as " << raw_video_capture_.format_label() << '\n';

                // Attempt to route frames through the bcm2835 hardware ISP so
                // that Bayer debayering is done in silicon rather than on the CPU.
                std::string isp_input_dev;
                std::string isp_output_dev;
                if (v4l2::IspPipeline::find_devices(&isp_input_dev, &isp_output_dev))
                {
                    std::string isp_error;
                    const std::uint32_t capture_fourcc =
                        v4l2::pixel_format_to_fourcc(raw_video_capture_.pixel_format());
                    if (capture_fourcc != 0 &&
                        isp_pipeline_.open(isp_input_dev, isp_output_dev,
                                           raw_video_capture_.width(),
                                           raw_video_capture_.height(),
                                           capture_fourcc,
                                           V4L2_PIX_FMT_YUYV,
                                           &isp_error) &&
                        isp_pipeline_.start())
                    {
                        use_isp_ = true;
                        camera_format_label_ = QString::fromStdString(raw_video_capture_.format_label())
                                               + QStringLiteral(" (ISP→YUYV)");
                        std::cout << "[isp-pipeline] active: "
                                  << isp_input_dev << " → " << isp_output_dev << '\n';
                    }
                    else
                    {
                        std::cerr << "[isp-pipeline] unavailable: "
                                  << (isp_error.empty() ? isp_pipeline_.error_message() : isp_error)
                                  << " — using CPU debayer\n";
                        isp_pipeline_.close();
                    }
                }
            }
            else
            {
                std::cerr << "[raw-capture] start failed: " << raw_video_capture_.error_message() << '\n';
            }
        }
        else
        {
            std::cerr << "[raw-capture] open failed: " << raw_video_capture_.error_message() << '\n';
        }
    }
#endif

#ifndef _WIN32
    // --- Loopback setup for each Playback-type layer that has loopback enabled.
    for (auto &[layer_name, ps] : playback_states_)
    {
        if (!ps)
        {
            continue;
        }
        const auto nl_it = scene_.named_layers.find(layer_name);
        if (nl_it == scene_.named_layers.end())
        {
            continue;
        }
        const SceneInput &input = nl_it->second.input;
        if (!input.loopback.enabled || input.file.empty())
        {
            restart_playback_source(layer_name, true);
            continue;
        }

        if (input.loopback.use_appsink)
        {
            ps->appsink_capture = new AppsinkCapture;
            const bool use_h264 = LoopbackPipeline::uses_h264();
            if (!ps->appsink_capture->start(input.loopback.udp_port, ps->sink, use_h264))
            {
                const QString error_text = QStringLiteral("Playback appsink capture failed: %1")
                                               .arg(ps->appsink_capture->status_message());
                fatal_render_error_ = error_text;
                status_message_ = error_text;
                delete ps->appsink_capture;
                ps->appsink_capture = nullptr;
            }
            else
            {
                std::cerr << "[appsink-playback:" << layer_name << "] "
                          << ps->appsink_capture->status_message().toStdString() << "\n";
            }

            const std::int64_t loop_end_ms = input.loop_end_ms.has_value() ? *input.loop_end_ms : -1;
            const bool started = ps->loopback.start_sender_only_for_file(
                input.file, input.loopback, input.start_ms, input.loop_start_ms, loop_end_ms);
            if (!started)
            {
                fatal_render_error_ =
                    QStringLiteral("Playback loopback sender failed to start: ") + ps->loopback.status_message();
                status_message_ = fatal_render_error_;
                if (ps->appsink_capture != nullptr)
                {
                    ps->appsink_capture->stop();
                    delete ps->appsink_capture;
                    ps->appsink_capture = nullptr;
                }
            }
        }
        else
        {
            QString prereq_error;
            if (!LoopbackPipeline::check_prerequisites(input.loopback, &prereq_error))
            {
                fatal_render_error_ = prereq_error;
                status_message_ = prereq_error;
            }
            else
            {
                const bool started = ps->loopback.start_for_file(input.file, input.loopback);
                if (started)
                {
                    if (!ps->loopback.wait_for_device_ready(8000))
                    {
                        fatal_render_error_ = QStringLiteral("Playback loopback device not ready: ") +
                                              ps->loopback.status_message();
                        status_message_ = fatal_render_error_;
                        ps->loopback.stop();
                    }
                    else
                    {
                        ps->loopback_capture = new LoopbackCapture;
                        if (!ps->loopback_capture->start(input.loopback.loopback_device,
                                                         ps->sink, settings_.verbose_debug))
                        {
                            const QString error_text =
                                QStringLiteral("Playback loopback capture failed: %1")
                                    .arg(ps->loopback_capture->status_message());
                            fatal_render_error_ = error_text;
                            status_message_ = error_text;
                            delete ps->loopback_capture;
                            ps->loopback_capture = nullptr;
                            ps->loopback.stop();
                        }
                    }
                }
                else
                {
                    fatal_render_error_ =
                        QStringLiteral("Playback loopback failed to start: ") + ps->loopback.status_message();
                    status_message_ = fatal_render_error_;
                }
            }
        }
    }
#else
    for (auto &[layer_name, ps] : playback_states_)
    {
        restart_playback_source(layer_name, true);
    }
#endif

    audio_playback_player_.setAudioOutput(&audio_playback_audio_output_);
    audio_playback_player_.setAudioBufferOutput(&audio_playback_buffer_output_);
    QObject::connect(&audio_playback_buffer_output_, &QAudioBufferOutput::audioBufferReceived, this,
                     [this](const QAudioBuffer &buffer) { process_audio_playback_buffer(buffer); });
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
                         audio_playback_error_text_ =
                             error_string.trimmed().isEmpty()
                                 ? QStringLiteral("Audio playback error %1").arg(static_cast<int>(error))
                                 : error_string.trimmed();
                     });
    restart_audio_playback_source(true);

    // Volume-fade timer: fires every 50 ms, calls tick_audio_playback_volume().
    audio_playback_volume_timer_.setInterval(50);
    QObject::connect(&audio_playback_volume_timer_, &QTimer::timeout, this, [this]() { tick_audio_playback_volume(); });
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
    else if (status_message_.isEmpty() && !video_label_.startsWith(QStringLiteral("appsink:")))
    {
        // Suppress the "no device" message if at least one Playback-type layer has a file.
        const bool any_playback_active = std::any_of(
            scene_.named_layers.begin(), scene_.named_layers.end(),
            [](const auto &kv) {
                return kv.second.type == SceneLayerType::Playback &&
                       kv.second.input.enabled &&
                       !kv.second.input.file.empty();
            });
        const bool any_independent_video_active = std::any_of(
            video_layer_states_.begin(), video_layer_states_.end(),
            [](const auto &kv) { return kv.second != nullptr; });
        if (!any_playback_active && !any_independent_video_active)
        {
            status_message_ = QStringLiteral("No video capture device was found");
        }
    }
}

ShaderVideoWindow::PlaybackState::~PlaybackState()
{
#ifndef _WIN32
    if (loopback_capture != nullptr)
    {
        loopback_capture->stop();
        delete loopback_capture;
        loopback_capture = nullptr;
    }
    if (appsink_capture != nullptr)
    {
        appsink_capture->stop();
        delete appsink_capture;
        appsink_capture = nullptr;
    }
    loopback.stop();
#endif
    // player, audio_output, sink do not have a Qt parent so delete directly.
    delete player;
    delete audio_output;
    delete sink;
}

ShaderVideoWindow::VideoLayerState::~VideoLayerState()
{
#ifndef _WIN32
    if (appsink_capture != nullptr)
    {
        appsink_capture->stop();
        delete appsink_capture;
        appsink_capture = nullptr;
    }
#endif
    delete sink;
}

ShaderVideoWindow::PlaybackState *ShaderVideoWindow::primary_playback_state() noexcept
{
    auto it = playback_states_.find("playback");
    if (it != playback_states_.end())
    {
        return it->second.get();
    }
    for (auto &[name, state] : playback_states_)
    {
        return state.get();
    }
    return nullptr;
}

const ShaderVideoWindow::PlaybackState *ShaderVideoWindow::primary_playback_state() const noexcept
{
    auto it = playback_states_.find("playback");
    if (it != playback_states_.end())
    {
        return it->second.get();
    }
    for (const auto &[name, state] : playback_states_)
    {
        return state.get();
    }
    return nullptr;
}

ShaderVideoWindow::~ShaderVideoWindow()
{
#ifndef _WIN32
    // Stop all playback loopback threads / GStreamer subprocesses before GL teardown.
    for (auto &[name, ps] : playback_states_)
    {
        if (ps)
        {
#ifndef _WIN32
            if (ps->loopback_capture != nullptr)
            {
                ps->loopback_capture->stop();
                delete ps->loopback_capture;
                ps->loopback_capture = nullptr;
            }
            if (ps->appsink_capture != nullptr)
            {
                ps->appsink_capture->stop();
                delete ps->appsink_capture;
                ps->appsink_capture = nullptr;
            }
            ps->loopback.stop();
#endif
        }
    }
#endif

    if (context() == nullptr)
    {
        for (auto &[name, fbo] : layer_fbos_)
        {
            delete fbo;
        }
        layer_fbos_.clear();
        for (auto &[name, fbo] : layer_fbos_alt_)
        {
            delete fbo;
        }
        layer_fbos_alt_.clear();
        delete composite_scene_fbo_;
        composite_scene_fbo_ = nullptr;
        return;
    }

    makeCurrent();
    for (auto &[name, fbo] : layer_fbos_)
    {
        delete fbo;
    }
    layer_fbos_.clear();
    for (auto &[name, fbo] : layer_fbos_alt_)
    {
        delete fbo;
    }
    layer_fbos_alt_.clear();
    delete composite_scene_fbo_;
    composite_scene_fbo_ = nullptr;
    for (auto &[name, ps] : playback_states_)
    {
        if (ps && ps->texture_id != 0)
        {
            glDeleteTextures(1, &ps->texture_id);
            ps->texture_id = 0;
        }
    }
    for (auto &[name, vs] : video_layer_states_)
    {
        if (vs && vs->texture_id != 0)
        {
            glDeleteTextures(1, &vs->texture_id);
            vs->texture_id = 0;
        }
    }
    if (texture_id_ != 0)
    {
        glDeleteTextures(1, &texture_id_);
        texture_id_ = 0;
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
    for (auto &stage : render_stages_)
    {
        for (GLuint &tex : stage.channel_textures)
        {
            if (tex != 0)
            {
                glDeleteTextures(1, &tex);
                tex = 0;
            }
        }
        for (auto &buf : stage.shader_buffers)
        {
            for (int i = 0; i < 2; ++i)
            {
                delete buf.fbo[i];
                buf.fbo[i] = nullptr;
            }
        }
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
    const auto *ps = primary_playback_state();
    return ps ? ps->position_ms : 0;
}

std::int64_t ShaderVideoWindow::playback_duration_ms() const
{
    const auto *ps = primary_playback_state();
    return ps ? ps->duration_ms : 0;
}

int ShaderVideoWindow::playback_loops_completed() const
{
    const auto *ps = primary_playback_state();
    return ps ? ps->loops_completed : 0;
}

double ShaderVideoWindow::playback_current_rate() const
{
    const auto *ps = primary_playback_state();
    return (ps && ps->player) ? ps->player->playbackRate() : 1.0;
}

QString ShaderVideoWindow::playback_error_text() const
{
    const auto *ps = primary_playback_state();
    return ps ? ps->error_text : QString{};
}

QString ShaderVideoWindow::playback_status_text() const
{
    const auto *ps = primary_playback_state();
    return ps ? ps->status_text : QString{};
}

std::optional<std::uintmax_t> ShaderVideoWindow::playback_file_size_bytes() const
{
    const auto *ps = primary_playback_state();
    return ps ? ps->file_size_bytes : std::nullopt;
}

QImage ShaderVideoWindow::latest_video_frame_image() const
{
    return latest_frame_.copy();
}

void ShaderVideoWindow::apply_scene_update(SceneDefinition scene)
{
    // Detect per-named-layer playback changes.
    struct PlaybackChanges { bool source{false}; bool transport{false}; bool start{false}; };
    std::map<std::string, PlaybackChanges> playback_changes;
    for (const auto &[name, new_nl] : scene.named_layers)
    {
        if (new_nl.type != SceneLayerType::Playback)
        {
            continue;
        }
        const auto old_it = scene_.named_layers.find(name);
        if (old_it == scene_.named_layers.end())
        {
            playback_changes[name].source = true;
            continue;
        }
        const SceneInput &old_in = old_it->second.input;
        const SceneInput &new_in = new_nl.input;
        PlaybackChanges ch;
        ch.source = old_in.enabled != new_in.enabled || old_in.file != new_in.file;
        ch.start = old_in.start_ms != new_in.start_ms;
        ch.transport = ch.start || old_in.loop_start_ms != new_in.loop_start_ms ||
                       old_in.loop_end_ms != new_in.loop_end_ms ||
                       old_in.loop_repeat != new_in.loop_repeat ||
                       old_in.playback_rate != new_in.playback_rate ||
                       old_in.playback_rate_looping != new_in.playback_rate_looping;
        playback_changes[name] = ch;
    }

    // Backward-compat: also check the old playback_input field for the "playback" layer.
    {
        const bool playback_source_changed = scene_.playback_input.enabled != scene.playback_input.enabled ||
                                             scene_.playback_input.file != scene.playback_input.file;
        const bool playback_start_changed = scene_.playback_input.start_ms != scene.playback_input.start_ms;
        const bool playback_transport_changed =
            playback_start_changed ||
            scene_.playback_input.loop_start_ms != scene.playback_input.loop_start_ms ||
            scene_.playback_input.loop_end_ms != scene.playback_input.loop_end_ms ||
            scene_.playback_input.loop_repeat != scene.playback_input.loop_repeat ||
            scene_.playback_input.playback_rate != scene.playback_input.playback_rate ||
            scene_.playback_input.playback_rate_looping != scene.playback_input.playback_rate_looping;
        if (playback_changes.find("playback") == playback_changes.end())
        {
            playback_changes["playback"] = {playback_source_changed, playback_transport_changed, playback_start_changed};
        }
    }

    const bool audio_source_changed = scene_.audio_playback_input.enabled != scene.audio_playback_input.enabled ||
                                      scene_.audio_playback_input.file != scene.audio_playback_input.file;
    const bool audio_transport_changed =
        scene_.audio_playback_input.start_ms != scene.audio_playback_input.start_ms ||
        scene_.audio_playback_input.loop_start_ms != scene.audio_playback_input.loop_start_ms ||
        scene_.audio_playback_input.loop_end_ms != scene.audio_playback_input.loop_end_ms ||
        scene_.audio_playback_input.loop_repeat != scene.audio_playback_input.loop_repeat ||
        scene_.audio_playback_input.playback_rate != scene.audio_playback_input.playback_rate ||
        scene_.audio_playback_input.playback_rate_looping != scene.audio_playback_input.playback_rate_looping;
    const bool audio_start_changed = scene_.audio_playback_input.start_ms != scene.audio_playback_input.start_ms;

    scene_ = std::move(scene);
    show_status_overlay_ = scene_.show_status_overlay;
    status_message_.clear();
    fatal_render_error_.clear();

    if (fatal_error_overlay_ != nullptr)
    {
        fatal_error_overlay_->hide();
    }

    if (show_status_overlay_)
    {
        if (status_overlay_ == nullptr)
        {
            status_overlay_ = new StatusOverlay{this};
            place_status_overlay(this, status_overlay_);
        }
        status_overlay_->show();
        status_overlay_->raise();
    }
    else if (status_overlay_ != nullptr)
    {
        status_overlay_->hide();
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

    for (const auto &[layer_name, ch] : playback_changes)
    {
        if (playback_states_.find(layer_name) == playback_states_.end())
        {
            continue; // layer not active (no player was created for it)
        }
        if (ch.source)
        {
            restart_playback_source(layer_name, true);
        }
        else if (ch.transport)
        {
            configure_playback_transport(layer_name, ch.start, true);
        }
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

void ShaderVideoWindow::update_pointer_state(const QPointF &position, bool pressed, bool new_press)
{
    const float clamped_x =
        std::clamp(static_cast<float>(position.x()), 0.0F, static_cast<float>(std::max(width() - 1, 0)));
    const float clamped_y =
        std::clamp(static_cast<float>(position.y()), 0.0F, static_cast<float>(std::max(height() - 1, 0)));

    pointer_position_ = QVector2D{clamped_x, clamped_y};
    if (new_press)
    {
        pointer_press_origin_ = pointer_position_;
    }

    pointer_has_position_ = true;
    pointer_pressed_ = pressed;
    update();
}

QVector4D ShaderVideoWindow::shadertoy_mouse_uniform(const QSize &render_size)
{
    sync_pointer_state_from_cursor();

    if (!pointer_has_position_)
    {
        return QVector4D{};
    }

    const float scale_x =
        static_cast<float>(std::max(render_size.width(), 1)) / static_cast<float>(std::max(width(), 1));
    const float scale_y =
        static_cast<float>(std::max(render_size.height(), 1)) / static_cast<float>(std::max(height(), 1));
    const float current_x = pointer_position_.x() * scale_x;
    const float current_y = static_cast<float>(render_size.height()) - pointer_position_.y() * scale_y;
    if (!pointer_pressed_)
    {
        return QVector4D{current_x, current_y, 0.0F, 0.0F};
    }

    return QVector4D{current_x, current_y, pointer_press_origin_.x() * scale_x,
                     static_cast<float>(render_size.height()) - pointer_press_origin_.y() * scale_y};
}

void ShaderVideoWindow::sync_pointer_state_from_cursor()
{
    const QPoint local_pos = mapFromGlobal(QCursor::pos());
    const bool inside_widget = rect().contains(local_pos);
    const bool left_button_down = (QApplication::mouseButtons() & Qt::LeftButton) != 0;

    if (!inside_widget && !left_button_down)
    {
        return;
    }

    const QVector2D previous_position = pointer_position_;
    const bool was_pressed = pointer_pressed_;
    update_pointer_state(local_pos, left_button_down, left_button_down && !was_pressed);

    if (!left_button_down && inside_widget)
    {
        pointer_pressed_ = false;
        if (pointer_position_ != previous_position || was_pressed)
        {
            update();
        }
    }
}

void ShaderVideoWindow::mousePressEvent(QMouseEvent *event)
{
    if (event != nullptr && event->button() == Qt::LeftButton)
    {
        update_pointer_state(event->position(), true, true);
        event->accept();
        return;
    }

    QOpenGLWidget::mousePressEvent(event);
}

void ShaderVideoWindow::mouseMoveEvent(QMouseEvent *event)
{
    if (event != nullptr)
    {
        update_pointer_state(event->position(), pointer_pressed_, false);
        event->accept();
        return;
    }

    QOpenGLWidget::mouseMoveEvent(event);
}

void ShaderVideoWindow::mouseReleaseEvent(QMouseEvent *event)
{
    if (event != nullptr && event->button() == Qt::LeftButton)
    {
        update_pointer_state(event->position(), false, false);
        event->accept();
        return;
    }

    QOpenGLWidget::mouseReleaseEvent(event);
}

bool ShaderVideoWindow::event(QEvent *event)
{
    if (event == nullptr)
    {
        return QOpenGLWidget::event(event);
    }

    switch (event->type())
    {
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate: {
        auto *touch_event = static_cast<QTouchEvent *>(event);
        if (!touch_event->points().isEmpty())
        {
            const auto &point = touch_event->points().front();
            update_pointer_state(point.position(), true, point.state() == QEventPoint::State::Pressed);
            event->accept();
            return true;
        }
        break;
    }
    case QEvent::TouchEnd: {
        auto *touch_event = static_cast<QTouchEvent *>(event);
        if (!touch_event->points().isEmpty())
        {
            const auto &point = touch_event->points().front();
            update_pointer_state(point.position(), false, false);
        }
        else
        {
            pointer_pressed_ = false;
            update();
        }
        event->accept();
        return true;
    }
    case QEvent::TouchCancel:
        pointer_pressed_ = false;
        update();
        event->accept();
        return true;
    default:
        break;
    }

    return QOpenGLWidget::event(event);
}

void ShaderVideoWindow::stop_playback_source(const std::string &layer_name)
{
    auto it = playback_states_.find(layer_name);
    if (it == playback_states_.end() || !it->second)
    {
        return;
    }
    PlaybackState &ps = *it->second;
    if (ps.player)
    {
        ps.player->stop();
        ps.player->setSource(QUrl{});
    }
    ps.position_ms = 0;
    ps.duration_ms = 0;
    ps.loops_completed = 0;
    ps.transport_pending_seek = false;
    ps.error_text.clear();
    ps.status_text = QStringLiteral("idle");
    ps.file_size_bytes.reset();
    ps.latest_frame = QImage{};
    ps.texture_dirty = false;
}

void ShaderVideoWindow::restart_playback_source(const std::string &layer_name, bool seek_to_start)
{
    auto it = playback_states_.find(layer_name);
    if (it == playback_states_.end() || !it->second)
    {
        return;
    }
    PlaybackState &ps = *it->second;
    ps.latest_frame = QImage{};
    ps.texture_dirty = false;
    ps.position_ms = 0;
    ps.duration_ms = 0;
    ps.loops_completed = 0;
    ps.error_text.clear();
    ps.status_text = QStringLiteral("idle");
    ps.file_size_bytes.reset();

    const auto nl_it = scene_.named_layers.find(layer_name);
    if (nl_it == scene_.named_layers.end())
    {
        stop_playback_source(layer_name);
        return;
    }
    const SceneInput &input = nl_it->second.input;
    if (!input.enabled || input.file.empty())
    {
        stop_playback_source(layer_name);
        return;
    }

    // Network URL (RTSP / HTTP MJPEG): bypass filesystem resolution and use QUrl directly.
    // Qt's GStreamer backend handles rtsp://, http://, https:// natively.
    // Seeking and looping are not attempted on network streams.
    const auto &file_str = input.file;
    const bool is_network = file_str.rfind("rtsp://", 0) == 0 ||
                            file_str.rfind("rtsps://", 0) == 0 ||
                            file_str.rfind("http://", 0) == 0 ||
                            file_str.rfind("https://", 0) == 0;
    if (is_network)
    {
        ps.status_text = QStringLiteral("loading");
        if (ps.player)
        {
            ps.player->stop();
            ps.player->setSource(QUrl{QString::fromStdString(file_str)});
            ps.player->play();
        }
        return;
    }

    const auto playback_path = helper::resolve_scene_resource_path(scene_.resources_directory, input.file);
    if (!playback_path.has_value())
    {
        stop_playback_source(layer_name);
        status_message_ = QStringLiteral("Playback file not found");
        return;
    }

    std::error_code file_error;
    const auto file_size = std::filesystem::file_size(*playback_path, file_error);
    if (!file_error)
    {
        ps.file_size_bytes = file_size;
    }

    ps.transport_pending_seek = seek_to_start;
    ps.status_text = QStringLiteral("loading");
    if (ps.player)
    {
        ps.player->stop();
        ps.player->setSource(QUrl::fromLocalFile(QString::fromStdString(playback_path->string())));
        ps.player->play();
    }
    configure_playback_transport(layer_name, seek_to_start, true);
}

void ShaderVideoWindow::configure_playback_transport(const std::string &layer_name, bool seek_to_start,
                                                     bool reset_loop_count)
{
    auto it = playback_states_.find(layer_name);
    if (it == playback_states_.end() || !it->second || !it->second->player)
    {
        return;
    }
    PlaybackState &ps = *it->second;
    if (ps.player->source().isEmpty())
    {
        return;
    }
    const auto nl_it = scene_.named_layers.find(layer_name);
    if (nl_it == scene_.named_layers.end())
    {
        return;
    }
    const SceneInput &input = nl_it->second.input;

    if (reset_loop_count)
    {
        ps.loops_completed = 0;
    }
    if (seek_to_start)
    {
        ps.position_ms = std::max<std::int64_t>(0, input.start_ms);
        ps.player->setPosition(ps.position_ms);
    }
    apply_playback_rate_for_position(layer_name, ps.position_ms);
}

bool ShaderVideoWindow::playback_loop_enabled(const std::string &layer_name) const
{
    return playback_effective_loop_end_ms(layer_name).has_value();
}

std::optional<std::int64_t> ShaderVideoWindow::playback_effective_loop_end_ms(const std::string &layer_name) const
{
    const auto ps_it = playback_states_.find(layer_name);
    if (ps_it == playback_states_.end() || !ps_it->second)
    {
        return std::nullopt;
    }
    const PlaybackState &ps = *ps_it->second;
    const auto nl_it = scene_.named_layers.find(layer_name);
    if (nl_it == scene_.named_layers.end())
    {
        return std::nullopt;
    }
    const SceneInput &input = nl_it->second.input;
    const auto loop_start_ms = std::max<std::int64_t>(0, input.loop_start_ms);
    if (input.loop_end_ms.has_value())
    {
        return *input.loop_end_ms > loop_start_ms
                   ? std::optional<std::int64_t>{*input.loop_end_ms}
                   : std::nullopt;
    }
    if (ps.duration_ms > loop_start_ms)
    {
        return ps.duration_ms;
    }
    return std::nullopt;
}

void ShaderVideoWindow::apply_playback_rate_for_position(const std::string &layer_name, std::int64_t position_ms)
{
    auto ps_it = playback_states_.find(layer_name);
    if (ps_it == playback_states_.end() || !ps_it->second || !ps_it->second->player)
    {
        return;
    }
    PlaybackState &ps = *ps_it->second;
    if (ps.player->source().isEmpty())
    {
        return;
    }
    const auto nl_it = scene_.named_layers.find(layer_name);
    if (nl_it == scene_.named_layers.end())
    {
        return;
    }
    const SceneInput &input = nl_it->second.input;
    const float base_rate = sanitized_playback_rate(input.playback_rate);
    float target_rate = base_rate;
    if (const auto loop_end_ms = playback_effective_loop_end_ms(layer_name); loop_end_ms.has_value())
    {
        const auto loop_start_ms = std::max<std::int64_t>(0, input.loop_start_ms);
        const bool loop_has_budget =
            input.loop_repeat == 0 || ps.loops_completed < input.loop_repeat;
        if (loop_has_budget && position_ms >= loop_start_ms && position_ms < *loop_end_ms)
        {
            target_rate = sanitized_playback_rate(input.playback_rate_looping, base_rate);
        }
    }
    if (std::fabs(ps.player->playbackRate() - target_rate) > 0.0001F)
    {
        ps.player->setPlaybackRate(target_rate);
    }
}


void ShaderVideoWindow::handle_playback_position_changed(const std::string &layer_name, std::int64_t position_ms)
{
    auto it = playback_states_.find(layer_name);
    if (it == playback_states_.end() || !it->second)
    {
        return;
    }
    PlaybackState &ps = *it->second;
    ps.position_ms = std::max<std::int64_t>(0, position_ms);

    if (const auto loop_end_ms = playback_effective_loop_end_ms(layer_name); loop_end_ms.has_value())
    {
        const auto nl_it = scene_.named_layers.find(layer_name);
        if (nl_it != scene_.named_layers.end())
        {
            const SceneInput &input = nl_it->second.input;
            const auto loop_start_ms = std::max<std::int64_t>(0, input.loop_start_ms);
            const bool loop_has_budget = input.loop_repeat == 0 || ps.loops_completed < input.loop_repeat;
            if (loop_has_budget && ps.position_ms >= *loop_end_ms)
            {
                ++ps.loops_completed;
                ps.position_ms = loop_start_ms;
                ps.player->setPosition(loop_start_ms);
                apply_playback_rate_for_position(layer_name, loop_start_ms);
                return;
            }
        }
    }
    apply_playback_rate_for_position(layer_name, ps.position_ms);
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

    const auto audio_path =
        helper::resolve_scene_resource_path(scene_.resources_directory, scene_.audio_playback_input.file);
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

    const float peak = std::clamp(cfg.volume, 0.0F, 1.0F);
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
        const std::int64_t loop_end = *loop_end_opt;
        const std::int64_t loop_len = loop_end - loop_start;

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
            case QAudioFormat::Int16: {
                qint16 s = 0;
                std::memcpy(&s, bytes + offset, sizeof(s));
                value = audio_analysis::sample_to_float(s);
                break;
            }
            case QAudioFormat::Int32: {
                qint32 s = 0;
                std::memcpy(&s, bytes + offset, sizeof(s));
                value = audio_analysis::sample_to_float(s);
                break;
            }
            case QAudioFormat::Float: {
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
                    const float start_ratio =
                        static_cast<float>(band) / static_cast<float>(audio_playback_fft_bands_.size());
                    const float end_ratio =
                        static_cast<float>(band + 1) / static_cast<float>(audio_playback_fft_bands_.size());

                    int start_bin =
                        static_cast<int>(std::pow(start_ratio, kBandCurve) * static_cast<float>(positive_bins));
                    int end_bin = static_cast<int>(std::pow(end_ratio, kBandCurve) * static_cast<float>(positive_bins));
                    start_bin = std::clamp(start_bin, 0, spectrum_bins - 1);
                    end_bin = std::clamp(end_bin, start_bin + 1, spectrum_bins);

                    float sum = 0.0F;
                    int bin_count = 0;
                    for (int bin = start_bin; bin < end_bin; ++bin)
                    {
                        const float magnitude = std::abs(audio_playback_fft_spectrum_[static_cast<std::size_t>(bin)]) /
                                                static_cast<float>(kAudioPlaybackFftSize);
                        sum += std::sqrt(std::max(magnitude, 0.0F));
                        ++bin_count;
                    }

                    const float normalized =
                        bin_count > 0 ? std::clamp((sum / static_cast<float>(bin_count)) * 1.8F, 0.0F, 1.0F) : 0.0F;
                    audio_playback_fft_bands_[band] = audio_playback_fft_bands_[band] * 0.82F + normalized * 0.18F;
                }
            }

            // Slide the ring buffer by half.
            audio_playback_fft_sample_count_ = audio_playback_fft_sample_buffer_.size() / 2;
            std::move(audio_playback_fft_sample_buffer_.begin() +
                          static_cast<std::ptrdiff_t>(audio_playback_fft_sample_buffer_.size() / 2),
                      audio_playback_fft_sample_buffer_.end(), audio_playback_fft_sample_buffer_.begin());
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
        const auto source_index =
            static_cast<std::size_t>(ratio * static_cast<float>(audio_playback_fft_sample_buffer_.size() - 1));
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
        qCritical().noquote() << "Fatal render error:" << text;
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
        !blit_program_.addShaderFromSourceCode(QOpenGLShader::Fragment,
                                               helper::shader_source_for_current_context(
                                                   QString::fromUtf8(helper::passthrough_fragment_shader_source()))) ||
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

QString ShaderVideoWindow::load_fragment_shader_source(std::string_view shader_file, bool allow_directory_scan,
                                                       std::filesystem::path *resource_dir_out) const
{
    // Helper: find a single .glsl file inside a directory and return its source.
    // If found and resource_dir_out is non-null, writes the directory path to it.
    // Returns true for files that are support/pass files within a subdirectory,
    // not the main Image-pass shader (common.glsl, bufferA-D.glsl, image.glsl).
    auto is_subdir_support_file = [](const std::filesystem::path &p) -> bool {
        const auto name = p.filename().string();
        if (name == "common.glsl" || name == "image.glsl")
        {
            return true;
        }
        // buffer[A-D].glsl
        if (name.size() == 11 && name.substr(0, 6) == "buffer" && name.substr(7) == ".glsl")
        {
            const char c = name[6];
            if (c >= 'A' && c <= 'D')
            {
                return true;
            }
        }
        return false;
    };

    auto scan_shader_dir = [&](const std::filesystem::path &dir) -> QString {
        std::error_code ec;
        for (const auto &entry : std::filesystem::directory_iterator{dir, ec})
        {
            if (!entry.is_regular_file())
            {
                continue;
            }
            const auto ext = entry.path().extension().string();
            if (ext != ".frag" && ext != ".glsl" && ext != ".vert" && ext != ".comp")
            {
                continue;
            }
            // Skip support/pass files — they are not the Image pass.
            if (is_subdir_support_file(entry.path()))
            {
                continue;
            }
            const auto source = helper::read_text_file_qstring(entry.path());
            if (!source.isEmpty())
            {
                if (resource_dir_out != nullptr)
                {
                    *resource_dir_out = dir;
                }
                return source;
            }
        }
        return {};
    };

    if (!shader_file.empty())
    {
        std::filesystem::path shader_path{shader_file};
        if (!shader_path.is_absolute())
        {
            shader_path = std::filesystem::path{settings_.shader_directory} / shader_path;
        }

        const auto resolved_shader_path = helper::resolve_relative_path(shader_path);
        if (resolved_shader_path.has_value())
        {
            // Case 1: the path points directly to an existing file.
            if (std::filesystem::is_regular_file(*resolved_shader_path))
            {
                const auto source = helper::read_text_file_qstring(*resolved_shader_path);
                if (!source.isEmpty())
                {
                    // If the parent directory differs from the top-level shader
                    // directory it is a per-shader resource directory.
                    const auto resolved_shader_dir =
                        helper::resolve_relative_path(std::filesystem::path{settings_.shader_directory});
                    if (resource_dir_out != nullptr && resolved_shader_dir.has_value() &&
                        resolved_shader_path->parent_path() != *resolved_shader_dir)
                    {
                        *resource_dir_out = resolved_shader_path->parent_path();
                    }
                    return source;
                }
            }
            // Case 2: the path points to a directory (e.g. "synth" → shaders/synth/).
            if (std::filesystem::is_directory(*resolved_shader_path))
            {
                const auto source = scan_shader_dir(*resolved_shader_path);
                if (!source.isEmpty())
                {
                    return source;
                }
            }
        }

        // Case 3: the path does not exist as a file.  Try treating the stem as a
        // subdirectory name: shaders/<stem>/<stem>.glsl or any .glsl in shaders/<stem>/.
        const auto stem = std::filesystem::path{shader_file}.stem();
        const std::filesystem::path subdir_path = std::filesystem::path{settings_.shader_directory} / stem;
        const auto resolved_subdir = helper::resolve_relative_path(subdir_path);
        if (resolved_subdir.has_value() && std::filesystem::is_directory(*resolved_subdir))
        {
            // Priority: image.glsl > <stem>.glsl > any other .glsl (excluding support files).
            for (const auto &candidate : {std::string{"image.glsl"}, std::string{stem} + ".glsl"})
            {
                const auto named_file = *resolved_subdir / candidate;
                if (std::filesystem::is_regular_file(named_file))
                {
                    const auto source = helper::read_text_file_qstring(named_file);
                    if (!source.isEmpty())
                    {
                        if (resource_dir_out != nullptr)
                        {
                            *resource_dir_out = *resolved_subdir;
                        }
                        return source;
                    }
                }
            }
            // Fall back to any .glsl in the directory (excluding support files).
            const auto source = scan_shader_dir(*resolved_subdir);
            if (!source.isEmpty())
            {
                return source;
            }
        }

        const_cast<ShaderVideoWindow *>(this)->record_fatal_render_error(
            QStringLiteral("Shader import failed: could not resolve '%1'")
                .arg(QString::fromStdString(shader_path.string())));
        return {};
    }

    if (!allow_directory_scan)
    {
        return {};
    }

    const auto resolved_shader_directory =
        helper::resolve_relative_path(std::filesystem::path{settings_.shader_directory});
    if (!resolved_shader_directory.has_value())
    {
        return {};
    }

    return scan_shader_dir(*resolved_shader_directory);
}

} // namespace cockscreen::runtime
