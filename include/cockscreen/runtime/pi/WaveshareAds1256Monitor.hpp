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

    bool start();
    void stop();

  private:
    struct Impl;

    std::unique_ptr<Impl> impl_;
};

} // namespace cockscreen::runtime

#endif