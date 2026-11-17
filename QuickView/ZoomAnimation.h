#pragma once

#include <windows.h>
#include <d2d1_2.h>
#include "AppContext.h"

class SmoothZoomController {
public:
    explicit SmoothZoomController(AppContext& context);
    ~SmoothZoomController() = default;

    void Reset();
    void Configure(HWND hwnd,
                   float sourceZoom,
                   float sourcePanX,
                   float sourcePanY,
                   float targetZoom,
                   float targetPanX,
                   float targetPanY,
                   const POINT* anchorScreenPt,
                   bool animateWindow,
                   const RECT* targetWindowRect);
    void SyncToLogical(float winW, float winH, bool activate);
    bool Tick(HWND hwnd);

    void ResolvePan(HWND hwnd, float zoom, float& outPanX, float& outPanY) const;
    bool IsActive() const;
    
private:
    AppContext& m_context;
};

// [Barrier] RAII guard to suppress re-entrant DComp commits during programmatic window resizing
extern bool g_programmaticResize;
extern bool g_deferProgrammaticZoomResizeSync;

struct ProgrammaticResizeScope {
    ProgrammaticResizeScope() noexcept {
        g_programmaticResize = true;
        g_deferProgrammaticZoomResizeSync = true;
    }
    ~ProgrammaticResizeScope() noexcept {
        g_programmaticResize = false;
        g_deferProgrammaticZoomResizeSync = false;
    }
    ProgrammaticResizeScope(const ProgrammaticResizeScope&) = delete;
    ProgrammaticResizeScope& operator=(const ProgrammaticResizeScope&) = delete;
};
