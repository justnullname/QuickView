// ============================================================
// GeekIconLibrary.cpp - Unified 1px Ultra-Thin Vector Icon Library
// ============================================================
// All icons drawn with ID2D1PathGeometry for crisp arcs/beziers.
// Design unit: 16x16 logical, scaled by bounding rect width.
// ============================================================

#include "pch.h"
#include "GeekIconLibrary.h"
#include <d2d1_1.h>
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace GeekIcons {

// ============================================================
// Internal Helpers
// ============================================================
static constexpr float PI = 3.14159265f;

// Center + unit scale from bounding rect
static void CxCyS(const D2D1_RECT_F& r, float& cx, float& cy, float& s) {
    cx = (r.left + r.right) / 2;
    cy = (r.top + r.bottom) / 2;
    s = (r.right - r.left) / 16.0f;
}

// Get D2D factory from render target (caller must Release)
static ID2D1Factory* GetFactory(ID2D1RenderTarget* rt) {
    ID2D1Factory* f = nullptr;
    rt->GetFactory(&f);
    return f;
}

// ============================================================
// File Operations
// ============================================================

void Open(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, ID2D1Brush* b, float sw) {
    float cx, cy, s; CxCyS(r, cx, cy, s);
    auto* f = GetFactory(rt);
    if (!f) return;
    ComPtr<ID2D1PathGeometry> geo;
    f->CreatePathGeometry(&geo); f->Release();
    if (!geo) return;
    ComPtr<ID2D1GeometrySink> sink;
    geo->Open(&sink);
    // Folder body with tab
    sink->BeginFigure({cx-6*s, cy-2*s}, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLine({cx-6*s, cy+5*s}); sink->AddLine({cx+6*s, cy+5*s}); sink->AddLine({cx+6*s, cy-2*s});
    sink->AddLine({cx+1*s, cy-2*s}); sink->AddLine({cx, cy-5*s}); sink->AddLine({cx-3*s, cy-5*s});
    sink->AddLine({cx-6*s, cy-2*s});
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    // Arrow up
    sink->BeginFigure({cx, cy+3*s}, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLine({cx, cy-1*s});
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->BeginFigure({cx-2*s, cy+1*s}, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLine({cx, cy-1*s}); sink->AddLine({cx+2*s, cy+1*s});
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    rt->DrawGeometry(geo.Get(), b, sw);
}

void Rename(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, ID2D1Brush* b, float sw) {
    float cx, cy, s; CxCyS(r, cx, cy, s);
    auto* f = GetFactory(rt);
    if (!f) return;
    ComPtr<ID2D1PathGeometry> geo;
    f->CreatePathGeometry(&geo); f->Release();
    if (!geo) return;
    ComPtr<ID2D1GeometrySink> sink;
    geo->Open(&sink);
    // Pen body (diagonal rectangle)
    float px = -4*s, py = 4*s;
    float dx = 2*s, dy = -2*s;
    sink->BeginFigure({cx+px+dx*0.4f, cy+py+dy*0.4f}, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLine({cx+5*s+dx*0.4f, cy-5*s+dy*0.4f});
    sink->AddLine({cx+5*s-dx*0.4f, cy-5*s-dy*0.4f});
    sink->AddLine({cx+px-dx*0.4f, cy+py-dy*0.4f});
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    // Pen tip
    sink->BeginFigure({cx+px+dx*0.4f, cy+py+dy*0.4f}, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLine({cx-5.5f*s, cy+6*s});
    sink->AddLine({cx+px-dx*0.4f, cy+py-dy*0.4f});
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    // Top cap (eraser)
    sink->BeginFigure({cx+5*s+dx*0.4f, cy-5*s+dy*0.4f}, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLine({cx+6*s+dx*0.15f, cy-6.5f*s+dy*0.15f});
    sink->AddLine({cx+6*s-dx*0.4f, cy-6.5f*s-dy*0.4f});
    sink->AddLine({cx+5*s-dx*0.4f, cy-5*s-dy*0.4f});
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    rt->DrawGeometry(geo.Get(), b, sw);
}

void Edit(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, ID2D1Brush* b, float sw) {
    float cx, cy, s; CxCyS(r, cx, cy, s);
    auto* f = GetFactory(rt);
    if (!f) return;
    ComPtr<ID2D1PathGeometry> geo;
    f->CreatePathGeometry(&geo); f->Release();
    if (!geo) return;
    ComPtr<ID2D1GeometrySink> sink;
    geo->Open(&sink);
    // Pencil body
    sink->BeginFigure({cx-4*s, cy+3.5f*s}, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLine({cx+3.5f*s, cy-4*s});
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->BeginFigure({cx-3*s, cy+4.5f*s}, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLine({cx+4.5f*s, cy-3*s});
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    // Tip
    sink->BeginFigure({cx-4*s, cy+3.5f*s}, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLine({cx-6*s, cy+6.5f*s});
    sink->AddLine({cx-3*s, cy+4.5f*s});
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    // Cap
    sink->BeginFigure({cx+3.5f*s, cy-4*s}, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLine({cx+5*s, cy-6*s});
    sink->AddLine({cx+6*s, cy-5*s});
    sink->AddLine({cx+4.5f*s, cy-3*s});
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    // Paper baseline
    sink->BeginFigure({cx-2*s, cy+7*s}, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLine({cx+6*s, cy+7*s});
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    rt->DrawGeometry(geo.Get(), b, sw);
}

void Delete(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, ID2D1Brush* b, float sw) {
    float cx, cy, s; CxCyS(r, cx, cy, s);
    auto* f = GetFactory(rt);
    if (!f) return;
    ComPtr<ID2D1PathGeometry> geo;
    f->CreatePathGeometry(&geo); f->Release();
    if (!geo) return;
    ComPtr<ID2D1GeometrySink> sink;
    geo->Open(&sink);
    // Lid
    sink->BeginFigure({cx-6*s, cy-4*s}, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLine({cx+6*s, cy-4*s});
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    // Handle
    sink->BeginFigure({cx-2*s, cy-4*s}, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLine({cx-2*s, cy-6*s}); sink->AddLine({cx+2*s, cy-6*s});
    sink->AddLine({cx+2*s, cy-4*s});
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    // Body (tapered)
    sink->BeginFigure({cx-5*s, cy-3*s}, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLine({cx-4*s, cy+6*s}); sink->AddLine({cx+4*s, cy+6*s});
    sink->AddLine({cx+5*s, cy-3*s});
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    // Inner lines
    sink->BeginFigure({cx, cy-2*s}, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLine({cx, cy+4*s});
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->BeginFigure({cx-2*s, cy-2*s}, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLine({cx-1.5f*s, cy+4*s});
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->BeginFigure({cx+2*s, cy-2*s}, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLine({cx+1.5f*s, cy+4*s});
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    rt->DrawGeometry(geo.Get(), b, sw);
}

void OpenWith(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, ID2D1Brush* b, float sw) {
    float cx, cy, s; CxCyS(r, cx, cy, s);
    for (int row = -1; row <= 1; row++)
        for (int col = -1; col <= 1; col++)
            rt->FillEllipse(D2D1::Ellipse({cx + col*5*s, cy + row*5*s}, 1.2f*s, 1.2f*s), b);
}

void Copy(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, ID2D1Brush* b, float sw) {
    float cx, cy, s; CxCyS(r, cx, cy, s);
    rt->DrawRoundedRectangle(D2D1::RoundedRect(
        D2D1::RectF(cx-5*s, cy-5*s, cx+2*s, cy+2*s), 1.5f*s, 1.5f*s), b, sw);
    rt->DrawRoundedRectangle(D2D1::RoundedRect(
        D2D1::RectF(cx-2*s, cy-2*s, cx+5*s, cy+5*s), 1.5f*s, 1.5f*s), b, sw);
}

void Explorer(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, ID2D1Brush* b, float sw) {
    float cx, cy, s; CxCyS(r, cx, cy, s);
    auto* f = GetFactory(rt);
    if (!f) return;
    ComPtr<ID2D1PathGeometry> geo;
    f->CreatePathGeometry(&geo); f->Release();
    if (!geo) return;
    ComPtr<ID2D1GeometrySink> sink;
    geo->Open(&sink);
    // Folder with right arrow
    sink->BeginFigure({cx-6*s, cy-2*s}, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLine({cx-6*s, cy+5*s}); sink->AddLine({cx+3*s, cy+5*s});
    sink->AddLine({cx+3*s, cy-2*s});
    sink->AddLine({cx, cy-2*s}); sink->AddLine({cx-1*s, cy-4*s});
    sink->AddLine({cx-4*s, cy-4*s}); sink->AddLine({cx-6*s, cy-2*s});
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->BeginFigure({cx+2*s, cy+1*s}, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLine({cx+7*s, cy+1*s});
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->BeginFigure({cx+5*s, cy-1*s}, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLine({cx+7*s, cy+1*s}); sink->AddLine({cx+5*s, cy+3*s});
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    rt->DrawGeometry(geo.Get(), b, sw);
}

void Folder(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, ID2D1Brush* b, float sw) {
    float cx, cy, s; CxCyS(r, cx, cy, s);
    auto* f = GetFactory(rt);
    if (!f) return;
    ComPtr<ID2D1PathGeometry> geo;
    f->CreatePathGeometry(&geo); f->Release();
    if (!geo) return;
    ComPtr<ID2D1GeometrySink> sink;
    geo->Open(&sink);
    sink->BeginFigure({cx-6*s, cy-2*s}, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLine({cx-6*s, cy+5*s}); sink->AddLine({cx+6*s, cy+5*s});
    sink->AddLine({cx+6*s, cy-2*s});
    sink->AddLine({cx+1*s, cy-2*s}); sink->AddLine({cx, cy-5*s});
    sink->AddLine({cx-3*s, cy-5*s}); sink->AddLine({cx-6*s, cy-2*s});
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    rt->DrawGeometry(geo.Get(), b, sw);
}

void Link(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, ID2D1Brush* b, float sw) {
    float cx, cy, s; CxCyS(r, cx, cy, s);
    rt->DrawRoundedRectangle(D2D1::RoundedRect(
        D2D1::RectF(cx-7*s, cy-2*s, cx-0.5f*s, cy+2*s), 2*s, 2*s), b, sw);
    rt->DrawRoundedRectangle(D2D1::RoundedRect(
        D2D1::RectF(cx+0.5f*s, cy-2*s, cx+7*s, cy+2*s), 2*s, 2*s), b, sw);
}

void Print(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, ID2D1Brush* b, float sw) {
    float cx, cy, s; CxCyS(r, cx, cy, s);
    rt->DrawRoundedRectangle(D2D1::RoundedRect(
        D2D1::RectF(cx-6*s, cy-2*s, cx+6*s, cy+3*s), s, s), b, sw);
    rt->DrawRectangle(D2D1::RectF(cx-4*s, cy-6*s, cx+4*s, cy-2*s), b, sw);
    rt->DrawRectangle(D2D1::RectF(cx-4*s, cy+3*s, cx+4*s, cy+6*s), b, sw);
}

void FixExt(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, ID2D1Brush* b, float sw) {
    float cx, cy, s; CxCyS(r, cx, cy, s);
    auto* f = GetFactory(rt);
    if (!f) return;
    ComPtr<ID2D1PathGeometry> geo;
    f->CreatePathGeometry(&geo); f->Release();
    if (!geo) return;
    ComPtr<ID2D1GeometrySink> sink;
    geo->Open(&sink);
    sink->BeginFigure({cx-5*s, cy+5*s}, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLine({cx+2*s, cy-2*s});
    sink->AddLine({cx+3*s, cy-5*s});
    sink->AddLine({cx+6*s, cy-4*s});
    sink->AddLine({cx+5*s, cy-2*s});
    sink->AddLine({cx+2*s, cy-2*s});
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    rt->DrawGeometry(geo.Get(), b, sw);
}

// ============================================================
// View & Display
// ============================================================

void Eye(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, ID2D1Brush* b, float sw) {
    float cx, cy, s; CxCyS(r, cx, cy, s);
    auto* f = GetFactory(rt);
    if (!f) return;
    ComPtr<ID2D1PathGeometry> geo;
    f->CreatePathGeometry(&geo); f->Release();
    if (!geo) return;
    ComPtr<ID2D1GeometrySink> sink;
    geo->Open(&sink);
    sink->BeginFigure({cx-6*s, cy}, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddBezier({{cx-3*s, cy-4*s}, {cx+3*s, cy-4*s}, {cx+6*s, cy}});
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->BeginFigure({cx-6*s, cy}, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddBezier({{cx-3*s, cy+4*s}, {cx+3*s, cy+4*s}, {cx+6*s, cy}});
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    rt->DrawGeometry(geo.Get(), b, sw);
    rt->FillEllipse(D2D1::Ellipse({cx, cy}, 2*s, 2*s), b);
}

void Info(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, ID2D1Brush* b, float sw) {
    float cx, cy, s; CxCyS(r, cx, cy, s);
    rt->DrawEllipse(D2D1::Ellipse({cx, cy}, 6*s, 6*s), b, sw);
    rt->FillEllipse(D2D1::Ellipse({cx, cy-3*s}, 1*s, 1*s), b);
    rt->DrawLine({cx, cy-1*s}, {cx, cy+4*s}, b, sw);
}

void Compare(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, ID2D1Brush* b, float sw) {
    float cx, cy, s; CxCyS(r, cx, cy, s);
    rt->DrawRectangle(D2D1::RectF(cx-6*s, cy-5*s, cx+6*s, cy+5*s), b, sw);
    rt->DrawLine({cx, cy-5*s}, {cx, cy+5*s}, b, sw);
}

void Wallpaper(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, ID2D1Brush* b, float sw) {
    float cx, cy, s; CxCyS(r, cx, cy, s);
    rt->DrawRectangle(D2D1::RectF(cx-6*s, cy-5*s, cx+6*s, cy+5*s), b, sw);
    rt->DrawLine({cx-4*s, cy+3*s}, {cx-1*s, cy-1*s}, b, sw);
    rt->DrawLine({cx-1*s, cy-1*s}, {cx+1*s, cy+1*s}, b, sw);
    rt->DrawLine({cx+1*s, cy+1*s}, {cx+4*s, cy-2*s}, b, sw);
    rt->DrawEllipse(D2D1::Ellipse({cx+3*s, cy-3*s}, 1.5f*s, 1.5f*s), b, sw);
}

// ============================================================
// Transform
// ============================================================

void Transform(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, ID2D1Brush* b, float sw) {
    float cx, cy, s; CxCyS(r, cx, cy, s);
    auto* f = GetFactory(rt);
    if (!f) return;
    ComPtr<ID2D1PathGeometry> geo;
    f->CreatePathGeometry(&geo); f->Release();
    if (!geo) return;
    ComPtr<ID2D1GeometrySink> sink;
    geo->Open(&sink);
    float rad = 5*s;
    float startAngle = -60.0f * PI / 180.0f;
    float endAngle = 210.0f * PI / 180.0f;
    sink->BeginFigure({cx + rad*cosf(startAngle), cy + rad*sinf(startAngle)}, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(D2D1::ArcSegment(
        {cx + rad*cosf(endAngle), cy + rad*sinf(endAngle)},
        {rad, rad}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    float ex = cx + rad*cosf(endAngle), ey = cy + rad*sinf(endAngle);
    float aLen = 2.5f*s;
    sink->BeginFigure({ex - aLen, ey - aLen*0.5f}, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLine({ex, ey}); sink->AddLine({ex + aLen*0.5f, ey - aLen});
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    rt->DrawGeometry(geo.Get(), b, sw);
}

// ============================================================
// Color & Proofing
// ============================================================

void Color(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, ID2D1Brush* b, float sw) {
    float cx, cy, s; CxCyS(r, cx, cy, s);
    rt->DrawEllipse(D2D1::Ellipse({cx, cy}, 6*s, 6*s), b, sw);
    rt->FillEllipse(D2D1::Ellipse({cx-2.5f*s, cy-1.5f*s}, 1.5f*s, 1.5f*s), b);
    rt->FillEllipse(D2D1::Ellipse({cx+2.5f*s, cy-1.5f*s}, 1.5f*s, 1.5f*s), b);
    rt->FillEllipse(D2D1::Ellipse({cx, cy+2.5f*s}, 1.5f*s, 1.5f*s), b);
}

void SoftProof(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, ID2D1Brush* b, float sw) {
    float cx, cy, s; CxCyS(r, cx, cy, s);
    rt->DrawRoundedRectangle(D2D1::RoundedRect(
        D2D1::RectF(cx-6*s, cy-5*s, cx+6*s, cy+2*s), s, s), b, sw);
    rt->DrawLine({cx, cy+2*s}, {cx, cy+4*s}, b, sw);
    rt->DrawLine({cx-3*s, cy+5*s}, {cx+3*s, cy+5*s}, b, sw);
}

// ============================================================
// Sort & Navigation
// ============================================================

void Sort(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, ID2D1Brush* b, float sw) {
    float cx, cy, s; CxCyS(r, cx, cy, s);
    rt->DrawLine({cx-5*s, cy-4*s}, {cx+5*s, cy-4*s}, b, sw);
    rt->DrawLine({cx-5*s, cy}, {cx+3*s, cy}, b, sw);
    rt->DrawLine({cx-5*s, cy+4*s}, {cx+1*s, cy+4*s}, b, sw);
}

void Navigation(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, ID2D1Brush* b, float sw) {
    float cx, cy, s; CxCyS(r, cx, cy, s);
    rt->DrawLine({cx-3*s, cy-2*s}, {cx, cy-6*s}, b, sw);
    rt->DrawLine({cx+3*s, cy-2*s}, {cx, cy-6*s}, b, sw);
    rt->DrawLine({cx-3*s, cy+2*s}, {cx, cy+6*s}, b, sw);
    rt->DrawLine({cx+3*s, cy+2*s}, {cx, cy+6*s}, b, sw);
    rt->DrawLine({cx, cy-5*s}, {cx, cy+5*s}, b, sw);
}

// ============================================================
// Application
// ============================================================

void Settings(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, ID2D1Brush* b, float sw) {
    float cx, cy, s; CxCyS(r, cx, cy, s);
    rt->DrawEllipse(D2D1::Ellipse({cx, cy}, 2.5f*s, 2.5f*s), b, sw);
    for (int i = 0; i < 8; i++) {
        float a = PI * 2.0f * i / 8;
        rt->DrawLine({cx + 4*s*cosf(a), cy + 4*s*sinf(a)},
                     {cx + 6.5f*s*cosf(a), cy + 6.5f*s*sinf(a)}, b, sw * 1.3f);
    }
}

void About(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, ID2D1Brush* b, float sw) {
    float cx, cy, s; CxCyS(r, cx, cy, s);
    auto* f = GetFactory(rt);
    if (!f) return;
    ComPtr<ID2D1PathGeometry> geo;
    f->CreatePathGeometry(&geo); f->Release();
    if (!geo) return;
    ComPtr<ID2D1GeometrySink> sink;
    geo->Open(&sink);
    // Full circle
    sink->BeginFigure({cx + 6*s, cy}, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(D2D1::ArcSegment(
        {cx - 6*s, cy}, {6*s, 6*s}, 0,
        D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    sink->AddArc(D2D1::ArcSegment(
        {cx + 6*s, cy}, {6*s, 6*s}, 0,
        D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    // Question mark curve (bezier)
    sink->BeginFigure({cx-2*s, cy-3*s}, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddBezier({{cx-2*s, cy-5.5f*s}, {cx+3*s, cy-5.5f*s}, {cx+2*s, cy-2.5f*s}});
    sink->AddBezier({{cx+1.5f*s, cy-1*s}, {cx, cy-1*s}, {cx, cy}});
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    rt->DrawGeometry(geo.Get(), b, sw);
    rt->FillEllipse(D2D1::Ellipse({cx, cy+3*s}, 1*s, 1*s), b);
}

void Exit(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, ID2D1Brush* b, float sw) {
    float cx, cy, s; CxCyS(r, cx, cy, s);
    auto* f = GetFactory(rt);
    if (!f) return;
    ComPtr<ID2D1PathGeometry> geo;
    f->CreatePathGeometry(&geo); f->Release();
    if (!geo) return;
    ComPtr<ID2D1GeometrySink> sink;
    geo->Open(&sink);
    // Power circle: arc from -60° to 240° (open at top)
    float rad = 5.5f * s;
    float startA = -60.0f * PI / 180.0f;
    float endA = 240.0f * PI / 180.0f;
    D2D1_POINT_2F startPt = {cx + rad*cosf(startA), cy + rad*sinf(startA)};
    D2D1_POINT_2F endPt = {cx + rad*cosf(endA), cy + rad*sinf(endA)};
    sink->BeginFigure(startPt, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(D2D1::ArcSegment(
        endPt, {rad, rad}, 0,
        D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    // Power stem
    sink->BeginFigure({cx, cy-6*s}, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLine({cx, cy-0.5f*s});
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    rt->DrawGeometry(geo.Get(), b, sw);
}

// ============================================================
// UI Glyphs
// ============================================================

void Chevron(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, ID2D1Brush* b, float sw) {
    float cx, cy, s; CxCyS(r, cx, cy, s);
    rt->DrawLine({cx-2*s, cy-4*s}, {cx+2*s, cy}, b, sw);
    rt->DrawLine({cx+2*s, cy}, {cx-2*s, cy+4*s}, b, sw);
}

void Check(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, ID2D1Brush* b, float sw) {
    float cx, cy, s; CxCyS(r, cx, cy, s);
    rt->DrawLine({cx-4*s, cy}, {cx-1*s, cy+3*s}, b, sw * 1.3f);
    rt->DrawLine({cx-1*s, cy+3*s}, {cx+4*s, cy-3*s}, b, sw * 1.3f);
}

} // namespace GeekIcons
