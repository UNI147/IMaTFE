#include "engine/Engine.h"

#include <iostream>

int main()
{
    std::cout << "IMaTFE - Initializing...\n";

    imatfe::Engine engine;

    if (!engine.initialize())
    {
        std::cerr << "IMaTFE - Initialization failed.\n";
        return 1;
    }

    std::cout << "IMaTFE - Initialization successful.\n";

    engine.shutdown();

    std::cout << "IMaTFE - Shutdown complete.\n";

    return 0;
}