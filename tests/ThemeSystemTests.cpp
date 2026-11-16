#include <gtest/gtest.h>
#include <array>
#include <cstdint>
#include "ThemePalette.h"

namespace QuickView::UI::Theme {
namespace {

TEST(ThemePaletteTest, ParseAccentPaletteBytes_Valid32Bytes) {
    // Windows AccentPalette stores 8 RGBA entries (32 bytes): Light3, Light2, Light1, Accent, Dark1, Dark2, Dark3, Unused
    const std::array<uint8_t, 32> bytes = {
        0xE5, 0xF3, 0xFF, 0xFF, // [0] Light3
        0x99, 0xD1, 0xFF, 0xFF, // [1] Light2
        0x4C, 0xA8, 0xFF, 0xFF, // [2] Light1
        0x00, 0x78, 0xD4, 0xFF, // [3] Accent
        0x00, 0x5A, 0x9E, 0xFF, // [4] Dark1
        0x00, 0x45, 0x78, 0xFF, // [5] Dark2
        0x00, 0x2B, 0x4D, 0xFF, // [6] Dark3
        0x00, 0x00, 0x00, 0x00, // [7] Unused
    };

    SystemAccentPalette out{};
    ASSERT_TRUE(ParseAccentPaletteBytes(bytes, out));
    EXPECT_TRUE(out.valid);
    EXPECT_NEAR(out.light3.r, 0xE5 / 255.0f, 1e-4f);
    EXPECT_NEAR(out.light2.r, 0x99 / 255.0f, 1e-4f);
    EXPECT_NEAR(out.accent.r, 0x00 / 255.0f, 1e-4f);
    EXPECT_NEAR(out.accent.g, 0x78 / 255.0f, 1e-4f);
    EXPECT_NEAR(out.accent.b, 0xD4 / 255.0f, 1e-4f);
    EXPECT_NEAR(out.dark1.b, 0x9E / 255.0f, 1e-4f);
    EXPECT_NEAR(out.dark2.g, 0x45 / 255.0f, 1e-4f);
    EXPECT_NEAR(out.dark3.b, 0x4D / 255.0f, 1e-4f);
    EXPECT_FLOAT_EQ(out.accent.a, 1.0f);
}

TEST(ThemePaletteTest, ParseAccentPaletteBytes_TooShortFails) {
    const std::array<uint8_t, 20> shortBytes{};
    SystemAccentPalette out{};
    EXPECT_FALSE(ParseAccentPaletteBytes(shortBytes, out));
    EXPECT_FALSE(out.valid);
}

TEST(ThemePaletteTest, ColorFromAbgrDword_UnpacksChannels) {
    // 0xAA_BB_GG_RR = 0xFF_D4_78_12 -> R=0x12, G=0x78, B=0xD4
    const D2D1_COLOR_F c = ColorFromAbgrDword(0xFFD47812u);
    EXPECT_NEAR(c.r, 0x12 / 255.0f, 1e-4f);
    EXPECT_NEAR(c.g, 0x78 / 255.0f, 1e-4f);
    EXPECT_NEAR(c.b, 0xD4 / 255.0f, 1e-4f);
    EXPECT_FLOAT_EQ(c.a, 1.0f);
}

TEST(ThemePaletteTest, DeriveFallbackPalette_ProducesMonotonicLuminance) {
    const D2D1_COLOR_F base = D2D1::ColorF(0.0f, 0.47f, 0.84f, 1.0f);
    const SystemAccentPalette pal = DeriveFallbackPaletteFromAccent(base);
    EXPECT_TRUE(pal.valid);
    const float l3 = ComputeApproxLinearLuminance(pal.light3);
    const float l2 = ComputeApproxLinearLuminance(pal.light2);
    const float la = ComputeApproxLinearLuminance(pal.accent);
    const float d1 = ComputeApproxLinearLuminance(pal.dark1);
    const float d2 = ComputeApproxLinearLuminance(pal.dark2);
    EXPECT_GT(l3, l2);
    EXPECT_GT(l2, la);
    EXPECT_GT(la, d1);
    EXPECT_GT(d1, d2);
}

TEST(ThemePaletteTest, ComputeOnAccentText_ContrastDecision) {
    // Bright yellow -> dark charcoal ink
    const D2D1_COLOR_F onYellow = ComputeOnAccentText(D2D1::ColorF(1.0f, 0.90f, 0.10f, 1.0f));
    EXPECT_LT(onYellow.r, 0.15f);
    EXPECT_LT(onYellow.g, 0.15f);
    EXPECT_LT(onYellow.b, 0.15f);

    // Light cyan (WinUI Light2) -> dark ink
    const D2D1_COLOR_F onLightCyan = ComputeOnAccentText(D2D1::ColorF(0.60f, 0.85f, 1.0f, 1.0f));
    EXPECT_LT(onLightCyan.r, 0.15f);

    // Deep blue -> white ink
    const D2D1_COLOR_F onDarkBlue = ComputeOnAccentText(D2D1::ColorF(0.0f, 0.35f, 0.65f, 1.0f));
    EXPECT_FLOAT_EQ(onDarkBlue.r, 1.0f);
    EXPECT_FLOAT_EQ(onDarkBlue.g, 1.0f);
    EXPECT_FLOAT_EQ(onDarkBlue.b, 1.0f);

    // Windows Blue (#0078D4) -> white ink (approx linear lum ~ 0.21 <= 0.22)
    const D2D1_COLOR_F onWindowsBlue = ComputeOnAccentText(D2D1::ColorF(0.0f, 0.471f, 0.831f, 1.0f));
    EXPECT_FLOAT_EQ(onWindowsBlue.r, 1.0f);

    // Classic Dark high-luminance fill (0.20, 0.60, 1.00) -> dark charcoal ink
    const D2D1_COLOR_F onSkyBlue = ComputeOnAccentText(D2D1::ColorF(0.20f, 0.60f, 1.00f, 1.0f));
    EXPECT_LT(onSkyBlue.r, 0.15f);
}

TEST(ThemePaletteTest, ResolveThemePalette_WinUI3StaticSystemMapping) {
    SystemAccentPalette sys{};
    sys.light3 = D2D1::ColorF(0.90f, 0.96f, 1.00f, 1.0f);
    sys.light2 = D2D1::ColorF(0.60f, 0.82f, 1.00f, 1.0f);
    sys.light1 = D2D1::ColorF(0.30f, 0.66f, 0.96f, 1.0f);
    sys.accent = D2D1::ColorF(0.00f, 0.47f, 0.83f, 1.0f);
    sys.dark1  = D2D1::ColorF(0.00f, 0.35f, 0.62f, 1.0f);
    sys.dark2  = D2D1::ColorF(0.00f, 0.27f, 0.47f, 1.0f);
    sys.dark3  = D2D1::ColorF(0.00f, 0.17f, 0.30f, 1.0f);
    sys.valid  = true;

    // Dark mode: accentFill = Light2, accentText = Light3, onAccentText = dark charcoal
    ThemeResolveInput darkIn{};
    darkIn.isLight = false;
    darkIn.accentMode = 0;
    darkIn.sysAccent = sys;
    const ThemePalette darkPal = ResolveThemePalette(darkIn);
    EXPECT_FALSE(darkPal.isLight);
    EXPECT_FLOAT_EQ(darkPal.accentFill.r, sys.light2.r);
    EXPECT_FLOAT_EQ(darkPal.accentFill.g, sys.light2.g);
    EXPECT_FLOAT_EQ(darkPal.accentFill.b, sys.light2.b);
    EXPECT_FLOAT_EQ(darkPal.accentText.r, sys.light3.r);
    EXPECT_FLOAT_EQ(darkPal.accentText.g, sys.light3.g);
    EXPECT_LT(darkPal.onAccentText.r, 0.15f);

    // Light mode: accentFill = Dark1, accentText = Dark2, onAccentText = white
    ThemeResolveInput lightIn{};
    lightIn.isLight = true;
    lightIn.accentMode = 0;
    lightIn.sysAccent = sys;
    const ThemePalette lightPal = ResolveThemePalette(lightIn);
    EXPECT_TRUE(lightPal.isLight);
    EXPECT_FLOAT_EQ(lightPal.accentFill.r, sys.dark1.r);
    EXPECT_FLOAT_EQ(lightPal.accentFill.b, sys.dark1.b);
    EXPECT_FLOAT_EQ(lightPal.accentText.r, sys.dark2.r);
    EXPECT_FLOAT_EQ(lightPal.accentText.b, sys.dark2.b);
    EXPECT_FLOAT_EQ(lightPal.onAccentText.r, 1.0f);
}

TEST(ThemePaletteTest, ResolveThemePalette_InvalidSystemFallbackAndCustom) {
    // Invalid system registry -> safely falls back to built-in blue without zeroing out colors
    ThemeResolveInput brokenSysDark{};
    brokenSysDark.isLight = false;
    brokenSysDark.accentMode = 0;
    brokenSysDark.sysAccent.valid = false;
    const ThemePalette fallbackDark = ResolveThemePalette(brokenSysDark);
    EXPECT_GT(fallbackDark.accentFill.b, 0.5f);
    EXPECT_LT(fallbackDark.onAccentText.r, 0.15f);

    ThemeResolveInput brokenSysLight{};
    brokenSysLight.isLight = true;
    brokenSysLight.accentMode = 0;
    brokenSysLight.sysAccent.valid = false;
    const ThemePalette fallbackLight = ResolveThemePalette(brokenSysLight);
    EXPECT_GT(fallbackLight.accentFill.b, 0.5f);
    EXPECT_FLOAT_EQ(fallbackLight.onAccentText.r, 1.0f);

    // Custom mode (1) preserves user-selected RGB
    ThemeResolveInput customInput{};
    customInput.isLight = false;
    customInput.accentMode = 1;
    customInput.customAccent = D2D1::ColorF(0.95f, 0.45f, 0.10f, 1.0f);
    const ThemePalette customPal = ResolveThemePalette(customInput);
    EXPECT_NEAR(customPal.accentFill.r, 0.95f, 0.01f);
    EXPECT_NEAR(customPal.windowBorderAccent.r, 0.95f, 0.01f);
}

TEST(ThemePaletteTest, ClampCustomAccentText_ProtectsDarkAndLightLegibility) {
    // Near-black custom color in Dark mode must be lifted for text readability
    const D2D1_COLOR_F liftedForDark = ClampCustomAccentText(D2D1::ColorF(0.05f, 0.05f, 0.08f, 1.0f), false);
    EXPECT_GT(ComputeApproxLinearLuminance(liftedForDark), 0.18f);

    // Near-white custom color in Light mode must be darkened for text readability
    const D2D1_COLOR_F darkenedForLight = ClampCustomAccentText(D2D1::ColorF(0.98f, 0.96f, 0.90f, 1.0f), true);
    EXPECT_LT(ComputeApproxLinearLuminance(darkenedForLight), 0.25f);
}

TEST(ThemePaletteTest, MigrateLegacyThemeConfig_HandlesThemeMode3AndSchemaV2) {
    // Case 1: Fresh or un-migrated config with accentMode == -1 uses System (0)
    int themeMode = 0;
    int accentMode = -1;
    MigrateLegacyThemeConfig(themeMode, accentMode, 1, 0, 0.f, 0.f, 0.f, 1.f, 1.f, 1.f, 0.f, 0.47f, 0.84f);
    EXPECT_EQ(themeMode, 0);
    EXPECT_EQ(accentMode, 0);

    // Case 2: Legacy ThemeMode == 3 (slider triggered auto-custom) with dark glass & default DodgerBlue -> Dark + System accent
    themeMode = 3;
    accentMode = -1;
    MigrateLegacyThemeConfig(themeMode, accentMode, 1, 1, 0.08f, 0.08f, 0.10f, 1.0f, 1.0f, 1.0f, 0.0f, 0.47f, 0.84f);
    EXPECT_EQ(themeMode, 1);
    EXPECT_EQ(accentMode, 0);

    // Case 3: Legacy ThemeMode == 3 with explicit user custom accent (orange) -> preserves Custom Accent (1 in schema v2)
    themeMode = 3;
    accentMode = -1;
    MigrateLegacyThemeConfig(themeMode, accentMode, 1, 1, 0.985f, 0.99f, 1.0f, 0.1f, 0.1f, 0.1f, 0.95f, 0.45f, 0.10f);
    EXPECT_EQ(themeMode, 2); // light text -> Light theme
    EXPECT_EQ(accentMode, 1); // preserved user custom accent in new schema (0=System, 1=Custom)

    // Case 4: Legacy schema (v1) Classic (1) migrates to System (0), legacy Custom (2) migrates to Custom (1)
    themeMode = 1;
    accentMode = 1;
    MigrateLegacyThemeConfig(themeMode, accentMode, 1, 0, 0.f, 0.f, 0.f, 1.f, 1.f, 1.f, 0.f, 0.47f, 0.84f);
    EXPECT_EQ(accentMode, 0);

    accentMode = 2;
    MigrateLegacyThemeConfig(themeMode, accentMode, 1, 0, 0.f, 0.f, 0.f, 1.f, 1.f, 1.f, 0.95f, 0.45f, 0.10f);
    EXPECT_EQ(accentMode, 1);

    // Case 5: Schema v2 preserves Custom (1) without treating it as legacy Classic
    accentMode = 1;
    MigrateLegacyThemeConfig(themeMode, accentMode, 2, 0, 0.f, 0.f, 0.f, 1.f, 1.f, 1.f, 0.95f, 0.45f, 0.10f);
    EXPECT_EQ(accentMode, 1);
}

TEST(ThemePaletteTest, ResolveWindowBorderColorRef_CoversModesAndStates) {
    ThemePalette pal{};
    pal.systemBorderAccent = D2D1::ColorF(0.20f, 0.60f, 1.00f, 1.0f);
    pal.windowBorderAccent = D2D1::ColorF(1.00f, 0.40f, 0.00f, 1.0f);

    // 1. Follow System (0): ColorPrevalence disabled -> default DWM border; enabled -> systemBorderAccent
    EXPECT_EQ(ResolveWindowBorderColorRef(pal, 0, false, true, false, false), 0xFFFFFFFFu);
    const uint32_t sysCol = ResolveWindowBorderColorRef(pal, 0, true, true, false, false);
    EXPECT_EQ(sysCol & 0xFFu, 51u);          // R ~= 0.20 * 255
    EXPECT_EQ((sysCol >> 8) & 0xFFu, 153u);  // G ~= 0.60 * 255
    EXPECT_EQ((sysCol >> 16) & 0xFFu, 255u); // B ~= 1.00 * 255

    // 2. Always Follow Theme Accent (1): overrides ColorPrevalence==false using windowBorderAccent
    const uint32_t themeCol = ResolveWindowBorderColorRef(pal, 1, false, true, false, false);
    EXPECT_EQ(themeCol & 0xFFu, 255u);        // R = 1.00 * 255
    EXPECT_EQ((themeCol >> 8) & 0xFFu, 102u); // G = 0.40 * 255
    EXPECT_EQ((themeCol >> 16) & 0xFFu, 0u);  // B = 0.00 * 255

    // 3. Off (2): always default DWM border even when ColorPrevalence is enabled
    EXPECT_EQ(ResolveWindowBorderColorRef(pal, 2, true, true, false, false), 0xFFFFFFFFu);

    // 4. Inactive window reverts to default DWM border
    EXPECT_EQ(ResolveWindowBorderColorRef(pal, 1, true, false, false, false), 0xFFFFFFFFu);

    // 5. Fullscreen hides DWM border (DWMWA_COLOR_NONE)
    EXPECT_EQ(ResolveWindowBorderColorRef(pal, 1, true, true, true, false), 0xFFFFFFFEu);

    // 6. Spotlight slideshow forces black border (0x00000000)
    EXPECT_EQ(ResolveWindowBorderColorRef(pal, 1, true, true, false, true), 0x00000000u);
}

} // namespace
} // namespace QuickView::UI::Theme
