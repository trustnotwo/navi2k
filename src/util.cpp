#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>

std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty())
        return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), NULL, 0);
    if (n <= 0)
        return std::wstring();
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n);
    return out;
}

std::string WideToUtf8(const std::wstring& s)
{
    if (s.empty())
        return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), NULL, 0, NULL, NULL);
    if (n <= 0)
        return std::string();
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n, NULL, NULL);
    return out;
}

std::string UrlEncode(const std::string& s)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::wstring Fmt(const wchar_t* fmt, ...)
{
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 1023, fmt, ap);
    va_end(ap);
    buf[1023] = 0;
    return buf;
}

std::wstring ExeDirectory()
{
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return L".";
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash)
        *slash = 0;
    return path;
}

bool EndsWithNoCase(const std::wstring& s, const wchar_t* suffix)
{
    size_t n = wcslen(suffix);
    return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, suffix) == 0;
}

static CRITICAL_SECTION g_logLock;
static HANDLE g_logFile = INVALID_HANDLE_VALUE;
static bool g_logReady = false;

void LogInit(const std::wstring& path)
{
    InitializeCriticalSection(&g_logLock);
    g_logFile = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    g_logReady = true;
}

void LogF(const wchar_t* fmt, ...)
{
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 1023, fmt, ap);
    va_end(ap);
    buf[1023] = 0;

    OutputDebugStringW(buf);
    OutputDebugStringW(L"\r\n");
    if (!g_logReady || g_logFile == INVALID_HANDLE_VALUE)
        return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    char stamp[32];
    _snprintf(stamp, sizeof(stamp) - 1, "%02u:%02u:%02u.%03u ",
              st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    stamp[sizeof(stamp) - 1] = 0;
    std::string line = stamp + WideToUtf8(buf) + "\r\n";

    EnterCriticalSection(&g_logLock);
    DWORD written;
    WriteFile(g_logFile, line.data(), (DWORD)line.size(), &written, NULL);
    LeaveCriticalSection(&g_logLock);
}
