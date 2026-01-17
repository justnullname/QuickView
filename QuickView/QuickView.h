#pragma once
#include <d2d1.h> 

#define MAX_LOADSTRING 100

// [Refactor] Single Source of Truth for Visual Dimensions & State
// Used by WM_MOUSEWHEEL, SyncDCompState, and GetEffectiveImageSize
struct VisualState {
    // 1. Physical Layer
    D2D1_SIZE_F PhysicalSize;    // Raw Surface/Bitmap pixels (Unrotated)
    
    // 2. Transform Layer
    float TotalRotation;         // 0, 90, 180, 270 (Exif + User)
    bool IsRotated90;            // True if 90 or 270
    
    // 3. Visual Layer
    D2D1_SIZE_F VisualSize;      // Logically swapped dimensions (if 90 deg)
    
    // 4. Layout Helper
    float AspectRatio() const {
        return (VisualSize.height > 0) ? VisualSize.width / VisualSize.height : 1.0f;
    }
};
