#pragma once
#include <cstdint>
#include <string>

enum class HotkeyAction : uint8_t;
enum class MouseTrigger : uint8_t;
enum class MouseActionTarget : int;

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
        Spanish = 7,
        French = 8
    };

    void Init();
    void SetLanguage(Language lang);

    struct Strings {
        const wchar_t* CurrentLocale = nullptr;


        // OSD Messages
        const wchar_t* OSD_NoImage = nullptr;
        const wchar_t* OSD_FolderEmpty = nullptr;
        const wchar_t* OSD_SvgStaticFallback = nullptr;
        const wchar_t* OSD_Lossless = nullptr;
        const wchar_t* OSD_ReencodedLossless = nullptr;
        const wchar_t* OSD_EdgeAdapted = nullptr;
        const wchar_t* OSD_Reencoded = nullptr;
        const wchar_t* OSD_ReadOnly = nullptr;
        const wchar_t* OSD_NotPerfect = nullptr;
        const wchar_t* OSD_SpanOn = nullptr;
        const wchar_t* OSD_SpanOff = nullptr;
        const wchar_t* OSD_CompareBefore = nullptr;
        const wchar_t* OSD_CompareAfter = nullptr;
        const wchar_t* OSD_CompareAnimNotSupported = nullptr;
        const wchar_t* OSD_UndoDeleteSuccess = nullptr;
        const wchar_t* OSD_UndoDeleteFailed = nullptr;
        const wchar_t* OSD_UndoRenameSuccess = nullptr;
        const wchar_t* OSD_UndoRenameFailed = nullptr;
        const wchar_t* OSD_UndoTransformSuccess = nullptr;
        const wchar_t* OSD_UndoTransformFailed = nullptr;
        // Transform Action Names
        const wchar_t* Action_RotateCW = nullptr;
        const wchar_t* Action_RotateCCW = nullptr;
        const wchar_t* Action_Rotate180 = nullptr;
        const wchar_t* Action_FlipH = nullptr;
        const wchar_t* Action_FlipV = nullptr;

        // Dialog Strings
        const wchar_t* Dialog_SaveTitle = nullptr;
        const wchar_t* Dialog_SaveContent = nullptr;
        const wchar_t* Dialog_ButtonSave = nullptr;
        const wchar_t* Dialog_ButtonSaveAs = nullptr;
        const wchar_t* Dialog_ButtonDiscard = nullptr;
        const wchar_t* Dialog_ButtonContinue = nullptr;
        const wchar_t* Dialog_CropUnsavedTitle = nullptr;
        const wchar_t* Dialog_SaveAsTitle = nullptr;
        const wchar_t* Dialog_ExportTitle = nullptr;
        const wchar_t* Dialog_ButtonOverwrite = nullptr;
        const wchar_t* Dialog_EmbedICC = nullptr;
        const wchar_t* Dialog_SizeEstimating = nullptr;
        const wchar_t* Dialog_ExportError = nullptr;

        // Context Menu
        const wchar_t* Context_Open = nullptr;
        const wchar_t* Context_Crop = nullptr;
        const wchar_t* Context_SuperResolution = nullptr;
        const wchar_t* Context_AiAction = nullptr;
        const wchar_t* Context_OpenWith = nullptr;
        const wchar_t* Context_Edit = nullptr;
        const wchar_t* Context_ShowInExplorer = nullptr;
        const wchar_t* Context_OpenFolder = nullptr;
        const wchar_t* Context_CopyPixels = nullptr;
        const wchar_t* Context_CopyFile = nullptr;
        const wchar_t* Context_CopyPath = nullptr;
        const wchar_t* Context_Print = nullptr;
        const wchar_t* Context_RotateCW = nullptr;
        const wchar_t* Context_RotateCCW = nullptr;
        const wchar_t* Context_FlipH = nullptr;
        const wchar_t* Context_FlipV = nullptr;
        const wchar_t* Context_Transform = nullptr;
        const wchar_t* Context_ActualSize = nullptr;
        const wchar_t* Context_FitToScreen = nullptr;
        const wchar_t* Context_FitWindow = nullptr;
        const wchar_t* Context_FillWindow = nullptr;
        const wchar_t* Context_ZoomIn = nullptr;
        const wchar_t* Context_ZoomOut = nullptr;
        const wchar_t* Context_LockWindow = nullptr;
        const wchar_t* Context_AlwaysOnTop = nullptr;
        const wchar_t* Context_HUDGallery = nullptr;
        const wchar_t* Context_LiteInfoPanel = nullptr;
        const wchar_t* Context_FullInfoPanel = nullptr;
        const wchar_t* Context_RenderRAW = nullptr;
        const wchar_t* Context_PixelArtMode = nullptr;
        const wchar_t* Context_Fullscreen = nullptr;
        const wchar_t* Context_SlideshowMode = nullptr;
        const wchar_t* Context_SpanDisplays = nullptr;
        const wchar_t* Context_View = nullptr;
        const wchar_t* Context_WallpaperFill = nullptr;
        const wchar_t* Context_WallpaperFit = nullptr;
        const wchar_t* Context_WallpaperTile = nullptr;
        const wchar_t* Context_SetAsWallpaper = nullptr;
        const wchar_t* Context_Rename = nullptr;
        const wchar_t* Context_FixExtension = nullptr;
        const wchar_t* Context_Delete = nullptr;
        const wchar_t* Context_UndoDelete = nullptr;
        const wchar_t* Context_UndoRename = nullptr;
        const wchar_t* Context_UndoTransform = nullptr;
        const wchar_t* Context_Undo = nullptr; // New
        const wchar_t* Context_SortBy = nullptr;
        const wchar_t* Context_NavOrder = nullptr;
        const wchar_t* Context_SortAscending = nullptr;
        const wchar_t* Context_SortDescending = nullptr;
        const wchar_t* Context_Settings = nullptr;
        const wchar_t* Context_CompareMode = nullptr; // New
        const wchar_t* Context_OverlayMode = nullptr;
        const wchar_t* Context_GalleryOpenCompare = nullptr;
        const wchar_t* Context_GalleryOpenNewWindow = nullptr;
        const wchar_t* Context_Exit = nullptr;

        // Taskbar / System Menu
        const wchar_t* Menu_ExitPassthrough = nullptr;

        // Messages
        const wchar_t* Message_SaveErrorTitle = nullptr;
        const wchar_t* Message_SaveErrorContent = nullptr;

        // Toolbar Tooltips
        const wchar_t* Toolbar_Tooltip_Prev = nullptr;
        const wchar_t* Toolbar_Tooltip_Next = nullptr;
        const wchar_t* Toolbar_Tooltip_RotateL = nullptr;
        const wchar_t* Toolbar_Tooltip_RotateR = nullptr;
        const wchar_t* Toolbar_Tooltip_FlipH = nullptr;
        const wchar_t* Toolbar_Tooltip_Lock = nullptr;
        const wchar_t* Toolbar_Tooltip_Unlock = nullptr;
        const wchar_t* Toolbar_Tooltip_Gallery = nullptr;
        const wchar_t* Toolbar_Tooltip_Info = nullptr;
        const wchar_t* Toolbar_Tooltip_RawPreview = nullptr; // Fast
        const wchar_t* Toolbar_Tooltip_RawFull = nullptr;    // Full
        const wchar_t* Toolbar_Tooltip_RawPairView = nullptr; // Paired item: switch to the RAW
        const wchar_t* Toolbar_Tooltip_RawPairBack = nullptr; // Paired RAW view: back to rendered
        const wchar_t* Toolbar_Tooltip_FixExtension = nullptr;
        const wchar_t* Toolbar_Tooltip_Pin = nullptr;
        const wchar_t* Toolbar_Tooltip_Unpin = nullptr;
        const wchar_t* Toolbar_Tooltip_GamutWarning = nullptr;
        const wchar_t* Toolbar_Tooltip_CropCopy = nullptr;
        const wchar_t* Toolbar_Tooltip_CropSave = nullptr;
        const wchar_t* Toolbar_Tooltip_CropApply = nullptr;
        const wchar_t* Toolbar_Tooltip_CropCancel = nullptr;

        // Overlay Mode Tooltips
        const wchar_t* Toolbar_Tooltip_OverlayAlphaUp = nullptr;
        const wchar_t* Toolbar_Tooltip_OverlayAlphaDown = nullptr;
        const wchar_t* Toolbar_Tooltip_OverlayPassthroughOn = nullptr;
        const wchar_t* Toolbar_Tooltip_OverlayPassthroughOff = nullptr;
        const wchar_t* Toolbar_Tooltip_OverlayExit = nullptr;
        const wchar_t* Toolbar_Tooltip_OverlayZoomIn = nullptr;
        const wchar_t* Toolbar_Tooltip_OverlayZoomOut = nullptr;
        const wchar_t* Toolbar_Tooltip_NormalMode = nullptr;
        const wchar_t* Toolbar_Tooltip_CompareMode = nullptr;
        const wchar_t* Toolbar_Tooltip_SinglePage = nullptr;
        const wchar_t* Toolbar_Tooltip_DualPage = nullptr;
        const wchar_t* Toolbar_Tooltip_CompareOpen = nullptr;
        const wchar_t* Toolbar_Tooltip_CompareSwap = nullptr;
        const wchar_t* Toolbar_Tooltip_CompareLayout = nullptr;
        const wchar_t* Toolbar_Tooltip_CompareInfo = nullptr;
        const wchar_t* Toolbar_Tooltip_CompareDelete = nullptr;
        const wchar_t* Toolbar_Tooltip_CompareZoomIn = nullptr;
        const wchar_t* Toolbar_Tooltip_CompareZoomOut = nullptr;
        const wchar_t* Toolbar_Tooltip_CompareSyncZoomOn = nullptr;
        const wchar_t* Toolbar_Tooltip_CompareSyncZoomOff = nullptr;
        const wchar_t* Toolbar_Tooltip_CompareSyncPanOn = nullptr;
        const wchar_t* Toolbar_Tooltip_CompareSyncPanOff = nullptr;
        const wchar_t* Toolbar_Tooltip_SlideshowImmersiveToggle = nullptr;
        const wchar_t* Toolbar_Tooltip_SlideshowExit = nullptr;
        const wchar_t* Toolbar_Tooltip_CompareExit = nullptr;
        const wchar_t* Toolbar_Tooltip_AnimPlay = nullptr;
        const wchar_t* Toolbar_Tooltip_AnimPause = nullptr;
        const wchar_t* Toolbar_Tooltip_AnimPrev = nullptr;
        const wchar_t* Toolbar_Tooltip_AnimNext = nullptr;
        const wchar_t* Toolbar_Tooltip_AnimDirtyOn = nullptr;
        const wchar_t* Toolbar_Tooltip_AnimDirtyOff = nullptr;
        const wchar_t* Toolbar_Tooltip_AnimSpeed = nullptr;

        // OSD Messages
        const wchar_t* OSD_Copied = nullptr;
        const wchar_t* OSD_PixelsCopied = nullptr;
        const wchar_t* OSD_PixelsExtracting = nullptr;
        const wchar_t* OSD_FileCopied = nullptr;
        const wchar_t* OSD_CoordinatesCopied = nullptr;
        const wchar_t* OSD_FilePathCopied = nullptr;
        const wchar_t* OSD_Zoom100 = nullptr;
        const wchar_t* OSD_ZoomFit = nullptr;
        const wchar_t* OSD_ZoomFitWindow = nullptr;
        const wchar_t* OSD_ZoomFill = nullptr;
        const wchar_t* OSD_PrintInstruction = nullptr;
        const wchar_t* OSD_MovedToRecycleBin = nullptr;
        const wchar_t* OSD_WindowLocked = nullptr;
        const wchar_t* OSD_WindowUnlocked = nullptr;
        const wchar_t* OSD_AlwaysOnTopOn = nullptr;
        const wchar_t* OSD_AlwaysOnTopOff = nullptr;
        const wchar_t* OSD_WallpaperSet = nullptr;
        const wchar_t* OSD_WallpaperFailed = nullptr;
        const wchar_t* OSD_Renamed = nullptr;
        const wchar_t* OSD_RenameFailed = nullptr;
        const wchar_t* OSD_Restored = nullptr; // New
        const wchar_t* OSD_ExtensionFixed = nullptr;
        const wchar_t* OSD_FirstImage = nullptr;
        const wchar_t* OSD_LastImage = nullptr;
        const wchar_t* OSD_HD = nullptr; // High Definition / Full Load
        const wchar_t* OSD_ZoomPrefix = nullptr;
        const wchar_t* OSD_AnimPlaying = nullptr;
        const wchar_t* OSD_AnimPaused = nullptr;
        const wchar_t* OSD_AnimDirtyOn = nullptr;
        const wchar_t* OSD_AnimDirtyOff = nullptr;
        const wchar_t* OSD_GamutDetected = nullptr;
        const wchar_t* OSD_GamutIncompatible = nullptr;
        const wchar_t* OSD_GamutFailed = nullptr;
        const wchar_t* OSD_SlideshowStarted = nullptr;
        const wchar_t* OSD_SlideshowStopped = nullptr;
        const wchar_t* OSD_SlideshowResumed = nullptr;
        const wchar_t* OSD_SlideshowPaused = nullptr;
        const wchar_t* OSD_ImmersiveSpotlight = nullptr;
        const wchar_t* OSD_ImmersiveNormal = nullptr;
        const wchar_t* OSD_PrintJobStarted = nullptr;
        const wchar_t* OSD_PrintJobFinished = nullptr;
        const wchar_t* OSD_EnterCropMode = nullptr;
        const wchar_t* OSD_CropCopied = nullptr;
        const wchar_t* OSD_CropApplied = nullptr;

        const wchar_t* Context_ColorSpace = nullptr;

        // CMS Options
        const wchar_t* Settings_Option_CmsUnmanaged = nullptr;
        const wchar_t* Settings_Option_CmssRGB = nullptr;
        const wchar_t* Settings_Option_CmsP3 = nullptr;
        const wchar_t* Settings_Option_CmsAdobeRGB = nullptr;
        const wchar_t* Settings_Option_CmsGray = nullptr;
        const wchar_t* Settings_Option_CmsProPhoto = nullptr;


        // Checkbox Labels
        const wchar_t* Checkbox_AlwaysSaveLossless = nullptr;
        const wchar_t* Checkbox_AlwaysSaveEdgeAdapted = nullptr;
        const wchar_t* Checkbox_AlwaysSaveLossy = nullptr;
        const wchar_t* Checkbox_NeverConfirmDelete = nullptr;

        // HEIC Codec Missing
        const wchar_t* OSD_HEICCodecMissing = nullptr;
        const wchar_t* Dialog_HEICTitle = nullptr;
        const wchar_t* Dialog_HEICContent = nullptr;
        const wchar_t* Dialog_HEICGetExtension = nullptr;
        const wchar_t* Dialog_Cancel = nullptr;

        // Settings UI
        const wchar_t* Settings_Tab_General = nullptr;
        const wchar_t* Settings_Tab_About = nullptr;
        const wchar_t* Settings_Tab_Shortcuts = nullptr;
        const wchar_t* Settings_Hotkey_PressKey = nullptr;
        const wchar_t* Settings_Hotkey_Conflict = nullptr;
        const wchar_t* Settings_Hotkey_ConflictPrompt = nullptr;
        const wchar_t* Settings_Hotkey_Restore = nullptr;
        const wchar_t* Settings_Hotkey_Restored = nullptr;
        const wchar_t* Settings_Hotkey_MouseTip = nullptr;

        // Gallery Trigger Mode (Top Hover Gallery)
        const wchar_t* Settings_Header_GalleryTrigger = nullptr;
        const wchar_t* Settings_Label_GalleryTriggerMode = nullptr;
        const wchar_t* Settings_Label_KeepGalleryVisibleOnThumbnailClick = nullptr;
        const wchar_t* Settings_Tooltip_KeepGalleryVisibleOnThumbnailClick = nullptr;
        const wchar_t* Settings_Label_GalleryExpandOnClickOnly = nullptr;
        const wchar_t* Settings_Tooltip_GalleryExpandOnClickOnly = nullptr;
        const wchar_t* Settings_Option_TriggerClick = nullptr;
        const wchar_t* Settings_Option_TriggerHover = nullptr;
        const wchar_t* Settings_Option_GalleryTriggerAuto = nullptr;
        const wchar_t* Settings_Option_GalleryTriggerDelay = nullptr;
        const wchar_t* Settings_Option_GalleryTriggerClick = nullptr;
        const wchar_t* Settings_Option_GalleryTriggerDisable = nullptr;
        const wchar_t* Settings_Tooltip_GalleryTrigger = nullptr;
        const wchar_t* Settings_Label_GalleryTriggerAreaHeight = nullptr;
        const wchar_t* Settings_Label_GalleryDwellTime = nullptr;
        const wchar_t* Settings_Label_GalleryExitDelay = nullptr;
        const wchar_t* Settings_Label_GalleryMinSize = nullptr;


        const wchar_t* Settings_Group_Foundation = nullptr;
        const wchar_t* Settings_Group_Startup = nullptr;
        const wchar_t* Settings_Group_Habits = nullptr; // was "Habits"

        const wchar_t* Settings_Label_Language = nullptr;
        const wchar_t* Settings_Label_SingleInstance = nullptr;
        const wchar_t* Settings_Label_CheckUpdates = nullptr;
        const wchar_t* Settings_Label_UpdateChannel = nullptr;
        const wchar_t* Settings_Option_UpdateStable = nullptr;
        const wchar_t* Settings_Option_UpdatePreRelease = nullptr;
        const wchar_t* Settings_Tooltip_PreRelease = nullptr;
        const wchar_t* Settings_Label_NavLoopMode = nullptr;
        const wchar_t* Settings_Label_SortOrder = nullptr;
        const wchar_t* Settings_Label_SortDescending = nullptr;
        const wchar_t* Settings_Label_SortArchivesByNameAscending = nullptr;
        const wchar_t* Settings_Label_ConfirmDel = nullptr;
        const wchar_t* Settings_Label_Portable = nullptr;
        const wchar_t* Settings_Tooltip_Portable = nullptr;
        const wchar_t* Settings_Label_SpanDisplays = nullptr;
        const wchar_t* Settings_Label_UIScale = nullptr;

        // Settings Status Messages
        const wchar_t* Settings_Status_RestartRequired = nullptr;
        const wchar_t* Settings_Status_NoWritePerm = nullptr;
        const wchar_t* Settings_Status_Enabled = nullptr;

        const wchar_t* Settings_Header_PoweredBy = nullptr;
        const wchar_t* Settings_Text_Copyright = nullptr;
        const wchar_t* Settings_Text_License = nullptr;

        // Tabs
        const wchar_t* Settings_Tab_Visuals = nullptr;
        const wchar_t* Settings_Tab_Controls = nullptr;
        const wchar_t* Settings_ControlSub_Mouse = nullptr;
        const wchar_t* Settings_ControlSub_Keyboard = nullptr;
        const wchar_t* Settings_ControlSub_Behavior = nullptr;
        const wchar_t* Settings_Gesture_Preset = nullptr;
        const wchar_t* Settings_Gesture_Preset_Default = nullptr;
        const wchar_t* Settings_Gesture_Preset_Standard = nullptr;
        const wchar_t* Settings_Gesture_Preset_Browser = nullptr;
        const wchar_t* Settings_Gesture_Add = nullptr;
        const wchar_t* Settings_Gesture_Reset = nullptr;
        const wchar_t* Settings_Tab_Image = nullptr;
        const wchar_t* Settings_Tab_Advanced = nullptr;

        // Visuals
        const wchar_t* Settings_Header_Backdrop = nullptr;
        const wchar_t* Settings_Header_Window = nullptr;
        const wchar_t* Settings_Header_WindowLock = nullptr;
        const wchar_t* Settings_Header_Panel = nullptr;

        const wchar_t* Settings_Label_CanvasColor = nullptr;
        const wchar_t* Settings_Option_Effects = nullptr;
        const wchar_t* Settings_Label_ContextMenuBackdrop = nullptr;
        const wchar_t* Settings_Label_CanvasEffectStyle = nullptr;
        const wchar_t* Settings_Tooltip_BackdropEffectsTip = nullptr;
        const wchar_t* Settings_Label_Overlay = nullptr;
        const wchar_t* Settings_Label_ShowGrid = nullptr;
        const wchar_t* Settings_Label_AlwaysOnTop = nullptr;
        const wchar_t* Settings_Label_LockWindow = nullptr;
        const wchar_t* Settings_Tooltip_LockWindow = nullptr;
        const wchar_t* Settings_Label_AutoHideTitle = nullptr;
        const wchar_t* Settings_Label_RoundedCorners = nullptr; // [v3.1.2]
        const wchar_t* Settings_Label_UIBorders = nullptr;
        const wchar_t* Settings_Tooltip_RoundedCorners = nullptr;
        const wchar_t* Settings_Label_LockToolbar = nullptr;
        const wchar_t* Settings_Label_WindowMinSize = nullptr;
        const wchar_t* Settings_Label_WindowMaxSizePercent = nullptr;
        const wchar_t* Settings_Label_ShowBorderIndicator = nullptr;
        const wchar_t* Settings_Tooltip_ShowBorderIndicator = nullptr;
        const wchar_t* Settings_Label_ShowNavigator = nullptr;
        const wchar_t* Settings_Tooltip_ShowNavigator = nullptr;

        const wchar_t* Settings_Label_KeepWindowSizeOnNav = nullptr;
        const wchar_t* Settings_Label_RememberLastWindowSizeAndPosition = nullptr;
        const wchar_t* Settings_Label_UpscaleSmallImagesWhenLocked = nullptr;
        const wchar_t* Settings_Label_SlideshowInterval = nullptr;
        const wchar_t* Settings_Label_SlideshowImmersive = nullptr;
        const wchar_t* Settings_Option_SlideshowNormal = nullptr;
        const wchar_t* Settings_Option_SlideshowSpotlight = nullptr;
        const wchar_t* Settings_Label_ExifMode = nullptr;
        const wchar_t* Settings_Label_ToolbarInfoDefault = nullptr;
        const wchar_t* Settings_Label_OpenFullScreenMode = nullptr;
        const wchar_t* Settings_Label_FullScreenZoomMode = nullptr;
        const wchar_t* Settings_Label_CustomLiteInfoPanel = nullptr;
        const wchar_t* Settings_Label_CustomFullInfoPanel = nullptr;
        const wchar_t* Settings_Label_InfoPanelScale = nullptr;
        const wchar_t* Settings_Label_ItemsInNormalMode = nullptr;
        const wchar_t* Settings_Label_ItemsInCompareMode = nullptr;
        const wchar_t* Settings_Label_SeparatorPreset = nullptr;
        const wchar_t* Settings_Option_FitScreen = nullptr;

        const wchar_t* Settings_Option_Black = nullptr;
        const wchar_t* Settings_Option_White = nullptr;
        const wchar_t* Settings_Option_Grid = nullptr;
        const wchar_t* Settings_Option_Custom = nullptr;
        const wchar_t* Settings_Option_Off = nullptr;
        const wchar_t* Settings_Option_On = nullptr;
        const wchar_t* Settings_Option_Lite = nullptr;
        const wchar_t* Settings_Option_Full = nullptr;
        const wchar_t* Settings_Option_LargeOnly = nullptr;
        const wchar_t* Settings_Option_All = nullptr;
        const wchar_t* Settings_Option_SoftProofing = nullptr;

        // Controls
        const wchar_t* Settings_Header_Mouse = nullptr;
        const wchar_t* Settings_Header_Edge = nullptr;

        const wchar_t* Settings_Label_InvertWheel = nullptr;
        const wchar_t* Settings_Label_MouseAnchorZoom = nullptr;
        const wchar_t* Settings_Label_RightButtonDragZoom = nullptr;
        const wchar_t* Settings_Label_WheelZoomSpeed = nullptr;
        const wchar_t* Settings_Tooltip_WheelZoomSpeed = nullptr;
        const wchar_t* Settings_Label_ThumbWheel = nullptr;
        const wchar_t* Settings_Tooltip_ThumbWheel = nullptr;
        const wchar_t* Settings_Tooltip_MiddleDrag = nullptr;
        const wchar_t* Settings_Label_DoubleClick = nullptr;
        const wchar_t* Settings_Option_DoubleClick_Smart = nullptr;
        const wchar_t* Settings_Option_DoubleClick_WheelMode1 = nullptr;
        const wchar_t* Settings_Option_DoubleClick_WheelMode2 = nullptr;
        const wchar_t* Settings_Tooltip_DoubleClick = nullptr;
        const wchar_t* Settings_Tooltip_PanStepNormal = nullptr;
        const wchar_t* OSD_WheelMode1_NextPrevZoom = nullptr;
        const wchar_t* OSD_WheelMode1_ZoomNextPrev = nullptr;
        const wchar_t* OSD_WheelMode2_Pan = nullptr;
        const wchar_t* OSD_WheelMode2_Default = nullptr;
        const wchar_t* Settings_Label_RightDragZoomSpeed = nullptr;
        const wchar_t* OSD_WheelZoomSpeed = nullptr;
        const wchar_t* Settings_Label_InvertButtons = nullptr;
        const wchar_t* Settings_Header_KeyboardPan = nullptr;
        const wchar_t* Settings_Label_PanStepNormal = nullptr;
        const wchar_t* Settings_Label_PanStepFast = nullptr;
        const wchar_t* Settings_Label_UseFixedZoom = nullptr;
        const wchar_t* Settings_Tooltip_UseFixedZoom = nullptr;
        const wchar_t* Settings_Label_FixedZoomLevels = nullptr;
        const wchar_t* Dialog_FixedZoomTitle = nullptr;
        const wchar_t* Dialog_FixedZoomMsg = nullptr;
        const wchar_t* Settings_Label_ZoomModeIn = nullptr;
        const wchar_t* Settings_Tooltip_ZoomModeIn = nullptr;
        const wchar_t* Settings_Label_ZoomModeOut = nullptr;
        const wchar_t* Settings_Label_LeftDrag = nullptr;
        const wchar_t* Settings_Label_MiddleDrag = nullptr;
        const wchar_t* Settings_Label_MiddleClick = nullptr;
        const wchar_t* Settings_Label_EdgeNavClick = nullptr;
        const wchar_t* Settings_Label_ShowOSD = nullptr;
        const wchar_t* Settings_Label_DisableEdgeNavInCompare = nullptr;
        const wchar_t* Settings_Label_NavIndicator = nullptr;

        const wchar_t* Settings_Option_Window = nullptr;
        const wchar_t* Settings_Option_Pan = nullptr;
        const wchar_t* Settings_Option_None = nullptr;
        const wchar_t* Settings_Option_Exit = nullptr;
        const wchar_t* Settings_Option_Arrow = nullptr;
        const wchar_t* Settings_Option_Cursor = nullptr;
        const wchar_t* Settings_Option_Manual = nullptr;

        // Sort Options
        const wchar_t* Settings_Option_SortAuto = nullptr;
        const wchar_t* Settings_Option_SortName = nullptr;
        const wchar_t* Settings_Option_SortModified = nullptr;
        const wchar_t* Settings_Option_SortDateTaken = nullptr;
        const wchar_t* Settings_Option_SortSize = nullptr;
        const wchar_t* Settings_Option_SortType = nullptr;

        // Nav Options
        const wchar_t* Settings_Option_NavLoop = nullptr;
        const wchar_t* Settings_Option_NavThrough = nullptr;

        const wchar_t* Settings_Option_Linear = nullptr;
        const wchar_t* Settings_Option_Nearest = nullptr;
        const wchar_t* Settings_Option_HighQualityCubic = nullptr;
        const wchar_t* Settings_Option_FSR = nullptr;

        // Image
        const wchar_t* Settings_Header_Render = nullptr;
        const wchar_t* Settings_Header_Hdr = nullptr;
        const wchar_t* Settings_Header_Prompts = nullptr;
        const wchar_t* Settings_Header_System = nullptr;

        const wchar_t* Settings_Label_AutoRotate = nullptr;
        const wchar_t* Settings_Label_FsrSharpness = nullptr;
        const wchar_t* Settings_Tooltip_FsrSharpness = nullptr;
        // Plugins
        const wchar_t* Settings_Tab_Plugins = nullptr;
        const wchar_t* Settings_Label_EnableSrPlugin = nullptr;
        const wchar_t* Settings_Label_SrModel = nullptr;
        const wchar_t* Settings_Label_SrAutoTrigger = nullptr;
        const wchar_t* Settings_Label_SrAutoTriggerMaxWidth = nullptr;
        const wchar_t* Settings_Tooltip_SrAutoTriggerMaxWidth = nullptr;
        const wchar_t* Settings_Label_SrAutoTriggerMaxHeight = nullptr;
        const wchar_t* Settings_Tooltip_SrAutoTriggerMaxHeight = nullptr;
        const wchar_t* Settings_Value_Unlimited = nullptr;
        const wchar_t* Settings_Label_SrOpenInCompare = nullptr;
        const wchar_t* Settings_Label_SrPromptModel = nullptr;
        const wchar_t* Export_Checkbox_SuperResolution = nullptr;
        const wchar_t* Settings_Label_SrDebounce = nullptr;
        const wchar_t* Settings_Tooltip_SrDebounce = nullptr;
        const wchar_t* Settings_Header_PluginTriggers = nullptr;
        const wchar_t* Settings_Header_PluginParams = nullptr;
        const wchar_t* Settings_Label_SrStorageFolder = nullptr;
        const wchar_t* Settings_Button_OpenModelFolder = nullptr;
        const wchar_t* Settings_Prompt_DownloadModelFirst = nullptr;
        const wchar_t* Settings_Segment_InstalledPlugins = nullptr;
        const wchar_t* Settings_Segment_Marketplace = nullptr;
        const wchar_t* Settings_Label_SrEngine = nullptr;
        const wchar_t* Settings_Label_SrModelStatus = nullptr;
        const wchar_t* Settings_Format_DownloadModel = nullptr;
        const wchar_t* Settings_Header_SrComponent = nullptr;
        const wchar_t* Settings_Label_QuickEnableEngine = nullptr;
        const wchar_t* Settings_Button_QuickEnableNcnn = nullptr;
        const wchar_t* Settings_Label_OnlineExtensions = nullptr;
        const wchar_t* Settings_Button_BrowseMarketplace = nullptr;
        const wchar_t* Settings_Label_MarketManifest = nullptr;
        const wchar_t* Settings_Button_RefreshManifest = nullptr;
        const wchar_t* Settings_Button_SyncingManifest = nullptr;
        const wchar_t* Settings_Format_PluginPackageSize = nullptr;
        const wchar_t* Settings_Button_InstalledUpToDate = nullptr;
        const wchar_t* Settings_Format_UpdateTo = nullptr;
        const wchar_t* Settings_Button_InstallPlugin = nullptr;
        const wchar_t* Settings_Label_PluginUpdate = nullptr;
        const wchar_t* Settings_Label_UninstallPlugin = nullptr;
        const wchar_t* Settings_Button_UninstallPlugin = nullptr;

        // Super-Resolution OSD & Dialog
        const wchar_t* OSD_SrProcessing = nullptr;
        const wchar_t* OSD_SrSuccessFormat = nullptr;
        const wchar_t* OSD_SrSuccessFormatSimple = nullptr;
        const wchar_t* OSD_SrFailed = nullptr;
        const wchar_t* OSD_SrPluginDisabledOrMissing = nullptr;
        const wchar_t* OSD_SrAnimationProcessingFormat = nullptr;
        const wchar_t* OSD_SrAnimationSuccessFormat = nullptr;
        const wchar_t* OSD_SrVectorLosslessSkipped = nullptr;
        const wchar_t* OSD_SrAnimationTooManyFrames = nullptr;
        const wchar_t* OSD_SrImageTooLargeFormat = nullptr;
        const wchar_t* Dialog_Title_SelectSrModel = nullptr;
        const wchar_t* Dialog_Msg_SelectSrModel = nullptr;
        const wchar_t* Dialog_Checkbox_RememberSrModelChoice = nullptr;
        const wchar_t* Dialog_Button_OK = nullptr;
        const wchar_t* Settings_Label_CMS = nullptr;
        const wchar_t* Settings_Label_AdvancedColor = nullptr;
        const wchar_t* Settings_Label_CmsFallback = nullptr;
        const wchar_t* Settings_Label_HdrToneMapping = nullptr;
        const wchar_t* Settings_Label_HdrSplineKnee = nullptr;
        const wchar_t* Settings_Tooltip_HdrToneMapping = nullptr;
        const wchar_t* Settings_Tooltip_HdrSplineKnee = nullptr;
        const wchar_t* Settings_Label_HdrPeakNitsOverride = nullptr;
        const wchar_t* Settings_Tooltip_HdrPeakNitsOverride = nullptr;
        const wchar_t* Settings_Label_HdrPeakPercentile = nullptr;
        const wchar_t* Settings_Tooltip_HdrPeakPercentile = nullptr;
        const wchar_t* Settings_Option_HdrPeakPercentile_100 = nullptr;
        const wchar_t* Settings_Option_HdrPeakPercentile_99995 = nullptr;
        const wchar_t* Settings_Option_HdrPeakPercentile_999 = nullptr;
        const wchar_t* Settings_Label_HdrDesatThreshold = nullptr;
        const wchar_t* Settings_Tooltip_HdrDesatThreshold = nullptr;
        const wchar_t* Settings_Label_HdrMaxDesat = nullptr;
        const wchar_t* Settings_Tooltip_HdrMaxDesat = nullptr;
        const wchar_t* Settings_Option_HdrColorimetric = nullptr;
        const wchar_t* Settings_Option_HdrSpline = nullptr;
        const wchar_t* Settings_Option_HdrLegacyReinhard = nullptr;

        const wchar_t* Settings_Label_CustomProof = nullptr;
        const wchar_t* Settings_Label_CmsIntent = nullptr;
        const wchar_t* Settings_Label_GamutWarning = nullptr;
        const wchar_t* Settings_Tooltip_GamutWarning = nullptr;
        const wchar_t* Settings_Label_GamutAutoPrompt = nullptr;
        const wchar_t* Settings_Tooltip_GamutAutoPrompt = nullptr;
        const wchar_t* Settings_Label_GamutColor = nullptr;
        const wchar_t* Settings_Option_CmsIntentRelative = nullptr;
        const wchar_t* Settings_Option_CmsIntentPerceptual = nullptr;
        const wchar_t* Settings_Tooltip_CMS = nullptr;
        const wchar_t* Settings_Tooltip_CmsIntent = nullptr;
        const wchar_t* Settings_Tooltip_AdvancedColor = nullptr;
        const wchar_t* Settings_Tooltip_ZoomAuto = nullptr;
        const wchar_t* Context_SoftProofing = nullptr;
        const wchar_t* Context_SoftProofProfile = nullptr;
        const wchar_t* Context_SoftProofCustom = nullptr;
        const wchar_t* Settings_Value_ComingSoon = nullptr;
        const wchar_t* Settings_Label_ForceRaw = nullptr;
        const wchar_t* Settings_Label_PairRawJpeg = nullptr;
        const wchar_t* Settings_Tooltip_PairRawJpeg = nullptr;
        const wchar_t* Settings_Label_Exposure = nullptr;
        const wchar_t* Settings_Tooltip_Exposure = nullptr;
        const wchar_t* Settings_Label_AddToOpenWith = nullptr;
        const wchar_t* Settings_Label_CustomEditor = nullptr;
        const wchar_t* Context_SelectEditor = nullptr;
        const wchar_t* OSD_EditorLaunchFailed = nullptr;
        const wchar_t* Settings_Action_Add = nullptr;
        const wchar_t* Settings_Action_Added = nullptr;
        const wchar_t* Settings_Status_DisabledInPortable = nullptr; // Hint for portable mode

        // Advanced
        const wchar_t* Settings_Header_Features = nullptr;
        const wchar_t* Settings_Header_Performance = nullptr;
        const wchar_t* Settings_Header_Transparency = nullptr;

        const wchar_t* Settings_Label_DebugHUD = nullptr;
        const wchar_t* Settings_Label_Prefetch = nullptr;
        const wchar_t* Settings_Label_MemoryReclaim = nullptr;
        const wchar_t* Settings_Option_MemSmart = nullptr;
        const wchar_t* Settings_Option_MemAggressive = nullptr;
        const wchar_t* Settings_Option_MemOnDemand = nullptr;
        const wchar_t* Settings_Tooltip_MemoryReclaim = nullptr;

        const wchar_t* Settings_Label_Reset = nullptr;
        const wchar_t* Settings_Action_Restore = nullptr;
        const wchar_t* Settings_Action_Done = nullptr;
        const wchar_t* Settings_Header_ConfigManagement = nullptr;

        // Removed duplicated Color Management declarations

        // About
        const wchar_t* Settings_Action_CheckUpdates = nullptr;
        const wchar_t* Settings_Action_ViewUpdate = nullptr;
        const wchar_t* Settings_Status_Checking = nullptr;
        const wchar_t* Settings_Status_UpToDate = nullptr;
        const wchar_t* Settings_Link_GitHub = nullptr;
        const wchar_t* Settings_Link_ReportIssue = nullptr;
        const wchar_t* Settings_Link_Hotkeys = nullptr;
        const wchar_t* Settings_Label_Version = nullptr;
        const wchar_t* Settings_Label_Build = nullptr;

        // Update Dialog (Bilingual)
        const wchar_t* Dialog_UpdateTitle = nullptr;
        const wchar_t* Dialog_UpdateContent = nullptr;
        const wchar_t* Dialog_UpdateLogHeader = nullptr;
        const wchar_t* Dialog_ButtonUpdate = nullptr;
        const wchar_t* Dialog_ButtonLater = nullptr;
        const wchar_t* Dialog_ButtonStar = nullptr;
        const wchar_t* Dialog_Update_LoveTitle = nullptr;
        const wchar_t* Dialog_Update_LoveMessage = nullptr;

        // Passthrough Mode
        const wchar_t* Dialog_PassthroughTitle = nullptr;
        const wchar_t* Dialog_PassthroughContent = nullptr;
        const wchar_t* OSD_PassthroughOn = nullptr;
        const wchar_t* OSD_PassthroughOff = nullptr;
        const wchar_t* OSD_OverlayModeOn = nullptr;
        const wchar_t* OSD_OverlayModeOff = nullptr;
        const wchar_t* OSD_Opacity = nullptr;

        // Help Overlay
        const wchar_t* Help_Desc_Edit = nullptr;

        // Glossary / Tips
        const wchar_t* Help_Header_Tips = nullptr;
        const wchar_t* Help_Tip_ContextScope = nullptr;
        const wchar_t* Help_Tip_Rotation = nullptr;
        const wchar_t* Help_Tip_VideoWall = nullptr;
        const wchar_t* Help_Tip_DesignerMode = nullptr;
        const wchar_t* Help_Tip_GamutDetection = nullptr;
        const wchar_t* Help_Tip_Raw = nullptr;
        const wchar_t* Help_Tip_JpegQ = nullptr;
        const wchar_t* Help_Tip_SoftProofCompare = nullptr;



        const wchar_t* Settings_Option_Auto = nullptr;
        const wchar_t* Settings_Option_Eco = nullptr;
        const wchar_t* Settings_Option_Balanced = nullptr;
        const wchar_t* Settings_Option_Ultra = nullptr;

        // Compare HUD (Mode 1/2) Localized Labels
        const wchar_t* HUD_Group_Physical = nullptr;
        const wchar_t* HUD_Group_Scientific = nullptr;
        const wchar_t* HUD_Group_Encoding = nullptr;

        // HUD Tooltips
        const wchar_t* HUD_Tip_Sharp_Desc = nullptr;
        const wchar_t* HUD_Tip_Sharp_High = nullptr;
        const wchar_t* HUD_Tip_Sharp_Low = nullptr;
        const wchar_t* HUD_Tip_Sharp_Ref = nullptr;

        const wchar_t* HUD_Tip_Ent_Desc = nullptr;
        const wchar_t* HUD_Tip_Ent_High = nullptr;
        const wchar_t* HUD_Tip_Ent_Low = nullptr;
        const wchar_t* HUD_Tip_Ent_Ref = nullptr;

        const wchar_t* HUD_Tip_BPP_Desc = nullptr;
        const wchar_t* HUD_Tip_BPP_High = nullptr;
        const wchar_t* HUD_Tip_BPP_Low = nullptr;
        const wchar_t* HUD_Tip_BPP_Ref = nullptr;

        const wchar_t* HUD_Label_High = nullptr;
        const wchar_t* HUD_Label_Low = nullptr;
        const wchar_t* HUD_Label_Ref = nullptr;

        // Geek Glass Settings
        const wchar_t* Settings_Header_GeekGlass = nullptr;
        const wchar_t* Settings_Label_EnableGeekGlass = nullptr;
        const wchar_t* Settings_Label_GlassUIAnimations = nullptr;
        const wchar_t* Settings_Header_CoreMaterial = nullptr;
        const wchar_t* Settings_Label_BlurSigma = nullptr;
        const wchar_t* Settings_Status_GlassDisabled = nullptr;
        const wchar_t* Settings_Label_TintDensity = nullptr;
        const wchar_t* Settings_Tooltip_TintDensity = nullptr;
        const wchar_t* Settings_Label_SpecularOpacity = nullptr;
        const wchar_t* Settings_Tooltip_SpecularOpacity = nullptr;
        const wchar_t* Settings_Label_ShadowIntensity = nullptr;
        const wchar_t* Settings_Tooltip_ShadowIntensity = nullptr;
        const wchar_t* Settings_Header_VectorAssets = nullptr;
        const wchar_t* Settings_Label_VectorStrokeWeight = nullptr;
        const wchar_t* Settings_Option_StrokeStandard = nullptr;
        const wchar_t* Settings_Option_StrokeFine = nullptr;
        const wchar_t* Settings_Header_GlassTint = nullptr;
        const wchar_t* Settings_Label_TintProfile = nullptr;
        const wchar_t* Settings_Option_TintAuto = nullptr;
        const wchar_t* Settings_Option_TintCustom = nullptr;
        const wchar_t* Settings_Label_GlassCustomColor = nullptr;
        const wchar_t* Settings_Header_DensityMatrix = nullptr;
        const wchar_t* Settings_Label_OsdDensity = nullptr;
        const wchar_t* Settings_Tooltip_OsdDensity = nullptr;
        const wchar_t* Settings_Label_PanelsDensity = nullptr;
        const wchar_t* Settings_Tooltip_PanelsDensity = nullptr;
        const wchar_t* Settings_Label_ModalsDensity = nullptr;
        const wchar_t* Settings_Tooltip_ModalsDensity = nullptr;
        const wchar_t* Settings_Label_MenusDensity = nullptr;
        const wchar_t* Settings_Tooltip_MenusDensity = nullptr;

        // Professional Tools [v3.5]
        const wchar_t* Settings_Header_Professional = nullptr;
        const wchar_t* Settings_Label_ShowDirtyRect = nullptr;
        const wchar_t* Settings_Tooltip_ShowDirtyRect = nullptr;
        const wchar_t* Settings_Label_LoupeShape = nullptr;
        const wchar_t* Settings_Option_LoupeShapeSquare = nullptr;
        const wchar_t* Settings_Option_LoupeShapeCircle = nullptr;
        const wchar_t* Settings_Option_LoupeShapeFullscreen = nullptr;
        const wchar_t* Settings_Label_LoupeZoom = nullptr;
        const wchar_t* Settings_Tooltip_LoupeHotkey = nullptr;
        const wchar_t* Settings_Tooltip_CropModeHotkey = nullptr;

        const wchar_t* Settings_Tab_Theme = nullptr;
        const wchar_t* Settings_Label_ThemeMode = nullptr;
        const wchar_t* Settings_Option_ThemeAuto = nullptr;
        const wchar_t* Settings_Option_ThemeDark = nullptr;
        const wchar_t* Settings_Option_ThemeLight = nullptr;
        const wchar_t* Settings_Option_ThemeCustom = nullptr;
        const wchar_t* Settings_Label_AmbientDimmer = nullptr;
        const wchar_t* Settings_Tooltip_AmbientDimmer = nullptr;
        const wchar_t* Settings_Label_AccentColor = nullptr;
        const wchar_t* Settings_Option_AccentSystem = nullptr;
        const wchar_t* Settings_Option_AccentCustom = nullptr;
        const wchar_t* Settings_Label_WindowBorderAccent = nullptr;
        const wchar_t* Settings_Option_BorderFollowSystem = nullptr;
        const wchar_t* Settings_Option_BorderAccent = nullptr;
        const wchar_t* Settings_Label_ResetGlass = nullptr;
        const wchar_t* Settings_Action_ResetGlass = nullptr;
        const wchar_t* Settings_Header_ThemeManagement = nullptr;
        const wchar_t* Settings_Action_ExportTheme = nullptr;
        const wchar_t* Settings_Action_ImportTheme = nullptr;

        // AI Actions & Multi-modal System
        const wchar_t* Toolbar_Tooltip_CompareSave = nullptr;
        const wchar_t* Dialog_MultiLineInputHint = nullptr;
        const wchar_t* Dialog_AiApiKeyTitle = nullptr;
        const wchar_t* Dialog_AiApiKeyPrompt = nullptr;
        const wchar_t* Dialog_AiPromptTitle = nullptr;
        const wchar_t* Dialog_AiPromptPrompt = nullptr;
        const wchar_t* Dialog_AiActionNameTitle = nullptr;
        const wchar_t* Dialog_AiActionNamePrompt = nullptr;
        const wchar_t* Dialog_AiBaseUrlTitle = nullptr;
        const wchar_t* Dialog_AiBaseUrlPrompt = nullptr;
        const wchar_t* AiAction_Title = nullptr;
        const wchar_t* AiAction_EscHint = nullptr;
        const wchar_t* AiAction_ScopeCropAndBlend = nullptr;
        const wchar_t* AiAction_ScopeForceFull = nullptr;
        const wchar_t* AiAction_FooterQuickKeys = nullptr;
        const wchar_t* AiAction_Placeholder = nullptr;
        const wchar_t* AiAction_InpaintTitle = nullptr;
        const wchar_t* AiAction_InpaintDesc = nullptr;
        const wchar_t* AiAction_InpaintTag = nullptr;
        const wchar_t* AiAction_AdhocPrefix = nullptr;
        const wchar_t* AiAction_AdhocSend = nullptr;
        const wchar_t* AiAction_CurrentModel = nullptr;
        const wchar_t* AiAction_NoModelSelected = nullptr;
        const wchar_t* OSD_AiProcessing = nullptr;
        const wchar_t* OSD_AiGenerationSuccess = nullptr;
        const wchar_t* OSD_AiCompleted = nullptr;
        const wchar_t* OSD_AiPreparing = nullptr;
        const wchar_t* OSD_AiProcessingElapsed = nullptr;
        const wchar_t* OSD_AiConnecting = nullptr;
        const wchar_t* OSD_AiSamplingProgress = nullptr;
        const wchar_t* OSD_AiSamplingProgressEta = nullptr;
        const wchar_t* OSD_AiAdhocPrompt = nullptr;
        const wchar_t* OSD_AiTaskCancelled = nullptr;
        const wchar_t* OSD_AiInpaintGuide = nullptr;
        const wchar_t* OSD_AiInpaintGuideActionFormat = nullptr;
        const wchar_t* OSD_AiSelectionTooSmall = nullptr;
        const wchar_t* OSD_AiEmptyResult = nullptr;
        const wchar_t* OSD_AiDecodeFailed = nullptr;
        const wchar_t* OSD_AiTextureFailed = nullptr;
        const wchar_t* OSD_AiEmptyPromptHint = nullptr;
        const wchar_t* OSD_AiInpaintPromptEmptyHint = nullptr;
        const wchar_t* Settings_Tab_Ai = nullptr;
        const wchar_t* Settings_Header_AiProviders = nullptr;
        const wchar_t* Settings_Desc_AiProviders = nullptr;
        const wchar_t* Settings_Label_AiProviderName = nullptr;
        const wchar_t* Settings_Label_AiBaseUrl = nullptr;
        const wchar_t* Settings_Label_AiApiKey = nullptr;
        const wchar_t* Settings_Label_AiModelId = nullptr;
        const wchar_t* Settings_Label_AiModelUnconfigured = nullptr;
        const wchar_t* Settings_Label_AiMaxRes = nullptr;
        const wchar_t* Settings_Label_AiDefaultProfile = nullptr;
        const wchar_t* Settings_Button_AiSetDefault = nullptr;
        const wchar_t* Settings_Button_AiTestConn = nullptr;
        const wchar_t* Settings_Button_AiSaveProf = nullptr;
        const wchar_t* Settings_Button_AiDeleteProf = nullptr;
        const wchar_t* Settings_Button_AiAddProf = nullptr;
        const wchar_t* Settings_Header_AiActions = nullptr;
        const wchar_t* Settings_Button_AiAddAction = nullptr;
        const wchar_t* Settings_Label_AiActionName = nullptr;
        const wchar_t* Settings_Label_AiPromptTemplate = nullptr;
        const wchar_t* Settings_Label_AiExecModel = nullptr;
        const wchar_t* Settings_Option_AiDefaultModel = nullptr;
        const wchar_t* Settings_Label_AiScopeMode = nullptr;
        const wchar_t* Settings_Button_AiDeleteAction = nullptr;
        const wchar_t* Settings_AiModelCustom = nullptr;
        const wchar_t* Settings_AiModelFetching = nullptr;
        const wchar_t* Settings_AiModelFetchAuto = nullptr;
        const wchar_t* Settings_Option_AiRes4K = nullptr;
        const wchar_t* Settings_Option_AiRes2K = nullptr;
        const wchar_t* Settings_Option_AiRes1K = nullptr;
        const wchar_t* Settings_Option_AiResOriginal = nullptr;
        const wchar_t* Settings_Option_AiScopeForceFull = nullptr;
        const wchar_t* Settings_Option_AiScopeCropBlend = nullptr;
        const wchar_t* Settings_Option_AiCustomEndpoint = nullptr;
        const wchar_t* Settings_Option_AiLocalComfyUI = nullptr;
        const wchar_t* Settings_Option_AiLocalSdWebUI = nullptr;
        const wchar_t* OSD_AiFetchingModels = nullptr;
        const wchar_t* OSD_AiFetchModelsFailed = nullptr;
        const wchar_t* OSD_AiFetchModelsSuccessFormat = nullptr;
        const wchar_t* OSD_AiSetDefaultSuccessFormat = nullptr;
        const wchar_t* OSD_AiTestingConnection = nullptr;
        const wchar_t* OSD_AiConnSuccessFormat = nullptr;
        const wchar_t* OSD_AiSaveFailedBaseUrlEmpty = nullptr;
        const wchar_t* OSD_AiSaveFailedApiKeyEmpty = nullptr;
        const wchar_t* OSD_AiSaveFailedModelEmpty = nullptr;
        const wchar_t* OSD_AiSaveProfileSuccess = nullptr;
        const wchar_t* AiError_ApiKeyEmpty = nullptr;
        const wchar_t* AiError_ModelEmpty = nullptr;
        const wchar_t* AiError_InvalidUrl = nullptr;
        const wchar_t* AiError_InitWinHttpFailed = nullptr;
        const wchar_t* AiError_ConnectFailed = nullptr;
        const wchar_t* AiError_CreateReqFailed = nullptr;
        const wchar_t* AiError_NoValidDataReturned = nullptr;
        const wchar_t* AiError_RequestTimeout = nullptr;
        const wchar_t* AiError_HttpStatusPrefix = nullptr;
        const wchar_t* AiError_JsonParseFailed = nullptr;
        const wchar_t* AiError_NoModelsFound = nullptr;
        const wchar_t* AiTest_ConnSuccess = nullptr;
        const wchar_t* AiTest_AuthFailedPrefix = nullptr;
        const wchar_t* AiTest_HttpErrorPrefix = nullptr;
        const wchar_t* AiError_NoAvailableProfile = nullptr;

        // --- File Association Center ---
        const wchar_t* Settings_Tab_FileAssoc = nullptr;
        const wchar_t* Settings_Header_FileAssocStatus = nullptr;
        const wchar_t* Settings_Header_FileAssocOptions = nullptr;
        const wchar_t* Settings_Header_FileAssocFormats = nullptr;
        const wchar_t* Settings_Label_FileAssocIconStyle = nullptr;
        const wchar_t* Settings_Option_FileAssocIconNative = nullptr;
        const wchar_t* Settings_Option_FileAssocIconApp = nullptr;
        const wchar_t* Settings_Label_FileAssocContextMenu = nullptr;
        const wchar_t* Settings_Label_FileAssocAutoRepair = nullptr;
        const wchar_t* Settings_Label_FileAssocPresets = nullptr;
        const wchar_t* Settings_Action_PresetRecommended = nullptr;
        const wchar_t* Settings_Action_PresetAllImages = nullptr;
        const wchar_t* Settings_Action_PresetPhotoRaw = nullptr;
        const wchar_t* Settings_Action_PresetClearAll = nullptr;
        const wchar_t* Settings_Action_ApplyAssoc = nullptr;
        const wchar_t* Settings_Action_ApplyPending = nullptr;
        const wchar_t* Settings_Action_SetDefaultWin = nullptr;
        const wchar_t* Settings_Action_RepairAssoc = nullptr;
        const wchar_t* Settings_Label_FileAssocCatStandard = nullptr;
        const wchar_t* Settings_Label_FileAssocCatModern = nullptr;
        const wchar_t* Settings_Label_FileAssocCatPro = nullptr;
        const wchar_t* Settings_Label_FileAssocCatRaw = nullptr;
        const wchar_t* Settings_Label_FileAssocCatExtRaw = nullptr;
        const wchar_t* Settings_Label_FileAssocCatArchive = nullptr;
        const wchar_t* Settings_Tooltip_FileAssocArchive = nullptr;
        const wchar_t* Settings_Status_AssocApplied = nullptr;
        const wchar_t* Settings_Status_AssocSummaryFormat = nullptr;
        const wchar_t* Settings_Status_AssocBrokenHint = nullptr;
        const wchar_t* Settings_Status_AssocPendingHint = nullptr;
        const wchar_t* Settings_Status_AssocStoreManaged = nullptr;
        const wchar_t* Settings_Label_StoreUpdatesManaged = nullptr;
        const wchar_t* Settings_Action_OpenStore = nullptr;
        const wchar_t* Settings_Status_AssocLegend = nullptr;
        const wchar_t* Context_OpenWithQuickView = nullptr;

    };

    inline Strings g{};

    // Hotkey Action Name resolver
    std::wstring GetHotkeyActionName(::HotkeyAction action);

    // Mouse Gesture Name resolver
    std::wstring GetMouseModifierName(uint8_t mod);
    std::wstring GetMouseTriggerName(::MouseTrigger trigger);
    std::wstring GetMouseActionTargetName(::MouseActionTarget action);
}

