#include "../../include/cockscreen/core/ModulationBus.hpp"

namespace cockscreen::core
{

void ModulationBus::update(ControlFrame frame)
{
    std::scoped_lock lock(mutex_);
    latest_ = frame;
}

ControlFrame ModulationBus::snapshot() const
{
    std::scoped_lock lock(mutex_);
    return latest_;
}

} // namespace cockscreen::core
