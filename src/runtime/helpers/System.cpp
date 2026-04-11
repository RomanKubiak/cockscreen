#include "cockscreen/runtime/RuntimeHelpers.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>

namespace cockscreen::runtime
{

namespace
{

constexpr std::string_view kPiZero2W{"raspberry-pi-zero-2w"};

bool is_pi_target_impl()
{
    return std::string_view{COCKSCREEN_TARGET_PLATFORM} == kPiZero2W;
}

bool has_direct_drm_access_impl()
{
    return std::filesystem::exists("/dev/dri/card0") || std::filesystem::exists("/dev/dri/renderD128");
}

} // namespace

bool is_pi_target()
{
    return is_pi_target_impl();
}

bool has_direct_drm_access()
{
    return has_direct_drm_access_impl();
}

std::optional<std::string> read_text_file(const std::filesystem::path &path)
{
    std::ifstream file{path};
    if (!file.is_open())
    {
        return std::nullopt;
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

bool looks_like_touch_input(const std::string &name)
{
    const auto lower = [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); };
    std::string normalized;
    normalized.reserve(name.size());
    for (unsigned char ch : name)
    {
        normalized.push_back(lower(ch));
    }

    return normalized.find("touch") != std::string::npos || normalized.find("tablet") != std::string::npos ||
           normalized.find("ts") != std::string::npos;
}

std::optional<std::string> find_usb_touchscreen()
{
    const std::filesystem::path input_root{"/sys/class/input"};
    if (!std::filesystem::exists(input_root))
    {
        return std::nullopt;
    }

    for (const auto &entry : std::filesystem::directory_iterator{input_root})
    {
        const auto device_dir = entry.path() / "device";
        const auto name = read_text_file(device_dir / "name");
        if (!name.has_value() || !looks_like_touch_input(*name))
        {
            continue;
        }

        std::error_code error;
        const auto resolved = std::filesystem::canonical(device_dir, error);
        if (error)
        {
            continue;
        }

        const auto resolved_text = resolved.string();
        if (resolved_text.find("/usb") != std::string::npos || resolved_text.find("usb") != std::string::npos)
        {
            return *name;
        }
    }

    return std::nullopt;
}

bool print_startup_preflight()
{
    if (!is_pi_target())
    {
        return true;
    }

    if (!has_direct_drm_access())
    {
        std::cerr << "Qt startup check failed: no direct DRM/KMS device was found.\n";
        std::cerr << "eglfs runs without a compositor, but it still needs access to /dev/dri/card0"
                  << " or /dev/dri/renderD128.\n";
        std::cerr << "If you are starting from SSH, make sure the Pi is on a local VT with DRM access"
                  << " and that the device nodes are present and readable.\n";
        return false;
    }

    if (std::getenv("DISPLAY") != nullptr || std::getenv("WAYLAND_DISPLAY") != nullptr)
    {
        std::cerr << "Qt startup note: DISPLAY/WAYLAND_DISPLAY are set, but the Pi build uses eglfs"
                  << " and ignores any compositor session.\n";
    }

    if (const auto touchscreen = find_usb_touchscreen(); touchscreen.has_value())
    {
        std::cerr << "Qt startup note: USB touch input detected: " << *touchscreen << "\n";
    }
    else
    {
        std::cerr << "Qt startup note: no USB touchscreen-like input device was detected under /sys/class/input.\n";
    }

    return true;
}

bool validate_render_path(const ApplicationSettings &settings)
{
    if (settings.render_path == "qt" || settings.render_path == "qt-shader" || settings.render_path == "v4l2-dmabuf-egl")
    {
        return true;
    }

    std::cerr << "Unknown render path: " << settings.render_path << "\n";
    std::cerr << "Supported render paths: qt, qt-shader, v4l2-dmabuf-egl\n";
    return false;
}

} // namespace cockscreen::runtime