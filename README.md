# Navi2K Player

A Navidrome (Subsonic API) music player for **Windows 2000 SP4** and **Windows XP SP3** that
runs on CPUs **without SSE2**. Minimum CPU is a Pentium Pro / Pentium II / Celeron / Athlon
(i686, CMOV); the app tells you clearly if the CPU is older.

Native Win32 C++. Not .NET: Framework 3.5 can't be installed on Windows 2000 (2.0 is the last
version that supports it), and native code has no runtime to install on either OS.

| Layer      | Choice                                   | Why |
|------------|------------------------------------------|-----|
| UI         | Win32 + common controls (ListView, status bar) | Works on Win2000's comctl32 5.8x; themed on XP via manifest |
| Network    | WinINet                                  | Built into Win2000/XP (IE5+). WinHTTP 5.1 needs Win2000 SP3+ and adds nothing here |
| API        | Subsonic REST, `f=json`, token auth      | Navidrome's client API |
| JSON       | cJSON                                    | Single C89 file |
| Decode     | libmad 0.15.1b (fixed-point)             | Integer-only, so it runs at full quality without an FPU or SIMD |
| Formats    | Navidrome transcodes everything to MP3 (`format=mp3`) | One decoder handles FLAC, M4A and OGG sources |
| Audio out  | waveOut                                  | Present on every install; no DirectX runtime needed |
| Cover art  | `getCoverArt` + stb_image (JPEG/PNG, no SIMD) | Win2000 has no PNG decoder and no GDI+ |

## Features

* **Layout** (after the classic Spotify desktop client, using only classic Win32 controls):
  a menu bar (File / View / Playback / Playlist / Help, F5 = Connect); a sidebar tree with
  LIBRARY ▸ Albums and PLAYLISTS ▸ …, a "+ New Playlist" button, and the playing track's
  cover; a header with cover, large title, details and Play / Shuffle buttons; and a bottom
  transport bar with drawn Play / Pause / Stop / Shuffle icon buttons (owner-drawn classic 3D
  buttons; Pause and Shuffle stay pressed while active), the seek bar and volume.
* **Browse:** click Albums in the sidebar for the album list; double-click (or Enter) an album
  to open it, and Backspace or Albums to go back. **Artists** lists every artist (`getArtists`);
  opening one shows a page with their image and albums (`getArtist`). Backspace steps back:
  album → artist → artists. Navidrome sends a WebP placeholder when it has no artist photo,
  which isn't decoded, so the artist's first album cover is shown instead. **Songs** lists the
  whole library (fetched once per connection with `search3` and an empty query, 500 per
  request; ~7,000 songs in about half a second on a LAN), sorted by artist, album, disc, track.
  The track table is a virtual list (`LVS_OWNERDATA`), so even that fills instantly on a P3.
* **Columns follow the window size:** number columns (Year, Tracks, #, Rating, Time, Albums)
  keep their width and the text columns share the rest by weight (Album 5 : Artist 4; Track 5 :
  Artist 3 : Album 4), with room always left for the scrollbar.
* **Search** (Albums, Artists, Songs; not on album/artist/playlist pages): filters what's already
  loaded, no server requests. Albums match name + artist, artists their name, songs title +
  artist + album; several words must all match, in any order. Each section keeps its own
  search, and the header shows "8 of 656 albums". Filtering waits for a 250 ms pause in typing.
  Ctrl+F focuses the box, Esc clears it, Enter/↓ moves into the results. Playlists open from the sidebar. The playing
  track is shown in bold. Playback keeps its own queue, so browsing doesn't disturb it, and it
  auto-advances.
* **Shuffle:** a toggle (saved as `[Player] Shuffle=`). Turning it on reorders the rest of the
  queue around the current track; turning it off resumes the original order from that track.
  The header's Shuffle button plays the album or playlist in random order.
* **Volume:** applied in software, so it changes only this app, not the Win2000 system wave
  volume. It uses a squared curve and is saved as `[Player] Volume=` in the ini. Changes are
  heard after the ~0.5 s of audio already queued.
* **Progress and scrubbing:** the position comes from `waveOutGetPosition`, so it reflects what
  you hear. Drag the bar, click the channel (±10 s) or use the arrow keys. Transcoded tracks
  seek on the server with `timeOffset`. Original MP3s that Navidrome passes through untouched
  are skipped client-side by parsing frame headers only.
* **Album art:** 160×160 from the server, area-averaged if larger, cached for 48 albums.
* **Ratings:** a Rating column of drawn stars (GDI polygons; the Win2000 UI font has no ★).
  Click a star to rate (click the current rating to clear), press 0–5, or right-click → Rate.
  The change shows immediately and is reverted if the server refuses it (`setRating`).
* **Playlists:** an Albums | Playlists tab on the left. Selecting a playlist shows its tracks,
  cover and owner, and it plays like an album. Right-click a track → **Add to playlist**
  (existing or new); right-click an album → **Add album to playlist**. In a playlist, tracks
  can be moved up/down or removed (Del). Right-click the playlist list for New / Rename / Delete.
  Playlists you don't own, and Navidrome smart playlists, are shown read-only. Edits are sent
  as plain GETs (Windows 2000's WinINet fails form POSTs with error 12150); long track lists
  are split into several requests, each under ~1.8 KB so they fit that WinINet's URL limit.

## SSE2 policy

* `LEGACY_NO_SSE2=ON` (default) builds everything with `-march=i686 -mno-sse -mno-sse2 -mfpmath=387`.
  `cpu.h` fails the build with `#error` if `__SSE2__` is still defined.
* `LEGACY_SSE2_DISPATCH=ON` adds **one** SSE2 routine (`LpConvertPcmSse2`, PCM conversion). It is
  used only when CPUID reports SSE2 **and** the OS saves XMM state; otherwise the integer path
  runs. Output is bit-identical either way.
* At startup, `CpuCheckBuildCompatible` refuses to run an SSE2 build on a non-SSE2 CPU with a
  clear message. `InstallIllegalInstructionHandler` turns any stray SSE2 opcode into a readable
  dialog instead of a Dr. Watson crash.
* After each build, `cmake/check_binary.cmake` disassembles the .exe and fails if any function
  other than `LpConvertPcmSse2` touches XMM registers. It also checks that the PE subsystem
  version is 5.0 and that no XP-only imports or `msvcrt *_s` functions are used. Results are
  written to `build/compat-check/`.

## TLS on Windows 2000 / XP

Win2000/XP SChannel only offers SSL 3.0/TLS 1.0 with RC4/3DES. Almost every HTTPS server
refuses that, and the old root stores don't trust Let's Encrypt. The app doesn't hide this:

* Handshake failures and certificate failures are reported separately, each with a fix.
  The handshake message also says when "Use TLS 1.0" is switched off in Internet Options
  (the IE6 default on Win2000). The app reads that setting but never changes it.
* **Recommended setup:** `Url=http://<navidrome-lan-ip>:4533`, plain HTTP on the LAN.
* Optional `LanUrl` + `AllowHttpFallback=1`: if the https `Url` fails TLS, retry over plain
  HTTP. This is allowed only for private/LAN hosts, so the auth token never crosses the
  internet unencrypted.
* `AllowUntrustedCert=1` is opt-in, for self-signed or unknown-CA certificates.
* For HTTPS from outside your network, put a reverse proxy on the LAN that accepts TLS 1.0 with
  `TLS_RSA_WITH_3DES_EDE_CBC_SHA` and forwards to Navidrome.

## Building

Toolchain: **32-bit MinGW-w64, msvcrt runtime, win32 threads**. Verified with niXman's
`i686-16.2.0-release-win32-dwarf-msvcrt-rt_v14-rev1` (github.com/niXman/mingw-builds-binaries)
and CMake 4.4.4. UCRT and posix-thread builds will not run on Windows 2000. If the post-build
check finds XMM code in `libstdc++`/`libgcc` functions, your toolchain's runtime was built for
Pentium 4. Use one configured `--with-arch=i686`.

Modern GCC runtimes have Vista-isms that the build works around (see `src/compat_win2000.cpp`):

* libgcc's win32 thread layer (GCC 13+) imports `GetThreadId`; a shim supplies it via
  `NtQueryInformationThread`.
* libstdc++'s static-init guard uses condition variables, so C++ is compiled with
  `-fno-threadsafe-statics`. Don't rely on function-local statics with dynamic initialisers
  from multiple threads.
* The stock i686 runtime contains CMOV, so true i586 (Pentium/K6) needs a runtime built for i586.
  `-DLEGACY_MARCH=i586` turns on a CMOV scan that will tell you.

Dependencies (already vendored in `third_party/`):

```
third_party/libmad/   libmad 0.15.1b with Debian's 0.15.1b-11 patch set applied
                      (includes the length-check fix for CVE-2017-8372/8373/8374)
third_party/cJSON/    cJSON 1.7.19
third_party/stb/      stb_image.h v2.30
```

```
cmake -G "MinGW Makefiles" -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Options: `-DLEGACY_MARCH=i586` (see above), `-DLIBMAD_FPM=FPM_64BIT`
(pure C instead of i386 asm), `-DLEGACY_STRICT_CHECK=OFF` (turns check failures into warnings).

## Running

Copy `Navi2KPlayer.exe` anywhere and run it. It connects on startup. On first run it creates
`Navi2KPlayer.ini` next to itself and offers to open it: fill in `Url`, `User` and `Password`.
On first connect the password is replaced by a Subsonic token and salt. An existing
`LegacyPlayer.ini` from before the rename is picked up automatically. Logs go to
`Navi2KPlayer.log` (tokens are never logged).

Navidrome needs a working ffmpeg so it can transcode to MP3. This is on by default.

## Probe (console test tool)

`navi2k_probe.exe` uses the same code as the player and prints PASS/FAIL:

```
navi2k_probe play 10          play 10 s; volume steps to 50% halfway (expects out/in = 0.25)
navi2k_probe seek 60          check the server honours timeOffset, then play from 1:00
navi2k_probe seek-client 60   same, forcing the client-side header-skip path
navi2k_probe art 3            fetch + decode album 3's cover (saves cover_test.jpg)
navi2k_probe rate 0 2         change, clear, then restore track 2's rating (verified via getSong)
navi2k_probe songs            fetch the whole library; count must match the albums' track totals
navi2k_probe artists          getArtists/getArtist check (read-only)
navi2k_probe playlist         create/rename/add/remove/reorder/150-entry batches on a throwaway
                              "Navi2K test" playlist, then delete it; others are only listed
```

Album/track indexes can follow, e.g. `navi2k_probe seek 90 12 4`.

## Not done yet

A settings dialog, search, drag-and-drop playlist reordering, gapless playback, scrobbling
(`/rest/scrobble`), and a DirectSound output backend.
