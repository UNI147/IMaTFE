#include "Engine.h"

namespace imatfe
{

bool Engine::initialize()
{
    initialized_ = true;

    return true;
}

void Engine::shutdown()
{
    if (!initialized_)
        return;

    initialized_ = false;
}

} // namespace imatfe