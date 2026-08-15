#include "pch.h"
#include "ThemeSystem.h"
#include "yyjson.h"
#include <commdlg.h>
#include <cstring>
#include <shlwapi.h>

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shlwapi.lib")

extern AppConfig g_config;
extern void SaveConfig(); // Defined in main.cpp

namespace {

    std::wstring ShowFileDialog(HWND hwnd, bool isSave, const wchar_t* filter, const wchar_t* defExt, const wchar_t* defaultName = nullptr) {
        wchar_t szFile[MAX_PATH] = { 0 };
        if (isSave && defaultName && defaultName[0]) {
            wcsncpy_s(szFile, defaultName, _TRUNCATE);
        }
        OPENFILENAMEW ofn = {}; ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd;
        ofn.lpstrFile = szFile;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrFilter = filter;
        ofn.nFilterIndex = 1;
        ofn.lpstrDefExt = defExt;
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

        if (isSave) {
            ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
            if (GetSaveFileNameW(&ofn)) return szFile;
        } else {
            if (GetOpenFileNameW(&ofn)) return szFile;
        }
        return L"";
    }

    bool LooksLikeQuickViewIni(const std::wstring& path) {
        HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) return false;

        char buf[4096] = {};
        DWORD bytesRead = 0;
        BOOL ok = ReadFile(hFile, buf, sizeof(buf) - 1, &bytesRead, nullptr);
        CloseHandle(hFile);
        if (!ok || bytesRead < 4) return false;

        size_t i = 0;
        bool utf16le = false;
        if (bytesRead >= 3 &&
            static_cast<unsigned char>(buf[0]) == 0xEF &&
            static_cast<unsigned char>(buf[1]) == 0xBB &&
            static_cast<unsigned char>(buf[2]) == 0xBF) {
            i = 3;
        } else if (bytesRead >= 2 &&
            static_cast<unsigned char>(buf[0]) == 0xFF &&
            static_cast<unsigned char>(buf[1]) == 0xFE) {
            utf16le = true;
            i = 2;
        }

        auto skipWs = [&]() {
            if (utf16le) {
                while (i + 1 < bytesRead) {
                    wchar_t ch = static_cast<unsigned char>(buf[i]) |
                                 (static_cast<wchar_t>(static_cast<unsigned char>(buf[i + 1])) << 8);
                    if (ch != L' ' && ch != L'\t' && ch != L'\r' && ch != L'\n') break;
                    i += 2;
                }
            } else {
                while (i < bytesRead && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r' || buf[i] == '\n')) ++i;
            }
        };
        skipWs();

        if (utf16le) {
            if (i + 1 < bytesRead) {
                wchar_t ch = static_cast<unsigned char>(buf[i]) |
                             (static_cast<wchar_t>(static_cast<unsigned char>(buf[i + 1])) << 8);
                if (ch == L'{') return false;
            }
        } else if (i < bytesRead && buf[i] == '{') {
            return false;
        }

        auto containsAscii = [&](const char* needle) -> bool {
            const size_t n = strlen(needle);
            if (n == 0 || n > bytesRead) return false;
            for (size_t p = 0; p + n <= bytesRead; ++p) {
                if (memcmp(buf + p, needle, n) == 0) return true;
            }
            return false;
        };
        auto containsUtf16Le = [&](const wchar_t* needle) -> bool {
            const size_t n = wcslen(needle);
            if (n == 0 || (n * 2) > bytesRead) return false;
            for (size_t p = 0; p + n * 2 <= bytesRead; p += 2) {
                bool match = true;
                for (size_t k = 0; k < n; ++k) {
                    wchar_t ch = static_cast<unsigned char>(buf[p + k * 2]) |
                                 (static_cast<wchar_t>(static_cast<unsigned char>(buf[p + k * 2 + 1])) << 8);
                    if (ch != needle[k]) { match = false; break; }
                }
                if (match) return true;
            }
            return false;
        };

        return containsAscii("[General]") || containsAscii("[Theme]") || containsAscii("[View]") ||
               containsUtf16Le(L"[General]") || containsUtf16Le(L"[Theme]") || containsUtf16Le(L"[View]");
    }

}

namespace QuickView::UI::ThemeSystem {

    bool ExportTheme(HWND hwnd, const AppConfig& config) {
        std::wstring path = ShowFileDialog(hwnd, true,
            L"QuickView Theme (*.qvtheme)\0*.qvtheme\0All Files (*.*)\0*.*\0", L"qvtheme");
        if (path.empty()) return false;

        yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *root = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, root);

        yyjson_mut_obj_add_real(doc, root, "version", 1.0);
        yyjson_mut_obj_add_int(doc, root, "theme_mode", config.ThemeMode);
        yyjson_mut_obj_add_int(doc, root, "menu_backdrop_style", config.MenuBackdropStyle);
        yyjson_mut_obj_add_bool(doc, root, "glass_enabled", config.EnableGeekGlass);
        yyjson_mut_obj_add_bool(doc, root, "animations", config.GlassUIAnimations);
        yyjson_mut_obj_add_real(doc, root, "blur", config.GlassBlurSigma);
        yyjson_mut_obj_add_real(doc, root, "tint_alpha", config.GlassTintAlpha);
        yyjson_mut_obj_add_real(doc, root, "specular", config.GlassSpecularOpacity);
        yyjson_mut_obj_add_real(doc, root, "shadow", config.GlassShadowOpacity);
        
        yyjson_mut_obj_add_real(doc, root, "density_osd", config.GlassOsdOpacity);
        yyjson_mut_obj_add_real(doc, root, "density_panels", config.GlassPanelsOpacity);
        yyjson_mut_obj_add_real(doc, root, "density_modals", config.GlassModalsOpacity);
        yyjson_mut_obj_add_real(doc, root, "density_menus", config.GlassMenusOpacity);
        
        yyjson_mut_obj_add_int(doc, root, "stroke_weight", config.GlassVectorStrokeWeightIndex);
        yyjson_mut_obj_add_int(doc, root, "tint_profile", config.GlassTintProfile);
        
        auto add_color = [&](const char* key, float r, float g, float b) {
            yyjson_mut_val *arr = yyjson_mut_arr(doc);
            yyjson_mut_arr_add_real(doc, arr, r);
            yyjson_mut_arr_add_real(doc, arr, g);
            yyjson_mut_arr_add_real(doc, arr, b);
            yyjson_mut_obj_add_val(doc, root, key, arr);
        };

        add_color("tint_color", config.GlassCustomTintR, config.GlassCustomTintG, config.GlassCustomTintB);
        add_color("accent_color", config.ThemeCustomAccentR, config.ThemeCustomAccentG, config.ThemeCustomAccentB);
        add_color("text_color", config.ThemeCustomTextR, config.ThemeCustomTextG, config.ThemeCustomTextB);

        yyjson_mut_obj_add_int(doc, root, "canvas_color", config.CanvasColor);
        yyjson_mut_obj_add_int(doc, root, "canvas_effect_style", config.CanvasEffectStyle);
        add_color("canvas_custom", config.CanvasCustomR, config.CanvasCustomG, config.CanvasCustomB);

        size_t len;
        char *json = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY, &len);
        if (json) {
            HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hFile != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                BOOL res = WriteFile(hFile, json, static_cast<DWORD>(len), &written, nullptr);
                CloseHandle(hFile);
                free(json);
                yyjson_mut_doc_free(doc);
                return res && (written == len);
            }
            free(json);
        }
        yyjson_mut_doc_free(doc);
        return false;
    }

    bool ImportTheme(HWND hwnd, AppConfig& config) {
        std::wstring path = ShowFileDialog(hwnd, false,
            L"QuickView Theme (*.qvtheme)\0*.qvtheme\0All Files (*.*)\0*.*\0", L"qvtheme");
        if (path.empty()) return false;

        HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) return false;

        LARGE_INTEGER fileSize;
        if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart <= 0) {
            CloseHandle(hFile);
            return false;
        }

        std::string json_str(static_cast<size_t>(fileSize.QuadPart), '\0');
        DWORD bytesRead = 0;
        BOOL success = ReadFile(hFile, json_str.data(), static_cast<DWORD>(fileSize.QuadPart), &bytesRead, nullptr);
        CloseHandle(hFile);

        if (!success || bytesRead != fileSize.QuadPart) return false;

        yyjson_doc *doc = yyjson_read(json_str.c_str(), json_str.size(), 0);
        if (!doc) return false;

        yyjson_val *obj = yyjson_doc_get_root(doc);
        if (!yyjson_is_obj(obj)) {
            yyjson_doc_free(doc);
            return false;
        }

        // Safe extraction helpers
        auto get_double = [&](const char* key, float& out) {
            yyjson_val *v = yyjson_obj_get(obj, key);
            if (v && yyjson_is_num(v)) out = (float)yyjson_get_num(v);
        };
        auto get_int = [&](const char* key, int& out) {
            yyjson_val *v = yyjson_obj_get(obj, key);
            if (v && yyjson_is_num(v)) out = (int)yyjson_get_num(v);
        };
        auto get_bool = [&](const char* key, bool& out) {
            yyjson_val *v = yyjson_obj_get(obj, key);
            if (v && yyjson_is_bool(v)) out = yyjson_get_bool(v);
        };
        auto get_color = [&](const char* key, float& r, float& g, float& b) {
            yyjson_val *arr = yyjson_obj_get(obj, key);
            if (arr && yyjson_is_arr(arr) && yyjson_arr_size(arr) >= 3) {
                r = (float)yyjson_get_num(yyjson_arr_get(arr, 0));
                g = (float)yyjson_get_num(yyjson_arr_get(arr, 1));
                b = (float)yyjson_get_num(yyjson_arr_get(arr, 2));
            }
        };

        get_int("theme_mode", config.ThemeMode);
        get_int("menu_backdrop_style", config.MenuBackdropStyle);
        get_bool("glass_enabled", config.EnableGeekGlass);
        get_bool("animations", config.GlassUIAnimations);
        get_double("blur", config.GlassBlurSigma);
        get_double("tint_alpha", config.GlassTintAlpha);
        get_double("specular", config.GlassSpecularOpacity);
        get_double("shadow", config.GlassShadowOpacity);
        
        get_double("density_osd", config.GlassOsdOpacity);
        get_double("density_panels", config.GlassPanelsOpacity);
        get_double("density_modals", config.GlassModalsOpacity);
        get_double("density_menus", config.GlassMenusOpacity);
        
        get_int("stroke_weight", config.GlassVectorStrokeWeightIndex);
        get_int("tint_profile", config.GlassTintProfile);
        
        get_color("tint_color", config.GlassCustomTintR, config.GlassCustomTintG, config.GlassCustomTintB);
        get_color("accent_color", config.ThemeCustomAccentR, config.ThemeCustomAccentG, config.ThemeCustomAccentB);
        get_color("text_color", config.ThemeCustomTextR, config.ThemeCustomTextG, config.ThemeCustomTextB);

        get_int("canvas_color", config.CanvasColor);
        get_int("canvas_effect_style", config.CanvasEffectStyle);
        get_color("canvas_custom", config.CanvasCustomR, config.CanvasCustomG, config.CanvasCustomB);

        config.EnforceGlassSafetyLimits();
        ::SaveConfig(); // Persist to QuickView.ini as requested
        yyjson_doc_free(doc);
        return true;
    }
}

namespace QuickView::UI::ConfigIO {

    bool ExportConfig(HWND hwnd) {
        ::SaveConfig();
        std::wstring dest = ShowFileDialog(hwnd, true,
            L"QuickView Config (*.ini)\0*.ini\0All Files (*.*)\0*.*\0", L"ini", L"QuickView.ini");
        if (dest.empty()) return false;

        std::wstring src = GetConfigPath();
        if (src.empty() || GetFileAttributesW(src.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
        if (_wcsicmp(src.c_str(), dest.c_str()) == 0) return true;
        return CopyFileW(src.c_str(), dest.c_str(), FALSE) != FALSE;
    }

    bool ImportConfig(HWND hwnd) {
        std::wstring src = ShowFileDialog(hwnd, false,
            L"QuickView Config (*.ini)\0*.ini\0All Files (*.*)\0*.*\0", L"ini");
        if (src.empty()) return false;
        if (!LooksLikeQuickViewIni(src)) return false;

        const bool portable = g_config.PortableMode;
        std::wstring dest = GetConfigPath();
        if (dest.empty()) return false;

        if (_wcsicmp(src.c_str(), dest.c_str()) != 0) {
            if (!CopyFileW(src.c_str(), dest.c_str(), FALSE)) return false;
        }

        LoadConfig();
        g_config.PortableMode = portable;
        g_config.EnforceGlassSafetyLimits();
        ::SaveConfig();
        return true;
    }

}
