#pragma once

#include <array>
#include <cstddef>

namespace cockscreen::core
{

inline constexpr std::size_t kAudioFftBandCount{16};
inline constexpr std::size_t kMidiEventCount{8};
inline constexpr std::size_t kMidiChannelCount{16};
inline constexpr std::size_t kMidiCcCount{128};

struct ControlFrame
{
    float audio_level{0.0F};
    std::array<float, kAudioFftBandCount> audio_fft_bands{};
    float bass{0.0F};
    float mid{0.0F};
    float treble{0.0F};
    float osc_x{0.5F};
    float osc_y{0.5F};
    float midi_primary{0.0F};
    float midi_secondary{0.0F};
    std::array<float, kMidiEventCount> midi_notes{};
    std::array<float, kMidiEventCount> midi_velocities{};
    std::array<float, kMidiEventCount> midi_ages{};
    std::array<float, kMidiEventCount> midi_channels{};
    std::array<float, kMidiChannelCount * kMidiCcCount> midi_cc_values{};
    float gain{1.0F};
};

} // namespace cockscreen::core
