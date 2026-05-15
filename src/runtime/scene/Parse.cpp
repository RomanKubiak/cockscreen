#include "cockscreen/runtime/scene/Parse.hpp"

#include <QJsonArray>
#include <QJsonObject>

#include <algorithm>

namespace cockscreen::runtime::scene_parser
{

namespace
{

std::string json_string(const QJsonObject &object, const char *key, std::string fallback = {})
{
    const auto value = object.value(QString::fromUtf8(key));
    return value.isString() ? value.toString().toStdString() : std::move(fallback);
}

bool json_bool(const QJsonObject &object, const char *key, bool fallback)
{
    const auto value = object.value(QString::fromUtf8(key));
    return value.isBool() ? value.toBool() : fallback;
}

std::int64_t json_int64(const QJsonObject &object, const char *key, std::int64_t fallback)
{
    const auto value = object.value(QString::fromUtf8(key));
    return value.isDouble() ? static_cast<std::int64_t>(value.toDouble(static_cast<double>(fallback))) : fallback;
}

int json_int(const QJsonObject &object, const char *key, int fallback)
{
    const auto value = object.value(QString::fromUtf8(key));
    return value.isDouble() ? value.toInt(fallback) : fallback;
}

float json_float(const QJsonObject &object, const char *key, float fallback)
{
    const auto value = object.value(QString::fromUtf8(key));
    return value.isDouble() ? static_cast<float>(value.toDouble(fallback)) : fallback;
}

PinkKeySettings parse_pink_key_settings(const QJsonValue &value)
{
    PinkKeySettings settings;
    if (!value.isObject())
    {
        return settings;
    }

    const auto object = value.toObject();
    settings.audio_algorithm = std::clamp(json_float(object, "audio_algorithm", settings.audio_algorithm), 0.0F, 5.0F);
    settings.audio_reactivity =
        std::clamp(json_float(object, "audio_reactivity", settings.audio_reactivity), 0.0F, 1.5F);
    settings.midi_reactivity = std::clamp(json_float(object, "midi_reactivity", settings.midi_reactivity), 0.0F, 1.5F);
    return settings;
}

SceneColor parse_color(const QJsonValue &value)
{
    SceneColor color;
    if (!value.isObject())
    {
        return color;
    }

    const auto object = value.toObject();
    color.red = std::clamp(json_float(object, "r", json_float(object, "red", 0.0F)), 0.0F, 1.0F);
    color.green = std::clamp(json_float(object, "g", json_float(object, "green", 0.0F)), 0.0F, 1.0F);
    color.blue = std::clamp(json_float(object, "b", json_float(object, "blue", 0.0F)), 0.0F, 1.0F);
    color.alpha = std::clamp(json_float(object, "a", json_float(object, "alpha", 1.0F)), 0.0F, 1.0F);
    return color;
}

SceneGeometry parse_geometry(const QJsonValue &value)
{
    SceneGeometry geometry;
    if (!value.isObject())
    {
        return geometry;
    }

    const auto object = value.toObject();
    geometry.width = std::max(1, json_int(object, "width", geometry.width));
    geometry.height = std::max(1, json_int(object, "height", geometry.height));
    return geometry;
}

BackgroundImagePlacement parse_background_image_placement(const std::string &placement)
{
    const QString normalized = QString::fromStdString(placement).trimmed().toLower();
    if (normalized == QStringLiteral("stretched"))
    {
        return BackgroundImagePlacement::Stretched;
    }
    if (normalized == QStringLiteral("proportional-stretch") || normalized == QStringLiteral("propotional-stretch") ||
        normalized == QStringLiteral("proportional_stretch"))
    {
        return BackgroundImagePlacement::ProportionalStretch;
    }
    if (normalized == QStringLiteral("tiled"))
    {
        return BackgroundImagePlacement::Tiled;
    }

    return BackgroundImagePlacement::Center;
}

RenderTargetPresentation parse_render_target_presentation(const std::string &presentation)
{
    const QString normalized = QString::fromStdString(presentation).trimmed().toLower();
    if (normalized == QStringLiteral("stretch"))
    {
        return RenderTargetPresentation::Stretch;
    }
    if (normalized == QStringLiteral("fill") || normalized == QStringLiteral("crop"))
    {
        return RenderTargetPresentation::Fill;
    }
    if (normalized == QStringLiteral("center") || normalized == QStringLiteral("centered"))
    {
        return RenderTargetPresentation::Center;
    }
    if (normalized == QStringLiteral("integer-scale") || normalized == QStringLiteral("integer_scale") ||
        normalized == QStringLiteral("integer"))
    {
        return RenderTargetPresentation::IntegerScale;
    }

    return RenderTargetPresentation::Fit;
}

RenderTargetFilter parse_render_target_filter(const std::string &filter)
{
    const QString normalized = QString::fromStdString(filter).trimmed().toLower();
    if (normalized == QStringLiteral("nearest") || normalized == QStringLiteral("pixelated"))
    {
        return RenderTargetFilter::Nearest;
    }

    return RenderTargetFilter::Linear;
}

SceneRenderTarget parse_render_target(const QJsonValue &value, const SceneGeometry &geometry)
{
    SceneRenderTarget target;
    target.width = geometry.width;
    target.height = geometry.height;
    if (!value.isObject())
    {
        return target;
    }

    const auto object = value.toObject();
    target.enabled = json_bool(object, "enabled", false);
    target.width = std::max(1, json_int(object, "width", target.width));
    target.height = std::max(1, json_int(object, "height", target.height));
    target.presentation = parse_render_target_presentation(json_string(object, "presentation", "fit"));
    target.filter = parse_render_target_filter(json_string(object, "filter", "linear"));
    return target;
}

SceneLayer parse_layer(const QJsonValue &value);

SecondaryDisplayPage parse_secondary_display_page(const std::string &page)
{
    const QString normalized = QString::fromStdString(page).trimmed().toLower().replace(QChar{'_'}, QChar{'-'});
    if (normalized == QStringLiteral("mode"))
    {
        return SecondaryDisplayPage::Mode;
    }
    if (normalized == QStringLiteral("system") || normalized == QStringLiteral("system-performance") ||
        normalized == QStringLiteral("system-performance-overview"))
    {
        return SecondaryDisplayPage::SystemPerformance;
    }
    if (normalized == QStringLiteral("app-status") || normalized == QStringLiteral("app-status-modulation") ||
        normalized == QStringLiteral("modulation"))
    {
        return SecondaryDisplayPage::AppStatusModulation;
    }
    if (normalized == QStringLiteral("linux") || normalized == QStringLiteral("linux-os") ||
        normalized == QStringLiteral("linux-os-stats") || normalized == QStringLiteral("os-stats") ||
        normalized == QStringLiteral("system-stats"))
    {
        return SecondaryDisplayPage::LinuxOsStats;
    }

    return SecondaryDisplayPage::VideoInput;
}

std::vector<SecondaryDisplayControlMapping> default_secondary_display_controls()
{
    return {
        SecondaryDisplayControlMapping{"key1", 21, true, "cycle_page", SecondaryDisplayPage::Mode},
        SecondaryDisplayControlMapping{"key2", 20, true, "set_page", SecondaryDisplayPage::VideoInput},
        SecondaryDisplayControlMapping{"key3", 16, true, "set_page", SecondaryDisplayPage::SystemPerformance},
        SecondaryDisplayControlMapping{"joystick_press", 13, true, "set_page", SecondaryDisplayPage::AppStatusModulation},
        SecondaryDisplayControlMapping{"joystick_up", 6, true, "previous_page", SecondaryDisplayPage::Mode},
        SecondaryDisplayControlMapping{"joystick_down", 19, true, "next_page", SecondaryDisplayPage::Mode},
        SecondaryDisplayControlMapping{"joystick_left", 5, true, "previous_page", SecondaryDisplayPage::Mode},
        SecondaryDisplayControlMapping{"joystick_right", 26, true, "next_page", SecondaryDisplayPage::Mode},
    };
}

SceneSecondaryDisplay parse_secondary_display(const QJsonValue &value)
{
    SceneSecondaryDisplay display;
    display.controls = default_secondary_display_controls();
    display.video_layer.enabled = true;

    if (!value.isObject())
    {
        return display;
    }

    const auto object = value.toObject();
    display.enabled = json_bool(object, "enabled", display.enabled);
    display.device = json_string(object, "device", display.device);
    display.model = json_string(object, "model", display.model);
    display.width = std::max(1, json_int(object, "width", display.width));
    display.height = std::max(1, json_int(object, "height", display.height));
    display.rotation_degrees = json_int(object, "rotation_degrees", json_int(object, "rotation", display.rotation_degrees));
    if (const auto background = object.value(QStringLiteral("background_color")); background.isObject())
    {
        display.background_color = parse_color(background);
    }
    else if (const auto background = object.value(QStringLiteral("background")); background.isObject())
    {
        display.background_color = parse_color(background);
    }

    SceneGeometry secondary_geometry;
    secondary_geometry.width = display.width;
    secondary_geometry.height = display.height;
    display.render_target = parse_render_target(object.value(QStringLiteral("render_target")), secondary_geometry);
    if (!display.render_target.enabled)
    {
        display.render_target.width = display.width;
        display.render_target.height = display.height;
    }

    display.default_page = parse_secondary_display_page(json_string(object, "default_page", "video_input"));
    if (const auto source = object.value(QStringLiteral("source")); source.isString())
    {
        display.default_page = parse_secondary_display_page(source.toString().toStdString());
    }
    if (const auto mode = object.value(QStringLiteral("mode")); mode.isString())
    {
        display.default_page = parse_secondary_display_page(mode.toString().toStdString());
    }

    display.video_layer = parse_layer(object.value(QStringLiteral("video")));
    if (!object.value(QStringLiteral("video")).isObject())
    {
        display.video_layer.enabled = true;
    }
    if (const auto controls = object.value(QStringLiteral("controls")); controls.isObject())
    {
        display.controls.clear();
        const auto controls_object = controls.toObject();
        for (auto it = controls_object.begin(); it != controls_object.end(); ++it)
        {
            if (!it.value().isObject())
            {
                continue;
            }
            const auto control_object = it.value().toObject();
            SecondaryDisplayControlMapping mapping;
            mapping.control = it.key().toStdString();
            mapping.gpio = json_int(control_object, "gpio", -1);
            mapping.active_low = json_bool(control_object, "active_low", true);
            mapping.action = json_string(control_object, "action", "set_page");
            mapping.page = parse_secondary_display_page(json_string(control_object, "page", mapping.control));
            if (mapping.gpio >= 0)
            {
                display.controls.push_back(std::move(mapping));
            }
        }
    }

    return display;
}

ArtifactParams parse_artifact(const QJsonValue &value)
{
    ArtifactParams params;
    if (!value.isObject())
    {
        return params;
    }

    const auto object = value.toObject();
    params.enabled = json_bool(object, "enabled", false);
    params.scanline_drop = json_bool(object, "scanline_drop", false);
    params.block_corrupt = json_bool(object, "block_corrupt", false);
    params.bit_flip = json_bool(object, "bit_flip", false);
    params.smear = json_bool(object, "smear", false);
    params.strength = std::clamp(json_float(object, "strength", 0.3F), 0.0F, 1.0F);
    params.block_size = std::max(2, json_int(object, "block_size", 16));
    params.seed = static_cast<std::uint32_t>(std::max(0, json_int(object, "seed", 42)));
    return params;
}

LoopbackParams parse_loopback(const QJsonValue &value)
{
    LoopbackParams params;
    if (!value.isObject())
    {
        return params;
    }

    const auto object = value.toObject();
    params.enabled = json_bool(object, "enabled", false);
    params.loss_percent = std::clamp(json_float(object, "loss_percent", 0.0F), 0.0F, 100.0F);
    params.corrupt_percent = std::clamp(json_float(object, "corrupt_percent", 0.0F), 0.0F, 100.0F);
    params.delay_ms = std::max(0, json_int(object, "delay_ms", 0));
    params.reorder_percent = std::clamp(json_float(object, "reorder_percent", 0.0F), 0.0F, 100.0F);
    params.udp_port = std::clamp(json_int(object, "udp_port", 5004), 1024, 65535);
    params.loopback_device = json_string(object, "loopback_device", "/dev/video10");
    params.use_appsink = json_bool(object, "use_appsink", false);
    return params;
}

TransformAnimationPreset parse_transform_animation_preset(const std::string &preset)
{
    const QString normalized = QString::fromStdString(preset).trimmed().toLower();
    if (normalized == QStringLiteral("rotate"))
    {
        return TransformAnimationPreset::Rotate;
    }
    if (normalized == QStringLiteral("resize"))
    {
        return TransformAnimationPreset::Resize;
    }
    if (normalized == QStringLiteral("move-x") || normalized == QStringLiteral("movex") ||
        normalized == QStringLiteral("move_x"))
    {
        return TransformAnimationPreset::MoveX;
    }
    if (normalized == QStringLiteral("move-y") || normalized == QStringLiteral("movey") ||
        normalized == QStringLiteral("move_y"))
    {
        return TransformAnimationPreset::MoveY;
    }
    if (normalized == QStringLiteral("orbit"))
    {
        return TransformAnimationPreset::Orbit;
    }
    if (normalized == QStringLiteral("wobble"))
    {
        return TransformAnimationPreset::Wobble;
    }
    if (normalized == QStringLiteral("bounce"))
    {
        return TransformAnimationPreset::Bounce;
    }

    return TransformAnimationPreset::None;
}

TransformAnimation parse_transform_animation(const QJsonValue &value)
{
    TransformAnimation animation;
    if (!value.isObject())
    {
        return animation;
    }

    const auto object = value.toObject();
    animation.enabled = json_bool(object, "enabled", false);
    animation.preset = parse_transform_animation_preset(json_string(object, "preset"));
    animation.speed = std::max(0.0F, json_float(object, "speed", animation.speed));
    animation.amount = json_float(object, "amount", animation.amount);
    animation.phase = json_float(object, "phase", animation.phase);
    animation.axis = json_string(object, "axis");
    if (animation.preset == TransformAnimationPreset::None)
    {
        animation.enabled = false;
    }
    return animation;
}

SceneLayerTransform parse_layer_transform(const QJsonObject &object)
{
    SceneLayerTransform transform;

    const auto parse_fields = [&](const QJsonObject &source) {
        if (const auto scale = source.value(QStringLiteral("scale")); scale.isDouble())
        {
            transform.scale = std::max(0.01F, static_cast<float>(scale.toDouble()));
            transform.configured = true;
        }
        if (const auto position = source.value(QStringLiteral("position")); position.isObject())
        {
            const auto position_object = position.toObject();
            if (const auto x = position_object.value(QStringLiteral("x")); x.isDouble())
            {
                transform.position_x = static_cast<float>(x.toDouble());
                transform.configured = true;
            }
            if (const auto y = position_object.value(QStringLiteral("y")); y.isDouble())
            {
                transform.position_y = static_cast<float>(y.toDouble());
                transform.configured = true;
            }
        }
        if (const auto x = source.value(QStringLiteral("position_x")); x.isDouble())
        {
            transform.position_x = static_cast<float>(x.toDouble());
            transform.configured = true;
        }
        if (const auto y = source.value(QStringLiteral("position_y")); y.isDouble())
        {
            transform.position_y = static_cast<float>(y.toDouble());
            transform.configured = true;
        }
        if (const auto rotation = source.value(QStringLiteral("rotation")); rotation.isDouble())
        {
            transform.rotation = static_cast<float>(rotation.toDouble());
            transform.configured = true;
        }
        if (const auto animation = source.value(QStringLiteral("animation")); animation.isObject())
        {
            transform.animation = parse_transform_animation(animation);
            if (transform.animation.enabled)
            {
                transform.configured = true;
            }
        }
    };

    parse_fields(object);
    if (const auto nested = object.value(QStringLiteral("transform")); nested.isObject())
    {
        parse_fields(nested.toObject());
    }

    return transform;
}

SceneInput parse_input(const QJsonObject &object)
{
    SceneInput input;
    input.enabled = json_bool(object, "enabled", true);
    input.device = json_string(object, "device");
    input.file = json_string(object, "file");
    input.format = json_string(object, "format");
    input.scale = std::max(0.01F, json_float(object, "scale", 1.0F));
    input.rotation = json_float(object, "rotation", 0.0F);

    if (const auto position = object.value(QStringLiteral("position")); position.isObject())
    {
        const auto position_object = position.toObject();
        input.position_x = json_float(position_object, "x", 0.0F);
        input.position_y = json_float(position_object, "y", 0.0F);
    }
    else
    {
        input.position_x = json_float(object, "position_x", 0.0F);
        input.position_y = json_float(object, "position_y", 0.0F);
    }

    input.start_ms = std::max<std::int64_t>(0, json_int64(object, "start_ms", 0));
    input.loop_start_ms = std::max<std::int64_t>(0, json_int64(object, "loop_start_ms", 0));
    if (const auto loop_end_ms = object.value(QStringLiteral("loop_end_ms")); loop_end_ms.isDouble())
    {
        input.loop_end_ms = std::max<std::int64_t>(0, static_cast<std::int64_t>(loop_end_ms.toDouble()));
    }
    input.loop_repeat = std::max(0, json_int(object, "loop_repeat", 0));
    input.playback_rate = std::max(0.01F, json_float(object, "playback_rate", 1.0F));
    input.playback_rate_looping = std::max(0.01F, json_float(object, "playback_rate_looping", 1.0F));
    input.volume = std::clamp(json_float(object, "volume", 1.0F), 0.0F, 1.0F);
    input.volume_initial = std::clamp(json_float(object, "volume_initial", 0.0F), 0.0F, 1.0F);
    input.volume_fade_in_ms = std::max<std::int64_t>(0, json_int64(object, "volume_fade_in_ms", 0));
    input.volume_loop_fade_in_ms = std::max<std::int64_t>(0, json_int64(object, "volume_loop_fade_in_ms", 0));
    input.volume_loop_fade_out_ms = std::max<std::int64_t>(0, json_int64(object, "volume_loop_fade_out_ms", 0));
    input.volume_fade_out_ms = std::max<std::int64_t>(0, json_int64(object, "volume_fade_out_ms", 0));
    input.fft_analysis_from_playback = json_bool(object, "fft_analysis_from_playback", false);
    input.fft_analysis_enabled = json_bool(object, "fft_analysis_enabled", true);

    input.artifact = parse_artifact(object.value(QStringLiteral("artifact")));
    input.loopback = parse_loopback(object.value(QStringLiteral("loopback")));
    input.animation = parse_transform_animation(object.value(QStringLiteral("animation")));
    if (!input.animation.enabled)
    {
        if (const auto transform = object.value(QStringLiteral("transform")); transform.isObject())
        {
            input.animation = parse_transform_animation(transform.toObject().value(QStringLiteral("animation")));
        }
    }

    if (!input.enabled)
    {
        input.device.clear();
        input.format.clear();
    }
    return input;
}

SceneLayer parse_layer(const QJsonValue &value)
{
    SceneLayer layer;
    if (!value.isObject())
    {
        return layer;
    }

    const auto object = value.toObject();
    layer.enabled = json_bool(object, "enabled", true);
    layer.opacity = std::clamp(json_float(object, "opacity", layer.opacity), 0.0F, 1.0F);
    if (const auto background = object.value(QStringLiteral("background_color")); background.isObject())
    {
        layer.background_color = parse_color(background);
    }
    else if (const auto background = object.value(QStringLiteral("background")); background.isObject())
    {
        layer.background_color = parse_color(background);
    }

    if (const auto background_image = object.value(QStringLiteral("background_image")); background_image.isObject())
    {
        const auto background_object = background_image.toObject();
        layer.background_image.file = json_string(background_object, "file");
        if (const auto placement = background_object.value(QStringLiteral("placement")); placement.isString())
        {
            layer.background_image.placement = parse_background_image_placement(placement.toString().toStdString());
        }
    }
    else if (background_image.isString())
    {
        layer.background_image.file = background_image.toString().toStdString();
    }

    if (const auto shaders = object.value(QStringLiteral("shaders")); shaders.isArray())
    {
        for (const auto &shader_value : shaders.toArray())
        {
            if (shader_value.isString())
            {
                layer.shaders.push_back(shader_value.toString().toStdString());
            }
        }
    }
    layer.transform = parse_layer_transform(object);
    return layer;
}

std::vector<std::string> parse_layer_order(const QJsonValue &value)
{
    if (!value.isArray())
    {
        return {};
    }

    std::vector<std::string> layer_order;
    layer_order.reserve(3);
    for (const auto &entry : value.toArray())
    {
        if (!entry.isString())
        {
            continue;
        }

        const QString normalized = entry.toString().trimmed().toLower();
        if (normalized != QStringLiteral("video") && normalized != QStringLiteral("playback") &&
            normalized != QStringLiteral("screen"))
        {
            continue;
        }

        const std::string layer_name = normalized.toStdString();
        if (std::find(layer_order.begin(), layer_order.end(), layer_name) == layer_order.end())
        {
            layer_order.push_back(layer_name);
        }
    }

    return layer_order;
}

std::filesystem::path resolve_shader_path(const std::filesystem::path &base_dir, const std::string &shader_file)
{
    if (shader_file.empty())
    {
        return {};
    }

    const std::filesystem::path requested{shader_file};
    if (requested.is_absolute())
    {
        return requested;
    }

    return base_dir / requested;
}

} // namespace

SceneDefinition parse_scene_definition(const QJsonObject &root, const std::filesystem::path &source_path)
{
    SceneDefinition scene;
    scene.source_path = source_path;
    const auto base_dir = source_path.parent_path();
    scene.resources_directory = base_dir;

    if (const auto inputs = root.value(QStringLiteral("inputs")); inputs.isObject())
    {
        const auto inputs_object = inputs.toObject();
        if (const auto background = inputs_object.value(QStringLiteral("background_color")); background.isObject())
        {
            scene.background_color = parse_color(background);
        }
        else if (const auto background = inputs_object.value(QStringLiteral("background")); background.isObject())
        {
            scene.background_color = parse_color(background);
        }

        if (const auto video = inputs_object.value(QStringLiteral("video")); video.isObject())
        {
            scene.video_input = parse_input(video.toObject());
        }
        if (const auto playback = inputs_object.value(QStringLiteral("playback")); playback.isObject())
        {
            scene.playback_input = parse_input(playback.toObject());
        }
        if (const auto audio = inputs_object.value(QStringLiteral("audio")); audio.isObject())
        {
            scene.audio_input = parse_input(audio.toObject());
        }
        if (const auto audio_playback = inputs_object.value(QStringLiteral("audio_playback"));
            audio_playback.isObject())
        {
            scene.audio_playback_input = parse_input(audio_playback.toObject());
        }
        if (const auto midi = inputs_object.value(QStringLiteral("midi")); midi.isObject())
        {
            scene.midi_input = parse_input(midi.toObject());
        }
    }

    if (scene.background_color.red == 0.0F && scene.background_color.green == 0.0F &&
        scene.background_color.blue == 0.0F)
    {
        if (const auto background = root.value(QStringLiteral("background_color")); background.isObject())
        {
            scene.background_color = parse_color(background);
        }
        else if (const auto background = root.value(QStringLiteral("background")); background.isObject())
        {
            scene.background_color = parse_color(background);
        }
    }

    if (const auto geometry = root.value(QStringLiteral("geometry")); geometry.isObject())
    {
        scene.geometry = parse_geometry(geometry);
    }
    else
    {
        scene.geometry.width = std::max(1, json_int(root, "width", scene.geometry.width));
        scene.geometry.height = std::max(1, json_int(root, "height", scene.geometry.height));
    }

    scene.render_target = parse_render_target(root.value(QStringLiteral("render_target")), scene.geometry);
    scene.secondary_display = parse_secondary_display(root.value(QStringLiteral("secondary_display")));

    if (const auto show_status_overlay = root.value(QStringLiteral("show_status_overlay"));
        show_status_overlay.isBool())
    {
        scene.show_status_overlay = show_status_overlay.toBool();
    }

    if (const auto timecode = root.value(QStringLiteral("timecode")); timecode.isBool())
    {
        scene.timecode = timecode.toBool();
    }

    if (const auto render_path = root.value(QStringLiteral("render_path")); render_path.isString())
    {
        scene.render_path = render_path.toString().toStdString();
    }

    if (const auto render_device = root.value(QStringLiteral("render_device")); render_device.isString())
    {
        scene.render_device = render_device.toString().toStdString();
    }

    if (const auto shader_directory = root.value(QStringLiteral("shader_directory")); shader_directory.isString())
    {
        scene.shader_directory = resolve_shader_path(base_dir, shader_directory.toString().toStdString()).string();
    }

    if (const auto resources_directory = root.value(QStringLiteral("resources_directory"));
        resources_directory.isString())
    {
        scene.resources_directory = resolve_shader_path(base_dir, resources_directory.toString().toStdString());
    }

    if (const auto note_font_file = root.value(QStringLiteral("note_font_file")); note_font_file.isString())
    {
        scene.note_font_file = note_font_file.toString().toStdString();
    }

    if (const auto background_image = root.value(QStringLiteral("background_image")); background_image.isObject())
    {
        const auto background_object = background_image.toObject();
        scene.background_image.file = json_string(background_object, "file");
        if (const auto placement = background_object.value(QStringLiteral("placement")); placement.isString())
        {
            scene.background_image.placement = parse_background_image_placement(placement.toString().toStdString());
        }
    }
    else if (background_image.isString())
    {
        scene.background_image.file = background_image.toString().toStdString();
    }

    if (const auto pink_key = root.value(QStringLiteral("pink_key")); pink_key.isObject())
    {
        scene.pink_key = parse_pink_key_settings(pink_key);
    }

    if (scene.background_image.file.empty())
    {
        if (const auto background_image_file = root.value(QStringLiteral("background_image_file"));
            background_image_file.isString())
        {
            scene.background_image.file = background_image_file.toString().toStdString();
        }
        if (const auto background_image_placement = root.value(QStringLiteral("background_image_placement"));
            background_image_placement.isString())
        {
            scene.background_image.placement =
                parse_background_image_placement(background_image_placement.toString().toStdString());
        }
    }

    scene.video_layer = parse_layer(root.value(QStringLiteral("video")));
    scene.playback_layer = parse_layer(root.value(QStringLiteral("playback")));
    scene.screen_layer = parse_layer(root.value(QStringLiteral("screen")));
    scene.layer_order = parse_layer_order(root.value(QStringLiteral("layer_order")));

    if (const auto mappings = root.value(QStringLiteral("midi_cc_mappings")); mappings.isArray())
    {
        for (const auto &mapping_value : mappings.toArray())
        {
            if (!mapping_value.isObject())
            {
                continue;
            }

            const auto mapping_object = mapping_value.toObject();
            MidiCcMapping mapping;
            mapping.layer = json_string(mapping_object, "layer");
            mapping.shader = json_string(mapping_object, "shader");
            mapping.uniform = json_string(mapping_object, "uniform");
            mapping.channel = json_int(mapping_object, "channel", -1);
            mapping.controller = json_int(mapping_object, "cc", 0);
            mapping.minimum = json_float(mapping_object, "min", 0.0F);
            mapping.maximum = json_float(mapping_object, "max", 1.0F);
            mapping.exponent = std::max(0.01F, json_float(mapping_object, "exponent", 1.0F));

            if (!mapping.layer.empty() && !mapping.uniform.empty())
            {
                scene.midi_cc_mappings.push_back(std::move(mapping));
            }
        }
    }

    if (const auto mappings = root.value(QStringLiteral("midi_note_mappings")); mappings.isArray())
    {
        for (const auto &mapping_value : mappings.toArray())
        {
            if (!mapping_value.isObject())
            {
                continue;
            }

            const auto mapping_object = mapping_value.toObject();
            MidiNoteMapping mapping;
            mapping.layer = json_string(mapping_object, "layer");
            mapping.shader = json_string(mapping_object, "shader");
            mapping.uniform = json_string(mapping_object, "uniform");
            mapping.channel = json_int(mapping_object, "channel", -1);
            mapping.note = json_int(mapping_object, "note", -1);
            mapping.minimum = json_float(mapping_object, "min", 0.0F);
            mapping.maximum = json_float(mapping_object, "max", 1.0F);
            mapping.exponent = std::max(0.01F, json_float(mapping_object, "exponent", 1.0F));

            if (!mapping.layer.empty() && !mapping.uniform.empty())
            {
                scene.midi_note_mappings.push_back(std::move(mapping));
            }
        }
    }

    if (const auto mappings = root.value(QStringLiteral("osc_mappings")); mappings.isArray())
    {
        for (const auto &mapping_value : mappings.toArray())
        {
            if (!mapping_value.isObject())
            {
                continue;
            }

            const auto mapping_object = mapping_value.toObject();
            OscMapping mapping;
            mapping.address = json_string(mapping_object, "address");
            mapping.layer = json_string(mapping_object, "layer");
            mapping.shader = json_string(mapping_object, "shader");
            mapping.uniform = json_string(mapping_object, "uniform");
            mapping.minimum = json_float(mapping_object, "min", 0.0F);
            mapping.maximum = json_float(mapping_object, "max", 1.0F);
            mapping.exponent = std::max(0.01F, json_float(mapping_object, "exponent", 1.0F));

            if (!mapping.address.empty() && !mapping.layer.empty() && !mapping.uniform.empty())
            {
                scene.osc_mappings.push_back(std::move(mapping));
            }
        }
    }

    if (const auto uniforms = root.value(QStringLiteral("shader_uniforms")); uniforms.isArray())
    {
        for (const auto &uniform_value : uniforms.toArray())
        {
            if (!uniform_value.isObject())
            {
                continue;
            }

            const auto uniform_object = uniform_value.toObject();
            SceneShaderUniform uniform;
            uniform.layer = json_string(uniform_object, "layer");
            uniform.shader = json_string(uniform_object, "shader");
            uniform.uniform = json_string(uniform_object, "uniform");
            uniform.value = json_float(uniform_object, "value", 0.0F);

            if (!uniform.layer.empty() && !uniform.uniform.empty())
            {
                scene.shader_uniforms.push_back(std::move(uniform));
            }
        }
    }

    return scene;
}

} // namespace cockscreen::runtime::scene_parser
