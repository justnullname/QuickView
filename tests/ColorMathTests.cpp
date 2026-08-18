#include <gtest/gtest.h>
#include "ColorMath.h"

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
