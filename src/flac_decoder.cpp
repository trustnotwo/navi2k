#include "flac_decoder.h"
#include "util.h"

#include "dr_flac.h"

static drflac* Dr(void* handle) { return (drflac*)handle; }

// Forward gaps up to this size are read and thrown away; bigger ones (cover art blocks,
// seeks) reopen the request at the new offset.
static const unsigned long long kSkipByReading = 128 * 1024;

FlacDecoder::FlacDecoder(HttpRequest& req, const std::wstring& url, const HttpOptions& opts)
    : m_req(req), m_url(url), m_opts(opts), m_handle(NULL), m_pos(0), m_netPos(0), m_size(0), m_fetched(0),
      m_reconnects(0), m_connected(false), m_failed(false), m_noRanges(false), m_failStatus(0)
{
}

std::wstring FlacDecoder::FailureText() const
{
    return m_failStatus ? Fmt(L"HTTP %lu", m_failStatus) : Fmt(L"WinINet error %lu", m_req.LastError());
}

FlacDecoder::~FlacDecoder()
{
    if (m_handle)
        drflac_close(Dr(m_handle));
}



bool FlacDecoder::Open(std::wstring& err)
{
    HttpResult r = m_req.Open(m_url, m_opts);
    if (r != HTTP_OK) {
        err = r == HTTP_ERR_CANCELLED ? std::wstring()
                                      : HttpDescribe(r, m_req.StatusCode(), m_req.LastError(), HttpIsHttpsUrl(m_url));
        return false;
    }
    m_connected = true;
    m_size = m_req.ContentLength();
    m_handle = drflac_open((drflac_read_proc)OnRead, (drflac_seek_proc)OnSeek, (drflac_tell_proc)OnTell, this, NULL);
    if (!Dr(m_handle)) {
        err = m_failed ? L"The connection dropped while opening the FLAC file (" + FailureText() + L")."
                       : std::wstring(L"The server's file isn't a FLAC stream this player can read.");
        return false;
    }
    LogF(L"FLAC %u Hz, %u ch, %u bit, %I64u frames, %I64u bytes", Dr(m_handle)->sampleRate, Dr(m_handle)->channels,
         Dr(m_handle)->bitsPerSample, (unsigned long long)Dr(m_handle)->totalPCMFrameCount, m_size);
    return true;
}

unsigned FlacDecoder::Rate() const { return Dr(m_handle) ? Dr(m_handle)->sampleRate : 0; }
unsigned FlacDecoder::Channels() const { return Dr(m_handle) ? Dr(m_handle)->channels : 0; }
unsigned FlacDecoder::BitsPerSample() const { return Dr(m_handle) ? Dr(m_handle)->bitsPerSample : 0; }
unsigned long long FlacDecoder::TotalFrames() const { return Dr(m_handle) ? Dr(m_handle)->totalPCMFrameCount : 0; }

bool FlacDecoder::SeekMs(unsigned long ms)
{
    if (!Dr(m_handle))
        return false;
    drflac_uint64 frame = (drflac_uint64)ms * Dr(m_handle)->sampleRate / 1000;
    if (Dr(m_handle)->totalPCMFrameCount && frame >= Dr(m_handle)->totalPCMFrameCount)
        return false;
    return drflac_seek_to_pcm_frame(Dr(m_handle), frame) != DRFLAC_FALSE;
}

unsigned FlacDecoder::Read(std::vector<short>& pcm, unsigned maxFrames)
{
    if (!Dr(m_handle))
        return 0;
    pcm.resize((size_t)maxFrames * Dr(m_handle)->channels);
    drflac_uint64 got = drflac_read_pcm_frames_s16(Dr(m_handle), maxFrames, &pcm[0]);
    pcm.resize((size_t)got * Dr(m_handle)->channels);
    return (unsigned)got;
}

// --- byte stream for dr_flac ----------------------------------------------------------------

bool FlacDecoder::Reconnect(unsigned long long pos)
{
    if (pos > 0xFFFFFFFFull)
        return false;
    ++m_reconnects;
    if (m_noRanges)
        pos = 0;
    HttpResult r = m_req.Open(m_url, m_opts, (DWORD)pos);
    if (r != HTTP_OK && r != HTTP_ERR_CANCELLED && pos > 0 && m_req.StatusCode() != 416) {
        // Some servers, proxies and old WinINet versions don't answer range requests properly:
        // read from the start instead, for this and every later jump.
        LogF(L"FLAC range request at byte %I64u failed (HTTP %lu, WinINet error %lu); "
             L"reading from the start of the file instead.", pos, m_req.StatusCode(), m_req.LastError());
        m_noRanges = true;
        pos = 0;
        r = m_req.Open(m_url, m_opts);
    }
    if (r != HTTP_OK) {
        // 416 = past the end: dr_flac probes beyond the file when seeking and expects a refusal.
        if (r != HTTP_ERR_CANCELLED && m_req.StatusCode() != 416) {
            m_failed = true;
            m_failStatus = r == HTTP_ERR_STATUS ? m_req.StatusCode() : 0;
            LogF(L"FLAC request at byte %I64u failed: HTTP %lu, WinINet error %lu", pos, m_req.StatusCode(),
                 m_req.LastError());
        }
        m_connected = false;
        return false;
    }
    m_connected = true;
    m_netPos = pos;
    if (!m_size && pos == 0)
        m_size = m_req.ContentLength();
    else if (!m_size && m_req.ContentLength())
        m_size = pos + m_req.ContentLength();
    return true;
}

size_t FlacDecoder::ReadAt(void* buf, size_t bytes)
{
    if (m_failed || (m_size && m_pos >= m_size))
        return 0;
    if (!m_connected || m_pos < m_netPos || (!m_noRanges && m_pos - m_netPos > kSkipByReading)) {
        if (!Reconnect(m_pos))
            return 0;
    }
    char scrap[8192];
    while (m_netPos < m_pos) {   // short forward gap: read past it
        unsigned long long gap = m_pos - m_netPos;
        int n = m_req.Read(scrap, gap < sizeof(scrap) ? (int)gap : (int)sizeof(scrap));
        if (n <= 0) {
            m_failed = n < 0;
            if (n < 0)
                LogF(L"FLAC read failed at byte %I64u: WinINet error %lu", m_netPos, m_req.LastError());
            m_connected = false;
            return 0;
        }
        m_netPos += n;
        m_fetched += n;
    }
    size_t total = 0;
    while (total < bytes) {
        int n = m_req.Read((char*)buf + total, (int)(bytes - total));
        if (n < 0) {
            m_failed = true;
            m_connected = false;
            LogF(L"FLAC read failed at byte %I64u: WinINet error %lu", m_netPos + total, m_req.LastError());
            break;
        }
        if (n == 0)
            break;
        total += n;
    }
    m_pos += total;
    m_netPos += total;
    m_fetched += total;
    return total;
}

size_t FlacDecoder::OnRead(void* self, void* buf, size_t bytes)
{
    return static_cast<FlacDecoder*>(self)->ReadAt(buf, bytes);
}

unsigned FlacDecoder::OnSeek(void* self, int offset, int origin)
{
    // Only moves the cursor; the network catches up lazily on the next read, so dr_flac's
    // "seek to the end, tell, seek back" size check costs nothing.
    FlacDecoder* d = static_cast<FlacDecoder*>(self);
    unsigned long long target;
    if (origin == DRFLAC_SEEK_SET)
        target = (unsigned long long)offset;
    else if (origin == DRFLAC_SEEK_CUR)
        target = d->m_pos + (unsigned long long)offset;
    else if (d->m_size)
        target = d->m_size + (unsigned long long)offset;
    else
        return DRFLAC_FALSE;   // end unknown
    if (d->m_size && target > d->m_size)
        return DRFLAC_FALSE;
    d->m_pos = target;
    return DRFLAC_TRUE;
}

unsigned FlacDecoder::OnTell(void* self, long long* cursor)
{
    *cursor = (long long)static_cast<FlacDecoder*>(self)->m_pos;
    return DRFLAC_TRUE;
}
