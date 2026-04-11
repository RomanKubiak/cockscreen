#include "../include/cockscreen/app/CliSupport.hpp"
#include "../include/cockscreen/runtime/Application.hpp"

#include <filesystem>
#include <iostream>
#include <system_error>

int main(int argc, char *argv[])
{
    namespace cli = cockscreen::app;

    const std::filesystem::path executable_directory = [argv]() {
        std::error_code error;
        const auto resolved = std::filesystem::read_symlink("/proc/self/exe", error);
        if (!error)
        {
            return resolved.parent_path();
        }

        const auto fallback = std::filesystem::absolute(std::filesystem::path{argv[0]}, error);
        if (!error)
        {
            return fallback.parent_path();
        }

        return std::filesystem::current_path();
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

    const auto config_selection = cli::select_config_file(argc, argv);
    if (!config_selection.path.has_value())
    {
        std::cerr << "No config file found on the command line or in $HOME/.config/cockscreen/config.ini\n";
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
        cli::print_capture_modes(command_line.settings);
    }

    if (command_line.list_devices_requested || command_line.list_capture_modes_requested)
    {
        return 0;
    }

    cockscreen::runtime::Application application{command_line.settings};
    return application.run(argc, argv);
}
