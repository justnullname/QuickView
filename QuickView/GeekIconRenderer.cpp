#include "pch.h"
#include "GeekIconLibrary.h"
#include "GeekIconRenderer.h"

#include <unordered_map>
#include <mutex>
#include <array>

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
    bool figureOpen = false;
    for (size_t i = 0; i < icon.count; i++) {
        const auto& cmd = icon.commands[i];
        switch (cmd.type) {
            case 'M':
                if (figureOpen) {
                    sink->EndFigure(D2D1_FIGURE_END_OPEN);
                }
                sink->BeginFigure({cmd.x1 * InvScale, cmd.y1 * InvScale}, D2D1_FIGURE_BEGIN_FILLED);
                figureOpen = true;
                break;
            case 'L':
                if (!figureOpen) {
                    sink->BeginFigure({cmd.x1 * InvScale, cmd.y1 * InvScale}, D2D1_FIGURE_BEGIN_FILLED);
                    figureOpen = true;
                } else {
                    sink->AddLine({cmd.x1 * InvScale, cmd.y1 * InvScale});
                }
                break;
            case 'B':
                if (!figureOpen) {
                    sink->BeginFigure({cmd.x1 * InvScale, cmd.y1 * InvScale}, D2D1_FIGURE_BEGIN_FILLED);
                    figureOpen = true;
                }
                sink->AddBezier({{cmd.x1 * InvScale, cmd.y1 * InvScale}, {cmd.x2 * InvScale, cmd.y2 * InvScale}, {cmd.x3 * InvScale, cmd.y3 * InvScale}});
                break;
            case 'Z':
                if (figureOpen) {
                    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                    figureOpen = false;
                }
                break;
        }
    }
    if (figureOpen) {
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
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

void GeekIconRenderer::DrawLogo(ID2D1RenderTarget* dc, const D2D1_RECT_F& rect) {
  if (!dc) return;

  // Brushes use normalized [0, 1] relative coordinates mapping to the unit square geometry.
  // The D2D RenderTarget Transform set by DrawVectorIcon will automatically handle scaling and translation.

  // 1. st5 Gradient (Background Card)
  {
    std::array<D2D1_GRADIENT_STOP, 2> stops = {
        D2D1_GRADIENT_STOP{0.0f, D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f)},
        D2D1_GRADIENT_STOP{1.0f, D2D1::ColorF(0.2235f, 0.2235f, 0.2588f, 1.0f)} // #393942
    };
    ComPtr<ID2D1GradientStopCollection> stopCollection;
    if (SUCCEEDED(dc->CreateGradientStopCollection(stops.data(), static_cast<UINT32>(stops.size()), &stopCollection))) {
      ComPtr<ID2D1LinearGradientBrush> brush;
      if (SUCCEEDED(dc->CreateLinearGradientBrush(
              D2D1::LinearGradientBrushProperties(D2D1::Point2F(0.05323f, 0.94677f), D2D1::Point2F(0.94677f, 0.05323f)),
              stopCollection.Get(), &brush))) {
        QuickView::UI::GeekIconRenderer::DrawVectorIcon(dc, Icons::Logo_st5Vector, rect, brush.Get());
      }
    }
  }

  // 2. st3 Solid Color Brush (Border Outline)
  {
    ComPtr<ID2D1SolidColorBrush> brush;
    if (SUCCEEDED(dc->CreateSolidColorBrush(D2D1::ColorF(0.3255f, 0.3569f, 0.4000f, 1.0f), &brush))) { // #535b66
      QuickView::UI::GeekIconRenderer::DrawVectorIcon(dc, Icons::Logo_st3Vector, rect, brush.Get());
    }
  }

  // 3. st4 Gradient (Shadow Arc)
  {
    std::array<D2D1_GRADIENT_STOP, 2> stops = {
        D2D1_GRADIENT_STOP{0.0f, D2D1::ColorF(0.0275f, 0.3294f, 0.8157f, 1.0f)}, // #0754d0
        D2D1_GRADIENT_STOP{1.0f, D2D1::ColorF(0.2431f, 0.7765f, 0.9176f, 1.0f)}  // #3ec6ea
    };
    ComPtr<ID2D1GradientStopCollection> stopCollection;
    if (SUCCEEDED(dc->CreateGradientStopCollection(stops.data(), static_cast<UINT32>(stops.size()), &stopCollection))) {
      ComPtr<ID2D1LinearGradientBrush> brush;
      if (SUCCEEDED(dc->CreateLinearGradientBrush(
              D2D1::LinearGradientBrushProperties(D2D1::Point2F(0.31162f, 0.78474f), D2D1::Point2F(0.66387f, 0.17463f)),
              stopCollection.Get(), &brush))) {
        QuickView::UI::GeekIconRenderer::DrawVectorIcon(dc, Icons::Logo_st4Vector, rect, brush.Get());
      }
    }
  }

  // 4. st0 Gradient (Primary Arc)
  {
    std::array<D2D1_GRADIENT_STOP, 6> stops = {
        D2D1_GRADIENT_STOP{0.0f, D2D1::ColorF(0.0f, 0.60f, 0.988f, 1.0f)},    // #0099fc
        D2D1_GRADIENT_STOP{0.16f, D2D1::ColorF(0.059f, 0.702f, 0.973f, 1.0f)}, // #0fb3f8
        D2D1_GRADIENT_STOP{0.35f, D2D1::ColorF(0.118f, 0.804f, 0.961f, 1.0f)}, // #1ecdf5
        D2D1_GRADIENT_STOP{0.55f, D2D1::ColorF(0.157f, 0.875f, 0.953f, 1.0f)}, // #28dff3
        D2D1_GRADIENT_STOP{0.76f, D2D1::ColorF(0.18f, 0.918f, 0.949f, 1.0f)},   // #2eeaf2
        D2D1_GRADIENT_STOP{1.0f, D2D1::ColorF(0.192f, 0.933f, 0.949f, 1.0f)}   // #31eef2
    };
    ComPtr<ID2D1GradientStopCollection> stopCollection;
    if (SUCCEEDED(dc->CreateGradientStopCollection(stops.data(), static_cast<UINT32>(stops.size()), &stopCollection))) {
      ComPtr<ID2D1LinearGradientBrush> brush;
      if (SUCCEEDED(dc->CreateLinearGradientBrush(
              D2D1::LinearGradientBrushProperties(D2D1::Point2F(0.30651f, 0.79435f), D2D1::Point2F(0.66964f, 0.16539f)),
              stopCollection.Get(), &brush))) {
        QuickView::UI::GeekIconRenderer::DrawVectorIcon(dc, Icons::Logo_st0Vector, rect, brush.Get());
      }
    }
  }

  // 5. st1 Gradient (Pointer Highlight)
  {
    std::array<D2D1_GRADIENT_STOP, 9> stops = {
        D2D1_GRADIENT_STOP{0.0f, D2D1::ColorF(0.118f, 0.89f, 0.976f, 1.0f)},  // #1ee3f9
        D2D1_GRADIENT_STOP{0.0f, D2D1::ColorF(0.129f, 0.89f, 0.976f, 1.0f)},  // #21e3f9
        D2D1_GRADIENT_STOP{0.16f, D2D1::ColorF(0.357f, 0.918f, 0.98f, 1.0f)},  // #5beafa
        D2D1_GRADIENT_STOP{0.31f, D2D1::ColorF(0.553f, 0.941f, 0.984f, 1.0f)}, // #8df0fb
        D2D1_GRADIENT_STOP{0.46f, D2D1::ColorF(0.714f, 0.961f, 0.992f, 1.0f)}, // #b6f5fd
        D2D1_GRADIENT_STOP{0.61f, D2D1::ColorF(0.839f, 0.976f, 0.992f, 1.0f)}, // #d6f9fd
        D2D1_GRADIENT_STOP{0.75f, D2D1::ColorF(0.925f, 0.988f, 0.996f, 1.0f)}, // #ecfcfe
        D2D1_GRADIENT_STOP{0.88f, D2D1::ColorF(0.98f, 0.996f, 0.996f, 1.0f)},  // #fafefe
        D2D1_GRADIENT_STOP{1.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f)}        // #ffffff
    };
    ComPtr<ID2D1GradientStopCollection> stopCollection;
    if (SUCCEEDED(dc->CreateGradientStopCollection(stops.data(), static_cast<UINT32>(stops.size()), &stopCollection))) {
      ComPtr<ID2D1LinearGradientBrush> brush;
      if (SUCCEEDED(dc->CreateLinearGradientBrush(
              D2D1::LinearGradientBrushProperties(D2D1::Point2F(0.43819f, 0.63506f), D2D1::Point2F(0.78186f, 0.43664f)),
              stopCollection.Get(), &brush))) {
        QuickView::UI::GeekIconRenderer::DrawVectorIcon(dc, Icons::Logo_st1Vector, rect, brush.Get());
      }
    }
  }

  // 6. st2 Gradient (Light Rays)
  {
    std::array<D2D1_GRADIENT_STOP, 9> stops = {
        D2D1_GRADIENT_STOP{0.0f, D2D1::ColorF(0.0f, 0.792f, 0.961f, 1.0f)},    // #00caf5
        D2D1_GRADIENT_STOP{0.09f, D2D1::ColorF(0.169f, 0.827f, 0.965f, 1.0f)}, // #2bd3f6
        D2D1_GRADIENT_STOP{0.22f, D2D1::ColorF(0.361f, 0.871f, 0.973f, 1.0f)}, // #5cdef8
        D2D1_GRADIENT_STOP{0.34f, D2D1::ColorF(0.525f, 0.91f, 0.976f, 1.0f)},  // #86e8f9
        D2D1_GRADIENT_STOP{0.47f, D2D1::ColorF(0.659f, 0.941f, 0.98f, 1.0f)},  // #a8f0fa
        D2D1_GRADIENT_STOP{0.59f, D2D1::ColorF(0.765f, 0.965f, 0.984f, 1.0f)}, // #c3f6fb
        D2D1_GRADIENT_STOP{0.72f, D2D1::ColorF(0.839f, 0.98f, 0.988f, 1.0f)},  // #d6fafc
        D2D1_GRADIENT_STOP{0.86f, D2D1::ColorF(0.882f, 0.992f, 0.988f, 1.0f)}, // #e1fdfc
        D2D1_GRADIENT_STOP{1.0f, D2D1::ColorF(0.898f, 0.996f, 0.992f, 1.0f)}   // #e5fefd
    };
    ComPtr<ID2D1GradientStopCollection> stopCollection;
    if (SUCCEEDED(dc->CreateGradientStopCollection(stops.data(), static_cast<UINT32>(stops.size()), &stopCollection))) {
      ComPtr<ID2D1LinearGradientBrush> brush;
      if (SUCCEEDED(dc->CreateLinearGradientBrush(
              D2D1::LinearGradientBrushProperties(D2D1::Point2F(0.44847f, 0.71705f), D2D1::Point2F(0.77433f, 0.39119f)),
              stopCollection.Get(), &brush))) {
        QuickView::UI::GeekIconRenderer::DrawVectorIcon(dc, Icons::Logo_st2Vector, rect, brush.Get());
      }
    }
  }
}

} // namespace QuickView::UI
