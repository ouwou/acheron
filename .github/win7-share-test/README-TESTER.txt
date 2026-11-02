Acheron - Windows 7 screen sharing test
=======================================

What this is: a test build of Acheron with the fixes from your last report. It writes down
everything about how it goes. You do not need to understand any of it; the developer reads
the report.

What changed since the last build
---------------------------------
- Your card only half decodes H265 and leaves the rest to the processor. Acheron now notices
  a codec that keeps it busy and asks for a lighter one by itself, without any setting.
- Phone streams should no longer be sideways.
- Encoding your own share WAS on the graphics card last time (h264_nvenc). This build
  measures where the processor time goes instead.
- The technical settings moved: Settings, Video, then click "Advanced". There is a new
  "Receive with" row there. "Send with" never affected what you receive.

Where this came from
--------------------
GitHub built this folder from the public source code; BUILT-BY-GITHUB.txt links the exact
commit and build. Anyone with the GitHub CLI can check acheron.exe against that build with
the command written there.

Before you start
----------------
- Windows 7 SP1, 64-bit, with Windows Update's "Update for Universal C Runtime" (KB2999226)
  and the SHA-2 updates. If acheron.exe says a DLL like api-ms-win-crt-*.dll is missing,
  that update is what is missing.
- Keep the folder together: acheron.exe, the DLLs, and the "video-samples" folder must stay
  side by side.
- For the first test you need someone streaming, ideally the phone stream that came in as
  H265 last time. For the second, someone in a call to tell you what they see and hear.

What sharing can do on Windows 7
--------------------------------
- Whole screens only. Windows 7 has no way to share a single window.
- Anything drawn straight to the screen, such as a full-screen game or some video players,
  may show as black.
- The shared sound is everything your speakers or headphones play, with the voices of the
  people in the call taken back out. That last part is what most needs testing: tell the
  developer if anyone hears themselves, or the others, a second time.

Steps
-----
1. Double-click RUN-ME.bat (not acheron.exe).
2. It first checks the graphics card by itself for about a minute, with no window. Nothing
   to do here.
3. Then Acheron opens. Log in and do the two tests below.
4. Close Acheron normally (X button). The RUN-ME window then collects the report and runs
   dxdiag (that takes up to a minute).
5. Send the "report-for-developer" folder, plus your answers to the questions below.

What to test
------------
First test: watching. Leave every box under "Receive with" ticked for this.
  Open the stream, turn on Stats, and keep watching for at least a minute. Then answer:
    - Which codec did Stats show at the start, and which after a minute?
      It should change from H265 to H264 by itself after about 15 seconds.
    - Is the picture the right way up?
    - Roughly how much processor did Task Manager show before and after the change?
    - Did the picture freeze or break up when it changed, and for how long?
  If it stayed on H265: open Settings, Video, Advanced, untick H265 and AV1 under "Receive
  with", open the stream again, and say which codec Stats shows then.
  Acheron remembers the change until you close it, so restart it to see the change again.

Second test: sharing.
  Share your screen with "Share audio" ticked, play a video or some music, and ask them:
    - does the picture look right, and does it keep up? Is it sharp or blurry?
    - can they hear the video or music?
    - do they hear themselves, or anyone else in the call, a second time?
  Leave it running for a couple of minutes, with the picture changing.

If Acheron crashes
------------------
Just close the error window. A crash dump (acheron-crash-*.dmp) is written and picked up
by RUN-ME.bat. If Acheron freezes instead of crashing, open Task Manager, right-click
acheron.exe on the Processes tab, choose "Create dump file", and send that file too
(Task Manager shows where it saved it).

What is in the report
---------------------
- hardware-check.txt and video-log.txt: only the lines of Acheron's log about video, screen
  sharing and its sound (no messages, no passwords or tokens). They name the graphics card,
  the driver version and Windows' version.
- dxdiag.txt: Windows' own DirectX report (includes the computer's name).
- acheron-crash-*.dmp: only if it crashed.
