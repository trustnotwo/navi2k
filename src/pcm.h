#ifndef LP_PCM_H
#define LP_PCM_H

// Converts libmad's 4.28 fixed-point samples to interleaved signed 16-bit PCM.
// 'right' is NULL for mono. 'out' receives count (mono) or 2*count (stereo) samples.
typedef void (*PcmConvertFn)(const int* left, const int* right, unsigned count, short* out);

// Picks the converter once at startup. With sse2Usable == false (or a build without
// LP_SSE2_DISPATCH) the portable integer path is used; output is bit-identical either way.
// Returns a short description for the log/status bar.
const wchar_t* PcmInit(bool sse2Usable);

void PcmConvert(const int* left, const int* right, unsigned count, short* out);

#endif
