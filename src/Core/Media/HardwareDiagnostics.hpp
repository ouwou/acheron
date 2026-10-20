#pragma once

#include <QStringList>

namespace Acheron {
namespace Core {
namespace Media {

// TEMPORARY, for bringing hardware video up on Windows 7 with a tester who has the machine:
// everything here writes to acheron.log with "[hwdiag]" in the line, so that one log answers
// what the machine has and what was tried on it. Remove once Windows 7 is known to work.
//
// ffmpeg's own messages go to the log from the start (ACHERON_FFMPEG_LOG=quiet|error|warning|
// info|verbose|debug, verbose by default). On a thread of its own, the machine is described
// (Windows, processor, graphics adapters and drivers, Direct3D 9 / 11 video decoding profiles,
// Media Foundation, NVENC, AMF and Quick Sync runtimes), every encoder is put through its trial,
// and each clip in a "video-samples" folder beside the executable is decoded in software and
// then with the graphics card, and the two compared.
void startHardwareDiagnostics();
// Waits for that thread, which stops early when asked to here. Before the log is closed.
void finishHardwareDiagnostics();

// For "acheron.exe --hwdiag-only": the description and trials without logging in, then the
// process ends at once (a hardware encoder's trial thread can keep it from ending otherwise).
[[nodiscard]] bool onlyDiagnosticsWereAskedFor(const QStringList &arguments);
[[noreturn]] void runOnlyDiagnosticsAndExit();

} // namespace Media
} // namespace Core
} // namespace Acheron
