#include "Engine.h"

#include <iostream>

namespace imatfe
{

bool Engine::initialize()
{
    std::cout << "Engine::initialize()\n";

    initialized_ = true;

    return true;
}

void Engine::shutdown()
{
    if (!initialized_)
        return;

    std::cout << "Engine::shutdown()\n";

    initialized_ = false;
}

} // namespace imatfe