#ifndef LP_MAD_DECODER_H
#define LP_MAD_DECODER_H

#include <stddef.h>
#include <string>
#include <vector>

enum DecodeResult {
    DEC_FRAME,       // pcm/rate/channels filled
    DEC_NEED_MORE,   // call Feed() (or SetEof()) and try again
    DEC_END,         // stream finished
    DEC_ERROR        // unrecoverable; see LastError()
};

// Push-style MP3 decoder on top of libmad (integer fixed-point: no FPU/SIMD requirements).
class MadDecoder {
public:
    MadDecoder();
    ~MadDecoder();

    void Feed(const unsigned char* data, size_t len);
    void SetEof();
    // Discard this much audio from the start by parsing frame headers only (no decoding),
    // for seeking in streams the server sends from the beginning.
    void SkipMs(unsigned long ms);
    DecodeResult Decode(std::vector<short>& pcm, unsigned& rate, unsigned& channels);
    std::wstring LastError() const;

private:
    MadDecoder(const MadDecoder&);
    MadDecoder& operator=(const MadDecoder&);
    struct State;
    State* m_s;
};

#endif
