#ifndef LP_PLAYER_H
#define LP_PLAYER_H

#include "http.h"
#include <windows.h>
#include <string>

class WaveOut;

enum PlayerState {
    PS_BUFFERING,
    PS_PLAYING,
    PS_FINISHED,    // reached the end of the track (not sent after Stop())
    PS_ERROR
};

// Posted to the notify window. wParam = MAKEWPARAM(state, generation);
// lParam = malloc'd wchar_t* message or NULL; the receiver free()s it.
// Compare the generation with Player::Generation() to drop messages from an older track.
#define WM_PLAYER_STATE (WM_APP + 1)

// Streams one MP3 URL: WinINet -> libmad -> volume -> waveOut, on its own thread.
class Player {
public:
    Player();
    ~Player();

    void SetNotifyWindow(HWND hwnd) { m_notify = hwnd; }

    // startMs: where in the track this stream begins (for PositionMs).
    // clientSkipMs: audio to discard before playing, for streams the server can't offset.
    bool Play(const std::wstring& url, const HttpOptions& opts, DWORD startMs = 0, DWORD clientSkipMs = 0);
    void Stop();
    bool TogglePause();     // returns the new paused state
    bool IsActive() const { return m_thread != NULL; }
    bool IsPaused() const { return m_thread != NULL && m_paused; }
    WORD Generation() const { return m_generation; }

    // 0..100, perceptual (squared) curve. Applies to the next decoded audio, so it is heard
    // after the ~0.5 s already queued in waveOut.
    void SetVolume(int percent);
    int  Volume() const { return m_volume; }

    // Track position actually heard, in ms.
    DWORD PositionMs();

    // Peak |sample| seen since the last call, before and after the volume gain (for tests/meters).
    void TakePeaks(int& before, int& after);

private:
    Player(const Player&);
    Player& operator=(const Player&);
    static unsigned __stdcall ThreadMain(void* self);
    void Run();
    bool Cancelled() const;
    void Notify(PlayerState state, const std::wstring& text);

    HWND m_notify;
    HANDLE m_thread;
    HANDLE m_cancel;            // manual-reset; set by Stop()
    CRITICAL_SECTION m_lock;    // guards m_req, m_out, m_paused, m_playedMs
    HttpRequest* m_req;
    WaveOut* m_out;
    bool m_paused;
    WORD m_generation;
    std::wstring m_url;
    HttpOptions m_opts;
    DWORD m_startMs;
    DWORD m_clientSkipMs;
    DWORD m_playedMs;           // final device position of the last finished/stopped stream
    int m_volume;
    volatile LONG m_gain;       // Q15, 32768 = unity
    volatile LONG m_peakIn;
    volatile LONG m_peakOut;
};

#endif
