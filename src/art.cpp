#include "art.h"
#include "util.h"

#include "stb_image.h"

namespace {
struct Rgba {
    unsigned char* p;
    int w, h;
};

// Scales the source to fill the tile, cropping the centre, area-averaged, onto a 32-bit
// top-down DIB of width 'stride' pixels.
void FillTile(const Rgba& src, unsigned char* dib, int stride, int tx, int ty, int tw, int th,
              COLORREF background)
{
    // The largest centred part of the source with the tile's aspect ratio.
    int sw = src.w, sh = src.h;
    if ((long long)src.w * th > (long long)src.h * tw)
        sw = (int)((long long)src.h * tw / th);
    else
        sh = (int)((long long)src.w * th / tw);
    if (sw < 1) sw = 1;
    if (sh < 1) sh = 1;
    const int ox = (src.w - sw) / 2, oy = (src.h - sh) / 2;
    const int bgR = GetRValue(background), bgG = GetGValue(background), bgB = GetBValue(background);
    for (int y = 0; y < th; ++y) {
        int sy0 = oy + y * sh / th, sy1 = oy + (y + 1) * sh / th;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        unsigned char* dst = dib + ((size_t)(ty + y) * stride + tx) * 4;
        for (int x = 0; x < tw; ++x, dst += 4) {
            int sx0 = ox + x * sw / tw, sx1 = ox + (x + 1) * sw / tw;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            unsigned r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int sy = sy0; sy < sy1; ++sy) {
                const unsigned char* p = src.p + ((size_t)sy * src.w + sx0) * 4;
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
        }
    }
}
}

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

bool ArtIsPlaceholder(HBITMAP bmp)
{
    DIBSECTION ds;
    if (!bmp || GetObject(bmp, sizeof(ds), &ds) != sizeof(ds) || !ds.dsBm.bmBits || ds.dsBm.bmBitsPixel != 32)
        return false;
    const int w = ds.dsBm.bmWidth, h = ds.dsBm.bmHeight < 0 ? -ds.dsBm.bmHeight : ds.dsBm.bmHeight;
    if (w < 16 || w != h)
        return false;
    const unsigned char* bits = (const unsigned char*)ds.dsBm.bmBits;
    double cell[64] = { 0 };
    for (int y = 0; y < h; ++y) {
        const unsigned char* p = bits + (size_t)y * ds.dsBm.bmWidthBytes;
        for (int x = 0; x < w; ++x, p += 4) {
            const int hi = p[0] > p[1] ? (p[0] > p[2] ? p[0] : p[2]) : (p[1] > p[2] ? p[1] : p[2]);
            const int lo = p[0] < p[1] ? (p[0] < p[2] ? p[0] : p[2]) : (p[1] < p[2] ? p[1] : p[2]);
            if (hi - lo > 16)
                return false;   // has colour: a real picture
            cell[(y * 8 / h) * 8 + x * 8 / w] += 0.114 * p[0] + 0.587 * p[1] + 0.299 * p[2];
        }
    }
    double mean = 0;
    for (int i = 0; i < 64; ++i)
        mean += cell[i] / 64;
    unsigned long long hash = 0;
    for (int i = 0; i < 64; ++i)
        if (cell[i] > mean)
            hash |= 1ULL << i;
    // Light corners, and at most 4 of 64 cells differing from the silhouette's fingerprint.
    const double cornerMin = 200.0 * ((double)w * h / 64);
    if (cell[0] < cornerMin || cell[7] < cornerMin || cell[56] < cornerMin || cell[63] < cornerMin)
        return false;
    unsigned long long diff = hash ^ 0xFFFFC3C3E7E7FFFFULL;
    int n = 0;
    for (; diff; diff &= diff - 1)
        ++n;
    return n <= 4;
}

HBITMAP ArtCollage(const std::vector<std::string>& images, int side, COLORREF background, std::wstring& err)
{
    std::vector<Rgba> src;
    for (size_t i = 0; i < images.size() && src.size() < 4; ++i) {
        Rgba r;
        int comp = 0;
        r.p = stbi_load_from_memory((const stbi_uc*)images[i].data(), (int)images[i].size(), &r.w, &r.h, &comp, 4);
        if (r.p)
            src.push_back(r);
        else
            err = Utf8ToWide(std::string("Could not decode a collage image: ") + stbi_failure_reason());
    }
    if (src.empty())
        return NULL;

    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = side;
    bmi.bmiHeader.biHeight = -side;   // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = NULL;
    HBITMAP bmp = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (bmp) {
        unsigned char* dib = (unsigned char*)bits;
        const int half = side / 2, rest = side - half;
        switch (src.size()) {
        case 1:
            FillTile(src[0], dib, side, 0, 0, side, side, background);
            break;
        case 2:
            FillTile(src[0], dib, side, 0, 0, half, side, background);
            FillTile(src[1], dib, side, half, 0, rest, side, background);
            break;
        case 3:
            FillTile(src[0], dib, side, 0, 0, half, side, background);
            FillTile(src[1], dib, side, half, 0, rest, half, background);
            FillTile(src[2], dib, side, half, half, rest, rest, background);
            break;
        default:
            FillTile(src[0], dib, side, 0, 0, half, half, background);
            FillTile(src[1], dib, side, half, 0, rest, half, background);
            FillTile(src[2], dib, side, 0, half, half, rest, background);
            FillTile(src[3], dib, side, half, half, rest, rest, background);
            break;
        }
        GdiFlush();
    } else {
        err = L"Could not create a bitmap for the collage.";
    }
    for (size_t i = 0; i < src.size(); ++i)
        stbi_image_free(src[i].p);
    return bmp;
}
