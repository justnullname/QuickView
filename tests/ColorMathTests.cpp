#include <gtest/gtest.h>
#include "ColorMath.h"
#include "TileTypes.h"
#include "ImageLoaderSimd.h"

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
