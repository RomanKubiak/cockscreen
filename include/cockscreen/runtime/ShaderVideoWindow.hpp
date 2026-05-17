#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <QAudioBuffer>
#include <QAudioBufferOutput>
#include <QAudioOutput>
#include <QImage>
#include <QMediaCaptureSession>
#include <QMediaPlayer>
#include <QMouseEvent>
#include <QOpenGLBuffer>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>
#include <QResizeEvent>
#include <QScreen>
#include <QStringList>
#include <QTimer>
#include <QTouchEvent>
#include <QVector2D>
#include <QVector4D>
#include <QVideoFrame>
#include <QVideoSink>
#include <QWidget>
#include <QtMultimedia/QCamera>

#include <array>
#include <complex>
#include <cstddef>

#include <pffft/pffft.hpp>

#include "../core/ControlFrame.hpp"
#include "Application.hpp"
#include "ArtifactInjector.hpp"
#ifndef _WIN32
#include "AppsinkCapture.hpp"
#include "LoopbackCapture.hpp"
#include "LoopbackPipeline.hpp"
#include "V4l2Capture.hpp"
#include "cockscreen/runtime/v4l2/IspPipeline.hpp"
#endif
#include "RuntimeHelpers.hpp"
#include "Scene.hpp"

namespace cockscreen::runtime
{

class StatusOverlay;

class ShaderVideoWindow final : public QOpenGLWidget, protected QOpenGLFunctions
{
  public:
    explicit ShaderVideoWindow(const ApplicationSettings &settings, SceneDefinition scene, QCameraDevice video_device,
                               QString video_device_path, QString video_label, QString format_label,
                               bool show_status_overlay, QWidget *parent = nullptr);
    ~ShaderVideoWindow() override;

    [[nodiscard]] double processing_fps() const;
    [[nodiscard]] double render_fps() const;
    [[nodiscard]] QString status_message() const;
    [[nodiscard]] QString fatal_render_error() const;
    [[nodiscard]] std::int64_t playback_position_ms() const;
    [[nodiscard]] std::int64_t playback_duration_ms() const;
    [[nodiscard]] int playback_loops_completed() const;
    [[nodiscard]] double playback_current_rate() const;
    [[nodiscard]] QString playback_error_text() const;
    [[nodiscard]] QString playback_status_text() const;
    [[nodiscard]] std::optional<std::uintmax_t> playback_file_size_bytes() const;
    [[nodiscard]] QImage latest_video_frame_image() const;

    // Returns a pointer to the internal QVideoSink so external capture sources
    // (e.g. AppsinkCapture) can push frames directly without going through QCamera.
    [[nodiscard]] QVideoSink *video_sink_ptr()
    {
        return &video_sink_;
    }

    void apply_scene_update(SceneDefinition scene);
    void set_status_overlay_text(QString text);
    void set_shader_uniform_override(const std::string &shader_name, const std::string &uniform_name, float value);
    [[nodiscard]] std::map<std::string, float> shader_uniform_overrides(std::string_view shader_name) const;

    void set_frame(const core::ControlFrame &frame);

  protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    bool event(QEvent *event) override;

  private:
    static constexpr int kStatusBarHeight{64};

  public:
    struct RenderStage
    {
        enum class ChannelSourceKind
        {
            InputTexture,
            PreviousFrame,
            BufferOutput,
            StaticTexture,
            BlankTexture,
        };

        struct ChannelSource
        {
            ChannelSourceKind kind{ChannelSourceKind::InputTexture};
            char buffer_name{'\0'};
            int static_texture_index{-1};
        };

        struct ShaderBuffer
        {
            char name{'A'};
            std::unique_ptr<QOpenGLShaderProgram> program;
            QOpenGLFramebufferObject *fbo[2]{nullptr, nullptr};
            int ping{0};
            bool has_custom_channel_sources{false};
            std::array<ChannelSource, 4> channel_sources{};
        };

        QString layer_name;
        std::string shader_path;
        std::filesystem::path resource_directory;
        std::array<GLuint, 3> channel_textures{0, 0, 0};
        std::vector<ShaderBuffer> shader_buffers;
        SceneColor background_color;
        SceneBackgroundImage background_image;
        GLuint background_image_texture_id{0};
        int background_image_texture_width{0};
        int background_image_texture_height{0};
        bool has_custom_channel_sources{false};
        std::array<ChannelSource, 4> channel_sources{};
        bool camera_fit_vertex{false};
        bool allow_directory_scan{false};
        QString label;
        std::unique_ptr<QOpenGLShaderProgram> program;
    };

    // Per-playback-layer state.  Owned by ShaderVideoWindow via unique_ptr map.
    // QObjects (player/audio_output/sink) are raw owning pointers deleted in ~PlaybackState.
    struct PlaybackState
    {
        QMediaPlayer *player{nullptr};
        QAudioOutput *audio_output{nullptr};
        QVideoSink *sink{nullptr};

        QImage latest_frame;
        GLuint texture_id{0};
        int texture_width{0};
        int texture_height{0};
        bool texture_dirty{false};

        std::int64_t position_ms{0};
        std::int64_t duration_ms{0};
        int loops_completed{0};
        bool transport_pending_seek{false};
        QString error_text;
        QString status_text;
        std::optional<std::uintmax_t> file_size_bytes;
        std::uint64_t artifact_frame_counter{0};

#ifndef _WIN32
        LoopbackPipeline loopback;
        LoopbackCapture *loopback_capture{nullptr};
        AppsinkCapture *appsink_capture{nullptr};
#endif

        PlaybackState() = default;
        ~PlaybackState();
        PlaybackState(const PlaybackState &) = delete;
        PlaybackState &operator=(const PlaybackState &) = delete;
    };

    // Per-video-layer state for independent non-primary feeds.  The built-in
    // "video" layer still uses the primary QCamera/raw/appsink path.
    struct VideoLayerState
    {
        QVideoSink *sink{nullptr};
        QImage latest_frame;
        GLuint texture_id{0};
        int texture_width{0};
        int texture_height{0};
        bool texture_dirty{false};
        std::uint64_t artifact_frame_counter{0};

#ifndef _WIN32
        AppsinkCapture *appsink_capture{nullptr};
#endif

        VideoLayerState() = default;
        ~VideoLayerState();
        VideoLayerState(const VideoLayerState &) = delete;
        VideoLayerState &operator=(const VideoLayerState &) = delete;
    };

  private:
    void handle_frame(const QVideoFrame &frame);
    void handle_video_layer_frame(const std::string &layer_name, const QVideoFrame &frame);
    void handle_playback_frame(const std::string &layer_name, const QVideoFrame &frame);
    void handle_playback_position_changed(const std::string &layer_name, std::int64_t position_ms);
    void handle_audio_playback_position_changed(std::int64_t position_ms);
    void ensure_texture();
    void ensure_video_layer_textures();
    void ensure_playback_texture();
    void ensure_note_label_atlas_texture();
    void ensure_icon_atlas_texture();
    void ensure_background_image_texture();
    void ensure_scene_fbos();
    void ensure_shader_buffer_fbos(RenderStage &stage);
    void render_shader_buffers(RenderStage &stage, GLuint video_texture, float elapsed_seconds,
                               float frame_delta_seconds, int frame_index);
    void ensure_blank_texture();
    void ensure_background_texture();
    void upload_latest_frame();
    void upload_latest_video_layer_frames();
    void upload_latest_playback_frame();
    static QImage build_no_signal_frame(int width, int height);
    QString load_fragment_shader_source(std::string_view shader_file, bool allow_directory_scan,
                                        std::filesystem::path *resource_dir_out = nullptr) const;
    void record_fatal_render_error(QString text);
    void build_render_stages();
    void stop_playback_source(const std::string &layer_name);
    void restart_playback_source(const std::string &layer_name, bool seek_to_start);
    void configure_playback_transport(const std::string &layer_name, bool seek_to_start, bool reset_loop_count);
    void apply_playback_rate_for_position(const std::string &layer_name, std::int64_t position_ms);
    [[nodiscard]] std::optional<std::int64_t> playback_effective_loop_end_ms(const std::string &layer_name) const;
    [[nodiscard]] bool playback_loop_enabled(const std::string &layer_name) const;
    [[nodiscard]] PlaybackState *primary_playback_state() noexcept;
    [[nodiscard]] const PlaybackState *primary_playback_state() const noexcept;
    void stop_audio_playback_source();
    void restart_audio_playback_source(bool seek_to_start);
    void configure_audio_playback_transport(bool seek_to_start, bool reset_loop_count);
    void apply_audio_playback_rate_for_position(std::int64_t position_ms);
    [[nodiscard]] std::optional<std::int64_t> audio_playback_effective_loop_end_ms() const;
    [[nodiscard]] bool audio_playback_loop_enabled() const;
    void tick_audio_playback_volume();
    void process_audio_playback_buffer(const QAudioBuffer &buffer);
    void update_pointer_state(const QPointF &position, bool pressed, bool new_press);
    void sync_pointer_state_from_cursor();
    [[nodiscard]] QVector4D shadertoy_mouse_uniform(const QSize &render_size);
    void bind_stage_common_uniforms(QOpenGLShaderProgram *program, const RenderStage &stage, float elapsed_seconds);
    void bind_shadertoy_uniforms(QOpenGLShaderProgram *program, float elapsed_seconds, float frame_delta_seconds,
                                 int frame_index, const QVector2D &channel0_resolution);
    void apply_scene_shader_uniforms(QOpenGLShaderProgram *program, const RenderStage &stage) const;
    void apply_scene_midi_mappings(QOpenGLShaderProgram *program, const RenderStage &stage) const;
    void apply_scene_osc_mappings(QOpenGLShaderProgram *program, const RenderStage &stage) const;
    void apply_shader_uniform_overrides(QOpenGLShaderProgram *program, const RenderStage &stage) const;
    GLuint render_stage(RenderStage *stage, GLuint input_texture, bool input_valid, bool output_to_screen,
                        float elapsed_seconds, float frame_delta_seconds, int frame_index);

    ApplicationSettings settings_;
    SceneDefinition scene_;
    QString video_label_;
    // Per-layer playback instances, keyed by named_layer name.
    std::map<std::string, std::unique_ptr<PlaybackState>> playback_states_;
    std::map<std::string, std::unique_ptr<VideoLayerState>> video_layer_states_;
    bool show_status_overlay_{true};
    core::ControlFrame frame_;
    QCamera *camera_{nullptr};
    QMediaCaptureSession capture_session_;
    QVideoSink video_sink_;
    QMediaPlayer audio_playback_player_;
    QAudioOutput audio_playback_audio_output_;
    QAudioBufferOutput audio_playback_buffer_output_;
    QTimer render_tick_timer_;
    QTimer audio_playback_volume_timer_;
    QImage latest_frame_;
    QString camera_format_label_{QStringLiteral("unknown")};
    QString status_message_;
    QString fatal_render_error_;
    QString status_overlay_text_;
    StatusOverlay *fatal_error_overlay_{nullptr};
    bool camera_signal_locked_{false};
    bool camera_placeholder_shown_{false};
    std::chrono::steady_clock::time_point camera_capture_start_time_{std::chrono::steady_clock::now()};
#ifndef _WIN32
    V4l2Capture raw_video_capture_;
    bool raw_video_capture_active_{false};
    v4l2::IspPipeline isp_pipeline_;
    bool use_isp_{false};
    bool raw_video_frame_received_{false};
    bool raw_video_placeholder_shown_{false};
    int raw_video_blank_frame_count_{0};
    std::chrono::steady_clock::time_point raw_video_capture_start_time_{std::chrono::steady_clock::now()};
#endif
    QOpenGLShaderProgram video_program_;
    QOpenGLShaderProgram screen_program_;
    QOpenGLShaderProgram blit_program_;
    QOpenGLBuffer quad_vertex_buffer_{QOpenGLBuffer::VertexBuffer};
    GLuint texture_id_{0};
    int texture_width_{0};
    int texture_height_{0};
    bool texture_dirty_{false};
    GLuint note_label_atlas_texture_id_{0};
    int note_label_atlas_texture_width_{0};
    int note_label_atlas_texture_height_{0};
    bool note_label_atlas_texture_dirty_{false};
    GLuint icon_atlas_texture_id_{0};
    bool icon_atlas_texture_dirty_{true};
    StatusOverlay *status_overlay_{nullptr};
    // Per-named-layer FBOs (ping-pong pair), keyed by layer name.
    std::map<std::string, QOpenGLFramebufferObject *> layer_fbos_;
    std::map<std::string, QOpenGLFramebufferObject *> layer_fbos_alt_;
    QOpenGLFramebufferObject *composite_scene_fbo_{nullptr};
    int scene_fbo_width_{0};
    int scene_fbo_height_{0};
    bool scene_fbo_dirty_{true};
    GLuint blank_texture_id_{0};
    bool blank_texture_dirty_{true};
    GLuint background_texture_id_{0};
    bool background_texture_dirty_{true};
    GLuint background_image_texture_id_{0};
    int background_image_texture_width_{0};
    int background_image_texture_height_{0};
    bool background_image_texture_dirty_{false};
    std::map<std::string, std::map<std::string, float>> shader_uniform_overrides_;
    std::vector<RenderStage> render_stages_;
    QVector2D pointer_position_{};
    QVector2D pointer_press_origin_{};
    bool pointer_pressed_{false};
    bool pointer_has_position_{false};
    int render_stage_index_{0};
    int render_frame_index_{0};
    std::chrono::steady_clock::time_point start_time_{std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point last_frame_time_{};
    std::chrono::steady_clock::time_point last_profile_report_{};
    std::int64_t audio_playback_position_ms_{0};
    std::int64_t audio_playback_duration_ms_{0};
    int audio_playback_loops_completed_{0};
    bool audio_playback_transport_pending_seek_{false};
    QString audio_playback_error_text_;
    QString audio_playback_status_text_;
    // Volume fade state for audio_playback
    float audio_playback_current_volume_{0.0F};
    bool audio_playback_outro_active_{false}; // fading out after final loop / EOM
    // FFT analysis state driven by QAudioBufferOutput (used when fft_analysis_from_playback)
    static constexpr int kAudioPlaybackFftSize{1024};
    std::array<float, kAudioPlaybackFftSize> audio_playback_fft_sample_buffer_{};
    std::array<float, kAudioPlaybackFftSize> audio_playback_fft_window_{};
    std::size_t audio_playback_fft_sample_count_{0};
    pffft::Fft<float> audio_playback_fft_{kAudioPlaybackFftSize};
    pffft::AlignedVector<float> audio_playback_fft_input_{audio_playback_fft_.valueVector()};
    pffft::AlignedVector<std::complex<float>> audio_playback_fft_spectrum_{audio_playback_fft_.spectrumVector()};
    std::array<float, core::kAudioFftBandCount> audio_playback_fft_bands_{};
    std::array<float, core::kAudioWaveformSampleCount> audio_playback_waveform_{};
    float audio_playback_analysis_rms_{0.0F};
    float audio_playback_analysis_peak_{0.0F};
    double processing_fps_{0.0};
    double render_fps_{0.0};
    std::uint64_t video_artifact_frame_counter_{0};
};

} // namespace cockscreen::runtime
