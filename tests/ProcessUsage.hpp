#pragma once

#include <QDir>
#include <QElapsedTimer>
#include <QtGlobal>

#ifdef Q_OS_WIN
#include <windows.h>

#include <tlhelp32.h>
#else
#include <ctime>
#endif

namespace ProcessUsage {

#ifdef Q_OS_WIN
// counted in cycles: Windows' thread times are sampled at its clock tick, which misses threads that
// wake for a few milliseconds at a time
inline double processCpuMs()
{
    static const double cyclesPerMs = [] {
        constexpr qint64 CALIBRATION_NS = 20'000'000;
        ULONG64 before = 0;
        ULONG64 after = 0;
        QueryThreadCycleTime(GetCurrentThread(), &before);
        QElapsedTimer spinning;
        spinning.start();
        while (spinning.nsecsElapsed() < CALIBRATION_NS) {}
        QueryThreadCycleTime(GetCurrentThread(), &after);
        return static_cast<double>(after - before) / (static_cast<double>(CALIBRATION_NS) / 1e6);
    }();

    ULONG64 cycles = 0;
    QueryProcessCycleTime(GetCurrentProcess(), &cycles);
    return static_cast<double>(cycles) / cyclesPerMs;
}

inline int threadsOfThisProcess()
{
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 entry = {};
    entry.dwSize = sizeof(entry);
    int count = 0;
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID == GetCurrentProcessId())
            count++;
    }
    CloseHandle(snapshot);
    return count;
}
#else
inline double processCpuMs()
{
    timespec used = {};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &used);
    return static_cast<double>(used.tv_sec) * 1e3 + static_cast<double>(used.tv_nsec) / 1e6;
}

inline int threadsOfThisProcess()
{
    return static_cast<int>(QDir(QStringLiteral("/proc/self/task")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size());
}
#endif

} // namespace ProcessUsage
