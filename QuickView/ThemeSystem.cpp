#include "pch.h"
#include "ThemeSystem.h"
#include <commdlg.h>
#include <shlwapi.h>

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shlwapi.lib")

extern void SaveConfig(); // Defined in main.cpp

namespace QuickView::UI::ThemeSystem {

    static std::wstring ShowFileDialog(HWND hwnd, bool isSave) {
        wchar_t szFile[MAX_PATH] = { 0 };
        OPENFILENAMEW ofn = { sizeof(ofn) };
        ofn.hwndOwner = hwnd;
        ofn.lpstrFile = szFile;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrFilter = L"QuickView Theme (*.qvtheme)\0*.qvtheme\0All Files (*.*)\0*.*\0";
        ofn.nFilterIndex = 1;
        ofn.lpstrDefExt = L"qvtheme";
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

        if (isSave) {
            ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
            if (GetSaveFileNameW(&ofn)) return szFile;
        } else {
            if (GetOpenFileNameW(&ofn)) return szFile;
        }
        return L"";
    }

    // Helper to write float values
    static void WriteFloat(const wchar_t* section, const wchar_t* key, float value, const std::wstring& path) {
        wchar_t buffer[32];
        swprintf_s(buffer, L"%.6f", value);
        WritePrivateProfileStringW(section, key, buffer, path.c_str());
    }

    // Helper to write int values
    static void WriteInt(const wchar_t* section, const wchar_t* key, int value, const std::wstring& path) {
        wchar_t buffer[32];
        swprintf_s(buffer, L"%d", value);
        WritePrivateProfileStringW(section, key, buffer, path.c_str());
    }

    // Helper to write bool values
    static void WriteBool(const wchar_t* section, const wchar_t* key, bool value, const std::wstring& path) {
        WritePrivateProfileStringW(section, key, value ? L"1" : L"0", path.c_str());
    }

    bool ExportTheme(HWND hwnd, const AppConfig& config) {
        std::wstring path = ShowFileDialog(hwnd, true);
        if (path.empty()) return false;

        const wchar_t* sec = L"Theme";

        WritePrivateProfileStringW(sec, L"version", L"1.0", path.c_str());
        WriteInt(sec, L"theme_mode", config.ThemeMode, path);
        WriteBool(sec, L"glass_enabled", config.EnableGeekGlass, path);
        WriteBool(sec, L"animations", config.GlassUIAnimations, path);
        WriteFloat(sec, L"blur", config.GlassBlurSigma, path);
        WriteFloat(sec, L"tint_alpha", config.GlassTintAlpha, path);
        WriteFloat(sec, L"specular", config.GlassSpecularOpacity, path);
        WriteFloat(sec, L"shadow", config.GlassShadowOpacity, path);
        
        WriteFloat(sec, L"density_osd", config.GlassOsdOpacity, path);
        WriteFloat(sec, L"density_panels", config.GlassPanelsOpacity, path);
        WriteFloat(sec, L"density_modals", config.GlassModalsOpacity, path);
        WriteFloat(sec, L"density_menus", config.GlassMenusOpacity, path);
        
        WriteInt(sec, L"stroke_weight", config.GlassVectorStrokeWeightIndex, path);
        WriteInt(sec, L"tint_profile", config.GlassTintProfile, path);
        
        WriteFloat(sec, L"tint_color_r", config.GlassCustomTintR, path);
        WriteFloat(sec, L"tint_color_g", config.GlassCustomTintG, path);
        WriteFloat(sec, L"tint_color_b", config.GlassCustomTintB, path);

        WriteFloat(sec, L"accent_color_r", config.ThemeCustomAccentR, path);
        WriteFloat(sec, L"accent_color_g", config.ThemeCustomAccentG, path);
        WriteFloat(sec, L"accent_color_b", config.ThemeCustomAccentB, path);

        WriteFloat(sec, L"text_color_r", config.ThemeCustomTextR, path);
        WriteFloat(sec, L"text_color_g", config.ThemeCustomTextG, path);
        WriteFloat(sec, L"text_color_b", config.ThemeCustomTextB, path);

        WriteInt(sec, L"canvas_color", config.CanvasColor, path);
        WriteFloat(sec, L"canvas_custom_r", config.CanvasCustomR, path);
        WriteFloat(sec, L"canvas_custom_g", config.CanvasCustomG, path);
        WriteFloat(sec, L"canvas_custom_b", config.CanvasCustomB, path);

        return true;
    }

    // Helper to read float values
    static float ReadFloat(const wchar_t* section, const wchar_t* key, float defaultValue, const std::wstring& path) {
        wchar_t buffer[32];
        if (GetPrivateProfileStringW(section, key, L"", buffer, 32, path.c_str())) {
            return std::wcstof(buffer, nullptr);
        }
        return defaultValue;
    }

    // Helper to read int values
    static int ReadInt(const wchar_t* section, const wchar_t* key, int defaultValue, const std::wstring& path) {
        return GetPrivateProfileIntW(section, key, defaultValue, path.c_str());
    }

    // Helper to read bool values
    static bool ReadBool(const wchar_t* section, const wchar_t* key, bool defaultValue, const std::wstring& path) {
        return GetPrivateProfileIntW(section, key, defaultValue ? 1 : 0, path.c_str()) != 0;
    }

    bool ImportTheme(HWND hwnd, AppConfig& config) {
        std::wstring path = ShowFileDialog(hwnd, false);
        if (path.empty()) return false;

        const wchar_t* sec = L"Theme";

        // Optional: Check version or if it's a valid theme file
        wchar_t versionBuf[32];
        if (!GetPrivateProfileStringW(sec, L"version", L"", versionBuf, 32, path.c_str())) {
             // Basic check to see if it's an INI
        }

        config.ThemeMode = ReadInt(sec, L"theme_mode", config.ThemeMode, path);
        config.EnableGeekGlass = ReadBool(sec, L"glass_enabled", config.EnableGeekGlass, path);
        config.GlassUIAnimations = ReadBool(sec, L"animations", config.GlassUIAnimations, path);
        config.GlassBlurSigma = ReadFloat(sec, L"blur", config.GlassBlurSigma, path);
        config.GlassTintAlpha = ReadFloat(sec, L"tint_alpha", config.GlassTintAlpha, path);
        config.GlassSpecularOpacity = ReadFloat(sec, L"specular", config.GlassSpecularOpacity, path);
        config.GlassShadowOpacity = ReadFloat(sec, L"shadow", config.GlassShadowOpacity, path);
        
        config.GlassOsdOpacity = ReadFloat(sec, L"density_osd", config.GlassOsdOpacity, path);
        config.GlassPanelsOpacity = ReadFloat(sec, L"density_panels", config.GlassPanelsOpacity, path);
        config.GlassModalsOpacity = ReadFloat(sec, L"density_modals", config.GlassModalsOpacity, path);
        config.GlassMenusOpacity = ReadFloat(sec, L"density_menus", config.GlassMenusOpacity, path);
        
        config.GlassVectorStrokeWeightIndex = ReadInt(sec, L"stroke_weight", config.GlassVectorStrokeWeightIndex, path);
        config.GlassTintProfile = ReadInt(sec, L"tint_profile", config.GlassTintProfile, path);
        
        config.GlassCustomTintR = ReadFloat(sec, L"tint_color_r", config.GlassCustomTintR, path);
        config.GlassCustomTintG = ReadFloat(sec, L"tint_color_g", config.GlassCustomTintG, path);
        config.GlassCustomTintB = ReadFloat(sec, L"tint_color_b", config.GlassCustomTintB, path);

        config.ThemeCustomAccentR = ReadFloat(sec, L"accent_color_r", config.ThemeCustomAccentR, path);
        config.ThemeCustomAccentG = ReadFloat(sec, L"accent_color_g", config.ThemeCustomAccentG, path);
        config.ThemeCustomAccentB = ReadFloat(sec, L"accent_color_b", config.ThemeCustomAccentB, path);

        config.ThemeCustomTextR = ReadFloat(sec, L"text_color_r", config.ThemeCustomTextR, path);
        config.ThemeCustomTextG = ReadFloat(sec, L"text_color_g", config.ThemeCustomTextG, path);
        config.ThemeCustomTextB = ReadFloat(sec, L"text_color_b", config.ThemeCustomTextB, path);

        config.CanvasColor = ReadInt(sec, L"canvas_color", config.CanvasColor, path);
        config.CanvasCustomR = ReadFloat(sec, L"canvas_custom_r", config.CanvasCustomR, path);
        config.CanvasCustomG = ReadFloat(sec, L"canvas_custom_g", config.CanvasCustomG, path);
        config.CanvasCustomB = ReadFloat(sec, L"canvas_custom_b", config.CanvasCustomB, path);

        config.EnforceGlassSafetyLimits();
        ::SaveConfig(); // Persist to QuickView.ini as requested
        return true;
    }
}
