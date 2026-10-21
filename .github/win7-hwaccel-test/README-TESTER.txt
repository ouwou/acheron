Acheron - Windows 7 graphics card video test
=============================================

What this is: a test build of Acheron that tries to decode video (streams and cameras in
calls) on the graphics card on Windows 7, and writes down everything about how that goes.
You do not need to understand any of it; the developer reads the report.

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

Steps
-----
1. Double-click RUN-ME.bat (not acheron.exe).
2. It first checks the graphics card by itself for about a minute, with no window. It
   decodes the clips in "video-samples" and tries each video encoder. Nothing to do here.
3. Then Acheron opens. Log in as usual. If you can, join a voice call where someone shares
   their screen or camera, watch it for a minute or two (full screen for a bit, too), and
   note whether it looks right, stutters, shows green or garbled pictures, or nothing at all.
   Even without a call, the first step's report is most of what is needed.
4. Close Acheron normally (X button). The RUN-ME window then collects the report and runs
   dxdiag (that takes up to a minute).
5. Send the "report-for-developer" folder, plus a sentence about what you saw in step 3.

If Acheron crashes
------------------
Just close the error window. A crash dump (acheron-crash-*.dmp) is written and picked up
by RUN-ME.bat. If Acheron freezes instead of crashing, open Task Manager, right-click
acheron.exe on the Processes tab, choose "Create dump file", and send that file too
(Task Manager shows where it saved it).

What is in the report
---------------------
- hardware-check.txt and video-log.txt: only the lines of Acheron's log about video (no
  messages, no passwords or tokens). They name the graphics card, the driver version and
  Windows' version.
- dxdiag.txt: Windows' own DirectX report (includes the computer's name).
- acheron-crash-*.dmp: only if it crashed.

Optional extra runs (only if the developer asks)
-------------------------------------------------
Open a command prompt in this folder and set one of these before "RUN-ME.bat":
  set ACHERON_HW_DECODE_DEVICES=dxva2      try only the DXVA2 decoder
  set ACHERON_HW_DECODE_DEVICES=cuda       NVIDIA cards: try NVIDIA's own decoder instead
  set ACHERON_FFMPEG_LOG=debug             far more detail from ffmpeg
Turning graphics card decoding off for a comparison is in Acheron's settings (Video).
