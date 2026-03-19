#pragma once

/// <summary>
/// Centralized string resources for easy localization
/// Refactored for Runtime Switching (Pointer-based)
/// </summary>
namespace AppStrings {
    enum class Language : int {
        Auto = 0,
        English = 1,
        ChineseSimplified = 2,
        ChineseTraditional = 3,
        Japanese = 4,
        Russian = 5,
        German = 6,
        Spanish = 7
    };

    void Init();
    void SetLanguage(Language lang);

    // OSD Messages
    extern const wchar_t* OSD_NoImage;
    extern const wchar_t* OSD_Lossless;
    extern const wchar_t* OSD_ReencodedLossless;
    extern const wchar_t* OSD_EdgeAdapted;
    extern const wchar_t* OSD_Reencoded;
    extern const wchar_t* OSD_ReadOnly;
    extern const wchar_t* OSD_NotPerfect;
    extern const wchar_t* OSD_SpanOn;
    extern const wchar_t* OSD_SpanOff;
    
    // Transform Action Names
    extern const wchar_t* Action_RotateCW;
    extern const wchar_t* Action_RotateCCW;
    extern const wchar_t* Action_Rotate180;
    extern const wchar_t* Action_FlipH;
    extern const wchar_t* Action_FlipV;

    // Dialog Strings
    extern const wchar_t* Dialog_SaveTitle;
    extern const wchar_t* Dialog_SaveContent;
    extern const wchar_t* Dialog_ButtonSave;
    extern const wchar_t* Dialog_ButtonSaveAs;
    extern const wchar_t* Dialog_ButtonDiscard;

    // Context Menu
    extern const wchar_t* Context_Open;
    extern const wchar_t* Context_OpenWith;
    extern const wchar_t* Context_Edit;
    extern const wchar_t* Context_ShowInExplorer;
    extern const wchar_t* Context_OpenFolder;
    extern const wchar_t* Context_CopyImage;
    extern const wchar_t* Context_CopyPath;
    extern const wchar_t* Context_Print;
    extern const wchar_t* Context_RotateCW;
    extern const wchar_t* Context_RotateCCW;
    extern const wchar_t* Context_FlipH;
    extern const wchar_t* Context_FlipV;
    extern const wchar_t* Context_Transform;
    extern const wchar_t* Context_ActualSize;
    extern const wchar_t* Context_FitToScreen;
    extern const wchar_t* Context_ZoomIn;
    extern const wchar_t* Context_ZoomOut;
    extern const wchar_t* Context_LockWindow;
    extern const wchar_t* Context_AlwaysOnTop;
    extern const wchar_t* Context_HUDGallery;
    extern const wchar_t* Context_LiteInfoPanel;
    extern const wchar_t* Context_FullInfoPanel;
    extern const wchar_t* Context_RenderRAW;
    extern const wchar_t* Context_PixelArtMode;
    extern const wchar_t* Context_Fullscreen;
    extern const wchar_t* Context_SpanDisplays;
    extern const wchar_t* Context_View;
    extern const wchar_t* Context_WallpaperFill;
    extern const wchar_t* Context_WallpaperFit;
    extern const wchar_t* Context_WallpaperTile;
    extern const wchar_t* Context_SetAsWallpaper;
    extern const wchar_t* Context_Rename;
    extern const wchar_t* Context_FixExtension;
    extern const wchar_t* Context_Delete;
    extern const wchar_t* Context_Settings;
    extern const wchar_t* Context_About;
    extern const wchar_t* Context_CompareMode; // New
    extern const wchar_t* Context_Exit;
    
    // Messages
    extern const wchar_t* Message_SaveErrorTitle;
    extern const wchar_t* Message_SaveErrorContent;
    
    // Toolbar Tooltips
    extern const wchar_t* Toolbar_Tooltip_Prev;
    extern const wchar_t* Toolbar_Tooltip_Next;
    extern const wchar_t* Toolbar_Tooltip_RotateL;
    extern const wchar_t* Toolbar_Tooltip_RotateR;
    extern const wchar_t* Toolbar_Tooltip_FlipH;
    extern const wchar_t* Toolbar_Tooltip_Lock;
    extern const wchar_t* Toolbar_Tooltip_Unlock;
    extern const wchar_t* Toolbar_Tooltip_Gallery;
    extern const wchar_t* Toolbar_Tooltip_Info;
    extern const wchar_t* Toolbar_Tooltip_RawPreview; // Fast
    extern const wchar_t* Toolbar_Tooltip_RawFull;    // Full
    extern const wchar_t* Toolbar_Tooltip_FixExtension;
    extern const wchar_t* Toolbar_Tooltip_Pin;
    extern const wchar_t* Toolbar_Tooltip_Unpin;
    extern const wchar_t* Toolbar_Tooltip_NormalMode;
    extern const wchar_t* Toolbar_Tooltip_CompareMode;
    extern const wchar_t* Toolbar_Tooltip_CompareOpen;
    extern const wchar_t* Toolbar_Tooltip_CompareSwap;
    extern const wchar_t* Toolbar_Tooltip_CompareLayout;
    extern const wchar_t* Toolbar_Tooltip_CompareInfo;
    extern const wchar_t* Toolbar_Tooltip_CompareDelete;
    extern const wchar_t* Toolbar_Tooltip_CompareZoomIn;
    extern const wchar_t* Toolbar_Tooltip_CompareZoomOut;
    extern const wchar_t* Toolbar_Tooltip_CompareSyncZoomOn;
    extern const wchar_t* Toolbar_Tooltip_CompareSyncZoomOff;
    extern const wchar_t* Toolbar_Tooltip_CompareSyncPanOn;
    extern const wchar_t* Toolbar_Tooltip_CompareSyncPanOff;
    extern const wchar_t* Toolbar_Tooltip_CompareExit;

    // OSD Messages
    extern const wchar_t* OSD_Copied;
    extern const wchar_t* OSD_CoordinatesCopied;
    extern const wchar_t* OSD_FilePathCopied;
    extern const wchar_t* OSD_Zoom100;
    extern const wchar_t* OSD_ZoomFit;
    extern const wchar_t* OSD_PrintInstruction;
    extern const wchar_t* OSD_MovedToRecycleBin;
    extern const wchar_t* OSD_WindowLocked;
    extern const wchar_t* OSD_WindowUnlocked;
    extern const wchar_t* OSD_AlwaysOnTopOn;
    extern const wchar_t* OSD_AlwaysOnTopOff;
    extern const wchar_t* OSD_WallpaperSet;
    extern const wchar_t* OSD_WallpaperFailed;
    extern const wchar_t* OSD_Renamed;
    extern const wchar_t* OSD_RenameFailed;
    extern const wchar_t* OSD_Restored; // New
    extern const wchar_t* OSD_ExtensionFixed;
    extern const wchar_t* OSD_FirstImage;
    extern const wchar_t* OSD_LastImage;
    extern const wchar_t* OSD_HD; // High Definition / Full Load
    extern const wchar_t* OSD_ZoomPrefix;

    
    // Checkbox Labels
    extern const wchar_t* Checkbox_AlwaysSaveLossless;
    extern const wchar_t* Checkbox_AlwaysSaveEdgeAdapted;
    extern const wchar_t* Checkbox_AlwaysSaveLossy;
    
    // HEIC Codec Missing
    extern const wchar_t* OSD_HEICCodecMissing;
    extern const wchar_t* Dialog_HEICTitle;
    extern const wchar_t* Dialog_HEICContent;
    extern const wchar_t* Dialog_HEICGetExtension;
    extern const wchar_t* Dialog_Cancel;

    // Settings UI
    extern const wchar_t* Settings_Tab_General;
    extern const wchar_t* Settings_Tab_About;
    
    extern const wchar_t* Settings_Group_Foundation;
    extern const wchar_t* Settings_Group_Startup;
    extern const wchar_t* Settings_Group_Habits; // was "Habits"

    extern const wchar_t* Settings_Label_Language;
    extern const wchar_t* Settings_Label_SingleInstance;
    extern const wchar_t* Settings_Label_CheckUpdates;
    extern const wchar_t* Settings_Label_LoopNav;
    extern const wchar_t* Settings_Label_ConfirmDel;
    extern const wchar_t* Settings_Label_Portable;
    extern const wchar_t* Settings_Label_SpanDisplays;
    extern const wchar_t* Settings_Label_UIScale;
    
    // Settings Status Messages
    extern const wchar_t* Settings_Status_RestartRequired;
    extern const wchar_t* Settings_Status_NoWritePerm;
    extern const wchar_t* Settings_Status_Enabled;

    extern const wchar_t* Settings_Header_PoweredBy;
    extern const wchar_t* Settings_Text_Copyright;

    // Tabs
    extern const wchar_t* Settings_Tab_Visuals;
    extern const wchar_t* Settings_Tab_Controls;
    extern const wchar_t* Settings_Tab_Image;
    extern const wchar_t* Settings_Tab_Advanced;

    // Visuals
    extern const wchar_t* Settings_Header_Backdrop;
    extern const wchar_t* Settings_Header_Window;
    extern const wchar_t* Settings_Header_WindowLock;
    extern const wchar_t* Settings_Header_Panel;
    
    extern const wchar_t* Settings_Label_CanvasColor;
    extern const wchar_t* Settings_Label_Overlay;
    extern const wchar_t* Settings_Label_ShowGrid;
    extern const wchar_t* Settings_Label_CrossFade;
    extern const wchar_t* Settings_Label_AlwaysOnTop;
    extern const wchar_t* Settings_Label_LockWindow;
    extern const wchar_t* Settings_Label_AutoHideTitle;
    extern const wchar_t* Settings_Label_RoundedCorners; // [v3.1.2]
    extern const wchar_t* Settings_Label_LockToolbar;
    extern const wchar_t* Settings_Label_WindowMinSize;
    extern const wchar_t* Settings_Label_WindowMaxSizePercent;

    extern const wchar_t* Settings_Label_KeepWindowSizeOnNav;
    extern const wchar_t* Settings_Label_RememberLastWindowSize;
    extern const wchar_t* Settings_Label_UpscaleSmallImagesWhenLocked;
    extern const wchar_t* Settings_Label_ExifMode;
    extern const wchar_t* Settings_Label_ToolbarInfoDefault;
    extern const wchar_t* Settings_Label_OpenFullScreenMode;
    
    extern const wchar_t* Settings_Option_Black;
    extern const wchar_t* Settings_Option_White;
    extern const wchar_t* Settings_Option_Grid;
    extern const wchar_t* Settings_Option_Custom;
    extern const wchar_t* Settings_Option_ZoomAuto;
    extern const wchar_t* Settings_Option_Off;
    extern const wchar_t* Settings_Option_Lite;
    extern const wchar_t* Settings_Option_Full;
    extern const wchar_t* Settings_Option_LargeOnly;
    extern const wchar_t* Settings_Option_All;

    // Controls
    extern const wchar_t* Settings_Header_Mouse;
    extern const wchar_t* Settings_Header_Edge;
    
    extern const wchar_t* Settings_Label_InvertWheel;
    extern const wchar_t* Settings_Label_ZoomSnapDamping; // New
    extern const wchar_t* Settings_Label_MouseAnchorZoom;
    extern const wchar_t* Settings_Label_InvertButtons;
    extern const wchar_t* Settings_Label_ZoomModeIn;
    extern const wchar_t* Settings_Label_ZoomModeOut;
    extern const wchar_t* Settings_Label_LeftDrag;
    extern const wchar_t* Settings_Label_MiddleDrag;
    extern const wchar_t* Settings_Label_MiddleClick;
    extern const wchar_t* Settings_Label_EdgeNavClick;
    extern const wchar_t* Settings_Label_NavIndicator;
    
    extern const wchar_t* Settings_Option_Window;
    extern const wchar_t* Settings_Option_Pan;
    extern const wchar_t* Settings_Option_None;
    extern const wchar_t* Settings_Option_Exit;
    extern const wchar_t* Settings_Option_Arrow;
    extern const wchar_t* Settings_Option_Cursor;
    extern const wchar_t* Settings_Option_Manual;
    extern const wchar_t* Settings_Option_Linear;
    extern const wchar_t* Settings_Option_Nearest;
    extern const wchar_t* Settings_Option_HighQualityCubic;

    // Image
    extern const wchar_t* Settings_Header_Render;
    extern const wchar_t* Settings_Header_Prompts;
    extern const wchar_t* Settings_Header_System;
    
    extern const wchar_t* Settings_Label_AutoRotate;
    extern const wchar_t* Settings_Label_CMS;
    extern const wchar_t* Settings_Value_ComingSoon;
    extern const wchar_t* Settings_Label_ForceRaw;
    extern const wchar_t* Settings_Label_AddToOpenWith;
    extern const wchar_t* Settings_Action_Add;
    extern const wchar_t* Settings_Action_Added;
    extern const wchar_t* Settings_Status_DisabledInPortable; // Hint for portable mode

    // Advanced
    extern const wchar_t* Settings_Header_Features;
    extern const wchar_t* Settings_Header_Performance;
    extern const wchar_t* Settings_Header_Transparency;
    
    extern const wchar_t* Settings_Label_DebugHUD;
    extern const wchar_t* Settings_Label_Prefetch;
    extern const wchar_t* Settings_Label_InfoPanelAlpha;
    extern const wchar_t* Settings_Label_ToolbarAlpha;
    extern const wchar_t* Settings_Label_SettingsAlpha;
    extern const wchar_t* Settings_Label_Reset;
    extern const wchar_t* Settings_Action_Restore;
    extern const wchar_t* Settings_Action_Done;
    
    // About
    extern const wchar_t* Settings_Action_CheckUpdates;
    extern const wchar_t* Settings_Action_ViewUpdate;
    extern const wchar_t* Settings_Status_Checking;
    extern const wchar_t* Settings_Status_UpToDate;
    extern const wchar_t* Settings_Link_GitHub;
    extern const wchar_t* Settings_Link_ReportIssue;
    extern const wchar_t* Settings_Link_Hotkeys;
    extern const wchar_t* Settings_Label_Version;
    extern const wchar_t* Settings_Label_Build;

    // Update Dialog (Bilingual)
    extern const wchar_t* Dialog_UpdateTitle;
    extern const wchar_t* Dialog_UpdateContent;
    extern const wchar_t* Dialog_UpdateLogHeader;
    extern const wchar_t* Dialog_ButtonUpdate;
    extern const wchar_t* Dialog_ButtonLater;
    extern const wchar_t* Dialog_ButtonStar;
    extern const wchar_t* Dialog_Update_LoveTitle;
    extern const wchar_t* Dialog_Update_LoveMessage;

    // Help Overlay
    extern const wchar_t* Help_Header_Shortcuts;
    extern const wchar_t* Help_Header_Mouse;
    extern const wchar_t* Help_Item_NextPrev;
    extern const wchar_t* Help_Item_Zoom;
    extern const wchar_t* Help_Item_Pan;
    extern const wchar_t* Help_Item_Rotate;
    extern const wchar_t* Help_Item_Fit;
    extern const wchar_t* Help_Item_Delete;
    extern const wchar_t* Help_Item_Fullscreen;
    extern const wchar_t* Help_Item_Close;
    extern const wchar_t* Help_Item_Compare; // New
    extern const wchar_t* Help_Mouse_Left;
    extern const wchar_t* Help_Mouse_Middle;
    extern const wchar_t* Help_Mouse_Wheel;
    extern const wchar_t* Help_Mouse_Right;
    extern const wchar_t* Help_Action_MoveWindow;
    extern const wchar_t* Help_Action_PanImage;
    extern const wchar_t* Help_Action_ContextMenu;
    extern const wchar_t* Help_Action_NextPrev;
    extern const wchar_t* Help_Action_Zoom;
    extern const wchar_t* Help_Action_SmartZoom;
    extern const wchar_t* Help_Desc_Copy;
    extern const wchar_t* Help_Desc_Edit;
    
    // Glossary / Tips
    extern const wchar_t* Help_Header_Tips;
    extern const wchar_t* Help_Tip_ContextScope;
    extern const wchar_t* Help_Tip_Rotation;
    extern const wchar_t* Help_Tip_VideoWall;
    extern const wchar_t* Help_Tip_DesignerMode;
    extern const wchar_t* Help_Tip_Raw;
    extern const wchar_t* Help_Tip_JpegQ;


    extern const wchar_t* Settings_Option_Auto;
    extern const wchar_t* Settings_Option_Eco;
    extern const wchar_t* Settings_Option_Balanced;
    extern const wchar_t* Settings_Option_Ultra;
    
    // Compare HUD (Mode 1/2) Localized Labels
    extern const wchar_t* HUD_Group_Physical;
    extern const wchar_t* HUD_Group_Scientific;
    extern const wchar_t* HUD_Group_Encoding;

    // HUD Tooltips
    extern const wchar_t* HUD_Tip_Sharp_Desc;
    extern const wchar_t* HUD_Tip_Sharp_High;
    extern const wchar_t* HUD_Tip_Sharp_Low;
    extern const wchar_t* HUD_Tip_Sharp_Ref;

    extern const wchar_t* HUD_Tip_Ent_Desc;
    extern const wchar_t* HUD_Tip_Ent_High;
    extern const wchar_t* HUD_Tip_Ent_Low;
    extern const wchar_t* HUD_Tip_Ent_Ref;

    extern const wchar_t* HUD_Tip_BPP_Desc;
    extern const wchar_t* HUD_Tip_BPP_High;
    extern const wchar_t* HUD_Tip_BPP_Low;
    extern const wchar_t* HUD_Tip_BPP_Ref;
    
    extern const wchar_t* HUD_Label_High;
    extern const wchar_t* HUD_Label_Low;
    extern const wchar_t* HUD_Label_Ref;
}
