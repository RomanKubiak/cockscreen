#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <QtMultimedia/QCamera>
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
#include <QVector2D>
#include <QVideoFrame>
#include <QVideoSink>
#include <QWidget>

#include "../core/ControlFrame.hpp"
#include "Application.hpp"
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
      bool camera_fit_vertex{false};
      bool allow_directory_scan{false};
      QString label;
      std::unique_ptr<QOpenGLShaderProgram> program;
    };

    void handle_frame(const QVideoFrame &frame);
    void handle_playback_frame(const QVideoFrame &frame);
    void handle_playback_position_changed(std::int64_t position_ms);
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
    QString load_fragment_shader_source(std::string_view shader_file, bool allow_directory_scan) const;
    void record_fatal_render_error(QString text);
    void build_render_stages();
    void stop_playback_source();
    void restart_playback_source(bool seek_to_start);
    void configure_playback_transport(bool seek_to_start, bool reset_loop_count);
    void apply_playback_rate_for_position(std::int64_t position_ms);
    [[nodiscard]] std::optional<std::int64_t> playback_effective_loop_end_ms() const;
    [[nodiscard]] bool playback_loop_enabled() const;
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
    double processing_fps_{0.0};
    double render_fps_{0.0};
};

} // namespace cockscreen::runtime