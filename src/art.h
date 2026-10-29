#ifndef LP_ART_H
#define LP_ART_H

#include <windows.h>
#include <string>
#include <vector>

// Decodes JPEG/PNG bytes into a 32-bit top-down DIB section no larger than maxSide x maxSide
// (aspect kept, area-averaged downscale), with any transparency composited onto 'background'.
// Safe to call from a worker thread. Returns NULL on failure; caller DeleteObject()s the result.
HBITMAP ArtDecode(const std::string& bytes, int maxSide, COLORREF background, std::wstring& err);

// True for the grey "no photo" person silhouette some online sources hand Navidrome as an
// artist image: a light, colourless picture matching its 8x8 brightness fingerprint.
bool ArtIsPlaceholder(HBITMAP bmp);

// Builds a side x side collage from 2-4 encoded images (each cropped to fill its tile):
// 2 side by side, 3 as one on the left and two stacked on the right, 4 as a 2x2 grid.
// Images that fail to decode are left out. Safe on a worker thread; NULL if none decode.
HBITMAP ArtCollage(const std::vector<std::string>& images, int side, COLORREF background, std::wstring& err);

#endif
