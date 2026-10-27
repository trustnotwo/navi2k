#include "mad_decoder.h"
#include "pcm.h"
#include "util.h"

#include <string.h>

// The pieces of the generated mad.h, included directly from the libmad source tree.
extern "C" {
#include "version.h"
#include "fixed.h"
#include "bit.h"
#include "timer.h"
#include "stream.h"
#include "frame.h"
#include "synth.h"
}

struct MadDecoder::State {
    mad_stream stream;
    mad_frame frame;
    mad_synth synth;
    std::vector<unsigned char> buf;
    size_t len;
    bool eof;
    unsigned long skipMs;
    std::string error;
};

// Size of an ID3v2 tag at p (header + body + optional footer), or 0.
// Skipping it stops libmad from "finding" frame syncs inside embedded cover art.
static long Id3v2TagSize(const unsigned char* p, long avail)
{
    if (avail < 10 || p[0] != 'I' || p[1] != 'D' || p[2] != '3')
        return 0;
    if (p[3] == 0xFF || p[4] == 0xFF || ((p[6] | p[7] | p[8] | p[9]) & 0x80))
        return 0;
    long size = ((long)p[6] << 21) | ((long)p[7] << 14) | ((long)p[8] << 7) | p[9];
    size += 10;
    if (p[5] & 0x10)
        size += 10;
    return size;
}

MadDecoder::MadDecoder() : m_s(new State)
{
    mad_stream_init(&m_s->stream);
    mad_frame_init(&m_s->frame);
    mad_synth_init(&m_s->synth);
    m_s->len = 0;
    m_s->eof = false;
    m_s->skipMs = 0;
}

void MadDecoder::SkipMs(unsigned long ms)
{
    m_s->skipMs = ms;
}

MadDecoder::~MadDecoder()
{
    mad_synth_finish(&m_s->synth);
    mad_frame_finish(&m_s->frame);
    mad_stream_finish(&m_s->stream);
    delete m_s;
}

void MadDecoder::Feed(const unsigned char* data, size_t n)
{
    State& s = *m_s;
    if (n == 0)
        return;

    // Keep the undecoded tail (a partial frame) and append the new bytes after it.
    size_t keep = s.len;
    if (s.stream.buffer && s.stream.next_frame) {
        keep = s.len - (size_t)(s.stream.next_frame - &s.buf[0]);
        if (keep && s.stream.next_frame != &s.buf[0])
            memmove(&s.buf[0], s.stream.next_frame, keep);
    }
    if (s.buf.size() < keep + n)
        s.buf.resize(keep + n);
    memcpy(&s.buf[keep], data, n);
    s.len = keep + n;
    mad_stream_buffer(&s.stream, &s.buf[0], s.len);
}

void MadDecoder::SetEof()
{
    if (m_s->eof)
        return;
    m_s->eof = true;
    // libmad needs MAD_BUFFER_GUARD zero bytes after the last frame to decode it.
    unsigned char guard[MAD_BUFFER_GUARD];
    memset(guard, 0, sizeof(guard));
    Feed(guard, sizeof(guard));
}

DecodeResult MadDecoder::Decode(std::vector<short>& pcm, unsigned& rate, unsigned& channels)
{
    State& s = *m_s;
    for (;;) {
        if (s.len == 0)
            return s.eof ? DEC_END : DEC_NEED_MORE;

        const bool skipping = s.skipMs > 0;
        int rc = skipping ? mad_header_decode(&s.frame.header, &s.stream)
                          : mad_frame_decode(&s.frame, &s.stream);
        if (rc == 0 && skipping) {
            unsigned long d = (unsigned long)mad_timer_count(s.frame.header.duration, MAD_UNITS_MILLISECONDS);
            s.skipMs = s.skipMs > d ? s.skipMs - d : 0;
            // Header-only decode leaves the frame flagged incomplete; clear it so the next
            // mad_frame_decode starts at the following frame instead of this skipped one.
            s.frame.header.flags &= ~MAD_FLAG_INCOMPLETE;
            continue;
        }
        if (rc == -1) {
            if (s.stream.error == MAD_ERROR_BUFLEN)
                return s.eof ? DEC_END : DEC_NEED_MORE;
            if (MAD_RECOVERABLE(s.stream.error)) {
                if (s.stream.error == MAD_ERROR_LOSTSYNC) {
                    long tag = Id3v2TagSize(s.stream.this_frame, (long)(s.stream.bufend - s.stream.this_frame));
                    if (tag > 0)
                        mad_stream_skip(&s.stream, tag);
                }
                continue;   // corrupt frame: libmad resyncs on the next one
            }
            s.error = mad_stream_errorstr(&s.stream);
            return DEC_ERROR;
        }

        mad_synth_frame(&s.synth, &s.frame);
        const mad_pcm& p = s.synth.pcm;
        if (p.length == 0)
            continue;

        rate = p.samplerate;
        channels = p.channels;
        pcm.resize((size_t)p.length * p.channels);
        PcmConvert(p.samples[0], p.channels == 2 ? p.samples[1] : NULL, p.length, &pcm[0]);
        return DEC_FRAME;
    }
}

std::wstring MadDecoder::LastError() const
{
    return Utf8ToWide(m_s->error);
}
