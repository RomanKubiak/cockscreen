#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <alsa/asoundlib.h>

#include "../core/ControlFrame.hpp"
#include "Scene.hpp"

class QString;

namespace cockscreen::runtime
{

class MidiInputMonitor final
{
  public:
    explicit MidiInputMonitor(std::string requested_device, const std::vector<MidiCcMapping> *scene_midi_cc_mappings = nullptr);
    ~MidiInputMonitor();

    MidiInputMonitor(const MidiInputMonitor &) = delete;
    MidiInputMonitor &operator=(const MidiInputMonitor &) = delete;

    [[nodiscard]] bool is_active() const;
    [[nodiscard]] QString status_message() const;
    [[nodiscard]] QString activity_message() const;

    void advance(float delta_seconds);
    void poll();
    void populate_frame(core::ControlFrame *frame) const;

  private:
    struct MidiEvent
    {
        float note{0.0F};
        float velocity{0.0F};
        float age{-1.0F};
        float channel{0.0F};
        bool active{false};
    };

    static constexpr float kEventLifetimeSeconds{1.25F};

    void open_sequence();
    void close_sequence();
    void push_note_on(int channel, int note, int velocity);
    void update_control_change(int channel, int controller, int value);
    void prune_events();
    void set_status(std::string message);

    static bool contains_case_insensitive(std::string_view text, std::string_view needle);
    static std::optional<std::pair<int, int>> parse_numeric_port(std::string_view text);
    static std::optional<std::pair<int, int>> resolve_requested_port(std::string_view requested_device,
                                      std::string *resolved_label);

    std::array<MidiEvent, core::kMidiEventCount> events_{};
    std::array<float, core::kMidiChannelCount * core::kMidiCcCount> cc_values_{};
    const std::vector<MidiCcMapping> *scene_midi_cc_mappings_{nullptr};
    std::string requested_device_;
    std::string resolved_label_;
    std::string status_message_;
    std::string activity_message_;
    std::size_t note_on_count_{0};
    std::size_t controller_count_{0};
    snd_seq_t *sequence_{nullptr};
    int source_client_{-1};
    int source_port_{-1};
    int input_port_{-1};
    bool active_{false};
};

} // namespace cockscreen::runtime