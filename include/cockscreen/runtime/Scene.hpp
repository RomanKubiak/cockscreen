#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace cockscreen::runtime
{

// ---------------------------------------------------------------------------
// ArtifactParams — Step 1: direct CPU-buffer corruption before GPU upload.
// Applied to the raw pixel buffer (YUYV / RGB24 / RGBA8888) per-frame.
// All effects are reproducible given the same seed + frame index.
// ---------------------------------------------------------------------------
struct ArtifactParams
{
    bool enabled{false};

    bool scanline_drop{false}; // randomly blank / freeze scan lines
    bool block_corrupt{false}; // randomise aligned pixel blocks
    bool bit_flip{false};      // burst XOR bit-flips at random offsets
    bool smear{false};         // horizontal byte smear within rows

    float strength{0.3F};   // 0.0–1.0 overall intensity
    int block_size{16};     // pixel-block edge length for block_corrupt
    std::uint32_t seed{42}; // reproducibility seed
};

// ---------------------------------------------------------------------------
// LoopbackParams — Step 2: GStreamer RTP-over-UDP loopback with tc-netem.
// Requires:  gst-launch-1.0, gst-plugins-good/bad/ugly, v4l2loopback module.
// Requires CAP_NET_ADMIN (or root) for tc commands.
// ---------------------------------------------------------------------------
struct LoopbackParams
{
    bool enabled{false};

    float loss_percent{0.0F};    // UDP packet loss  (0–100)
    float corrupt_percent{0.0F}; // UDP packet corruption (0–100)
    int delay_ms{0};             // additional one-way delay in ms
    float reorder_percent{0.0F}; // packet reorder probability (0–100)

    int udp_port{5004};          // loopback UDP port used for RTP stream
    std::string loopback_device; // output v4l2loopback device, e.g. /dev/video10

    // When true: run sender-only (no receiver/v4l2loopback); the app reads
    // decoded frames directly from GStreamer via appsink → QVideoSink.
    bool use_appsink{false};
};

enum class TransformAnimationPreset
{
    None,
    Rotate,
    Resize,
    MoveX,
    MoveY,
    Orbit,
    Wobble,
    Bounce,
};

struct TransformAnimation
{
    bool enabled{false};
    TransformAnimationPreset preset{TransformAnimationPreset::None};
    float speed{1.0F};
    float amount{0.0F};
    float phase{0.0F};
    std::string axis;
};

struct SceneInput
{
    bool enabled{true};
    std::string device;
    std::string file;
    std::string format;
    float scale{1.0F};
    float position_x{0.0F};
    float position_y{0.0F};
    float rotation{0.0F};
    TransformAnimation animation;
    std::int64_t start_ms{0};
    std::int64_t loop_start_ms{0};
    std::optional<std::int64_t> loop_end_ms;
    int loop_repeat{0};
    float playback_rate{1.0F};
    float playback_rate_looping{1.0F};
    // Audio volume controls (used by audio_playback_input).
    float volume{1.0F};                      // peak playback volume [0, 1]
    float volume_initial{0.0F};              // volume at t=0 (before intro fade-in)
    std::int64_t volume_fade_in_ms{0};       // intro: ms to ramp volume_initial → volume
    std::int64_t volume_loop_fade_in_ms{0};  // per-loop: ms at loop start to ramp up to volume
    std::int64_t volume_loop_fade_out_ms{0}; // per-loop: ms before loop end to ramp down
    std::int64_t volume_fade_out_ms{0};      // outro: ms to ramp to 0 after final loop / EOM
    // FFT analysis routing (audio_playback_input only).
    bool fft_analysis_from_playback{false}; // feed audio_playback into the FFT/level uniforms
    // FFT analysis routing (audio_input only).
    bool fft_analysis_enabled{true}; // when false, device audio does NOT drive FFT uniforms

    ArtifactParams artifact;
    LoopbackParams loopback;
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

struct SceneLayerRect
{
    float x{0.0F};
    float y{0.0F};
    float w{1.0F};
    float h{1.0F};
};

struct SceneLayerTransform
{
    bool configured{false};
    std::optional<float> scale;
    std::optional<float> position_x;
    std::optional<float> position_y;
    std::optional<float> rotation;
    TransformAnimation animation;
    // When set, overrides scale/position: absolute normalized rect [0,1] on screen.
    std::optional<SceneLayerRect> rect;
};

struct SceneGeometry
{
    int width{1024};
    int height{600};
};

enum class RenderTargetPresentation
{
    Stretch,
    Fit,
    Fill,
    Center,
    IntegerScale,
};

enum class RenderTargetFilter
{
    Linear,
    Nearest,
};

struct SceneRenderTarget
{
    bool enabled{false};
    int width{1024};
    int height{600};
    RenderTargetPresentation presentation{RenderTargetPresentation::Fit};
    RenderTargetFilter filter{RenderTargetFilter::Linear};
};

struct SceneLayer
{
    bool enabled{true};
    float opacity{1.0F};
    std::vector<std::string> shaders;
    SceneColor background_color;
    SceneBackgroundImage background_image;
    SceneLayerTransform transform;
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

struct SceneShaderUniform
{
    std::string layer;
    std::string shader;
    std::string uniform;
    float value{0.0F};
};

enum class SecondaryDisplayPage
{
    Mode,
    VideoInput,
    SystemPerformance,
    AppStatusModulation,
    LinuxOsStats,
};

struct SecondaryDisplayControlMapping
{
    std::string control;
    int gpio{-1};
    bool active_low{true};
    std::string action{"set_page"};
    SecondaryDisplayPage page{SecondaryDisplayPage::Mode};
};

struct SceneSecondaryDisplay
{
    bool enabled{true};
    std::string device{"/dev/fb1"};
    std::string model{"waveshare-1.3inch-lcd-hat"};
    std::string interface{"framebuffer"};
    int width{240};
    int height{240};
    int rotation_degrees{90};
    int i2c_address{0x3c};
    int spi_speed_hz{8000000};
    int gpio_dc{-1};
    int gpio_reset{-1};
    SceneColor background_color;
    SceneRenderTarget render_target;
    SecondaryDisplayPage default_page{SecondaryDisplayPage::VideoInput};
    SceneLayer video_layer;
    std::vector<SecondaryDisplayControlMapping> controls;
};

enum class SceneLayerType
{
    Screen,
    Video,
    Playback,
};

// A fully named layer combining display properties and (for Video/Playback) input config.
struct SceneNamedLayer
{
    SceneLayerType type{SceneLayerType::Screen};
    SceneLayer layer;
    SceneInput input; // meaningful for Video and Playback types
};

struct SceneDefinition
{
    std::filesystem::path source_path;
    SceneColor background_color;
    SceneBackgroundImage background_image;
    SceneGeometry geometry;
    SceneRenderTarget render_target;
    bool show_status_overlay{true};
    bool timecode{false};
    std::string render_path{"qt-shader"};
    std::string render_device;
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
    std::vector<SceneShaderUniform> shader_uniforms;
    SceneSecondaryDisplay secondary_display;
    // Named layers: populated from old-style video/playback/screen keys and from
    // the new "layers" dict. All rendering uses this map; old fields are kept for
    // backward compatibility with non-rendering code (web server, etc.).
    std::map<std::string, SceneNamedLayer> named_layers;
};

std::string peek_scene_render_device(const std::filesystem::path &path);
std::optional<SceneDefinition> load_scene_definition(const std::filesystem::path &path,
                                                     std::string *error_message = nullptr);

} // namespace cockscreen::runtime
