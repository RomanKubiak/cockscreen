#include "../include/cockscreen/app/CliSupport.hpp"
#include "../include/cockscreen/runtime/Application.hpp"
#include "../include/cockscreen/runtime/Scene.hpp"

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

    const auto config_selection = cli::select_config_file(argc, argv, executable_directory);
    if (!config_selection.path.has_value())
    {
        std::cerr << "No config file found. Tried:\n"
                  << "  " << (executable_directory / "cockscreen.ini").string() << "\n"
#ifdef _WIN32
                  << "  %APPDATA%\\cockscreen\\config.ini\n"
                  << "  %USERPROFILE%\\cockscreen\\config.ini\n";
#else
                  << "  $HOME/.config/cockscreen/config.ini\n";
#endif
        std::cerr << "Pass --config-file PATH to specify one explicitly.\n";
        return 2;
    }

    cockscreen::runtime::ApplicationSettings initial_settings;
    if (config_selection.path.has_value() && !cli::load_config_file(*config_selection.path, &initial_settings))
    {
        return 2;
    }

    auto command_line = cli::parse_arguments(argc, argv, std::move(initial_settings));
    command_line.settings.executable_directory = executable_directory.string();

    if (command_line.settings.shader_directory.empty())
    {
        command_line.settings.shader_directory = command_line.settings.executable_directory;
    }

    cli::enable_shader_render_path_if_requested(&command_line.settings);

    if (command_line.help_requested)
    {
        cli::print_help();
        return 0;
    }

    if (command_line.list_devices_requested)
    {
        cli::print_device_list();
    }

    if (command_line.list_capture_modes_requested)
    {
        if (command_line.settings.scene_file.empty())
        {
            std::cerr
                << "Capture mode listing requires runtime.scene_file in config (or --scene-file) with inputs.video.device set\n";
            return 2;
        }

        std::filesystem::path scene_path{command_line.settings.scene_file};
        if (!scene_path.is_absolute())
        {
            const auto resolved_scene_path = cli::resolve_relative_path(scene_path);
            if (!resolved_scene_path.has_value())
            {
                std::cerr << "Scene file not found: " << command_line.settings.scene_file << '\n';
                return 2;
            }

            scene_path = *resolved_scene_path;
        }

        std::string scene_error;
        const auto loaded_scene = cockscreen::runtime::load_scene_definition(scene_path, &scene_error);
        if (!loaded_scene.has_value())
        {
            std::cerr << scene_error << '\n';
            return 2;
        }

        if (loaded_scene->video_input.device.empty())
        {
            std::cerr << "Capture mode listing requires inputs.video.device in scene: " << scene_path.string() << '\n';
            return 2;
        }

        command_line.settings.video_device = loaded_scene->video_input.device;
        cli::print_capture_modes(command_line.settings);
    }

    if (command_line.list_devices_requested || command_line.list_capture_modes_requested)
    {
        return 0;
    }

    cockscreen::runtime::Application application{command_line.settings};
    return application.run(argc, argv);
}
