#include "engine/Engine.h"
#include "engine/cpu/CPU.h"
#include "tests/Stage1Tests.h"
#include "tests/Stage2Tests.h"
#include "tests/Stage3Tests.h"
#include "tests/Stage4Tests.h"
#include "tests/Stage5Tests.h"
#include "tests/Stage6Tests.h"
#include "tests/Stage7Tests.h"

#include <iostream>

int main()
{
    imatfe::Engine engine;

    if (!engine.initialize())
    {
        std::cerr << "IMaTFE - Initialization failed.\n";
        return 1;
    }

    {
        imatfe::cpu::CPU cpu(engine.bus());
        cpu.reset(0x80000000u);
    }

    bool all_passed = true;

    if (!imatfe::tests::run_stage1_tests())
    {
        std::cerr << "IMaTFE - Stage 1 verification failed.\n";
        all_passed = false;
    }

    if (!imatfe::tests::run_stage2_tests())
    {
        std::cerr << "IMaTFE - Stage 2 verification failed.\n";
        all_passed = false;
    }

    if (!imatfe::tests::run_stage3_tests())
    {
        std::cerr << "IMaTFE - Stage 3 verification failed.\n";
        all_passed = false;
    }

    if (!imatfe::tests::run_stage4_tests())
    {
        std::cerr << "IMaTFE - Stage 4 verification failed.\n";
        all_passed = false;
    }

    if (!imatfe::tests::run_stage5_tests())
    {
        std::cerr << "IMaTFE - Stage 5 verification failed.\n";
        all_passed = false;
    }

    if (!imatfe::tests::run_stage6_tests())
    {
        std::cerr << "IMaTFE - Stage 6 verification failed.\n";
        all_passed = false;
    }

    if (!imatfe::tests::run_stage7_tests())
    {
        std::cerr << "IMaTFE - Stage 7 verification failed.\n";
        all_passed = false;
    }

    engine.shutdown();

    return all_passed ? 0 : 2;
}
