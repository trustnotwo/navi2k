#include "player.h"
#include "flac_decoder.h"
#include "mad_decoder.h"
#include "util.h"
#include "waveout.h"

#include <process.h>
#include <stdlib.h>
#include <vector>

static const int kReadChunk = 16384;
static const LONG kUnityGain = 32768;
// Connect to the next track this long before the current one ends, so its first audio is
// ready before the ~0.5 s queued in waveOut runs out.
static const DWORD kPrefetchMs = 8000;
// LAME's encoder delay plus the MP3 decoder delay (576 + 529 samples): the silence at the
// start of every MP3 that ffmpeg makes. Trimmed when a transcoded track is joined gaplessly.
static const DWORD kMp3DelayFrames = 1105;

// One stream: its connection, and for FLAC the decoder reading it. Opened on the playback
// thread, or ahead of time on a prefetch thread.
struct Player::Source {
    std::wstring url, tag;
    StreamCodec codec;
    DWORD durationMs;
    HttpOptions opts;
    HttpRequest req;
    FlacDecoder* flac;
    bool ok;
    std::wstring err;
    HANDLE thread;

    Source(const std::wstring& u, StreamCodec c, const std::wstring& t, DWORD d, const HttpOptions& o)
        : url(u), tag(t), codec(c), durationMs(d), opts(o), flac(NULL), ok(false), thread(NULL) {}
    ~Source()
    {
        delete flac;   // reads 'req', so goes first
        if (thread)
            CloseHandle(thread);
    }
};

Player::Player()
    : m_notify(NULL), m_thread(NULL), m_req(NULL), m_prefetch(NULL), m_out(NULL), m_paused(false), m_generation(0),
      m_startMs(0), m_clientSkipMs(0), m_codec(CODEC_MP3), m_durationMs(0), m_streamStartMs(0), m_streamFrames(0),
      m_writtenUs(0), m_playedMs(0), m_device(WAVE_MAPPER), m_bufferCount(12), m_volume(100), m_gain(kUnityGain),
      m_peakIn(0), m_peakOut(0), m_hasNext(false), m_nextCodec(CODEC_MP3), m_nextDurationMs(0), m_heardStartMs(0),
      m_heardBaseMs(0), m_switchPending(false), m_switchAtMs(0)
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

bool Player::Play(const std::wstring& url, const HttpOptions& opts, DWORD startMs, DWORD clientSkipMs,
                  StreamCodec codec, DWORD durationMs)
{
    Stop();
    m_url = url;
    m_codec = codec;
    m_opts = opts;
    m_paused = false;
    m_startMs = startMs;
    m_clientSkipMs = clientSkipMs;
    m_durationMs = durationMs;
    m_playedMs = 0;
    m_hasNext = false;
    m_heardStartMs = startMs;
    m_heardBaseMs = 0;
    m_switchPending = false;
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
    if (m_prefetch)
        m_prefetch->req.Abort();
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

void Player::SetNext(const std::wstring& url, StreamCodec codec, const std::wstring& tag, DWORD durationMs)
{
    EnterCriticalSection(&m_lock);
    // Already joined and about to be heard: the UI just hasn't been told yet.
    if (!(m_switchPending && tag == m_switchTag)) {
        m_hasNext = true;
        m_nextUrl = url;
        m_nextCodec = codec;
        m_nextTag = tag;
        m_nextDurationMs = durationMs;
    }
    LeaveCriticalSection(&m_lock);
}

void Player::ClearNext()
{
    EnterCriticalSection(&m_lock);
    m_hasNext = false;
    LeaveCriticalSection(&m_lock);
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
    DWORD pos = m_heardStartMs + (played > m_heardBaseMs ? played - m_heardBaseMs : 0);
    LeaveCriticalSection(&m_lock);
    return pos;
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

// Connects (and for FLAC reads the header). Runs on either thread; Abort() on the request
// cancels it.
void Player::OpenSource(Source* s)
{
    if (s->codec == CODEC_FLAC) {
        s->flac = new FlacDecoder(s->req, s->url, s->opts);
        s->ok = s->flac->Open(s->err);
        return;
    }
    HttpResult r = s->req.Open(s->url, s->opts);
    s->ok = r == HTTP_OK;
    if (!s->ok && r != HTTP_ERR_CANCELLED)
        s->err = HttpDescribe(r, s->req.StatusCode(), s->req.LastError(), HttpIsHttpsUrl(s->url));
}

unsigned __stdcall Player::PrefetchMain(void* source)
{
    OpenSource(static_cast<Source*>(source));
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

bool Player::Output(WaveOut& out, std::vector<short>& pcm, unsigned rate, unsigned channels, bool& started,
                    std::wstring& failure)
{
    // (Re)open the device on the first audio or a mid-stream format change.
    if (!out.IsOpenFor(rate, channels)) {
        out.Drain(m_cancel);
        if (!out.Open(rate, channels, m_device, failure))
            return false;
    }
    ApplyGain(pcm, m_gain, &m_peakIn, &m_peakOut);
    if (!out.Write(&pcm[0], pcm.size(), m_cancel))
        return false;
    const unsigned long long frames = pcm.size() / channels;
    m_writtenUs += frames * 1000000ull / rate;
    m_streamFrames += frames;
    if (!started && out.IsStarted()) {
        started = true;
        Notify(PS_PLAYING, L"");
    }
    CheckSwitch(out, false);
    if (m_durationMs && m_streamStartMs + (DWORD)(m_streamFrames * 1000 / rate) + kPrefetchMs >= m_durationMs)
        MaybePrefetch();
    return true;
}

// Starts connecting to the next track, once.
void Player::MaybePrefetch()
{
    EnterCriticalSection(&m_lock);
    if (m_prefetch || !m_hasNext) {
        LeaveCriticalSection(&m_lock);
        return;
    }
    Source* s = new Source(m_nextUrl, m_nextCodec, m_nextTag, m_nextDurationMs, m_opts);
    m_prefetch = s;
    LeaveCriticalSection(&m_lock);
    s->thread = (HANDLE)_beginthreadex(NULL, 0, PrefetchMain, s, 0, NULL);
    if (!s->thread)
        OpenSource(s);   // no thread to spare: connect now (may cause a short gap)
}

void Player::DropPrefetch()
{
    EnterCriticalSection(&m_lock);
    Source* s = m_prefetch;
    m_prefetch = NULL;
    LeaveCriticalSection(&m_lock);
    if (!s)
        return;
    s->req.Abort();
    if (s->thread)
        WaitForSingleObject(s->thread, INFINITE);
    delete s;
}

// At the end of a track: the next one, connected, or NULL to stop (none set, or cancelled).
Player::Source* Player::TakeNext()
{
    EnterCriticalSection(&m_lock);
    const bool has = m_hasNext;
    const std::wstring url = m_nextUrl, tag = m_nextTag;
    const StreamCodec codec = m_nextCodec;
    const DWORD duration = m_nextDurationMs;
    Source* s = m_prefetch;
    m_hasNext = false;
    LeaveCriticalSection(&m_lock);

    if (!has) {
        DropPrefetch();
        return NULL;
    }
    if (s && (s->url != url || s->tag != tag)) {   // the next track changed after prefetching
        DropPrefetch();
        s = NULL;
    }
    if (!s) {
        s = new Source(url, codec, tag, duration, m_opts);
        EnterCriticalSection(&m_lock);
        m_prefetch = s;   // so Stop() can abort it
        LeaveCriticalSection(&m_lock);
        OpenSource(s);
    } else if (s->thread) {
        HANDLE h[2] = { s->thread, m_cancel };
        WaitForMultipleObjects(2, h, FALSE, INFINITE);
    }
    if (Cancelled()) {
        DropPrefetch();
        return NULL;
    }
    if (s->thread)
        WaitForSingleObject(s->thread, INFINITE);
    // It becomes the stream being decoded in one step, so Stop() can always reach it.
    EnterCriticalSection(&m_lock);
    m_prefetch = NULL;
    if (s->ok)
        m_req = &s->req;
    LeaveCriticalSection(&m_lock);
    return s;
}

// A new track's audio starts at the current end of the device queue.
void Player::BeginSwitch(WaveOut& out, const std::wstring& tag)
{
    CheckSwitch(out, true);   // a very short track: announce the previous join first
    EnterCriticalSection(&m_lock);
    m_switchPending = true;
    m_switchAtMs = (DWORD)(m_writtenUs / 1000);
    m_switchTag = tag;
    LeaveCriticalSection(&m_lock);
}

// Tells the UI once the device reaches a joined track ('force': it has, or soon will).
void Player::CheckSwitch(WaveOut& out, bool force)
{
    bool fire = false;
    std::wstring tag;
    EnterCriticalSection(&m_lock);
    if (m_switchPending && (force || out.PlayedMs() >= m_switchAtMs)) {
        m_heardStartMs = 0;
        m_heardBaseMs = m_switchAtMs;
        m_switchPending = false;
        tag = m_switchTag;
        fire = true;
    }
    LeaveCriticalSection(&m_lock);
    if (fire)
        Notify(PS_NEXT_TRACK, tag);
}

void Player::RunMp3(Source& src, DWORD skipMs, DWORD trimFrames, WaveOut& out, bool& started, bool& ended,
                    std::wstring& failure)
{
    MadDecoder dec;
    if (skipMs)
        dec.SkipMs(skipMs);

    HttpRequest& req = src.req;
    std::vector<unsigned char> chunk(kReadChunk);
    std::vector<short> pcm;

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
                    failure = Fmt(L"The connection dropped while streaming (WinINet error %lu).", req.LastError());
                return;
            }
            continue;
        }
        if (d == DEC_END) {
            ended = true;
            return;
        }
        if (d == DEC_ERROR) {
            failure = L"MP3 decoding failed: " + dec.LastError();
            return;
        }
        if (trimFrames && channels) {
            DWORD frames = (DWORD)(pcm.size() / channels);
            DWORD drop = trimFrames < frames ? trimFrames : frames;
            pcm.erase(pcm.begin(), pcm.begin() + drop * channels);
            trimFrames -= drop;
            if (pcm.empty())
                continue;
        }
        if (!Output(out, pcm, rate, channels, started, failure))
            return;
    }
}

void Player::RunFlac(Source& src, DWORD skipMs, WaveOut& out, bool& started, bool& ended, std::wstring& failure)
{
    FlacDecoder& dec = *src.flac;
    if (dec.Channels() > 2) {
        failure = Fmt(L"This FLAC file has %u channels; only mono and stereo can be played. "
                      L"Choose an MP3 quality in File > Settings to play it.", dec.Channels());
        return;
    }
    if (skipMs && !dec.SeekMs(skipMs)) {
        if (!Cancelled())
            failure = dec.Failed() ? L"The connection dropped while seeking (" + dec.FailureText() + L")."
                                   : std::wstring(L"Could not seek in this FLAC file.");
        return;
    }

    // About 0.1 s per block: small enough to keep volume changes and Stop responsive.
    const unsigned block = dec.Rate() / 10 ? dec.Rate() / 10 : 4096;
    std::vector<short> pcm;
    while (!Cancelled()) {
        if (dec.Read(pcm, block) == 0) {
            if (Cancelled())
                return;
            if (dec.Failed()) {
                failure = L"The connection dropped while streaming (" + dec.FailureText() + L").";
                return;
            }
            ended = true;
            return;
        }
        if (!Output(out, pcm, dec.Rate(), dec.Channels(), started, failure))
            return;
    }
}

void Player::Run()
{
    WaveOut out;
    out.SetBufferCount(m_bufferCount);
    m_writtenUs = 0;
    Source* src = new Source(m_url, m_codec, std::wstring(), m_durationMs, m_opts);

    EnterCriticalSection(&m_lock);
    m_req = &src->req;
    m_out = &out;
    out.SetPaused(m_paused);
    LeaveCriticalSection(&m_lock);

    Notify(PS_BUFFERING, L"Connecting...");
    OpenSource(src);

    std::wstring failure;
    bool finished = false, started = false;
    DWORD skip = m_clientSkipMs, trim = 0;
    m_streamStartMs = m_startMs;
    for (;;) {
        if (!src->ok) {
            if (!Cancelled())
                failure = src->err;
            break;
        }
        m_durationMs = src->durationMs;
        m_streamFrames = 0;
        bool ended = false;
        if (src->codec == CODEC_FLAC)
            RunFlac(*src, skip, out, started, ended, failure);
        else
            RunMp3(*src, skip, trim, out, started, ended, failure);
        if (!ended || Cancelled())
            break;

        // Gapless: carry on with the next track on the same device. It's seamless if the next
        // track is ready before the audio still queued runs out.
        const DWORD queuedMs = (DWORD)(m_writtenUs / 1000) - out.PlayedMs();
        const DWORD waitStart = GetTickCount();
        Source* next = TakeNext();
        if (next && next->ok)
            LogF(L"Gapless join: next track ready after %lu ms, %lu ms of audio was queued", GetTickCount() - waitStart,
                 queuedMs);
        if (next && !next->ok) {
            // Couldn't connect: finish normally, and the UI starts it (and reports the error).
            LogF(L"Gapless: could not open the next track: %ls", next->err.c_str());
            delete next;
            next = NULL;
        }
        if (!next) {
            out.Drain(m_cancel);
            CheckSwitch(out, true);
            finished = !Cancelled();
            break;
        }
        delete src;   // TakeNext() already pointed m_req at next
        src = next;
        BeginSwitch(out, src->tag);
        skip = 0;
        trim = src->codec == CODEC_MP3 ? kMp3DelayFrames : 0;
        m_streamStartMs = 0;
    }
    if (Cancelled())
        failure.clear();

    EnterCriticalSection(&m_lock);
    m_playedMs = out.PlayedMs();
    m_req = NULL;
    m_out = NULL;
    LeaveCriticalSection(&m_lock);
    DropPrefetch();
    out.Close();
    delete src;

    if (!failure.empty()) {
        LogF(L"Playback error: %ls", failure.c_str());
        Notify(PS_ERROR, failure);
    } else if (finished) {
        Notify(PS_FINISHED, L"");
    }
}
