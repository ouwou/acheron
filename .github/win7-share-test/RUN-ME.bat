@echo off
rem Runs the hardware check, then Acheron itself, then gathers what the developer needs into this folder.
setlocal
set HERE=%~dp0
set LOGDIR=%APPDATA%\ouwou\Acheron
set OUT=%HERE%report-for-developer
if not exist "%OUT%" mkdir "%OUT%"

echo Step 1 of 3: checking the graphics card (about a minute, no window opens)...
start "" /wait "%HERE%acheron.exe" --hwdiag-only
findstr /C:"[acheron.video]" /C:"=== Acheron startup" "%LOGDIR%\acheron.log" > "%OUT%\hardware-check.txt"
echo Done.
echo.

echo Step 2 of 3: starting Acheron. In a voice call with someone:
echo   1. Share your screen with "Share audio" ticked, play a video or some music,
echo      and ask whoever watches whether they hear it, and whether they hear
echo      themselves or the others in the call a second time.
echo   2. Watch someone else's stream or camera if you can.
echo Then close Acheron normally (the X button).
set ACHERON_NO_HWDIAG=1
start "" /wait "%HERE%acheron.exe"
set ACHERON_NO_HWDIAG=

echo Step 3 of 3: collecting the report (dxdiag takes up to a minute)...
findstr /C:"[acheron.video]" /C:"=== Acheron startup" /C:"Starting Acheron" /C:"[CRT]" /C:"[FTL]" /C:"own sound" /C:"output device" /C:"Screen share" /C:"Sound capture" /C:"could not capture the sound" "%LOGDIR%\acheron.log" > "%OUT%\video-log.txt"
copy /y "%LOGDIR%\acheron-crash-*.dmp" "%OUT%\" >nul 2>&1
start "" /wait dxdiag /whql:off /t "%OUT%\dxdiag.txt"
echo.
echo Done. Please send the folder "%OUT%" to the developer,
echo with a sentence about what the shared screen looked and sounded like to the others.
pause
