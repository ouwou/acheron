### Acheron
---
Alternative Discord client made in C++ with Qt 6 (+ Qt 5 compatible)

<img width="1528" height="864" alt="acheron_vofHKu0r4B" src="https://github.com/user-attachments/assets/f2a1bce5-4170-4207-86ce-3b35974f0f1b" />

<a href="https://discord.gg/wkCU3vuzG5"><img src="https://discord.com/api/guilds/858156817711890443/widget.png?style=shield"></a>

Current features:
* Not Electron
* No, not Tauri either
* Cross-platform support (Win 7+, Linux, macOS)
* Voice support (E2EE & noise suppression)
* Multi-account support
* Browser impersonation to avoid spam filter
* Per-channel tabs
* Discord-compatible markdown parsing
* Embed support
* File upload support
* Unread and mention indicators
* Guild folders
* Edit, delete, pin, reply, react
* Emoji support
* Image viewer
* Typing indicators
* DMs and group DMs
* Threads
* Forums
* QR code login

Planned features:
* Animated emojis
* Server management
* Notifications
* Sounds
* A lot of other stuff

### Downloads:

Latest nightly Windows build: https://nightly.link/ouwou/acheron/workflows/build/master/acheron-windows-MinSizeRel.zip

### Dependencies:

* Qt 6.7+ or Qt 5.14+
* libcurl-impersonate (technically just libcurl is supported but you should use libcurl-impersonate)
* OpenSSL
* nayuki/QR-Code-generator (vendored)
* zlib (either via Qt ZlibPrivate or system)
* QtKeychain
* emoji-segmenter (vendored)
* libsodium (optional, voice support)
* libopus (optional, voice support)
* libdave (optional, voice support, vendored)
* mlspp (optional, voice support, vendored)
* miniaudio (optional, voice support, vendored)
* rnnoise (optional, noise suppression, vendored)

### Build Instructions:

Later
