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
#include <mmsystem.h>
#include <shellapi.h>
#include <process.h>
#include <stdlib.h>
#include <algorithm>
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
#define APP_VERSION L"1.1"

enum {
    IDC_PLAY = 102, IDC_PAUSE, IDC_STOP,
    IDC_ALBUMS, IDC_SONGS, IDC_STATUS,
    IDC_VOLUME, IDC_SEEK = 110, IDC_TIME_CUR, IDC_TIME_TOTAL,
    IDC_TREE, IDC_NEW_PLAYLIST, IDC_SHUFFLE, IDC_HDR_PLAY, IDC_HDR_SHUFFLE, IDC_ARTISTS,
    IDC_SEARCH, IDC_SEARCH_LABEL,
    // Mini player.
    IDC_MINI_PREV, IDC_MINI_PLAYPAUSE, IDC_MINI_STOP, IDC_MINI_NEXT,
    IDC_MINI_SEEK, IDC_MINI_VOLUME, IDC_MINI_TIME_CUR, IDC_MINI_TIME_TOTAL,
    IDC_REPEAT, IDC_PREV, IDC_NEXT
};

// Menu bar.
enum {
    IDM_CONNECT = 40001, IDM_EXIT, IDM_PLAY, IDM_PAUSE, IDM_STOP, IDM_SHUFFLE,
    IDM_ALBUMS, IDM_NEW_PLAYLIST, IDM_ABOUT, IDM_ARTISTS, IDM_SONGS, IDM_FIND,
    IDM_MINI, IDM_PREV, IDM_NEXT, IDM_SETTINGS, IDM_FAVORITES, IDM_QUEUE, IDM_SHOW, IDM_PLAYPAUSE,
    IDM_REPEAT_OFF, IDM_REPEAT_ALL, IDM_REPEAT_ONE, IDM_REPEAT_CYCLE, IDM_EDIT_PLAYLIST
};

enum RepeatMode { REPEAT_OFF, REPEAT_ALL, REPEAT_ONE };

// Media keys, as global hotkeys (Settings > Display) and as WM_APPCOMMAND when focused.
enum { HOTKEY_PLAYPAUSE = 1, HOTKEY_STOP, HOTKEY_PREV, HOTKEY_NEXT };
#ifndef APPCOMMAND_MEDIA_PLAY       // Windows XP and later only; harmless to handle on 2000
#define APPCOMMAND_MEDIA_PLAY  46
#define APPCOMMAND_MEDIA_PAUSE 47
#endif
#ifndef HDF_SORTUP                  // comctl32 6 (XP themes); set only when it's loaded
#define HDF_SORTUP   0x0400
#define HDF_SORTDOWN 0x0200
#endif

// Popup menu commands (TrackPopupMenu with TPM_RETURNCMD).
enum {
    CMD_PLAY = 1001, CMD_MOVE_UP, CMD_MOVE_DOWN, CMD_REMOVE,
    CMD_PL_PLAY, CMD_PL_NEW, CMD_PL_RENAME, CMD_PL_DELETE, CMD_FAV,
    CMD_PLAY_NEXT, CMD_ADD_QUEUE, CMD_CLEAR_UPCOMING, CMD_GO_ALBUM, CMD_GO_ARTIST,
    CMD_ADD_NEW = 1050,
    CMD_RATE_BASE = 1100,       // + 0..5
    CMD_ADD_BASE = 2000         // + playlist index
};

enum { TIMER_POSITION = 1, TIMER_SEARCH = 2, TIMER_DRAGSCROLL = 3 };
enum { COL_NUM = 0, COL_TITLE, COL_ARTIST, COL_FAV, COL_RATING, COL_LENGTH, COL_ALBUM };

// Sidebar tree item data (playlists use their index, >= 0).
enum { TREE_LIBRARY = -1, TREE_ALBUMS = -2, TREE_PLAYLISTS = -3, TREE_ARTISTS = -4, TREE_SONGS = -5,
       TREE_FAVORITES = -6, TREE_QUEUE = -7 };

#define WM_APP_JOB_DONE (WM_APP + 10)
#define WM_APP_TEST_DONE (WM_APP + 11)   // Settings > Test Connection finished (to the page)
#define WM_APP_TRAY (WM_APP + 12)        // notification-area icon mouse messages
static const UINT kTrayId = 1;

static const int kArtSize = 160;        // requested from the server and drawn 1:1
static const size_t kArtCacheMax = 48;  // ~5 MB of 160x160 DIBs
static const int kPad = 6;
static const int kSidebarW = kArtSize + 12;

// --- Background jobs ------------------------------------------------------------------
// Loads (connect/album/playlist) run one at a time. Writes (ratings, playlist edits) and
// now-playing cover fetches run alongside them; playlist edits are serialised because
// removal is by index.

enum JobKind { JOB_CONNECT, JOB_ALBUM, JOB_PLAYLIST, JOB_RATE, JOB_PLEDIT, JOB_ART, JOB_ARTIST, JOB_SONGS,
               JOB_FAVORITES, JOB_STAR, JOB_SCROBBLE, JOB_QUEUE_FETCH, JOB_INFO };
enum PlEdit { PL_CREATE, PL_RENAME, PL_ADD, PL_REMOVE, PL_SETSONGS, PL_DELETE };

static bool IsLoadJob(JobKind k)
{
    return k == JOB_CONNECT || k == JOB_ALBUM || k == JOB_PLAYLIST || k == JOB_ARTIST || k == JOB_SONGS ||
           k == JOB_FAVORITES;
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
    bool flag, oldFlag;         // star: new/old state; scrobble: submission; queue fetch: play next

    // playlist edit
    PlEdit edit;
    std::string plId;
    std::wstring name, comment;
    std::wstring notes;         // Last.fm description (JOB_INFO: songId = album/artist, flag = artist)
    std::vector<std::string> songIds;
    std::string addAlbumId;     // add/create from a whole album (fetched in the job)
    int index;
    std::string reloadId;       // playlist on screen, re-read after the edit
    bool reloaded;
    int selectAfter;
};

enum SongSource { SRC_NONE, SRC_ALBUM, SRC_PLAYLIST, SRC_LIBRARY, SRC_FAVORITES, SRC_QUEUE };
// VIEW_ALBUMS: all albums. VIEW_ARTISTS: all artists. VIEW_ARTIST: one artist's albums
// (same list control as VIEW_ALBUMS). VIEW_TRACKS: an album's or playlist's tracks.
// VIEW_SONGS: every song in the library; VIEW_FAVORITES: starred songs; VIEW_QUEUE: the play
// queue (all use the same list control as VIEW_TRACKS).
enum MainView { VIEW_ALBUMS, VIEW_TRACKS, VIEW_ARTISTS, VIEW_ARTIST, VIEW_SONGS, VIEW_FAVORITES, VIEW_QUEUE };

struct Shown {                  // what the header describes
    bool has;
    std::string id, coverArt;
    std::wstring title, line2, line3;
    std::wstring note;          // free text under the details (playlist comment)
};

struct App {
    HWND wnd, tree, btnNewPlaylist, albums, artists, songs, status;
    HWND search, searchLabel;
    // Mini player (a separate top-level window; Ctrl+M swaps it with the main window).
    HWND mini, miniPrev, miniPlayPause, miniStop, miniNext, miniSeek, miniVolume, miniTimeCur, miniTimeTotal;
    HWND miniTooltip;
    RECT miniSpeakerRect;
    HWND btnPrev, btnPlay, btnPause, btnStop, btnNext, btnShuffle, btnRepeat, hdrPlay, hdrShuffle;
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
    SubsonicAlbum shownAlbum;               // valid when songSource == SRC_ALBUM
    std::string shownArtistCover;            // artist image, or a collage of their album covers
    std::vector<SubsonicSong> allSongs;      // the whole library, loaded on first visit to Songs
    bool allSongsLoaded;
    bool pendingSongs;
    HTREEITEM treeFavorites, treeQueue;
    bool favoritesLoaded;
    bool pendingFavorites;

    // Scrobbling: listening time of the current play, counted from position changes so
    // that seeking doesn't count as listening.
    bool scrobbleEnabled;
    std::string scrobbleId;
    DWORD listenedMs, lastPosMs;
    bool nowPlayingSent, scrobbled;

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
    // Editing the queue (Play next, Add to queue, remove, reorder) makes its current order
    // the new source.
    std::vector<SubsonicSong> queueSource, queue;
    std::vector<int> queueSrcIdx;
    int queueIndex;
    bool queueFinished;                // played to the end; adding tracks starts them
    bool shuffle;
    int repeat;                        // RepeatMode

    // Column sorting. The track list sorts a copy (songUnsorted keeps the original order,
    // restored by clicking "#"); songSortKey names the list it applies to, so another
    // album or playlist opens unsorted. -1 = not sorted.
    int songSortCol, albumSortCol, artistSortCol;
    bool songSortDesc, albumSortDesc, artistSortDesc;
    std::string songSortKey;
    std::vector<SubsonicSong> songUnsorted;
    bool headerArrows;                 // comctl32 6 draws sort arrows in list headers

    bool mediaKeysGlobal;              // Settings > Display
    bool lastfmInfo;                   // Settings > Display: album/artist descriptions (off by default)
    std::map<std::string, std::wstring> infoCache;   // "al:"/"ar:" + id -> description ("" = none)
    std::string infoPending;
    RECT noteRect;                     // where the header's description was drawn
    bool noteCut;                      // ...and whether it was cut short (click shows all of it)
    bool hotkeysRegistered;
    bool showFavorites, showRatings;   // File > Settings > Display
    bool playingFlac;                  // current track streams as original FLAC
    bool gapless;                      // Settings > Playback
    bool dragging;                     // a track is being dragged to a new place
    int dragFrom, dragInsert;          // its row, and the row it would go before (-1: none)
    POINT dragPt;                      // last mouse position, in the track list's coordinates
    int gaplessIdx;                    // queue entry given to the player as the next track

    // Notification-area icon (Settings > Display).
    bool trayIcon, trayMinimize;
    bool trayAdded;
    HWND trayHidden;                   // window hidden by minimize-to-tray, shown again on click
    UINT taskbarCreatedMsg;            // Explorer restarted: add the icon again

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

// No artist photo: one album's cover, or a collage of up to four (worker thread).
static void ArtistCollage(Job* job)
{
    std::vector<std::string> ids;
    for (size_t i = 0; i < job->albums.size() && ids.size() < 4; ++i) {
        const std::string& id = job->albums[i].coverArt;
        if (!id.empty() && std::find(ids.begin(), ids.end(), id) == ids.end())
            ids.push_back(id);
    }
    if (ids.empty())
        return;
    if (ids.size() == 1) {   // the album's own cover, shared with its album page
        job->artId = ids[0];
        job->artTried = false;
        job->needArt = true;
        FetchArt(job);
        return;
    }
    std::vector<std::string> images;
    for (size_t i = 0; i < ids.size(); ++i) {
        std::string bytes;
        std::wstring err;
        if (g_app.client.GetCoverArt(ids[i], kArtSize, bytes, err))
            images.push_back(bytes);
        else
            LogF(L"Cover art: %ls", err.c_str());
    }
    std::wstring err;
    job->artId = "collage:" + job->artist.id;
    job->art = ArtCollage(images, kArtSize, GetSysColor(COLOR_BTNFACE), err);
    if (!job->art)
        LogF(L"Artist collage: %ls", err.c_str());
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
        job->ok = c.SetPlaylistDetails(job->plId, job->name, job->comment, job->message);
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
    case JOB_FAVORITES:
        job->ok = c.GetStarredSongs(job->songs, job->message);
        break;
    case JOB_STAR:
        job->ok = c.SetStarred(job->songId, job->flag, job->message);
        break;
    case JOB_SCROBBLE:
        job->ok = c.Scrobble(job->songId, job->flag, job->message);
        break;
    case JOB_QUEUE_FETCH:   // an album's or playlist's tracks, for Play next / Add to queue
        job->ok = !job->addAlbumId.empty() ? c.GetAlbum(job->addAlbumId, job->songs, job->message)
                                           : c.GetPlaylist(job->plId, job->songs, job->message);
        break;
    case JOB_ARTIST:
        job->ok = c.GetArtist(job->artist.id, job->albums, job->message);
        // Navidrome sends a WebP placeholder when it has no artist photo, which we can't
        // decode, or a "no photo" silhouette from an online source; fall back to the
        // artist's album covers.
        FetchArt(job);
        if (job->art && ArtIsPlaceholder(job->art)) {
            DeleteObject(job->art);
            job->art = NULL;
        }
        if (!job->art && job->ok)
            ArtistCollage(job);
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
    case JOB_INFO:
        job->ok = job->flag ? c.GetArtistBio(job->songId, job->notes, job->message)
                            : c.GetAlbumNotes(job->songId, job->notes, job->message);
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
    job->flag = job->oldFlag = false;
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
                     const std::wstring& line2, const std::wstring& line3,
                     const std::wstring& note = std::wstring())
{
    g_app.shown.has = true;
    g_app.shown.id = id;
    g_app.shown.coverArt = coverArt;
    g_app.shown.title = title;
    g_app.shown.line2 = line2;
    g_app.shown.line3 = line3;
    g_app.shown.note = note;
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
             Fmt(L"%d tracks  \x00B7  %ls", pl.songCount, FormatTime(pl.duration).c_str()), pl.comment);
}

// --- Last.fm descriptions (Settings > Display, off by default) -----------------------------

static std::string InfoKey(bool artist, const std::string& id)
{
    return (artist ? "ar:" : "al:") + id;
}

// The album or artist page on screen, as an InfoKey; "" on other pages.
static std::string ShownInfoKey()
{
    if (g_app.mainView == VIEW_ARTIST)
        return InfoKey(true, g_app.shownArtist.id);
    if (g_app.mainView == VIEW_TRACKS && g_app.songSource == SRC_ALBUM)
        return InfoKey(false, g_app.shownAlbum.id);
    return std::string();
}

// The cached description, or "" for now and a background fetch the first time.
static std::wstring InfoNote(bool artist, const std::string& id)
{
    if (!g_app.lastfmInfo || id.empty())
        return std::wstring();
    const std::string key = InfoKey(artist, id);
    std::map<std::string, std::wstring>::iterator it = g_app.infoCache.find(key);
    if (it != g_app.infoCache.end())
        return it->second;
    if (g_app.infoPending != key) {
        g_app.infoPending = key;
        Job* job = NewJob(JOB_INFO);
        job->songId = id;
        job->flag = artist;
        LaunchJob(job);
    }
    return std::wstring();
}

static void ShowAlbumInfo(const SubsonicAlbum& a)
{
    g_app.shownAlbum = a;
    std::wstring details = Fmt(L"%d tracks  \x00B7  %ls", (int)g_app.songData.size(),
                               FormatTime(TotalDuration(g_app.songData)).c_str());
    if (a.year)
        details = Fmt(L"%d  \x00B7  ", a.year) + details;
    ShowInfo(a.id, a.coverArt, a.name, L"by " + a.artist, details, InfoNote(false, a.id));
}

static bool QueueValid()
{
    return g_app.queueIndex >= 0 && g_app.queueIndex < (int)g_app.queue.size();
}

// --- Notification-area icon -------------------------------------------------------------
// Built with _WIN32_IE=0x0500, so NOTIFYICONDATAW is the Windows 2000 size (128-char tip).

static std::wstring NowPlayingCaption()
{
    if (!QueueValid())
        return APP_TITLE;
    const SubsonicSong& s = g_app.queue[g_app.queueIndex];
    return L"Navi2K - " + s.title + (s.artist.empty() ? L"" : L" - " + s.artist);
}

static void TrayData(NOTIFYICONDATAW& nid)
{
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_app.wnd;
    nid.uID = kTrayId;
}

static void TrayUpdate()
{
    if (!g_app.trayAdded)
        return;
    NOTIFYICONDATAW nid;
    TrayData(nid);
    nid.uFlags = NIF_TIP;
    lstrcpynW(nid.szTip, NowPlayingCaption().c_str(), sizeof(nid.szTip) / sizeof(nid.szTip[0]));
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void TrayAdd()
{
    if (g_app.trayAdded)
        return;
    NOTIFYICONDATAW nid;
    TrayData(nid);
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = (HICON)GetClassLongPtrW(g_app.wnd, GCLP_HICONSM);
    lstrcpynW(nid.szTip, NowPlayingCaption().c_str(), sizeof(nid.szTip) / sizeof(nid.szTip[0]));
    g_app.trayAdded = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    if (!g_app.trayAdded)
        LogF(L"Could not add the notification-area icon (%lu).", GetLastError());
}

static void TrayRemove()
{
    if (!g_app.trayAdded)
        return;
    NOTIFYICONDATAW nid;
    TrayData(nid);
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_app.trayAdded = false;
}

// Brings back the window that minimize-to-tray hid (the main window or the mini player).
static void TrayRestore()
{
    HWND w = g_app.trayHidden;
    if (!w)
        w = (g_app.mini && IsWindowVisible(g_app.mini)) ? g_app.mini : g_app.wnd;
    g_app.trayHidden = NULL;
    ShowWindow(w, IsIconic(w) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(w);
}

// Called on WM_SIZE / SIZE_MINIMIZED: true if the window was hidden into the tray.
static bool TrayMinimize(HWND w)
{
    if (!g_app.trayIcon || !g_app.trayMinimize || !g_app.trayAdded)
        return false;
    g_app.trayHidden = w;
    ShowWindow(w, SW_HIDE);
    return true;
}

// --- Media keys -------------------------------------------------------------------------
// With "media keys in the background" on, the four media keys are global hotkeys (only one
// program can hold them; if another has, we fall back). Otherwise they arrive as
// WM_APPCOMMAND while the player has focus.

static void RegisterMediaKeys(bool on)
{
    if (g_app.hotkeysRegistered) {
        for (int id = HOTKEY_PLAYPAUSE; id <= HOTKEY_NEXT; ++id)
            UnregisterHotKey(g_app.wnd, id);
        g_app.hotkeysRegistered = false;
    }
    if (!on)
        return;
    const struct { int id; UINT vk; } keys[] = {
        { HOTKEY_PLAYPAUSE, VK_MEDIA_PLAY_PAUSE }, { HOTKEY_STOP, VK_MEDIA_STOP },
        { HOTKEY_PREV, VK_MEDIA_PREV_TRACK }, { HOTKEY_NEXT, VK_MEDIA_NEXT_TRACK },
    };
    int ok = 0;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i)
        if (RegisterHotKey(g_app.wnd, keys[i].id, 0, keys[i].vk))
            ++ok;
        else
            LogF(L"Media key %u is taken by another program (%lu).", keys[i].vk, GetLastError());
    g_app.hotkeysRegistered = ok > 0;
}

// comctl32 6 (XP with themes) draws sort arrows in list headers; 5.x doesn't know the flags.
static bool CommonControls6()
{
    struct DllVersion { DWORD cbSize, major, minor, build, platform; };
    typedef HRESULT (CALLBACK* GetVersionFn)(DllVersion*);
    HMODULE cc = GetModuleHandleW(L"comctl32.dll");
    GetVersionFn fn = cc ? (GetVersionFn)GetProcAddress(cc, "DllGetVersion") : NULL;
    DllVersion v;
    ZeroMemory(&v, sizeof(v));
    v.cbSize = sizeof(v);
    return fn && SUCCEEDED(fn(&v)) && v.major >= 6;
}

// Shows the playing track's cover in the sidebar, fetching it if needed, and names the
// track in the title bar (and so the taskbar button) and the tray icon's tooltip.
static void UpdateNowPlaying()
{
    SetWindowTextW(g_app.wnd, NowPlayingCaption().c_str());
    TrayUpdate();
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

// --- Column sorting ------------------------------------------------------------------------
// Click a column header to sort, again to reverse. Sorts are stable, so equal rows keep the
// server's order. The track list's "#" column restores the original order.

// Sort arrows in the header: comctl32 6 only (XP with themes); older versions just don't show them.
static void SetHeaderArrow(HWND lv, int col, bool desc)
{
    if (!g_app.headerArrows)
        return;
    HWND hdr = ListView_GetHeader(lv);
    const int n = Header_GetItemCount(hdr);
    for (int i = 0; i < n; ++i) {
        HDITEMW it;
        ZeroMemory(&it, sizeof(it));
        it.mask = HDI_FORMAT;
        Header_GetItem(hdr, i, &it);
        it.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (i == col)
            it.fmt |= desc ? HDF_SORTDOWN : HDF_SORTUP;
        Header_SetItem(hdr, i, &it);
    }
}

static int CmpText(const std::wstring& a, const std::wstring& b)
{
    return lstrcmpiW(a.c_str(), b.c_str());
}

static int CmpInt(int a, int b)
{
    return a < b ? -1 : (a > b ? 1 : 0);
}

struct AlbumLess {
    int col;
    bool operator()(const SubsonicAlbum& a, const SubsonicAlbum& b) const
    {
        int c = 0;
        switch (col) {
        case 0: c = CmpText(a.name, b.name); break;
        case 1:   // artist, then their albums by year
            c = CmpText(a.artist, b.artist);
            if (!c) c = CmpInt(a.year, b.year);
            if (!c) c = CmpText(a.name, b.name);
            break;
        case 2: c = CmpInt(a.year, b.year); break;
        case 3: c = CmpInt(a.songCount, b.songCount); break;
        }
        return c < 0;
    }
};

struct ArtistLess {
    int col;
    bool operator()(const SubsonicArtist& a, const SubsonicArtist& b) const
    {
        return (col == 0 ? CmpText(a.name, b.name) : CmpInt(a.albumCount, b.albumCount)) < 0;
    }
};

struct SongLess {
    int col;
    bool operator()(const SubsonicSong& a, const SubsonicSong& b) const
    {
        int c = 0;
        switch (col) {
        case COL_TITLE:  c = CmpText(a.title, b.title); break;
        case COL_ARTIST:   // artist, then album, then track order
            c = CmpText(a.artist, b.artist);
            if (!c) c = CmpText(a.album, b.album);
            if (!c) c = CmpInt(a.disc, b.disc);
            if (!c) c = CmpInt(a.track, b.track);
            break;
        case COL_ALBUM:
            c = CmpText(a.album, b.album);
            if (!c) c = CmpInt(a.disc, b.disc);
            if (!c) c = CmpInt(a.track, b.track);
            break;
        case COL_FAV:    c = CmpInt(a.starred ? 1 : 0, b.starred ? 1 : 0); break;
        case COL_RATING: c = CmpInt(a.userRating, b.userRating); break;
        case COL_LENGTH: c = CmpInt(a.duration, b.duration); break;
        }
        return c < 0;
    }
};

// Descending sorts swap the arguments, which keeps them stable.
template <class T, class Less>
struct Reversed {
    Less less;
    bool operator()(const T& a, const T& b) const { return less(b, a); }
};

template <class T, class Less>
static void SortList(std::vector<T>& v, int col, bool desc)
{
    Less less;
    less.col = col;
    if (desc) {
        Reversed<T, Less> r;
        r.less = less;
        std::stable_sort(v.begin(), v.end(), r);
    } else {
        std::stable_sort(v.begin(), v.end(), less);
    }
}

// Rebuilds the album list from every album (filtered by the Albums search) or the open artist.
static void RebuildAlbumList()
{
    if (g_app.albumListIsArtist)
        g_app.albumList = g_app.artistAlbums;
    else
        FilterInto(g_app.albumData, g_app.albumKeys, g_app.queryAlbums, g_app.albumList);
    if (g_app.albumSortCol >= 0)
        SortList<SubsonicAlbum, AlbumLess>(g_app.albumList, g_app.albumSortCol, g_app.albumSortDesc);
    FillAlbums();
}

static void FillArtists()
{
    HWND lv = g_app.artists;
    FilterInto(g_app.artistData, g_app.artistKeys, g_app.queryArtists, g_app.artistList);
    if (g_app.artistSortCol >= 0)
        SortList<SubsonicArtist, ArtistLess>(g_app.artistList, g_app.artistSortCol, g_app.artistSortDesc);
    SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(lv);
    for (size_t i = 0; i < g_app.artistList.size(); ++i) {
        SetCell(lv, (int)i, 0, g_app.artistList[i].name);
        SetCell(lv, (int)i, 1, Fmt(L"%d", g_app.artistList[i].albumCount));
    }
    SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lv, NULL, TRUE);
}

// Which list the track table shows, so a sort stays with that list (a Songs search or a
// playlist reload keeps it) and a different album or playlist starts unsorted.
static std::string SongListKey()
{
    switch (g_app.songSource) {
    case SRC_ALBUM:     return "a:" + g_app.shown.id;
    case SRC_PLAYLIST:  return "p:" + g_app.shown.id;
    case SRC_LIBRARY:   return "library";
    case SRC_FAVORITES: return "favorites";
    default:            return std::string();   // the queue (its order is the play order) and nothing
    }
}

static bool SongsSorted()
{
    return g_app.songSortCol >= 0;
}

// The track table is going to show new songData: re-applies the sort if it's the same list.
static void ApplySongSortToNewData()
{
    const std::string key = SongListKey();
    if (SongsSorted() && !key.empty() && key == g_app.songSortKey) {
        g_app.songUnsorted = g_app.songData;
        SortList<SubsonicSong, SongLess>(g_app.songData, g_app.songSortCol, g_app.songSortDesc);
    } else if (SongsSorted()) {
        g_app.songSortCol = -1;
        g_app.songUnsorted.clear();
        SetHeaderArrow(g_app.songs, -1, false);
    }
}

// The track list is virtual (LVS_OWNERDATA): it holds only a row count and asks for the
// text of visible cells (OnSongsDispInfo), so the 7,000-song library view fills instantly.
static void FillSongs()
{
    HWND lv = g_app.songs;
    ApplySongSortToNewData();
    ListView_SetItemState(lv, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetItemCountEx(lv, (int)g_app.songData.size(), 0);
    if (!g_app.songData.empty())
        ListView_EnsureVisible(lv, 0, FALSE);
    InvalidateRect(lv, NULL, TRUE);
}

static const wchar_t* const kSongColNames[] = { L"#", L"Track", L"Artist", L"Favorites", L"Rating", L"Time", L"Album" };

// Header click on the track table. Rating and favorites sort highest first.
static void OnSongsColumnClick(int col)
{
    if (g_app.songSource == SRC_QUEUE) {
        SetStatus(L"The queue plays in the order shown: right-click a track to move it.");
        return;
    }
    if (SongListKey().empty() || (col == COL_FAV && !g_app.showFavorites) || (col == COL_RATING && !g_app.showRatings))
        return;
    const int sel = SelectedIndex(g_app.songs);
    const std::string selId = sel >= 0 && sel < (int)g_app.songData.size() ? g_app.songData[sel].id : std::string();

    if (col == COL_NUM) {
        if (!SongsSorted())
            return;
        g_app.songData = g_app.songUnsorted;
        g_app.songUnsorted.clear();
        g_app.songSortCol = -1;
        SetHeaderArrow(g_app.songs, -1, false);
        SetStatus(L"Back to the original order.");
    } else {
        if (!SongsSorted()) {
            g_app.songUnsorted = g_app.songData;
            g_app.songSortKey = SongListKey();
        }
        g_app.songSortDesc = col == g_app.songSortCol ? !g_app.songSortDesc : (col == COL_FAV || col == COL_RATING);
        g_app.songSortCol = col;
        g_app.songData = g_app.songUnsorted;
        SortList<SubsonicSong, SongLess>(g_app.songData, col, g_app.songSortDesc);
        SetHeaderArrow(g_app.songs, col, g_app.songSortDesc);
        std::wstring msg = Fmt(L"Sorted by %ls%ls. Click # for the original order", kSongColNames[col],
                               g_app.songSortDesc ? L" (reversed)" : L"");
        if (g_app.songSource == SRC_PLAYLIST)
            msg += L" (needed to move or remove tracks)";
        SetStatus(msg + L".");
    }
    ListView_SetItemState(g_app.songs, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    InvalidateRect(g_app.songs, NULL, TRUE);
    for (size_t i = 0; i < g_app.songData.size() && !selId.empty(); ++i)
        if (g_app.songData[i].id == selId) {
            SelectRow(g_app.songs, (int)i);
            break;
        }
}

static void OnAlbumsColumnClick(int col)
{
    const int sel = SelectedIndex(g_app.albums);
    const std::string selId = sel >= 0 && sel < (int)g_app.albumList.size() ? g_app.albumList[sel].id : std::string();
    g_app.albumSortDesc = col == g_app.albumSortCol ? !g_app.albumSortDesc : false;
    g_app.albumSortCol = col;
    RebuildAlbumList();
    SetHeaderArrow(g_app.albums, col, g_app.albumSortDesc);
    for (size_t i = 0; i < g_app.albumList.size(); ++i)
        if (g_app.albumList[i].id == selId) {
            SelectRow(g_app.albums, (int)i);
            return;
        }
    SelectRow(g_app.albums, 0);
}

static void OnArtistsColumnClick(int col)
{
    const int sel = SelectedIndex(g_app.artists);
    const std::string selId = sel >= 0 && sel < (int)g_app.artistList.size() ? g_app.artistList[sel].id : std::string();
    g_app.artistSortDesc = col == g_app.artistSortCol ? !g_app.artistSortDesc : col == 1;
    g_app.artistSortCol = col;
    FillArtists();
    SetHeaderArrow(g_app.artists, col, g_app.artistSortDesc);
    for (size_t i = 0; i < g_app.artistList.size(); ++i)
        if (g_app.artistList[i].id == selId) {
            SelectRow(g_app.artists, (int)i);
            return;
        }
    SelectRow(g_app.artists, 0);
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
        // Playlists and the queue number by position; albums and the library by track number.
        int n = (g_app.songSource == SRC_PLAYLIST || g_app.songSource == SRC_FAVORITES ||
                 g_app.songSource == SRC_QUEUE) ? i + 1 : s.track;
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

static HTREEITEM TreeAdd(HTREEITEM parent, const std::wstring& text, LPARAM data, bool bold,
                         HTREEITEM after = TVI_LAST)
{
    TVINSERTSTRUCTW ins;
    ZeroMemory(&ins, sizeof(ins));
    ins.hParent = parent;
    ins.hInsertAfter = after;
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
    else if (g_app.mainView == VIEW_FAVORITES)
        TreeView_SelectItem(g_app.tree, g_app.treeFavorites);
    else if (g_app.mainView == VIEW_QUEUE)
        TreeView_SelectItem(g_app.tree, g_app.treeQueue);
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

// Heart outline, 11 x 10 px around (cx, cy). The UI font on Windows 2000 has no ♥ glyph.
static void DrawHeart(HDC dc, int cx, int cy, bool filled, COLORREF line)
{
    static const int kHeart[14][2] = {
        {0, 4}, {-5, -1}, {-5, -3}, {-4, -4}, {-3, -5}, {-2, -5}, {-1, -4},
        {0, -3}, {1, -4}, {2, -5}, {3, -5}, {4, -4}, {5, -3}, {5, -1}
    };
    POINT pts[14];
    for (int i = 0; i < 14; ++i) {
        pts[i].x = cx + kHeart[i][0];
        pts[i].y = cy + kHeart[i][1];
    }
    const COLORREF red = RGB(214, 40, 57);
    HPEN pen = CreatePen(PS_SOLID, 1, filled ? red : line);
    HBRUSH brush = filled ? CreateSolidBrush(red) : NULL;
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, brush ? (HGDIOBJ)brush : GetStockObject(NULL_BRUSH));
    Polygon(dc, pts, 14);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
    if (brush)
        DeleteObject(brush);
}

// Draws the heart and star columns of one row (custom-draw post-paint).
static void DrawRowExtras(HDC dc, int item)
{
    if (item < 0 || item >= (int)g_app.songData.size())
        return;
    const SubsonicSong& song = g_app.songData[item];
    const bool highlighted = (ListView_GetItemState(g_app.songs, item, LVIS_SELECTED) & LVIS_SELECTED) &&
                             GetFocus() == g_app.songs;
    const COLORREF empty = GetSysColor(highlighted ? COLOR_HIGHLIGHTTEXT : COLOR_GRAYTEXT);

    RECT cell;
    if (g_app.showFavorites && ListView_GetSubItemRect(g_app.songs, item, COL_FAV, LVIR_BOUNDS, &cell))
        DrawHeart(dc, (cell.left + cell.right) / 2, (cell.top + cell.bottom) / 2, song.starred, empty);

    if (!g_app.showRatings || !ListView_GetSubItemRect(g_app.songs, item, COL_RATING, LVIR_BOUNDS, &cell))
        return;
    const int s = StarSize(cell), r = s / 2, cy = (cell.top + cell.bottom) / 2 + 1;
    HRGN clip = CreateRectRgnIndirect(&cell);
    SelectClipRgn(dc, clip);
    for (int i = 0; i < 5; ++i)
        DrawStar(dc, cell.left + 4 + r + i * (s + 2), cy, r, i < song.userRating, empty);
    SelectClipRgn(dc, NULL);
    DeleteObject(clip);
}

static LRESULT OnSongsCustomDraw(NMLVCUSTOMDRAW* cd)
{
    switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT: {
        // The playing track is shown in bold, as in the classic Spotify list. In the queue
        // it's the row at the play position (a track can be queued twice), and tracks
        // already played are greyed.
        int item = (int)cd->nmcd.dwItemSpec;
        if (g_app.songSource == SRC_QUEUE) {
            if (item == g_app.queueIndex) {
                SelectObject(cd->nmcd.hdc, g_app.fontBold);
                return CDRF_NEWFONT | CDRF_NOTIFYPOSTPAINT;
            }
            if (item < g_app.queueIndex && !ListView_GetItemState(g_app.songs, item, LVIS_SELECTED))
                cd->clrText = GetSysColor(COLOR_GRAYTEXT);
            return CDRF_NEWFONT | CDRF_NOTIFYPOSTPAINT;
        }
        if (QueueValid() && item < (int)g_app.songData.size() &&
            g_app.songData[item].id == g_app.queue[g_app.queueIndex].id) {
            SelectObject(cd->nmcd.hdc, g_app.fontBold);
            return CDRF_NEWFONT | CDRF_NOTIFYPOSTPAINT;
        }
        return CDRF_NOTIFYPOSTPAINT;
    }
    case CDDS_ITEMPOSTPAINT: {
        const int item = (int)cd->nmcd.dwItemSpec;
        DrawRowExtras(cd->nmcd.hdc, item);
        // Drag and drop: a line where the track would go (above this row, or below the last).
        const int last = (int)g_app.songData.size() - 1;
        if (g_app.dragging && (item == g_app.dragInsert || (item == last && g_app.dragInsert == last + 1))) {
            RECT r;
            ListView_GetItemRect(g_app.songs, item, &r, LVIR_BOUNDS);
            const int y = item == g_app.dragInsert ? r.top : r.bottom - 2;
            RECT line = { r.left, y, r.right, y + 2 };
            FillRect(cd->nmcd.hdc, &line, GetSysColorBrush(COLOR_WINDOWTEXT));
        }
        return CDRF_DODEFAULT;
    }
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
    for (size_t i = 0; i < g_app.songUnsorted.size(); ++i)
        if (g_app.songUnsorted[i].id == songId)
            g_app.songUnsorted[i].userRating = rating;
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

// Updates the favorite flag everywhere the track appears and repaints its rows.
static void ApplyFavoriteLocal(const std::string& songId, bool starred)
{
    for (size_t i = 0; i < g_app.songData.size(); ++i)
        if (g_app.songData[i].id == songId) {
            g_app.songData[i].starred = starred;
            ListView_RedrawItems(g_app.songs, (int)i, (int)i);
        }
    for (size_t i = 0; i < g_app.songUnsorted.size(); ++i)
        if (g_app.songUnsorted[i].id == songId)
            g_app.songUnsorted[i].starred = starred;
    for (size_t i = 0; i < g_app.queue.size(); ++i)
        if (g_app.queue[i].id == songId)
            g_app.queue[i].starred = starred;
    for (size_t i = 0; i < g_app.queueSource.size(); ++i)
        if (g_app.queueSource[i].id == songId)
            g_app.queueSource[i].starred = starred;
    for (size_t i = 0; i < g_app.allSongs.size(); ++i)
        if (g_app.allSongs[i].id == songId) {
            g_app.allSongs[i].starred = starred;
            break;
        }
}

static void ToggleFavorite(int i)
{
    if (i < 0 || i >= (int)g_app.songData.size())
        return;
    const SubsonicSong& s = g_app.songData[i];
    Job* job = NewJob(JOB_STAR);
    job->songId = s.id;
    job->flag = !s.starred;
    job->oldFlag = s.starred;
    // Optimistic: show it now, revert if the server refuses.
    ApplyFavoriteLocal(s.id, job->flag);
    SetStatus((job->flag ? L"Added to Favorites: \"" : L"Removed from Favorites: \"") + s.title + L"\"");
    LaunchJob(job);
}

// Click on the heart toggles Favorites; on the stars, star N sets N (the current one clears).
static void OnSongsClick(NMITEMACTIVATE* ia)
{
    LVHITTESTINFO hti;
    ZeroMemory(&hti, sizeof(hti));
    hti.pt = ia->ptAction;
    if (ListView_SubItemHitTest(g_app.songs, &hti) < 0)
        return;
    if (hti.iSubItem == COL_FAV && g_app.showFavorites) {
        ToggleFavorite(hti.iItem);
        return;
    }
    if (hti.iSubItem != COL_RATING || !g_app.showRatings)
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

// --- Scrobbling -------------------------------------------------------------------------
// "Now playing" when a track starts; a play is submitted once half the track (or 4 minutes)
// has actually been listened to, as Last.fm counts it. Tracks under 30 s are never submitted.

static void SendScrobble(const std::string& songId, bool submission)
{
    Job* job = NewJob(JOB_SCROBBLE);
    job->songId = songId;
    job->flag = submission;
    LaunchJob(job);
}

// A new play of a track resets the count; a seek within it doesn't.
static void ScrobbleTrackStarted(const std::string& songId, DWORD startMs)
{
    if (startMs == 0 || songId != g_app.scrobbleId) {
        g_app.scrobbleId = songId;
        g_app.listenedMs = 0;
        g_app.nowPlayingSent = false;
        g_app.scrobbled = false;
    }
    g_app.lastPosMs = startMs;
}

static DWORD ScrobbleThresholdMs(const SubsonicSong& s)
{
    DWORD half = (DWORD)s.duration * 500;
    return (s.duration > 0 && half < 240000) ? half : 240000;
}

static void ScrobbleCheck(bool trackFinished)
{
    if (!g_app.scrobbleEnabled || g_app.scrobbled || !QueueValid())
        return;
    const SubsonicSong& s = g_app.queue[g_app.queueIndex];
    if (s.id != g_app.scrobbleId || (s.duration > 0 && s.duration < 30))
        return;
    // At the end of a track the last timer tick may have just missed the threshold.
    DWORD slack = trackFinished ? 1000 : 0;
    if (g_app.listenedMs + slack >= ScrobbleThresholdMs(s)) {
        g_app.scrobbled = true;
        SendScrobble(s.id, true);
        LogF(L"Scrobbled \"%ls\" after %lu s of listening", s.title.c_str(), g_app.listenedMs / 1000);
    }
}

// Called with each position update: forward movement in small steps counts as listening;
// jumps (seeks) and pauses don't.
static void ScrobbleTrackPosition(DWORD posMs)
{
    if (posMs > g_app.lastPosMs && posMs - g_app.lastPosMs < 2000)
        g_app.listenedMs += posMs - g_app.lastPosMs;
    g_app.lastPosMs = posMs;
    ScrobbleCheck(false);
}

static void UpdatePosition()
{
    // After the last track ends the player thread lingers (IsActive) until the next Play/Stop.
    if (g_app.seekDragging || !g_app.player.IsActive() || !QueueValid() || g_app.queueFinished)
        return;
    const DWORD posMs = g_app.player.PositionMs();
    ScrobbleTrackPosition(posMs);
    int sec = (int)(posMs / 1000);
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
    InvalidateRect(g_app.btnRepeat, NULL, FALSE);
    if (g_app.miniPlayPause)
        InvalidateRect(g_app.miniPlayPause, NULL, FALSE);
    CheckMenuItem(g_app.menu, IDM_SHUFFLE, g_app.shuffle ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuRadioItem(g_app.menu, IDM_REPEAT_OFF, IDM_REPEAT_ONE, IDM_REPEAT_OFF + g_app.repeat, MF_BYCOMMAND);
}

// The stream for a queue entry. Transcoded streams seek on the server (timeOffset, whole
// seconds); original MP3s that Navidrome passes through untouched are skipped client-side by
// frame headers; FLAC originals (lossless option) seek client-side with HTTP Range requests.
static std::wstring TrackUrl(const SubsonicSong& s, DWORD startMs, DWORD& start, DWORD& skip, bool& flac)
{
    flac = g_app.client.StreamsLossless(s);
    const bool serverSide = !flac && g_app.client.ServerTranscodes(s);
    const int offsetSec = (int)(startMs / 1000);
    start = serverSide ? (DWORD)offsetSec * 1000 : startMs;
    skip = serverSide ? 0 : startMs;
    return flac ? g_app.client.OriginalUrl(s.id) : g_app.client.StreamUrl(s.id, serverSide ? offsetSec : 0);
}

// Where the queue goes when the current track ends (the PS_FINISHED rules), or -1.
static int GaplessNextIndex()
{
    if (!QueueValid())
        return -1;
    if (g_app.repeat == REPEAT_ONE)
        return g_app.queueIndex;
    if (g_app.queueIndex + 1 < (int)g_app.queue.size())
        return g_app.queueIndex + 1;
    return g_app.repeat == REPEAT_ALL ? 0 : -1;
}

// Tells the player what to join on to the current track. Called whenever that may change:
// a new track, queue edits, shuffle, repeat, settings.
static void UpdateGaplessNext()
{
    const int i = (g_app.gapless && g_app.player.IsActive() && !g_app.queueFinished) ? GaplessNextIndex() : -1;
    g_app.gaplessIdx = i;
    if (i < 0) {
        g_app.player.ClearNext();
        return;
    }
    const SubsonicSong& s = g_app.queue[i];
    DWORD start, skip;
    bool flac;
    const std::wstring url = TrackUrl(s, 0, start, skip, flac);
    g_app.player.SetNext(url, flac ? CODEC_FLAC : CODEC_MP3, Utf8ToWide(s.id), (DWORD)s.duration * 1000);
}

static const wchar_t* RepeatName(int mode)
{
    return mode == REPEAT_ALL ? L"Repeat all" : (mode == REPEAT_ONE ? L"Repeat one" : L"Repeat off");
}

// Repeat off / all (the whole queue) / one (the current track); remembered in the ini.
static void SetRepeat(int mode)
{
    g_app.repeat = (mode >= REPEAT_OFF && mode <= REPEAT_ONE) ? mode : REPEAT_OFF;
    WritePrivateProfileStringW(L"Player", L"Repeat", Fmt(L"%d", g_app.repeat).c_str(), g_app.iniPath.c_str());
    RefreshTransport();
    UpdateGaplessNext();
    SetStatus(std::wstring(RepeatName(g_app.repeat)) + L".");
}

static int RandomBelow(int n)
{
    return n > 0 ? (int)((((unsigned)rand() << 15) | (unsigned)rand()) % (unsigned)n) : 0;
}

// If the Play Queue page is showing, refills it from the queue and selects 'select'
// (-1: keep the selected row where it is).
static void RefreshQueueView(int select = -1)
{
    if (g_app.songSource != SRC_QUEUE)
        return;
    if (select < 0)
        select = SelectedIndex(g_app.songs);
    const int top = ListView_GetTopIndex(g_app.songs);
    g_app.songData = g_app.queue;
    ListView_SetItemState(g_app.songs, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetItemCountEx(g_app.songs, (int)g_app.songData.size(), LVSICF_NOSCROLL);
    if (select >= (int)g_app.songData.size())
        select = (int)g_app.songData.size() - 1;
    if (top > 0 && top < (int)g_app.songData.size()) {
        // Keep the scroll position: scroll to the bottom, then back up to the old top row.
        ListView_EnsureVisible(g_app.songs, (int)g_app.songData.size() - 1, FALSE);
        ListView_EnsureVisible(g_app.songs, top, FALSE);
    }
    SelectRow(g_app.songs, select);
    InvalidateRect(g_app.songs, NULL, TRUE);
    InvalidateHeader();
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
    RefreshQueueView(g_app.queueIndex);
    UpdateGaplessNext();
}

// After the queue itself is edited, its current order becomes the source, so turning
// shuffle off later keeps the user's order rather than the original album/playlist order.
static void QueueEdited(int select)
{
    g_app.queueSource = g_app.queue;
    g_app.queueSrcIdx.clear();
    for (int i = 0; i < (int)g_app.queue.size(); ++i)
        g_app.queueSrcIdx.push_back(i);
    RefreshQueueView(select);
    UpdateGaplessNext();
}

// Highlights queue entry i if the list on screen holds it at the same position.
static void ShowTrackRow(int i)
{
    int row = g_app.songSource == SRC_QUEUE ? i : g_app.queueSrcIdx[i];
    if (row < (int)g_app.songData.size() && g_app.songData[row].id == g_app.queue[i].id)
        SelectRow(g_app.songs, row);
    InvalidateRect(g_app.songs, NULL, FALSE);   // bold "now playing" row
    if (g_app.songSource == SRC_QUEUE)
        InvalidateHeader();                     // "track N of M"
}

static void StartQueue(int i, DWORD startMs)
{
    if (i < 0 || i >= (int)g_app.queue.size())
        return;
    const SubsonicSong& s = g_app.queue[i];
    g_app.queueIndex = i;
    g_app.queueFinished = false;

    DWORD start, skip;
    bool flac;
    const std::wstring url = TrackUrl(s, startMs, start, skip, flac);
    g_app.playingFlac = flac;

    ResetSeekBar(s.duration);
    if (start) {
        g_app.shownSec = (int)(start / 1000);
        SetSeekPos(g_app.shownSec);
        SetTimeCur(g_app.shownSec);
    }
    ShowTrackRow(i);
    SetStatus(L"Buffering: " + s.title);
    ScrobbleTrackStarted(s.id, startMs);
    g_app.player.Play(url, g_app.client.Options(), start, skip, flac ? CODEC_FLAC : CODEC_MP3,
                      (DWORD)s.duration * 1000);
    UpdateGaplessNext();
    UpdateNowPlaying();
    RefreshTransport();
}

// The player joined the next track with no gap and it's now being heard: move the queue
// on to it (the player tells us which by its song id).
static void AdoptNextTrack(const wchar_t* tag)
{
    ScrobbleCheck(true);
    const std::string id = tag ? WideToUtf8(tag) : std::string();
    const int n = (int)g_app.queue.size();
    int i = g_app.gaplessIdx;
    if (i < 0 || i >= n || g_app.queue[i].id != id) {   // the queue changed meanwhile
        i = -1;
        for (int k = 1; k <= n && i < 0; ++k)
            if (g_app.queue[(g_app.queueIndex + k) % n].id == id)
                i = (g_app.queueIndex + k) % n;
    }
    if (i < 0)
        return;
    const SubsonicSong& s = g_app.queue[i];
    g_app.queueIndex = i;
    g_app.queueFinished = false;
    g_app.playingFlac = g_app.client.StreamsLossless(s);
    ResetSeekBar(s.duration);
    ShowTrackRow(i);
    ScrobbleTrackStarted(s.id, 0);
    SetStatus(L"Playing: " + s.title + L"  -  " + s.artist + (g_app.playingFlac ? L"  (FLAC)" : L""));
    if (g_app.scrobbleEnabled) {
        g_app.nowPlayingSent = true;
        SendScrobble(s.id, false);
    }
    UpdateNowPlaying();
    RefreshTransport();
    UpdateGaplessNext();
}

static void PlayFromSongList(int i)
{
    if (i < 0 || i >= (int)g_app.songData.size())
        return;
    if (g_app.songSource == SRC_QUEUE) {   // the list is the queue: jump within it
        StartQueue(i, 0);
        return;
    }
    g_app.queueSource = g_app.songData;
    BuildQueue(i);
    StartQueue(g_app.queueIndex, 0);
}

// Play next / Add to queue. With nothing queued, or the queue played to the end, the new
// tracks start playing straight away.
static void QueueSongs(const std::vector<SubsonicSong>& songs, bool next)
{
    if (songs.empty())
        return;
    const std::wstring what = songs.size() == 1 ? L"\"" + songs[0].title + L"\""
                                                : Fmt(L"%u tracks", (unsigned)songs.size());
    if (!QueueValid()) {
        g_app.queue = songs;
        g_app.queueIndex = 0;
        QueueEdited(0);
        StartQueue(0, 0);
        return;
    }
    const bool resume = g_app.queueFinished;
    const int at = (next && !resume) ? g_app.queueIndex + 1 : (int)g_app.queue.size();
    g_app.queue.insert(g_app.queue.begin() + at, songs.begin(), songs.end());
    QueueEdited(-1);
    if (resume) {
        StartQueue(at, 0);
        return;
    }
    SetStatus((next ? L"Playing next: " : L"Added to the queue: ") + what);
}

// Fetches an album's or playlist's tracks in the background, then queues them (OnJobDone).
static void QueueFetched(const std::string& albumId, const std::string& playlistId, const std::wstring& name,
                         bool next)
{
    Job* job = NewJob(JOB_QUEUE_FETCH);
    job->addAlbumId = albumId;
    job->plId = playlistId;
    job->flag = next;
    SetStatus(L"Getting the tracks of \"" + name + L"\"...");
    LaunchJob(job);
}

static void RemoveFromQueue(int i)
{
    // The playing track stays; skip it with Next instead.
    if (i < 0 || i >= (int)g_app.queue.size() || i == g_app.queueIndex)
        return;
    const std::wstring title = g_app.queue[i].title;
    g_app.queue.erase(g_app.queue.begin() + i);
    if (i < g_app.queueIndex)
        --g_app.queueIndex;
    QueueEdited(i);
    SetStatus(L"Removed from the queue: \"" + title + L"\"");
}

// Moves queue entry 'from' so it ends up at 'to'; the playing track keeps playing.
static void MoveQueueEntry(int from, int to)
{
    const int n = (int)g_app.queue.size();
    if (from < 0 || to < 0 || from >= n || to >= n || from == to)
        return;
    const SubsonicSong s = g_app.queue[from];
    g_app.queue.erase(g_app.queue.begin() + from);
    g_app.queue.insert(g_app.queue.begin() + to, s);
    int& cur = g_app.queueIndex;
    if (cur == from)
        cur = to;
    else if (from < cur && to >= cur)
        --cur;
    else if (from > cur && to <= cur)
        ++cur;
    QueueEdited(to);
}

static void MoveInQueue(int i, int delta)
{
    MoveQueueEntry(i, i + delta);
}

// Drops everything after the playing track.
static void ClearUpcoming()
{
    if (!QueueValid() || g_app.queueIndex + 1 >= (int)g_app.queue.size())
        return;
    g_app.queue.erase(g_app.queue.begin() + g_app.queueIndex + 1, g_app.queue.end());
    QueueEdited(g_app.queueIndex);
    SetStatus(L"Cleared the upcoming tracks.");
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
    g_app.queueFinished = false;
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
    if ((g_app.mainView == VIEW_TRACKS || g_app.mainView == VIEW_SONGS || g_app.mainView == VIEW_FAVORITES ||
         g_app.mainView == VIEW_QUEUE) && !g_app.songData.empty()) {
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

// Next: the following track, or back to the first with Repeat all. (Repeat one only
// repeats a track that plays to its end; Next still moves on.)
static void PlayNext()
{
    if (!QueueValid())
        return;
    if (g_app.queueIndex + 1 < (int)g_app.queue.size())
        StartQueue(g_app.queueIndex + 1, 0);
    else if (g_app.repeat == REPEAT_ALL)
        StartQueue(0, 0);
}

// Play/pause for the keyboard, media keys and tray: pause or resume, else start the queue
// where it is (from the top once it has finished), else play from the list on screen.
static void PlayPause()
{
    if (g_app.queueFinished && QueueValid())
        StartQueue(0, 0);
    else if (g_app.player.IsActive())
        DoPause();
    else if (QueueValid())
        StartQueue(g_app.queueIndex, 0);
    else
        DoPlay();
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
    case VIEW_SONGS:
    case VIEW_FAVORITES:
    case VIEW_QUEUE:   return g_app.songs;
    default:           return g_app.albums;
    }
}

static void SetView(MainView v)
{
    g_app.mainView = v;
    if (v != VIEW_QUEUE && g_app.songSource == SRC_QUEUE)
        g_app.songSource = SRC_NONE;   // the hidden list no longer follows the queue
    const bool albumList = v == VIEW_ALBUMS || v == VIEW_ARTIST;
    const bool trackList = v == VIEW_TRACKS || v == VIEW_SONGS || v == VIEW_FAVORITES;
    ShowWindow(g_app.albums, albumList ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app.artists, v == VIEW_ARTISTS ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app.songs, trackList || v == VIEW_QUEUE ? SW_SHOW : SW_HIDE);
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

static void LoadFavorites()
{
    if (!TryBeginLoad()) {
        g_app.pendingFavorites = true;   // retried when the running load finishes
        return;
    }
    SetStatus(L"Loading Favorites...");
    LaunchJob(NewJob(JOB_FAVORITES));
}

// Library > Favorites: starred songs, re-read from the server each visit so it's current.
static void ShowFavoritesView()
{
    if (!g_app.showFavorites)
        return;
    g_app.songSource = SRC_FAVORITES;
    g_app.songData.clear();
    g_app.favoritesLoaded = false;
    FillSongs();
    SetView(VIEW_FAVORITES);
    TreeSelectQuietly(g_app.treeFavorites);
    SetFocus(g_app.songs);
    LoadFavorites();
}

// Library > Play Queue: the play order, with the playing track in bold. It follows the queue
// as it changes (RefreshQueueView).
static void ShowQueueView()
{
    g_app.songSource = SRC_QUEUE;
    g_app.songData = g_app.queue;
    FillSongs();
    SetView(VIEW_QUEUE);
    TreeSelectQuietly(g_app.treeQueue);
    SelectRow(g_app.songs, QueueValid() ? g_app.queueIndex : 0);
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
             Fmt(L"%d tracks", tracks), InfoNote(true, ar.id));
}

// After the setting changes: show or drop the description on the page on screen.
static void RefreshShownInfo()
{
    if (g_app.mainView == VIEW_ARTIST)
        ShowArtistInfo();
    else if (g_app.mainView == VIEW_TRACKS && g_app.songSource == SRC_ALBUM)
        ShowAlbumInfo(g_app.shownAlbum);
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
        // Not set up yet (first run): open Settings rather than sending people to the ini.
        InterlockedExchange(&g_app.loadBusy, 0);
        SetStatus(L"Not connected. " + err);
        PostMessageW(g_app.wnd, WM_COMMAND, MAKEWPARAM(IDM_SETTINGS, 0), 0);
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
    g_app.favoritesLoaded = false;
    g_app.pendingFavorites = false;
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
    g_app.queueFinished = false;
    g_app.nowCoverPending.clear();
    g_app.infoCache.clear();
    g_app.infoPending.clear();
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

static void OpenArtist(const SubsonicArtist& ar)
{
    Job* job = NewJob(JOB_ARTIST);
    job->artist = ar;
    job->artId = ar.coverArt;
    job->needArt = !ar.coverArt.empty();   // always try: a cached miss still allows the album fallback
    SetStatus(L"Loading " + ar.name + L"...");
    LaunchJob(job);
}

static void OpenSelectedArtist()
{
    int i = SelectedIndex(g_app.artists);
    if (i < 0 || i >= (int)g_app.artistList.size() || !TryBeginLoad())
        return;
    OpenArtist(g_app.artistList[i]);
}

static void OpenAlbum(const SubsonicAlbum& a, MainView origin)
{
    g_app.albumOrigin = origin;
    Job* job = NewJob(JOB_ALBUM);
    job->album = a;
    job->artId = a.coverArt;
    job->needArt = NeedArt(a.coverArt);
    SetStatus(L"Loading " + a.name + L"...");
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
    OpenAlbum(list[i], g_app.mainView == VIEW_ARTIST ? VIEW_ARTIST : VIEW_ALBUMS);
}

// Right-click a track > Go to album / Go to artist. Uses the library lists loaded at connect;
// a newer album or artist falls back to what the track itself says.
static void GoToAlbum(const SubsonicSong& s)
{
    if (s.albumId.empty())
        return;
    if (!TryBeginLoad()) {
        SetStatus(L"Still loading; try again in a moment.");
        return;
    }
    SubsonicAlbum a;
    a.id = s.albumId;
    a.coverArt = s.coverArt;
    a.name = s.album;
    a.artist = s.artist;
    a.year = a.songCount = 0;
    for (size_t k = 0; k < g_app.albumData.size(); ++k)
        if (g_app.albumData[k].id == s.albumId) {
            a = g_app.albumData[k];
            break;
        }
    OpenAlbum(a, VIEW_ALBUMS);
}

static void GoToArtist(const SubsonicSong& s)
{
    if (s.artistId.empty())
        return;
    if (!TryBeginLoad()) {
        SetStatus(L"Still loading; try again in a moment.");
        return;
    }
    SubsonicArtist ar;
    ar.id = s.artistId;
    ar.name = s.artist;
    ar.albumCount = 0;
    for (size_t k = 0; k < g_app.artistData.size(); ++k)
        if (g_app.artistData[k].id == s.artistId) {
            ar = g_app.artistData[k];
            break;
        }
    OpenArtist(ar);
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

// Moving and removing go by position, so they need the playlist's own order (not sorted).
static bool ShownPlaylistEditable()
{
    return g_app.songSource == SRC_PLAYLIST && g_app.client.CanEdit(g_app.shownPlaylist) && !SongsSorted();
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

// Moves track 'from' of the playlist on screen so it ends up at 'to'.
static void MovePlaylistEntry(int from, int to)
{
    const int n = (int)g_app.songData.size();
    if (!ShownPlaylistEditable() || from < 0 || to < 0 || from >= n || to >= n || from == to)
        return;
    Job* job = NewJob(JOB_PLEDIT);
    job->edit = PL_SETSONGS;
    job->plId = g_app.shownPlaylist.id;
    for (int k = 0; k < n; ++k)
        job->songIds.push_back(g_app.songData[k].id);
    const std::string id = job->songIds[from];
    job->songIds.erase(job->songIds.begin() + from);
    job->songIds.insert(job->songIds.begin() + to, id);
    job->selectAfter = to;
    StartEdit(job, L"Moving \"" + g_app.songData[from].title + L"\"...");
}

static void MoveInPlaylist(int i, int delta)
{
    MovePlaylistEntry(i, i + delta);
}

// --- Drag and drop in the track list (playlists and the Play Queue) ---------------------------

static void DragBegin(int row)
{
    if (g_app.songSource == SRC_PLAYLIST && SongsSorted()) {
        SetStatus(L"The playlist is sorted: click # to restore its order, then drag tracks.");
        return;
    }
    if (row < 0 || !(g_app.songSource == SRC_QUEUE || ShownPlaylistEditable()))
        return;
    g_app.dragging = true;
    g_app.dragFrom = row;
    g_app.dragInsert = -1;
    SetCapture(g_app.wnd);
    SetTimer(g_app.wnd, TIMER_DRAGSCROLL, 80, NULL);
}

// Works out where the track would go for a mouse position in the list.
static void DragUpdate(POINT pt)
{
    if (!g_app.dragging)
        return;
    g_app.dragPt = pt;
    RECT rc, hr;
    GetClientRect(g_app.songs, &rc);
    GetWindowRect(ListView_GetHeader(g_app.songs), &hr);
    const int headerH = hr.bottom - hr.top;
    const int n = (int)g_app.songData.size();
    int ins = -1;
    if (pt.x >= rc.left - 40 && pt.x <= rc.right + 40 && n > 0) {
        RECT last;
        ListView_GetItemRect(g_app.songs, n - 1, &last, LVIR_BOUNDS);
        if (pt.y < headerH) {
            ins = ListView_GetTopIndex(g_app.songs);
        } else if (pt.y >= last.bottom) {
            ins = n;
        } else {
            LVHITTESTINFO ht;
            ZeroMemory(&ht, sizeof(ht));
            ht.pt.x = 4;
            ht.pt.y = pt.y;
            const int r = ListView_HitTest(g_app.songs, &ht);
            if (r >= 0) {
                RECT ir;
                ListView_GetItemRect(g_app.songs, r, &ir, LVIR_BOUNDS);
                ins = pt.y < (ir.top + ir.bottom) / 2 ? r : r + 1;
            }
        }
    }
    SetCursor(LoadCursor(NULL, ins >= 0 ? IDC_ARROW : IDC_NO));
    if (ins != g_app.dragInsert) {
        g_app.dragInsert = ins;
        InvalidateRect(g_app.songs, NULL, FALSE);
    }
}

// Scrolls while the mouse is held above or below the list.
static void DragScroll()
{
    if (!g_app.dragging)
        return;
    RECT rc, hr;
    GetClientRect(g_app.songs, &rc);
    GetWindowRect(ListView_GetHeader(g_app.songs), &hr);
    RECT row;
    const int rowH = ListView_GetItemRect(g_app.songs, 0, &row, LVIR_BOUNDS) ? row.bottom - row.top : 16;
    if (g_app.dragPt.y < hr.bottom - hr.top + 4)
        ListView_Scroll(g_app.songs, 0, -rowH);
    else if (g_app.dragPt.y > rc.bottom - 4)
        ListView_Scroll(g_app.songs, 0, rowH);
    else
        return;
    POINT pt = g_app.dragPt;
    g_app.dragInsert = -2;   // force a recalculation and repaint after scrolling
    DragUpdate(pt);
}

static void DragEnd(bool drop)
{
    if (!g_app.dragging)
        return;
    g_app.dragging = false;   // first: ReleaseCapture() sends WM_CAPTURECHANGED back here
    KillTimer(g_app.wnd, TIMER_DRAGSCROLL);
    if (GetCapture() == g_app.wnd)
        ReleaseCapture();
    const int from = g_app.dragFrom, ins = g_app.dragInsert;
    g_app.dragInsert = -1;
    InvalidateRect(g_app.songs, NULL, FALSE);
    if (!drop || ins < 0)
        return;
    const int to = ins > from ? ins - 1 : ins;
    if (to == from)
        return;
    if (g_app.songSource == SRC_QUEUE)
        MoveQueueEntry(from, to);
    else
        MovePlaylistEntry(from, to);
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

// The edit box wants CRLF; the server stores what it's given, so send plain LF.
static std::wstring ReplaceAll(std::wstring s, const std::wstring& from, const std::wstring& to)
{
    for (size_t at = s.find(from); at != std::wstring::npos; at = s.find(from, at + to.size()))
        s.replace(at, from.size(), to);
    return s;
}

static std::wstring Trimmed(const std::wstring& s, const wchar_t* space)
{
    size_t a = s.find_first_not_of(space), b = s.find_last_not_of(space);
    return a == std::wstring::npos ? std::wstring() : s.substr(a, b - a + 1);
}

struct PlaylistDlgData {
    std::wstring name, comment;
};

static std::wstring DlgFullText(HWND dlg, int id)
{
    const int n = GetWindowTextLengthW(GetDlgItem(dlg, id));
    std::vector<wchar_t> buf(n + 1);
    GetDlgItemTextW(dlg, id, &buf[0], n + 1);
    return std::wstring(&buf[0]);
}

static INT_PTR CALLBACK PlaylistDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG: {
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        PlaylistDlgData* d = (PlaylistDlgData*)lp;
        SendDlgItemMessageW(dlg, IDC_NAME_EDIT, EM_LIMITTEXT, 200, 0);
        SendDlgItemMessageW(dlg, IDC_PL_COMMENT, EM_LIMITTEXT, 2000, 0);
        SetDlgItemTextW(dlg, IDC_NAME_EDIT, d->name.c_str());
        SetDlgItemTextW(dlg, IDC_PL_COMMENT, ReplaceAll(d->comment, L"\n", L"\r\n").c_str());
        SendDlgItemMessageW(dlg, IDC_NAME_EDIT, EM_SETSEL, 0, -1);
        SetFocus(GetDlgItem(dlg, IDC_NAME_EDIT));
        return FALSE;   // focus set by hand
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            std::wstring name = Trimmed(DlgFullText(dlg, IDC_NAME_EDIT), L" \t");
            if (name.empty()) {
                MessageBeep(MB_ICONEXCLAMATION);
                SetFocus(GetDlgItem(dlg, IDC_NAME_EDIT));
                return TRUE;
            }
            PlaylistDlgData* d = (PlaylistDlgData*)GetWindowLongPtrW(dlg, DWLP_USER);
            d->name = name;
            d->comment = Trimmed(ReplaceAll(DlgFullText(dlg, IDC_PL_COMMENT), L"\r\n", L"\n"), L" \t\r\n");
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

static void EditPlaylist(int plIndex)
{
    if (plIndex < 0 || plIndex >= (int)g_app.playlistData.size())
        return;
    const SubsonicPlaylist& p = g_app.playlistData[plIndex];
    if (!g_app.client.CanEdit(p)) {
        SetStatus(L"\"" + p.name + L"\" is read-only.");
        return;
    }
    PlaylistDlgData d;
    d.name = p.name;
    d.comment = p.comment;
    if (DialogBoxParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_PLAYLIST), g_app.wnd, PlaylistDlgProc,
                        (LPARAM)&d) != IDOK)
        return;
    if (d.name == p.name && d.comment == p.comment)
        return;
    Job* job = NewJob(JOB_PLEDIT);
    job->edit = PL_RENAME;
    job->plId = p.id;
    job->name = d.name;
    job->comment = d.comment;
    StartEdit(job, L"Saving \"" + d.name + L"\"...");
}

// Playlist menu: the playlist on screen.
static void EditShownPlaylist()
{
    if (g_app.songSource == SRC_PLAYLIST)
        for (size_t i = 0; i < g_app.playlistData.size(); ++i)
            if (g_app.playlistData[i].id == g_app.shownPlaylist.id) {
                EditPlaylist((int)i);
                return;
            }
    SetStatus(L"Open a playlist first, or right-click one in the sidebar.");
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
                // Navidrome gives the playlist a new cover id whenever it changes.
                if (NeedArt(g_app.shownPlaylist.coverArt)) {
                    Job* art = NewJob(JOB_ART);
                    art->artId = g_app.shownPlaylist.coverArt;
                    art->needArt = true;
                    LaunchJob(art);
                }
            }
    }
    RebuildPlaylistTree();

    if (!job->ok) {
        SetStatus(L"The playlist change failed.");
        MessageBoxW(g_app.wnd, job->message.c_str(), APP_TITLE, MB_ICONERROR);
        return;
    }
    static const wchar_t* kDone[] = { L"Playlist created.", L"Playlist details saved.", L"Added to playlist.",
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

    case JOB_FAVORITES:
        if (job->ok) {
            if (g_app.mainView == VIEW_FAVORITES) {
                g_app.songData.swap(job->songs);
                g_app.favoritesLoaded = true;
                FillSongs();
                SelectRow(g_app.songs, 0);
                InvalidateHeader();
            }
            SetStatus(L"Favorites loaded.");
        } else {
            SetStatus(L"Could not load Favorites.");
            MessageBoxW(g_app.wnd, job->message.c_str(), APP_TITLE, MB_ICONERROR);
        }
        break;

    case JOB_STAR:
        if (!job->ok) {
            ApplyFavoriteLocal(job->songId, job->oldFlag);
            SetStatus(L"The favorite was not saved.");
            MessageBoxW(g_app.wnd, job->message.c_str(), APP_TITLE, MB_ICONERROR);
        }
        break;

    case JOB_SCROBBLE:
        // Best effort: a failed scrobble is logged, not worth interrupting playback for.
        if (!job->ok)
            LogF(L"Scrobble (%ls) failed: %ls", job->flag ? L"play" : L"now playing", job->message.c_str());
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

    case JOB_QUEUE_FETCH:
        if (job->ok && !job->songs.empty()) {
            QueueSongs(job->songs, job->flag);
        } else if (job->ok) {
            SetStatus(L"Nothing to add: the list is empty.");
        } else {
            SetStatus(L"Could not get the tracks.");
            MessageBoxW(g_app.wnd, job->message.c_str(), APP_TITLE, MB_ICONERROR);
        }
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

    case JOB_INFO: {
        const std::string key = InfoKey(job->flag, job->songId);
        if (g_app.infoPending == key)
            g_app.infoPending.clear();
        if (!job->ok) {
            LogF(L"Last.fm description: %ls", job->message.c_str());   // tried again next visit
            break;
        }
        g_app.infoCache[key] = job->notes;
        if (g_app.lastfmInfo && g_app.shown.has && ShownInfoKey() == key) {
            g_app.shown.note = job->notes;
            InvalidateHeader();
        }
        break;
    }
    }
    delete job;

    if (g_app.pendingLoad) {
        g_app.pendingLoad = false;
        if (g_app.pendingPlaylist >= 0)
            OpenPlaylist(g_app.pendingPlaylist);
        else
            OpenSelectedAlbum();
    } else if (g_app.pendingFavorites && g_app.mainView == VIEW_FAVORITES) {
        g_app.pendingFavorites = false;
        LoadFavorites();
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
            SetStatus(L"Playing: " + title + L"  -  " + g_app.queue[g_app.queueIndex].artist +
                      (g_app.playingFlac ? L"  (FLAC)" : L""));
            if (g_app.scrobbleEnabled && !g_app.nowPlayingSent) {
                g_app.nowPlayingSent = true;
                SendScrobble(g_app.queue[g_app.queueIndex].id, false);
            }
            break;
        case PS_NEXT_TRACK:
            AdoptNextTrack(text);
            break;
        case PS_FINISHED:
            ScrobbleCheck(true);
            if (g_app.repeat == REPEAT_ONE) {
                StartQueue(g_app.queueIndex, 0);
            } else if (g_app.queueIndex + 1 < (int)g_app.queue.size()) {
                StartQueue(g_app.queueIndex + 1, 0);
            } else if (g_app.repeat == REPEAT_ALL) {
                StartQueue(0, 0);
            } else {
                g_app.queueFinished = true;
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

    const bool inQueue = g_app.songSource == SRC_QUEUE;
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, CMD_PLAY, L"&Play");
    if (!inQueue) {
        AppendMenuW(m, MF_STRING, CMD_PLAY_NEXT, L"Play &next");
        AppendMenuW(m, MF_STRING, CMD_ADD_QUEUE, L"Add to &queue");
    }
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    const bool onAlbum = g_app.songSource == SRC_ALBUM && g_app.shownAlbum.id == s.albumId;
    AppendMenuW(m, MF_STRING | (!s.albumId.empty() && !onAlbum ? 0 : MF_GRAYED), CMD_GO_ALBUM, L"Go to a&lbum");
    AppendMenuW(m, MF_STRING | (!s.artistId.empty() ? 0 : MF_GRAYED), CMD_GO_ARTIST, L"Go to ar&tist");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    if (g_app.showRatings)
        AppendMenuW(m, MF_POPUP, (UINT_PTR)rate, L"&Rate");
    else
        DestroyMenu(rate);
    if (g_app.showFavorites)
        AppendMenuW(m, MF_STRING, CMD_FAV, s.starred ? L"Remove from &Favorites" : L"Add to &Favorites");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)BuildAddToMenu(), L"&Add to playlist");
    if (g_app.songSource == SRC_PLAYLIST) {
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING | (editable && i > 0 ? 0 : MF_GRAYED), CMD_MOVE_UP, L"Move &up");
        AppendMenuW(m, MF_STRING | (editable && i + 1 < (int)g_app.songData.size() ? 0 : MF_GRAYED),
                    CMD_MOVE_DOWN, L"Move &down");
        AppendMenuW(m, MF_STRING | (editable ? 0 : MF_GRAYED), CMD_REMOVE,
                    SongsSorted() ? L"Remo&ve from playlist (click # to unsort first)" : L"Remo&ve from playlist\tDel");
    } else if (inQueue) {
        const int n = (int)g_app.queue.size();
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING | (i > 0 ? 0 : MF_GRAYED), CMD_MOVE_UP, L"Move &up");
        AppendMenuW(m, MF_STRING | (i + 1 < n ? 0 : MF_GRAYED), CMD_MOVE_DOWN, L"Move &down");
        AppendMenuW(m, MF_STRING | (i != g_app.queueIndex ? 0 : MF_GRAYED), CMD_REMOVE,
                    L"Remo&ve from queue\tDel");
        AppendMenuW(m, MF_STRING | (QueueValid() && g_app.queueIndex + 1 < n ? 0 : MF_GRAYED),
                    CMD_CLEAR_UPCOMING, L"&Clear upcoming tracks");
    }

    POINT pt = ListMenuPoint(g_app.songs, lp, i);
    UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_app.wnd, NULL);
    DestroyMenu(m);   // destroys the submenus too

    std::vector<std::string> one(1, s.id);
    if (cmd == CMD_PLAY)
        PlayFromSongList(i);
    else if (cmd == CMD_GO_ALBUM)
        GoToAlbum(s);
    else if (cmd == CMD_GO_ARTIST)
        GoToArtist(s);
    else if (cmd == CMD_PLAY_NEXT || cmd == CMD_ADD_QUEUE)
        QueueSongs(std::vector<SubsonicSong>(1, s), cmd == CMD_PLAY_NEXT);
    else if (inQueue && cmd == CMD_MOVE_UP)
        MoveInQueue(i, -1);
    else if (inQueue && cmd == CMD_MOVE_DOWN)
        MoveInQueue(i, +1);
    else if (inQueue && cmd == CMD_REMOVE)
        RemoveFromQueue(i);
    else if (cmd == CMD_CLEAR_UPCOMING)
        ClearUpcoming();
    else if (cmd == CMD_FAV)
        ToggleFavorite(i);
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
    AppendMenuW(m, MF_STRING, CMD_PLAY_NEXT, L"Play &next");
    AppendMenuW(m, MF_STRING, CMD_ADD_QUEUE, L"Add to &queue");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)BuildAddToMenu(), L"&Add album to playlist");
    POINT pt = ListMenuPoint(g_app.albums, lp, i);
    UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_app.wnd, NULL);
    DestroyMenu(m);

    const std::string albumId = list[i].id;
    const std::wstring albumName = list[i].name;
    if (cmd == CMD_PLAY)
        OpenSelectedAlbum();
    else if (cmd == CMD_PLAY_NEXT || cmd == CMD_ADD_QUEUE)
        QueueFetched(albumId, std::string(), albumName, cmd == CMD_PLAY_NEXT);
    else if (cmd == CMD_ADD_NEW)
        NewPlaylist(std::vector<std::string>(), albumId, albumName);
    else if (cmd >= CMD_ADD_BASE)
        AddToPlaylist(cmd - CMD_ADD_BASE, std::vector<std::string>(), albumId);
}

static void OnCommand(int id);

// Right-click on the notification-area icon.
static void TrayContextMenu()
{
    const bool active = g_app.player.IsActive() && !g_app.queueFinished;
    const bool hidden = !IsWindowVisible(g_app.wnd) && !(g_app.mini && IsWindowVisible(g_app.mini));
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_SHOW, hidden ? L"&Show Navi2K Player" : L"&Bring to front");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (active || QueueValid() ? 0 : MF_GRAYED), IDM_PLAYPAUSE,
                !active ? L"&Play" : (g_app.player.IsPaused() ? L"&Resume" : L"P&ause"));
    AppendMenuW(m, MF_STRING | (active ? 0 : MF_GRAYED), IDM_STOP, L"S&top");
    AppendMenuW(m, MF_STRING | (QueueValid() ? 0 : MF_GRAYED), IDM_PREV, L"Pre&vious");
    const bool canNext = QueueValid() && (g_app.queueIndex + 1 < (int)g_app.queue.size() || g_app.repeat == REPEAT_ALL);
    AppendMenuW(m, MF_STRING | (canNext ? 0 : MF_GRAYED), IDM_NEXT, L"&Next");
    AppendMenuW(m, MF_STRING | (g_app.shuffle ? MF_CHECKED : 0), IDM_SHUFFLE, L"S&huffle");
    HMENU rep = CreatePopupMenu();
    AppendMenuW(rep, MF_STRING, IDM_REPEAT_OFF, L"&Off");
    AppendMenuW(rep, MF_STRING, IDM_REPEAT_ALL, L"&All");
    AppendMenuW(rep, MF_STRING, IDM_REPEAT_ONE, L"O&ne");
    CheckMenuRadioItem(rep, IDM_REPEAT_OFF, IDM_REPEAT_ONE, IDM_REPEAT_OFF + g_app.repeat, MF_BYCOMMAND);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)rep, L"&Repeat");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"E&xit");
    SetMenuDefaultItem(m, IDM_SHOW, FALSE);

    POINT pt;
    GetCursorPos(&pt);
    // Without the foreground window the menu wouldn't close when clicking elsewhere (KB135788).
    SetForegroundWindow(g_app.wnd);
    UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_app.wnd, NULL);
    PostMessageW(g_app.wnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    if (cmd)
        OnCommand((int)cmd);
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
        AppendMenuW(m, MF_STRING, CMD_PLAY_NEXT, L"Play ne&xt");
        AppendMenuW(m, MF_STRING, CMD_ADD_QUEUE, L"Add to &queue");
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    }
    AppendMenuW(m, MF_STRING, CMD_PL_NEW, L"&New playlist...");
    if (isPlaylist) {
        AppendMenuW(m, MF_STRING | g, CMD_PL_RENAME, L"&Edit name and description...");
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
    } else if (cmd == CMD_PLAY_NEXT || cmd == CMD_ADD_QUEUE) {
        QueueFetched(std::string(), g_app.playlistData[pl].id, g_app.playlistData[pl].name, cmd == CMD_PLAY_NEXT);
    } else if (cmd == CMD_PL_NEW) {
        NewPlaylist(std::vector<std::string>(), std::string(), L"New playlist");
    } else if (cmd == CMD_PL_RENAME) {
        EditPlaylist(pl);
    } else if (cmd == CMD_PL_DELETE) {
        DeletePlaylist(pl);
    }
}

// --- Drawn icons ------------------------------------------------------------------------

enum Glyph { GLYPH_NONE, GLYPH_PLAY, GLYPH_PAUSE, GLYPH_STOP, GLYPH_SHUFFLE, GLYPH_PREV, GLYPH_NEXT,
             GLYPH_REPEAT, GLYPH_REPEAT_ONE };

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
    case GLYPH_REPEAT:
    case GLYPH_REPEAT_ONE: {
        // A loop (16 x 11 px): the top runs right into an arrowhead, the bottom runs left into
        // one. Repeat one adds a small "1" inside.
        const int x0 = cx - 8, x1 = cx + 8, y0 = cy - 5, y1 = cy + 5;
        HPEN pen = CreatePen(PS_SOLID, 1, c);
        HGDIOBJ op = SelectObject(dc, pen);
        POINT top[4] = { { x0, y1 - 3 }, { x0, y0 + 2 }, { x0 + 2, y0 }, { x1 - 4, y0 } };
        POINT bot[4] = { { x1, y0 + 3 }, { x1, y1 - 2 }, { x1 - 2, y1 }, { x0 + 4, y1 } };
        Polyline(dc, top, 4);
        Polyline(dc, bot, 4);
        // Second pass one pixel inside thickens the strokes.
        POINT top2[4] = { { x0 + 1, y1 - 3 }, { x0 + 1, y0 + 2 }, { x0 + 2, y0 + 1 }, { x1 - 4, y0 + 1 } };
        POINT bot2[4] = { { x1 - 1, y0 + 3 }, { x1 - 1, y1 - 2 }, { x1 - 2, y1 - 1 }, { x0 + 4, y1 - 1 } };
        Polyline(dc, top2, 4);
        Polyline(dc, bot2, 4);
        SelectObject(dc, op);
        DeleteObject(pen);
        POINT ra[3] = { { x1, y0 }, { x1 - 3, y0 - 3 }, { x1 - 3, y0 + 3 } };
        POINT la[3] = { { x0, y1 }, { x0 + 3, y1 - 3 }, { x0 + 3, y1 + 3 } };
        FillPoly(dc, ra, 3, c);
        FillPoly(dc, la, 3, c);
        if (g == GLYPH_REPEAT_ONE) {   // "1": a 2 px stem with a flag, rows cy-2..cy+2
            FillBox(dc, cx, cy - 2, cx + 2, cy + 3, c);
            FillBox(dc, cx - 1, cy - 1, cx, cy, c);
        }
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
    case IDC_REPEAT:
        glyph = g_app.repeat == REPEAT_ONE ? GLYPH_REPEAT_ONE : GLYPH_REPEAT;
        latched = g_app.repeat != REPEAT_OFF;
        break;
    case IDC_HDR_PLAY:    glyph = GLYPH_PLAY; caption = L"Play"; break;
    case IDC_HDR_SHUFFLE: glyph = GLYPH_SHUFFLE; caption = L"Shuffle"; break;
    case IDC_PREV:
    case IDC_MINI_PREV:   glyph = GLYPH_PREV; break;
    case IDC_MINI_STOP:   glyph = GLYPH_STOP; break;
    case IDC_NEXT:
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

// Word-wrapped text in whole lines between top and bottom, with "..." if it doesn't all fit.
static void DrawNote(HDC dc, const std::wstring& note, int left, int top, int right, int bottom, int lineH)
{
    const int lines = (bottom - top) / lineH;
    if (lines < 1 || right <= left)
        return;
    const wchar_t* more = L"... (click for more)";
    RECT r = { left, top, right, top + lines * lineH };
    const UINT fmt = DT_LEFT | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX;
    std::wstring s = note;
    RECT calc = r;
    DrawTextW(dc, s.c_str(), -1, &calc, fmt | DT_CALCRECT);
    if (calc.bottom > r.bottom) {
        // Longest prefix that still fits once "..." is added.
        size_t lo = 0, hi = note.size();
        while (lo < hi) {
            const size_t mid = (lo + hi + 1) / 2;
            const std::wstring tryS = note.substr(0, mid) + more;
            calc = r;
            DrawTextW(dc, tryS.c_str(), -1, &calc, fmt | DT_CALCRECT);
            if (calc.bottom <= r.bottom)
                lo = mid;
            else
                hi = mid - 1;
        }
        size_t cut = lo;
        while (cut > 0 && (note[cut - 1] == L' ' || note[cut - 1] == L'\n' || note[cut - 1] == L'\r'))
            --cut;
        s = note.substr(0, cut) + more;
        g_app.noteCut = true;
    }
    SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
    DrawTextW(dc, s.c_str(), -1, &r, fmt);
    g_app.noteRect = r;
}

static void PaintHeader(HDC dc)
{
    RECT h = g_app.headerRect;
    SetBkMode(dc, TRANSPARENT);
    const COLORREF text = GetSysColor(COLOR_BTNTEXT);

    int textX = h.left + 4;
    std::wstring title, line2, line3, note;
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
    } else if (g_app.mainView == VIEW_FAVORITES) {
        title = L"Favorites";
        if (!g_app.favoritesLoaded)
            line2 = L"Loading...";
        else if (g_app.songData.empty())
            line2 = L"No favorites yet: click a track's heart to add it.";
        else
            line2 = Fmt(L"%u song%ls  \x00B7  %ls", (unsigned)g_app.songData.size(),
                        g_app.songData.size() == 1 ? L"" : L"s", FormatTime(TotalDuration(g_app.songData)).c_str());
    } else if (g_app.mainView == VIEW_QUEUE) {
        title = L"Play Queue";
        if (g_app.queue.empty())
            line2 = L"Empty: right-click a track and choose Play next or Add to queue.";
        else
            line2 = Fmt(L"Track %d of %u  \x00B7  %ls", (QueueValid() ? g_app.queueIndex : 0) + 1,
                        (unsigned)g_app.queue.size(), FormatTime(TotalDuration(g_app.queue)).c_str());
    } else if (g_app.shown.has) {
        DrawCover(dc, g_app.artRect, g_app.shownArt, L"No cover");
        textX = g_app.artRect.right + 14;
        title = g_app.shown.title;
        line2 = g_app.shown.line2;
        line3 = g_app.shown.line3;
        note = g_app.shown.note;
    }

    // Section headers (Albums/Artists/Songs) are compact: title at the top, count right under it.
    const bool compact = !g_app.shown.has || g_app.mainView == VIEW_ALBUMS || g_app.mainView == VIEW_ARTISTS ||
                         g_app.mainView == VIEW_SONGS || g_app.mainView == VIEW_FAVORITES ||
                         g_app.mainView == VIEW_QUEUE;
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
    SetRectEmpty(&g_app.noteRect);
    g_app.noteCut = false;
    if (!note.empty() && !compact) {
        // Stops above the Play/Shuffle buttons; artist pages have none.
        const int noteBottom = h.top + kArtSize - (g_app.mainView == VIEW_ARTIST ? 0 : 26 + 8);
        DrawNote(dc, note, t.left, t.bottom + 10, h.right, noteBottom, tm.tmHeight);
    }
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
static const ColSpec kSongCols[]   = { { 32, 0, 0 }, { 0, 5, 120 }, { 0, 3, 90 }, { 24, 0, 0 }, { 80, 0, 0 },
                                       { 48, 0, 0 }, { 0, 4, 90 } };

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
    MoveWindow(g_app.btnPrev, x, by, bw, bh, TRUE);
    x += bw + 2;
    MoveWindow(g_app.btnPlay, x, by, bw, bh, TRUE);
    x += bw + 2;
    MoveWindow(g_app.btnPause, x, by, bw, bh, TRUE);
    x += bw + 2;
    MoveWindow(g_app.btnStop, x, by, bw, bh, TRUE);
    x += bw + 2;
    MoveWindow(g_app.btnNext, x, by, bw, bh, TRUE);
    x += bw + 10;
    MoveWindow(g_app.btnShuffle, x, by, bw, bh, TRUE);
    x += bw + 2;
    MoveWindow(g_app.btnRepeat, x, by, bw, bh, TRUE);
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
    ColSpec songCols[sizeof(kSongCols) / sizeof(kSongCols[0])];
    memcpy(songCols, kSongCols, sizeof(songCols));
    const ColSpec hidden = { 0, 0, 0 };   // flexible with no share: width 0
    if (!g_app.showFavorites)
        songCols[COL_FAV] = hidden;
    if (!g_app.showRatings)
        songCols[COL_RATING] = hidden;
    FitColumns(g_app.songs, songCols, sizeof(songCols) / sizeof(songCols[0]));
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
    AppendMenuW(file, MF_STRING, IDM_SETTINGS, L"&Settings...");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, IDM_EXIT, L"E&xit");

    HMENU view = CreatePopupMenu();
    AppendMenuW(view, MF_STRING, IDM_ALBUMS, L"&Albums");
    AppendMenuW(view, MF_STRING, IDM_ARTISTS, L"A&rtists");
    AppendMenuW(view, MF_STRING, IDM_SONGS, L"&Songs");
    AppendMenuW(view, MF_STRING, IDM_FAVORITES, L"Fa&vorites");
    AppendMenuW(view, MF_STRING, IDM_QUEUE, L"Play &Queue\tCtrl+Q");
    AppendMenuW(view, MF_SEPARATOR, 0, NULL);
    AppendMenuW(view, MF_STRING, IDM_FIND, L"&Find\tCtrl+F");
    AppendMenuW(view, MF_STRING, IDM_MINI, L"&Mini Player\tCtrl+M");

    // Shortcuts follow Windows Media Player's (Ctrl+P, Ctrl+S, Ctrl+B, Ctrl+H, Ctrl+T).
    HMENU playback = CreatePopupMenu();
    AppendMenuW(playback, MF_STRING, IDM_PLAY, L"&Play");
    AppendMenuW(playback, MF_STRING, IDM_PLAYPAUSE, L"P&ause / Resume\tCtrl+P");
    AppendMenuW(playback, MF_STRING, IDM_STOP, L"&Stop\tCtrl+S");
    AppendMenuW(playback, MF_STRING, IDM_PREV, L"Pre&vious\tCtrl+B");
    AppendMenuW(playback, MF_STRING, IDM_NEXT, L"&Next\tCtrl+N");
    AppendMenuW(playback, MF_SEPARATOR, 0, NULL);
    AppendMenuW(playback, MF_STRING, IDM_SHUFFLE, L"S&huffle\tCtrl+H");
    AppendMenuW(playback, MF_STRING, IDM_REPEAT_OFF, L"&Repeat off");
    AppendMenuW(playback, MF_STRING, IDM_REPEAT_ALL, L"Repeat a&ll");
    AppendMenuW(playback, MF_STRING, IDM_REPEAT_ONE, L"Repeat &one");
    AppendMenuW(playback, MF_STRING, IDM_REPEAT_CYCLE, L"Change repea&t mode\tCtrl+T");

    HMENU playlist = CreatePopupMenu();
    AppendMenuW(playlist, MF_STRING, IDM_NEW_PLAYLIST, L"&New Playlist...");
    AppendMenuW(playlist, MF_STRING, IDM_EDIT_PLAYLIST, L"&Edit Name and Description...");

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

// --- Settings (File > Settings) -----------------------------------------------------------
// A two-page property sheet. Pages are created when first shown, so only the pages the user
// visited get PSN_APPLY; each saves its own settings.

struct SettingsState {
    SubsonicConfig cfg;      // as saved
    PlaybackSettings pb;
    bool showFavorites, showRatings;
    bool trayIcon, trayMinimize;
    bool mediaKeys;
    bool lastfmInfo;
    bool serverChanged;      // reconnect afterwards
    bool playbackChanged;    // apply to the player afterwards
    bool displayChanged;
};
static SettingsState* g_settings;

static const int kQualities[] = { 96, 128, 160, 192, 256, 320 };
static const int kLosslessChoice = sizeof(kQualities) / sizeof(kQualities[0]);   // last entry
static const struct { int buffers; const wchar_t* label; } kBufferChoices[] = {
    { 8,  L"Short (0.4 s): quickest volume and seeking" },
    { 12, L"Normal (0.6 s)" },
    { 24, L"Long (1.1 s): for a slow network" },
    { 32, L"Extra long (1.5 s)" },
};

static UINT ResolveDevice(const std::wstring& name)
{
    if (name.empty())
        return WAVE_MAPPER;
    UINT n = waveOutGetNumDevs();
    for (UINT i = 0; i < n; ++i) {
        WAVEOUTCAPSW caps;
        if (waveOutGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR && name == caps.szPname)
            return i;
    }
    return WAVE_MAPPER;   // device gone: use the default
}

static void ApplyPlayback(const PlaybackSettings& pb)
{
    g_app.player.SetOutput(ResolveDevice(pb.device), pb.bufferCount);
    g_app.scrobbleEnabled = pb.scrobble;
    g_app.gapless = pb.gapless;
    UpdateGaplessNext();
}

static std::wstring DlgText(HWND dlg, int id)
{
    wchar_t buf[512];
    GetDlgItemTextW(dlg, id, buf, 512);
    std::wstring s = buf;
    size_t a = s.find_first_not_of(L" \t"), b = s.find_last_not_of(L" \t");
    return a == std::wstring::npos ? std::wstring() : s.substr(a, b - a + 1);
}

static bool IsHttpUrl(const std::wstring& url)
{
    return _wcsnicmp(url.c_str(), L"http://", 7) == 0 || _wcsnicmp(url.c_str(), L"https://", 8) == 0;
}

static void SetPasswordHint(HWND dlg)
{
    SetDlgItemTextW(dlg, IDC_SET_PWHINT, g_settings->cfg.token.empty()
                                             ? L"Required."
                                             : L"A password is saved. Leave blank to keep it.");
}

// Reads the Server page into cfg/password; false (with a message) if something's missing.
static bool ReadServerPage(HWND dlg, SubsonicConfig& cfg, std::wstring& password, bool requireLogin)
{
    cfg = g_settings->cfg;
    cfg.url = DlgText(dlg, IDC_SET_URL);
    cfg.user = WideToUtf8(DlgText(dlg, IDC_SET_USER));
    cfg.lanUrl = DlgText(dlg, IDC_SET_LANURL);
    cfg.allowHttpFallback = IsDlgButtonChecked(dlg, IDC_SET_FALLBACK) == BST_CHECKED;
    cfg.allowUntrustedCert = IsDlgButtonChecked(dlg, IDC_SET_UNTRUSTED) == BST_CHECKED;
    wchar_t pw[256];
    GetDlgItemTextW(dlg, IDC_SET_PASSWORD, pw, 256);   // not trimmed: spaces may be part of it
    password = pw;
    SecureZeroMemory(pw, sizeof(pw));

    const wchar_t* problem = NULL;
    int field = 0;
    if (!IsHttpUrl(cfg.url)) {
        problem = L"Enter the server address, starting with http:// (or https://).";
        field = IDC_SET_URL;
    } else if (cfg.user.empty()) {
        problem = L"Enter your user name.";
        field = IDC_SET_USER;
    } else if (requireLogin && password.empty() && cfg.token.empty()) {
        problem = L"Enter your password.";
        field = IDC_SET_PASSWORD;
    } else if (cfg.allowHttpFallback && !IsHttpUrl(cfg.lanUrl)) {
        problem = L"Enter the LAN address (starting with http://), or untick the fallback.";
        field = IDC_SET_LANURL;
    }
    if (problem) {
        MessageBoxW(dlg, problem, L"Settings", MB_ICONEXCLAMATION);
        SetFocus(GetDlgItem(dlg, field));
        return false;
    }
    return true;
}

struct TestRequest {
    HWND page;
    SubsonicConfig cfg;
    std::wstring password;
};

// Test Connection runs off the UI thread with its own client; the result is posted back.
static unsigned __stdcall TestConnectionThread(void* p)
{
    TestRequest* t = (TestRequest*)p;
    bool ok = false;
    std::wstring msg;
    if (!t->password.empty() && !SubsonicMakeToken(WideToUtf8(t->password), t->cfg.token, t->cfg.salt)) {
        msg = L"Could not hash the password.";
    } else {
        SubsonicClient client;
        client.Configure(t->cfg);
        std::wstring notice;
        ok = client.Connect(notice);
        msg = ok ? (notice.empty() ? std::wstring(L"Connected: the address and login work.") : L"Connected. " + notice)
                 : notice;
    }
    wchar_t* copy = _wcsdup(msg.c_str());
    if (!PostMessageW(t->page, WM_APP_TEST_DONE, ok, (LPARAM)copy))
        free(copy);   // the dialog was closed meanwhile
    delete t;
    InterlockedDecrement(&g_app.jobsRunning);
    return 0;
}

static INT_PTR CALLBACK ServerPageProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG: {
        const SubsonicConfig& c = g_settings->cfg;
        SendDlgItemMessageW(dlg, IDC_SET_URL, EM_LIMITTEXT, 300, 0);
        SendDlgItemMessageW(dlg, IDC_SET_USER, EM_LIMITTEXT, 100, 0);
        SendDlgItemMessageW(dlg, IDC_SET_PASSWORD, EM_LIMITTEXT, 200, 0);
        SendDlgItemMessageW(dlg, IDC_SET_LANURL, EM_LIMITTEXT, 300, 0);
        SetDlgItemTextW(dlg, IDC_SET_URL, c.url.c_str());
        SetDlgItemTextW(dlg, IDC_SET_USER, Utf8ToWide(c.user).c_str());
        SetDlgItemTextW(dlg, IDC_SET_LANURL, c.lanUrl.c_str());
        CheckDlgButton(dlg, IDC_SET_FALLBACK, c.allowHttpFallback ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_SET_UNTRUSTED, c.allowUntrustedCert ? BST_CHECKED : BST_UNCHECKED);
        EnableWindow(GetDlgItem(dlg, IDC_SET_LANURL), c.allowHttpFallback);
        SetPasswordHint(dlg);
        return TRUE;
    }

    case WM_COMMAND:
        if (LOWORD(wp) == IDC_SET_FALLBACK && HIWORD(wp) == BN_CLICKED) {
            EnableWindow(GetDlgItem(dlg, IDC_SET_LANURL), IsDlgButtonChecked(dlg, IDC_SET_FALLBACK) == BST_CHECKED);
        } else if (LOWORD(wp) == IDC_SET_TEST && HIWORD(wp) == BN_CLICKED) {
            TestRequest* t = new TestRequest;
            if (!ReadServerPage(dlg, t->cfg, t->password, true)) {
                delete t;
                return TRUE;
            }
            t->page = dlg;
            EnableWindow(GetDlgItem(dlg, IDC_SET_TEST), FALSE);
            SetDlgItemTextW(dlg, IDC_SET_TESTRESULT, L"Connecting...");
            InterlockedIncrement(&g_app.jobsRunning);
            HANDLE th = (HANDLE)_beginthreadex(NULL, 0, TestConnectionThread, t, 0, NULL);
            if (th) {
                CloseHandle(th);
            } else {
                InterlockedDecrement(&g_app.jobsRunning);
                delete t;
                EnableWindow(GetDlgItem(dlg, IDC_SET_TEST), TRUE);
                SetDlgItemTextW(dlg, IDC_SET_TESTRESULT, L"Could not start the test.");
            }
        }
        return TRUE;

    case WM_APP_TEST_DONE: {
        wchar_t* text = (wchar_t*)lp;
        EnableWindow(GetDlgItem(dlg, IDC_SET_TEST), TRUE);
        if (wp) {
            SetDlgItemTextW(dlg, IDC_SET_TESTRESULT, text);
        } else {
            // Failures (TLS, login, ...) come with several lines of advice.
            SetDlgItemTextW(dlg, IDC_SET_TESTRESULT, L"The test failed (details shown).");
            MessageBoxW(dlg, text, L"Test Connection", MB_ICONERROR);
        }
        free(text);
        return TRUE;
    }

    case WM_NOTIFY:
        if (((NMHDR*)lp)->code == PSN_APPLY) {
            SubsonicConfig cfg;
            std::wstring password, err;
            if (!ReadServerPage(dlg, cfg, password, true)) {
                SetWindowLongPtrW(dlg, DWLP_MSGRESULT, PSNRET_INVALID);
                return TRUE;
            }
            const SubsonicConfig& old = g_settings->cfg;
            bool changed = !password.empty() || cfg.url != old.url || cfg.user != old.user ||
                           cfg.lanUrl != old.lanUrl || cfg.allowHttpFallback != old.allowHttpFallback ||
                           cfg.allowUntrustedCert != old.allowUntrustedCert;
            if (!ConfigSaveServer(g_app.iniPath, cfg, password, err)) {
                MessageBoxW(dlg, err.c_str(), L"Settings", MB_ICONERROR);
                SetWindowLongPtrW(dlg, DWLP_MSGRESULT, PSNRET_INVALID);
                return TRUE;
            }
            SecureZeroMemory(&password[0], password.size() * sizeof(wchar_t));
            g_settings->serverChanged = changed;
            SetWindowLongPtrW(dlg, DWLP_MSGRESULT, PSNRET_NOERROR);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static INT_PTR CALLBACK PlaybackPageProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG: {
        HWND q = GetDlgItem(dlg, IDC_SET_QUALITY);
        int sel = 3;
        for (int i = 0; i < (int)(sizeof(kQualities) / sizeof(kQualities[0])); ++i) {
            SendMessageW(q, CB_ADDSTRING, 0, (LPARAM)Fmt(L"%d kbps%ls", kQualities[i],
                                                         kQualities[i] == 320 ? L" (best)" : L"").c_str());
            if (kQualities[i] <= g_settings->cfg.maxBitRate)
                sel = i;
        }
        SendMessageW(q, CB_ADDSTRING, 0, (LPARAM)L"Lossless (FLAC files as they are)");
        if (g_settings->cfg.lossless)
            sel = kLosslessChoice;
        SendMessageW(q, CB_SETCURSEL, sel, 0);

        HWND d = GetDlgItem(dlg, IDC_SET_DEVICE);
        SendMessageW(d, CB_ADDSTRING, 0, (LPARAM)L"Default sound device");
        int dsel = 0;
        UINT n = waveOutGetNumDevs();
        for (UINT i = 0; i < n; ++i) {
            WAVEOUTCAPSW caps;
            if (waveOutGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR)
                continue;
            int idx = (int)SendMessageW(d, CB_ADDSTRING, 0, (LPARAM)caps.szPname);
            if (g_settings->pb.device == caps.szPname)
                dsel = idx;
        }
        SendMessageW(d, CB_SETCURSEL, dsel, 0);

        HWND b = GetDlgItem(dlg, IDC_SET_BUFFER);
        int bsel = 1;
        for (int i = 0; i < (int)(sizeof(kBufferChoices) / sizeof(kBufferChoices[0])); ++i) {
            SendMessageW(b, CB_ADDSTRING, 0, (LPARAM)kBufferChoices[i].label);
            if (kBufferChoices[i].buffers <= g_settings->pb.bufferCount)
                bsel = i;
        }
        SendMessageW(b, CB_SETCURSEL, bsel, 0);

        CheckDlgButton(dlg, IDC_SET_SCROBBLE, g_settings->pb.scrobble ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_SET_GAPLESS, g_settings->pb.gapless ? BST_CHECKED : BST_UNCHECKED);
        return TRUE;
    }

    case WM_NOTIFY:
        if (((NMHDR*)lp)->code == PSN_APPLY) {
            int q = (int)SendDlgItemMessageW(dlg, IDC_SET_QUALITY, CB_GETCURSEL, 0, 0);
            int b = (int)SendDlgItemMessageW(dlg, IDC_SET_BUFFER, CB_GETCURSEL, 0, 0);
            int d = (int)SendDlgItemMessageW(dlg, IDC_SET_DEVICE, CB_GETCURSEL, 0, 0);
            PlaybackSettings pb;
            pb.bufferCount = kBufferChoices[b >= 0 ? b : 1].buffers;
            pb.scrobble = IsDlgButtonChecked(dlg, IDC_SET_SCROBBLE) == BST_CHECKED;
            pb.gapless = IsDlgButtonChecked(dlg, IDC_SET_GAPLESS) == BST_CHECKED;
            if (d > 0) {
                wchar_t name[MAXPNAMELEN + 1] = L"";
                SendDlgItemMessageW(dlg, IDC_SET_DEVICE, CB_GETLBTEXT, d, (LPARAM)name);
                pb.device = name;
            }
            // Lossless still needs an MP3 quality for files that aren't FLAC: use the best.
            g_settings->cfg.lossless = q == kLosslessChoice;
            g_settings->cfg.maxBitRate = q == kLosslessChoice ? 320 : kQualities[q >= 0 ? q : 3];
            WritePrivateProfileStringW(L"Server", L"MaxBitRate", Fmt(L"%d", g_settings->cfg.maxBitRate).c_str(),
                                       g_app.iniPath.c_str());
            WritePrivateProfileStringW(L"Server", L"Lossless", g_settings->cfg.lossless ? L"1" : L"0",
                                       g_app.iniPath.c_str());
            PlaybackSave(g_app.iniPath, pb);
            g_settings->pb = pb;
            g_settings->playbackChanged = true;
            SetWindowLongPtrW(dlg, DWLP_MSGRESULT, PSNRET_NOERROR);
            return TRUE;
        }
        break;
    }
    (void)wp;
    return FALSE;
}

static INT_PTR CALLBACK DisplayPageProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG:
        CheckDlgButton(dlg, IDC_SET_SHOWFAV, g_settings->showFavorites ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_SET_SHOWRATING, g_settings->showRatings ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_SET_TRAYICON, g_settings->trayIcon ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_SET_TRAYMIN, g_settings->trayMinimize ? BST_CHECKED : BST_UNCHECKED);
        EnableWindow(GetDlgItem(dlg, IDC_SET_TRAYMIN), g_settings->trayIcon);
        CheckDlgButton(dlg, IDC_SET_MEDIAKEYS, g_settings->mediaKeys ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_SET_LASTFM, g_settings->lastfmInfo ? BST_CHECKED : BST_UNCHECKED);
        return TRUE;

    case WM_COMMAND:
        if (LOWORD(wp) == IDC_SET_TRAYICON && HIWORD(wp) == BN_CLICKED)
            EnableWindow(GetDlgItem(dlg, IDC_SET_TRAYMIN), IsDlgButtonChecked(dlg, IDC_SET_TRAYICON) == BST_CHECKED);
        return TRUE;

    case WM_NOTIFY:
        if (((NMHDR*)lp)->code == PSN_APPLY) {
            g_settings->showFavorites = IsDlgButtonChecked(dlg, IDC_SET_SHOWFAV) == BST_CHECKED;
            g_settings->showRatings = IsDlgButtonChecked(dlg, IDC_SET_SHOWRATING) == BST_CHECKED;
            g_settings->trayIcon = IsDlgButtonChecked(dlg, IDC_SET_TRAYICON) == BST_CHECKED;
            g_settings->trayMinimize = IsDlgButtonChecked(dlg, IDC_SET_TRAYMIN) == BST_CHECKED;
            g_settings->mediaKeys = IsDlgButtonChecked(dlg, IDC_SET_MEDIAKEYS) == BST_CHECKED;
            g_settings->lastfmInfo = IsDlgButtonChecked(dlg, IDC_SET_LASTFM) == BST_CHECKED;
            g_settings->displayChanged = true;
            SetWindowLongPtrW(dlg, DWLP_MSGRESULT, PSNRET_NOERROR);
            return TRUE;
        }
        break;
    }
    (void)wp;
    return FALSE;
}

// Shows or hides favorites and ratings: the track-list columns, and Favorites in the sidebar
// and View menu. Clicks, keys and right-click items check the flags themselves.
static void ApplyDisplay()
{
    // Sidebar: Library > Favorites sits after Songs.
    g_app.treeUpdating = true;
    if (!g_app.showFavorites && g_app.treeFavorites) {
        TreeView_DeleteItem(g_app.tree, g_app.treeFavorites);
        g_app.treeFavorites = NULL;
    } else if (g_app.showFavorites && !g_app.treeFavorites) {
        g_app.treeFavorites = TreeAdd(g_app.treeLibrary, L"Favorites", TREE_FAVORITES, false, g_app.treeSongs);
    }
    g_app.treeUpdating = false;

    // View menu: Fa&vorites follows &Songs.
    HMENU view = GetSubMenu(g_app.menu, 1);
    DeleteMenu(view, IDM_FAVORITES, MF_BYCOMMAND);
    if (g_app.showFavorites) {
        int n = GetMenuItemCount(view);
        for (int i = 0; i < n; ++i)
            if (GetMenuItemID(view, i) == IDM_SONGS) {
                InsertMenuW(view, i + 1, MF_BYPOSITION | MF_STRING, IDM_FAVORITES, L"Fa&vorites");
                break;
            }
    }

    if (!g_app.showFavorites && g_app.mainView == VIEW_FAVORITES)
        ShowAlbumsView();
    Layout(g_app.wnd);   // refits the song columns
    InvalidateRect(g_app.songs, NULL, TRUE);
}

static void ShowSettings()
{
    SettingsState st;
    ConfigRead(g_app.iniPath, st.cfg);
    PlaybackLoad(g_app.iniPath, st.pb);
    st.showFavorites = g_app.showFavorites;
    st.showRatings = g_app.showRatings;
    st.trayIcon = g_app.trayIcon;
    st.trayMinimize = g_app.trayMinimize;
    st.mediaKeys = g_app.mediaKeysGlobal;
    st.lastfmInfo = g_app.lastfmInfo;
    st.serverChanged = st.playbackChanged = st.displayChanged = false;
    g_settings = &st;

    PROPSHEETPAGEW pages[3];
    ZeroMemory(pages, sizeof(pages));
    const int ids[3] = { IDD_SET_SERVER, IDD_SET_PLAYBACK, IDD_SET_DISPLAY };
    DLGPROC procs[3] = { ServerPageProc, PlaybackPageProc, DisplayPageProc };
    for (int i = 0; i < 3; ++i) {
        pages[i].dwSize = sizeof(PROPSHEETPAGEW);
        pages[i].dwFlags = PSP_DEFAULT;
        pages[i].hInstance = GetModuleHandleW(NULL);
        pages[i].pszTemplate = MAKEINTRESOURCEW(ids[i]);
        pages[i].pfnDlgProc = procs[i];
    }
    PROPSHEETHEADERW sheet;
    ZeroMemory(&sheet, sizeof(sheet));
    sheet.dwSize = sizeof(sheet);
    sheet.dwFlags = PSH_PROPSHEETPAGE | PSH_NOAPPLYNOW | PSH_NOCONTEXTHELP;
    sheet.hwndParent = g_app.wnd;
    sheet.hInstance = GetModuleHandleW(NULL);
    sheet.pszCaption = L"Settings";
    sheet.nPages = 3;
    sheet.ppsp = pages;
    PropertySheetW(&sheet);
    g_settings = NULL;

    if (st.playbackChanged) {
        ApplyPlayback(st.pb);
        g_app.client.SetMaxBitRate(st.cfg.maxBitRate);
        g_app.client.SetLossless(st.cfg.lossless);
        UpdateGaplessNext();   // the next track's stream follows the new quality
        SetStatus(L"Playback settings saved; they apply from the next track or seek.");
    }
    if (st.displayChanged && (st.showFavorites != g_app.showFavorites || st.showRatings != g_app.showRatings)) {
        g_app.showFavorites = st.showFavorites;
        g_app.showRatings = st.showRatings;
        const wchar_t* ini = g_app.iniPath.c_str();
        WritePrivateProfileStringW(L"View", L"ShowFavorites", g_app.showFavorites ? L"1" : L"0", ini);
        WritePrivateProfileStringW(L"View", L"ShowRatings", g_app.showRatings ? L"1" : L"0", ini);
        ApplyDisplay();
    }
    if (st.displayChanged && (st.trayIcon != g_app.trayIcon || st.trayMinimize != g_app.trayMinimize)) {
        g_app.trayIcon = st.trayIcon;
        g_app.trayMinimize = st.trayMinimize;
        const wchar_t* ini = g_app.iniPath.c_str();
        WritePrivateProfileStringW(L"View", L"TrayIcon", g_app.trayIcon ? L"1" : L"0", ini);
        WritePrivateProfileStringW(L"View", L"MinimizeToTray", g_app.trayMinimize ? L"1" : L"0", ini);
        if (g_app.trayIcon)
            TrayAdd();
        else
            TrayRemove();
    }
    if (st.displayChanged && st.mediaKeys != g_app.mediaKeysGlobal) {
        g_app.mediaKeysGlobal = st.mediaKeys;
        WritePrivateProfileStringW(L"Player", L"GlobalMediaKeys", g_app.mediaKeysGlobal ? L"1" : L"0",
                                   g_app.iniPath.c_str());
        RegisterMediaKeys(g_app.mediaKeysGlobal);
    }
    if (st.displayChanged && st.lastfmInfo != g_app.lastfmInfo) {
        g_app.lastfmInfo = st.lastfmInfo;
        WritePrivateProfileStringW(L"View", L"LastFmInfo", g_app.lastfmInfo ? L"1" : L"0", g_app.iniPath.c_str());
        RefreshShownInfo();
    }
    if (st.serverChanged)
        DoConnect();
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
    g_app.treeFavorites = TreeAdd(g_app.treeLibrary, L"Favorites", TREE_FAVORITES, false);
    g_app.treeQueue = TreeAdd(g_app.treeLibrary, L"Play Queue", TREE_QUEUE, false);
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
    AddColumn(g_app.songs, COL_FAV, L"", 24, LVCFMT_LEFT);   // heart, drawn
    AddColumn(g_app.songs, COL_RATING, L"Rating", 80, LVCFMT_LEFT);
    AddColumn(g_app.songs, COL_LENGTH, L"Time", 48, LVCFMT_RIGHT);
    AddColumn(g_app.songs, COL_ALBUM, L"Album", 170, LVCFMT_LEFT);

    // Transport bar.
    g_app.btnPrev = MakeChild(hwnd, L"BUTTON", L"Previous", WS_TABSTOP | BS_OWNERDRAW, IDC_PREV);
    g_app.btnPlay = MakeChild(hwnd, L"BUTTON", L"Play", WS_TABSTOP | BS_OWNERDRAW, IDC_PLAY);
    g_app.btnPause = MakeChild(hwnd, L"BUTTON", L"Pause", WS_TABSTOP | BS_OWNERDRAW, IDC_PAUSE);
    g_app.btnStop = MakeChild(hwnd, L"BUTTON", L"Stop", WS_TABSTOP | BS_OWNERDRAW, IDC_STOP);
    g_app.btnNext = MakeChild(hwnd, L"BUTTON", L"Next", WS_TABSTOP | BS_OWNERDRAW, IDC_NEXT);
    g_app.btnShuffle = MakeChild(hwnd, L"BUTTON", L"Shuffle", WS_TABSTOP | BS_OWNERDRAW, IDC_SHUFFLE);
    g_app.btnRepeat = MakeChild(hwnd, L"BUTTON", L"Repeat", WS_TABSTOP | BS_OWNERDRAW, IDC_REPEAT);
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
    g_app.repeat = (int)GetPrivateProfileIntW(L"Player", L"Repeat", REPEAT_OFF, g_app.iniPath.c_str());
    if (g_app.repeat < REPEAT_OFF || g_app.repeat > REPEAT_ONE)
        g_app.repeat = REPEAT_OFF;
    g_app.songSortCol = g_app.albumSortCol = g_app.artistSortCol = -1;
    g_app.headerArrows = CommonControls6();
    g_app.mediaKeysGlobal = GetPrivateProfileIntW(L"Player", L"GlobalMediaKeys", 1, g_app.iniPath.c_str()) != 0;
    RegisterMediaKeys(g_app.mediaKeysGlobal);
    g_app.lastfmInfo = GetPrivateProfileIntW(L"View", L"LastFmInfo", 0, g_app.iniPath.c_str()) != 0;
    PlaybackSettings pb;
    PlaybackLoad(g_app.iniPath, pb);
    ApplyPlayback(pb);
    g_app.showFavorites = GetPrivateProfileIntW(L"View", L"ShowFavorites", 1, g_app.iniPath.c_str()) != 0;
    g_app.showRatings = GetPrivateProfileIntW(L"View", L"ShowRatings", 1, g_app.iniPath.c_str()) != 0;
    ApplyDisplay();
    g_app.trayIcon = GetPrivateProfileIntW(L"View", L"TrayIcon", 1, g_app.iniPath.c_str()) != 0;
    g_app.trayMinimize = GetPrivateProfileIntW(L"View", L"MinimizeToTray", 0, g_app.iniPath.c_str()) != 0;
    g_app.taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
    if (g_app.trayIcon)
        TrayAdd();

    g_app.status = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                                   0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_STATUS, GetModuleHandleW(NULL), NULL);

    g_app.tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_ALWAYSTIP,
                                    CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                    hwnd, NULL, GetModuleHandleW(NULL), NULL);
    AddTooltip(g_app.btnPrev, L"Previous (Ctrl+B)");
    AddTooltip(g_app.btnPlay, L"Play");
    AddTooltip(g_app.btnPause, L"Pause / Resume");
    AddTooltip(g_app.btnStop, L"Stop (Ctrl+S)");
    AddTooltip(g_app.btnNext, L"Next (Ctrl+N)");
    AddTooltip(g_app.btnShuffle, L"Shuffle (Ctrl+H)");
    AddTooltip(g_app.btnRepeat, L"Repeat: off, all, one (Ctrl+T)");
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
    if (nm->hwndFrom == ListView_GetHeader(g_app.songs) &&
        (nm->code == HDN_BEGINTRACKW || nm->code == HDN_BEGINTRACKA ||
         nm->code == HDN_DIVIDERDBLCLICKW || nm->code == HDN_DIVIDERDBLCLICKA)) {
        int col = ((NMHEADERW*)nm)->iItem;
        if ((col == COL_FAV && !g_app.showFavorites) || (col == COL_RATING && !g_app.showRatings))
            return TRUE;   // hidden column: keep it at width 0
    }
    switch (nm->idFrom) {
    case IDC_SONGS:
        if (nm->code == NM_CUSTOMDRAW)
            return OnSongsCustomDraw((NMLVCUSTOMDRAW*)nm);
        if (nm->code == LVN_GETDISPINFOW) {
            OnSongsDispInfo((NMLVDISPINFOW*)nm);
            return 0;
        }
        if (nm->code == LVN_COLUMNCLICK)
            OnSongsColumnClick(((NMLISTVIEW*)nm)->iSubItem);
        else if (nm->code == LVN_BEGINDRAG)
            DragBegin(((NMLISTVIEW*)nm)->iItem);
        else if (nm->code == NM_CLICK)
            OnSongsClick((NMITEMACTIVATE*)nm);
        else if (nm->code == NM_DBLCLK || nm->code == NM_RETURN)
            PlayFromSongList(SelectedIndex(g_app.songs));
        else if (nm->code == NM_SETFOCUS || nm->code == NM_KILLFOCUS)
            InvalidateRect(g_app.songs, NULL, FALSE);   // star outline colour follows focus
        else if (nm->code == LVN_KEYDOWN) {
            WORD vk = ((NMLVKEYDOWN*)nm)->wVKey;
            int i = SelectedIndex(g_app.songs);
            if (g_app.dragging) {
                if (vk == VK_ESCAPE)
                    DragEnd(false);
            } else if (g_app.showRatings && vk >= '0' && vk <= '5')
                SetSongRating(i, vk - '0');
            else if (g_app.showRatings && vk >= VK_NUMPAD0 && vk <= VK_NUMPAD5)
                SetSongRating(i, vk - VK_NUMPAD0);
            else if (vk == VK_SPACE)
                PlayPause();
            else if (vk == VK_DELETE && g_app.songSource == SRC_QUEUE)
                RemoveFromQueue(i);
            else if (vk == VK_DELETE && g_app.songSource == SRC_PLAYLIST && SongsSorted())
                SetStatus(L"The playlist is sorted: click # to restore its order, then remove the track.");
            else if (vk == VK_DELETE)
                RemoveFromPlaylist(i);
            else if (vk == VK_BACK)
                GoBack();
        }
        break;

    case IDC_ALBUMS:
        if (nm->code == LVN_COLUMNCLICK)
            OnAlbumsColumnClick(((NMLISTVIEW*)nm)->iSubItem);
        else if (nm->code == NM_DBLCLK || nm->code == NM_RETURN)
            OpenSelectedAlbum();
        else if (nm->code == LVN_KEYDOWN && ((NMLVKEYDOWN*)nm)->wVKey == VK_BACK)
            GoBack();
        break;

    case IDC_ARTISTS:
        if (nm->code == LVN_COLUMNCLICK)
            OnArtistsColumnClick(((NMLISTVIEW*)nm)->iSubItem);
        else if (nm->code == NM_DBLCLK || nm->code == NM_RETURN)
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
            else if (data == TREE_FAVORITES)
                ShowFavoritesView();
            else if (data == TREE_QUEUE)
                ShowQueueView();
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
    case IDM_SETTINGS:
        ShowSettings();
        break;
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
    case IDM_FAVORITES:
        ShowFavoritesView();
        break;
    case IDM_QUEUE:
        ShowQueueView();
        break;
    case IDM_SHOW:
        TrayRestore();
        break;
    case IDM_PLAYPAUSE:
        PlayPause();
        break;
    case IDC_REPEAT:
    case IDM_REPEAT_CYCLE:
        SetRepeat((g_app.repeat + 1) % 3);
        break;
    case IDM_REPEAT_OFF:
    case IDM_REPEAT_ALL:
    case IDM_REPEAT_ONE:
        SetRepeat(id - IDM_REPEAT_OFF);
        break;
    case IDM_FIND:
        FocusSearch();
        break;
    case IDM_MINI:
        ToggleMini();
        break;
    case IDC_PREV:
    case IDM_PREV:
        PlayPrevious();
        break;
    case IDC_NEXT:
    case IDM_NEXT:
        PlayNext();
        break;
    case IDC_NEW_PLAYLIST:
    case IDM_NEW_PLAYLIST:
        NewPlaylist(std::vector<std::string>(), std::string(), L"New playlist");
        break;
    case IDM_EDIT_PLAYLIST:
        EditShownPlaylist();
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

// Media keys (and keyboard/remote media buttons) while the player is focused. True if handled.
static bool OnAppCommand(int cmd)
{
    switch (cmd) {
    case APPCOMMAND_MEDIA_PLAY_PAUSE: PlayPause(); break;
    case APPCOMMAND_MEDIA_PLAY:
        if (!g_app.player.IsActive() || g_app.player.IsPaused())
            PlayPause();
        break;
    case APPCOMMAND_MEDIA_PAUSE:
        if (g_app.player.IsActive() && !g_app.player.IsPaused())
            DoPause();
        break;
    case APPCOMMAND_MEDIA_STOP:           StopPlayback(); break;
    case APPCOMMAND_MEDIA_PREVIOUSTRACK:  PlayPrevious(); break;
    case APPCOMMAND_MEDIA_NEXTTRACK:      PlayNext(); break;
    default:
        return false;
    }
    RefreshTransport();
    return true;
}

// Playback shortcuts work in the mini player as they are; the rest switch to the main window.
static bool IsPlaybackCommand(int id)
{
    return id == IDM_PLAYPAUSE || id == IDM_STOP || id == IDM_PREV || id == IDM_NEXT || id == IDM_SHUFFLE ||
           (id >= IDM_REPEAT_OFF && id <= IDM_REPEAT_CYCLE);
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
            if (!IsPlaybackCommand(id))
                ToggleMini();
            OnCommand(id);
            break;
        }
        RefreshTransport();
        return 0;
    }

    case WM_APPCOMMAND:
        if (OnAppCommand(GET_APPCOMMAND_LPARAM(lp)))
            return TRUE;
        break;

    case WM_CLOSE:
        ToggleMini();   // closing the mini player goes back to the full window
        return 0;

    case WM_SIZE:
        if (wp == SIZE_MINIMIZED)
            TrayMinimize(hwnd);
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
        if (wp == SIZE_MINIMIZED && TrayMinimize(hwnd))
            return 0;
        Layout(hwnd);
        return 0;

    case WM_MOUSEMOVE:
        if (g_app.dragging) {
            POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
            MapWindowPoints(hwnd, g_app.songs, &pt, 1);
            DragUpdate(pt);
        }
        return 0;

    case WM_CAPTURECHANGED:
        if (g_app.dragging && (HWND)lp != hwnd)
            DragEnd(false);
        return 0;

    case WM_LBUTTONUP: {
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        if (g_app.dragging) {
            MapWindowPoints(hwnd, g_app.songs, &pt, 1);
            DragUpdate(pt);
            DragEnd(true);
            return 0;
        }
        if (g_app.noteCut && g_app.shown.has && PtInRect(&g_app.noteRect, pt)) {
            std::wstring text = g_app.shown.note;
            for (size_t at = text.find(L'\n'); at != std::wstring::npos; at = text.find(L'\n', at + 2))
                text.replace(at, 1, L"\r\n");
            MessageBoxW(hwnd, text.c_str(), g_app.shown.title.c_str(), MB_OK);
        }
        return 0;
    }

    case WM_SETCURSOR:
        if ((HWND)wp == hwnd && LOWORD(lp) == HTCLIENT && g_app.noteCut) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            if (PtInRect(&g_app.noteRect, pt)) {
                SetCursor(LoadCursor(NULL, IDC_HAND));
                return TRUE;
            }
        }
        break;

    case WM_APP_TRAY:
        // Left click: bring the player back (or to the front). Right click: controls.
        if (lp == WM_LBUTTONUP || lp == WM_LBUTTONDBLCLK)
            TrayRestore();
        else if (lp == WM_RBUTTONUP)
            TrayContextMenu();
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
        else if (wp == TIMER_DRAGSCROLL)
            DragScroll();
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

    case WM_HOTKEY:
        switch ((int)wp) {
        case HOTKEY_PLAYPAUSE: PlayPause(); break;
        case HOTKEY_STOP:      StopPlayback(); break;
        case HOTKEY_PREV:      PlayPrevious(); break;
        case HOTKEY_NEXT:      PlayNext(); break;
        }
        RefreshTransport();
        return 0;

    case WM_APPCOMMAND:
        if (OnAppCommand(GET_APPCOMMAND_LPARAM(lp)))
            return TRUE;
        break;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_POSITION);
        RegisterMediaKeys(false);
        TrayRemove();
        g_app.player.Stop();
        ClearArtCache();
        DeleteObject(g_app.fontBold);
        DeleteObject(g_app.fontTitle);
        DeleteObject(g_app.latchBrush);
        PostQuitMessage(0);
        return 0;
    }
    if (msg == g_app.taskbarCreatedMsg && msg != 0) {
        // Explorer restarted and lost the icon.
        g_app.trayAdded = false;
        if (g_app.trayIcon)
            TrayAdd();
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
        { FVIRTKEY | FCONTROL, 'Q', IDM_QUEUE },
        { FVIRTKEY | FCONTROL, 'P', IDM_PLAYPAUSE },
        { FVIRTKEY | FCONTROL, 'S', IDM_STOP },
        { FVIRTKEY | FCONTROL, 'B', IDM_PREV },
        { FVIRTKEY | FCONTROL, 'N', IDM_NEXT },
        { FVIRTKEY | FCONTROL, 'H', IDM_SHUFFLE },
        { FVIRTKEY | FCONTROL, 'T', IDM_REPEAT_CYCLE },
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
