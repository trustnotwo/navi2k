// Navi2K Player: Navidrome/Subsonic music client for Windows 2000 SP4 / XP SP3.
// Plain Win32 + common controls; no .NET, no DirectX, no SSE2 requirement.
//
// Layout (after the classic Spotify desktop client):
//   menu bar
//   sidebar: LIBRARY > Albums, PLAYLISTS > ... | header: cover, big title, details, Play/Shuffle
//   [+ New Playlist], now-playing cover       | album list or track table
//   transport: play/pause/stop/shuffle icons, position, seek bar, volume
//   status bar

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <process.h>
#include <stdlib.h>
#include <map>
#include <string>
#include <vector>

#include "art.h"
#include "config.h"
#include "cpu.h"
#include "http.h"
#include "pcm.h"
#include "player.h"
#include "resource.h"
#include "subsonic.h"
#include "util.h"

#define APP_TITLE   L"Navi2K Player"
#define APP_VERSION L"0.2"

enum {
    IDC_PLAY = 102, IDC_PAUSE, IDC_STOP,
    IDC_ALBUMS, IDC_SONGS, IDC_STATUS,
    IDC_VOLUME, IDC_SEEK = 110, IDC_TIME_CUR, IDC_TIME_TOTAL,
    IDC_TREE, IDC_NEW_PLAYLIST, IDC_SHUFFLE, IDC_HDR_PLAY, IDC_HDR_SHUFFLE, IDC_ARTISTS,
    IDC_SEARCH, IDC_SEARCH_LABEL,
    // Mini player.
    IDC_MINI_PREV, IDC_MINI_PLAYPAUSE, IDC_MINI_STOP, IDC_MINI_NEXT,
    IDC_MINI_SEEK, IDC_MINI_VOLUME, IDC_MINI_TIME_CUR, IDC_MINI_TIME_TOTAL
};

// Menu bar.
enum {
    IDM_CONNECT = 40001, IDM_EXIT, IDM_PLAY, IDM_PAUSE, IDM_STOP, IDM_SHUFFLE,
    IDM_ALBUMS, IDM_NEW_PLAYLIST, IDM_ABOUT, IDM_ARTISTS, IDM_SONGS, IDM_FIND,
    IDM_MINI, IDM_PREV, IDM_NEXT
};

// Popup menu commands (TrackPopupMenu with TPM_RETURNCMD).
enum {
    CMD_PLAY = 1001, CMD_MOVE_UP, CMD_MOVE_DOWN, CMD_REMOVE,
    CMD_PL_PLAY, CMD_PL_NEW, CMD_PL_RENAME, CMD_PL_DELETE,
    CMD_ADD_NEW = 1050,
    CMD_RATE_BASE = 1100,       // + 0..5
    CMD_ADD_BASE = 2000         // + playlist index
};

enum { TIMER_POSITION = 1, TIMER_SEARCH = 2 };
enum { COL_NUM = 0, COL_TITLE, COL_ARTIST, COL_RATING, COL_LENGTH, COL_ALBUM };

// Sidebar tree item data (playlists use their index, >= 0).
enum { TREE_LIBRARY = -1, TREE_ALBUMS = -2, TREE_PLAYLISTS = -3, TREE_ARTISTS = -4, TREE_SONGS = -5 };

#define WM_APP_JOB_DONE (WM_APP + 10)

static const int kArtSize = 160;        // requested from the server and drawn 1:1
static const size_t kArtCacheMax = 48;  // ~5 MB of 160x160 DIBs
static const int kPad = 6;
static const int kSidebarW = kArtSize + 12;

// --- Background jobs ------------------------------------------------------------------
// Loads (connect/album/playlist) run one at a time. Writes (ratings, playlist edits) and
// now-playing cover fetches run alongside them; playlist edits are serialised because
// removal is by index.

enum JobKind { JOB_CONNECT, JOB_ALBUM, JOB_PLAYLIST, JOB_RATE, JOB_PLEDIT, JOB_ART, JOB_ARTIST, JOB_SONGS };
enum PlEdit { PL_CREATE, PL_RENAME, PL_ADD, PL_REMOVE, PL_SETSONGS, PL_DELETE };

static bool IsLoadJob(JobKind k)
{
    return k == JOB_CONNECT || k == JOB_ALBUM || k == JOB_PLAYLIST || k == JOB_ARTIST || k == JOB_SONGS;
}

struct Job {
    JobKind kind;
    bool ok;
    std::wstring message;

    // loads
    SubsonicAlbum album;
    SubsonicPlaylist playlist;
    SubsonicArtist artist;
    std::vector<SubsonicArtist> artists;
    bool needArt, artTried;
    std::string artId;
    HBITMAP art;
    std::vector<SubsonicAlbum> albums;
    std::vector<SubsonicSong> songs;
    std::vector<SubsonicPlaylist> playlists;

    // rating
    std::string songId;
    int rating, oldRating;

    // playlist edit
    PlEdit edit;
    std::string plId;
    std::wstring name;
    std::vector<std::string> songIds;
    std::string addAlbumId;     // add/create from a whole album (fetched in the job)
    int index;
    std::string reloadId;       // playlist on screen, re-read after the edit
    bool reloaded;
    int selectAfter;
};

enum SongSource { SRC_NONE, SRC_ALBUM, SRC_PLAYLIST, SRC_LIBRARY };
// VIEW_ALBUMS: all albums. VIEW_ARTISTS: all artists. VIEW_ARTIST: one artist's albums
// (same list control as VIEW_ALBUMS). VIEW_TRACKS: an album's or playlist's tracks.
// VIEW_SONGS: every song in the library (same list control as VIEW_TRACKS).
enum MainView { VIEW_ALBUMS, VIEW_TRACKS, VIEW_ARTISTS, VIEW_ARTIST, VIEW_SONGS };

struct Shown {                  // what the header describes
    bool has;
    std::string id, coverArt;
    std::wstring title, line2, line3;
};

struct App {
    HWND wnd, tree, btnNewPlaylist, albums, artists, songs, status;
    HWND search, searchLabel;
    // Mini player (a separate top-level window; Ctrl+M swaps it with the main window).
    HWND mini, miniPrev, miniPlayPause, miniStop, miniNext, miniSeek, miniVolume, miniTimeCur, miniTimeTotal;
    HWND miniTooltip;
    RECT miniSpeakerRect;
    HWND btnPlay, btnPause, btnStop, btnShuffle, hdrPlay, hdrShuffle;
    HWND volume, seek, timeCur, timeTotal, tooltip;
    HMENU menu;
    HFONT font, fontBold, fontTitle;
    HBRUSH latchBrush;
    HTREEITEM treeLibrary, treeAlbums, treeArtists, treeSongs, treePlaylists;
    bool treeUpdating;
    RECT headerRect, artRect, npRect, speakerRect, transportRect;
    MainView mainView;
    MainView albumOrigin;                   // where the open album came from, for Back

    SubsonicClient client;
    Player player;
    std::vector<SubsonicAlbum> albumData;
    std::vector<SubsonicArtist> artistData;
    std::vector<SubsonicAlbum> artistAlbums; // albums of shownArtist
    bool albumListIsArtist;                  // album list control shows artistAlbums
    SubsonicArtist shownArtist;
    std::string shownArtistCover;            // artist image, or their first album's cover
    std::vector<SubsonicSong> allSongs;      // the whole library, loaded on first visit to Songs
    bool allSongsLoaded;
    bool pendingSongs;

    // Search (Albums, Artists, Songs). Each source list has a parallel lower-cased key list,
    // built once when the data arrives; the lists on screen are filtered copies.
    std::vector<SubsonicAlbum> albumList;    // what the album list control shows
    std::vector<SubsonicArtist> artistList;  // what the artist list control shows
    std::vector<std::wstring> albumKeys, artistKeys, songKeys;
    std::wstring queryAlbums, queryArtists, querySongs;
    bool searchUpdating;                     // setting the box's text ourselves
    std::vector<SubsonicPlaylist> playlistData;
    std::vector<SubsonicSong> songData;     // tracks of the album/playlist being shown
    SongSource songSource;
    SubsonicPlaylist shownPlaylist;         // valid when songSource == SRC_PLAYLIST

    // Playback queue: queueSource is the list as it was when playback started; queue is the
    // play order (shuffled or not) and queueSrcIdx maps each queue entry back to the source.
    std::vector<SubsonicSong> queueSource, queue;
    std::vector<int> queueSrcIdx;
    int queueIndex;
    bool shuffle;

    Shown shown;
    HBITMAP shownArt;
    std::string nowCover;                   // cover id of the playing track
    std::string nowCoverPending;
    HBITMAP nowArt;
    std::map<std::string, HBITMAP> artCache; // coverArt id -> bitmap (NULL = none)

    bool seekDragging;
    int shownSec;
    bool pendingLoad;
    int pendingPlaylist;

    volatile LONG loadBusy;
    volatile LONG jobsRunning;
    bool editBusy;
    std::wstring iniPath;
    std::wstring serverUrl;
    CpuCaps cpu;
    std::wstring cpuSummary;
};
static App g_app;

static void SetStatus(const std::wstring& text)
{
    SendMessageW(g_app.status, SB_SETTEXTW, 0, (LPARAM)text.c_str());
}

static std::wstring FormatTime(int sec)
{
    if (sec < 0)
        sec = 0;
    if (sec >= 3600)
        return Fmt(L"%d:%02d:%02d", sec / 3600, (sec / 60) % 60, sec % 60);
    return Fmt(L"%d:%02d", sec / 60, sec % 60);
}

// --- Job thread ----------------------------------------------------------------------

static void FetchArt(Job* job)
{
    if (!job->needArt)
        return;
    std::string bytes;
    std::wstring artErr;
    if (g_app.client.GetCoverArt(job->artId, kArtSize, bytes, artErr))
        job->art = ArtDecode(bytes, kArtSize, GetSysColor(COLOR_BTNFACE), artErr);
    if (!job->art)
        LogF(L"Cover art: %ls", artErr.c_str());
    job->artTried = true;
}

static void RunPlaylistEdit(Job* job)
{
    SubsonicClient& c = g_app.client;
    // Whole-album adds fetch the album's track ids here, off the UI thread.
    bool idsOk = true;
    if (!job->addAlbumId.empty()) {
        std::vector<SubsonicSong> songs;
        idsOk = c.GetAlbum(job->addAlbumId, songs, job->message);
        for (size_t i = 0; i < songs.size(); ++i)
            job->songIds.push_back(songs[i].id);
    }
    switch (job->edit) {
    case PL_CREATE:
        job->ok = idsOk && c.CreatePlaylist(job->name, job->songIds, NULL, job->message);
        break;
    case PL_RENAME:
        job->ok = c.RenamePlaylist(job->plId, job->name, job->message);
        break;
    case PL_ADD:
        job->ok = idsOk && c.AddToPlaylist(job->plId, job->songIds, job->message);
        break;
    case PL_REMOVE:
        job->ok = c.RemoveFromPlaylist(job->plId, job->index, job->message);
        break;
    case PL_SETSONGS:
        job->ok = c.SetPlaylistSongs(job->plId, job->songIds, job->message);
        break;
    case PL_DELETE:
        job->ok = c.DeletePlaylist(job->plId, job->message);
        break;
    }
    // Refresh what's on screen whether or not the edit worked.
    std::wstring err;
    c.GetPlaylists(job->playlists, err);
    if (!job->reloadId.empty() && !(job->edit == PL_DELETE && job->reloadId == job->plId))
        job->reloaded = c.GetPlaylist(job->reloadId, job->songs, err);
}

static unsigned __stdcall JobThread(void* p)
{
    Job* job = (Job*)p;
    SubsonicClient& c = g_app.client;
    switch (job->kind) {
    case JOB_CONNECT:
        job->ok = c.Connect(job->message);
        if (job->ok) {
            std::wstring err;
            if (!c.GetAlbumList(job->albums, err)) {
                job->ok = false;
                job->message = err;
            } else {
                if (!c.GetPlaylists(job->playlists, err))
                    LogF(L"getPlaylists: %ls", err.c_str());   // not fatal
                if (!c.GetArtists(job->artists, err))
                    LogF(L"getArtists: %ls", err.c_str());     // not fatal
            }
        }
        break;
    case JOB_SONGS:
        job->ok = c.GetAllSongs(job->songs, job->message);
        break;
    case JOB_ARTIST:
        job->ok = c.GetArtist(job->artist.id, job->albums, job->message);
        // Navidrome sends a WebP placeholder when it has no artist photo, which we can't
        // decode; fall back to the artist's first album cover.
        FetchArt(job);
        if (!job->art && job->ok) {
            for (size_t i = 0; i < job->albums.size(); ++i)
                if (!job->albums[i].coverArt.empty()) {
                    job->artId = job->albums[i].coverArt;
                    job->artTried = false;
                    job->needArt = true;
                    FetchArt(job);
                    break;
                }
        }
        break;
    case JOB_ALBUM:
        job->ok = c.GetAlbum(job->album.id, job->songs, job->message);
        FetchArt(job);
        break;
    case JOB_PLAYLIST:
        job->ok = c.GetPlaylist(job->playlist.id, job->songs, job->message);
        FetchArt(job);
        break;
    case JOB_RATE:
        job->ok = c.SetRating(job->songId, job->rating, job->message);
        break;
    case JOB_PLEDIT:
        RunPlaylistEdit(job);
        break;
    case JOB_ART:
        FetchArt(job);
        job->ok = job->art != NULL;
        break;
    }
    if (!PostMessageW(g_app.wnd, WM_APP_JOB_DONE, 0, (LPARAM)job)) {
        if (job->art)
            DeleteObject(job->art);
        delete job;
    }
    InterlockedDecrement(&g_app.jobsRunning);
    return 0;
}

static Job* NewJob(JobKind kind)
{
    Job* job = new Job;
    job->kind = kind;
    job->ok = false;
    job->needArt = job->artTried = false;
    job->art = NULL;
    job->rating = job->oldRating = 0;
    job->edit = PL_CREATE;
    job->index = -1;
    job->reloaded = false;
    job->selectAfter = -1;
    return job;
}

static void LaunchJob(Job* job)
{
    InterlockedIncrement(&g_app.jobsRunning);
    HANDLE t = (HANDLE)_beginthreadex(NULL, 0, JobThread, job, 0, NULL);
    if (t) {
        CloseHandle(t);
        return;
    }
    InterlockedDecrement(&g_app.jobsRunning);
    if (IsLoadJob(job->kind))
        InterlockedExchange(&g_app.loadBusy, 0);
    if (job->kind == JOB_PLEDIT)
        g_app.editBusy = false;
    delete job;
    SetStatus(L"Could not start a worker thread.");
}

static bool TryBeginLoad()
{
    return InterlockedCompareExchange(&g_app.loadBusy, 1, 0) == 0;
}

// --- Cover art cache ------------------------------------------------------------------

static void ClearArtCache()
{
    for (std::map<std::string, HBITMAP>::iterator it = g_app.artCache.begin(); it != g_app.artCache.end(); ++it)
        if (it->second)
            DeleteObject(it->second);
    g_app.artCache.clear();
    g_app.shownArt = NULL;
    g_app.nowArt = NULL;
}

static HBITMAP CachedArt(const std::string& id)
{
    std::map<std::string, HBITMAP>::iterator it = g_app.artCache.find(id);
    return it != g_app.artCache.end() ? it->second : NULL;
}

static void CacheArt(const std::string& id, HBITMAP bmp)
{
    std::map<std::string, HBITMAP>::iterator it = g_app.artCache.find(id);
    if (it != g_app.artCache.end()) {
        // Fetched twice (header and now-playing at once): keep the first, it may be on screen.
        if (it->second) {
            if (bmp)
                DeleteObject(bmp);
        } else {
            it->second = bmp;
        }
        return;
    }
    if (g_app.artCache.size() >= kArtCacheMax) {
        // Simple reset that keeps the two bitmaps currently on screen.
        std::map<std::string, HBITMAP> keep;
        for (it = g_app.artCache.begin(); it != g_app.artCache.end(); ++it) {
            if (it->second && (it->second == g_app.shownArt || it->second == g_app.nowArt))
                keep[it->first] = it->second;
            else if (it->second)
                DeleteObject(it->second);
        }
        g_app.artCache.swap(keep);
    }
    g_app.artCache[id] = bmp;
}

static bool NeedArt(const std::string& coverArt)
{
    return !coverArt.empty() && g_app.artCache.find(coverArt) == g_app.artCache.end();
}

// --- Header and now-playing panel ------------------------------------------------------------

static void InvalidateHeader()
{
    InvalidateRect(g_app.wnd, &g_app.headerRect, TRUE);
}

static void ShowInfo(const std::string& id, const std::string& coverArt, const std::wstring& title,
                     const std::wstring& line2, const std::wstring& line3)
{
    g_app.shown.has = true;
    g_app.shown.id = id;
    g_app.shown.coverArt = coverArt;
    g_app.shown.title = title;
    g_app.shown.line2 = line2;
    g_app.shown.line3 = line3;
    g_app.shownArt = CachedArt(coverArt);
    InvalidateHeader();
}

static int TotalDuration(const std::vector<SubsonicSong>& songs)
{
    int total = 0;
    for (size_t i = 0; i < songs.size(); ++i)
        total += songs[i].duration;
    return total;
}

static void ShowPlaylistInfo(const SubsonicPlaylist& pl)
{
    std::wstring by = L"Playlist by " + pl.owner;
    if (!g_app.client.CanEdit(pl))
        by += L"  (read-only)";
    ShowInfo(pl.id, pl.coverArt, pl.name, by,
             Fmt(L"%d tracks  \x00B7  %ls", pl.songCount, FormatTime(pl.duration).c_str()));
}

static void ShowAlbumInfo(const SubsonicAlbum& a)
{
    std::wstring details = Fmt(L"%d tracks  \x00B7  %ls", (int)g_app.songData.size(),
                               FormatTime(TotalDuration(g_app.songData)).c_str());
    if (a.year)
        details = Fmt(L"%d  \x00B7  ", a.year) + details;
    ShowInfo(a.id, a.coverArt, a.name, L"by " + a.artist, details);
}

static bool QueueValid()
{
    return g_app.queueIndex >= 0 && g_app.queueIndex < (int)g_app.queue.size();
}

// Shows the playing track's cover in the sidebar, fetching it if needed.
static void UpdateNowPlaying()
{
    g_app.nowCover = QueueValid() ? g_app.queue[g_app.queueIndex].coverArt : std::string();
    g_app.nowArt = CachedArt(g_app.nowCover);
    if (NeedArt(g_app.nowCover) && g_app.nowCoverPending != g_app.nowCover) {
        g_app.nowCoverPending = g_app.nowCover;
        Job* job = NewJob(JOB_ART);
        job->artId = g_app.nowCover;
        job->needArt = true;
        LaunchJob(job);
    }
    InvalidateRect(g_app.wnd, &g_app.npRect, TRUE);
    if (g_app.mini)
        InvalidateRect(g_app.mini, NULL, TRUE);   // cover, title, artist
}

// --- List views -----------------------------------------------------------------------

static void AddColumn(HWND lv, int index, const wchar_t* text, int width, int fmt)
{
    LVCOLUMNW c;
    ZeroMemory(&c, sizeof(c));
    c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
    c.fmt = fmt;
    c.cx = width;
    c.pszText = (LPWSTR)text;
    ListView_InsertColumn(lv, index, &c);
}

static void SetCell(HWND lv, int row, int col, const std::wstring& text)
{
    if (col == 0) {
        LVITEMW it;
        ZeroMemory(&it, sizeof(it));
        it.mask = LVIF_TEXT;
        it.iItem = row;
        it.pszText = (LPWSTR)text.c_str();
        ListView_InsertItem(lv, &it);
    } else {
        ListView_SetItemText(lv, row, col, (LPWSTR)text.c_str());
    }
}

static int SelectedIndex(HWND lv)
{
    return ListView_GetNextItem(lv, -1, LVNI_SELECTED);
}

static void SelectRow(HWND lv, int i)
{
    if (i < 0 || i >= ListView_GetItemCount(lv))
        return;
    ListView_SetItemState(lv, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(lv, i, FALSE);
}

// --- Search -------------------------------------------------------------------------------

static std::wstring Lower(const std::wstring& s)
{
    std::wstring r = s;
    if (!r.empty())
        CharLowerBuffW(&r[0], (DWORD)r.size());
    return r;
}

// Space-separated words, lower-cased; a row matches when it contains every word.
static std::vector<std::wstring> SearchTerms(const std::wstring& query)
{
    std::vector<std::wstring> terms;
    std::wstring q = Lower(query);
    size_t i = 0;
    while (i < q.size()) {
        size_t j = q.find(L' ', i);
        if (j == std::wstring::npos)
            j = q.size();
        if (j > i)
            terms.push_back(q.substr(i, j - i));
        i = j + 1;
    }
    return terms;
}

static bool MatchesAll(const std::wstring& key, const std::vector<std::wstring>& terms)
{
    for (size_t i = 0; i < terms.size(); ++i)
        if (key.find(terms[i]) == std::wstring::npos)
            return false;
    return true;
}

// Lower-cased "field\nfield" keys, built once per data load rather than on every keystroke.
static void BuildAlbumKeys()
{
    g_app.albumKeys.clear();
    for (size_t i = 0; i < g_app.albumData.size(); ++i)
        g_app.albumKeys.push_back(Lower(g_app.albumData[i].name + L"\n" + g_app.albumData[i].artist));
}

static void BuildArtistKeys()
{
    g_app.artistKeys.clear();
    for (size_t i = 0; i < g_app.artistData.size(); ++i)
        g_app.artistKeys.push_back(Lower(g_app.artistData[i].name));
}

static void BuildSongKeys()
{
    g_app.songKeys.clear();
    for (size_t i = 0; i < g_app.allSongs.size(); ++i) {
        const SubsonicSong& s = g_app.allSongs[i];
        g_app.songKeys.push_back(Lower(s.title + L"\n" + s.artist + L"\n" + s.album));
    }
}

template <class T>
static void FilterInto(const std::vector<T>& all, const std::vector<std::wstring>& keys, const std::wstring& query,
                       std::vector<T>& out)
{
    out.clear();
    std::vector<std::wstring> terms = SearchTerms(query);
    if (terms.empty()) {
        out = all;
        return;
    }
    for (size_t i = 0; i < all.size() && i < keys.size(); ++i)
        if (MatchesAll(keys[i], terms))
            out.push_back(all[i]);
}

// The album list control shows the (searched) list of every album, or one artist's albums.
static std::vector<SubsonicAlbum>& ShownAlbums()
{
    return g_app.albumList;
}

static void FillAlbums()
{
    HWND lv = g_app.albums;
    const std::vector<SubsonicAlbum>& list = ShownAlbums();
    SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(lv);
    for (size_t i = 0; i < list.size(); ++i) {
        const SubsonicAlbum& a = list[i];
        SetCell(lv, (int)i, 0, a.name);
        SetCell(lv, (int)i, 1, a.artist);
        SetCell(lv, (int)i, 2, a.year ? Fmt(L"%d", a.year) : std::wstring());
        SetCell(lv, (int)i, 3, Fmt(L"%d", a.songCount));
    }
    SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lv, NULL, TRUE);
}

// Rebuilds the album list from every album (filtered by the Albums search) or the open artist.
static void RebuildAlbumList()
{
    if (g_app.albumListIsArtist)
        g_app.albumList = g_app.artistAlbums;
    else
        FilterInto(g_app.albumData, g_app.albumKeys, g_app.queryAlbums, g_app.albumList);
    FillAlbums();
}

static void FillArtists()
{
    HWND lv = g_app.artists;
    FilterInto(g_app.artistData, g_app.artistKeys, g_app.queryArtists, g_app.artistList);
    SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(lv);
    for (size_t i = 0; i < g_app.artistList.size(); ++i) {
        SetCell(lv, (int)i, 0, g_app.artistList[i].name);
        SetCell(lv, (int)i, 1, Fmt(L"%d", g_app.artistList[i].albumCount));
    }
    SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lv, NULL, TRUE);
}

// The track list is virtual (LVS_OWNERDATA): it holds only a row count and asks for the
// text of visible cells (OnSongsDispInfo), so the 7,000-song library view fills instantly.
static void FillSongs()
{
    HWND lv = g_app.songs;
    ListView_SetItemState(lv, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetItemCountEx(lv, (int)g_app.songData.size(), 0);
    if (!g_app.songData.empty())
        ListView_EnsureVisible(lv, 0, FALSE);
    InvalidateRect(lv, NULL, TRUE);
}

static void OnSongsDispInfo(NMLVDISPINFOW* di)
{
    const int i = di->item.iItem;
    if (!(di->item.mask & LVIF_TEXT) || di->item.cchTextMax <= 0 || i < 0 || i >= (int)g_app.songData.size())
        return;
    const SubsonicSong& s = g_app.songData[i];
    std::wstring text;
    switch (di->item.iSubItem) {
    case COL_NUM: {
        // Playlists number by position; albums and the library by track number.
        int n = g_app.songSource == SRC_PLAYLIST ? i + 1 : s.track;
        if (n)
            text = Fmt(L"%d", n);
        break;
    }
    case COL_TITLE:  text = s.title; break;
    case COL_ARTIST: text = s.artist; break;
    case COL_LENGTH: text = FormatTime(s.duration); break;
    case COL_ALBUM:  text = s.album; break;
    }
    lstrcpynW(di->item.pszText, text.c_str(), di->item.cchTextMax);
}

// --- Sidebar tree ----------------------------------------------------------------------

static HTREEITEM TreeAdd(HTREEITEM parent, const std::wstring& text, LPARAM data, bool bold)
{
    TVINSERTSTRUCTW ins;
    ZeroMemory(&ins, sizeof(ins));
    ins.hParent = parent;
    ins.hInsertAfter = TVI_LAST;
    ins.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_STATE;
    ins.item.pszText = (LPWSTR)text.c_str();
    ins.item.lParam = data;
    ins.item.stateMask = TVIS_BOLD;
    ins.item.state = bold ? TVIS_BOLD : 0;
    return TreeView_InsertItem(g_app.tree, &ins);
}

static LPARAM TreeData(HTREEITEM item)
{
    TVITEMW it;
    ZeroMemory(&it, sizeof(it));
    it.mask = TVIF_PARAM;
    it.hItem = item;
    if (!item || !TreeView_GetItem(g_app.tree, &it))
        return TREE_LIBRARY;
    return it.lParam;
}

// Programmatic selection changes don't count as the user picking something.
static void TreeSelectQuietly(HTREEITEM item)
{
    g_app.treeUpdating = true;
    TreeView_SelectItem(g_app.tree, item);
    g_app.treeUpdating = false;
}

static void RebuildPlaylistTree()
{
    g_app.treeUpdating = true;
    HTREEITEM child;
    while ((child = TreeView_GetChild(g_app.tree, g_app.treePlaylists)) != NULL)
        TreeView_DeleteItem(g_app.tree, child);
    HTREEITEM select = NULL;
    for (size_t i = 0; i < g_app.playlistData.size(); ++i) {
        HTREEITEM h = TreeAdd(g_app.treePlaylists, g_app.playlistData[i].name, (LPARAM)i, false);
        if (g_app.mainView == VIEW_TRACKS && g_app.songSource == SRC_PLAYLIST &&
            g_app.playlistData[i].id == g_app.shownPlaylist.id)
            select = h;
    }
    TreeView_Expand(g_app.tree, g_app.treePlaylists, TVE_EXPAND);
    if (select)
        TreeView_SelectItem(g_app.tree, select);
    else if (g_app.mainView == VIEW_ALBUMS)
        TreeView_SelectItem(g_app.tree, g_app.treeAlbums);
    else if (g_app.mainView == VIEW_ARTISTS)
        TreeView_SelectItem(g_app.tree, g_app.treeArtists);
    else if (g_app.mainView == VIEW_SONGS)
        TreeView_SelectItem(g_app.tree, g_app.treeSongs);
    g_app.treeUpdating = false;
}

// --- Star rating column ----------------------------------------------------------------

// Unit star outline: 10 points, outer/inner alternating, x1000 (no libm needed).
static const int kStar[10][2] = {
    {0, -1000}, {225, -309}, {951, -309}, {363, 118}, {588, 809},
    {0, 382}, {-588, 809}, {-363, 118}, {-951, -309}, {-225, -309}
};

static int StarSize(const RECT& cell)
{
    int s = cell.bottom - cell.top - 4;
    return s > 12 ? 12 : (s < 7 ? 7 : s);
}

static void DrawStar(HDC dc, int cx, int cy, int r, bool filled, COLORREF line)
{
    POINT pts[10];
    for (int i = 0; i < 10; ++i) {
        pts[i].x = cx + kStar[i][0] * r / 1000;
        pts[i].y = cy + kStar[i][1] * r / 1000;
    }
    HPEN pen = CreatePen(PS_SOLID, 1, filled ? RGB(176, 120, 0) : line);
    HBRUSH brush = filled ? CreateSolidBrush(RGB(244, 184, 0)) : NULL;
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, brush ? (HGDIOBJ)brush : GetStockObject(NULL_BRUSH));
    Polygon(dc, pts, 10);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
    if (brush)
        DeleteObject(brush);
}

static void DrawRating(HDC dc, int item)
{
    if (item < 0 || item >= (int)g_app.songData.size())
        return;
    RECT cell;
    if (!ListView_GetSubItemRect(g_app.songs, item, COL_RATING, LVIR_BOUNDS, &cell))
        return;
    const int rating = g_app.songData[item].userRating;
    const bool highlighted = (ListView_GetItemState(g_app.songs, item, LVIS_SELECTED) & LVIS_SELECTED) &&
                             GetFocus() == g_app.songs;
    const COLORREF empty = GetSysColor(highlighted ? COLOR_HIGHLIGHTTEXT : COLOR_GRAYTEXT);
    const int s = StarSize(cell), r = s / 2, cy = (cell.top + cell.bottom) / 2 + 1;

    HRGN clip = CreateRectRgnIndirect(&cell);
    SelectClipRgn(dc, clip);
    for (int i = 0; i < 5; ++i)
        DrawStar(dc, cell.left + 4 + r + i * (s + 2), cy, r, i < rating, empty);
    SelectClipRgn(dc, NULL);
    DeleteObject(clip);
}

static LRESULT OnSongsCustomDraw(NMLVCUSTOMDRAW* cd)
{
    switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT: {
        // The playing track is shown in bold, as in the classic Spotify list.
        int item = (int)cd->nmcd.dwItemSpec;
        if (QueueValid() && item < (int)g_app.songData.size() &&
            g_app.songData[item].id == g_app.queue[g_app.queueIndex].id) {
            SelectObject(cd->nmcd.hdc, g_app.fontBold);
            return CDRF_NEWFONT | CDRF_NOTIFYPOSTPAINT;
        }
        return CDRF_NOTIFYPOSTPAINT;
    }
    case CDDS_ITEMPOSTPAINT:
        DrawRating(cd->nmcd.hdc, (int)cd->nmcd.dwItemSpec);
        return CDRF_DODEFAULT;
    }
    return CDRF_DODEFAULT;
}

// Updates the rating everywhere the track appears and repaints its rows.
static void ApplyRatingLocal(const std::string& songId, int rating)
{
    for (size_t i = 0; i < g_app.songData.size(); ++i)
        if (g_app.songData[i].id == songId) {
            g_app.songData[i].userRating = rating;
            ListView_RedrawItems(g_app.songs, (int)i, (int)i);
        }
    for (size_t i = 0; i < g_app.queue.size(); ++i)
        if (g_app.queue[i].id == songId)
            g_app.queue[i].userRating = rating;
    for (size_t i = 0; i < g_app.queueSource.size(); ++i)
        if (g_app.queueSource[i].id == songId)
            g_app.queueSource[i].userRating = rating;
    for (size_t i = 0; i < g_app.allSongs.size(); ++i)
        if (g_app.allSongs[i].id == songId) {
            g_app.allSongs[i].userRating = rating;
            break;
        }
}

static void SetSongRating(int i, int rating)
{
    if (i < 0 || i >= (int)g_app.songData.size() || rating < 0 || rating > 5)
        return;
    SubsonicSong& s = g_app.songData[i];
    if (s.userRating == rating)
        return;
    Job* job = NewJob(JOB_RATE);
    job->songId = s.id;
    job->rating = rating;
    job->oldRating = s.userRating;
    // Optimistic: show it now, revert if the server refuses.
    ApplyRatingLocal(s.id, rating);
    SetStatus(rating ? Fmt(L"Rated \"%ls\" %d/5", s.title.c_str(), rating)
                     : Fmt(L"Cleared the rating of \"%ls\"", s.title.c_str()));
    LaunchJob(job);
}

// Click on the stars: star N sets N; clicking the current rating clears it.
static void OnSongsClick(NMITEMACTIVATE* ia)
{
    LVHITTESTINFO hti;
    ZeroMemory(&hti, sizeof(hti));
    hti.pt = ia->ptAction;
    if (ListView_SubItemHitTest(g_app.songs, &hti) < 0 || hti.iSubItem != COL_RATING)
        return;
    RECT cell;
    if (!ListView_GetSubItemRect(g_app.songs, hti.iItem, COL_RATING, LVIR_BOUNDS, &cell))
        return;
    const int s = StarSize(cell);
    int star = (hti.pt.x - cell.left - 4) / (s + 2) + 1;
    if (star < 1 || star > 5)
        return;
    if (g_app.songData[hti.iItem].userRating == star)
        star = 0;
    SetSongRating(hti.iItem, star);
}

// --- Position / seek bar ---------------------------------------------------------------

// The main window and the mini player each have a seek bar and time labels; these keep
// both in step (the mini player's are NULL until it is created).
static void SetSeekPos(int sec)
{
    SendMessageW(g_app.seek, TBM_SETPOS, TRUE, sec);
    if (g_app.miniSeek)
        SendMessageW(g_app.miniSeek, TBM_SETPOS, TRUE, sec);
}

static void SetTimeCur(int sec)
{
    std::wstring t = FormatTime(sec);
    SetWindowTextW(g_app.timeCur, t.c_str());
    if (g_app.miniTimeCur)
        SetWindowTextW(g_app.miniTimeCur, t.c_str());
}

static void ResetSeekBar(int durationSec)
{
    HWND bars[2] = { g_app.seek, g_app.miniSeek };
    for (int i = 0; i < 2; ++i) {
        if (!bars[i])
            continue;
        SendMessageW(bars[i], TBM_SETRANGEMIN, FALSE, 0);
        SendMessageW(bars[i], TBM_SETRANGEMAX, FALSE, durationSec > 0 ? durationSec : 1);
    }
    SetSeekPos(0);
    SetTimeCur(0);
    SetWindowTextW(g_app.timeTotal, FormatTime(durationSec).c_str());
    if (g_app.miniTimeTotal)
        SetWindowTextW(g_app.miniTimeTotal, FormatTime(durationSec).c_str());
    g_app.shownSec = 0;
}

static void UpdatePosition()
{
    if (g_app.seekDragging || !g_app.player.IsActive() || !QueueValid())
        return;
    int sec = (int)(g_app.player.PositionMs() / 1000);
    int dur = g_app.queue[g_app.queueIndex].duration;
    if (dur > 0 && sec > dur)
        sec = dur;
    if (sec == g_app.shownSec)
        return;
    g_app.shownSec = sec;
    SetSeekPos(sec);
    SetTimeCur(sec);
}

// --- Playback and shuffle ---------------------------------------------------------------------

static void RefreshTransport()
{
    InvalidateRect(g_app.btnPause, NULL, FALSE);
    InvalidateRect(g_app.btnShuffle, NULL, FALSE);
    if (g_app.miniPlayPause)
        InvalidateRect(g_app.miniPlayPause, NULL, FALSE);
    CheckMenuItem(g_app.menu, IDM_SHUFFLE, g_app.shuffle ? MF_CHECKED : MF_UNCHECKED);
}

static int RandomBelow(int n)
{
    return n > 0 ? (int)((((unsigned)rand() << 15) | (unsigned)rand()) % (unsigned)n) : 0;
}

// Builds the play order from queueSource. With shuffle on, 'startSrc' plays first and the
// rest follow in random order; otherwise the source order is kept.
static void BuildQueue(int startSrc)
{
    const int n = (int)g_app.queueSource.size();
    g_app.queueSrcIdx.clear();
    if (g_app.shuffle) {
        std::vector<int> rest;
        for (int i = 0; i < n; ++i)
            if (i != startSrc)
                rest.push_back(i);
        for (int i = (int)rest.size() - 1; i > 0; --i)
            std::swap(rest[i], rest[RandomBelow(i + 1)]);   // Fisher-Yates
        g_app.queueSrcIdx.push_back(startSrc);
        g_app.queueSrcIdx.insert(g_app.queueSrcIdx.end(), rest.begin(), rest.end());
    } else {
        for (int i = 0; i < n; ++i)
            g_app.queueSrcIdx.push_back(i);
    }
    g_app.queue.clear();
    for (size_t i = 0; i < g_app.queueSrcIdx.size(); ++i)
        g_app.queue.push_back(g_app.queueSource[g_app.queueSrcIdx[i]]);
    g_app.queueIndex = g_app.shuffle ? 0 : startSrc;
}

static void StartQueue(int i, DWORD startMs)
{
    if (i < 0 || i >= (int)g_app.queue.size())
        return;
    const SubsonicSong& s = g_app.queue[i];
    g_app.queueIndex = i;

    // Transcoded streams seek on the server (timeOffset, whole seconds); original MP3s that
    // Navidrome passes through untouched are skipped client-side by frame headers.
    const bool serverSide = g_app.client.ServerTranscodes(s);
    const int offsetSec = (int)(startMs / 1000);
    std::wstring url = g_app.client.StreamUrl(s.id, serverSide ? offsetSec : 0);
    DWORD start = serverSide ? (DWORD)offsetSec * 1000 : startMs;
    DWORD skip = serverSide ? 0 : startMs;

    ResetSeekBar(s.duration);
    if (start) {
        g_app.shownSec = (int)(start / 1000);
        SetSeekPos(g_app.shownSec);
        SetTimeCur(g_app.shownSec);
    }

    // Highlight it if the list on screen holds it at the same position.
    int row = g_app.queueSrcIdx[i];
    if (row < (int)g_app.songData.size() && g_app.songData[row].id == s.id)
        SelectRow(g_app.songs, row);
    InvalidateRect(g_app.songs, NULL, FALSE);   // bold "now playing" row
    SetStatus(L"Buffering: " + s.title);
    g_app.player.Play(url, g_app.client.Options(), start, skip);
    UpdateNowPlaying();
    RefreshTransport();
}

static void PlayFromSongList(int i)
{
    if (i < 0 || i >= (int)g_app.songData.size())
        return;
    g_app.queueSource = g_app.songData;
    BuildQueue(i);
    StartQueue(g_app.queueIndex, 0);
}

static void ToggleShuffle()
{
    g_app.shuffle = !g_app.shuffle;
    WritePrivateProfileStringW(L"Player", L"Shuffle", g_app.shuffle ? L"1" : L"0", g_app.iniPath.c_str());
    if (QueueValid()) {
        // Reorder what's left around the current track; it keeps playing.
        int cur = g_app.queueSrcIdx[g_app.queueIndex];
        BuildQueue(cur);
    }
    RefreshTransport();
    SetStatus(g_app.shuffle ? L"Shuffle on." : L"Shuffle off.");
}

static void SeekTo(int sec)
{
    if (!QueueValid()) {
        ResetSeekBar(0);
        return;
    }
    int dur = g_app.queue[g_app.queueIndex].duration;
    if (sec < 0)
        sec = 0;
    if (dur > 1 && sec > dur - 1)
        sec = dur - 1;
    StartQueue(g_app.queueIndex, (DWORD)sec * 1000);
}

static void StopPlayback()
{
    g_app.player.Stop();
    ResetSeekBar(QueueValid() ? g_app.queue[g_app.queueIndex].duration : 0);
    RefreshTransport();
    SetStatus(L"Stopped.");
}

static void DoPause()
{
    if (!g_app.player.IsActive())
        return;
    if (g_app.player.TogglePause())
        SetStatus(L"Paused.");
    else if (QueueValid())
        SetStatus(L"Playing: " + g_app.queue[g_app.queueIndex].title);
    RefreshTransport();
}

// Play: resume if paused, otherwise play the selected track (or restart the current one).
static void DoPlay()
{
    if (g_app.player.IsPaused()) {
        DoPause();
        return;
    }
    if ((g_app.mainView == VIEW_TRACKS || g_app.mainView == VIEW_SONGS) && !g_app.songData.empty()) {
        int sel = SelectedIndex(g_app.songs);
        PlayFromSongList(sel >= 0 ? sel : (g_app.shuffle ? RandomBelow((int)g_app.songData.size()) : 0));
    } else if (QueueValid()) {
        StartQueue(g_app.queueIndex, 0);
    }
}

// Previous: restart the track if more than 3 s in (or it's the first), else the one before.
static void PlayPrevious()
{
    if (!QueueValid())
        return;
    if (g_app.queueIndex == 0 || (g_app.player.IsActive() && g_app.player.PositionMs() > 3000))
        StartQueue(g_app.queueIndex, 0);
    else
        StartQueue(g_app.queueIndex - 1, 0);
}

static void PlayNext()
{
    if (QueueValid() && g_app.queueIndex + 1 < (int)g_app.queue.size())
        StartQueue(g_app.queueIndex + 1, 0);
}

// --- Views and loading -----------------------------------------------------------------------

static void Layout(HWND hwnd);
static void ToggleMini();

// The search text of the current library section, or NULL on album/artist/playlist pages.
static std::wstring* CurrentQuery()
{
    switch (g_app.mainView) {
    case VIEW_ALBUMS:  return &g_app.queryAlbums;
    case VIEW_ARTISTS: return &g_app.queryArtists;
    case VIEW_SONGS:   return &g_app.querySongs;
    default:           return NULL;
    }
}

static HWND CurrentList()
{
    switch (g_app.mainView) {
    case VIEW_ARTISTS: return g_app.artists;
    case VIEW_TRACKS:
    case VIEW_SONGS:   return g_app.songs;
    default:           return g_app.albums;
    }
}

static void SetView(MainView v)
{
    g_app.mainView = v;
    const bool albumList = v == VIEW_ALBUMS || v == VIEW_ARTIST;
    const bool trackList = v == VIEW_TRACKS || v == VIEW_SONGS;
    ShowWindow(g_app.albums, albumList ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app.artists, v == VIEW_ARTISTS ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app.songs, trackList ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app.hdrPlay, trackList ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app.hdrShuffle, trackList ? SW_SHOW : SW_HIDE);
    // Each library section keeps its own search text.
    const std::wstring* query = CurrentQuery();
    ShowWindow(g_app.search, query ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app.searchLabel, query ? SW_SHOW : SW_HIDE);
    if (query) {
        g_app.searchUpdating = true;
        SetWindowTextW(g_app.search, query->c_str());
        g_app.searchUpdating = false;
    }
    Layout(g_app.wnd);
    InvalidateHeader();
}

// Puts every album (or the open artist's albums) into the album list, if it isn't already.
static void ShowAlbumList(bool artist)
{
    if (g_app.albumListIsArtist != artist) {
        g_app.albumListIsArtist = artist;
        RebuildAlbumList();
        SelectRow(g_app.albums, 0);
    }
}

static void ShowAlbumsView()
{
    ShowAlbumList(false);
    SetView(VIEW_ALBUMS);
    TreeSelectQuietly(g_app.treeAlbums);
    SetFocus(g_app.albums);
}

static void LoadAllSongs()
{
    if (!TryBeginLoad()) {
        g_app.pendingSongs = true;   // retried when the running load finishes
        return;
    }
    SetStatus(L"Loading every song in the library...");
    LaunchJob(NewJob(JOB_SONGS));
}

// Library > Songs. The full list is fetched once per connection, then reused.
static void ShowSongsView()
{
    g_app.songSource = SRC_LIBRARY;
    if (g_app.allSongsLoaded)
        FilterInto(g_app.allSongs, g_app.songKeys, g_app.querySongs, g_app.songData);
    else {
        g_app.songData.clear();
        LoadAllSongs();
    }
    FillSongs();
    SetView(VIEW_SONGS);
    TreeSelectQuietly(g_app.treeSongs);
    SelectRow(g_app.songs, 0);
    SetFocus(g_app.songs);
}

static void ShowArtistsView()
{
    SetView(VIEW_ARTISTS);
    TreeSelectQuietly(g_app.treeArtists);
    SetFocus(g_app.artists);
}

// Re-filters the current library section from the search box (after a short typing pause).
static void ApplySearch()
{
    KillTimer(g_app.wnd, TIMER_SEARCH);
    std::wstring* query = CurrentQuery();
    if (!query)
        return;
    wchar_t buf[256];
    GetWindowTextW(g_app.search, buf, 256);
    if (*query == buf)
        return;
    *query = buf;
    if (g_app.mainView == VIEW_ALBUMS) {
        RebuildAlbumList();
        SelectRow(g_app.albums, 0);
    } else if (g_app.mainView == VIEW_ARTISTS) {
        FillArtists();
        SelectRow(g_app.artists, 0);
    } else if (g_app.mainView == VIEW_SONGS && g_app.allSongsLoaded) {
        FilterInto(g_app.allSongs, g_app.songKeys, g_app.querySongs, g_app.songData);
        FillSongs();
        SelectRow(g_app.songs, 0);
    }
    InvalidateHeader();
}

// Search box keys: Esc clears it; Enter or Down moves into the results.
static WNDPROC g_searchEditProc;

static LRESULT CALLBACK SearchEditProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN) {
        if (wp == VK_ESCAPE) {
            SetWindowTextW(h, L"");
            ApplySearch();
            return 0;
        }
        if (wp == VK_RETURN || wp == VK_DOWN) {
            ApplySearch();
            HWND lv = CurrentList();
            SetFocus(lv);
            if (SelectedIndex(lv) < 0)
                SelectRow(lv, 0);
            return 0;
        }
    }
    if (msg == WM_CHAR && (wp == L'\r' || wp == 27))
        return 0;   // no beep
    return CallWindowProcW(g_searchEditProc, h, msg, wp, lp);
}

// Ctrl+F: focus the search box, switching to Albums from a page that has none.
static void FocusSearch()
{
    if (!CurrentQuery())
        ShowAlbumsView();
    SetFocus(g_app.search);
    SendMessageW(g_app.search, EM_SETSEL, 0, -1);
}

static void ShowArtistInfo()
{
    const SubsonicArtist& ar = g_app.shownArtist;
    int tracks = 0;
    for (size_t i = 0; i < g_app.artistAlbums.size(); ++i)
        tracks += g_app.artistAlbums[i].songCount;
    ShowInfo(ar.id, g_app.shownArtistCover, ar.name,
             Fmt(L"%u album%ls", (unsigned)g_app.artistAlbums.size(), g_app.artistAlbums.size() == 1 ? L"" : L"s"),
             Fmt(L"%d tracks", tracks));
}

// Back to the artist page from one of their albums (its list is still loaded).
static void ShowArtistPage()
{
    ShowAlbumList(true);
    ShowArtistInfo();
    SetView(VIEW_ARTIST);
    TreeSelectQuietly(NULL);
    SetFocus(g_app.albums);
}

// Backspace: tracks -> the album list it came from; an artist's albums -> all artists.
static void GoBack()
{
    if (g_app.mainView == VIEW_TRACKS && g_app.songSource == SRC_ALBUM) {
        if (g_app.albumOrigin == VIEW_ARTIST)
            ShowArtistPage();
        else
            ShowAlbumsView();
    } else if (g_app.mainView == VIEW_ARTIST) {
        ShowArtistsView();
    }
}

static void ClearSongs()
{
    g_app.songData.clear();
    g_app.songSource = SRC_NONE;
    g_app.shown.has = false;
    g_app.shownArt = NULL;
    FillSongs();
    InvalidateHeader();
}

static void DoConnect()
{
    if (!TryBeginLoad())
        return;
    if (g_app.jobsRunning > 0) {     // a rating/playlist edit is still talking to the server
        InterlockedExchange(&g_app.loadBusy, 0);
        SetStatus(L"Still saving a change; try Connect again in a moment.");
        return;
    }

    SubsonicConfig cfg;
    std::wstring err;
    if (!ConfigLoad(g_app.iniPath, cfg, err)) {
        InterlockedExchange(&g_app.loadBusy, 0);
        if (MessageBoxW(g_app.wnd, (err + L"\r\n\r\nOpen it in Notepad now?").c_str(), APP_TITLE,
                        MB_ICONINFORMATION | MB_YESNO) == IDYES)
            ShellExecuteW(g_app.wnd, L"open", L"notepad.exe", g_app.iniPath.c_str(), NULL, SW_SHOWNORMAL);
        return;
    }

    g_app.player.Stop();
    g_app.albumData.clear();
    g_app.playlistData.clear();
    g_app.artistData.clear();
    g_app.artistAlbums.clear();
    g_app.albumListIsArtist = false;
    g_app.allSongs.clear();
    g_app.allSongsLoaded = false;
    g_app.pendingSongs = false;
    g_app.albumKeys.clear();
    g_app.artistKeys.clear();
    g_app.songKeys.clear();
    g_app.queryAlbums.clear();
    g_app.queryArtists.clear();
    g_app.querySongs.clear();
    FillArtists();
    g_app.queue.clear();
    g_app.queueSource.clear();
    g_app.queueSrcIdx.clear();
    g_app.queueIndex = -1;
    g_app.nowCoverPending.clear();
    ClearArtCache();
    ClearSongs();
    RebuildAlbumList();
    RebuildPlaylistTree();
    ResetSeekBar(0);
    UpdateNowPlaying();
    ShowAlbumsView();
    g_app.client.Configure(cfg);
    g_app.serverUrl = cfg.url;

    SetStatus(L"Connecting to " + cfg.url + L"...");
    LaunchJob(NewJob(JOB_CONNECT));
}

static void OpenSelectedArtist()
{
    int i = SelectedIndex(g_app.artists);
    if (i < 0 || i >= (int)g_app.artistList.size() || !TryBeginLoad())
        return;
    const SubsonicArtist& ar = g_app.artistList[i];
    Job* job = NewJob(JOB_ARTIST);
    job->artist = ar;
    job->artId = ar.coverArt;
    job->needArt = !ar.coverArt.empty();   // always try: a cached miss still allows the album fallback
    SetStatus(L"Loading " + ar.name + L"...");
    LaunchJob(job);
}

static void OpenSelectedAlbum()
{
    const std::vector<SubsonicAlbum>& list = ShownAlbums();
    int i = SelectedIndex(g_app.albums);
    if (i < 0 || i >= (int)list.size())
        return;
    if (!TryBeginLoad()) {
        g_app.pendingLoad = true;   // retried when the running load finishes
        g_app.pendingPlaylist = -1;
        return;
    }
    const SubsonicAlbum& a = list[i];
    g_app.albumOrigin = g_app.mainView == VIEW_ARTIST ? VIEW_ARTIST : VIEW_ALBUMS;
    Job* job = NewJob(JOB_ALBUM);
    job->album = a;
    job->artId = a.coverArt;
    job->needArt = NeedArt(a.coverArt);
    SetStatus(L"Loading " + a.name + L"...");
    LaunchJob(job);
}

static void OpenPlaylist(int i)
{
    if (i < 0 || i >= (int)g_app.playlistData.size())
        return;
    if (!TryBeginLoad()) {
        g_app.pendingLoad = true;
        g_app.pendingPlaylist = i;
        return;
    }
    const SubsonicPlaylist& p = g_app.playlistData[i];
    Job* job = NewJob(JOB_PLAYLIST);
    job->playlist = p;
    job->artId = p.coverArt;
    job->needArt = NeedArt(p.coverArt);
    SetStatus(L"Loading " + p.name + L"...");
    LaunchJob(job);
}

// --- Playlist editing -----------------------------------------------------------------------

struct NameDlgData {
    const wchar_t* caption;
    std::wstring* name;
};

static INT_PTR CALLBACK NameDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG: {
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        NameDlgData* d = (NameDlgData*)lp;
        SetWindowTextW(dlg, d->caption);
        SendDlgItemMessageW(dlg, IDC_NAME_EDIT, EM_LIMITTEXT, 200, 0);
        SetDlgItemTextW(dlg, IDC_NAME_EDIT, d->name->c_str());
        SendDlgItemMessageW(dlg, IDC_NAME_EDIT, EM_SETSEL, 0, -1);
        SetFocus(GetDlgItem(dlg, IDC_NAME_EDIT));
        return FALSE;   // focus set by hand
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            wchar_t buf[256];
            GetDlgItemTextW(dlg, IDC_NAME_EDIT, buf, 256);
            std::wstring s = buf;
            size_t a = s.find_first_not_of(L" \t"), b = s.find_last_not_of(L" \t");
            if (a == std::wstring::npos) {
                MessageBeep(MB_ICONEXCLAMATION);
                return TRUE;
            }
            *((NameDlgData*)GetWindowLongPtrW(dlg, DWLP_USER))->name = s.substr(a, b - a + 1);
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static bool AskName(const wchar_t* caption, std::wstring& name)
{
    std::wstring value = name;
    NameDlgData d = { caption, &value };
    INT_PTR r = DialogBoxParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_NAME), g_app.wnd,
                                NameDlgProc, (LPARAM)&d);
    if (r != IDOK)
        return false;
    name = value;
    return true;
}

static void StartEdit(Job* job, const std::wstring& status)
{
    if (g_app.editBusy) {
        delete job;
        SetStatus(L"Still saving the previous playlist change...");
        return;
    }
    g_app.editBusy = true;
    if (g_app.songSource == SRC_PLAYLIST)
        job->reloadId = g_app.shownPlaylist.id;
    SetStatus(status);
    LaunchJob(job);
}

static bool ShownPlaylistEditable()
{
    return g_app.songSource == SRC_PLAYLIST && g_app.client.CanEdit(g_app.shownPlaylist);
}

static void NewPlaylist(const std::vector<std::string>& songIds, const std::string& albumId,
                        const std::wstring& suggested)
{
    std::wstring name = suggested;
    if (!AskName(L"New playlist", name))
        return;
    Job* job = NewJob(JOB_PLEDIT);
    job->edit = PL_CREATE;
    job->name = name;
    job->songIds = songIds;
    job->addAlbumId = albumId;
    StartEdit(job, L"Creating playlist \"" + name + L"\"...");
}

static void AddToPlaylist(int plIndex, const std::vector<std::string>& songIds, const std::string& albumId)
{
    if (plIndex < 0 || plIndex >= (int)g_app.playlistData.size())
        return;
    Job* job = NewJob(JOB_PLEDIT);
    job->edit = PL_ADD;
    job->plId = g_app.playlistData[plIndex].id;
    job->songIds = songIds;
    job->addAlbumId = albumId;
    StartEdit(job, L"Adding to \"" + g_app.playlistData[plIndex].name + L"\"...");
}

static void MoveInPlaylist(int i, int delta)
{
    int j = i + delta;
    if (!ShownPlaylistEditable() || i < 0 || j < 0 || j >= (int)g_app.songData.size())
        return;
    Job* job = NewJob(JOB_PLEDIT);
    job->edit = PL_SETSONGS;
    job->plId = g_app.shownPlaylist.id;
    for (size_t k = 0; k < g_app.songData.size(); ++k)
        job->songIds.push_back(g_app.songData[k].id);
    std::swap(job->songIds[i], job->songIds[j]);
    job->selectAfter = j;
    StartEdit(job, L"Moving \"" + g_app.songData[i].title + L"\"...");
}

static void RemoveFromPlaylist(int i)
{
    if (!ShownPlaylistEditable() || i < 0 || i >= (int)g_app.songData.size())
        return;
    Job* job = NewJob(JOB_PLEDIT);
    job->edit = PL_REMOVE;
    job->plId = g_app.shownPlaylist.id;
    job->index = i;
    job->selectAfter = (i + 1 < (int)g_app.songData.size()) ? i : i - 1;
    StartEdit(job, L"Removing \"" + g_app.songData[i].title + L"\"...");
}

static void RenamePlaylist(int plIndex)
{
    if (plIndex < 0 || plIndex >= (int)g_app.playlistData.size())
        return;
    const SubsonicPlaylist& p = g_app.playlistData[plIndex];
    std::wstring name = p.name;
    if (!AskName(L"Rename playlist", name) || name == p.name)
        return;
    Job* job = NewJob(JOB_PLEDIT);
    job->edit = PL_RENAME;
    job->plId = p.id;
    job->name = name;
    StartEdit(job, L"Renaming to \"" + name + L"\"...");
}

static void DeletePlaylist(int plIndex)
{
    if (plIndex < 0 || plIndex >= (int)g_app.playlistData.size())
        return;
    const SubsonicPlaylist& p = g_app.playlistData[plIndex];
    std::wstring q = L"Delete the playlist \"" + p.name + L"\"?\r\n\r\nThis can't be undone.";
    if (MessageBoxW(g_app.wnd, q.c_str(), APP_TITLE, MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) != IDYES)
        return;
    Job* job = NewJob(JOB_PLEDIT);
    job->edit = PL_DELETE;
    job->plId = p.id;
    StartEdit(job, L"Deleting \"" + p.name + L"\"...");
}

// --- Job completion ----------------------------------------------------------------------

static void OnPlaylistEditDone(Job* job)
{
    g_app.editBusy = false;
    if (!job->playlists.empty() || job->ok)
        g_app.playlistData.swap(job->playlists);

    if (job->edit == PL_DELETE && job->ok && g_app.songSource == SRC_PLAYLIST && g_app.shownPlaylist.id == job->plId) {
        ClearSongs();
        g_app.mainView = VIEW_ALBUMS;   // RebuildPlaylistTree selects Albums
        SetView(VIEW_ALBUMS);
    }
    if (job->reloaded && g_app.songSource == SRC_PLAYLIST && g_app.shownPlaylist.id == job->reloadId) {
        g_app.songData.swap(job->songs);
        FillSongs();
        SelectRow(g_app.songs, job->selectAfter);
        for (size_t i = 0; i < g_app.playlistData.size(); ++i)
            if (g_app.playlistData[i].id == job->reloadId) {
                g_app.shownPlaylist = g_app.playlistData[i];
                ShowPlaylistInfo(g_app.shownPlaylist);
            }
    }
    RebuildPlaylistTree();

    if (!job->ok) {
        SetStatus(L"The playlist change failed.");
        MessageBoxW(g_app.wnd, job->message.c_str(), APP_TITLE, MB_ICONERROR);
        return;
    }
    static const wchar_t* kDone[] = { L"Playlist created.", L"Playlist renamed.", L"Added to playlist.",
                                      L"Removed from playlist.", L"Playlist reordered.", L"Playlist deleted." };
    SetStatus(kDone[job->edit]);
}

static void OnJobDone(Job* job)
{
    if (IsLoadJob(job->kind))
        InterlockedExchange(&g_app.loadBusy, 0);
    if (job->artTried) {
        CacheArt(job->artId, job->art);
        // Either panel may be waiting for this cover.
        if (g_app.nowCover == job->artId) {
            g_app.nowArt = CachedArt(job->artId);
            InvalidateRect(g_app.wnd, &g_app.npRect, TRUE);
            if (g_app.mini)
                InvalidateRect(g_app.mini, NULL, TRUE);
        }
        if (g_app.shown.has && g_app.shown.coverArt == job->artId) {
            g_app.shownArt = CachedArt(job->artId);
            InvalidateHeader();
        }
    }

    switch (job->kind) {
    case JOB_CONNECT:
        if (job->ok) {
            g_app.albumData.swap(job->albums);
            g_app.playlistData.swap(job->playlists);
            g_app.artistData.swap(job->artists);
            BuildAlbumKeys();
            BuildArtistKeys();
            RebuildAlbumList();
            FillArtists();
            SelectRow(g_app.artists, 0);
            RebuildPlaylistTree();
            SetStatus(Fmt(L"%u albums, %u playlist%ls. %ls", (unsigned)g_app.albumData.size(),
                          (unsigned)g_app.playlistData.size(), g_app.playlistData.size() == 1 ? L"" : L"s",
                          g_app.cpuSummary.c_str()));
            if (!job->message.empty())
                MessageBoxW(g_app.wnd, job->message.c_str(), APP_TITLE, MB_ICONWARNING);
            SelectRow(g_app.albums, 0);
            ShowAlbumsView();
        } else {
            SetStatus(L"Not connected.");
            MessageBoxW(g_app.wnd, job->message.c_str(), APP_TITLE L" - connection failed", MB_ICONERROR);
        }
        InvalidateHeader();
        break;

    case JOB_ALBUM:
    case JOB_PLAYLIST:
        if (job->ok) {
            g_app.songData.swap(job->songs);
            if (job->kind == JOB_ALBUM) {
                g_app.songSource = SRC_ALBUM;
                ShowAlbumInfo(job->album);
                TreeSelectQuietly(NULL);   // so clicking "Albums" goes back
            } else {
                g_app.songSource = SRC_PLAYLIST;
                g_app.shownPlaylist = job->playlist;
                ShowPlaylistInfo(job->playlist);
            }
            FillSongs();
            SetView(VIEW_TRACKS);
            SelectRow(g_app.songs, 0);
            SetFocus(g_app.songs);
            SetStatus(Fmt(L"%ls: %u tracks.", g_app.shown.title.c_str(), (unsigned)g_app.songData.size()));
        } else {
            SetStatus(L"Could not load the list.");
            MessageBoxW(g_app.wnd, job->message.c_str(), APP_TITLE, MB_ICONERROR);
        }
        break;

    case JOB_RATE:
        if (!job->ok) {
            ApplyRatingLocal(job->songId, job->oldRating);
            SetStatus(L"The rating was not saved.");
            MessageBoxW(g_app.wnd, job->message.c_str(), APP_TITLE, MB_ICONERROR);
        }
        break;

    case JOB_PLEDIT:
        OnPlaylistEditDone(job);
        break;

    case JOB_SONGS:
        if (job->ok) {
            g_app.allSongs.swap(job->songs);
            g_app.allSongsLoaded = true;
            BuildSongKeys();
            if (g_app.mainView == VIEW_SONGS) {
                FilterInto(g_app.allSongs, g_app.songKeys, g_app.querySongs, g_app.songData);
                FillSongs();
                SelectRow(g_app.songs, 0);
                InvalidateHeader();
            }
            SetStatus(Fmt(L"%u songs.", (unsigned)g_app.allSongs.size()));
        } else {
            SetStatus(L"Could not load the song list.");
            MessageBoxW(g_app.wnd, job->message.c_str(), APP_TITLE, MB_ICONERROR);
        }
        break;

    case JOB_ARTIST:
        if (job->ok) {
            g_app.shownArtist = job->artist;
            g_app.shownArtistCover = job->art ? job->artId : std::string();
            g_app.artistAlbums.swap(job->albums);
            g_app.albumListIsArtist = false;   // force a refill
            ShowArtistPage();
            SetStatus(Fmt(L"%ls: %u album%ls.", job->artist.name.c_str(), (unsigned)g_app.artistAlbums.size(),
                          g_app.artistAlbums.size() == 1 ? L"" : L"s"));
        } else {
            SetStatus(L"Could not load the artist.");
            MessageBoxW(g_app.wnd, job->message.c_str(), APP_TITLE, MB_ICONERROR);
        }
        break;

    case JOB_ART:
        if (g_app.nowCoverPending == job->artId)
            g_app.nowCoverPending.clear();
        break;
    }
    delete job;

    if (g_app.pendingLoad) {
        g_app.pendingLoad = false;
        if (g_app.pendingPlaylist >= 0)
            OpenPlaylist(g_app.pendingPlaylist);
        else
            OpenSelectedAlbum();
    } else if (g_app.pendingSongs && !g_app.allSongsLoaded) {
        g_app.pendingSongs = false;
        LoadAllSongs();
    }
}

static void OnPlayerState(WPARAM wp, LPARAM lp)
{
    wchar_t* text = (wchar_t*)lp;
    PlayerState state = (PlayerState)LOWORD(wp);
    if (HIWORD(wp) == g_app.player.Generation() && QueueValid()) {
        const std::wstring& title = g_app.queue[g_app.queueIndex].title;
        switch (state) {
        case PS_BUFFERING:
            SetStatus(L"Buffering: " + title);
            break;
        case PS_PLAYING:
            SetStatus(L"Playing: " + title + L"  -  " + g_app.queue[g_app.queueIndex].artist);
            break;
        case PS_FINISHED:
            if (g_app.queueIndex + 1 < (int)g_app.queue.size()) {
                StartQueue(g_app.queueIndex + 1, 0);
            } else {
                ResetSeekBar(0);
                RefreshTransport();
                SetStatus(L"Finished.");
            }
            break;
        case PS_ERROR:
            SetStatus(L"Playback failed.");
            RefreshTransport();
            MessageBoxW(g_app.wnd, text ? text : L"Playback failed.", APP_TITLE, MB_ICONERROR);
            break;
        }
    }
    free(text);
}

// --- Context menus -------------------------------------------------------------------------

// "Add to playlist" submenu: every playlist this user can edit, plus "New playlist...".
static HMENU BuildAddToMenu()
{
    HMENU m = CreatePopupMenu();
    int added = 0;
    for (size_t i = 0; i < g_app.playlistData.size(); ++i)
        if (g_app.client.CanEdit(g_app.playlistData[i])) {
            AppendMenuW(m, MF_STRING, CMD_ADD_BASE + (UINT)i, g_app.playlistData[i].name.c_str());
            ++added;
        }
    if (added)
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, CMD_ADD_NEW, L"&New playlist...");
    return m;
}

static POINT MenuPointAt(HWND ctl, LPARAM lp, const RECT* itemRect)
{
    POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
    if (lp == (LPARAM)-1) {   // keyboard (Shift+F10 / menu key): under the selected item
        RECT r;
        if (itemRect)
            r = *itemRect;
        else
            GetClientRect(ctl, &r);
        pt.x = r.left + 8;
        pt.y = r.bottom;
        ClientToScreen(ctl, &pt);
    }
    return pt;
}

static POINT ListMenuPoint(HWND lv, LPARAM lp, int item)
{
    RECT r;
    bool has = item >= 0 && ListView_GetItemRect(lv, item, &r, LVIR_LABEL);
    return MenuPointAt(lv, lp, has ? &r : NULL);
}

static void SongsContextMenu(LPARAM lp)
{
    int i = SelectedIndex(g_app.songs);
    if (i < 0 || i >= (int)g_app.songData.size())
        return;
    const SubsonicSong& s = g_app.songData[i];
    const bool editable = ShownPlaylistEditable();

    HMENU rate = CreatePopupMenu();
    static const wchar_t* kRate[] = { L"&5 stars", L"&4 stars", L"&3 stars", L"&2 stars", L"&1 star" };
    for (int r = 5; r >= 1; --r)
        AppendMenuW(rate, MF_STRING | (s.userRating == r ? MF_CHECKED : 0), CMD_RATE_BASE + r, kRate[5 - r]);
    AppendMenuW(rate, MF_SEPARATOR, 0, NULL);
    AppendMenuW(rate, MF_STRING | (s.userRating ? 0 : MF_GRAYED), CMD_RATE_BASE, L"&Clear rating");

    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, CMD_PLAY, L"&Play");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)rate, L"&Rate");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)BuildAddToMenu(), L"&Add to playlist");
    if (g_app.songSource == SRC_PLAYLIST) {
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING | (editable && i > 0 ? 0 : MF_GRAYED), CMD_MOVE_UP, L"Move &up");
        AppendMenuW(m, MF_STRING | (editable && i + 1 < (int)g_app.songData.size() ? 0 : MF_GRAYED),
                    CMD_MOVE_DOWN, L"Move &down");
        AppendMenuW(m, MF_STRING | (editable ? 0 : MF_GRAYED), CMD_REMOVE, L"Remo&ve from playlist\tDel");
    }

    POINT pt = ListMenuPoint(g_app.songs, lp, i);
    UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_app.wnd, NULL);
    DestroyMenu(m);   // destroys the submenus too

    std::vector<std::string> one(1, s.id);
    if (cmd == CMD_PLAY)
        PlayFromSongList(i);
    else if (cmd >= CMD_RATE_BASE && cmd <= CMD_RATE_BASE + 5)
        SetSongRating(i, cmd - CMD_RATE_BASE);
    else if (cmd == CMD_ADD_NEW)
        NewPlaylist(one, std::string(), L"New playlist");
    else if (cmd >= CMD_ADD_BASE)
        AddToPlaylist(cmd - CMD_ADD_BASE, one, std::string());
    else if (cmd == CMD_MOVE_UP)
        MoveInPlaylist(i, -1);
    else if (cmd == CMD_MOVE_DOWN)
        MoveInPlaylist(i, +1);
    else if (cmd == CMD_REMOVE)
        RemoveFromPlaylist(i);
}

static void AlbumsContextMenu(LPARAM lp)
{
    const std::vector<SubsonicAlbum>& list = ShownAlbums();
    int i = SelectedIndex(g_app.albums);
    if (i < 0 || i >= (int)list.size())
        return;
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, CMD_PLAY, L"&Open");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)BuildAddToMenu(), L"&Add album to playlist");
    POINT pt = ListMenuPoint(g_app.albums, lp, i);
    UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_app.wnd, NULL);
    DestroyMenu(m);

    const std::string albumId = list[i].id;
    const std::wstring albumName = list[i].name;
    if (cmd == CMD_PLAY)
        OpenSelectedAlbum();
    else if (cmd == CMD_ADD_NEW)
        NewPlaylist(std::vector<std::string>(), albumId, albumName);
    else if (cmd >= CMD_ADD_BASE)
        AddToPlaylist(cmd - CMD_ADD_BASE, std::vector<std::string>(), albumId);
}

static void TreeContextMenu(LPARAM lp)
{
    HTREEITEM item = TreeView_GetSelection(g_app.tree);
    if (lp != (LPARAM)-1) {
        // Right-click doesn't select in a TreeView; pick the item under the cursor.
        TVHITTESTINFO ht;
        ZeroMemory(&ht, sizeof(ht));
        ht.pt.x = (short)LOWORD(lp);
        ht.pt.y = (short)HIWORD(lp);
        ScreenToClient(g_app.tree, &ht.pt);
        HTREEITEM hit = TreeView_HitTest(g_app.tree, &ht);
        if (hit) {
            item = hit;
            TreeView_SelectItem(g_app.tree, item);   // opens it, like a left click
        }
    }
    const LPARAM data = TreeData(item);
    const int pl = (int)data;
    const bool isPlaylist = data >= 0 && pl < (int)g_app.playlistData.size();
    const UINT g = (isPlaylist && g_app.client.CanEdit(g_app.playlistData[pl])) ? 0 : MF_GRAYED;

    HMENU m = CreatePopupMenu();
    if (isPlaylist) {
        AppendMenuW(m, MF_STRING, CMD_PL_PLAY, L"&Play");
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    }
    AppendMenuW(m, MF_STRING, CMD_PL_NEW, L"&New playlist...");
    if (isPlaylist) {
        AppendMenuW(m, MF_STRING | g, CMD_PL_RENAME, L"&Rename...");
        AppendMenuW(m, MF_STRING | g, CMD_PL_DELETE, L"&Delete...");
    }
    RECT r;
    bool has = item && TreeView_GetItemRect(g_app.tree, item, &r, TRUE);
    POINT pt = MenuPointAt(g_app.tree, lp, has ? &r : NULL);
    UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_app.wnd, NULL);
    DestroyMenu(m);

    if (cmd == CMD_PL_PLAY) {
        if (g_app.songSource == SRC_PLAYLIST && g_app.shownPlaylist.id == g_app.playlistData[pl].id)
            PlayFromSongList(g_app.shuffle ? RandomBelow((int)g_app.songData.size()) : 0);
        else
            OpenPlaylist(pl);
    } else if (cmd == CMD_PL_NEW) {
        NewPlaylist(std::vector<std::string>(), std::string(), L"New playlist");
    } else if (cmd == CMD_PL_RENAME) {
        RenamePlaylist(pl);
    } else if (cmd == CMD_PL_DELETE) {
        DeletePlaylist(pl);
    }
}

// --- Drawn icons ------------------------------------------------------------------------

enum Glyph { GLYPH_NONE, GLYPH_PLAY, GLYPH_PAUSE, GLYPH_STOP, GLYPH_SHUFFLE, GLYPH_PREV, GLYPH_NEXT };

static void FillPoly(HDC dc, const POINT* pts, int n, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    HPEN p = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    Polygon(dc, pts, n);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
}

static void FillBox(HDC dc, int l, int t, int r, int b, COLORREF c)
{
    RECT rc = { l, t, r, b };
    HBRUSH br = CreateSolidBrush(c);
    FillRect(dc, &rc, br);
    DeleteObject(br);
}

// Glyph centred on (cx, cy) within an s x s box.
static void DrawGlyph(HDC dc, Glyph g, int cx, int cy, int s, COLORREF c)
{
    const int h = s / 2;
    switch (g) {
    case GLYPH_PLAY: {
        POINT pts[3] = { { cx - h + 2, cy - h }, { cx - h + 2, cy + h }, { cx + h, cy } };
        FillPoly(dc, pts, 3, c);
        break;
    }
    case GLYPH_PAUSE: {
        const int w = s / 3;
        FillBox(dc, cx - h + 1, cy - h, cx - h + 1 + w, cy + h + 1, c);
        FillBox(dc, cx + h - w, cy - h, cx + h, cy + h + 1, c);
        break;
    }
    case GLYPH_STOP:
        FillBox(dc, cx - h + 1, cy - h + 1, cx + h, cy + h, c);
        break;
    case GLYPH_PREV: {
        // |◀ : bar on the left, triangle pointing left.
        FillBox(dc, cx - h, cy - h, cx - h + 2, cy + h + 1, c);
        POINT pts[3] = { { cx + h, cy - h }, { cx + h, cy + h }, { cx - h + 2, cy } };
        FillPoly(dc, pts, 3, c);
        break;
    }
    case GLYPH_NEXT: {
        // ▶| : triangle pointing right, bar on the right.
        POINT pts[3] = { { cx - h, cy - h }, { cx - h, cy + h }, { cx + h - 2, cy } };
        FillPoly(dc, pts, 3, c);
        FillBox(dc, cx + h - 2, cy - h, cx + h, cy + h + 1, c);
        break;
    }
    case GLYPH_SHUFFLE: {
        // Two paths that cross, each ending in an arrowhead on the right (16 x 9 px).
        const int x0 = cx - 8, x1 = cx + 8, y0 = cy - 4, y1 = cy + 4;
        HPEN pen = CreatePen(PS_SOLID, 1, c);
        HGDIOBJ op = SelectObject(dc, pen);
        POINT a[4] = { { x0, y0 }, { x0 + 4, y0 }, { x1 - 7, y1 }, { x1 - 2, y1 } };
        POINT b[4] = { { x0, y1 }, { x0 + 4, y1 }, { x1 - 7, y0 }, { x1 - 2, y0 } };
        Polyline(dc, a, 4);
        Polyline(dc, b, 4);
        // Second pass one pixel down thickens the strokes without merging them.
        for (int i = 0; i < 4; ++i) { ++a[i].y; ++b[i].y; }
        Polyline(dc, a, 4);
        Polyline(dc, b, 4);
        SelectObject(dc, op);
        DeleteObject(pen);
        POINT top[3] = { { x1 + 1, y0 }, { x1 - 3, y0 - 3 }, { x1 - 3, y0 + 4 } };
        POINT bot[3] = { { x1 + 1, y1 + 1 }, { x1 - 3, y1 - 3 }, { x1 - 3, y1 + 4 } };
        FillPoly(dc, top, 3, c);
        FillPoly(dc, bot, 3, c);
        break;
    }
    case GLYPH_NONE:
        break;
    }
}

static void DrawSpeaker(HDC dc, const RECT& r)
{
    const COLORREF c = GetSysColor(COLOR_BTNTEXT);
    const int x = r.left, cy = (r.top + r.bottom) / 2;
    POINT body[6] = { { x, cy - 2 }, { x + 3, cy - 2 }, { x + 7, cy - 6 }, { x + 7, cy + 6 }, { x + 3, cy + 2 }, { x, cy + 2 } };
    FillPoly(dc, body, 6, c);
    HPEN pen = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ op = SelectObject(dc, pen);
    Arc(dc, x + 5, cy - 4, x + 13, cy + 5, x + 9, cy + 5, x + 9, cy - 4);
    Arc(dc, x + 5, cy - 8, x + 17, cy + 9, x + 11, cy + 9, x + 11, cy - 8);
    SelectObject(dc, op);
    DeleteObject(pen);
}

// Classic 3D push button with a drawn glyph (and optional caption). Shuffle and Pause
// stay "latched" (pressed, dithered face) while active, like a classic toggle button.
static void DrawIconButton(const DRAWITEMSTRUCT* d)
{
    Glyph glyph = GLYPH_NONE;
    const wchar_t* caption = NULL;
    bool latched = false;
    switch (d->CtlID) {
    case IDC_PLAY:        glyph = GLYPH_PLAY; break;
    case IDC_PAUSE:       glyph = GLYPH_PAUSE; latched = g_app.player.IsPaused(); break;
    case IDC_STOP:        glyph = GLYPH_STOP; break;
    case IDC_SHUFFLE:     glyph = GLYPH_SHUFFLE; latched = g_app.shuffle; break;
    case IDC_HDR_PLAY:    glyph = GLYPH_PLAY; caption = L"Play"; break;
    case IDC_HDR_SHUFFLE: glyph = GLYPH_SHUFFLE; caption = L"Shuffle"; break;
    case IDC_MINI_PREV:   glyph = GLYPH_PREV; break;
    case IDC_MINI_STOP:   glyph = GLYPH_STOP; break;
    case IDC_MINI_NEXT:   glyph = GLYPH_NEXT; break;
    case IDC_MINI_PLAYPAUSE:
        // One button: shows Pause while playing, Play when paused or stopped.
        glyph = (g_app.player.IsActive() && !g_app.player.IsPaused()) ? GLYPH_PAUSE : GLYPH_PLAY;
        break;
    }
    HDC dc = d->hDC;
    RECT r = d->rcItem;
    const bool pressed = (d->itemState & ODS_SELECTED) != 0;
    const bool disabled = (d->itemState & ODS_DISABLED) != 0;

    DrawFrameControl(dc, &r, DFC_BUTTON, DFCS_BUTTONPUSH | ((pressed || latched) ? DFCS_PUSHED : 0));
    if (latched && !pressed) {
        RECT in = r;
        InflateRect(&in, -2, -2);
        COLORREF ot = SetTextColor(dc, GetSysColor(COLOR_BTNFACE));
        COLORREF ob = SetBkColor(dc, GetSysColor(COLOR_BTNHIGHLIGHT));
        FillRect(dc, &in, g_app.latchBrush);
        SetTextColor(dc, ot);
        SetBkColor(dc, ob);
    }

    const int off = (pressed || latched) ? 1 : 0;
    const COLORREF c = GetSysColor(disabled ? COLOR_GRAYTEXT : COLOR_BTNTEXT);
    const int gs = 10;
    const int gw = (glyph == GLYPH_SHUFFLE) ? 18 : gs;   // drawn width of the glyph
    int cx = (r.left + r.right) / 2 + off, cy = (r.top + r.bottom) / 2 + off;
    if (caption) {
        HGDIOBJ of = SelectObject(dc, g_app.font);
        SIZE ts;
        GetTextExtentPoint32W(dc, caption, (int)wcslen(caption), &ts);
        const int total = gw + 6 + ts.cx;
        const int left = (r.left + r.right - total) / 2 + off;
        DrawGlyph(dc, glyph, left + gw / 2, cy, gs, c);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, c);
        TextOutW(dc, left + gw + 6, cy - ts.cy / 2, caption, (int)wcslen(caption));
        SelectObject(dc, of);
    } else {
        DrawGlyph(dc, glyph, cx, cy, gs, c);
    }
    if (d->itemState & ODS_FOCUS) {
        RECT f = r;
        InflateRect(&f, -4, -4);
        DrawFocusRect(dc, &f);
    }
}

// --- Painting ---------------------------------------------------------------------------

static void DrawCover(HDC dc, const RECT& r, HBITMAP bmp, const wchar_t* emptyText)
{
    if (bmp) {
        BITMAP bm;
        GetObjectW(bmp, sizeof(bm), &bm);
        HDC mem = CreateCompatibleDC(dc);
        HGDIOBJ old = SelectObject(mem, bmp);
        int x = r.left + (r.right - r.left - bm.bmWidth) / 2;
        int y = r.top + (r.bottom - r.top - bm.bmHeight) / 2;
        BitBlt(dc, x, y, bm.bmWidth, bm.bmHeight, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteDC(mem);
    } else {
        RECT e = r;
        DrawEdge(dc, &e, EDGE_SUNKEN, BF_RECT);
        if (emptyText) {
            SelectObject(dc, g_app.font);
            SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
            DrawTextW(dc, emptyText, -1, &e, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }
}

static void PaintHeader(HDC dc)
{
    RECT h = g_app.headerRect;
    SetBkMode(dc, TRANSPARENT);
    const COLORREF text = GetSysColor(COLOR_BTNTEXT);

    int textX = h.left + 4;
    std::wstring title, line2, line3;
    const bool searching = CurrentQuery() && !SearchTerms(*CurrentQuery()).empty();
    if (g_app.mainView == VIEW_ALBUMS) {
        title = L"Albums";
        if (g_app.albumData.empty())
            line2 = L"Not connected";
        else if (searching)
            line2 = Fmt(L"%u of %u albums", (unsigned)g_app.albumList.size(), (unsigned)g_app.albumData.size());
        else
            line2 = Fmt(L"%u albums", (unsigned)g_app.albumData.size());
    } else if (g_app.mainView == VIEW_ARTISTS) {
        title = L"Artists";
        if (g_app.artistData.empty())
            line2 = L"Not connected";
        else if (searching)
            line2 = Fmt(L"%u of %u artists", (unsigned)g_app.artistList.size(), (unsigned)g_app.artistData.size());
        else
            line2 = Fmt(L"%u artists", (unsigned)g_app.artistData.size());
    } else if (g_app.mainView == VIEW_SONGS) {
        title = L"Songs";
        if (!g_app.allSongsLoaded)
            line2 = L"Loading...";
        else if (searching)
            line2 = Fmt(L"%u of %u songs", (unsigned)g_app.songData.size(), (unsigned)g_app.allSongs.size());
        else
            line2 = Fmt(L"%u songs  \x00B7  %ls", (unsigned)g_app.allSongs.size(),
                        FormatTime(TotalDuration(g_app.allSongs)).c_str());
    } else if (g_app.shown.has) {
        DrawCover(dc, g_app.artRect, g_app.shownArt, L"No cover");
        textX = g_app.artRect.right + 14;
        title = g_app.shown.title;
        line2 = g_app.shown.line2;
        line3 = g_app.shown.line3;
    }

    // Section headers (Albums/Artists/Songs) are compact: title at the top, count right under it.
    const bool compact = !g_app.shown.has || g_app.mainView == VIEW_ALBUMS || g_app.mainView == VIEW_ARTISTS ||
                         g_app.mainView == VIEW_SONGS;
    RECT t = { textX, h.top + (compact ? 0 : 6), h.right, h.top + (compact ? 32 : 40) };
    SetTextColor(dc, text);
    SelectObject(dc, g_app.fontTitle);
    DrawTextW(dc, title.c_str(), -1, &t, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, g_app.font);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    t.top = h.top + (compact ? 31 : 42);
    t.bottom = t.top + tm.tmHeight + 2;
    DrawTextW(dc, line2.c_str(), -1, &t, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
    t.top = t.bottom + 2;
    t.bottom = t.top + tm.tmHeight + 2;
    DrawTextW(dc, line3.c_str(), -1, &t, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

static void PaintNowPlaying(HDC dc)
{
    RECT r = g_app.npRect;
    RECT art = { r.left + (r.right - r.left - kArtSize) / 2, r.top, 0, r.top + kArtSize };
    art.right = art.left + kArtSize;
    DrawCover(dc, art, g_app.nowArt, QueueValid() ? L"No cover" : L"Nothing playing");
    if (!QueueValid())
        return;
    const SubsonicSong& s = g_app.queue[g_app.queueIndex];
    SetBkMode(dc, TRANSPARENT);
    TEXTMETRICW tm;
    SelectObject(dc, g_app.fontBold);
    GetTextMetricsW(dc, &tm);
    RECT t = { r.left, art.bottom + 4, r.right, art.bottom + 4 + tm.tmHeight };
    SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
    DrawTextW(dc, s.title.c_str(), -1, &t, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, g_app.font);
    t.top = t.bottom + 1;
    t.bottom = t.top + tm.tmHeight;
    SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
    DrawTextW(dc, s.artist.c_str(), -1, &t, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

static void OnPaint(HDC dc)
{
    PaintHeader(dc);
    PaintNowPlaying(dc);
    DrawSpeaker(dc, g_app.speakerRect);
    // Etched separator above the transport bar.
    RECT sep = g_app.transportRect;
    sep.bottom = sep.top + 2;
    DrawEdge(dc, &sep, EDGE_ETCHED, BF_TOP);
}

// --- Window ----------------------------------------------------------------------------

// Column sizing: fixed columns keep their width; the rest share what's left by weight.
struct ColSpec {
    int fixed;      // > 0: always this wide
    int weight;     // share of the remaining width for flexible columns
    int minWidth;
};

static const ColSpec kAlbumCols[]  = { { 0, 5, 120 }, { 0, 4, 100 }, { 50, 0, 0 }, { 55, 0, 0 } };
static const ColSpec kArtistCols[] = { { 0, 1, 150 }, { 60, 0, 0 } };
static const ColSpec kSongCols[]   = { { 32, 0, 0 }, { 0, 5, 120 }, { 0, 3, 90 }, { 80, 0, 0 }, { 48, 0, 0 },
                                       { 0, 4, 90 } };

static void FitColumns(HWND lv, const ColSpec* cols, int n)
{
    // From the window width rather than the client area, always reserving the vertical
    // scrollbar, so columns don't jump when it appears and no horizontal one is needed.
    RECT wr;
    GetWindowRect(lv, &wr);
    int avail = (wr.right - wr.left) - 2 * GetSystemMetrics(SM_CXEDGE) - GetSystemMetrics(SM_CXVSCROLL) - 1;
    int fixed = 0, weights = 0;
    for (int i = 0; i < n; ++i) {
        fixed += cols[i].fixed;
        weights += cols[i].weight;
    }
    int flexible = avail - fixed;
    int used = 0, lastFlex = -1;
    for (int i = 0; i < n; ++i) {
        if (cols[i].fixed > 0)
            continue;
        int w = weights ? flexible * cols[i].weight / weights : 0;
        if (w < cols[i].minWidth)
            w = cols[i].minWidth;
        ListView_SetColumnWidth(lv, i, w);
        used += w;
        lastFlex = i;
    }
    // Rounding leftovers go to the last flexible column.
    if (lastFlex >= 0 && flexible > used) {
        int w = (int)SendMessageW(lv, LVM_GETCOLUMNWIDTH, lastFlex, 0);
        ListView_SetColumnWidth(lv, lastFlex, w + (flexible - used));
    }
    for (int i = 0; i < n; ++i)
        if (cols[i].fixed > 0)
            ListView_SetColumnWidth(lv, i, cols[i].fixed);
}

static void Layout(HWND hwnd)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    SendMessageW(g_app.status, WM_SIZE, 0, 0);
    RECT sr = { 0, 0, 0, 0 };
    GetWindowRect(g_app.status, &sr);
    const int statusH = sr.bottom - sr.top;

    // Transport bar along the bottom.
    const int tH = 38, bw = 34, bh = 28;
    const int ty = rc.bottom - statusH - tH;
    SetRect(&g_app.transportRect, 0, ty, rc.right, ty + tH);
    const int by = ty + (tH - bh) / 2 + 1;
    int x = kPad;
    MoveWindow(g_app.btnPlay, x, by, bw, bh, TRUE);
    x += bw + 2;
    MoveWindow(g_app.btnPause, x, by, bw, bh, TRUE);
    x += bw + 2;
    MoveWindow(g_app.btnStop, x, by, bw, bh, TRUE);
    x += bw + 10;
    MoveWindow(g_app.btnShuffle, x, by, bw, bh, TRUE);
    x += bw + 12;

    const int volW = 100, timeW = 48;
    const int volX = rc.right - kPad - volW;
    SetRect(&g_app.speakerRect, volX - 22, by, volX - 4, by + bh);
    MoveWindow(g_app.volume, volX, by + 2, volW, bh - 4, TRUE);
    const int seekRight = g_app.speakerRect.left - 12;
    MoveWindow(g_app.timeCur, x, by + 8, timeW, bh - 8, TRUE);
    MoveWindow(g_app.seek, x + timeW, by + 2, seekRight - timeW - (x + timeW), bh - 4, TRUE);
    MoveWindow(g_app.timeTotal, seekRight - timeW, by + 8, timeW, bh - 8, TRUE);

    // Sidebar: tree, "+ New Playlist", now-playing cover.
    const int contentBottom = ty - kPad;
    TEXTMETRICW tm;
    HDC dc = GetDC(hwnd);
    HGDIOBJ of = SelectObject(dc, g_app.font);
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, of);
    ReleaseDC(hwnd, dc);
    const int npH = kArtSize + 8 + 2 * (tm.tmHeight + 1);
    SetRect(&g_app.npRect, kPad, contentBottom - npH, kPad + kSidebarW, contentBottom);
    const int newH = 24;
    const int newY = g_app.npRect.top - kPad - newH;
    MoveWindow(g_app.btnNewPlaylist, kPad, newY, kSidebarW, newH, TRUE);
    int treeH = newY - 4 - kPad;
    MoveWindow(g_app.tree, kPad, kPad, kSidebarW, treeH > 0 ? treeH : 0, TRUE);

    // Main area: header, then the album list or track table.
    const int mx = kPad * 2 + kSidebarW, mw = rc.right - mx - kPad;
    const bool bigHeader = g_app.mainView == VIEW_TRACKS || g_app.mainView == VIEW_ARTIST;
    const int headerH = bigHeader ? kArtSize : 48;
    SetRect(&g_app.headerRect, mx, kPad, mx + mw, kPad + headerH);
    SetRect(&g_app.artRect, mx, kPad, mx + kArtSize, kPad + kArtSize);
    // Play/Shuffle: under the details next to the cover, or at the right of the short
    // "Songs" header.
    int hbY = kPad + kArtSize - 26, hbX = g_app.artRect.right + 14;
    if (!bigHeader) {
        hbY = kPad + 6;
        hbX = g_app.headerRect.right - 186;
    }
    MoveWindow(g_app.hdrPlay, hbX, hbY, 84, 26, TRUE);
    MoveWindow(g_app.hdrShuffle, hbX + 90, hbY, 96, 26, TRUE);
    // Search box at the right of the compact header (left of Play/Shuffle on Songs).
    const int searchW = 200, labelW = 44;
    const int searchRight = (g_app.mainView == VIEW_SONGS) ? hbX - 12 : g_app.headerRect.right;
    MoveWindow(g_app.search, searchRight - searchW, kPad + 8, searchW, 22, TRUE);
    MoveWindow(g_app.searchLabel, searchRight - searchW - labelW - 4, kPad + 12, labelW, 16, TRUE);
    const int listTop = g_app.headerRect.bottom + kPad;
    const int listH = contentBottom - listTop;
    MoveWindow(g_app.albums, mx, listTop, mw, listH > 0 ? listH : 0, TRUE);
    MoveWindow(g_app.artists, mx, listTop, mw, listH > 0 ? listH : 0, TRUE);
    MoveWindow(g_app.songs, mx, listTop, mw, listH > 0 ? listH : 0, TRUE);
    FitColumns(g_app.albums, kAlbumCols, sizeof(kAlbumCols) / sizeof(kAlbumCols[0]));
    FitColumns(g_app.artists, kArtistCols, sizeof(kArtistCols) / sizeof(kArtistCols[0]));
    FitColumns(g_app.songs, kSongCols, sizeof(kSongCols) / sizeof(kSongCols[0]));
    InvalidateRect(hwnd, NULL, TRUE);
}

static HWND MakeChild(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD exStyle = 0)
{
    HWND c = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, parent,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_app.font, FALSE);
    return c;
}

static HWND MakeList(HWND parent, int id, DWORD extraStyle = 0)
{
    HWND lv = MakeChild(parent, WC_LISTVIEWW, L"",
                        WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | extraStyle, id, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT);
    return lv;
}

static void AddTooltip(HWND ctl, const wchar_t* text)
{
    TOOLINFOW ti;
    ZeroMemory(&ti, sizeof(ti));
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = g_app.wnd;
    ti.uId = (UINT_PTR)ctl;
    ti.lpszText = (LPWSTR)text;
    SendMessageW(g_app.tooltip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

static HMENU BuildMenuBar()
{
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, IDM_CONNECT, L"&Connect\tF5");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, IDM_EXIT, L"E&xit");

    HMENU view = CreatePopupMenu();
    AppendMenuW(view, MF_STRING, IDM_ALBUMS, L"&Albums");
    AppendMenuW(view, MF_STRING, IDM_ARTISTS, L"A&rtists");
    AppendMenuW(view, MF_STRING, IDM_SONGS, L"&Songs");
    AppendMenuW(view, MF_SEPARATOR, 0, NULL);
    AppendMenuW(view, MF_STRING, IDM_FIND, L"&Find\tCtrl+F");
    AppendMenuW(view, MF_STRING, IDM_MINI, L"&Mini Player\tCtrl+M");

    HMENU playback = CreatePopupMenu();
    AppendMenuW(playback, MF_STRING, IDM_PLAY, L"&Play");
    AppendMenuW(playback, MF_STRING, IDM_PAUSE, L"P&ause / Resume");
    AppendMenuW(playback, MF_STRING, IDM_STOP, L"&Stop");
    AppendMenuW(playback, MF_STRING, IDM_PREV, L"Pre&vious");
    AppendMenuW(playback, MF_STRING, IDM_NEXT, L"&Next");
    AppendMenuW(playback, MF_SEPARATOR, 0, NULL);
    AppendMenuW(playback, MF_STRING, IDM_SHUFFLE, L"S&huffle");

    HMENU playlist = CreatePopupMenu();
    AppendMenuW(playlist, MF_STRING, IDM_NEW_PLAYLIST, L"&New Playlist...");

    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, IDM_ABOUT, L"&About Navi2K Player");

    HMENU bar = CreateMenu();
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)file, L"&File");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)view, L"&View");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)playback, L"&Playback");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)playlist, L"P&laylist");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)help, L"&Help");
    return bar;
}

static void OnCreate(HWND hwnd)
{
    g_app.wnd = hwnd;
    g_app.font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    LOGFONTW lf;
    GetObjectW(g_app.font, sizeof(lf), &lf);
    lf.lfWeight = FW_BOLD;
    g_app.fontBold = CreateFontIndirectW(&lf);
    // Large header title. Tahoma ships with Win2000 and scales; MS Sans Serif is bitmap-only.
    lf.lfHeight = -24;
    lstrcpyW(lf.lfFaceName, L"Tahoma");
    g_app.fontTitle = CreateFontIndirectW(&lf);

    // 50% dither for latched toggle buttons, as classic Windows draws them.
    WORD dither[8] = { 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA };
    HBITMAP pattern = CreateBitmap(8, 8, 1, 1, dither);
    g_app.latchBrush = CreatePatternBrush(pattern);
    DeleteObject(pattern);

    g_app.menu = BuildMenuBar();
    SetMenu(hwnd, g_app.menu);

    // Sidebar.
    g_app.tree = MakeChild(hwnd, WC_TREEVIEWW, L"",
                           WS_TABSTOP | TVS_SHOWSELALWAYS | TVS_FULLROWSELECT | TVS_DISABLEDRAGDROP,
                           IDC_TREE, WS_EX_CLIENTEDGE);
    TreeView_SetIndent(g_app.tree, 12);
    TreeView_SetItemHeight(g_app.tree, 18);
    g_app.treeUpdating = true;
    g_app.treeLibrary = TreeAdd(TVI_ROOT, L"LIBRARY", TREE_LIBRARY, true);
    g_app.treeAlbums = TreeAdd(g_app.treeLibrary, L"Albums", TREE_ALBUMS, false);
    g_app.treeArtists = TreeAdd(g_app.treeLibrary, L"Artists", TREE_ARTISTS, false);
    g_app.treeSongs = TreeAdd(g_app.treeLibrary, L"Songs", TREE_SONGS, false);
    g_app.treePlaylists = TreeAdd(TVI_ROOT, L"PLAYLISTS", TREE_PLAYLISTS, true);
    TreeView_Expand(g_app.tree, g_app.treeLibrary, TVE_EXPAND);
    g_app.treeUpdating = false;
    g_app.btnNewPlaylist = MakeChild(hwnd, L"BUTTON", L"+ &New Playlist", WS_TABSTOP | BS_PUSHBUTTON, IDC_NEW_PLAYLIST);

    // Main area.
    g_app.searchLabel = MakeChild(hwnd, L"STATIC", L"Search:", SS_RIGHT, IDC_SEARCH_LABEL);
    g_app.search = MakeChild(hwnd, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, IDC_SEARCH, WS_EX_CLIENTEDGE);
    SendMessageW(g_app.search, EM_LIMITTEXT, 200, 0);
    g_searchEditProc = (WNDPROC)SetWindowLongPtrW(g_app.search, GWLP_WNDPROC, (LONG_PTR)SearchEditProc);
    g_app.hdrPlay = MakeChild(hwnd, L"BUTTON", L"Play", WS_TABSTOP | BS_OWNERDRAW, IDC_HDR_PLAY);
    g_app.hdrShuffle = MakeChild(hwnd, L"BUTTON", L"Shuffle", WS_TABSTOP | BS_OWNERDRAW, IDC_HDR_SHUFFLE);

    g_app.albums = MakeList(hwnd, IDC_ALBUMS);
    AddColumn(g_app.albums, 0, L"Album", 260, LVCFMT_LEFT);
    AddColumn(g_app.albums, 1, L"Artist", 200, LVCFMT_LEFT);
    AddColumn(g_app.albums, 2, L"Year", 50, LVCFMT_RIGHT);
    AddColumn(g_app.albums, 3, L"Tracks", 55, LVCFMT_RIGHT);

    g_app.artists = MakeList(hwnd, IDC_ARTISTS);
    AddColumn(g_app.artists, 0, L"Artist", 320, LVCFMT_LEFT);
    AddColumn(g_app.artists, 1, L"Albums", 60, LVCFMT_RIGHT);

    g_app.songs = MakeList(hwnd, IDC_SONGS, LVS_OWNERDATA);
    AddColumn(g_app.songs, COL_NUM, L"#", 32, LVCFMT_RIGHT);
    AddColumn(g_app.songs, COL_TITLE, L"Track", 220, LVCFMT_LEFT);
    AddColumn(g_app.songs, COL_ARTIST, L"Artist", 140, LVCFMT_LEFT);
    AddColumn(g_app.songs, COL_RATING, L"Rating", 80, LVCFMT_LEFT);
    AddColumn(g_app.songs, COL_LENGTH, L"Time", 48, LVCFMT_RIGHT);
    AddColumn(g_app.songs, COL_ALBUM, L"Album", 170, LVCFMT_LEFT);

    // Transport bar.
    g_app.btnPlay = MakeChild(hwnd, L"BUTTON", L"Play", WS_TABSTOP | BS_OWNERDRAW, IDC_PLAY);
    g_app.btnPause = MakeChild(hwnd, L"BUTTON", L"Pause", WS_TABSTOP | BS_OWNERDRAW, IDC_PAUSE);
    g_app.btnStop = MakeChild(hwnd, L"BUTTON", L"Stop", WS_TABSTOP | BS_OWNERDRAW, IDC_STOP);
    g_app.btnShuffle = MakeChild(hwnd, L"BUTTON", L"Shuffle", WS_TABSTOP | BS_OWNERDRAW, IDC_SHUFFLE);
    g_app.timeCur = MakeChild(hwnd, L"STATIC", L"0:00", SS_RIGHT, IDC_TIME_CUR);
    g_app.seek = MakeChild(hwnd, TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_HORZ | TBS_NOTICKS | TBS_BOTH, IDC_SEEK);
    SendMessageW(g_app.seek, TBM_SETPAGESIZE, 0, 10);
    SendMessageW(g_app.seek, TBM_SETLINESIZE, 0, 5);
    g_app.timeTotal = MakeChild(hwnd, L"STATIC", L"0:00", SS_LEFT, IDC_TIME_TOTAL);
    g_app.volume = MakeChild(hwnd, TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_HORZ | TBS_NOTICKS | TBS_BOTH, IDC_VOLUME);
    SendMessageW(g_app.volume, TBM_SETRANGE, FALSE, MAKELPARAM(0, 100));
    SendMessageW(g_app.volume, TBM_SETPAGESIZE, 0, 10);
    g_app.player.SetVolume((int)GetPrivateProfileIntW(L"Player", L"Volume", 100, g_app.iniPath.c_str()));
    SendMessageW(g_app.volume, TBM_SETPOS, TRUE, g_app.player.Volume());
    g_app.shuffle = GetPrivateProfileIntW(L"Player", L"Shuffle", 0, g_app.iniPath.c_str()) != 0;

    g_app.status = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                                   0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_STATUS, GetModuleHandleW(NULL), NULL);

    g_app.tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_ALWAYSTIP,
                                    CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                    hwnd, NULL, GetModuleHandleW(NULL), NULL);
    AddTooltip(g_app.btnPlay, L"Play");
    AddTooltip(g_app.btnPause, L"Pause / Resume");
    AddTooltip(g_app.btnStop, L"Stop");
    AddTooltip(g_app.btnShuffle, L"Shuffle");
    AddTooltip(g_app.volume, L"Volume");

    SetView(VIEW_ALBUMS);
    RefreshTransport();
    SetStatus(g_app.cpuSummary);
    g_app.player.SetNotifyWindow(hwnd);
    SetTimer(hwnd, TIMER_POSITION, 250, NULL);
}

// Seek and volume sliders of both the main window and the mini player.
static void OnHScroll(HWND bar, int code)
{
    int pos = (int)SendMessageW(bar, TBM_GETPOS, 0, 0);
    if (bar == g_app.volume || bar == g_app.miniVolume) {
        g_app.player.SetVolume(pos);
        HWND other = (bar == g_app.volume) ? g_app.miniVolume : g_app.volume;
        if (other)
            SendMessageW(other, TBM_SETPOS, TRUE, pos);
        if (code == TB_ENDTRACK)
            WritePrivateProfileStringW(L"Player", L"Volume", Fmt(L"%d", pos).c_str(), g_app.iniPath.c_str());
    } else if (bar == g_app.seek || bar == g_app.miniSeek) {
        if (code == TB_ENDTRACK) {
            // End of a drag, click on the channel, or arrow key: one seek per gesture.
            g_app.seekDragging = false;
            SeekTo(pos);
        } else {
            g_app.seekDragging = true;
            SetTimeCur(pos);
        }
    }
}

static LRESULT OnNotify(NMHDR* nm)
{
    switch (nm->idFrom) {
    case IDC_SONGS:
        if (nm->code == NM_CUSTOMDRAW)
            return OnSongsCustomDraw((NMLVCUSTOMDRAW*)nm);
        if (nm->code == LVN_GETDISPINFOW) {
            OnSongsDispInfo((NMLVDISPINFOW*)nm);
            return 0;
        }
        if (nm->code == NM_CLICK)
            OnSongsClick((NMITEMACTIVATE*)nm);
        else if (nm->code == NM_DBLCLK || nm->code == NM_RETURN)
            PlayFromSongList(SelectedIndex(g_app.songs));
        else if (nm->code == NM_SETFOCUS || nm->code == NM_KILLFOCUS)
            InvalidateRect(g_app.songs, NULL, FALSE);   // star outline colour follows focus
        else if (nm->code == LVN_KEYDOWN) {
            WORD vk = ((NMLVKEYDOWN*)nm)->wVKey;
            int i = SelectedIndex(g_app.songs);
            if (vk >= '0' && vk <= '5')
                SetSongRating(i, vk - '0');
            else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD5)
                SetSongRating(i, vk - VK_NUMPAD0);
            else if (vk == VK_DELETE)
                RemoveFromPlaylist(i);
            else if (vk == VK_BACK)
                GoBack();
        }
        break;

    case IDC_ALBUMS:
        if (nm->code == NM_DBLCLK || nm->code == NM_RETURN)
            OpenSelectedAlbum();
        else if (nm->code == LVN_KEYDOWN && ((NMLVKEYDOWN*)nm)->wVKey == VK_BACK)
            GoBack();
        break;

    case IDC_ARTISTS:
        if (nm->code == NM_DBLCLK || nm->code == NM_RETURN)
            OpenSelectedArtist();
        break;

    case IDC_TREE:
        if (nm->code == TVN_SELCHANGEDW && !g_app.treeUpdating) {
            NMTREEVIEWW* tv = (NMTREEVIEWW*)nm;
            LPARAM data = tv->itemNew.lParam;
            if (data == TREE_ALBUMS)
                ShowAlbumsView();
            else if (data == TREE_ARTISTS)
                ShowArtistsView();
            else if (data == TREE_SONGS)
                ShowSongsView();
            else if (data >= 0)
                OpenPlaylist((int)data);
        } else if (nm->code == TVN_ITEMEXPANDINGW) {
            return ((NMTREEVIEWW*)nm)->action == TVE_COLLAPSE;   // sections stay open
        }
        break;
    }
    return 0;
}

static void OnCommand(int id)
{
    switch (id) {
    case IDM_CONNECT:
        DoConnect();
        break;
    case IDM_EXIT:
        DestroyWindow(g_app.wnd);
        break;
    case IDC_PLAY:
    case IDM_PLAY:
        DoPlay();
        break;
    case IDC_PAUSE:
    case IDM_PAUSE:
        DoPause();
        break;
    case IDC_STOP:
    case IDM_STOP:
        StopPlayback();
        break;
    case IDC_SHUFFLE:
    case IDM_SHUFFLE:
        ToggleShuffle();
        break;
    case IDC_HDR_PLAY:
        PlayFromSongList(g_app.shuffle ? RandomBelow((int)g_app.songData.size()) : 0);
        break;
    case IDC_HDR_SHUFFLE:
        if (!g_app.shuffle)
            ToggleShuffle();
        PlayFromSongList(RandomBelow((int)g_app.songData.size()));
        break;
    case IDM_ALBUMS:
        ShowAlbumsView();
        break;
    case IDM_ARTISTS:
        ShowArtistsView();
        break;
    case IDM_SONGS:
        ShowSongsView();
        break;
    case IDM_FIND:
        FocusSearch();
        break;
    case IDM_MINI:
        ToggleMini();
        break;
    case IDM_PREV:
        PlayPrevious();
        break;
    case IDM_NEXT:
        PlayNext();
        break;
    case IDC_NEW_PLAYLIST:
    case IDM_NEW_PLAYLIST:
        NewPlaylist(std::vector<std::string>(), std::string(), L"New playlist");
        break;
    case IDM_ABOUT: {
        std::wstring about = L"Navi2K Player " APP_VERSION L"\r\nNavidrome client for Windows 2000 / XP\r\n\r\n";
        about += L"Server: " + (g_app.serverUrl.empty() ? std::wstring(L"(not connected)") : g_app.serverUrl);
        about += L"\r\nCPU: " + g_app.cpuSummary;
        MessageBoxW(g_app.wnd, about.c_str(), L"About " APP_TITLE, MB_ICONINFORMATION);
        break;
    }
    }
}

// --- Mini player ------------------------------------------------------------------------
// A small top-level window: cover, title, artist, seek bar, prev / play-pause / stop / next
// and volume. Ctrl+M (View > Mini Player) swaps it with the main window; closing it, or
// double-clicking the cover, swaps back. Its controls mirror the main window's.

static const int kMiniArt = 112, kMiniW = 420, kMiniH = 128, kMiniPad = 8;

static RECT MiniArtRect()
{
    RECT r = { kMiniPad, kMiniPad, kMiniPad + kMiniArt, kMiniPad + kMiniArt };
    return r;
}

static void PaintMini(HDC dc)
{
    RECT art = MiniArtRect();
    if (g_app.nowArt) {
        // The cached cover is up to 160 px; scale it down keeping its shape. HALFTONE gives a
        // smooth result on Windows 2000/XP.
        BITMAP bm;
        GetObjectW(g_app.nowArt, sizeof(bm), &bm);
        int w = kMiniArt, h = kMiniArt;
        if (bm.bmWidth > bm.bmHeight)
            h = bm.bmHeight * kMiniArt / bm.bmWidth;
        else if (bm.bmHeight > bm.bmWidth)
            w = bm.bmWidth * kMiniArt / bm.bmHeight;
        HDC mem = CreateCompatibleDC(dc);
        HGDIOBJ old = SelectObject(mem, g_app.nowArt);
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, NULL);
        StretchBlt(dc, art.left + (kMiniArt - w) / 2, art.top + (kMiniArt - h) / 2, w, h,
                   mem, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
        SelectObject(mem, old);
        DeleteDC(mem);
    } else {
        DrawCover(dc, art, NULL, QueueValid() ? L"No cover" : L"Nothing playing");
    }

    if (QueueValid()) {
        const SubsonicSong& s = g_app.queue[g_app.queueIndex];
        RECT client;
        GetClientRect(g_app.mini, &client);
        SetBkMode(dc, TRANSPARENT);
        TEXTMETRICW tm;
        SelectObject(dc, g_app.fontBold);
        GetTextMetricsW(dc, &tm);
        RECT t = { art.right + 12, kMiniPad + 2, client.right - kMiniPad, kMiniPad + 2 + tm.tmHeight };
        SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
        DrawTextW(dc, s.title.c_str(), -1, &t, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, g_app.font);
        t.top = t.bottom + 3;
        t.bottom = t.top + tm.tmHeight;
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        DrawTextW(dc, s.artist.c_str(), -1, &t, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    DrawSpeaker(dc, g_app.miniSpeakerRect);
}

static LRESULT CALLBACK MiniWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        PaintMini(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DRAWITEM:
        DrawIconButton((const DRAWITEMSTRUCT*)lp);
        return TRUE;

    case WM_HSCROLL:
        OnHScroll((HWND)lp, LOWORD(wp));
        return 0;

    case WM_LBUTTONDBLCLK: {
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        RECT art = MiniArtRect();
        if (PtInRect(&art, pt))
            ToggleMini();
        return 0;
    }

    case WM_COMMAND: {
        const int id = LOWORD(wp), code = HIWORD(wp);
        if (code != BN_CLICKED && code != BN_DOUBLECLICKED && code != 1 /* accelerator */)
            return 0;
        switch (id) {
        case IDC_MINI_PREV:
            PlayPrevious();
            break;
        case IDC_MINI_NEXT:
            PlayNext();
            break;
        case IDC_MINI_STOP:
            StopPlayback();
            break;
        case IDC_MINI_PLAYPAUSE:
            if (g_app.player.IsActive())
                DoPause();
            else
                DoPlay();
            break;
        case IDM_MINI:
            ToggleMini();
            break;
        default:
            // Other shortcuts (F5, Ctrl+F) belong to the main window.
            ToggleMini();
            OnCommand(id);
            break;
        }
        RefreshTransport();
        return 0;
    }

    case WM_CLOSE:
        ToggleMini();   // closing the mini player goes back to the full window
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void CreateMini(HINSTANCE inst, HICON icon, HICON iconSmall)
{
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = MiniWndProc;
    wc.hInstance = inst;
    wc.hIcon = icon;
    wc.hIconSm = iconSmall;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"Navi2KPlayerMini";
    if (!RegisterClassExW(&wc))
        return;

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT r = { 0, 0, kMiniW, kMiniH };
    AdjustWindowRect(&r, style, FALSE);
    HWND w = CreateWindowExW(0, wc.lpszClassName, APP_TITLE, style, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    if (!w)
        return;
    g_app.mini = w;

    // Right of the cover: title/artist (painted), then the seek row, then the button row.
    const int x = kMiniPad + kMiniArt + 12, right = kMiniW - kMiniPad, timeW = 40;
    g_app.miniTimeCur = MakeChild(w, L"STATIC", L"0:00", SS_LEFT, IDC_MINI_TIME_CUR);
    g_app.miniSeek = MakeChild(w, TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_HORZ | TBS_NOTICKS | TBS_BOTH, IDC_MINI_SEEK);
    g_app.miniTimeTotal = MakeChild(w, L"STATIC", L"0:00", SS_RIGHT, IDC_MINI_TIME_TOTAL);
    MoveWindow(g_app.miniTimeCur, x, 52, timeW, 16, FALSE);
    MoveWindow(g_app.miniSeek, x + timeW, 47, right - timeW - (x + timeW), 24, FALSE);
    MoveWindow(g_app.miniTimeTotal, right - timeW, 52, timeW, 16, FALSE);
    SendMessageW(g_app.miniSeek, TBM_SETPAGESIZE, 0, 10);
    SendMessageW(g_app.miniSeek, TBM_SETLINESIZE, 0, 5);

    const int by = 86, bw = 34, bh = 28;
    g_app.miniPrev = MakeChild(w, L"BUTTON", L"Previous", WS_TABSTOP | BS_OWNERDRAW, IDC_MINI_PREV);
    g_app.miniPlayPause = MakeChild(w, L"BUTTON", L"Play/Pause", WS_TABSTOP | BS_OWNERDRAW, IDC_MINI_PLAYPAUSE);
    g_app.miniStop = MakeChild(w, L"BUTTON", L"Stop", WS_TABSTOP | BS_OWNERDRAW, IDC_MINI_STOP);
    g_app.miniNext = MakeChild(w, L"BUTTON", L"Next", WS_TABSTOP | BS_OWNERDRAW, IDC_MINI_NEXT);
    MoveWindow(g_app.miniPrev, x, by, bw, bh, FALSE);
    MoveWindow(g_app.miniPlayPause, x + (bw + 2), by, bw, bh, FALSE);
    MoveWindow(g_app.miniStop, x + 2 * (bw + 2), by, bw, bh, FALSE);
    MoveWindow(g_app.miniNext, x + 3 * (bw + 2), by, bw, bh, FALSE);

    const int volW = 100;
    g_app.miniVolume = MakeChild(w, TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_HORZ | TBS_NOTICKS | TBS_BOTH, IDC_MINI_VOLUME);
    MoveWindow(g_app.miniVolume, right - volW, by + 2, volW, bh - 4, FALSE);
    SendMessageW(g_app.miniVolume, TBM_SETRANGE, FALSE, MAKELPARAM(0, 100));
    SendMessageW(g_app.miniVolume, TBM_SETPAGESIZE, 0, 10);
    SendMessageW(g_app.miniVolume, TBM_SETPOS, TRUE, g_app.player.Volume());
    SetRect(&g_app.miniSpeakerRect, right - volW - 22, by, right - volW - 4, by + bh);

    // Start in step with the main window's seek bar.
    LRESULT maxSec = SendMessageW(g_app.seek, TBM_GETRANGEMAX, 0, 0);
    SendMessageW(g_app.miniSeek, TBM_SETRANGEMIN, FALSE, 0);
    SendMessageW(g_app.miniSeek, TBM_SETRANGEMAX, FALSE, maxSec);
    SendMessageW(g_app.miniSeek, TBM_SETPOS, TRUE, SendMessageW(g_app.seek, TBM_GETPOS, 0, 0));

    g_app.miniTooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_ALWAYSTIP,
                                        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                        w, NULL, inst, NULL);
    const struct { HWND ctl; const wchar_t* text; } tips[] = {
        { g_app.miniPrev, L"Previous" }, { g_app.miniPlayPause, L"Play / Pause" },
        { g_app.miniStop, L"Stop" }, { g_app.miniNext, L"Next" }, { g_app.miniVolume, L"Volume" },
    };
    for (size_t i = 0; i < sizeof(tips) / sizeof(tips[0]); ++i) {
        TOOLINFOW ti;
        ZeroMemory(&ti, sizeof(ti));
        ti.cbSize = sizeof(ti);
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = w;
        ti.uId = (UINT_PTR)tips[i].ctl;
        ti.lpszText = (LPWSTR)tips[i].text;
        SendMessageW(g_app.miniTooltip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    }
}

static int IniInt(const wchar_t* key, int def)
{
    // GetPrivateProfileInt can't return negative numbers (windows left of the main monitor).
    wchar_t buf[32];
    GetPrivateProfileStringW(L"Player", key, L"", buf, 32, g_app.iniPath.c_str());
    return buf[0] ? _wtoi(buf) : def;
}

static void ToggleMini()
{
    if (!g_app.mini)
        return;
    if (IsWindowVisible(g_app.mini)) {
        RECT r;
        GetWindowRect(g_app.mini, &r);
        WritePrivateProfileStringW(L"Player", L"MiniX", Fmt(L"%ld", r.left).c_str(), g_app.iniPath.c_str());
        WritePrivateProfileStringW(L"Player", L"MiniY", Fmt(L"%ld", r.top).c_str(), g_app.iniPath.c_str());
        ShowWindow(g_app.wnd, SW_SHOW);
        ShowWindow(g_app.mini, SW_HIDE);
        SetForegroundWindow(g_app.wnd);
        return;
    }
    // Where it was last time, else near the main window's corner; always fully on screen.
    RECT work, mr, main;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    GetWindowRect(g_app.mini, &mr);
    GetWindowRect(g_app.wnd, &main);
    const int w = mr.right - mr.left, h = mr.bottom - mr.top;
    int x = IniInt(L"MiniX", main.left + 40), y = IniInt(L"MiniY", main.top + 40);
    if (x > work.right - w) x = work.right - w;
    if (y > work.bottom - h) y = work.bottom - h;
    if (x < work.left) x = work.left;
    if (y < work.top) y = work.top;
    SetWindowPos(g_app.mini, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(g_app.mini, NULL, TRUE);
    ShowWindow(g_app.mini, SW_SHOW);
    ShowWindow(g_app.wnd, SW_HIDE);
    SetForegroundWindow(g_app.mini);
    SetFocus(g_app.miniPlayPause);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        OnCreate(hwnd);
        return 0;

    case WM_SIZE:
        Layout(hwnd);
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mm = (MINMAXINFO*)lp;
        mm->ptMinTrackSize.x = 760;
        mm->ptMinTrackSize.y = 540;
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        OnPaint(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DRAWITEM:
        DrawIconButton((const DRAWITEMSTRUCT*)lp);
        return TRUE;

    case WM_HSCROLL:
        OnHScroll((HWND)lp, LOWORD(wp));
        return 0;

    case WM_TIMER:
        if (wp == TIMER_POSITION)
            UpdatePosition();
        else if (wp == TIMER_SEARCH)
            ApplySearch();
        return 0;

    case WM_CONTEXTMENU:
        if ((HWND)wp == g_app.songs)
            SongsContextMenu(lp);
        else if ((HWND)wp == g_app.albums)
            AlbumsContextMenu(lp);
        else if ((HWND)wp == g_app.tree)
            TreeContextMenu(lp);
        return 0;

    case WM_COMMAND:
        if (LOWORD(wp) == IDC_SEARCH) {
            // Filter after a short pause in typing, not on every keystroke (7,000 songs on a P3).
            if (HIWORD(wp) == EN_CHANGE && !g_app.searchUpdating)
                SetTimer(hwnd, TIMER_SEARCH, 250, NULL);
            return 0;
        }
        // Owner-drawn buttons report a quick second click as BN_DOUBLECLICKED.
        if (HIWORD(wp) == BN_CLICKED || HIWORD(wp) == BN_DOUBLECLICKED || HIWORD(wp) == 1 /* accelerator */)
            OnCommand(LOWORD(wp));
        return 0;

    case WM_NOTIFY:
        return OnNotify((NMHDR*)lp);

    case WM_APP_JOB_DONE:
        OnJobDone((Job*)lp);
        return 0;

    case WM_PLAYER_STATE:
        OnPlayerState(wp, lp);
        RefreshTransport();
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_POSITION);
        g_app.player.Stop();
        ClearArtCache();
        DeleteObject(g_app.fontBold);
        DeleteObject(g_app.fontTitle);
        DeleteObject(g_app.latchBrush);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int show)
{
    InstallIllegalInstructionHandler();

    const std::wstring dir = ExeDirectory();
    g_app.iniPath = dir + L"\\Navi2KPlayer.ini";
    g_app.queueIndex = -1;
    g_app.pendingPlaylist = -1;
    LogInit(dir + L"\\Navi2KPlayer.log");
    srand(GetTickCount());

    // --- CPU check before anything that might execute SIMD code ---
    g_app.cpu = DetectCpu();
    std::wstring cpuProblem;
    if (!CpuCheckBuildCompatible(g_app.cpu, cpuProblem)) {
        LogF(L"Refusing to start: %ls", cpuProblem.c_str());
        MessageBoxW(NULL, cpuProblem.c_str(), APP_TITLE, MB_ICONSTOP);
        return 1;
    }
    const wchar_t* pcmPath = PcmInit(g_app.cpu.Sse2Usable());
    LogF(L"CPU %hs: cpuid=%d cmov=%d mmx=%d sse=%d sse2=%d os-xmm=%d -> %ls",
         g_app.cpu.vendor, g_app.cpu.cpuid, g_app.cpu.cmov, g_app.cpu.mmx, g_app.cpu.sse, g_app.cpu.sse2,
         g_app.cpu.osXmm, pcmPath);
    g_app.cpuSummary = g_app.cpu.Sse2Usable() ? L"[SSE2]" : L"[no SSE2: integer decode path]";

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_TREEVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    if (!HttpInit(L"Navi2KPlayer/" APP_VERSION)) {
        MessageBoxW(NULL, L"WinINet could not be initialised.", APP_TITLE, MB_ICONSTOP);
        return 1;
    }

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                 GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"Navi2KPlayerMain";
    if (!RegisterClassExW(&wc))
        return 1;

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, APP_TITLE, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, 1000, 680, NULL, NULL, inst, NULL);
    if (!hwnd)
        return 1;
    CreateMini(inst, wc.hIcon, wc.hIconSm);   // hidden until Ctrl+M
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);
    // Connect straight away; on first run this explains how to fill in the ini.
    PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDM_CONNECT, 0), 0);

    ACCEL accel[] = {
        { FVIRTKEY, VK_F5, IDM_CONNECT },
        { FVIRTKEY | FCONTROL, 'F', IDM_FIND },
        { FVIRTKEY | FCONTROL, 'M', IDM_MINI },
    };
    HACCEL accelTable = CreateAcceleratorTableW(accel, sizeof(accel) / sizeof(accel[0]));

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        // Shortcuts go to whichever of the two windows is showing.
        HWND target = (g_app.mini && IsWindowVisible(g_app.mini)) ? g_app.mini : hwnd;
        if (TranslateAcceleratorW(target, accelTable, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    DestroyAcceleratorTable(accelTable);

    // Let in-flight requests (e.g. a rating being saved) finish before WinINet goes away.
    for (int i = 0; i < 100 && g_app.jobsRunning > 0; ++i)
        Sleep(50);
    HttpShutdown();
    return (int)msg.wParam;
}
