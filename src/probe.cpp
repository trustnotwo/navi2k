// navi2k_probe: console end-to-end checks using the same code as the player.
//
//   navi2k_probe [play] [seconds=15] [album=0] [track=0]
//       Play a track; print position and peak levels every second; halfway through, set the
//       volume to 50% (gain 0.25) and confirm the output peaks drop by that factor.
//   navi2k_probe seek <offsetSec> [album=0] [track=0]
//       Check whether the server honours timeOffset (decode the whole stream and time it),
//       then play from the offset and confirm the reported position starts there.
//   navi2k_probe art [album=0]
//       Fetch the album's cover, save the raw bytes as cover_test.jpg/.png, decode it.
//   navi2k_probe rate [album=0] [track=0]
//       Change the track's rating, clear it, then restore the original; verify each via getSong.
//   navi2k_probe playlist [album=0]
//       Create a throwaway "Navi2K test" playlist and exercise rename/add/remove/reorder and a
//       150-entry batched replace/create on it, verifying each step; always deletes it. Other
//       playlists are only listed, never changed.
//
// Reads Navi2KPlayer.ini next to the exe. Exit code 0 = pass. Never prints credentials.

#include <windows.h>
#include <shellapi.h>
#include <stdlib.h>
#include <string>
#include <vector>

#include "art.h"
#include "config.h"
#include "cpu.h"
#include "http.h"
#include "mad_decoder.h"
#include "pcm.h"
#include "player.h"
#include "subsonic.h"
#include "util.h"

static void Out(const std::wstring& s)
{
    std::wstring line = s + L"\r\n";
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD n;
    if (!WriteConsoleW(h, line.c_str(), (DWORD)line.size(), &n, NULL)) {
        std::string u = WideToUtf8(line);   // redirected: emit UTF-8
        WriteFile(h, u.data(), (DWORD)u.size(), &n, NULL);
    }
}

// --- Shared playback harness ------------------------------------------------------

enum { TIMER_LIMIT = 1, TIMER_TICK = 2 };

static Player g_player;
static DWORD g_startTick;
static bool g_reachedPlaying = false;
static bool g_failed = false;
static int g_seconds = 15;
static int g_tick = 0;
static bool g_volumeTest = false;
static bool g_volumeOk = true;
static DWORD g_expectStartMs = 0;
static bool g_positionOk = true;

static double Elapsed()
{
    return (GetTickCount() - g_startTick) / 1000.0;
}

static void OnTick()
{
    ++g_tick;
    int in, out;
    g_player.TakePeaks(in, out);
    DWORD pos = g_player.PositionMs();
    double ratio = in ? (double)out / in : 0.0;
    Out(Fmt(L"[%5.2fs] pos %lu:%06.3f  vol %3d%%  peak in %5d out %5d  (out/in %.3f)", Elapsed(),
            pos / 60000, (pos % 60000) / 1000.0, g_player.Volume(), in, out, ratio));

    if (g_tick == 1 && g_reachedPlaying && (pos + 1500 < g_expectStartMs || pos > g_expectStartMs + 2500)) {
        Out(Fmt(L"  ! position %lu ms is not near the expected start %lu ms", pos, g_expectStartMs));
        g_positionOk = false;
    }
    if (g_volumeTest) {
        // Peaks are taken at decode time, so the ratio switches on the first tick after the change.
        double expect = (g_player.Volume() == 100) ? 1.0 : 0.25;
        if (in > 1000 && (ratio < expect - 0.01 || ratio > expect + 0.01)) {
            Out(Fmt(L"  ! expected out/in %.2f", expect));
            g_volumeOk = false;
        }
        if (g_tick == g_seconds / 2) {
            g_player.SetVolume(50);
            Out(L"  -> volume set to 50% (gain 0.25)");
        }
    }
}

static LRESULT CALLBACK ProbeWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PLAYER_STATE) {
        wchar_t* text = (wchar_t*)lp;
        switch ((PlayerState)LOWORD(wp)) {
        case PS_BUFFERING:
            Out(Fmt(L"[%5.2fs] buffering", Elapsed()));
            break;
        case PS_PLAYING:
            g_reachedPlaying = true;
            Out(Fmt(L"[%5.2fs] PLAYING (prebuffer filled, audio handed to waveOut)", Elapsed()));
            break;
        case PS_FINISHED:
            Out(Fmt(L"[%5.2fs] track finished", Elapsed()));
            PostQuitMessage(0);
            break;
        case PS_ERROR:
            g_failed = true;
            Out(Fmt(L"[%5.2fs] ERROR: %ls", Elapsed(), text ? text : L"(none)"));
            PostQuitMessage(0);
            break;
        }
        free(text);
        return 0;
    }
    if (msg == WM_TIMER) {
        if (wp == TIMER_TICK) {
            OnTick();
        } else {
            Out(Fmt(L"[%5.2fs] time limit reached, stopping", Elapsed()));
            PostQuitMessage(0);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static bool RunPlayback(const std::wstring& url, const HttpOptions& opts, DWORD startMs, DWORD skipMs)
{
    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = ProbeWndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"Navi2KProbe";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(L"Navi2KProbe", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);

    g_player.SetNotifyWindow(hwnd);
    g_player.SetVolume(100);
    g_expectStartMs = startMs;
    g_startTick = GetTickCount();
    SetTimer(hwnd, TIMER_LIMIT, g_seconds * 1000 + 300, NULL);
    SetTimer(hwnd, TIMER_TICK, 1000, NULL);
    g_player.Play(url, opts, startMs, skipMs);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0)
        DispatchMessageW(&msg);
    g_player.Stop();
    return g_reachedPlaying && !g_failed && g_positionOk && g_volumeOk;
}

// Downloads and decodes a whole stream; returns the audio duration in ms (or -1).
static long MeasureStreamMs(const std::wstring& url, const HttpOptions& opts, DWORD skipMs, size_t* bytes)
{
    HttpRequest req;
    if (req.Open(url, opts) != HTTP_OK)
        return -1;
    MadDecoder dec;
    if (skipMs)
        dec.SkipMs(skipMs);
    std::vector<unsigned char> chunk(16384);
    std::vector<short> pcm;
    double ms = 0;
    *bytes = 0;
    for (;;) {
        unsigned rate = 0, ch = 0;
        DecodeResult d = dec.Decode(pcm, rate, ch);
        if (d == DEC_FRAME) {
            ms += (double)(pcm.size() / ch) * 1000.0 / rate;
        } else if (d == DEC_NEED_MORE) {
            int n = req.Read(&chunk[0], (int)chunk.size());
            if (n < 0)
                return -1;
            if (n == 0) {
                dec.SetEof();
            } else {
                dec.Feed(&chunk[0], n);
                *bytes += n;
            }
        } else {
            return d == DEC_END ? (long)ms : -1;
        }
    }
}

// --- rate / playlist helpers -------------------------------------------------------------

static bool RateAndVerify(SubsonicClient& client, const std::string& id, int rating)
{
    std::wstring err;
    if (!client.SetRating(id, rating, err)) {
        Out(Fmt(L"  setRating %d FAILED: %ls", rating, err.c_str()));
        return false;
    }
    SubsonicSong s;
    if (!client.GetSong(id, s, err)) {
        Out(L"  getSong FAILED: " + err);
        return false;
    }
    bool ok = s.userRating == rating;
    Out(Fmt(L"  setRating %d -> server now reports %d  %ls", rating, s.userRating, ok ? L"ok" : L"MISMATCH"));
    return ok;
}

// Positions of the playlist's entries within 'songs' (the album used to build it), e.g. "[0,2,3]".
static std::wstring Describe(const std::vector<std::string>& ids, const std::vector<SubsonicSong>& songs)
{
    std::wstring s = L"[";
    for (size_t i = 0; i < ids.size(); ++i) {
        int pos = -1;
        for (size_t j = 0; j < songs.size(); ++j)
            if (songs[j].id == ids[i])
                pos = (int)j;
        s += (i ? L"," : L"") + Fmt(L"%d", pos);
    }
    return s + L"]";
}

static bool ExpectPlaylist(SubsonicClient& client, const std::string& plId, const std::vector<std::string>& expect,
                           const std::vector<SubsonicSong>& songs, const wchar_t* step)
{
    std::vector<SubsonicSong> entries;
    std::wstring err;
    if (!client.GetPlaylist(plId, entries, err)) {
        Out(Fmt(L"  %ls: getPlaylist FAILED: %ls", step, err.c_str()));
        return false;
    }
    std::vector<std::string> got;
    for (size_t i = 0; i < entries.size(); ++i)
        got.push_back(entries[i].id);
    bool ok = got == expect;
    if (expect.size() > 10)
        Out(Fmt(L"  %-28ls %u entries (expected %u)  %ls", step, (unsigned)got.size(), (unsigned)expect.size(),
                ok ? L"ok" : L"MISMATCH"));
    else
        Out(Fmt(L"  %-28ls %ls (expected %ls)  %ls", step, Describe(got, songs).c_str(),
                Describe(expect, songs).c_str(), ok ? L"ok" : L"MISMATCH"));
    return ok;
}

static bool FindPlaylist(SubsonicClient& client, const std::string& id, SubsonicPlaylist* out)
{
    std::vector<SubsonicPlaylist> pls;
    std::wstring err;
    if (!client.GetPlaylists(pls, err))
        return false;
    for (size_t i = 0; i < pls.size(); ++i)
        if (pls[i].id == id) {
            if (out)
                *out = pls[i];
            return true;
        }
    return false;
}

static int RunPlaylistTest(SubsonicClient& client, const std::vector<SubsonicSong>& songs)
{
    std::wstring err;
    std::vector<SubsonicPlaylist> pls;
    if (!client.GetPlaylists(pls, err)) {
        Out(L"getPlaylists FAILED: " + err);
        return 1;
    }
    Out(Fmt(L"getPlaylists: %u existing (listed only, not touched)", (unsigned)pls.size()));
    for (size_t i = 0; i < pls.size(); ++i)
        Out(Fmt(L"  %ls  by %ls, %d tracks%ls%ls", pls[i].name.c_str(), pls[i].owner.c_str(), pls[i].songCount,
                pls[i].readOnly ? L", read-only" : L"", client.CanEdit(pls[i]) ? L", editable" : L""));
    if (songs.size() < 4) {
        Out(L"Need an album with at least 4 tracks.");
        return 1;
    }

    const std::wstring name = Fmt(L"Navi2K test %lu", GetTickCount());
    std::vector<std::string> expect;
    for (int i = 0; i < 3; ++i)
        expect.push_back(songs[i].id);

    std::string plId;
    if (!client.CreatePlaylist(name, expect, &plId, err)) {
        Out(L"createPlaylist FAILED: " + err);
        return 1;
    }
    if (plId.empty()) {   // pre-1.14 servers don't return the new playlist; find it by name
        client.GetPlaylists(pls, err);
        for (size_t i = 0; i < pls.size(); ++i)
            if (pls[i].name == name)
                plId = pls[i].id;
    }
    Out(Fmt(L"created \"%ls\" (id %hs)", name.c_str(), plId.c_str()));
    if (plId.empty()) {
        Out(L"Could not find the new playlist's id; delete it by hand.");
        return 1;
    }

    bool ok = ExpectPlaylist(client, plId, expect, songs, L"create with tracks 0,1,2");

    SubsonicPlaylist info;
    const std::wstring renamed = name + L" (renamed)";
    ok = client.RenamePlaylist(plId, renamed, err) && FindPlaylist(client, plId, &info) && info.name == renamed && ok;
    Out(Fmt(L"  %-28ls %ls  %ls", L"rename", info.name.c_str(), info.name == renamed ? L"ok" : L"MISMATCH"));
    Out(Fmt(L"  %-28ls %ls", L"editable by this user", client.CanEdit(info) ? L"yes  ok" : L"no  MISMATCH"));
    ok = client.CanEdit(info) && ok;

    std::vector<std::string> add(1, songs[3].id);
    expect.push_back(songs[3].id);
    ok = client.AddToPlaylist(plId, add, err) && ExpectPlaylist(client, plId, expect, songs, L"add track 3") && ok;

    expect.erase(expect.begin() + 1);
    ok = client.RemoveFromPlaylist(plId, 1, err) && ExpectPlaylist(client, plId, expect, songs, L"remove position 1") && ok;

    std::vector<std::string> reversed(expect.rbegin(), expect.rend());
    ok = client.SetPlaylistSongs(plId, reversed, err) && ExpectPlaylist(client, plId, reversed, songs, L"reorder (reverse)") && ok;

    // A track list far too long for one Win2000-safe URL: proves the split-into-batches path.
    std::vector<std::string> big;
    for (int i = 0; i < 150; ++i)
        big.push_back(songs[i % songs.size()].id);
    ok = client.SetPlaylistSongs(plId, big, err) && ExpectPlaylist(client, plId, big, songs, L"150 entries, batched") && ok;
    // Same for creating: a new playlist with 150 tracks, then delete it.
    std::string bigId;
    bool bigOk = client.CreatePlaylist(name + L" (big)", big, &bigId, err) && !bigId.empty() &&
                 ExpectPlaylist(client, bigId, big, songs, L"create with 150, batched");
    if (!bigId.empty()) {
        std::wstring delErr;
        client.DeletePlaylist(bigId, delErr);
    }
    ok = bigOk && ok;
    if (!err.empty())
        Out(L"  last error: " + err);

    // Cleanup always runs.
    std::wstring delErr;
    bool deleted = client.DeletePlaylist(plId, delErr) && !FindPlaylist(client, plId, NULL);
    Out(Fmt(L"  %-28ls %ls", L"delete test playlist", deleted ? L"gone  ok" : (L"FAILED: " + delErr).c_str()));
    return (ok && deleted) ? 0 : 1;
}

static int ArgInt(int argc, wchar_t** argv, int i, int def)
{
    return (i < argc) ? _wtoi(argv[i]) : def;
}

int main()
{
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring mode = L"play";
    int a = 1;
    if (argc > 1 && (argv[1][0] < L'0' || argv[1][0] > L'9'))
        mode = argv[a++];

    int seekSec = 0;
    if (mode == L"play")
        g_seconds = ArgInt(argc, argv, a++, 15);
    else if (mode == L"seek" || mode == L"seek-client")
        seekSec = ArgInt(argc, argv, a++, 60);
    else if (mode == L"setrating")
        seekSec = ArgInt(argc, argv, a++, -1);   // reused as the rating value
    else if (mode != L"art" && mode != L"rate" && mode != L"playlist" && mode != L"info" && mode != L"artists" &&
             mode != L"songs") {
        Out(L"usage: navi2k_probe [play [sec] | seek <offsetSec> | seek-client <offsetSec> | art | rate | playlist |"
            L" info | artists | songs]"
            L" [album] [track]");
        return 2;
    }
    const int albumIndex = ArgInt(argc, argv, a++, 0);
    const int trackIndex = ArgInt(argc, argv, a++, 0);

    const std::wstring dir = ExeDirectory();
    LogInit(dir + L"\\navi2k_probe.log");

    CpuCaps cpu = DetectCpu();
    std::wstring cpuProblem;
    if (!CpuCheckBuildCompatible(cpu, cpuProblem)) {
        Out(L"CPU: " + cpuProblem);
        return 3;
    }
    Out(Fmt(L"CPU %hs: cmov=%d sse=%d sse2=%d os-xmm=%d -> %ls", cpu.vendor, cpu.cmov, cpu.sse,
            cpu.sse2, cpu.osXmm, PcmInit(cpu.Sse2Usable())));

    SubsonicConfig cfg;
    std::wstring err;
    if (!ConfigLoad(dir + L"\\Navi2KPlayer.ini", cfg, err)) {
        Out(L"CONFIG: " + err);
        return 2;
    }
    if (!HttpInit(L"Navi2KPlayer/0.1 (probe)")) {
        Out(L"WinINet init failed");
        return 2;
    }

    SubsonicClient client;
    client.Configure(cfg);
    std::wstring notice;
    if (!client.Connect(notice)) {
        Out(L"CONNECT FAILED:\r\n" + notice);
        return 1;
    }
    std::vector<SubsonicAlbum> albums;
    if (!client.GetAlbumList(albums, err)) {
        Out(L"getAlbumList2 FAILED: " + err);
        return 1;
    }
    if (albumIndex < 0 || albumIndex >= (int)albums.size()) {
        Out(L"No album at that index.");
        return 1;
    }
    const SubsonicAlbum& album = albums[albumIndex];
    Out(Fmt(L"Connected to %ls: %u albums. Album [%d] %ls - %ls", cfg.url.c_str(),
            (unsigned)albums.size(), albumIndex, album.artist.c_str(), album.name.c_str()));

    // --- songs (read-only): the whole library via search3 ---
    if (mode == L"songs") {
        int expected = 0;
        for (size_t i = 0; i < albums.size(); ++i)
            expected += albums[i].songCount;
        std::vector<SubsonicSong> all;
        DWORD t0 = GetTickCount();
        if (!client.GetAllSongs(all, err)) {
            Out(L"search3 FAILED: " + err);
            return 1;
        }
        Out(Fmt(L"search3: %u songs in %lu ms (albums list adds up to %d)", (unsigned)all.size(),
                GetTickCount() - t0, expected));
        for (size_t i = 0; i < all.size() && i < 4; ++i)
            Out(Fmt(L"  %ls - %ls - %d.%d %ls", all[i].artist.c_str(), all[i].album.c_str(), all[i].disc,
                    all[i].track, all[i].title.c_str()));
        bool ok = !all.empty() && (int)all.size() == expected;
        Out(ok ? L"RESULT: PASS" : L"RESULT: FAIL (count mismatch)");
        return ok ? 0 : 1;
    }

    // --- artists (read-only) ---
    if (mode == L"artists") {
        std::vector<SubsonicArtist> artists;
        DWORD t0 = GetTickCount();
        if (!client.GetArtists(artists, err)) {
            Out(L"getArtists FAILED: " + err);
            return 1;
        }
        int withCover = 0, totalAlbums = 0;
        for (size_t i = 0; i < artists.size(); ++i) {
            withCover += artists[i].coverArt.empty() ? 0 : 1;
            totalAlbums += artists[i].albumCount;
        }
        Out(Fmt(L"getArtists: %u artists, %d with a cover id, %d albums in total (%lu ms)",
                (unsigned)artists.size(), withCover, totalAlbums, GetTickCount() - t0));
        if (artists.empty())
            return 1;
        const SubsonicArtist& ar = artists[albumIndex < (int)artists.size() ? albumIndex : 0];
        std::vector<SubsonicAlbum> albs;
        if (!client.GetArtist(ar.id, albs, err)) {
            Out(L"getArtist FAILED: " + err);
            return 1;
        }
        Out(Fmt(L"getArtist \"%ls\": %u albums (index says %d)", ar.name.c_str(), (unsigned)albs.size(), ar.albumCount));
        for (size_t i = 0; i < albs.size() && i < 5; ++i)
            Out(Fmt(L"  %ls (%d)", albs[i].name.c_str(), albs[i].year));
        if (!ar.coverArt.empty()) {
            std::string bytes;
            bool got = client.GetCoverArt(ar.coverArt, 160, bytes, err);
            HBITMAP bmp = got ? ArtDecode(bytes, 160, RGB(212, 208, 200), err) : NULL;
            std::wstring magic;
            for (size_t i = 0; i < bytes.size() && i < 16; ++i)
                magic += Fmt(L"%c", (bytes[i] >= 32 && bytes[i] < 127) ? (wchar_t)bytes[i] : L'.');
            Out(bmp ? Fmt(L"artist image: %u bytes, decoded ok", (unsigned)bytes.size())
                    : Fmt(L"artist image: %u bytes starting \"%ls\": ", (unsigned)bytes.size(), magic.c_str()) + err);
            if (bmp)
                DeleteObject(bmp);
        }
        bool ok = (int)albs.size() == ar.albumCount;
        Out(ok ? L"RESULT: PASS" : L"RESULT: FAIL (album count mismatch)");
        return ok ? 0 : 1;
    }

    // --- art ---
    if (mode == L"art") {
        if (album.coverArt.empty()) {
            Out(L"This album has no coverArt id.");
            return 1;
        }
        std::string bytes;
        DWORD t0 = GetTickCount();
        if (!client.GetCoverArt(album.coverArt, 160, bytes, err)) {
            Out(L"getCoverArt FAILED: " + err);
            return 1;
        }
        bool png = bytes.size() > 4 && (unsigned char)bytes[0] == 0x89 && bytes[1] == 'P';
        bool jpg = bytes.size() > 3 && (unsigned char)bytes[0] == 0xFF && (unsigned char)bytes[1] == 0xD8;
        Out(Fmt(L"getCoverArt: %u bytes, %ls (%lu ms)", (unsigned)bytes.size(),
                png ? L"PNG" : jpg ? L"JPEG" : L"unknown format", GetTickCount() - t0));
        std::wstring path = dir + (png ? L"\\cover_test.png" : L"\\cover_test.jpg");
        HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD w;
            WriteFile(f, bytes.data(), (DWORD)bytes.size(), &w, NULL);
            CloseHandle(f);
            Out(L"saved " + path);
        }
        t0 = GetTickCount();
        HBITMAP bmp = ArtDecode(bytes, 160, RGB(212, 208, 200), err);
        if (!bmp) {
            Out(L"DECODE FAILED: " + err);
            return 1;
        }
        BITMAP bm;
        GetObjectW(bmp, sizeof(bm), &bm);
        Out(Fmt(L"decoded to %dx%d %d-bit DIB (%lu ms)", bm.bmWidth, bm.bmHeight, bm.bmBitsPixel, GetTickCount() - t0));
        DeleteObject(bmp);
        Out(L"RESULT: PASS");
        return 0;
    }

    std::vector<SubsonicSong> songs;
    if (!client.GetAlbum(album.id, songs, err) || trackIndex < 0 || trackIndex >= (int)songs.size()) {
        Out(L"getAlbum FAILED or no such track: " + err);
        return 1;
    }
    if (mode == L"info") {   // read-only snapshot: playlists and this album's ratings
        std::vector<SubsonicPlaylist> pls;
        client.GetPlaylists(pls, err);
        Out(Fmt(L"playlists: %u", (unsigned)pls.size()));
        for (size_t i = 0; i < pls.size(); ++i)
            Out(Fmt(L"  %ls (%d tracks)", pls[i].name.c_str(), pls[i].songCount));
        for (size_t i = 0; i < songs.size(); ++i)
            Out(Fmt(L"  track %u rating %d  %ls", (unsigned)i, songs[i].userRating, songs[i].title.c_str()));
        return 0;
    }

    if (mode == L"playlist") {
        int rc = RunPlaylistTest(client, songs);
        Out(rc == 0 ? L"RESULT: PASS" : L"RESULT: FAIL");
        return rc;
    }

    const SubsonicSong& song = songs[trackIndex];

    if (mode == L"setrating") {   // navi2k_probe setrating <0..5> [album] [track]
        if (seekSec < 0 || seekSec > 5) {
            Out(L"Rating must be 0..5.");
            return 2;
        }
        bool ok = RateAndVerify(client, song.id, seekSec);
        Out(ok ? L"RESULT: PASS" : L"RESULT: FAIL");
        return ok ? 0 : 1;
    }

    if (mode == L"rate") {
        SubsonicSong current;
        if (!client.GetSong(song.id, current, err)) {
            Out(L"getSong FAILED: " + err);
            return 1;
        }
        const int original = current.userRating;
        Out(Fmt(L"Track \"%ls\": current rating %d (will be restored)", song.title.c_str(), original));
        bool ok = RateAndVerify(client, song.id, original == 4 ? 2 : 4);
        ok = RateAndVerify(client, song.id, 0) && ok;
        bool restored = RateAndVerify(client, song.id, original);
        Out(restored ? Fmt(L"restored original rating %d", original)
                     : Fmt(L"!! COULD NOT RESTORE rating %d on \"%ls\"", original, song.title.c_str()));
        Out(ok && restored ? L"RESULT: PASS" : L"RESULT: FAIL");
        return ok && restored ? 0 : 1;
    }

    const bool transcodes = client.ServerTranscodes(song);
    Out(Fmt(L"Track [%d] \"%ls\" %d:%02d, source %ls %d kbps -> %ls", trackIndex, song.title.c_str(),
            song.duration / 60, song.duration % 60, song.suffix.c_str(), song.bitRate,
            transcodes ? L"server transcodes to MP3" : L"server sends original MP3"));

    // --- seek ---
    if (mode == L"seek" || mode == L"seek-client") {
        if (seekSec <= 0 || seekSec >= song.duration) {
            Out(L"Offset must be inside the track.");
            return 2;
        }
        // seek-client forces the header-skip path used for untouched original MP3s.
        const bool serverSide = transcodes && mode == L"seek";
        std::wstring url = client.StreamUrl(song.id, serverSide ? seekSec : 0);
        DWORD skipMs = serverSide ? 0 : (DWORD)seekSec * 1000;
        Out(Fmt(L"Measuring stream from %d s (%ls)...", seekSec,
                serverSide ? L"server-side timeOffset" : L"client-side header skip"));
        size_t bytes = 0;
        DWORD t0 = GetTickCount();
        long ms = MeasureStreamMs(url, client.Options(), skipMs, &bytes);
        if (ms < 0) {
            Out(L"Stream measurement failed.");
            return 1;
        }
        long expect = (song.duration - seekSec) * 1000L;
        Out(Fmt(L"  decoded %ld ms of audio from %u bytes in %lu ms; expected ~%ld ms", ms,
                (unsigned)bytes, GetTickCount() - t0, expect));
        bool offsetOk = labs(ms - expect) < 3000;
        Out(offsetOk ? L"  offset honoured" : L"  ! offset NOT honoured (stream started elsewhere)");

        g_seconds = 6;
        bool ok = RunPlayback(url, client.Options(), (DWORD)seekSec * 1000, skipMs) && offsetOk;
        HttpShutdown();
        Out(ok ? L"RESULT: PASS" : L"RESULT: FAIL");
        return ok ? 0 : 1;
    }

    // --- play (with volume step) ---
    g_volumeTest = true;
    Out(Fmt(L"Streaming as MP3 @ %d kbps for %d s; volume 100%% then 50%% at %d s",
            cfg.maxBitRate, g_seconds, g_seconds / 2));
    bool ok = RunPlayback(client.StreamUrl(song.id), client.Options(), 0, 0);
    HttpShutdown();
    Out(Fmt(L"position tracking %ls, volume %ls", g_positionOk ? L"ok" : L"WRONG", g_volumeOk ? L"ok" : L"WRONG"));
    Out(ok ? L"RESULT: PASS" : L"RESULT: FAIL");
    return ok ? 0 : 1;
}
