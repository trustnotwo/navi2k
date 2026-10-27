#include "subsonic.h"
#include "util.h"

#include <wincrypt.h>
#include <algorithm>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "cJSON.h"

static const char* kApiVersion = "1.16.1";
static const char* kClientName = "Navi2KPlayer";

static std::string HexEncode(const BYTE* p, size_t n)
{
    static const char hex[] = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        s += hex[p[i] >> 4];
        s += hex[p[i] & 15];
    }
    return s;
}

bool SubsonicMakeToken(const std::string& passwordUtf8, std::string& token, std::string& salt)
{
    HCRYPTPROV prov = 0;
    if (!CryptAcquireContextW(&prov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
        return false;

    BYTE rnd[8];
    bool ok = CryptGenRandom(prov, sizeof(rnd), rnd) != 0;
    if (ok) {
        salt = HexEncode(rnd, sizeof(rnd));
        std::string input = passwordUtf8 + salt;
        HCRYPTHASH hash = 0;
        ok = CryptCreateHash(prov, CALG_MD5, 0, 0, &hash) != 0;
        if (ok) {
            BYTE digest[16];
            DWORD len = sizeof(digest);
            ok = CryptHashData(hash, (const BYTE*)input.data(), (DWORD)input.size(), 0) &&
                 CryptGetHashParam(hash, HP_HASHVAL, digest, &len, 0);
            if (ok)
                token = HexEncode(digest, sizeof(digest));
            CryptDestroyHash(hash);
        }
    }
    CryptReleaseContext(prov, 0);
    return ok;
}

// --- JSON helpers -------------------------------------------------------------

static cJSON* JGet(cJSON* obj, const char* key)
{
    return cJSON_GetObjectItemCaseSensitive(obj, key);
}

static std::wstring JStr(cJSON* obj, const char* key)
{
    cJSON* v = JGet(obj, key);
    return (cJSON_IsString(v) && v->valuestring) ? Utf8ToWide(v->valuestring) : std::wstring();
}

static int JInt(cJSON* obj, const char* key)
{
    cJSON* v = JGet(obj, key);
    return cJSON_IsNumber(v) ? v->valueint : 0;
}

// Navidrome ids are strings; some Subsonic servers use numbers.
static std::string JId(cJSON* obj, const char* key)
{
    cJSON* v = JGet(obj, key);
    if (cJSON_IsString(v) && v->valuestring)
        return v->valuestring;
    if (cJSON_IsNumber(v)) {
        char buf[32];
        _snprintf(buf, sizeof(buf) - 1, "%.0f", v->valuedouble);
        buf[sizeof(buf) - 1] = 0;
        return buf;
    }
    return std::string();
}

// One "song"/"entry" object from getAlbum, getSong or getPlaylist.
static SubsonicSong ParseSong(cJSON* s)
{
    SubsonicSong song;
    song.id = JId(s, "id");
    song.coverArt = JId(s, "coverArt");
    song.title = JStr(s, "title");
    song.artist = JStr(s, "artist");
    song.album = JStr(s, "album");
    song.suffix = JStr(s, "suffix");
    song.track = JInt(s, "track");
    song.disc = JInt(s, "discNumber");
    song.duration = JInt(s, "duration");
    song.bitRate = JInt(s, "bitRate");
    song.userRating = JInt(s, "userRating");
    return song;
}

// One "album" object from getAlbumList2 or getArtist.
static SubsonicAlbum ParseAlbum(cJSON* a)
{
    SubsonicAlbum al;
    al.id = JId(a, "id");
    al.coverArt = JId(a, "coverArt");
    al.name = JStr(a, "name");
    al.artist = JStr(a, "artist");
    al.year = JInt(a, "year");
    al.songCount = JInt(a, "songCount");
    return al;
}

// Plain-HTTP fallback is only allowed to private/LAN hosts, so the token never
// crosses the internet unencrypted.
static bool IsLanUrl(const std::wstring& url)
{
    URL_COMPONENTSW uc;
    ZeroMemory(&uc, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.dwHostNameLength = 1;
    if (!InternetCrackUrlW(url.c_str(), (DWORD)url.size(), 0, &uc) || !uc.lpszHostName)
        return false;
    std::wstring host(uc.lpszHostName, uc.dwHostNameLength);

    unsigned a, b, c, d;
    if (swscanf(host.c_str(), L"%u.%u.%u.%u", &a, &b, &c, &d) == 4)
        return a == 10 || a == 127 || (a == 192 && b == 168) ||
               (a == 172 && b >= 16 && b <= 31) || (a == 169 && b == 254);
    if (host.find(L'.') == std::wstring::npos)
        return true;   // single-label name such as "nas"
    return EndsWithNoCase(host, L".local") || EndsWithNoCase(host, L".lan") ||
           EndsWithNoCase(host, L".home.arpa");
}

// --- Client -------------------------------------------------------------------

void SubsonicClient::Configure(const SubsonicConfig& cfg)
{
    m_cfg = cfg;
    m_activeBase = cfg.url;
}

HttpOptions SubsonicClient::Options() const
{
    HttpOptions o;
    o.allowUntrustedCert = m_cfg.allowUntrustedCert;
    return o;
}

std::wstring SubsonicClient::BuildUrl(const char* method, const std::string& extraQuery) const
{
    std::wstring base = m_activeBase;
    while (!base.empty() && base[base.size() - 1] == L'/')
        base.erase(base.size() - 1);

    std::string q = std::string("u=") + UrlEncode(m_cfg.user) + "&t=" + m_cfg.token +
                    "&s=" + m_cfg.salt + "&v=" + kApiVersion + "&c=" + kClientName + extraQuery;
    return base + L"/rest/" + Utf8ToWide(method) + L"?" + Utf8ToWide(q);
}

bool SubsonicClient::Call(const char* method, const std::string& extraQuery, cJSON** root,
                          cJSON** response, HttpResult* httpResult, std::wstring& err)
{
    *root = NULL;
    *response = NULL;
    std::string body;
    DWORD status = 0, winErr = 0;
    LogF(L"API %hs", method);   // never log the URL: it carries the auth token
    HttpResult r = HttpGetAll(BuildUrl(method, extraQuery + "&f=json"), Options(), body, &status, &winErr);
    if (httpResult)
        *httpResult = r;
    if (r != HTTP_OK) {
        err = HttpDescribe(r, status, winErr, HttpIsHttpsUrl(m_activeBase));
        LogF(L"API %hs failed: result=%d status=%lu winerr=%lu", method, (int)r, status, winErr);
        return false;
    }

    cJSON* json = cJSON_Parse(body.c_str());
    cJSON* sr = json ? JGet(json, "subsonic-response") : NULL;
    if (!cJSON_IsObject(sr)) {
        cJSON_Delete(json);
        err = L"The server's reply is not a Subsonic API response. Is Url pointing at Navidrome?";
        return false;
    }

    cJSON* st = JGet(sr, "status");
    if (!(cJSON_IsString(st) && st->valuestring && strcmp(st->valuestring, "ok") == 0)) {
        cJSON* e = JGet(sr, "error");
        int code = JInt(e, "code");
        err = Fmt(L"Server error %d: %ls", code, JStr(e, "message").c_str());
        if (code == 40)
            err += L"\r\n\r\nCheck User and Password in Navi2KPlayer.ini "
                   L"(delete Token/Salt and enter Password again).";
        cJSON_Delete(json);
        return false;
    }

    *root = json;
    *response = sr;
    return true;
}

bool SubsonicClient::Connect(std::wstring& notice)
{
    notice.clear();
    m_activeBase = m_cfg.url;

    cJSON *root, *sr;
    HttpResult r = HTTP_OK;
    std::wstring err;
    if (Call("ping", "", &root, &sr, &r, err)) {
        cJSON_Delete(root);
        return true;
    }

    if (!HttpIsTlsFailure(r) || m_cfg.lanUrl.empty() || !m_cfg.allowHttpFallback) {
        notice = err;
        return false;
    }
    if (!IsLanUrl(m_cfg.lanUrl)) {
        notice = err + L"\r\n\r\nLanUrl was not used: unencrypted fallback is only allowed "
                       L"to addresses on your local network.";
        return false;
    }

    LogF(L"Secure connection failed (result %d); trying LAN fallback", (int)r);
    m_activeBase = m_cfg.lanUrl;
    std::wstring err2;
    if (Call("ping", "", &root, &sr, NULL, err2)) {
        cJSON_Delete(root);
        notice = L"The secure connection failed, so Navi2K Player is using the LAN address " +
                 m_cfg.lanUrl + L" (unencrypted).";
        return true;
    }
    m_activeBase = m_cfg.url;
    notice = err + L"\r\n\r\nThe LAN fallback address also failed: " + err2;
    return false;
}

bool SubsonicClient::GetAlbumList(std::vector<SubsonicAlbum>& out, std::wstring& err)
{
    static const int kPage = 500;   // Subsonic maximum
    out.clear();
    for (int offset = 0; offset < 50000; offset += kPage) {
        char extra[96];
        _snprintf(extra, sizeof(extra) - 1, "&type=alphabeticalByArtist&size=%d&offset=%d", kPage, offset);
        extra[sizeof(extra) - 1] = 0;

        cJSON *root, *sr;
        if (!Call("getAlbumList2", extra, &root, &sr, NULL, err))
            return false;

        int n = 0;
        cJSON* a;
        cJSON_ArrayForEach(a, JGet(JGet(sr, "albumList2"), "album")) {
            out.push_back(ParseAlbum(a));
            ++n;
        }
        cJSON_Delete(root);
        if (n < kPage)
            break;
    }
    return true;
}

bool SubsonicClient::GetAlbum(const std::string& albumId, std::vector<SubsonicSong>& out, std::wstring& err)
{
    out.clear();
    cJSON *root, *sr;
    if (!Call("getAlbum", "&id=" + UrlEncode(albumId), &root, &sr, NULL, err))
        return false;

    cJSON* s;
    cJSON_ArrayForEach(s, JGet(JGet(sr, "album"), "song"))
        out.push_back(ParseSong(s));
    cJSON_Delete(root);
    return true;
}

// --- All songs ----------------------------------------------------------------------

namespace {
// Tags sometimes carry stray leading spaces; don't let them sort to the top.
const wchar_t* SortKey(const std::wstring& s)
{
    const wchar_t* p = s.c_str();
    while (*p == L' ' || *p == L'\t')
        ++p;
    return p;
}

struct SongOrder {
    bool operator()(const SubsonicSong& a, const SubsonicSong& b) const
    {
        int c = _wcsicmp(SortKey(a.artist), SortKey(b.artist));
        if (c)
            return c < 0;
        c = _wcsicmp(SortKey(a.album), SortKey(b.album));
        if (c)
            return c < 0;
        if (a.disc != b.disc)
            return a.disc < b.disc;
        return a.track < b.track;
    }
};
}

bool SubsonicClient::GetAllSongs(std::vector<SubsonicSong>& out, std::wstring& err)
{
    static const int kPage = 500;
    out.clear();
    for (int offset = 0; offset < 500000; offset += kPage) {
        char extra[128];
        _snprintf(extra, sizeof(extra) - 1, "&query=&artistCount=0&albumCount=0&songCount=%d&songOffset=%d",
                  kPage, offset);
        extra[sizeof(extra) - 1] = 0;
        cJSON *root, *sr;
        if (!Call("search3", extra, &root, &sr, NULL, err))
            return false;
        int n = 0;
        cJSON* s;
        cJSON_ArrayForEach(s, JGet(JGet(sr, "searchResult3"), "song")) {
            out.push_back(ParseSong(s));
            ++n;
        }
        cJSON_Delete(root);
        if (n < kPage)
            break;
    }
    std::stable_sort(out.begin(), out.end(), SongOrder());
    return true;
}

// --- Artists ------------------------------------------------------------------------

bool SubsonicClient::GetArtists(std::vector<SubsonicArtist>& out, std::wstring& err)
{
    out.clear();
    cJSON *root, *sr;
    if (!Call("getArtists", "", &root, &sr, NULL, err))
        return false;
    // { "artists": { "index": [ { "name": "A", "artist": [ ... ] }, ... ] } }
    cJSON* index;
    cJSON_ArrayForEach(index, JGet(JGet(sr, "artists"), "index")) {
        cJSON* a;
        cJSON_ArrayForEach(a, JGet(index, "artist")) {
            SubsonicArtist ar;
            ar.id = JId(a, "id");
            ar.coverArt = JId(a, "coverArt");
            ar.name = JStr(a, "name");
            ar.albumCount = JInt(a, "albumCount");
            out.push_back(ar);
        }
    }
    cJSON_Delete(root);
    return true;
}

bool SubsonicClient::GetArtist(const std::string& artistId, std::vector<SubsonicAlbum>& albums, std::wstring& err)
{
    albums.clear();
    cJSON *root, *sr;
    if (!Call("getArtist", "&id=" + UrlEncode(artistId), &root, &sr, NULL, err))
        return false;
    cJSON* a;
    cJSON_ArrayForEach(a, JGet(JGet(sr, "artist"), "album"))
        albums.push_back(ParseAlbum(a));
    cJSON_Delete(root);
    return true;
}

// --- Ratings ------------------------------------------------------------------------

bool SubsonicClient::GetSong(const std::string& songId, SubsonicSong& out, std::wstring& err)
{
    cJSON *root, *sr;
    if (!Call("getSong", "&id=" + UrlEncode(songId), &root, &sr, NULL, err))
        return false;
    cJSON* s = JGet(sr, "song");
    bool ok = cJSON_IsObject(s);
    if (ok)
        out = ParseSong(s);
    else
        err = L"The server returned no song.";
    cJSON_Delete(root);
    return ok;
}

bool SubsonicClient::SetRating(const std::string& songId, int rating, std::wstring& err)
{
    if (rating < 0 || rating > 5)
        return false;
    char r[16];
    _snprintf(r, sizeof(r) - 1, "&rating=%d", rating);
    r[sizeof(r) - 1] = 0;
    return CallNoResult("setRating", "&id=" + UrlEncode(songId) + r, err);
}

// --- Playlists ----------------------------------------------------------------------

bool SubsonicClient::CallNoResult(const char* method, const std::string& params, std::wstring& err)
{
    cJSON *root, *sr;
    bool ok = Call(method, params, &root, &sr, NULL, err);
    if (ok)
        cJSON_Delete(root);
    return ok;
}

static std::string SongIdParams(const std::vector<std::string>& ids, const char* key)
{
    std::string p;
    for (size_t i = 0; i < ids.size(); ++i)
        p += std::string("&") + key + "=" + UrlEncode(ids[i]);
    return p;
}

// Playlist writes are plain GETs: Windows 2000's WinINet (IE5/6) fails form POSTs to Navidrome
// with error 12150. To stay under that WinINet's ~2 KB URL limit, long track lists are split
// into groups that each fit in one request.
static const size_t kMaxUrlChars = 1800;

std::vector<std::vector<std::string> > SubsonicClient::SplitIds(const char* method, const std::string& fixedParams,
                                                                const std::vector<std::string>& ids,
                                                                const char* key) const
{
    const size_t base = BuildUrl(method, fixedParams + "&f=json").size();
    const size_t budget = base < kMaxUrlChars ? kMaxUrlChars - base : 0;
    std::vector<std::vector<std::string> > groups(1);
    size_t used = 0;
    for (size_t i = 0; i < ids.size(); ++i) {
        size_t len = 2 + strlen(key) + UrlEncode(ids[i]).size();   // "&key=id"
        if (!groups.back().empty() && used + len > budget) {
            groups.push_back(std::vector<std::string>());
            used = 0;
        }
        groups.back().push_back(ids[i]);
        used += len;
    }
    return groups;
}

static bool JBool(cJSON* obj, const char* key)
{
    return cJSON_IsTrue(JGet(obj, key));
}

bool SubsonicClient::GetPlaylists(std::vector<SubsonicPlaylist>& out, std::wstring& err)
{
    out.clear();
    cJSON *root, *sr;
    if (!Call("getPlaylists", "", &root, &sr, NULL, err))
        return false;
    cJSON* p;
    cJSON_ArrayForEach(p, JGet(JGet(sr, "playlists"), "playlist")) {
        SubsonicPlaylist pl;
        pl.id = JId(p, "id");
        pl.coverArt = JId(p, "coverArt");
        pl.name = JStr(p, "name");
        pl.owner = JStr(p, "owner");
        pl.songCount = JInt(p, "songCount");
        pl.duration = JInt(p, "duration");
        pl.readOnly = JBool(p, "readonly");   // OpenSubsonic field; Navidrome sets it for smart playlists
        out.push_back(pl);
    }
    cJSON_Delete(root);
    return true;
}

bool SubsonicClient::GetPlaylist(const std::string& playlistId, std::vector<SubsonicSong>& out, std::wstring& err)
{
    out.clear();
    cJSON *root, *sr;
    if (!Call("getPlaylist", "&id=" + UrlEncode(playlistId), &root, &sr, NULL, err))
        return false;
    cJSON* s;
    cJSON_ArrayForEach(s, JGet(JGet(sr, "playlist"), "entry"))
        out.push_back(ParseSong(s));
    cJSON_Delete(root);
    return true;
}

// The first group goes with createPlaylist; the rest are appended with updatePlaylist.
// Splitting makes a long create/replace non-atomic, so a failure part-way says so.
bool SubsonicClient::CreateOrReplace(const std::string& fixedParams, const std::vector<std::string>& songIds,
                                     std::string* playlistId, std::wstring& err)
{
    std::vector<std::vector<std::string> > groups = SplitIds("createPlaylist", fixedParams, songIds, "songId");
    cJSON *root, *sr;
    if (!Call("createPlaylist", fixedParams + SongIdParams(groups[0], "songId"), &root, &sr, NULL, err))
        return false;
    std::string id = JId(JGet(sr, "playlist"), "id");   // API 1.14+ returns the playlist
    cJSON_Delete(root);
    if (playlistId && playlistId->empty())
        *playlistId = id;
    if (groups.size() == 1)
        return true;
    if (!playlistId || playlistId->empty()) {
        err = L"The server didn't return the playlist's id, so only the first tracks were saved.";
        return false;
    }
    std::vector<std::string> rest(songIds.begin() + groups[0].size(), songIds.end());
    if (!AddToPlaylist(*playlistId, rest, err)) {
        err = Fmt(L"Only the first %u of %u tracks were saved: ", (unsigned)groups[0].size(),
                  (unsigned)songIds.size()) + err;
        return false;
    }
    return true;
}

bool SubsonicClient::CreatePlaylist(const std::wstring& name, const std::vector<std::string>& songIds,
                                    std::string* newId, std::wstring& err)
{
    std::string id;
    bool ok = CreateOrReplace("&name=" + UrlEncode(WideToUtf8(name)), songIds, &id, err);
    if (newId)
        *newId = id;
    return ok;
}

bool SubsonicClient::SetPlaylistSongs(const std::string& playlistId, const std::vector<std::string>& songIds,
                                      std::wstring& err)
{
    // createPlaylist with playlistId overwrites that playlist's tracks.
    std::string id = playlistId;
    return CreateOrReplace("&playlistId=" + UrlEncode(playlistId), songIds, &id, err);
}

bool SubsonicClient::RenamePlaylist(const std::string& playlistId, const std::wstring& name, std::wstring& err)
{
    return CallNoResult("updatePlaylist",
                        "&playlistId=" + UrlEncode(playlistId) + "&name=" + UrlEncode(WideToUtf8(name)), err);
}

bool SubsonicClient::AddToPlaylist(const std::string& playlistId, const std::vector<std::string>& songIds,
                                   std::wstring& err)
{
    if (songIds.empty())
        return true;
    const std::string fixed = "&playlistId=" + UrlEncode(playlistId);
    std::vector<std::vector<std::string> > groups = SplitIds("updatePlaylist", fixed, songIds, "songIdToAdd");
    for (size_t i = 0; i < groups.size(); ++i)
        if (!CallNoResult("updatePlaylist", fixed + SongIdParams(groups[i], "songIdToAdd"), err))
            return false;
    return true;
}

bool SubsonicClient::RemoveFromPlaylist(const std::string& playlistId, int index, std::wstring& err)
{
    char p[40];
    _snprintf(p, sizeof(p) - 1, "&songIndexToRemove=%d", index);
    p[sizeof(p) - 1] = 0;
    return CallNoResult("updatePlaylist", "&playlistId=" + UrlEncode(playlistId) + p, err);
}

bool SubsonicClient::DeletePlaylist(const std::string& playlistId, std::wstring& err)
{
    return CallNoResult("deletePlaylist", "&id=" + UrlEncode(playlistId), err);
}

bool SubsonicClient::CanEdit(const SubsonicPlaylist& pl) const
{
    return !pl.readOnly && WideToUtf8(pl.owner) == m_cfg.user;
}

std::wstring SubsonicClient::StreamUrl(const std::string& songId, int timeOffsetSec) const
{
    char extra[96];
    _snprintf(extra, sizeof(extra) - 1, "&format=mp3&maxBitRate=%d", m_cfg.maxBitRate);
    extra[sizeof(extra) - 1] = 0;
    std::string q = "&id=" + UrlEncode(songId) + extra;
    if (timeOffsetSec > 0) {
        _snprintf(extra, sizeof(extra) - 1, "&timeOffset=%d", timeOffsetSec);
        q += extra;
    }
    return BuildUrl("stream", q);
}

bool SubsonicClient::ServerTranscodes(const SubsonicSong& song) const
{
    bool mp3 = _wcsicmp(song.suffix.c_str(), L"mp3") == 0;
    return !(mp3 && song.bitRate > 0 && song.bitRate <= m_cfg.maxBitRate);
}

bool SubsonicClient::GetCoverArt(const std::string& coverArtId, int size, std::string& bytes, std::wstring& err)
{
    char extra[32];
    _snprintf(extra, sizeof(extra) - 1, "&size=%d", size);
    extra[sizeof(extra) - 1] = 0;
    LogF(L"API getCoverArt");
    DWORD status = 0, winErr = 0;
    HttpResult r = HttpGetAll(BuildUrl("getCoverArt", "&id=" + UrlEncode(coverArtId) + extra),
                              Options(), bytes, &status, &winErr);
    if (r != HTTP_OK) {
        err = HttpDescribe(r, status, winErr, HttpIsHttpsUrl(m_activeBase));
        return false;
    }
    // On failure the server answers 200 with a JSON/XML error body instead of an image.
    if (bytes.size() < 8 || bytes[0] == '{' || bytes[0] == '<') {
        err = L"The server returned no image for this album.";
        return false;
    }
    return true;
}
