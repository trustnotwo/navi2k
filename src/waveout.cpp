#include "waveout.h"
#include "util.h"

#include <string.h>

WaveOut::WaveOut()
    : m_wo(NULL), m_numBuffers(12), m_prebuffer(4), m_cur(0), m_fill(0), m_rate(0), m_channels(0),
      m_submitted(0), m_started(false), m_paused(false), m_closedMs(0)
{
    InitializeCriticalSection(&m_lock);
    m_doneEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    m_mem = new char[MAX_BUFFERS * BUFFER_BYTES];
    ZeroMemory(m_hdr, sizeof(m_hdr));
    for (int i = 0; i < MAX_BUFFERS; ++i)
        m_queued[i] = false;
}

void WaveOut::SetBufferCount(int count)
{
    if (m_wo)
        return;
    if (count < 4)
        count = 4;
    if (count > MAX_BUFFERS)
        count = MAX_BUFFERS;
    m_numBuffers = count;
    m_prebuffer = count / 3 > 2 ? count / 3 : 2;
}

WaveOut::~WaveOut()
{
    Close();
    delete[] m_mem;
    CloseHandle(m_doneEvent);
    DeleteCriticalSection(&m_lock);
}

bool WaveOut::Open(unsigned rate, unsigned channels, UINT device, std::wstring& err)
{
    Close();

    WAVEFORMATEX wf;
    ZeroMemory(&wf, sizeof(wf));
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = (WORD)channels;
    wf.nSamplesPerSec = rate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = (WORD)(channels * 2);
    wf.nAvgBytesPerSec = rate * wf.nBlockAlign;

    EnterCriticalSection(&m_lock);
    MMRESULT mr = waveOutOpen(&m_wo, device, &wf, (DWORD_PTR)m_doneEvent, 0, CALLBACK_EVENT);
    if (mr != MMSYSERR_NOERROR && device != WAVE_MAPPER) {
        LogF(L"waveOutOpen on device %u failed (%u); using the default device", device, mr);
        mr = waveOutOpen(&m_wo, WAVE_MAPPER, &wf, (DWORD_PTR)m_doneEvent, 0, CALLBACK_EVENT);
    }
    if (mr != MMSYSERR_NOERROR) {
        m_wo = NULL;
        LeaveCriticalSection(&m_lock);
        wchar_t text[MAXERRORLENGTH] = L"";
        waveOutGetErrorTextW(mr, text, MAXERRORLENGTH);
        err = Fmt(L"Could not open the sound device for %u Hz, %u channel(s): %ls", rate, channels, text);
        return false;
    }
    // Hold output until m_prebuffer buffers are queued, so a slow network start doesn't stutter.
    waveOutPause(m_wo);
    m_rate = rate;
    m_channels = channels;
    m_cur = 0;
    m_fill = 0;
    m_submitted = 0;
    m_started = false;
    LeaveCriticalSection(&m_lock);
    return true;
}

void WaveOut::Close()
{
    EnterCriticalSection(&m_lock);
    if (m_wo) {
        m_closedMs += DevicePositionMs();
        waveOutReset(m_wo);   // marks every queued buffer done
        for (int i = 0; i < MAX_BUFFERS; ++i) {
            if (m_queued[i]) {
                waveOutUnprepareHeader(m_wo, &m_hdr[i], sizeof(WAVEHDR));
                m_queued[i] = false;
            }
        }
        waveOutClose(m_wo);
        m_wo = NULL;
    }
    m_rate = m_channels = 0;
    m_cur = 0;
    m_fill = 0;
    m_submitted = 0;
    m_started = false;
    LeaveCriticalSection(&m_lock);
}

bool WaveOut::IsOpenFor(unsigned rate, unsigned channels) const
{
    return m_wo && m_rate == rate && m_channels == channels;
}

bool WaveOut::WaitForBuffer(int i, HANDLE cancel)
{
    while (m_queued[i]) {
        // dwFlags is updated by the driver thread.
        if (*(volatile DWORD*)&m_hdr[i].dwFlags & WHDR_DONE) {
            EnterCriticalSection(&m_lock);
            waveOutUnprepareHeader(m_wo, &m_hdr[i], sizeof(WAVEHDR));
            m_queued[i] = false;
            LeaveCriticalSection(&m_lock);
            break;
        }
        // Timeout guards against a missed auto-reset signal; also keeps pause responsive.
        HANDLE h[2] = { cancel, m_doneEvent };
        if (WaitForMultipleObjects(2, h, FALSE, 250) == WAIT_OBJECT_0)
            return false;
    }
    return true;
}

bool WaveOut::Submit()
{
    WAVEHDR& h = m_hdr[m_cur];
    ZeroMemory(&h, sizeof(h));
    h.lpData = m_mem + m_cur * BUFFER_BYTES;
    h.dwBufferLength = m_fill;

    EnterCriticalSection(&m_lock);
    MMRESULT mr = waveOutPrepareHeader(m_wo, &h, sizeof(h));
    if (mr == MMSYSERR_NOERROR) {
        mr = waveOutWrite(m_wo, &h, sizeof(h));
        if (mr != MMSYSERR_NOERROR)
            waveOutUnprepareHeader(m_wo, &h, sizeof(h));
    }
    if (mr == MMSYSERR_NOERROR) {
        m_queued[m_cur] = true;
        if (!m_started && ++m_submitted >= m_prebuffer) {
            m_started = true;
            if (!m_paused)
                waveOutRestart(m_wo);
        }
    }
    LeaveCriticalSection(&m_lock);

    m_cur = (m_cur + 1) % m_numBuffers;
    m_fill = 0;
    if (mr != MMSYSERR_NOERROR)
        LogF(L"waveOutWrite failed: %u", mr);
    return mr == MMSYSERR_NOERROR;
}

bool WaveOut::Write(const short* samples, size_t count, HANDLE cancel)
{
    if (!m_wo)
        return false;
    const char* src = (const char*)samples;
    size_t bytes = count * sizeof(short);
    while (bytes > 0) {
        if (m_fill == 0 && !WaitForBuffer(m_cur, cancel))
            return false;
        DWORD n = BUFFER_BYTES - m_fill;
        if (n > bytes)
            n = (DWORD)bytes;
        memcpy(m_mem + m_cur * BUFFER_BYTES + m_fill, src, n);
        m_fill += n;
        src += n;
        bytes -= n;
        if (m_fill == BUFFER_BYTES)
            Submit();
    }
    return true;
}

void WaveOut::Drain(HANDLE cancel)
{
    if (!m_wo)
        return;
    if (m_fill > 0)
        Submit();

    EnterCriticalSection(&m_lock);
    if (!m_started) {   // track shorter than the prebuffer
        m_started = true;
        if (!m_paused)
            waveOutRestart(m_wo);
    }
    LeaveCriticalSection(&m_lock);

    for (int i = 0; i < m_numBuffers; ++i)
        if (!WaitForBuffer(i, cancel))
            return;
}

DWORD WaveOut::DevicePositionMs()
{
    if (!m_wo || !m_rate)
        return 0;
    MMTIME mt;
    ZeroMemory(&mt, sizeof(mt));
    mt.wType = TIME_SAMPLES;
    if (waveOutGetPosition(m_wo, &mt, sizeof(mt)) != MMSYSERR_NOERROR)
        return 0;
    // Drivers may answer in another unit than requested.
    unsigned __int64 samples;
    if (mt.wType == TIME_SAMPLES)
        samples = mt.u.sample;
    else if (mt.wType == TIME_BYTES)
        samples = mt.u.cb / (m_channels * 2);
    else if (mt.wType == TIME_MS)
        return mt.u.ms;
    else
        return 0;
    return (DWORD)(samples * 1000 / m_rate);
}

DWORD WaveOut::PlayedMs()
{
    EnterCriticalSection(&m_lock);
    DWORD ms = m_closedMs + DevicePositionMs();
    LeaveCriticalSection(&m_lock);
    return ms;
}

void WaveOut::SetPaused(bool paused)
{
    EnterCriticalSection(&m_lock);
    m_paused = paused;
    if (m_wo && m_started) {
        if (paused)
            waveOutPause(m_wo);
        else
            waveOutRestart(m_wo);
    }
    LeaveCriticalSection(&m_lock);
}
