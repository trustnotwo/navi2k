#ifndef LP_PLAYER_H
#define LP_PLAYER_H

#include "http.h"
#include <windows.h>
#include <string>
#include <vector>

class WaveOut;
class FlacDecoder;

enum PlayerState {
    PS_BUFFERING,
    PS_PLAYING,
    PS_FINISHED,    // reached the end of the track (not sent after Stop())
    PS_ERROR,
    PS_NEXT_TRACK   // gapless: the track given to SetNext() is now being heard; text = its tag
};

// Posted to the notify window. wParam = MAKEWPARAM(state, generation);
// lParam = malloc'd wchar_t* message or NULL; the receiver free()s it.
// Compare the generation with Player::Generation() to drop messages from an older track.
#define WM_PLAYER_STATE (WM_APP + 1)

enum StreamCodec {
    CODEC_MP3,      // libmad; the server may transcode and offset it
    CODEC_FLAC      // dr_flac on the original file; seeks with HTTP Range requests
};

// Streams URLs: WinINet -> libmad or dr_flac -> volume -> waveOut, on its own thread.
// With a next track set (SetNext), it connects to that one a few seconds before the current
// one ends and feeds it to the same open device, so there is no gap between them.
class Player {
public:
    Player();
    ~Player();

    void SetNotifyWindow(HWND hwnd) { m_notify = hwnd; }

    // Output device (waveOut id or WAVE_MAPPER) and buffers queued ahead; used from the next
    // Play() (a new track or a seek).
    void SetOutput(UINT device, int bufferCount) { m_device = device; m_bufferCount = bufferCount; }

    // startMs: where in the track this stream begins (for PositionMs).
    // clientSkipMs: audio to skip before playing, for streams the server can't offset
    // (MP3: by frame headers; FLAC: a real seek).
    // durationMs: the track's length, so the next track can be fetched in time (0 = unknown:
    // it is then fetched at the end, with a short gap).
    // Forgets any next track; set it again after Play().
    bool Play(const std::wstring& url, const HttpOptions& opts, DWORD startMs = 0, DWORD clientSkipMs = 0,
              StreamCodec codec = CODEC_MP3, DWORD durationMs = 0);
    void Stop();
    bool TogglePause();     // returns the new paused state
    bool IsActive() const { return m_thread != NULL; }
    bool IsPaused() const { return m_thread != NULL && m_paused; }
    WORD Generation() const { return m_generation; }

    // Gapless: the track to play straight after the current one (same HTTP options). 'tag'
    // comes back as the text of PS_NEXT_TRACK. Replaces any earlier next track.
    void SetNext(const std::wstring& url, StreamCodec codec, const std::wstring& tag, DWORD durationMs);
    void ClearNext();

    // 0..100, perceptual (squared) curve. Applies to the next decoded audio, so it is heard
    // after the ~0.5 s already queued in waveOut.
    void SetVolume(int percent);
    int  Volume() const { return m_volume; }

    // Position in the track being heard, in ms.
    DWORD PositionMs();

    // Peak |sample| seen since the last call, before and after the volume gain (for tests/meters).
    void TakePeaks(int& before, int& after);

private:
    struct Source;
    Player(const Player&);
    Player& operator=(const Player&);
    static unsigned __stdcall ThreadMain(void* self);
    static unsigned __stdcall PrefetchMain(void* source);
    static void OpenSource(Source* s);
    void Run();
    void RunMp3(Source& src, DWORD skipMs, DWORD trimFrames, WaveOut& out, bool& started, bool& ended,
                std::wstring& failure);
    void RunFlac(Source& src, DWORD skipMs, WaveOut& out, bool& started, bool& ended, std::wstring& failure);
    // Volume, (re)open the device if the format changed, queue the audio. False to stop.
    bool Output(WaveOut& out, std::vector<short>& pcm, unsigned rate, unsigned channels, bool& started,
                std::wstring& failure);
    void MaybePrefetch();
    Source* TakeNext();
    void DropPrefetch();
    void BeginSwitch(WaveOut& out, const std::wstring& tag);
    void CheckSwitch(WaveOut& out, bool force);
    bool Cancelled() const;
    void Notify(PlayerState state, const std::wstring& text);

    HWND m_notify;
    HANDLE m_thread;
    HANDLE m_cancel;            // manual-reset; set by Stop()
    CRITICAL_SECTION m_lock;    // guards the members marked (L)
    HttpRequest* m_req;         // (L) the stream being decoded
    Source* m_prefetch;         // (L) the next track, connecting or connected
    WaveOut* m_out;             // (L)
    bool m_paused;              // (L)
    WORD m_generation;
    std::wstring m_url;
    HttpOptions m_opts;
    DWORD m_startMs;
    DWORD m_clientSkipMs;
    StreamCodec m_codec;
    DWORD m_durationMs;         // of the track being decoded
    DWORD m_streamStartMs;      // where that stream began in its track
    unsigned long long m_streamFrames;   // decoded from that stream
    unsigned long long m_writtenUs;      // all audio queued to the device by this thread
    DWORD m_playedMs;           // (L) final device position of the last finished/stopped stream
    UINT m_device;
    int m_bufferCount;
    int m_volume;
    volatile LONG m_gain;       // Q15, 32768 = unity
    volatile LONG m_peakIn;
    volatile LONG m_peakOut;

    // Next track (L)
    bool m_hasNext;
    std::wstring m_nextUrl, m_nextTag;
    StreamCodec m_nextCodec;
    DWORD m_nextDurationMs;

    // What is being heard (L): PositionMs = heardStart + device position - heardBase.
    DWORD m_heardStartMs, m_heardBaseMs;
    bool m_switchPending;       // a joined track is queued but not heard yet
    DWORD m_switchAtMs;         // device position where it starts
    std::wstring m_switchTag;
};

#endif
