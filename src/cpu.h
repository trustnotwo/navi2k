#ifndef LP_CPU_H
#define LP_CPU_H

#include <string>

// Build-configuration macros come from CMake (LEGACY_NO_SSE2 / LEGACY_SSE2_DISPATCH).
#ifndef LP_BUILD_NO_SSE2
#define LP_BUILD_NO_SSE2 1
#endif
#ifndef LP_SSE2_DISPATCH
#define LP_SSE2_DISPATCH 0
#endif
#ifndef LP_REQUIRE_CMOV
#define LP_REQUIRE_CMOV 1
#endif

// Catch a misconfigured build at compile time rather than on a Pentium III.
#if LP_BUILD_NO_SSE2 && defined(__SSE2__)
#error "LP_BUILD_NO_SSE2 is set but the compiler is generating SSE2 code (check -march / -mno-sse2)."
#endif

struct CpuCaps {
    bool cpuid;     // CPUID instruction available
    bool cmov;      // P6 conditional moves (Pentium Pro/II and later; not Pentium MMX or K6)
    bool mmx;
    bool sse;
    bool sse2;
    bool osXmm;     // OS saves XMM state across context switches
    char vendor[13];

    // SSE2 is usable only if the CPU has it AND the OS preserves XMM registers.
    // (IsProcessorFeaturePresent(PF_XMMI64_...) can't be used: it always says FALSE on Win2000.)
    bool Sse2Usable() const { return sse2 && osXmm; }
};

CpuCaps DetectCpu();

// False if this .exe was compiled to require SSE/SSE2 and the CPU lacks it; 'message' explains.
bool CpuCheckBuildCompatible(const CpuCaps& caps, std::wstring& message);

// Turns an illegal-instruction crash (e.g. SSE2 opcode on a Pentium II) into a readable
// message instead of a Dr. Watson dump. Install first thing in WinMain.
void InstallIllegalInstructionHandler();

#endif
