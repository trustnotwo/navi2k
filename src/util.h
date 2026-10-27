#ifndef LP_UTIL_H
#define LP_UTIL_H

#include <windows.h>
#include <string>

std::wstring Utf8ToWide(const std::string& s);
std::string  WideToUtf8(const std::wstring& s);
std::string  UrlEncode(const std::string& s);       // RFC 3986 unreserved set kept as-is
std::wstring Fmt(const wchar_t* fmt, ...);          // msvcrt rules: %ls = wide, %hs = narrow
std::wstring ExeDirectory();
bool         EndsWithNoCase(const std::wstring& s, const wchar_t* suffix);

// Appends to a UTF-8 log next to the exe (and OutputDebugString). Never logs tokens.
void LogInit(const std::wstring& path);
void LogF(const wchar_t* fmt, ...);

#endif
