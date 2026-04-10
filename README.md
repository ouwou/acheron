### Acheron
---
Alternative Discord client made in C++ with Qt 6

<img width="1528" height="864" alt="acheron_vofHKu0r4B" src="https://github.com/user-attachments/assets/f2a1bce5-4170-4207-86ce-3b35974f0f1b" />

<a href="https://discord.gg/wkCU3vuzG5"><img src="https://discord.com/api/guilds/858156817711890443/widget.png?style=shield"></a>

Current features:
* Not Electron
* No, not Tauri either
* Voice support
* Multi-account support
* Browser impersonation to avoid spam filter
* Per-channel tabs
* Discord-compatible markdown parsing
* Embed support
* Unread and mention indicators
* Guild folders
* Edit, delete, pin, reply, react
* Emoji support
* Image viewer
* Typing indicators

Planned features:
* Animated emojis
* Server management
* Notifications
* Sounds
* Threads
* Forums
* A lot of other stuff

### Downloads:

Latest nightly Windows build: https://nightly.link/ucjd/acheron/workflows/build/master/acheron-windows-MinSizeRel.zip

### Dependencies:

* Qt 6.9+ (I am considering supporting compatibility with Qt 5. If a lower Qt 6 version is something you require, open an issue)
* libcurl-impersonate (technically just libcurl is supported but you should use libcurl-impersonate)
* zlib (either via Qt ZlibPrivate or system)
* QtKeychain
* libsodium (optional, voice support)
* libopus (optional, voice support)
* libdave (optional, voice support)
* miniaudio (optional, voice support, vendored)
* emoji-segmenter (vendored)

### Build Instructions:

Later
