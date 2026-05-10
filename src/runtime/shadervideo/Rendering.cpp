#include "cockscreen/runtime/ShaderVideoWindow.hpp"

#include "cockscreen/runtime/StatusOverlay.hpp"
#include "cockscreen/runtime/shadervideo/Support.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QColor>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFont>
#include <QFontDatabase>
#include <QOpenGLFramebufferObject>
#include <QPainter>
#include <QRectF>
#include <QVector2D>
#include <QVector3D>
#include <QVector4D>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>

namespace cockscreen::runtime
{

namespace helper = shader_window;

namespace
{

std::string strip_jsonc_comments(std::string_view input)
{
    std::string output;
    output.reserve(input.size());

    bool in_string = false;
    bool escaping = false;
    bool in_line_comment = false;
    bool in_block_comment = false;

    for (std::size_t index = 0; index < input.size(); ++index)
    {
        const char current = input[index];
        const char next = index + 1 < input.size() ? input[index + 1] : '\0';

        if (in_line_comment)
        {
            if (current == '\n')
            {
                in_line_comment = false;
                output.push_back(current);
            }
            continue;
        }

        if (in_block_comment)
        {
            if (current == '*' && next == '/')
            {
                in_block_comment = false;
                ++index;
            }
            else if (current == '\n')
            {
                output.push_back('\n');
            }
            continue;
        }

        if (!in_string && current == '/' && next == '/')
        {
            in_line_comment = true;
            ++index;
            continue;
        }

        if (!in_string && current == '/' && next == '*')
        {
            in_block_comment = true;
            ++index;
            continue;
        }

        output.push_back(current);

        if (!in_string)
        {
            if (current == '"')
            {
                in_string = true;
            }
            continue;
        }

        if (escaping)
        {
            escaping = false;
            continue;
        }

        if (current == '\\')
        {
            escaping = true;
        }
        else if (current == '"')
        {
            in_string = false;
        }
    }

    return output;
}

ShaderVideoWindow::RenderStage::ChannelSource input_channel_source()
{
    return {};
}

ShaderVideoWindow::RenderStage::ChannelSource previous_frame_channel_source()
{
    ShaderVideoWindow::RenderStage::ChannelSource source;
    source.kind = ShaderVideoWindow::RenderStage::ChannelSourceKind::PreviousFrame;
    return source;
}

ShaderVideoWindow::RenderStage::ChannelSource blank_channel_source()
{
    ShaderVideoWindow::RenderStage::ChannelSource source;
    source.kind = ShaderVideoWindow::RenderStage::ChannelSourceKind::BlankTexture;
    return source;
}

ShaderVideoWindow::RenderStage::ChannelSource buffer_channel_source(char name)
{
    ShaderVideoWindow::RenderStage::ChannelSource source;
    source.kind = ShaderVideoWindow::RenderStage::ChannelSourceKind::BufferOutput;
    source.buffer_name = name;
    return source;
}

ShaderVideoWindow::RenderStage::ChannelSource static_channel_source(int index)
{
    ShaderVideoWindow::RenderStage::ChannelSource source;
    source.kind = ShaderVideoWindow::RenderStage::ChannelSourceKind::StaticTexture;
    source.static_texture_index = index;
    return source;
}

std::array<ShaderVideoWindow::RenderStage::ChannelSource, 4> default_main_channel_sources()
{
    return {input_channel_source(), static_channel_source(0), static_channel_source(1), static_channel_source(2)};
}

std::array<ShaderVideoWindow::RenderStage::ChannelSource, 4> default_buffer_channel_sources()
{
    return {input_channel_source(), previous_frame_channel_source(), blank_channel_source(), blank_channel_source()};
}

std::optional<ShaderVideoWindow::RenderStage::ChannelSource> parse_channel_source_name(QString value)
{
    value = value.trimmed().toLower();
    if (value.isEmpty() || value == QStringLiteral("input") || value == QStringLiteral("video") ||
        value == QStringLiteral("source"))
    {
        return input_channel_source();
    }
    if (value == QStringLiteral("self") || value == QStringLiteral("previous") || value == QStringLiteral("feedback"))
    {
        return previous_frame_channel_source();
    }
    if (value == QStringLiteral("blank") || value == QStringLiteral("none"))
    {
        return blank_channel_source();
    }
    if (value == QStringLiteral("channel1"))
    {
        return static_channel_source(0);
    }
    if (value == QStringLiteral("channel2"))
    {
        return static_channel_source(1);
    }
    if (value == QStringLiteral("channel3"))
    {
        return static_channel_source(2);
    }
    if (value == QStringLiteral("buffera"))
    {
        return buffer_channel_source('A');
    }
    if (value == QStringLiteral("bufferb"))
    {
        return buffer_channel_source('B');
    }
    if (value == QStringLiteral("bufferc"))
    {
        return buffer_channel_source('C');
    }
    if (value == QStringLiteral("bufferd"))
    {
        return buffer_channel_source('D');
    }
    return std::nullopt;
}

bool load_channel_routes(const std::filesystem::path &resource_directory,
                         std::array<ShaderVideoWindow::RenderStage::ChannelSource, 4> *image_routes,
                         std::array<ShaderVideoWindow::RenderStage::ChannelSource, 4> *buffer_a_routes,
                         std::array<ShaderVideoWindow::RenderStage::ChannelSource, 4> *buffer_b_routes,
                         std::array<ShaderVideoWindow::RenderStage::ChannelSource, 4> *buffer_c_routes,
                         std::array<ShaderVideoWindow::RenderStage::ChannelSource, 4> *buffer_d_routes,
                         QString *error_message)
{
    if (image_routes == nullptr || buffer_a_routes == nullptr || buffer_b_routes == nullptr ||
        buffer_c_routes == nullptr || buffer_d_routes == nullptr)
    {
        return false;
    }

    const auto config_path = resource_directory / "channels.jsonc";
    if (!std::filesystem::is_regular_file(config_path))
    {
        return false;
    }

    const auto text = read_text_file(config_path);
    if (!text.has_value())
    {
        if (error_message != nullptr)
        {
            *error_message = QStringLiteral("Unable to read %1").arg(QString::fromStdString(config_path.string()));
        }
        return false;
    }

    QJsonParseError parse_error;
    const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(strip_jsonc_comments(*text)), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject())
    {
        if (error_message != nullptr)
        {
            *error_message = QStringLiteral("Invalid channels.jsonc in %1: %2")
                                 .arg(QString::fromStdString(config_path.string()), parse_error.errorString());
        }
        return false;
    }

    const auto parse_routes = [&](const QJsonObject &object, const QString &name,
                                  std::array<ShaderVideoWindow::RenderStage::ChannelSource, 4> *routes) -> bool {
        if (routes == nullptr || !object.contains(name) || !object.value(name).isObject())
        {
            return false;
        }

        bool changed = false;
        const auto channel_object = object.value(name).toObject();
        for (int index = 0; index < 4; ++index)
        {
            const QString key = QStringLiteral("iChannel%1").arg(index);
            const QString alias = QStringLiteral("channel%1").arg(index);
            const auto value = channel_object.contains(key) ? channel_object.value(key)
                              : channel_object.contains(alias) ? channel_object.value(alias)
                              : QJsonValue{};
            if (!value.isString())
            {
                continue;
            }
            const auto parsed = parse_channel_source_name(value.toString());
            if (!parsed.has_value())
            {
                if (error_message != nullptr)
                {
                    *error_message = QStringLiteral("Invalid %1.%2 source '%3' in %4")
                                         .arg(name, key, value.toString(), QString::fromStdString(config_path.string()));
                }
                return false;
            }
            (*routes)[index] = *parsed;
            changed = true;
        }
        return changed;
    };

    const auto object = document.object();
    bool any = false;
    any = parse_routes(object, QStringLiteral("image"), image_routes) || any;
    any = parse_routes(object, QStringLiteral("bufferA"), buffer_a_routes) || any;
    any = parse_routes(object, QStringLiteral("bufferB"), buffer_b_routes) || any;
    any = parse_routes(object, QStringLiteral("bufferC"), buffer_c_routes) || any;
    any = parse_routes(object, QStringLiteral("bufferD"), buffer_d_routes) || any;
    return any;
}

const ShaderVideoWindow::RenderStage::ShaderBuffer *find_shader_buffer(const ShaderVideoWindow::RenderStage &stage,
                                                                       char name)
{
    for (const auto &buffer : stage.shader_buffers)
    {
        if (buffer.name == name)
        {
            return &buffer;
        }
    }
    return nullptr;
}

GLuint resolve_channel_texture(const ShaderVideoWindow::RenderStage &stage,
                               const ShaderVideoWindow::RenderStage::ChannelSource &source,
                               GLuint input_texture, GLuint previous_frame_texture, GLuint blank_texture_id)
{
    switch (source.kind)
    {
        case ShaderVideoWindow::RenderStage::ChannelSourceKind::InputTexture:
            return input_texture != 0 ? input_texture : blank_texture_id;
        case ShaderVideoWindow::RenderStage::ChannelSourceKind::PreviousFrame:
            return previous_frame_texture != 0 ? previous_frame_texture : blank_texture_id;
        case ShaderVideoWindow::RenderStage::ChannelSourceKind::BufferOutput:
        {
            const auto *buffer = find_shader_buffer(stage, source.buffer_name);
            if (buffer == nullptr || buffer->fbo[buffer->ping] == nullptr)
            {
                return blank_texture_id;
            }
            return buffer->fbo[buffer->ping]->texture();
        }
        case ShaderVideoWindow::RenderStage::ChannelSourceKind::StaticTexture:
            if (source.static_texture_index < 0 ||
                source.static_texture_index >= static_cast<int>(stage.channel_textures.size()))
            {
                return blank_texture_id;
            }
            return stage.channel_textures[static_cast<std::size_t>(source.static_texture_index)] != 0
                       ? stage.channel_textures[static_cast<std::size_t>(source.static_texture_index)]
                       : blank_texture_id;
        case ShaderVideoWindow::RenderStage::ChannelSourceKind::BlankTexture:
            return blank_texture_id;
    }

    return blank_texture_id;
}

bool shader_mapping_matches(const std::string &mapping_shader, const std::string &stage_shader)
{
    if (mapping_shader.empty())
    {
        return true;
    }

    if (mapping_shader == stage_shader)
    {
        return true;
    }

    return std::filesystem::path{mapping_shader}.filename() == std::filesystem::path{stage_shader}.filename();
}

float mapped_note_value(const core::ControlFrame &frame, const MidiNoteMapping &mapping)
{
    float value = 0.0F;
    for (std::size_t index = 0; index < frame.midi_notes.size(); ++index)
    {
        if (index >= frame.midi_velocities.size() || index >= frame.midi_ages.size() || index >= frame.midi_channels.size())
        {
            break;
        }

        if (frame.midi_ages[index] < 0.0F)
        {
            continue;
        }

        const int event_channel = static_cast<int>(std::lround(frame.midi_channels[index]));
        if (mapping.channel >= 0 && event_channel != mapping.channel)
        {
            continue;
        }

        const int event_note = static_cast<int>(std::lround(frame.midi_notes[index]));
        if (mapping.note >= 0 && event_note != mapping.note)
        {
            continue;
        }

        value = std::max(value, std::clamp(frame.midi_velocities[index], 0.0F, 1.0F));
    }

    value = std::pow(value, mapping.exponent);
    return mapping.minimum + (mapping.maximum - mapping.minimum) * value;
}

QString format_timecode_value(std::int64_t position_ms)
{
    constexpr std::int64_t kFramesPerSecond{25};
    const std::int64_t clamped_ms = std::max<std::int64_t>(0, position_ms);
    const std::int64_t total_seconds = clamped_ms / 1000;
    const std::int64_t frames = ((clamped_ms % 1000) * kFramesPerSecond) / 1000;
    const std::int64_t hours = total_seconds / 3600;
    const std::int64_t minutes = (total_seconds / 60) % 60;
    const std::int64_t seconds = total_seconds % 60;
    return QStringLiteral("%1:%2:%3:%4")
        .arg(hours, 2, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'))
        .arg(frames, 2, 10, QLatin1Char('0'));
}

void draw_timecode_overlay(QPainter *painter, const QRect &viewport_rect, std::int64_t position_ms)
{
    if (painter == nullptr)
    {
        return;
    }

    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setBold(true);
    font.setPointSizeF(std::max(12.0, viewport_rect.height() * 0.026));
    font.setLetterSpacing(QFont::AbsoluteSpacing, 1.2);
    painter->setFont(font);

    const QString label = QStringLiteral("TC %1  MS %2")
                              .arg(format_timecode_value(position_ms))
                              .arg(std::max<std::int64_t>(0, position_ms));
    const QFontMetrics metrics{font};
    const QRect text_rect = metrics.boundingRect(label).adjusted(-16, -10, 16, 10);
    const QRect padded_rect = viewport_rect.adjusted(24, 24, -24, -24);
    const QRect target{padded_rect.right() - text_rect.width(), padded_rect.bottom() - text_rect.height(),
                       text_rect.width(), text_rect.height()};

    painter->setRenderHint(QPainter::TextAntialiasing, false);
    painter->fillRect(target, QColor{38, 28, 16, 170});
    painter->setPen(QColor{36, 24, 12, 220});
    painter->drawText(target.translated(2, 2), Qt::AlignCenter, label);
    painter->setPen(QColor{255, 214, 117, 235});
    painter->drawText(target, Qt::AlignCenter, label);
}

std::vector<QString> effective_layer_order(const SceneDefinition &scene, bool video_on_top)
{
    if (scene.layer_order.size() == 3)
    {
        std::vector<QString> order;
        order.reserve(scene.layer_order.size());
        for (const auto &layer_name : scene.layer_order)
        {
            order.push_back(QString::fromStdString(layer_name));
        }
        return order;
    }

    if (video_on_top)
    {
        return {QStringLiteral("video"), QStringLiteral("playback"), QStringLiteral("screen")};
    }

    return {QStringLiteral("screen"), QStringLiteral("playback"), QStringLiteral("video")};
}

} // namespace

void ShaderVideoWindow::paintGL()
{
    QElapsedTimer render_timer;
    render_timer.start();

    if (!fatal_render_error_.isEmpty())
    {
        glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT);

        render_fps_ = render_timer.nsecsElapsed() > 0 ? 1.0e9 / static_cast<double>(render_timer.nsecsElapsed())
                                                      : render_fps_;
        return;
    }

    const QColor clear_color = helper::scene_clear_color(scene_.background_color);

    glClearColor(clear_color.redF(), clear_color.greenF(), clear_color.blueF(), clear_color.alphaF());
    glClear(GL_COLOR_BUFFER_BIT);

    upload_latest_frame();
    upload_latest_playback_frame();
    ensure_scene_fbos();
    ensure_blank_texture();
    ensure_background_texture();
    ensure_background_image_texture();

    const auto now = std::chrono::steady_clock::now();
    const float elapsed_seconds = std::chrono::duration<float>(now - start_time_).count();
    const float frame_delta_seconds = last_frame_time_ == std::chrono::steady_clock::time_point{}
                                          ? 0.0F
                                          : std::chrono::duration<float>(now - last_frame_time_).count();
    const int frame_index = render_frame_index_;
    const GLfloat top_layer_opacity = static_cast<GLfloat>(std::clamp(settings_.top_layer_opacity, 0.0, 1.0));
    const GLuint camera_texture = texture_id_ != 0 ? texture_id_ : blank_texture_id_;
    const bool camera_valid = texture_id_ != 0;
    const bool playback_requested = scene_.playback_input.enabled && !scene_.playback_input.file.empty();
    const GLuint playback_texture = playback_texture_id_ != 0 ? playback_texture_id_ : blank_texture_id_;
    const bool playback_valid = playback_requested && playback_texture_id_ != 0 && !latest_playback_frame_.isNull();
    const QRectF video_rect = helper::video_display_rect(scene_.video_input, QSize{width(), height()});
    const QRectF playback_rect = helper::video_display_rect(scene_.playback_input, QSize{width(), height()});
    const QRectF full_rect{0.0, 0.0, static_cast<qreal>(width()), static_cast<qreal>(height())};

    auto draw_textured_quad = [&](GLuint texture, const QRectF &rect, const QRectF &uv_rect, GLfloat opacity) {
        if (texture == 0 || !blit_program_.isLinked())
        {
            return;
        }

        const GLfloat left = static_cast<GLfloat>(rect.left() / std::max(static_cast<float>(width()), 1.0F));
        const GLfloat right = static_cast<GLfloat>(rect.right() / std::max(static_cast<float>(width()), 1.0F));
        const GLfloat top = static_cast<GLfloat>(rect.top() / std::max(static_cast<float>(height()), 1.0F));
        const GLfloat bottom = static_cast<GLfloat>(rect.bottom() / std::max(static_cast<float>(height()), 1.0F));

        const GLfloat vertices[] = {
            left, top, static_cast<GLfloat>(uv_rect.left()), static_cast<GLfloat>(uv_rect.top()),
            right, top, static_cast<GLfloat>(uv_rect.right()), static_cast<GLfloat>(uv_rect.top()),
            left, bottom, static_cast<GLfloat>(uv_rect.left()), static_cast<GLfloat>(uv_rect.bottom()),
            right, bottom, static_cast<GLfloat>(uv_rect.right()), static_cast<GLfloat>(uv_rect.bottom()),
        };

        blit_program_.bind();
        blit_program_.setUniformValue("u_texture", 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
        quad_vertex_buffer_.bind();
        quad_vertex_buffer_.allocate(vertices, static_cast<int>(sizeof(vertices)));
        blit_program_.enableAttributeArray("a_position");
        blit_program_.enableAttributeArray("a_texcoord");
        blit_program_.setAttributeBuffer("a_position", GL_FLOAT, 0, 2, 4 * sizeof(GLfloat));
        blit_program_.setAttributeBuffer("a_texcoord", GL_FLOAT, 2 * sizeof(GLfloat), 2, 4 * sizeof(GLfloat));

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        blit_program_.setUniformValue("u_opacity", opacity);

        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        blit_program_.disableAttributeArray("a_position");
        blit_program_.disableAttributeArray("a_texcoord");
        quad_vertex_buffer_.release();
        glBindTexture(GL_TEXTURE_2D, 0);
        glDisable(GL_BLEND);
        blit_program_.release();
    };

    if (background_image_texture_id_ != 0 && background_image_texture_width_ > 0 &&
        background_image_texture_height_ > 0)
    {
        QRectF background_rect = full_rect;
        QRectF background_uv{0.0, 0.0, 1.0, 1.0};

        switch (scene_.background_image.placement)
        {
            case BackgroundImagePlacement::Center:
                background_rect = QRectF{(static_cast<qreal>(width()) - background_image_texture_width_) * 0.5,
                                         (static_cast<qreal>(height()) - background_image_texture_height_) * 0.5,
                                         static_cast<qreal>(background_image_texture_width_),
                                         static_cast<qreal>(background_image_texture_height_)};
                break;
            case BackgroundImagePlacement::Stretched:
                background_rect = full_rect;
                break;
            case BackgroundImagePlacement::ProportionalStretch:
            {
                const float scale_x = static_cast<float>(width()) / std::max(background_image_texture_width_, 1);
                const float scale_y = static_cast<float>(height()) / std::max(background_image_texture_height_, 1);
                const float scale = std::min(scale_x, scale_y);
                const qreal scaled_width = static_cast<qreal>(background_image_texture_width_) * scale;
                const qreal scaled_height = static_cast<qreal>(background_image_texture_height_) * scale;
                background_rect = QRectF{(static_cast<qreal>(width()) - scaled_width) * 0.5,
                                         (static_cast<qreal>(height()) - scaled_height) * 0.5, scaled_width,
                                         scaled_height};
                break;
            }
            case BackgroundImagePlacement::Tiled:
                background_rect = full_rect;
                background_uv = QRectF{0.0, 0.0,
                                       static_cast<qreal>(width()) /
                                           std::max(static_cast<qreal>(background_image_texture_width_), 1.0),
                                       static_cast<qreal>(height()) /
                                           std::max(static_cast<qreal>(background_image_texture_height_), 1.0)};
                break;
        }

        draw_textured_quad(background_image_texture_id_, background_rect, background_uv, 1.0F);
    }

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

            current_texture = render_stage(&stage, current_texture, current_valid, false, elapsed_seconds,
                                           frame_delta_seconds, frame_index);
            current_valid = true;
            ++render_stage_index_;
        }
        return current_valid ? current_texture : 0;
    };

    const GLuint video_output = render_layer_chain(QStringLiteral("video"), camera_texture, camera_valid);
    const GLuint playback_output = render_layer_chain(QStringLiteral("playback"), playback_texture, playback_valid);
    const GLuint screen_base_texture = background_image_texture_id_ != 0 ? background_image_texture_id_ : background_texture_id_;
    const bool screen_base_valid = screen_base_texture != 0;
    const GLuint screen_output = render_layer_chain(QStringLiteral("screen"), screen_base_texture, screen_base_valid);

    const auto layer_order = effective_layer_order(scene_, video_on_top_);
    for (const auto &layer_name : layer_order)
    {
        if (layer_name == QStringLiteral("video"))
        {
            draw_textured_quad(video_output, video_rect, QRectF{0.0, 0.0, 1.0, 1.0}, top_layer_opacity);
            continue;
        }

        if (layer_name == QStringLiteral("playback"))
        {
            draw_textured_quad(playback_output, playback_rect, QRectF{0.0, 0.0, 1.0, 1.0}, 1.0F);
            continue;
        }

        if (layer_name == QStringLiteral("screen"))
        {
            draw_textured_quad(screen_output, full_rect, QRectF{0.0, 0.0, 1.0, 1.0}, 1.0F);
        }
    }

    if (status_overlay_ != nullptr)
    {
        status_overlay_->set_status_overlay_text(status_overlay_text_);
        status_overlay_->raise();
    }

    if (scene_.timecode && scene_.playback_input.enabled && !scene_.playback_input.file.empty())
    {
        QPainter painter{this};
        draw_timecode_overlay(&painter, rect(), playback_position_ms_);
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
    ++render_frame_index_;

    render_fps_ = render_timer.nsecsElapsed() > 0 ? 1.0e9 / static_cast<double>(render_timer.nsecsElapsed()) : render_fps_;
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
    program->setUniformValue("u_resolution", QVector2D{static_cast<float>(width()), static_cast<float>(height())});

    if (scene_.audio_playback_input.enabled && scene_.audio_playback_input.fft_analysis_from_playback)
    {
        // Build a scratch frame with the playback-derived audio data so that
        // u_audio_fft / u_audio_rms / u_audio_peak etc. reflect the audio file
        // rather than the external capture device.
        core::ControlFrame playback_frame = frame_;
        playback_frame.audio_rms = audio_playback_analysis_rms_;
        playback_frame.audio_peak = audio_playback_analysis_peak_;
        playback_frame.audio_level = audio_playback_analysis_rms_;
        playback_frame.audio_fft_bands = audio_playback_fft_bands_;
        playback_frame.audio_waveform = audio_playback_waveform_;
        helper::set_audio_uniforms(program, playback_frame);
    }
    else if (!scene_.audio_input.fft_analysis_enabled)
    {
        // Device audio is explicitly excluded from the FFT uniforms.
        core::ControlFrame silent_frame = frame_;
        silent_frame.audio_fft_bands.fill(0.0F);
        silent_frame.audio_rms = 0.0F;
        silent_frame.audio_peak = 0.0F;
        silent_frame.audio_level = 0.0F;
        helper::set_audio_uniforms(program, silent_frame);
    }
    else
    {
        helper::set_audio_uniforms(program, frame_);
    }
    if (note_label_atlas_texture_id_ != 0)
    {
        program->setUniformValue("u_note_label_atlas", 1);
        program->setUniformValue("u_note_label_grid",
                                 QVector2D{static_cast<float>(helper::kNoteAtlasColumns),
                                           static_cast<float>(helper::kNoteAtlasRows)});
    }
    if (icon_atlas_texture_id_ != 0)
    {
        program->setUniformValue("u_icon_atlas", 2);
        program->setUniformValue("u_icon_grid",
                                 QVector2D{static_cast<float>(helper::kIconAtlasColumns),
                                           static_cast<float>(helper::kIconAtlasRows)});
    }
    helper::set_midi_uniforms(program, frame_);

    if (shader_mapping_matches("pink_key.glsl", stage.shader_path))
    {
        program->setUniformValue("u_audio_algorithm", scene_.pink_key.audio_algorithm);
        program->setUniformValue("u_audio_reactivity", scene_.pink_key.audio_reactivity);
        program->setUniformValue("u_midi_reactivity", scene_.pink_key.midi_reactivity);
    }
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

        if (!shader_mapping_matches(mapping.shader, stage.shader_path))
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

    for (const auto &mapping : scene_.midi_note_mappings)
    {
        if (mapping.layer != stage.layer_name.toStdString())
        {
            continue;
        }

        if (!shader_mapping_matches(mapping.shader, stage.shader_path))
        {
            continue;
        }

        program->setUniformValue(mapping.uniform.c_str(), mapped_note_value(frame_, mapping));
    }
}

void ShaderVideoWindow::apply_scene_osc_mappings(QOpenGLShaderProgram *program, const RenderStage &stage) const
{
    if (program == nullptr || !program->isLinked())
    {
        return;
    }

    for (const auto &mapping : scene_.osc_mappings)
    {
        if (mapping.layer != stage.layer_name.toStdString())
        {
            continue;
        }

        if (!shader_mapping_matches(mapping.shader, stage.shader_path))
        {
            continue;
        }

        const auto it = frame_.osc_values.find(mapping.address);
        if (it == frame_.osc_values.end())
        {
            continue;
        }

        float value = std::clamp(it->second, 0.0F, 1.0F);
        value = std::pow(value, mapping.exponent);
        value = mapping.minimum + (mapping.maximum - mapping.minimum) * value;
        program->setUniformValue(mapping.uniform.c_str(), value);
    }
}

void ShaderVideoWindow::bind_shadertoy_uniforms(QOpenGLShaderProgram *program, float elapsed_seconds,
                                                float frame_delta_seconds, int frame_index,
                                                const QVector2D &channel0_resolution) const
{
    if (program == nullptr)
    {
        return;
    }

    const float frame_rate = frame_delta_seconds > 0.0F ? 1.0F / frame_delta_seconds : 0.0F;
    const auto local_time = QDateTime::currentDateTime();
    const auto date = local_time.date();
    const auto time = local_time.time();
    const float seconds_since_midnight = static_cast<float>(time.hour() * 3600 + time.minute() * 60 + time.second()) +
                                         static_cast<float>(time.msec()) / 1000.0F;
    const QVector3D channel_resolutions[4] = {
        QVector3D{channel0_resolution.x(), channel0_resolution.y(), 1.0F},
        QVector3D{},
        QVector3D{},
        QVector3D{},
    };
    const GLfloat channel_times[4] = {elapsed_seconds, 0.0F, 0.0F, 0.0F};

    program->setUniformValue("iTime", elapsed_seconds);
    program->setUniformValue("iTimeDelta", frame_delta_seconds);
    program->setUniformValue("iFrameRate", frame_rate);
    program->setUniformValue("iSampleRate", 44100.0F);
    program->setUniformValue("iFrame", frame_index);
    program->setUniformValue("iResolution", QVector3D{static_cast<float>(width()), static_cast<float>(height()), 1.0F});
    program->setUniformValue("iMouse", QVector4D{});
    program->setUniformValue(
        "iDate", QVector4D{static_cast<float>(date.year()), static_cast<float>(date.month()),
                             static_cast<float>(date.day()), seconds_since_midnight});
    program->setUniformValue("iChannel0", 0);
    program->setUniformValue("iChannel1", 3);
    program->setUniformValue("iChannel2", 4);
    program->setUniformValue("iChannel3", 5);
    program->setUniformValueArray("iChannelResolution", channel_resolutions, 4);
    program->setUniformValueArray("iChannelTime", channel_times, 4, 1);
}

GLuint ShaderVideoWindow::render_stage(RenderStage *stage, GLuint input_texture, bool input_valid, bool output_to_screen,
                                       float elapsed_seconds, float frame_delta_seconds, int frame_index)
{
    if (stage == nullptr || stage->program == nullptr || !stage->program->isLinked())
    {
        return input_valid ? input_texture : blank_texture_id_;
    }

    // Render multi-pass buffers (Buffer A/B/C/D) before the main stage so their
    // outputs are ready to bind as iChannel1-3.
    render_shader_buffers(*stage, input_valid ? input_texture : blank_texture_id_,
                          elapsed_seconds, frame_delta_seconds, frame_index);

    ensure_blank_texture();
    const QColor clear_color = helper::scene_clear_color(scene_.background_color);

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
        const QColor layer_clear_color = (is_video_stage || is_playback_stage) ? QColor{0, 0, 0, 0} : clear_color;
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
    auto buffer_tex_for = [&](char name) -> GLuint {
        const auto *buffer = find_shader_buffer(*stage, name);
        if (buffer == nullptr || buffer->fbo[buffer->ping] == nullptr)
        {
            return 0;
        }
        return buffer->fbo[buffer->ping]->texture();
    };
    const GLuint main_channel0 = stage->has_custom_channel_sources
                                     ? resolve_channel_texture(*stage, stage->channel_sources[0], sampled_texture, 0,
                                                               blank_texture_id_)
                                     : sampled_texture;
    const QVector2D viewport_size{static_cast<float>(width()), static_cast<float>(height())};
    const QVector2D video_size{
        static_cast<float>(main_channel0 == texture_id_ && texture_width_ > 0 ? texture_width_ : width()),
        static_cast<float>(main_channel0 == texture_id_ && texture_height_ > 0 ? texture_height_ : height())};

    stage->program->bind();
    stage->program->setUniformValue("u_viewport_size", viewport_size);
    stage->program->setUniformValue("u_video_size", video_size);
    stage->program->setUniformValue("u_status_bar_height", static_cast<float>(kStatusBarHeight));
    stage->program->setUniformValue("u_texture", 0);
    bind_stage_common_uniforms(stage->program.get(), *stage, elapsed_seconds);
    bind_shadertoy_uniforms(stage->program.get(), elapsed_seconds, frame_delta_seconds, frame_index, video_size);
    apply_scene_midi_mappings(stage->program.get(), *stage);
    apply_scene_osc_mappings(stage->program.get(), *stage);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, main_channel0);
    if (note_label_atlas_texture_id_ != 0)
    {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, note_label_atlas_texture_id_);
        glActiveTexture(GL_TEXTURE0);
    }
    if (icon_atlas_texture_id_ != 0)
    {
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, icon_atlas_texture_id_);
        glActiveTexture(GL_TEXTURE0);
    }
    // Bind iChannel1-3 (GL_TEXTURE3-5).
    // Priority: shader buffer output > static channel texture > blank.
    // Buffer A → iChannel1, B → iChannel2, C → iChannel3, D → iChannel4 (if ever added).
    const GLuint ch1 = stage->has_custom_channel_sources
                           ? resolve_channel_texture(*stage, stage->channel_sources[1], sampled_texture, 0,
                                                     blank_texture_id_)
                           : (buffer_tex_for('A') != 0 ? buffer_tex_for('A')
                                                       : (stage->channel_textures[0] != 0 ? stage->channel_textures[0]
                                                                                          : blank_texture_id_));
    const GLuint ch2 = stage->has_custom_channel_sources
                           ? resolve_channel_texture(*stage, stage->channel_sources[2], sampled_texture, 0,
                                                     blank_texture_id_)
                           : (buffer_tex_for('B') != 0 ? buffer_tex_for('B')
                                                       : (stage->channel_textures[1] != 0 ? stage->channel_textures[1]
                                                                                          : blank_texture_id_));
    const GLuint ch3 = stage->has_custom_channel_sources
                           ? resolve_channel_texture(*stage, stage->channel_sources[3], sampled_texture, 0,
                                                     blank_texture_id_)
                           : (buffer_tex_for('C') != 0 ? buffer_tex_for('C')
                                                       : (stage->channel_textures[2] != 0 ? stage->channel_textures[2]
                                                                                          : blank_texture_id_));
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, ch1);
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, ch2);
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D, ch3);
    glActiveTexture(GL_TEXTURE0);
    quad_vertex_buffer_.bind();
    quad_vertex_buffer_.allocate(kVertices, static_cast<int>(sizeof(kVertices)));
    stage->program->enableAttributeArray("a_position");
    stage->program->enableAttributeArray("a_texcoord");
    stage->program->setAttributeBuffer("a_position", GL_FLOAT, 0, 2, 4 * sizeof(GLfloat));
    stage->program->setAttributeBuffer("a_texcoord", GL_FLOAT, 2 * sizeof(GLfloat), 2, 4 * sizeof(GLfloat));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    stage->program->disableAttributeArray("a_position");
    stage->program->disableAttributeArray("a_texcoord");
    quad_vertex_buffer_.release();
    glBindTexture(GL_TEXTURE_2D, 0);
    if (note_label_atlas_texture_id_ != 0)
    {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
    }
    if (icon_atlas_texture_id_ != 0)
    {
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
    }
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
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
    // Release any per-stage channel textures and shader buffers from the previous build.
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

        const auto vertex_shader_source =
            helper::shader_source_for_current_context(QString::fromUtf8(helper::fullscreen_vertex_shader_source()));

        QStringList labels;
        for (const auto &shader_path : layer.shaders)
        {
            RenderStage stage;
            stage.layer_name = layer_name;
            stage.shader_path = shader_path;
            stage.channel_sources = default_main_channel_sources();
            stage.label = QString::fromStdString(std::filesystem::path{shader_path}.filename().string());
            if (stage.label.isEmpty())
            {
                stage.label = QStringLiteral("default");
            }

            stage.program = std::make_unique<QOpenGLShaderProgram>();
            const auto fragment_source_raw = load_fragment_shader_source(shader_path, false, &stage.resource_directory);

            // Load common.glsl if present — it gets prepended to the main shader
            // and all buffer passes (equivalent to ShaderToy's Common tab).
            QString common_source;
            if (!stage.resource_directory.empty())
            {
                const auto common_path = stage.resource_directory / "common.glsl";
                if (std::filesystem::is_regular_file(common_path))
                {
                    common_source = helper::read_text_file_qstring(common_path);
                }
            }
            const QString fragment_source = common_source.isEmpty()
                                                ? fragment_source_raw
                                                : common_source + QStringLiteral("\n") + fragment_source_raw;

            // Load per-shader channel textures (iChannel1-3) from the resource directory.
            // Looks for channel1.{png,jpg,bmp,ppm}, channel2.{...}, channel3.{...}.
            if (!stage.resource_directory.empty())
            {
                static constexpr std::array<const char *, 3> kChannelNames{"channel1", "channel2", "channel3"};
                static constexpr std::array<const char *, 4> kImageExts{".png", ".jpg", ".bmp", ".ppm"};
                for (std::size_t ch = 0; ch < kChannelNames.size(); ++ch)
                {
                    for (const auto *ext : kImageExts)
                    {
                        const auto candidate = stage.resource_directory / (std::string{kChannelNames[ch]} + ext);
                        if (!std::filesystem::is_regular_file(candidate))
                        {
                            continue;
                        }
                        QImage img(QString::fromStdString(candidate.string()));
                        if (img.isNull())
                        {
                            break;
                        }
                        const QImage rgba = img.convertToFormat(QImage::Format_RGBA8888).mirrored(false, true);
                        GLuint tex_id = 0;
                        glGenTextures(1, &tex_id);
                        glBindTexture(GL_TEXTURE_2D, tex_id);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
                        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rgba.width(), rgba.height(), 0, GL_RGBA,
                                     GL_UNSIGNED_BYTE, rgba.constBits());
                        glBindTexture(GL_TEXTURE_2D, 0);
                        stage.channel_textures[ch] = tex_id;
                        break; // found a valid image for this channel
                    }
                }
            }
            // Scan for multi-pass buffer shaders (bufferA.glsl … bufferD.glsl).
            // These are compiled into ShaderBuffer entries whose FBO outputs feed
            // iChannel1–iChannel3+extra of the main shader each frame.
            if (!stage.resource_directory.empty())
            {
                std::array<RenderStage::ChannelSource, 4> buffer_a_routes = default_buffer_channel_sources();
                std::array<RenderStage::ChannelSource, 4> buffer_b_routes = default_buffer_channel_sources();
                std::array<RenderStage::ChannelSource, 4> buffer_c_routes = default_buffer_channel_sources();
                std::array<RenderStage::ChannelSource, 4> buffer_d_routes = default_buffer_channel_sources();
                QString channels_error;
                stage.has_custom_channel_sources = load_channel_routes(stage.resource_directory, &stage.channel_sources,
                                                                       &buffer_a_routes, &buffer_b_routes,
                                                                       &buffer_c_routes, &buffer_d_routes,
                                                                       &channels_error);
                if (!channels_error.isEmpty())
                {
                    record_fatal_render_error(channels_error);
                }

                static constexpr std::array<char, 4> kBufferNames{'A', 'B', 'C', 'D'};
                for (char name : kBufferNames)
                {
                    const std::string buf_filename = std::string{"buffer"} + name + ".glsl";
                    const auto buf_path = stage.resource_directory / buf_filename;
                    if (!std::filesystem::is_regular_file(buf_path))
                    {
                        continue;
                    }
                    const auto buf_src_raw = helper::read_text_file_qstring(buf_path);
                    if (buf_src_raw.isEmpty())
                    {
                        continue;
                    }
                    // Prepend common.glsl if available.
                    const QString buf_src = common_source.isEmpty()
                                               ? buf_src_raw
                                               : common_source + QStringLiteral("\n") + buf_src_raw;
                    const auto buf_adapted = helper::adapt_fragment_shader_source(buf_src);
                    const auto buf_fragment = helper::shader_source_for_current_context(
                        buf_adapted.isEmpty()
                            ? QString::fromUtf8(helper::passthrough_fragment_shader_source())
                            : buf_adapted);

                    RenderStage::ShaderBuffer buf;
                    buf.name = name;
                    buf.channel_sources = default_buffer_channel_sources();
                    switch (name)
                    {
                        case 'A':
                            buf.channel_sources = buffer_a_routes;
                            buf.has_custom_channel_sources = stage.has_custom_channel_sources;
                            break;
                        case 'B':
                            buf.channel_sources = buffer_b_routes;
                            buf.has_custom_channel_sources = stage.has_custom_channel_sources;
                            break;
                        case 'C':
                            buf.channel_sources = buffer_c_routes;
                            buf.has_custom_channel_sources = stage.has_custom_channel_sources;
                            break;
                        case 'D':
                            buf.channel_sources = buffer_d_routes;
                            buf.has_custom_channel_sources = stage.has_custom_channel_sources;
                            break;
                    }
                    buf.program = std::make_unique<QOpenGLShaderProgram>();
                    if (!buf.program->addShaderFromSourceCode(QOpenGLShader::Vertex, vertex_shader_source) ||
                        !buf.program->addShaderFromSourceCode(QOpenGLShader::Fragment, buf_fragment) ||
                        !buf.program->link())
                    {
                        const auto log = buf.program->log();
                        record_fatal_render_error(
                            QStringLiteral("Buffer%1 shader failed for '%2':\n%3")
                                .arg(name)
                                .arg(stage.label)
                                .arg(log.isEmpty() ? QStringLiteral("<no shader log>") : log));
                    }
                    // FBOs are sized lazily in ensure_shader_buffer_fbos().
                    stage.shader_buffers.push_back(std::move(buf));
                }
            }

            const auto unsupported_reason = helper::shadertoy_unsupported_reason(fragment_source);
            if (!unsupported_reason.isEmpty())
            {
                record_fatal_render_error(
                    QStringLiteral("ShaderToy import failed for layer '%1' shader '%2':\n- %3")
                        .arg(layer_name, stage.label, unsupported_reason));
                labels.push_back(stage.label);
                render_stages_.push_back(std::move(stage));
                continue;
            }
            const auto runtime_fragment = helper::adapt_fragment_shader_source(fragment_source);
            const auto effective_fragment = helper::shader_source_for_current_context(
                runtime_fragment.isEmpty() ? QString::fromUtf8(helper::passthrough_fragment_shader_source())
                                          : runtime_fragment);
            if (!stage.program->addShaderFromSourceCode(QOpenGLShader::Vertex, vertex_shader_source) ||
                !stage.program->addShaderFromSourceCode(QOpenGLShader::Fragment, effective_fragment) ||
                !stage.program->link())
            {
                const auto log = stage.program->log();
                record_fatal_render_error(
                    QStringLiteral("Shader stage initialization failed for layer '%1' shader '%2':\n%3")
                        .arg(layer_name, stage.label, log.isEmpty() ? QStringLiteral("<no shader log>") : log));
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

void ShaderVideoWindow::ensure_shader_buffer_fbos(RenderStage &stage)
{
    if (stage.shader_buffers.empty() || width() <= 0 || height() <= 0)
    {
        return;
    }
    QOpenGLFramebufferObjectFormat fmt;
    fmt.setAttachment(QOpenGLFramebufferObject::NoAttachment);
    for (auto &buf : stage.shader_buffers)
    {
        for (int i = 0; i < 2; ++i)
        {
            if (buf.fbo[i] == nullptr || buf.fbo[i]->width() != width() || buf.fbo[i]->height() != height())
            {
                delete buf.fbo[i];
                buf.fbo[i] = new QOpenGLFramebufferObject(width(), height(), fmt);
            }
        }
    }
}

void ShaderVideoWindow::render_shader_buffers(RenderStage &stage, GLuint video_texture,
                                              float elapsed_seconds, float frame_delta_seconds,
                                              int frame_index)
{
    if (stage.shader_buffers.empty())
    {
        return;
    }
    ensure_shader_buffer_fbos(stage);
    ensure_blank_texture();

    const auto vertex_shader_source_str =
        helper::shader_source_for_current_context(QString::fromUtf8(helper::fullscreen_vertex_shader_source()));
    Q_UNUSED(vertex_shader_source_str);

    static constexpr GLfloat kVertices[] = {
        0.0F, 0.0F, 0.0F, 0.0F,
        1.0F, 0.0F, 1.0F, 0.0F,
        0.0F, 1.0F, 0.0F, 1.0F,
        1.0F, 1.0F, 1.0F, 1.0F,
    };

    const QVector2D viewport_size{static_cast<float>(width()), static_cast<float>(height())};
    const QVector2D video_size{
        static_cast<float>(video_texture == texture_id_ && texture_width_ > 0 ? texture_width_ : width()),
        static_cast<float>(video_texture == texture_id_ && texture_height_ > 0 ? texture_height_ : height())};

    for (auto &buf : stage.shader_buffers)
    {
        if (buf.fbo[0] == nullptr || buf.fbo[1] == nullptr)
        {
            continue;
        }
        if (!buf.program || !buf.program->isLinked())
        {
            continue;
        }

        const int write_idx = buf.ping;
        const int read_idx  = 1 - buf.ping;
        QOpenGLFramebufferObject *target = buf.fbo[write_idx];
        const GLuint self_prev_tex = buf.fbo[read_idx]->texture();

        target->bind();
        glViewport(0, 0, target->width(), target->height());
        glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
        glClear(GL_COLOR_BUFFER_BIT);

        buf.program->bind();
        buf.program->setUniformValue("u_viewport_size", viewport_size);
        buf.program->setUniformValue("u_video_size", video_size);
        buf.program->setUniformValue("u_status_bar_height", static_cast<float>(kStatusBarHeight));
        buf.program->setUniformValue("u_texture", 0);
        bind_stage_common_uniforms(buf.program.get(), stage, elapsed_seconds);
        bind_shadertoy_uniforms(buf.program.get(), elapsed_seconds, frame_delta_seconds, frame_index, video_size);

        const auto routes = buf.has_custom_channel_sources ? buf.channel_sources : default_buffer_channel_sources();
        const GLuint channel0_texture = resolve_channel_texture(stage, routes[0], video_texture, self_prev_tex,
                                    blank_texture_id_);
        const GLuint channel1_texture = resolve_channel_texture(stage, routes[1], video_texture, self_prev_tex,
                                    blank_texture_id_);
        const GLuint channel2_texture = resolve_channel_texture(stage, routes[2], video_texture, self_prev_tex,
                                    blank_texture_id_);
        const GLuint channel3_texture = resolve_channel_texture(stage, routes[3], video_texture, self_prev_tex,
                                    blank_texture_id_);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, channel0_texture);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, channel1_texture);
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, channel2_texture);
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_2D, channel3_texture);
        glActiveTexture(GL_TEXTURE0);

        quad_vertex_buffer_.bind();
        quad_vertex_buffer_.allocate(kVertices, static_cast<int>(sizeof(kVertices)));
        buf.program->enableAttributeArray("a_position");
        buf.program->enableAttributeArray("a_texcoord");
        buf.program->setAttributeBuffer("a_position", GL_FLOAT, 0, 2, 4 * sizeof(GLfloat));
        buf.program->setAttributeBuffer("a_texcoord", GL_FLOAT, 2 * sizeof(GLfloat), 2, 4 * sizeof(GLfloat));
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        buf.program->disableAttributeArray("a_position");
        buf.program->disableAttributeArray("a_texcoord");
        quad_vertex_buffer_.release();

        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);

        buf.program->release();
        target->release();

        // Flip ping-pong.
        buf.ping = read_idx;
    }

    // Restore default framebuffer viewport.
    glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
    glViewport(0, 0, width(), height());
}

} // namespace cockscreen::runtime