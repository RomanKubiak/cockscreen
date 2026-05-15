#ifndef _WIN32

#include "cockscreen/runtime/Scene.hpp"
#include "cockscreen/runtime/pi/FramebufferMirror.hpp"

#include "config.h"

#include <QGuiApplication>
#include <QImage>
#include <QStringList>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace cockscreen::tools::spi_linux_stats
{

namespace
{

std::atomic_bool g_running{true};

struct Options
{
    std::string framebuffer_device{config::kFramebufferDevice};
    int width{config::kFramebufferWidth};
    int height{config::kFramebufferHeight};
    int rotation_degrees{config::kRotationDegrees};
    int update_interval_ms{config::kUpdateIntervalMs};
    bool verbose{config::kVerboseLogging};
    bool daemonize{config::kDaemonizeByDefault};
    bool once{false};
    bool help{false};
};

void handle_signal(int)
{
    g_running = false;
}

void print_help()
{
    std::cout << "Usage: cockscreen_spi_linux_stats [options]\n"
              << "  --daemonize          Fork into the background\n"
              << "  --device PATH        Framebuffer device (default from config.h)\n"
              << "  --interval-ms N      Refresh interval in milliseconds\n"
              << "  --rotation N         Clockwise rotation in degrees\n"
              << "  --width N            Logical page width\n"
              << "  --height N           Logical page height\n"
              << "  --once               Render one frame and exit\n"
              << "  -v, --verbose        Enable framebuffer/GPIO debug logs\n"
              << "  -h, --help           Show this help text\n";
}

std::optional<int> parse_int(std::string_view text)
{
    try
    {
        std::size_t consumed = 0;
        const int value = std::stoi(std::string{text}, &consumed);
        if (consumed == text.size())
        {
            return value;
        }
    }
    catch (...)
    {
    }
    return std::nullopt;
}

std::string_view next_value(int &index, int argc, char *argv[])
{
    if (index + 1 >= argc)
    {
        return {};
    }
    ++index;
    return argv[index];
}

bool parse_arguments(int argc, char *argv[], Options *options)
{
    if (options == nullptr)
    {
        return false;
    }

    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument{argv[index]};
        if (argument == "-h" || argument == "--help")
        {
            options->help = true;
        }
        else if (argument == "-v" || argument == "--verbose")
        {
            options->verbose = true;
        }
        else if (argument == "--daemonize")
        {
            options->daemonize = true;
        }
        else if (argument == "--once")
        {
            options->once = true;
        }
        else if (argument == "--device")
        {
            const auto value = next_value(index, argc, argv);
            if (value.empty())
            {
                std::cerr << "Missing value for --device\n";
                return false;
            }
            options->framebuffer_device = std::string{value};
        }
        else if (argument == "--interval-ms")
        {
            const auto value = parse_int(next_value(index, argc, argv));
            if (!value.has_value() || *value <= 0)
            {
                std::cerr << "Invalid value for --interval-ms\n";
                return false;
            }
            options->update_interval_ms = *value;
        }
        else if (argument == "--rotation")
        {
            const auto value = parse_int(next_value(index, argc, argv));
            if (!value.has_value())
            {
                std::cerr << "Invalid value for --rotation\n";
                return false;
            }
            options->rotation_degrees = *value;
        }
        else if (argument == "--width")
        {
            const auto value = parse_int(next_value(index, argc, argv));
            if (!value.has_value() || *value <= 0)
            {
                std::cerr << "Invalid value for --width\n";
                return false;
            }
            options->width = *value;
        }
        else if (argument == "--height")
        {
            const auto value = parse_int(next_value(index, argc, argv));
            if (!value.has_value() || *value <= 0)
            {
                std::cerr << "Invalid value for --height\n";
                return false;
            }
            options->height = *value;
        }
        else
        {
            std::cerr << "Unknown argument: " << argument << '\n';
            return false;
        }
    }

    return true;
}

bool daemonize_process()
{
    const pid_t pid = ::fork();
    if (pid < 0)
    {
        std::cerr << "fork() failed: " << std::strerror(errno) << '\n';
        return false;
    }
    if (pid > 0)
    {
        std::cout << "Started background PID " << pid << '\n';
        std::_Exit(0);
    }

    if (::setsid() < 0)
    {
        std::cerr << "setsid() failed: " << std::strerror(errno) << '\n';
        return false;
    }

    const pid_t second_pid = ::fork();
    if (second_pid < 0)
    {
        std::cerr << "second fork() failed: " << std::strerror(errno) << '\n';
        return false;
    }
    if (second_pid > 0)
    {
        std::_Exit(0);
    }

    ::umask(0);
    ::chdir("/");

    const int null_fd = ::open("/dev/null", O_RDWR);
    if (null_fd >= 0)
    {
        ::dup2(null_fd, STDIN_FILENO);
        ::dup2(null_fd, STDOUT_FILENO);
        ::dup2(null_fd, STDERR_FILENO);
        if (null_fd > STDERR_FILENO)
        {
            ::close(null_fd);
        }
    }

    return true;
}

cockscreen::runtime::SceneSecondaryDisplay build_display_config(const Options &options)
{
    using namespace cockscreen::runtime;

    SceneSecondaryDisplay display;
    display.enabled = true;
    display.device = options.framebuffer_device;
    display.model = config::kDisplayModel;
    display.width = options.width;
    display.height = options.height;
    display.rotation_degrees = options.rotation_degrees;
    display.background_color.red = config::kBackgroundRed;
    display.background_color.green = config::kBackgroundGreen;
    display.background_color.blue = config::kBackgroundBlue;
    display.background_color.alpha = config::kBackgroundAlpha;
    display.render_target.enabled = config::kEnableRenderTarget;
    display.render_target.width = options.width;
    display.render_target.height = options.height;
    display.default_page = SecondaryDisplayPage::LinuxOsStats;
    display.video_layer.enabled = false;
    display.video_layer.opacity = 0.0F;
    display.controls.clear();
    return display;
}

QStringList build_system_lines(double process_fps, double render_fps, const Options &options)
{
    return {
        QStringLiteral("FPS process %1 | render %2")
            .arg(process_fps, 0, 'f', 1)
            .arg(render_fps, 0, 'f', 1),
        QStringLiteral("fb %1 | %2x%3 | %4ms")
            .arg(QString::fromStdString(options.framebuffer_device))
            .arg(options.width)
            .arg(options.height)
            .arg(options.update_interval_ms),
    };
}

} // namespace

} // namespace cockscreen::tools::spi_linux_stats

int main(int argc, char *argv[])
{
    using namespace cockscreen::tools::spi_linux_stats;

    Options options;
    if (!parse_arguments(argc, argv, &options))
    {
        print_help();
        return 2;
    }

    if (options.help)
    {
        print_help();
        return 0;
    }

    if (options.daemonize && !daemonize_process())
    {
        return 2;
    }

    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
    {
        qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    }

    QGuiApplication app{argc, argv};
    (void)app;

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    cockscreen::runtime::pi::FramebufferMirror mirror{build_display_config(options), options.verbose};
    if (!mirror.ready())
    {
        std::cerr << "SPI stats framebuffer init failed for " << options.framebuffer_device
                  << ": " << mirror.status_message() << '\n';
        return 1;
    }

    if (options.verbose)
    {
        std::cerr << "[spi-linux-stats] ready on " << mirror.device_path()
                  << " (" << mirror.width() << "x" << mirror.height()
                  << " " << mirror.bits_per_pixel() << "bpp)\n";
    }

    using clock = std::chrono::steady_clock;
    auto previous_frame_time = clock::now();
    double smoothed_fps = 0.0;

    while (g_running.load())
    {
        const auto now = clock::now();
        const double delta_seconds = std::chrono::duration<double>(now - previous_frame_time).count();
        previous_frame_time = now;

        if (delta_seconds > 0.0)
        {
            const double instant_fps = 1.0 / delta_seconds;
            smoothed_fps = smoothed_fps <= 0.0 ? instant_fps : smoothed_fps * 0.85 + instant_fps * 0.15;
        }

        const QStringList system_lines = build_system_lines(smoothed_fps, smoothed_fps, options);
        if (!mirror.present_frame(QImage{}, cockscreen::core::ControlFrame{}, system_lines, {}))
        {
            std::cerr << "SPI stats present failed: " << mirror.status_message() << '\n';
            return 1;
        }

        if (options.once)
        {
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds{options.update_interval_ms});
    }
    return 0;
}

#else
int main()
{
    return 1;
}
#endif
