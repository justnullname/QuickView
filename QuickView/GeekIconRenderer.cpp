#include "pch.h"
#include "GeekIconLibrary.h"
#include "GeekIconRenderer.h"

#include <unordered_map>
#include <mutex>

namespace QuickView::UI {

// Factory-aware geometry cache.
// ID2D1PathGeometry is bound to the ID2D1Factory that created it.
// We track the factory pointer and invalidate on mismatch.
static ID2D1Factory* s_cachedFactory = nullptr;
static std::unordered_map<const GeekIcons::VectorIcon*, ComPtr<ID2D1PathGeometry>> s_geometryCache;
static std::mutex s_cacheMutex;

static ComPtr<ID2D1PathGeometry> BuildGeometry(ID2D1Factory* factory, const GeekIcons::VectorIcon& icon) {
    ComPtr<ID2D1PathGeometry> geometry;
    if (FAILED(factory->CreatePathGeometry(&geometry))) return nullptr;

    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geometry->Open(&sink))) return nullptr;

    // TrueType glyph outlines use non-zero winding fill:
    // Outer contours wind clockwise, inner contours (holes) wind counter-clockwise.
    // This is CRITICAL for correct rendering of outlined/hollow icon shapes.
    sink->SetFillMode(D2D1_FILL_MODE_WINDING);

    constexpr float InvScale = 1.0f / 30000.0f;
    for (size_t i = 0; i < icon.count; i++) {
        const auto& cmd = icon.commands[i];
        switch (cmd.type) {
            case 'M': sink->BeginFigure({cmd.x1 * InvScale, cmd.y1 * InvScale}, D2D1_FIGURE_BEGIN_FILLED); break;
            case 'L': sink->AddLine({cmd.x1 * InvScale, cmd.y1 * InvScale}); break;
            case 'B': sink->AddBezier({{cmd.x1 * InvScale, cmd.y1 * InvScale}, {cmd.x2 * InvScale, cmd.y2 * InvScale}, {cmd.x3 * InvScale, cmd.y3 * InvScale}}); break;
            case 'Z': sink->EndFigure(D2D1_FIGURE_END_CLOSED); break;
        }
    }
    sink->Close();
    return geometry;
}

void GeekIconRenderer::DrawVectorIcon(
    ID2D1RenderTarget* dc,
    const GeekIcons::VectorIcon& icon,
    const D2D1_RECT_F& rect,
    ID2D1Brush* brush,
    float rotationAngle)
{
    if (!dc || !icon.commands || icon.count == 0 || !brush) return;

    ComPtr<ID2D1Factory> factory;
    dc->GetFactory(&factory);

    ComPtr<ID2D1PathGeometry> geometry;
    {
        std::lock_guard<std::mutex> lock(s_cacheMutex);

        // Invalidate entire cache if factory changed (device recreated)
        if (s_cachedFactory != factory.Get()) {
            s_geometryCache.clear();
            s_cachedFactory = factory.Get();
        }

        auto it = s_geometryCache.find(&icon);
        if (it != s_geometryCache.end()) {
            geometry = it->second;
        } else {
            geometry = BuildGeometry(factory.Get(), icon);
            if (geometry) s_geometryCache[&icon] = geometry;
        }
    }

    if (!geometry) return;

    // Save current transform, compose local scaling on top
    D2D1_MATRIX_3X2_F oldTransform;
    dc->GetTransform(&oldTransform);

    // 1. Rotation first (on unit square)
    D2D1_MATRIX_3X2_F local = D2D1::Matrix3x2F::Rotation(rotationAngle, D2D1::Point2F(0.5f, 0.5f));

    // 2. Then scale to target size
    local = local * D2D1::Matrix3x2F::Scale(rect.right - rect.left, rect.bottom - rect.top);

    // 3. Finally translate to target position
    local = local * D2D1::Matrix3x2F::Translation(rect.left, rect.top);

    dc->SetTransform(local * oldTransform);
    dc->FillGeometry(geometry.Get(), brush);
    dc->SetTransform(oldTransform);
}

} // namespace QuickView::UI
