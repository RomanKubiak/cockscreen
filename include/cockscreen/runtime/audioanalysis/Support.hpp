#pragma once

namespace cockscreen::runtime::audio_analysis
{

inline constexpr float kReferenceDbfs{-14.0F};
inline constexpr float kDisplayBottomDb{-20.0F};
inline constexpr float kDisplayTopDb{3.0F};
inline constexpr float kPi{3.14159265358979323846F};

inline float sample_to_float(quint8 value)
{
    return (static_cast<float>(value) - 128.0F) / 128.0F;
}

inline float sample_to_float(qint16 value)
{
    return static_cast<float>(value) / 32768.0F;
}

inline float sample_to_float(qint32 value)
{
    return static_cast<float>(value) / 2147483648.0F;
}

inline float sample_to_float(float value)
{
    return value;
}

} // namespace cockscreen::runtime::audio_analysis