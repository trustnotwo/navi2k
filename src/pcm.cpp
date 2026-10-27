#include "pcm.h"
#include "cpu.h"

#include <stddef.h>

// Must match MAD_F_FRACBITS in libmad's fixed.h (28 for every FPM_* backend).
#define MAD_FRACBITS 28

static inline short ScaleSample(int s)
{
    s += 1 << (MAD_FRACBITS - 16);                  // round
    if (s >= (1 << MAD_FRACBITS))
        s = (1 << MAD_FRACBITS) - 1;                // clip
    else if (s < -(1 << MAD_FRACBITS))
        s = -(1 << MAD_FRACBITS);
    return (short)(s >> (MAD_FRACBITS + 1 - 16));   // quantize
}

static void ConvertGeneric(const int* left, const int* right, unsigned count, short* out)
{
    if (right) {
        for (unsigned i = 0; i < count; ++i) {
            *out++ = ScaleSample(left[i]);
            *out++ = ScaleSample(right[i]);
        }
    } else {
        for (unsigned i = 0; i < count; ++i)
            *out++ = ScaleSample(left[i]);
    }
}

#if LP_SSE2_DISPATCH
#include <emmintrin.h>

// The only SSE2 code in the program. It is compiled for SSE2 via the target attribute
// (everything else is built with -mno-sse2) and called only after CPUID confirms SSE2.
// force_align_arg_pointer: Win32 only guarantees a 4-byte aligned stack.
// Saturating packs give the same result as ScaleSample's explicit clip.
extern "C" __attribute__((target("sse2"), force_align_arg_pointer, noinline))
void LpConvertPcmSse2(const int* left, const int* right, unsigned count, short* out)
{
    const __m128i round = _mm_set1_epi32(1 << (MAD_FRACBITS - 16));
    unsigned i = 0;
    if (right) {
        for (; i + 4 <= count; i += 4) {
            __m128i l = _mm_srai_epi32(_mm_add_epi32(_mm_loadu_si128((const __m128i*)(left + i)), round), MAD_FRACBITS + 1 - 16);
            __m128i r = _mm_srai_epi32(_mm_add_epi32(_mm_loadu_si128((const __m128i*)(right + i)), round), MAD_FRACBITS + 1 - 16);
            __m128i l16 = _mm_packs_epi32(l, l);
            __m128i r16 = _mm_packs_epi32(r, r);
            _mm_storeu_si128((__m128i*)out, _mm_unpacklo_epi16(l16, r16));
            out += 8;
        }
    } else {
        for (; i + 8 <= count; i += 8) {
            __m128i a = _mm_srai_epi32(_mm_add_epi32(_mm_loadu_si128((const __m128i*)(left + i)), round), MAD_FRACBITS + 1 - 16);
            __m128i b = _mm_srai_epi32(_mm_add_epi32(_mm_loadu_si128((const __m128i*)(left + i + 4)), round), MAD_FRACBITS + 1 - 16);
            _mm_storeu_si128((__m128i*)out, _mm_packs_epi32(a, b));
            out += 8;
        }
    }
    ConvertGeneric(left + i, right ? right + i : NULL, count - i, out);
}
#endif

static PcmConvertFn g_convert = ConvertGeneric;

const wchar_t* PcmInit(bool sse2Usable)
{
#if LP_SSE2_DISPATCH
    if (sse2Usable) {
        g_convert = LpConvertPcmSse2;
        return L"SSE2 PCM conversion";
    }
    return L"integer PCM conversion (no SSE2 on this CPU)";
#else
    (void)sse2Usable;
    return L"integer PCM conversion (SSE2 paths not compiled in)";
#endif
}

void PcmConvert(const int* left, const int* right, unsigned count, short* out)
{
    g_convert(left, right, count, out);
}
