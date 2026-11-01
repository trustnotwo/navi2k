Navi2K Player 1.3-dev
=====================

A Navidrome (Subsonic API) music player for Windows 95 and up.

Requirements
------------
* Windows 95 and up (Windows 95 needs Internet Explorer 5 or later).
* A Pentium or newer CPU (Pentium MMX and AMD K6 included). Tested on a Pentium 90,
  where MP3 plays in real time.
* A Navidrome server with ffmpeg (Navidrome's default), reachable over plain HTTP on
  your local network, e.g. http://192.168.1.10:4533. Older Windows can't use modern
  HTTPS.

Files
-----
Navi2KPlayer.exe   The player. Copy it anywhere; nothing to install.
navi2k_probe.exe   Optional command-line test tool (see below).
DroidSansFallback.ttf  Font for Japanese, Chinese and Korean. Keep it next to the exe
                   for those on Windows 95/98/ME.
licenses\          Licenses of the bundled libraries and font (libmad, cJSON,
                   stb_image, dr_flac, Droid Sans Fallback).

Windows 95, 98 and ME
---------------------
Windows 95, 98 and ME have almost no Unicode support, so the player uses the system code
page instead:
* Text in other scripts (for example Japanese on an English Windows 98) shows in the
  player's lists, sidebar and panels, but as "?" in the title bar, menus, search box and
  dialogs. Japanese, Chinese and Korean use DroidSansFallback.ttf next to the exe, or an
  installed font for the language.
* Download file names replace those characters with "_".
* Navi2KPlayer.ini is a plain ANSI file. An ini copied from a newer Windows can't be
  read on 9x: set the player up again in Settings.
* Media keys probably don't work on 95/98, and the tray tooltip is cut to 63 characters.

On newer Windows without East Asian language support, the same font is used for the
player's lists and panels.

First run
---------
1. Run Navi2KPlayer.exe. The Settings window opens.
2. On the Server tab, enter the address (e.g. http://192.168.1.10:4533), your user name and
   password. Test Connection checks them without saving.
3. Click OK. The player connects.

Settings are kept in Navi2KPlayer.ini next to the exe. The password itself is never saved,
only a token and salt. Keep the ini private anyway: the token works like a password for this
server. Logs go to Navi2KPlayer.log next to the exe (tokens are never logged).

Using it
--------
* Sidebar: Library (Albums, Artists, Songs, Favorites, Play Queue, Downloads) and your
  playlists. File > Settings > Sidebar picks which Library sections show, including the
  Discover lists (Recently Added, Recently Played, Most Played, Random Albums).
* Double-click (or Enter) opens an album or artist, or plays a track. Backspace goes
  back. Ctrl+F searches Albums, Artists or Songs.
* Right-click a track for Play next, Add to queue, Go to album or artist, Rate,
  Favorites, Add to playlist and Download. Select several tracks (Ctrl-click,
  Shift-click, Ctrl+A) to act on them together.
* Playlists: right-click one to rename, describe, download or delete it; "+ New
  Playlist" creates one. Drag tracks to reorder them; Del removes them.
* Play Queue (Ctrl+Q): see what's coming up, drag to reorder, Del to remove.
* Offline: downloads go to a Downloads folder next to the exe. If the server can't be
  reached, the player opens with them, and plays are sent to Navidrome later. While
  connected, tracks stream unless File > Settings > Downloads says to play the
  downloaded copies.
* Libraries: if your server has several, File > Settings > Libraries chooses which ones
  show.
* Mini player: Ctrl+M. The notification-area icon has playback controls on right-click.
* Keyboard: Ctrl+P (or Space) play/pause, Ctrl+S stop, Ctrl+B / Ctrl+N previous / next,
  Ctrl+H shuffle, Ctrl+T repeat, Ctrl+F search, F5 reconnect. Media keys work too, even
  with the player in the background.
* File > Settings also has streaming quality (including Lossless FLAC), gapless
  playback, scrobbling, Last.fm descriptions, and options to hide ratings and favorites.

Test tool
---------
Run from a Command Prompt in the same folder (it uses the same Navi2KPlayer.ini):
    navi2k_probe play 10     play 10 seconds and check volume/position
    navi2k_probe seek 60     check seeking against your server
    navi2k_probe songs       check the whole library loads
    navi2k_probe artists     check artists load
    navi2k_probe libraries   list your libraries and check each one filters correctly
    navi2k_probe nowplaying  check "now playing" reaches the server (no play is counted)
    navi2k_probe flac        check lossless FLAC streaming and seeking
    navi2k_probe flac-norange  the same, reading from the start of the file instead of
                             jumping with range requests (the fallback if those fail)
Each prints RESULT: PASS or FAIL. The "rate", "favorite", "playlist" and "libplaylist"
modes change data on your server (and put it back), so try them on a test server first.

Source and licenses
-------------------
The source code is distributed separately (Navi2K-Player-source). Navi2K Player includes
libmad, which is licensed under the GNU GPL version 2; see licenses\libmad-COPYING.txt.
