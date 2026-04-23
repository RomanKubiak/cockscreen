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

SceneInput parse_input(const QJsonObject &object)
{
    SceneInput input;
    input.enabled = json_bool(object, "enabled", true);
    input.device = json_string(object, "device");
    input.file = json_string(object, "file");
    input.format = json_string(object, "format");
    input.scale = std::max(0.01F, json_float(object, "scale", 1.0F));

    if (const auto on_top = object.value(QStringLiteral("on_top")); on_top.isBool())
    {
        input.on_top = on_top.toBool();
    }

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

    return layer_order.size() == 3 ? layer_order : std::vector<std::string>{};
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
        if (const auto midi = inputs_object.value(QStringLiteral("midi")); midi.isObject())
        {
            scene.midi_input = parse_input(midi.toObject());
        }
    }

    if (scene.background_color.red == 0.0F && scene.background_color.green == 0.0F && scene.background_color.blue == 0.0F)
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

    if (const auto show_status_overlay = root.value(QStringLiteral("show_status_overlay")); show_status_overlay.isBool())
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

    if (const auto shader_directory = root.value(QStringLiteral("shader_directory")); shader_directory.isString())
    {
        scene.shader_directory = resolve_shader_path(base_dir, shader_directory.toString().toStdString()).string();
    }

    if (const auto resources_directory = root.value(QStringLiteral("resources_directory")); resources_directory.isString())
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

    return scene;
}

} // namespace cockscreen::runtime::scene_parser