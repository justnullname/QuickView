#pragma once
#include "EditState.h"
#include <string>

namespace QuickView::UI::ThemeSystem {

    /// <summary>
    /// Exports current theme settings to a .qvtheme file (JSON).
    /// </summary>
    bool ExportTheme(HWND hwnd, const AppConfig& config);

    /// <summary>
    /// Imports theme settings from a .qvtheme file and saves to quickview.ini.
    /// Returns true if configuration was modified and needs refresh.
    /// </summary>
    bool ImportTheme(HWND hwnd, AppConfig& config);

}
