#include "art.h"
#include "util.h"

#include "stb_image.h"

HBITMAP ArtDecode(const std::string& bytes, int maxSide, COLORREF background, std::wstring& err)
{
    int w = 0, h = 0, comp = 0;
    unsigned char* rgba = stbi_load_from_memory((const stbi_uc*)bytes.data(), (int)bytes.size(),
                                                &w, &h, &comp, 4);
    if (!rgba) {
        err = Utf8ToWide(std::string("Could not decode the cover image: ") + stbi_failure_reason());
        return NULL;
    }

    int tw = w, th = h;
    if (w > maxSide || h > maxSide) {
        if (w >= h) {
            tw = maxSide;
            th = h * maxSide / w;
        } else {
            th = maxSide;
            tw = w * maxSide / h;
        }
        if (tw < 1) tw = 1;
        if (th < 1) th = 1;
    }

    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = tw;
    bmi.bmiHeader.biHeight = -th;   // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = NULL;
    HBITMAP bmp = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp) {
        stbi_image_free(rgba);
        err = L"Could not create a bitmap for the cover image.";
        return NULL;
    }

    const int bgR = GetRValue(background), bgG = GetGValue(background), bgB = GetBValue(background);
    unsigned char* dst = (unsigned char*)bits;
    for (int y = 0; y < th; ++y) {
        int sy0 = y * h / th, sy1 = (y + 1) * h / th;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int x = 0; x < tw; ++x) {
            int sx0 = x * w / tw, sx1 = (x + 1) * w / tw;
            if (sx1 <= sx0) sx1 = sx0 + 1;

            // Area average of the source block (exact copy when not scaling).
            unsigned r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int sy = sy0; sy < sy1; ++sy) {
                const unsigned char* p = rgba + ((size_t)sy * w + sx0) * 4;
                for (int sx = sx0; sx < sx1; ++sx, p += 4) {
                    r += p[0]; g += p[1]; b += p[2]; a += p[3];
                    ++n;
                }
            }
            r /= n; g /= n; b /= n; a /= n;
            dst[0] = (unsigned char)((b * a + bgB * (255 - a)) / 255);
            dst[1] = (unsigned char)((g * a + bgG * (255 - a)) / 255);
            dst[2] = (unsigned char)((r * a + bgR * (255 - a)) / 255);
            dst[3] = 0;
            dst += 4;
        }
    }
    stbi_image_free(rgba);
    GdiFlush();
    return bmp;
}
