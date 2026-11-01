#ifndef LP_COMPAT_WIN9X_H
#define LP_COMPAT_WIN9X_H

// Windows 95/98/ME support. compat_win9x.cpp stands in for the Unicode ("W") API functions
// the player calls: on Windows NT (2000, XP and later) each one calls the real function, and
// on Windows 9x, which has almost none of them, it converts the strings and calls the ANSI
// ("A") function instead.

#include <windows.h>
#include <string>

// True on Windows 95/98/ME, and on NT when NAVI2K_FORCE_ANSI=1 is set in the environment
// (for testing the 9x code paths on a newer Windows).
bool Win9xMode();

// True if the character survives a round trip through the ANSI code page, so it can be
// used in a file name on 9x.
bool AnsiCanHold(wchar_t c);

// Loads DroidSansFallback.ttf from the exe's folder, if it's there (call before the first
// window paints), so Japanese, Chinese and Korean show without installing a font:
//   - Windows 9x: always, for this session (AddFontResource)
//   - 2000 and later: only when no East Asian font is installed, privately for this process
// UnloadFallbackFont removes it at exit.
void LoadFallbackFont(const std::wstring& exeDir);
void UnloadFallbackFont();

// True when the player has to draw some text itself (its lists, tree and panels): always on
// 9x, and on 2000 and later while the fallback font is in use.
bool OwnTextDrawing();

// True when this text has to be drawn with DrawTextWithFallback (DrawTextW does that by
// itself): on 9x, characters the ANSI code page can't hold; on 2000+ with the fallback font,
// Japanese, Chinese and Korean.
bool NeedsUnicodeDrawing(const wchar_t* text, int len = -1);

// DrawText for such text: draws it with the Unicode GDI functions, picking a font for each run
// of characters the current font lacks (compat_win9x_text.cpp). Handles the DrawText formats
// the player uses: single line with alignment, vertical centring and an end ellipsis; word
// wrap; DT_CALCRECT.
int DrawTextWithFallback(HDC dc, const wchar_t* text, int len, RECT* rc, UINT fmt);

#endif
