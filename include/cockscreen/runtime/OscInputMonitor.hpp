#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "../core/ControlFrame.hpp"
#include "Scene.hpp"

class QString;

namespace cockscreen::runtime
{

class OscInputMonitor final
{
  public:
    explicit OscInputMonitor(std::string endpoint, const std::vector<OscMapping> *scene_osc_mappings = nullptr);
    ~OscInputMonitor();

    OscInputMonitor(const OscInputMonitor &) = delete;
    OscInputMonitor &operator=(const OscInputMonitor &) = delete;

    [[nodiscard]] bool is_active() const;
    [[nodiscard]] QString status_message() const;
    [[nodiscard]] QString activity_message() const;
    [[nodiscard]] unsigned long message_count() const;
    [[nodiscard]] int address_count() const;

    void poll();
    void populate_frame(core::ControlFrame *frame) const;

  private:
    bool open_socket();
    void close_socket();
    void process_packet(const char *data, int size);

    const std::vector<OscMapping> *scene_osc_mappings_{nullptr};
    std::string endpoint_;
    int socket_fd_{-1};
    bool active_{false};
    std::string status_message_;
    std::string activity_message_;
    std::unordered_map<std::string, float> values_;
    unsigned long message_count_{0};
};

} // namespace cockscreen::runtime
