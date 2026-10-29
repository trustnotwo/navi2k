/* The one translation unit that compiles dr_flac. */
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO        /* we decode from our own HTTP stream callbacks */
#define DR_FLAC_NO_OGG          /* Navidrome serves .flac files; no Ogg FLAC */
#define DR_FLAC_NO_SIMD         /* no SSE2/SSE4.1 paths, even when built with SSE2 enabled */
#define DR_FLAC_NO_WCHAR
#define DR_FLAC_BUFFER_SIZE 16384
#include "dr_flac.h"
