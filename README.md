# Navi2K Player

A native music player for [Navidrome](https://www.navidrome.org/) on Windows 2000 and XP. It runs on CPUs without SSE2 and requires no extra runtimes.

## Features

- Browse by album, artist, or song, and search within each view
- Album and artist art
- Playlists: create, rename, delete, add tracks, and reorder
- Star ratings and favorites (♥), synced to Navidrome, with a Favorites section in the library
- Scrobbling: plays count in Navidrome (and Last.fm, if linked there)
- Mini player (**Ctrl+M**) with cover art, seek bar, and transport controls
- Shuffle, seeking, and an app-only volume control
- The title bar shows the current song and artist
- A Settings dialog for the server, playback, and display options

## Requirements

- An i686-class CPU (Pentium Pro or newer)
- A Navidrome server with ffmpeg available. Navidrome converts every track to MP3 for the player; this is enabled by default.

## Setup

1. Copy `Navi2KPlayer.exe` to any folder and run it.
2. On first run, the Settings dialog opens. Enter your server address (for example `http://192.168.1.10:4533`), user name, and password. **Test Connection** checks them before you save.
3. Click **OK** to connect.

Settings are saved in `Navi2KPlayer.ini` next to the exe. Your password is never stored, only a login token. Logs are written to `Navi2KPlayer.log`.

## Configuration

Everything below can be changed in **File > Settings**, or by editing the ini directly.

| Setting | Default | What it does |
|---|---|---|
| `[Server] Url` | — | Your Navidrome server address |
| `[Server] User` | — | Your Navidrome user name. The password is entered in Settings. |
| `[Server] LanUrl` | — | A plain-HTTP fallback address on your local network |
| `[Server] AllowHttpFallback` | `0` | Set to `1` to use `LanUrl` when HTTPS fails. Only works for local addresses. |
| `[Server] AllowUntrustedCert` | `0` | Set to `1` to accept self-signed certificates |
| `[Server] MaxBitRate` | `192` | Streaming quality in kbps (96–320) |
| `[Player] OutputDevice` | system default | Sound device to play through |
| `[Player] Buffers` | `12` | Audio buffer size: 8 (about 0.4 s) to 32 (about 1.5 s). Raise it on a slow network. |
| `[Player] Scrobble` | `1` | Reports plays to Navidrome. A play counts after half the track or 4 minutes. |
| `[View] ShowFavorites` | `1` | Shows the ♥ column and the Favorites section |
| `[View] ShowRatings` | `1` | Shows the star rating column |
| `[Player] Shuffle` | `0` | Remembers the shuffle setting |
| `[Player] Volume` | `100` | Remembers the volume level |

### HTTP vs. HTTPS

Windows 2000 and XP only support old encryption that modern servers reject. **Use plain HTTP on your local network**, for example `http://<server-ip>:4533`.

If you need access from outside your network, put a reverse proxy on your LAN that accepts TLS 1.0 and forwards requests to Navidrome.

## Building

Build with 32-bit MinGW-w64 using the msvcrt runtime and win32 threads. UCRT builds won't run on Windows 2000.

```
cmake -G "MinGW Makefiles" -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```