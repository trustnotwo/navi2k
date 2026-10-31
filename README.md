# Navi2K Player

A native music player for [Navidrome](https://www.navidrome.org/) on Windows 2000 and up. It runs on CPUs without SSE2 and requires no extra runtimes.

## Features

- Browse albums, artists, and songs, with search, cover art, and Discover lists
- Playlists you can create, edit, and reorder, plus a play queue
- Download tracks, albums, and playlists for offline listening
- Star ratings, favorites, and scrobbling, synced with Navidrome
- Optional lossless FLAC playback, and gapless playback
- Choose which libraries and sidebar sections to show
- Mini player, tray icon, keyboard shortcuts, and media keys

## Requirements

- Windows 2000 and up
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
| `[Server] Libraries` | all | Comma-separated ids of the libraries to show (File > Settings > Libraries lists them by name). Empty or missing shows every library. |
| `[Server] Lossless` | `0` | Set to `1` to play FLAC files as they are (about 1 Mbps). Other formats, and FLAC above 48 kHz or with more than 2 channels, still stream as MP3. |
| `[Player] OutputDevice` | system default | Sound device to play through |
| `[Player] Buffers` | `12` | Audio buffer size: 8 (about 0.4 s) to 32 (about 1.5 s). Raise it on a slow network. |
| `[Player] Gapless` | `1` | Plays the next track with no pause between them. The next track starts loading 8 seconds before the current one ends. |
| `[Player] Scrobble` | `1` | Reports plays to Navidrome. A play counts after half the track or 4 minutes. |
| `[View] ShowFavorites` | `1` | Shows the ♥ column and the Favorites section |
| `[View] HiddenSections` | `added,played,frequent,random` | Sidebar sections to hide, comma-separated (empty shows them all): `albums`, `artists`, `songs`, `favorites`, `added`, `played`, `frequent`, `random`, `queue`, `downloads` |
| `[View] ShowRatings` | `1` | Shows the star rating column |
| `[View] LastFmInfo` | `0` | Set to `1` to show album and artist descriptions from Last.fm. Navidrome fetches them, so Last.fm must be turned on there. |
| `[View] TrayIcon` | `1` | Shows an icon in the notification area |
| `[View] MinimizeToTray` | `0` | Set to `1` to hide the player in the notification area when minimized |
| `[Player] GlobalMediaKeys` | `1` | Media keys control the player even when it's in the background. Set to `0` to leave them for other programs. |
| `[Player] Shuffle` | `0` | Remembers the shuffle setting |
| `[Player] Repeat` | `0` | Remembers repeat: `0` off, `1` all, `2` one |
| `[Downloads] Quality` | `0` | Quality of new downloads: `0` the streaming quality, `1` Lossless (FLAC as is), or an MP3 bitrate in kbps (96–320) |
| `[Downloads] PlayDownloaded` | `0` | Set to `1` to play downloaded copies even while connected. Otherwise tracks stream at the Playback quality and downloads are used only offline. |
| `[Downloads] Folder` | `Downloads` next to the exe | Where downloaded tracks are kept, as `Artist\Album\01 Title.mp3` (or `.flac`), with `downloads.json` listing them and their covers in `art\`. Each folder keeps its own downloads. |
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
