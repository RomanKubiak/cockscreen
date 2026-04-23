#include "cockscreen/runtime/ShaderVideoWindow.hpp"

#include "cockscreen/runtime/StatusOverlay.hpp"
#include "cockscreen/runtime/shadervideo/Support.hpp"

#include <QColor>
#include <QDateTime>
#include <QElapsedTimer>
#include <QOpenGLFramebufferObject>
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
    helper::set_audio_uniforms(program, frame_);
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
    const QVector2D viewport_size{static_cast<float>(width()), static_cast<float>(height())};
    const QVector2D video_size{
        static_cast<float>(sampled_texture == texture_id_ && texture_width_ > 0 ? texture_width_ : width()),
        static_cast<float>(sampled_texture == texture_id_ && texture_height_ > 0 ? texture_height_ : height())};

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
    glBindTexture(GL_TEXTURE_2D, sampled_texture);
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
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, blank_texture_id_);
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, blank_texture_id_);
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D, blank_texture_id_);
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
            stage.label = QString::fromStdString(std::filesystem::path{shader_path}.filename().string());
            if (stage.label.isEmpty())
            {
                stage.label = QStringLiteral("default");
            }

            stage.program = std::make_unique<QOpenGLShaderProgram>();
            const auto fragment_source = load_fragment_shader_source(shader_path, false);
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

} // namespace cockscreen::runtime