# Navi2K Player

A native music player for [Navidrome](https://www.navidrome.org/) on **Windows 2000 SP4** and **Windows XP SP3**.

It runs on CPUs without SSE2, back to the Pentium II, Celeron, and Athlon. It needs no runtimes, no .NET, and no DirectX.

## Features

- Browse by album, artist, or song, and search within each view
- Album and artist art
- Playlists: create, rename, delete, add tracks, and reorder
- Star ratings, synced to Navidrome
- Shuffle, seeking, and an app-only volume control

## Requirements

- Windows 2000 SP4 or Windows XP SP3
- An i686-class CPU (Pentium Pro or newer)
- A Navidrome server with ffmpeg available. Navidrome converts every track to MP3 for the player; this is enabled by default.

## Setup

1. Copy `Navi2KPlayer.exe` to any folder and run it.
2. On first run it creates `Navi2KPlayer.ini` next to the exe. Open it and fill in your server details:

   ```ini
   Url=http://192.168.1.10:4533
   User=yourname
   Password=yourpassword
   ```

3. Restart the player, or press **F5** to connect.

After the first connection, the player replaces your password in the ini with a login token. Logs are written to `Navi2KPlayer.log`.

## Configuration

| Setting | Default | What it does |
|---|---|---|
| `Url` | — | Your Navidrome server address |
| `User` / `Password` | — | Your Navidrome login |
| `LanUrl` | — | A plain-HTTP fallback address on your local network |
| `AllowHttpFallback` | `0` | Set to `1` to use `LanUrl` when HTTPS fails. Only works for local addresses. |
| `AllowUntrustedCert` | `0` | Set to `1` to accept self-signed certificates |
| `[Player] Shuffle` | `0` | Remembers the shuffle setting |
| `[Player] Volume` | — | Remembers the volume level |

### HTTP vs. HTTPS

Windows 2000 and XP only support old encryption that modern servers reject. **Use plain HTTP on your local network**, for example `http://<server-ip>:4533`.

If you need access from outside your network, put a reverse proxy on your LAN that accepts TLS 1.0 and forwards requests to Navidrome.

## Building

Build with 32-bit MinGW-w64 using the msvcrt runtime and win32 threads. UCRT builds won't run on Windows 2000.

```
cmake -G "MinGW Makefiles" -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Not done yet

- Settings dialog
- Gapless playback
- Scrobbling
- Drag-and-drop playlist reordering
