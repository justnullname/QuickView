#include "pch.h"
#include "ThemeSystem.h"
#include "yyjson.h"
#include <fstream>
#include <commdlg.h>
#include <shlwapi.h>

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shlwapi.lib")

extern void SaveConfig(); // Defined in main.cpp
namespace QuickView::UI::ThemeSystem {

    static std::wstring ShowFileDialog(HWND hwnd, bool isSave) {
        wchar_t szFile[MAX_PATH] = { 0 };
        OPENFILENAMEW ofn = {}; ofn.lStructSize = sizeof(ofn);
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

    bool ExportTheme(HWND hwnd, const AppConfig& config) {
        std::wstring path = ShowFileDialog(hwnd, true);
        if (path.empty()) return false;

        yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *root = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, root);

        yyjson_mut_obj_add_real(doc, root, "version", 1.0);
        yyjson_mut_obj_add_int(doc, root, "theme_mode", (int)config.ThemeMode);
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
        add_color("canvas_custom", config.CanvasCustomR, config.CanvasCustomG, config.CanvasCustomB);

        size_t len;
        char *json = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY, &len);
        if (json) {
            std::ofstream ofs(path);
            if (ofs.is_open()) {
                ofs.write(json, len);
                ofs.close();
                free(json);
                yyjson_mut_doc_free(doc);
                return true;
            }
            free(json);
        }
        yyjson_mut_doc_free(doc);
        return false;
    }

    bool ImportTheme(HWND hwnd, AppConfig& config) {
        std::wstring path = ShowFileDialog(hwnd, false);
        if (path.empty()) return false;

        std::ifstream ifs(path, std::ios::binary);
        if (!ifs.is_open()) return false;
        std::string json_str((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

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
        get_color("canvas_custom", config.CanvasCustomR, config.CanvasCustomG, config.CanvasCustomB);

        config.EnforceGlassSafetyLimits();
        ::SaveConfig(); // Persist to QuickView.ini as requested
        yyjson_doc_free(doc);
        return true;
    }
}
