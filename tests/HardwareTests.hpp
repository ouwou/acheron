#pragma once

#include <QTest>

// tests needing this machine's graphics card, screens, camera or sound run only with
// ACHERON_HARDWARE_TESTS=1: they take minutes, open windows and play a tone
inline bool hardwareTestsAreAskedFor()
{
    return qEnvironmentVariableIntValue("ACHERON_HARDWARE_TESTS") > 0;
}

#define SKIP_UNLESS_HARDWARE_IS_ASKED_FOR() \
    if (!hardwareTestsAreAskedFor())        \
    QSKIP("needs this machine's own hardware, which ACHERON_HARDWARE_TESTS=1 asks for")
