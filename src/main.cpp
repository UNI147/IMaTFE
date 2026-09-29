#include "engine/Engine.h"
#include "engine/cpu/CPU.h"
#include "tests/Stage1Tests.h"
#include "tests/Stage2Tests.h"

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

    {
        imatfe::core::psx::Memory memory;
        imatfe::cpu::CPU cpu(memory);
        cpu.reset(0x80000000u);
    }

    if (!imatfe::tests::run_stage1_tests())
    {
        std::cerr << "IMaTFE - Stage 1 verification failed.\n";
        engine.shutdown();
        return 2;
    }

    if (!imatfe::tests::run_stage2_tests())
    {
        std::cerr << "IMaTFE - Stage 2 verification failed.\n";
        engine.shutdown();
        return 3;
    }

    engine.shutdown();

    std::cout << "IMaTFE - Shutdown complete.\n";
    return 0;
}
