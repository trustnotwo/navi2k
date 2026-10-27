# Post-build compatibility check for Windows 2000 / non-SSE2 CPUs.
#
#   cmake -DOBJDUMP=... -DEXE=... -DOUT_DIR=... [-DSCAN_XMM=ON] [-DALLOW_XMM_IN=regex] [-DSTRICT=ON]
#         -P check_binary.cmake
#
# 1. PE subsystem version must be <= 5 or the Win2000 loader rejects the image.
# 2. No imports that only exist on XP or later (best-effort list + every msvcrt *_s function).
# 3. With SCAN_XMM, no function may touch XMM registers except the runtime-dispatched ones.
#
# Full import list is written to OUT_DIR/imports.txt for manual review. Passing this check is
# necessary, not sufficient: the real test is launching the .exe on Windows 2000 SP4.

cmake_minimum_required(VERSION 3.13)   # IN_LIST needs policy CMP0057 in -P mode

foreach(v OBJDUMP EXE OUT_DIR)
    if(NOT ${v})
        message(FATAL_ERROR "check_binary: ${v} is not set")
    endif()
endforeach()
file(MAKE_DIRECTORY "${OUT_DIR}")
set(problems "")

# --- 1. PE header -----------------------------------------------------------
execute_process(COMMAND "${OBJDUMP}" -p "${EXE}"
    OUTPUT_FILE "${OUT_DIR}/pe_headers.txt" RESULT_VARIABLE rc)
if(rc)
    message(FATAL_ERROR "check_binary: objdump -p failed on ${EXE}")
endif()

file(STRINGS "${OUT_DIR}/pe_headers.txt" subsys_line REGEX "^MajorSubsystemVersion")
string(REGEX MATCH "[0-9]+" subsys_major "${subsys_line}")
if(subsys_major GREATER 5)
    list(APPEND problems "PE MajorSubsystemVersion is ${subsys_major}; Windows 2000 needs 5 or lower")
endif()

# --- 2. Imports ---------------------------------------------------------------
set(xp_or_later
    # kernel32
    GetTickCount64 InitializeSRWLock AcquireSRWLockExclusive AcquireSRWLockShared
    InitializeConditionVariable SleepConditionVariableCS SleepConditionVariableSRW
    WakeConditionVariable WakeAllConditionVariable
    InitOnceExecuteOnce InitializeCriticalSectionEx FlsAlloc FlsGetValue FlsSetValue
    GetNativeSystemInfo GetModuleHandleExA GetModuleHandleExW AddVectoredExceptionHandler
    RemoveVectoredExceptionHandler EncodePointer DecodePointer SetDllDirectoryW SetDllDirectoryA
    GetSystemTimes IsWow64Process AttachConsole GetLogicalProcessorInformation
    InitializeSListHead InterlockedPushEntrySList InterlockedPopEntrySList InterlockedFlushSList
    GetProcessId GetThreadId CancelIoEx GetCurrentProcessorNumber SetThreadStackGuarantee
    WTSGetActiveConsoleSessionId GetFinalPathNameByHandleW CreateSymbolicLinkW
    # msvcrt functions absent from the Win2000 msvcrt.dll
    _aligned_malloc _aligned_free _fseeki64 _ftelli64
    # ws2_32
    getaddrinfo freeaddrinfo getnameinfo GetAddrInfoW inet_pton inet_ntop WSAPoll)

# Import rows are "<tab><vma> ... <name>"; the middle columns vary between binutils
# versions (newer ones add an Ordinal column), so only the first and last fields are used.
file(STRINGS "${OUT_DIR}/pe_headers.txt" import_lines
    REGEX "(DLL Name: )|(^\t[0-9a-fA-F]+[ \t]+[^ \t])")
set(dll "")
set(import_report "")
set(import_count 0)
foreach(line IN LISTS import_lines)
    if(line MATCHES "DLL Name: (.+)$")
        string(TOLOWER "${CMAKE_MATCH_1}" dll)
        continue()
    endif()
    if(dll STREQUAL "")
        continue()
    endif()
    string(REGEX REPLACE "^.*[ \t]([^ \t]+)$" "\\1" name "${line}")
    string(APPEND import_report "${dll}!${name}\n")
    math(EXPR import_count "${import_count} + 1")
    if(name IN_LIST xp_or_later)
        list(APPEND problems "imports ${dll}!${name}, which does not exist on Windows 2000")
    elseif(dll STREQUAL "msvcrt.dll" AND name MATCHES "_s$")
        list(APPEND problems "imports msvcrt!${name}; the Win2000 msvcrt has no *_s functions")
    elseif(dll MATCHES "^(ucrtbase|api-ms-win-)")
        list(APPEND problems "imports ${dll}; use an msvcrt-based MinGW-w64 toolchain, not UCRT")
    endif()
endforeach()
file(WRITE "${OUT_DIR}/imports.txt" "${import_report}")
if(import_count EQUAL 0)
    list(APPEND problems "could not parse the import table (objdump format changed?); imports were not checked")
endif()

# --- 3. Instruction scan -------------------------------------------------------
# SCAN_XMM:  SSE/SSE2 (XMM registers) anywhere except ALLOW_XMM_IN.
# SCAN_CMOV: P6 instructions (cmov/fcmov/fcomi/fucomi), for -march=i586 builds.
#            Prebuilt libgcc/libstdc++ from an i686 toolchain will trip this.
if(SCAN_XMM OR SCAN_CMOV)
    execute_process(COMMAND "${OBJDUMP}" -d --no-show-raw-insn "${EXE}"
        OUTPUT_FILE "${OUT_DIR}/disasm.txt" RESULT_VARIABLE rc)
    if(rc)
        message(FATAL_ERROR "check_binary: objdump -d failed on ${EXE}")
    endif()
    file(STRINGS "${OUT_DIR}/disasm.txt" asm_lines
        REGEX "(^[0-9a-f]+ <[^>]+>:$)|(%xmm)|(\t(f?cmov|fu?comip?)[a-z]* )")
    set(func "?")
    set(xmm_funcs "")
    set(cmov_funcs "")
    foreach(line IN LISTS asm_lines)
        if(line MATCHES "^[0-9a-f]+ <([^>]+)>:$")
            set(func "${CMAKE_MATCH_1}")
        elseif(line MATCHES "%xmm")
            if(SCAN_XMM AND NOT (ALLOW_XMM_IN AND func MATCHES "${ALLOW_XMM_IN}"))
                list(APPEND xmm_funcs "${func}")
            endif()
        elseif(SCAN_CMOV)
            list(APPEND cmov_funcs "${func}")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES xmm_funcs)
    list(REMOVE_DUPLICATES cmov_funcs)
    foreach(f IN LISTS xmm_funcs)
        list(APPEND problems "function ${f} uses XMM registers outside the runtime-dispatched path")
    endforeach()
    foreach(f IN LISTS cmov_funcs)
        list(APPEND problems "function ${f} uses P6-only instructions (cmov/fcomi), which crash on Pentium/K6")
    endforeach()
endif()

# --- Report -----------------------------------------------------------------
if(problems)
    list(LENGTH problems n)
    set(msg "Windows 2000 / non-SSE2 compatibility check found ${n} problem(s):")
    foreach(p IN LISTS problems)
        string(APPEND msg "\n  - ${p}")
    endforeach()
    string(APPEND msg "\nDetails: ${OUT_DIR}")
    if(STRICT)
        message(FATAL_ERROR "${msg}")
    else()
        message(WARNING "${msg}")
    endif()
else()
    message(STATUS "Win2000/SSE2 check passed (imports listed in ${OUT_DIR}/imports.txt)")
endif()
