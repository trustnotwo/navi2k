// Stand-ins for kernel32 functions that the toolchain's static runtime imports but that
// don't exist on Windows 2000/XP. Each one is defined under the import-table symbol name
// (__imp__Name@N), so the linker resolves the runtime's call here and never adds the real
// import - which would stop the .exe from loading at all.
//
// cmake/check_binary.cmake flags any new XP+/Vista+ import; add a shim here when it does.

#include <windows.h>

// --- GetThreadId (Vista+) --------------------------------------------------------
// Used by libgcc's win32 thread layer (gthr-win32.o) since GCC 13.

namespace {

struct ClientId {
    HANDLE uniqueProcess;
    HANDLE uniqueThread;
};

struct ThreadBasicInformation {
    LONG exitStatus;
    PVOID tebBaseAddress;
    ClientId clientId;
    ULONG_PTR affinityMask;
    LONG priority;
    LONG basePriority;
};

typedef DWORD (WINAPI *GetThreadIdFn)(HANDLE);
typedef LONG (WINAPI *NtQueryInformationThreadFn)(HANDLE, int, PVOID, ULONG, PULONG);

DWORD WINAPI LpGetThreadId(HANDLE thread)
{
    if (thread == GetCurrentThread())
        return GetCurrentThreadId();

    // Looked up on every call: a function-local static would need libstdc++'s init guard,
    // which itself uses Vista-only condition variables.

    // Vista and later: use the real thing.
    GetThreadIdFn real = (GetThreadIdFn)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadId");
    if (real)
        return real(thread);

    // NT 3.1 - XP: ThreadBasicInformation (class 0) carries the thread id.
    NtQueryInformationThreadFn query =
        (NtQueryInformationThreadFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");
    ThreadBasicInformation tbi;
    if (!query || query(thread, 0, &tbi, sizeof(tbi), NULL) < 0) {
        SetLastError(ERROR_INVALID_HANDLE);
        return 0;
    }
    return (DWORD)(ULONG_PTR)tbi.clientId.uniqueThread;
}

} // namespace

extern "C" {
GetThreadIdFn lp_imp_GetThreadId __asm__("__imp__GetThreadId@4") = LpGetThreadId;
}
