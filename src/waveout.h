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

    // Number of 8 KB buffers queued ahead (4..MAX_BUFFERS); call while closed.
    // 12 = ~0.56 s at 44.1 kHz stereo. This is also the volume/seek latency, since volume
    // is applied before queueing.
    void SetBufferCount(int count);

    // device: a waveOut device id, or WAVE_MAPPER for the system default. Falls back to the
    // default if the chosen device can't be opened (e.g. unplugged).
    bool Open(unsigned rate, unsigned channels, UINT device, std::wstring& err);
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
public:
    enum { MAX_BUFFERS = 32, BUFFER_BYTES = 8192 };

private:

    WaveOut(const WaveOut&);
    WaveOut& operator=(const WaveOut&);
    bool WaitForBuffer(int i, HANDLE cancel);
    bool Submit();
    DWORD DevicePositionMs();   // caller holds m_lock

    CRITICAL_SECTION m_lock;
    HWAVEOUT m_wo;
    HANDLE m_doneEvent;
    WAVEHDR m_hdr[MAX_BUFFERS];
    bool m_queued[MAX_BUFFERS];
    int m_numBuffers;
    int m_prebuffer;            // playback starts once this many are queued
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
