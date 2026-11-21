#include "subsonic.h"
#include "util.h"

#include <algorithm>
#include <process.h>
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

// MD5 (RFC 1321) for the Subsonic token: Windows 95 may have no CryptoAPI.
static void Md5(const std::string& msg, BYTE digest[16])
{
    static const unsigned kShift[64] = { 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                         5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                         4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                         6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21 };
    static const unsigned kK[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };
    unsigned h[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
    std::string m = msg;
    const unsigned long long bits = (unsigned long long)msg.size() * 8;
    m += (char)0x80;
    while (m.size() % 64 != 56)
        m += (char)0;
    for (int i = 0; i < 8; ++i)
        m += (char)(bits >> (8 * i));
    for (size_t off = 0; off < m.size(); off += 64) {
        unsigned w[16];
        for (int i = 0; i < 16; ++i) {
            const unsigned char* b = (const unsigned char*)m.data() + off + i * 4;
            w[i] = b[0] | (b[1] << 8) | (b[2] << 16) | ((unsigned)b[3] << 24);
        }
        unsigned a = h[0], b = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 64; ++i) {
            unsigned f, g;
            if (i < 16) {
                f = (b & c) | (~b & d);
                g = i;
            } else if (i < 32) {
                f = (d & b) | (~d & c);
                g = (5 * i + 1) % 16;
            } else if (i < 48) {
                f = b ^ c ^ d;
                g = (3 * i + 5) % 16;
            } else {
                f = c ^ (b | ~d);
                g = (7 * i) % 16;
            }
            unsigned t = d;
            d = c;
            c = b;
            unsigned x = a + f + kK[i] + w[g];
            b = b + ((x << kShift[i]) | (x >> (32 - kShift[i])));
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
    }
    for (int i = 0; i < 16; ++i)
        digest[i] = (BYTE)(h[i / 4] >> (8 * (i % 4)));
}

// The salt only has to differ between logins, so the CryptoAPI is used when it's there
// (Windows 98 and later) and otherwise timers and addresses are mixed through MD5.
static void RandomBytes(BYTE* out, size_t n)
{
    typedef BOOL (WINAPI* AcquireFn)(ULONG_PTR*, LPCSTR, LPCSTR, DWORD, DWORD);
    typedef BOOL (WINAPI* GenFn)(ULONG_PTR, DWORD, BYTE*);
    typedef BOOL (WINAPI* ReleaseFn)(ULONG_PTR, DWORD);
    HMODULE adv = LoadLibraryA("advapi32.dll");
    AcquireFn acquire = adv ? (AcquireFn)GetProcAddress(adv, "CryptAcquireContextA") : NULL;
    GenFn gen = adv ? (GenFn)GetProcAddress(adv, "CryptGenRandom") : NULL;
    ReleaseFn release = adv ? (ReleaseFn)GetProcAddress(adv, "CryptReleaseContext") : NULL;
    ULONG_PTR prov = 0;
    bool done = false;
    if (acquire && gen && release && acquire(&prov, NULL, NULL, 1 /* PROV_RSA_FULL */, 0xF0000000 /* VERIFYCONTEXT */)) {
        done = gen(prov, (DWORD)n, out) != 0;
        release(prov, 0);
    }
    if (adv)
        FreeLibrary(adv);
    if (done)
        return;
    LARGE_INTEGER pc;
    QueryPerformanceCounter(&pc);
    SYSTEMTIME st;
    GetSystemTime(&st);
    char seed[128];
    _snprintf(seed, sizeof(seed) - 1, "%lu-%lu-%lu-%lu-%p-%u", GetTickCount(), (unsigned long)pc.LowPart,
              (unsigned long)GetCurrentProcessId(), (unsigned long)GetCurrentThreadId(), (void*)&pc,
              (unsigned)st.wMilliseconds);
    seed[sizeof(seed) - 1] = 0;
    BYTE digest[16];
    Md5(seed, digest);
    for (size_t i = 0; i < n; ++i)
        out[i] = digest[i % 16];
}

bool SubsonicMakeToken(const std::string& passwordUtf8, std::string& token, std::string& salt)
{
    BYTE rnd[8];
    RandomBytes(rnd, sizeof(rnd));
    salt = HexEncode(rnd, sizeof(rnd));
    BYTE digest[16];
    Md5(passwordUtf8 + salt, digest);
    token = HexEncode(digest, sizeof(digest));
    return true;
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
    song.albumArtist = JStr(s, "displayAlbumArtist");
    song.albumId = JId(s, "albumId");
    song.artistId = JId(s, "artistId");
    song.suffix = JStr(s, "suffix");
    song.track = JInt(s, "track");
    song.disc = JInt(s, "discNumber");
    song.duration = JInt(s, "duration");
    song.bitRate = JInt(s, "bitRate");
    song.bitDepth = JInt(s, "bitDepth");             // OpenSubsonic fields (Navidrome 0.53+)
    song.samplingRate = JInt(s, "samplingRate");
    song.channelCount = JInt(s, "channelCount");
    song.userRating = JInt(s, "userRating");
    song.starred = JGet(s, "starred") != NULL;   // present (a timestamp) only when starred
    cJSON* g;
    cJSON_ArrayForEach(g, JGet(s, "genres")) {   // Navidrome: every genre; "genre" is the first
        const std::wstring name = JStr(g, "name");
        if (!name.empty())
            song.genres += name + L"\n";
    }
    if (song.genres.empty() && !JStr(s, "genre").empty())
        song.genres = JStr(s, "genre") + L"\n";
    return song;
}

void SubsonicSong::swap(SubsonicSong& o)
{
    id.swap(o.id);
    coverArt.swap(o.coverArt);
    albumId.swap(o.albumId);
    artistId.swap(o.artistId);
    title.swap(o.title);
    artist.swap(o.artist);
    album.swap(o.album);
    suffix.swap(o.suffix);
    albumArtist.swap(o.albumArtist);
    std::swap(track, o.track);
    std::swap(disc, o.disc);
    std::swap(duration, o.duration);
    std::swap(bitRate, o.bitRate);
    std::swap(bitDepth, o.bitDepth);
    std::swap(samplingRate, o.samplingRate);
    std::swap(channelCount, o.channelCount);
    std::swap(userRating, o.userRating);
    std::swap(starred, o.starred);
    genres.swap(o.genres);
}

// One "album" object from getAlbumList2 or getArtist.
static SubsonicAlbum ParseAlbum(cJSON* a)
{
    SubsonicAlbum al;
    al.id = JId(a, "id");
    al.coverArt = JId(a, "coverArt");
    al.name = JStr(a, "name");
    al.artist = JStr(a, "artist");
    std::string artistId = JId(a, "artistId");
    if (!artistId.empty())
        al.artistIds.push_back(artistId);
    cJSON* ar;
    cJSON_ArrayForEach(ar, JGet(a, "artists")) {
        std::string id = JId(ar, "id");
        if (!id.empty() && id != artistId)
            al.artistIds.push_back(id);
    }
    al.year = JInt(a, "year");
    al.songCount = JInt(a, "songCount");
    return al;
}

// Plain-HTTP fallback is only allowed to private/LAN hosts, so the token never
// crosses the internet unencrypted.
static bool IsLanUrl(const std::wstring& url)
{
    // ANSI WinINet (Windows 9x has no Unicode one); addresses are ASCII.
    std::string urlA(url.size(), '?');
    for (size_t i = 0; i < url.size(); ++i)
        if (url[i] < 0x80)
            urlA[i] = (char)url[i];
    URL_COMPONENTSA uc;
    ZeroMemory(&uc, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.dwHostNameLength = 1;
    if (!InternetCrackUrlA(urlA.c_str(), (DWORD)urlA.size(), 0, &uc) || !uc.lpszHostName)
        return false;
    std::wstring host(uc.lpszHostName, uc.lpszHostName + uc.dwHostNameLength);

    // A dotted IPv4 address (parsed by hand: MinGW's swscanf was built for i686).
    unsigned part[4] = { 0, 0, 0, 0 };
    int parts = 0, digits = 0;
    bool ip = !host.empty();
    for (size_t i = 0; i < host.size() && ip; ++i) {
        wchar_t ch = host[i];
        if (ch >= L'0' && ch <= L'9' && digits < 3) {
            part[parts] = part[parts] * 10 + (ch - L'0');
            ++digits;
        } else if (ch == L'.' && digits > 0 && parts < 3) {
            ++parts;
            digits = 0;
        } else {
            ip = false;
        }
    }
    if (ip && parts == 3 && digits > 0) {
        const unsigned a = part[0], b = part[1];
        return a == 10 || a == 127 || (a == 192 && b == 168) ||
               (a == 172 && b >= 16 && b <= 31) || (a == 169 && b == 254);
    }
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
    DWORD ms = 0;
    return Fetch(method, extraQuery, body, httpResult, err, &ms) && Parse(method, body, ms, root, response, err);
}

bool SubsonicClient::Fetch(const char* method, const std::string& extraQuery, std::string& body,
                           HttpResult* httpResult, std::wstring& err, DWORD* ms) const
{
    DWORD status = 0, winErr = 0;
    LogF(L"API %hs", method);   // never log the URL: it carries the auth token
    const DWORD t0 = GetTickCount();
    HttpResult r = HttpGetAll(BuildUrl(method, extraQuery + "&f=json"), Options(), body, &status, &winErr);
    *ms = GetTickCount() - t0;
    if (httpResult)
        *httpResult = r;
    if (r != HTTP_OK) {
        err = HttpDescribe(r, status, winErr, HttpIsHttpsUrl(m_activeBase));
        LogF(L"API %hs failed: result=%d status=%lu winerr=%lu", method, (int)r, status, winErr);
        return false;
    }
    return true;
}

bool SubsonicClient::Parse(const char* method, const std::string& body, DWORD fetchMs, cJSON** root,
                           cJSON** response, std::wstring& err) const
{
    *root = NULL;
    *response = NULL;
    const DWORD t1 = GetTickCount();
    cJSON* json = cJSON_Parse(body.c_str());
    if (body.size() >= 64 * 1024)   // big replies (the song list): where the time goes
        LogF(L"API %hs: %u KB downloaded in %lu ms, parsed in %lu ms", method, (unsigned)(body.size() / 1024),
             fetchMs, GetTickCount() - t1);
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

// --- Libraries ----------------------------------------------------------------------

bool SubsonicClient::GetLibraries(std::vector<SubsonicLibrary>& out, std::wstring& err)
{
    out.clear();
    cJSON *root, *sr;
    if (!Call("getMusicFolders", "", &root, &sr, NULL, err))
        return false;
    cJSON* f;
    cJSON_ArrayForEach(f, JGet(JGet(sr, "musicFolders"), "musicFolder")) {
        SubsonicLibrary lib;
        lib.id = JId(f, "id");
        lib.name = JStr(f, "name");
        if (!lib.id.empty())
            out.push_back(lib);
    }
    cJSON_Delete(root);
    return true;
}

// Navidrome takes the parameter more than once to mean "any of these libraries".
std::string SubsonicClient::LibraryParams() const
{
    std::string q;
    for (size_t i = 0; i < m_cfg.libraries.size(); ++i)
        q += "&musicFolderId=" + UrlEncode(m_cfg.libraries[i]);
    return q;
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
        if (!Call("getAlbumList2", extra + LibraryParams(), &root, &sr, NULL, err))
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

bool SubsonicClient::GetAlbumListOf(const char* type, int size, std::vector<SubsonicAlbum>& out, std::wstring& err)
{
    out.clear();
    char extra[96];
    _snprintf(extra, sizeof(extra) - 1, "&type=%s&size=%d&offset=0", type, size);
    extra[sizeof(extra) - 1] = 0;
    cJSON *root, *sr;
    if (!Call("getAlbumList2", extra + LibraryParams(), &root, &sr, NULL, err))
        return false;
    cJSON* a;
    cJSON_ArrayForEach(a, JGet(JGet(sr, "albumList2"), "album"))
        out.push_back(ParseAlbum(a));
    cJSON_Delete(root);
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
}

bool SongLibraryOrder(const SubsonicSong& a, const SubsonicSong& b)
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

void MergeSongs(std::vector<SubsonicSong>& into, std::vector<SubsonicSong>& more, std::vector<int>* from)
{
    std::vector<SubsonicSong> merged(into.size() + more.size());
    if (from)
        from->resize(merged.size());
    size_t a = 0, b = 0;
    for (size_t i = 0; i < merged.size(); ++i) {
        if (a == into.size() || (b < more.size() && SongLibraryOrder(more[b], into[a]))) {
            merged[i].swap(more[b]);
            if (from)
                (*from)[i] = -1 - (int)b;
            ++b;
        } else {
            merged[i].swap(into[a]);
            if (from)
                (*from)[i] = (int)a;
            ++a;
        }
    }
    into.swap(merged);
    more.clear();
}

namespace {
// Compares songs by their position in 'songs', so the sort moves ints, not whole songs.
struct SongOrder {
    const std::vector<SubsonicSong>* songs;
    bool operator()(int x, int y) const { return SongLibraryOrder((*songs)[x], (*songs)[y]); }
};

// Sorts by SongLibraryOrder (stable): sorts positions, then swaps the songs into that order.
void SortSongs(std::vector<SubsonicSong>& songs)
{
    std::vector<int> order(songs.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = (int)i;
    SongOrder less;
    less.songs = &songs;
    std::stable_sort(order.begin(), order.end(), less);
    std::vector<SubsonicSong> sorted(songs.size());
    for (size_t i = 0; i < order.size(); ++i)
        sorted[i].swap(songs[order[i]]);
    songs.swap(sorted);
}
}

// --- The song list's fast reader --------------------------------------------------------------
// A search3 page is about 600 KB, and building cJSON's tree of every field took a Pentium over
// a second. This walks the text once and keeps only what ParseSong keeps, with the same results.
// Anything it doesn't expect makes it give up, and the page goes through cJSON instead.

namespace {
class SongScan {
public:
    explicit SongScan(const char* json) : p(json), bad(false) {}

    bool Failed() const { return bad; }

    // Reads the reply: { "subsonic-response": { "status": "ok", "searchResult3": { "song": [...] } } }
    bool ReadPage(std::vector<SubsonicSong>& page)
    {
        bool ok = false;
        if (!Open('{'))
            return false;
        bool first = true;
        const char* k;
        size_t n;
        while (Member('}', first, k, n)) {
            if (Is(k, n, "subsonic-response"))
                ReadResponse(page, ok);
            else
                Skip();
        }
        return !bad && ok;
    }

private:
    const char* p;
    bool bad;

    static bool Is(const char* k, size_t n, const char* name)
    {
        return strlen(name) == n && memcmp(k, name, n) == 0;
    }

    void Ws()
    {
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')
            ++p;
    }

    bool Open(char c)
    {
        Ws();
        if (*p != c) {
            bad = true;
            return false;
        }
        ++p;
        return true;
    }

    // The next element of a list or object, if there is one ('first' goes false after the first).
    bool More(char close, bool& first)
    {
        Ws();
        if (*p == close) {
            ++p;
            return false;
        }
        if (!first) {
            if (*p != ',') {
                bad = true;
                return false;
            }
            ++p;
            Ws();
        }
        first = false;
        if (bad || !*p) {
            bad = true;
            return false;
        }
        return true;
    }

    // Past the next key and its colon. Keys with escapes just won't match any name.
    bool Member(char close, bool& first, const char*& key, size_t& len)
    {
        if (bad || !More(close, first))
            return false;
        if (*p != '"') {
            bad = true;
            return false;
        }
        key = p + 1;
        if (!SkipString())
            return false;
        len = (size_t)(p - 1 - key);
        Ws();
        if (*p != ':') {
            bad = true;
            return false;
        }
        ++p;
        Ws();
        return true;
    }

    bool SkipString()
    {
        for (++p;; ++p) {
            if (*p == '"') {
                ++p;
                return true;
            }
            if (*p == '\\' && p[1])
                ++p;
            else if (!*p || *p == '\\') {
                bad = true;
                return false;
            }
        }
    }

    void Skip()
    {
        Ws();
        if (*p == '"') {
            SkipString();
        } else if (*p == '{' || *p == '[') {
            int depth = 0;
            do {
                if (*p == '"') {
                    if (!SkipString())
                        return;
                    continue;
                }
                if (!*p) {
                    bad = true;
                    return;
                }
                if (*p == '{' || *p == '[')
                    ++depth;
                else if (*p == '}' || *p == ']')
                    --depth;
                ++p;
            } while (depth);
        } else {
            const char* start = p;
            while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t')
                ++p;
            bad = bad || p == start;
        }
    }

    static int Hex4(const char* s)
    {
        int v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s[i];
            const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                        : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0)
                return -1;
            v = v * 16 + d;
        }
        return v;
    }

    static void PutUtf8(std::string& out, unsigned cp)
    {
        if (cp < 0x80) {
            out += (char)cp;
        } else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }

    // The string at p, as UTF-8: [s, s + n), either in the reply or (if it had escapes) in 'tmp'.
    bool Text(const char*& s, size_t& n, std::string& tmp)
    {
        const char* start = p + 1;
        if (!SkipString())
            return false;
        s = start;
        n = (size_t)(p - 1 - start);
        if (!memchr(s, '\\', n))
            return true;
        tmp.clear();
        for (const char* c = start; c < p - 1; ++c) {
            if (*c != '\\') {
                tmp += *c;
                continue;
            }
            ++c;
            switch (*c) {
            case '"': case '\\': case '/': tmp += *c; break;
            case 'b': tmp += '\b'; break;
            case 'f': tmp += '\f'; break;
            case 'n': tmp += '\n'; break;
            case 'r': tmp += '\r'; break;
            case 't': tmp += '\t'; break;
            case 'u': {
                int cp = Hex4(c + 1);
                c += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && c[1] == '\\' && c[2] == 'u') {
                    const int low = Hex4(c + 3);
                    if (low < 0xDC00 || low > 0xDFFF) {
                        bad = true;
                        return false;
                    }
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    c += 6;
                } else if (cp <= 0 || (cp >= 0xD800 && cp <= 0xDFFF)) {   // includes \u0000
                    bad = true;
                    return false;
                }
                PutUtf8(tmp, (unsigned)cp);
                break;
            }
            default:
                bad = true;
                return false;
            }
        }
        s = tmp.data();
        n = tmp.size();
        return true;
    }

    void Wide(std::wstring& out)
    {
        Ws();
        if (*p != '"') {     // as JStr: not a string reads as empty
            Skip();
            out.clear();
            return;
        }
        const char* s = NULL;
        size_t n = 0;
        std::string tmp;
        if (Text(s, n, tmp))
            out = Utf8ToWide(s, n);
    }

    void Narrow(std::string& out)
    {
        Ws();
        if (*p != '"') {     // JId also takes numbers: leave those to cJSON
            bad = bad || (*p >= '0' && *p <= '9') || *p == '-';
            Skip();
            out.clear();
            return;
        }
        const char* s = NULL;
        size_t n = 0;
        std::string tmp;
        if (Text(s, n, tmp))
            out.assign(s, n);
    }

    void Int(int& out)
    {
        Ws();
        if (*p == '"' || *p == '{' || *p == '[' || (*p >= 'a' && *p <= 'z')) {   // as JInt: 0
            Skip();
            out = 0;
            return;
        }
        const bool neg = *p == '-';
        if (neg)
            ++p;
        const char* start = p;
        int v = 0;
        while (*p >= '0' && *p <= '9') {
            if (p - start >= 9) {   // beyond a plain int: leave it to cJSON
                bad = true;
                return;
            }
            v = v * 10 + (*p++ - '0');
        }
        if (p == start || *p == '.' || *p == 'e' || *p == 'E') {
            bad = true;
            return;
        }
        out = neg ? -v : v;
    }

    void ReadResponse(std::vector<SubsonicSong>& page, bool& ok)
    {
        if (!Open('{'))
            return;
        bool first = true;
        const char* k;
        size_t n;
        while (Member('}', first, k, n)) {
            if (Is(k, n, "status")) {
                std::string status;
                Narrow(status);
                ok = status == "ok";
            } else if (Is(k, n, "searchResult3")) {
                ReadResult(page);
            } else {
                Skip();
            }
        }
    }

    void ReadResult(std::vector<SubsonicSong>& page)
    {
        if (!Open('{'))
            return;
        bool first = true;
        const char* k;
        size_t n;
        while (Member('}', first, k, n)) {
            if (!Is(k, n, "song")) {
                Skip();
                continue;
            }
            if (!Open('['))
                return;
            bool firstSong = true;
            while (!bad && More(']', firstSong)) {
                page.resize(page.size() + 1);
                ReadSong(page.back());
            }
        }
    }

    void ReadGenres(std::wstring& genres)
    {
        Ws();
        if (*p != '[') {
            Skip();
            return;
        }
        ++p;
        bool first = true;
        while (!bad && More(']', first)) {
            Ws();
            if (*p != '{') {     // not the { "name": ... } that Navidrome sends
                bad = true;
                return;
            }
            ++p;
            bool firstKey = true;
            const char* k;
            size_t n;
            std::wstring name;
            while (Member('}', firstKey, k, n)) {
                if (Is(k, n, "name"))
                    Wide(name);
                else
                    Skip();
            }
            if (!name.empty())
                genres += name + L"\n";
        }
    }

    // One song object: the same fields and defaults as ParseSong.
    void ReadSong(SubsonicSong& s)
    {
        s.track = s.disc = s.duration = s.bitRate = s.bitDepth = s.samplingRate = s.channelCount = s.userRating = 0;
        s.starred = false;
        if (!Open('{'))
            return;
        std::wstring genre;
        bool first = true;
        const char* k;
        size_t n;
        while (Member('}', first, k, n)) {
            switch (n) {
            case 2:
                if (Is(k, n, "id")) { Narrow(s.id); continue; }
                break;
            case 5:
                if (Is(k, n, "title")) { Wide(s.title); continue; }
                if (Is(k, n, "album")) { Wide(s.album); continue; }
                if (Is(k, n, "track")) { Int(s.track); continue; }
                if (Is(k, n, "genre")) { Wide(genre); continue; }
                break;
            case 6:
                if (Is(k, n, "artist")) { Wide(s.artist); continue; }
                if (Is(k, n, "suffix")) { Wide(s.suffix); continue; }
                if (Is(k, n, "genres")) { ReadGenres(s.genres); continue; }
                break;
            case 7:
                if (Is(k, n, "albumId")) { Narrow(s.albumId); continue; }
                if (Is(k, n, "bitRate")) { Int(s.bitRate); continue; }
                if (Is(k, n, "starred")) { s.starred = true; Skip(); continue; }
                break;
            case 8:
                if (Is(k, n, "coverArt")) { Narrow(s.coverArt); continue; }
                if (Is(k, n, "artistId")) { Narrow(s.artistId); continue; }
                if (Is(k, n, "duration")) { Int(s.duration); continue; }
                if (Is(k, n, "bitDepth")) { Int(s.bitDepth); continue; }
                break;
            case 10:
                if (Is(k, n, "discNumber")) { Int(s.disc); continue; }
                if (Is(k, n, "userRating")) { Int(s.userRating); continue; }
                break;
            case 12:
                if (Is(k, n, "samplingRate")) { Int(s.samplingRate); continue; }
                if (Is(k, n, "channelCount")) { Int(s.channelCount); continue; }
                break;
            case 18:
                if (Is(k, n, "displayAlbumArtist")) { Wide(s.albumArtist); continue; }
                break;
            }
            Skip();
        }
        if (s.genres.empty() && !genre.empty())
            s.genres = genre + L"\n";
    }
};
}

// search3 parameters for songs [offset, offset + count).
std::string SubsonicClient::PageQuery(int offset, int count) const
{
    char extra[128];
    _snprintf(extra, sizeof(extra) - 1, "&query=&artistCount=0&albumCount=0&songCount=%d&songOffset=%d", count,
              offset);
    extra[sizeof(extra) - 1] = 0;
    return extra + LibraryParams();
}

// One search3 page, downloaded on its own thread (FetchPage) while the previous one is parsed.
struct SubsonicClient::PageFetch {
    const SubsonicClient* client;
    std::string query;
    std::string body;
    std::wstring err;
    DWORD ms;
    bool ok;
};

unsigned __stdcall SubsonicClient::FetchPage(void* p)
{
    PageFetch* f = (PageFetch*)p;
    f->ok = f->client->Fetch("search3", f->query, f->body, NULL, f->err, &f->ms);
    return 0;
}

bool SubsonicClient::GetAllSongs(std::vector<SubsonicSong>& out, std::wstring& err, SongPageFn onPage, void* ctx)
{
    static const int kPage = 500;
    out.clear();
    err.clear();
    const DWORD start = GetTickCount();
    int total = 0;
    // Two downloads take turns: while page k is parsed, page k+1 downloads on a thread (on a
    // Pentium each takes about as long as the other). After the last page there's one
    // download too many, of an empty page.
    PageFetch fetch[2];
    HANDLE thread = NULL;
    bool ok = true;
    for (int k = 0; k < 1000; ++k) {
        PageFetch& cur = fetch[k % 2];
        if (k == 0) {
            cur.client = this;
            cur.query = PageQuery(0, kPage);
            cur.ok = Fetch("search3", cur.query, cur.body, NULL, cur.err, &cur.ms);
        } else if (thread) {
            const DWORD w = GetTickCount();
            WaitForSingleObject(thread, INFINITE);
            CloseHandle(thread);
            thread = NULL;
            LogF(L"All songs: waited %lu ms for page %d", GetTickCount() - w, k + 1);
        }
        if (!cur.ok) {
            err = cur.err;
            ok = false;
            break;
        }
        PageFetch& next = fetch[(k + 1) % 2];
        next.client = this;
        next.query = PageQuery((k + 1) * kPage, kPage);
        next.body.clear();
        next.ok = false;
        thread = (HANDLE)_beginthreadex(NULL, 0, FetchPage, &next, 0, NULL);
        if (!thread)   // no thread: download it in turn instead
            next.ok = Fetch("search3", next.query, next.body, NULL, next.err, &next.ms);

        const DWORD t0 = GetTickCount();
        std::vector<SubsonicSong> page;
        page.reserve(kPage);
        SongScan scan(cur.body.c_str());
        if (!scan.ReadPage(page)) {   // anything unusual (or an error reply): the full parser
            LogF(L"All songs: using cJSON for page %d", k + 1);
            page.clear();
            cJSON *root, *sr;
            if (!Parse("search3", cur.body, cur.ms, &root, &sr, err)) {
                ok = false;
                break;
            }
            cJSON* s;
            cJSON_ArrayForEach(s, JGet(JGet(sr, "searchResult3"), "song")) {
                SubsonicSong song = ParseSong(s);
                page.resize(page.size() + 1);
                page.back().swap(song);
            }
            cJSON_Delete(root);
        }
        LogF(L"All songs: %u KB downloaded in %lu ms", (unsigned)(cur.body.size() / 1024), cur.ms);
        std::string().swap(cur.body);
        const DWORD t1 = GetTickCount();
        SortSongs(page);
        const int n = (int)page.size();
        total += n;
        LogF(L"All songs: page of %d read in %lu ms, sorted in %lu ms", n, t1 - t0, GetTickCount() - t1);
        if (onPage) {
            if (!onPage(page, ctx)) {
                ok = false;
                break;
            }
        } else {
            MergeSongs(out, page, NULL);
        }
        if (n < kPage)
            break;
    }
    if (thread) {   // the download after the last page (or after a stop)
        WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);
    }
    if (ok)
        LogF(L"All songs: %d loaded in %lu ms", total, GetTickCount() - start);
    return ok;
}

// --- Artists ------------------------------------------------------------------------

bool SubsonicClient::GetArtists(std::vector<SubsonicArtist>& out, std::wstring& err)
{
    out.clear();
    cJSON *root, *sr;
    if (!Call("getArtists", LibraryParams(), &root, &sr, NULL, err))
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

// --- Favorites ----------------------------------------------------------------------

bool SubsonicClient::SetStarred(const std::string& songId, bool starred, std::wstring& err)
{
    return CallNoResult(starred ? "star" : "unstar", "&id=" + UrlEncode(songId), err);
}

bool SubsonicClient::GetStarredSongs(std::vector<SubsonicSong>& out, std::wstring& err)
{
    out.clear();
    cJSON *root, *sr;
    if (!Call("getStarred2", LibraryParams(), &root, &sr, NULL, err))
        return false;
    cJSON* s;
    cJSON_ArrayForEach(s, JGet(JGet(sr, "starred2"), "song"))
        out.push_back(ParseSong(s));
    cJSON_Delete(root);
    return true;
}

// --- Scrobbling ---------------------------------------------------------------------

bool SubsonicClient::Scrobble(const std::string& songId, bool submission, std::wstring& err,
                              unsigned long long timeMs)
{
    std::string q = "&id=" + UrlEncode(songId) + (submission ? "&submission=true" : "&submission=false");
    if (timeMs) {
        char t[40];
        _snprintf(t, sizeof(t) - 1, "&time=%I64u", timeMs);
        t[sizeof(t) - 1] = 0;
        q += t;
    }
    return CallNoResult("scrobble", q, err);
}

bool SubsonicClient::GetNowPlayingIds(std::vector<std::string>& ids, std::wstring& err)
{
    ids.clear();
    cJSON *root, *sr;
    if (!Call("getNowPlaying", "", &root, &sr, NULL, err))
        return false;
    cJSON* e;
    cJSON_ArrayForEach(e, JGet(JGet(sr, "nowPlaying"), "entry"))
        ids.push_back(JId(e, "id"));
    cJSON_Delete(root);
    return true;
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

// Last.fm summaries are HTML: tags (the "Read more on Last.fm" link) and entities. Keeps line
// breaks, turns <br> into one, and folds runs of spaces.
static std::wstring HtmlToText(const std::wstring& html)
{
    std::wstring out;
    for (size_t i = 0; i < html.size(); ++i) {
        wchar_t c = html[i];
        if (c == L'<') {
            size_t end = html.find(L'>', i);
            if (end == std::wstring::npos)
                break;
            std::wstring tag = html.substr(i + 1, end - i - 1);
            if (tag.size() >= 2 && (tag[0] == L'b' || tag[0] == L'B') && (tag[1] == L'r' || tag[1] == L'R'))
                out += L'\n';
            i = end;
            continue;
        }
        if (c == L'&') {
            size_t semi = html.find(L';', i);
            if (semi != std::wstring::npos && semi - i <= 10) {
                std::wstring ent = html.substr(i + 1, semi - i - 1);
                wchar_t ch = 0;
                if (ent == L"amp") ch = L'&';
                else if (ent == L"lt") ch = L'<';
                else if (ent == L"gt") ch = L'>';
                else if (ent == L"quot") ch = L'"';
                else if (ent == L"apos") ch = L'\'';
                else if (ent == L"nbsp") ch = L' ';
                else if (ent.size() > 1 && ent[0] == L'#')
                    ch = (wchar_t)((ent[1] == L'x' || ent[1] == L'X') ? wcstoul(ent.c_str() + 2, NULL, 16)
                                                                       : wcstoul(ent.c_str() + 1, NULL, 10));
                if (ch) {
                    c = ch;
                    i = semi;
                }
            }
        }
        if (c == L'\r')
            continue;
        if ((c == L' ' || c == L'\t') && (out.empty() || out[out.size() - 1] == L' ' || out[out.size() - 1] == L'\n'))
            continue;
        if (c == L'\t')
            c = L' ';
        out += c;
    }
    const wchar_t* more = L"Read more on Last.fm";
    size_t at = out.rfind(more);
    if (at != std::wstring::npos && out.find_first_not_of(L" .\n", at + wcslen(more)) == std::wstring::npos)
        out.erase(at);
    size_t last = out.find_last_not_of(L" \n");
    out.erase(last == std::wstring::npos ? 0 : last + 1);
    size_t first = out.find_first_not_of(L" \n");
    return first == std::wstring::npos ? std::wstring() : out.substr(first);
}

bool SubsonicClient::GetAlbumNotes(const std::string& albumId, std::wstring& notes, std::wstring& err)
{
    notes.clear();
    cJSON *root, *sr;
    if (!Call("getAlbumInfo2", "&id=" + UrlEncode(albumId), &root, &sr, NULL, err))
        return false;
    notes = HtmlToText(JStr(JGet(sr, "albumInfo"), "notes"));
    cJSON_Delete(root);
    return true;
}

bool SubsonicClient::GetArtistBio(const std::string& artistId, std::wstring& bio, std::wstring& err)
{
    bio.clear();
    cJSON *root, *sr;
    if (!Call("getArtistInfo2", "&id=" + UrlEncode(artistId), &root, &sr, NULL, err))
        return false;
    bio = HtmlToText(JStr(JGet(sr, "artistInfo2"), "biography"));
    cJSON_Delete(root);
    return true;
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
        pl.comment = JStr(p, "comment");
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

bool SubsonicClient::SetPlaylistDetails(const std::string& playlistId, const std::wstring& name,
                                        const std::wstring& comment, std::wstring& err)
{
    return CallNoResult("updatePlaylist", "&playlistId=" + UrlEncode(playlistId) + "&name=" +
                        UrlEncode(WideToUtf8(name)) + "&comment=" + UrlEncode(WideToUtf8(comment)), err);
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

std::wstring SubsonicClient::StreamUrl(const std::string& songId, int timeOffsetSec, int maxBitRate) const
{
    char extra[96];
    _snprintf(extra, sizeof(extra) - 1, "&format=mp3&maxBitRate=%d", maxBitRate > 0 ? maxBitRate : m_cfg.maxBitRate);
    extra[sizeof(extra) - 1] = 0;
    std::string q = "&id=" + UrlEncode(songId) + extra;
    if (timeOffsetSec > 0) {
        _snprintf(extra, sizeof(extra) - 1, "&timeOffset=%d", timeOffsetSec);
        q += extra;
    }
    return BuildUrl("stream", q);
}

bool SubsonicClient::StreamsLossless(const SubsonicSong& song) const
{
    return m_cfg.lossless && CanPlayFlac(song);
}

bool SubsonicClient::CanPlayFlac(const SubsonicSong& song) const
{
    // Hi-res and multichannel files would need resampling or downmixing, which old sound
    // cards and CPUs can't take; Navidrome's MP3 transcode handles those instead.
    return _wcsicmp(song.suffix.c_str(), L"flac") == 0 && song.samplingRate <= 48000 && song.channelCount <= 2 &&
           song.bitDepth <= 24;
}

std::wstring SubsonicClient::OriginalUrl(const std::string& songId) const
{
    return BuildUrl("stream", "&id=" + UrlEncode(songId) + "&format=raw");
}

bool SubsonicClient::ServerTranscodes(const SubsonicSong& song) const
{
    bool mp3 = _wcsicmp(song.suffix.c_str(), L"mp3") == 0;
    return !(mp3 && song.bitRate > 0 && song.bitRate <= m_cfg.maxBitRate);
}

std::wstring SubsonicClient::CoverArtUrl(const std::string& coverArtId, int size) const
{
    char extra[32];
    _snprintf(extra, sizeof(extra) - 1, "&size=%d", size);
    extra[sizeof(extra) - 1] = 0;
    return BuildUrl("getCoverArt", "&id=" + UrlEncode(coverArtId) + extra);
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
