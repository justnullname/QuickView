#pragma once
#include <cstdint>
#include <span>
#include <d2d1_3.h>

struct AppConfig;

namespace QuickView::UI::Theme {

/// Windows AccentPalette 7-step shade family (Index 0..6 from AccentPalette registry binary).
struct SystemAccentPalette {
    D2D1_COLOR_F light3{};
    D2D1_COLOR_F light2{};
    D2D1_COLOR_F light1{};
    D2D1_COLOR_F accent{};
    D2D1_COLOR_F dark1{};
    D2D1_COLOR_F dark2{};
    D2D1_COLOR_F dark3{};
    bool valid = false;
};

/// Single source of truth for resolved UI colors across all overlays, widgets, and HUDs.
struct ThemePalette {
    bool isLight = false;

    // Surfaces & neutral text
    D2D1_COLOR_F dimmer{};
    D2D1_COLOR_F panelBg{};
    D2D1_COLOR_F controlBg{};
    D2D1_COLOR_F border{};
    D2D1_COLOR_F text{};
    D2D1_COLOR_F textDim{};
    D2D1_COLOR_F subtleTint{};
    D2D1_COLOR_F hoverTint{};
    D2D1_COLOR_F disabledFill{};
    D2D1_COLOR_F shadow{};
    D2D1_COLOR_F tooltipBg{};

    // Semantic accent tokens
    D2D1_COLOR_F accentFill{};          ///< Solid interactive fill (Primary button, Toggle ON, Active Segment)
    D2D1_COLOR_F accentText{};          ///< Readable accent (Labels, icons, focus rings, stars, overflow line)
    D2D1_COLOR_F onAccentText{};        ///< Contrasting foreground on top of accentFill (#101014 or #FFFFFF)
    D2D1_COLOR_F accentSubtle{};        ///< Low-alpha selection/hover tint
    D2D1_COLOR_F accentGlow{};          ///< Luminous progress glow / track highlight
    D2D1_COLOR_F systemBorderAccent{};  ///< OS raw accent color for Win11 DWM border when following system
    D2D1_COLOR_F windowBorderAccent{};  ///< Effective accent color for Win11 DWM border when following QuickView theme

    // Semantic status colors
    D2D1_COLOR_F success{};
    D2D1_COLOR_F warning{};
    D2D1_COLOR_F error{};
};

/// Input parameters for pure palette resolution (enables deterministic unit testing).
struct ThemeResolveInput {
    bool isLight = false;
    int accentMode = 0; // 0=System, 1=Custom
    D2D1_COLOR_F customAccent = { 0.00f, 0.47f, 0.84f, 1.0f };
    float glassModalsOpacity = 55.0f;
    SystemAccentPalette sysAccent{};
};

/// Parse HKCU\...\Explorer\Accent\AccentPalette bytes (32 bytes = 8 RGBA entries, first 7 used).
bool ParseAccentPaletteBytes(std::span<const uint8_t> bytes, SystemAccentPalette& out) noexcept;

/// Convert a Win32 ABGR DWORD (e.g. DWM\AccentColor) into a D2D1_COLOR_F with alpha = 1.0.
D2D1_COLOR_F ColorFromAbgrDword(uint32_t abgr) noexcept;

/// Build a 7-step fallback shade family from a single base accent color.
SystemAccentPalette DeriveFallbackPaletteFromAccent(D2D1_COLOR_F baseAccent) noexcept;

/// Fast perceptual relative luminance using quadratic sRGB approximation (0 pow calls).
float ComputeApproxLinearLuminance(D2D1_COLOR_F c) noexcept;

/// Choose deep charcoal (#101014) or pure white (#FFFFFF) for maximum legibility over accentFill.
D2D1_COLOR_F ComputeOnAccentText(D2D1_COLOR_F accentFill) noexcept;

/// Apply minimal lightness guardrails to user custom accent colors for text/icon legibility.
D2D1_COLOR_F ClampCustomAccentText(D2D1_COLOR_F accent, bool isLight) noexcept;

/// Resolve a complete ThemePalette deterministically from inputs.
ThemePalette ResolveThemePalette(const ThemeResolveInput& input) noexcept;

/// Migrate legacy ThemeMode=3 (Custom), legacy Classic AccentMode=1, and uninitialized AccentMode (-1) safely.
/// accentSchemaVersion < 2 indicates legacy 3-state AccentMode (0=System, 1=Classic->System, 2=Custom->1).
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
    float customAccentB) noexcept;

/// Read HKCU\...\Explorer\Accent\AccentPalette (fallback to DWM\AccentColor) directly from registry.
SystemAccentPalette ReadSystemAccentPaletteFromRegistry() noexcept;

/// Read HKCU\...\Themes\Personalize\AppsUseLightTheme directly from registry.
bool ReadSystemAppsUseLightThemeFromRegistry(bool defaultValue = false) noexcept;

/// Read HKCU\...\DWM\ColorPrevalence ("Show accent color on title bars and window borders").
bool ReadSystemDwmColorPrevalenceFromRegistry(bool defaultValue = false) noexcept;

/// Refresh the process-wide cached ThemePalette from registry + AppConfig.
/// Call on startup, config changes, WM_SETTINGCHANGE("ImmersiveColorSet"), and WM_DWMCOLORIZATIONCOLORCHANGED.
void RefreshThemePalette(const AppConfig& config) noexcept;

/// Zero-overhead read access to the cached ThemePalette on the UI thread.
const ThemePalette& GetThemePalette() noexcept;

/// Returns cached OS light theme flag updated by RefreshThemePalette.
bool IsSystemLightThemeCached() noexcept;

/// Returns cached OS DWM ColorPrevalence flag updated by RefreshThemePalette.
bool IsSystemColorPrevalenceCached() noexcept;

/// Returns effective light theme state for the current AppConfig using cached OS state when ThemeMode == 0.
bool ResolveEffectiveLightTheme(const AppConfig& config) noexcept;

/// Resolve Win32 COLORREF (0x00BBGGRR), DWMWA_COLOR_DEFAULT (0xFFFFFFFF), or DWMWA_COLOR_NONE (0xFFFFFFFE).
uint32_t ResolveWindowBorderColorRef(
    const ThemePalette& palette,
    int windowBorderAccentMode,
    bool osColorPrevalence,
    bool isActiveWindow,
    bool isFullScreen,
    bool isSpotlightSlideshow) noexcept;

/// Resolve effective Win11 DWM border attribute color from current config and cached palette.
uint32_t ResolveEffectiveWindowBorderColor(
    const AppConfig& config,
    bool isActiveWindow,
    bool isFullScreen,
    bool isSpotlightSlideshow) noexcept;

} // namespace QuickView::UI::Theme
