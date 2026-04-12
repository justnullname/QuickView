#include "pch.h"
#include "ThemeSystem.h"
#include "picojson.h"
#include <fstream>
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

    bool ExportTheme(HWND hwnd, const AppConfig& config) {
        std::wstring path = ShowFileDialog(hwnd, true);
        if (path.empty()) return false;

        picojson::object theme;
        theme["version"] = picojson::value(1.0);
        theme["theme_mode"] = picojson::value((double)config.ThemeMode);
        theme["glass_enabled"] = picojson::value(config.EnableGeekGlass);
        theme["animations"] = picojson::value(config.GlassUIAnimations);
        theme["blur"] = picojson::value((double)config.GlassBlurSigma);
        theme["tint_alpha"] = picojson::value((double)config.GlassTintAlpha);
        theme["specular"] = picojson::value((double)config.GlassSpecularOpacity);
        theme["shadow"] = picojson::value((double)config.GlassShadowOpacity);
        
        theme["density_osd"] = picojson::value((double)config.GlassOsdOpacity);
        theme["density_panels"] = picojson::value((double)config.GlassPanelsOpacity);
        theme["density_modals"] = picojson::value((double)config.GlassModalsOpacity);
        theme["density_menus"] = picojson::value((double)config.GlassMenusOpacity);
        
        theme["stroke_weight"] = picojson::value((double)config.GlassVectorStrokeWeightIndex);
        theme["tint_profile"] = picojson::value((double)config.GlassTintProfile);
        
        picojson::array tint;
        tint.push_back(picojson::value((double)config.GlassCustomTintR));
        tint.push_back(picojson::value((double)config.GlassCustomTintG));
        tint.push_back(picojson::value((double)config.GlassCustomTintB));
        theme["tint_color"] = picojson::value(tint);

        picojson::array accent;
        accent.push_back(picojson::value((double)config.ThemeCustomAccentR));
        accent.push_back(picojson::value((double)config.ThemeCustomAccentG));
        accent.push_back(picojson::value((double)config.ThemeCustomAccentB));
        theme["accent_color"] = picojson::value(accent);

        picojson::array text;
        text.push_back(picojson::value((double)config.ThemeCustomTextR));
        text.push_back(picojson::value((double)config.ThemeCustomTextG));
        text.push_back(picojson::value((double)config.ThemeCustomTextB));
        theme["text_color"] = picojson::value(text);

        theme["canvas_color"] = picojson::value((double)config.CanvasColor);
        picojson::array canvas;
        canvas.push_back(picojson::value((double)config.CanvasCustomR));
        canvas.push_back(picojson::value((double)config.CanvasCustomG));
        canvas.push_back(picojson::value((double)config.CanvasCustomB));
        theme["canvas_custom"] = picojson::value(canvas);

        std::string json = picojson::value(theme).serialize(true);
        std::ofstream ofs(path);
        if (ofs.is_open()) {
            ofs << json;
            ofs.close();
            return true;
        }
        return false;
    }

    bool ImportTheme(HWND hwnd, AppConfig& config) {
        std::wstring path = ShowFileDialog(hwnd, false);
        if (path.empty()) return false;

        std::ifstream ifs(path);
        if (!ifs.is_open()) return false;

        picojson::value v;
        std::string err = picojson::parse(v, ifs);
        if (!err.empty()) return false;

        if (!v.is<picojson::object>()) return false;
        auto& obj = v.get<picojson::object>();

        // Safe extraction helpers
        auto get_double = [&](const std::string& key, float& out) {
            if (obj.count(key) && obj.at(key).is<double>()) out = (float)obj.at(key).get<double>();
        };
        auto get_int = [&](const std::string& key, int& out) {
            if (obj.count(key) && obj.at(key).is<double>()) out = (int)obj.at(key).get<double>();
        };
        auto get_bool = [&](const std::string& key, bool& out) {
            if (obj.count(key) && obj.at(key).is<bool>()) out = obj.at(key).get<bool>();
        };
        auto get_color = [&](const std::string& key, float& r, float& g, float& b) {
            if (obj.count(key) && obj.at(key).is<picojson::array>()) {
                auto& arr = obj.at(key).get<picojson::array>();
                if (arr.size() >= 3) {
                    r = (float)arr[0].get<double>();
                    g = (float)arr[1].get<double>();
                    b = (float)arr[2].get<double>();
                }
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
        return true;
    }
}
