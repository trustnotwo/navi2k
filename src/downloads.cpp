#include "downloads.h"
#include "util.h"
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include "cJSON.h"

static const wchar_t kIndexName[] = L"downloads.json";
static const size_t kMaxPart = 60;   // characters per folder or file name

// One folder or file name: characters Windows forbids become '_', no leading/trailing dots or
// spaces (Explorer can't handle those), no device names (CON, NUL, COM1...), and short.
static std::wstring CleanPart(const std::wstring& in, const wchar_t* fallback)
{
    std::wstring s;
    for (size_t i = 0; i < in.size(); ++i) {
        wchar_t c = in[i];
        s += (c < 32 || wcschr(L"\\/:*?\"<>|", c)) ? L'_' : c;
    }
    if (s.size() > kMaxPart)
        s.erase(kMaxPart);
    size_t a = s.find_first_not_of(L" ."), b = s.find_last_not_of(L" .");
    s = a == std::wstring::npos ? std::wstring() : s.substr(a, b - a + 1);
    if (s.empty())
        return fallback;
    std::wstring stem = s.substr(0, s.find(L'.'));
    static const wchar_t* const kDevices[] = { L"CON", L"PRN", L"AUX", L"NUL", L"CLOCK$" };
    bool device = (stem.size() == 4 && (_wcsnicmp(stem.c_str(), L"COM", 3) == 0 || _wcsnicmp(stem.c_str(), L"LPT", 3) == 0) &&
                   stem[3] >= L'1' && stem[3] <= L'9');
    for (size_t i = 0; i < sizeof(kDevices) / sizeof(kDevices[0]) && !device; ++i)
        device = _wcsicmp(stem.c_str(), kDevices[i]) == 0;
    return device ? s + L"_" : s;
}

// Makes each folder of a relative path under 'root' ("A\B\file" makes root\A and root\A\B).
static void MakeFolders(const std::wstring& root, const std::wstring& rel)
{
    for (size_t at = rel.find(L'\\'); at != std::wstring::npos; at = rel.find(L'\\', at + 1))
        CreateDirectoryW((root + L"\\" + rel.substr(0, at)).c_str(), NULL);
}

// Removes the folders of a relative path that are now empty, deepest first.
static void PruneFolders(const std::wstring& root, const std::wstring& rel)
{
    for (size_t at = rel.rfind(L'\\'); at != std::wstring::npos && at > 0; at = rel.rfind(L'\\', at - 1)) {
        if (!RemoveDirectoryW((root + L"\\" + rel.substr(0, at)).c_str()))
            break;   // not empty
        if (at == 0)
            break;
    }
}

std::wstring SafeFileName(const std::string& id)
{
    std::wstring s;
    for (size_t i = 0; i < id.size(); ++i) {
        char c = id[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        s += ok ? (wchar_t)c : L'_';
    }
    return s.empty() ? std::wstring(L"_") : s;
}

Downloads::Downloads()
    : m_notify(NULL), m_message(0), m_req(NULL), m_running(false), m_cancel(0)
{
    InitializeCriticalSection(&m_lock);
    m_idle = CreateEventW(NULL, TRUE, TRUE, NULL);
}

Downloads::~Downloads()
{
    CloseHandle(m_idle);
    DeleteCriticalSection(&m_lock);
}

void Downloads::Init(HWND notify, UINT message)
{
    m_notify = notify;
    m_message = message;
}

void Downloads::SetFolder(const std::wstring& folder)
{
    m_folder = folder;
    while (m_folder.size() > 3 && (m_folder[m_folder.size() - 1] == L'\\' || m_folder[m_folder.size() - 1] == L'/'))
        m_folder.erase(m_folder.size() - 1);
    m_tracks.clear();
    m_playlists.clear();
    EnterCriticalSection(&m_lock);
    m_workFolder = m_folder;
    LeaveCriticalSection(&m_lock);
    Load();
    Reindex();
}

bool Downloads::EnsureFolder(std::wstring& err)
{
    if (!CreateDirectoryW(m_folder.c_str(), NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        err = Fmt(L"Could not create the downloads folder %ls (error %lu). Choose another one under "
                  L"File > Settings > Downloads.", m_folder.c_str(), GetLastError());
        return false;
    }
    CreateDirectoryW((m_folder + L"\\art").c_str(), NULL);
    return true;
}

void Downloads::Reindex()
{
    m_byId.clear();
    for (size_t i = 0; i < m_tracks.size(); ++i)
        m_byId[m_tracks[i].song.id] = i;
}

const DownloadedTrack* Downloads::Find(const std::string& songId) const
{
    std::map<std::string, size_t>::const_iterator it = m_byId.find(songId);
    return it == m_byId.end() ? NULL : &m_tracks[it->second];
}

unsigned long long Downloads::TotalBytes() const
{
    unsigned long long n = 0;
    for (size_t i = 0; i < m_tracks.size(); ++i)
        n += m_tracks[i].bytes;
    return n;
}

std::wstring Downloads::ArtPath(const std::string& coverArtId) const
{
    return m_folder + L"\\art\\" + SafeFileName(coverArtId);
}

static bool ReadWholeFile(const std::wstring& path, std::string& bytes)
{
    bytes.clear();
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    DWORD size = GetFileSize(f, NULL);
    bool ok = size != INVALID_FILE_SIZE && size < 64 * 1024 * 1024;
    if (ok && size) {
        bytes.resize(size);
        DWORD got = 0;
        ok = ReadFile(f, &bytes[0], size, &got, NULL) && got == size;
    }
    CloseHandle(f);
    return ok;
}

static bool WriteWholeFile(const std::wstring& path, const std::string& bytes)
{
    // Written beside the target and renamed over it, so a crash never leaves half a file.
    const std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    DWORD put = 0;
    bool ok = bytes.empty() || (WriteFile(f, bytes.data(), (DWORD)bytes.size(), &put, NULL) && put == bytes.size());
    CloseHandle(f);
    if (ok)
        ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok)
        DeleteFileW(tmp.c_str());
    return ok;
}

bool Downloads::LoadArt(const std::string& coverArtId, std::string& bytes) const
{
    return !coverArtId.empty() && !m_folder.empty() && ReadWholeFile(ArtPath(coverArtId), bytes) && !bytes.empty();
}

// --- Index (downloads.json) -------------------------------------------------------------

static std::string JText(cJSON* o, const char* key)
{
    cJSON* v = cJSON_GetObjectItemCaseSensitive(o, key);
    return (cJSON_IsString(v) && v->valuestring) ? v->valuestring : std::string();
}

static int JNum(cJSON* o, const char* key)
{
    cJSON* v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? v->valueint : 0;
}

void Downloads::Load()
{
    std::string text;
    if (!ReadWholeFile(m_folder + L"\\" + kIndexName, text))
        return;
    cJSON* root = cJSON_Parse(text.c_str());
    if (!root) {
        LogF(L"downloads.json could not be read; starting with an empty list");
        return;
    }
    cJSON* t;
    cJSON_ArrayForEach(t, cJSON_GetObjectItemCaseSensitive(root, "tracks")) {
        DownloadedTrack d;
        SubsonicSong& s = d.song;
        s.id = JText(t, "id");
        s.coverArt = JText(t, "coverArt");
        s.albumId = JText(t, "albumId");
        s.artistId = JText(t, "artistId");
        s.title = Utf8ToWide(JText(t, "title"));
        s.artist = Utf8ToWide(JText(t, "artist"));
        s.album = Utf8ToWide(JText(t, "album"));
        s.albumArtist = Utf8ToWide(JText(t, "albumArtist"));
        s.suffix = Utf8ToWide(JText(t, "suffix"));
        s.track = JNum(t, "track");
        s.disc = JNum(t, "disc");
        s.duration = JNum(t, "duration");
        s.bitRate = JNum(t, "bitRate");
        s.bitDepth = JNum(t, "bitDepth");
        s.samplingRate = JNum(t, "samplingRate");
        s.channelCount = JNum(t, "channelCount");
        s.userRating = JNum(t, "userRating");
        s.starred = JNum(t, "starred") != 0;
        d.file = Utf8ToWide(JText(t, "file"));
        d.flac = JNum(t, "flac") != 0;
        d.server = JText(t, "server");
        cJSON* b = cJSON_GetObjectItemCaseSensitive(t, "bytes");
        d.bytes = cJSON_IsNumber(b) ? (unsigned long long)b->valuedouble : 0;
        // Only list what is really there.
        if (!s.id.empty() && !d.file.empty() &&
            GetFileAttributesW((m_folder + L"\\" + d.file).c_str()) != INVALID_FILE_ATTRIBUTES)
            m_tracks.push_back(d);
    }
    cJSON* p;
    cJSON_ArrayForEach(p, cJSON_GetObjectItemCaseSensitive(root, "playlists")) {
        DownloadedPlaylist pl;
        pl.info.id = JText(p, "id");
        pl.info.coverArt = JText(p, "coverArt");
        pl.info.name = Utf8ToWide(JText(p, "name"));
        pl.info.owner = Utf8ToWide(JText(p, "owner"));
        pl.info.comment = Utf8ToWide(JText(p, "comment"));
        pl.info.duration = JNum(p, "duration");
        pl.info.readOnly = JNum(p, "readOnly") != 0;
        cJSON* s;
        cJSON_ArrayForEach(s, cJSON_GetObjectItemCaseSensitive(p, "songs"))
            if (cJSON_IsString(s) && s->valuestring)
                pl.songIds.push_back(s->valuestring);
        pl.info.songCount = (int)pl.songIds.size();
        if (!pl.info.id.empty())
            m_playlists.push_back(pl);
    }
    cJSON_Delete(root);
}

void Downloads::Save() const
{
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON* arr = cJSON_AddArrayToObject(root, "tracks");
    for (size_t i = 0; i < m_tracks.size(); ++i) {
        const DownloadedTrack& d = m_tracks[i];
        const SubsonicSong& s = d.song;
        cJSON* t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "id", s.id.c_str());
        cJSON_AddStringToObject(t, "coverArt", s.coverArt.c_str());
        cJSON_AddStringToObject(t, "albumId", s.albumId.c_str());
        cJSON_AddStringToObject(t, "artistId", s.artistId.c_str());
        cJSON_AddStringToObject(t, "title", WideToUtf8(s.title).c_str());
        cJSON_AddStringToObject(t, "artist", WideToUtf8(s.artist).c_str());
        cJSON_AddStringToObject(t, "album", WideToUtf8(s.album).c_str());
        cJSON_AddStringToObject(t, "albumArtist", WideToUtf8(s.albumArtist).c_str());
        cJSON_AddStringToObject(t, "suffix", WideToUtf8(s.suffix).c_str());
        cJSON_AddNumberToObject(t, "track", s.track);
        cJSON_AddNumberToObject(t, "disc", s.disc);
        cJSON_AddNumberToObject(t, "duration", s.duration);
        cJSON_AddNumberToObject(t, "bitRate", s.bitRate);
        cJSON_AddNumberToObject(t, "bitDepth", s.bitDepth);
        cJSON_AddNumberToObject(t, "samplingRate", s.samplingRate);
        cJSON_AddNumberToObject(t, "channelCount", s.channelCount);
        cJSON_AddNumberToObject(t, "userRating", s.userRating);
        cJSON_AddNumberToObject(t, "starred", s.starred ? 1 : 0);
        cJSON_AddStringToObject(t, "file", WideToUtf8(d.file).c_str());
        cJSON_AddNumberToObject(t, "flac", d.flac ? 1 : 0);
        cJSON_AddNumberToObject(t, "bytes", (double)d.bytes);
        cJSON_AddStringToObject(t, "server", d.server.c_str());
        cJSON_AddItemToArray(arr, t);
    }
    cJSON* parr = cJSON_AddArrayToObject(root, "playlists");
    for (size_t i = 0; i < m_playlists.size(); ++i) {
        const DownloadedPlaylist& pl = m_playlists[i];
        cJSON* p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "id", pl.info.id.c_str());
        cJSON_AddStringToObject(p, "coverArt", pl.info.coverArt.c_str());
        cJSON_AddStringToObject(p, "name", WideToUtf8(pl.info.name).c_str());
        cJSON_AddStringToObject(p, "owner", WideToUtf8(pl.info.owner).c_str());
        cJSON_AddStringToObject(p, "comment", WideToUtf8(pl.info.comment).c_str());
        cJSON_AddNumberToObject(p, "duration", pl.info.duration);
        cJSON_AddNumberToObject(p, "readOnly", pl.info.readOnly ? 1 : 0);
        cJSON* songs = cJSON_AddArrayToObject(p, "songs");
        for (size_t k = 0; k < pl.songIds.size(); ++k)
            cJSON_AddItemToArray(songs, cJSON_CreateString(pl.songIds[k].c_str()));
        cJSON_AddItemToArray(parr, p);
    }
    char* text = cJSON_Print(root);
    if (text) {
        if (!WriteWholeFile(m_folder + L"\\" + kIndexName, text))
            LogF(L"Could not save downloads.json (error %lu)", GetLastError());
        cJSON_free(text);
    }
    cJSON_Delete(root);
}

void Downloads::Add(const DownloadedTrack& t)
{
    std::map<std::string, size_t>::const_iterator it = m_byId.find(t.song.id);
    if (it != m_byId.end()) {
        m_tracks[it->second] = t;
    } else {
        m_byId[t.song.id] = m_tracks.size();
        m_tracks.push_back(t);
    }
    Save();
}

bool Downloads::FromOtherServer(const std::string& songId) const
{
    const DownloadedTrack* t = Find(songId);
    return t && !m_server.empty() && t->server != m_server;
}

std::map<std::string, std::vector<std::string> > Downloads::OtherServerSamples(size_t perServer) const
{
    std::map<std::string, std::vector<size_t> > groups;
    for (size_t i = 0; i < m_tracks.size(); ++i)
        if (m_tracks[i].server != m_server)
            groups[m_tracks[i].server].push_back(i);
    std::map<std::string, std::vector<std::string> > out;
    for (std::map<std::string, std::vector<size_t> >::const_iterator g = groups.begin(); g != groups.end(); ++g) {
        const std::vector<size_t>& idx = g->second;
        const size_t n = idx.size() < perServer ? idx.size() : perServer;
        for (size_t k = 0; k < n; ++k)
            out[g->first].push_back(m_tracks[idx[k * idx.size() / n]].song.id);
    }
    return out;
}

int Downloads::Adopt(const std::string& key)
{
    int n = 0;
    for (size_t i = 0; i < m_tracks.size(); ++i)
        if (m_tracks[i].server == key && key != m_server) {
            m_tracks[i].server = m_server;
            ++n;
        }
    if (n)
        Save();
    return n;
}

const DownloadedPlaylist* Downloads::FindPlaylist(const std::string& playlistId) const
{
    for (size_t i = 0; i < m_playlists.size(); ++i)
        if (m_playlists[i].info.id == playlistId)
            return &m_playlists[i];
    return NULL;
}

void Downloads::SavePlaylist(const SubsonicPlaylist& info, const std::vector<std::string>& songIds)
{
    DownloadedPlaylist pl;
    pl.info = info;
    pl.songIds = songIds;
    pl.info.songCount = (int)songIds.size();
    for (size_t i = 0; i < m_playlists.size(); ++i)
        if (m_playlists[i].info.id == info.id) {
            m_playlists[i] = pl;
            Save();
            return;
        }
    m_playlists.push_back(pl);
    Save();
}

void Downloads::RemovePlaylist(const std::string& playlistId)
{
    std::vector<std::string> ids;
    for (size_t i = 0; i < m_playlists.size(); ++i)
        if (m_playlists[i].info.id == playlistId) {
            ids = m_playlists[i].songIds;
            m_playlists.erase(m_playlists.begin() + i);
            break;
        }
    for (size_t i = 0; i < ids.size(); ++i) {
        bool shared = false;
        for (size_t p = 0; p < m_playlists.size() && !shared; ++p)
            for (size_t k = 0; k < m_playlists[p].songIds.size() && !shared; ++k)
                shared = m_playlists[p].songIds[k] == ids[i];
        if (!shared)
            Remove(ids[i]);   // saves the index
    }
    Save();
}

void Downloads::SaveArt(const std::string& coverArtId, const std::string& bytes)
{
    if (!coverArtId.empty() && !bytes.empty())
        WriteWholeFile(ArtPath(coverArtId), bytes);
}

bool Downloads::Remove(const std::string& songId)
{
    for (size_t i = 0; i < m_tracks.size(); ++i)
        if (m_tracks[i].song.id == songId) {
            // A track that is playing is open with delete sharing: it goes when playback ends.
            if (!DeleteFileW(PathOf(m_tracks[i]).c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
                LogF(L"Could not delete %ls (error %lu)", PathOf(m_tracks[i]).c_str(), GetLastError());
            PruneFolders(m_folder, m_tracks[i].file);
            m_tracks.erase(m_tracks.begin() + i);
            Reindex();
            Save();
            return true;
        }
    return false;
}

void Downloads::RemoveAll()
{
    Cancel();
    for (size_t i = 0; i < m_tracks.size(); ++i) {
        DeleteFileW(PathOf(m_tracks[i]).c_str());
        PruneFolders(m_folder, m_tracks[i].file);
    }
    m_tracks.clear();
    m_byId.clear();
    m_playlists.clear();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((m_folder + L"\\art\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                DeleteFileW((m_folder + L"\\art\\" + fd.cFileName).c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    Save();
}

// --- Queue and worker -------------------------------------------------------------------

// True if another download (saved or waiting) already uses this name, or a file we don't
// know about is in the way. Call with m_lock held.
bool Downloads::NameTaken(const std::wstring& file, const std::string& skipId) const
{
    for (size_t i = 0; i < m_tracks.size(); ++i)
        if (m_tracks[i].song.id != skipId && _wcsicmp(m_tracks[i].file.c_str(), file.c_str()) == 0)
            return true;
    for (size_t i = 0; i < m_queue.size(); ++i)
        if (m_queue[i].song.id != skipId && _wcsicmp(m_queue[i].file.c_str(), file.c_str()) == 0)
            return true;
    return GetFileAttributesW((m_folder + L"\\" + file).c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Artist\Album\NN Title.ext, made unique, and short enough for MAX_PATH with room for
// ".part"; falls back to the song id if the folder path is too long for that.
std::wstring Downloads::OrganizedName(const SubsonicSong& s, bool flac, const std::string& skipId) const
{
    const std::wstring ext = flac ? L".flac" : L".mp3";
    const std::wstring dir = CleanPart(s.albumArtist.empty() ? s.artist : s.albumArtist, L"Unknown Artist") +
                             L"\\" + CleanPart(s.album, L"Unknown Album") + L"\\";
    std::wstring num;
    if (s.disc > 1)
        num = Fmt(L"%d-%02d ", s.disc, s.track);
    else if (s.track > 0)
        num = Fmt(L"%02d ", s.track);
    std::wstring title = CleanPart(num + s.title, L"Untitled");
    const size_t room = MAX_PATH - 12 - m_folder.size() - 1 - dir.size() - ext.size();   // 12: ".part", " [abcdef]"
    if (room < 8 || room > MAX_PATH)
        return SafeFileName(s.id) + ext;
    if (title.size() > room)
        title.erase(room);
    std::wstring file = dir + title + ext;
    if (NameTaken(file, skipId))
        file = dir + title + L" [" + Utf8ToWide(s.id.substr(0, 6)) + L"]" + ext;
    return file;
}

int Downloads::Organize(const std::map<std::string, std::wstring>& albumArtists)
{
    // Album artist for old entries: the server's album list, else the artist most of the
    // album's downloaded tracks have (so "X feat. Y" tracks stay with the album).
    std::map<std::string, std::map<std::wstring, int> > votes;
    for (size_t i = 0; i < m_tracks.size(); ++i)
        ++votes[m_tracks[i].song.albumId][m_tracks[i].song.artist];
    int moved = 0;
    EnterCriticalSection(&m_lock);
    for (size_t i = 0; i < m_tracks.size(); ++i) {
        DownloadedTrack& t = m_tracks[i];
        if (t.file.find(L'\\') != std::wstring::npos)
            continue;   // already in folders
        if (t.song.albumArtist.empty()) {
            std::map<std::string, std::wstring>::const_iterator a = albumArtists.find(t.song.albumId);
            if (a != albumArtists.end() && !a->second.empty()) {
                t.song.albumArtist = a->second;
            } else {
                const std::map<std::wstring, int>& v = votes[t.song.albumId];
                int best = 0;
                for (std::map<std::wstring, int>::const_iterator it = v.begin(); it != v.end(); ++it)
                    if (it->second > best || (it->second == best && it->first.size() < t.song.albumArtist.size())) {
                        best = it->second;
                        t.song.albumArtist = it->first;
                    }
            }
        }
        const std::wstring file = OrganizedName(t.song, t.flac, t.song.id);
        if (file.find(L'\\') == std::wstring::npos)
            continue;   // the folder path is too long for folders
        MakeFolders(m_folder, file);
        if (MoveFileW(PathOf(t).c_str(), (m_folder + L"\\" + file).c_str())) {
            t.file = file;
            ++moved;
        } else {
            LogF(L"Could not move %ls to %ls (error %lu)", t.file.c_str(), file.c_str(), GetLastError());
        }
    }
    LeaveCriticalSection(&m_lock);
    if (moved)
        Save();
    return moved;
}

int Downloads::Enqueue(const std::vector<DownloadItem>& items)
{
    int added = 0;
    EnterCriticalSection(&m_lock);
    for (size_t i = 0; i < items.size(); ++i) {
        const std::string& id = items[i].song.id;
        bool skip = Find(id) != NULL;
        for (size_t k = 0; k < m_queue.size() && !skip; ++k)
            skip = m_queue[k].song.id == id;
        if (!skip) {
            m_queue.push_back(items[i]);
            m_queue.back().file = OrganizedName(items[i].song, items[i].flac, id);
            ++added;
        }
    }
    bool start = added > 0 && !m_running;
    if (start) {
        m_running = true;
        ResetEvent(m_idle);
    }
    LeaveCriticalSection(&m_lock);
    if (start) {
        HANDLE t = (HANDLE)_beginthreadex(NULL, 0, ThreadMain, this, 0, NULL);
        if (t) {
            CloseHandle(t);
        } else {
            EnterCriticalSection(&m_lock);
            m_queue.clear();
            m_running = false;
            SetEvent(m_idle);
            LeaveCriticalSection(&m_lock);
            LogF(L"Could not start the download thread");
            return 0;
        }
    }
    return added;
}

bool Downloads::IsQueued(const std::string& songId) const
{
    EnterCriticalSection(const_cast<CRITICAL_SECTION*>(&m_lock));
    bool found = false;
    for (size_t k = 0; k < m_queue.size() && !found; ++k)
        found = m_queue[k].song.id == songId;
    LeaveCriticalSection(const_cast<CRITICAL_SECTION*>(&m_lock));
    return found;
}

int Downloads::Pending() const
{
    EnterCriticalSection(const_cast<CRITICAL_SECTION*>(&m_lock));
    int n = (int)m_queue.size();
    LeaveCriticalSection(const_cast<CRITICAL_SECTION*>(&m_lock));
    return n;
}

void Downloads::Cancel()
{
    EnterCriticalSection(&m_lock);
    // The front entry is the one downloading; the worker removes it when it stops.
    if (m_queue.size() > 1)
        m_queue.erase(m_queue.begin() + 1, m_queue.end());
    if (m_running) {
        InterlockedExchange(&m_cancel, 1);
        if (m_req)
            m_req->Abort();
    }
    LeaveCriticalSection(&m_lock);
}

void Downloads::Shutdown()
{
    m_notify = NULL;   // nothing more to report
    Cancel();
    if (WaitForSingleObject(m_idle, 5000) != WAIT_OBJECT_0)
        LogF(L"The download thread did not stop in time");
}

void Downloads::Post(DownloadEvent* e)
{
    HWND w = m_notify;
    if (!w || !PostMessageW(w, m_message, 0, (LPARAM)e))
        delete e;
}

unsigned __stdcall Downloads::ThreadMain(void* self)
{
    ((Downloads*)self)->Run();
    return 0;
}

void Downloads::Run()
{
    for (;;) {
        EnterCriticalSection(&m_lock);
        if (m_queue.empty()) {
            m_running = false;
            InterlockedExchange(&m_cancel, 0);
            SetEvent(m_idle);
            LeaveCriticalSection(&m_lock);
            DownloadEvent* e = new DownloadEvent;
            e->kind = DL_IDLE;
            e->remaining = 0;
            Post(e);
            return;
        }
        DownloadItem item = m_queue.front();
        const int remaining = (int)m_queue.size() - 1;
        LeaveCriticalSection(&m_lock);

        DownloadEvent* started = new DownloadEvent;
        started->kind = DL_STARTED;
        started->title = item.song.title;
        started->remaining = remaining;
        Post(started);

        DownloadEvent* e = new DownloadEvent;
        e->title = item.song.title;
        bool ok = Fetch(item, e->track, e->error);

        EnterCriticalSection(&m_lock);
        const bool cancelled = InterlockedExchange(&m_cancel, 0) != 0;
        if (!m_queue.empty())
            m_queue.pop_front();
        e->remaining = (int)m_queue.size();
        LeaveCriticalSection(&m_lock);
        if (cancelled && ok) {
            // Finished just as it was cancelled: keep it, nothing is lost by that.
        } else if (cancelled) {
            delete e;
            continue;
        }
        e->kind = ok ? DL_DONE : DL_FAILED;
        Post(e);
    }
}

bool Downloads::Fetch(const DownloadItem& item, DownloadedTrack& done, std::wstring& err)
{
    EnterCriticalSection(&m_lock);
    const std::wstring folder = m_workFolder;
    LeaveCriticalSection(&m_lock);
    const std::wstring name = item.file;
    const std::wstring path = folder + L"\\" + name, part = path + L".part";
    MakeFolders(folder, name);

    HttpRequest req;
    EnterCriticalSection(&m_lock);
    m_req = &req;
    const bool cancelled = m_cancel != 0;
    LeaveCriticalSection(&m_lock);
    HttpResult r = cancelled ? HTTP_ERR_CANCELLED : req.Open(item.url, item.opts);

    bool ok = false;
    unsigned long long total = 0;
    if (r != HTTP_OK) {
        err = HttpDescribe(r, req.StatusCode(), req.LastError(), HttpIsHttpsUrl(item.url));
    } else {
        HANDLE f = CreateFileW(part.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f == INVALID_HANDLE_VALUE) {
            err = Fmt(L"Could not create %ls (error %lu).", part.c_str(), GetLastError());
        } else {
            static char buf[64 * 1024];   // only this thread downloads
            ok = true;
            bool first = true;
            for (;;) {
                int n = req.Read(buf, sizeof(buf));
                if (n == 0)
                    break;
                if (n < 0) {
                    err = Fmt(L"The connection dropped after %I64u KB (WinINet error %lu).", total / 1024,
                              req.LastError());
                    ok = false;
                    break;
                }
                // Navidrome reports a failed stream as a short JSON or XML error body.
                if (first && (buf[0] == '{' || buf[0] == '<')) {
                    err = L"The server sent an error instead of the track: " +
                          Utf8ToWide(std::string(buf, n < 200 ? n : 200));
                    ok = false;
                    break;
                }
                first = false;
                DWORD put = 0;
                if (!WriteFile(f, buf, (DWORD)n, &put, NULL) || put != (DWORD)n) {
                    err = Fmt(L"Could not write the file (error %lu); is the disk full?", GetLastError());
                    ok = false;
                    break;
                }
                total += (unsigned)n;
            }
            CloseHandle(f);
            if (ok && req.ContentLength() && total != req.ContentLength()) {
                err = Fmt(L"Only %I64u of %lu bytes arrived.", total, req.ContentLength());
                ok = false;
            }
            if (ok && total == 0) {
                err = L"The server sent an empty file.";
                ok = false;
            }
            if (ok && !MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                err = Fmt(L"Could not rename the finished file (error %lu).", GetLastError());
                ok = false;
            }
            if (!ok) {
                DeleteFileW(part.c_str());
                PruneFolders(folder, name);
            }
        }
    }
    EnterCriticalSection(&m_lock);
    m_req = NULL;
    LeaveCriticalSection(&m_lock);

    // The cover, once per album; a missing cover is no reason to fail the track.
    if (ok && !item.artUrl.empty() && !item.song.coverArt.empty()) {
        const std::wstring art = folder + L"\\art\\" + SafeFileName(item.song.coverArt);
        if (GetFileAttributesW(art.c_str()) == INVALID_FILE_ATTRIBUTES) {
            std::string bytes;
            DWORD status = 0, winErr = 0;
            if (HttpGetAll(item.artUrl, item.opts, bytes, &status, &winErr) == HTTP_OK && !bytes.empty())
                WriteWholeFile(art, bytes);
            else
                LogF(L"Download: no cover for \"%ls\" (HTTP %lu, WinINet error %lu)", item.song.title.c_str(),
                     status, winErr);
        }
    }

    if (ok) {
        done.song = item.song;
        done.file = name;
        done.flac = item.flac;
        done.bytes = total;
        done.server = item.server;
    }
    return ok;
}
