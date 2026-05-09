#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <QtMultimedia/QCamera>
#include <QAudioBuffer>
#include <QAudioBufferOutput>
#include <QAudioOutput>
#include <QImage>
#include <QMediaCaptureSession>
#include <QMediaPlayer>
#include <QOpenGLFramebufferObject>
#include <QOpenGLBuffer>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>
#include <QStringList>
#include <QResizeEvent>
#include <QScreen>
#include <QTimer>
#include <QVector2D>
#include <QVideoFrame>
#include <QVideoSink>
#include <QWidget>

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
#endif
#include "Scene.hpp"
#include "RuntimeHelpers.hpp"

namespace cockscreen::runtime
{

class StatusOverlay;

class ShaderVideoWindow final : public QOpenGLWidget, protected QOpenGLFunctions
{
  public:
    explicit ShaderVideoWindow(const ApplicationSettings &settings, SceneDefinition scene, QCameraDevice video_device,
                               QString video_label, QString format_label, bool video_on_top,
                               bool show_status_overlay,
                               QWidget *parent = nullptr);
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

    // Returns a pointer to the internal QVideoSink so external capture sources
    // (e.g. AppsinkCapture) can push frames directly without going through QCamera.
    [[nodiscard]] QVideoSink *video_sink_ptr() { return &video_sink_; }

    void apply_scene_update(SceneDefinition scene);
    void set_status_overlay_text(QString text);

    void set_frame(const core::ControlFrame &frame);

  protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeEvent(QResizeEvent *event) override;

  private:
    static constexpr int kStatusBarHeight{64};

    struct RenderStage
    {
      QString layer_name;
      std::string shader_path;
      // Directory co-located with the shader file (non-empty for subdir shaders).
      std::filesystem::path resource_directory;
      // Per-stage channel textures: index 0→iChannel1, 1→iChannel2, 2→iChannel3.
      // 0 means "not loaded" – the renderer falls back to blank_texture_id_.
      std::array<GLuint, 3> channel_textures{0, 0, 0};
      bool camera_fit_vertex{false};
      bool allow_directory_scan{false};
      QString label;
      std::unique_ptr<QOpenGLShaderProgram> program;
    };

    void handle_frame(const QVideoFrame &frame);
    void handle_playback_frame(const QVideoFrame &frame);
    void handle_playback_position_changed(std::int64_t position_ms);
    void handle_audio_playback_position_changed(std::int64_t position_ms);
    void ensure_texture();
    void ensure_playback_texture();
    void ensure_note_label_atlas_texture();
    void ensure_icon_atlas_texture();
    void ensure_background_image_texture();
    void ensure_scene_fbos();
    void ensure_blank_texture();
    void ensure_background_texture();
    void upload_latest_frame();
    void upload_latest_playback_frame();
    // Loads a fragment shader's GLSL source.  When the shader lives inside a
    // per-shader subdirectory (e.g. shaders/synth/synth.glsl), the directory
    // path is written into *resource_dir_out (if non-null).
    QString load_fragment_shader_source(std::string_view shader_file, bool allow_directory_scan,
                                        std::filesystem::path *resource_dir_out = nullptr) const;
    void record_fatal_render_error(QString text);
    void build_render_stages();
    void stop_playback_source();
    void restart_playback_source(bool seek_to_start);
    void configure_playback_transport(bool seek_to_start, bool reset_loop_count);
    void apply_playback_rate_for_position(std::int64_t position_ms);
    [[nodiscard]] std::optional<std::int64_t> playback_effective_loop_end_ms() const;
    [[nodiscard]] bool playback_loop_enabled() const;
    void stop_audio_playback_source();
    void restart_audio_playback_source(bool seek_to_start);
    void configure_audio_playback_transport(bool seek_to_start, bool reset_loop_count);
    void apply_audio_playback_rate_for_position(std::int64_t position_ms);
    [[nodiscard]] std::optional<std::int64_t> audio_playback_effective_loop_end_ms() const;
    [[nodiscard]] bool audio_playback_loop_enabled() const;
    void tick_audio_playback_volume();
    void process_audio_playback_buffer(const QAudioBuffer &buffer);
    void bind_stage_common_uniforms(QOpenGLShaderProgram *program, const RenderStage &stage, float elapsed_seconds);
    void bind_shadertoy_uniforms(QOpenGLShaderProgram *program, float elapsed_seconds, float frame_delta_seconds,
                   int frame_index, const QVector2D &channel0_resolution) const;
    void apply_scene_midi_mappings(QOpenGLShaderProgram *program, const RenderStage &stage) const;
    void apply_scene_osc_mappings(QOpenGLShaderProgram *program, const RenderStage &stage) const;
    GLuint render_stage(RenderStage *stage, GLuint input_texture, bool input_valid, bool output_to_screen,
              float elapsed_seconds, float frame_delta_seconds, int frame_index);

    ApplicationSettings settings_;
    SceneDefinition scene_;
    QString video_label_;
    QString video_shader_label_;
    QString playback_shader_label_;
    QString screen_shader_label_;
    bool video_on_top_{false};
    bool show_status_overlay_{true};
    core::ControlFrame frame_;
    QCamera *camera_{nullptr};
    QMediaCaptureSession capture_session_;
    QVideoSink video_sink_;
    QMediaPlayer playback_player_;
    QAudioOutput playback_audio_output_;
    QVideoSink playback_sink_;
    QMediaPlayer audio_playback_player_;
    QAudioOutput audio_playback_audio_output_;
    QAudioBufferOutput audio_playback_buffer_output_;
    QTimer audio_playback_volume_timer_;
    QImage latest_frame_;
    QImage latest_playback_frame_;
    QString camera_format_label_{QStringLiteral("unknown")};
    QString status_message_;
    QString fatal_render_error_;
    QString status_overlay_text_;
    StatusOverlay *fatal_error_overlay_{nullptr};
    QOpenGLShaderProgram video_program_;
    QOpenGLShaderProgram screen_program_;
    QOpenGLShaderProgram blit_program_;
    QOpenGLBuffer quad_vertex_buffer_{QOpenGLBuffer::VertexBuffer};
    GLuint texture_id_{0};
    int texture_width_{0};
    int texture_height_{0};
    bool texture_dirty_{false};
    GLuint playback_texture_id_{0};
    int playback_texture_width_{0};
    int playback_texture_height_{0};
    bool playback_texture_dirty_{false};
    GLuint note_label_atlas_texture_id_{0};
    int note_label_atlas_texture_width_{0};
    int note_label_atlas_texture_height_{0};
    bool note_label_atlas_texture_dirty_{false};
    GLuint icon_atlas_texture_id_{0};
    bool icon_atlas_texture_dirty_{true};
    StatusOverlay *status_overlay_{nullptr};
    QOpenGLFramebufferObject *video_scene_fbo_{nullptr};
    QOpenGLFramebufferObject *video_scene_fbo_alt_{nullptr};
    QOpenGLFramebufferObject *playback_scene_fbo_{nullptr};
    QOpenGLFramebufferObject *playback_scene_fbo_alt_{nullptr};
    QOpenGLFramebufferObject *screen_scene_fbo_{nullptr};
    QOpenGLFramebufferObject *screen_scene_fbo_alt_{nullptr};
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
    std::vector<RenderStage> render_stages_;
    int render_stage_index_{0};
    int render_frame_index_{0};
    std::chrono::steady_clock::time_point start_time_{std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point last_frame_time_{};
    std::chrono::steady_clock::time_point last_profile_report_{};
    std::int64_t playback_position_ms_{0};
    std::int64_t playback_duration_ms_{0};
    int playback_loops_completed_{0};
    bool playback_transport_pending_seek_{false};
    QString playback_error_text_;
    QString playback_status_text_;
    std::optional<std::uintmax_t> playback_file_size_bytes_;
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
    std::uint64_t playback_artifact_frame_counter_{0};
#ifndef _WIN32
    // Playback-layer loopback (Step 2): GStreamer file-source pipeline →
    // RTP/UDP → tc-netem → v4l2loopback → LoopbackCapture (raw V4L2 MMAP).
    // Active only when scene_.playback_input.loopback.enabled.
    LoopbackPipeline playback_loopback_;
    LoopbackCapture *playback_loopback_capture_{nullptr};
    // Alternative to v4l2loopback: in-process GStreamer appsink receiver.
    // Active when loopback.enabled && loopback.use_appsink.
    AppsinkCapture *playback_appsink_capture_{nullptr};
#endif
};

} // namespace cockscreen::runtime