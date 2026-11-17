#include "pch.h"
#include "ThemePalette.h"
#include "EditState.h"
#include <algorithm>
#include <cmath>

namespace QuickView::UI::Theme {

namespace {

constexpr D2D1_COLOR_F kClassicDarkFill   = { 0.20f, 0.60f, 1.00f, 1.0f };
constexpr D2D1_COLOR_F kClassicDarkText   = { 0.35f, 0.74f, 1.00f, 1.0f };
constexpr D2D1_COLOR_F kClassicLightFill  = { 0.00f, 0.45f, 0.90f, 1.0f };
constexpr D2D1_COLOR_F kClassicLightText  = { 0.00f, 0.36f, 0.78f, 1.0f };

constexpr D2D1_COLOR_F kCharcoalText      = { 0.06f, 0.06f, 0.08f, 1.0f };
constexpr D2D1_COLOR_F kPureWhiteText     = { 1.00f, 1.00f, 1.00f, 1.0f };

constexpr float kOnAccentLuminanceThreshold = 0.22f;
constexpr float kMinDarkAccentTextLuminance = 0.185f;
constexpr float kMaxLightAccentTextLuminance = 0.22f;

constexpr uint32_t kDwmColorNone = 0xFFFFFFFEu;
constexpr uint32_t kDwmColorDefault = 0xFFFFFFFFu;
constexpr D2D1_COLOR_F kFallbackBorderAccent = { 0.00f, 0.47f, 0.84f, 1.0f };

uint32_t ToWin32ColorRef(D2D1_COLOR_F c) noexcept {
    const auto byteOf = [](float v) noexcept -> uint32_t {
        const float clamped = (std::clamp)(v, 0.0f, 1.0f);
        return static_cast<uint32_t>(std::lround(clamped * 255.0f));
    };
    return byteOf(c.r) | (byteOf(c.g) << 8) | (byteOf(c.b) << 16);
}

inline float Clamp01(float v) noexcept {
    return (std::clamp)(v, 0.0f, 1.0f);
}

inline D2D1_COLOR_F LerpRgb(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) noexcept {
    const float ct = Clamp01(t);
    return {
        a.r + (b.r - a.r) * ct,
        a.g + (b.g - a.g) * ct,
        a.b + (b.b - a.b) * ct,
        1.0f
    };
}

inline D2D1_COLOR_F SanitizeOpaqueColor(D2D1_COLOR_F c) noexcept {
    return { Clamp01(c.r), Clamp01(c.g), Clamp01(c.b), 1.0f };
}

inline bool NearlyEqualColor(float r1, float g1, float b1, float r2, float g2, float b2) noexcept {
    constexpr float kEps = 0.015f;
    return std::fabs(r1 - r2) <= kEps && std::fabs(g1 - g2) <= kEps && std::fabs(b1 - b2) <= kEps;
}

// Runtime cache (updated on startup, config change, and OS theme broadcasts)
ThemePalette s_cachedPalette = ResolveThemePalette(ThemeResolveInput{});
bool s_cachedSystemLightTheme = false;
bool s_systemThemeInitialized = false;
bool s_cachedSystemColorPrevalence = false;
bool s_colorPrevalenceInitialized = false;

} // namespace

bool ParseAccentPaletteBytes(std::span<const uint8_t> bytes, SystemAccentPalette& out) noexcept {
    // Windows AccentPalette stores 8 entries x 4 bytes (R, G, B, 0x00).
    // Indices 0..6 map to Light3, Light2, Light1, Accent, Dark1, Dark2, Dark3.
    if (bytes.size() < 28) {
        out.valid = false;
        return false;
    }

    auto readEntry = [&](size_t idx) noexcept -> D2D1_COLOR_F {
        const size_t off = idx * 4;
        return {
            static_cast<float>(bytes[off + 0]) / 255.0f,
            static_cast<float>(bytes[off + 1]) / 255.0f,
            static_cast<float>(bytes[off + 2]) / 255.0f,
            1.0f
        };
    };

    out.light3 = readEntry(0);
    out.light2 = readEntry(1);
    out.light1 = readEntry(2);
    out.accent = readEntry(3);
    out.dark1  = readEntry(4);
    out.dark2  = readEntry(5);
    out.dark3  = readEntry(6);
    out.valid  = true;
    return true;
}

D2D1_COLOR_F ColorFromAbgrDword(uint32_t abgr) noexcept {
    return {
        static_cast<float>(abgr & 0xFFu) / 255.0f,
        static_cast<float>((abgr >> 8) & 0xFFu) / 255.0f,
        static_cast<float>((abgr >> 16) & 0xFFu) / 255.0f,
        1.0f
    };
}

SystemAccentPalette DeriveFallbackPaletteFromAccent(D2D1_COLOR_F baseAccent) noexcept {
    const D2D1_COLOR_F base = SanitizeOpaqueColor(baseAccent);
    constexpr D2D1_COLOR_F kWhite = { 1.0f, 1.0f, 1.0f, 1.0f };
    constexpr D2D1_COLOR_F kBlack = { 0.0f, 0.0f, 0.0f, 1.0f };

    SystemAccentPalette out{};
    out.light3 = LerpRgb(base, kWhite, 0.70f);
    out.light2 = LerpRgb(base, kWhite, 0.48f);
    out.light1 = LerpRgb(base, kWhite, 0.25f);
    out.accent = base;
    out.dark1  = LerpRgb(base, kBlack, 0.22f);
    out.dark2  = LerpRgb(base, kBlack, 0.42f);
    out.dark3  = LerpRgb(base, kBlack, 0.62f);
    out.valid  = true;
    return out;
}

float ComputeApproxLinearLuminance(D2D1_COLOR_F c) noexcept {
    const float r = Clamp01(c.r);
    const float g = Clamp01(c.g);
    const float b = Clamp01(c.b);
    return 0.2126f * (r * r) + 0.7152f * (g * g) + 0.0722f * (b * b);
}

D2D1_COLOR_F ComputeOnAccentText(D2D1_COLOR_F accentFill) noexcept {
    return (ComputeApproxLinearLuminance(accentFill) > kOnAccentLuminanceThreshold)
        ? kCharcoalText
        : kPureWhiteText;
}

D2D1_COLOR_F ClampCustomAccentText(D2D1_COLOR_F accent, bool isLight) noexcept {
    const D2D1_COLOR_F base = SanitizeOpaqueColor(accent);
    const float lum = ComputeApproxLinearLuminance(base);

    if (!isLight) {
        if (lum >= kMinDarkAccentTextLuminance) return base;
        float lo = 0.0f;
        float hi = 1.0f;
        for (int i = 0; i < 8; ++i) {
            const float mid = 0.5f * (lo + hi);
            const D2D1_COLOR_F candidate = LerpRgb(base, kPureWhiteText, mid);
            if (ComputeApproxLinearLuminance(candidate) < kMinDarkAccentTextLuminance) {
                lo = mid;
            } else {
                hi = mid;
            }
        }
        return LerpRgb(base, kPureWhiteText, hi);
    }

    if (lum <= kMaxLightAccentTextLuminance) return base;
    constexpr D2D1_COLOR_F kBlack = { 0.0f, 0.0f, 0.0f, 1.0f };
    float lo = 0.0f;
    float hi = 1.0f;
    for (int i = 0; i < 8; ++i) {
        const float mid = 0.5f * (lo + hi);
        const D2D1_COLOR_F candidate = LerpRgb(base, kBlack, mid);
        if (ComputeApproxLinearLuminance(candidate) > kMaxLightAccentTextLuminance) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return LerpRgb(base, kBlack, hi);
}

ThemePalette ResolveThemePalette(const ThemeResolveInput& input) noexcept {
    ThemePalette pal{};
    pal.isLight = input.isLight;
    const float modalAlpha = (std::clamp)(input.glassModalsOpacity / 100.0f, 0.01f, 1.0f);

    if (input.isLight) {
        pal.dimmer       = { 0.94f, 0.96f, 0.99f, 0.52f };
        pal.text         = { 0.10f, 0.12f, 0.15f, 1.00f };
        pal.textDim      = { 0.35f, 0.40f, 0.48f, 1.00f };
        pal.controlBg    = { 0.92f, 0.94f, 0.97f, 0.70f };
        pal.border       = { 0.80f, 0.84f, 0.89f, 1.00f };
        pal.success      = { 0.11f, 0.62f, 0.23f, 1.00f };
        pal.warning      = { 0.85f, 0.50f, 0.02f, 1.00f };
        pal.error        = { 0.79f, 0.19f, 0.16f, 1.00f };
        pal.panelBg      = { 0.985f, 0.99f, 1.00f, modalAlpha };
        pal.hoverTint    = { 0.00f, 0.18f, 0.42f, 0.10f };
        pal.disabledFill = { 0.85f, 0.88f, 0.92f, 0.65f };
        pal.subtleTint   = { 0.06f, 0.08f, 0.12f, 0.06f };
        pal.shadow       = { 0.00f, 0.00f, 0.00f, 0.10f };
        pal.tooltipBg    = { 0.96f, 0.97f, 0.99f, 0.96f };
    } else {
        pal.dimmer       = { 0.00f, 0.00f, 0.00f, 0.40f };
        pal.text         = { 1.00f, 1.00f, 1.00f, 1.00f };
        pal.textDim      = { 0.75f, 0.75f, 0.75f, 1.00f };
        pal.controlBg    = { 0.25f, 0.25f, 0.25f, 0.70f };
        pal.border       = { 0.30f, 0.30f, 0.30f, 1.00f };
        pal.success      = { 0.10f, 0.80f, 0.10f, 1.00f };
        pal.warning      = { 1.00f, 0.72f, 0.18f, 1.00f };
        pal.error        = { 0.80f, 0.10f, 0.10f, 1.00f };
        pal.panelBg      = { 0.08f, 0.08f, 0.10f, modalAlpha };
        pal.hoverTint    = { 1.00f, 1.00f, 1.00f, 0.12f };
        pal.disabledFill = { 0.30f, 0.30f, 0.30f, 0.50f };
        pal.subtleTint   = { 1.00f, 1.00f, 1.00f, 0.10f };
        pal.shadow       = { 0.00f, 0.00f, 0.00f, 0.20f };
        pal.tooltipBg    = { 0.14f, 0.14f, 0.16f, 0.96f };
    }

    // Resolve raw accent tokens for Win11 DWM 1px window border
    pal.systemBorderAccent = input.sysAccent.valid
        ? SanitizeOpaqueColor(input.sysAccent.accent)
        : kFallbackBorderAccent;

    // Resolve Accent Roles using WinUI 3 deterministic shade mapping (0=System, 1=Custom)
    if (input.accentMode == 1) {
        pal.accentFill = SanitizeOpaqueColor(input.customAccent);
        pal.accentText = ClampCustomAccentText(pal.accentFill, input.isLight);
        pal.windowBorderAccent = pal.accentFill;
    } else if (input.sysAccent.valid) {
        if (input.isLight) {
            pal.accentFill = SanitizeOpaqueColor(input.sysAccent.dark1);
            pal.accentText = SanitizeOpaqueColor(input.sysAccent.dark2);
        } else {
            pal.accentFill = SanitizeOpaqueColor(input.sysAccent.light2);
            pal.accentText = SanitizeOpaqueColor(input.sysAccent.light3);
        }
        pal.windowBorderAccent = pal.systemBorderAccent;
    } else {
        // Fallback blue when OS accent registry entries are unavailable
        if (input.isLight) {
            pal.accentFill = kClassicLightFill;
            pal.accentText = kClassicLightText;
        } else {
            pal.accentFill = kClassicDarkFill;
            pal.accentText = kClassicDarkText;
        }
        pal.windowBorderAccent = pal.systemBorderAccent;
    }

    pal.onAccentText = ComputeOnAccentText(pal.accentFill);
    pal.accentSubtle = { pal.accentFill.r, pal.accentFill.g, pal.accentFill.b, input.isLight ? 0.14f : 0.24f };
    pal.accentGlow   = { pal.accentText.r, pal.accentText.g, pal.accentText.b, 0.45f };
    return pal;
}

void MigrateLegacyThemeConfig(
    int& themeMode,
    int& accentMode,
    int accentSchemaVersion,
    int glassTintProfile,
    float glassCustomTintR,
    float glassCustomTintG,
    float glassCustomTintB,
    float customTextR,
    float customTextG,
    float customTextB,
    float customAccentR,
    float customAccentG,
    float customAccentB) noexcept {
    if (accentSchemaVersion < 2) {
        // Legacy 3-state AccentMode: 0=System, 1=Classic (-> System), 2=Custom (-> 1)
        if (accentMode == 1) {
            accentMode = 0;
        } else if (accentMode == 2) {
            accentMode = 1;
        }
    }

    if (themeMode == 3) {
        bool legacyIsLight = false;
        if (glassTintProfile == 1) {
            const float tintLuma = glassCustomTintR * 0.299f + glassCustomTintG * 0.587f + glassCustomTintB * 0.114f;
            legacyIsLight = tintLuma > 0.5f;
        } else {
            const float textLuma = customTextR * 0.299f + customTextG * 0.587f + customTextB * 0.114f;
            legacyIsLight = textLuma < 0.5f;
        }
        themeMode = legacyIsLight ? 2 : 1;

        if (accentMode < 0) {
            const bool isDefaultBlue =
                NearlyEqualColor(customAccentR, customAccentG, customAccentB, 0.00f, 0.47f, 0.84f) ||
                NearlyEqualColor(customAccentR, customAccentG, customAccentB, 0.00f, 0.60f, 1.00f) ||
                NearlyEqualColor(customAccentR, customAccentG, customAccentB, 0.00f, 0.45f, 0.90f);
            accentMode = isDefaultBlue ? 0 : 1;
        }
    }

    if (themeMode < 0 || themeMode > 2) themeMode = 0;
    if (accentMode < 0 || accentMode > 1) accentMode = 0;
}

SystemAccentPalette ReadSystemAccentPaletteFromRegistry() noexcept {
    SystemAccentPalette out{};
    uint8_t buffer[32] = {};
    DWORD size = sizeof(buffer);
    const LONG status = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
        L"AccentPalette",
        RRF_RT_REG_BINARY,
        nullptr,
        buffer,
        &size);
    if (status == ERROR_SUCCESS && ParseAccentPaletteBytes(std::span<const uint8_t>(buffer, size), out)) {
        return out;
    }

    DWORD dwmAccent = 0;
    DWORD dwmSize = sizeof(dwmAccent);
    const LONG dwmStatus = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\DWM",
        L"AccentColor",
        RRF_RT_REG_DWORD,
        nullptr,
        &dwmAccent,
        &dwmSize);
    if (dwmStatus == ERROR_SUCCESS) {
        return DeriveFallbackPaletteFromAccent(ColorFromAbgrDword(dwmAccent));
    }

    return out;
}

bool ReadSystemAppsUseLightThemeFromRegistry(bool defaultValue) noexcept {
    DWORD value = defaultValue ? 1u : 0u;
    DWORD size = sizeof(value);
    const LONG status = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme",
        RRF_RT_REG_DWORD,
        nullptr,
        &value,
        &size);
    if (status != ERROR_SUCCESS) return defaultValue;
    return value != 0;
}

bool ReadSystemDwmColorPrevalenceFromRegistry(bool defaultValue) noexcept {
    DWORD value = defaultValue ? 1u : 0u;
    DWORD size = sizeof(value);
    const LONG status = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\DWM",
        L"ColorPrevalence",
        RRF_RT_REG_DWORD,
        nullptr,
        &value,
        &size);
    if (status != ERROR_SUCCESS) return defaultValue;
    return value != 0;
}

void RefreshThemePalette(const AppConfig& config) noexcept {
    s_cachedSystemLightTheme = ReadSystemAppsUseLightThemeFromRegistry(false);
    s_systemThemeInitialized = true;
    s_cachedSystemColorPrevalence = ReadSystemDwmColorPrevalenceFromRegistry(false);
    s_colorPrevalenceInitialized = true;
    const SystemAccentPalette sysAccent = (config.AccentMode == 0 || config.WindowBorderAccentMode == 0)
        ? ReadSystemAccentPaletteFromRegistry()
        : SystemAccentPalette{};

    ThemeResolveInput input{};
    input.isLight = ResolveEffectiveLightTheme(config);
    input.accentMode = config.AccentMode;
    input.customAccent = { config.ThemeCustomAccentR, config.ThemeCustomAccentG, config.ThemeCustomAccentB, 1.0f };
    input.glassModalsOpacity = config.GlassModalsOpacity;
    input.sysAccent = sysAccent;

    s_cachedPalette = ResolveThemePalette(input);
}

const ThemePalette& GetThemePalette() noexcept {
    return s_cachedPalette;
}

bool IsSystemLightThemeCached() noexcept {
    if (!s_systemThemeInitialized) {
        s_cachedSystemLightTheme = ReadSystemAppsUseLightThemeFromRegistry(false);
        s_systemThemeInitialized = true;
    }
    return s_cachedSystemLightTheme;
}

bool IsSystemColorPrevalenceCached() noexcept {
    if (!s_colorPrevalenceInitialized) {
        s_cachedSystemColorPrevalence = ReadSystemDwmColorPrevalenceFromRegistry(false);
        s_colorPrevalenceInitialized = true;
    }
    return s_cachedSystemColorPrevalence;
}

bool ResolveEffectiveLightTheme(const AppConfig& config) noexcept {
    switch (config.ThemeMode) {
        case 1: return false;
        case 2: return true;
        default: return IsSystemLightThemeCached();
    }
}

uint32_t ResolveWindowBorderColorRef(
    const ThemePalette& palette,
    int windowBorderAccentMode,
    bool osColorPrevalence,
    bool isActiveWindow,
    bool isFullScreen,
    bool isSpotlightSlideshow) noexcept {
    if (isFullScreen) return kDwmColorNone;
    if (isSpotlightSlideshow) return 0x00000000u;
    if (!isActiveWindow || windowBorderAccentMode == 2) return kDwmColorDefault;

    if (windowBorderAccentMode == 0) {
        if (!osColorPrevalence) return kDwmColorDefault;
        return ToWin32ColorRef(palette.systemBorderAccent);
    }
    if (windowBorderAccentMode == 1) {
        return ToWin32ColorRef(palette.windowBorderAccent);
    }
    return kDwmColorDefault;
}

uint32_t ResolveEffectiveWindowBorderColor(
    const AppConfig& config,
    bool isActiveWindow,
    bool isFullScreen,
    bool isSpotlightSlideshow) noexcept {
    return ResolveWindowBorderColorRef(
        s_cachedPalette,
        config.WindowBorderAccentMode,
        IsSystemColorPrevalenceCached(),
        isActiveWindow,
        isFullScreen,
        isSpotlightSlideshow);
}

} // namespace QuickView::UI::Theme
