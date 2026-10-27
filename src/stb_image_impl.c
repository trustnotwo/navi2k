/* The one translation unit that compiles stb_image (JPEG + PNG only). */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_SIMD            /* no SSE2 paths, even when built with SSE2 enabled */
#define STBI_NO_THREAD_LOCALS   /* __thread would pull in libgcc emutls */
#define STBI_MAX_DIMENSIONS 4096
#include "stb_image.h"
