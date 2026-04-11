#pragma once

#include <mutex>

#include "ControlFrame.hpp"

namespace cockscreen::core
{

class ModulationBus
{
  public:
    void update(ControlFrame frame);

    [[nodiscard]] ControlFrame snapshot() const;

  private:
    mutable std::mutex mutex_;
    ControlFrame latest_{};
};

} // namespace cockscreen::core
