#pragma once

#if defined(__linux__) && defined(__aarch64__)

#include <memory>

namespace cockscreen::runtime
{

class WaveshareAds1256Monitor final
{
  public:
    WaveshareAds1256Monitor();
    ~WaveshareAds1256Monitor();

    WaveshareAds1256Monitor(const WaveshareAds1256Monitor &) = delete;
    WaveshareAds1256Monitor &operator=(const WaveshareAds1256Monitor &) = delete;

    bool start(bool verbose_debug = false, double vref_volts = 0.0);
    void stop();

    // Raw voltage in volts for channel 0–7.  Returns <= -9.0 before the first reading.
    float channel_voltage(unsigned int channel) const;

    // Normalized [0, 1] value (voltage / VREF).  Returns -1.0 before the first reading.
    float channel_value(unsigned int channel) const;

  private:
    struct Impl;

    std::unique_ptr<Impl> impl_;
};

} // namespace cockscreen::runtime

#endif