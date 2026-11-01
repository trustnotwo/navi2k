// Drawing text that Windows won't draw by itself:
//   - Windows 95/98/ME: anything the ANSI code page can't hold (Japanese on an English Windows
//     98, say). 9x's window controls are ANSI, but its GDI draws Unicode through ExtTextOutW and
//     measures it with GetTextExtentPoint32W, given a font that has the characters.
//   - Windows 2000 and later without East Asian language support: Japanese, Chinese and
//     Korean, which no installed font has. DroidSansFallback.ttf next to the exe is loaded
//     privately and the player draws those strings itself in its lists and panels.
// Neither has font fallback that would find our font, so each run of characters gets a font
// chosen here:
//
//   - characters the code page holds: the font selected in the DC
//   - anything else: "Arial Unicode MS" if it's installed (it covers every script)
//   - Japanese, Chinese and Korean: an installed font for that script's character set (East
//     Asian language support or Office installs them)
//   - other scripts (Cyrillic, Greek, Central European, ...): the selected font in the
//     character set whose code page holds the character
//
// 9x converts ExtTextOutW's text to the selected font's code page, which is why the character
// set matters as much as the font. Characters no installed font covers still show as boxes.

#include "compat_win9x.h"

#include <windows.h>
#include <string.h>
#include <map>
#include <string>
#include <vector>

namespace {

struct Choice {               // a way to draw a character that the base font can't
    BYTE charset;
    std::string face;         // empty: the base font's face in that character set
};

std::vector<Choice> g_choices;          // index = choice number
std::vector<signed char> g_charChoice;  // per character: -2 unknown, -1 base font, else choice
bool g_fontsProbed = false;
std::string g_universalFace;            // "Arial Unicode MS" when installed
std::map<BYTE, std::string> g_eastAsianFace;   // charset -> an installed TrueType face
bool g_ntFallback = false;              // 2000+: no East Asian font installed, bundled one loaded
const char kBundledFace[] = "Droid Sans Fallback";
const wchar_t kBundledFile[] = L"\\DroidSansFallback.ttf";

struct FontEntry {
    HFONT font;
    int ascent;
};
std::map<std::pair<HFONT, int>, FontEntry> g_fonts;   // (base font, choice) -> font

// How well a face suits UI text: proportional UI fonts ("MS UI Gothic", "MS PGothic") read
// best, then other proportional and sans-serif faces, then whatever is installed.
int FaceScore(const char* face)
{
    std::string f = face;
    int score = 1;
    if (f.find(" UI ") != std::string::npos || f.find("UI Gothic") != std::string::npos)
        score += 8;
    if (f.find("MS P") == 0 || f.find("PMingLiU") == 0)
        score += 4;
    if (f.find("Gothic") != std::string::npos || f.find("Gulim") != std::string::npos ||
        f.find("Dotum") != std::string::npos || f.find("SimHei") != std::string::npos ||
        f.find("YaHei") != std::string::npos || f.find("JhengHei") != std::string::npos)
        score += 2;
    if (f.find("Light") != std::string::npos || f.find("Thin") != std::string::npos)   // too faint for UI text
        score -= 10;
    return score;
}

struct BestFace {
    std::string face;
    int score;
};

int CALLBACK BestTrueType(const LOGFONTA* lf, const TEXTMETRICA*, DWORD type, LPARAM out)
{
    if (!(type & TRUETYPE_FONTTYPE) || lf->lfFaceName[0] == '@')   // '@': vertical-writing variant
        return 1;
    BestFace* best = (BestFace*)out;
    const int score = FaceScore(lf->lfFaceName);
    if (score > best->score) {
        best->face = lf->lfFaceName;
        best->score = score;
    }
    return 1;
}

void ProbeFonts()
{
    g_fontsProbed = true;
    HDC dc = GetDC(NULL);
    LOGFONTA lf;
    ZeroMemory(&lf, sizeof(lf));
    lf.lfCharSet = DEFAULT_CHARSET;
    lstrcpyA(lf.lfFaceName, "Arial Unicode MS");
    BestFace found;
    found.score = 0;
    EnumFontFamiliesExA(dc, &lf, BestTrueType, (LPARAM)&found, 0);
    g_universalFace = found.face;
    static const BYTE kEastAsian[] = { SHIFTJIS_CHARSET, GB2312_CHARSET, CHINESEBIG5_CHARSET, HANGUL_CHARSET };
    for (size_t i = 0; i < sizeof(kEastAsian); ++i) {
        ZeroMemory(&lf, sizeof(lf));
        lf.lfCharSet = kEastAsian[i];
        found.face.clear();
        found.score = 0;
        EnumFontFamiliesExA(dc, &lf, BestTrueType, (LPARAM)&found, 0);
        if (!found.face.empty())
            g_eastAsianFace[kEastAsian[i]] = found.face;
    }
    ReleaseDC(NULL, dc);
}

int AddChoice(BYTE charset, const std::string& face)
{
    for (size_t i = 0; i < g_choices.size(); ++i)
        if (g_choices[i].charset == charset && g_choices[i].face == face)
            return (int)i;
    Choice c;
    c.charset = charset;
    c.face = face;
    g_choices.push_back(c);
    return (int)g_choices.size() - 1;
}

// True if code page cp holds c; when that code page isn't installed, 'unknown' is returned.
bool CodePageHolds(UINT cp, wchar_t c, bool unknown)
{
    char buf[8];
    BOOL lossy = FALSE;
    int n = WideCharToMultiByte(cp, 0, &c, 1, buf, sizeof(buf), NULL, &lossy);
    if (n <= 0)
        return GetLastError() == ERROR_INVALID_PARAMETER ? unknown : false;
    return !lossy;
}

bool IsKana(wchar_t c) { return (c >= 0x3040 && c <= 0x30FF) || (c >= 0x31F0 && c <= 0x31FF) || (c >= 0xFF65 && c <= 0xFF9F); }
bool IsHangul(wchar_t c) { return (c >= 0x1100 && c <= 0x11FF) || (c >= 0x3130 && c <= 0x318F) || (c >= 0xAC00 && c <= 0xD7AF); }
bool IsHan(wchar_t c)
{
    return (c >= 0x2E80 && c <= 0x2FDF) || (c >= 0x3000 && c <= 0x303F) || (c >= 0x3190 && c <= 0x9FFF) ||
           (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE4F) || (c >= 0xFF00 && c <= 0xFFEF);
}

int ChoiceFor(wchar_t c)
{
    if (g_charChoice.empty())
        g_charChoice.assign(65536, -2);
    signed char& slot = g_charChoice[(unsigned short)c];
    if (slot != -2)
        return slot;
    if (!g_fontsProbed)
        ProbeFonts();
    int choice = -1;
    if (AnsiCanHold(c)) {
        choice = -1;
    } else if (!g_universalFace.empty()) {
        choice = AddChoice(DEFAULT_CHARSET, g_universalFace);
    } else if (IsKana(c) || IsHangul(c) || IsHan(c)) {
        // Japanese first for kana and shared ideographs, Korean first for hangul.
        static const struct { BYTE cs; UINT cp; } kOrder[] = {
            { SHIFTJIS_CHARSET, 932 }, { GB2312_CHARSET, 936 }, { CHINESEBIG5_CHARSET, 950 }, { HANGUL_CHARSET, 949 } };
        const int n = sizeof(kOrder) / sizeof(kOrder[0]);
        for (int k = 0; k < n && choice < 0; ++k) {
            const int i = IsHangul(c) ? (k + n - 1) % n : k;   // hangul: Korean, then the rest
            std::map<BYTE, std::string>::const_iterator f = g_eastAsianFace.find(kOrder[i].cs);
            if (f != g_eastAsianFace.end() && CodePageHolds(kOrder[i].cp, c, true))
                choice = AddChoice(kOrder[i].cs, f->second);
        }
        if (choice < 0 && g_ntFallback)   // the private font isn't always listed by EnumFontFamiliesEx
            choice = AddChoice(IsHangul(c) ? HANGUL_CHARSET : SHIFTJIS_CHARSET, kBundledFace);
    } else {
        static const struct { BYTE cs; UINT cp; } kSingleByte[] = {
            { EASTEUROPE_CHARSET, 1250 }, { RUSSIAN_CHARSET, 1251 }, { GREEK_CHARSET, 1253 }, { TURKISH_CHARSET, 1254 },
            { BALTIC_CHARSET, 1257 }, { HEBREW_CHARSET, 1255 }, { ARABIC_CHARSET, 1256 }, { THAI_CHARSET, 874 },
            { VIETNAMESE_CHARSET, 1258 } };
        for (size_t i = 0; i < sizeof(kSingleByte) / sizeof(kSingleByte[0]) && choice < 0; ++i)
            if (CodePageHolds(kSingleByte[i].cp, c, false))
                choice = AddChoice(kSingleByte[i].cs, std::string());
    }
    slot = (signed char)(choice > 120 ? -1 : choice);
    return slot;
}

int AscentOf(HDC dc, HFONT font)
{
    HGDIOBJ old = SelectObject(dc, font);
    TEXTMETRICA tm;
    int ascent = GetTextMetricsA(dc, &tm) ? tm.tmAscent : 0;
    SelectObject(dc, old);
    return ascent;
}

const FontEntry& FontFor(HDC dc, HFONT base, int choice)
{
    std::pair<HFONT, int> key(base, choice);
    std::map<std::pair<HFONT, int>, FontEntry>::iterator it = g_fonts.find(key);
    if (it != g_fonts.end())
        return it->second;
    FontEntry e;
    e.font = base;
    if (choice >= 0) {
        LOGFONTA lf;
        if (GetObjectA(base, sizeof(lf), &lf)) {
            lf.lfCharSet = g_choices[choice].charset;
            if (!g_choices[choice].face.empty())
                lstrcpynA(lf.lfFaceName, g_choices[choice].face.c_str(), LF_FACESIZE);
            lf.lfOutPrecision = OUT_TT_PRECIS;
            HFONT f = CreateFontIndirectA(&lf);
            if (f)
                e.font = f;
        }
    }
    e.ascent = AscentOf(dc, e.font);
    return g_fonts[key] = e;
}

// One line of text: each character's font and width.
struct Line {
    std::wstring text;
    std::vector<int> font;    // choice per character (-1 base)
    std::vector<int> width;   // advance per character
    int Width(size_t n) const
    {
        int w = 0;
        for (size_t i = 0; i < n && i < width.size(); ++i)
            w += width[i];
        return w;
    }
};

// Character widths per font, measured once: the description panel measures its text many
// times per paint to find where to cut it, which would be slow on a Pentium otherwise.
std::map<std::pair<HFONT, wchar_t>, int> g_widths;

void Measure(HDC dc, HFONT base, Line& line)
{
    line.font.resize(line.text.size());
    line.width.resize(line.text.size());
    HGDIOBJ old = NULL;
    HFONT selected = NULL;
    for (size_t i = 0; i < line.text.size(); ++i) {
        const wchar_t c = line.text[i];
        line.font[i] = ChoiceFor(c);
        HFONT f = FontFor(dc, base, line.font[i]).font;
        std::pair<HFONT, wchar_t> key(f, c);
        std::map<std::pair<HFONT, wchar_t>, int>::const_iterator w = g_widths.find(key);
        if (w != g_widths.end()) {
            line.width[i] = w->second;
            continue;
        }
        if (f != selected) {
            HGDIOBJ prev = SelectObject(dc, f);
            if (!old)
                old = prev;
            selected = f;
        }
        SIZE sz = { 0, 0 };
        GetTextExtentPoint32W(dc, &c, 1, &sz);
        line.width[i] = g_widths[key] = sz.cx;
    }
    if (old)
        SelectObject(dc, old);
}

void DrawRuns(HDC dc, HFONT base, const Line& line, size_t count, int x, int baseline, const RECT* clip)
{
    UINT oldAlign = SetTextAlign(dc, TA_BASELINE | TA_LEFT | TA_NOUPDATECP);
    for (size_t i = 0; i < count;) {
        size_t j = i;
        while (j < count && line.font[j] == line.font[i])
            ++j;
        HGDIOBJ old = SelectObject(dc, FontFor(dc, base, line.font[i]).font);
        ExtTextOutW(dc, x, baseline, clip ? ETO_CLIPPED : 0, clip, &line.text[i], (UINT)(j - i), NULL);
        SelectObject(dc, old);
        for (size_t k = i; k < j; ++k)
            x += line.width[k];
        i = j;
    }
    SetTextAlign(dc, oldAlign);
}

bool BreaksAnywhere(wchar_t c) { return IsKana(c) || IsHan(c) || IsHangul(c); }

// Greedy word wrap: break after spaces and between East Asian characters; a word wider than
// the line is broken between characters.
void Wrap(const Line& para, int maxWidth, std::vector<Line>& out)
{
    size_t start = 0;
    const size_t n = para.text.size();
    while (start < n) {
        int w = 0;
        size_t i = start, lastBreak = std::wstring::npos;
        for (; i < n; ++i) {
            if (w + para.width[i] > maxWidth && i > start)
                break;
            w += para.width[i];
            if (para.text[i] == L' ' || BreaksAnywhere(para.text[i]) ||
                (i + 1 < n && BreaksAnywhere(para.text[i + 1])))
                lastBreak = i + 1;
        }
        size_t end = i;
        if (i < n && lastBreak != std::wstring::npos && lastBreak > start)
            end = lastBreak;
        Line l;
        l.text = para.text.substr(start, end - start);
        l.font.assign(para.font.begin() + start, para.font.begin() + end);
        l.width.assign(para.width.begin() + start, para.width.begin() + end);
        while (!l.text.empty() && l.text[l.text.size() - 1] == L' ') {   // trailing spaces don't count
            l.text.erase(l.text.size() - 1);
            l.font.pop_back();
            l.width.pop_back();
        }
        out.push_back(l);
        start = end;
        while (start < n && para.text[start] == L' ')
            ++start;
    }
    if (n == 0)
        out.push_back(Line());
}

std::string g_bundledFont;    // 9x: path loaded with AddFontResourceA, removed at exit
std::wstring g_bundledFontW;  // 2000+: path loaded privately with AddFontResourceExW

// True if any TrueType font for Japanese, Chinese or Korean is installed (on 2000 and later,
// East Asian language support adds them and Windows' own font linking then draws that text).
bool EastAsianFontInstalled()
{
    static const BYTE kEastAsian[] = { SHIFTJIS_CHARSET, GB2312_CHARSET, CHINESEBIG5_CHARSET, HANGUL_CHARSET };
    HDC dc = GetDC(NULL);
    bool found = false;
    for (size_t i = 0; i < sizeof(kEastAsian) && !found; ++i) {
        LOGFONTA lf;
        ZeroMemory(&lf, sizeof(lf));
        lf.lfCharSet = kEastAsian[i];
        BestFace best;
        best.score = 0;
        EnumFontFamiliesExA(dc, &lf, BestTrueType, (LPARAM)&best, 0);
        found = !best.face.empty();
    }
    ReleaseDC(NULL, dc);
    return found;
}

} // namespace

void LoadFallbackFont(const std::wstring& exeDir)
{
    if (!g_bundledFont.empty() || !g_bundledFontW.empty())
        return;
    const std::wstring pathW = exeDir + kBundledFile;
    if (!Win9xMode()) {
        // 2000 and later: only needed when nothing installed covers East Asian text.
        typedef int (WINAPI *AddExFn)(LPCWSTR, DWORD, PVOID);
        AddExFn add = (AddExFn)GetProcAddress(GetModuleHandleA("gdi32.dll"), "AddFontResourceExW");
        if (!add || GetFileAttributesW(pathW.c_str()) == INVALID_FILE_ATTRIBUTES || EastAsianFontInstalled())
            return;
        if (add(pathW.c_str(), FR_PRIVATE, NULL) > 0) {   // this process only; gone when it exits
            g_bundledFontW = pathW;
            g_ntFallback = true;
        }
        return;
    }
    int n = WideCharToMultiByte(CP_ACP, 0, pathW.c_str(), -1, NULL, 0, NULL, NULL);
    if (n <= 0)
        return;
    std::string path(n, '\0');
    WideCharToMultiByte(CP_ACP, 0, pathW.c_str(), -1, &path[0], n, NULL, NULL);
    path.resize(n - 1);
    if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return;
    // For this session only (nothing is installed); it then shows up in the font list under the
    // Japanese, Chinese and Korean character sets it declares, where ProbeFonts finds it.
    if (AddFontResourceA(path.c_str()) > 0)
        g_bundledFont = path;
}

void UnloadFallbackFont()
{
    if (!g_bundledFont.empty())
        RemoveFontResourceA(g_bundledFont.c_str());
    if (!g_bundledFontW.empty()) {
        typedef BOOL (WINAPI *RemoveExFn)(LPCWSTR, DWORD, PVOID);
        RemoveExFn remove = (RemoveExFn)GetProcAddress(GetModuleHandleA("gdi32.dll"), "RemoveFontResourceExW");
        if (remove)
            remove(g_bundledFontW.c_str(), FR_PRIVATE, NULL);
    }
    g_bundledFont.clear();
    g_bundledFontW.clear();
    g_ntFallback = false;
}

bool OwnTextDrawing()
{
    return Win9xMode() || g_ntFallback;
}

bool NeedsUnicodeDrawing(const wchar_t* text, int len)
{
    if (!text || !OwnTextDrawing())
        return false;
    if (len < 0)
        len = lstrlenW(text);
    for (int i = 0; i < len; ++i) {
        const wchar_t c = text[i];
        if (c < 0x80)
            continue;
        if (g_ntFallback ? (IsKana(c) || IsHan(c) || IsHangul(c)) : !AnsiCanHold(c))
            return true;
    }
    return false;
}

int DrawTextWithFallback(HDC dc, const wchar_t* text, int len, RECT* rc, UINT fmt)
{
    if (len < 0)
        len = lstrlenW(text);
    HFONT base = (HFONT)GetCurrentObject(dc, OBJ_FONT);
    TEXTMETRICA tm;
    if (!GetTextMetricsA(dc, &tm))
        return 0;
    const int lineH = tm.tmHeight;
    const int ascent = tm.tmAscent;

    // Paragraphs and lines.
    std::wstring all(text, len);
    std::vector<Line> lines;
    const bool single = (fmt & DT_SINGLELINE) || !(fmt & DT_WORDBREAK);
    size_t pos = 0;
    while (pos <= all.size()) {
        size_t nl = single ? std::wstring::npos : all.find(L'\n', pos);
        std::wstring para = all.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
        for (size_t i = 0; i < para.size(); ++i)
            if (para[i] == L'\r' || para[i] == L'\n' || para[i] == L'\t')
                para[i] = L' ';
        Line l;
        l.text = para;
        Measure(dc, base, l);
        if (single || !(fmt & DT_WORDBREAK))
            lines.push_back(l);
        else
            Wrap(l, rc->right - rc->left, lines);
        if (nl == std::wstring::npos)
            break;
        pos = nl + 1;
    }

    const int height = (int)lines.size() * lineH;
    if (fmt & DT_CALCRECT) {
        int widest = 0;
        for (size_t i = 0; i < lines.size(); ++i)
            if (lines[i].Width(lines[i].text.size()) > widest)
                widest = lines[i].Width(lines[i].text.size());
        if (single || !(fmt & DT_WORDBREAK))
            rc->right = rc->left + widest;
        rc->bottom = rc->top + height;
        return height;
    }

    int top = rc->top;
    if ((fmt & DT_SINGLELINE) && (fmt & DT_VCENTER))
        top = rc->top + (rc->bottom - rc->top - lineH) / 2;
    else if ((fmt & DT_SINGLELINE) && (fmt & DT_BOTTOM))
        top = rc->bottom - lineH;
    const RECT* clip = (fmt & DT_NOCLIP) ? NULL : rc;
    const int avail = rc->right - rc->left;

    Line dots;
    dots.text = L"...";
    Measure(dc, base, dots);
    for (size_t i = 0; i < lines.size(); ++i) {
        Line& l = lines[i];
        size_t count = l.text.size();
        int w = l.Width(count);
        bool ellipsis = false;
        if ((fmt & (DT_END_ELLIPSIS | DT_WORD_ELLIPSIS)) && w > avail) {
            const int room = avail - dots.Width(3);
            while (count > 0 && l.Width(count) > room)
                --count;
            w = l.Width(count) + dots.Width(3);
            ellipsis = true;
        }
        int x = rc->left;
        if (fmt & DT_CENTER)
            x = rc->left + (avail - w) / 2;
        else if (fmt & DT_RIGHT)
            x = rc->right - w;
        const int baseline = top + (int)i * lineH + ascent;
        DrawRuns(dc, base, l, count, x, baseline, clip);
        if (ellipsis)
            DrawRuns(dc, base, dots, 3, x + l.Width(count), baseline, clip);
    }
    return height;
}
