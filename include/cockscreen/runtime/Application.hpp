#pragma once

#include <string>

#include "../core/ModulationBus.hpp"

namespace cockscreen::runtime
{

struct ApplicationSettings
{
    std::string video_device{"/dev/video0"};
    std::string audio_device{};
    std::string osc_endpoint{"0.0.0.0:9000"};
    std::string midi_input{};
    std::string scene_file{};
    std::string shader_directory{};
    std::string executable_directory{};
    std::string shader_file{};
    std::string screen_shader_file{};
    std::string web_server_bind_url{};
    bool scene_file_is_read_only{false};
    std::string top_layer{"screen"};
    double top_layer_opacity{0.75};
    std::string render_path{"qt"};
    int width{1024};
    int height{600};
    int frame_rate{30};
};

class Application
{
  public:
    explicit Application(ApplicationSettings settings);

    [[nodiscard]] int run(int argc, char *argv[]);

  private:
    ApplicationSettings settings_;
    core::ModulationBus modulation_bus_;
};

} // namespace cockscreen::runtime
