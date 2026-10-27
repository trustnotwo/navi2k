#include "cpu.h"

#include <windows.h>
#include <cpuid.h>
#include <string.h>

CpuCaps DetectCpu()
{
    CpuCaps c;
    memset(&c, 0, sizeof(c));

    // Returns 0 when the EFLAGS.ID bit can't be toggled, i.e. no CPUID (early 486).
    unsigned maxLeaf = __get_cpuid_max(0, NULL);
    if (maxLeaf == 0)
        return c;
    c.cpuid = true;

    unsigned a, b, cx, d;
    __cpuid(0, a, b, cx, d);
    memcpy(c.vendor + 0, &b, 4);
    memcpy(c.vendor + 4, &d, 4);
    memcpy(c.vendor + 8, &cx, 4);
    c.vendor[12] = 0;

    if (maxLeaf >= 1) {
        __cpuid(1, a, b, cx, d);
        c.cmov = (d & bit_CMOV) != 0;
        c.mmx  = (d & bit_MMX) != 0;
        c.sse  = (d & bit_SSE) != 0;
        c.sse2 = (d & bit_SSE2) != 0;
    }

    // PF_XMMI_INSTRUCTIONS_AVAILABLE is TRUE only when the kernel enabled FXSAVE/XMM
    // context switching, which is exactly what SSE2 code also needs.
    c.osXmm = c.sse && IsProcessorFeaturePresent(PF_XMMI_INSTRUCTIONS_AVAILABLE);
    return c;
}

bool CpuCheckBuildCompatible(const CpuCaps& caps, std::wstring& message)
{
    message.clear();
#if LP_REQUIRE_CMOV
    if (!caps.cmov) {
        message = L"This copy of Navi2K Player needs a Pentium Pro, Pentium II, Celeron, "
                  L"Athlon or newer processor (CMOV instructions).\r\n\r\n"
                  L"Pentium, Pentium MMX and AMD K6 need a build made with -DLEGACY_MARCH=i586 "
                  L"and a toolchain whose runtime was built for i586.";
        return false;
    }
#endif
#if defined(__SSE2__)
    if (!caps.Sse2Usable()) {
        message = L"This copy of Navi2K Player was built for processors with SSE2 "
                  L"(Pentium 4 / Athlon 64 or newer), and this processor does not support it.\r\n\r\n"
                  L"Please use the non-SSE2 build (configured with -DLEGACY_NO_SSE2=ON).";
        return false;
    }
#elif defined(__SSE__)
    if (!(caps.sse && caps.osXmm)) {
        message = L"This copy of Navi2K Player was built for processors with SSE "
                  L"(Pentium III or newer). Please use the non-SSE build.";
        return false;
    }
#else
    (void)caps;
#endif
    return true;
}

static LONG WINAPI IllegalInstructionFilter(EXCEPTION_POINTERS* ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_PRIV_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;

    wchar_t text[512];
    _snwprintf(text, 511,
        L"Navi2K Player hit an instruction this processor does not support "
        L"(at address 0x%08lX).\r\n\r\n"
        L"This usually means the program was compiled with SSE2 enabled. "
        L"Rebuild with -DLEGACY_NO_SSE2=ON, or use the non-SSE2 release.",
        (unsigned long)(ULONG_PTR)ep->ExceptionRecord->ExceptionAddress);
    text[511] = 0;
    MessageBoxW(NULL, text, L"Navi2K Player - unsupported processor", MB_ICONSTOP | MB_OK | MB_TASKMODAL);
    return EXCEPTION_EXECUTE_HANDLER;   // terminates the process quietly
}

void InstallIllegalInstructionHandler()
{
    SetUnhandledExceptionFilter(IllegalInstructionFilter);
}
