#ifndef LP_ART_H
#define LP_ART_H

#include <windows.h>
#include <string>

// Decodes JPEG/PNG bytes into a 32-bit top-down DIB section no larger than maxSide x maxSide
// (aspect kept, area-averaged downscale), with any transparency composited onto 'background'.
// Safe to call from a worker thread. Returns NULL on failure; caller DeleteObject()s the result.
HBITMAP ArtDecode(const std::string& bytes, int maxSide, COLORREF background, std::wstring& err);

#endif
