#include "pch.h"
#include <gtest/gtest.h>
#include "MetafileCodec.h"

#include <vector>

using namespace QuickView::Metafile;

// Placeable WMF: Aldus header + a 9-word standard header (no records needed for Detect).
static std::vector<uint8_t> MakePlaceableWmfHeader() {
    std::vector<uint8_t> b(22 + 18, 0);
    // key
    b[0] = 0xD7; b[1] = 0xCD; b[2] = 0xC6; b[3] = 0x9A;
    // bbox in twips: 0,0,1440,1440 (1 inch square at 1440 twips/inch)
    b[10] = 0xA0; b[11] = 0x05; // 1440
    b[12] = 0xA0; b[13] = 0x05;
    b[14] = 0xA0; b[15] = 0x05; // inch = 1440
    // standard WMF header after 22 bytes
    b[22] = 1; // type memory
    b[24] = 9; // header size in words
    b[26] = 0x00; b[27] = 0x03; // version 0x0300
    return b;
}

static std::vector<uint8_t> MakeEmfHeader(int frameWHimetric, int frameHHimetric) {
    std::vector<uint8_t> b(88, 0);
    b[0] = 1; // EMR_HEADER
    b[4] = 88; // nSize
    // rclFrame at 24
    auto putI32 = [&](size_t off, int32_t v) {
        b[off] = static_cast<uint8_t>(v);
        b[off + 1] = static_cast<uint8_t>(v >> 8);
        b[off + 2] = static_cast<uint8_t>(v >> 16);
        b[off + 3] = static_cast<uint8_t>(v >> 24);
    };
    putI32(32, frameWHimetric);
    putI32(36, frameHHimetric);
    putI32(40, static_cast<int32_t>(kEmfSignature));
    return b;
}

TEST(MetafileCodec, DetectEmfBySignature) {
    auto emf = MakeEmfHeader(2540, 2540);
    EXPECT_EQ(Detect(emf.data(), emf.size()), Kind::Emf);
}

TEST(MetafileCodec, DetectPlaceableWmf) {
    auto wmf = MakePlaceableWmfHeader();
    EXPECT_EQ(Detect(wmf.data(), wmf.size()), Kind::Wmf);
}

TEST(MetafileCodec, DetectRejectsTooSmall) {
    uint8_t tiny[] = {1, 0, 0, 0};
    EXPECT_EQ(Detect(tiny, sizeof(tiny)), Kind::None);
    EXPECT_EQ(Detect(nullptr, 0), Kind::None);
}

TEST(MetafileCodec, MeasureEmfHimetricToPixels) {
    // 1 inch x 2 inch at 96 dpi
    auto emf = MakeEmfHeader(2540, 5080);
    auto sz = MeasureHeader(emf.data(), emf.size(), 96, 96);
    EXPECT_TRUE(sz.valid);
    EXPECT_EQ(sz.width, 96);
    EXPECT_EQ(sz.height, 192);
}

TEST(MetafileCodec, MeasurePlaceableWmfTwipsToPixels) {
    auto wmf = MakePlaceableWmfHeader();
    auto sz = MeasureHeader(wmf.data(), wmf.size(), 96, 96);
    EXPECT_TRUE(sz.valid);
    EXPECT_EQ(sz.width, 96);
    EXPECT_EQ(sz.height, 96);
}

TEST(MetafileCodec, RasterizeGdiCreatedEmf) {
    HDC screen = GetDC(nullptr);
    HDC meta = CreateEnhMetaFileW(screen, nullptr, nullptr, L"QuickView test\0");
    ASSERT_TRUE(meta != nullptr);
    RECT rc{0, 0, 100, 50};
    HBRUSH brush = CreateSolidBrush(RGB(255, 0, 0));
    FillRect(meta, &rc, brush);
    DeleteObject(brush);
    HENHMETAFILE hemf = CloseEnhMetaFile(meta);
    ReleaseDC(nullptr, screen);
    ASSERT_TRUE(hemf != nullptr);

    int logicalW = 0, logicalH = 0;
    ASSERT_TRUE(MeasureViaGdi(hemf, 96, 96, logicalW, logicalH));
    EXPECT_GT(logicalW, 0);
    EXPECT_GT(logicalH, 0);

    int w = 0, h = 0;
    ChooseRasterSize(logicalW, logicalH, 0, 0, w, h);
    std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4, 0);
    ASSERT_TRUE(Rasterize(hemf, w, h, pixels.data(), w * 4));
    DeleteEnhMetaFile(hemf);

    // At least one pixel should be the red fill (B=0, G=0, R=255, A=255)
    bool foundRed = false;
    for (int i = 0; i < w * h; ++i) {
        if (pixels[i * 4 + 0] == 0 && pixels[i * 4 + 1] == 0 &&
            pixels[i * 4 + 2] == 255 && pixels[i * 4 + 3] == 255) {
            foundRed = true;
            break;
        }
    }
    EXPECT_TRUE(foundRed);
}

TEST(MetafileCodec, RasterizeTransparentEmfPreservesAlpha) {
    HDC screen = GetDC(nullptr);
    // 100x100 pixels at 96 DPI in HIMETRIC (0.01 mm): 100 * 2540 / 96 = 2645
    RECT rclFrame{0, 0, 100 * 2540 / 96, 100 * 2540 / 96};
    HDC meta = CreateEnhMetaFileW(screen, nullptr, &rclFrame, L"QuickView transparent test\0");
    ASSERT_TRUE(meta != nullptr);
    // Draw a small red rectangle in the center (25..75), leaving background border untouched
    RECT rc{25, 25, 75, 75};
    HBRUSH brush = CreateSolidBrush(RGB(255, 0, 0));
    FillRect(meta, &rc, brush);
    DeleteObject(brush);
    HENHMETAFILE hemf = CloseEnhMetaFile(meta);
    ReleaseDC(nullptr, screen);
    ASSERT_TRUE(hemf != nullptr);

    int logicalW = 0, logicalH = 0;
    ASSERT_TRUE(MeasureViaGdi(hemf, 96, 96, logicalW, logicalH));

    int w = 0, h = 0;
    ChooseRasterSize(logicalW, logicalH, 0, 0, w, h);
    std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4, 0);
    bool hasAlpha = false;
    ASSERT_TRUE(Rasterize(hemf, w, h, pixels.data(), w * 4, &hasAlpha));
    DeleteEnhMetaFile(hemf);

    // Should detect transparency
    EXPECT_TRUE(hasAlpha);

    // Corner pixel (0,0) must be completely transparent (A = 0)
    EXPECT_EQ(pixels[3], 0);

    // Center pixel must have alpha > 0 and red color
    const int midX = w / 2;
    const int midY = h / 2;
    const size_t midIdx = (static_cast<size_t>(midY) * w + midX) * 4;
    EXPECT_EQ(pixels[midIdx + 0], 0);   // B
    EXPECT_EQ(pixels[midIdx + 1], 0);   // G
    EXPECT_EQ(pixels[midIdx + 2], 255); // R
    EXPECT_EQ(pixels[midIdx + 3], 255); // A
}

TEST(MetafileCodec, RasterizeOpaqueEmfHasOpaquePixels) {
    HDC screen = GetDC(nullptr);
    HDC meta = CreateEnhMetaFileW(screen, nullptr, nullptr, L"QuickView opaque test\0");
    ASSERT_TRUE(meta != nullptr);
    RECT rc{0, 0, 100, 100};
    HBRUSH brush = CreateSolidBrush(RGB(0, 128, 255));
    FillRect(meta, &rc, brush);
    DeleteObject(brush);
    HENHMETAFILE hemf = CloseEnhMetaFile(meta);
    ReleaseDC(nullptr, screen);
    ASSERT_TRUE(hemf != nullptr);

    int logicalW = 0, logicalH = 0;
    ASSERT_TRUE(MeasureViaGdi(hemf, 96, 96, logicalW, logicalH));

    int w = 0, h = 0;
    ChooseRasterSize(logicalW, logicalH, 0, 0, w, h);
    std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4, 0);
    bool hasAlpha = true;
    ASSERT_TRUE(Rasterize(hemf, w, h, pixels.data(), w * 4, &hasAlpha));
    DeleteEnhMetaFile(hemf);

    // Center region of opaque fill must be solid opaque (A=255) and blue
    const int midX = w / 2;
    const int midY = h / 2;
    const size_t midIdx = (static_cast<size_t>(midY) * w + midX) * 4;
    EXPECT_EQ(pixels[midIdx + 0], 255); // B
    EXPECT_EQ(pixels[midIdx + 1], 128); // G
    EXPECT_EQ(pixels[midIdx + 2], 0);   // R
    EXPECT_EQ(pixels[midIdx + 3], 255); // A
}

TEST(MetafileCodec, RasterizeEmfPlusAntiAliasing) {
    EnsureGdiplusInitialized();
    IStream *stream = SHCreateMemStream(nullptr, 0);
    ASSERT_TRUE(stream != nullptr);
    HDC screen = GetDC(nullptr);
    Gdiplus::RectF frame(0, 0, 100, 100);
    {
        Gdiplus::Metafile metaRecord(stream, screen, frame, Gdiplus::MetafileFrameUnitPixel, Gdiplus::EmfTypeEmfPlusDual);
        Gdiplus::Graphics g(&metaRecord);
        g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
        Gdiplus::SolidBrush brush(Gdiplus::Color(255, 0, 200, 0));
        g.FillEllipse(&brush, 10.0f, 10.0f, 80.0f, 80.0f);
    }
    ReleaseDC(nullptr, screen);

    STATSTG stat{};
    stream->Stat(&stat, STATFLAG_NONAME);
    const size_t sz = static_cast<size_t>(stat.cbSize.QuadPart);
    ASSERT_GT(sz, 0u);
    std::vector<uint8_t> buf(sz);
    LARGE_INTEGER zero{};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    ULONG readBytes = 0;
    stream->Read(buf.data(), static_cast<ULONG>(sz), &readBytes);
    stream->Release();

    int w = 200, h = 200;
    std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4, 0);
    bool hasAlpha = false;
    ASSERT_TRUE(RasterizeBuffer(buf.data(), buf.size(), w, h, pixels.data(), w * 4, &hasAlpha));
    EXPECT_TRUE(hasAlpha);

    // Verify that GDI+ anti-aliasing generated smooth subpixel edges (0 < Alpha < 255)
    bool foundAntiAliasedEdge = false;
    for (int i = 0; i < w * h; ++i) {
        uint8_t a = pixels[i * 4 + 3];
        if (a > 0 && a < 255) {
            foundAntiAliasedEdge = true;
            break;
        }
    }
    EXPECT_TRUE(foundAntiAliasedEdge);

    // Verify transparent corner
    EXPECT_EQ(pixels[3], 0);
}

TEST(MetafileCodec, RasterizeBufferMemoryStream) {
    HDC screen = GetDC(nullptr);
    RECT rclFrame{0, 0, 100 * 2540 / 96, 100 * 2540 / 96};
    HDC meta = CreateEnhMetaFileW(screen, nullptr, &rclFrame, L"QuickView buffer test\0");
    ASSERT_TRUE(meta != nullptr);
    RECT rc{10, 10, 90, 90};
    HBRUSH brush = CreateSolidBrush(RGB(200, 100, 50));
    FillRect(meta, &rc, brush);
    DeleteObject(brush);
    HENHMETAFILE hemf = CloseEnhMetaFile(meta);
    ReleaseDC(nullptr, screen);
    ASSERT_TRUE(hemf != nullptr);

    const UINT bytesLen = GetEnhMetaFileBits(hemf, 0, nullptr);
    ASSERT_GT(bytesLen, 0u);
    std::vector<uint8_t> buffer(bytesLen);
    GetEnhMetaFileBits(hemf, bytesLen, buffer.data());
    DeleteEnhMetaFile(hemf);

    int w = 200, h = 200;
    std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4, 0);
    bool hasAlpha = false;
    ASSERT_TRUE(RasterizeBuffer(buffer.data(), buffer.size(), w, h, pixels.data(), w * 4, &hasAlpha));
    EXPECT_TRUE(hasAlpha);
}
