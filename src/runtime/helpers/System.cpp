#include "cockscreen/runtime/RuntimeHelpers.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>
#include <thread>

#if defined(__linux__)
#include <sys/resource.h>
#include <sys/sysinfo.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#endif

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

double timeval_to_seconds(const timeval &tv)
{
    return static_cast<double>(tv.tv_sec) + static_cast<double>(tv.tv_usec) / 1.0e6;
}

double elapsed_seconds(const std::chrono::steady_clock::time_point &now,
                       const std::chrono::steady_clock::time_point &previous)
{
    return std::chrono::duration<double>(now - previous).count();
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
#ifdef _WIN32
    if (settings.render_path == "qt" || settings.render_path == "qt-shader" ||
        settings.render_path == "v4l2-dmabuf-egl")
    {
        return true; // v4l2-dmabuf-egl is redirected to qt-shader at runtime
    }

    std::cerr << "Unknown render path: " << settings.render_path << "\n";
    std::cerr << "Supported render paths on Windows: qt, qt-shader\n";
    return false;
#else
    if (settings.render_path == "qt" || settings.render_path == "qt-shader" || settings.render_path == "v4l2-dmabuf-egl")
    {
        return true;
    }

    std::cerr << "Unknown render path: " << settings.render_path << "\n";
    std::cerr << "Supported render paths: qt, qt-shader, v4l2-dmabuf-egl\n";
    return false;
#endif
}

SystemMetricsSnapshot SystemMetricsSampler::sample()
{
    SystemMetricsSnapshot snapshot;

#if defined(__linux__)
    snapshot.available = true;

    const auto now = std::chrono::steady_clock::now();

    struct rusage usage
    {
    };
    if (getrusage(RUSAGE_SELF, &usage) == 0)
    {
        const double cpu_seconds = timeval_to_seconds(usage.ru_utime) + timeval_to_seconds(usage.ru_stime);
        if (have_previous_sample_)
        {
            const double wall_seconds = std::max(elapsed_seconds(now, previous_sample_time_), 1.0e-6);
            const double cpu_delta = std::max(cpu_seconds - previous_cpu_seconds_, 0.0);
            const double cores = std::max(1.0, static_cast<double>(std::thread::hardware_concurrency()));
            snapshot.cpu_percent = std::clamp((cpu_delta / wall_seconds) * 100.0 / cores, 0.0, 100.0);
        }
        previous_cpu_seconds_ = cpu_seconds;
        previous_sample_time_ = now;
        have_previous_sample_ = true;
    }

    struct sysinfo info
    {
    };
    if (sysinfo(&info) == 0 && info.totalram > 0)
    {
        const double unit = static_cast<double>(info.mem_unit);
        const double total_bytes = static_cast<double>(info.totalram) * unit;
        const double free_bytes = static_cast<double>(info.freeram) * unit;
        const double used_bytes = std::max(total_bytes - free_bytes, 0.0);
        snapshot.memory_total_mb = total_bytes / (1024.0 * 1024.0);
        snapshot.memory_used_mb = used_bytes / (1024.0 * 1024.0);
        snapshot.memory_percent = std::clamp((used_bytes / total_bytes) * 100.0, 0.0, 100.0);
    }
#elif defined(_WIN32)
    snapshot.available = true;

    const auto now = std::chrono::steady_clock::now();

    FILETIME creation_time{}, exit_time{}, kernel_time{}, user_time{};
    if (GetProcessTimes(GetCurrentProcess(), &creation_time, &exit_time, &kernel_time, &user_time))
    {
        auto filetime_to_seconds = [](const FILETIME &ft) -> double {
            const ULONGLONG ticks = (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
            return static_cast<double>(ticks) / 1.0e7; // 100-nanosecond intervals
        };
        const double cpu_seconds = filetime_to_seconds(kernel_time) + filetime_to_seconds(user_time);
        if (have_previous_sample_)
        {
            const double wall_seconds = std::max(elapsed_seconds(now, previous_sample_time_), 1.0e-6);
            const double cpu_delta = std::max(cpu_seconds - previous_cpu_seconds_, 0.0);
            const double cores = std::max(1.0, static_cast<double>(std::thread::hardware_concurrency()));
            snapshot.cpu_percent = std::clamp((cpu_delta / wall_seconds) * 100.0 / cores, 0.0, 100.0);
        }
        previous_cpu_seconds_ = cpu_seconds;
        previous_sample_time_ = now;
        have_previous_sample_ = true;
    }

    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem))
    {
        const double total_bytes = static_cast<double>(mem.ullTotalPhys);
        const double avail_bytes = static_cast<double>(mem.ullAvailPhys);
        const double used_bytes  = std::max(total_bytes - avail_bytes, 0.0);
        snapshot.memory_total_mb = total_bytes / (1024.0 * 1024.0);
        snapshot.memory_used_mb  = used_bytes  / (1024.0 * 1024.0);
        snapshot.memory_percent  = std::clamp((used_bytes / total_bytes) * 100.0, 0.0, 100.0);
    }
#else
    (void)have_previous_sample_;
    (void)previous_cpu_seconds_;
    (void)previous_sample_time_;
#endif

    return snapshot;
}

} // namespace cockscreen::runtime