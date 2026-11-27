# Fork additions

Everything specific to this fork lives in this folder so the daily sync with
ouwou/acheron rarely conflicts.

- `acheron.ico` - custom app icon (purple robot).
- `acheron.rc` - embeds it as `IDI_ICON1` on Windows. Qt's Windows platform plugin
  uses that resource name as the default icon for every window (title bar, taskbar,
  Alt+Tab), and Explorer shows it as the .exe icon.
- `tray-*.png` + `fork.qrc` - the same icon at each size, used for the system tray
  (`src/UI/TrayIcon.cpp` prefers `:/fork/tray-*.png` when present).
- `AboutPage.cpp/.hpp` - Settings > About: build number and workflow, build date,
  commit, configuration and CPU architecture, Qt/libcurl versions, OS, and a
  "Copy build info" button.
- `fork.cmake` - adds all of the above to the build and passes the build information
  (from GitHub Actions' environment, or the local git checkout) to the About page.

- `UpdateChecker.cpp/.hpp` + `updater/` - the built-in updater. `acheron-updater(.exe)` is built
  next to `acheron.exe` (only Qt Core + Qt Network) and does the GitHub lookups, download,
  SHA-256 check, backup, file swap and rollback; Acheron runs `--check` 15 s after launch, every
  6 hours and from "Check for Updates" (tray and View menu), shows the prompt, and on "Update Now"
  starts `--install` and quits. State is shared with the old PowerShell updater
  (`%LOCALAPPDATA%\AcheronUpdater\state.json`); `updater/Retire-PowerShellUpdater.ps1` removes its
  scheduled task and shortcut. Command line: `--check [--manual]`, `--skip <run>`,
  `--install <run> [--wait-pid <pid>] [--relaunch]`, `--rollback`, `--source Upstream|Fork`,
  `--flavor <name>`, `--fork-repo <owner/repo>`.

Fork-only workflows: `.github/workflows/sync-upstream.yml` (daily merge of
ouwou/acheron) and `.github/workflows/build-windows-arm64.yml` (native ARM64 build).

Upstream files touched, kept to a line or two each so syncing rarely conflicts:
- `CMakeLists.txt` - `include(fork/fork.cmake)`
- `src/UI/TrayIcon.cpp` - tray icon lookup
- `src/UI/Settings/SettingsWindow.cpp` - include + `addPage(tr("About"), ...)`
- `src/UI/MainWindow.cpp/.hpp`, `src/UI/TrayIcon.cpp/.hpp` - create the update checker, add
  "Check for Updates" to the View and tray menus
- `.github/workflows/build.yml` - `if:` lines skipping the macOS/Linux/AppImage jobs
