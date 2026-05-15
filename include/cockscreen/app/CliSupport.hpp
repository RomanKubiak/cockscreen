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
};

bool has_help_argument(int argc, char *argv[]);
bool has_list_devices_argument(int argc, char *argv[]);
std::optional<std::filesystem::path> resolve_relative_path(const std::filesystem::path &path);
std::optional<std::filesystem::path> default_scene_file(const std::filesystem::path &executable_dir);

std::optional<std::string> detect_default_audio_device();
std::vector<std::string> detect_midi_devices();
std::optional<std::string> detect_default_midi_device();

#ifndef _WIN32
struct RpiCameraDevice
{
    std::string path;
    std::string driver;
    std::string card;
    bool requires_media_controller{false};
    std::vector<std::string> modes;
};

std::vector<RpiCameraDevice> detect_rpi_cameras();
#endif

CommandLine parse_arguments(int argc, char *argv[], runtime::ApplicationSettings settings);
void print_help();
void print_device_list();

} // namespace cockscreen::app