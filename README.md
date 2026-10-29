# Navi2K Player

A native music player for [Navidrome](https://www.navidrome.org/) on Windows 2000 SP4 and XP SP3. It runs on CPUs without SSE2 and requires no extra runtimes.

## Features

- Browse by album, artist, or song, and search within each view
- Album and artist art (artists without a photo get a collage of their album covers)
- Right-click any track to go to its album or artist
- Optional album and artist descriptions from Last.fm (off by default; File > Settings > Display)
- Playlists: create, rename, delete, add tracks, and reorder (drag and drop), plus a description shown under the playlist's title
- Star ratings and favorites (♥), synced to Navidrome, with a Favorites section in the library
- Scrobbling: plays count in Navidrome (and Last.fm, if linked there)
- Play queue (**Ctrl+Q**): Play next, Add to queue (tracks, albums, playlists), reorder (drag and drop), and remove
- Mini player (**Ctrl+M**) with cover art, seek bar, and transport controls
- Notification-area icon with playback controls, and optional minimize-to-tray
- Shuffle, repeat (all / one), seeking, and an app-only volume control
- Gapless playback: the next track starts with no pause between them
- Keyboard shortcuts and media keys (see below)
- Sort any list by clicking a column header
- Optional lossless playback: FLAC files stream untouched and are decoded on your PC
- The title bar shows the current song and artist
- A Settings dialog for the server, playback, and display options

## Requirements

- An i686-class CPU (Pentium Pro or newer)
- A Navidrome server with ffmpeg available (it is by default). Navidrome converts tracks to MP3 for the player, except FLAC files when Lossless is on.

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
| `[Server] Lossless` | `0` | Set to `1` to play FLAC files as they are (about 1 Mbps). Other formats, and FLAC above 48 kHz or with more than 2 channels, still stream as MP3. |
| `[Player] OutputDevice` | system default | Sound device to play through |
| `[Player] Buffers` | `12` | Audio buffer size: 8 (about 0.4 s) to 32 (about 1.5 s). Raise it on a slow network. |
| `[Player] Gapless` | `1` | Plays the next track with no pause between them. The next track starts loading 8 seconds before the current one ends. |
| `[Player] Scrobble` | `1` | Reports plays to Navidrome. A play counts after half the track or 4 minutes. |
| `[View] ShowFavorites` | `1` | Shows the ♥ column and the Favorites section |
| `[View] ShowRatings` | `1` | Shows the star rating column |
| `[View] LastFmInfo` | `0` | Set to `1` to show album and artist descriptions from Last.fm. Navidrome fetches them, so Last.fm must be turned on there. |
| `[View] TrayIcon` | `1` | Shows an icon in the notification area |
| `[View] MinimizeToTray` | `0` | Set to `1` to hide the player in the notification area when minimized |
| `[Player] GlobalMediaKeys` | `1` | Media keys control the player even when it's in the background. Set to `0` to leave them for other programs. |
| `[Player] Shuffle` | `0` | Remembers the shuffle setting |
| `[Player] Repeat` | `0` | Remembers repeat: `0` off, `1` all, `2` one |
| `[Player] Volume` | `100` | Remembers the volume level |

## Keyboard shortcuts

| Keys | Action |
|---|---|
| Ctrl+P (or Space in a track list) | Play / pause |
| Ctrl+S | Stop |
| Ctrl+B / Ctrl+N | Previous / next |
| Ctrl+H | Shuffle on or off |
| Ctrl+T | Repeat: off, all, one |
| Ctrl+Q | Play Queue |
| Ctrl+M | Mini player |
| Ctrl+F | Search |
| F5 | Reconnect |

The keyboard's media keys (play/pause, stop, previous, next) also work.

### HTTP vs. HTTPS

Windows 2000 and XP only support old encryption that modern servers reject. **Use plain HTTP on your local network**, for example `http://<server-ip>:4533`.

If you need access from outside your network, put a reverse proxy on your LAN that accepts TLS 1.0 and forwards requests to Navidrome.

## Building

Build with 32-bit MinGW-w64 using the msvcrt runtime and win32 threads. UCRT builds won't run on Windows 2000.

```
cmake -G "MinGW Makefiles" -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```
