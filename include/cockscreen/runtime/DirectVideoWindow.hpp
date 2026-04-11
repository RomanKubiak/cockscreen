#pragma once

#include <chrono>
#include <memory>
#include <unordered_map>
#include <vector>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>
#include <QResizeEvent>
#include <QString>
#include <QWidget>

#include "Application.hpp"
#include "RuntimeHelpers.hpp"
#include "V4l2Capture.hpp"

namespace cockscreen::runtime
{

class StatusOverlay;

using GLeglImageOES = void *;

class DirectVideoWindow final : public QOpenGLWidget, protected QOpenGLFunctions
{
  public:
    explicit DirectVideoWindow(const ApplicationSettings &settings, QString shader_label, bool show_status_overlay,
                               QWidget *parent = nullptr);
    ~DirectVideoWindow() override;

    [[nodiscard]] QString capture_format_label() const;
    [[nodiscard]] bool dmabuf_export_supported() const;
    [[nodiscard]] QString status_message() const;

    void set_frame(const core::ControlFrame &frame);

  protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int width, int height) override;
    void resizeEvent(QResizeEvent *event) override;

  private:
    enum class RenderMode
    {
        cpu_upload,
        dmabuf_egl,
    };

    static constexpr int kStatusBarHeight{64};

    struct ImportedTexture
    {
        GLuint texture{0};
        EGLImageKHR image{EGL_NO_IMAGE_KHR};
        int width{0};
        int height{0};
        V4l2PixelFormat pixel_format{V4l2PixelFormat::unsupported};
    };

    static int bytes_per_line(const V4l2FrameView &frame);
    static std::uint32_t drm_fourcc_for(V4l2PixelFormat pixel_format);

    void upload_latest_frame();
    void ensure_cpu_texture(const V4l2FrameView &frame);
    bool ensure_dmabuf_texture(const V4l2FrameView &frame);
    void destroy_imported_textures();
    bool initialize_dmabuf_import();

    ApplicationSettings settings_;
    QString shader_label_;
    core::ControlFrame frame_;
    V4l2Capture capture_;
    QOpenGLShaderProgram cpu_program_;
    QOpenGLShaderProgram dmabuf_program_;
    GLuint texture_id_{0};
    bool current_texture_is_external_{false};
    StatusOverlay *status_overlay_{nullptr};
    bool show_status_overlay_{true};
    EGLDisplay egl_display_{EGL_NO_DISPLAY};
    using EglCreateImageProc = EGLImageKHR (*)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *);
    using EglDestroyImageProc = EGLBoolean (*)(EGLDisplay, EGLImageKHR);
    using GlEglImageTargetTexture2DProc = void (*)(GLenum, GLeglImageOES);
    EglCreateImageProc egl_create_image_{nullptr};
    EglDestroyImageProc egl_destroy_image_{nullptr};
    GlEglImageTargetTexture2DProc gl_egl_image_target_texture_2d_{nullptr};
    RenderMode render_mode_{RenderMode::cpu_upload};
    bool dmabuf_import_ready_{false};
    bool egl_import_modifiers_supported_{false};
    int texture_width_{0};
    int texture_height_{0};
    int video_width_{0};
    int video_height_{0};
    V4l2PixelFormat current_pixel_format_{V4l2PixelFormat::unsupported};
    float texture_layout_{0.0F};
    QString capture_format_label_{QStringLiteral("unknown")};
    QString status_message_;
    std::vector<std::uint8_t> staging_;
    std::unordered_map<int, ImportedTexture> imported_textures_;
    std::chrono::steady_clock::time_point last_capture_time_{};
    std::chrono::steady_clock::time_point last_render_time_{};
    std::chrono::steady_clock::time_point last_profile_report_{};
    double capture_fps_{0.0};
    double render_fps_{0.0};
    double upload_ms_{0.0};
    double render_ms_{0.0};
};

} // namespace cockscreen::runtime