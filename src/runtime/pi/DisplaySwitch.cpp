#include "cockscreen/runtime/pi/DisplaySwitch.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <unistd.h>
#include <limits.h>  // NOLINT(modernize-deprecated-headers) — PATH_MAX

namespace cockscreen::runtime::pi
{

const char *preferred_connector_name()
{
    std::FILE *f = std::fopen(kDisplayPreferenceFile, "r"); // NOLINT(cppcoreguidelines-owning-memory)
    if (f == nullptr)
    {
        return kHdmiConnectorName;
    }
    static char buf[32]{};
    std::memset(buf, 0, sizeof(buf));
    std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f); // NOLINT(cppcoreguidelines-owning-memory)
    for (int i = static_cast<int>(std::strlen(buf)) - 1;
         i >= 0 && (buf[i] == '\n' || buf[i] == '\r' || buf[i] == ' '); --i)
        buf[i] = '\0';
    if (std::strcmp(buf, kCompositeConnectorName) == 0)
        return kCompositeConnectorName;
    return kHdmiConnectorName;
}

DisplaySwitchResult switch_display_output(DisplayOutput output, int argc, char **argv)
{
    const char *connector = (output == DisplayOutput::Hdmi) ? kHdmiConnectorName : kCompositeConnectorName;

    // Persist the preference — the restarted process reads this and sets up
    // the single-connector KMS config before QApplication is constructed.
    std::FILE *f = std::fopen(kDisplayPreferenceFile, "w"); // NOLINT(cppcoreguidelines-owning-memory)
    if (f == nullptr)
    {
        return {false, std::string{"Could not write display preference to "} + kDisplayPreferenceFile
                           + ": " + std::strerror(errno)};
    }
    std::fputs(connector, f);
    std::fflush(f);
    std::fclose(f); // NOLINT(cppcoreguidelines-owning-memory)

    // Resolve the absolute executable path via /proc/self/exe so execv works
    // regardless of how the process was invoked (sudo, relative path, etc.)
    char exe_path[PATH_MAX]{};
    const ssize_t len = ::readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len <= 0)
    {
        return {false, std::string{"readlink /proc/self/exe failed: "} + std::strerror(errno)};
    }
    exe_path[len] = '\0';

    // Close all file descriptors except stdin/stdout/stderr before execv.
    // This releases Qt's DRM file descriptors so the new process can claim the
    // shared CRTC (both HDMI and Composite use the same CRTC on vc4-kms-v3d).
    if (DIR *d = ::opendir("/proc/self/fd"))
    {
        const int dir_fd = ::dirfd(d);
        while (const struct dirent *entry = ::readdir(d))
        {
            const int fd = std::atoi(entry->d_name); // NOLINT(cert-err34-c)
            if (fd > STDERR_FILENO && fd != dir_fd)
                ::close(fd);
        }
        ::closedir(d);
    }

    // Replace argv[0] with the resolved absolute path, keep all other arguments.
    if (argc > 0 && argv != nullptr)
    {
        argv[0] = exe_path; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        ::execv(exe_path, argv); // NOLINT(concurrency-mt-unsafe)
        return {false, std::string{"execv("} + exe_path + ") failed: " + std::strerror(errno)};
    }
    // No argv — restart with just the binary (scene will be auto-discovered)
    char *minimal_argv[] = {exe_path, nullptr};
    ::execv(exe_path, minimal_argv); // NOLINT(concurrency-mt-unsafe)
    return {false, std::string{"execv("} + exe_path + ") failed: " + std::strerror(errno)};
}

} // namespace cockscreen::runtime::pi
