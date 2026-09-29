#pragma once

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

private:
    bool initialized_ = false;
};

} // namespace imatfe