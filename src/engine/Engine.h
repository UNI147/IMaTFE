#pragma once

#include "engine/bus/PSXBus.h"

namespace imatfe
{

class Engine
{
public:
    Engine() = default;
    ~Engine() = default;

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    bool initialize();
    void shutdown();

    bus::PSXBus& bus() noexcept { return bus_; }
    const bus::PSXBus& bus() const noexcept { return bus_; }

private:
    bus::PSXBus bus_;
    bool initialized_ = false;
};

} // namespace imatfe
