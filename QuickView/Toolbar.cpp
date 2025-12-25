#include "pch.h"
#include "Toolbar.h"
#include "EditState.h"

extern AppConfig g_config;

// Icon Codes (Segoe Fluent Icons)
#define ICON_PREV L"\uE76B"
#define ICON_NEXT L"\uE76C"
#define ICON_ROTATE_L L"\uE7AD"
#define ICON_ROTATE_R L"\uE7AD" 
#define ICON_FLIP L"\uE8AB" // Mirror
#define ICON_LOCK L"\uE9A6" // Lock icon (same for both states, color changes)
#define ICON_UNLOCK L"\uE9A6"
#define ICON_GALLERY L"\uE80A"
#define ICON_INFO L"\uE946"
#define ICON_RAW L"\uE722" // RAW icon (same for both states, color changes)
#define ICON_WARNING L"\uE7BA"
#define ICON_PIN L"\uE718"
#define ICON_UNPIN L"\uE77A"

Toolbar::Toolbar() {
    // Define Buttons
    m_buttons = {
        { ToolbarButtonID::Prev,        ICON_PREV[0], L"Previous (Left)", {}, true },
        { ToolbarButtonID::Next,        ICON_NEXT[0], L"Next (Right)", {}, true },
        // Spacer? Just gap.
        { ToolbarButtonID::RotateL,     ICON_ROTATE_L[0], L"Rotate Left (Shift+R)", {}, true },
        { ToolbarButtonID::RotateR,     ICON_ROTATE_R[0], L"Rotate Right (R)", {}, true },
        { ToolbarButtonID::FlipH,       ICON_FLIP[0], L"Flip Horizontal (H)", {}, true },
        
        { ToolbarButtonID::LockSize,    ICON_LOCK[0], L"Lock Window Size", {}, true, false },
        { ToolbarButtonID::Gallery,     ICON_GALLERY[0], L"Gallery (T)", {}, true },
        
        { ToolbarButtonID::Exif,        ICON_INFO[0], L"Info Panel", {}, true, false },
        { ToolbarButtonID::RawToggle,   ICON_RAW[0], L"RAW Preview (Fast)", {}, false, false }, // Hidden/Disabled if not RAW
        { ToolbarButtonID::FixExtension, ICON_WARNING[0], L"Extension Mismatch (Fix)", {}, false, false, true }, // Hidden if no mismatch
        
        { ToolbarButtonID::Pin,         ICON_PIN[0], L"Pin Toolbar", {}, true, false }
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
    
    // Simpler: Count visible buttons
    int visibleCount = 0;
    for (const auto& btn : m_buttons) {
        bool visible = true;
        if (btn.id == ToolbarButtonID::RawToggle && !btn.isEnabled) visible = false; 
        if (btn.id == ToolbarButtonID::FixExtension && !btn.isWarning) visible = false;
        if (visible) visibleCount++;
    }
    
    // Calculate total width: padding + buttons + gaps between buttons
    float totalW = PADDING_X * 2 + (visibleCount * BUTTON_SIZE);
    if (visibleCount > 1) totalW += (visibleCount - 1) * GAP;

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
        
        // Sync Pin State
        if (btn.id == ToolbarButtonID::Pin) {
             btn.isToggled = m_isPinned;
             btn.iconChar = m_isPinned ? ICON_UNPIN[0] : ICON_PIN[0];
             btn.tooltip = m_isPinned ? L"Unpin Toolbar" : L"Pin Toolbar";
        }
        
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
        m_brushBg->SetOpacity(g_config.ToolbarAlpha); // Base opacity from config
        pRT->FillRoundedRectangle(m_bgRect, m_brushBg.Get());
        
        // Buttons
        for (const auto& btn : m_buttons) {
            if (btn.rect.right == 0) continue; // Hidden
            
            // Hover effect with rounded corners
            if (btn.isHovered) {
                D2D1_ROUNDED_RECT hoverRect = D2D1::RoundedRect(btn.rect, 6.0f, 6.0f);
                pRT->FillRoundedRectangle(hoverRect, m_brushHover.Get());
            }
            
            // Icon Brush
            ID2D1SolidColorBrush* pBrush = m_brushIcon.Get();
            if (btn.isToggled) pBrush = m_brushIconActive.Get();
            if (btn.isWarning) pBrush = m_brushWarning.Get();
            if (btn.id == ToolbarButtonID::LockSize && btn.isToggled) pBrush = m_brushIconActive.Get();
            if (btn.id == ToolbarButtonID::Pin && btn.isToggled) pBrush = m_brushIconActive.Get();
            
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
    
    // Tooltip for hovered button (rendered OUTSIDE layer for full opacity)
    for (const auto& btn : m_buttons) {
        if (btn.isHovered && !btn.tooltip.empty()) {
            static ComPtr<IDWriteTextFormat> tooltipFormat;
            if (!tooltipFormat && m_dwriteFactory) {
                m_dwriteFactory->CreateTextFormat(
                    L"Segoe UI", NULL,
                    DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                    12.0f, L"en-us", &tooltipFormat);
                if (tooltipFormat) {
                    tooltipFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                    tooltipFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                }
            }
            
            if (tooltipFormat) {
                float tipWidth = btn.tooltip.length() * 7.0f + 16.0f;
                float tipHeight = 22.0f;
                float tipX = (btn.rect.left + btn.rect.right) / 2 - tipWidth / 2;
                float tipY = m_bgRect.rect.top - tipHeight - 8.0f;
                if (tipX < 5) tipX = 5;
                
                D2D1_RECT_F tipRect = D2D1::RectF(tipX, tipY, tipX + tipWidth, tipY + tipHeight);
                
                ComPtr<ID2D1SolidColorBrush> tipBg;
                pRT->CreateSolidColorBrush(D2D1::ColorF(0.15f, 0.15f, 0.15f, 0.95f), &tipBg);
                pRT->FillRoundedRectangle(D2D1::RoundedRect(tipRect, 4.0f, 4.0f), tipBg.Get());
                pRT->DrawText(btn.tooltip.c_str(), (UINT32)btn.tooltip.length(), tooltipFormat.Get(), tipRect, m_brushIcon.Get());
            }
            break;
        }
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
            // Icon stays the same (E9A6), only color changes via isToggled
            btn.tooltip = locked ? L"Unlock Window Size" : L"Lock Window Size";
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
                // Icon stays the same (E722), only color changes via isToggled
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
