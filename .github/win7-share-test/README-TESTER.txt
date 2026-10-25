Acheron - Windows 7 screen sharing test
=======================================

What this is: a test build of Acheron that can share your screen, with its sound, on
Windows 7, and that decodes the video you watch on the graphics card. It writes down
everything about how that goes. You do not need to understand any of it; the developer
reads the report.

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
- You need someone in a voice call with you to tell you what your shared screen looks and
  sounds like on their end.

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
3. Then Acheron opens. Log in and join a voice call with someone. Share your screen with
   "Share audio" ticked, play a video or some music, and ask them:
     - does the picture look right, and does it keep up?
     - can they hear the video or music?
     - do they hear themselves, or anyone else in the call, a second time?
   If someone else streams or turns on a camera, watch that for a minute too.
4. Close Acheron normally (X button). The RUN-ME window then collects the report and runs
   dxdiag (that takes up to a minute).
5. Send the "report-for-developer" folder, plus a few words on the answers from step 3.

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
