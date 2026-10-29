#ifndef LP_FLAC_DECODER_H
#define LP_FLAC_DECODER_H

#include "http.h"
#include <string>
#include <vector>

// Pull-style FLAC decoder (dr_flac, integer only) reading an original .flac file over HTTP.
// Seeking reopens the request with a Range header, so it works on untranscoded streams that
// the server can't offset; short forward skips just read past the data. If a range request
// fails, it reads from the start of the file up to the wanted spot instead (slower, but works).
class FlacDecoder {
public:
    // 'req' is owned by the caller, so another thread can Abort() it.
    FlacDecoder(HttpRequest& req, const std::wstring& url, const HttpOptions& opts);
    ~FlacDecoder();

    // Connects and reads the FLAC header. False with 'err' set on failure.
    bool Open(std::wstring& err);
    unsigned Rate() const;
    unsigned Channels() const;
    unsigned BitsPerSample() const;
    unsigned long long TotalFrames() const;

    bool SeekMs(unsigned long ms);
    // Decodes up to maxFrames into interleaved 16-bit PCM (24-bit sources are reduced to 16).
    // Returns frames decoded; 0 at the end of the stream or on a network error (see Failed()).
    unsigned Read(std::vector<short>& pcm, unsigned maxFrames);

    // True if reading stopped because the connection failed rather than at the end.
    bool Failed() const { return m_failed; }
    // What went wrong, for messages: "HTTP 500" or "WinINet error 12030".
    std::wstring FailureText() const;
    unsigned long long BytesFetched() const { return m_fetched; }
    unsigned Reconnects() const { return m_reconnects; }
    // Testing: behave as if range requests fail (navi2k_probe flac-norange).
    void DisableRanges() { m_noRanges = true; }

private:
    FlacDecoder(const FlacDecoder&);
    FlacDecoder& operator=(const FlacDecoder&);
    static size_t OnRead(void* self, void* buf, size_t bytes);
    static unsigned OnSeek(void* self, int offset, int origin);
    static unsigned OnTell(void* self, long long* cursor);
    size_t ReadAt(void* buf, size_t bytes);
    bool Reconnect(unsigned long long pos);

    HttpRequest& m_req;
    std::wstring m_url;
    HttpOptions m_opts;
    void* m_handle;           // drflac* (an anonymous struct in dr_flac.h, so not declarable here)
    unsigned long long m_pos;       // where the decoder thinks it is
    unsigned long long m_netPos;    // where the open HTTP body is
    unsigned long long m_size;      // whole file, 0 if unknown
    unsigned long long m_fetched;
    unsigned m_reconnects;
    bool m_connected;
    bool m_failed;
    bool m_noRanges;          // a range request failed: always read from the start instead
    DWORD m_failStatus;       // HTTP status of the failed request, 0 if it was a network error
};

#endif
