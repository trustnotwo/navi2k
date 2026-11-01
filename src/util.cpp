#include "util.h"
#include <objbase.h>
#include <shlobj.h>

#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>

// UTF-8 by hand: Windows 95 has no CP_UTF8. Invalid bytes become U+FFFD; characters
// beyond the BMP become surrogate pairs, as Windows does.
std::wstring Utf8ToWide(const std::string& s)
{
    std::wstring out;
    out.reserve(s.size());
    const unsigned char* p = (const unsigned char*)s.data();
    const size_t n = s.size();
    for (size_t i = 0; i < n;) {
        unsigned c = p[i];
        int extra = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
        if (extra < 0 || i + (size_t)extra >= n) {
            out += (wchar_t)0xFFFD;
            ++i;
            continue;
        }
        unsigned cp = extra == 0 ? c : extra == 1 ? (c & 0x1F) : extra == 2 ? (c & 0x0F) : (c & 0x07);
        bool ok = true;
        for (int k = 1; k <= extra; ++k) {
            if ((p[i + k] & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (p[i + k] & 0x3F);
        }
        static const unsigned kMin[4] = { 0, 0x80, 0x800, 0x10000 };
        if (!ok || cp < kMin[extra] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            out += (wchar_t)0xFFFD;
            ++i;
            continue;
        }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out += (wchar_t)(0xD800 + (cp >> 10));
            out += (wchar_t)(0xDC00 + (cp & 0x3FF));
        } else {
            out += (wchar_t)cp;
        }
        i += extra + 1;
    }
    return out;
}

std::string WideToUtf8(const std::wstring& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned cp = s[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            ++i;
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = 0xFFFD;   // unpaired surrogate
        }
        if (cp < 0x80) {
            out += (char)cp;
        } else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }
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

bool BrowseForFolder(HWND owner, const wchar_t* title, std::wstring& path)
{
    CoInitialize(NULL);
    BROWSEINFOW bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.hwndOwner = owner;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS;
    bool ok = false;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t buf[MAX_PATH];
        if (SHGetPathFromIDListW(pidl, buf)) {
            path = buf;
            ok = true;
        }
        CoTaskMemFree(pidl);
    }
    CoUninitialize();
    return ok;
}
