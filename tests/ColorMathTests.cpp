#include <gtest/gtest.h>
#include "ColorMath.h"
#include "TileTypes.h"
#include "ImageLoaderSimd.h"
#include "WuffsLoader.h"
#include <zlib.h>
#include <cmath>
#include <cstring>
#include <vector>

// Basic tests for ColorMath pure functions

TEST(ColorMathTest, SrgbToLinear_Black) {
    EXPECT_FLOAT_EQ(ColorMath::SrgbToLinear(0.0f), 0.0f);
}

TEST(ColorMathTest, SrgbToLinear_White) {
    EXPECT_FLOAT_EQ(ColorMath::SrgbToLinear(1.0f), 1.0f);
}

TEST(ColorMathTest, SrgbToLinear_MidGray) {
    // 0.5 sRGB is approx 0.214 in linear
    float linear = ColorMath::SrgbToLinear(0.5f);
    EXPECT_NEAR(linear, 0.214f, 0.001f);
}

TEST(ColorMathTest, NormalizePrimaries) {
    EXPECT_EQ(ColorMath::NormalizePrimaries(QuickView::ColorPrimaries::Unknown), QuickView::ColorPrimaries::SRGB);
    EXPECT_EQ(ColorMath::NormalizePrimaries(QuickView::ColorPrimaries::DisplayP3), QuickView::ColorPrimaries::DisplayP3);
}

TEST(ColorMathTest, IsEffectivelyPixelArtMode) {
    // override wins
    EXPECT_TRUE(ColorMath::IsEffectivelyPixelArtMode(1.0f, 100, 100, 1, 0, 0));
    EXPECT_FALSE(ColorMath::IsEffectivelyPixelArtMode(1.0f, 100, 100, 2, 2, 2));

    // heuristics - auto mode (0), scale >= 3.0
    EXPECT_TRUE(ColorMath::IsEffectivelyPixelArtMode(4.0f, 1000, 1000, 0, 0, 0));
    EXPECT_FALSE(ColorMath::IsEffectivelyPixelArtMode(2.0f, 1000, 1000, 0, 0, 0));

    // heuristics - small original image
    EXPECT_TRUE(ColorMath::IsEffectivelyPixelArtMode(2.0f, 128, 128, 0, 0, 0));

    // Content-Aware: Large 1080p pixel art image with low Shannon Entropy (e.g. 4.5 <= 5.8)
    EXPECT_TRUE(ColorMath::IsEffectivelyPixelArtMode(1.5f, 1920, 1080, 0, 0, 0, 4.5, true));
    // High entropy normal photo (e.g. 7.2 > 5.8) should NOT be pixel art at 1.5x
    EXPECT_FALSE(ColorMath::IsEffectivelyPixelArtMode(1.5f, 1920, 1080, 0, 0, 0, 7.2, true));
}

TEST(ColorMathTest, GetAdaptiveFsrSharpness) {
    const float base = 0.20f;

    // 1. Fallback when no metrics available
    EXPECT_FLOAT_EQ(ColorMath::GetAdaptiveFsrSharpness(base, 0.0, 0.0, false), base);

    // 2. High ISO / Grainy Noisy Image (Sharpness >= 800 && Entropy >= 7.3) -> Suppress
    float noisySharp = ColorMath::GetAdaptiveFsrSharpness(base, 1200.0, 7.6, true);
    EXPECT_LE(noisySharp, 0.05f);

    // 3. Soft / Slightly blurry image (Sharpness <= 120 && Entropy >= 6.0) -> Boost
    float softSharp = ColorMath::GetAdaptiveFsrSharpness(base, 80.0, 6.5, true);
    EXPECT_GT(softSharp, base);
    EXPECT_LE(softSharp, 0.40f);

    // 4. Standard clean photograph (Sharpness ~ 450, Entropy ~ 7.1) -> Maintain base
    float cleanSharp = ColorMath::GetAdaptiveFsrSharpness(base, 450.0, 7.1, true);
    EXPECT_FLOAT_EQ(cleanSharp, base);
}

TEST(ColorMathTest, MatrixMultiplication) {
    ColorMath::ColorMatrix3 a = {{{1, 2, 3}, {4, 5, 6}, {7, 8, 9}}};
    ColorMath::ColorMatrix3 b = {{{9, 8, 7}, {6, 5, 4}, {3, 2, 1}}};
    
    ColorMath::ColorMatrix3 c = ColorMath::MultiplyColorMatrices(a, b);
    
    EXPECT_FLOAT_EQ(c.m[0][0], 30);
    EXPECT_FLOAT_EQ(c.m[0][1], 24);
    EXPECT_FLOAT_EQ(c.m[0][2], 18);
    EXPECT_FLOAT_EQ(c.m[1][0], 84);
    EXPECT_FLOAT_EQ(c.m[2][2], 90);
}

TEST(ColorMathTest, ComputeBoundedBitmapSurfaceSize) {
    // 1. Non-Titan tall screenshot (2549x6510) fits within 16.77MP budget and 8192 axis cap -> full 1:1
    auto tall = ColorMath::ComputeBoundedBitmapSurfaceSize(2549.0f, 6510.0f, 1.0f);
    EXPECT_EQ(tall.width, 2549u);
    EXPECT_EQ(tall.height, 6510u);

    // 2. Downsampled fit scale for the same tall image preserves target viewport resolution
    auto downsampled = ColorMath::ComputeBoundedBitmapSurfaceSize(2549.0f, 6510.0f, 0.2f);
    EXPECT_EQ(downsampled.width, 510u);
    EXPECT_EQ(downsampled.height, 1302u);

    // 3. Large square image exceeding 4096*4096 pixel budget scales proportionally
    auto oversizedSquare = ColorMath::ComputeBoundedBitmapSurfaceSize(8000.0f, 8000.0f, 1.0f);
    EXPECT_EQ(oversizedSquare.width, 4096u);
    EXPECT_EQ(oversizedSquare.height, 4096u);

    // 4. Ultra-wide or tall image within pixel budget up to maxAxis (8192) remains 1:1
    auto tallStrip = ColorMath::ComputeBoundedBitmapSurfaceSize(1000.0f, 8192.0f, 1.0f);
    EXPECT_EQ(tallStrip.width, 1000u);
    EXPECT_EQ(tallStrip.height, 8192u);

    // 5. Image upscaled past maxAxis clamps cleanly to maxAxis while preserving aspect ratio
    auto srUpscaled = ColorMath::ComputeBoundedBitmapSurfaceSize(5000.0f, 2000.0f, 2.0f);
    EXPECT_LE(srUpscaled.width, 8192u);
    EXPECT_LE(srUpscaled.height, 8192u);
    EXPECT_LE(static_cast<uint64_t>(srUpscaled.width) * srUpscaled.height, 4096ull * 4096ull + 4096ull);

    // 6. Invalid inputs return zero
    auto invalidZero = ColorMath::ComputeBoundedBitmapSurfaceSize(0.0f, 100.0f, 1.0f);
    EXPECT_EQ(invalidZero.width, 0u);
    EXPECT_EQ(invalidZero.height, 0u);

    // 7. FL11+ 16384 axis cap preserves 1:1 strip dimensions within 16.77MP budget
    auto strip16kW = ColorMath::ComputeBoundedBitmapSurfaceSize(8193.0f, 100.0f, 1.0f, 16384u);
    EXPECT_EQ(strip16kW.width, 8193u);
    EXPECT_EQ(strip16kW.height, 100u);

    auto strip16kH = ColorMath::ComputeBoundedBitmapSurfaceSize(1080.0f, 15000.0f, 1.0f, 16384u);
    EXPECT_EQ(strip16kH.width, 1080u);
    EXPECT_EQ(strip16kH.height, 15000u);
}

TEST(TitanPolicyTest, ShouldTriggerTitanByDimensions) {
    using QuickView::ShouldTriggerTitanByDimensions;
    using QuickView::kDefaultStandardAxisLimit;
    using QuickView::kMaxProbeStandardAxisLimit;

    // Issue #308 regression: 8193x100 (0.82MP) runs Standard Mode on FL11+ (16384) and Titan on FL10 (8192)
    EXPECT_FALSE(ShouldTriggerTitanByDimensions(8193u, 100u, kMaxProbeStandardAxisLimit));
    EXPECT_TRUE(ShouldTriggerTitanByDimensions(8193u, 100u, kDefaultStandardAxisLimit));

    // Long phone screenshot (1080x15000 = 16.2MP <= 16.77MP budget) stays Standard on FL11+
    EXPECT_FALSE(ShouldTriggerTitanByDimensions(1080u, 15000u, kMaxProbeStandardAxisLimit));
    EXPECT_TRUE(ShouldTriggerTitanByDimensions(1080u, 15000u, kDefaultStandardAxisLimit));

    // Medium-giant image (4000x12000 = 48MP > 16.77MP budget) must still enter Titan to avoid 16.77MP downsampling blur
    EXPECT_TRUE(ShouldTriggerTitanByDimensions(4000u, 12000u, kMaxProbeStandardAxisLimit));

    // Extreme strip (> 16384) always triggers Titan regardless of small pixel count
    EXPECT_TRUE(ShouldTriggerTitanByDimensions(20000u, 100u, kMaxProbeStandardAxisLimit));
    EXPECT_TRUE(ShouldTriggerTitanByDimensions(100u, 20000u, kMaxProbeStandardAxisLimit));

    // Pixel trigger (> 50MP) triggers Titan even if both sides <= 8192
    EXPECT_TRUE(ShouldTriggerTitanByDimensions(8192u, 8192u, kMaxProbeStandardAxisLimit));

    // Normal mid-size image <= 8192 & <= 50MP stays Standard
    EXPECT_FALSE(ShouldTriggerTitanByDimensions(6000u, 6000u, kMaxProbeStandardAxisLimit));
    EXPECT_FALSE(ShouldTriggerTitanByDimensions(6000u, 6000u, kDefaultStandardAxisLimit));

    // Zero dimensions do not trigger
    EXPECT_FALSE(ShouldTriggerTitanByDimensions(0u, 100u, kMaxProbeStandardAxisLimit));
    EXPECT_FALSE(ShouldTriggerTitanByDimensions(100u, 0u, kMaxProbeStandardAxisLimit));
}

TEST(TitanPolicyTest, ShouldWarmupMasterBackingForTitan) {
    using QuickView::ShouldWarmupMasterBackingForTitan;
    using QuickView::TitanFormat;

    // Baseline JPEG <= 128 MB BGRA (~32 MP) warms up MasterBackingStore for O(1) RAM slicing
    EXPECT_TRUE(ShouldWarmupMasterBackingForTitan(25000, 100, TitanFormat::JPEG, false));
    EXPECT_TRUE(ShouldWarmupMasterBackingForTitan(25000, 8, TitanFormat::JPEG, false));
    EXPECT_TRUE(ShouldWarmupMasterBackingForTitan(16384, 2048, TitanFormat::JPEG, false));  // exact 128 MB
    EXPECT_FALSE(ShouldWarmupMasterBackingForTitan(16384, 2049, TitanFormat::JPEG, false)); // > 128 MB
    EXPECT_FALSE(ShouldWarmupMasterBackingForTitan(20000, 10000, TitanFormat::JPEG, false)); // 800 MB

    // Progressive JPEG keeps MasterBackingStore warmup up to 1.5 GiB (ROI reparses full scans)
    EXPECT_TRUE(ShouldWarmupMasterBackingForTitan(20000, 10000, TitanFormat::JPEG, true));  // 800 MB <= 1.5 GiB
    EXPECT_FALSE(ShouldWarmupMasterBackingForTitan(21000, 20000, TitanFormat::JPEG, true)); // 1.68 GB > 1.5 GiB

    // Tier 3 (> 1.5 GiB) warms up only non-ROI formats (PNG, TIFF, AVIF, JXL)
    EXPECT_TRUE(ShouldWarmupMasterBackingForTitan(21000, 20000, TitanFormat::PNG, false));
    EXPECT_FALSE(ShouldWarmupMasterBackingForTitan(21000, 20000, TitanFormat::WEBP, false));

    // Images < 8000 on both dimensions never trigger MasterBackingStore warmup
    EXPECT_FALSE(ShouldWarmupMasterBackingForTitan(4000, 4000, TitanFormat::PNG, false));
}

TEST(TitanPolicyTest, ComputeTitanBaseLayerSize) {
    using QuickView::ComputeTitanBaseLayerSize;

    int w = 0, h = 0;

    // Extreme horizontal strip never rounds height down to 0
    ComputeTitanBaseLayerSize(25000, 8, 1920, 1080, w, h);
    EXPECT_EQ(w, 1920);
    EXPECT_EQ(h, 1);

    // Extreme vertical strip never rounds width down to 0
    ComputeTitanBaseLayerSize(8, 25000, 1920, 1080, w, h);
    EXPECT_EQ(w, 1);
    EXPECT_EQ(h, 1080);

    // Thin strip preserves aspect ratio without 8-pixel rounding bloat (4 px instead of 8 px)
    ComputeTitanBaseLayerSize(25000, 50, 1920, 1080, w, h);
    EXPECT_EQ(w, 1920);
    EXPECT_EQ(h, 4);

    // Moderate strip preserves exact proportion
    ComputeTitanBaseLayerSize(20000, 100, 1920, 1080, w, h);
    EXPECT_EQ(w, 1920);
    EXPECT_EQ(h, 10);

    // Standard Titan landscape image
    ComputeTitanBaseLayerSize(12000, 9000, 1920, 1080, w, h);
    EXPECT_EQ(w, 1440);
    EXPECT_EQ(h, 1080);

    // Smaller than screen does not upscale
    ComputeTitanBaseLayerSize(1000, 500, 1920, 1080, w, h);
    EXPECT_EQ(w, 1000);
    EXPECT_EQ(h, 500);
}

TEST(ImageLoaderSimdTest, ConvertRGB16ToBGRARow_LEAndBE) {
    constexpr int kWidth = 37; // 32-lane AVX2 body + 5-pixel scalar tail
    std::vector<uint8_t> srcLE(kWidth * 6);
    std::vector<uint8_t> srcBE(kWidth * 6);
    std::vector<uint8_t> dstLE(kWidth * 4, 0);
    std::vector<uint8_t> dstBE(kWidth * 4, 0);

    for (int x = 0; x < kWidth; ++x) {
        const uint8_t rHi = static_cast<uint8_t>(10 + x * 5);
        const uint8_t gHi = static_cast<uint8_t>(20 + x * 3);
        const uint8_t bHi = static_cast<uint8_t>(30 + x * 2);
        // Little-endian: [lo, hi]
        srcLE[x * 6 + 0] = 0xAA; srcLE[x * 6 + 1] = rHi;
        srcLE[x * 6 + 2] = 0xBB; srcLE[x * 6 + 3] = gHi;
        srcLE[x * 6 + 4] = 0xCC; srcLE[x * 6 + 5] = bHi;
        // Big-endian: [hi, lo]
        srcBE[x * 6 + 0] = rHi; srcBE[x * 6 + 1] = 0xAA;
        srcBE[x * 6 + 2] = gHi; srcBE[x * 6 + 3] = 0xBB;
        srcBE[x * 6 + 4] = bHi; srcBE[x * 6 + 5] = 0xCC;
    }

    ImageLoaderSimd::ConvertRGB16ToBGRARow(srcLE.data(), dstLE.data(), kWidth, true);
    ImageLoaderSimd::ConvertRGB16ToBGRARow(srcBE.data(), dstBE.data(), kWidth, false);

    for (int x = 0; x < kWidth; ++x) {
        const uint8_t rHi = static_cast<uint8_t>(10 + x * 5);
        const uint8_t gHi = static_cast<uint8_t>(20 + x * 3);
        const uint8_t bHi = static_cast<uint8_t>(30 + x * 2);
        EXPECT_EQ(dstLE[x * 4 + 0], bHi);
        EXPECT_EQ(dstLE[x * 4 + 1], gHi);
        EXPECT_EQ(dstLE[x * 4 + 2], rHi);
        EXPECT_EQ(dstLE[x * 4 + 3], 255);

        EXPECT_EQ(dstBE[x * 4 + 0], bHi);
        EXPECT_EQ(dstBE[x * 4 + 1], gHi);
        EXPECT_EQ(dstBE[x * 4 + 2], rHi);
        EXPECT_EQ(dstBE[x * 4 + 3], 255);
    }
}

TEST(ImageLoaderSimdTest, ResizeBilinear_MultiThreadGradientAndSmall) {
    // Large enough to trigger multi-threaded SIMD row dispatch (dstH >= 128 && pixels >= 262144)
    constexpr int kSrcW = 1024, kSrcH = 1024;
    constexpr int kDstW = 600, kDstH = 600;
    std::vector<uint8_t> src(static_cast<size_t>(kSrcW) * kSrcH * 4);
    for (int y = 0; y < kSrcH; ++y) {
        for (int x = 0; x < kSrcW; ++x) {
            const size_t off = (static_cast<size_t>(y) * kSrcW + x) * 4;
            src[off + 0] = static_cast<uint8_t>(x >> 2);         // B horizontal ramp
            src[off + 1] = static_cast<uint8_t>(y >> 2);         // G vertical ramp
            src[off + 2] = 0x7F;                                 // R constant
            src[off + 3] = 0xFF;                                 // A constant
        }
    }

    std::vector<uint8_t> dst(static_cast<size_t>(kDstW) * kDstH * 4, 0);
    ImageLoaderSimd::ResizeBilinear(src.data(), kSrcW, kSrcH, kSrcW * 4,
                                    dst.data(), kDstW, kDstH, kDstW * 4);

    // Corner checks + constant channel preservation across all worker slices
    EXPECT_EQ(dst[0], 0);
    EXPECT_EQ(dst[1], 0);
    EXPECT_EQ(dst[2], 0x7F);
    EXPECT_EQ(dst[3], 0xFF);
    const size_t lastOff = (static_cast<size_t>(kDstH - 1) * kDstW + (kDstW - 1)) * 4;
    EXPECT_EQ(dst[lastOff + 0], 255);
    EXPECT_EQ(dst[lastOff + 1], 255);
    EXPECT_EQ(dst[lastOff + 2], 0x7F);
    EXPECT_EQ(dst[lastOff + 3], 0xFF);
}

TEST(ImageLoaderSimdTest, PngUnfilterRow_AllFiltersAndBpps) {
    auto scalarPaeth = [](int a, int b, int c) -> uint8_t {
        const int pa = std::abs(b - c);
        const int pb = std::abs(a - c);
        const int pc = std::abs((a - c) + (b - c));
        return static_cast<uint8_t>((pa <= pb && pa <= pc) ? a : ((pb <= pc) ? b : c));
    };

    const int bpps[] = {1, 2, 3, 4, 6, 8};
    const int pixelCounts[] = {1, 2, 3, 7, 19, 37, 129};

    for (int bpp : bpps) {
        for (int width : pixelCounts) {
            const size_t rowBytes = static_cast<size_t>(width) * static_cast<size_t>(bpp);
            std::vector<uint8_t> prevRow(rowBytes);
            std::vector<uint8_t> expectedRow(rowBytes);
            for (size_t i = 0; i < rowBytes; ++i) {
                prevRow[i] = static_cast<uint8_t>((i * 73u + 19u) & 0xFFu);
                expectedRow[i] = static_cast<uint8_t>((i * 137u + 41u) & 0xFFu);
            }

            for (bool hasPrev : {false, true}) {
                for (uint8_t f = 0; f <= 4; ++f) {
                    // Encode filtered row + trailing guard canary to verify zero OOB overwrite
                    std::vector<uint8_t> filtered(rowBytes + 16, 0xA5);
                    const uint8_t* prevPtr = hasPrev ? prevRow.data() : nullptr;

                    for (size_t i = 0; i < rowBytes; ++i) {
                        const int a = (i >= static_cast<size_t>(bpp)) ? expectedRow[i - static_cast<size_t>(bpp)] : 0;
                        const int b = prevPtr ? prevPtr[i] : 0;
                        const int c = (prevPtr && i >= static_cast<size_t>(bpp)) ? prevPtr[i - static_cast<size_t>(bpp)] : 0;
                        uint8_t pred = 0;
                        switch (f) {
                        case 0: pred = 0; break;
                        case 1: pred = static_cast<uint8_t>(a); break;
                        case 2: pred = static_cast<uint8_t>(b); break;
                        case 3: pred = static_cast<uint8_t>((a + b) >> 1); break;
                        case 4: pred = scalarPaeth(a, b, c); break;
                        }
                        filtered[i] = static_cast<uint8_t>(expectedRow[i] - pred);
                    }

                    ImageLoaderSimd::PngUnfilterRow(f, filtered.data(), prevPtr, rowBytes, bpp);

                    for (size_t i = 0; i < rowBytes; ++i) {
                        ASSERT_EQ(filtered[i], expectedRow[i])
                            << "bpp=" << bpp << " width=" << width << " hasPrev=" << hasPrev
                            << " filter=" << static_cast<int>(f) << " i=" << i;
                    }
                    for (size_t i = rowBytes; i < filtered.size(); ++i) {
                        ASSERT_EQ(filtered[i], 0xA5u) << "OOB overwrite at i=" << i;
                    }
                }
            }
        }
    }
}

namespace {

uint32_t TestPngCrc32(const uint8_t* data, size_t len) {
    return static_cast<uint32_t>(crc32(0L, data, static_cast<uInt>(len)));
}

void AppendTestPngChunk(std::vector<uint8_t>& out, const char type[4],
                        const std::vector<uint8_t>& payload) {
    const uint32_t len = static_cast<uint32_t>(payload.size());
    out.push_back(static_cast<uint8_t>(len >> 24));
    out.push_back(static_cast<uint8_t>(len >> 16));
    out.push_back(static_cast<uint8_t>(len >> 8));
    out.push_back(static_cast<uint8_t>(len));
    const size_t typePos = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), payload.begin(), payload.end());
    const uint32_t crc = TestPngCrc32(out.data() + typePos, 4 + payload.size());
    out.push_back(static_cast<uint8_t>(crc >> 24));
    out.push_back(static_cast<uint8_t>(crc >> 16));
    out.push_back(static_cast<uint8_t>(crc >> 8));
    out.push_back(static_cast<uint8_t>(crc));
}

std::vector<uint8_t> BuildSyntheticPng(
    uint32_t width, uint32_t height, uint8_t bitDepth, uint8_t colorType,
    const std::vector<uint8_t>& rawPixels, bool cycleAllFilters,
    const std::vector<uint8_t>& iccPayload, const std::vector<uint8_t>& trnsPayload) {
    const int channels = (colorType == 0) ? 1 : (colorType == 2) ? 3 : (colorType == 4) ? 2 : 4;
    const size_t bpp = static_cast<size_t>(channels) * (bitDepth / 8);
    const size_t rowBytes = static_cast<size_t>(width) * bpp;

    std::vector<uint8_t> scanlines(static_cast<size_t>(height) * (1 + rowBytes));
    auto paeth = [](int a, int b, int c) -> uint8_t {
        const int pa = std::abs(b - c);
        const int pb = std::abs(a - c);
        const int pc = std::abs((a - c) + (b - c));
        return static_cast<uint8_t>((pa <= pb && pa <= pc) ? a : ((pb <= pc) ? b : c));
    };

    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t f = cycleAllFilters ? static_cast<uint8_t>(y % 5) : 0;
        const uint8_t* curr = rawPixels.data() + static_cast<size_t>(y) * rowBytes;
        const uint8_t* prev = (y > 0) ? (rawPixels.data() + static_cast<size_t>(y - 1) * rowBytes) : nullptr;
        uint8_t* dst = scanlines.data() + static_cast<size_t>(y) * (1 + rowBytes);
        dst[0] = f;
        for (size_t i = 0; i < rowBytes; ++i) {
            const int a = (i >= bpp) ? curr[i - bpp] : 0;
            const int b = prev ? prev[i] : 0;
            const int c = (prev && i >= bpp) ? prev[i - bpp] : 0;
            uint8_t pred = 0;
            switch (f) {
            case 0: pred = 0; break;
            case 1: pred = static_cast<uint8_t>(a); break;
            case 2: pred = static_cast<uint8_t>(b); break;
            case 3: pred = static_cast<uint8_t>((a + b) >> 1); break;
            case 4: pred = paeth(a, b, c); break;
            }
            dst[1 + i] = static_cast<uint8_t>(curr[i] - pred);
        }
    }

    uLongf compLen = compressBound(static_cast<uLong>(scanlines.size()));
    std::vector<uint8_t> comp(compLen);
    EXPECT_EQ(compress2(comp.data(), &compLen, scanlines.data(),
                        static_cast<uLong>(scanlines.size()), Z_BEST_SPEED), Z_OK);
    comp.resize(compLen);

    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> ihdr(13, 0);
    ihdr[0] = static_cast<uint8_t>(width >> 24);
    ihdr[1] = static_cast<uint8_t>(width >> 16);
    ihdr[2] = static_cast<uint8_t>(width >> 8);
    ihdr[3] = static_cast<uint8_t>(width);
    ihdr[4] = static_cast<uint8_t>(height >> 24);
    ihdr[5] = static_cast<uint8_t>(height >> 16);
    ihdr[6] = static_cast<uint8_t>(height >> 8);
    ihdr[7] = static_cast<uint8_t>(height);
    ihdr[8] = bitDepth;
    ihdr[9] = colorType;
    AppendTestPngChunk(png, "IHDR", ihdr);

    if (!iccPayload.empty()) {
        uLongf iccCompLen = compressBound(static_cast<uLong>(iccPayload.size()));
        std::vector<uint8_t> iccComp(iccCompLen);
        EXPECT_EQ(compress2(iccComp.data(), &iccCompLen, iccPayload.data(),
                            static_cast<uLong>(iccPayload.size()), Z_BEST_SPEED), Z_OK);
        iccComp.resize(iccCompLen);
        std::vector<uint8_t> iccp = {'T', 'e', 's', 't', 0, 0};
        iccp.insert(iccp.end(), iccComp.begin(), iccComp.end());
        AppendTestPngChunk(png, "iCCP", iccp);
    }

    if (!trnsPayload.empty()) {
        AppendTestPngChunk(png, "tRNS", trnsPayload);
    }

    // Exercise 0-byte IDAT + split IDAT chunk boundaries
    AppendTestPngChunk(png, "IDAT", {});
    const size_t split = comp.size() / 2;
    AppendTestPngChunk(png, "IDAT", std::vector<uint8_t>(comp.begin(), comp.begin() + split));
    AppendTestPngChunk(png, "IDAT", std::vector<uint8_t>(comp.begin() + split, comp.end()));
    AppendTestPngChunk(png, "IEND", {});
    return png;
}

} // namespace

TEST(PngStreamingDecodeTest, Decodes8And16BitWithAllFiltersIccAndTrnsFallback) {
    // 1. 16-bit RGB across all 5 filters + split IDAT + iCCP (force8BitBgra=true and false)
    {
        constexpr uint32_t kW = 39, kH = 11;
        std::vector<uint8_t> raw(kW * kH * 6);
        for (uint32_t y = 0; y < kH; ++y) {
            for (uint32_t x = 0; x < kW; ++x) {
                const size_t off = (static_cast<size_t>(y) * kW + x) * 6;
                raw[off + 0] = static_cast<uint8_t>((x * 7 + y * 3) & 0xFF); raw[off + 1] = 0x12; // R BE
                raw[off + 2] = static_cast<uint8_t>((x * 5 + y * 11) & 0xFF); raw[off + 3] = 0x34; // G BE
                raw[off + 4] = static_cast<uint8_t>((x * 3 + y * 13) & 0xFF); raw[off + 5] = 0x56; // B BE
            }
        }
        const std::vector<uint8_t> dummyIcc = {0x00, 0x00, 0x00, 0x10, 't', 'e', 's', 't', 1, 2, 3, 4};
        const auto png = BuildSyntheticPng(kW, kH, 16, 2, raw, true, dummyIcc, {});

        uint32_t outW = 0, outH = 0;
        std::pmr::vector<uint8_t> bgra;
        WuffsLoader::WuffsImageInfo info8;
        ASSERT_TRUE(WuffsLoader::DecodePNG(png.data(), png.size(), &outW, &outH, bgra, {}, &info8, true));
        EXPECT_EQ(outW, kW);
        EXPECT_EQ(outH, kH);
        EXPECT_EQ(info8.bitDepth, 16);
        EXPECT_FALSE(info8.hasAlpha);
        EXPECT_EQ(info8.iccProfile.size(), dummyIcc.size());
        ASSERT_EQ(bgra.size(), static_cast<size_t>(kW) * kH * 4);
        for (uint32_t i = 0; i < kW * kH; ++i) {
            EXPECT_EQ(bgra[i * 4 + 0], raw[i * 6 + 4]);
            EXPECT_EQ(bgra[i * 4 + 1], raw[i * 6 + 2]);
            EXPECT_EQ(bgra[i * 4 + 2], raw[i * 6 + 0]);
            EXPECT_EQ(bgra[i * 4 + 3], 255);
        }

        std::pmr::vector<uint8_t> rgba16;
        WuffsLoader::WuffsImageInfo info16;
        ASSERT_TRUE(WuffsLoader::DecodePNG(png.data(), png.size(), &outW, &outH, rgba16, {}, &info16, false));
        ASSERT_EQ(rgba16.size(), static_cast<size_t>(kW) * kH * 8);
        const uint16_t* px16 = reinterpret_cast<const uint16_t*>(rgba16.data());
        for (uint32_t i = 0; i < kW * kH; ++i) {
            EXPECT_EQ(px16[i * 4 + 0], static_cast<uint16_t>((raw[i * 6 + 0] << 8) | raw[i * 6 + 1]));
            EXPECT_EQ(px16[i * 4 + 1], static_cast<uint16_t>((raw[i * 6 + 2] << 8) | raw[i * 6 + 3]));
            EXPECT_EQ(px16[i * 4 + 2], static_cast<uint16_t>((raw[i * 6 + 4] << 8) | raw[i * 6 + 5]));
            EXPECT_EQ(px16[i * 4 + 3], 0xFFFFu);
        }
    }

    // 2. 8-bit RGBA & 16-bit RGBA premultiplied alpha + GrayAlpha
    {
        constexpr uint32_t kW = 19, kH = 7;
        std::vector<uint8_t> raw8(kW * kH * 4);
        for (size_t i = 0; i < kW * kH; ++i) {
            raw8[i * 4 + 0] = static_cast<uint8_t>(200);
            raw8[i * 4 + 1] = static_cast<uint8_t>(100);
            raw8[i * 4 + 2] = static_cast<uint8_t>(50);
            raw8[i * 4 + 3] = static_cast<uint8_t>((i % 3 == 0) ? 0 : (i % 3 == 1 ? 128 : 255));
        }
        const auto png8 = BuildSyntheticPng(kW, kH, 8, 6, raw8, true, {}, {});
        uint32_t outW = 0, outH = 0;
        std::vector<uint8_t> out8;
        WuffsLoader::WuffsImageInfo info;
        ASSERT_TRUE(WuffsLoader::DecodePNG(png8.data(), png8.size(), &outW, &outH, out8, {}, &info, true));
        EXPECT_TRUE(info.hasAlpha);
        ASSERT_EQ(out8.size(), kW * kH * 4);
        EXPECT_EQ(out8[0 * 4 + 3], 0);
        EXPECT_EQ(out8[0 * 4 + 2], 0);
        EXPECT_NEAR(out8[1 * 4 + 2], (200 * 128 + 127) / 255, 1);
        EXPECT_EQ(out8[2 * 4 + 2], 200);
        EXPECT_EQ(out8[2 * 4 + 0], 50);

        std::vector<uint8_t> raw16 = {
            0x80, 0x00, 0x40, 0x00, 0x20, 0x00, 0x80, 0x00
        };
        const auto png16 = BuildSyntheticPng(1, 1, 16, 6, raw16, false, {}, {});
        std::vector<uint8_t> out16;
        ASSERT_TRUE(WuffsLoader::DecodePNG(png16.data(), png16.size(), &outW, &outH, out16, {}, nullptr, true));
        ASSERT_EQ(out16.size(), 4u);
        EXPECT_EQ(out16[3], 0x80);
        EXPECT_EQ(out16[2], 0x40); // ~0x8000 * 0x8000 / 0xFFFF >> 8 == 0x40
    }

    // 3. RGB with tRNS chunk must fall back cleanly to Wuffs and preserve alpha transparency
    {
        std::vector<uint8_t> raw = {
            10, 20, 30,  // matches tRNS key -> transparent
            40, 50, 60   // opaque
        };
        std::vector<uint8_t> trns = {0, 10, 0, 20, 0, 30};
        const auto pngTrns = BuildSyntheticPng(2, 1, 8, 2, raw, false, {}, trns);
        uint32_t outW = 0, outH = 0;
        std::vector<uint8_t> out;
        WuffsLoader::WuffsImageInfo info;
        ASSERT_TRUE(WuffsLoader::DecodePNG(pngTrns.data(), pngTrns.size(), &outW, &outH, out, {}, &info, true));
        EXPECT_TRUE(info.hasAlpha);
        ASSERT_EQ(out.size(), 8u);
        EXPECT_EQ(out[3], 0);
        EXPECT_EQ(out[7], 255);
    }
}

TEST(PngStreamingDecodeTest, PipelineRingBufferWraparound) {
    // 2048 x 1400 x 6 Bpp (~17.2 MB raw scanlines) triggers the 64-row SPSC ring pipeline
    // and wraps around > 20 times across all 5 PNG scanline filters.
    constexpr uint32_t kW = 2048, kH = 1400;
    std::vector<uint8_t> raw(static_cast<size_t>(kW) * kH * 6);
    for (uint32_t y = 0; y < kH; ++y) {
        const uint8_t yMod = static_cast<uint8_t>(y * 5u);
        uint8_t* row = raw.data() + static_cast<size_t>(y) * kW * 6;
        for (uint32_t x = 0; x < kW; ++x) {
            row[x * 6 + 0] = static_cast<uint8_t>((x + yMod) & 0xFFu);
            row[x * 6 + 1] = 0x11;
            row[x * 6 + 2] = static_cast<uint8_t>(((x >> 1) ^ yMod) & 0xFFu);
            row[x * 6 + 3] = 0x22;
            row[x * 6 + 4] = static_cast<uint8_t>((x * 3u + y) & 0xFFu);
            row[x * 6 + 5] = 0x33;
        }
    }

    const auto png = BuildSyntheticPng(kW, kH, 16, 2, raw, true, {}, {});
    uint32_t outW = 0, outH = 0;
    std::vector<uint8_t> bgra;
    WuffsLoader::WuffsImageInfo info;
    ASSERT_TRUE(WuffsLoader::DecodePNG(png.data(), png.size(), &outW, &outH, bgra, {}, &info, true));
    EXPECT_EQ(outW, kW);
    EXPECT_EQ(outH, kH);
    ASSERT_EQ(bgra.size(), static_cast<size_t>(kW) * kH * 4);

    // Check initial rows, multiple 64-row ring wraparound boundaries, and final rows in full
    const uint32_t checkRows[] = {0, 1, 62, 63, 64, 65, 127, 128, 129, 511, 512, 1023, 1024, kH - 2, kH - 1};
    for (uint32_t y : checkRows) {
        const uint8_t* srcRow = raw.data() + static_cast<size_t>(y) * kW * 6;
        const uint8_t* dstRow = bgra.data() + static_cast<size_t>(y) * kW * 4;
        for (uint32_t x = 0; x < kW; ++x) {
            ASSERT_EQ(dstRow[x * 4 + 0], srcRow[x * 6 + 4]) << "y=" << y << " x=" << x;
            ASSERT_EQ(dstRow[x * 4 + 1], srcRow[x * 6 + 2]) << "y=" << y << " x=" << x;
            ASSERT_EQ(dstRow[x * 4 + 2], srcRow[x * 6 + 0]) << "y=" << y << " x=" << x;
            ASSERT_EQ(dstRow[x * 4 + 3], 255) << "y=" << y << " x=" << x;
        }
    }
}
