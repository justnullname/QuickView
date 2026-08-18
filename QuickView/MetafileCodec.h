#pragma once

// WMF / EMF (and EMF+ Dual) via GDI+ with true alpha and anti-aliasing.
// Office exports these as the Windows-native vector path into Word/Excel.

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shlwapi.h>
#include <algorithm>
#include <cstdint>
#include <cstring>

namespace QuickView::Metafile {

enum class Kind { None, Emf, Wmf };

inline constexpr uint32_t kAldusKey = 0x9AC6CDD7u;      // placeable WMF
inline constexpr uint32_t kEmfSignature = 0x464D4520u; // " EMF"
inline constexpr int kHimetricPerInch = 2540;
inline constexpr int kMaxEdge = 8192;
inline constexpr int kRasterScale = 2;

inline void EnsureGdiplusInitialized() {
    struct GdiplusLifecycle {
        ULONG_PTR token = 0;
        GdiplusLifecycle() {
            Gdiplus::GdiplusStartupInput input;
            Gdiplus::GdiplusStartup(&token, &input, nullptr);
        }
        ~GdiplusLifecycle() {
            if (token) {
                Gdiplus::GdiplusShutdown(token);
            }
        }
    };
    static GdiplusLifecycle s_gdiplusLifecycle;
}

inline uint16_t ReadLE16(const uint8_t *p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

inline uint32_t ReadLE32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

inline Kind Detect(const uint8_t *data, size_t size) {
    if (!data) return Kind::None;
    if (size >= 44 && ReadLE32(data) == 1 && ReadLE32(data + 40) == kEmfSignature) {
        return Kind::Emf;
    }
    if (size >= 22 && ReadLE32(data) == kAldusKey) {
        return Kind::Wmf;
    }
    if (size >= 18) {
        const uint16_t type = ReadLE16(data);
        const uint16_t headerSize = ReadLE16(data + 2);
        const uint16_t version = ReadLE16(data + 4);
        if ((type == 1 || type == 2) && headerSize >= 9 &&
            (version == 0x0100 || version == 0x0300)) {
            return Kind::Wmf;
        }
    }
    return Kind::None;
}

inline const wchar_t *KindName(Kind k) {
    switch (k) {
    case Kind::Emf: return L"EMF";
    case Kind::Wmf: return L"WMF";
    default: return L"Unknown";
    }
}

struct LogicalSize {
    int width = 0;
    int height = 0;
    bool valid = false;
};

inline LogicalSize MeasureHeader(const uint8_t *data, size_t size, int dpiX = 96, int dpiY = 96) {
    LogicalSize out;
    if (dpiX <= 0) dpiX = 96;
    if (dpiY <= 0) dpiY = 96;

    const Kind k = Detect(data, size);
    if (k == Kind::Emf && size >= 40) {
        const int32_t fl = static_cast<int32_t>(ReadLE32(data + 24));
        const int32_t ft = static_cast<int32_t>(ReadLE32(data + 28));
        const int32_t fr = static_cast<int32_t>(ReadLE32(data + 32));
        const int32_t fb = static_cast<int32_t>(ReadLE32(data + 36));
        const int frameW = fr - fl;
        const int frameH = fb - ft;
        if (frameW > 0 && frameH > 0) {
            out.width = MulDiv(frameW, dpiX, kHimetricPerInch);
            out.height = MulDiv(frameH, dpiY, kHimetricPerInch);
        } else if (size >= 24) {
            const int32_t bl = static_cast<int32_t>(ReadLE32(data + 8));
            const int32_t bt = static_cast<int32_t>(ReadLE32(data + 12));
            const int32_t br = static_cast<int32_t>(ReadLE32(data + 16));
            const int32_t bb = static_cast<int32_t>(ReadLE32(data + 20));
            out.width = br - bl;
            out.height = bb - bt;
        }
    } else if (k == Kind::Wmf && size >= 22 && ReadLE32(data) == kAldusKey) {
        const int16_t l = static_cast<int16_t>(ReadLE16(data + 6));
        const int16_t t = static_cast<int16_t>(ReadLE16(data + 8));
        const int16_t r = static_cast<int16_t>(ReadLE16(data + 10));
        const int16_t b = static_cast<int16_t>(ReadLE16(data + 12));
        uint16_t inch = ReadLE16(data + 14);
        if (inch == 0) inch = 1440;
        out.width = MulDiv(std::abs(r - l), dpiX, inch);
        out.height = MulDiv(std::abs(b - t), dpiY, inch);
    }

    if (out.width < 1) out.width = 1;
    if (out.height < 1) out.height = 1;
    // Bare WMF (no Aldus header) has no reliable size — caller must use GDI.
    if (k == Kind::Wmf && ReadLE32(data) != kAldusKey) {
        out.width = 0;
        out.height = 0;
        out.valid = false;
        return out;
    }
    out.valid = (k != Kind::None && out.width > 0 && out.height > 0);
    return out;
}

inline HENHMETAFILE OpenHemf(const uint8_t *data, size_t size) {
    if (!data || size == 0) return nullptr;
    const Kind k = Detect(data, size);
    if (k == Kind::Emf) {
        return SetEnhMetaFileBits(static_cast<UINT>(size), data);
    }
    if (k == Kind::Wmf && size >= 22 && ReadLE32(data) == kAldusKey) {
        const int16_t l = static_cast<int16_t>(ReadLE16(data + 6));
        const int16_t t = static_cast<int16_t>(ReadLE16(data + 8));
        const int16_t r = static_cast<int16_t>(ReadLE16(data + 10));
        const int16_t b = static_cast<int16_t>(ReadLE16(data + 12));
        uint16_t inch = ReadLE16(data + 14);
        if (inch == 0) inch = 1440;
        METAFILEPICT mfp{};
        mfp.mm = MM_ANISOTROPIC;
        mfp.xExt = MulDiv(std::abs(r - l), kHimetricPerInch, inch);
        mfp.yExt = MulDiv(std::abs(b - t), kHimetricPerInch, inch);
        if (mfp.xExt <= 0) mfp.xExt = kHimetricPerInch;
        if (mfp.yExt <= 0) mfp.yExt = kHimetricPerInch;
        return SetWinMetaFileBits(static_cast<UINT>(size - 22), data + 22, nullptr, &mfp);
    }
    if (k == Kind::Wmf) {
        return SetWinMetaFileBits(static_cast<UINT>(size), data, nullptr, nullptr);
    }
    return nullptr;
}

inline void ChooseRasterSize(int logicalW, int logicalH, int targetW, int targetH,
                             int &outW, int &outH) {
    if (logicalW < 1) logicalW = 1;
    if (logicalH < 1) logicalH = 1;

    int w = logicalW * kRasterScale;
    int h = logicalH * kRasterScale;
    if (w > kMaxEdge || h > kMaxEdge) {
        const float s = (std::min)(static_cast<float>(kMaxEdge) / w,
                                   static_cast<float>(kMaxEdge) / h);
        w = (std::max)(1, static_cast<int>(w * s));
        h = (std::max)(1, static_cast<int>(h * s));
    }

    if (targetW > 0 && targetH > 0) {
        const float s = (std::min)(static_cast<float>(targetW) / w,
                                   static_cast<float>(targetH) / h);
        if (s < 1.0f) {
            w = (std::max)(1, static_cast<int>(w * s + 0.5f));
            h = (std::max)(1, static_cast<int>(h * s + 0.5f));
        }
    }
    outW = w;
    outH = h;
}

inline bool RasterizeGdiplus(Gdiplus::Metafile &metafile, int width, int height,
                             uint8_t *dst, int stride, bool *outHasAlpha = nullptr) {
    if (metafile.GetLastStatus() != Gdiplus::Ok || width < 1 || height < 1 || !dst || stride < width * 4) {
        return false;
    }

    const Gdiplus::Rect destRect(0, 0, width, height);

    // 1. Render on Black Background (Alpha=255, RGB=0,0,0)
    Gdiplus::Bitmap bmpBlack(width, height, PixelFormat32bppPARGB);
    if (bmpBlack.GetLastStatus() != Gdiplus::Ok) return false;
    {
        Gdiplus::Graphics gBlack(&bmpBlack);
        if (gBlack.GetLastStatus() != Gdiplus::Ok) return false;
        gBlack.Clear(Gdiplus::Color(255, 0, 0, 0));
        gBlack.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
        gBlack.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        gBlack.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        gBlack.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        gBlack.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
        if (gBlack.DrawImage(&metafile, destRect) != Gdiplus::Ok) return false;
    }

    // 2. Render on White Background (Alpha=255, RGB=255,255,255)
    Gdiplus::Bitmap bmpWhite(width, height, PixelFormat32bppPARGB);
    if (bmpWhite.GetLastStatus() != Gdiplus::Ok) return false;
    {
        Gdiplus::Graphics gWhite(&bmpWhite);
        if (gWhite.GetLastStatus() != Gdiplus::Ok) return false;
        gWhite.Clear(Gdiplus::Color(255, 255, 255, 255));
        gWhite.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
        gWhite.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        gWhite.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        gWhite.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        gWhite.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
        if (gWhite.DrawImage(&metafile, destRect) != Gdiplus::Ok) return false;
    }

    // 3. Extract pixels and reconstruct mathematical alpha channel with anti-aliasing
    Gdiplus::BitmapData dataBlack{};
    Gdiplus::BitmapData dataWhite{};
    const Gdiplus::Rect lockRect(0, 0, width, height);
    if (bmpBlack.LockBits(&lockRect, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &dataBlack) != Gdiplus::Ok) {
        return false;
    }
    if (bmpWhite.LockBits(&lockRect, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &dataWhite) != Gdiplus::Ok) {
        bmpBlack.UnlockBits(&dataBlack);
        return false;
    }

    const uint8_t *srcBlack = static_cast<const uint8_t *>(dataBlack.Scan0);
    const uint8_t *srcWhite = static_cast<const uint8_t *>(dataWhite.Scan0);
    const int strideBlack = dataBlack.Stride;
    const int strideWhite = dataWhite.Stride;
    bool hasAlpha = false;

    for (int y = 0; y < height; ++y) {
        const uint8_t *rowB = srcBlack + static_cast<size_t>(y) * strideBlack;
        const uint8_t *rowW = srcWhite + static_cast<size_t>(y) * strideWhite;
        uint8_t *rowDst = dst + static_cast<size_t>(y) * stride;

        for (int x = 0; x < width; ++x) {
            const int b0 = rowB[x * 4 + 0];
            const int g0 = rowB[x * 4 + 1];
            const int r0 = rowB[x * 4 + 2];

            const int b1 = rowW[x * 4 + 0];
            const int g1 = rowW[x * 4 + 1];
            const int r1 = rowW[x * 4 + 2];

            const int diffB = b1 - b0;
            const int diffG = g1 - g0;
            const int diffR = r1 - r0;
            const int maxDiff = (std::max)({diffB, diffG, diffR, 0});
            const int alphaVal = (std::clamp)(255 - maxDiff, 0, 255);

            rowDst[x * 4 + 0] = static_cast<uint8_t>(b0);
            rowDst[x * 4 + 1] = static_cast<uint8_t>(g0);
            rowDst[x * 4 + 2] = static_cast<uint8_t>(r0);
            rowDst[x * 4 + 3] = static_cast<uint8_t>(alphaVal);

            if (alphaVal < 255) {
                hasAlpha = true;
            }
        }
    }

    bmpWhite.UnlockBits(&dataWhite);
    bmpBlack.UnlockBits(&dataBlack);

    if (outHasAlpha) {
        *outHasAlpha = hasAlpha;
    }
    return true;
}

inline bool RasterizeFallbackGdi(HENHMETAFILE hemf, int width, int height,
                                 uint8_t *dst, int stride, bool *outHasAlpha = nullptr) {
    if (!hemf || !dst || width < 1 || height < 1 || stride < width * 4) {
        return false;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void *bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!mem || !dib || !bits) {
        if (dib) DeleteObject(dib);
        if (mem) DeleteDC(mem);
        if (screen) ReleaseDC(nullptr, screen);
        return false;
    }

    HGDIOBJ old = SelectObject(mem, dib);
    RECT rc{0, 0, width, height};
    FillRect(mem, &rc, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    SetBkMode(mem, TRANSPARENT);
    const BOOL ok = PlayEnhMetaFile(mem, hemf, &rc);
    SelectObject(mem, old);
    DeleteDC(mem);
    if (screen) ReleaseDC(nullptr, screen);

    if (!ok) {
        DeleteObject(dib);
        return false;
    }

    const int srcStride = width * 4;
    const uint8_t *src = static_cast<const uint8_t *>(bits);
    for (int y = 0; y < height; ++y) {
        uint8_t *row = dst + static_cast<size_t>(y) * stride;
        std::memcpy(row, src + static_cast<size_t>(y) * srcStride, static_cast<size_t>(srcStride));
        for (int x = 0; x < width; ++x) {
            row[x * 4 + 3] = 255;
        }
    }
    DeleteObject(dib);
    if (outHasAlpha) {
        *outHasAlpha = false;
    }
    return true;
}

inline bool Rasterize(HENHMETAFILE hemf, int width, int height,
                      uint8_t *dst, int stride, bool *outHasAlpha = nullptr) {
    if (!hemf || !dst || width < 1 || height < 1 || stride < width * 4) {
        return false;
    }

    EnsureGdiplusInitialized();

    // 1. Try high-quality GDI+ vector rasterization with anti-aliasing and true alpha
    {
        Gdiplus::Metafile metafile(hemf, FALSE);
        if (metafile.GetLastStatus() == Gdiplus::Ok) {
            if (RasterizeGdiplus(metafile, width, height, dst, stride, outHasAlpha)) {
                return true;
            }
        }
    }

    // 2. Fallback to legacy GDI PlayEnhMetaFile
    return RasterizeFallbackGdi(hemf, width, height, dst, stride, outHasAlpha);
}

inline bool RasterizeBuffer(const uint8_t *data, size_t size, int width, int height,
                            uint8_t *dst, int stride, bool *outHasAlpha = nullptr) {
    if (!data || size == 0 || !dst || width < 1 || height < 1 || stride < width * 4) {
        return false;
    }

    EnsureGdiplusInitialized();

    // 1. Try GDI+ directly from memory stream (preserves native EMF+ dual/plus records)
    IStream *stream = SHCreateMemStream(data, static_cast<UINT>(size));
    if (stream) {
        Gdiplus::Metafile metafile(stream);
        stream->Release();
        if (metafile.GetLastStatus() == Gdiplus::Ok) {
            if (RasterizeGdiplus(metafile, width, height, dst, stride, outHasAlpha)) {
                return true;
            }
        }
    }

    // 2. Fallback via OpenHemf
    HENHMETAFILE hemf = OpenHemf(data, size);
    if (hemf) {
        const bool ok = Rasterize(hemf, width, height, dst, stride, outHasAlpha);
        DeleteEnhMetaFile(hemf);
        return ok;
    }
    return false;
}

inline bool MeasureViaGdi(HENHMETAFILE hemf, int dpiX, int dpiY, int &outW, int &outH) {
    if (!hemf) return false;
    if (dpiX <= 0) dpiX = 96;
    if (dpiY <= 0) dpiY = 96;
    ENHMETAHEADER hdr{};
    if (GetEnhMetaFileHeader(hemf, sizeof(hdr), &hdr) == 0) return false;

    const int frameW = hdr.rclFrame.right - hdr.rclFrame.left;
    const int frameH = hdr.rclFrame.bottom - hdr.rclFrame.top;
    if (frameW > 0 && frameH > 0) {
        outW = MulDiv(frameW, dpiX, kHimetricPerInch);
        outH = MulDiv(frameH, dpiY, kHimetricPerInch);
    } else {
        outW = hdr.rclBounds.right - hdr.rclBounds.left;
        outH = hdr.rclBounds.bottom - hdr.rclBounds.top;
    }
    if (outW < 1) outW = 1;
    if (outH < 1) outH = 1;
    return true;
}

} // namespace QuickView::Metafile
