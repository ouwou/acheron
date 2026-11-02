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

echo Step 2 of 3: starting Acheron. See "What to test" in README-TESTER.txt:
echo   1. Watch a stream that arrives as H265, with Stats on, for a minute.
echo   2. Share your screen with "Share audio" ticked and ask whoever watches
echo      what they see and hear.
echo Then close Acheron normally (the X button).
set ACHERON_NO_HWDIAG=1
start "" /wait "%HERE%acheron.exe"
set ACHERON_NO_HWDIAG=

echo Step 3 of 3: collecting the report (dxdiag takes up to a minute)...
findstr /C:"[acheron.video]" /C:"=== Acheron startup" /C:"Starting Acheron" /C:"[CRT]" /C:"[FTL]" /C:"own sound" /C:"output device" /C:"Screen share" /C:"Sound capture" /C:"could not capture the sound" /C:"Withdrawing" /C:"decoder backlog" "%LOGDIR%\acheron.log" > "%OUT%\video-log.txt"
copy /y "%LOGDIR%\acheron-crash-*.dmp" "%OUT%\" >nul 2>&1
start "" /wait dxdiag /whql:off /t "%OUT%\dxdiag.txt"
echo.
echo Done. Please send the folder "%OUT%" to the developer,
echo with your answers to the questions in README-TESTER.txt.
pause
