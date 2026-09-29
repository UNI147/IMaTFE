#include "engine/Engine.h"
#include "tests/Stage1Tests.h"

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

    if (!imatfe::tests::run_stage1_tests())
    {
        std::cerr << "IMaTFE - Stage 1 verification failed.\n";
        engine.shutdown();
        return 2;
    }

    engine.shutdown();

    std::cout << "IMaTFE - Shutdown complete.\n";
    return 0;
}
