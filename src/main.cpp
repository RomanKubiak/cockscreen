#include "../include/cockscreen/app/CliSupport.hpp"
#include "../include/cockscreen/runtime/Application.hpp"

#include <filesystem>
#include <iostream>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

int main(int argc, char *argv[])
{
    namespace cli = cockscreen::app;

    const std::filesystem::path executable_directory = [argv]() {
#ifdef _WIN32
        wchar_t path[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, path, MAX_PATH) != 0)
        {
            return std::filesystem::path{path}.parent_path();
        }
#else
        std::error_code error;
        const auto resolved = std::filesystem::read_symlink("/proc/self/exe", error);
        if (!error)
        {
            return resolved.parent_path();
        }
#endif
        const auto fallback = std::filesystem::absolute(std::filesystem::path{argv[0]});
        return fallback.parent_path();
    }();

    if (cli::has_help_argument(argc, argv))
    {
        cli::print_help();
        return 0;
    }

    if (cli::has_list_devices_argument(argc, argv))
    {
        cli::print_device_list();
        return 0;
    }

    auto command_line = cli::parse_arguments(argc, argv, cockscreen::runtime::ApplicationSettings{});
    command_line.settings.executable_directory = executable_directory.string();

    if (command_line.help_requested)
    {
        cli::print_help();
        return 0;
    }

    if (command_line.settings.scene_file.empty())
    {
        const auto default_scene = cli::default_scene_file(executable_directory);
        if (!default_scene.has_value())
        {
            std::cerr << "No scene file specified and no default scene was found next to the executable or in the current workspace.\n";
            std::cerr << "Pass --scene-file PATH.\n";
            return 2;
        }

        command_line.settings.scene_file = default_scene->string();
    }

    if (command_line.list_devices_requested)
    {
        cli::print_device_list();
        return 0;
    }

    cockscreen::runtime::Application application{command_line.settings};
    return application.run(argc, argv);
}
