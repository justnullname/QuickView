#pragma once
// ============================================================
// GeekIconLibrary.h - Unified 1px Ultra-Thin Vector Icon Library
// ============================================================
// All icons are self-drawn as 1px-1.5px vector wireframes using
// ID2D1PathGeometry for crisp, DPI-independent rendering.
//
// Usage:  #include "GeekIconLibrary.h"
//         GeekIcons::Open(rt, bounds, brush, strokeWidth);
//
// Design: Each function draws into a 16x16 logical bounding rect,
//         scaled by the width of the provided D2D1_RECT_F.
// ============================================================

#include <d2d1_1.h>
#include <wrl/client.h>

// ============================================================
// Icon Drawing Function Signature
// ============================================================
// Each icon is drawn as a 1px-1.5px ultra-thin vector wireframe
// within the specified bounding rect.
using IconDrawFn = void(*)(ID2D1RenderTarget* rt, const D2D1_RECT_F& bounds,
                           ID2D1Brush* brush, float strokeWidth);

// ============================================================
// Icon Drawing Functions
// ============================================================
namespace GeekIcons {

    // --- File Operations ---
    void Open(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, ID2D1Brush* b, float sw);
    void Rename(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, ID2D1Brush* b, float sw);
    void Edit(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, ID2D1Brush* b, float sw);
    void Delete(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, ID2D1Brush* b, float sw);
    void OpenWith(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, ID2D1Brush* b, float sw);
    void Copy(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, ID2D1Brush* b, float sw);
    void Explorer(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, ID2D1Brush* b, float sw);
    void Folder(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, ID2D1Brush* b, float sw);
    void Link(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, ID2D1Brush* b, float sw);
    void Print(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, ID2D1Brush* b, float sw);
    void FixExt(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, ID2D1Brush* b, float sw);

    // --- View & Display ---
    void Eye(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, ID2D1Brush* b, float sw);
    void Info(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, ID2D1Brush* b, float sw);
    void Compare(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, ID2D1Brush* b, float sw);
    void Wallpaper(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, ID2D1Brush* b, float sw);

    // --- Transform ---
    void Transform(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, ID2D1Brush* b, float sw);

    // --- Color & Proofing ---
    void Color(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, ID2D1Brush* b, float sw);
    void SoftProof(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, ID2D1Brush* b, float sw);

    // --- Sort & Navigation ---
    void Sort(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, ID2D1Brush* b, float sw);
    void Navigation(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, ID2D1Brush* b, float sw);

    // --- Application ---
    void Settings(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, ID2D1Brush* b, float sw);
    void About(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, ID2D1Brush* b, float sw);
    void Exit(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, ID2D1Brush* b, float sw);

    // --- UI Glyphs ---
    void Chevron(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, ID2D1Brush* b, float sw);
    void Check(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, ID2D1Brush* b, float sw);

} // namespace GeekIcons
