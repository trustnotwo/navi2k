#ifndef LP_WAVEOUT_H
#define LP_WAVEOUT_H

#include <windows.h>
#include <mmsystem.h>
#include <string>

// 16-bit PCM output through waveOut (present on every Win2000/XP box, no DirectX needed).
// Open/Close/Write/Drain are called from the playback thread only; SetPaused and PlayedMs
// may be called from any thread.
class WaveOut {
public:
    WaveOut();
    ~WaveOut();

    bool Open(unsigned rate, unsigned channels, std::wstring& err);
    void Close();
    bool IsOpenFor(unsigned rate, unsigned channels) const;

    // Blocks while all buffers are queued. Returns false if 'cancel' was signalled.
    bool Write(const short* samples, size_t count, HANDLE cancel);
    // Plays out everything queued, unless cancelled.
    void Drain(HANDLE cancel);

    void SetPaused(bool paused);
    bool IsStarted() const { return m_started; }

    // Milliseconds actually played by the device since this object was created
    // (survives a reopen on a mid-stream format change).
    DWORD PlayedMs();

private:
    // 12 x 8 KB = ~0.56 s at 44.1 kHz stereo, playback starts after ~0.19 s. Kept short
    // because volume is applied before queueing: this is the volume/seek latency.
    enum { NUM_BUFFERS = 12, BUFFER_BYTES = 8192, PREBUFFER = 4 };

    WaveOut(const WaveOut&);
    WaveOut& operator=(const WaveOut&);
    bool WaitForBuffer(int i, HANDLE cancel);
    bool Submit();
    DWORD DevicePositionMs();   // caller holds m_lock

    CRITICAL_SECTION m_lock;
    HWAVEOUT m_wo;
    HANDLE m_doneEvent;
    WAVEHDR m_hdr[NUM_BUFFERS];
    bool m_queued[NUM_BUFFERS];
    char* m_mem;
    int m_cur;
    DWORD m_fill;
    unsigned m_rate, m_channels;
    int m_submitted;
    bool m_started;
    bool m_paused;
    DWORD m_closedMs;           // played time of earlier opens
};

#endif
