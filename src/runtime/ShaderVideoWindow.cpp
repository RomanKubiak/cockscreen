#include "../../include/cockscreen/runtime/ShaderVideoWindow.hpp"

#include <QAudioOutput>
#include <QColor>
#include <QElapsedTimer>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFramebufferObjectFormat>
#include <QOpenGLContext>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QMediaPlayer>
#include <QResizeEvent>
#include <QUrl>
#include <QVideoFrame>
#include <QVector2D>
#include <QStringList>
#include <QRectF>

#include <chrono>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <optional>
#include <utility>

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

QString strip_precision_qualifiers(QString source)
{
    QString result;
    const auto lines = source.split('\n');
    result.reserve(source.size());

    for (const auto &line : lines)
    {
        if (line.trimmed().startsWith(QStringLiteral("precision ")))
        {
            continue;
        }

        result += line;
        result += QLatin1Char('\n');
    }

    if (!result.isEmpty())
    {
        result.chop(1);
    }

    return result;
}

QString shader_source_for_current_context(QString source)
{
    const auto *context = QOpenGLContext::currentContext();
    if (context != nullptr && !context->isOpenGLES())
    {
        return strip_precision_qualifiers(std::move(source));
    }

    return source;
}

constexpr const char *kVertexShader = R"(
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

constexpr const char *kFullscreenVertexShader = R"(
    attribute vec2 a_position;
    attribute vec2 a_texcoord;
    varying vec2 v_texcoord;
    void main()
    {
        gl_Position = vec4(a_position * 2.0 - 1.0, 0.0, 1.0);
        v_texcoord = a_texcoord;
    }
)";

constexpr const char *kPassthroughFragmentShader = R"(
    precision mediump float;
    varying vec2 v_texcoord;
    uniform sampler2D u_texture;
    uniform float u_opacity;
    void main()
    {
        vec4 color = texture2D(u_texture, v_texcoord);
        gl_FragColor = vec4(color.rgb, color.a * u_opacity);
    }
)";

void set_midi_uniforms(QOpenGLShaderProgram *program, const core::ControlFrame &frame)
{
    if (program == nullptr)
    {
        return;
    }

    program->setUniformValue("u_midi_primary", frame.midi_primary);
    program->setUniformValue("u_midi_secondary", frame.midi_secondary);
    program->setUniformValueArray("u_midi_notes", frame.midi_notes.data(), static_cast<int>(frame.midi_notes.size()), 1);
    program->setUniformValueArray("u_midi_velocities", frame.midi_velocities.data(),
                                  static_cast<int>(frame.midi_velocities.size()), 1);
    program->setUniformValueArray("u_midi_ages", frame.midi_ages.data(), static_cast<int>(frame.midi_ages.size()), 1);
    program->setUniformValueArray("u_midi_channels", frame.midi_channels.data(),
                                  static_cast<int>(frame.midi_channels.size()), 1);
}

QString read_text_file_qstring(const std::filesystem::path &path)
{
    const auto text = read_text_file(path);
    if (!text.has_value())
    {
        return {};
    }

    return QString::fromStdString(*text);
}

std::optional<std::filesystem::path> resolve_relative_path(const std::filesystem::path &relative_path)
{
    if (relative_path.is_absolute())
    {
        return std::filesystem::exists(relative_path) ? std::optional{relative_path} : std::nullopt;
    }

    auto current = std::filesystem::current_path();
    while (true)
    {
        const auto candidate = current / relative_path;
        if (std::filesystem::exists(candidate))
        {
            return candidate;
        }

        if (!current.has_parent_path() || current == current.parent_path())
        {
            break;
        }

        current = current.parent_path();
    }

    return std::nullopt;
}

QColor scene_clear_color(const SceneColor &color)
{
    return QColor::fromRgbF(color.red, color.green, color.blue, color.alpha);
}

std::pair<int, int> requested_video_dimensions(const SceneDefinition &scene, const ApplicationSettings &settings)
{
    if (const auto requested = parse_capture_mode_dimensions(scene.video_input.format); requested.has_value())
    {
        return *requested;
    }

    return {settings.width, settings.height};
}

QRectF video_display_rect(const SceneInput &video_input, const QSize &viewport_size)
{
    const float scale = std::max(video_input.scale, 0.01F);
    const float width = static_cast<float>(viewport_size.width()) * scale;
    const float height = static_cast<float>(viewport_size.height()) * scale;
    const float x = std::clamp(video_input.position_x, 0.0F, 1.0F) * (static_cast<float>(viewport_size.width()) - width);
    const float y = std::clamp(video_input.position_y, 0.0F, 1.0F) * (static_cast<float>(viewport_size.height()) - height);
    return QRectF{x, y, width, height};
}

struct DirectUploadFormat
{
    GLenum external_format{0};
    int bytes_per_pixel{0};
};

constexpr int kNoteAtlasColumns{16};
constexpr int kNoteAtlasRows{8};
constexpr int kNoteAtlasCellSize{96};

QString note_label_for_midi_note(int note_number)
{
    static constexpr const char *kPitchClasses[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const int normalized = std::clamp(note_number, 0, 127);
    const int pitch_class = normalized % 12;
    const int octave = normalized / 12 - 1;
    return QStringLiteral("%1%2").arg(QString::fromUtf8(kPitchClasses[pitch_class])).arg(octave);
}

std::optional<std::filesystem::path> resolve_scene_resource_path(const std::filesystem::path &resources_directory,
                                                                 const std::string &resource_file)
{
    if (resource_file.empty())
    {
        return std::nullopt;
    }

    const std::filesystem::path requested{resource_file};
    if (requested.is_absolute())
    {
        return std::filesystem::exists(requested) ? std::optional{requested} : std::nullopt;
    }

    const auto candidate = resources_directory / requested;
    return std::filesystem::exists(candidate) ? std::optional{candidate} : std::nullopt;
}

QString note_font_family_for_scene(const SceneDefinition &scene)
{
    const auto font_path = resolve_scene_resource_path(scene.resources_directory, scene.note_font_file);
    if (!font_path.has_value())
    {
        return QStringLiteral("Sans Serif");
    }

    const int font_id = QFontDatabase::addApplicationFont(QString::fromStdString(font_path->string()));
    if (font_id < 0)
    {
        return QStringLiteral("Sans Serif");
    }

    const auto families = QFontDatabase::applicationFontFamilies(font_id);
    if (families.isEmpty())
    {
        return QStringLiteral("Sans Serif");
    }

    return families.front();
}

bool image_has_opaque_pixels(const QImage &image)
{
    for (int y = 0; y < image.height(); ++y)
    {
        const auto *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x)
        {
            if (qAlpha(row[x]) > 0)
            {
                return true;
            }
        }
    }

    return false;
}

float image_opaque_coverage(const QImage &image)
{
    if (image.isNull() || image.width() <= 0 || image.height() <= 0)
    {
        return 0.0F;
    }

    int opaque_pixels = 0;
    const int total_pixels = image.width() * image.height();

    for (int y = 0; y < image.height(); ++y)
    {
        const auto *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x)
        {
            if (qAlpha(row[x]) > 0)
            {
                ++opaque_pixels;
            }
        }
    }

    return static_cast<float>(opaque_pixels) / static_cast<float>(std::max(total_pixels, 1));
}

QImage build_note_label_atlas_image(const QString &font_family)
{
    QImage atlas{kNoteAtlasColumns * kNoteAtlasCellSize, kNoteAtlasRows * kNoteAtlasCellSize, QImage::Format_RGBA8888};
    atlas.fill(Qt::transparent);

    QPainter painter{&atlas};
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    QFont font{font_family};
    font.setFixedPitch(true);
    font.setBold(false);
    font.setWeight(QFont::Normal);
    font.setStyleHint(QFont::Monospace, QFont::PreferMatch);
    font.setHintingPreference(QFont::PreferFullHinting);

    const QString longest_label = QStringLiteral("C#-1");
    const float target_width = static_cast<float>(kNoteAtlasCellSize) * 0.70F;
    const float target_height = static_cast<float>(kNoteAtlasCellSize) * 0.45F;
    int pixel_size = static_cast<int>(kNoteAtlasCellSize * 0.36F);
    for (; pixel_size >= 8; --pixel_size)
    {
        font.setPixelSize(pixel_size);
        const QFontMetricsF metrics{font};
        if (metrics.horizontalAdvance(longest_label) <= target_width && metrics.height() <= target_height)
        {
            break;
        }
    }
    font.setPixelSize(std::max(pixel_size, 8));
    painter.setFont(font);
    painter.setPen(Qt::white);

    for (int note = 0; note < 128; ++note)
    {
        const int column = note % kNoteAtlasColumns;
        const int row = note / kNoteAtlasColumns;
        const QRect cell{column * kNoteAtlasCellSize, row * kNoteAtlasCellSize, kNoteAtlasCellSize, kNoteAtlasCellSize};
        painter.drawText(cell, Qt::AlignCenter, note_label_for_midi_note(note));
    }

    return atlas;
}

QImage vertically_flipped_image(const QImage &image)
{
    return image.mirrored(false, true);
}

} // namespace

ShaderVideoWindow::ShaderVideoWindow(const ApplicationSettings &settings, SceneDefinition scene, QCameraDevice video_device,
                                                                         QString video_label, QString format_label, bool video_on_top,
                                                                         bool show_status_overlay,
                                                                         QWidget *parent)
        : QOpenGLWidget{parent}, settings_{settings}, scene_{std::move(scene)}, video_label_{std::move(video_label)},
            video_on_top_{video_on_top}, show_status_overlay_{show_status_overlay}, camera_format_label_{std::move(format_label)}
{
    setWindowTitle(QString::fromStdString(settings_.window_title));
    resize(settings_.width, settings_.height);
    setMinimumSize(900, 540);
    setAutoFillBackground(false);
    setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);
    setAttribute(Qt::WA_AcceptTouchEvents, true);
    setCursor(Qt::BlankCursor);

    if (show_status_overlay_)
    {
        status_overlay_ = new StatusOverlay{this};
        status_overlay_->setGeometry(0, height() - kStatusBarHeight, width(), kStatusBarHeight);
        status_overlay_->raise();
    }

    capture_session_.setVideoSink(&video_sink_);
    QObject::connect(&video_sink_, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame &frame) {
        handle_frame(frame);
    });

    playback_player_.setVideoOutput(&playback_sink_);
    playback_audio_output_.setVolume(0.0F);
    playback_player_.setAudioOutput(&playback_audio_output_);
    QObject::connect(&playback_sink_, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame &frame) {
        handle_playback_frame(frame);
    });

    if (!video_device.isNull())
    {
        camera_ = new QCamera{video_device, this};
        const auto [requested_width, requested_height] = requested_video_dimensions(scene_, settings_);
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
        const auto playback_path = resolve_scene_resource_path(scene_.resources_directory, scene_.playback_input.file);
        if (playback_path.has_value())
        {
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
    if (note_label_atlas_texture_id_ != 0)
    {
        glDeleteTextures(1, &note_label_atlas_texture_id_);
        note_label_atlas_texture_id_ = 0;
    }
    doneCurrent();
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
    const QColor clear_color = scene_clear_color(scene_.background_color);
    glClearColor(clear_color.redF(), clear_color.greenF(), clear_color.blueF(), clear_color.alphaF());

    build_render_stages();

    const QString vertex_shader_source = shader_source_for_current_context(QString::fromUtf8(kVertexShader));
    const QString fullscreen_vertex_shader_source =
        shader_source_for_current_context(QString::fromUtf8(kFullscreenVertexShader));

    if (!blit_program_.addShaderFromSourceCode(QOpenGLShader::Vertex, fullscreen_vertex_shader_source) ||
        !blit_program_.addShaderFromSourceCode(QOpenGLShader::Fragment,
                                               shader_source_for_current_context(QString::fromUtf8(kPassthroughFragmentShader))) ||
        !blit_program_.link())
    {
        status_message_ = blit_program_.log();
    }

    ensure_blank_texture();
    ensure_background_texture();
    ensure_note_label_atlas_texture();
    scene_fbo_dirty_ = true;
}

void ShaderVideoWindow::paintGL()
{
    QElapsedTimer render_timer;
    render_timer.start();

    const QColor clear_color = scene_clear_color(scene_.background_color);

    glClearColor(clear_color.redF(), clear_color.greenF(), clear_color.blueF(), clear_color.alphaF());
    glClear(GL_COLOR_BUFFER_BIT);

    upload_latest_frame();
    upload_latest_playback_frame();
    ensure_scene_fbos();
    ensure_blank_texture();
    ensure_background_texture();

    const auto now = std::chrono::steady_clock::now();
    const float elapsed_seconds = std::chrono::duration<float>(now - start_time_).count();
    const GLfloat top_layer_opacity = static_cast<GLfloat>(std::clamp(settings_.top_layer_opacity, 0.0, 1.0));
    const auto screen_pixels = static_cast<long long>(width()) * static_cast<long long>(height());

    const GLuint camera_texture = texture_id_ != 0 ? texture_id_ : blank_texture_id_;
    const bool camera_valid = texture_id_ != 0;
    const GLuint playback_texture = playback_texture_id_ != 0 ? playback_texture_id_ : blank_texture_id_;
    const bool playback_valid = playback_texture_id_ != 0;
    const QRectF video_rect = video_display_rect(scene_.video_input, QSize{width(), height()});
    const QRectF playback_rect = video_display_rect(scene_.playback_input, QSize{width(), height()});

    auto render_layer_chain = [&](const QString &layer_name, GLuint source_texture, bool source_valid) -> GLuint {
        const SceneLayer *layer = nullptr;
        if (layer_name == QStringLiteral("video"))
        {
            layer = &scene_.video_layer;
        }
        else if (layer_name == QStringLiteral("playback"))
        {
            layer = &scene_.playback_layer;
        }
        else if (layer_name == QStringLiteral("screen"))
        {
            layer = &scene_.screen_layer;
        }

        if (layer == nullptr || !layer->enabled)
        {
            return 0;
        }

        GLuint current_texture = source_texture;
        bool current_valid = source_valid;
        render_stage_index_ = 0;

        for (auto &stage : render_stages_)
        {
            if (stage.layer_name != layer_name)
            {
                continue;
            }

            current_texture = render_stage(&stage, current_texture, current_valid, false, elapsed_seconds);
            current_valid = true;
            ++render_stage_index_;
        }

        return current_texture;
    };

    const GLuint video_output = render_layer_chain(QStringLiteral("video"), camera_texture, camera_valid);
    const GLuint playback_output = render_layer_chain(QStringLiteral("playback"), playback_texture, playback_valid);
    const GLuint screen_output = render_layer_chain(QStringLiteral("screen"), background_texture_id_, true);

    auto draw_texture = [&](GLuint texture, const QRectF &rect, GLfloat opacity) {
        if (texture == 0 || !blit_program_.isLinked())
        {
            return;
        }

        const GLfloat left = static_cast<GLfloat>(rect.left() / std::max(static_cast<float>(width()), 1.0F));
        const GLfloat right = static_cast<GLfloat>(rect.right() / std::max(static_cast<float>(width()), 1.0F));
        const GLfloat top = static_cast<GLfloat>(rect.top() / std::max(static_cast<float>(height()), 1.0F));
        const GLfloat bottom = static_cast<GLfloat>(rect.bottom() / std::max(static_cast<float>(height()), 1.0F));

        const GLfloat vertices[] = {
            left, top, 0.0F, 0.0F,
            right, top, 1.0F, 0.0F,
            left, bottom, 0.0F, 1.0F,
            right, bottom, 1.0F, 1.0F,
        };

        blit_program_.bind();
        blit_program_.setUniformValue("u_texture", 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
        blit_program_.enableAttributeArray("a_position");
        blit_program_.enableAttributeArray("a_texcoord");
        blit_program_.setAttributeArray("a_position", GL_FLOAT, vertices, 2, 4 * sizeof(GLfloat));
        blit_program_.setAttributeArray("a_texcoord", GL_FLOAT, vertices + 2, 2, 4 * sizeof(GLfloat));

        if (opacity >= 0.999F)
        {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }
        else
        {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }

        blit_program_.setUniformValue("u_opacity", opacity);

        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        blit_program_.disableAttributeArray("a_position");
        blit_program_.disableAttributeArray("a_texcoord");
        glBindTexture(GL_TEXTURE_2D, 0);
        glDisable(GL_BLEND);
        blit_program_.release();
    };

    const QRectF full_rect{0.0, 0.0, static_cast<qreal>(width()), static_cast<qreal>(height())};

    if (video_on_top_)
    {
        draw_texture(screen_output, full_rect, 1.0F);
        draw_texture(playback_output, playback_rect, 1.0F);
        draw_texture(video_output, video_rect, top_layer_opacity);
    }
    else
    {
        draw_texture(video_output, video_rect, 1.0F);
        draw_texture(playback_output, playback_rect, 1.0F);
        draw_texture(screen_output, full_rect, top_layer_opacity);
    }

    if (status_overlay_ != nullptr)
    {
        const QString status_line = QStringLiteral("FPS %1 | Gain %2 | Px %3 | V %4 | P %5 | S %6")
                                        .arg(processing_fps_, 0, 'f', 1)
                                        .arg(frame_.gain, 0, 'f', 2)
                                        .arg(screen_pixels)
                                        .arg(video_shader_label_)
                                        .arg(playback_shader_label_)
                                        .arg(screen_shader_label_);
        status_overlay_->set_status(status_line, status_message_);
    }

    if (last_frame_time_ != std::chrono::steady_clock::time_point{})
    {
        const double delta_seconds = std::chrono::duration<double>(now - last_frame_time_).count();
        if (delta_seconds > 0.0)
        {
            const double instant_fps = 1.0 / delta_seconds;
            processing_fps_ = processing_fps_ <= 0.0 ? instant_fps : processing_fps_ * 0.85 + instant_fps * 0.15;
        }
    }
    last_frame_time_ = now;

    render_fps_ = render_timer.nsecsElapsed() > 0 ? 1.0e9 / static_cast<double>(render_timer.nsecsElapsed()) : render_fps_;
    if (now - last_profile_report_ > std::chrono::seconds{1})
    {
        last_profile_report_ = now;
        std::cout << "Shader video profile: camera=" << processing_fps_ << " fps"
                  << ", render=" << render_fps_ << " fps"
                  << ", pixels=" << screen_pixels
                  << ", video-shader=" << video_shader_label_.toStdString()
                  << ", screen-shader=" << screen_shader_label_.toStdString() << '\n';
    }
}

void ShaderVideoWindow::resizeEvent(QResizeEvent *event)
{
    QOpenGLWidget::resizeEvent(event);
    scene_fbo_dirty_ = true;
    if (status_overlay_ != nullptr)
    {
        status_overlay_->setGeometry(0, height() - kStatusBarHeight, width(), kStatusBarHeight);
        status_overlay_->raise();
    }
}

void ShaderVideoWindow::handle_frame(const QVideoFrame &frame)
{
    if (!frame.isValid())
    {
        return;
    }

    const QImage image = frame.toImage();
    if (image.isNull())
    {
        return;
    }

    latest_frame_ = image.convertToFormat(QImage::Format_RGBA8888);
    texture_dirty_ = true;
    update();
}

void ShaderVideoWindow::handle_playback_frame(const QVideoFrame &frame)
{
    if (!frame.isValid())
    {
        return;
    }

    const QImage image = frame.toImage();
    if (image.isNull())
    {
        return;
    }

    latest_playback_frame_ = image.convertToFormat(QImage::Format_RGBA8888);
    playback_texture_dirty_ = true;
    update();
}

void ShaderVideoWindow::ensure_texture()
{
    if (texture_id_ == 0)
    {
        glGenTextures(1, &texture_id_);
        glBindTexture(GL_TEXTURE_2D, texture_id_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
}

void ShaderVideoWindow::ensure_playback_texture()
{
    if (playback_texture_id_ == 0)
    {
        glGenTextures(1, &playback_texture_id_);
        glBindTexture(GL_TEXTURE_2D, playback_texture_id_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
}

void ShaderVideoWindow::ensure_note_label_atlas_texture()
{
    if (note_label_atlas_texture_id_ == 0)
    {
        glGenTextures(1, &note_label_atlas_texture_id_);
        glBindTexture(GL_TEXTURE_2D, note_label_atlas_texture_id_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    if (!note_label_atlas_texture_dirty_)
    {
        return;
    }

    QString font_family = note_font_family_for_scene(scene_);
    QImage atlas = build_note_label_atlas_image(font_family);
    const float atlas_coverage = image_opaque_coverage(atlas);
    if ((!image_has_opaque_pixels(atlas) || atlas_coverage > 0.35F) && font_family != QStringLiteral("Sans Serif"))
    {
        atlas = build_note_label_atlas_image(QStringLiteral("Sans Serif"));
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, note_label_atlas_texture_id_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, atlas.width(), atlas.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 atlas.constBits());
    glBindTexture(GL_TEXTURE_2D, 0);
    note_label_atlas_texture_width_ = atlas.width();
    note_label_atlas_texture_height_ = atlas.height();
    note_label_atlas_texture_dirty_ = false;
}

void ShaderVideoWindow::ensure_blank_texture()
{
    if (blank_texture_id_ != 0)
    {
        return;
    }

    static constexpr unsigned char kBlankPixel[] = {0, 0, 0, 0};
    glGenTextures(1, &blank_texture_id_);
    glBindTexture(GL_TEXTURE_2D, blank_texture_id_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, kBlankPixel);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void ShaderVideoWindow::ensure_background_texture()
{
    if (background_texture_id_ == 0)
    {
        glGenTextures(1, &background_texture_id_);
        glBindTexture(GL_TEXTURE_2D, background_texture_id_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    if (!background_texture_dirty_)
    {
        glBindTexture(GL_TEXTURE_2D, 0);
        return;
    }

    const QColor clear_color = scene_clear_color(scene_.background_color);
    const unsigned char pixel[] = {
        static_cast<unsigned char>(std::clamp(clear_color.redF(), 0.0F, 1.0F) * 255.0F),
        static_cast<unsigned char>(std::clamp(clear_color.greenF(), 0.0F, 1.0F) * 255.0F),
        static_cast<unsigned char>(std::clamp(clear_color.blueF(), 0.0F, 1.0F) * 255.0F),
        static_cast<unsigned char>(std::clamp(clear_color.alphaF(), 0.0F, 1.0F) * 255.0F),
    };

    glBindTexture(GL_TEXTURE_2D, background_texture_id_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    glBindTexture(GL_TEXTURE_2D, 0);
    background_texture_dirty_ = false;
}

void ShaderVideoWindow::bind_stage_common_uniforms(QOpenGLShaderProgram *program, const RenderStage &stage,
                                                   float elapsed_seconds)
{
    if (program == nullptr)
    {
        return;
    }

    Q_UNUSED(stage);
    program->setUniformValue("u_time", elapsed_seconds);
    program->setUniformValue("u_audio_level", frame_.audio_level);
    program->setUniformValueArray("u_audio_fft", frame_.audio_fft_bands.data(),
                                  static_cast<int>(frame_.audio_fft_bands.size()), 1);
    if (note_label_atlas_texture_id_ != 0)
    {
        program->setUniformValue("u_note_label_atlas", 1);
        program->setUniformValue("u_note_label_grid", QVector2D{static_cast<float>(kNoteAtlasColumns),
                                                                static_cast<float>(kNoteAtlasRows)});
    }
    set_midi_uniforms(program, frame_);
}

void ShaderVideoWindow::apply_scene_midi_mappings(QOpenGLShaderProgram *program, const RenderStage &stage) const
{
    if (program == nullptr || !program->isLinked())
    {
        return;
    }

    for (const auto &mapping : scene_.midi_cc_mappings)
    {
        if (mapping.layer != stage.layer_name.toStdString())
        {
            continue;
        }

        if (!mapping.shader.empty() && mapping.shader != stage.shader_path)
        {
            continue;
        }

        if (mapping.channel < 0 || mapping.channel >= static_cast<int>(core::kMidiChannelCount))
        {
            continue;
        }

        if (mapping.controller < 0 || mapping.controller >= static_cast<int>(core::kMidiCcCount))
        {
            continue;
        }

        const auto index = static_cast<std::size_t>(mapping.channel * core::kMidiCcCount + mapping.controller);
        if (index >= frame_.midi_cc_values.size())
        {
            continue;
        }

        float value = frame_.midi_cc_values[index];
        value = std::clamp(value, 0.0F, 1.0F);
        value = std::pow(value, mapping.exponent);
        value = mapping.minimum + (mapping.maximum - mapping.minimum) * value;
        program->setUniformValue(mapping.uniform.c_str(), value);
    }
}

GLuint ShaderVideoWindow::render_stage(RenderStage *stage, GLuint input_texture, bool input_valid, bool output_to_screen,
                                       float elapsed_seconds)
{
    if (stage == nullptr || stage->program == nullptr || !stage->program->isLinked())
    {
        return input_valid ? input_texture : blank_texture_id_;
    }

    ensure_blank_texture();
    const QColor clear_color = scene_clear_color(scene_.background_color);

    static constexpr GLfloat kVertices[] = {
        0.0F, 0.0F, 0.0F, 0.0F,
        1.0F, 0.0F, 1.0F, 0.0F,
        0.0F, 1.0F, 0.0F, 1.0F,
        1.0F, 1.0F, 1.0F, 1.0F,
    };

    GLuint output_texture = input_valid ? input_texture : blank_texture_id_;
    QOpenGLFramebufferObject *target_fbo = nullptr;
    if (!output_to_screen)
    {
        ensure_scene_fbos();
        const bool is_video_stage = stage->layer_name == QStringLiteral("video");
        const bool is_playback_stage = stage->layer_name == QStringLiteral("playback");
        if (is_video_stage)
        {
            target_fbo = (render_stage_index_ % 2 == 0) ? video_scene_fbo_ : video_scene_fbo_alt_;
        }
        else if (is_playback_stage)
        {
            target_fbo = (render_stage_index_ % 2 == 0) ? playback_scene_fbo_ : playback_scene_fbo_alt_;
        }
        else
        {
            target_fbo = (render_stage_index_ % 2 == 0) ? screen_scene_fbo_ : screen_scene_fbo_alt_;
        }
        if (target_fbo == nullptr)
        {
            return output_texture;
        }

        target_fbo->bind();
        glViewport(0, 0, target_fbo->width(), target_fbo->height());
        const QColor layer_clear_color = (is_video_stage || is_playback_stage) ? QColor{0, 0, 0, 0}
                                                                               : clear_color;
        glClearColor(layer_clear_color.redF(), layer_clear_color.greenF(), layer_clear_color.blueF(),
                     layer_clear_color.alphaF());
        glClear(GL_COLOR_BUFFER_BIT);
        output_texture = target_fbo->texture();
    }
    else
    {
        glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
        glViewport(0, 0, width(), height());
    }

    const GLuint sampled_texture = input_valid ? input_texture : blank_texture_id_;
    const QVector2D viewport_size{static_cast<float>(width()), static_cast<float>(height())};
    const QVector2D video_size{static_cast<float>(sampled_texture == texture_id_ && texture_width_ > 0 ? texture_width_ : width()),
                               static_cast<float>(sampled_texture == texture_id_ && texture_height_ > 0 ? texture_height_ : height())};

    stage->program->bind();
    stage->program->setUniformValue("u_viewport_size", viewport_size);
    stage->program->setUniformValue("u_video_size", video_size);
    stage->program->setUniformValue("u_status_bar_height", static_cast<float>(kStatusBarHeight));
    stage->program->setUniformValue("u_texture", 0);
    bind_stage_common_uniforms(stage->program.get(), *stage, elapsed_seconds);
    apply_scene_midi_mappings(stage->program.get(), *stage);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sampled_texture);
    if (note_label_atlas_texture_id_ != 0)
    {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, note_label_atlas_texture_id_);
        glActiveTexture(GL_TEXTURE0);
    }

    stage->program->enableAttributeArray("a_position");
    stage->program->enableAttributeArray("a_texcoord");
    stage->program->setAttributeArray("a_position", GL_FLOAT, kVertices, 2, 4 * sizeof(GLfloat));
    stage->program->setAttributeArray("a_texcoord", GL_FLOAT, kVertices + 2, 2, 4 * sizeof(GLfloat));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    stage->program->disableAttributeArray("a_position");
    stage->program->disableAttributeArray("a_texcoord");
    glBindTexture(GL_TEXTURE_2D, 0);
    if (note_label_atlas_texture_id_ != 0)
    {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
    }
    stage->program->release();

    if (target_fbo != nullptr)
    {
        target_fbo->release();
        output_texture = target_fbo->texture();
    }

    return output_texture;
}

void ShaderVideoWindow::build_render_stages()
{
    render_stages_.clear();
    video_shader_label_.clear();
    playback_shader_label_.clear();
    screen_shader_label_.clear();

    const auto add_layer = [&](const SceneLayer &layer, const QString &layer_name, QString *summary) {
        if (!layer.enabled)
        {
            *summary = QStringLiteral("<disabled>");
            return;
        }

        const auto vertex_shader_source = shader_source_for_current_context(
            QString::fromUtf8(kFullscreenVertexShader));

        QStringList labels;
        for (const auto &shader_path : layer.shaders)
        {
            RenderStage stage;
            stage.layer_name = layer_name;
            stage.shader_path = shader_path;
            stage.label = QString::fromStdString(std::filesystem::path{shader_path}.filename().string());
            if (stage.label.isEmpty())
            {
                stage.label = QStringLiteral("default");
            }

            stage.program = std::make_unique<QOpenGLShaderProgram>();
            const auto fragment_source = load_fragment_shader_source(shader_path, false);
            const auto effective_fragment = shader_source_for_current_context(
                fragment_source.isEmpty() ? QString::fromUtf8(kPassthroughFragmentShader) : fragment_source);
            if (!stage.program->addShaderFromSourceCode(QOpenGLShader::Vertex, vertex_shader_source) ||
                !stage.program->addShaderFromSourceCode(QOpenGLShader::Fragment, effective_fragment) ||
                !stage.program->link())
            {
                const auto log = stage.program->log();
                if (!log.isEmpty())
                {
                    if (status_message_.isEmpty())
                    {
                        status_message_ = log;
                    }
                    else
                    {
                        status_message_ += "\n" + log;
                    }
                }
            }

            labels.push_back(stage.label);
            render_stages_.push_back(std::move(stage));
        }

        *summary = labels.isEmpty() ? QStringLiteral("<none>") : labels.join(QStringLiteral(" > "));
    };

    add_layer(scene_.video_layer, QStringLiteral("video"), &video_shader_label_);
    add_layer(scene_.playback_layer, QStringLiteral("playback"), &playback_shader_label_);
    add_layer(scene_.screen_layer, QStringLiteral("screen"), &screen_shader_label_);

    if (render_stages_.empty())
    {
        video_shader_label_ = QStringLiteral("<none>");
        playback_shader_label_ = QStringLiteral("<none>");
        screen_shader_label_ = QStringLiteral("<none>");
    }
}

void ShaderVideoWindow::ensure_scene_fbos()
{
    const int target_width = width();
    const int target_height = height();
    if (!scene_fbo_dirty_ && video_scene_fbo_ != nullptr && video_scene_fbo_alt_ != nullptr &&
        playback_scene_fbo_ != nullptr && playback_scene_fbo_alt_ != nullptr && screen_scene_fbo_ != nullptr &&
        screen_scene_fbo_alt_ != nullptr && scene_fbo_width_ == target_width && scene_fbo_height_ == target_height)
    {
        return;
    }

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

    if (target_width <= 0 || target_height <= 0)
    {
        scene_fbo_dirty_ = true;
        return;
    }

    QOpenGLFramebufferObjectFormat format;
    format.setAttachment(QOpenGLFramebufferObject::NoAttachment);
    video_scene_fbo_ = new QOpenGLFramebufferObject(target_width, target_height, format);
    video_scene_fbo_alt_ = new QOpenGLFramebufferObject(target_width, target_height, format);
    playback_scene_fbo_ = new QOpenGLFramebufferObject(target_width, target_height, format);
    playback_scene_fbo_alt_ = new QOpenGLFramebufferObject(target_width, target_height, format);
    screen_scene_fbo_ = new QOpenGLFramebufferObject(target_width, target_height, format);
    screen_scene_fbo_alt_ = new QOpenGLFramebufferObject(target_width, target_height, format);
    scene_fbo_width_ = target_width;
    scene_fbo_height_ = target_height;
    scene_fbo_dirty_ = false;
}

void ShaderVideoWindow::upload_latest_frame()
{
    if (latest_frame_.isNull() || !texture_dirty_)
    {
        return;
    }

    ensure_texture();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_id_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    const QImage image = vertically_flipped_image(latest_frame_);
    if (texture_width_ != image.width() || texture_height_ != image.height())
    {
        texture_width_ = image.width();
        texture_height_ = image.height();
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texture_width_, texture_height_, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     image.constBits());
    }
    else
    {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, texture_width_, texture_height_, GL_RGBA, GL_UNSIGNED_BYTE,
                        image.constBits());
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    texture_dirty_ = false;
}

void ShaderVideoWindow::upload_latest_playback_frame()
{
    if (latest_playback_frame_.isNull() || !playback_texture_dirty_)
    {
        return;
    }

    ensure_playback_texture();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, playback_texture_id_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    const QImage image = vertically_flipped_image(latest_playback_frame_);
    if (playback_texture_width_ != image.width() || playback_texture_height_ != image.height())
    {
        playback_texture_width_ = image.width();
        playback_texture_height_ = image.height();
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, playback_texture_width_, playback_texture_height_, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, image.constBits());
    }
    else
    {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, playback_texture_width_, playback_texture_height_, GL_RGBA,
                        GL_UNSIGNED_BYTE, image.constBits());
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    playback_texture_dirty_ = false;
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

        const auto resolved_shader_path = resolve_relative_path(shader_path);
        if (!resolved_shader_path.has_value())
        {
            return {};
        }

        return read_text_file_qstring(*resolved_shader_path);
    }

    if (!allow_directory_scan)
    {
        return {};
    }

    const auto resolved_shader_directory = resolve_relative_path(std::filesystem::path{settings_.shader_directory});
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
            return read_text_file_qstring(entry.path());
        }
    }

    return {};
}

} // namespace cockscreen::runtime