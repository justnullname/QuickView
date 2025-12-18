#pragma once

/// <summary>
/// Centralized string resources for easy localization
/// </summary>
namespace AppStrings {
    // OSD Messages
    static const wchar_t* OSD_NoImage = L"No image loaded";
    static const wchar_t* OSD_Lossless = L"Lossless";
    static const wchar_t* OSD_ReencodedLossless = L"Re-encoded (Lossless)";
    static const wchar_t* OSD_EdgeAdapted = L"Edge Adapted";
    static const wchar_t* OSD_Reencoded = L"Re-encoded";
    static const wchar_t* OSD_ReadOnly = L"Access denied - file may be in use or read-only";
    static const wchar_t* OSD_NotPerfect = L"Transform is not perfect (Edge optimized)";
    
    // Transform Action Names
    static const wchar_t* Action_RotateCW = L"Rotate 90\x00B0 CW";
    static const wchar_t* Action_RotateCCW = L"Rotate 90\x00B0 CCW";
    static const wchar_t* Action_Rotate180 = L"Rotate 180\x00B0";
    static const wchar_t* Action_FlipH = L"Flip Horizontal";
    static const wchar_t* Action_FlipV = L"Flip Vertical";

    // Dialog Strings
    static const wchar_t* Dialog_SaveTitle = L"Save Changes?";
    static const wchar_t* Dialog_SaveContent = L"The image has been modified. Do you want to save changes?";
    static const wchar_t* Dialog_ButtonSave = L"Save";
    static const wchar_t* Dialog_ButtonSaveAs = L"Save As...";
    static const wchar_t* Dialog_ButtonDiscard = L"Discard";
    
    // Checkbox Labels
    static const wchar_t* Checkbox_AlwaysSaveLossless = L"Always save lossless transforms";
    static const wchar_t* Checkbox_AlwaysSaveEdgeAdapted = L"Always save edge-adapted";
    static const wchar_t* Checkbox_AlwaysSaveLossy = L"Always save re-encoded";
}
