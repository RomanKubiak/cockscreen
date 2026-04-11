#include "../../include/cockscreen/runtime/DirectVideoWindow.hpp"

#include <EGL/eglext.h>
#include <QElapsedTimer>
#include <QFont>
#include <QPaintEvent>
#include <QOpenGLContext>
#include <QOpenGLShader>
#include <QPainter>
#include <QPen>
#include <QResizeEvent>
#include <QVector2D>

#include <chrono>
#include <cstring>
#include <iostream>
#include <string_view>
#include <utility>
#include <vector>

#include <drm/drm_fourcc.h>

namespace cockscreen::runtime
{

class StatusOverlay final : public QWidget
{
  public:
    explicit StatusOverlay(QWidget *parent = nullptr) : QWidget{parent}
    {
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setAttribute(Qt::WA_OpaquePaintEvent, true);
        setAutoFillBackground(false);
    }

    void set_status(QString line, QString message)
    {
        if (line_ == line && message_ == message)
        {
            return;
        }

        line_ = std::move(line);
        message_ = std::move(message);
        update();
    }

  protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter{this};
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(rect(), Qt::black);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        painter.setRenderHint(QPainter::TextAntialiasing, true);

        QFont status_font{"Sans Serif", 10};
        status_font.setBold(true);
        painter.setFont(status_font);
        painter.setPen(Qt::white);

        painter.drawText(rect().adjusted(16, 0, -16, 0), Qt::AlignVCenter | Qt::AlignLeft, line_);
        if (!message_.isEmpty())
        {
            painter.drawText(rect().adjusted(16, 0, -16, 0), Qt::AlignVCenter | Qt::AlignRight, message_);
        }
    }

  private:
    QString line_;
    QString message_;
};

namespace
{

constexpr GLenum kTextureExternalOes{0x8D65};

constexpr const char *kCpuVertexShader = R"(
    precision mediump float;
    attribute vec2 a_position;
    attribute vec2 a_texcoord;
    varying vec2 v_texcoord;
    uniform vec2 u_viewport_size;
    uniform vec2 u_video_size;
    uniform float u_status_bar_height;
    void main()
    {
        vec2 usable = vec2(u_viewport_size.x, max(u_viewport_size.y - u_status_bar_height, 1.0));
        vec2 origin = floor((usable - u_video_size) * 0.5);
        vec2 pixel = origin + a_position * u_video_size;
        vec2 ndc = vec2((pixel.x / u_viewport_size.x) * 2.0 - 1.0,
                        1.0 - (pixel.y / u_viewport_size.y) * 2.0);
        gl_Position = vec4(ndc, 0.0, 1.0);
        v_texcoord = vec2(a_texcoord.x, 1.0 - a_texcoord.y);
    }
)";

constexpr const char *kCpuFragmentShader = R"(
    precision mediump float;
    varying vec2 v_texcoord;
    uniform sampler2D u_texture;
    uniform vec2 u_video_size;
    uniform float u_layout;

    vec3 yuv_to_rgb(float y, float u, float v)
    {
        float r = y + 1.402 * v;
        float g = y - 0.344136 * u - 0.714136 * v;
        float b = y + 1.772 * u;
        return clamp(vec3(r, g, b), 0.0, 1.0);
    }

    void main()
    {
        if (u_layout < 1.5)
        {
            float pixel_x = floor(v_texcoord.x * u_video_size.x);
            float pair_width = max(u_video_size.x * 0.5, 1.0);
            vec2 pair_coord = vec2((floor(pixel_x * 0.5) + 0.5) / pair_width, v_texcoord.y);
            vec4 pair = texture2D(u_texture, pair_coord);
            float y = 0.0;
            float u = 0.0;
            float v = 0.0;
            if (u_layout < 0.5)
            {
                y = mod(pixel_x, 2.0) < 0.5 ? pair.r : pair.b;
                u = pair.g - 0.5;
                v = pair.a - 0.5;
            }
            else
            {
                y = mod(pixel_x, 2.0) < 0.5 ? pair.g : pair.a;
                u = pair.r - 0.5;
                v = pair.b - 0.5;
            }
            gl_FragColor = vec4(yuv_to_rgb(y, u, v), 1.0);
            return;
        }

        vec3 rgb = texture2D(u_texture, v_texcoord).rgb;
        if (u_layout > 2.5)
        {
            rgb = rgb.bgr;
        }
        gl_FragColor = vec4(rgb, 1.0);
    }
)";

constexpr const char *kExternalFragmentShader = R"(
    #extension GL_OES_EGL_image_external : require
    precision mediump float;
    varying vec2 v_texcoord;
    uniform samplerExternalOES u_texture;
    void main()
    {
        gl_FragColor = texture2D(u_texture, v_texcoord);
    }
)";

bool contains_extension(const char *extensions, std::string_view needle)
{
    if (extensions == nullptr || needle.empty())
    {
        return false;
    }

    return std::strstr(extensions, std::string{needle}.c_str()) != nullptr;
}

std::uint32_t drm_fourcc_value(V4l2PixelFormat pixel_format)
{
    switch (pixel_format)
    {
    case V4l2PixelFormat::yuyv:
        return DRM_FORMAT_YUYV;
    case V4l2PixelFormat::uyvy:
        return DRM_FORMAT_UYVY;
    case V4l2PixelFormat::rgb24:
        return DRM_FORMAT_RGB888;
    case V4l2PixelFormat::bgr24:
        return DRM_FORMAT_BGR888;
    default:
        return 0;
    }
}

} // namespace

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
        if (!program.addShaderFromSourceCode(QOpenGLShader::Vertex, kCpuVertexShader))
        {
            return false;
        }

        if (!program.addShaderFromSourceCode(QOpenGLShader::Fragment, fragment_shader))
        {
            return false;
        }

        return program.link();
    };

    if (!compile_program(cpu_program_, kCpuFragmentShader))
    {
        status_message_ = cpu_program_.log();
    }

    if (render_mode_ == RenderMode::dmabuf_egl && initialize_dmabuf_import())
    {
        if (!compile_program(dmabuf_program_, kExternalFragmentShader))
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

void DirectVideoWindow::paintGL()
{
    QElapsedTimer render_timer;
    render_timer.start();

    glClear(GL_COLOR_BUFFER_BIT);

    upload_latest_frame();

    if (texture_id_ != 0)
    {
        static constexpr GLfloat kVertices[] = {
            0.0F, 0.0F, 0.0F, 0.0F,
            1.0F, 0.0F, 1.0F, 0.0F,
            0.0F, 1.0F, 0.0F, 1.0F,
            1.0F, 1.0F, 1.0F, 1.0F,
        };

        QOpenGLShaderProgram *program = &cpu_program_;
        GLenum texture_target = GL_TEXTURE_2D;
        if (current_texture_is_external_ && render_mode_ == RenderMode::dmabuf_egl)
        {
            program = &dmabuf_program_;
            texture_target = kTextureExternalOes;
        }

        if (program->isLinked())
        {
            program->bind();
            program->setUniformValue("u_viewport_size", QVector2D{static_cast<float>(width()), static_cast<float>(height())});
            program->setUniformValue("u_video_size", QVector2D{static_cast<float>(video_width_), static_cast<float>(video_height_)});
            program->setUniformValue("u_status_bar_height", static_cast<float>(kStatusBarHeight));
            program->setUniformValue("u_layout", texture_layout_);
            program->setUniformValue("u_texture", 0);

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(texture_target, texture_id_);

            program->enableAttributeArray("a_position");
            program->enableAttributeArray("a_texcoord");
            program->setAttributeArray("a_position", GL_FLOAT, kVertices, 2, 4 * sizeof(GLfloat));
            program->setAttributeArray("a_texcoord", GL_FLOAT, kVertices + 2, 2, 4 * sizeof(GLfloat));
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            program->disableAttributeArray("a_position");
            program->disableAttributeArray("a_texcoord");
            glBindTexture(texture_target, 0);
            program->release();
        }
    }

    if (status_overlay_ != nullptr)
    {
        const QString status_line = QStringLiteral("FPS %1 | Gain %2 | Shader %3")
                                        .arg(capture_fps_, 0, 'f', 1)
                                        .arg(frame_.gain, 0, 'f', 2)
                                        .arg(shader_label_);
        status_overlay_->set_status(status_line, status_message_);
    }

    const auto now = std::chrono::steady_clock::now();
    if (last_render_time_ != std::chrono::steady_clock::time_point{})
    {
        const double delta_seconds = std::chrono::duration<double>(now - last_render_time_).count();
        if (delta_seconds > 0.0)
        {
            const double instant_fps = 1.0 / delta_seconds;
            render_fps_ = render_fps_ <= 0.0 ? instant_fps : render_fps_ * 0.85 + instant_fps * 0.15;
        }
    }
    last_render_time_ = now;

    render_ms_ = render_timer.nsecsElapsed() / 1.0e6;
    if (now - last_profile_report_ > std::chrono::seconds{1})
    {
        last_profile_report_ = now;
        std::cout << "Direct render profile: capture=" << capture_fps_ << " fps"
                  << ", upload=" << upload_ms_ << " ms"
                  << ", render=" << render_ms_ << " ms"
                  << ", dmabuf-export=" << (capture_.dmabuf_export_supported() ? "yes" : "no") << '\n';
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

void DirectVideoWindow::upload_latest_frame()
{
    const auto frame = capture_.dequeue();
    if (!frame.has_value())
    {
        return;
    }

    QElapsedTimer upload_timer;
    upload_timer.start();

    const auto expected_stride = bytes_per_line(*frame);
    bool imported_frame = false;

    if (render_mode_ == RenderMode::dmabuf_egl && frame->dmabuf_fd >= 0)
    {
        imported_frame = ensure_dmabuf_texture(*frame);
        if (!imported_frame)
        {
            render_mode_ = RenderMode::cpu_upload;
            current_texture_is_external_ = false;
            texture_id_ = 0;
            status_message_ = QStringLiteral("DMABUF import unavailable, using CPU upload");
        }
    }

    if (!imported_frame)
    {
        ensure_cpu_texture(*frame);
        const std::uint8_t *source = frame->data;
        if (frame->stride != expected_stride)
        {
            staging_.resize(static_cast<std::size_t>(expected_stride) * static_cast<std::size_t>(frame->height));
            for (int row = 0; row < frame->height; ++row)
            {
                std::memcpy(staging_.data() + static_cast<std::size_t>(row) * expected_stride,
                            frame->data + static_cast<std::size_t>(row) * frame->stride,
                            static_cast<std::size_t>(expected_stride));
            }
            source = staging_.data();
        }

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture_id_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

        if (frame->pixel_format == V4l2PixelFormat::yuyv || frame->pixel_format == V4l2PixelFormat::uyvy)
        {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame->width / 2, frame->height, GL_RGBA, GL_UNSIGNED_BYTE, source);
            texture_layout_ = frame->pixel_format == V4l2PixelFormat::yuyv ? 0.0F : 1.0F;
        }
        else if (frame->pixel_format == V4l2PixelFormat::rgb24 || frame->pixel_format == V4l2PixelFormat::bgr24)
        {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame->width, frame->height, GL_RGB, GL_UNSIGNED_BYTE, source);
            texture_layout_ = frame->pixel_format == V4l2PixelFormat::rgb24 ? 2.0F : 3.0F;
        }

        glBindTexture(GL_TEXTURE_2D, 0);
        current_texture_is_external_ = false;
    }

    upload_ms_ = upload_timer.nsecsElapsed() / 1.0e6;

    const auto now = std::chrono::steady_clock::now();
    if (last_capture_time_ != std::chrono::steady_clock::time_point{})
    {
        const double delta_seconds = std::chrono::duration<double>(now - last_capture_time_).count();
        if (delta_seconds > 0.0)
        {
            const double instant_fps = 1.0 / delta_seconds;
            capture_fps_ = capture_fps_ <= 0.0 ? instant_fps : capture_fps_ * 0.85 + instant_fps * 0.15;
        }
    }
    last_capture_time_ = now;

    if (imported_frame || status_message_ != QStringLiteral("DMABUF import unavailable, using CPU upload"))
    {
        status_message_.clear();
    }

    capture_.release();
}

void DirectVideoWindow::ensure_cpu_texture(const V4l2FrameView &frame)
{
    const int target_width = frame.pixel_format == V4l2PixelFormat::yuyv || frame.pixel_format == V4l2PixelFormat::uyvy
                                 ? frame.width / 2
                                 : frame.width;
    const int target_height = frame.height;

    if (texture_id_ == 0 || current_texture_is_external_)
    {
        if (texture_id_ != 0 && current_texture_is_external_)
        {
            texture_id_ = 0;
        }

        glGenTextures(1, &texture_id_);
        current_texture_is_external_ = false;
    }

    if (target_width == texture_width_ && target_height == texture_height_ && frame.pixel_format == current_pixel_format_)
    {
        return;
    }

    video_width_ = frame.width;
    video_height_ = frame.height;
    texture_width_ = target_width;
    texture_height_ = target_height;
    current_pixel_format_ = frame.pixel_format;

    glBindTexture(GL_TEXTURE_2D, texture_id_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    if (frame.pixel_format == V4l2PixelFormat::yuyv || frame.pixel_format == V4l2PixelFormat::uyvy)
    {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, target_width, target_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    }
    else
    {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, target_width, target_height, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
    }

    glBindTexture(GL_TEXTURE_2D, 0);
}

bool DirectVideoWindow::ensure_dmabuf_texture(const V4l2FrameView &frame)
{
    if (!dmabuf_import_ready_ || egl_create_image_ == nullptr || egl_destroy_image_ == nullptr ||
        gl_egl_image_target_texture_2d_ == nullptr)
    {
        return false;
    }

    const auto fourcc = drm_fourcc_value(frame.pixel_format);
    if (fourcc == 0 || frame.buffer_index < 0 || frame.dmabuf_fd < 0)
    {
        return false;
    }

    auto &entry = imported_textures_[frame.buffer_index];
    if (entry.texture != 0 && entry.width == frame.width && entry.height == frame.height &&
        entry.pixel_format == frame.pixel_format)
    {
        texture_id_ = entry.texture;
        texture_width_ = frame.width;
        texture_height_ = frame.height;
        video_width_ = frame.width;
        video_height_ = frame.height;
        current_pixel_format_ = frame.pixel_format;
        current_texture_is_external_ = true;
        return true;
    }

    if (entry.image != EGL_NO_IMAGE_KHR)
    {
        egl_destroy_image_(egl_display_, entry.image);
        entry.image = EGL_NO_IMAGE_KHR;
    }

    if (entry.texture != 0)
    {
        glDeleteTextures(1, &entry.texture);
        entry.texture = 0;
    }

    const auto modifier = static_cast<GLuint64>(DRM_FORMAT_MOD_LINEAR);
    std::vector<EGLint> attributes{
        EGL_WIDTH, frame.width,
        EGL_HEIGHT, frame.height,
        EGL_LINUX_DRM_FOURCC_EXT, static_cast<EGLint>(fourcc),
        EGL_DMA_BUF_PLANE0_FD_EXT, frame.dmabuf_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, frame.stride,
    };

    if (egl_import_modifiers_supported_)
    {
        attributes.push_back(EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT);
        attributes.push_back(static_cast<EGLint>(modifier & 0xffffffffULL));
        attributes.push_back(EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT);
        attributes.push_back(static_cast<EGLint>(modifier >> 32));
    }

    attributes.push_back(EGL_NONE);

    entry.image = egl_create_image_(egl_display_, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                                    static_cast<EGLClientBuffer>(nullptr), attributes.data());
    if (entry.image == EGL_NO_IMAGE_KHR)
    {
        return false;
    }

    glGenTextures(1, &entry.texture);
    glBindTexture(kTextureExternalOes, entry.texture);
    glTexParameteri(kTextureExternalOes, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(kTextureExternalOes, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(kTextureExternalOes, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(kTextureExternalOes, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl_egl_image_target_texture_2d_(kTextureExternalOes, reinterpret_cast<GLeglImageOES>(entry.image));
    glBindTexture(kTextureExternalOes, 0);

    entry.width = frame.width;
    entry.height = frame.height;
    entry.pixel_format = frame.pixel_format;

    texture_id_ = entry.texture;
    texture_width_ = frame.width;
    texture_height_ = frame.height;
    video_width_ = frame.width;
    video_height_ = frame.height;
    current_pixel_format_ = frame.pixel_format;
    current_texture_is_external_ = true;
    return true;
}

void DirectVideoWindow::destroy_imported_textures()
{
    for (auto &item : imported_textures_)
    {
        auto &entry = item.second;
        if (entry.image != EGL_NO_IMAGE_KHR && egl_destroy_image_ != nullptr)
        {
            egl_destroy_image_(egl_display_, entry.image);
            entry.image = EGL_NO_IMAGE_KHR;
        }

        if (entry.texture != 0)
        {
            if (entry.texture == texture_id_)
            {
                texture_id_ = 0;
            }

            glDeleteTextures(1, &entry.texture);
            entry.texture = 0;
        }
    }

    imported_textures_.clear();
}

bool DirectVideoWindow::initialize_dmabuf_import()
{
    if (!capture_.dmabuf_export_supported())
    {
        return false;
    }

    const auto *context = QOpenGLContext::currentContext();
    if (context == nullptr || !context->isOpenGLES())
    {
        return false;
    }

    egl_display_ = eglGetCurrentDisplay();
    if (egl_display_ == EGL_NO_DISPLAY)
    {
        return false;
    }

    const char *egl_extensions = eglQueryString(egl_display_, EGL_EXTENSIONS);
    if (!contains_extension(egl_extensions, "EGL_EXT_image_dma_buf_import"))
    {
        return false;
    }

    egl_import_modifiers_supported_ = contains_extension(egl_extensions, "EGL_EXT_image_dma_buf_import_modifiers");

    egl_create_image_ = reinterpret_cast<EglCreateImageProc>(eglGetProcAddress("eglCreateImageKHR"));
    egl_destroy_image_ = reinterpret_cast<EglDestroyImageProc>(eglGetProcAddress("eglDestroyImageKHR"));
    gl_egl_image_target_texture_2d_ = reinterpret_cast<GlEglImageTargetTexture2DProc>(
        context->getProcAddress("glEGLImageTargetTexture2DOES"));

    dmabuf_import_ready_ = egl_create_image_ != nullptr && egl_destroy_image_ != nullptr &&
                           gl_egl_image_target_texture_2d_ != nullptr;
    return dmabuf_import_ready_;
}

int DirectVideoWindow::bytes_per_line(const V4l2FrameView &frame)
{
    switch (frame.pixel_format)
    {
    case V4l2PixelFormat::yuyv:
    case V4l2PixelFormat::uyvy:
        return frame.width * 2;
    case V4l2PixelFormat::rgb24:
    case V4l2PixelFormat::bgr24:
        return frame.width * 3;
    default:
        return frame.stride;
    }
}

std::uint32_t DirectVideoWindow::drm_fourcc_for(V4l2PixelFormat pixel_format)
{
    return drm_fourcc_value(pixel_format);
}

} // namespace cockscreen::runtime