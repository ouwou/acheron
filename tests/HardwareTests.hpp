#pragma once

#include <QTest>

// the tests that need this machine's graphics card, screens, camera or sound take minutes, open windows and play a tone
inline bool hardwareTestsAreAskedFor()
{
    return qEnvironmentVariableIntValue("ACHERON_HARDWARE_TESTS") > 0;
}

#define SKIP_UNLESS_HARDWARE_IS_ASKED_FOR() \
    if (!hardwareTestsAreAskedFor())        \
    QSKIP("needs this machine's own hardware, which ACHERON_HARDWARE_TESTS=1 asks for")
