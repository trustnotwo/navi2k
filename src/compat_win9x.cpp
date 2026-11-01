// Windows 95/98/ME support: stand-ins for the Unicode ("W") API functions the player uses.
//
// Windows 9x exports most W functions only as stubs that fail, and some not at all (which
// stops the .exe from loading). Each stand-in below is defined under the import-table symbol
// name (__imp__Name@N), like compat_win2000.cpp, so every call to the W function - from our
// code and from the static runtime - lands here and the real W import is never added.
//
// On Windows NT (2000, XP and later) each stand-in looks up and calls the real W function.
// On 9x it converts the strings to the ANSI code page and calls the A function. Windows
// created through these are ANSI windows on 9x, so their window procedures get ANSI messages;
// the player only reads text through the stand-ins, so that doesn't matter. Common controls
// are switched to Unicode notifications so LVN_GETDISPINFOW, TVN_SELCHANGEDW etc. still come.
//
// Native on 9x and left alone: MessageBoxW, TextOutW, ExtTextOutW, GetTextExtentPoint32W,
// MultiByteToWideChar, WideCharToMultiByte.
//
// cmake/check_binary.cmake (WIN9X=ON) fails the build if any other W import appears; add a
// stand-in here when it does.

#include "compat_win9x.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <mmsystem.h>
#include <string.h>
#include <string>
#include <vector>

namespace {

int g_mode = -1;   // plain data: no static-init guard

FARPROC Proc(const char* dll, const char* name)
{
    HMODULE m = GetModuleHandleA(dll);
    if (!m)
        m = LoadLibraryA(dll);
    return m ? GetProcAddress(m, name) : NULL;
}

// On NT: call the real W function (looked up once). Falls through to the ANSI code if the
// lookup fails, which only happens on 9x.
#define NT_FIRST(dll, name, args)                                                     \
    if (!Win9xMode()) {                                                               \
        static __typeof__(&name) real_;                                               \
        if (!real_)                                                                   \
            real_ = (__typeof__(&name))Proc(dll, #name);                              \
        if (real_)                                                                    \
            return real_ args;                                                        \
    }

// ANSI copy of a wide string. NULL and MAKEINTRESOURCE values pass through unchanged.
class A {
public:
    explicit A(const wchar_t* w) : p(NULL)
    {
        if (!w)
            return;
        if (IS_INTRESOURCE(w)) {
            p = (const char*)w;
            return;
        }
        int n = WideCharToMultiByte(CP_ACP, 0, w, -1, NULL, 0, NULL, NULL);
        if (n <= 0)
            n = 1;
        s.assign(n, '\0');
        WideCharToMultiByte(CP_ACP, 0, w, -1, &s[0], n, NULL, NULL);
        p = s.c_str();
    }
    A(const wchar_t* w, int len) : p(NULL)
    {
        if (len < 0) {
            A whole(w);
            s = whole.p ? whole.p : "";
        } else if (len > 0) {
            int n = WideCharToMultiByte(CP_ACP, 0, w, len, NULL, 0, NULL, NULL);
            if (n > 0) {
                s.assign(n, '\0');
                WideCharToMultiByte(CP_ACP, 0, w, len, &s[0], n, NULL, NULL);
            }
        }
        p = s.c_str();
    }
    const char* p;
private:
    std::string s;
    A(const A&);
    A& operator=(const A&);
};

std::wstring Wide(const char* a, int len = -1)
{
    if (!a)
        return std::wstring();
    if (len < 0)
        len = (int)strlen(a);
    if (len == 0)
        return std::wstring();
    int n = MultiByteToWideChar(CP_ACP, 0, a, len, NULL, 0);
    std::wstring w(n > 0 ? n : 0, L'\0');
    if (n > 0)
        MultiByteToWideChar(CP_ACP, 0, a, len, &w[0], n);
    return w;
}

// Copies into a caller's buffer of 'chars' characters; returns the length copied.
int CopyOut(const std::wstring& w, wchar_t* out, int chars)
{
    if (!out || chars <= 0)
        return 0;
    int n = (int)w.size() < chars - 1 ? (int)w.size() : chars - 1;
    if (n > 0)
        memcpy(out, w.data(), n * sizeof(wchar_t));
    out[n] = 0;
    return n;
}

// Copies into a fixed char array, cutting on a character boundary.
void CopyAnsi(const char* src, char* dst, size_t size)
{
    size_t i = 0;
    while (src[i] && i + 1 < size) {
        if (IsDBCSLeadByte((BYTE)src[i])) {
            if (i + 2 >= size || !src[i + 1])
                break;
            dst[i] = src[i];
            ++i;
        }
        dst[i] = src[i];
        ++i;
    }
    dst[i] = 0;
}

void LogFontToA(const LOGFONTW& w, LOGFONTA& a)
{
    memcpy(&a, &w, offsetof(LOGFONTW, lfFaceName));
    A face(w.lfFaceName);
    CopyAnsi(face.p, a.lfFaceName, LF_FACESIZE);
}

void LogFontToW(const LOGFONTA& a, LOGFONTW& w)
{
    memcpy(&w, &a, offsetof(LOGFONTA, lfFaceName));
    CopyOut(Wide(a.lfFaceName), w.lfFaceName, LF_FACESIZE);
}

// --- kernel32 -------------------------------------------------------------------------

HANDLE WINAPI LpCreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp,
                            DWORD flags, HANDLE tmpl)
{
    NT_FIRST("kernel32.dll", CreateFileW, (name, access, share, sa, disp, flags, tmpl));
    A a(name);
    // 9x rejects FILE_SHARE_DELETE (and files there can't be deleted while open anyway).
    return CreateFileA(a.p, access, share & ~FILE_SHARE_DELETE, sa, disp, flags, tmpl);
}

BOOL WINAPI LpCreateDirectoryW(LPCWSTR path, LPSECURITY_ATTRIBUTES sa)
{
    NT_FIRST("kernel32.dll", CreateDirectoryW, (path, sa));
    A a(path);
    return CreateDirectoryA(a.p, sa);
}

BOOL WINAPI LpRemoveDirectoryW(LPCWSTR path)
{
    NT_FIRST("kernel32.dll", RemoveDirectoryW, (path));
    A a(path);
    return RemoveDirectoryA(a.p);
}

BOOL WINAPI LpDeleteFileW(LPCWSTR path)
{
    NT_FIRST("kernel32.dll", DeleteFileW, (path));
    A a(path);
    return DeleteFileA(a.p);
}

DWORD WINAPI LpGetFileAttributesW(LPCWSTR path)
{
    NT_FIRST("kernel32.dll", GetFileAttributesW, (path));
    A a(path);
    return GetFileAttributesA(a.p);
}

BOOL WINAPI LpMoveFileW(LPCWSTR from, LPCWSTR to)
{
    NT_FIRST("kernel32.dll", MoveFileW, (from, to));
    A f(from), t(to);
    return MoveFileA(f.p, t.p);
}

BOOL WINAPI LpMoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags)
{
    NT_FIRST("kernel32.dll", MoveFileExW, (from, to, flags));
    // MoveFileEx doesn't exist on 9x: replace by deleting the target first.
    A f(from), t(to);
    if ((flags & MOVEFILE_REPLACE_EXISTING) && t.p && GetFileAttributesA(t.p) != INVALID_FILE_ATTRIBUTES)
        DeleteFileA(t.p);
    return MoveFileA(f.p, t.p);
}

void FindDataToW(const WIN32_FIND_DATAA& a, WIN32_FIND_DATAW& w)
{
    w.dwFileAttributes = a.dwFileAttributes;
    w.ftCreationTime = a.ftCreationTime;
    w.ftLastAccessTime = a.ftLastAccessTime;
    w.ftLastWriteTime = a.ftLastWriteTime;
    w.nFileSizeHigh = a.nFileSizeHigh;
    w.nFileSizeLow = a.nFileSizeLow;
    w.dwReserved0 = a.dwReserved0;
    w.dwReserved1 = a.dwReserved1;
    CopyOut(Wide(a.cFileName), w.cFileName, MAX_PATH);
    CopyOut(Wide(a.cAlternateFileName), w.cAlternateFileName, 14);
}

HANDLE WINAPI LpFindFirstFileW(LPCWSTR pattern, LPWIN32_FIND_DATAW fd)
{
    NT_FIRST("kernel32.dll", FindFirstFileW, (pattern, fd));
    A a(pattern);
    WIN32_FIND_DATAA fa;
    HANDLE h = FindFirstFileA(a.p, &fa);
    if (h != INVALID_HANDLE_VALUE)
        FindDataToW(fa, *fd);
    return h;
}

BOOL WINAPI LpFindNextFileW(HANDLE h, LPWIN32_FIND_DATAW fd)
{
    NT_FIRST("kernel32.dll", FindNextFileW, (h, fd));
    WIN32_FIND_DATAA fa;
    if (!FindNextFileA(h, &fa))
        return FALSE;
    FindDataToW(fa, *fd);
    return TRUE;
}

DWORD WINAPI LpGetModuleFileNameW(HMODULE m, LPWSTR out, DWORD chars)
{
    NT_FIRST("kernel32.dll", GetModuleFileNameW, (m, out, chars));
    char buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameA(m, buf, sizeof(buf));
    if (!n)
        return 0;
    return (DWORD)CopyOut(Wide(buf, (int)n), out, (int)chars);
}

HMODULE WINAPI LpGetModuleHandleW(LPCWSTR name)
{
    NT_FIRST("kernel32.dll", GetModuleHandleW, (name));
    A a(name);
    return GetModuleHandleA(a.p);
}

HMODULE WINAPI LpLoadLibraryW(LPCWSTR name)
{
    NT_FIRST("kernel32.dll", LoadLibraryW, (name));
    A a(name);
    return LoadLibraryA(a.p);
}

HANDLE WINAPI LpCreateEventW(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL initial, LPCWSTR name)
{
    NT_FIRST("kernel32.dll", CreateEventW, (sa, manual, initial, name));
    A a(name);
    return CreateEventA(sa, manual, initial, a.p);
}

void WINAPI LpOutputDebugStringW(LPCWSTR s)
{
    NT_FIRST("kernel32.dll", OutputDebugStringW, (s));
    A a(s);
    OutputDebugStringA(a.p ? a.p : "");
}

UINT WINAPI LpGetPrivateProfileIntW(LPCWSTR app, LPCWSTR key, INT def, LPCWSTR file)
{
    NT_FIRST("kernel32.dll", GetPrivateProfileIntW, (app, key, def, file));
    A a(app), k(key), f(file);
    return GetPrivateProfileIntA(a.p, k.p, def, f.p);
}

DWORD WINAPI LpGetPrivateProfileStringW(LPCWSTR app, LPCWSTR key, LPCWSTR def, LPWSTR out, DWORD chars,
                                        LPCWSTR file)
{
    NT_FIRST("kernel32.dll", GetPrivateProfileStringW, (app, key, def, out, chars, file));
    if (!out || chars == 0)
        return 0;
    A a(app), k(key), d(def), f(file);
    std::vector<char> buf(chars * 2 + 2);
    DWORD n = GetPrivateProfileStringA(a.p, k.p, d.p, &buf[0], chars * 2, f.p);
    std::wstring w = Wide(&buf[0], (int)n);
    // A NULL section or key returns a list of names, each ending in a NUL, with one more at
    // the end; keep room for that.
    const bool list = !app || !key;
    int room = (int)chars - (list ? 1 : 0);
    int len = CopyOut(w, out, room);
    if (list && len + 1 < (int)chars)
        out[len + 1] = 0;
    return (DWORD)len;
}

BOOL WINAPI LpWritePrivateProfileStringW(LPCWSTR app, LPCWSTR key, LPCWSTR value, LPCWSTR file)
{
    NT_FIRST("kernel32.dll", WritePrivateProfileStringW, (app, key, value, file));
    A a(app), k(key), v(value), f(file);
    return WritePrivateProfileStringA(a.p, k.p, v.p, f.p);
}

int WINAPI LplstrcmpiW(LPCWSTR x, LPCWSTR y)
{
    NT_FIRST("kernel32.dll", lstrcmpiW, (x, y));
    A a(x), b(y);
    return lstrcmpiA(a.p ? a.p : "", b.p ? b.p : "");
}

LPWSTR WINAPI LplstrcpyW(LPWSTR dst, LPCWSTR src)
{
    LPWSTR d = dst;
    while ((*d++ = *src++) != 0) {
    }
    return dst;
}

LPWSTR WINAPI LplstrcpynW(LPWSTR dst, LPCWSTR src, int n)
{
    if (n <= 0)
        return dst;
    int i = 0;
    for (; i < n - 1 && src[i]; ++i)
        dst[i] = src[i];
    dst[i] = 0;
    return dst;
}

// navi2k_probe: console output in the OEM code page, as 9x consoles show it.
BOOL WINAPI LpWriteConsoleW(HANDLE h, const VOID* text, DWORD chars, LPDWORD written, LPVOID reserved)
{
    NT_FIRST("kernel32.dll", WriteConsoleW, (h, text, chars, written, reserved));
    const wchar_t* w = (const wchar_t*)text;
    int n = WideCharToMultiByte(CP_OEMCP, 0, w, (int)chars, NULL, 0, NULL, NULL);
    std::string a(n > 0 ? n : 0, '\0');
    if (n > 0)
        WideCharToMultiByte(CP_OEMCP, 0, w, (int)chars, &a[0], n, NULL, NULL);
    DWORD put = 0;
    BOOL ok = WriteConsoleA(h, a.data(), (DWORD)a.size(), &put, reserved);
    if (written)
        *written = ok ? chars : 0;
    return ok;
}

// Not W functions, but NT-only: imported by the static runtime (libgcc's thread layer) and
// cpu.cpp. Without them the .exe wouldn't load on 9x.
BOOL WINAPI LpTryEnterCriticalSection(LPCRITICAL_SECTION cs)
{
    typedef BOOL (WINAPI *Fn)(LPCRITICAL_SECTION);
    static Fn real;
    if (!real)
        real = (Fn)Proc("kernel32.dll", "TryEnterCriticalSection");
    if (real)
        return real(cs);
    EnterCriticalSection(cs);   // 9x: wait instead of failing, so callers still make progress
    return TRUE;
}

BOOL WINAPI LpIsProcessorFeaturePresent(DWORD feature)
{
    typedef BOOL (WINAPI *Fn)(DWORD);
    static Fn real;
    if (!real)
        real = (Fn)Proc("kernel32.dll", "IsProcessorFeaturePresent");
    return real ? real(feature) : FALSE;   // 9x: assume no SSE support from the OS
}

// msvcrt's _beginthreadex passes its last argument to CreateThread, and on Windows 9x
// CreateThread fails when that thread-id pointer is NULL (NT accepts it). The player never
// needs the id, so give it somewhere to go.
typedef uintptr_t (__cdecl *BeginThreadExFn)(void*, unsigned, unsigned (__stdcall*)(void*), void*, unsigned,
                                             unsigned*);

uintptr_t __cdecl LpBeginThreadEx(void* security, unsigned stack, unsigned (__stdcall* start)(void*), void* arg,
                                  unsigned flags, unsigned* id)
{
    static BeginThreadExFn real;
    if (!real)
        real = (BeginThreadExFn)Proc("msvcrt.dll", "_beginthreadex");
    if (!real)
        return 0;
    unsigned unused;
    return real(security, stack, start, arg, flags, id ? id : &unused);
}

// --- gdi32 ----------------------------------------------------------------------------

HFONT WINAPI LpCreateFontIndirectW(const LOGFONTW* lf)
{
    NT_FIRST("gdi32.dll", CreateFontIndirectW, (lf));
    LOGFONTA a;
    LogFontToA(*lf, a);
    return CreateFontIndirectA(&a);
}

int WINAPI LpGetObjectW(HANDLE h, int size, LPVOID out)
{
    NT_FIRST("gdi32.dll", GetObjectW, (h, size, out));
    if (GetObjectType(h) != OBJ_FONT)
        return GetObjectA(h, size, out);   // bitmaps, brushes, pens: same layout
    LOGFONTA a;
    if (!GetObjectA(h, sizeof(a), &a))
        return 0;
    if (!out)
        return sizeof(LOGFONTW);
    if (size < (int)sizeof(LOGFONTW))
        return 0;
    LogFontToW(a, *(LOGFONTW*)out);
    return sizeof(LOGFONTW);
}

BOOL WINAPI LpGetTextMetricsW(HDC dc, LPTEXTMETRICW tm)
{
    NT_FIRST("gdi32.dll", GetTextMetricsW, (dc, tm));
    TEXTMETRICA a;
    if (!GetTextMetricsA(dc, &a))
        return FALSE;
    tm->tmHeight = a.tmHeight;
    tm->tmAscent = a.tmAscent;
    tm->tmDescent = a.tmDescent;
    tm->tmInternalLeading = a.tmInternalLeading;
    tm->tmExternalLeading = a.tmExternalLeading;
    tm->tmAveCharWidth = a.tmAveCharWidth;
    tm->tmMaxCharWidth = a.tmMaxCharWidth;
    tm->tmWeight = a.tmWeight;
    tm->tmOverhang = a.tmOverhang;
    tm->tmDigitizedAspectX = a.tmDigitizedAspectX;
    tm->tmDigitizedAspectY = a.tmDigitizedAspectY;
    tm->tmFirstChar = (BYTE)a.tmFirstChar;
    tm->tmLastChar = (BYTE)a.tmLastChar;
    tm->tmDefaultChar = (BYTE)a.tmDefaultChar;
    tm->tmBreakChar = (BYTE)a.tmBreakChar;
    tm->tmItalic = a.tmItalic;
    tm->tmUnderlined = a.tmUnderlined;
    tm->tmStruckOut = a.tmStruckOut;
    tm->tmPitchAndFamily = a.tmPitchAndFamily;
    tm->tmCharSet = a.tmCharSet;
    return TRUE;
}

// --- user32: windows and messages --------------------------------------------------------

bool IsCommonControl(HWND h)
{
    char cls[64];
    if (!GetClassNameA(h, cls, sizeof(cls)))
        return false;
    return strncmp(cls, "Sys", 3) == 0 || strncmp(cls, "msctls_", 7) == 0 || strcmp(cls, "ToolbarWindow32") == 0 ||
           strcmp(cls, "ReBarWindow32") == 0 || strcmp(cls, "tooltips_class32") == 0 ||
           strcmp(cls, "ComboBoxEx32") == 0;
}

HWND WINAPI LpCreateWindowExW(DWORD exStyle, LPCWSTR cls, LPCWSTR name, DWORD style, int x, int y, int w,
                              int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
{
    NT_FIRST("user32.dll", CreateWindowExW, (exStyle, cls, name, style, x, y, w, h, parent, menu, inst, param));
    A c(cls), n(name);
    HWND wnd = CreateWindowExA(exStyle, c.p, n.p, style, x, y, w, h, parent, menu, inst, param);
    // An ANSI list or tree view would send LVN_GETDISPINFOA, TVN_SELCHANGEDA, ...; the
    // player handles the W forms.
    if (wnd && IsCommonControl(wnd))
        SendMessageA(wnd, CCM_SETUNICODEFORMAT, TRUE, 0);
    return wnd;
}

LRESULT WINAPI LpDefWindowProcW(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    NT_FIRST("user32.dll", DefWindowProcW, (h, msg, wp, lp));
    if (msg == WM_NOTIFYFORMAT && lp == NF_QUERY)
        return NFR_UNICODE;   // controls ask their parent which notifications it wants
    return DefWindowProcA(h, msg, wp, lp);
}

LRESULT WINAPI LpCallWindowProcW(WNDPROC proc, HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    NT_FIRST("user32.dll", CallWindowProcW, (proc, h, msg, wp, lp));
    return CallWindowProcA(proc, h, msg, wp, lp);
}

ATOM WINAPI LpRegisterClassExW(const WNDCLASSEXW* wc)
{
    NT_FIRST("user32.dll", RegisterClassExW, (wc));
    A menu(wc->lpszMenuName), name(wc->lpszClassName);
    WNDCLASSEXA a;
    ZeroMemory(&a, sizeof(a));
    a.cbSize = sizeof(a);
    a.style = wc->style;
    a.lpfnWndProc = wc->lpfnWndProc;
    a.cbClsExtra = wc->cbClsExtra;
    a.cbWndExtra = wc->cbWndExtra;
    a.hInstance = wc->hInstance;
    a.hIcon = wc->hIcon;
    a.hCursor = wc->hCursor;
    a.hbrBackground = wc->hbrBackground;
    a.lpszMenuName = menu.p;
    a.lpszClassName = name.p;
    a.hIconSm = wc->hIconSm;
    return RegisterClassExA(&a);
}

ATOM WINAPI LpRegisterClassW(const WNDCLASSW* wc)
{
    NT_FIRST("user32.dll", RegisterClassW, (wc));
    A menu(wc->lpszMenuName), name(wc->lpszClassName);
    WNDCLASSA a;
    ZeroMemory(&a, sizeof(a));
    a.style = wc->style;
    a.lpfnWndProc = wc->lpfnWndProc;
    a.cbClsExtra = wc->cbClsExtra;
    a.cbWndExtra = wc->cbWndExtra;
    a.hInstance = wc->hInstance;
    a.hIcon = wc->hIcon;
    a.hCursor = wc->hCursor;
    a.hbrBackground = wc->hbrBackground;
    a.lpszMenuName = menu.p;
    a.lpszClassName = name.p;
    return RegisterClassA(&a);
}

UINT WINAPI LpRegisterWindowMessageW(LPCWSTR name)
{
    NT_FIRST("user32.dll", RegisterWindowMessageW, (name));
    A a(name);
    return RegisterWindowMessageA(a.p);
}

BOOL WINAPI LpGetMessageW(LPMSG m, HWND h, UINT lo, UINT hi)
{
    NT_FIRST("user32.dll", GetMessageW, (m, h, lo, hi));
    return GetMessageA(m, h, lo, hi);
}

LRESULT WINAPI LpDispatchMessageW(const MSG* m)
{
    NT_FIRST("user32.dll", DispatchMessageW, (m));
    return DispatchMessageA(m);
}

int WINAPI LpTranslateAcceleratorW(HWND h, HACCEL acc, LPMSG m)
{
    NT_FIRST("user32.dll", TranslateAcceleratorW, (h, acc, m));
    return TranslateAcceleratorA(h, acc, m);
}

HACCEL WINAPI LpCreateAcceleratorTableW(LPACCEL acc, int n)
{
    NT_FIRST("user32.dll", CreateAcceleratorTableW, (acc, n));
    return CreateAcceleratorTableA(acc, n);
}

BOOL WINAPI LpPostMessageW(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    NT_FIRST("user32.dll", PostMessageW, (h, msg, wp, lp));
    return PostMessageA(h, msg, wp, lp);
}

// The messages that carry text for standard windows and controls; everything else (the
// list/tree/status W messages included) passes through as is.
LRESULT WINAPI LpSendMessageW(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    NT_FIRST("user32.dll", SendMessageW, (h, msg, wp, lp));
    switch (msg) {
    case WM_SETTEXT:
    case EM_REPLACESEL:
    case CB_ADDSTRING:
    case CB_INSERTSTRING:
    case CB_FINDSTRING:
    case CB_FINDSTRINGEXACT:
    case CB_SELECTSTRING:
    case LB_ADDSTRING:
    case LB_INSERTSTRING:
    case LB_FINDSTRING:
    case LB_FINDSTRINGEXACT:
    case LB_SELECTSTRING: {
        A a((LPCWSTR)lp);
        return SendMessageA(h, msg, wp, (LPARAM)a.p);
    }
    case WM_GETTEXT: {
        if (wp == 0 || !lp)
            return 0;
        std::vector<char> buf(wp * 2 + 2);
        LRESULT n = SendMessageA(h, WM_GETTEXT, wp * 2, (LPARAM)&buf[0]);
        return CopyOut(Wide(&buf[0], (int)n), (LPWSTR)lp, (int)wp);
    }
    case CB_GETLBTEXT:
    case LB_GETTEXT: {
        LRESULT len = SendMessageA(h, msg == CB_GETLBTEXT ? CB_GETLBTEXTLEN : LB_GETTEXTLEN, wp, 0);
        if (len < 0)
            return len;
        std::vector<char> buf(len + 2);
        LRESULT n = SendMessageA(h, msg, wp, (LPARAM)&buf[0]);
        if (n < 0)
            return n;
        std::wstring w = Wide(&buf[0], (int)n);
        memcpy((LPWSTR)lp, w.c_str(), (w.size() + 1) * sizeof(wchar_t));
        return (LRESULT)w.size();
    }
    }
    return SendMessageA(h, msg, wp, lp);
}

LRESULT WINAPI LpSendDlgItemMessageW(HWND dlg, int id, UINT msg, WPARAM wp, LPARAM lp)
{
    NT_FIRST("user32.dll", SendDlgItemMessageW, (dlg, id, msg, wp, lp));
    HWND h = GetDlgItem(dlg, id);
    return h ? LpSendMessageW(h, msg, wp, lp) : 0;
}

BOOL WINAPI LpSetWindowTextW(HWND h, LPCWSTR text)
{
    NT_FIRST("user32.dll", SetWindowTextW, (h, text));
    A a(text);
    return SetWindowTextA(h, a.p);
}

int WINAPI LpGetWindowTextW(HWND h, LPWSTR out, int chars)
{
    NT_FIRST("user32.dll", GetWindowTextW, (h, out, chars));
    return (int)LpSendMessageW(h, WM_GETTEXT, (WPARAM)chars, (LPARAM)out);
}

int WINAPI LpGetWindowTextLengthW(HWND h)
{
    NT_FIRST("user32.dll", GetWindowTextLengthW, (h));
    return GetWindowTextLengthA(h);   // bytes: never less than the characters
}

BOOL WINAPI LpSetDlgItemTextW(HWND dlg, int id, LPCWSTR text)
{
    NT_FIRST("user32.dll", SetDlgItemTextW, (dlg, id, text));
    A a(text);
    return SetDlgItemTextA(dlg, id, a.p);
}

UINT WINAPI LpGetDlgItemTextW(HWND dlg, int id, LPWSTR out, int chars)
{
    NT_FIRST("user32.dll", GetDlgItemTextW, (dlg, id, out, chars));
    HWND h = GetDlgItem(dlg, id);
    if (!h) {
        CopyOut(std::wstring(), out, chars);
        return 0;
    }
    return (UINT)LpSendMessageW(h, WM_GETTEXT, (WPARAM)chars, (LPARAM)out);
}

LONG WINAPI LpGetWindowLongW(HWND h, int index)
{
    NT_FIRST("user32.dll", GetWindowLongW, (h, index));
    return GetWindowLongA(h, index);
}

LONG WINAPI LpSetWindowLongW(HWND h, int index, LONG value)
{
    NT_FIRST("user32.dll", SetWindowLongW, (h, index, value));
    return SetWindowLongA(h, index, value);
}

DWORD WINAPI LpGetClassLongW(HWND h, int index)
{
    NT_FIRST("user32.dll", GetClassLongW, (h, index));
    return GetClassLongA(h, index);
}

INT_PTR WINAPI LpDialogBoxParamW(HINSTANCE inst, LPCWSTR tmpl, HWND parent, DLGPROC proc, LPARAM lp)
{
    NT_FIRST("user32.dll", DialogBoxParamW, (inst, tmpl, parent, proc, lp));
    A t(tmpl);
    return DialogBoxParamA(inst, t.p, parent, proc, lp);
}

BOOL WINAPI LpSystemParametersInfoW(UINT action, UINT param, PVOID data, UINT winIni)
{
    NT_FIRST("user32.dll", SystemParametersInfoW, (action, param, data, winIni));
    return SystemParametersInfoA(action, param, data, winIni);   // only SPI_GETWORKAREA is used
}

// --- user32: menus, resources, drawing -----------------------------------------------------

BOOL WINAPI LpAppendMenuW(HMENU m, UINT flags, UINT_PTR id, LPCWSTR text)
{
    NT_FIRST("user32.dll", AppendMenuW, (m, flags, id, text));
    if (flags & (MF_BITMAP | MF_OWNERDRAW | MF_SEPARATOR))
        return AppendMenuA(m, flags, id, (LPCSTR)text);
    A a(text);
    return AppendMenuA(m, flags, id, a.p);
}

BOOL WINAPI LpInsertMenuW(HMENU m, UINT pos, UINT flags, UINT_PTR id, LPCWSTR text)
{
    NT_FIRST("user32.dll", InsertMenuW, (m, pos, flags, id, text));
    if (flags & (MF_BITMAP | MF_OWNERDRAW | MF_SEPARATOR))
        return InsertMenuA(m, pos, flags, id, (LPCSTR)text);
    A a(text);
    return InsertMenuA(m, pos, flags, id, a.p);
}

HCURSOR WINAPI LpLoadCursorW(HINSTANCE inst, LPCWSTR name)
{
    NT_FIRST("user32.dll", LoadCursorW, (inst, name));
    A a(name);
    return LoadCursorA(inst, a.p);
}

HANDLE WINAPI LpLoadImageW(HINSTANCE inst, LPCWSTR name, UINT type, int cx, int cy, UINT flags)
{
    NT_FIRST("user32.dll", LoadImageW, (inst, name, type, cx, cy, flags));
    A a(name);
    return LoadImageA(inst, a.p, type, cx, cy, flags);
}

int WINAPI LpDrawTextW(HDC dc, LPCWSTR text, int len, LPRECT rc, UINT fmt)
{
    // Text Windows can't draw here (9x: outside the code page; 2000+ without East Asian fonts:
    // Japanese, Chinese, Korean) is drawn with the fallback fonts.
    if (NeedsUnicodeDrawing(text, len))
        return DrawTextWithFallback(dc, text, len, rc, fmt);
    NT_FIRST("user32.dll", DrawTextW, (dc, text, len, rc, fmt));
    A a(text, len);
    return DrawTextA(dc, a.p, -1, rc, fmt & ~DT_MODIFYSTRING);
}

DWORD WINAPI LpCharLowerBuffW(LPWSTR s, DWORD len)
{
    NT_FIRST("user32.dll", CharLowerBuffW, (s, len));
    for (DWORD i = 0; i < len; ++i) {
        wchar_t c = s[i];
        if ((c >= L'A' && c <= L'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7))
            s[i] = (wchar_t)(c + 0x20);   // ASCII and Latin-1 letters
    }
    return len;
}

// --- comctl32 ---------------------------------------------------------------------------

INT_PTR WINAPI LpPropertySheetW(LPCPROPSHEETHEADERW h)
{
    NT_FIRST("comctl32.dll", PropertySheetW, (h));
    if (!(h->dwFlags & PSH_PROPSHEETPAGE))
        return -1;   // the player always passes page structures
    // Version-1 structures: the only ones the comctl32 of Windows 95 knows.
    std::vector<std::string> titles(h->nPages);
    std::vector<char> pages(h->nPages * PROPSHEETPAGEA_V1_SIZE);
    const BYTE* src = (const BYTE*)h->ppsp;
    for (UINT i = 0; i < h->nPages; ++i) {
        const PROPSHEETPAGEW* w = (const PROPSHEETPAGEW*)src;
        PROPSHEETPAGEA a;
        ZeroMemory(&a, sizeof(a));
        a.dwSize = PROPSHEETPAGEA_V1_SIZE;
        a.dwFlags = w->dwFlags & ~(PSP_USEHEADERTITLE | PSP_USEHEADERSUBTITLE | PSP_HIDEHEADER);
        a.hInstance = w->hInstance;
        a.pszTemplate = (LPCSTR)w->pszTemplate;   // MAKEINTRESOURCE
        a.hIcon = w->hIcon;
        if ((w->dwFlags & PSP_USETITLE) && w->pszTitle && !IS_INTRESOURCE(w->pszTitle)) {
            A t(w->pszTitle);
            titles[i] = t.p;
            a.pszTitle = titles[i].c_str();
        } else {
            a.pszTitle = (LPCSTR)w->pszTitle;
        }
        a.pfnDlgProc = w->pfnDlgProc;
        a.lParam = w->lParam;
        memcpy(&pages[i * PROPSHEETPAGEA_V1_SIZE], &a, PROPSHEETPAGEA_V1_SIZE);
        src += w->dwSize;
    }
    A caption(h->pszCaption);
    PROPSHEETHEADERA a;
    ZeroMemory(&a, sizeof(a));
    a.dwSize = PROPSHEETHEADERA_V1_SIZE;
    a.dwFlags = h->dwFlags;
    a.hwndParent = h->hwndParent;
    a.hInstance = h->hInstance;
    a.hIcon = h->hIcon;
    a.pszCaption = caption.p;
    a.nPages = h->nPages;
    a.nStartPage = h->nStartPage;
    a.ppsp = (LPCPROPSHEETPAGEA)&pages[0];
    return PropertySheetA(&a);
}

// --- shell32 ----------------------------------------------------------------------------

BOOL WINAPI LpShell_NotifyIconW(DWORD msg, PNOTIFYICONDATAW w)
{
    NT_FIRST("shell32.dll", Shell_NotifyIconW, (msg, w));
    NOTIFYICONDATAA a;
    ZeroMemory(&a, sizeof(a));
    a.cbSize = NOTIFYICONDATAA_V1_SIZE;   // 64-character tip: what Windows 95/98 accept
    a.hWnd = w->hWnd;
    a.uID = w->uID;
    a.uFlags = w->uFlags & (NIF_MESSAGE | NIF_ICON | NIF_TIP);
    a.uCallbackMessage = w->uCallbackMessage;
    a.hIcon = w->hIcon;
    if (w->uFlags & NIF_TIP) {
        A tip(w->szTip);
        CopyAnsi(tip.p, a.szTip, 64);
    }
    return Shell_NotifyIconA(msg, &a);
}

// The usual rules: spaces separate arguments, quotes group them, and backslashes escape a
// quote only when they come right before one. One LocalAlloc block, as Windows returns it.
LPWSTR* WINAPI LpCommandLineToArgvW(LPCWSTR cmd, int* count)
{
    NT_FIRST("shell32.dll", CommandLineToArgvW, (cmd, count));
    std::vector<std::wstring> args;
    const wchar_t* p = cmd ? cmd : L"";
    while (*p) {
        while (*p == L' ' || *p == L'\t')
            ++p;
        if (!*p)
            break;
        std::wstring arg;
        bool quoted = false;
        while (*p && (quoted || (*p != L' ' && *p != L'\t'))) {
            if (*p == L'\\') {
                size_t slashes = 0;
                while (*p == L'\\') {
                    ++slashes;
                    ++p;
                }
                if (*p == L'"') {
                    arg.append(slashes / 2, L'\\');
                    if (slashes % 2) {
                        arg += L'"';
                        ++p;
                    }
                } else {
                    arg.append(slashes, L'\\');
                }
            } else if (*p == L'"') {
                quoted = !quoted;
                ++p;
            } else {
                arg += *p++;
            }
        }
        args.push_back(arg);
    }
    size_t bytes = (args.size() + 1) * sizeof(LPWSTR);
    for (size_t i = 0; i < args.size(); ++i)
        bytes += (args[i].size() + 1) * sizeof(wchar_t);
    LPWSTR* out = (LPWSTR*)LocalAlloc(LMEM_FIXED, bytes);
    if (!out)
        return NULL;
    wchar_t* text = (wchar_t*)(out + args.size() + 1);
    for (size_t i = 0; i < args.size(); ++i) {
        out[i] = text;
        memcpy(text, args[i].c_str(), (args[i].size() + 1) * sizeof(wchar_t));
        text += args[i].size() + 1;
    }
    out[args.size()] = NULL;
    if (count)
        *count = (int)args.size();
    return out;
}

LPITEMIDLIST WINAPI LpSHBrowseForFolderW(LPBROWSEINFOW w)
{
    NT_FIRST("shell32.dll", SHBrowseForFolderW, (w));
    A title(w->lpszTitle);
    char name[MAX_PATH * 2];
    BROWSEINFOA a;
    ZeroMemory(&a, sizeof(a));
    a.hwndOwner = w->hwndOwner;
    a.pidlRoot = w->pidlRoot;
    a.pszDisplayName = name;
    a.lpszTitle = title.p;
    a.ulFlags = w->ulFlags;
    LPITEMIDLIST r = SHBrowseForFolderA(&a);
    if (r && w->pszDisplayName)
        CopyOut(Wide(name), w->pszDisplayName, MAX_PATH);
    return r;
}

BOOL WINAPI LpSHGetPathFromIDListW(LPCITEMIDLIST pidl, LPWSTR out)
{
    NT_FIRST("shell32.dll", SHGetPathFromIDListW, (pidl, out));
    char buf[MAX_PATH * 2];
    if (!SHGetPathFromIDListA(pidl, buf))
        return FALSE;
    CopyOut(Wide(buf), out, MAX_PATH);
    return TRUE;
}

// --- winmm ------------------------------------------------------------------------------

MMRESULT WINAPI LpwaveOutGetDevCapsW(UINT_PTR id, LPWAVEOUTCAPSW w, UINT size)
{
    NT_FIRST("winmm.dll", waveOutGetDevCapsW, (id, w, size));
    WAVEOUTCAPSA a;
    MMRESULT r = waveOutGetDevCapsA(id, &a, sizeof(a));
    if (r != MMSYSERR_NOERROR || size < sizeof(WAVEOUTCAPSW))
        return r;
    w->wMid = a.wMid;
    w->wPid = a.wPid;
    w->vDriverVersion = a.vDriverVersion;
    CopyOut(Wide(a.szPname), w->szPname, MAXPNAMELEN);
    w->dwFormats = a.dwFormats;
    w->wChannels = a.wChannels;
    w->wReserved1 = a.wReserved1;
    w->dwSupport = a.dwSupport;
    return r;
}

MMRESULT WINAPI LpwaveOutGetErrorTextW(MMRESULT err, LPWSTR out, UINT chars)
{
    NT_FIRST("winmm.dll", waveOutGetErrorTextW, (err, out, chars));
    char buf[MAXERRORLENGTH * 2];
    MMRESULT r = waveOutGetErrorTextA(err, buf, sizeof(buf));
    if (r == MMSYSERR_NOERROR)
        CopyOut(Wide(buf), out, (int)chars);
    return r;
}

} // namespace

bool Win9xMode()
{
    if (g_mode < 0) {
        char v[8];
        DWORD n = GetEnvironmentVariableA("NAVI2K_FORCE_ANSI", v, sizeof(v));
        bool forced = n > 0 && n < sizeof(v) && v[0] == '1';
        g_mode = ((GetVersion() & 0x80000000u) != 0 || forced) ? 1 : 0;
    }
    return g_mode == 1;
}

bool AnsiCanHold(wchar_t c)
{
    if (c < 0x80)
        return true;
    char buf[8];
    BOOL lossy = FALSE;
    int n = WideCharToMultiByte(CP_ACP, 0, &c, 1, buf, sizeof(buf), NULL, &lossy);
    if (n <= 0 || lossy)
        return false;
    wchar_t back = 0;
    return MultiByteToWideChar(CP_ACP, 0, buf, n, &back, 1) == 1 && back == c;
}

// The import slots. The names carry the stdcall argument size, which the linker matches.
#define LP_SLOT(fn, decorated) void* lp_slot_##fn __asm__("__imp__" decorated) = (void*)Lp##fn;

extern "C" {
LP_SLOT(CreateFileW, "CreateFileW@28")
LP_SLOT(CreateDirectoryW, "CreateDirectoryW@8")
LP_SLOT(RemoveDirectoryW, "RemoveDirectoryW@4")
LP_SLOT(DeleteFileW, "DeleteFileW@4")
LP_SLOT(GetFileAttributesW, "GetFileAttributesW@4")
LP_SLOT(MoveFileW, "MoveFileW@8")
LP_SLOT(MoveFileExW, "MoveFileExW@12")
LP_SLOT(FindFirstFileW, "FindFirstFileW@8")
LP_SLOT(FindNextFileW, "FindNextFileW@8")
LP_SLOT(GetModuleFileNameW, "GetModuleFileNameW@12")
LP_SLOT(GetModuleHandleW, "GetModuleHandleW@4")
LP_SLOT(LoadLibraryW, "LoadLibraryW@4")
LP_SLOT(CreateEventW, "CreateEventW@16")
LP_SLOT(OutputDebugStringW, "OutputDebugStringW@4")
LP_SLOT(GetPrivateProfileIntW, "GetPrivateProfileIntW@16")
LP_SLOT(GetPrivateProfileStringW, "GetPrivateProfileStringW@24")
LP_SLOT(WritePrivateProfileStringW, "WritePrivateProfileStringW@16")
LP_SLOT(lstrcmpiW, "lstrcmpiW@8")
LP_SLOT(lstrcpyW, "lstrcpyW@8")
LP_SLOT(lstrcpynW, "lstrcpynW@12")
LP_SLOT(TryEnterCriticalSection, "TryEnterCriticalSection@4")
LP_SLOT(IsProcessorFeaturePresent, "IsProcessorFeaturePresent@4")
LP_SLOT(CreateFontIndirectW, "CreateFontIndirectW@4")
LP_SLOT(GetObjectW, "GetObjectW@12")
LP_SLOT(GetTextMetricsW, "GetTextMetricsW@8")
LP_SLOT(CreateWindowExW, "CreateWindowExW@48")
LP_SLOT(DefWindowProcW, "DefWindowProcW@16")
LP_SLOT(CallWindowProcW, "CallWindowProcW@20")
LP_SLOT(RegisterClassExW, "RegisterClassExW@4")
LP_SLOT(RegisterWindowMessageW, "RegisterWindowMessageW@4")
LP_SLOT(GetMessageW, "GetMessageW@16")
LP_SLOT(DispatchMessageW, "DispatchMessageW@4")
LP_SLOT(TranslateAcceleratorW, "TranslateAcceleratorW@12")
LP_SLOT(CreateAcceleratorTableW, "CreateAcceleratorTableW@8")
LP_SLOT(PostMessageW, "PostMessageW@16")
LP_SLOT(SendMessageW, "SendMessageW@16")
LP_SLOT(SendDlgItemMessageW, "SendDlgItemMessageW@20")
LP_SLOT(SetWindowTextW, "SetWindowTextW@8")
LP_SLOT(GetWindowTextW, "GetWindowTextW@12")
LP_SLOT(GetWindowTextLengthW, "GetWindowTextLengthW@4")
LP_SLOT(SetDlgItemTextW, "SetDlgItemTextW@12")
LP_SLOT(GetDlgItemTextW, "GetDlgItemTextW@16")
LP_SLOT(GetWindowLongW, "GetWindowLongW@8")
LP_SLOT(SetWindowLongW, "SetWindowLongW@12")
LP_SLOT(GetClassLongW, "GetClassLongW@8")
LP_SLOT(DialogBoxParamW, "DialogBoxParamW@20")
LP_SLOT(SystemParametersInfoW, "SystemParametersInfoW@16")
LP_SLOT(AppendMenuW, "AppendMenuW@16")
LP_SLOT(InsertMenuW, "InsertMenuW@20")
LP_SLOT(LoadCursorW, "LoadCursorW@8")
LP_SLOT(LoadImageW, "LoadImageW@24")
LP_SLOT(DrawTextW, "DrawTextW@20")
LP_SLOT(CharLowerBuffW, "CharLowerBuffW@8")
LP_SLOT(PropertySheetW, "PropertySheetW@4")
LP_SLOT(Shell_NotifyIconW, "Shell_NotifyIconW@8")
LP_SLOT(SHBrowseForFolderW, "SHBrowseForFolderW@4")
LP_SLOT(SHGetPathFromIDListW, "SHGetPathFromIDListW@8")
LP_SLOT(waveOutGetDevCapsW, "waveOutGetDevCapsW@12")
LP_SLOT(waveOutGetErrorTextW, "waveOutGetErrorTextW@12")
LP_SLOT(WriteConsoleW, "WriteConsoleW@20")
LP_SLOT(RegisterClassW, "RegisterClassW@4")
LP_SLOT(CommandLineToArgvW, "CommandLineToArgvW@8")
void* lp_slot_beginthreadex __asm__("__imp___beginthreadex") = (void*)LpBeginThreadEx;
}

// The C runtime calls these two directly (not through the import slot), which would pull in
// kernel32's own definitions; defining the direct-call names too keeps them out.
extern "C" BOOL WINAPI lp_thunk_IsProcessorFeaturePresent(DWORD f) __asm__("_IsProcessorFeaturePresent@4");
extern "C" BOOL WINAPI lp_thunk_IsProcessorFeaturePresent(DWORD f)
{
    return LpIsProcessorFeaturePresent(f);
}

extern "C" BOOL WINAPI lp_thunk_TryEnterCriticalSection(LPCRITICAL_SECTION cs) __asm__("_TryEnterCriticalSection@4");
extern "C" BOOL WINAPI lp_thunk_TryEnterCriticalSection(LPCRITICAL_SECTION cs)
{
    return LpTryEnterCriticalSection(cs);
}
