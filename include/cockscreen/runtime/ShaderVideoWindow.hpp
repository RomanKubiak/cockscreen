#pragma once

#include <chrono>
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
    void ensure_texture();
    void ensure_playback_texture();
    void ensure_note_label_atlas_texture();
    void ensure_background_image_texture();
    void ensure_scene_fbos();
    void ensure_blank_texture();
    void ensure_background_texture();
    void upload_latest_frame();
    void upload_latest_playback_frame();
    QString load_fragment_shader_source(std::string_view shader_file, bool allow_directory_scan) const;
    void build_render_stages();
    void bind_stage_common_uniforms(QOpenGLShaderProgram *program, const RenderStage &stage, float elapsed_seconds);
    void apply_scene_midi_mappings(QOpenGLShaderProgram *program, const RenderStage &stage) const;
    GLuint render_stage(RenderStage *stage, GLuint input_texture, bool input_valid, bool output_to_screen,
              float elapsed_seconds);

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
    std::chrono::steady_clock::time_point start_time_{std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point last_frame_time_{};
    std::chrono::steady_clock::time_point last_profile_report_{};
    double processing_fps_{0.0};
    double render_fps_{0.0};
};

} // namespace cockscreen::runtime