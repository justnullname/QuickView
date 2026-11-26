#pragma once

// Header-only toolbar button visibility rules shared by Toolbar.cpp and QuickViewTests.
enum class ToolbarButtonID {
    None,
    Prev, Next,
    RotateL, RotateR, FlipH,
    LockSize, Gallery,
    Exif, RawToggle, FixExtension,
    GamutWarning,
    Pin,
    CompareToggle,
    CompareOpen,
    CompareSwap,
    CompareLayout,
    CompareInfo,
    CompareRawToggle,
    CompareDelete,
    CompareZoomIn,
    CompareZoomOut,
    CompareSyncZoom,
    CompareSyncPan,
    CompareSave,
    CompareExit,
    // Animation mode
    AnimPlayPause,
    AnimPrevFrame,
    AnimNextFrame,
    AnimDirtyRect,
    AnimSpeedUp,
    AnimSpeedDown,
    AnimSeek,
    // Overlay (Tracing) mode
    OverlayAlphaUp,
    OverlayAlphaDown,
    OverlayPassthrough,
    OverlayExit,
    // Slideshow mode
    SlideshowImmersiveToggle,
    SlideshowExit,
    // Crop mode
    CropCopy,
    CropApply,
    CropSave,
    CropCancel,
    // Multi-page / Book mode
    BookMode,
    PageSeek
};

inline bool IsToolbarButtonVisibleInMode(
    ToolbarButtonID id,
    bool cropMode,
    bool overlayMode,
    bool slideshowMode,
    bool compareMode,
    bool animMode,
    bool comicOrBookMode,
    bool bookModeAvailable,
    bool isButtonEnabled = true,
    bool isButtonWarning = false,
    bool compareHasAnim = false,
    bool isDirty = false,
    bool showDirtyRectButton = false) {
    auto isCompareButton = [](ToolbarButtonID btnId) {
        switch (btnId) {
        case ToolbarButtonID::CompareOpen:
        case ToolbarButtonID::CompareSwap:
        case ToolbarButtonID::CompareLayout:
        case ToolbarButtonID::CompareInfo:
        case ToolbarButtonID::CompareDelete:
        case ToolbarButtonID::CompareZoomIn:
        case ToolbarButtonID::CompareZoomOut:
        case ToolbarButtonID::CompareSyncZoom:
        case ToolbarButtonID::CompareSyncPan:
        case ToolbarButtonID::CompareRawToggle:
        case ToolbarButtonID::CompareSave:
        case ToolbarButtonID::CompareExit:
            return true;
        default:
            return false;
        }
    };
    auto isOverlayButton = [](ToolbarButtonID btnId) {
        return btnId == ToolbarButtonID::OverlayAlphaUp ||
               btnId == ToolbarButtonID::OverlayAlphaDown ||
               btnId == ToolbarButtonID::OverlayPassthrough ||
               btnId == ToolbarButtonID::OverlayExit;
    };
    auto isAnimButton = [](ToolbarButtonID btnId) {
        return btnId == ToolbarButtonID::AnimPlayPause ||
               btnId == ToolbarButtonID::AnimPrevFrame ||
               btnId == ToolbarButtonID::AnimNextFrame ||
               btnId == ToolbarButtonID::AnimDirtyRect;
    };
    auto isCropButton = [](ToolbarButtonID btnId) {
        return btnId == ToolbarButtonID::RotateL ||
               btnId == ToolbarButtonID::RotateR ||
               btnId == ToolbarButtonID::FlipH ||
               btnId == ToolbarButtonID::CropCopy ||
               btnId == ToolbarButtonID::CropSave ||
               btnId == ToolbarButtonID::CropApply ||
               btnId == ToolbarButtonID::CropCancel;
    };
    auto isAlwaysVisible = [](ToolbarButtonID btnId) {
        return btnId == ToolbarButtonID::Pin;
    };

    if (id == ToolbarButtonID::PageSeek) {
        return false;
    }
    if (cropMode) {
        return isCropButton(id);
    }
    if (slideshowMode) {
        const bool isSlideshowButton =
            id == ToolbarButtonID::SlideshowImmersiveToggle ||
            id == ToolbarButtonID::SlideshowExit ||
            id == ToolbarButtonID::AnimPrevFrame ||
            id == ToolbarButtonID::AnimPlayPause ||
            id == ToolbarButtonID::AnimNextFrame;
        if (isSlideshowButton || id == ToolbarButtonID::Gallery ||
            id == ToolbarButtonID::Exif || id == ToolbarButtonID::Pin) {
            return isButtonEnabled;
        }
        return false;
    }
    if (id == ToolbarButtonID::SlideshowImmersiveToggle ||
        id == ToolbarButtonID::SlideshowExit) {
        return false;
    }
    if (overlayMode) {
        return isOverlayButton(id) ||
               id == ToolbarButtonID::CompareZoomIn ||
               id == ToolbarButtonID::CompareZoomOut ||
               id == ToolbarButtonID::LockSize ||
               isAlwaysVisible(id);
    }
    if (comicOrBookMode) {
        // Strict whitelist for comic/book reading mode (takes priority over CompareMode for Dual-Page reading)
        switch (id) {
        case ToolbarButtonID::Prev:
        case ToolbarButtonID::Next:
        case ToolbarButtonID::RotateL:
        case ToolbarButtonID::RotateR:
        case ToolbarButtonID::Gallery:
        case ToolbarButtonID::Exif:
        case ToolbarButtonID::CompareToggle:
        case ToolbarButtonID::Pin:
            return true;
        case ToolbarButtonID::BookMode:
            return bookModeAvailable;
        default:
            return false;
        }
    }
    if (compareMode) {
        if (id == ToolbarButtonID::AnimPlayPause) return compareHasAnim;
        if (!isCompareButton(id) && !isAlwaysVisible(id)) return false;
        if (id == ToolbarButtonID::CompareRawToggle && !isButtonWarning) return false;
        if (id == ToolbarButtonID::CompareSave) return isDirty;
        return true;
    }
    if (animMode) {
        if (id == ToolbarButtonID::AnimDirtyRect) return showDirtyRectButton;
        if (id == ToolbarButtonID::Prev || id == ToolbarButtonID::Next ||
            id == ToolbarButtonID::Exif || id == ToolbarButtonID::LockSize) {
            return true;
        }
        return isAnimButton(id) || isAlwaysVisible(id);
    }
    if (isCompareButton(id) || isAnimButton(id) || isOverlayButton(id)) {
        return false;
    }
    if (id == ToolbarButtonID::CropApply || id == ToolbarButtonID::CropCopy ||
        id == ToolbarButtonID::CropCancel) {
        return false;
    }
    if (id == ToolbarButtonID::CropSave) {
        return isDirty;
    }
    if (id == ToolbarButtonID::BookMode) {
        return bookModeAvailable;
    }
    if (id == ToolbarButtonID::RawToggle && !isButtonEnabled) {
        return false;
    }
    if (id == ToolbarButtonID::FixExtension && !isButtonWarning) {
        return false;
    }
    if (id == ToolbarButtonID::GamutWarning && !isButtonEnabled) {
        return false;
    }
    return true;
}
