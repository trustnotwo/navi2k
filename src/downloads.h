#ifndef LP_DOWNLOADS_H
#define LP_DOWNLOADS_H

#include "http.h"
#include "subsonic.h"
#include <windows.h>
#include <deque>
#include <map>
#include <string>
#include <vector>

// Tracks saved for offline listening. Each one is a file named by its song id in the
// downloads folder, with the details Navidrome gave for it in downloads.json and its cover in
// art\. The index is only touched on the UI thread; a worker thread fetches the files and
// posts each result back.

struct DownloadItem {            // one track to fetch
    SubsonicSong song;
    std::wstring url;            // stream URL, built on the UI thread (it carries the login token)
    std::wstring artUrl;         // cover, or empty
    HttpOptions opts;
    bool flac;                   // url sends the original FLAC; otherwise MP3
    std::wstring file;           // set by Enqueue: Artist\Album\NN Title.ext, within the folder
    std::string server;          // Server() when queued
};

struct DownloadedTrack {
    SubsonicSong song;
    std::wstring file;           // path within the folder: "Artist\Album\01 Title.flac" (or, from
                                 // older versions until Organize(), "<song id>.flac")
    bool flac;
    unsigned long long bytes;
    std::string server;          // the server it came from (see SetServer); "" from older versions
};

struct DownloadedPlaylist {      // a playlist downloaded as a whole, kept for offline use
    SubsonicPlaylist info;
    std::vector<std::string> songIds;   // its order when last downloaded or opened online
};

enum DownloadEventKind {
    DL_STARTED,                  // title = the track now downloading
    DL_DONE,                     // track = the finished download
    DL_FAILED,                   // title, error
    DL_IDLE                      // the queue is empty (finished or cancelled)
};

// Posted to the notify window with the message given to Init(): lParam = DownloadEvent*,
// which the receiver deletes.
struct DownloadEvent {
    DownloadEventKind kind;
    DownloadedTrack track;
    std::wstring title, error;
    int remaining;               // waiting after this one
};

class Downloads {
public:
    Downloads();
    ~Downloads();

    void Init(HWND notify, UINT message);
    // Loads the folder's index, if there is one. The folder itself is made by EnsureFolder()
    // on the first download.
    void SetFolder(const std::wstring& folder);
    bool EnsureFolder(std::wstring& err);
    const std::wstring& Folder() const { return m_folder; }

    // The connected server, as a lowercase address ("" until set). Song ids belong to one
    // server, so a download tagged with another one can't be streamed, rated or scrobbled here.
    void SetServer(const std::string& key) { m_server = key; }
    const std::string& Server() const { return m_server; }
    bool FromOtherServer(const std::string& songId) const;
    // Up to perServer song ids from each other tag (including ""), spread over its tracks, to
    // ask the connected server whether it has them: it may be the same server at another address.
    std::map<std::string, std::vector<std::string> > OtherServerSamples(size_t perServer) const;
    // Retags the downloads tagged with key as the connected server's; returns how many.
    int Adopt(const std::string& key);

    const std::vector<DownloadedTrack>& Tracks() const { return m_tracks; }
    const DownloadedTrack* Find(const std::string& songId) const;
    std::wstring PathOf(const DownloadedTrack& t) const { return m_folder + L"\\" + t.file; }
    unsigned long long TotalBytes() const;
    bool LoadArt(const std::string& coverArtId, std::string& bytes) const;

    const std::vector<DownloadedPlaylist>& Playlists() const { return m_playlists; }
    const DownloadedPlaylist* FindPlaylist(const std::string& playlistId) const;
    // Adds or replaces a playlist's record (its tracks are downloaded separately).
    void SavePlaylist(const SubsonicPlaylist& info, const std::vector<std::string>& songIds);
    // Forgets the playlist and deletes its tracks that no other saved playlist uses.
    void RemovePlaylist(const std::string& playlistId);
    void SaveArt(const std::string& coverArtId, const std::string& bytes);

    void Add(const DownloadedTrack& t);         // a DL_DONE result
    bool Remove(const std::string& songId);     // deletes the file too
    void RemoveAll();                           // every file, the covers and the index

    // Moves downloads saved by older versions (named by song id) into Artist\Album folders.
    // albumArtists maps album ids to their artist where known. Returns how many moved.
    int Organize(const std::map<std::string, std::wstring>& albumArtists);

    // Queues tracks not already downloaded or waiting; returns how many were added.
    int Enqueue(const std::vector<DownloadItem>& items);
    bool IsQueued(const std::string& songId) const;
    int Pending() const;                        // waiting, including the one downloading
    void Cancel();                              // drops the queue and stops the current file
    void Shutdown();                            // Cancel, then wait briefly for the worker

private:
    Downloads(const Downloads&);
    Downloads& operator=(const Downloads&);
    static unsigned __stdcall ThreadMain(void* self);
    void Run();
    bool Fetch(const DownloadItem& item, DownloadedTrack& done, std::wstring& err);
    void Post(DownloadEvent* e);
    void Load();
    void Save() const;
    void Reindex();
    std::wstring OrganizedName(const SubsonicSong& s, bool flac, const std::string& skipId) const;
    bool NameTaken(const std::wstring& file, const std::string& skipId) const;
    std::wstring ArtPath(const std::string& coverArtId) const;

    HWND m_notify;
    UINT m_message;
    std::wstring m_folder;
    std::string m_server;
    std::vector<DownloadedTrack> m_tracks;      // UI thread only
    std::map<std::string, size_t> m_byId;       // song id -> index in m_tracks
    std::vector<DownloadedPlaylist> m_playlists; // UI thread only

    CRITICAL_SECTION m_lock;                    // guards the members below
    std::deque<DownloadItem> m_queue;           // front = downloading now
    std::wstring m_workFolder;                  // the folder the worker writes to
    HttpRequest* m_req;                         // the current download, for Cancel()
    bool m_running;
    volatile LONG m_cancel;
    HANDLE m_idle;                              // manual-reset: set while no worker runs
};

// A file name made only of characters any file system accepts.
std::wstring SafeFileName(const std::string& id);

#endif
