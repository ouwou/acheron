# Fork additions

Everything specific to this fork lives in this folder so the daily sync with
ouwou/acheron rarely conflicts.

- `acheron.ico` - custom app icon (purple robot).
- `acheron.rc` - embeds it as `IDI_ICON1` on Windows. Qt's Windows platform plugin
  uses that resource name as the default icon for every window (title bar, taskbar,
  Alt+Tab), and Explorer shows it as the .exe icon.
- `tray-*.png` + `fork.qrc` - the same icon at each size, used for the system tray
  (`src/UI/TrayIcon.cpp` prefers `:/fork/tray-*.png` when present).

Upstream files touched: `CMakeLists.txt` (adds the two files above) and
`src/UI/TrayIcon.cpp` (tray icon lookup).
