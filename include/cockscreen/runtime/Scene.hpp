#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace cockscreen::runtime
{

struct SceneInput
{
    bool enabled{true};
    std::string device;
    std::string file;
    std::string format;
    float scale{1.0F};
    float position_x{0.0F};
    float position_y{0.0F};
    std::optional<bool> on_top;
};

struct SceneColor
{
    float red{0.0F};
    float green{0.0F};
    float blue{0.0F};
    float alpha{1.0F};
};

enum class BackgroundImagePlacement
{
    Center,
    Stretched,
    ProportionalStretch,
    Tiled,
};

struct SceneBackgroundImage
{
    std::string file;
    BackgroundImagePlacement placement{BackgroundImagePlacement::Center};
};

struct SceneLayer
{
    bool enabled{true};
    std::vector<std::string> shaders;
};

struct MidiCcMapping
{
    std::string layer;
    std::string shader;
    std::string uniform;
    int channel{-1};
    int controller{0};
    float minimum{0.0F};
    float maximum{1.0F};
    float exponent{1.0F};
};

struct MidiNoteMapping
{
    std::string layer;
    std::string shader;
    std::string uniform;
    int channel{-1};
    int note{-1};
    float minimum{0.0F};
    float maximum{1.0F};
    float exponent{1.0F};
};

struct SceneDefinition
{
    std::filesystem::path source_path;
    SceneColor background_color;
    SceneBackgroundImage background_image;
    bool show_status_overlay{true};
    std::string shader_directory;
    std::filesystem::path resources_directory;
    std::string note_font_file;
    SceneInput video_input;
    SceneInput playback_input;
    SceneInput audio_input;
    SceneInput midi_input;
    SceneLayer video_layer;
    SceneLayer playback_layer;
    SceneLayer screen_layer;
    std::vector<MidiCcMapping> midi_cc_mappings;
    std::vector<MidiNoteMapping> midi_note_mappings;
};

std::optional<SceneDefinition> load_scene_definition(const std::filesystem::path &path, std::string *error_message = nullptr);

} // namespace cockscreen::runtime