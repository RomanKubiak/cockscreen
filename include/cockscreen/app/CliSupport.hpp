#pragma once

#include "cockscreen/runtime/Application.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cockscreen::app
{

struct CommandLine
{
    runtime::ApplicationSettings settings;
    bool help_requested{false};
    bool list_devices_requested{false};
    bool list_capture_modes_requested{false};
};

struct ConfigFileSelection
{
    std::optional<std::filesystem::path> path;
    bool explicit_requested{false};
};

bool has_help_argument(int argc, char *argv[]);
bool has_list_devices_argument(int argc, char *argv[]);
std::optional<std::filesystem::path> resolve_relative_path(const std::filesystem::path &path);
ConfigFileSelection select_config_file(int argc, char *argv[],
                                       const std::filesystem::path &executable_dir = {});
bool load_config_file(const std::filesystem::path &path, runtime::ApplicationSettings *settings);
void enable_shader_render_path_if_requested(runtime::ApplicationSettings *settings);

std::optional<std::string> detect_default_audio_device();
std::vector<std::string> detect_midi_devices();
std::optional<std::string> detect_default_midi_device();

CommandLine parse_arguments(int argc, char *argv[], runtime::ApplicationSettings settings);
void print_help();
void print_capture_modes(const runtime::ApplicationSettings &settings);
void print_device_list();

} // namespace cockscreen::app