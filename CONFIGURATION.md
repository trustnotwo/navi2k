# Navi2K Player configuration

Everything here can be changed in **File > Settings** (volume, shuffle, and repeat on the main window). The player saves it in `Navi2KPlayer.ini` next to the exe, and this file lists every key for anyone who wants to edit the ini directly, for example to set up several machines the same way.

On Windows 95, 98, and ME the ini is a plain ANSI file. On newer Windows it is UTF-16.

## `[Server]` (Settings > Server, Libraries, Playback)

| Setting | Default | What it does |
|---|---|---|
| `Url` | — | Your Navidrome server address |
| `User` | — | Your Navidrome user name. The password is entered in Settings. |
| `LanUrl` | — | A plain-HTTP fallback address on your local network |
| `AllowHttpFallback` | `0` | Set to `1` to use `LanUrl` when HTTPS fails. Only works for local addresses. |
| `AllowUntrustedCert` | `0` | Set to `1` to accept self-signed certificates |
| `MaxBitRate` | `192` | Streaming quality in kbps (96–320) |
| `Libraries` | all | Comma-separated ids of the libraries to show (File > Settings > Libraries lists them by name). Empty or missing shows every library. |
| `Lossless` | `0` | Set to `1` to play FLAC files as they are (about 1 Mbps). Other formats, and FLAC above 48 kHz or with more than 2 channels, still stream as MP3. |

The password itself is never stored: Settings saves a login token (`Token` and `Salt`) instead.

## `[Player]` (Settings > Playback and Display, and the main window)

| Setting | Default | What it does |
|---|---|---|
| `OutputDevice` | system default | Sound device to play through |
| `Buffers` | `12` | Audio buffer size: 8 (about 0.4 s) to 32 (about 1.5 s). Raise it on a slow network. |
| `Gapless` | `1` | Plays the next track with no pause between them. The next track starts loading 8 seconds before the current one ends. |
| `Scrobble` | `1` | Reports plays to Navidrome. A play counts after half the track or 4 minutes. |
| `GlobalMediaKeys` | `1` | Media keys control the player even when it's in the background. Set to `0` to leave them for other programs. |
| `Shuffle` | `0` | Remembers the shuffle setting |
| `Repeat` | `0` | Remembers repeat: `0` off, `1` all, `2` one |
| `Volume` | `100` | Remembers the volume level |

## `[View]` (Settings > Sidebar and Display)

| Setting | Default | What it does |
|---|---|---|
| `ShowFavorites` | `1` | Shows the ♥ column and the Favorites section |
| `ShowRatings` | `1` | Shows the star rating column |
| `HiddenSections` | `added,played,frequent,random` | Sidebar sections to hide, comma-separated (empty shows them all): `albums`, `artists`, `songs`, `favorites`, `added`, `played`, `frequent`, `random`, `queue`, `downloads` |
| `LastFmInfo` | `0` | Set to `1` to show album and artist descriptions from Last.fm. Navidrome fetches them, so Last.fm must be turned on there. |
| `TrayIcon` | `1` | Shows an icon in the notification area |
| `MinimizeToTray` | `0` | Set to `1` to hide the player in the notification area when minimized |

## `[Downloads]` (Settings > Downloads)

| Setting | Default | What it does |
|---|---|---|
| `Quality` | `0` | Quality of new downloads: `0` the streaming quality, `1` Lossless (FLAC as is), or an MP3 bitrate in kbps (96–320) |
| `PlayDownloaded` | `0` | Set to `1` to play downloaded copies even while connected. Otherwise tracks stream at the Playback quality and downloads are used only offline. |
| `Folder` | `Downloads` next to the exe | Where downloaded tracks are kept, as `Artist\Album\01 Title.mp3` (or `.flac`), with `downloads.json` listing them and their covers in `art\`. Each folder keeps its own downloads. |
