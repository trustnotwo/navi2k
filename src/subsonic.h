#ifndef LP_SUBSONIC_H
#define LP_SUBSONIC_H

#include "http.h"
#include <string>
#include <vector>

struct cJSON;

struct SubsonicConfig {
    std::wstring url;            // primary address, e.g. https://music.example.com
    std::wstring lanUrl;         // optional plain-HTTP LAN address, e.g. http://192.168.1.10:4533
    bool allowHttpFallback;      // use lanUrl when the secure connection fails
    bool allowUntrustedCert;
    std::string user;
    std::string token;           // md5(password + salt), lowercase hex
    std::string salt;
    int maxBitRate;              // kbps for server-side MP3 transcoding
    SubsonicConfig() : allowHttpFallback(false), allowUntrustedCert(false), maxBitRate(192) {}
};

struct SubsonicAlbum {
    std::string id;
    std::string coverArt;        // id for getCoverArt; empty if the album has none
    std::wstring name, artist;
    int year, songCount;
};

struct SubsonicSong {
    std::string id;
    std::string coverArt;           // the track's album cover, for the now-playing panel
    std::wstring title, artist, album, suffix;
    int track, disc, duration, bitRate;   // duration in s, bitRate in kbps (0 = unknown)
    int userRating;                 // 0 = unrated, 1..5
    bool starred;                   // in Favorites
};

struct SubsonicArtist {
    std::string id;
    std::string coverArt;        // artist image id for getCoverArt; may be empty
    std::wstring name;
    int albumCount;
};

struct SubsonicPlaylist {
    std::string id;
    std::string coverArt;
    std::wstring name, owner;
    int songCount, duration;
    bool readOnly;               // smart playlist (.nsp) or otherwise not editable by the server
};

// Subsonic token auth: random salt, token = md5(password + salt). Uses CryptoAPI (Win2000+).
bool SubsonicMakeToken(const std::string& passwordUtf8, std::string& token, std::string& salt);

// Navidrome speaks the Subsonic REST API (/rest/*, f=json).
class SubsonicClient {
public:
    void Configure(const SubsonicConfig& cfg);
    // Streaming quality for the next StreamUrl, without reconnecting.
    void SetMaxBitRate(int kbps) { m_cfg.maxBitRate = kbps; }

    // Pings the primary URL; on a TLS failure, falls back to lanUrl if allowed.
    // On success 'notice' may describe the fallback that was taken.
    bool Connect(std::wstring& notice);

    bool GetAlbumList(std::vector<SubsonicAlbum>& out, std::wstring& err);
    bool GetAlbum(const std::string& albumId, std::vector<SubsonicSong>& out, std::wstring& err);

    // Every song in the library, via search3 with an empty query, 500 per request.
    // Sorted by artist, album, disc, track.
    bool GetAllSongs(std::vector<SubsonicSong>& out, std::wstring& err);

    // All artists, alphabetical (getArtists), and one artist's albums (getArtist).
    bool GetArtists(std::vector<SubsonicArtist>& out, std::wstring& err);
    bool GetArtist(const std::string& artistId, std::vector<SubsonicAlbum>& albums, std::wstring& err);

    // Always asks for MP3: the decoder is libmad only, so Navidrome transcodes FLAC/M4A/OGG.
    // timeOffsetSec starts the transcode partway in (honoured only when the server transcodes).
    std::wstring StreamUrl(const std::string& songId, int timeOffsetSec = 0) const;

    // False when Navidrome will send the original file untouched (already MP3 within
    // maxBitRate); such streams ignore timeOffset, so the client must skip instead.
    bool ServerTranscodes(const SubsonicSong& song) const;

    // Raw image bytes (JPEG/PNG) scaled by the server to fit 'size' pixels.
    bool GetCoverArt(const std::string& coverArtId, int size, std::string& bytes, std::wstring& err);

    // --- Ratings ---
    bool GetSong(const std::string& songId, SubsonicSong& out, std::wstring& err);
    bool SetRating(const std::string& songId, int rating, std::wstring& err);   // 0 clears

    // --- Favorites (starred songs) ---
    bool SetStarred(const std::string& songId, bool starred, std::wstring& err);
    bool GetStarredSongs(std::vector<SubsonicSong>& out, std::wstring& err);

    // --- Scrobbling ---
    // submission=false: "now playing"; true: a completed play (play count, Last.fm if linked).
    bool Scrobble(const std::string& songId, bool submission, std::wstring& err);
    bool GetNowPlayingIds(std::vector<std::string>& ids, std::wstring& err);

    // --- Playlists ---
    bool GetPlaylists(std::vector<SubsonicPlaylist>& out, std::wstring& err);
    bool GetPlaylist(const std::string& playlistId, std::vector<SubsonicSong>& out, std::wstring& err);
    bool CreatePlaylist(const std::wstring& name, const std::vector<std::string>& songIds,
                        std::string* newId, std::wstring& err);
    // Replaces the playlist's whole track list (used for reordering).
    bool SetPlaylistSongs(const std::string& playlistId, const std::vector<std::string>& songIds,
                          std::wstring& err);
    bool RenamePlaylist(const std::string& playlistId, const std::wstring& name, std::wstring& err);
    bool AddToPlaylist(const std::string& playlistId, const std::vector<std::string>& songIds,
                       std::wstring& err);
    bool RemoveFromPlaylist(const std::string& playlistId, int index, std::wstring& err);
    bool DeletePlaylist(const std::string& playlistId, std::wstring& err);

    // True if this user may change the playlist (owned by them and not read-only).
    bool CanEdit(const SubsonicPlaylist& pl) const;

    HttpOptions Options() const;

private:
    std::wstring BuildUrl(const char* method, const std::string& extraQuery) const;
    bool Call(const char* method, const std::string& extraQuery, cJSON** root, cJSON** response,
              HttpResult* httpResult, std::wstring& err);
    bool CallNoResult(const char* method, const std::string& params, std::wstring& err);
    // Groups song ids so each request's URL stays short enough for Windows 2000's WinINet.
    std::vector<std::vector<std::string> > SplitIds(const char* method, const std::string& fixedParams,
                                                    const std::vector<std::string>& ids, const char* key) const;
    bool CreateOrReplace(const std::string& fixedParams, const std::vector<std::string>& songIds,
                         std::string* playlistId, std::wstring& err);

    SubsonicConfig m_cfg;
    std::wstring m_activeBase;
};

#endif
