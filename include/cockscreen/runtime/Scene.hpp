#pragma once

#include <cstdint>
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
    float rotation{0.0F};  // degrees, positive = clockwise
    std::optional<bool> on_top;
    std::int64_t start_ms{0};
    std::int64_t loop_start_ms{0};
    std::optional<std::int64_t> loop_end_ms;
    int loop_repeat{0};
    float playback_rate{1.0F};
    float playback_rate_looping{1.0F};
    // Audio volume controls (used by audio_playback_input).
    float volume{1.0F};          // peak playback volume [0, 1]
    float volume_initial{0.0F};  // volume at t=0 (before intro fade-in)
    std::int64_t volume_fade_in_ms{0};       // intro: ms to ramp volume_initial → volume
    std::int64_t volume_loop_fade_in_ms{0};  // per-loop: ms at loop start to ramp up to volume
    std::int64_t volume_loop_fade_out_ms{0}; // per-loop: ms before loop end to ramp down
    std::int64_t volume_fade_out_ms{0};      // outro: ms to ramp to 0 after final loop / EOM
    // FFT analysis routing (audio_playback_input only).
    bool fft_analysis_from_playback{false};  // feed audio_playback into the FFT/level uniforms
    // FFT analysis routing (audio_input only).
    bool fft_analysis_enabled{true};         // when false, device audio does NOT drive FFT uniforms
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

struct SceneGeometry
{
    int width{1024};
    int height{600};
};

struct SceneLayer
{
    bool enabled{true};
    std::vector<std::string> shaders;
};

struct PinkKeySettings
{
    float audio_algorithm{0.0F};
    float audio_reactivity{0.45F};
    float midi_reactivity{0.35F};
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

struct OscMapping
{
    std::string address;
    std::string layer;
    std::string shader;
    std::string uniform;
    float minimum{0.0F};
    float maximum{1.0F};
    float exponent{1.0F};
};

// Modulation sources that drive the video quad's transform at runtime.
// Values are additive on top of SceneInput::rotation / position / scale.
struct SceneVideoModulation
{
    // OSC: float 0..1 at this address is scaled to min..max degrees and added to base rotation
    std::string rotation_osc;
    float rotation_osc_min{0.0F};
    float rotation_osc_max{360.0F};
    // MIDI CC: controller in [0..127] on given channel (0-based) drives the same range
    int rotation_midi_cc{-1};      // -1 = disabled
    int rotation_midi_channel{0};  // 0-based channel index
    float rotation_midi_min{0.0F};
    float rotation_midi_max{360.0F};
};

struct SceneDefinition
{
    std::filesystem::path source_path;
    SceneColor background_color;
    SceneBackgroundImage background_image;
    SceneGeometry geometry;
    bool show_status_overlay{true};
    bool timecode{false};
    std::string render_path{"qt-shader"};
    std::string shader_directory;
    std::filesystem::path resources_directory;
    std::string note_font_file;
    SceneInput video_input;
    SceneInput playback_input;
    SceneInput audio_input;
    SceneInput audio_playback_input;
    SceneInput midi_input;
    SceneLayer video_layer;
    SceneLayer playback_layer;
    SceneLayer screen_layer;
    PinkKeySettings pink_key;
    std::vector<std::string> layer_order;
    std::vector<MidiCcMapping> midi_cc_mappings;
    std::vector<MidiNoteMapping> midi_note_mappings;
    std::vector<OscMapping> osc_mappings;
    SceneVideoModulation video_modulation;
};

std::optional<SceneDefinition> load_scene_definition(const std::filesystem::path &path, std::string *error_message = nullptr);

} // namespace cockscreen::runtime