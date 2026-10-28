#include "player.h"
#include "mad_decoder.h"
#include "util.h"
#include "waveout.h"

#include <process.h>
#include <stdlib.h>
#include <vector>

static const int kReadChunk = 16384;
static const LONG kUnityGain = 32768;

Player::Player()
    : m_notify(NULL), m_thread(NULL), m_req(NULL), m_out(NULL), m_paused(false), m_generation(0),
      m_startMs(0), m_clientSkipMs(0), m_playedMs(0), m_device(WAVE_MAPPER), m_bufferCount(12),
      m_volume(100), m_gain(kUnityGain), m_peakIn(0), m_peakOut(0)
{
    InitializeCriticalSection(&m_lock);
    m_cancel = CreateEventW(NULL, TRUE, FALSE, NULL);
}

Player::~Player()
{
    Stop();
    CloseHandle(m_cancel);
    DeleteCriticalSection(&m_lock);
}

bool Player::Play(const std::wstring& url, const HttpOptions& opts, DWORD startMs, DWORD clientSkipMs)
{
    Stop();
    m_url = url;
    m_opts = opts;
    m_paused = false;
    m_startMs = startMs;
    m_clientSkipMs = clientSkipMs;
    m_playedMs = 0;
    ++m_generation;
    m_thread = (HANDLE)_beginthreadex(NULL, 0, ThreadMain, this, 0, NULL);
    if (!m_thread) {
        Notify(PS_ERROR, L"Could not start the playback thread.");
        return false;
    }
    // Keep decoding ahead of the UI thread on a busy single-CPU box.
    SetThreadPriority(m_thread, THREAD_PRIORITY_ABOVE_NORMAL);
    return true;
}

void Player::Stop()
{
    if (!m_thread)
        return;
    SetEvent(m_cancel);
    EnterCriticalSection(&m_lock);
    if (m_req)
        m_req->Abort();
    LeaveCriticalSection(&m_lock);
    // Every blocking call in Run() is cancellable (Abort() unblocks WinINet, WaveOut
    // waits on m_cancel), so this returns promptly.
    WaitForSingleObject(m_thread, INFINITE);
    CloseHandle(m_thread);
    m_thread = NULL;
    ResetEvent(m_cancel);
}

bool Player::TogglePause()
{
    EnterCriticalSection(&m_lock);
    m_paused = !m_paused;
    if (m_out)
        m_out->SetPaused(m_paused);
    bool paused = m_paused;
    LeaveCriticalSection(&m_lock);
    return paused;
}

void Player::SetVolume(int percent)
{
    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;
    m_volume = percent;
    // Squared curve: equal slider steps sound roughly like equal loudness steps.
    InterlockedExchange(&m_gain, (LONG)(percent * percent * kUnityGain / 10000));
}

DWORD Player::PositionMs()
{
    EnterCriticalSection(&m_lock);
    DWORD played = m_out ? m_out->PlayedMs() : m_playedMs;
    LeaveCriticalSection(&m_lock);
    return m_startMs + played;
}

void Player::TakePeaks(int& before, int& after)
{
    before = (int)InterlockedExchange(&m_peakIn, 0);
    after = (int)InterlockedExchange(&m_peakOut, 0);
}

unsigned __stdcall Player::ThreadMain(void* self)
{
    static_cast<Player*>(self)->Run();
    return 0;
}

bool Player::Cancelled() const
{
    return WaitForSingleObject(m_cancel, 0) == WAIT_OBJECT_0;
}

void Player::Notify(PlayerState state, const std::wstring& text)
{
    if (!m_notify)
        return;
    wchar_t* copy = text.empty() ? NULL : _wcsdup(text.c_str());
    if (!PostMessageW(m_notify, WM_PLAYER_STATE, MAKEWPARAM(state, m_generation), (LPARAM)copy))
        free(copy);
}

// Applies the Q15 gain in place and records peaks (only this thread writes the peaks
// upward; TakePeaks resets them, so a lost update just means one smaller reading).
static void ApplyGain(std::vector<short>& pcm, LONG gain, volatile LONG* peakIn, volatile LONG* peakOut)
{
    int in = 0, out = 0;
    for (size_t i = 0; i < pcm.size(); ++i) {
        int s = pcm[i];
        int a = s < 0 ? -s : s;
        if (a > in)
            in = a;
        if (gain != kUnityGain) {
            s = (s * gain) >> 15;
            pcm[i] = (short)s;
            a = s < 0 ? -s : s;
        }
        if (a > out)
            out = a;
    }
    if (in > *peakIn)
        InterlockedExchange(peakIn, in);
    if (out > *peakOut)
        InterlockedExchange(peakOut, out);
}

void Player::Run()
{
    HttpRequest req;
    WaveOut out;
    MadDecoder dec;
    out.SetBufferCount(m_bufferCount);
    if (m_clientSkipMs)
        dec.SkipMs(m_clientSkipMs);

    EnterCriticalSection(&m_lock);
    m_req = &req;
    m_out = &out;
    out.SetPaused(m_paused);
    LeaveCriticalSection(&m_lock);

    Notify(PS_BUFFERING, L"Connecting...");
    std::wstring failure;
    bool finished = false;

    HttpResult r = req.Open(m_url, m_opts);
    if (r != HTTP_OK) {
        if (r != HTTP_ERR_CANCELLED && !Cancelled())
            failure = HttpDescribe(r, req.StatusCode(), req.LastError(), HttpIsHttpsUrl(m_url));
    } else {
        std::vector<unsigned char> chunk(kReadChunk);
        std::vector<short> pcm;
        bool started = false;

        while (!Cancelled()) {
            unsigned rate = 0, channels = 0;
            DecodeResult d = dec.Decode(pcm, rate, channels);

            if (d == DEC_NEED_MORE) {
                int n = req.Read(&chunk[0], (int)chunk.size());
                if (n > 0) {
                    dec.Feed(&chunk[0], n);
                } else if (n == 0) {
                    dec.SetEof();
                } else {
                    if (!Cancelled())
                        failure = Fmt(L"The connection dropped while streaming (WinINet error %lu).",
                                      req.LastError());
                    break;
                }
                continue;
            }
            if (d == DEC_END) {
                out.Drain(m_cancel);
                finished = !Cancelled();
                break;
            }
            if (d == DEC_ERROR) {
                failure = L"MP3 decoding failed: " + dec.LastError();
                break;
            }

            // DEC_FRAME. (Re)open the device on the first frame or a mid-stream format change.
            if (!out.IsOpenFor(rate, channels)) {
                out.Drain(m_cancel);
                std::wstring err;
                if (!out.Open(rate, channels, m_device, err)) {
                    failure = err;
                    break;
                }
            }
            ApplyGain(pcm, m_gain, &m_peakIn, &m_peakOut);
            if (!out.Write(&pcm[0], pcm.size(), m_cancel))
                break;
            if (!started && out.IsStarted()) {
                started = true;
                Notify(PS_PLAYING, L"");
            }
        }
    }

    EnterCriticalSection(&m_lock);
    m_playedMs = out.PlayedMs();
    m_req = NULL;
    m_out = NULL;
    LeaveCriticalSection(&m_lock);
    out.Close();

    if (!failure.empty()) {
        LogF(L"Playback error: %ls", failure.c_str());
        Notify(PS_ERROR, failure);
    } else if (finished) {
        Notify(PS_FINISHED, L"");
    }
}
