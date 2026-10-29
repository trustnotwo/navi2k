# Changelog

## 1.1

- Lossless playback: choose **Lossless** under File > Settings > Playback > Quality to stream FLAC files untouched. Seeking uses HTTP range requests. Other formats, and hi-res or multichannel FLAC, still play as 320 kbps MP3.
- Play queue: right-click a track, album, or playlist and choose **Play next** or **Add to queue**. **Library > Play Queue** (Ctrl+Q) shows what's coming up, where you can jump to a track, reorder, remove (Del), or clear what's upcoming. Adding tracks after the queue has finished starts them right away.
- Previous and Next buttons on the main window's transport bar (they were only in the mini player and the Playback menu).
- Playlist descriptions: a playlist's description (its Navidrome comment) shows under its title. Right-click a playlist, or use **Playlist > Edit Name and Description**, to change it.
- Gapless playback: the next track starts with no pause, with the next track loading a few seconds early. On by default; it can be turned off under File > Settings > Playback. MP3s can keep a few milliseconds of silence from the encoder; FLAC (Lossless) joins exactly.
- Artists without a photo in Navidrome (or with only a grey "no photo" silhouette) show a collage of up to four of their album covers, or the one cover for a single album.
- Right-click a track and choose **Go to album** or **Go to artist** to open that page, from any track list (playlists, Songs, Favorites, the Play Queue).
- Drag and drop to reorder tracks in a playlist or the Play Queue. A line shows where the track will go; Esc cancels.
- Repeat: off, all, or one, from the new button next to Shuffle, the Playback menu, or Ctrl+T.
- Keyboard shortcuts: Ctrl+P play/pause, Ctrl+S stop, Ctrl+B previous, Ctrl+N next, Ctrl+H shuffle, Ctrl+T repeat, and Space in a track list.
- Media keys (play/pause, stop, previous, next) work even when the player is in the background. This can be turned off under File > Settings > Display.
- Sort lists by clicking a column header, and click again to reverse. In track lists, clicking **#** restores the original order.
- Album and artist descriptions from Last.fm, under the album or artist name. Off by default: turn them on under File > Settings > Display. Navidrome fetches them, so Last.fm must be turned on there. Click a long description to read all of it.
- Notification-area (tray) icon with the current song as its tooltip. Click it to bring the player back; right-click it for play/pause, stop, previous, next, shuffle, and repeat. Optional minimize-to-tray under File > Settings > Display.

## 1.0 (initial release)

First public release of Navi2K Player, a Navidrome client for Windows 2000 SP4 and XP SP3.

- Stream music from Navidrome
- Browse by album, artist, or song, with search
- Album and artist art
- Playlists: create, rename, delete, add tracks, and reorder
- Star ratings and favorites, synced to Navidrome
- Scrobbling
- Mini player (Ctrl+M)
- Shuffle, seeking, and volume control
- Settings dialog for server, playback, and display options
- Runs on CPUs without SSE2 (Pentium Pro or newer)
