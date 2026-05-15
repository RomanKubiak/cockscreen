#pragma once

namespace cockscreen::tools::spi_linux_stats::config
{

inline constexpr const char *kFramebufferDevice = "/dev/fb1";
inline constexpr const char *kDisplayModel = "waveshare-1.3inch-lcd-hat";

inline constexpr int kFramebufferWidth = 240;
inline constexpr int kFramebufferHeight = 240;
inline constexpr int kRotationDegrees = 90;
inline constexpr int kUpdateIntervalMs = 200;

inline constexpr bool kEnableRenderTarget = true;
inline constexpr bool kVerboseLogging = false;
inline constexpr bool kDaemonizeByDefault = false;

inline constexpr float kBackgroundRed = 0.0F;
inline constexpr float kBackgroundGreen = 0.0F;
inline constexpr float kBackgroundBlue = 0.0F;
inline constexpr float kBackgroundAlpha = 1.0F;

} // namespace cockscreen::tools::spi_linux_stats::config
