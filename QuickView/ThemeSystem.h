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

namespace QuickView::UI::ConfigIO {

    /// Exports the live QuickView.ini to a user-chosen path.
    bool ExportConfig(HWND hwnd);

    /// Imports a QuickView.ini over the live config and reloads it.
    /// PortableMode of the current install is preserved.
    bool ImportConfig(HWND hwnd);

}
