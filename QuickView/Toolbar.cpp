#include "pch.h"
#include "Toolbar.h"

// Icon Codes (Segoe Fluent Icons)
#define ICON_PREV L"\uE76B"
#define ICON_NEXT L"\uE76C"
#define ICON_ROTATE_L L"\uE7AD" // Need to mirror? Or use Undo \uE7A9? Rotate is E7AD.
#define ICON_ROTATE_R L"\uE7AD" 
#define ICON_FLIP L"\uE840"
#define ICON_LOCK L"\uE72E"
#define ICON_UNLOCK L"\uE785"
#define ICON_GALLERY L"\uE80A"
#define ICON_INFO L"\uE946"
#define ICON_RAW_PREV L"\uE7B3"
#define ICON_RAW_FULL L"\uE890" 
#define ICON_WARNING L"\uE7BA"

Toolbar::Toolbar() {
    // Define Buttons
    m_buttons = {
        { ToolbarButtonID::Prev,        ICON_PREV[0], L"Previous (Left)", {}, true },
        { ToolbarButtonID::Next,        ICON_NEXT[0], L"Next (Right)", {}, true },
        // Spacer? Just gap.
        { ToolbarButtonID::RotateL,     ICON_ROTATE_L[0], L"Rotate Left (Shift+R)", {}, true },
        { ToolbarButtonID::RotateR,     ICON_ROTATE_R[0], L"Rotate Right (R)", {}, true },
        { ToolbarButtonID::FlipH,       ICON_FLIP[0], L"Flip Horizontal (H)", {}, true },
        
        { ToolbarButtonID::LockSize,    ICON_UNLOCK[0], L"Lock Window Size", {}, true, false },
        { ToolbarButtonID::Gallery,     ICON_GALLERY[0], L"Gallery (T)", {}, true },
        
        { ToolbarButtonID::Exif,        ICON_INFO[0], L"Info Panel", {}, true, false },
        { ToolbarButtonID::RawToggle,   ICON_RAW_PREV[0], L"RAW Preview (Fast)", {}, false, false }, // Hidden/Disabled if not RAW
        { ToolbarButtonID::FixExtension, ICON_WARNING[0], L"Extension Mismatch (Fix)", {}, false, false, true } // Hidden if no mismatch
    };
}

Toolbar::~Toolbar() {}

void Toolbar::CreateResources(ID2D1RenderTarget* pRT) {
    if (!m_brushBg) {
        pRT->CreateSolidColorBrush(D2D1::ColorF(0.1f, 0.1f, 0.1f, 0.85f), &m_brushBg); // Dark background
        pRT->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &m_brushIcon);
        pRT->CreateSolidColorBrush(D2D1::ColorF(0.4f, 0.6f, 1.0f, 1.0f), &m_brushIconActive); // Blue for active
        pRT->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.3f), &m_brushIconDisabled);
        pRT->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.3f, 0.3f, 1.0f), &m_brushWarning); // Red for warning
        pRT->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.1f), &m_brushHover); // Hover highlight
        
        // Font
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf()));
        if (m_dwriteFactory) {
            m_dwriteFactory->CreateTextFormat(
                L"Segoe Fluent Icons",
                NULL,
                DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                20.0f, // Icon Size
                L"en-us",
                &m_textFormatIcon
            );
            // Fallback for Win10? "Segoe MDL2 Assets"
            // If Segoe Fluent Icons not found, it shows box.
            // We can try to detect or just use MDL2 if simpler?
            // Users requested Fluent. Keep Fluent.
            if (m_textFormatIcon) {
                m_textFormatIcon->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                m_textFormatIcon->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
        }
    }
}

void Toolbar::Init(ID2D1RenderTarget* pRT) {
    CreateResources(pRT);
}

void Toolbar::UpdateLayout(float winW, float winH) {
    // Calculate total width based on visible buttons
    float visibleCount = 0;
    for (const auto& btn : m_buttons) {
        if (btn.isEnabled || btn.id == ToolbarButtonID::FixExtension && btn.isWarning) visibleCount++; 
        // Logic: if not enabled, do we hide or dim?
        // For Raw/Extension, we HIDE if not applicable.
        // For Lock/Exif, we DIM/Active.
        // Let's refine visibility logic in Render/Layout.
    }
    
    // Simpler: Count visible buttons
    float totalW = PADDING_X * 2;
    int idx = 0;
    for (auto& btn : m_buttons) {
        // Condition to show:
        bool visible = true;
        if (btn.id == ToolbarButtonID::RawToggle && !btn.isEnabled) visible = false; 
        if (btn.id == ToolbarButtonID::FixExtension && !btn.isWarning) visible = false;
        
        if (visible) {
            totalW += BUTTON_SIZE;
            if (idx > 0) totalW += GAP;
            idx++;
        }
    }
    // Remove last gap logic handled by loop?
    if (idx > 0) totalW -= GAP; // Correct for last gap
    else totalW = PADDING_X * 2; // Empty?

    float startX = (winW - totalW) / 2.0f;
    float startY = winH - BOTTOM_MARGIN - BUTTON_SIZE - PADDING_Y * 2; 

    m_bgRect = D2D1::RoundedRect(
        D2D1::RectF(startX, startY, startX + totalW, startY + BUTTON_SIZE + PADDING_Y * 2),
        20.0f, 20.0f // Capsule radius
    );
    
    // Layout Buttons
    float cx = startX + PADDING_X;
    float cy = startY + PADDING_Y;
    
    for (auto& btn : m_buttons) {
        bool visible = true;
        if (btn.id == ToolbarButtonID::RawToggle && !btn.isEnabled) visible = false; 
        if (btn.id == ToolbarButtonID::FixExtension && !btn.isWarning) visible = false;
        
        if (visible) {
            btn.rect = D2D1::RectF(cx, cy, cx + BUTTON_SIZE, cy + BUTTON_SIZE);
            cx += BUTTON_SIZE + GAP;
        } else {
            btn.rect = D2D1::RectF(0,0,0,0); // Hide
        }
    }
}

void Toolbar::Render(ID2D1RenderTarget* pRT) {
    if (m_opacity <= 0.0f) return;
    
    CreateResources(pRT); // Ensure resources
    
    // Set global opacity
    // Cannot set on RenderTarget easily without Layer.
    // Use Layer or set Brush opacity.
    // Layer is cleaner for semi-transparent group.
    
    ComPtr<ID2D1Layer> layer;
    if (SUCCEEDED(pRT->CreateLayer(&layer))) {
        D2D1_LAYER_PARAMETERS params = D2D1::LayerParameters();
        params.contentBounds = m_bgRect.rect;
        params.opacity = m_opacity;
        
        pRT->PushLayer(params, layer.Get());
        
        // Background
        m_brushBg->SetOpacity(0.85f); // Base opacity
        pRT->FillRoundedRectangle(m_bgRect, m_brushBg.Get());
        
        // Buttons
        for (const auto& btn : m_buttons) {
            if (btn.rect.right == 0) continue; // Hidden
            
            // Hover effect
            if (btn.isHovered) {
                pRT->FillRectangle(btn.rect, m_brushHover.Get());
            }
            
            // Icon Brush
            ID2D1SolidColorBrush* pBrush = m_brushIcon.Get();
            if (btn.isToggled) pBrush = m_brushIconActive.Get();
            if (btn.isWarning) pBrush = m_brushWarning.Get();
            if (btn.id == ToolbarButtonID::LockSize && btn.isToggled) pBrush = m_brushIconActive.Get();
            
            // Specific Icon Logic
            wchar_t icon = btn.iconChar;
            
            // Rotate Mirroring check
            if (btn.id == ToolbarButtonID::RotateL) {
                 // Manual mirror? Or just render. Rotate icon usually implies CW.
                 // For CCW, we can flip using matrix or just accept it.
                 // Let's use matrix to flip horizontally for RotateL if needed.
                 // Center of button
                 // D2D1::Matrix3x2F::Scale(-1.0f, 1.0f, center)
                 float cx = (btn.rect.left + btn.rect.right)/2;
                 float cy = (btn.rect.top + btn.rect.bottom)/2;
                 pRT->SetTransform(D2D1::Matrix3x2F::Scale(-1.0f, 1.0f, D2D1::Point2F(cx, cy)));
                 pRT->DrawText(&icon, 1, m_textFormatIcon.Get(), btn.rect, pBrush);
                 pRT->SetTransform(D2D1::Matrix3x2F::Identity());
                 continue;
            }
            
            pRT->DrawText(&icon, 1, m_textFormatIcon.Get(), btn.rect, pBrush);
        }
        
        pRT->PopLayer();
    }
}

void Toolbar::OnMouseMove(float x, float y) {
    // Check if near bottom
    // We don't have window height here unless passed or stored.
    // UpdateLayout stores rect.
    // Trigger zone is usually managed by MainWindow, calling SetVisible.
    // But button hover needs x/y.
    
    bool changed = false;
    for (auto& btn : m_buttons) {
        bool wasHovered = btn.isHovered;
        btn.isHovered = false;
        if (btn.rect.right > 0) { // Visible
            if (x >= btn.rect.left && x < btn.rect.right && y >= btn.rect.top && y < btn.rect.bottom) {
                btn.isHovered = true;
            }
        }
        if (btn.isHovered != wasHovered) changed = true;
    }
    // Return changed?
}

bool Toolbar::OnClick(float x, float y, ToolbarButtonID& outId) {
    if (!IsVisible()) return false;
    
    // Check background hit
    if (HitTest(x, y)) {
        
        // Check buttons
        for (auto& btn : m_buttons) {
            if (btn.rect.right > 0) {
                 if (x >= btn.rect.left && x < btn.rect.right && y >= btn.rect.top && y < btn.rect.bottom) {
                     outId = btn.id;
                     return true;
                 }
            }
        }
        return true; // Consumed click on toolbar background
    }
    return false;
}

bool Toolbar::HitTest(float x, float y) {
    if (!IsVisible()) return false;
    return (x >= m_bgRect.rect.left && x <= m_bgRect.rect.right &&
            y >= m_bgRect.rect.top && y <= m_bgRect.rect.bottom);
}

void Toolbar::SetVisible(bool visible) {
    m_targetVisible = visible;
}

bool Toolbar::UpdateAnimation() {
    float speed = 0.1f;
    if (m_targetVisible) {
        if (m_opacity < 1.0f) {
            m_opacity += speed;
            if (m_opacity > 1.0f) m_opacity = 1.0f;
            return true;
        }
    } else {
        if (m_opacity > 0.0f) {
            m_opacity -= speed;
            if (m_opacity < 0.0f) m_opacity = 0.0f;
            return true;
        }
    }
    return false;
}

void Toolbar::SetLockState(bool locked) {
    for (auto& btn : m_buttons) {
        if (btn.id == ToolbarButtonID::LockSize) {
            btn.isToggled = locked;
            btn.iconChar = locked ? ICON_LOCK[0] : ICON_UNLOCK[0];
            btn.tooltip = locked ? L"Inlock Window Size" : L"Lock Window Size"; // Typo logic: Locked -> Unlock action? Or State?
            // Usually icon shows state. Locked = Lock Icon.
        }
    }
}

void Toolbar::SetExifState(bool open) {
    for (auto& btn : m_buttons) {
        if (btn.id == ToolbarButtonID::Exif) {
            btn.isToggled = open;
        }
    }
}

void Toolbar::SetRawState(bool isRaw, bool isFullDecode) {
    for (auto& btn : m_buttons) {
        if (btn.id == ToolbarButtonID::RawToggle) {
            btn.isEnabled = isRaw;
            if (isRaw) {
                btn.isToggled = isFullDecode;
                btn.iconChar = isFullDecode ? ICON_RAW_FULL[0] : ICON_RAW_PREV[0];
                btn.tooltip = isFullDecode ? L"RAW: Full Decode (Click for Preview)" : L"RAW: Preview (Click for Full)";
            }
        }
    }
}

void Toolbar::SetExtensionWarning(bool hasMismatch) {
    for (auto& btn : m_buttons) {
        if (btn.id == ToolbarButtonID::FixExtension) {
            btn.isWarning = hasMismatch; // Only visible if warning?
            // UpdateLayout should handle visibility based on isWarning flag logic I wrote above.
        }
    }
}
