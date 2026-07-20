#include "pch.h"
#include "AppStrings.h"
#include <windows.h> // For GetUserDefaultUILanguage
#include "EditState.h"

namespace AppStrings {

// ----------------------------------------------------------------
// ----------------------------------------------------------------
// Global Pointers (Definition)
// ----------------------------------------------------------------
const wchar_t *CurrentLocale = L"en-us";
const wchar_t *OSD_NoImage = nullptr;
const wchar_t *OSD_Lossless = nullptr;
const wchar_t *OSD_ReencodedLossless = nullptr;
const wchar_t *OSD_EdgeAdapted = nullptr;
const wchar_t *OSD_Reencoded = nullptr;
const wchar_t *OSD_ReadOnly = nullptr;
const wchar_t *OSD_NotPerfect = nullptr;
const wchar_t *OSD_SpanOn = nullptr;
const wchar_t *OSD_SpanOff = nullptr;
const wchar_t *OSD_CompareBefore = nullptr;
const wchar_t *OSD_CompareAfter = nullptr;
const wchar_t *OSD_UndoDeleteSuccess = nullptr;
const wchar_t *OSD_UndoDeleteFailed = nullptr;
const wchar_t *OSD_GamutDetected = nullptr;
const wchar_t *OSD_GamutIncompatible = nullptr;
const wchar_t *OSD_GamutFailed = nullptr;
const wchar_t *Action_RotateCW = nullptr;
const wchar_t *Action_RotateCCW = nullptr;
const wchar_t *Action_Rotate180 = nullptr;
const wchar_t *Action_FlipH = nullptr;
const wchar_t *Action_FlipV = nullptr;
const wchar_t *Dialog_SaveTitle = nullptr;
const wchar_t *Dialog_SaveContent = nullptr;
const wchar_t *Dialog_ButtonSave = nullptr;
const wchar_t *Dialog_ButtonSaveAs = nullptr;
const wchar_t *Dialog_ButtonDiscard = nullptr;
const wchar_t *Dialog_ButtonContinue = nullptr;
const wchar_t *Checkbox_AlwaysSaveLossless = nullptr;
const wchar_t *Checkbox_AlwaysSaveEdgeAdapted = nullptr;
const wchar_t *Checkbox_AlwaysSaveLossy = nullptr;
const wchar_t *Checkbox_NeverConfirmDelete = nullptr;
const wchar_t *Context_UndoDelete = nullptr;
const wchar_t *Context_UndoRename = nullptr;
const wchar_t *Context_UndoTransform = nullptr;
const wchar_t *Context_Undo = nullptr; // New
const wchar_t *OSD_UndoRenameSuccess = nullptr;
const wchar_t *OSD_UndoRenameFailed = nullptr;
const wchar_t *OSD_UndoTransformSuccess = nullptr;
const wchar_t *OSD_UndoTransformFailed = nullptr;
const wchar_t *OSD_HEICCodecMissing = nullptr;
const wchar_t *Dialog_HEICTitle = nullptr;
const wchar_t *Dialog_HEICContent = nullptr;
const wchar_t *Dialog_HEICGetExtension = nullptr;
const wchar_t *Dialog_Cancel = nullptr;
const wchar_t *Settings_Tab_General = nullptr;
const wchar_t *Settings_Tab_About = nullptr;
const wchar_t *Settings_Tab_Shortcuts = nullptr;
const wchar_t *Settings_Hotkey_PressKey = nullptr;
const wchar_t *Settings_Hotkey_Conflict = nullptr;
const wchar_t *Settings_Hotkey_Restore = nullptr;
const wchar_t *Settings_Hotkey_Restored = nullptr;
const wchar_t *Settings_Hotkey_MouseTip = nullptr;
const wchar_t *Settings_Group_Foundation = nullptr;
const wchar_t *Settings_Group_Startup = nullptr;
const wchar_t *Settings_Group_Habits = nullptr;
const wchar_t *Settings_Label_Language = nullptr;
const wchar_t *Settings_Label_SingleInstance = nullptr;
const wchar_t *Settings_Label_CheckUpdates = nullptr;
const wchar_t *Settings_Label_UpdateChannel = nullptr;
const wchar_t *Settings_Option_UpdateStable = nullptr;
const wchar_t *Settings_Option_UpdatePreRelease = nullptr;
const wchar_t *Settings_Tooltip_PreRelease = nullptr;
const wchar_t *Settings_Label_NavLoopMode = nullptr;
const wchar_t *Settings_Label_SortOrder = nullptr;
const wchar_t *Settings_Label_SortDescending = nullptr;
const wchar_t *Settings_Label_SortArchivesByNameAscending = nullptr;
const wchar_t *Settings_Label_ConfirmDel = nullptr;
const wchar_t *Settings_Label_Portable = nullptr;
const wchar_t *Settings_Tooltip_Portable = nullptr;
const wchar_t *Settings_Label_SpanDisplays = nullptr;
const wchar_t *Settings_Label_UIScale = nullptr;
const wchar_t *Settings_Status_RestartRequired = nullptr;
const wchar_t *Settings_Status_NoWritePerm = nullptr;
const wchar_t *Settings_Status_Enabled = nullptr;
const wchar_t *Settings_Header_PoweredBy = nullptr;
const wchar_t *Context_Open = nullptr;
const wchar_t *Context_OpenWith = nullptr;
const wchar_t *Context_Edit = nullptr;
const wchar_t *Context_ShowInExplorer = nullptr;
const wchar_t *Context_OpenFolder = nullptr;
const wchar_t *Context_CopyImage = nullptr;
const wchar_t *Context_CopyPath = nullptr;
const wchar_t *Context_Print = nullptr;
const wchar_t *Context_RotateCW = nullptr;
const wchar_t *Context_RotateCCW = nullptr;
const wchar_t *Context_FlipH = nullptr;
const wchar_t *Context_FlipV = nullptr;
const wchar_t *Context_Transform = nullptr;
const wchar_t *Context_ActualSize = nullptr;
const wchar_t *Context_FitToScreen = nullptr;
const wchar_t *Context_FitWindow = nullptr;
const wchar_t *Context_FillWindow = nullptr;
const wchar_t *Context_ZoomIn = nullptr;
const wchar_t *Context_ZoomOut = nullptr;
const wchar_t *Context_LockWindow = nullptr;
const wchar_t *Context_AlwaysOnTop = nullptr;
const wchar_t *Context_HUDGallery = nullptr;
const wchar_t *Context_LiteInfoPanel = nullptr;
const wchar_t *Context_FullInfoPanel = nullptr;
const wchar_t *Context_RenderRAW = nullptr;
const wchar_t *Context_PixelArtMode = nullptr;
const wchar_t *Context_ColorSpace = nullptr;
const wchar_t *Context_Fullscreen = nullptr;
const wchar_t *Context_View = nullptr;
const wchar_t *Context_WallpaperFill = nullptr;
const wchar_t *Context_WallpaperFit = nullptr;
const wchar_t *Context_WallpaperTile = nullptr;
const wchar_t *Context_SetAsWallpaper = nullptr;
const wchar_t *Context_Rename = nullptr;
const wchar_t *Context_FixExtension = nullptr;
const wchar_t *Context_Delete = nullptr;
const wchar_t *Context_SortBy = nullptr;
const wchar_t *Context_NavOrder = nullptr;
const wchar_t *Context_SortAscending = nullptr;
const wchar_t *Context_SortDescending = nullptr;
const wchar_t *Context_Settings = nullptr;
const wchar_t *Context_CompareMode = nullptr;
const wchar_t *Context_OverlayMode = nullptr;
const wchar_t *Context_GalleryOpenCompare = nullptr;
const wchar_t *Context_GalleryOpenNewWindow = nullptr;
const wchar_t *Context_Exit = nullptr;
const wchar_t *Menu_ExitPassthrough = nullptr;
const wchar_t *Dialog_PassthroughTitle = nullptr;
const wchar_t *Dialog_PassthroughContent = nullptr;
const wchar_t *OSD_PassthroughOn = nullptr;
const wchar_t *OSD_PassthroughOff = nullptr;
const wchar_t *OSD_OverlayModeOn = nullptr;
const wchar_t *OSD_OverlayModeOff = nullptr;
const wchar_t *OSD_Opacity = nullptr;
const wchar_t *Toolbar_Tooltip_OverlayZoomIn = nullptr;
const wchar_t *Toolbar_Tooltip_OverlayZoomOut = nullptr;
const wchar_t *Toolbar_Tooltip_OverlayAlphaUp = nullptr;
const wchar_t *Toolbar_Tooltip_OverlayAlphaDown = nullptr;
const wchar_t *Toolbar_Tooltip_OverlayPassthroughOn = nullptr;
const wchar_t *Toolbar_Tooltip_OverlayPassthroughOff = nullptr;
const wchar_t *Toolbar_Tooltip_OverlayExit = nullptr;
const wchar_t *Message_SaveErrorTitle = nullptr;
const wchar_t *Message_SaveErrorContent = nullptr;
const wchar_t *Toolbar_Tooltip_Prev = nullptr;
const wchar_t *Toolbar_Tooltip_Next = nullptr;
const wchar_t *Toolbar_Tooltip_RotateL = nullptr;
const wchar_t *Toolbar_Tooltip_RotateR = nullptr;
const wchar_t *Toolbar_Tooltip_FlipH = nullptr;
const wchar_t *Toolbar_Tooltip_Lock = nullptr;
const wchar_t *Toolbar_Tooltip_Unlock = nullptr;
const wchar_t *Toolbar_Tooltip_Gallery = nullptr;
const wchar_t *Toolbar_Tooltip_Info = nullptr;
const wchar_t *Toolbar_Tooltip_RawPreview = nullptr;
const wchar_t *Toolbar_Tooltip_RawFull = nullptr;
const wchar_t *Toolbar_Tooltip_RawPairView = nullptr;
const wchar_t *Toolbar_Tooltip_RawPairBack = nullptr;
const wchar_t *Toolbar_Tooltip_FixExtension = nullptr;
const wchar_t *Toolbar_Tooltip_Pin = nullptr;
const wchar_t *Toolbar_Tooltip_Unpin = nullptr;
const wchar_t *Toolbar_Tooltip_GamutWarning = nullptr;
const wchar_t *Toolbar_Tooltip_NormalMode = nullptr;
const wchar_t *Toolbar_Tooltip_CompareMode = nullptr;
const wchar_t *Toolbar_Tooltip_SinglePage = nullptr;
const wchar_t *Toolbar_Tooltip_DualPage = nullptr;
const wchar_t *Toolbar_Tooltip_CompareOpen = nullptr;
const wchar_t *Toolbar_Tooltip_CompareSwap = nullptr;
const wchar_t *Toolbar_Tooltip_CompareLayout = nullptr;
const wchar_t *Toolbar_Tooltip_CompareInfo = nullptr;
const wchar_t *Toolbar_Tooltip_CompareDelete = nullptr;
const wchar_t *Toolbar_Tooltip_CompareZoomIn = nullptr;
const wchar_t *Toolbar_Tooltip_CompareZoomOut = nullptr;
const wchar_t *Toolbar_Tooltip_CompareSyncZoomOn = nullptr;
const wchar_t *Toolbar_Tooltip_CompareSyncZoomOff = nullptr;
const wchar_t *Toolbar_Tooltip_CompareSyncPanOn = nullptr;
const wchar_t *Toolbar_Tooltip_CompareSyncPanOff = nullptr;
const wchar_t *Toolbar_Tooltip_SlideshowImmersiveToggle = nullptr;
const wchar_t *Toolbar_Tooltip_SlideshowExit = nullptr;
const wchar_t *Toolbar_Tooltip_CompareExit = nullptr;
const wchar_t *Toolbar_Tooltip_AnimPlay = nullptr;
const wchar_t *Toolbar_Tooltip_AnimPause = nullptr;
const wchar_t *Toolbar_Tooltip_AnimPrev = nullptr;
const wchar_t *Toolbar_Tooltip_AnimNext = nullptr;
const wchar_t *Toolbar_Tooltip_AnimDirtyOn = nullptr;
const wchar_t *Toolbar_Tooltip_AnimDirtyOff = nullptr;
const wchar_t *Toolbar_Tooltip_AnimSpeed = nullptr;
const wchar_t *Settings_Header_Performance = nullptr;
const wchar_t *Settings_Header_Professional = nullptr;
const wchar_t *Settings_Label_MemoryReclaim = nullptr;
const wchar_t *Settings_Option_MemSmart = nullptr;
const wchar_t *Settings_Option_MemAggressive = nullptr;
const wchar_t *Settings_Option_MemOnDemand = nullptr;
const wchar_t *Settings_Tooltip_MemoryReclaim = nullptr;
const wchar_t *Settings_Label_ShowDirtyRect = nullptr;
const wchar_t *Settings_Tooltip_ShowDirtyRect = nullptr;
const wchar_t *Settings_Label_LoupeShape = nullptr;
const wchar_t *Settings_Option_LoupeShapeSquare = nullptr;
const wchar_t *Settings_Option_LoupeShapeCircle = nullptr;
const wchar_t *Settings_Tooltip_LoupeHotkey = nullptr;
const wchar_t *OSD_Copied = nullptr;
const wchar_t *OSD_CoordinatesCopied = nullptr;
const wchar_t *OSD_FilePathCopied = nullptr;
const wchar_t *OSD_Zoom100 = nullptr;
const wchar_t *OSD_ZoomFit = nullptr;
const wchar_t *OSD_ZoomFitWindow = nullptr;
const wchar_t *OSD_ZoomFill = nullptr;
const wchar_t *OSD_PrintInstruction = nullptr;
const wchar_t *OSD_MovedToRecycleBin = nullptr;
const wchar_t *OSD_WindowLocked = nullptr;
const wchar_t *OSD_WindowUnlocked = nullptr;
const wchar_t *OSD_AlwaysOnTopOn = nullptr;
const wchar_t *OSD_AlwaysOnTopOff = nullptr;
const wchar_t *OSD_WallpaperSet = nullptr;
const wchar_t *OSD_WallpaperFailed = nullptr;
const wchar_t *OSD_Renamed = nullptr;
const wchar_t *OSD_RenameFailed = nullptr;
const wchar_t *OSD_ExtensionFixed = nullptr;
const wchar_t *OSD_Restored = nullptr;
const wchar_t *OSD_FirstImage = nullptr;
const wchar_t *OSD_LastImage = nullptr;
const wchar_t *OSD_HD = nullptr;
const wchar_t *OSD_ZoomPrefix = nullptr;
const wchar_t *OSD_AnimPlaying = nullptr;
const wchar_t *OSD_AnimPaused = nullptr;
const wchar_t *OSD_AnimDirtyOn = nullptr;
const wchar_t *OSD_AnimDirtyOff = nullptr;
const wchar_t *OSD_PrintJobStarted = nullptr;
const wchar_t *OSD_PrintJobFinished = nullptr;
const wchar_t *Context_SpanDisplays = nullptr;
const wchar_t *Settings_Tab_Visuals = nullptr;
const wchar_t *Settings_Tab_Controls = nullptr;
const wchar_t *Settings_Tab_Image = nullptr;
const wchar_t *Settings_Tab_Advanced = nullptr;
const wchar_t *Settings_Header_Backdrop = nullptr;
const wchar_t *Settings_Header_Window = nullptr;
const wchar_t *Settings_Header_WindowLock = nullptr;
const wchar_t *Settings_Header_Panel = nullptr;
const wchar_t *Settings_Header_Mouse = nullptr;
const wchar_t *Settings_Header_Edge = nullptr;
const wchar_t *Settings_Header_Render = nullptr;
const wchar_t *Settings_Header_Hdr = nullptr;
const wchar_t *Settings_Header_Prompts = nullptr;
const wchar_t *Settings_Header_System = nullptr;
const wchar_t *Settings_Header_Features = nullptr;
const wchar_t *Settings_Header_Transparency = nullptr;
const wchar_t *Settings_Header_GeekGlass = nullptr;
const wchar_t *Settings_Label_EnableGeekGlass = nullptr;
const wchar_t *Settings_Label_GlassUIAnimations = nullptr;
const wchar_t *Settings_Header_CoreMaterial = nullptr;
const wchar_t *Settings_Label_BlurSigma = nullptr;
const wchar_t *Settings_Status_GlassDisabled = nullptr;
const wchar_t *Settings_Label_TintDensity = nullptr;
const wchar_t *Settings_Tooltip_TintDensity = nullptr;
const wchar_t *Settings_Label_SpecularOpacity = nullptr;
const wchar_t *Settings_Tooltip_SpecularOpacity = nullptr;
const wchar_t *Settings_Label_ShadowIntensity = nullptr;
const wchar_t *Settings_Tooltip_ShadowIntensity = nullptr;
const wchar_t *Settings_Header_VectorAssets = nullptr;
const wchar_t *Settings_Label_VectorStrokeWeight = nullptr;
const wchar_t *Settings_Option_StrokeStandard = nullptr;
const wchar_t *Settings_Option_StrokeFine = nullptr;
const wchar_t *Settings_Header_GlassTint = nullptr;
const wchar_t *Settings_Label_TintProfile = nullptr;
const wchar_t *Settings_Option_TintAuto = nullptr;
const wchar_t *Settings_Option_TintCustom = nullptr;
const wchar_t *Settings_Label_GlassCustomColor = nullptr;
const wchar_t *Settings_Header_DensityMatrix = nullptr;
const wchar_t *Settings_Label_OsdDensity = nullptr;
const wchar_t *Settings_Tooltip_OsdDensity = nullptr;
const wchar_t *Settings_Label_PanelsDensity = nullptr;
const wchar_t *Settings_Tooltip_PanelsDensity = nullptr;
const wchar_t *Settings_Label_ModalsDensity = nullptr;
const wchar_t *Settings_Tooltip_ModalsDensity = nullptr;
const wchar_t *Settings_Label_MenusDensity = nullptr;
const wchar_t *Settings_Tooltip_MenusDensity = nullptr;
const wchar_t *Settings_Tab_Theme = nullptr;
const wchar_t *Settings_Label_ThemeMode = nullptr;
const wchar_t *Settings_Option_ThemeAuto = nullptr;
const wchar_t *Settings_Option_ThemeDark = nullptr;
const wchar_t *Settings_Option_ThemeLight = nullptr;
const wchar_t *Settings_Option_ThemeCustom = nullptr;
const wchar_t *Settings_Label_AmbientDimmer = nullptr;
const wchar_t *Settings_Tooltip_AmbientDimmer = nullptr;
const wchar_t *Settings_Label_AccentColor = nullptr;
const wchar_t *Settings_Label_TextColor = nullptr;
const wchar_t *Settings_Header_ThemeManagement = nullptr;
const wchar_t *Settings_Action_ExportTheme = nullptr;
const wchar_t *Settings_Action_ImportTheme = nullptr;
const wchar_t *Settings_Label_CanvasColor = nullptr;
const wchar_t *Settings_Label_Overlay = nullptr;
const wchar_t *Settings_Label_ShowGrid = nullptr;
const wchar_t *Settings_Label_CrossFade = nullptr;
const wchar_t *Settings_Label_AlwaysOnTop = nullptr;
const wchar_t *Settings_Label_LockWindow = nullptr;
const wchar_t *Settings_Tooltip_LockWindow = nullptr;
const wchar_t *Settings_Label_AutoHideTitle = nullptr;
const wchar_t *Settings_Label_RoundedCorners = nullptr;
const wchar_t *Settings_Label_UIBorders = nullptr;
const wchar_t *Settings_Tooltip_RoundedCorners = nullptr;
const wchar_t *Settings_Label_LockToolbar = nullptr;
const wchar_t *Settings_Label_WindowMinSize = nullptr;
const wchar_t *Settings_Label_WindowMaxSizePercent = nullptr;
const wchar_t *Settings_Label_ShowBorderIndicator = nullptr;
const wchar_t *Settings_Tooltip_ShowBorderIndicator = nullptr;
const wchar_t *Settings_Label_ShowNavigator = nullptr;
const wchar_t *Settings_Option_NavigatorAuto = nullptr;
const wchar_t *Settings_Option_NavigatorOn = nullptr;
const wchar_t *Settings_Option_NavigatorOff = nullptr;
const wchar_t *Settings_Label_KeepWindowSizeOnNav = nullptr;
const wchar_t *Settings_Label_RememberLastWindowSizeAndPosition = nullptr;
const wchar_t *Settings_Label_UpscaleSmallImagesWhenLocked = nullptr;
const wchar_t *Settings_Label_EnableSmoothScaling = nullptr;
const wchar_t *Settings_Label_ExifMode = nullptr;
const wchar_t *Settings_Label_ToolbarInfoDefault = nullptr;
const wchar_t *Settings_Label_OpenFullScreenMode = nullptr;
const wchar_t *Settings_Label_FullScreenZoomMode = nullptr;
const wchar_t *Settings_Option_FitScreen = nullptr;
const wchar_t *Settings_Option_AutoFit = nullptr;
const wchar_t *Settings_Label_InvertWheel = nullptr;
const wchar_t *Settings_Label_ZoomSnapDamping = nullptr;
const wchar_t *Settings_Label_MouseAnchorZoom = nullptr;
const wchar_t *Settings_Label_RightButtonDragZoom = nullptr;
const wchar_t *Settings_Label_WheelZoomSpeed = nullptr;
const wchar_t *Settings_Label_ThumbWheel = nullptr;
const wchar_t *Settings_Label_RightDragZoomSpeed = nullptr;
const wchar_t *OSD_WheelZoomSpeed = nullptr;
const wchar_t *Help_Action_AdjustZoomSpeed = nullptr;
const wchar_t *Help_Action_LockWindowZoom = nullptr;
const wchar_t *Settings_Label_InvertButtons = nullptr;
const wchar_t *Settings_Header_KeyboardPan = nullptr;
const wchar_t *Settings_Label_PanStepNormal = nullptr;
const wchar_t *Settings_Label_PanStepFast = nullptr;
const wchar_t *Settings_Label_UseFixedZoom = nullptr;
const wchar_t *Settings_Tooltip_UseFixedZoom = nullptr;
const wchar_t *Settings_Label_FixedZoomLevels = nullptr;
const wchar_t *Dialog_FixedZoomTitle = nullptr;
const wchar_t *Dialog_FixedZoomMsg = nullptr;
const wchar_t *Settings_Label_ZoomModeIn = nullptr;
const wchar_t *Settings_Label_ZoomModeOut = nullptr;
const wchar_t *Settings_Label_LeftDrag = nullptr;
const wchar_t *Settings_Label_MiddleDrag = nullptr;
const wchar_t *Settings_Label_MiddleClick = nullptr;
const wchar_t *Settings_Label_EdgeNavClick = nullptr;
const wchar_t *Settings_Label_DisableEdgeNavInCompare = nullptr;
const wchar_t *Settings_Label_NavIndicator = nullptr;
const wchar_t *Settings_Label_AutoRotate = nullptr;
const wchar_t *Settings_Label_CMS = nullptr;
const wchar_t *Settings_Label_AdvancedColor = nullptr;
const wchar_t *Settings_Label_HdrToneMapping = nullptr;
const wchar_t *Settings_Label_HdrSplineKnee = nullptr;
const wchar_t *Settings_Tooltip_HdrToneMapping = nullptr;
const wchar_t *Settings_Tooltip_HdrSplineKnee = nullptr;
const wchar_t *Settings_Label_HdrPeakNitsOverride = nullptr;
const wchar_t *Settings_Tooltip_HdrPeakNitsOverride = nullptr;
const wchar_t *Settings_Label_HdrPeakPercentile = nullptr;
const wchar_t *Settings_Tooltip_HdrPeakPercentile = nullptr;
const wchar_t *Settings_Option_HdrPeakPercentile_100 = nullptr;
const wchar_t *Settings_Option_HdrPeakPercentile_99995 = nullptr;
const wchar_t *Settings_Option_HdrPeakPercentile_999 = nullptr;
const wchar_t *Settings_Label_HdrDesatThreshold = nullptr;
const wchar_t *Settings_Tooltip_HdrDesatThreshold = nullptr;
const wchar_t *Settings_Label_HdrMaxDesat = nullptr;
const wchar_t *Settings_Tooltip_HdrMaxDesat = nullptr;
const wchar_t *Settings_Option_HdrColorimetric = nullptr;
const wchar_t *Settings_Option_HdrSpline = nullptr;
const wchar_t *Settings_Option_HdrLegacyReinhard = nullptr;
const wchar_t *Settings_Label_CmsFallback = nullptr;
const wchar_t *Settings_Label_CustomProof = nullptr;
const wchar_t *Context_SoftProofing = nullptr;
const wchar_t *Context_SoftProofProfile = nullptr;
const wchar_t *Context_SoftProofCustom = nullptr;
const wchar_t *Settings_Value_ComingSoon = nullptr;
const wchar_t *Settings_Label_ForceRaw = nullptr;
const wchar_t *Settings_Label_PairRawJpeg = nullptr;
const wchar_t *Settings_Tooltip_PairRawJpeg = nullptr;
const wchar_t *Settings_Label_Exposure = nullptr;
const wchar_t *Settings_Tooltip_Exposure = nullptr;
const wchar_t *Settings_Label_AddToOpenWith = nullptr;
const wchar_t *Settings_Label_CustomEditor = nullptr;
const wchar_t *Context_SelectEditor = nullptr;
const wchar_t *OSD_EditorLaunchFailed = nullptr;
const wchar_t *Settings_Action_Add = nullptr;
const wchar_t *Settings_Action_Added = nullptr;
const wchar_t *Settings_Status_DisabledInPortable = nullptr;
const wchar_t *Settings_Label_DebugHUD = nullptr;
const wchar_t *Settings_Label_Prefetch = nullptr;
const wchar_t *Settings_Label_InfoPanelAlpha = nullptr;
const wchar_t *Settings_Label_ToolbarAlpha = nullptr;
const wchar_t *Settings_Label_SettingsAlpha = nullptr;
const wchar_t *Settings_Label_Reset = nullptr;
const wchar_t *Settings_Action_Restore = nullptr;
const wchar_t *Settings_Action_Done = nullptr;
const wchar_t *Settings_Option_CmsUnmanaged = nullptr;
const wchar_t *Settings_Option_CmssRGB = nullptr;
const wchar_t *Settings_Option_CmsP3 = nullptr;
const wchar_t *Settings_Option_CmsAdobeRGB = nullptr;
const wchar_t *Settings_Option_CmsGray = nullptr;
const wchar_t *Settings_Option_CmsProPhoto = nullptr;
const wchar_t *Settings_Label_CmsIntent = nullptr;
const wchar_t *Settings_Label_GamutWarning = nullptr;
const wchar_t *Settings_Tooltip_GamutWarning = nullptr;
const wchar_t *Settings_Label_GamutAutoPrompt = nullptr;
const wchar_t *Settings_Tooltip_GamutAutoPrompt = nullptr;
const wchar_t *Settings_Label_GamutColor = nullptr;
const wchar_t *Settings_Option_CmsIntentRelative = nullptr;
const wchar_t *Settings_Option_CmsIntentPerceptual = nullptr;
const wchar_t *Settings_Tooltip_CMS = nullptr;
const wchar_t *Settings_Tooltip_CmsIntent = nullptr;
const wchar_t *Settings_Tooltip_AdvancedColor = nullptr;
const wchar_t *Settings_Tooltip_ZoomAuto = nullptr;
const wchar_t *Settings_Action_CheckUpdates = nullptr;
const wchar_t *Settings_Action_ViewUpdate = nullptr;
const wchar_t *Settings_Status_Checking = nullptr;
const wchar_t *Settings_Status_UpToDate = nullptr;
const wchar_t *Settings_Link_GitHub = nullptr;
const wchar_t *Settings_Link_ReportIssue = nullptr;
const wchar_t *Settings_Link_Hotkeys = nullptr;
const wchar_t *Settings_Label_Version = nullptr;
const wchar_t *Settings_Label_Build = nullptr;
const wchar_t *Settings_Option_Black = nullptr;
const wchar_t *Settings_Option_White = nullptr;
const wchar_t *Settings_Option_Grid = nullptr;
const wchar_t *Settings_Option_Custom = nullptr;
const wchar_t *Settings_Option_Off = nullptr;
const wchar_t *Settings_Option_On = nullptr;
const wchar_t *Settings_Option_Lite = nullptr;
const wchar_t *Settings_Option_Full = nullptr;
const wchar_t *Settings_Option_LargeOnly = nullptr;
const wchar_t *Settings_Option_All = nullptr;
const wchar_t *Settings_Option_SoftProofing = nullptr;
const wchar_t *Settings_Option_Window = nullptr;
const wchar_t *Settings_Option_Pan = nullptr;
const wchar_t *Settings_Option_None = nullptr;
const wchar_t *Settings_Option_Exit = nullptr;
const wchar_t *Settings_Option_Arrow = nullptr;
const wchar_t *Settings_Option_Cursor = nullptr;
const wchar_t *Settings_Option_Manual = nullptr;
const wchar_t *Settings_Option_SortAuto = nullptr;
const wchar_t *Settings_Option_SortName = nullptr;
const wchar_t *Settings_Option_SortModified = nullptr;
const wchar_t *Settings_Option_SortDateTaken = nullptr;
const wchar_t *Settings_Option_SortSize = nullptr;
const wchar_t *Settings_Option_SortType = nullptr;
const wchar_t *Settings_Option_NavLoop = nullptr;
const wchar_t *Settings_Option_NavThrough = nullptr;
const wchar_t *Settings_Option_Linear = nullptr;
const wchar_t *Settings_Option_Nearest = nullptr;
const wchar_t *Settings_Option_HighQualityCubic = nullptr;
const wchar_t *Settings_Option_ZoomAuto = nullptr;
const wchar_t *Settings_Option_Auto = nullptr;
const wchar_t *Settings_Option_Eco = nullptr;
const wchar_t *Settings_Option_Balanced = nullptr;
const wchar_t *Settings_Option_Ultra = nullptr;
const wchar_t *Help_Header_Shortcuts = nullptr;
const wchar_t *Help_Header_Mouse = nullptr;
const wchar_t *Help_Item_NextPrev = nullptr;
const wchar_t *Help_Item_Zoom = nullptr;
const wchar_t *Help_Item_Pan = nullptr;
const wchar_t *Help_Item_Rotate = nullptr;
const wchar_t *Help_Item_Fit = nullptr;
const wchar_t *Help_Item_Delete = nullptr;
const wchar_t *Help_Item_Fullscreen = nullptr;
const wchar_t *Help_Item_Close = nullptr;
const wchar_t *Help_Mouse_Left = nullptr;
const wchar_t *Help_Mouse_Middle = nullptr;
const wchar_t *Help_Mouse_Wheel = nullptr;
const wchar_t *Help_Mouse_Right = nullptr;
const wchar_t *Help_Mouse_RightVerticalDrag = nullptr;
const wchar_t *Help_Action_MoveWindow = nullptr;
const wchar_t *Help_Action_PanImage = nullptr;
const wchar_t *Help_Action_ContextMenu = nullptr;
const wchar_t *Help_Action_NextPrev = nullptr;
const wchar_t *Help_Action_Zoom = nullptr;
const wchar_t *Help_Action_SmartZoom = nullptr;
const wchar_t *Help_Desc_Copy = nullptr;
const wchar_t *Help_Desc_Edit = nullptr;
const wchar_t *Help_Header_Tips = nullptr;
const wchar_t *Help_Tip_ContextScope = nullptr;
const wchar_t *Help_Tip_Rotation = nullptr;
const wchar_t *Help_Tip_VideoWall = nullptr;
const wchar_t *Help_Tip_DesignerMode = nullptr;
const wchar_t *Help_Tip_GamutDetection = nullptr;
const wchar_t *Help_Tip_Raw = nullptr;
const wchar_t *Help_Tip_JpegQ = nullptr;
const wchar_t *Help_Tip_SoftProofCompare = nullptr;
const wchar_t *Dialog_UpdateTitle = nullptr;
const wchar_t *Dialog_UpdateContent = nullptr;
const wchar_t *Dialog_UpdateLogHeader = nullptr;
const wchar_t *Dialog_ButtonUpdate = nullptr;
const wchar_t *Dialog_ButtonLater = nullptr;
const wchar_t *Dialog_ButtonStar = nullptr;
const wchar_t *Dialog_Update_LoveTitle = nullptr;
const wchar_t *Dialog_Update_LoveMessage = nullptr;
const wchar_t *Help_Item_Compare = nullptr;
const wchar_t *Help_Item_FirstLast = nullptr;
const wchar_t *HUD_Group_Physical = nullptr;
const wchar_t *HUD_Group_Scientific = nullptr;
const wchar_t *HUD_Group_Encoding = nullptr;
const wchar_t *HUD_Tip_Sharp_Desc = nullptr;
const wchar_t *HUD_Tip_Sharp_High = nullptr;
const wchar_t *HUD_Tip_Sharp_Low = nullptr;
const wchar_t *HUD_Tip_Sharp_Ref = nullptr;
const wchar_t *HUD_Tip_Ent_Desc = nullptr;
const wchar_t *HUD_Tip_Ent_High = nullptr;
const wchar_t *HUD_Tip_Ent_Low = nullptr;
const wchar_t *HUD_Tip_Ent_Ref = nullptr;
const wchar_t *HUD_Tip_BPP_Desc = nullptr;
const wchar_t *HUD_Tip_BPP_High = nullptr;
const wchar_t *HUD_Tip_BPP_Low = nullptr;
const wchar_t *HUD_Tip_BPP_Ref = nullptr;
const wchar_t *HUD_Label_High = nullptr;
const wchar_t *HUD_Label_Low = nullptr;
const wchar_t *HUD_Label_Ref = nullptr;
const wchar_t *Settings_Header_GalleryTrigger = nullptr;
const wchar_t *Settings_Label_GalleryTriggerMode = nullptr;
const wchar_t *Settings_Option_GalleryTriggerAuto = nullptr;
const wchar_t *Settings_Option_GalleryTriggerDelay = nullptr;
const wchar_t *Settings_Option_GalleryTriggerClick = nullptr;
const wchar_t *Settings_Option_GalleryTriggerDisable = nullptr;
const wchar_t *Settings_Tooltip_GalleryTrigger = nullptr;

const wchar_t *OSD_SlideshowStarted = nullptr;
const wchar_t *OSD_SlideshowStopped = nullptr;
const wchar_t *OSD_SlideshowResumed = nullptr;
const wchar_t *OSD_SlideshowPaused = nullptr;
const wchar_t *OSD_ImmersiveSpotlight = nullptr;
const wchar_t *OSD_ImmersiveNormal = nullptr;
const wchar_t *Context_SlideshowMode = nullptr;
const wchar_t *Settings_Label_SlideshowInterval = nullptr;
const wchar_t *Settings_Label_SlideshowImmersive = nullptr;
const wchar_t *Settings_Label_CustomLiteInfoPanel = nullptr;
const wchar_t *Settings_Label_ItemsInNormalMode = nullptr;
const wchar_t *Settings_Label_ItemsInCompareMode = nullptr;
const wchar_t *Settings_Label_SeparatorPreset = nullptr;
const wchar_t *Settings_Option_SlideshowNormal = nullptr;
const wchar_t *Settings_Option_SlideshowSpotlight = nullptr;

// --- Static Constants ---
const wchar_t *Settings_Text_Copyright = L"Copyright \u00A9 2025-%s Vivor Loong (Github@justnullname)";
const wchar_t *Settings_Text_License = L"Licensed under GNU GPL v3.0";

// ----------------------------------------------------------------
// Language Table Structure
// ----------------------------------------------------------------
struct LanguageTable {
    const wchar_t *OSD_NoImage;
    const wchar_t *OSD_Lossless;
    const wchar_t *OSD_ReencodedLossless;
    const wchar_t *OSD_EdgeAdapted;
    const wchar_t *OSD_Reencoded;
    const wchar_t *OSD_ReadOnly;
    const wchar_t *OSD_NotPerfect;
    const wchar_t *OSD_SpanOn;
    const wchar_t *OSD_SpanOff;
    const wchar_t *OSD_CompareBefore;
    const wchar_t *OSD_CompareAfter;
    const wchar_t *OSD_UndoDeleteSuccess;
    const wchar_t *OSD_UndoDeleteFailed;
    const wchar_t *OSD_UndoRenameSuccess;
    const wchar_t *OSD_UndoRenameFailed;
    const wchar_t *OSD_UndoTransformSuccess;
    const wchar_t *OSD_UndoTransformFailed;
    const wchar_t *OSD_GamutDetected;
    const wchar_t *OSD_GamutIncompatible;
    const wchar_t *OSD_GamutFailed;
    const wchar_t *Action_RotateCW;
    const wchar_t *Action_RotateCCW;
    const wchar_t *Action_Rotate180;
    const wchar_t *Action_FlipH;
    const wchar_t *Action_FlipV;
    const wchar_t *Dialog_SaveTitle;
    const wchar_t *Dialog_SaveContent;
    const wchar_t *Dialog_ButtonSave;
    const wchar_t *Dialog_ButtonSaveAs;
    const wchar_t *Dialog_ButtonDiscard;
    const wchar_t *Dialog_ButtonContinue;
    const wchar_t *Checkbox_AlwaysSaveLossless;
    const wchar_t *Checkbox_AlwaysSaveEdgeAdapted;
    const wchar_t *Checkbox_AlwaysSaveLossy;
    const wchar_t *Checkbox_NeverConfirmDelete;
    const wchar_t *OSD_HEICCodecMissing;
    const wchar_t *Dialog_HEICTitle;
    const wchar_t *Dialog_HEICContent;
    const wchar_t *Dialog_HEICGetExtension;
    const wchar_t *Dialog_Cancel;
    const wchar_t *Settings_Tab_General;
    const wchar_t *Settings_Tab_About;
    const wchar_t *Settings_Group_Foundation;
    const wchar_t *Settings_Group_Startup;
    const wchar_t *Settings_Group_Habits;
    const wchar_t *Settings_Label_Language;
    const wchar_t *Settings_Label_SingleInstance;
    const wchar_t *Settings_Label_CheckUpdates;
    const wchar_t *Settings_Label_UpdateChannel;
    const wchar_t *Settings_Option_UpdateStable;
    const wchar_t *Settings_Option_UpdatePreRelease;
    const wchar_t *Settings_Tooltip_PreRelease;
    const wchar_t *Settings_Label_NavLoopMode;
    const wchar_t *Settings_Label_SortOrder;
    const wchar_t *Settings_Label_SortDescending;
    const wchar_t *Settings_Label_SortArchivesByNameAscending;
    const wchar_t *Settings_Label_ConfirmDel;
    const wchar_t *Settings_Label_Portable;
    const wchar_t *Settings_Tooltip_Portable;
    const wchar_t *Settings_Label_SpanDisplays;
    const wchar_t *Settings_Label_UIScale;
    const wchar_t *Settings_Status_RestartRequired;
    const wchar_t *Settings_Status_NoWritePerm;
    const wchar_t *Settings_Status_Enabled;
    const wchar_t *Settings_Header_PoweredBy;
    const wchar_t *Context_Open;
    const wchar_t *Context_OpenWith;
    const wchar_t *Context_Edit;
    const wchar_t *Context_ShowInExplorer;
    const wchar_t *Context_OpenFolder;
    const wchar_t *Context_CopyImage;
    const wchar_t *Context_CopyPath;
    const wchar_t *Context_Print;
    const wchar_t *Context_RotateCW;
    const wchar_t *Context_RotateCCW;
    const wchar_t *Context_FlipH;
    const wchar_t *Context_FlipV;
    const wchar_t *Context_Transform;
    const wchar_t *Context_ActualSize;
    const wchar_t *Context_FitToScreen;
    const wchar_t *Context_FitWindow;
    const wchar_t *Context_FillWindow;
    const wchar_t *Context_ZoomIn;
    const wchar_t *Context_ZoomOut;
    const wchar_t *Context_LockWindow;
    const wchar_t *Context_AlwaysOnTop;
    const wchar_t *Context_HUDGallery;
    const wchar_t *Context_LiteInfoPanel;
    const wchar_t *Context_FullInfoPanel;
    const wchar_t *Context_RenderRAW;
    const wchar_t *Context_PixelArtMode;
    const wchar_t *Context_ColorSpace;
    const wchar_t *Context_Fullscreen;
    const wchar_t *Context_View;
    const wchar_t *Context_WallpaperFill;
    const wchar_t *Context_WallpaperFit;
    const wchar_t *Context_WallpaperTile;
    const wchar_t *Context_SetAsWallpaper;
    const wchar_t *Context_Rename;
    const wchar_t *Context_FixExtension;
    const wchar_t *Context_Delete;
    const wchar_t *Context_UndoDelete;
    const wchar_t *Context_UndoRename;
    const wchar_t *Context_UndoTransform;
    const wchar_t *Context_Undo;
    const wchar_t *Context_SortBy;
    const wchar_t *Context_NavOrder;
    const wchar_t *Context_SortAscending;
    const wchar_t *Context_SortDescending;
    const wchar_t *Context_Settings;
    const wchar_t *Context_CompareMode;
    const wchar_t *Context_OverlayMode;
    const wchar_t *Context_GalleryOpenCompare;
    const wchar_t *Context_GalleryOpenNewWindow;
    const wchar_t *Context_Exit;
    const wchar_t *Menu_ExitPassthrough;
    const wchar_t *Dialog_PassthroughTitle;
    const wchar_t *Dialog_PassthroughContent;
    const wchar_t *OSD_PassthroughOn;
    const wchar_t *OSD_PassthroughOff;
    const wchar_t *OSD_OverlayModeOn;
    const wchar_t *OSD_OverlayModeOff;
    const wchar_t *OSD_Opacity;
    const wchar_t *Toolbar_Tooltip_OverlayZoomIn;
    const wchar_t *Toolbar_Tooltip_OverlayZoomOut;
    const wchar_t *Toolbar_Tooltip_OverlayAlphaUp;
    const wchar_t *Toolbar_Tooltip_OverlayAlphaDown;
    const wchar_t *Toolbar_Tooltip_OverlayPassthroughOn;
    const wchar_t *Toolbar_Tooltip_OverlayPassthroughOff;
    const wchar_t *Toolbar_Tooltip_OverlayExit;
    const wchar_t *Message_SaveErrorTitle;
    const wchar_t *Message_SaveErrorContent;
    const wchar_t *Toolbar_Tooltip_Prev;
    const wchar_t *Toolbar_Tooltip_Next;
    const wchar_t *Toolbar_Tooltip_RotateL;
    const wchar_t *Toolbar_Tooltip_RotateR;
    const wchar_t *Toolbar_Tooltip_FlipH;
    const wchar_t *Toolbar_Tooltip_Lock;
    const wchar_t *Toolbar_Tooltip_Unlock;
    const wchar_t *Toolbar_Tooltip_Gallery;
    const wchar_t *Toolbar_Tooltip_Info;
    const wchar_t *Toolbar_Tooltip_RawPreview;
    const wchar_t *Toolbar_Tooltip_RawFull;
    const wchar_t *Toolbar_Tooltip_RawPairView;
    const wchar_t *Toolbar_Tooltip_RawPairBack;
    const wchar_t *Toolbar_Tooltip_FixExtension;
    const wchar_t *Toolbar_Tooltip_Pin;
    const wchar_t *Toolbar_Tooltip_Unpin;
    const wchar_t *Toolbar_Tooltip_GamutWarning;
    const wchar_t *Toolbar_Tooltip_NormalMode;
    const wchar_t *Toolbar_Tooltip_CompareMode;
    const wchar_t *Toolbar_Tooltip_SinglePage;
    const wchar_t *Toolbar_Tooltip_DualPage;
    const wchar_t *Toolbar_Tooltip_CompareOpen;
    const wchar_t *Toolbar_Tooltip_CompareSwap;
    const wchar_t *Toolbar_Tooltip_CompareLayout;
    const wchar_t *Toolbar_Tooltip_CompareInfo;
    const wchar_t *Toolbar_Tooltip_CompareDelete;
    const wchar_t *Toolbar_Tooltip_CompareZoomIn;
    const wchar_t *Toolbar_Tooltip_CompareZoomOut;
    const wchar_t *Toolbar_Tooltip_CompareSyncZoomOn;
    const wchar_t *Toolbar_Tooltip_CompareSyncZoomOff;
    const wchar_t *Toolbar_Tooltip_CompareSyncPanOn;
    const wchar_t *Toolbar_Tooltip_CompareSyncPanOff;
    const wchar_t *Toolbar_Tooltip_SlideshowImmersiveToggle;
    const wchar_t *Toolbar_Tooltip_SlideshowExit;
    const wchar_t *Toolbar_Tooltip_CompareExit;
    const wchar_t *Toolbar_Tooltip_AnimPlay;
    const wchar_t *Toolbar_Tooltip_AnimPause;
    const wchar_t *Toolbar_Tooltip_AnimPrev;
    const wchar_t *Toolbar_Tooltip_AnimNext;
    const wchar_t *Toolbar_Tooltip_AnimDirtyOn;
    const wchar_t *Toolbar_Tooltip_AnimDirtyOff;
    const wchar_t *Toolbar_Tooltip_AnimSpeed;
    const wchar_t *Settings_Header_Performance;
    const wchar_t *Settings_Header_Professional;
    const wchar_t *Settings_Label_MemoryReclaim;
    const wchar_t *Settings_Option_MemSmart;
    const wchar_t *Settings_Option_MemAggressive;
    const wchar_t *Settings_Option_MemOnDemand;
    const wchar_t *Settings_Tooltip_MemoryReclaim;
    const wchar_t *Settings_Label_ShowDirtyRect;
    const wchar_t *Settings_Tooltip_ShowDirtyRect;
    const wchar_t *OSD_Copied;
    const wchar_t *OSD_CoordinatesCopied;
    const wchar_t *OSD_FilePathCopied;
    const wchar_t *OSD_Zoom100;
    const wchar_t *OSD_ZoomFit;
    const wchar_t *OSD_ZoomFitWindow;
    const wchar_t *OSD_ZoomFill;
    const wchar_t *OSD_PrintInstruction;
    const wchar_t *OSD_MovedToRecycleBin;
    const wchar_t *OSD_WindowLocked;
    const wchar_t *OSD_WindowUnlocked;
    const wchar_t *OSD_AlwaysOnTopOn;
    const wchar_t *OSD_AlwaysOnTopOff;
    const wchar_t *OSD_WallpaperSet;
    const wchar_t *OSD_WallpaperFailed;
    const wchar_t *OSD_Renamed;
    const wchar_t *OSD_RenameFailed;
    const wchar_t *OSD_ExtensionFixed;
    const wchar_t *OSD_Restored;
    const wchar_t *OSD_FirstImage;
    const wchar_t *OSD_LastImage;
    const wchar_t *OSD_HD;
    const wchar_t *OSD_ZoomPrefix;
    const wchar_t *OSD_AnimPlaying;
    const wchar_t *OSD_AnimPaused;
    const wchar_t *OSD_AnimDirtyOn;
    const wchar_t *OSD_AnimDirtyOff;
    const wchar_t *Context_SpanDisplays;
    const wchar_t *Settings_Tab_Visuals;
    const wchar_t *Settings_Tab_Controls;
    const wchar_t *Settings_Tab_Image;
    const wchar_t *Settings_Tab_Advanced;
    const wchar_t *Settings_Header_Backdrop;
    const wchar_t *Settings_Header_Window;
    const wchar_t *Settings_Header_WindowLock;
    const wchar_t *Settings_Header_Panel;
    const wchar_t *Settings_Header_Mouse;
    const wchar_t *Settings_Header_Edge;
    const wchar_t *Settings_Header_Render;
    const wchar_t *Settings_Header_Hdr;
    const wchar_t *Settings_Header_Prompts;
    const wchar_t *Settings_Header_System;
    const wchar_t *Settings_Header_Features;
    const wchar_t *Settings_Header_Transparency;
    const wchar_t *Settings_Header_GeekGlass;
    const wchar_t *Settings_Label_EnableGeekGlass;
    const wchar_t *Settings_Label_GlassUIAnimations;
    const wchar_t *Settings_Header_CoreMaterial;
    const wchar_t *Settings_Label_BlurSigma;
    const wchar_t *Settings_Status_GlassDisabled;
    const wchar_t *Settings_Label_TintDensity;
    const wchar_t *Settings_Tooltip_TintDensity;
    const wchar_t *Settings_Label_SpecularOpacity;
    const wchar_t *Settings_Tooltip_SpecularOpacity;
    const wchar_t *Settings_Label_ShadowIntensity;
    const wchar_t *Settings_Tooltip_ShadowIntensity;
    const wchar_t *Settings_Header_VectorAssets;
    const wchar_t *Settings_Label_VectorStrokeWeight;
    const wchar_t *Settings_Option_StrokeStandard;
    const wchar_t *Settings_Option_StrokeFine;
    const wchar_t *Settings_Header_GlassTint;
    const wchar_t *Settings_Label_TintProfile;
    const wchar_t *Settings_Option_TintAuto;
    const wchar_t *Settings_Option_TintCustom;
    const wchar_t *Settings_Label_GlassCustomColor;
    const wchar_t *Settings_Header_DensityMatrix;
    const wchar_t *Settings_Label_OsdDensity;
    const wchar_t *Settings_Tooltip_OsdDensity;
    const wchar_t *Settings_Label_PanelsDensity;
    const wchar_t *Settings_Tooltip_PanelsDensity;
    const wchar_t *Settings_Label_ModalsDensity;
    const wchar_t *Settings_Tooltip_ModalsDensity;
    const wchar_t *Settings_Label_MenusDensity;
    const wchar_t *Settings_Tooltip_MenusDensity;
    const wchar_t *Settings_Tab_Theme;
    const wchar_t *Settings_Label_ThemeMode;
    const wchar_t *Settings_Option_ThemeAuto;
    const wchar_t *Settings_Option_ThemeDark;
    const wchar_t *Settings_Option_ThemeLight;
    const wchar_t *Settings_Option_ThemeCustom;
    const wchar_t *Settings_Label_AmbientDimmer;
    const wchar_t *Settings_Tooltip_AmbientDimmer;
    const wchar_t *Settings_Label_AccentColor;
    const wchar_t *Settings_Label_TextColor;
    const wchar_t *Settings_Header_ThemeManagement;
    const wchar_t *Settings_Action_ExportTheme;
    const wchar_t *Settings_Action_ImportTheme;
    const wchar_t *Settings_Label_CanvasColor;
    const wchar_t *Settings_Label_Overlay;
    const wchar_t *Settings_Label_ShowGrid;
    const wchar_t *Settings_Label_CrossFade;
    const wchar_t *Settings_Label_AlwaysOnTop;
    const wchar_t *Settings_Label_LockWindow;
    const wchar_t *Settings_Tooltip_LockWindow;
    const wchar_t *Settings_Label_AutoHideTitle;
    const wchar_t *Settings_Label_RoundedCorners;
    const wchar_t *Settings_Label_UIBorders;
    const wchar_t *Settings_Tooltip_RoundedCorners;
    const wchar_t *Settings_Label_LockToolbar;
    const wchar_t *Settings_Label_WindowMinSize;
    const wchar_t *Settings_Label_WindowMaxSizePercent;
    const wchar_t *Settings_Label_ShowBorderIndicator;
    const wchar_t *Settings_Tooltip_ShowBorderIndicator;
    const wchar_t *Settings_Label_ShowNavigator;
    const wchar_t *Settings_Option_NavigatorAuto;
    const wchar_t *Settings_Option_NavigatorOn;
    const wchar_t *Settings_Option_NavigatorOff;
    const wchar_t *Settings_Label_KeepWindowSizeOnNav;
    const wchar_t *Settings_Label_RememberLastWindowSizeAndPosition;
    const wchar_t *Settings_Label_UpscaleSmallImagesWhenLocked;
    const wchar_t *Settings_Label_EnableSmoothScaling;
    const wchar_t *Settings_Label_ExifMode;
    const wchar_t *Settings_Label_ToolbarInfoDefault;
    const wchar_t *Settings_Label_OpenFullScreenMode;
    const wchar_t *Settings_Label_FullScreenZoomMode;
    const wchar_t *Settings_Option_FitScreen;
    const wchar_t *Settings_Option_AutoFit;
    const wchar_t *Settings_Label_InvertWheel;
    const wchar_t *Settings_Label_ZoomSnapDamping;
    const wchar_t *Settings_Label_MouseAnchorZoom;
    const wchar_t *Settings_Label_RightButtonDragZoom;
    const wchar_t *Settings_Label_WheelZoomSpeed;
    const wchar_t *Settings_Label_ThumbWheel;
    const wchar_t *Settings_Label_RightDragZoomSpeed;
    const wchar_t *OSD_WheelZoomSpeed;
    const wchar_t *Help_Action_AdjustZoomSpeed;
    const wchar_t *Help_Action_LockWindowZoom;
    const wchar_t *Settings_Label_InvertButtons;
    const wchar_t *Settings_Label_UseFixedZoom;
    const wchar_t *Settings_Tooltip_UseFixedZoom;
    const wchar_t *Settings_Label_FixedZoomLevels;
    const wchar_t *Dialog_FixedZoomTitle;
    const wchar_t *Dialog_FixedZoomMsg;
    const wchar_t *Settings_Label_ZoomModeIn;
    const wchar_t *Settings_Label_ZoomModeOut;
    const wchar_t *Settings_Label_LeftDrag;
    const wchar_t *Settings_Label_MiddleDrag;
    const wchar_t *Settings_Label_MiddleClick;
    const wchar_t *Settings_Label_EdgeNavClick;
    const wchar_t *Settings_Label_DisableEdgeNavInCompare;
    const wchar_t *Settings_Label_NavIndicator;
    const wchar_t *Settings_Label_AutoRotate;
    const wchar_t *Settings_Label_CMS;
    const wchar_t *Settings_Label_AdvancedColor;
    const wchar_t *Settings_Label_HdrToneMapping;
    const wchar_t *Settings_Label_HdrSplineKnee;
    const wchar_t *Settings_Tooltip_HdrToneMapping;
    const wchar_t *Settings_Tooltip_HdrSplineKnee;
    const wchar_t *Settings_Label_HdrPeakNitsOverride;
    const wchar_t *Settings_Tooltip_HdrPeakNitsOverride;
    const wchar_t *Settings_Label_HdrPeakPercentile;
    const wchar_t *Settings_Tooltip_HdrPeakPercentile;
    const wchar_t *Settings_Option_HdrPeakPercentile_100;
    const wchar_t *Settings_Option_HdrPeakPercentile_99995;
    const wchar_t *Settings_Option_HdrPeakPercentile_999;
    const wchar_t *Settings_Label_HdrDesatThreshold;
    const wchar_t *Settings_Tooltip_HdrDesatThreshold;
    const wchar_t *Settings_Label_HdrMaxDesat;
    const wchar_t *Settings_Tooltip_HdrMaxDesat;
    const wchar_t *Settings_Option_HdrColorimetric;
    const wchar_t *Settings_Option_HdrSpline;
    const wchar_t *Settings_Option_HdrLegacyReinhard;
    const wchar_t *Settings_Label_CmsFallback;
    const wchar_t *Settings_Label_CustomProof;
    const wchar_t *Context_SoftProofing;
    const wchar_t *Context_SoftProofProfile;
    const wchar_t *Context_SoftProofCustom;
    const wchar_t *Settings_Value_ComingSoon;
    const wchar_t *Settings_Label_ForceRaw;
    const wchar_t *Settings_Label_PairRawJpeg;
    const wchar_t *Settings_Tooltip_PairRawJpeg;
    const wchar_t *Settings_Label_Exposure;
    const wchar_t *Settings_Tooltip_Exposure;
    const wchar_t *Settings_Label_AddToOpenWith;
    const wchar_t *Settings_Label_CustomEditor;
    const wchar_t *Context_SelectEditor;
    const wchar_t *OSD_EditorLaunchFailed;
    const wchar_t *Settings_Action_Add;
    const wchar_t *Settings_Action_Added;
    const wchar_t *Settings_Status_DisabledInPortable;
    const wchar_t *Settings_Label_DebugHUD;
    const wchar_t *Settings_Label_Prefetch;
    const wchar_t *Settings_Label_InfoPanelAlpha;
    const wchar_t *Settings_Label_ToolbarAlpha;
    const wchar_t *Settings_Label_SettingsAlpha;
    const wchar_t *Settings_Label_Reset;
    const wchar_t *Settings_Action_Restore;
    const wchar_t *Settings_Action_Done;
    const wchar_t *Settings_Option_CmsUnmanaged;
    const wchar_t *Settings_Option_CmssRGB;
    const wchar_t *Settings_Option_CmsP3;
    const wchar_t *Settings_Option_CmsAdobeRGB;
    const wchar_t *Settings_Option_CmsGray;
    const wchar_t *Settings_Option_CmsProPhoto;
    const wchar_t *Settings_Label_CmsIntent;
    const wchar_t *Settings_Label_GamutWarning;
    const wchar_t *Settings_Tooltip_GamutWarning;
    const wchar_t *Settings_Label_GamutAutoPrompt;
    const wchar_t *Settings_Tooltip_GamutAutoPrompt;
    const wchar_t *Settings_Label_GamutColor;
    const wchar_t *Settings_Option_CmsIntentRelative;
    const wchar_t *Settings_Option_CmsIntentPerceptual;
    const wchar_t *Settings_Tooltip_CMS;
    const wchar_t *Settings_Tooltip_CmsIntent;
    const wchar_t *Settings_Tooltip_AdvancedColor;
    const wchar_t *Settings_Tooltip_ZoomAuto;
    const wchar_t *Settings_Action_CheckUpdates;
    const wchar_t *Settings_Action_ViewUpdate;
    const wchar_t *Settings_Status_Checking;
    const wchar_t *Settings_Status_UpToDate;
    const wchar_t *Settings_Link_GitHub;
    const wchar_t *Settings_Link_ReportIssue;
    const wchar_t *Settings_Link_Hotkeys;
    const wchar_t *Settings_Label_Version;
    const wchar_t *Settings_Label_Build;
    const wchar_t *Settings_Option_Black;
    const wchar_t *Settings_Option_White;
    const wchar_t *Settings_Option_Grid;
    const wchar_t *Settings_Option_Custom;
    const wchar_t *Settings_Option_Off;
    const wchar_t *Settings_Option_On;
    const wchar_t *Settings_Option_Lite;
    const wchar_t *Settings_Option_Full;
    const wchar_t *Settings_Option_LargeOnly;
    const wchar_t *Settings_Option_All;
    const wchar_t *Settings_Option_SoftProofing;
    const wchar_t *Settings_Option_Window;
    const wchar_t *Settings_Option_Pan;
    const wchar_t *Settings_Option_None;
    const wchar_t *Settings_Option_Exit;
    const wchar_t *Settings_Option_Arrow;
    const wchar_t *Settings_Option_Cursor;
    const wchar_t *Settings_Option_Manual;
    const wchar_t *Settings_Option_SortAuto;
    const wchar_t *Settings_Option_SortName;
    const wchar_t *Settings_Option_SortModified;
    const wchar_t *Settings_Option_SortDateTaken;
    const wchar_t *Settings_Option_SortSize;
    const wchar_t *Settings_Option_SortType;
    const wchar_t *Settings_Option_NavLoop;
    const wchar_t *Settings_Option_NavThrough;
    const wchar_t *Settings_Option_Linear;
    const wchar_t *Settings_Option_Nearest;
    const wchar_t *Settings_Option_HighQualityCubic;
    const wchar_t *Settings_Option_ZoomAuto;
    const wchar_t *Settings_Option_Auto;
    const wchar_t *Settings_Option_Eco;
    const wchar_t *Settings_Option_Balanced;
    const wchar_t *Settings_Option_Ultra;
    const wchar_t *Help_Header_Shortcuts;
    const wchar_t *Help_Header_Mouse;
    const wchar_t *Help_Item_NextPrev;
    const wchar_t *Help_Item_Zoom;
    const wchar_t *Help_Item_Pan;
    const wchar_t *Help_Item_Rotate;
    const wchar_t *Help_Item_Fit;
    const wchar_t *Help_Item_Delete;
    const wchar_t *Help_Item_Fullscreen;
    const wchar_t *Help_Item_Close;
    const wchar_t *Help_Mouse_Left;
    const wchar_t *Help_Mouse_Middle;
    const wchar_t *Help_Mouse_Wheel;
    const wchar_t *Help_Mouse_Right;
    const wchar_t *Help_Mouse_RightVerticalDrag;
    const wchar_t *Help_Action_MoveWindow;
    const wchar_t *Help_Action_PanImage;
    const wchar_t *Help_Action_ContextMenu;
    const wchar_t *Help_Action_NextPrev;
    const wchar_t *Help_Action_Zoom;
    const wchar_t *Help_Action_SmartZoom;
    const wchar_t *Help_Desc_Copy;
    const wchar_t *Help_Desc_Edit;
    const wchar_t *Help_Header_Tips;
    const wchar_t *Help_Tip_ContextScope;
    const wchar_t *Help_Tip_Rotation;
    const wchar_t *Help_Tip_VideoWall;
    const wchar_t *Help_Tip_DesignerMode;
    const wchar_t *Help_Tip_GamutDetection;
    const wchar_t *Help_Tip_Raw;
    const wchar_t *Help_Tip_JpegQ;
    const wchar_t *Help_Tip_SoftProofCompare;
    const wchar_t *Dialog_UpdateTitle;
    const wchar_t *Dialog_UpdateContent;
    const wchar_t *Dialog_UpdateLogHeader;
    const wchar_t *Dialog_ButtonUpdate;
    const wchar_t *Dialog_ButtonLater;
    const wchar_t *Dialog_ButtonStar;
    const wchar_t *Dialog_Update_LoveTitle;
    const wchar_t *Dialog_Update_LoveMessage;
    const wchar_t *Help_Item_Compare;
    const wchar_t *Help_Item_FirstLast;
    const wchar_t *HUD_Group_Physical;
    const wchar_t *HUD_Group_Scientific;
    const wchar_t *HUD_Group_Encoding;
    const wchar_t *HUD_Tip_Sharp_Desc;
    const wchar_t *HUD_Tip_Sharp_High;
    const wchar_t *HUD_Tip_Sharp_Low;
    const wchar_t *HUD_Tip_Sharp_Ref;
    const wchar_t *HUD_Tip_Ent_Desc;
    const wchar_t *HUD_Tip_Ent_High;
    const wchar_t *HUD_Tip_Ent_Low;
    const wchar_t *HUD_Tip_Ent_Ref;
    const wchar_t *HUD_Tip_BPP_Desc;
    const wchar_t *HUD_Tip_BPP_High;
    const wchar_t *HUD_Tip_BPP_Low;
    const wchar_t *HUD_Tip_BPP_Ref;
    const wchar_t *HUD_Label_High;
    const wchar_t *HUD_Label_Low;
    const wchar_t *HUD_Label_Ref;
    const wchar_t *Settings_Header_GalleryTrigger;
    const wchar_t *Settings_Label_GalleryTriggerMode;
    const wchar_t *Settings_Option_GalleryTriggerAuto;
    const wchar_t *Settings_Option_GalleryTriggerDelay;
    const wchar_t *Settings_Option_GalleryTriggerClick;
    const wchar_t *Settings_Option_GalleryTriggerDisable;
    const wchar_t *Settings_Tooltip_GalleryTrigger;

    const wchar_t *OSD_SlideshowStarted;
    const wchar_t *OSD_SlideshowStopped;
    const wchar_t *OSD_SlideshowResumed;
    const wchar_t *OSD_SlideshowPaused;
    const wchar_t *OSD_ImmersiveSpotlight;
    const wchar_t *OSD_ImmersiveNormal;
    const wchar_t *OSD_PrintJobStarted;
    const wchar_t *OSD_PrintJobFinished;
    const wchar_t *Context_SlideshowMode;
    const wchar_t *Settings_Label_SlideshowInterval;
    const wchar_t *Settings_Label_SlideshowImmersive;
    const wchar_t *Settings_Label_CustomLiteInfoPanel;
    const wchar_t *Settings_Label_ItemsInNormalMode;
    const wchar_t *Settings_Label_ItemsInCompareMode;
    const wchar_t *Settings_Label_SeparatorPreset;
    const wchar_t *Settings_Option_SlideshowNormal;
    const wchar_t *Settings_Option_SlideshowSpotlight;
    const wchar_t *Settings_Label_LoupeShape;
    const wchar_t *Settings_Option_LoupeShapeSquare;
    const wchar_t *Settings_Option_LoupeShapeCircle;
    const wchar_t *Settings_Tooltip_LoupeHotkey;
};

// ----------------------------------------------------------------
// EN Table
// ----------------------------------------------------------------
static const LanguageTable Table_EN = {
    L"No image loaded", // OSD_NoImage
    L"Lossless", // OSD_Lossless
    L"Re-encoded (Lossless)", // OSD_ReencodedLossless
    L"Cropped", // OSD_EdgeAdapted
    L"Lossy", // OSD_Reencoded
    L"Access denied - file may be in use or read-only", // OSD_ReadOnly
    L"Transform is not perfect (Edge optimized)", // OSD_NotPerfect
    L"Video Wall: ON", // OSD_SpanOn
    L"Video Wall: OFF", // OSD_SpanOff
    L"Before (Original)", // OSD_CompareBefore
    L"After (Proofed)", // OSD_CompareAfter
    L"Undo delete succeeded", // OSD_UndoDeleteSuccess
    L"Undo delete failed", // OSD_UndoDeleteFailed
    L"Undo rename succeeded", // OSD_UndoRenameSuccess
    L"Undo rename failed", // OSD_UndoRenameFailed
    L"Undo rotate/flip succeeded", // OSD_UndoTransformSuccess
    L"Undo rotate/flip failed", // OSD_UndoTransformFailed
    L"Detected out-of-gamut colors", // OSD_GamutDetected
    L"Gamut: Incompatible profile or parsing failed", // OSD_GamutIncompatible
    L"Gamut: Analysis failed", // OSD_GamutFailed
    L"Rotate 90\x00B0 CW", // Action_RotateCW
    L"Rotate 90\x00B0 CCW", // Action_RotateCCW
    L"Rotate 180\x00B0", // Action_Rotate180
    L"Flip Horizontal", // Action_FlipH
    L"Flip Vertical", // Action_FlipV
    L"Save Changes?", // Dialog_SaveTitle
    L"The image has been modified. Do you want to save changes?", // Dialog_SaveContent
    L"Save", // Dialog_ButtonSave
    L"Save As...", // Dialog_ButtonSaveAs
    L"Discard", // Dialog_ButtonDiscard
    L"Continue", // Dialog_ButtonContinue
    L"Always save lossless transforms", // Checkbox_AlwaysSaveLossless
    L"Always save edge-adapted", // Checkbox_AlwaysSaveEdgeAdapted
    L"Always save re-encoded", // Checkbox_AlwaysSaveLossy
    L"Directly to Recycle Bin, do not confirm again", // Checkbox_NeverConfirmDelete
    L"Cannot decode HEIC - Install HEVC Video Extension", // OSD_HEICCodecMissing
    L"Cannot decode HEIC", // Dialog_HEICTitle
    L"Your system is missing the HEVC Video Extension.\nQuickView uses " L"system hardware acceleration for best performance.", // Dialog_HEICContent
    L"Get Extension (Free)", // Dialog_HEICGetExtension
    L"Cancel", // Dialog_Cancel
    L"General", // Settings_Tab_General
    L"About", // Settings_Tab_About
    L"Foundation", // Settings_Group_Foundation
    L"Startup", // Settings_Group_Startup
    L"Habits", // Settings_Group_Habits
    L"Language", // Settings_Label_Language
    L"Single Instance", // Settings_Label_SingleInstance
    L"Check Updates", // Settings_Label_CheckUpdates
    L"Update Channel", // Settings_Label_UpdateChannel
    L"Stable", // Settings_Option_UpdateStable
    L"Pre-release", // Settings_Option_UpdatePreRelease
    L"Stable: Published after a preview version proves stable.\nPre-release: Delivers the latest code changes immediately to collect your feedback. Note: These changes are not strictly tested.", // Settings_Tooltip_PreRelease
    L"Loop", // Settings_Label_NavLoopMode
    L"Sort Order", // Settings_Label_SortOrder
    L"Descending", // Settings_Label_SortDescending
    L"Always sort archives by name", // Settings_Label_SortArchivesByNameAscending
    L"Confirm Before Delete", // Settings_Label_ConfirmDel
    L"Portable Mode / Cleanup", // Settings_Label_Portable
    L"Portable Mode / Registry Cleanup:\nWhen enabled, QuickView runs in " L"portable mode. It will automatically clean up existing registry " L"associations, disable automatic registry modification, and store " L"configuration files in the application directory instead of AppData.", // Settings_Tooltip_Portable
    L"Span Displays", // Settings_Label_SpanDisplays
    L"UI Scale", // Settings_Label_UIScale
    L"Restart required", // Settings_Status_RestartRequired
    L"No Write Permission!", // Settings_Status_NoWritePerm
    L"Enabled", // Settings_Status_Enabled
    L"Powered by", // Settings_Header_PoweredBy
    L"Open...\tCtrl+O", // Context_Open
    L"Open With...", // Context_OpenWith
    L"Edit\tE", // Context_Edit
    L"Show in Explorer", // Context_ShowInExplorer
    L"Open Folder", // Context_OpenFolder
    L"Copy Image\tCtrl+C", // Context_CopyImage
    L"Copy Path\tCtrl+Alt+C", // Context_CopyPath
    L"Print\tCtrl+P", // Context_Print
    L"Rotate 90\x00B0 CW\tR", // Context_RotateCW
    L"Rotate 90\x00B0 CCW\tShift+R", // Context_RotateCCW
    L"Flip Horizontal\tH", // Context_FlipH
    L"Flip Vertical\tV", // Context_FlipV
    L"Transform", // Context_Transform
    L"Actual Size (100%)\t1 / Z", // Context_ActualSize
    L"Fit to Screen\t0 / F", // Context_FitToScreen
    L"Fit Window", // Context_FitWindow
    L"Fill Window", // Context_FillWindow
    L"Zoom In\t+ / Ctrl +", // Context_ZoomIn
    L"Zoom Out\t- / Ctrl -", // Context_ZoomOut
    L"Lock Window", // Context_LockWindow
    L"Always on Top\tCtrl+T", // Context_AlwaysOnTop
    L"HUD Gallery\tT", // Context_HUDGallery
    L"Lite Info Panel\tTab", // Context_LiteInfoPanel
    L"Full Info Panel\tI", // Context_FullInfoPanel
    L"Render RAW", // Context_RenderRAW
    L"Pixel Art Mode", // Context_PixelArtMode
    L"Color Space", // Context_ColorSpace
    L"Fullscreen\tF11", // Context_Fullscreen
    L"View", // Context_View
    L"Fill", // Context_WallpaperFill
    L"Fit", // Context_WallpaperFit
    L"Tile", // Context_WallpaperTile
    L"Set as Wallpaper", // Context_SetAsWallpaper
    L"Rename\tF2", // Context_Rename
    L"Fix Extension", // Context_FixExtension
    L"Delete\tDel", // Context_Delete
    L"Undo Delete\tCtrl+Z", // Context_UndoDelete
    L"Undo Rename\tCtrl+Z", // Context_UndoRename
    L"Undo Rotate/Flip\tCtrl+Z", // Context_UndoTransform
    L"Undo\tCtrl+Z", // Context_Undo
    L"Sort By", // Context_SortBy
    L"Navigation Order", // Context_NavOrder
    L"Ascending", // Context_SortAscending
    L"Descending", // Context_SortDescending
    L"Settings...", // Context_Settings
    L"Compare Mode\tC", // Context_CompareMode
    L"Overlay Mode\tCtrl+Shift+O", // Context_OverlayMode
    L"Open in Compare Mode", // Context_GalleryOpenCompare
    L"Open in New Window", // Context_GalleryOpenNewWindow
    L"Exit\tMButton/Esc", // Context_Exit
    L"Exit Mouse Passthrough", // Menu_ExitPassthrough
    L"Mouse Passthrough Mode", // Dialog_PassthroughTitle
    L"Mouse events will pass through to the window below.\nOnly global hotkey (Shift+Esc) or taskbar menu can exit this mode.\n\nContinue?", // Dialog_PassthroughContent
    L"Click-Through: ON (Shift+Esc to exit)", // OSD_PassthroughOn
    L"Click-Through: OFF", // OSD_PassthroughOff
    L"Overlay Mode: ON", // OSD_OverlayModeOn
    L"Overlay Mode: OFF", // OSD_OverlayModeOff
    L"Opacity", // OSD_Opacity
    L"Zoom In (Overlay)", // Toolbar_Tooltip_OverlayZoomIn
    L"Zoom Out (Overlay)", // Toolbar_Tooltip_OverlayZoomOut
    L"Increase Opacity", // Toolbar_Tooltip_OverlayAlphaUp
    L"Decrease Opacity", // Toolbar_Tooltip_OverlayAlphaDown
    L"Enter Mouse Passthrough", // Toolbar_Tooltip_OverlayPassthroughOn
    L"Exit Mouse Passthrough", // Toolbar_Tooltip_OverlayPassthroughOff
    L"Exit Overlay Mode", // Toolbar_Tooltip_OverlayExit
    L"Error", // Message_SaveErrorTitle
    L"Failed to save file. File locked?", // Message_SaveErrorContent
    L"Previous (Left)", // Toolbar_Tooltip_Prev
    L"Next (Right)", // Toolbar_Tooltip_Next
    L"Rotate Left (Shift+R)", // Toolbar_Tooltip_RotateL
    L"Rotate Right (R)", // Toolbar_Tooltip_RotateR
    L"Flip Horizontal (H)", // Toolbar_Tooltip_FlipH
    L"Lock Window (Temp)", // Toolbar_Tooltip_Lock
    L"Unlock Window", // Toolbar_Tooltip_Unlock
    L"Gallery (T)", // Toolbar_Tooltip_Gallery
    L"Info Panel", // Toolbar_Tooltip_Info
    L"RAW: Preview (Click for Full)", // Toolbar_Tooltip_RawPreview
    L"RAW: Full Decode (Click for Preview)", // Toolbar_Tooltip_RawFull
    L"RAW: View paired RAW (Full Decode)", // Toolbar_Tooltip_RawPairView
    L"RAW: Back to paired image", // Toolbar_Tooltip_RawPairBack
    L"Extension Mismatch (Fix)", // Toolbar_Tooltip_FixExtension
    L"Pin Toolbar", // Toolbar_Tooltip_Pin
    L"Unpin Toolbar", // Toolbar_Tooltip_Unpin
    L"Show out-of-gamut highlight areas", // Toolbar_Tooltip_GamutWarning
    L"Normal Mode", // Toolbar_Tooltip_NormalMode
    L"Compare Mode", // Toolbar_Tooltip_CompareMode
    L"Single Page Mode", // Toolbar_Tooltip_SinglePage
    L"Dual Page Mode", // Toolbar_Tooltip_DualPage
    L"Open New Image in Selection", // Toolbar_Tooltip_CompareOpen
    L"Swap Left/Right", // Toolbar_Tooltip_CompareSwap
    L"Toggle Layout", // Toolbar_Tooltip_CompareLayout
    L"Compare Info", // Toolbar_Tooltip_CompareInfo
    L"Delete Selected Image", // Toolbar_Tooltip_CompareDelete
    L"Zoom In (Fine-tune)", // Toolbar_Tooltip_CompareZoomIn
    L"Zoom Out (Fine-tune)", // Toolbar_Tooltip_CompareZoomOut
    L"Zoom Sync: ON", // Toolbar_Tooltip_CompareSyncZoomOn
    L"Zoom Sync: OFF", // Toolbar_Tooltip_CompareSyncZoomOff
    L"Pan Sync: ON", // Toolbar_Tooltip_CompareSyncPanOn
    L"Pan Sync: OFF", // Toolbar_Tooltip_CompareSyncPanOff
    L"Toggle Immersive Spotlight", // Toolbar_Tooltip_SlideshowImmersiveToggle
    L"Exit Slideshow Mode", // Toolbar_Tooltip_SlideshowExit
    L"Exit Compare", // Toolbar_Tooltip_CompareExit
    L"Play Animation", // Toolbar_Tooltip_AnimPlay
    L"Pause Animation", // Toolbar_Tooltip_AnimPause
    L"Previous Frame", // Toolbar_Tooltip_AnimPrev
    L"Next Frame", // Toolbar_Tooltip_AnimNext
    L"Dirty Rect Debug: ON", // Toolbar_Tooltip_AnimDirtyOn
    L"Dirty Rect Debug: OFF", // Toolbar_Tooltip_AnimDirtyOff
    L"Animation Speed", // Toolbar_Tooltip_AnimSpeed
    L"Performance", // Settings_Header_Performance
    L"Professional Tools", // Settings_Header_Professional
    L"Smart Memory Management", // Settings_Label_MemoryReclaim
    L"Smart", // Settings_Option_MemSmart
    L"Aggressive", // Settings_Option_MemAggressive
    L"On-Demand", // Settings_Option_MemOnDemand
    L"Smart: Automatically reclaim memory only when system RAM < 4GB.\n" L"Aggressive: Keep memory reserved for absolute 0ns allocation speed.\n" L"On-Demand: Always reclaim idle memory to save physical RAM.", // Settings_Tooltip_MemoryReclaim
    L"Show update regions button in animation mode", // Settings_Label_ShowDirtyRect
    L"Show the update region debug button in animation mode to visualize which parts of the frame are being redrawn.", // Settings_Tooltip_ShowDirtyRect
    L"Copied!", // OSD_Copied
    L"Coordinates copied!", // OSD_CoordinatesCopied
    L"File path copied!", // OSD_FilePathCopied
    L"Zoom: 100%", // OSD_Zoom100
    L"Zoom: Fit Screen", // OSD_ZoomFit
    L"Zoom: Fit Window", // OSD_ZoomFitWindow
    L"Zoom: Fill Window", // OSD_ZoomFill
    L"Print: Use Ctrl+P in opened app", // OSD_PrintInstruction
    L"Moved to Recycle Bin", // OSD_MovedToRecycleBin
    L"Window Locked", // OSD_WindowLocked
    L"Window Unlocked", // OSD_WindowUnlocked
    L"Always on Top: ON", // OSD_AlwaysOnTopOn
    L"Always on Top: OFF", // OSD_AlwaysOnTopOff
    L"Wallpaper Set", // OSD_WallpaperSet
    L"Failed to set wallpaper", // OSD_WallpaperFailed
    L"Renamed", // OSD_Renamed
    L"Rename Failed", // OSD_RenameFailed
    L"Extension Fixed", // OSD_ExtensionFixed
    L"Restored", // OSD_Restored
    L"First image", // OSD_FirstImage
    L"Last image", // OSD_LastImage
    L"HD", // OSD_HD
    L"Zoom: ", // OSD_ZoomPrefix
    L"Playing", // OSD_AnimPlaying
    L"Paused (Inspector Mode: Alt+Left/Right to Seek)", // OSD_AnimPaused
    L"Dirty Rect: ON", // OSD_AnimDirtyOn
    L"Dirty Rect: OFF", // OSD_AnimDirtyOff
    L"Span Displays (Video Wall)\tCtrl+F11", // Context_SpanDisplays
    L"Interface", // Settings_Tab_Visuals
    L"Controls", // Settings_Tab_Controls
    L"Image", // Settings_Tab_Image
    L"Advanced", // Settings_Tab_Advanced
    L"Backdrop", // Settings_Header_Backdrop
    L"Window", // Settings_Header_Window
    L"Window Lock", // Settings_Header_WindowLock
    L"Panel", // Settings_Header_Panel
    L"Mouse", // Settings_Header_Mouse
    L"Edge", // Settings_Header_Edge
    L"Render", // Settings_Header_Render
    L"HDR", // Settings_Header_Hdr
    L"Prompts", // Settings_Header_Prompts
    L"System", // Settings_Header_System
    L"Features", // Settings_Header_Features
    L"Transparency", // Settings_Header_Transparency
    L"Glass Engine (GPU)", // Settings_Header_GeekGlass
    L"Enable Glass", // Settings_Label_EnableGeekGlass
    L"UI Animations", // Settings_Label_GlassUIAnimations
    L"Component Material", // Settings_Header_CoreMaterial
    L"Glass Blur Sigma", // Settings_Label_BlurSigma
    L"Glass Disabled (System)", // Settings_Status_GlassDisabled
    L"Tint Layer", // Settings_Label_TintDensity
    L"Overall color intensity of the glass frost effect.", // Settings_Tooltip_TintDensity
    L"Reflex (Specular)", // Settings_Label_SpecularOpacity
    L"Brightness of the diagonal lighting reflections.", // Settings_Tooltip_SpecularOpacity
    L"Shadow Depth", // Settings_Label_ShadowIntensity
    L"Strength of the ambient occlusion shadows.", // Settings_Tooltip_ShadowIntensity
    L"Vector Rendering", // Settings_Header_VectorAssets
    L"Icon Stroke weight", // Settings_Label_VectorStrokeWeight
    L"Standard (1.5px)", // Settings_Option_StrokeStandard
    L"Fine (1.0px)", // Settings_Option_StrokeFine
    L"Tint Profile", // Settings_Header_GlassTint
    L"Color logic", // Settings_Label_TintProfile
    L"Auto (Adaptive)", // Settings_Option_TintAuto
    L"Custom Color", // Settings_Option_TintCustom
    L"Manual Tint", // Settings_Label_GlassCustomColor
    L"Control Surface Density (%)", // Settings_Header_DensityMatrix
    L"OSD & HUD", // Settings_Label_OsdDensity
    L"Transparency for small floating overlays.", // Settings_Tooltip_OsdDensity
    L"Toolbar & Sidebars", // Settings_Label_PanelsDensity
    L"Transparency for the bottom toolbar, information panel, gallery, and top-right window controls.", // Settings_Tooltip_PanelsDensity
    L"Dialogs & Settings", // Settings_Label_ModalsDensity
    L"Transparency for centered popups.", // Settings_Tooltip_ModalsDensity
    L"Menus", // Settings_Label_MenusDensity
    L"Transparency for right-click context menus.", // Settings_Tooltip_MenusDensity
    L"Theme", // Settings_Tab_Theme
    L"Preset", // Settings_Label_ThemeMode
    L"System", // Settings_Option_ThemeAuto
    L"Dark", // Settings_Option_ThemeDark
    L"Light", // Settings_Option_ThemeLight
    L"Design", // Settings_Option_ThemeCustom
    L"Modal Dimmer", // Settings_Label_AmbientDimmer
    L"Dim the background when a modal or settings window is open.", // Settings_Tooltip_AmbientDimmer
    L"Accent Color", // Settings_Label_AccentColor
    L"Text Color", // Settings_Label_TextColor
    L"Theme Engine", // Settings_Header_ThemeManagement
    L"Export", // Settings_Action_ExportTheme
    L"Import", // Settings_Action_ImportTheme
    L"Canvas Color", // Settings_Label_CanvasColor
    L"Overlay", // Settings_Label_Overlay
    L"Show Grid Overlay", // Settings_Label_ShowGrid
    L"Image Transition Fade", // Settings_Label_CrossFade
    L"Always on Top", // Settings_Label_AlwaysOnTop
    L"Lock Window", // Settings_Label_LockWindow
    L"Controls whether the program locks the window border by default on startup, rather than following image scaling.", // Settings_Tooltip_LockWindow
    L"Auto-Hide Title Bar", // Settings_Label_AutoHideTitle
    L"Rounded Corners", // Settings_Label_RoundedCorners
    L"Show UI Borders", // Settings_Label_UIBorders
    L"Controls window and context menu rounded corners. Requires Windows 11.", // Settings_Tooltip_RoundedCorners
    L"Lock Bottom Toolbar", // Settings_Label_LockToolbar
    L"Minimum Window Width", // Settings_Label_WindowMinSize
    L"Maximum Start Size (%)", // Settings_Label_WindowMaxSizePercent
    L"Show Edge Indicators", // Settings_Label_ShowBorderIndicator
    L"Shows a blue indicator line in the direction where the image exceeds the window borders.", // Settings_Tooltip_ShowBorderIndicator
    L"Show Minimap", // Settings_Label_ShowNavigator
    L"Auto", // Settings_Option_NavigatorAuto
    L"On", // Settings_Option_NavigatorOn
    L"Off", // Settings_Option_NavigatorOff
    L"Keep window size on navigation", // Settings_Label_KeepWindowSizeOnNav
    L"Remember last window size and position", // Settings_Label_RememberLastWindowSizeAndPosition
    L"Adapt small images", // Settings_Label_UpscaleSmallImagesWhenLocked
    L"Smooth Window Scaling (GPU)", // Settings_Label_EnableSmoothScaling
    L"EXIF Panel Mode", // Settings_Label_ExifMode
    L"Toolbar Info Default", // Settings_Label_ToolbarInfoDefault
    L"Open Fullscreen", // Settings_Label_OpenFullScreenMode
    L"Fullscreen Zoom Mode", // Settings_Label_FullScreenZoomMode
    L"Fit to Screen", // Settings_Option_FitScreen
    L"Auto", // Settings_Option_AutoFit
    L"Invert Wheel", // Settings_Label_InvertWheel
    L"Zoom 100% Snap Damping", // Settings_Label_ZoomSnapDamping
    L"Mouse-Anchored Window Zoom", // Settings_Label_MouseAnchorZoom
    L"Right Button Drag Zoom", // Settings_Label_RightButtonDragZoom
    L"Wheel Zoom Speed", // Settings_Label_WheelZoomSpeed
    L"Thumb Wheel", // Settings_Label_ThumbWheel
    L"Right Drag Zoom Speed", // Settings_Label_RightDragZoomSpeed
    L"Zoom Speed (Temp): ", // OSD_WheelZoomSpeed
    L"Temporarily Adjust Zoom Speed", // Help_Action_AdjustZoomSpeed
    L"Temporarily Lock Window Zoom", // Help_Action_LockWindowZoom
    L"Invert Side Buttons", // Settings_Label_InvertButtons
    L"Use Fixed Zoom Levels", // Settings_Label_UseFixedZoom
    L"When enabled, Alt + Wheel performs regular zoom instead of changing zoom speed.", // Settings_Tooltip_UseFixedZoom
    L"  └  Custom Zoom Levels", // Settings_Label_FixedZoomLevels
    L"Edit Zoom Levels", // Dialog_FixedZoomTitle
    L"Enter comma-separated zoom ratios (e.g. 0.5, 1, 2):", // Dialog_FixedZoomMsg
    L"Zoom Mode (In)", // Settings_Label_ZoomModeIn
    L"Zoom Mode (Out)", // Settings_Label_ZoomModeOut
    L"Left Drag", // Settings_Label_LeftDrag
    L"Middle Drag", // Settings_Label_MiddleDrag
    L"Middle Click", // Settings_Label_MiddleClick
    L"Edge Nav Click", // Settings_Label_EdgeNavClick
    L"Disable in Compare Mode", // Settings_Label_DisableEdgeNavInCompare
    L"Nav Indicator", // Settings_Label_NavIndicator
    L"Auto Rotate (EXIF)", // Settings_Label_AutoRotate
    L"Color Management", // Settings_Label_CMS
    L"Advanced Color (HDR)", // Settings_Label_AdvancedColor
    L"HDR Tone Mapping", // Settings_Label_HdrToneMapping
    L"Spline Knee Point", // Settings_Label_HdrSplineKnee
    L"HDR Tone Mapping strategy:\nDetermines how HDR images are displayed when " L"exceeding monitor capabilities.\nSpline: High-fidelity highlight " L"roll-off using piecewise spline (Recommended).\nColorimetric: Strict " L"luminance mapping; highlights exceeding the monitor limit are " L"clipped.\nBT.2390 (EETF): ITU-R BT.2390 EETF curve for high-fidelity tone mapping.", // Settings_Tooltip_HdrToneMapping
    L"0 = Auto (Calculated based on image luminance).\nThe value represents the ratio of the monitor's peak luminance. Brightness below this knee point maps 1:1, while brightness above is smoothly compressed using a spline curve.\n(Recommended: 0.4 - 0.75)", // Settings_Tooltip_HdrSplineKnee
    L"HDR Peak Brightness (Nits)", // Settings_Label_HdrPeakNitsOverride
    L"Set to 0 to use system detected brightness.", // Settings_Tooltip_HdrPeakNitsOverride
    L"HDR Peak Percentile", // Settings_Label_HdrPeakPercentile
    L"Discard extremely bright pixels to raise overall brightness (mpv default: 99.995%).", // Settings_Tooltip_HdrPeakPercentile
    L"100% (Absolute Peak)", // Settings_Option_HdrPeakPercentile_100
    L"99.995% (Stable)", // Settings_Option_HdrPeakPercentile_99995
    L"99.9% (Aggressive)", // Settings_Option_HdrPeakPercentile_999
    L"HDR Highlight Desaturation Range", // Settings_Label_HdrDesatThreshold
    L"Start threshold for highlight desaturation. 0.0 means desaturate all brightness, 1.0 means no desaturation. Recommended default is 0.18.", // Settings_Tooltip_HdrDesatThreshold
    L"HDR Highlight Desaturation Strength", // Settings_Label_HdrMaxDesat
    L"Maximum intensity of desaturation at extreme highlights. 0.0 means no desaturation, 1.0 means fully desaturated to white. Recommended default is 0.75.", // Settings_Tooltip_HdrMaxDesat
    L"Colorimetric", // Settings_Option_HdrColorimetric
    L"Spline", // Settings_Option_HdrSpline
    L"BT.2390 (EETF)", // Settings_Option_HdrLegacyReinhard
    L"Untagged Image Fallback", // Settings_Label_CmsFallback
    L"Soft Proof Profile (.icc)", // Settings_Label_CustomProof
    L"Soft Proofing Preview", // Context_SoftProofing
    L"Proof Profile", // Context_SoftProofProfile
    L"Custom...", // Context_SoftProofCustom
    L"Coming Soon", // Settings_Value_ComingSoon
    L"Force RAW Decode", // Settings_Label_ForceRaw
    L"RAW+JPEG Pairing", // Settings_Label_PairRawJpeg
    L"Show a RAW file and its same-name camera image (JPEG/HEIF) as one photo.\nOnly the rendered image stays in the list, marked with the RAW format (e.g. +CR3); capture times are verified in the background and mismatched pairs are split.\nSwitch between the camera image and the RAW decode with the RAW toolbar button or its hotkey, or view both with the pair-compare hotkey.", // Settings_Tooltip_PairRawJpeg
    L"Exposure (Brightness)", // Settings_Label_Exposure
    L"Adjust image brightness (Exposure Compensation). Range: 0.18x to 10.0x.", // Settings_Tooltip_Exposure
    L"Add to Open With", // Settings_Label_AddToOpenWith
    L"Custom Image Editor", // Settings_Label_CustomEditor
    L"Select Editor", // Context_SelectEditor
    L"Failed to launch editor. Please configure it again.", // OSD_EditorLaunchFailed
    L"Add", // Settings_Action_Add
    L"Added", // Settings_Action_Added
    L"Disabled in Portable Mode", // Settings_Status_DisabledInPortable
    L"Enable Debug HUD (F12)", // Settings_Label_DebugHUD
    L"Prefetch System", // Settings_Label_Prefetch
    L"Info Panel", // Settings_Label_InfoPanelAlpha
    L"Toolbar", // Settings_Label_ToolbarAlpha
    L"Settings", // Settings_Label_SettingsAlpha
    L"Reset All Settings", // Settings_Label_Reset
    L"Restore", // Settings_Action_Restore
    L"Done", // Settings_Action_Done
    L"Unmanaged", // Settings_Option_CmsUnmanaged
    L"sRGB", // Settings_Option_CmssRGB
    L"Display P3", // Settings_Option_CmsP3
    L"Adobe RGB (1998)", // Settings_Option_CmsAdobeRGB
    L"Grayscale (Tonal Check)", // Settings_Option_CmsGray
    L"ProPhoto RGB", // Settings_Option_CmsProPhoto
    L"Rendering Intent", // Settings_Label_CmsIntent
    L"Gamut Warning Detection", // Settings_Label_GamutWarning
    L"Analyze and highlight out-of-gamut areas. Options: Off, detect only in soft proofing mode, or detect both soft proofing and screen gamut.", // Settings_Tooltip_GamutWarning
    L"Auto Prompt on Gamut Error", // Settings_Label_GamutAutoPrompt
    L"Show OSD notification when gamut errors are detected. Highlights can be manually toggled via toolbar.", // Settings_Tooltip_GamutAutoPrompt
    L"Gamut Warning Highlight Color", // Settings_Label_GamutColor
    L"Relative Colorimetric", // Settings_Option_CmsIntentRelative
    L"Perceptual", // Settings_Option_CmsIntentPerceptual
    L"Enable Color Management System.\nWhen enabled, applies high-precision color space conversion via GPU to restore true colors.\nDisabling it reduces GPU load, but may result in oversaturated colors on wide-gamut displays.", // Settings_Tooltip_CMS
    L"Rendering Intent for color space conversion.\nPerceptual: Compresses out-of-gamut colors to preserve details and gradients (ideal for photos).\nRelative Colorimetric: Preserves in-gamut colors and clips out-of-gamut ones (ideal for UI and icons).\nNote: Visual differences only occur when using advanced ICC profiles containing LUTs (Lookup Tables). Standard matrix profiles will automatically fallback to Relative Colorimetric.", // Settings_Tooltip_CmsIntent
    L"Enable 16-bit floating-point rendering pipeline (scRGB).\nWhen enabled, perfectly renders photo highlights on HDR-capable displays by breaking the SDR limit.\nDisabling it forces mapping to SDR output.\nNote: Enabling increases VRAM usage.", // Settings_Tooltip_AdvancedColor
    L"Auto: 100% scale when image is smaller than screen, fit to screen when larger.", // Settings_Tooltip_ZoomAuto
    L"Check for Updates", // Settings_Action_CheckUpdates
    L"View Update", // Settings_Action_ViewUpdate
    L"Checking...", // Settings_Status_Checking
    L"Up to date", // Settings_Status_UpToDate
    L"GitHub Repo", // Settings_Link_GitHub
    L"Report Issue", // Settings_Link_ReportIssue
    L"Help (F1)", // Settings_Link_Hotkeys
    L"Version", // Settings_Label_Version
    L"Build", // Settings_Label_Build
    L"Black", // Settings_Option_Black
    L"White", // Settings_Option_White
    L"Grid", // Settings_Option_Grid
    L"Custom", // Settings_Option_Custom
    L"Off", // Settings_Option_Off
    L"On", // Settings_Option_On
    L"Lite", // Settings_Option_Lite
    L"Full", // Settings_Option_Full
    L"Large Only", // Settings_Option_LargeOnly
    L"All", // Settings_Option_All
    L"Soft Proofing", // Settings_Option_SoftProofing
    L"Window", // Settings_Option_Window
    L"Pan", // Settings_Option_Pan
    L"None", // Settings_Option_None
    L"Exit", // Settings_Option_Exit
    L"Arrow", // Settings_Option_Arrow
    L"Cursor", // Settings_Option_Cursor
    L"Manual", // Settings_Option_Manual
    L"Auto (Explorer)", // Settings_Option_SortAuto
    L"Name", // Settings_Option_SortName
    L"Date Modified", // Settings_Option_SortModified
    L"Date Taken (EXIF)", // Settings_Option_SortDateTaken
    L"Size", // Settings_Option_SortSize
    L"Type", // Settings_Option_SortType
    L"Loop", // Settings_Option_NavLoop
    L"Through subfolders", // Settings_Option_NavThrough
    L"Linear: Basic smoothing", // Settings_Option_Linear
    L"Nearest: Extreme sharpness", // Settings_Option_Nearest
    L"HQ Cubic: Extreme smoothing", // Settings_Option_HighQualityCubic
    L"Auto", // Settings_Option_ZoomAuto
    L"Auto", // Settings_Option_Auto
    L"Eco", // Settings_Option_Eco
    L"Balanced", // Settings_Option_Balanced
    L"Ultra", // Settings_Option_Ultra
    L"Keyboard Shortcuts", // Help_Header_Shortcuts
    L"Mouse Actions", // Help_Header_Mouse
    L"Next / Previous Image", // Help_Item_NextPrev
    L"Zoom In / Out", // Help_Item_Zoom
    L"Pan Image", // Help_Item_Pan
    L"Rotate", // Help_Item_Rotate
    L"Fit to Screen", // Help_Item_Fit
    L"Delete Image", // Help_Item_Delete
    L"Fullscreen", // Help_Item_Fullscreen
    L"Close", // Help_Item_Close
    L"Left Button", // Help_Mouse_Left
    L"Middle Button", // Help_Mouse_Middle
    L"Wheel", // Help_Mouse_Wheel
    L"Right Button", // Help_Mouse_Right
    L"Right Button Vertical Drag", // Help_Mouse_RightVerticalDrag
    L"Move Window / Exit Fullscreen / Exit Maximized", // Help_Action_MoveWindow
    L"Pan Image", // Help_Action_PanImage
    L"Context Menu", // Help_Action_ContextMenu
    L"Next/Prev Image", // Help_Action_NextPrev
    L"Zoom", // Help_Action_Zoom
    L"Smart Zoom (100% / Fit / Restore)", // Help_Action_SmartZoom
    L"Copy Image", // Help_Desc_Copy
    L"Edit", // Help_Desc_Edit
    L"Tips & Glossary", // Help_Header_Tips
    L"Note: Shortcuts apply to the current window only. Settings are global.", // Help_Tip_ContextScope
    L"Rotation: 'Edge Adapted' means minor cropping to align with codec " L"blocks (lossless). 'Lossy' means re-encoding is required.", // Help_Tip_Rotation
    L"Video Wall (Ctrl+F11): Spans all screens. If the close button is " L"hidden, double-click anywhere to exit.", // Help_Tip_VideoWall
    L"Tracing Mode / Film Mode: Once enabled, the image becomes semi-transparent, revealing underlying elements. You can adjust its size or transparency. Click the Mouse Passthrough toggle in the toolbar to enter Passthrough Mode, where all inputs except Shift+Esc are ignored, turning QuickView into a transparent overlay.", // Help_Tip_DesignerMode
    L"Gamut Warning: Detects out-of-gamut colors for the target display or soft proofing profile. Modes: Off, Soft Proofing only, or All (Default: Soft Proofing). Toggle via toolbar.", // Help_Tip_GamutDetection
    L"RAW: Shows embedded preview by default for speed. Click the RAW button " L"to fully decode (colors may vary).", // Help_Tip_Raw
    L"JPEG Quality: Estimated value (e.g. Photoshop 100% ≈ 98%). May vary " L"slightly from save settings due to encoder variance, which is normal.", // Help_Tip_JpegQ
    L"Soft Proof Comparison: Entering Compare Mode while Soft Proofing is active " L"will automatically compare the original vs. proofed image.", // Help_Tip_SoftProofCompare
    L"New Version Available!", // Dialog_UpdateTitle
    L"v%s is ready.", // Dialog_UpdateContent
    L"Changelog", // Dialog_UpdateLogHeader
    L"Update Now", // Dialog_ButtonUpdate
    L"Later", // Dialog_ButtonLater
    L"Star on GitHub", // Dialog_ButtonStar
    L"QuickView is built with love", // Dialog_Update_LoveTitle
    L"I maintain QuickView in my spare time because I believe Windows " L"deserves a faster, cleaner viewer. I don't have a marketing budget or " L"a team. If you enjoy this update, the biggest contribution you can " L"make is to Star us on GitHub or share it with a friend.", // Dialog_Update_LoveMessage
    L"Compare Mode", // Help_Item_Compare
    L"First / Last Image", // Help_Item_FirstLast
    L"PHYSICAL ATTRIBUTES", // HUD_Group_Physical
    L"SCIENTIFIC QUALITY", // HUD_Group_Scientific
    L"OPTICS & ENCODING", // HUD_Group_Encoding
    L"Edge definition (Laplacian Variance)", // HUD_Tip_Sharp_Desc
    L"Crisp edges, high detail", // HUD_Tip_Sharp_High
    L"Soft focus or motion blur", // HUD_Tip_Sharp_Low
    L"> 500 is very sharp", // HUD_Tip_Sharp_Ref
    L"Information density (Shannon Entropy)", // HUD_Tip_Ent_Desc
    L"Complex textures or high noise", // HUD_Tip_Ent_High
    L"Flat areas or low detail", // HUD_Tip_Ent_Low
    L"7.0-8.0 is high detail", // HUD_Tip_Ent_Ref
    L"Bits Per Pixel (Compression Efficiency)", // HUD_Tip_BPP_Desc
    L"Lower efficiency (more data preserved)", // HUD_Tip_BPP_High
    L"Higher efficiency (higher compression)", // HUD_Tip_BPP_Low
    L"24.0 (Raw RGB), ~2.0-3.0 (High JPEG), ~0.5-1.5 (WebP/AVIF)", // HUD_Tip_BPP_Ref
    L"High: ", // HUD_Label_High
    L"Low: ", // HUD_Label_Low
    L"Ref: ", // HUD_Label_Ref
    L"Gallery Filmstrip (Top Hover)", // Settings_Header_GalleryTrigger
    L"Trigger Mode", // Settings_Label_GalleryTriggerMode
    L"Auto Hover", // Settings_Option_GalleryTriggerAuto
    L"Hotspot Hover", // Settings_Option_GalleryTriggerDelay
    L"Click Hotspot", // Settings_Option_GalleryTriggerClick
    L"Disabled", // Settings_Option_GalleryTriggerDisable
    L"This feature is automatically disabled when the window is smaller than 600x450.", // Settings_Tooltip_GalleryTrigger

    L"Slideshow Started", // OSD_SlideshowStarted
    L"Slideshow Stopped", // OSD_SlideshowStopped
    L"Slideshow Resumed", // OSD_SlideshowResumed
    L"Slideshow Paused", // OSD_SlideshowPaused
    L"Immersive: Spotlight", // OSD_ImmersiveSpotlight
    L"Immersive: Normal", // OSD_ImmersiveNormal
    L"Print job submitted. You can continue browsing.", // OSD_PrintJobStarted
    L"Print job sent successfully.", // OSD_PrintJobFinished
    L"Slideshow Mode", // Context_SlideshowMode
    L"Interval (seconds)", // Settings_Label_SlideshowInterval
    L"Immersive Mode", // Settings_Label_SlideshowImmersive
    L"Custom Lite Info Panel", // Settings_Label_CustomLiteInfoPanel
    L"items in Normal mode", // Settings_Label_ItemsInNormalMode
    L"items in Compare mode", // Settings_Label_ItemsInCompareMode
    L"Separator preset", // Settings_Label_SeparatorPreset
    L"Normal", // Settings_Option_SlideshowNormal
    L"Spotlight", // Settings_Option_SlideshowSpotlight
    L"Magnifier Shape", // Settings_Label_LoupeShape
    L"Square", // Settings_Option_LoupeShapeSquare
    L"Circle", // Settings_Option_LoupeShapeCircle
    L"Hold the shortcut key and scroll the mouse wheel to adjust the magnifier size.", // Settings_Tooltip_LoupeHotkey
};

// ----------------------------------------------------------------
// CN Table
// ----------------------------------------------------------------
static const LanguageTable Table_CN = {
    L"没有加载图片", // OSD_NoImage
    L"无损", // OSD_Lossless
    L"重编码 (无损)", // OSD_ReencodedLossless
    L"边缘优化", // OSD_EdgeAdapted
    L"重编码", // OSD_Reencoded
    L"拒绝访问 - 文件即使被占用或只读", // OSD_ReadOnly
    L"变换不完美 (已进行边缘优化)", // OSD_NotPerfect
    L"跨屏模式: 开启", // OSD_SpanOn
    L"跨屏模式: 关闭", // OSD_SpanOff
    L"打样前 (Original)", // OSD_CompareBefore
    L"打样后 (Proofed)", // OSD_CompareAfter
    L"已撤销删除", // OSD_UndoDeleteSuccess
    L"撤销删除失败", // OSD_UndoDeleteFailed
    L"已撤销重命名", // OSD_UndoRenameSuccess
    L"撤销重命名失败", // OSD_UndoRenameFailed
    L"已撤销旋转/翻转", // OSD_UndoTransformSuccess
    L"撤销旋转/翻转失败", // OSD_UndoTransformFailed
    L"检测到色彩溢出", // OSD_GamutDetected
    L"色彩空间不匹配 / 配置文件解析失败", // OSD_GamutIncompatible
    L"色彩溢出分析失败", // OSD_GamutFailed
    L"顺时针旋转 90\x00B0", // Action_RotateCW
    L"逆时针旋转 90\x00B0", // Action_RotateCCW
    L"旋转 180\x00B0", // Action_Rotate180
    L"水平翻转", // Action_FlipH
    L"垂直翻转", // Action_FlipV
    L"保存更改?", // Dialog_SaveTitle
    L"图像已被修改。是否保存更改?", // Dialog_SaveContent
    L"保存", // Dialog_ButtonSave
    L"另存为...", // Dialog_ButtonSaveAs
    L"放弃", // Dialog_ButtonDiscard
    L"继续", // Dialog_ButtonContinue
    L"总是保存无损变换", // Checkbox_AlwaysSaveLossless
    L"总是保存边缘优化结果", // Checkbox_AlwaysSaveEdgeAdapted
    L"总是保存重编码结果", // Checkbox_AlwaysSaveLossy
    L"直接放入回收站，不再确认", // Checkbox_NeverConfirmDelete
    L"无法解码 HEIC - 请安装 HEVC 视频扩展", // OSD_HEICCodecMissing
    L"无法解码 HEIC", // Dialog_HEICTitle
    L"系统缺少 HEVC 视频扩展。\nQuickView 需要系统硬件加速以获得最佳性能。", // Dialog_HEICContent
    L"获取扩展 (免费)", // Dialog_HEICGetExtension
    L"取消", // Dialog_Cancel
    L"常规", // Settings_Tab_General
    L"关于", // Settings_Tab_About
    L"基础", // Settings_Group_Foundation
    L"启动", // Settings_Group_Startup
    L"习惯", // Settings_Group_Habits
    L"语言", // Settings_Label_Language
    L"单实例模式", // Settings_Label_SingleInstance
    L"检查更新", // Settings_Label_CheckUpdates
    L"更新通道", // Settings_Label_UpdateChannel
    L"正式版", // Settings_Option_UpdateStable
    L"预览版 (Pre-release)", // Settings_Option_UpdatePreRelease
    L"正式版：预览版发布一段时间后未收到Bug报告，将发布正式版。\n预览版：第一时间发布最新的代码修改，用于收集您的测试反馈。注意，这些修改并未经过严格测试。", // Settings_Tooltip_PreRelease
    L"循环播放", // Settings_Label_NavLoopMode
    L"列表排序方式", // Settings_Label_SortOrder
    L"降序", // Settings_Label_SortDescending
    L"始终以文件名升序浏览存档文件", // Settings_Label_SortArchivesByNameAscending
    L"删除前确认", // Settings_Label_ConfirmDel
    L"便携模式 / 清理", // Settings_Label_Portable
    L"便携模式与注册表清理：\n开启后，QuickView " L"将以便携方式运行。程序将自动清理已有的注册表关联，并禁用自动注册表修改功" L"能。同时，配置文件将存放在程序所在目录而非 AppData 目录。", // Settings_Tooltip_Portable
    L"跨屏模式 (电视墙)", // Settings_Label_SpanDisplays
    L"界面缩放", // Settings_Label_UIScale
    L"需要重启", // Settings_Status_RestartRequired
    L"无写入权限!", // Settings_Status_NoWritePerm
    L"已启用", // Settings_Status_Enabled
    L"驱动技术", // Settings_Header_PoweredBy
    L"打开...\tCtrl+O", // Context_Open
    L"打开方式...", // Context_OpenWith
    L"编辑\tE", // Context_Edit
    L"在资源管理器中显示", // Context_ShowInExplorer
    L"打开文件夹", // Context_OpenFolder
    L"复制图像\tCtrl+C", // Context_CopyImage
    L"复制路径\tCtrl+Alt+C", // Context_CopyPath
    L"打印\tCtrl+P", // Context_Print
    L"顺时针旋转 90\x00B0\tR", // Context_RotateCW
    L"逆时针旋转 90\x00B0\tShift+R", // Context_RotateCCW
    L"水平翻转\tH", // Context_FlipH
    L"垂直翻转\tV", // Context_FlipV
    L"变换", // Context_Transform
    L"实际大小 (100%)\t1 / Z", // Context_ActualSize
    L"适应屏幕\t0 / F", // Context_FitToScreen
    L"适应窗口", // Context_FitWindow
    L"填充窗口", // Context_FillWindow
    L"放大\t+ / Ctrl +", // Context_ZoomIn
    L"缩小\t- / Ctrl -", // Context_ZoomOut
    L"锁定窗口", // Context_LockWindow
    L"窗口置顶\tCtrl+T", // Context_AlwaysOnTop
    L"HUD 照片墙\tT", // Context_HUDGallery
    L"简略信息面板\tTab", // Context_LiteInfoPanel
    L"详细信息面板\tI", // Context_FullInfoPanel
    L"渲染 RAW", // Context_RenderRAW
    L"像素画模式", // Context_PixelArtMode
    L"色彩空间", // Context_ColorSpace
    L"全屏\tF11", // Context_Fullscreen
    L"视图", // Context_View
    L"填充", // Context_WallpaperFill
    L"适应", // Context_WallpaperFit
    L"平铺", // Context_WallpaperTile
    L"设为壁纸", // Context_SetAsWallpaper
    L"重命名\tF2", // Context_Rename
    L"修复扩展名", // Context_FixExtension
    L"删除\tDel", // Context_Delete
    L"撤销删除\tCtrl+Z", // Context_UndoDelete
    L"撤销重命名\tCtrl+Z", // Context_UndoRename
    L"撤销旋转/翻转\tCtrl+Z", // Context_UndoTransform
    L"撤销\tCtrl+Z", // Context_Undo
    L"排序方式", // Context_SortBy
    L"导航顺序", // Context_NavOrder
    L"升序", // Context_SortAscending
    L"降序", // Context_SortDescending
    L"设置...", // Context_Settings
    L"对比模式\tC", // Context_CompareMode
    L"临摹模式\tCtrl+Shift+O", // Context_OverlayMode
    L"在对比模式中打开", // Context_GalleryOpenCompare
    L"在新窗口中打开", // Context_GalleryOpenNewWindow
    L"退出\tMButton/Esc", // Context_Exit
    L"退出鼠标穿透", // Menu_ExitPassthrough
    L"鼠标穿透模式", // Dialog_PassthroughTitle
    L"鼠标操作将透传至底层窗口。\n只能通过全局快捷键 (Shift+Esc) 或任务栏菜单退出该模式。\n\n是否继续？", // Dialog_PassthroughContent
    L"鼠标穿透：开启 (Shift+Esc 退出)", // OSD_PassthroughOn
    L"鼠标穿透：关闭", // OSD_PassthroughOff
    L"临摹模式：开启", // OSD_OverlayModeOn
    L"临摹模式：关闭", // OSD_OverlayModeOff
    L"不透明度", // OSD_Opacity
    L"放大 (临摹)", // Toolbar_Tooltip_OverlayZoomIn
    L"缩小 (临摹)", // Toolbar_Tooltip_OverlayZoomOut
    L"增加不透明度", // Toolbar_Tooltip_OverlayAlphaUp
    L"减小不透明度", // Toolbar_Tooltip_OverlayAlphaDown
    L"进入鼠标穿透", // Toolbar_Tooltip_OverlayPassthroughOn
    L"退出鼠标穿透", // Toolbar_Tooltip_OverlayPassthroughOff
    L"退出临摹模式", // Toolbar_Tooltip_OverlayExit
    L"错误", // Message_SaveErrorTitle
    L"保存文件失败。文件是否被占用?", // Message_SaveErrorContent
    L"上一张 (方向键左)", // Toolbar_Tooltip_Prev
    L"下一张 (方向键右)", // Toolbar_Tooltip_Next
    L"向左旋转 (Shift+R)", // Toolbar_Tooltip_RotateL
    L"向右旋转 (R)", // Toolbar_Tooltip_RotateR
    L"水平翻转 (H)", // Toolbar_Tooltip_FlipH
    L"锁定窗口(临时)", // Toolbar_Tooltip_Lock
    L"解锁窗口", // Toolbar_Tooltip_Unlock
    L"缩略图 (T)", // Toolbar_Tooltip_Gallery
    L"信息面板", // Toolbar_Tooltip_Info
    L"RAW: 快速预览 (点击切换完整)", // Toolbar_Tooltip_RawPreview
    L"RAW: 完整解码 (点击切换预览)", // Toolbar_Tooltip_RawFull
    L"RAW: 查看配对 RAW (完整解码)", // Toolbar_Tooltip_RawPairView
    L"RAW: 返回配对图像", // Toolbar_Tooltip_RawPairBack
    L"扩展名不匹配 (修复)", // Toolbar_Tooltip_FixExtension
    L"固定工具栏", // Toolbar_Tooltip_Pin
    L"取消固定工具栏", // Toolbar_Tooltip_Unpin
    L"显示色彩溢出高亮区域", // Toolbar_Tooltip_GamutWarning
    L"普通模式", // Toolbar_Tooltip_NormalMode
    L"对比模式", // Toolbar_Tooltip_CompareMode
    L"单页模式", // Toolbar_Tooltip_SinglePage
    L"双页模式", // Toolbar_Tooltip_DualPage
    L"在选区打开新图", // Toolbar_Tooltip_CompareOpen
    L"交换左右", // Toolbar_Tooltip_CompareSwap
    L"切换布局", // Toolbar_Tooltip_CompareLayout
    L"对比信息", // Toolbar_Tooltip_CompareInfo
    L"删除所选图片", // Toolbar_Tooltip_CompareDelete
    L"放大 (微调)", // Toolbar_Tooltip_CompareZoomIn
    L"缩小 (微调)", // Toolbar_Tooltip_CompareZoomOut
    L"缩放同步: 开", // Toolbar_Tooltip_CompareSyncZoomOn
    L"缩放同步: 关", // Toolbar_Tooltip_CompareSyncZoomOff
    L"平移同步: 开", // Toolbar_Tooltip_CompareSyncPanOn
    L"平移同步: 关", // Toolbar_Tooltip_CompareSyncPanOff
    L"切换沉浸聚光灯", // Toolbar_Tooltip_SlideshowImmersiveToggle
    L"退出幻灯片模式", // Toolbar_Tooltip_SlideshowExit
    L"退出对比", // Toolbar_Tooltip_CompareExit
    L"播放动画", // Toolbar_Tooltip_AnimPlay
    L"暂停动画", // Toolbar_Tooltip_AnimPause
    L"上一帧", // Toolbar_Tooltip_AnimPrev
    L"下一帧", // Toolbar_Tooltip_AnimNext
    L"脏矩形调试: 开", // Toolbar_Tooltip_AnimDirtyOn
    L"脏矩形调试: 关", // Toolbar_Tooltip_AnimDirtyOff
    L"动画速度调节", // Toolbar_Tooltip_AnimSpeed
    L"性能", // Settings_Header_Performance
    L"专业工具", // Settings_Header_Professional
    L"智能内存管理", // Settings_Label_MemoryReclaim
    L"智能", // Settings_Option_MemSmart
    L"激进", // Settings_Option_MemAggressive
    L"按需", // Settings_Option_MemOnDemand
    L"智能 (推荐): 仅当系统可用内存 < 4GB 时，回收图片空闲内存。\n" L"激进: 永不回收，保持 2GB 内存独占以换取绝对 0ns 的看图切换极速。\n" L"按需: 图片切换时立刻回收空闲内存，保持最低物理内存占用。", // Settings_Tooltip_MemoryReclaim
    L"动画模式下显示重绘区域预览按钮", // Settings_Label_ShowDirtyRect
    L"在播放动画时显示用于调试重绘区域的工具按钮，以便可视化哪些部分正在更新。", // Settings_Tooltip_ShowDirtyRect
    L"已复制!", // OSD_Copied
    L"坐标已复制!", // OSD_CoordinatesCopied
    L"文件路径已复制!", // OSD_FilePathCopied
    L"缩放: 100%", // OSD_Zoom100
    L"缩放: 适应屏幕", // OSD_ZoomFit
    L"缩放: 适应窗口", // OSD_ZoomFitWindow
    L"缩放: 填充窗口", // OSD_ZoomFill
    L"打印: 请在打开的应用中使用 Ctrl+P", // OSD_PrintInstruction
    L"已移至回收站", // OSD_MovedToRecycleBin
    L"窗口已锁定", // OSD_WindowLocked
    L"窗口已解锁", // OSD_WindowUnlocked
    L"窗口置顶: 开", // OSD_AlwaysOnTopOn
    L"窗口置顶: 关", // OSD_AlwaysOnTopOff
    L"壁纸已设置", // OSD_WallpaperSet
    L"设置壁纸失败", // OSD_WallpaperFailed
    L"重命名成功", // OSD_Renamed
    L"重命名失败", // OSD_RenameFailed
    L"扩展名已修复", // OSD_ExtensionFixed
    L"已恢复原状", // OSD_Restored
    L"已是第一张", // OSD_FirstImage
    L"已是最后一张", // OSD_LastImage
    L"全高清", // OSD_HD
    L"缩放: ", // OSD_ZoomPrefix
    L"播放中", // OSD_AnimPlaying
    L"已暂停 (检查模式: Alt+左右方向键逐帧查看)", // OSD_AnimPaused
    L"脏矩形: 开", // OSD_AnimDirtyOn
    L"脏矩形: 关", // OSD_AnimDirtyOff
    L"跨越显示器 (电视墙模式)\tCtrl+F11", // Context_SpanDisplays
    L"界面", // Settings_Tab_Visuals
    L"操作", // Settings_Tab_Controls
    L"图像", // Settings_Tab_Image
    L"高级", // Settings_Tab_Advanced
    L"背景", // Settings_Header_Backdrop
    L"窗口", // Settings_Header_Window
    L"锁定窗口时", // Settings_Header_WindowLock
    L"面板", // Settings_Header_Panel
    L"鼠标", // Settings_Header_Mouse
    L"边缘", // Settings_Header_Edge
    L"渲染", // Settings_Header_Render
    L"高动态范围 (HDR)", // Settings_Header_Hdr
    L"提示", // Settings_Header_Prompts
    L"系统", // Settings_Header_System
    L"功能", // Settings_Header_Features
    L"透明度", // Settings_Header_Transparency
    L"玻璃引擎 (GPU加速)", // Settings_Header_GeekGlass
    L"启用玻璃渲染", // Settings_Label_EnableGeekGlass
    L"交互动画", // Settings_Label_GlassUIAnimations
    L"核心材质", // Settings_Header_CoreMaterial
    L"模糊半径", // Settings_Label_BlurSigma
    L"启用玻璃渲染后生效", // Settings_Status_GlassDisabled
    L"厚度 (浓度)", // Settings_Label_TintDensity
    L"控制玻璃底色深度。", // Settings_Tooltip_TintDensity
    L"光泽亮度", // Settings_Label_SpecularOpacity
    L"控制玻璃面板光泽强度。", // Settings_Tooltip_SpecularOpacity
    L"阴影强度", // Settings_Label_ShadowIntensity
    L"调节 GPU 加速物理落影的深浅。", // Settings_Tooltip_ShadowIntensity
    L"边缘", // Settings_Header_VectorAssets
    L"线框粗细", // Settings_Label_VectorStrokeWeight
    L"标准 (1.5px)", // Settings_Option_StrokeStandard
    L"极细 (1.0px)", // Settings_Option_StrokeFine
    L"玻璃着色", // Settings_Header_GlassTint
    L"着色模式", // Settings_Label_TintProfile
    L"自动", // Settings_Option_TintAuto
    L"自定义", // Settings_Option_TintCustom
    L"自定义底色", // Settings_Label_GlassCustomColor
    L"材质浓度", // Settings_Header_DensityMatrix
    L"信息提示 (OSD)", // Settings_Label_OsdDensity
    L"控制加载进度条、播放提示、缩放信息等浮层的厚度。", // Settings_Tooltip_OsdDensity
    L"工具面板", // Settings_Label_PanelsDensity
    L"控制底部工具栏、信息面板、画廊胶卷以及右上角窗口控制按钮的厚度。", // Settings_Tooltip_PanelsDensity
    L"模态窗口", // Settings_Label_ModalsDensity
    L"控制设置面板、帮助面板和对话框等弹出窗口的厚度。", // Settings_Tooltip_ModalsDensity
    L"右键菜单", // Settings_Label_MenusDensity
    L"控制右键菜单、下拉列表的厚度。", // Settings_Tooltip_MenusDensity
    L"主题", // Settings_Tab_Theme
    L"主题模式", // Settings_Label_ThemeMode
    L"自动", // Settings_Option_ThemeAuto
    L"深色", // Settings_Option_ThemeDark
    L"浅色", // Settings_Option_ThemeLight
    L"自定义", // Settings_Option_ThemeCustom
    L"环境遮罩", // Settings_Label_AmbientDimmer
    L"在画廊模式、设置窗口或对话框开启时，在后端添加沉浸式阴影遮罩。", // Settings_Tooltip_AmbientDimmer
    L"强调色", // Settings_Label_AccentColor
    L"文本颜色", // Settings_Label_TextColor
    L"导入导出 (.qvtheme)", // Settings_Header_ThemeManagement
    L"导出", // Settings_Action_ExportTheme
    L"导入", // Settings_Action_ImportTheme
    L"画布颜色", // Settings_Label_CanvasColor
    L"叠加层", // Settings_Label_Overlay
    L"显示网格", // Settings_Label_ShowGrid
    L"图片切换淡入淡出", // Settings_Label_CrossFade
    L"窗口置顶", // Settings_Label_AlwaysOnTop
    L"锁定窗口", // Settings_Label_LockWindow
    L"此处控制程序启动时默认锁定窗口边框，不跟随图片缩放。", // Settings_Tooltip_LockWindow
    L"自动隐藏标题栏", // Settings_Label_AutoHideTitle
    L"圆角窗口", // Settings_Label_RoundedCorners
    L"显示组件边框", // Settings_Label_UIBorders
    L"控制主窗口及右键菜单圆角，仅支持Windows 11。", // Settings_Tooltip_RoundedCorners
    L"锁定底部工具栏", // Settings_Label_LockToolbar
    L"默认最小窗口宽度", // Settings_Label_WindowMinSize
    L"默认最大启动尺寸 (%)", // Settings_Label_WindowMaxSizePercent
    L"显示边界指示器", // Settings_Label_ShowBorderIndicator
    L"在图片超出窗口边框的方向上显示蓝色提示线条。", // Settings_Tooltip_ShowBorderIndicator
    L"显示缩略导航图", // Settings_Label_ShowNavigator
    L"自动", // Settings_Option_NavigatorAuto
    L"开启", // Settings_Option_NavigatorOn
    L"关闭", // Settings_Option_NavigatorOff
    L"导航时保持窗口尺寸不变", // Settings_Label_KeepWindowSizeOnNav
    L"记住最后窗口位置和尺寸", // Settings_Label_RememberLastWindowSizeAndPosition
    L"小于窗口尺寸图片适应窗口", // Settings_Label_UpscaleSmallImagesWhenLocked
    L"启用窗口平滑缩放 (GPU)", // Settings_Label_EnableSmoothScaling
    L"EXIF 面板模式", // Settings_Label_ExifMode
    L"工具栏信息默认值", // Settings_Label_ToolbarInfoDefault
    L"打开时全屏", // Settings_Label_OpenFullScreenMode
    L"全屏时缩放模式", // Settings_Label_FullScreenZoomMode
    L"适应屏幕", // Settings_Option_FitScreen
    L"自动", // Settings_Option_AutoFit
    L"反转滚轮", // Settings_Label_InvertWheel
    L"缩放 100% 吸附阻尼", // Settings_Label_ZoomSnapDamping
    L"窗口缩放以鼠标为中线", // Settings_Label_MouseAnchorZoom
    L"右键拖动缩放", // Settings_Label_RightButtonDragZoom
    L"滚轮缩放速度", // Settings_Label_WheelZoomSpeed
    L"拇指滚轮", // Settings_Label_ThumbWheel
    L"右键拖拽缩放速度", // Settings_Label_RightDragZoomSpeed
    L"缩放速度(临时): ", // OSD_WheelZoomSpeed
    L"临时调节缩放速度", // Help_Action_AdjustZoomSpeed
    L"临时锁定窗口缩放", // Help_Action_LockWindowZoom
    L"反转侧键", // Settings_Label_InvertButtons
    L"使用固定的缩放级别", // Settings_Label_UseFixedZoom
    L"启用时，Alt + 滚轮将执行普通缩放而非修改缩放速率。", // Settings_Tooltip_UseFixedZoom
    L"  └  自定义缩放级别", // Settings_Label_FixedZoomLevels
    L"编辑固定缩放级别", // Dialog_FixedZoomTitle
    L"输入逗号分隔的缩放级别 (例如 0.5, 1, 2):", // Dialog_FixedZoomMsg
    L"放大插值算法", // Settings_Label_ZoomModeIn
    L"缩小插值算法", // Settings_Label_ZoomModeOut
    L"左键拖动", // Settings_Label_LeftDrag
    L"中键拖动", // Settings_Label_MiddleDrag
    L"中键点击", // Settings_Label_MiddleClick
    L"边缘点击翻页", // Settings_Label_EdgeNavClick
    L"在对比模式下禁用", // Settings_Label_DisableEdgeNavInCompare
    L"翻页指示器", // Settings_Label_NavIndicator
    L"自动旋转 (EXIF)", // Settings_Label_AutoRotate
    L"色彩管理 (CMS)", // Settings_Label_CMS
    L"高级色彩与 HDR (scRGB)", // Settings_Label_AdvancedColor
    L"HDR 色调映射", // Settings_Label_HdrToneMapping
    L"Spline 拐点", // Settings_Label_HdrSplineKnee
    L"HDR 降级策略 (Tone Mapping)：\n当 HDR 图片超出显示器极限时的映射方式。\n" L"Spline 样条映射：采用分段样条曲线，实现高保真的高光细节还原（推荐）。\n" L"色度模式：保持严格亮度映射，超出显示器极限的亮度将被直接裁剪。\n" L"BT.2390 EETF：ITU-R BT.2390 标准高保真降级曲线。", // Settings_Tooltip_HdrToneMapping
    L"0 = 自动模式（根据图片亮度自动计算）。\n数值表示显示器峰值亮度的比例。该拐点以下的亮度将进行 1:1 绝对映射，拐点以上的亮度将使用平滑曲线进行压缩映射。\n（推荐值：0.4 - 0.75）", // Settings_Tooltip_HdrSplineKnee
    L"HDR 峰值亮度 (Nits)", // Settings_Label_HdrPeakNitsOverride
    L"设为 0 表示通过系统自动检测亮度.", // Settings_Tooltip_HdrPeakNitsOverride
    L"HDR 峰值百分位", // Settings_Label_HdrPeakPercentile
    L"丢弃极少数过亮的像素以提升整体亮度 (mpv 默认: 99.995%).", // Settings_Tooltip_HdrPeakPercentile
    L"100% (绝对峰值)", // Settings_Option_HdrPeakPercentile_100
    L"99.995% (稳定)", // Settings_Option_HdrPeakPercentile_99995
    L"99.9% (激进)", // Settings_Option_HdrPeakPercentile_999
    L"高光去饱和范围", // Settings_Label_HdrDesatThreshold
    L"高光去饱和开始介入的亮度阈值。0.0 表示对所有亮度进行去饱和，1.0 表示完全不进行去饱和。推荐默认值 0.18。", // Settings_Tooltip_HdrDesatThreshold
    L"高光去饱和强度", // Settings_Label_HdrMaxDesat
    L"高光去饱和的最大强度。0.0 表示不进行去饱和，1.0 表示在高光处完全去饱和为白色。推荐默认值 0.75。", // Settings_Tooltip_HdrMaxDesat
    L"色度", // Settings_Option_HdrColorimetric
    L"样条映射", // Settings_Option_HdrSpline
    L"BT.2390 (EETF)", // Settings_Option_HdrLegacyReinhard
    L"无配置图片的默认回退", // Settings_Label_CmsFallback
    L"自定义软打样配置 (.icc)", // Settings_Label_CustomProof
    L"软打样预览", // Context_SoftProofing
    L"打样配置文件", // Context_SoftProofProfile
    L"自定义...", // Context_SoftProofCustom
    L"敬请期待", // Settings_Value_ComingSoon
    L"强制 RAW 解码", // Settings_Label_ForceRaw
    L"RAW+JPEG 配对", // Settings_Label_PairRawJpeg
    L"将 RAW 与同名的相机直出图 (JPEG/HEIF) 显示为同一张照片。\n列表中仅保留直出图，并标注 RAW 格式（如 +CR3）；后台会校验两者拍摄时间，不一致的配对自动拆开。\n可用 RAW 工具栏按钮或其热键切换相机直出和本机渲染，也可用配对对比热键并排查看。", // Settings_Tooltip_PairRawJpeg
    L"曝光度 (亮度)", // Settings_Label_Exposure
    L"调节图像亮度 (曝光补偿)。范围: 0.18x 至 10.0x。", // Settings_Tooltip_Exposure
    L"添加到打开方式", // Settings_Label_AddToOpenWith
    L"自定义图片编辑器", // Settings_Label_CustomEditor
    L"选择编辑器", // Context_SelectEditor
    L"编辑器启动失败，请重新配置。", // OSD_EditorLaunchFailed
    L"添加", // Settings_Action_Add
    L"已添加", // Settings_Action_Added
    L"便携模式下停用", // Settings_Status_DisabledInPortable
    L"启用调试 HUD (F12)", // Settings_Label_DebugHUD
    L"预加载系统", // Settings_Label_Prefetch
    L"信息面板", // Settings_Label_InfoPanelAlpha
    L"工具栏", // Settings_Label_ToolbarAlpha
    L"设置菜单", // Settings_Label_SettingsAlpha
    L"重置所有设置", // Settings_Label_Reset
    L"恢复", // Settings_Action_Restore
    L"完成", // Settings_Action_Done
    L"无管理", // Settings_Option_CmsUnmanaged
    L"sRGB", // Settings_Option_CmssRGB
    L"Display P3", // Settings_Option_CmsP3
    L"Adobe RGB (1998)", // Settings_Option_CmsAdobeRGB
    L"灰度模式 (影调检查)", // Settings_Option_CmsGray
    L"ProPhoto RGB", // Settings_Option_CmsProPhoto
    L"渲染意图", // Settings_Label_CmsIntent
    L"色彩溢出检测", // Settings_Label_GamutWarning
    L"分析并标出溢出区域：关闭、仅在软打样模式下检测（默认）、或者同时检测软打样和屏幕色彩溢出。", // Settings_Tooltip_GamutWarning
    L"自动提示色彩溢出", // Settings_Label_GamutAutoPrompt
    L"检测到溢出后弹出 OSD 提示。高亮区域可通过工具栏按钮手动查看。", // Settings_Tooltip_GamutAutoPrompt
    L"溢出高亮颜色", // Settings_Label_GamutColor
    L"相对色度", // Settings_Option_CmsIntentRelative
    L"感知意图", // Settings_Option_CmsIntentPerceptual
    L"启用色彩管理 (Color Management System)。\n开启后，将通过 GPU 进行高精度色彩空间转换以还原真实色彩。\n关闭可降低性能消耗，但在广色域屏幕上可能导致颜色过饱和。", // Settings_Tooltip_CMS
    L"色彩空间转换策略 (Rendering Intent)。\n感知模式 (Perceptual)：压缩超出色域的颜色，保留细节和渐变，适合照片。\n相对比色 (Relative Colorimetric)：保留在色域内的颜色，超出部分裁剪，适合 UI 和图标。\n注意：仅当图像或显示器使用包含 LUT（查找表）的高级 ICC 配置文件时，切换此选项才会有视觉差异。普通矩阵型配置文件会自动回退至相对色度。", // Settings_Tooltip_CmsIntent
    L"启用 16-bit 浮点渲染管线 (scRGB)。\n开启后，在支持 HDR 的显示器上能突破 SDR 亮度限制，完美呈现照片高光。\n关闭将强制降级映射至 SDR 输出。\n注意：开启会增加显存占用。", // Settings_Tooltip_AdvancedColor
    L"自动：图片小于屏幕尺寸时 100% 缩放，图片大于屏幕尺寸时适应屏幕尺寸缩放。", // Settings_Tooltip_ZoomAuto
    L"检查更新", // Settings_Action_CheckUpdates
    L"查看更新", // Settings_Action_ViewUpdate
    L"检查中...", // Settings_Status_Checking
    L"已是最新", // Settings_Status_UpToDate
    L"GitHub 仓库", // Settings_Link_GitHub
    L"反馈问题", // Settings_Link_ReportIssue
    L"帮助 (F1)", // Settings_Link_Hotkeys
    L"版本", // Settings_Label_Version
    L"构建", // Settings_Label_Build
    L"黑色", // Settings_Option_Black
    L"白色", // Settings_Option_White
    L"网格", // Settings_Option_Grid
    L"自定义", // Settings_Option_Custom
    L"关闭", // Settings_Option_Off
    L"开启", // Settings_Option_On
    L"简略", // Settings_Option_Lite
    L"详细", // Settings_Option_Full
    L"仅大图", // Settings_Option_LargeOnly
    L"全部", // Settings_Option_All
    L"软打样", // Settings_Option_SoftProofing
    L"窗口", // Settings_Option_Window
    L"平移", // Settings_Option_Pan
    L"无", // Settings_Option_None
    L"退出", // Settings_Option_Exit
    L"箭头", // Settings_Option_Arrow
    L"光标", // Settings_Option_Cursor
    L"手动", // Settings_Option_Manual
    L"自动 (资源管理器)", // Settings_Option_SortAuto
    L"文件名", // Settings_Option_SortName
    L"修改时间", // Settings_Option_SortModified
    L"拍摄时间 (EXIF)", // Settings_Option_SortDateTaken
    L"大小", // Settings_Option_SortSize
    L"类型", // Settings_Option_SortType
    L"循环播放", // Settings_Option_NavLoop
    L"穿透子文件夹", // Settings_Option_NavThrough
    L"线性 (Linear)：基础平滑", // Settings_Option_Linear
    L"最近邻 (Nearest)：极端锐利", // Settings_Option_Nearest
    L"高质量双三次 (HQ Cubic)：极端平滑", // Settings_Option_HighQualityCubic
    L"自动", // Settings_Option_ZoomAuto
    L"自动", // Settings_Option_Auto
    L"节能", // Settings_Option_Eco
    L"平衡", // Settings_Option_Balanced
    L"极速", // Settings_Option_Ultra
    L"键盘快捷键", // Help_Header_Shortcuts
    L"鼠标操作", // Help_Header_Mouse
    L"切换图片", // Help_Item_NextPrev
    L"缩放", // Help_Item_Zoom
    L"平移图片", // Help_Item_Pan
    L"旋转", // Help_Item_Rotate
    L"适应屏幕", // Help_Item_Fit
    L"删除图片", // Help_Item_Delete
    L"全屏", // Help_Item_Fullscreen
    L"关闭", // Help_Item_Close
    L"左键", // Help_Mouse_Left
    L"中键", // Help_Mouse_Middle
    L"滚轮", // Help_Mouse_Wheel
    L"右键", // Help_Mouse_Right
    L"右键上下拖动", // Help_Mouse_RightVerticalDrag
    L"平移窗口/退出全屏/退出最大化", // Help_Action_MoveWindow
    L"平移图片", // Help_Action_PanImage
    L"上下文菜单", // Help_Action_ContextMenu
    L"切换图片", // Help_Action_NextPrev
    L"缩放", // Help_Action_Zoom
    L"智能缩放 (100% / 适应窗口 / 恢复)", // Help_Action_SmartZoom
    L"复制图像", // Help_Desc_Copy
    L"编辑", // Help_Desc_Edit
    L"提示与术语", // Help_Header_Tips
    L"注意：快捷键仅对当前窗口生效，设置选项为全局永久生效。", // Help_Tip_ContextScope
    L"旋转说明：'边缘适配' 指为匹配编码块边界进行的微量裁剪(无损)；'有损' " L"指必须要进行重编码。", // Help_Tip_Rotation
    L"跨屏模式 " L"(Ctrl+F11)：合并所有显示器。若关闭按钮不可见(如L型布局)" L"，双击任意处即可退出。", // Help_Tip_VideoWall
    L"临摹模式/薄膜模式：开启后图像将变为半透明并露出底部元素，此时您可调整尺寸或透明度。" L"点击工具栏中的鼠标穿透模式开关，可进入鼠标穿透模式，此时除了退出快捷键 Shift+Esc 外的任何按键和鼠标输入均被忽略，" L"QuickView 将变为一层透明薄膜覆盖层。", // Help_Tip_DesignerMode
    L"色彩溢出检测：检测当前图片是否超出显示器或软打样配置的色域。模式：关闭、仅软打样（默认）、全部（包含屏幕）。可从工具栏手动开关。", // Help_Tip_GamutDetection
    L"RAW 渲染：默认显示内嵌预览图以提升速度。点击 RAW " L"按钮可进行完整解码(色彩可能不同)。", // Help_Tip_Raw
    L"JPEG 质量：基于算法估算的质量值 (例如 Photoshop 100% ≈ 98%)。因" L"编码器差异，测算结果可能与保存时的设置略有偏差，属正常现象。", // Help_Tip_JpegQ
    L"软打样对比：软打样后进入对比模式，将自动进行打样前后对比。", // Help_Tip_SoftProofCompare
    L"发现新版本！", // Dialog_UpdateTitle
    L"v%s 已准备就緒", // Dialog_UpdateContent
    L"更新日志", // Dialog_UpdateLogHeader
    L"立即更新", // Dialog_ButtonUpdate
    L"稍后", // Dialog_ButtonLater
    L"在 GitHub 点星", // Dialog_ButtonStar
    L"QuickView 源自热爱", // Dialog_Update_LoveTitle
    L"我利用业余时间维护 QuickView，只因我相信 Windows " L"值得拥有一个更快、更纯粹的看图工具。 " L"我没有推广预算，也没有团队。如果您喜欢这次更新，在 GitHub " L"上点一颗星或分享给朋友，就是对我最大的支持。", // Dialog_Update_LoveMessage
    L"对比模式", // Help_Item_Compare
    L"第一张 / 最后一张图片", // Help_Item_FirstLast
    L"物理属性", // HUD_Group_Physical
    L"科学指标", // HUD_Group_Scientific
    L"光学与编码", // HUD_Group_Encoding
    L"边缘清晰度 (拉普拉斯方差)", // HUD_Tip_Sharp_Desc
    L"边缘锐利，细节丰富", // HUD_Tip_Sharp_High
    L"软焦点或运动模糊", // HUD_Tip_Sharp_Low
    L"> 500 为非常锐利", // HUD_Tip_Sharp_Ref
    L"信息密度 (香农熵)", // HUD_Tip_Ent_Desc
    L"复杂纹理或高噪点", // HUD_Tip_Ent_High
    L"平坦区域或低细节", // HUD_Tip_Ent_Low
    L"7.0-8.0 为高细节", // HUD_Tip_Ent_Ref
    L"每像素比特数 (压缩效率)", // HUD_Tip_BPP_Desc
    L"低压缩率 (保留更多原始数据)", // HUD_Tip_BPP_High
    L"高压缩率 (空间利用率更高)", // HUD_Tip_BPP_Low
    L"24.0 (原始RGB), ~2.0-3.0 (高质量JPEG), ~0.5-1.5 (WebP/AVIF)", // HUD_Tip_BPP_Ref
    L"高: ", // HUD_Label_High
    L"低: ", // HUD_Label_Low
    L"参考: ", // HUD_Label_Ref
    L"图库胶片带 (顶部悬浮)", // Settings_Header_GalleryTrigger
    L"触发模式", // Settings_Label_GalleryTriggerMode
    L"自动悬停", // Settings_Option_GalleryTriggerAuto
    L"热点停留", // Settings_Option_GalleryTriggerDelay
    L"点击热点", // Settings_Option_GalleryTriggerClick
    L"停用", // Settings_Option_GalleryTriggerDisable
    L"该功能在窗口小于 600x450 时将自动禁用。", // Settings_Tooltip_GalleryTrigger

    L"开始播放幻灯片", // OSD_SlideshowStarted
    L"停止播放幻灯片", // OSD_SlideshowStopped
    L"继续播放幻灯片", // OSD_SlideshowResumed
    L"幻灯片已暂停", // OSD_SlideshowPaused
    L"沉浸模式: 聚光灯", // OSD_ImmersiveSpotlight
    L"沉浸模式: 普通", // OSD_ImmersiveNormal
    L"打印任务已发送，您可以继续浏览。", // OSD_PrintJobStarted
    L"打印任务已发送完毕。", // OSD_PrintJobFinished
    L"幻灯片模式", // Context_SlideshowMode
    L"切换间隔 (秒)", // Settings_Label_SlideshowInterval
    L"沉浸模式", // Settings_Label_SlideshowImmersive
    L"自定义精简信息栏", // Settings_Label_CustomLiteInfoPanel
    L"普通模式显示项", // Settings_Label_ItemsInNormalMode
    L"对比模式显示项", // Settings_Label_ItemsInCompareMode
    L"分隔符预设", // Settings_Label_SeparatorPreset
    L"普通", // Settings_Option_SlideshowNormal
    L"聚光灯", // Settings_Option_SlideshowSpotlight
    L"放大镜形状", // Settings_Label_LoupeShape
    L"正方形", // Settings_Option_LoupeShapeSquare
    L"圆形", // Settings_Option_LoupeShapeCircle
    L"按住快捷键并滚动鼠标滚轮可调节放大镜尺寸。", // Settings_Tooltip_LoupeHotkey
};

// ----------------------------------------------------------------
// TW Table
// ----------------------------------------------------------------
static const LanguageTable Table_TW = {
    L"沒有載入圖片", // OSD_NoImage
    L"無損", // OSD_Lossless
    L"重新編碼 (無損)", // OSD_ReencodedLossless
    L"剪裁", // OSD_EdgeAdapted
    L"有損", // OSD_Reencoded
    L"拒絕存取 - 檔案可能被佔用或唯讀", // OSD_ReadOnly
    L"變換不完美 (已進行邊緣優化)", // OSD_NotPerfect
    L"跨屏模式: 開啟", // OSD_SpanOn
    L"跨屏模式: 關閉", // OSD_SpanOff
    L"打樣前 (Original)", // OSD_CompareBefore
    L"打樣後 (Proofed)", // OSD_CompareAfter
    L"已撤銷刪除", // OSD_UndoDeleteSuccess
    L"撤銷刪除失敗", // OSD_UndoDeleteFailed
    L"已撤銷重命名", // OSD_UndoRenameSuccess
    L"撤銷重命名失敗", // OSD_UndoRenameFailed
    L"已撤銷旋轉/翻轉", // OSD_UndoTransformSuccess
    L"撤銷旋轉/翻轉失敗", // OSD_UndoTransformFailed
    L"檢測到色彩溢出", // OSD_GamutDetected
    L"色彩空間不匹配 / 設定檔解析失敗", // OSD_GamutIncompatible
    L"色彩溢出分析失敗", // OSD_GamutFailed
    L"順時針旋轉 90\x00B0", // Action_RotateCW
    L"逆時針旋轉 90\x00B0", // Action_RotateCCW
    L"旋轉 180\x00B0", // Action_Rotate180
    L"水平翻轉", // Action_FlipH
    L"垂直翻轉", // Action_FlipV
    L"儲存變更?", // Dialog_SaveTitle
    L"圖像已被修改。是否儲存變更?", // Dialog_SaveContent
    L"儲存", // Dialog_ButtonSave
    L"另存為...", // Dialog_ButtonSaveAs
    L"放棄", // Dialog_ButtonDiscard
    L"繼續", // Dialog_ButtonContinue
    L"總是儲存無損變換", // Checkbox_AlwaysSaveLossless
    L"總是儲存邊緣優化結果", // Checkbox_AlwaysSaveEdgeAdapted
    L"總是儲存重新編碼結果", // Checkbox_AlwaysSaveLossy
    L"直接放入回收站，不再確認", // Checkbox_NeverConfirmDelete
    L"無法解碼 HEIC - 請安裝 HEVC 視訊延伸模組", // OSD_HEICCodecMissing
    L"無法解碼 HEIC", // Dialog_HEICTitle
    L"系統缺少 HEVC 視訊延伸模組。\\nQuickView " L"需要系統硬體加速以獲得最佳效能。", // Dialog_HEICContent
    L"取得延伸模組 (免費)", // Dialog_HEICGetExtension
    L"取消", // Dialog_Cancel
    L"一般", // Settings_Tab_General
    L"關於", // Settings_Tab_About
    L"基礎", // Settings_Group_Foundation
    L"啟動", // Settings_Group_Startup
    L"習慣", // Settings_Group_Habits
    L"語言", // Settings_Label_Language
    L"單一實例模式", // Settings_Label_SingleInstance
    L"檢查更新", // Settings_Label_CheckUpdates
    L"更新通道", // Settings_Label_UpdateChannel
    L"正式版", // Settings_Option_UpdateStable
    L"預覽版 (Pre-release)", // Settings_Option_UpdatePreRelease
    L"正式版：預覽版發布一段時間後未收到Bug報告，將發布正式版。\n預覽版：第一時間發布最新的代碼修改，用於收集您的測試反饋。注意，這些修改並未經過嚴格測試。", // Settings_Tooltip_PreRelease
    L"循環導覽", // Settings_Label_NavLoopMode
    L"排序方式", // Settings_Label_SortOrder
    L"降冪", // Settings_Label_SortDescending
    L"始終以檔名升序瀏覽封存檔案", // Settings_Label_SortArchivesByNameAscending
    L"刪除前確認", // Settings_Label_ConfirmDel
    L"可攜式模式 / 清理", // Settings_Label_Portable
    L"可攜式模式與登錄檔清理：\n開啟後，QuickView " L"將以可攜式方式執行。程式将自動清理已有的登錄檔關聯，並禁用自動登錄檔修改功" L"能。同時，設定檔將存放在程式所在目錄而非 AppData 目錄。", // Settings_Tooltip_Portable
    L"跨屏模式 (電視牆)", // Settings_Label_SpanDisplays
    L"介面縮放", // Settings_Label_UIScale
    L"需要重新啟動", // Settings_Status_RestartRequired
    L"無寫入權限!", // Settings_Status_NoWritePerm
    L"已啟用", // Settings_Status_Enabled
    L"驅動技術", // Settings_Header_PoweredBy
    L"開啟...\tCtrl+O", // Context_Open
    L"開啟方式...", // Context_OpenWith
    L"編輯\tE", // Context_Edit
    L"在檔案總管中顯示", // Context_ShowInExplorer
    L"開啟資料夾", // Context_OpenFolder
    L"複製圖像\tCtrl+C", // Context_CopyImage
    L"複製路徑\tCtrl+Alt+C", // Context_CopyPath
    L"列印\tCtrl+P", // Context_Print
    L"順時針旋轉 90\x00B0\tR", // Context_RotateCW
    L"逆時針旋轉 90\x00B0\tShift+R", // Context_RotateCCW
    L"水平翻轉\tH", // Context_FlipH
    L"垂直翻轉\tV", // Context_FlipV
    L"變換", // Context_Transform
    L"實際大小 (100%)\t1 / Z", // Context_ActualSize
    L"適應螢幕\t0 / F", // Context_FitToScreen
    L"適應視窗", // Context_FitWindow
    L"填滿視窗", // Context_FillWindow
    L"放大\t+ / Ctrl +", // Context_ZoomIn
    L"縮小\t- / Ctrl -", // Context_ZoomOut
    L"鎖定視窗", // Context_LockWindow
    L"視窗置頂\tCtrl+T", // Context_AlwaysOnTop
    L"HUD 照片牆\tT", // Context_HUDGallery
    L"簡略資訊面板\tTab", // Context_LiteInfoPanel
    L"詳細資訊面板\tI", // Context_FullInfoPanel
    L"渲染 RAW", // Context_RenderRAW
    L"像素畫模式", // Context_PixelArtMode
    L"色彩空間", // Context_ColorSpace
    L"全螢幕\tF11", // Context_Fullscreen
    L"檢視", // Context_View
    L"填滿", // Context_WallpaperFill
    L"適應", // Context_WallpaperFit
    L"並排", // Context_WallpaperTile
    L"設為桌布", // Context_SetAsWallpaper
    L"重新命名\tF2", // Context_Rename
    L"修復副檔名", // Context_FixExtension
    L"刪除\tDel", // Context_Delete
    L"撤銷刪除\tCtrl+Z", // Context_UndoDelete
    L"撤銷重命名\tCtrl+Z", // Context_UndoRename
    L"撤銷旋轉/翻轉\tCtrl+Z", // Context_UndoTransform
    L"撤銷\tCtrl+Z", // Context_Undo
    L"排序方式", // Context_SortBy
    L"導覽順序", // Context_NavOrder
    L"升冪", // Context_SortAscending
    L"降冪", // Context_SortDescending
    L"設定...", // Context_Settings
    L"對比模式\tC", // Context_CompareMode
    L"臨摹模式\tCtrl+Shift+O", // Context_OverlayMode
    L"在對比模式中打開", // Context_GalleryOpenCompare
    L"在新視窗中打開", // Context_GalleryOpenNewWindow
    L"結束\tMButton/Esc", // Context_Exit
    L"退出滑鼠穿透", // Menu_ExitPassthrough
    L"滑鼠穿透模式", // Dialog_PassthroughTitle
    L"滑鼠操作將透傳至底層窗口。\n只能通過全局快捷鍵 (Shift+Esc) 或任務欄菜單退出該模式。\n\n是否繼續？", // Dialog_PassthroughContent
    L"滑鼠穿透：開啟 (Shift+Esc 退出)", // OSD_PassthroughOn
    L"滑鼠穿透：關閉", // OSD_PassthroughOff
    L"臨摹模式：開啟", // OSD_OverlayModeOn
    L"臨摹模式：關閉", // OSD_OverlayModeOff
    L"不透明度", // OSD_Opacity
    L"放大 (臨摹)", // Toolbar_Tooltip_OverlayZoomIn
    L"縮小 (臨摹)", // Toolbar_Tooltip_OverlayZoomOut
    L"增加不透明度", // Toolbar_Tooltip_OverlayAlphaUp
    L"減小不透明度", // Toolbar_Tooltip_OverlayAlphaDown
    L"進入滑鼠穿透", // Toolbar_Tooltip_OverlayPassthroughOn
    L"退出滑鼠穿透", // Toolbar_Tooltip_OverlayPassthroughOff
    L"退出臨摹模式", // Toolbar_Tooltip_OverlayExit
    L"錯誤", // Message_SaveErrorTitle
    L"儲存檔案失敗。檔案是否被佔用?", // Message_SaveErrorContent
    L"上一張 (方向鍵左)", // Toolbar_Tooltip_Prev
    L"下一張 (方向鍵右)", // Toolbar_Tooltip_Next
    L"向左旋轉 (Shift+R)", // Toolbar_Tooltip_RotateL
    L"向右旋轉 (R)", // Toolbar_Tooltip_RotateR
    L"水平翻轉 (H)", // Toolbar_Tooltip_FlipH
    L"鎖定視窗(臨時)", // Toolbar_Tooltip_Lock
    L"解鎖視窗", // Toolbar_Tooltip_Unlock
    L"縮圖 (T)", // Toolbar_Tooltip_Gallery
    L"資訊面板", // Toolbar_Tooltip_Info
    L"RAW: 快速預覽 (點選切換完整)", // Toolbar_Tooltip_RawPreview
    L"RAW: 完整解碼 (點選切換預覽)", // Toolbar_Tooltip_RawFull
    L"RAW: 檢視配對 RAW (完整解碼)", // Toolbar_Tooltip_RawPairView
    L"RAW: 返回配對圖像", // Toolbar_Tooltip_RawPairBack
    L"副檔名不符 (修復)", // Toolbar_Tooltip_FixExtension
    L"固定工具列", // Toolbar_Tooltip_Pin
    L"取消固定工具列", // Toolbar_Tooltip_Unpin
    L"顯示色彩溢出高亮區域", // Toolbar_Tooltip_GamutWarning
    L"普通模式", // Toolbar_Tooltip_NormalMode
    L"對比模式", // Toolbar_Tooltip_CompareMode
    L"單頁模式", // Toolbar_Tooltip_SinglePage
    L"雙頁模式", // Toolbar_Tooltip_DualPage
    L"在選區開啟新圖", // Toolbar_Tooltip_CompareOpen
    L"交換左右", // Toolbar_Tooltip_CompareSwap
    L"切換佈局", // Toolbar_Tooltip_CompareLayout
    L"對比資訊", // Toolbar_Tooltip_CompareInfo
    L"刪除所選圖片", // Toolbar_Tooltip_CompareDelete
    L"放大 (微調)", // Toolbar_Tooltip_CompareZoomIn
    L"縮小 (微調)", // Toolbar_Tooltip_CompareZoomOut
    L"縮放同步: 開", // Toolbar_Tooltip_CompareSyncZoomOn
    L"縮放同步: 關", // Toolbar_Tooltip_CompareSyncZoomOff
    L"平移同步: 開", // Toolbar_Tooltip_CompareSyncPanOn
    L"平移同步: 關", // Toolbar_Tooltip_CompareSyncPanOff
    L"切換沉浸聚光燈", // Toolbar_Tooltip_SlideshowImmersiveToggle
    L"退出幻燈片模式", // Toolbar_Tooltip_SlideshowExit
    L"退出對比", // Toolbar_Tooltip_CompareExit
    L"播放動畫", // Toolbar_Tooltip_AnimPlay
    L"暫停動畫", // Toolbar_Tooltip_AnimPause
    L"上一幀", // Toolbar_Tooltip_AnimPrev
    L"下一幀", // Toolbar_Tooltip_AnimNext
    L"髒矩形偵錯: 開", // Toolbar_Tooltip_AnimDirtyOn
    L"髒矩形偵錯: 關", // Toolbar_Tooltip_AnimDirtyOff
    L"動畫速度調節", // Toolbar_Tooltip_AnimSpeed
    L"效能", // Settings_Header_Performance
    L"專業工具", // Settings_Header_Professional
    L"Memory Reclaim Strategy:", // Settings_Label_MemoryReclaim
    L"Smart (Auto)", // Settings_Option_MemSmart
    L"Aggressive (Max Perf)", // Settings_Option_MemAggressive
    L"On-Demand (Min RAM)", // Settings_Option_MemOnDemand
    L"Smart: Balance performance and RAM.\nAggressive: Maximize performance, high memory usage.\nOn-Demand: Release memory immediately when idle.", // Settings_Tooltip_MemoryReclaim
    L"動畫模式下顯示髒矩形按鈕", // Settings_Label_ShowDirtyRect
    L"在動畫模式工具欄顯示髒矩形調試按鈕，用於觀察局部刷新區域。", // Settings_Tooltip_ShowDirtyRect
    L"已複製!", // OSD_Copied
    L"座標已複製!", // OSD_CoordinatesCopied
    L"檔案路徑已複製!", // OSD_FilePathCopied
    L"縮放: 100%", // OSD_Zoom100
    L"縮放: 適應螢幕", // OSD_ZoomFit
    L"縮放: 適應視窗", // OSD_ZoomFitWindow
    L"縮放: 填滿視窗", // OSD_ZoomFill
    L"列印: 請在開啟的應用程式中使用 Ctrl+P", // OSD_PrintInstruction
    L"已移至資源回收筒", // OSD_MovedToRecycleBin
    L"視窗已鎖定", // OSD_WindowLocked
    L"視窗已解鎖", // OSD_WindowUnlocked
    L"視窗置頂: 開", // OSD_AlwaysOnTopOn
    L"視窗置頂: 關", // OSD_AlwaysOnTopOff
    L"桌布已設定", // OSD_WallpaperSet
    L"設定桌布失敗", // OSD_WallpaperFailed
    L"重新命名成功", // OSD_Renamed
    L"重新命名失敗", // OSD_RenameFailed
    L"副檔名已修復", // OSD_ExtensionFixed
    L"已恢復原狀", // OSD_Restored
    L"已是第一張", // OSD_FirstImage
    L"已是最後一張", // OSD_LastImage
    L"高畫質", // OSD_HD
    L"縮放: ", // OSD_ZoomPrefix
    L"播放中", // OSD_AnimPlaying
    L"已暫停 (檢查模式: Alt+左右方向鍵逐幀查看)", // OSD_AnimPaused
    L"髒矩形: 開", // OSD_AnimDirtyOn
    L"髒矩形: 關", // OSD_AnimDirtyOff
    L"跨越顯示器 (電視牆模式)\tCtrl+F11", // Context_SpanDisplays
    L"界面", // Settings_Tab_Visuals
    L"操作", // Settings_Tab_Controls
    L"圖像", // Settings_Tab_Image
    L"進階", // Settings_Tab_Advanced
    L"背景", // Settings_Header_Backdrop
    L"視窗", // Settings_Header_Window
    L"鎖定視窗時", // Settings_Header_WindowLock
    L"面板", // Settings_Header_Panel
    L"滑鼠", // Settings_Header_Mouse
    L"邊緣", // Settings_Header_Edge
    L"渲染", // Settings_Header_Render
    L"高動態範圍 (HDR)", // Settings_Header_Hdr
    L"提示", // Settings_Header_Prompts
    L"系統", // Settings_Header_System
    L"功能", // Settings_Header_Features
    L"透明度", // Settings_Header_Transparency
    L"玻璃引擎 (GPU加速)", // Settings_Header_GeekGlass
    L"啟用玻璃渲染", // Settings_Label_EnableGeekGlass
    L"交互動畫", // Settings_Label_GlassUIAnimations
    L"核心材質", // Settings_Header_CoreMaterial
    L"模糊半徑", // Settings_Label_BlurSigma
    L"啟用玻璃渲染後生效", // Settings_Status_GlassDisabled
    L"厚度 (濃度)", // Settings_Label_TintDensity
    L"控制玻璃底色深度。", // Settings_Tooltip_TintDensity
    L"光澤亮度", // Settings_Label_SpecularOpacity
    L"控制玻璃面板光澤強度。", // Settings_Tooltip_SpecularOpacity
    L"陰影強度", // Settings_Label_ShadowIntensity
    L"調節 GPU 加速物理落影的深淺。", // Settings_Tooltip_ShadowIntensity
    L"邊緣", // Settings_Header_VectorAssets
    L"線框粗细", // Settings_Label_VectorStrokeWeight
    L"標準 (1.5px)", // Settings_Option_StrokeStandard
    L"極細 (1.0px)", // Settings_Option_StrokeFine
    L"玻璃著色", // Settings_Header_GlassTint
    L"著色模式", // Settings_Label_TintProfile
    L"自動", // Settings_Option_TintAuto
    L"自定義", // Settings_Option_TintCustom
    L"自定義底色", // Settings_Label_GlassCustomColor
    L"材質濃度", // Settings_Header_DensityMatrix
    L"信息提示 (OSD)", // Settings_Label_OsdDensity
    L"控制加載進度條、播放提示、縮放資訊等浮層的厚度。", // Settings_Tooltip_OsdDensity
    L"工具面板", // Settings_Label_PanelsDensity
    L"控制底部工具列、資訊面板、畫廊底片以及右上角窗口控制按鈕的厚度。", // Settings_Tooltip_PanelsDensity
    L"模態視窗", // Settings_Label_ModalsDensity
    L"控制設定面板、幫助面板和對話方塊等彈出窗口的厚度。", // Settings_Tooltip_ModalsDensity
    L"右鍵選單", // Settings_Label_MenusDensity
    L"控制右鍵選單、下拉列表的厚度。", // Settings_Tooltip_MenusDensity
    L"主題", // Settings_Tab_Theme
    L"主題模式", // Settings_Label_ThemeMode
    L"自動", // Settings_Option_ThemeAuto
    L"深色", // Settings_Option_ThemeDark
    L"淺色", // Settings_Option_ThemeLight
    L"自定義", // Settings_Option_ThemeCustom
    L"環境遮罩", // Settings_Label_AmbientDimmer
    L"在畫廊模式、設定視窗或對話方塊開啟時，為背景添加沉浸式陰影。", // Settings_Tooltip_AmbientDimmer
    L"強調色", // Settings_Label_AccentColor
    L"文字顏色", // Settings_Label_TextColor
    L"匯入匯出 (.qvtheme)", // Settings_Header_ThemeManagement
    L"匯出", // Settings_Action_ExportTheme
    L"匯入", // Settings_Action_ImportTheme
    L"畫布顏色", // Settings_Label_CanvasColor
    L"疊加層", // Settings_Label_Overlay
    L"顯示網格", // Settings_Label_ShowGrid
    L"圖片切換淡入淡出", // Settings_Label_CrossFade
    L"視窗置頂", // Settings_Label_AlwaysOnTop
    L"鎖定視窗", // Settings_Label_LockWindow
    L"此處控制程式啟動時預設鎖定視窗邊框，不跟隨圖片縮放。", // Settings_Tooltip_LockWindow
    L"自動隱藏標題列", // Settings_Label_AutoHideTitle
    L"圓角視窗", // Settings_Label_RoundedCorners
    L"顯示組件邊框", // Settings_Label_UIBorders
    L"控制主視窗及右鍵選單圓角，僅支援Windows 11。", // Settings_Tooltip_RoundedCorners
    L"鎖定底部工具列", // Settings_Label_LockToolbar
    L"預設最小視窗寬度", // Settings_Label_WindowMinSize
    L"預設最大啟動尺寸 (%)", // Settings_Label_WindowMaxSizePercent
    L"顯示邊界指示器", // Settings_Label_ShowBorderIndicator
    L"在圖片超出視窗邊框的方向上顯示藍色提示線條。", // Settings_Tooltip_ShowBorderIndicator
    L"顯示縮略導航圖", // Settings_Label_ShowNavigator
    L"自動", // Settings_Option_NavigatorAuto
    L"開啟", // Settings_Option_NavigatorOn
    L"關閉", // Settings_Option_NavigatorOff
    L"導航時保持視窗尺寸不變", // Settings_Label_KeepWindowSizeOnNav
    L"記住最後視窗位置和尺寸", // Settings_Label_RememberLastWindowSizeAndPosition
    L"小於視窗尺寸圖片適應視窗", // Settings_Label_UpscaleSmallImagesWhenLocked
    L"起動視窗平滑縮放 (GPU)", // Settings_Label_EnableSmoothScaling
    L"EXIF 面板模式", // Settings_Label_ExifMode
    L"工具列資訊預設值", // Settings_Label_ToolbarInfoDefault
    L"開啟時全螢幕", // Settings_Label_OpenFullScreenMode
    L"全螢幕縮放模式", // Settings_Label_FullScreenZoomMode
    L"適應螢幕", // Settings_Option_FitScreen
    L"自動", // Settings_Option_AutoFit
    L"反轉滾輪", // Settings_Label_InvertWheel
    L"縮放 100% 吸附阻尼", // Settings_Label_ZoomSnapDamping
    L"視窗縮放以滑鼠為中線", // Settings_Label_MouseAnchorZoom
    L"右鍵拖曳縮放", // Settings_Label_RightButtonDragZoom
    L"滾輪縮放速度", // Settings_Label_WheelZoomSpeed
    L"拇指滾輪", // Settings_Label_ThumbWheel
    L"右鍵拖曳縮放速度", // Settings_Label_RightDragZoomSpeed
    L"縮放速度(臨時): ", // OSD_WheelZoomSpeed
    L"臨時調節縮放速度", // Help_Action_AdjustZoomSpeed
    L"臨時鎖定視窗縮放", // Help_Action_LockWindowZoom
    L"反轉側鍵", // Settings_Label_InvertButtons
    L"使用固定的縮放級別", // Settings_Label_UseFixedZoom
    L"啟用時，Alt + 滾輪將執行普通縮放而非修改縮放速率。", // Settings_Tooltip_UseFixedZoom
    L"  └  自定義縮放級別", // Settings_Label_FixedZoomLevels
    L"編輯固定縮放級別", // Dialog_FixedZoomTitle
    L"輸入逗號分隔的縮放級別 (例如 0.5, 1, 2):", // Dialog_FixedZoomMsg
    L"放大插值演算法", // Settings_Label_ZoomModeIn
    L"縮小插值演算法", // Settings_Label_ZoomModeOut
    L"左鍵拖曳", // Settings_Label_LeftDrag
    L"中鍵拖曳", // Settings_Label_MiddleDrag
    L"中鍵點選", // Settings_Label_MiddleClick
    L"邊緣點選翻頁", // Settings_Label_EdgeNavClick
    L"在對比模式下停用", // Settings_Label_DisableEdgeNavInCompare
    L"翻頁指示器", // Settings_Label_NavIndicator
    L"自動旋轉 (EXIF)", // Settings_Label_AutoRotate
    L"色彩管理 (CMS)", // Settings_Label_CMS
    L"高級色彩與 HDR (scRGB)", // Settings_Label_AdvancedColor
    L"HDR 色調映射", // Settings_Label_HdrToneMapping
    L"Spline 拐點", // Settings_Label_HdrSplineKnee
    L"HDR 降級策略 (Tone Mapping)：\n當 HDR 圖片超出顯示器極限時的對映方式。\n" L"Spline 樣條對映：採用分段樣條曲線，實現高保真的高光細節還原（推薦）。\n" L"色度模式：保持嚴格亮度對映，超出顯示器極限的亮度將被直接裁剪。\n" L"BT.2390 EETF：ITU-R BT.2390 標準高保真降級曲線。", // Settings_Tooltip_HdrToneMapping
    L"0 = 自動模式（根據圖片亮度自動計算）。\n數值表示顯示器峰值亮度的比例。該拐點以下的亮度將進行 1:1 絕對映射，拐點以上的亮度將使用平滑曲線進行壓縮映射。\n（推薦值：0.4 - 0.75）", // Settings_Tooltip_HdrSplineKnee
    L"HDR 峰值亮度 (Nits)", // Settings_Label_HdrPeakNitsOverride
    L"設為 0 表示通過系統自動檢測亮度.", // Settings_Tooltip_HdrPeakNitsOverride
    L"HDR 峰值百分位", // Settings_Label_HdrPeakPercentile
    L"丟棄極少數過亮的像素以提升整體亮度 (mpv 預設: 99.995%).", // Settings_Tooltip_HdrPeakPercentile
    L"100% (絕對峰值)", // Settings_Option_HdrPeakPercentile_100
    L"99.995% (穩定)", // Settings_Option_HdrPeakPercentile_99995
    L"99.9% (激進)", // Settings_Option_HdrPeakPercentile_999
    L"高光去飽和範圍", // Settings_Label_HdrDesatThreshold
    L"高光去飽和開始介入的亮度閾值。0.0 表示對所有亮度進行去飽和，1.0 表示完全不進行去飽和。推薦預設值 0.18。", // Settings_Tooltip_HdrDesatThreshold
    L"高光去飽和強度", // Settings_Label_HdrMaxDesat
    L"高光去飽和的最大強度。0.0 表示不進行去飽和，1.0 表示在高光處完全去飽和為白色。推薦預設值 0.75。", // Settings_Tooltip_HdrMaxDesat
    L"色度", // Settings_Option_HdrColorimetric
    L"樣條映射", // Settings_Option_HdrSpline
    L"BT.2390 (EETF)", // Settings_Option_HdrLegacyReinhard
    L"無配置圖片的預設回退", // Settings_Label_CmsFallback
    L"自訂軟打樣配置 (.icc)", // Settings_Label_CustomProof
    L"軟打樣預覽", // Context_SoftProofing
    L"打樣設定檔", // Context_SoftProofProfile
    L"自訂...", // Context_SoftProofCustom
    L"敬請期待", // Settings_Value_ComingSoon
    L"強制 RAW 解碼", // Settings_Label_ForceRaw
    L"RAW+JPEG 配對", // Settings_Label_PairRawJpeg
    L"將 RAW 與同名的相機直出圖 (JPEG/HEIF) 顯示為同一張照片。\n清單中僅保留直出圖，並標註 RAW 格式（如 +CR3）；背景會校驗兩者拍攝時間，不一致的配對自動拆開。\n可用 RAW 工具列按鈕或其快速鍵切換相機直出與本機渲染，也可用配對比較快速鍵並排檢視。", // Settings_Tooltip_PairRawJpeg
    L"曝光度 (亮度)", // Settings_Label_Exposure
    L"調節圖像亮度 (曝光補償)。範圍: 0.18x 至 10.0x。", // Settings_Tooltip_Exposure
    L"新增至開啟方式", // Settings_Label_AddToOpenWith
    L"自訂圖片編輯器", // Settings_Label_CustomEditor
    L"選擇編輯器", // Context_SelectEditor
    L"編輯器啟動失敗，請重新配置。", // OSD_EditorLaunchFailed
    L"新增", // Settings_Action_Add
    L"已新增", // Settings_Action_Added
    L"可攜式模式下停用", // Settings_Status_DisabledInPortable
    L"啟用偵錯 HUD (F12)", // Settings_Label_DebugHUD
    L"預先載入系統", // Settings_Label_Prefetch
    L"資訊面板", // Settings_Label_InfoPanelAlpha
    L"工具列", // Settings_Label_ToolbarAlpha
    L"設定選單", // Settings_Label_SettingsAlpha
    L"重設所有設定", // Settings_Label_Reset
    L"還原", // Settings_Action_Restore
    L"完成", // Settings_Action_Done
    L"無管理", // Settings_Option_CmsUnmanaged
    L"sRGB", // Settings_Option_CmssRGB
    L"Display P3", // Settings_Option_CmsP3
    L"Adobe RGB (1998)", // Settings_Option_CmsAdobeRGB
    L"灰度模式 (影調檢查)", // Settings_Option_CmsGray
    L"ProPhoto RGB", // Settings_Option_CmsProPhoto
    L"渲染意圖", // Settings_Label_CmsIntent
    L"色彩溢出檢測", // Settings_Label_GamutWarning
    L"分析並標出溢出區域：關閉、僅在軟打樣模式下檢測（預設）、或者同時檢測軟打樣和螢幕色彩溢出。", // Settings_Tooltip_GamutWarning
    L"自動提示色彩溢出", // Settings_Label_GamutAutoPrompt
    L"檢測到溢出後彈出 OSD 提示。高亮區域可透過工具欄按鈕手動查看。", // Settings_Tooltip_GamutAutoPrompt
    L"溢出高亮顏色", // Settings_Label_GamutColor
    L"相對色度", // Settings_Option_CmsIntentRelative
    L"感知意圖", // Settings_Option_CmsIntentPerceptual
    L"啟用色彩管理 (Color Management System)。\n開啟後，將透過 GPU 進行高精度色彩空間轉換以還原真實色彩。\n關閉可降低效能消耗，但在廣色域螢幕上可能導致顏色過飽和。", // Settings_Tooltip_CMS
    L"色彩空間轉換策略 (Rendering Intent)。\n感知模式 (Perceptual)：壓縮超出色域的顏色，保留細節和漸變，適合照片。\n相對比色 (Relative Colorimetric)：保留在色域內的顏色，超出部分裁剪，適合 UI 和圖示。\n注意：僅當圖像或顯示器使用包含 LUT（查找表）的高級 ICC 配置文件時，切換此選項才會有視覺差異。普通矩陣型配置文件會自動回退至相對色度。", // Settings_Tooltip_CmsIntent
    L"啟用 16-bit 浮點渲染管線 (scRGB)。\n開啟後，在支援 HDR 的顯示器上能突破 SDR 亮度限制，完美呈現照片高光。\n關閉將強制降級對映至 SDR 輸出。\n注意：開啟會增加顯示卡記憶體佔用。", // Settings_Tooltip_AdvancedColor
    L"自動：圖片小於螢幕尺寸時 100% 縮放，圖片大於螢幕尺寸時適應螢幕尺寸縮放。", // Settings_Tooltip_ZoomAuto
    L"檢查更新", // Settings_Action_CheckUpdates
    L"檢視更新", // Settings_Action_ViewUpdate
    L"檢查中...", // Settings_Status_Checking
    L"已是最新", // Settings_Status_UpToDate
    L"GitHub 儲存庫", // Settings_Link_GitHub
    L"回報問題", // Settings_Link_ReportIssue
    L"快速鍵", // Settings_Link_Hotkeys
    L"版本", // Settings_Label_Version
    L"建置", // Settings_Label_Build
    L"黑色", // Settings_Option_Black
    L"白色", // Settings_Option_White
    L"網格", // Settings_Option_Grid
    L"自訂", // Settings_Option_Custom
    L"關閉", // Settings_Option_Off
    L"開啟", // Settings_Option_On
    L"簡略", // Settings_Option_Lite
    L"詳細", // Settings_Option_Full
    L"僅大圖", // Settings_Option_LargeOnly
    L"全部", // Settings_Option_All
    L"軟打樣", // Settings_Option_SoftProofing
    L"視窗", // Settings_Option_Window
    L"平移", // Settings_Option_Pan
    L"無", // Settings_Option_None
    L"結束", // Settings_Option_Exit
    L"箭頭", // Settings_Option_Arrow
    L"游標", // Settings_Option_Cursor
    L"手動", // Settings_Option_Manual
    L"自動 (檔案總管)", // Settings_Option_SortAuto
    L"檔案名稱", // Settings_Option_SortName
    L"修改時間", // Settings_Option_SortModified
    L"拍攝時間 (EXIF)", // Settings_Option_SortDateTaken
    L"大小", // Settings_Option_SortSize
    L"類型", // Settings_Option_SortType
    L"資料夾內循環", // Settings_Option_NavLoop
    L"穿透子資料夾", // Settings_Option_NavThrough
    L"線性 (Linear)：基礎平滑", // Settings_Option_Linear
    L"最近鄰 (Nearest)：極端銳利", // Settings_Option_Nearest
    L"高品質雙三次 (HQ Cubic)：極端平滑", // Settings_Option_HighQualityCubic
    L"自動", // Settings_Option_ZoomAuto
    L"自動", // Settings_Option_Auto
    L"節能", // Settings_Option_Eco
    L"平衡", // Settings_Option_Balanced
    L"極速", // Settings_Option_Ultra
    L"鍵盤快速鍵", // Help_Header_Shortcuts
    L"滑鼠操作", // Help_Header_Mouse
    L"切換圖片", // Help_Item_NextPrev
    L"縮放", // Help_Item_Zoom
    L"平移圖片", // Help_Item_Pan
    L"旋轉", // Help_Item_Rotate
    L"適應螢幕", // Help_Item_Fit
    L"刪除圖片", // Help_Item_Delete
    L"全螢幕", // Help_Item_Fullscreen
    L"關閉", // Help_Item_Close
    L"左鍵", // Help_Mouse_Left
    L"中鍵", // Help_Mouse_Middle
    L"滾輪", // Help_Mouse_Wheel
    L"右鍵", // Help_Mouse_Right
    L"右鍵上下拖曳", // Help_Mouse_RightVerticalDrag
    L"平移視窗/退出全屏/退出最大化", // Help_Action_MoveWindow
    L"平移圖片", // Help_Action_PanImage
    L"右鍵選單", // Help_Action_ContextMenu
    L"切換圖片", // Help_Action_NextPrev
    L"縮放", // Help_Action_Zoom
    L"智能縮放 (100% / 適應窗口 / 恢復)", // Help_Action_SmartZoom
    L"復制圖像", // Help_Desc_Copy
    L"編輯", // Help_Desc_Edit
    L"提示與術語", // Help_Header_Tips
    L"注意：快捷鍵或右鍵操作僅影響當前進程，設置中的配置為程序永久配置。", // Help_Tip_ContextScope
    L"旋轉說明：/邊緣適配/有損 " L"是由於某些圖片格式特性造成的無法完整無損操作。邊緣適配通常只損失邊緣N個" L"像素，接近無損。", // Help_Tip_Rotation
    L"電視牆模式 " L"(Ctrl+F11)：將所有顯示器視為一塊屏幕。若關閉按鈕位於顯示區外 " L"(如L型排布)，雙擊即可退出全屏。", // Help_Tip_VideoWall
    L"臨摹模式/薄膜模式：開啟後圖像將變為半透明並露出底部元素，此時您可調整尺寸或透明度。" L"點擊工具列中的滑鼠穿透模式開關，可進入滑鼠穿透模式，此時除了退出快捷鍵 Shift+Esc 外的任何按鍵和滑鼠輸入均被忽略，" L"QuickView 將變為一層透明薄膜覆蓋層。", // Help_Tip_DesignerMode
    L"色彩溢出檢測：檢測當前圖片是否超出顯示器或軟打樣配置的色域。模式：關閉、僅軟打樣（預設）、全部（包含螢幕）。可從工具列手動開關。", // Help_Tip_GamutDetection
    L"RAW 按鈕：QuickView 默認顯示 RAW " L"預覽圖。點擊此按鈕將使用默認參數完整渲染 RAW 文件 " L"(結果可能與預覽不同)。", // Help_Tip_Raw
    L"JPEG 壓縮率：信息面板顯示的 Q " L"值是逆向推算值。因算法差異，可能與保存時的數值略有出入 (例如 PS 100% " L"可能顯示為 98)，屬正常情況。", // Help_Tip_JpegQ
    L"軟打樣對比：軟打樣後進入對比模式，將自動進行打樣前後對比。", // Help_Tip_SoftProofCompare
    L"發現新版本！", // Dialog_UpdateTitle
    L"v%s 已準備就緒", // Dialog_UpdateContent
    L"更新日誌", // Dialog_UpdateLogHeader
    L"立即更新", // Dialog_ButtonUpdate
    L"稍後", // Dialog_ButtonLater
    L"在 GitHub 點星", // Dialog_ButtonStar
    L"QuickView 源自熱愛", // Dialog_Update_LoveTitle
    L"我利用業餘時間維護 QuickView，只因我相信 Windows " L"值得擁有一個更快、更純粹的看圖工具。 " L"我沒有推廣預算，也沒有團隊。如果您喜歡這次更新，在 GitHub " L"上點一顆星或分享給朋友，就是對我最大的支持。", // Dialog_Update_LoveMessage
    L"對比模式", // Help_Item_Compare
    L"第一張 / 最後一張圖片", // Help_Item_FirstLast
    L"物理屬性", // HUD_Group_Physical
    L"科學指標", // HUD_Group_Scientific
    L"光學與編碼", // HUD_Group_Encoding
    L"邊緣清晰度 (拉普拉斯方差)", // HUD_Tip_Sharp_Desc
    L"邊緣銳利，細節豐富", // HUD_Tip_Sharp_High
    L"軟焦點或運動模糊", // HUD_Tip_Sharp_Low
    L"> 500 為非常銳利", // HUD_Tip_Sharp_Ref
    L"信息密度 (香農熵)", // HUD_Tip_Ent_Desc
    L"複雜紋理或高噪點", // HUD_Tip_Ent_High
    L"平坦區域或低細節", // HUD_Tip_Ent_Low
    L"7.0-8.0 為高細節", // HUD_Tip_Ent_Ref
    L"每像素比特數 (壓縮效率)", // HUD_Tip_BPP_Desc
    L"低壓縮率 (保留更多原始數據)", // HUD_Tip_BPP_High
    L"高壓縮率 (空間利用率更高)", // HUD_Tip_BPP_Low
    L"24.0 (原始RGB), ~2.0-3.0 (高品質JPEG), ~0.5-1.5 (WebP/AVIF)", // HUD_Tip_BPP_Ref
    L"高: ", // HUD_Label_High
    L"低: ", // HUD_Label_Low
    L"參考: ", // HUD_Label_Ref
    L"圖庫底片帶 (頂部懸浮)", // Settings_Header_GalleryTrigger
    L"觸發模式", // Settings_Label_GalleryTriggerMode
    L"自動懸停", // Settings_Option_GalleryTriggerAuto
    L"熱點停留", // Settings_Option_GalleryTriggerDelay
    L"點擊熱點", // Settings_Option_GalleryTriggerClick
    L"停用", // Settings_Option_GalleryTriggerDisable
    L"該功能在視窗小於 600x450 時將自動禁用。", // Settings_Tooltip_GalleryTrigger

    L"開始播放幻燈片", // OSD_SlideshowStarted
    L"停止播放幻燈片", // OSD_SlideshowStopped
    L"繼續播放幻燈片", // OSD_SlideshowResumed
    L"幻燈片已暫停", // OSD_SlideshowPaused
    L"沉浸模式: 聚光燈", // OSD_ImmersiveSpotlight
    L"沉浸模式: 普通", // OSD_ImmersiveNormal
    L"打印任務已發送，您可以繼續瀏覽。", // OSD_PrintJobStarted
    L"打印任务已发送完毕。", // OSD_PrintJobFinished
    L"幻燈片模式", // Context_SlideshowMode
    L"切換間隔 (秒)", // Settings_Label_SlideshowInterval
    L"沉浸模式", // Settings_Label_SlideshowImmersive
    L"自定義精簡信息欄", // Settings_Label_CustomLiteInfoPanel
    L"普通模式顯示項", // Settings_Label_ItemsInNormalMode
    L"對比模式顯示項", // Settings_Label_ItemsInCompareMode
    L"分隔符預設", // Settings_Label_SeparatorPreset
    L"普通", // Settings_Option_SlideshowNormal
    L"聚光燈", // Settings_Option_SlideshowSpotlight
    L"放大鏡形狀", // Settings_Label_LoupeShape
    L"正方形", // Settings_Option_LoupeShapeSquare
    L"圓形", // Settings_Option_LoupeShapeCircle
    L"按住快捷鍵並滾動滑鼠滾輪可調節放大鏡尺寸。", // Settings_Tooltip_LoupeHotkey
};

// ----------------------------------------------------------------
// JA Table
// ----------------------------------------------------------------
static const LanguageTable Table_JA = {
    L"画像が読み込まれていません", // OSD_NoImage
    L"ロスレス", // OSD_Lossless
    L"再エンコード (ロスレス)", // OSD_ReencodedLossless
    L"エッジ最適化", // OSD_EdgeAdapted
    L"再エンコード", // OSD_Reencoded
    L"アクセス拒否 - ファイルが使用中または読み取り専用", // OSD_ReadOnly
    L"変換が不完全 (エッジ最適化済み)", // OSD_NotPerfect
    L"Video Wall: ON", // OSD_SpanOn
    L"Video Wall: OFF", // OSD_SpanOff
    L"校正前 (元画像)", // OSD_CompareBefore
    L"校正後 (プロファイル適用)", // OSD_CompareAfter
    L"削除が取り消されました", // OSD_UndoDeleteSuccess
    L"削除の取り消しに失敗しました", // OSD_UndoDeleteFailed
    L"名前の変更を取り消しました", // OSD_UndoRenameSuccess
    L"名前の変更の取り消しに失敗しました", // OSD_UndoRenameFailed
    L"回転・反転を取り消しました", // OSD_UndoTransformSuccess
    L"回転・反転の取り消しに失敗しました", // OSD_UndoTransformFailed
    L"色域外の色を検出しました", // OSD_GamutDetected
    L"色域: 互換性のないプロファイル", // OSD_GamutIncompatible
    L"色域: 解析に失敗しました", // OSD_GamutFailed
    L"時計回りに90\x00B0回転", // Action_RotateCW
    L"反時計回りに90\x00B0回転", // Action_RotateCCW
    L"180\x00B0回転", // Action_Rotate180
    L"水平反転", // Action_FlipH
    L"垂直反転", // Action_FlipV
    L"変更を保存しますか?", // Dialog_SaveTitle
    L"画像が変更されました。変更を保存しますか?", // Dialog_SaveContent
    L"保存", // Dialog_ButtonSave
    L"名前を付けて保存...", // Dialog_ButtonSaveAs
    L"破棄", // Dialog_ButtonDiscard
    L"続行", // Dialog_ButtonContinue
    L"ロスレス変換を常に保存", // Checkbox_AlwaysSaveLossless
    L"エッジ最適化を常に保存", // Checkbox_AlwaysSaveEdgeAdapted
    L"再エンコードを常に保存", // Checkbox_AlwaysSaveLossy
    L"確認なしで直接ごみ箱に移動", // Checkbox_NeverConfirmDelete
    L"HEICをデコードできません - HEVC拡張機能をインストールしてください", // OSD_HEICCodecMissing
    L"HEICをデコードできません", // Dialog_HEICTitle
    L"システムにHEVC拡張機能がありません。\\nQuickViewは最高のパフォーマンス" L"のためにハードウェアアクセラレーションを使用します。", // Dialog_HEICContent
    L"拡張機能を取得 (無料)", // Dialog_HEICGetExtension
    L"キャンセル", // Dialog_Cancel
    L"一般", // Settings_Tab_General
    L"情報", // Settings_Tab_About
    L"基本", // Settings_Group_Foundation
    L"起動", // Settings_Group_Startup
    L"習慣", // Settings_Group_Habits
    L"言語", // Settings_Label_Language
    L"単一インスタンス", // Settings_Label_SingleInstance
    L"更新確認", // Settings_Label_CheckUpdates
    L"アップデートチャンネル", // Settings_Label_UpdateChannel
    L"安定版", // Settings_Option_UpdateStable
    L"プレリリース (Pre-release)", // Settings_Option_UpdatePreRelease
    L"安定版: プレリリース版が一定期間安定して動作した後に公開されます。\nプレリリース: 最新の変更をいち早くお届けし、フィードバックを収集します。これらの変更は厳密なテストを受けていないことにご注意ください。", // Settings_Tooltip_PreRelease
    L"ループナビゲーション", // Settings_Label_NavLoopMode
    L"並べ替え順序", // Settings_Label_SortOrder
    L"降順", // Settings_Label_SortDescending
    L"常にアーカイブ内を名前順で表示", // Settings_Label_SortArchivesByNameAscending
    L"削除前に確認", // Settings_Label_ConfirmDel
    L"ポータブルモード / クリーンアップ", // Settings_Label_Portable
    L"ポータブルモードとレジストリのクリーンアップ：\n有効にすると、QuickView " L"はポータブルモードで動作します。既存のレジストリ関連付けを自動的にクリー" L"ンアップし、レジストリの自動変更を無効にします。また、設定ファイルは AppData " L"ではなくアプリケーションディレクトリに保存されます。", // Settings_Tooltip_Portable
    L"Span Displays (Video Wall)", // Settings_Label_SpanDisplays
    L"UI スケール", // Settings_Label_UIScale
    L"再起動が必要", // Settings_Status_RestartRequired
    L"書き込み権限なし!", // Settings_Status_NoWritePerm
    L"有効", // Settings_Status_Enabled
    L"Powered by", // Settings_Header_PoweredBy
    L"開く...\tCtrl+O", // Context_Open
    L"プログラムから開く...", // Context_OpenWith
    L"編集\tE", // Context_Edit
    L"エクスプローラーで表示", // Context_ShowInExplorer
    L"フォルダーを開く", // Context_OpenFolder
    L"画像をコピー\tCtrl+C", // Context_CopyImage
    L"パスをコピー\tCtrl+Alt+C", // Context_CopyPath
    L"印刷\tCtrl+P", // Context_Print
    L"時計回りに90\x00B0回転\tR", // Context_RotateCW
    L"反時計回りに90\x00B0回転\tShift+R", // Context_RotateCCW
    L"水平反転\tH", // Context_FlipH
    L"垂直反転\tV", // Context_FlipV
    L"変換", // Context_Transform
    L"実際のサイズ (100%)\t1 / Z", // Context_ActualSize
    L"画面に合わせる\t0 / F", // Context_FitToScreen
    L"ウィンドウに合わせる", // Context_FitWindow
    L"ウィンドウにフィット", // Context_FillWindow
    L"拡大\t+ / Ctrl +", // Context_ZoomIn
    L"縮小\t- / Ctrl -", // Context_ZoomOut
    L"ウィンドウをロック", // Context_LockWindow
    L"常に手前に表示\tCtrl+T", // Context_AlwaysOnTop
    L"HUDギャラリー\tT", // Context_HUDGallery
    L"簡易情報パネル\tTab", // Context_LiteInfoPanel
    L"詳細情報パネル\tI", // Context_FullInfoPanel
    L"RAWをレンダリング", // Context_RenderRAW
    L"ピクセルアートモード", // Context_PixelArtMode
    L"カラースペース", // Context_ColorSpace
    L"全画面\tF11", // Context_Fullscreen
    L"表示", // Context_View
    L"塗りつぶし", // Context_WallpaperFill
    L"フィット", // Context_WallpaperFit
    L"タイル", // Context_WallpaperTile
    L"壁紙に設定", // Context_SetAsWallpaper
    L"名前の変更\tF2", // Context_Rename
    L"拡張子を修正", // Context_FixExtension
    L"削除\tDel", // Context_Delete
    L"削除を取り消す\tCtrl+Z", // Context_UndoDelete
    L"名前の変更を取り消す\tCtrl+Z", // Context_UndoRename
    L"回転・反転を取り消す\tCtrl+Z", // Context_UndoTransform
    L"元に戻す\tCtrl+Z", // Context_Undo
    L"並べ替え", // Context_SortBy
    L"ナビゲーション順序", // Context_NavOrder
    L"昇順", // Context_SortAscending
    L"降順", // Context_SortDescending
    L"設定...", // Context_Settings
    L"比較モード\tC", // Context_CompareMode
    L"トレースモード\tCtrl+Shift+O", // Context_OverlayMode
    L"比較モードで開く", // Context_GalleryOpenCompare
    L"新しいウィンドウで開く", // Context_GalleryOpenNewWindow
    L"終了\tMButton/Esc", // Context_Exit
    L"マウススルーを解除", // Menu_ExitPassthrough
    L"マウススルーモード", // Dialog_PassthroughTitle
    L"マウス操作が下のウィンドウに透過されます。\n解除するにはグローバルショートカット (Shift+Esc) またはタスクバーメニューを使用してください。\n\n続行しますか？", // Dialog_PassthroughContent
    L"マウスクリック透過: ON (Shift+Esc で解除)", // OSD_PassthroughOn
    L"マウスクリック透過: OFF", // OSD_PassthroughOff
    L"トレースモード: ON", // OSD_OverlayModeOn
    L"トレースモード: OFF", // OSD_OverlayModeOff
    L"不透明度", // OSD_Opacity
    L"拡大 (トレース)", // Toolbar_Tooltip_OverlayZoomIn
    L"縮小 (トレース)", // Toolbar_Tooltip_OverlayZoomOut
    L"不透明度を上げる", // Toolbar_Tooltip_OverlayAlphaUp
    L"不透明度を下げる", // Toolbar_Tooltip_OverlayAlphaDown
    L"マウススルーを開始", // Toolbar_Tooltip_OverlayPassthroughOn
    L"マウススルーを解除", // Toolbar_Tooltip_OverlayPassthroughOff
    L"トレースモードを終了", // Toolbar_Tooltip_OverlayExit
    L"エラー", // Message_SaveErrorTitle
    L"ファイルの保存に失敗しました。ファイルがロックされていませんか?", // Message_SaveErrorContent
    L"前へ (左)", // Toolbar_Tooltip_Prev
    L"次へ (右)", // Toolbar_Tooltip_Next
    L"左回転 (Shift+R)", // Toolbar_Tooltip_RotateL
    L"右回転 (R)", // Toolbar_Tooltip_RotateR
    L"水平反転 (H)", // Toolbar_Tooltip_FlipH
    L"ウィンドウをロック(一時的)", // Toolbar_Tooltip_Lock
    L"ウィンドウ固定を解除", // Toolbar_Tooltip_Unlock
    L"ギャラリー (T)", // Toolbar_Tooltip_Gallery
    L"情報パネル", // Toolbar_Tooltip_Info
    L"RAW: プレビュー (クリックでフル)", // Toolbar_Tooltip_RawPreview
    L"RAW: フルデコード (クリックでプレビュー)", // Toolbar_Tooltip_RawFull
    L"RAW: ペアのRAWを表示 (フルデコード)", // Toolbar_Tooltip_RawPairView
    L"RAW: ペア画像に戻る", // Toolbar_Tooltip_RawPairBack
    L"拡張子不一致 (修正)", // Toolbar_Tooltip_FixExtension
    L"ツールバーを固定", // Toolbar_Tooltip_Pin
    L"ツールバーの固定を解除", // Toolbar_Tooltip_Unpin
    L"色域外の色を検出しました", // Toolbar_Tooltip_GamutWarning
    L"ノーマルモード", // Toolbar_Tooltip_NormalMode
    L"比較モード", // Toolbar_Tooltip_CompareMode
    L"1ページ表示", // Toolbar_Tooltip_SinglePage
    L"2ページ表示", // Toolbar_Tooltip_DualPage
    L"選択範囲に新しい画像を開く", // Toolbar_Tooltip_CompareOpen
    L"左右を入れ替え", // Toolbar_Tooltip_CompareSwap
    L"レイアウトを切り替え", // Toolbar_Tooltip_CompareLayout
    L"比較情報", // Toolbar_Tooltip_CompareInfo
    L"選択した画像を削除", // Toolbar_Tooltip_CompareDelete
    L"拡大 (微調整)", // Toolbar_Tooltip_CompareZoomIn
    L"縮小 (微調整)", // Toolbar_Tooltip_CompareZoomOut
    L"ズーム同期: オン", // Toolbar_Tooltip_CompareSyncZoomOn
    L"ズーム同期: オフ", // Toolbar_Tooltip_CompareSyncZoomOff
    L"移動同期: オン", // Toolbar_Tooltip_CompareSyncPanOn
    L"移動同期: オフ", // Toolbar_Tooltip_CompareSyncPanOff
    L"シネマティック スポットライトの切り替え", // Toolbar_Tooltip_SlideshowImmersiveToggle
    L"スライドショーモードの終了", // Toolbar_Tooltip_SlideshowExit
    L"比較を終了", // Toolbar_Tooltip_CompareExit
    L"アニメーション再生", // Toolbar_Tooltip_AnimPlay
    L"アニメーション一時停止", // Toolbar_Tooltip_AnimPause
    L"前のフレーム", // Toolbar_Tooltip_AnimPrev
    L"次のフレーム", // Toolbar_Tooltip_AnimNext
    L"ダーティ領域デバッグ: オン", // Toolbar_Tooltip_AnimDirtyOn
    L"ダーティ領域デバッグ: オフ", // Toolbar_Tooltip_AnimDirtyOff
    L"アニメーション速度", // Toolbar_Tooltip_AnimSpeed
    L"パフォーマンス", // Settings_Header_Performance
    L"プロ向けツール", // Settings_Header_Professional
    L"Memory Reclaim Strategy:", // Settings_Label_MemoryReclaim
    L"Smart (Auto)", // Settings_Option_MemSmart
    L"Aggressive (Max Perf)", // Settings_Option_MemAggressive
    L"On-Demand (Min RAM)", // Settings_Option_MemOnDemand
    L"Smart: Balance performance and RAM.\nAggressive: Maximize performance, high memory usage.\nOn-Demand: Release memory immediately when idle.", // Settings_Tooltip_MemoryReclaim
    L"アニメーションモードで脏矩形ボタンを表示", // Settings_Label_ShowDirtyRect
    L"アニメーションモードのツールバーにダーティレクタングル（更新領域）のデバッグボタンを表示します。", // Settings_Tooltip_ShowDirtyRect
    L"コピーしました!", // OSD_Copied
    L"座標をコピーしました!", // OSD_CoordinatesCopied
    L"ファイルパスをコピーしました!", // OSD_FilePathCopied
    L"ズーム: 100%", // OSD_Zoom100
    L"ズーム: 画面に合わせる", // OSD_ZoomFit
    L"ズーム: ウィンドウに合わせる", // OSD_ZoomFitWindow
    L"ズーム: ウィンドウにフィット", // OSD_ZoomFill
    L"印刷: 開いたアプリでCtrl+Pを使用", // OSD_PrintInstruction
    L"ごみ箱に移動しました", // OSD_MovedToRecycleBin
    L"ウィンドウ固定", // OSD_WindowLocked
    L"ウィンドウ固定解除", // OSD_WindowUnlocked
    L"常に手前: オン", // OSD_AlwaysOnTopOn
    L"常に手前: オフ", // OSD_AlwaysOnTopOff
    L"壁紙を設定しました", // OSD_WallpaperSet
    L"壁紙の設定に失敗", // OSD_WallpaperFailed
    L"名前変更完了", // OSD_Renamed
    L"名前変更失敗", // OSD_RenameFailed
    L"拡張子を修正しました", // OSD_ExtensionFixed
    L"復元されました", // OSD_Restored
    L"最初の画像", // OSD_FirstImage
    L"最後の画像", // OSD_LastImage
    L"HD", // OSD_HD
    L"ズーム: ", // OSD_ZoomPrefix
    L"再生中", // OSD_AnimPlaying
    L"一時停止 (Alt+左右でシーク)", // OSD_AnimPaused
    L"ダーティ領域: オン", // OSD_AnimDirtyOn
    L"ダーティ領域: オフ", // OSD_AnimDirtyOff
    L"Span Displays (Video Wall)\tCtrl+F11", // Context_SpanDisplays
    L"インターフェース", // Settings_Tab_Visuals
    L"操作", // Settings_Tab_Controls
    L"画像", // Settings_Tab_Image
    L"詳細", // Settings_Tab_Advanced
    L"背景", // Settings_Header_Backdrop
    L"ウィンドウ", // Settings_Header_Window
    L"ウィンドウロック", // Settings_Header_WindowLock
    L"パネル", // Settings_Header_Panel
    L"マウス", // Settings_Header_Mouse
    L"エッジ", // Settings_Header_Edge
    L"レンダリング", // Settings_Header_Render
    L"ハイダイナミックレンジ (HDR)", // Settings_Header_Hdr
    L"プロンプト", // Settings_Header_Prompts
    L"システム", // Settings_Header_System
    L"機能", // Settings_Header_Features
    L"透明度", // Settings_Header_Transparency
    L"ガラスエンジン (GPU)", // Settings_Header_GeekGlass
    L"ガラスレンダリング", // Settings_Label_EnableGeekGlass
    L"アニメーション", // Settings_Label_GlassUIAnimations
    L"核心マテリアル", // Settings_Header_CoreMaterial
    L"ぼかし半径", // Settings_Label_BlurSigma
    L"レンダリング有効時に適用", // Settings_Status_GlassDisabled
    L"厚み (濃度)", // Settings_Label_TintDensity
    L"ガラスのベースカラーের深さを制御します。", // Settings_Tooltip_TintDensity
    L"光沢の反射", // Settings_Label_SpecularOpacity
    L"ガラスパネルの光沢強度を制御します。", // Settings_Tooltip_SpecularOpacity
    L"影の強さ", // Settings_Label_ShadowIntensity
    L"影の強度を調整します。", // Settings_Tooltip_ShadowIntensity
    L"エッジ", // Settings_Header_VectorAssets
    L"線幅", // Settings_Label_VectorStrokeWeight
    L"標準 (1.5px)", // Settings_Option_StrokeStandard
    L"極細 (1.0px)", // Settings_Option_StrokeFine
    L"着色モード", // Settings_Header_GlassTint
    L"プロファイル", // Settings_Label_TintProfile
    L"自動", // Settings_Option_TintAuto
    L"カスタム", // Settings_Option_TintCustom
    L"カスタム底色", // Settings_Label_GlassCustomColor
    L"インターフェース密度", // Settings_Header_DensityMatrix
    L"OSD & HUD", // Settings_Label_OsdDensity
    L"OSDなどの浮層の厚みを制御します。", // Settings_Tooltip_OsdDensity
    L"ツールパネル", // Settings_Label_PanelsDensity
    L"ツールバー、情報パネル、ギャラリー、および右上ウィンドウコントロールの厚みを制御します。", // Settings_Tooltip_PanelsDensity
    L"モーダルウィンドウ", // Settings_Label_ModalsDensity
    L"設定、情報、ダイアログの厚みを制御します。", // Settings_Tooltip_ModalsDensity
    L"右クリックメニュー", // Settings_Label_MenusDensity
    L"右クリックメニュー、ドロップダウンの厚みを制御します。", // Settings_Tooltip_MenusDensity
    L"テーマ", // Settings_Tab_Theme
    L"モード", // Settings_Label_ThemeMode
    L"システム", // Settings_Option_ThemeAuto
    L"ダーク", // Settings_Option_ThemeDark
    L"ライト", // Settings_Option_ThemeLight
    L"デザイン", // Settings_Option_ThemeCustom
    L"モーダルディマー", // Settings_Label_AmbientDimmer
    L"没入型シャドウを追加します。", // Settings_Tooltip_AmbientDimmer
    L"アクセント色", // Settings_Label_AccentColor
    L"テキスト色", // Settings_Label_TextColor
    L"テーマエンジン", // Settings_Header_ThemeManagement
    L"エクスポート", // Settings_Action_ExportTheme
    L"インポート", // Settings_Action_ImportTheme
    L"キャンバス色", // Settings_Label_CanvasColor
    L"オーバーレイ", // Settings_Label_Overlay
    L"グリッド表示", // Settings_Label_ShowGrid
    L"画像の切り替えフェード", // Settings_Label_CrossFade
    L"常に手前", // Settings_Label_AlwaysOnTop
    L"ウィンドウをロック", // Settings_Label_LockWindow
    L"プログラムの起動時にウィンドウの境界線をデフォルトでロックし、画像の拡大縮小に追従させないように制御します。", // Settings_Tooltip_LockWindow
    L"タイトルバー自動非表示", // Settings_Label_AutoHideTitle
    L"角丸ウィンドウ", // Settings_Label_RoundedCorners
    L"UI境界線を表示", // Settings_Label_UIBorders
    L"ウィンドウとコンテキストメニューの角丸を制御します。Windows 11のみをサポートします。", // Settings_Tooltip_RoundedCorners
    L"下部ツールバー固定", // Settings_Label_LockToolbar
    L"最小ウィンドウ幅", // Settings_Label_WindowMinSize
    L"最大起動サイズ (%)", // Settings_Label_WindowMaxSizePercent
    L"エッジインジケーターを表示", // Settings_Label_ShowBorderIndicator
    L"画像がウィンドウ境界を超えている方向に青いインジケーターラインを表示します。", // Settings_Tooltip_ShowBorderIndicator
    L"ミニマップを表示", // Settings_Label_ShowNavigator
    L"自動", // Settings_Option_NavigatorAuto
    L"オン", // Settings_Option_NavigatorOn
    L"オフ", // Settings_Option_NavigatorOff
    L"ナビゲーション時にウィンドウサイズを保持", // Settings_Label_KeepWindowSizeOnNav
    L"最後のウィンドウの位置とサイズを記憶", // Settings_Label_RememberLastWindowSizeAndPosition
    L"小さな画像をウィンドウに合わせる", // Settings_Label_UpscaleSmallImagesWhenLocked
    L"ウィンドウの滑らかなスケーリング (GPU)", // Settings_Label_EnableSmoothScaling
    L"EXIFパネルモード", // Settings_Label_ExifMode
    L"ツールバー情報デフォルト", // Settings_Label_ToolbarInfoDefault
    L"開くときに全画面表示", // Settings_Label_OpenFullScreenMode
    L"全画面ズームモード", // Settings_Label_FullScreenZoomMode
    L"画面に合わせる", // Settings_Option_FitScreen
    L"自動", // Settings_Option_AutoFit
    L"ホイール反転", // Settings_Label_InvertWheel
    L"100%ズーム吸着ダンピング", // Settings_Label_ZoomSnapDamping
    L"マウス中心でウィンドウを拡大", // Settings_Label_MouseAnchorZoom
    L"右ドラッグでズーム", // Settings_Label_RightButtonDragZoom
    L"ホイールズーム速度", // Settings_Label_WheelZoomSpeed
    L"サムホイール", // Settings_Label_ThumbWheel
    L"右ドラッグズーム速度", // Settings_Label_RightDragZoomSpeed
    L"ズーム速度(一時的): ", // OSD_WheelZoomSpeed
    L"一時的にズーム速度を調整", // Help_Action_AdjustZoomSpeed
    L"ウィンドウのズームを一時的にロック", // Help_Action_LockWindowZoom
    L"サイドボタン反転", // Settings_Label_InvertButtons
    L"固定ズーム倍率を使用する", // Settings_Label_UseFixedZoom
    L"有効にすると、Alt + ホイールはズーム速度の変更ではなく、通常のズームを実行します。", // Settings_Tooltip_UseFixedZoom
    L"  └  カスタムズーム倍率", // Settings_Label_FixedZoomLevels
    L"ズーム倍率の編集", // Dialog_FixedZoomTitle
    L"カンマ区切りのズーム倍率を入力してください (例: 0.5, 1, 2):", // Dialog_FixedZoomMsg
    L"ズームインモード", // Settings_Label_ZoomModeIn
    L"ズームアウトモード", // Settings_Label_ZoomModeOut
    L"左ドラッグ", // Settings_Label_LeftDrag
    L"中ドラッグ", // Settings_Label_MiddleDrag
    L"中クリック", // Settings_Label_MiddleClick
    L"エッジナビクリック", // Settings_Label_EdgeNavClick
    L"比較モードで無効にする", // Settings_Label_DisableEdgeNavInCompare
    L"ナビインジケーター", // Settings_Label_NavIndicator
    L"自動回転 (EXIF)", // Settings_Label_AutoRotate
    L"カラー管理 (CMS)", // Settings_Label_CMS
    L"高度な色とHDR (scRGB)", // Settings_Label_AdvancedColor
    L"HDR トーンマッピング", // Settings_Label_HdrToneMapping
    L"Spline Knee Point", // Settings_Label_HdrSplineKnee
    L"HDR トーンマッピング戦略:\nモニターの限界を超えるHDR画像の表示方法を決定します。\n" L"スプライン:区分スプライン曲線を使用した高忠実度のハイライトロールオフ（推奨）。\n" L"測色: 厳密な輝度マッピング。モニターの限界を超えるハイライトはクリップされます。\n" L"BT.2390 (EETF): ITU-R BT.2390 EETF 曲線による高忠実度なトーンマッピング。", // Settings_Tooltip_HdrToneMapping
    L"0 = Auto (Calculated based on image luminance).\nThe value represents the ratio of the monitor's peak luminance. Brightness below this knee point maps 1:1, while brightness above is smoothly compressed using a spline curve.\n(Recommended: 0.4 - 0.75)", // Settings_Tooltip_HdrSplineKnee
    L"HDR ピーク輝度 (Nits)", // Settings_Label_HdrPeakNitsOverride
    L"システム検出輝度を使用する場合は0に設定します。", // Settings_Tooltip_HdrPeakNitsOverride
    L"HDR ピークパーセンタイル", // Settings_Label_HdrPeakPercentile
    L"極端に明るいピクセルを除外して全体を明るくします (mpvのデフォルト: 99.995%)", // Settings_Tooltip_HdrPeakPercentile
    L"100% (絶対ピーク)", // Settings_Option_HdrPeakPercentile_100
    L"99.995% (安定)", // Settings_Option_HdrPeakPercentile_99995
    L"99.9% (積極的)", // Settings_Option_HdrPeakPercentile_999
    L"HDR ハイライト去飽和範囲", // Settings_Label_HdrDesatThreshold
    L"ハイライト去飽和の開始しきい値。0.0はすべての輝度を去飽和し、1.0は去飽和なしを意味します。推奨デフォルトは0.18です。", // Settings_Tooltip_HdrDesatThreshold
    L"HDR ハイライト去飽和強度", // Settings_Label_HdrMaxDesat
    L"極端なハイライトでの去飽和の最大強度。0.0は去飽和なし、1.0は完全に白に去飽和されることを意味します。推奨デフォルトは0.75です。", // Settings_Tooltip_HdrMaxDesat
    L"測色", // Settings_Option_HdrColorimetric
    L"スプライン", // Settings_Option_HdrSpline
    L"BT.2390 (EETF)", // Settings_Option_HdrLegacyReinhard
    L"プロファイルなし画像のフォールバック", // Settings_Label_CmsFallback
    L"ソフトプルーフプロファイル (.icc)", // Settings_Label_CustomProof
    L"ソフトプルーフプレビュー", // Context_SoftProofing
    L"プルーフプロファイル", // Context_SoftProofProfile
    L"カスタム...", // Context_SoftProofCustom
    L"近日公開", // Settings_Value_ComingSoon
    L"RAW強制デコード", // Settings_Label_ForceRaw
    L"RAW+JPEG ペアリング", // Settings_Label_PairRawJpeg
    L"RAW と同名のカメラ撮って出し画像 (JPEG/HEIF) を 1 枚の写真として表示します。\n一覧には撮って出し画像のみが残り、RAW 形式 (+CR3 など) が表示されます。撮影時刻はバックグラウンドで照合され、一致しないペアは解除されます。\nRAW ツールバーボタンまたはそのホットキーで撮って出しと RAW デコード表示を切り替え、ペア比較ホットキーで並べて確認できます。", // Settings_Tooltip_PairRawJpeg
    L"露出 (明るさ)", // Settings_Label_Exposure
    L"画像の明るさを調整します (露出補正)。範囲: 0.18x ~ 10.0x。", // Settings_Tooltip_Exposure
    L"プログラムから開くに追加", // Settings_Label_AddToOpenWith
    L"カスタム画像エディタ", // Settings_Label_CustomEditor
    L"エディタを選択", // Context_SelectEditor
    L"エディタの起動に失敗しました。再設定してください。", // OSD_EditorLaunchFailed
    L"追加", // Settings_Action_Add
    L"追加済み", // Settings_Action_Added
    L"ポータブルモードでは無効", // Settings_Status_DisabledInPortable
    L"デバッグHUD有効 (F12)", // Settings_Label_DebugHUD
    L"プリフェッチシステム", // Settings_Label_Prefetch
    L"情報パネル", // Settings_Label_InfoPanelAlpha
    L"ツールバー", // Settings_Label_ToolbarAlpha
    L"設定", // Settings_Label_SettingsAlpha
    L"すべての設定をリセット", // Settings_Label_Reset
    L"復元", // Settings_Action_Restore
    L"完了", // Settings_Action_Done
    L"未管理 (高速)", // Settings_Option_CmsUnmanaged
    L"sRGB (標準)", // Settings_Option_CmssRGB
    L"Display P3 (広色域)", // Settings_Option_CmsP3
    L"Adobe RGB (1998)", // Settings_Option_CmsAdobeRGB
    L"グレースケール (トーン確認)", // Settings_Option_CmsGray
    L"ProPhoto RGB", // Settings_Option_CmsProPhoto
    L"レンダリングインテント", // Settings_Label_CmsIntent
    L"色域外警告の検出", // Settings_Label_GamutWarning
    L"色域外領域の分析とハイライト：オフ、ソフトプルーフモードのみ（デフォルト）、またはソフトプルーフとディスプレイ色域の両方。", // Settings_Tooltip_GamutWarning
    L"色域エラー時の自動プロンプト", // Settings_Label_GamutAutoPrompt
    L"色域エラーが検出されたときにOSD通知を表示します。ハイライトはツールバーで手動で切り替えられます。", // Settings_Tooltip_GamutAutoPrompt
    L"色域外警告のハイライト色", // Settings_Label_GamutColor
    L"相対的な色域を維持", // Settings_Option_CmsIntentRelative
    L"知覚的", // Settings_Option_CmsIntentPerceptual
    L"カラーマネジメントシステム (CMS) を有効にする。\n有効にすると、GPUによる高精度な色空間変換が適用され、正しい色を再現します。\n無効にするとGPUの負荷を減らせますが、広色域ディスプレイでは色が過飽和になる場合があります。", // Settings_Tooltip_CMS
    L"色空間の変換方法 (レンダリングインテント)。\n知覚的 (Perceptual)：色域外の色を圧縮し、階調とディテールを保持します (写真向け)。\n相対的な色域を維持 (Relative Colorimetric)：色域内の色はそのまま維持し、色域外の色はクリップします (UIやアイコン向け)。\n注意：LUT（ルックアップテーブル）を含む高度なICCプロファイルを使用している場合にのみ視覚的な違いが生じます。標準的なマトリックス型プロファイルでは自動的に相対的な色域を維持にフォールバックされます。", // Settings_Tooltip_CmsIntent
    L"16-bit浮動小数点レンダリングパイプライン (scRGB) を有効にする。\n有効にすると、HDR対応ディスプレイでSDRの制限を超え、写真のハイライトを完璧に表現します。\n無効にするとSDR出力に強制的にマッピングされます。\n注意: 有効にするとVRAMの使用量が増加します。", // Settings_Tooltip_AdvancedColor
    L"自動：画像が画面サイズより小さい場合は100%に拡大縮小し、大きい場合は画面サイズに合わせて拡大縮小します。", // Settings_Tooltip_ZoomAuto
    L"更新を確認", // Settings_Action_CheckUpdates
    L"更新を表示", // Settings_Action_ViewUpdate
    L"確認中...", // Settings_Status_Checking
    L"最新です", // Settings_Status_UpToDate
    L"GitHubリポジトリ", // Settings_Link_GitHub
    L"問題を報告", // Settings_Link_ReportIssue
    L"ホットキー", // Settings_Link_Hotkeys
    L"バージョン", // Settings_Label_Version
    L"ビルド", // Settings_Label_Build
    L"黒", // Settings_Option_Black
    L"白", // Settings_Option_White
    L"グリッド", // Settings_Option_Grid
    L"カスタム", // Settings_Option_Custom
    L"オフ", // Settings_Option_Off
    L"オン", // Settings_Option_On
    L"簡易", // Settings_Option_Lite
    L"詳細", // Settings_Option_Full
    L"大きい画像のみ", // Settings_Option_LargeOnly
    L"すべて", // Settings_Option_All
    L"ソフトプルーフ", // Settings_Option_SoftProofing
    L"ウィンドウ", // Settings_Option_Window
    L"パン", // Settings_Option_Pan
    L"なし", // Settings_Option_None
    L"終了", // Settings_Option_Exit
    L"矢印", // Settings_Option_Arrow
    L"カーソル", // Settings_Option_Cursor
    L"手動", // Settings_Option_Manual
    L"自動 (エクスプローラー)", // Settings_Option_SortAuto
    L"名前", // Settings_Option_SortName
    L"更新日時", // Settings_Option_SortModified
    L"撮影日時 (EXIF)", // Settings_Option_SortDateTaken
    L"サイズ", // Settings_Option_SortSize
    L"種類", // Settings_Option_SortType
    L"フォルダー内でループ", // Settings_Option_NavLoop
    L"サブフォルダーを貫通", // Settings_Option_NavThrough
    L"線形: 基本的な平滑化", // Settings_Option_Linear
    L"ニアレスト: 極端なシャープネス", // Settings_Option_Nearest
    L"HQキュービック: 極端な平滑化", // Settings_Option_HighQualityCubic
    L"自動", // Settings_Option_ZoomAuto
    L"自動", // Settings_Option_Auto
    L"エコ", // Settings_Option_Eco
    L"バランス", // Settings_Option_Balanced
    L"ウルトラ", // Settings_Option_Ultra
    L"キーボードショートカット", // Help_Header_Shortcuts
    L"マウス操作", // Help_Header_Mouse
    L"次の/前の画像", // Help_Item_NextPrev
    L"ズームイン/アウト", // Help_Item_Zoom
    L"画像をパン", // Help_Item_Pan
    L"回転", // Help_Item_Rotate
    L"画面に合わせる", // Help_Item_Fit
    L"画像を削除", // Help_Item_Delete
    L"全画面", // Help_Item_Fullscreen
    L"閉じる", // Help_Item_Close
    L"左ボタン", // Help_Mouse_Left
    L"中ボタン", // Help_Mouse_Middle
    L"ホイール", // Help_Mouse_Wheel
    L"右ボタン", // Help_Mouse_Right
    L"右ボタン上下ドラッグ", // Help_Mouse_RightVerticalDrag
    L"ウィンドウ移動 / 全画面終了 / 最大化解除", // Help_Action_MoveWindow
    L"画像をパン", // Help_Action_PanImage
    L"コンテキストメニュー", // Help_Action_ContextMenu
    L"次/前の画像", // Help_Action_NextPrev
    L"ズーム", // Help_Action_Zoom
    L"スマートズーム (100% / ウィンドウに合わせる / 復元)", // Help_Action_SmartZoom
    L"Copy Image", // Help_Desc_Copy
    L"Edit", // Help_Desc_Edit
    L"Tips & Glossary", // Help_Header_Tips
    L"Note: Shortcuts and context menu actions affect the current process " L"only. Settings are permanent.", // Help_Tip_ContextScope
    L"Rotation: 'Edge Adapted' means minor cropping to fit block boundaries " L"(lossless data). 'Lossy' means full re-encoding.", // Help_Tip_Rotation
    L"Video Wall (Ctrl+F11): Spans all monitors. If close button is hidden, " L"double-click to exit.", // Help_Tip_VideoWall
    L"トレースモード / フィルムモード：有効にすると画像が半透明になり、下の要素が表示されます。サイズや透明度を調整できます。ツールバーのマウスパススルー切り替えをクリックするとパススルーモードになり、Shift+Esc 以外のすべての入力が無視され、QuickView が透明なオーバーレイになります。", // Help_Tip_DesignerMode
    L"色域警告：ターゲットディスプレイまたはソフトプルーフプロファイルの色域外の色を検出します。モード：オフ、ソフトプルーフのみ、またはすべて（デフォルト：ソフトプルーフ）。ツールバーで切り替え可能。", // Help_Tip_GamutDetection
    L"RAW Button: QuickView shows embedded preview by default. Click to " L"fully decode (may look different due to rendering parameters).", // Help_Tip_Raw
    L"JPEG Quality: Estimated Q value (reverse engineered). May differ " L"slightly from save setting due to algorithm variations.", // Help_Tip_JpegQ
    L"校正設定の比較：校正中に比較モードに入ると、元画像と校正後の画像を自動的に比較します。", // Help_Tip_SoftProofCompare
    L"新しいバージョンが利用可能です！", // Dialog_UpdateTitle
    L"v%s is ready.", // Dialog_UpdateContent
    L"更新内容:", // Dialog_UpdateLogHeader
    L"今すぐ更新", // Dialog_ButtonUpdate
    L"後で", // Dialog_ButtonLater
    L"GitHubでスター", // Dialog_ButtonStar
    L"QuickViewは情熱から生まれました", // Dialog_Update_LoveTitle
    L"私は余暇を使ってQuickViewをメンテナンスしています。Windowsにはもっと高" L"速でクリーンなビューアが必要だと信じているからです。予算もチームもあり" L"ません。もしこの更新を気に入っていただけたら、GitHubでスターを付けるか" L"、友人に共有していただけると最大の支援になります。", // Dialog_Update_LoveMessage
    L"Compare Mode", // Help_Item_Compare
    L"最初 / 最後の画像", // Help_Item_FirstLast
    L"PHYSICAL ATTRIBUTES", // HUD_Group_Physical
    L"SCIENTIFIC QUALITY", // HUD_Group_Scientific
    L"OPTICS & ENCODING", // HUD_Group_Encoding
    L"Edge definition (Laplacian Variance)", // HUD_Tip_Sharp_Desc
    L"Crisp edges, high detail", // HUD_Tip_Sharp_High
    L"Soft focus or motion blur", // HUD_Tip_Sharp_Low
    L"> 500 is very sharp", // HUD_Tip_Sharp_Ref
    L"Information density (Shannon Entropy)", // HUD_Tip_Ent_Desc
    L"Complex textures or high noise", // HUD_Tip_Ent_High
    L"Flat areas or low detail", // HUD_Tip_Ent_Low
    L"7.0-8.0 is high detail", // HUD_Tip_Ent_Ref
    L"Bits Per Pixel (Compression Efficiency)", // HUD_Tip_BPP_Desc
    L"Lower efficiency (more data preserved)", // HUD_Tip_BPP_High
    L"Higher efficiency (higher compression)", // HUD_Tip_BPP_Low
    L"24.0 (Raw RGB), ~2.0-3.0 (High JPEG), ~0.5-1.5 (WebP/AVIF)", // HUD_Tip_BPP_Ref
    L"High: ", // HUD_Label_High
    L"Low: ", // HUD_Label_Low
    L"Ref: ", // HUD_Label_Ref
    L"ギャラリーフィルムストリップ（上部ホバー）", // Settings_Header_GalleryTrigger
    L"トリガーモード", // Settings_Label_GalleryTriggerMode
    L"自動ホバー", // Settings_Option_GalleryTriggerAuto
    L"ホットスポットホバー", // Settings_Option_GalleryTriggerDelay
    L"ホットスポットクリック", // Settings_Option_GalleryTriggerClick
    L"無効", // Settings_Option_GalleryTriggerDisable
    L"ウィンドウが600x450より小さい場合、この機能は自動的に無効になります。", // Settings_Tooltip_GalleryTrigger

    L"スライドショー開始", // OSD_SlideshowStarted
    L"スライドショー停止", // OSD_SlideshowStopped
    L"スライドショー再開", // OSD_SlideshowResumed
    L"スライドショー一時停止", // OSD_SlideshowPaused
    L"イマーシブ: スポットライト", // OSD_ImmersiveSpotlight
    L"イマーシブ: 通常", // OSD_ImmersiveNormal
    L"印刷ジョブが送信されました。閲覧を続けることができます。", // OSD_PrintJobStarted
    L"印刷ジョブが正常に送信されました。", // OSD_PrintJobFinished
    L"スライドショーモード", // Context_SlideshowMode
    L"切り替え間隔 (秒)", // Settings_Label_SlideshowInterval
    L"イマーシブモード", // Settings_Label_SlideshowImmersive
    L"簡易情報パネルのカスタマイズ", // Settings_Label_CustomLiteInfoPanel
    L"通常モードの表示項目", // Settings_Label_ItemsInNormalMode
    L"比較モードの表示項目", // Settings_Label_ItemsInCompareMode
    L"区切り文字プリセット", // Settings_Label_SeparatorPreset
    L"通常", // Settings_Option_SlideshowNormal
    L"スポットライト", // Settings_Option_SlideshowSpotlight
    L"ルーペの形状", // Settings_Label_LoupeShape
    L"正方形", // Settings_Option_LoupeShapeSquare
    L"円形", // Settings_Option_LoupeShapeCircle
    L"ショートカットキーを押しながらマウスホイールをスクロールすると、ルーペのサイズを調整できます。", // Settings_Tooltip_LoupeHotkey
};

// ----------------------------------------------------------------
// RU Table
// ----------------------------------------------------------------
static const LanguageTable Table_RU = {
    L"Изображение не загружено", // OSD_NoImage
    L"Без потерь", // OSD_Lossless
    L"Перекодировано (без потерь)", // OSD_ReencodedLossless
    L"Оптимизация краёв", // OSD_EdgeAdapted
    L"Перекодировано", // OSD_Reencoded
    L"Доступ запрещён - файл используется или только для чтения", // OSD_ReadOnly
    L"Преобразование неидеально (оптимизация краёв)", // OSD_NotPerfect
    L"Видеостена: ВКЛ", // OSD_SpanOn
    L"Видеостена: ВЫКЛ", // OSD_SpanOff
    L"До (Оригинал)", // OSD_CompareBefore
    L"После (Проба)", // OSD_CompareAfter
    L"Удаление отменено", // OSD_UndoDeleteSuccess
    L"Ошибка отмены удаления", // OSD_UndoDeleteFailed
    L"Переименование отменено", // OSD_UndoRenameSuccess
    L"Ошибка отмены переименования", // OSD_UndoRenameFailed
    L"Поворот/отражение отменено", // OSD_UndoTransformSuccess
    L"Ошибка отмены поворота/отражения", // OSD_UndoTransformFailed
    L"Обнаружены цвета вне цветового охвата", // OSD_GamutDetected
    L"Охват: Несовместимый профиль или ошибка разбора", // OSD_GamutIncompatible
    L"Охват: Ошибка анализа", // OSD_GamutFailed
    L"Повернуть на 90\x00B0 по часовой", // Action_RotateCW
    L"Повернуть на 90\x00B0 против часовой", // Action_RotateCCW
    L"Повернуть на 180\x00B0", // Action_Rotate180
    L"Отразить по горизонтали", // Action_FlipH
    L"Отразить по вертикали", // Action_FlipV
    L"Сохранить изменения?", // Dialog_SaveTitle
    L"Изображение было изменено. Сохранить изменения?", // Dialog_SaveContent
    L"Сохранить", // Dialog_ButtonSave
    L"Сохранить как...", // Dialog_ButtonSaveAs
    L"Отменить", // Dialog_ButtonDiscard
    L"Продолжить", // Dialog_ButtonContinue
    L"Всегда сохранять без потерь", // Checkbox_AlwaysSaveLossless
    L"Всегда сохранять с оптимизацией краёв", // Checkbox_AlwaysSaveEdgeAdapted
    L"Всегда сохранять перекодированное", // Checkbox_AlwaysSaveLossy
    L"Отправлять в корзину без подтверждения", // Checkbox_NeverConfirmDelete
    L"Невозможно декодировать HEIC, установите расширение HEVC", // OSD_HEICCodecMissing
    L"Невозможно декодировать HEIC", // Dialog_HEICTitle
    L"В системе отсутствует расширение HEVC.\\nQuickView использует " L"аппаратное ускорение для лучшей производительности.", // Dialog_HEICContent
    L"Получить расширение (бесплатно)", // Dialog_HEICGetExtension
    L"Отмена", // Dialog_Cancel
    L"Общие", // Settings_Tab_General
    L"О программе", // Settings_Tab_About
    L"Основные", // Settings_Group_Foundation
    L"Запуск", // Settings_Group_Startup
    L"Предпочтения", // Settings_Group_Habits
    L"Язык", // Settings_Label_Language
    L"Один экземпляр", // Settings_Label_SingleInstance
    L"Проверка обновлений", // Settings_Label_CheckUpdates
    L"Канал обновлений", // Settings_Label_UpdateChannel
    L"Стабильный", // Settings_Option_UpdateStable
    L"Предварительный (Pre-release)", // Settings_Option_UpdatePreRelease
    L"Стабильный: Выпускается после того, как предварительная версия доказала свою стабильность.\nПредварительный (Pre-release): Моментальная доставка последних изменений для сбора отзывов. Внимание: эти изменения не прошли строгую проверку.", // Settings_Tooltip_PreRelease
    L"Циклическая навигация", // Settings_Label_NavLoopMode
    L"Порядок сортировки", // Settings_Label_SortOrder
    L"По убыванию", // Settings_Label_SortDescending
    L"Всегда сортировать архивы по имени", // Settings_Label_SortArchivesByNameAscending
    L"Подтверждать перед удалением", // Settings_Label_ConfirmDel
    L"Портативный режим / Очистка", // Settings_Label_Portable
    L"Портативный режим и очистка реестра:\nПри включении QuickView работает в " L"портативном режиме. Будут автоматически очищены имеющиеся ассоциации в " L"реестре, отключено автоизменение реестра, а файлы конфигурации " L"будут храниться не в AppData, а в папке приложения.", // Settings_Tooltip_Portable
    L"Распределять по мониторам (видеостена)", // Settings_Label_SpanDisplays
    L"Масштаб интерфейса", // Settings_Label_UIScale
    L"Требуется перезапуск", // Settings_Status_RestartRequired
    L"Нет прав на запись!", // Settings_Status_NoWritePerm
    L"Включено", // Settings_Status_Enabled
    L"Работает на", // Settings_Header_PoweredBy
    L"Открыть...\tCtrl+O", // Context_Open
    L"Открыть с помощью...", // Context_OpenWith
    L"Изменить\tE", // Context_Edit
    L"Показать в Проводнике", // Context_ShowInExplorer
    L"Открыть папку", // Context_OpenFolder
    L"Скопировать изображение\tCtrl+C", // Context_CopyImage
    L"Скопировать путь\tCtrl+Alt+C", // Context_CopyPath
    L"Печать\tCtrl+P", // Context_Print
    L"Повернуть на 90\x00B0 вправо\tR", // Context_RotateCW
    L"Повернуть на 90\x00B0 влево\tShift+R", // Context_RotateCCW
    L"Отразить по горизонтали\tH", // Context_FlipH
    L"Отразить по вертикали\tV", // Context_FlipV
    L"Преобразовать", // Context_Transform
    L"Настоящий размер (100%)\t1 / Z", // Context_ActualSize
    L"По размеру экрана\t0 / F", // Context_FitToScreen
    L"По размеру окна", // Context_FitWindow
    L"Заполнить окно", // Context_FillWindow
    L"Увеличить\t+ / Ctrl +", // Context_ZoomIn
    L"Уменьшить\t- / Ctrl -", // Context_ZoomOut
    L"Заблокировать окно", // Context_LockWindow
    L"Поверх всех окон\tCtrl+T", // Context_AlwaysOnTop
    L"HUD-галерея\tT", // Context_HUDGallery
    L"Краткая панель информации\tTab", // Context_LiteInfoPanel
    L"Полная панель информации\tI", // Context_FullInfoPanel
    L"Рендеринг RAW", // Context_RenderRAW
    L"Режим пиксель-арта", // Context_PixelArtMode
    L"Цветовое пространство", // Context_ColorSpace
    L"Полный экран\tF11", // Context_Fullscreen
    L"Вид", // Context_View
    L"Заполнение", // Context_WallpaperFill
    L"По размеру", // Context_WallpaperFit
    L"Замостить", // Context_WallpaperTile
    L"Сделать обоями", // Context_SetAsWallpaper
    L"Переименовать\tF2", // Context_Rename
    L"Исправить расширение", // Context_FixExtension
    L"Удалить\tDel", // Context_Delete
    L"Отменить удаление\tCtrl+Z", // Context_UndoDelete
    L"Отменить переименование\tCtrl+Z", // Context_UndoRename
    L"Отменить поворот/отражение\tCtrl+Z", // Context_UndoTransform
    L"Отменить\tCtrl+Z", // Context_Undo
    L"Сортировка", // Context_SortBy
    L"Навигация", // Context_NavOrder
    L"По возрастанию", // Context_SortAscending
    L"По убыванию", // Context_SortDescending
    L"Настройки...", // Context_Settings
    L"Режим сравнения\tC", // Context_CompareMode
    L"Режим наложения\tCtrl+Shift+O", // Context_OverlayMode
    L"Открыть в режиме сравнения", // Context_GalleryOpenCompare
    L"Открыть в новом окне", // Context_GalleryOpenNewWindow
    L"Выход\tMButton/Esc", // Context_Exit
    L"Выйти из режима сквозного щелчка", // Menu_ExitPassthrough
    L"Режим сквозного щелчка", // Dialog_PassthroughTitle
    L"События мыши будут 'проходить' сквозь окно.\nВыйти можно только через глобальную горячую клавишу (Shift+Esc) или в меню панели задач.\n\nПродолжить?", // Dialog_PassthroughContent
    L"Сквозной щелчок: ВКЛ (Shift+Esc - выход)", // OSD_PassthroughOn
    L"Сквозной щелчок: ВЫКЛ", // OSD_PassthroughOff
    L"Режим наложения: ВКЛ", // OSD_OverlayModeOn
    L"Режим наложения: ВЫКЛ", // OSD_OverlayModeOff
    L"Непрозрачность", // OSD_Opacity
    L"Увеличить (наложение)", // Toolbar_Tooltip_OverlayZoomIn
    L"Уменьшить (наложение)", // Toolbar_Tooltip_OverlayZoomOut
    L"Увеличить непрозрачность", // Toolbar_Tooltip_OverlayAlphaUp
    L"Уменьшить непрозрачность", // Toolbar_Tooltip_OverlayAlphaDown
    L"Включить сквозной щелчок", // Toolbar_Tooltip_OverlayPassthroughOn
    L"Выключить сквозной щелчок", // Toolbar_Tooltip_OverlayPassthroughOff
    L"Выйти из режима наложения", // Toolbar_Tooltip_OverlayExit
    L"Ошибка", // Message_SaveErrorTitle
    L"Не удалось сохранить файл. Файл заблокирован?", // Message_SaveErrorContent
    L"Предыдущее (Влево)", // Toolbar_Tooltip_Prev
    L"Следующее (Вправо)", // Toolbar_Tooltip_Next
    L"Повернуть влево (Shift+R)", // Toolbar_Tooltip_RotateL
    L"Повернуть вправо (R)", // Toolbar_Tooltip_RotateR
    L"Отразить по горизонтали (H)", // Toolbar_Tooltip_FlipH
    L"Заблокировать окно (временно)", // Toolbar_Tooltip_Lock
    L"Разблокировать окно", // Toolbar_Tooltip_Unlock
    L"Галерея (T)", // Toolbar_Tooltip_Gallery
    L"Информационная панель", // Toolbar_Tooltip_Info
    L"RAW: Предпросмотр (нажмите для полного)", // Toolbar_Tooltip_RawPreview
    L"RAW: Полное декодирование (нажмите для предпросмотра)", // Toolbar_Tooltip_RawFull
    L"RAW: Показать парный RAW (полное декодирование)", // Toolbar_Tooltip_RawPairView
    L"RAW: Вернуться к парному изображению", // Toolbar_Tooltip_RawPairBack
    L"Несоответствие расширения (исправить)", // Toolbar_Tooltip_FixExtension
    L"Закрепить панель", // Toolbar_Tooltip_Pin
    L"Открепить панель", // Toolbar_Tooltip_Unpin
    L"Показать области вне охвата", // Toolbar_Tooltip_GamutWarning
    L"Обычный режим", // Toolbar_Tooltip_NormalMode
    L"Режим сравнения", // Toolbar_Tooltip_CompareMode
    L"Одностраничный режим", // Toolbar_Tooltip_SinglePage
    L"Двухстраничный режим", // Toolbar_Tooltip_DualPage
    L"Открыть новое изображение в выделении", // Toolbar_Tooltip_CompareOpen
    L"Поменять местами", // Toolbar_Tooltip_CompareSwap
    L"Переключить макет", // Toolbar_Tooltip_CompareLayout
    L"Информация о сравнении", // Toolbar_Tooltip_CompareInfo
    L"Удалить выбранное изображение", // Toolbar_Tooltip_CompareDelete
    L"Увеличить (точно)", // Toolbar_Tooltip_CompareZoomIn
    L"Уменьшить (точно)", // Toolbar_Tooltip_CompareZoomOut
    L"Синхр. масштаба: ДА", // Toolbar_Tooltip_CompareSyncZoomOn
    L"Синхр. масштаба: НЕТ", // Toolbar_Tooltip_CompareSyncZoomOff
    L"Синхр. панорамирования: ДА", // Toolbar_Tooltip_CompareSyncPanOn
    L"Синхр. панорамирования: НЕТ", // Toolbar_Tooltip_CompareSyncPanOff
    L"Вкл./выкл. эффект прожектора", // Toolbar_Tooltip_SlideshowImmersiveToggle
    L"Выйти из режима слайд-шоу", // Toolbar_Tooltip_SlideshowExit
    L"Выйти из сравнения", // Toolbar_Tooltip_CompareExit
    L"Воспроизвести анимацию", // Toolbar_Tooltip_AnimPlay
    L"Приостановить анимацию", // Toolbar_Tooltip_AnimPause
    L"Предыдущий кадр", // Toolbar_Tooltip_AnimPrev
    L"Следующий кадр", // Toolbar_Tooltip_AnimNext
    L"Отладка вид. обл.: ДА", // Toolbar_Tooltip_AnimDirtyOn
    L"Отладка вид. обл.: НЕТ", // Toolbar_Tooltip_AnimDirtyOff
    L"Скорость анимации", // Toolbar_Tooltip_AnimSpeed
    L"Производительность", // Settings_Header_Performance
    L"Профессиональные инструменты", // Settings_Header_Professional
    L"Стратегия восстановления памяти:", // Settings_Label_MemoryReclaim
    L"Умная (авто)", // Settings_Option_MemSmart
    L"Агрессивная (макс. производительность)", // Settings_Option_MemAggressive
    L"По требованию (мин. ОЗУ)", // Settings_Option_MemOnDemand
    L"Умная: Баланс производительности и ОЗУ.\nАгрессивная: Максимальная производительность и высокий уровень использования памяти.\nПо требованию: Сразу высвобождать память при простоях.", // Settings_Tooltip_MemoryReclaim
    L"Показывать кнопку отображаемой области в режиме анимации", // Settings_Label_ShowDirtyRect
    L"Показывать кнопку отладки отображаемой области на панели инструментов анимации для отображения обновляемых участков.", // Settings_Tooltip_ShowDirtyRect
    L"Скопировано!", // OSD_Copied
    L"Координаты скопированы!", // OSD_CoordinatesCopied
    L"Путь к файлу скопирован!", // OSD_FilePathCopied
    L"Масштаб: 100%", // OSD_Zoom100
    L"Масштаб: По размеру экрана", // OSD_ZoomFit
    L"Масштаб: По размеру окна", // OSD_ZoomFitWindow
    L"Масштаб: Заполнить окно", // OSD_ZoomFill
    L"Печать: Нажмите Ctrl+P в открытом приложении", // OSD_PrintInstruction
    L"Перемещено в Корзину", // OSD_MovedToRecycleBin
    L"Окно заблокировано", // OSD_WindowLocked
    L"Окно разблокировано", // OSD_WindowUnlocked
    L"Всегда сверху: ДА", // OSD_AlwaysOnTopOn
    L"Всегда сверху: НЕТ", // OSD_AlwaysOnTopOff
    L"Обои установлены", // OSD_WallpaperSet
    L"Не удалось установить обои", // OSD_WallpaperFailed
    L"Переименовано", // OSD_Renamed
    L"Ошибка переименования", // OSD_RenameFailed
    L"Расширение исправлено", // OSD_ExtensionFixed
    L"Восстановлено", // OSD_Restored
    L"Первое изображение", // OSD_FirstImage
    L"Последнее изображение", // OSD_LastImage
    L"HD", // OSD_HD
    L"Масштаб: ", // OSD_ZoomPrefix
    L"Воспроизведение", // OSD_AnimPlaying
    L"Пауза (Alt+Влево/Вправо - покадровый просмотр)", // OSD_AnimPaused
    L"Видимая область: ДА", // OSD_AnimDirtyOn
    L"Видимая область: НЕТ", // OSD_AnimDirtyOff
    L"Распределять по мониторам (видеостена)\tCtrl+F11", // Context_SpanDisplays
    L"Интерфейс", // Settings_Tab_Visuals
    L"Управление", // Settings_Tab_Controls
    L"Картинка", // Settings_Tab_Image
    L"Ещё", // Settings_Tab_Advanced
    L"Фон", // Settings_Header_Backdrop
    L"Окно", // Settings_Header_Window
    L"Блокировка окна", // Settings_Header_WindowLock
    L"Панель", // Settings_Header_Panel
    L"Мышь", // Settings_Header_Mouse
    L"Край", // Settings_Header_Edge
    L"Рендеринг", // Settings_Header_Render
    L"HDR", // Settings_Header_Hdr
    L"Запросы", // Settings_Header_Prompts
    L"Система", // Settings_Header_System
    L"Функции", // Settings_Header_Features
    L"Прозрачность", // Settings_Header_Transparency
    L"Стеклянный движок (ГП)", // Settings_Header_GeekGlass
    L"Эффекты стекла", // Settings_Label_EnableGeekGlass
    L"Анимации", // Settings_Label_GlassUIAnimations
    L"Материал", // Settings_Header_CoreMaterial
    L"Радиус размытия", // Settings_Label_BlurSigma
    L"Стекло отключено (система)", // Settings_Status_GlassDisabled
    L"Плотность", // Settings_Label_TintDensity
    L"Общая интенсивность цвета эффекта стекла.", // Settings_Tooltip_TintDensity
    L"Блики", // Settings_Label_SpecularOpacity
    L"Яркость диагональных световых бликов.", // Settings_Tooltip_SpecularOpacity
    L"Тени", // Settings_Label_ShadowIntensity
    L"Сила теней окружающего затенения.", // Settings_Tooltip_ShadowIntensity
    L"Векторный рендеринг", // Settings_Header_VectorAssets
    L"Обводка значков", // Settings_Label_VectorStrokeWeight
    L"Обычная (1.5 пкс)", // Settings_Option_StrokeStandard
    L"Тонкая (1.0 пкс)", // Settings_Option_StrokeFine
    L"Профиль оттенка", // Settings_Header_GlassTint
    L"Цветовая логика", // Settings_Label_TintProfile
    L"Авто (адаптивно)", // Settings_Option_TintAuto
    L"Свой цвет", // Settings_Option_TintCustom
    L"Оттенок вручную", // Settings_Label_GlassCustomColor
    L"Плотность контрольной поверхности (%)", // Settings_Header_DensityMatrix
    L"OSD и HUD", // Settings_Label_OsdDensity
    L"Прозрачность для небольших плавающих наложений.", // Settings_Tooltip_OsdDensity
    L"Панель инструментов и боковые панели", // Settings_Label_PanelsDensity
    L"Прозрачность для постоянных панелей управления.", // Settings_Tooltip_PanelsDensity
    L"Окна и настройки", // Settings_Label_ModalsDensity
    L"Прозрачность центрированных всплывающих окон.", // Settings_Tooltip_ModalsDensity
    L"Меню", // Settings_Label_MenusDensity
    L"Прозрачность контекстных меню, вызываемых правой кнопкой мыши.", // Settings_Tooltip_MenusDensity
    L"Тема", // Settings_Tab_Theme
    L"Пресет", // Settings_Label_ThemeMode
    L"Авто", // Settings_Option_ThemeAuto
    L"Тёмная", // Settings_Option_ThemeDark
    L"Светлая", // Settings_Option_ThemeLight
    L"Дизайн", // Settings_Option_ThemeCustom
    L"Затемнение (диммер)", // Settings_Label_AmbientDimmer
    L"Затемнять фон, когда открыто модальное окно или окно настроек.", // Settings_Tooltip_AmbientDimmer
    L"Акцентный цвет", // Settings_Label_AccentColor
    L"Цвет текста", // Settings_Label_TextColor
    L"Темы", // Settings_Header_ThemeManagement
    L"Экспорт", // Settings_Action_ExportTheme
    L"Импорт", // Settings_Action_ImportTheme
    L"Цвет холста", // Settings_Label_CanvasColor
    L"Наложение", // Settings_Label_Overlay
    L"Показывать сетку", // Settings_Label_ShowGrid
    L"Плавный переход между изображениями", // Settings_Label_CrossFade
    L"Поверх всех окон", // Settings_Label_AlwaysOnTop
    L"Заблокировать окно", // Settings_Label_LockWindow
    L"Определяет, будут ли блокироваться границы окна по умолчанию при запуске, а не после масштабирования изображения.", // Settings_Tooltip_LockWindow
    L"Автоскрытие заголовка", // Settings_Label_AutoHideTitle
    L"Скруглённые углы", // Settings_Label_RoundedCorners
    L"Показывать границы UI", // Settings_Label_UIBorders
    L"Определяет, будут ли окно и контекстное меню со скруглёнными углами. Требуется Windows 11.", // Settings_Tooltip_RoundedCorners
    L"Закрепить нижнюю панель", // Settings_Label_LockToolbar
    L"Мин. ширина окна", // Settings_Label_WindowMinSize
    L"Макс. начальный размер (%)", // Settings_Label_WindowMaxSizePercent
    L"Показывать индикаторы границ", // Settings_Label_ShowBorderIndicator
    L"Показывает синюю индикаторную линию в направлении, где изображение выходит за границы окна.", // Settings_Tooltip_ShowBorderIndicator
    L"Показывать мини-карту", // Settings_Label_ShowNavigator
    L"Авто", // Settings_Option_NavigatorAuto
    L"Вкл", // Settings_Option_NavigatorOn
    L"Выкл", // Settings_Option_NavigatorOff
    L"Не менять размер окна при навигации", // Settings_Label_KeepWindowSizeOnNav
    L"Запоминать последние размер и положение окна", // Settings_Label_RememberLastWindowSizeAndPosition
    L"Адаптировать мелкие изображения", // Settings_Label_UpscaleSmallImagesWhenLocked
    L"Плавное масштабирование окна (ГП)", // Settings_Label_EnableSmoothScaling
    L"Режим панели EXIF", // Settings_Label_ExifMode
    L"Информация в панели по умолчанию", // Settings_Label_ToolbarInfoDefault
    L"Полноэкранный режим при открытии", // Settings_Label_OpenFullScreenMode
    L"Масштаб в полноэкранном режиме", // Settings_Label_FullScreenZoomMode
    L"Вписывать", // Settings_Option_FitScreen
    L"Авто", // Settings_Option_AutoFit
    L"Инвертировать действие колёсика", // Settings_Label_InvertWheel
    L"Задержка привязки зума (100%)", // Settings_Label_ZoomSnapDamping
    L"Масштабировать окно от позиции мыши", // Settings_Label_MouseAnchorZoom
    L"Масштаб правой кнопкой мыши", // Settings_Label_RightButtonDragZoom
    L"Скорость зума колёсиком", // Settings_Label_WheelZoomSpeed
    L"Боковое колёсико", // Settings_Label_ThumbWheel
    L"Скорость зума правой кнопкой", // Settings_Label_RightDragZoomSpeed
    L"Скорость зума (временно): ", // OSD_WheelZoomSpeed
    L"Временно настроить скорость зума", // Help_Action_AdjustZoomSpeed
    L"Временно заблокировать масштаб окна", // Help_Action_LockWindowZoom
    L"Инвертировать действие боковых кнопок", // Settings_Label_InvertButtons
    L"Использовать фиксированные уровни масштабирования", // Settings_Label_UseFixedZoom
    L"Когда включено, Alt + колесико выполняет обычное масштабирование вместо изменения скорости.", // Settings_Tooltip_UseFixedZoom
    L"  └  Пользовательские уровни", // Settings_Label_FixedZoomLevels
    L"Редактировать масштабы", // Dialog_FixedZoomTitle
    L"Введите масштабы через запятую (например, 0.5, 1, 2):", // Dialog_FixedZoomMsg
    L"Увеличить", // Settings_Label_ZoomModeIn
    L"Уменьшить", // Settings_Label_ZoomModeOut
    L"Перетаскивание левой кнопкой", // Settings_Label_LeftDrag
    L"Перетаскивание средней кнопкой", // Settings_Label_MiddleDrag
    L"Щелчок средней кнопкой", // Settings_Label_MiddleClick
    L"Навигация при щелчке по краям", // Settings_Label_EdgeNavClick
    L"Отключить в режиме сравнения", // Settings_Label_DisableEdgeNavInCompare
    L"Индикатор навигации", // Settings_Label_NavIndicator
    L"Автоповорот (EXIF)", // Settings_Label_AutoRotate
    L"Управление цветом (CMS)", // Settings_Label_CMS
    L"Расширенный цвет (HDR)", // Settings_Label_AdvancedColor
    L"Тональная компрессия HDR", // Settings_Label_HdrToneMapping
    L"Точка перегиба сплайна", // Settings_Label_HdrSplineKnee
    L"Стратегия тональной компрессии HDR:\nОпределяет, как отображаются " L"HDR-изображения, превышающие возможности монитора.\nСплайн: " L"Высокоточное сжатие светов с использованием сплайнов (рекомендуется).\n" L"Колориметрическая: Строгое отображение яркости; светлые участки, " L"превышающие предел монитора, обрезаются.\nBT.2390 (EETF): Кривая ITU-R BT.2390 EETF для высокоточного отображения тонов.", // Settings_Tooltip_HdrToneMapping
    L"0 = Авто (рассчитывается на основе яркости изображения).\nЗначение представляет собой соотношение пиковой яркости монитора. Яркость ниже этой точки перегиба отображается в соотношении 1:1, а яркость выше плавно сжимается с помощью сплайновой кривой.\n(Рекомендуется: 0.4 - 0.75)", // Settings_Tooltip_HdrSplineKnee
    L"Пиковая яркость HDR (ниты)", // Settings_Label_HdrPeakNitsOverride
    L"Установите 0 для системной яркости.", // Settings_Tooltip_HdrPeakNitsOverride
    L"Пиковый процентиль HDR", // Settings_Label_HdrPeakPercentile
    L"Удаление слишком ярких пикселов для повышения общей яркости (mpv по умолчанию: 99.995%).", // Settings_Tooltip_HdrPeakPercentile
    L"100% (абсолютный пик)", // Settings_Option_HdrPeakPercentile_100
    L"99.995% (стабильный)", // Settings_Option_HdrPeakPercentile_99995
    L"99.9% (агрессивный)", // Settings_Option_HdrPeakPercentile_999
    L"Диапазон обесцвечивания светов HDR", // Settings_Label_HdrDesatThreshold
    L"Порог начала обесцвечивания светлых участков. 0.0 - обесцвечивать все света, 1.0 - не обесцвечивать. Рекомендуемое значение: 0.18.", // Settings_Tooltip_HdrDesatThreshold
    L"Интенсивность обесцвечивания светов HDR", // Settings_Label_HdrMaxDesat
    L"Максимальная интенсивность обесцвечивания экстремальных светов. 0.0 - не обесцвечивать, 1.0 - полностью до белого. Рекомендуемое значение: 0.75.", // Settings_Tooltip_HdrMaxDesat
    L"Колориметрическая", // Settings_Option_HdrColorimetric
    L"Сплайн", // Settings_Option_HdrSpline
    L"BT.2390 (EETF)", // Settings_Option_HdrLegacyReinhard
    L"Запасной профиль без тегов", // Settings_Label_CmsFallback
    L"Профиль цветопробы (.icc)", // Settings_Label_CustomProof
    L"Предпросмотр цветопробы", // Context_SoftProofing
    L"Профиль цветопробы", // Context_SoftProofProfile
    L"Свой...", // Context_SoftProofCustom
    L"Скоро", // Settings_Value_ComingSoon
    L"Принудительное декодирование RAW", // Settings_Label_ForceRaw
    L"Связывание RAW+JPEG", // Settings_Label_PairRawJpeg
    L"Показывать RAW и одноимённый снимок камеры (JPEG/HEIF) как одну фотографию.\nВ списке остаётся только готовое изображение с пометкой формата RAW (например, +CR3); время съёмки проверяется в фоне, несовпадающие пары разделяются.\nКнопка RAW на панели или её горячая клавиша переключает между снимком камеры и декодированным RAW; горячая клавиша парного сравнения показывает обе версии рядом.", // Settings_Tooltip_PairRawJpeg
    L"Экспозиция (яркость)", // Settings_Label_Exposure
    L"Регулировка яркости изображения (компенсация экспозиции). Диапазон: 0.18x - 10.0x.", // Settings_Tooltip_Exposure
    L"Добавить в 'Открыть с помощью'", // Settings_Label_AddToOpenWith
    L"Пользовательский редактор", // Settings_Label_CustomEditor
    L"Выбрать редактор", // Context_SelectEditor
    L"Не удалось запустить редактор. Настройте его снова.", // OSD_EditorLaunchFailed
    L"Добавить", // Settings_Action_Add
    L"Добавлено", // Settings_Action_Added
    L"Отключено в портативном режиме", // Settings_Status_DisabledInPortable
    L"Включить отладочный HUD (F12)", // Settings_Label_DebugHUD
    L"Система предзагрузки", // Settings_Label_Prefetch
    L"Панель информации", // Settings_Label_InfoPanelAlpha
    L"Панель инструментов", // Settings_Label_ToolbarAlpha
    L"Настройки", // Settings_Label_SettingsAlpha
    L"Сбросить все настройки", // Settings_Label_Reset
    L"Восстановить", // Settings_Action_Restore
    L"Готово", // Settings_Action_Done
    L"Неуправляемый (быстро)", // Settings_Option_CmsUnmanaged
    L"sRGB (стандарт)", // Settings_Option_CmssRGB
    L"Display P3 (широкий охват)", // Settings_Option_CmsP3
    L"Adobe RGB (1998)", // Settings_Option_CmsAdobeRGB
    L"Оттенки серого (контроль тона)", // Settings_Option_CmsGray
    L"ProPhoto RGB", // Settings_Option_CmsProPhoto
    L"Цель рендеринга", // Settings_Label_CmsIntent
    L"Определение выхода за охват", // Settings_Label_GamutWarning
    L"Анализ и выделение областей вне цветового охвата. Режимы: Выкл, только в режиме цветопробы (по умолчанию), либо для цветопробы и монитора одновременно.", // Settings_Tooltip_GamutWarning
    L"Автоуведомление об ошибке охвата", // Settings_Label_GamutAutoPrompt
    L"Показать OSD-уведомление при обнаружении ошибок охвата. Выделение можно включить вручную на панели инструментов.", // Settings_Tooltip_GamutAutoPrompt
    L"Цвет выделения вне охвата", // Settings_Label_GamutColor
    L"Относительный колориметрический (точность)", // Settings_Option_CmsIntentRelative
    L"Перцептивный (восприятие)", // Settings_Option_CmsIntentPerceptual
    L"Использовать систему управления цветом (CMS).\nЕсли включено, применяется высокоточное преобразование цветового пространства через ГП для восстановления истинных цветов.\nЕсли отключено, снижается нагрузка на ГП, но возможно перенасыщение цветов на дисплеях с широким цветовым охватом.", // Settings_Tooltip_CMS
    L"Метод преобразования цветового пространства (цель рендеринга).\nПерцептивный: сжимает цвета вне охвата для сохранения деталей и градиентов (идеально для фото).\nОтносительный колориметрический: сохраняет цвета в пределах охвата и обрезает выходящие за его пределы (идеально для интерфейса и значков).\n* Визуальные различия проявляются только при использовании улучшенных профилей ICC, содержащих LUT (таблицы поиска). Стандартные матричные профили автоматически возвращаются к относительной колориметрии.", // Settings_Tooltip_CmsIntent
    L"Использовать 16-разрядный конвейер рендеринга с плавающей запятой (scRGB).\nЕсли включено, идеально отображаются яркие участки фотографий на HDR-дисплеях, без ограничения SDR.\nЕсли отключено, изображение принудительно отображается как SDR.\n* При включении увеличивается потребление видеопамяти.", // Settings_Tooltip_AdvancedColor
    L"Авто: масштаб 100%, если изображение меньше экрана, и вписывание в экран, если больше.", // Settings_Tooltip_ZoomAuto
    L"Проверить наличие новой версии", // Settings_Action_CheckUpdates
    L"Посмотреть обновление", // Settings_Action_ViewUpdate
    L"Проверка...", // Settings_Status_Checking
    L"Актуальная версия", // Settings_Status_UpToDate
    L"Репозиторий GitHub", // Settings_Link_GitHub
    L"Сообщить о проблеме", // Settings_Link_ReportIssue
    L"Горячие клавиши (F1)", // Settings_Link_Hotkeys
    L"Версия", // Settings_Label_Version
    L"Сборка", // Settings_Label_Build
    L"Чёрный", // Settings_Option_Black
    L"Белый", // Settings_Option_White
    L"Сетка", // Settings_Option_Grid
    L"Свой", // Settings_Option_Custom
    L"Выкл", // Settings_Option_Off
    L"Вкл", // Settings_Option_On
    L"Кратко", // Settings_Option_Lite
    L"Полностью", // Settings_Option_Full
    L"Только большие", // Settings_Option_LargeOnly
    L"Все", // Settings_Option_All
    L"Цветопроба", // Settings_Option_SoftProofing
    L"Окно", // Settings_Option_Window
    L"Панорамирование", // Settings_Option_Pan
    L"Нет", // Settings_Option_None
    L"Выход", // Settings_Option_Exit
    L"Стрелка", // Settings_Option_Arrow
    L"Курсор", // Settings_Option_Cursor
    L"Вручную", // Settings_Option_Manual
    L"Авто (Проводник)", // Settings_Option_SortAuto
    L"Имя", // Settings_Option_SortName
    L"Дата изменения", // Settings_Option_SortModified
    L"Дата съёмки (EXIF)", // Settings_Option_SortDateTaken
    L"Размер", // Settings_Option_SortSize
    L"Тип", // Settings_Option_SortType
    L"Цикл в папке", // Settings_Option_NavLoop
    L"Через вложенные папки", // Settings_Option_NavThrough
    L"Линейный (простое сглаживание)", // Settings_Option_Linear
    L"По соседним (макс. резкость)", // Settings_Option_Nearest
    L"HQ кубический (макс. сглаживание)", // Settings_Option_HighQualityCubic
    L"Авто", // Settings_Option_ZoomAuto
    L"Авто", // Settings_Option_Auto
    L"Эко", // Settings_Option_Eco
    L"Баланс", // Settings_Option_Balanced
    L"Ультра", // Settings_Option_Ultra
    L"Горячие клавиши", // Help_Header_Shortcuts
    L"Мышь", // Help_Header_Mouse
    L"След./пред. изображение", // Help_Item_NextPrev
    L"Масштабирование", // Help_Item_Zoom
    L"Панорамирование", // Help_Item_Pan
    L"Поворот", // Help_Item_Rotate
    L"По размеру экрана", // Help_Item_Fit
    L"Удалить изображение", // Help_Item_Delete
    L"Полный экран", // Help_Item_Fullscreen
    L"Закрыть", // Help_Item_Close
    L"Левая кнопка", // Help_Mouse_Left
    L"Средняя кнопка", // Help_Mouse_Middle
    L"Колёсико", // Help_Mouse_Wheel
    L"Правая кнопка", // Help_Mouse_Right
    L"Вертикальное перетаскивание правой кнопкой", // Help_Mouse_RightVerticalDrag
    L"Перемещение окна / Выход из полноэкранного режима / Выход из максимизированного", // Help_Action_MoveWindow
    L"Панорамирование", // Help_Action_PanImage
    L"Контекстное меню", // Help_Action_ContextMenu
    L"След./Пред.", // Help_Action_NextPrev
    L"Масштаб", // Help_Action_Zoom
    L"Умный масштаб (100% / По размеру / Восстановить)", // Help_Action_SmartZoom
    L"Скопировать изображение", // Help_Desc_Copy
    L"Изменить", // Help_Desc_Edit
    L"Советы и термины", // Help_Header_Tips
    L"* Горячие клавиши и контекстное меню действуют только на текущий процесс. Настройки не изменяются.", // Help_Tip_ContextScope
    L"Поворот: При оптимизации краёв они слегка обрезаются, чтобы вписать в границы блока " L"(без потерь). В режиме с потерями выполняется полное перекодирование изображения.", // Help_Tip_Rotation
    L"Видеостена (Ctrl+F11): Распределение картинки по всем мониторам. Если кнопка закрытия скрыта, " L"для выхода дважды щёлкните мышью.", // Help_Tip_VideoWall
    L"Режим кальки/плёнки: Если включено, изображение становится полупрозрачным, открывая нижележащие элементы. Размер и прозрачность настраиваются. Включите на панели инструментов режим сквозных щелчков мышью, в котором всё, кроме Shift+Esc, игнорируется - QuickView становится прозрачным наложением.", // Help_Tip_DesignerMode
    L"Предупреждение о цветовом охвате: Обнаружение цветов вне охвата для монитора или профиля цветопробы. Режимы: Выкл/Только цветопроба/Все (по умолчанию: цветопроба). Переключение - через панель инструментов.", // Help_Tip_GamutDetection
    L"RAW: По умолчанию QuickView показывает встроенную картинку предпросмотра. Нажмите кнопку RAW " L"для полного декодирования (может выглядеть по-другому из-за параметров рендеринга).", // Help_Tip_Raw
    L"Качество JPEG: Ориентировочное значение качества. Может слегка " L"отличаться от настройки сохранения из-за различий в алгоритме " L"(например, Photoshop 100% \u2248 98%).", // Help_Tip_JpegQ
    L"Сравнение цветопробы: Вход в режим сравнения при включённой цветопробе автоматически сравнит оригинал и пробу.", // Help_Tip_SoftProofCompare
    L"Доступна новая версия!", // Dialog_UpdateTitle
    L"Доступна версия %s.", // Dialog_UpdateContent
    L"История изменений", // Dialog_UpdateLogHeader
    L"Обновить", // Dialog_ButtonUpdate
    L"Позже", // Dialog_ButtonLater
    L"Звезда на GitHub", // Dialog_ButtonStar
    L"QuickView создан с любовью", // Dialog_Update_LoveTitle
    L"Я разрабатываю QuickView в свободное время и считаю, что " L"в Windows должен быть более быстрый и эффективный просмотрщик. Если вам " L"нравится эта программа, пожалуйста, поставьте звезду на " L"GitHub или расскажите о QuickView другу.", // Dialog_Update_LoveMessage
    L"Режим сравнения", // Help_Item_Compare
    L"Первое/последнее изображение", // Help_Item_FirstLast
    L"ФИЗИЧЕСКИЕ АТРИБУТЫ", // HUD_Group_Physical
    L"НАУЧНОЕ КАЧЕСТВО", // HUD_Group_Scientific
    L"ОПТИКА И КОДИРОВАНИЕ", // HUD_Group_Encoding
    L"Определение границ (Лапласова дисперсия)", // HUD_Tip_Sharp_Desc
    L"Чёткие края, высокая детализация", // HUD_Tip_Sharp_High
    L"Мягкий фокус или размытие в движении", // HUD_Tip_Sharp_Low
    L"> 500 - очень резко", // HUD_Tip_Sharp_Ref
    L"Плотность информации (энтропия Шеннона)", // HUD_Tip_Ent_Desc
    L"Сложные текстуры или высокий уровень шума", // HUD_Tip_Ent_High
    L"Плоские участки или низкая детализация", // HUD_Tip_Ent_Low
    L"7.0-8.0 - высокая детализация", // HUD_Tip_Ent_Ref
    L"Бит на пиксел (эффективность сжатия)", // HUD_Tip_BPP_Desc
    L"Низкая эффективность (сохраняется больше данных)", // HUD_Tip_BPP_High
    L"Высокая эффективность (более плотное сжатие)", // HUD_Tip_BPP_Low
    L"24.0 (Raw RGB), ~2.0-3.0 (High JPEG), ~0.5-1.5 (WebP/AVIF)", // HUD_Tip_BPP_Ref
    L"Высокая: ", // HUD_Label_High
    L"Низкая: ", // HUD_Label_Low
    L"Эталон: ", // HUD_Label_Ref
    L"Диафильм из галереи (при наведении сверху)", // Settings_Header_GalleryTrigger
    L"Режим триггера", // Settings_Label_GalleryTriggerMode
    L"Автонаведение", // Settings_Option_GalleryTriggerAuto
    L"Наведение на точку", // Settings_Option_GalleryTriggerDelay
    L"Щелчок по точке", // Settings_Option_GalleryTriggerClick
    L"Отключено", // Settings_Option_GalleryTriggerDisable
    L"Эта функция автоматически отключается, если размер окна меньше 600x450 пкс.", // Settings_Tooltip_GalleryTrigger

    L"Слайд-шоу запущено", // OSD_SlideshowStarted
    L"Слайд-шоу остановлено", // OSD_SlideshowStopped
    L"Слайд-шоу возобновлено", // OSD_SlideshowResumed
    L"Слайд-шоу на паузе", // OSD_SlideshowPaused
    L"Эффект: Прожектор", // OSD_ImmersiveSpotlight
    L"Эффект: Обычный", // OSD_ImmersiveNormal
    L"Задание на печать отправлено. Вы можете продолжить просмотр.", // OSD_PrintJobStarted
    L"Задание на печать успешно отправлено.", // OSD_PrintJobFinished
    L"Режим слайд-шоу", // Context_SlideshowMode
    L"Интервал (сек)", // Settings_Label_SlideshowInterval
    L"Режим погружения", // Settings_Label_SlideshowImmersive
    L"Настройка краткой инфо-панели", // Settings_Label_CustomLiteInfoPanel
    L"Элементы в обычном режиме", // Settings_Label_ItemsInNormalMode
    L"Элементы в режиме сравнения", // Settings_Label_ItemsInCompareMode
    L"Шаблон разделителя", // Settings_Label_SeparatorPreset
    L"Обычный", // Settings_Option_SlideshowNormal
    L"Прожектор", // Settings_Option_SlideshowSpotlight
    L"Форма экранной лупы", // Settings_Label_LoupeShape
    L"Квадрат", // Settings_Option_LoupeShapeSquare
    L"Круг", // Settings_Option_LoupeShapeCircle
    L"Удерживайте сочетание клавиш и прокручивайте колесико мыши, чтобы настроить размер лупы.", // Settings_Tooltip_LoupeHotkey
};

// ----------------------------------------------------------------
// DE Table
// ----------------------------------------------------------------
static const LanguageTable Table_DE = {
    L"Kein Bild geladen", // OSD_NoImage
    L"Verlustfrei", // OSD_Lossless
    L"Neu kodiert (verlustfrei)", // OSD_ReencodedLossless
    L"Kantenoptimiert", // OSD_EdgeAdapted
    L"Neu kodiert", // OSD_Reencoded
    L"Zugriff verweigert - Datei wird verwendet oder ist schreibgeschützt", // OSD_ReadOnly
    L"Transformation nicht perfekt (Kantenoptimiert)", // OSD_NotPerfect
    L"Video Wall: ON", // OSD_SpanOn
    L"Video Wall: OFF", // OSD_SpanOff
    L"Vorher (Original)", // OSD_CompareBefore
    L"Nachher (Proof)", // OSD_CompareAfter
    L"Löschen rückgängig gemacht", // OSD_UndoDeleteSuccess
    L"Rückgängig machen fehlgeschlagen", // OSD_UndoDeleteFailed
    L"Umbenennen rückgängig gemacht", // OSD_UndoRenameSuccess
    L"Rückgängig machen fehlgeschlagen", // OSD_UndoRenameFailed
    L"Drehung/Spiegelung rückgängig gemacht", // OSD_UndoTransformSuccess
    L"Rückgängig machen fehlgeschlagen", // OSD_UndoTransformFailed
    L"Farbraum-Überschreitung erkannt", // OSD_GamutDetected
    L"Farbraum: Inkompatibles Profil", // OSD_GamutIncompatible
    L"Farbraum: Analyse fehlgeschlagen", // OSD_GamutFailed
    L"90\x00B0 im Uhrzeigersinn drehen", // Action_RotateCW
    L"90\x00B0 gegen Uhrzeigersinn drehen", // Action_RotateCCW
    L"180\x00B0 drehen", // Action_Rotate180
    L"Horizontal spiegeln", // Action_FlipH
    L"Vertikal spiegeln", // Action_FlipV
    L"Änderungen speichern?", // Dialog_SaveTitle
    L"Das Bild wurde geändert. Möchten Sie die Änderungen speichern?", // Dialog_SaveContent
    L"Speichern", // Dialog_ButtonSave
    L"Speichern unter...", // Dialog_ButtonSaveAs
    L"Verwerfen", // Dialog_ButtonDiscard
    L"Fortfahren", // Dialog_ButtonContinue
    L"Immer verlustfrei speichern", // Checkbox_AlwaysSaveLossless
    L"Immer kantenoptimiert speichern", // Checkbox_AlwaysSaveEdgeAdapted
    L"Immer neu kodiert speichern", // Checkbox_AlwaysSaveLossy
    L"Ohne Bestätigung in den Papierkorb verschieben", // Checkbox_NeverConfirmDelete
    L"HEIC kann nicht dekodiert werden - HEVC-Erweiterung installieren", // OSD_HEICCodecMissing
    L"HEIC kann nicht dekodiert werden", // Dialog_HEICTitle
    L"Ihr System hat keine HEVC-Erweiterung.\\nQuickView nutzt " L"Hardware-Beschleunigung für beste Leistung.", // Dialog_HEICContent
    L"Erweiterung holen (kostenlos)", // Dialog_HEICGetExtension
    L"Abbrechen", // Dialog_Cancel
    L"Allgemein", // Settings_Tab_General
    L"Über", // Settings_Tab_About
    L"Grundlagen", // Settings_Group_Foundation
    L"Start", // Settings_Group_Startup
    L"Gewohnheiten", // Settings_Group_Habits
    L"Sprache", // Settings_Label_Language
    L"Einzelinstanz", // Settings_Label_SingleInstance
    L"Updates prüfen", // Settings_Label_CheckUpdates
    L"Update-Kanal", // Settings_Label_UpdateChannel
    L"Stabil", // Settings_Option_UpdateStable
    L"Vorabversion (Pre-release)", // Settings_Option_UpdatePreRelease
    L"Stabil: Wird veröffentlicht, nachdem sich eine Vorabversion als stabil erwiesen hat.\nVorabversion (Pre-release): Liefert sofort die neuesten Code-Änderungen, um Ihr Feedback zu sammeln. Hinweis: Diese Änderungen sind nicht streng getestet.", // Settings_Tooltip_PreRelease
    L"Endlosnavigation", // Settings_Label_NavLoopMode
    L"Sortierreihenfolge", // Settings_Label_SortOrder
    L"Absteigend", // Settings_Label_SortDescending
    L"Archive immer nach Namen sortieren", // Settings_Label_SortArchivesByNameAscending
    L"Vor dem Löschen bestätigen", // Settings_Label_ConfirmDel
    L"Portabler Modus / Bereinigung", // Settings_Label_Portable
    L"Portabler Modus und Registry-Bereinigung:\nWenn aktiviert, wird QuickView im " L"portablen Modus ausgeführt. Es bereinigt automatisch vorhandene " L"Registry-Verknüpfungen, deaktiviert automatische Registry-Änderungen und " L"speichert Konfigurationsdateien im Anwendungsverzeichnis anstatt in AppData.", // Settings_Tooltip_Portable
    L"Span Displays (Video Wall)", // Settings_Label_SpanDisplays
    L"UI-Skalierung", // Settings_Label_UIScale
    L"Neustart erforderlich", // Settings_Status_RestartRequired
    L"Keine Schreibrechte!", // Settings_Status_NoWritePerm
    L"Aktiviert", // Settings_Status_Enabled
    L"Powered by", // Settings_Header_PoweredBy
    L"Öffnen...\tStrg+O", // Context_Open
    L"Öffnen mit...", // Context_OpenWith
    L"Bearbeiten\tE", // Context_Edit
    L"Im Explorer anzeigen", // Context_ShowInExplorer
    L"Ordner öffnen", // Context_OpenFolder
    L"Bild kopieren\tStrg+C", // Context_CopyImage
    L"Pfad kopieren\tStrg+Alt+C", // Context_CopyPath
    L"Drucken\tStrg+P", // Context_Print
    L"90\x00B0 im Uhrzeigersinn\tR", // Context_RotateCW
    L"90\x00B0 gegen Uhrzeigersinn\tShift+R", // Context_RotateCCW
    L"Horizontal spiegeln\tH", // Context_FlipH
    L"Vertikal spiegeln\tV", // Context_FlipV
    L"Transformieren", // Context_Transform
    L"Originalgröße (100%)\t1 / Z", // Context_ActualSize
    L"An Bildschirm anpassen\t0 / F", // Context_FitToScreen
    L"An Fenster anpassen", // Context_FitWindow
    L"Fenster ausfüllen", // Context_FillWindow
    L"Vergrößern\t+ / Strg +", // Context_ZoomIn
    L"Verkleinern\t- / Strg -", // Context_ZoomOut
    L"Fenstergröße sperren", // Context_LockWindow
    L"Immer im Vordergrund\tStrg+T", // Context_AlwaysOnTop
    L"HUD-Galerie\tT", // Context_HUDGallery
    L"Kompaktes Info-Panel\tTab", // Context_LiteInfoPanel
    L"Vollständiges Info-Panel\tI", // Context_FullInfoPanel
    L"RAW rendern", // Context_RenderRAW
    L"Pixel-Art-Modus", // Context_PixelArtMode
    L"Farbraum", // Context_ColorSpace
    L"Vollbild\tF11", // Context_Fullscreen
    L"Ansicht", // Context_View
    L"Ausfüllen", // Context_WallpaperFill
    L"Anpassen", // Context_WallpaperFit
    L"Kacheln", // Context_WallpaperTile
    L"Als Hintergrundbild setzen", // Context_SetAsWallpaper
    L"Umbenennen\tF2", // Context_Rename
    L"Erweiterung reparieren", // Context_FixExtension
    L"Löschen\tEntf", // Context_Delete
    L"Löschen rückgängig machen\tCtrl+Z", // Context_UndoDelete
    L"Umbenennen rückgängig machen\tCtrl+Z", // Context_UndoRename
    L"Drehung/Spiegelung rückgängig machen\tCtrl+Z", // Context_UndoTransform
    L"Rückgängig machen\tCtrl+Z", // Context_Undo
    L"Sortieren nach", // Context_SortBy
    L"Navigationsreihenfolge", // Context_NavOrder
    L"Aufsteigend", // Context_SortAscending
    L"Absteigend", // Context_SortDescending
    L"Einstellungen...", // Context_Settings
    L"Vergleichsmodus\tC", // Context_CompareMode
    L"Pausmodus\tCtrl+Shift+O", // Context_OverlayMode
    L"Im Vergleichsmodus öffnen", // Context_GalleryOpenCompare
    L"In neuem Fenster öffnen", // Context_GalleryOpenNewWindow
    L"Beenden\tMButton/Esc", // Context_Exit
    L"Maus-Passthrough beenden", // Menu_ExitPassthrough
    L"Maus-Passthrough-Modus", // Dialog_PassthroughTitle
    L"Mausereignisse werden an das darunterliegende Fenster weitergegeben.\nBeenden nur über globalen Hotkey (Shift+Esc) oder Taskleisten-Menü möglich.\n\nFortfahren?", // Dialog_PassthroughContent
    L"Durchklicken: AN (Shift+Esc zum Beenden)", // OSD_PassthroughOn
    L"Durchklicken: AUS", // OSD_PassthroughOff
    L"Pausmodus: AN", // OSD_OverlayModeOn
    L"Pausmodus: AUS", // OSD_OverlayModeOff
    L"Deckkraft", // OSD_Opacity
    L"Vergrößern (Pause)", // Toolbar_Tooltip_OverlayZoomIn
    L"Verkleinern (Pause)", // Toolbar_Tooltip_OverlayZoomOut
    L"Deckkraft erhöhen", // Toolbar_Tooltip_OverlayAlphaUp
    L"Deckkraft verringern", // Toolbar_Tooltip_OverlayAlphaDown
    L"Maus-Passthrough aktivieren", // Toolbar_Tooltip_OverlayPassthroughOn
    L"Maus-Passthrough deaktivieren", // Toolbar_Tooltip_OverlayPassthroughOff
    L"Pausmodus beenden", // Toolbar_Tooltip_OverlayExit
    L"Fehler", // Message_SaveErrorTitle
    L"Datei konnte nicht gespeichert werden. Datei gesperrt?", // Message_SaveErrorContent
    L"Vorheriges (Links)", // Toolbar_Tooltip_Prev
    L"Nächstes (Rechts)", // Toolbar_Tooltip_Next
    L"Links drehen (Shift+R)", // Toolbar_Tooltip_RotateL
    L"Rechts drehen (R)", // Toolbar_Tooltip_RotateR
    L"Horizontal spiegeln (H)", // Toolbar_Tooltip_FlipH
    L"Fenster sperren (temp)", // Toolbar_Tooltip_Lock
    L"Fenster entsperren", // Toolbar_Tooltip_Unlock
    L"Galerie (T)", // Toolbar_Tooltip_Gallery
    L"Info-Panel", // Toolbar_Tooltip_Info
    L"RAW: Vorschau (Klick für Voll)", // Toolbar_Tooltip_RawPreview
    L"RAW: Volle Dekodierung (Klick für Vorschau)", // Toolbar_Tooltip_RawFull
    L"RAW: Gepaartes RAW anzeigen (volle Dekodierung)", // Toolbar_Tooltip_RawPairView
    L"RAW: Zurück zum gepaarten Bild", // Toolbar_Tooltip_RawPairBack
    L"Erweiterung stimmt nicht (Reparieren)", // Toolbar_Tooltip_FixExtension
    L"Symbolleiste anheften", // Toolbar_Tooltip_Pin
    L"Symbolleiste lösen", // Toolbar_Tooltip_Unpin
    L"Farbraumwarnung anzeigen", // Toolbar_Tooltip_GamutWarning
    L"Normaler Modus", // Toolbar_Tooltip_NormalMode
    L"Vergleichsmodus", // Toolbar_Tooltip_CompareMode
    L"Einzelseiten-Modus", // Toolbar_Tooltip_SinglePage
    L"Doppelseiten-Modus", // Toolbar_Tooltip_DualPage
    L"Neues Bild in Auswahl öffnen", // Toolbar_Tooltip_CompareOpen
    L"Links/Rechts tauschen", // Toolbar_Tooltip_CompareSwap
    L"Layout umschalten", // Toolbar_Tooltip_CompareLayout
    L"Vergleichsinformationen", // Toolbar_Tooltip_CompareInfo
    L"Ausgewähltes Bild löschen", // Toolbar_Tooltip_CompareDelete
    L"Vergrößern (Feineinstellung)", // Toolbar_Tooltip_CompareZoomIn
    L"Verkleinern (Feineinstellung)", // Toolbar_Tooltip_CompareZoomOut
    L"Zoom-Sync: AN", // Toolbar_Tooltip_CompareSyncZoomOn
    L"Zoom-Sync: AUS", // Toolbar_Tooltip_CompareSyncZoomOff
    L"Pan-Sync: AN", // Toolbar_Tooltip_CompareSyncPanOn
    L"Pan-Sync: AUS", // Toolbar_Tooltip_CompareSyncPanOff
    L"Spotlight-Effekt umschalten", // Toolbar_Tooltip_SlideshowImmersiveToggle
    L"Diashow beenden", // Toolbar_Tooltip_SlideshowExit
    L"Vergleich beenden", // Toolbar_Tooltip_CompareExit
    L"Animation abspielen", // Toolbar_Tooltip_AnimPlay
    L"Animation pausieren", // Toolbar_Tooltip_AnimPause
    L"Vorheriger Frame", // Toolbar_Tooltip_AnimPrev
    L"Nächster Frame", // Toolbar_Tooltip_AnimNext
    L"Dirty Rect Debug: AN", // Toolbar_Tooltip_AnimDirtyOn
    L"Dirty Rect Debug: AUS", // Toolbar_Tooltip_AnimDirtyOff
    L"Animationsgeschwindigkeit", // Toolbar_Tooltip_AnimSpeed
    L"Leistung", // Settings_Header_Performance
    L"Profi-Werkzeuge", // Settings_Header_Professional
    L"Memory Reclaim Strategy:", // Settings_Label_MemoryReclaim
    L"Smart (Auto)", // Settings_Option_MemSmart
    L"Aggressive (Max Perf)", // Settings_Option_MemAggressive
    L"On-Demand (Min RAM)", // Settings_Option_MemOnDemand
    L"Smart: Balance performance and RAM.\nAggressive: Maximize performance, high memory usage.\nOn-Demand: Release memory immediately when idle.", // Settings_Tooltip_MemoryReclaim
    L"Schaltfläche \"Dirty Rect\" im Animationsmodus anzeigen", // Settings_Label_ShowDirtyRect
    L"Debug-Schaltfläche für Dirty Rects in der Animations-Symbolleiste anzeigen.", // Settings_Tooltip_ShowDirtyRect
    L"Kopiert!", // OSD_Copied
    L"Koordinaten kopiert!", // OSD_CoordinatesCopied
    L"Dateipfad kopiert!", // OSD_FilePathCopied
    L"Zoom: 100%", // OSD_Zoom100
    L"Zoom: An Bildschirm anpassen", // OSD_ZoomFit
    L"Zoom: An Fenster anpassen", // OSD_ZoomFitWindow
    L"Zoom: Fenster ausfüllen", // OSD_ZoomFill
    L"Drucken: Benutzen Sie Strg+P in der geöffneten App", // OSD_PrintInstruction
    L"In Papierkorb verschoben", // OSD_MovedToRecycleBin
    L"Fenster gesperrt", // OSD_WindowLocked
    L"Fenster entsperrt", // OSD_WindowUnlocked
    L"Immer im Vordergrund: AN", // OSD_AlwaysOnTopOn
    L"Immer im Vordergrund: AUS", // OSD_AlwaysOnTopOff
    L"Hintergrundbild gesetzt", // OSD_WallpaperSet
    L"Hintergrundbild setzen fehlgeschlagen", // OSD_WallpaperFailed
    L"Umbenannt", // OSD_Renamed
    L"Umbenennung fehlgeschlagen", // OSD_RenameFailed
    L"Erweiterung repariert", // OSD_ExtensionFixed
    L"Wiederhergestellt", // OSD_Restored
    L"Erstes Bild", // OSD_FirstImage
    L"Letztes Bild", // OSD_LastImage
    L"HD", // OSD_HD
    L"Zoom: ", // OSD_ZoomPrefix
    L"Spielt ab", // OSD_AnimPlaying
    L"Pausiert (Alt+Links/Rechts zum Suchen)", // OSD_AnimPaused
    L"Dirty Rect: AN", // OSD_AnimDirtyOn
    L"Dirty Rect: AUS", // OSD_AnimDirtyOff
    L"Span Displays (Video Wall)\tCtrl+F11", // Context_SpanDisplays
    L"Interface", // Settings_Tab_Visuals
    L"Steuerung", // Settings_Tab_Controls
    L"Bild", // Settings_Tab_Image
    L"Erweitert", // Settings_Tab_Advanced
    L"Hintergrund", // Settings_Header_Backdrop
    L"Fenster", // Settings_Header_Window
    L"Fenstersperre", // Settings_Header_WindowLock
    L"Panel", // Settings_Header_Panel
    L"Maus", // Settings_Header_Mouse
    L"Rand", // Settings_Header_Edge
    L"Rendering", // Settings_Header_Render
    L"HDR", // Settings_Header_Hdr
    L"Eingabeaufforderungen", // Settings_Header_Prompts
    L"System", // Settings_Header_System
    L"Funktionen", // Settings_Header_Features
    L"Transparenz", // Settings_Header_Transparency
    L"Glas-Engine (GPU)", // Settings_Header_GeekGlass
    L"Glas-Effekte", // Settings_Label_EnableGeekGlass
    L"UI Animations", // Settings_Label_GlassUIAnimations
    L"Material", // Settings_Header_CoreMaterial
    L"Unschärfe", // Settings_Label_BlurSigma
    L"Glass Disabled (System)", // Settings_Status_GlassDisabled
    L"Dichte", // Settings_Label_TintDensity
    L"Overall color intensity of the glass frost effect.", // Settings_Tooltip_TintDensity
    L"Reflex (Specular)", // Settings_Label_SpecularOpacity
    L"Brightness of the diagonal lighting reflections.", // Settings_Tooltip_SpecularOpacity
    L"Shadow Depth", // Settings_Label_ShadowIntensity
    L"Strength of the ambient occlusion shadows.", // Settings_Tooltip_ShadowIntensity
    L"Vector Rendering", // Settings_Header_VectorAssets
    L"Icon Stroke weight", // Settings_Label_VectorStrokeWeight
    L"Standard (1.5px)", // Settings_Option_StrokeStandard
    L"Fine (1.0px)", // Settings_Option_StrokeFine
    L"Tint Profile", // Settings_Header_GlassTint
    L"Color logic", // Settings_Label_TintProfile
    L"Auto (Adaptive)", // Settings_Option_TintAuto
    L"Custom Color", // Settings_Option_TintCustom
    L"Manual Tint", // Settings_Label_GlassCustomColor
    L"Control Surface Density (%)", // Settings_Header_DensityMatrix
    L"OSD & HUD", // Settings_Label_OsdDensity
    L"Transparency for small floating overlays.", // Settings_Tooltip_OsdDensity
    L"Toolbar & Sidebars", // Settings_Label_PanelsDensity
    L"Transparency for persistent control panels.", // Settings_Tooltip_PanelsDensity
    L"Dialogs & Settings", // Settings_Label_ModalsDensity
    L"Transparency for centered popups.", // Settings_Tooltip_ModalsDensity
    L"Menus", // Settings_Label_MenusDensity
    L"Transparency for right-click context menus.", // Settings_Tooltip_MenusDensity
    L"Theme", // Settings_Tab_Theme
    L"Preset", // Settings_Label_ThemeMode
    L"System", // Settings_Option_ThemeAuto
    L"Dunkel", // Settings_Option_ThemeDark
    L"Hell", // Settings_Option_ThemeLight
    L"Design", // Settings_Option_ThemeCustom
    L"Hintergrund abdunkeln", // Settings_Label_AmbientDimmer
    L"Dim the background when a modal or settings window is open.", // Settings_Tooltip_AmbientDimmer
    L"Accent Color", // Settings_Label_AccentColor
    L"Text Color", // Settings_Label_TextColor
    L"Theme Engine", // Settings_Header_ThemeManagement
    L"Export", // Settings_Action_ExportTheme
    L"Import", // Settings_Action_ImportTheme
    L"Leinwandfarbe", // Settings_Label_CanvasColor
    L"Überlagerung", // Settings_Label_Overlay
    L"Raster anzeigen", // Settings_Label_ShowGrid
    L"Bildübergang ausblenden", // Settings_Label_CrossFade
    L"Immer im Vordergrund", // Settings_Label_AlwaysOnTop
    L"Fenstergröße sperren", // Settings_Label_LockWindow
    L"Legt fest, ob das Programm beim Start standardmäßig den Fensterrahmen sperrt, anstatt der Bildskalierung zu folgen.", // Settings_Tooltip_LockWindow
    L"Titelleiste automatisch ausblenden", // Settings_Label_AutoHideTitle
    L"Abgerundete Ecken", // Settings_Label_RoundedCorners
    L"UI-Rahmen anzeigen", // Settings_Label_UIBorders
    L"Controls window and context menu rounded corners. Requires Windows 11.", // Settings_Tooltip_RoundedCorners
    L"Untere Symbolleiste sperren", // Settings_Label_LockToolbar
    L"Minimale Fensterbreite", // Settings_Label_WindowMinSize
    L"Maximale Startgröße (%)", // Settings_Label_WindowMaxSizePercent
    L"Randindikatoren anzeigen", // Settings_Label_ShowBorderIndicator
    L"Zeigt eine blaue Indikatorlinie in der Richtung an, in der das Bild die Fenstergrenzen überschreitet.", // Settings_Tooltip_ShowBorderIndicator
    L"Minimap anzeigen", // Settings_Label_ShowNavigator
    L"Auto", // Settings_Option_NavigatorAuto
    L"An", // Settings_Option_NavigatorOn
    L"Aus", // Settings_Option_NavigatorOff
    L"Fenstergröße bei Navigation beibehalten", // Settings_Label_KeepWindowSizeOnNav
    L"Letzte Fensterposition und -größe merken", // Settings_Label_RememberLastWindowSizeAndPosition
    L"Kleine Bilder anpassen", // Settings_Label_UpscaleSmallImagesWhenLocked
    L"Flüssige Fensterskalierung (GPU)", // Settings_Label_EnableSmoothScaling
    L"EXIF-Panel-Modus", // Settings_Label_ExifMode
    L"Symbolleisten-Info Standard", // Settings_Label_ToolbarInfoDefault
    L"Im Vollbildmodus öffnen", // Settings_Label_OpenFullScreenMode
    L"Vollbild-Zoom-Modus", // Settings_Label_FullScreenZoomMode
    L"An Bildschirm anpassen", // Settings_Option_FitScreen
    L"Auto", // Settings_Option_AutoFit
    L"Mausrad invertieren", // Settings_Label_InvertWheel
    L"Zoom 100% Einrast-Dämpfung", // Settings_Label_ZoomSnapDamping
    L"Fensterzoom am Mauszeiger ausrichten", // Settings_Label_MouseAnchorZoom
    L"Zoom mit Rechtsziehen", // Settings_Label_RightButtonDragZoom
    L"Mausrad-Zoomgeschwindigkeit", // Settings_Label_WheelZoomSpeed
    L"Daumenrad", // Settings_Label_ThumbWheel
    L"Rechtszieh-Zoomgeschwindigkeit", // Settings_Label_RightDragZoomSpeed
    L"Zoomgeschwindigkeit (temp): ", // OSD_WheelZoomSpeed
    L"Zoomgeschwindigkeit temporär anpassen", // Help_Action_AdjustZoomSpeed
    L"Fensterzoom vorübergehend sperren", // Help_Action_LockWindowZoom
    L"Seitentasten invertieren", // Settings_Label_InvertButtons
    L"Feste Zoomstufen verwenden", // Settings_Label_UseFixedZoom
    L"Wenn aktiviert, führt Alt + Rad einen normalen Zoom aus, anstatt die Zoomgeschwindigkeit zu ändern.", // Settings_Tooltip_UseFixedZoom
    L"  └  Benutzerdefinierte Zoomstufen", // Settings_Label_FixedZoomLevels
    L"Zoomstufen bearbeiten", // Dialog_FixedZoomTitle
    L"Geben Sie kommagetrennte Zoomstufen ein (z. B. 0.5, 1, 2):", // Dialog_FixedZoomMsg
    L"Vergrößerungsmodus", // Settings_Label_ZoomModeIn
    L"Verkleinerungsmodus", // Settings_Label_ZoomModeOut
    L"Links ziehen", // Settings_Label_LeftDrag
    L"Mitte ziehen", // Settings_Label_MiddleDrag
    L"Mittelklick", // Settings_Label_MiddleClick
    L"Randnavigation-Klick", // Settings_Label_EdgeNavClick
    L"Im Vergleichsmodus deaktivieren", // Settings_Label_DisableEdgeNavInCompare
    L"Navigationsanzeige", // Settings_Label_NavIndicator
    L"Automatisch drehen (EXIF)", // Settings_Label_AutoRotate
    L"Farbmanagement (CMS)", // Settings_Label_CMS
    L"Erweiterte Farbe (HDR)", // Settings_Label_AdvancedColor
    L"HDR-Tonzuordnung", // Settings_Label_HdrToneMapping
    L"Spline Knee Point", // Settings_Label_HdrSplineKnee
    L"HDR Tonzuordnungsstrategie:\nLegt fest, wie HDR-Bilder angezeigt werden, " L"wenn sie die Fähigkeiten des Monitors überschreiten.\nSpline: " L"Hochpräzises Highlight-Roll-off mittels stückweiser Splines " L"(Empfohlen).\nFarbmetrisch: Strikte Luminanzzuordnung; Highlights, die " L"das Monitorlimit überschreiten, werden abgeschnitten.\nBT.2390 (EETF): ITU-R BT.2390 EETF-Kurve für hochpräzise Tonzuordnung.", // Settings_Tooltip_HdrToneMapping
    L"0 = Auto (Calculated based on image luminance).\nThe value represents the ratio of the monitor's peak luminance. Brightness below this knee point maps 1:1, while brightness above is smoothly compressed using a spline curve.\n(Recommended: 0.4 - 0.75)", // Settings_Tooltip_HdrSplineKnee
    L"HDR Spitzenhelligkeit (Nits)", // Settings_Label_HdrPeakNitsOverride
    L"Auf 0 setzen, um erkannte Helligkeit zu verwenden.", // Settings_Tooltip_HdrPeakNitsOverride
    L"HDR Peak Percentile", // Settings_Label_HdrPeakPercentile
    L"Discard extremely bright pixels to raise overall brightness (mpv default: 99.995%).", // Settings_Tooltip_HdrPeakPercentile
    L"100% (Absolute Peak)", // Settings_Option_HdrPeakPercentile_100
    L"99.995% (Stable)", // Settings_Option_HdrPeakPercentile_99995
    L"99.9% (Aggressive)", // Settings_Option_HdrPeakPercentile_999
    L"HDR-Highlights Entsättigungsbereich", // Settings_Label_HdrDesatThreshold
    L"Startschwellenwert für die Entsättigung von Highlights. 0,0 bedeutet Entsättigung aller Helligkeitsstufen, 1,0 bedeutet keine Entsättigung. Empfohlener Standardwert ist 0,18.", // Settings_Tooltip_HdrDesatThreshold
    L"HDR-Highlights Entsättigungsstärke", // Settings_Label_HdrMaxDesat
    L"Maximale Intensität der Entsättigung bei extremen Highlights. 0,0 bedeutet keine Entsättigung, 1,0 bedeutet vollständige Entsättigung zu Weiß. Empfohlener Standardwert ist 0,75.", // Settings_Tooltip_HdrMaxDesat
    L"Farbmetrisch", // Settings_Option_HdrColorimetric
    L"Spline", // Settings_Option_HdrSpline
    L"BT.2390 (EETF)", // Settings_Option_HdrLegacyReinhard
    L"Fallback für Bilder ohne Tags", // Settings_Label_CmsFallback
    L"Softproof-Profil (.icc)", // Settings_Label_CustomProof
    L"Softproof-Vorschau", // Context_SoftProofing
    L"Proof-Profil", // Context_SoftProofProfile
    L"Benutzerdefiniert...", // Context_SoftProofCustom
    L"Demnächst", // Settings_Value_ComingSoon
    L"RAW erzwingen", // Settings_Label_ForceRaw
    L"RAW+JPEG-Paarung", // Settings_Label_PairRawJpeg
    L"Eine RAW-Datei und das gleichnamige Kamerabild (JPEG/HEIF) als ein Foto anzeigen.\nIn der Liste bleibt nur das gerenderte Bild, markiert mit dem RAW-Format (z. B. +CR3); Aufnahmezeiten werden im Hintergrund geprüft, abweichende Paare werden getrennt.\nMit dem RAW-Button oder seinem Hotkey zwischen Kamerabild und RAW-Dekodierung wechseln, mit dem Paarvergleich-Hotkey beide nebeneinander ansehen.", // Settings_Tooltip_PairRawJpeg
    L"Belichtung (Helligkeit)", // Settings_Label_Exposure
    L"Passt die Bildhelligkeit an (Belichtungskorrektur). Bereich: 0.18x bis 10.0x.", // Settings_Tooltip_Exposure
    L"Zu Öffnen mit hinzufügen", // Settings_Label_AddToOpenWith
    L"Benutzerdefinierter Editor", // Settings_Label_CustomEditor
    L"Editor auswählen", // Context_SelectEditor
    L"Editor konnte nicht gestartet werden. Bitte neu konfigurieren.", // OSD_EditorLaunchFailed
    L"Hinzufügen", // Settings_Action_Add
    L"Hinzugefügt", // Settings_Action_Added
    L"Im portablen Modus deaktiviert", // Settings_Status_DisabledInPortable
    L"Debug-HUD aktivieren (F12)", // Settings_Label_DebugHUD
    L"Vorlade-System", // Settings_Label_Prefetch
    L"Info-Panel", // Settings_Label_InfoPanelAlpha
    L"Symbolleiste", // Settings_Label_ToolbarAlpha
    L"Einstellungen", // Settings_Label_SettingsAlpha
    L"Alle Einstellungen zurücksetzen", // Settings_Label_Reset
    L"Wiederherstellen", // Settings_Action_Restore
    L"Fertig", // Settings_Action_Done
    L"Unverwaltet (schnell)", // Settings_Option_CmsUnmanaged
    L"sRGB (Standard)", // Settings_Option_CmssRGB
    L"Display P3 (Breites Farbspektrum)", // Settings_Option_CmsP3
    L"Adobe RGB (1998)", // Settings_Option_CmsAdobeRGB
    L"Graustufen (Tonwertkontrolle)", // Settings_Option_CmsGray
    L"ProPhoto RGB", // Settings_Option_CmsProPhoto
    L"Rendering-Intent", // Settings_Label_CmsIntent
    L"Farbraumwarnung-Erkennung", // Settings_Label_GamutWarning
    L"Analysiert und markiert Farbbereiche außerhalb des Farbraums bei Softproofing oder wenn das Bild den Bildschirmfarbraum überschreitet.", // Settings_Tooltip_GamutWarning
    L"Automatische Benachrichtigung bei Farbraumfehlern", // Settings_Label_GamutAutoPrompt
    L"OSD-Benachrichtigung bei Farbraumfehlern anzeigen. Markierungen können manuell über die Symbolleiste umgeschaltet werden.", // Settings_Tooltip_GamutAutoPrompt
    L"Farbraumwarnung-Highlight-Farbe", // Settings_Label_GamutColor
    L"Relativ farbmetrisch", // Settings_Option_CmsIntentRelative
    L"Perzeptiv", // Settings_Option_CmsIntentPerceptual
    L"Farbmanagementsystem (CMS) aktivieren.\nWenn aktiviert, wird eine hochpräzise Farbraumkonvertierung über die GPU angewendet, um echte Farben wiederherzustellen.\nDeaktivieren verringert die GPU-Auslastung, kann aber auf Displays mit großem Farbraum zu übersättigten Farben führen.", // Settings_Tooltip_CMS
    L"Rendering-Intent für die Farbraumkonvertierung.\nPerzeptiv: Komprimiert Farben außerhalb des Farbraums, um Details und Farbverläufe zu erhalten (ideal für Fotos).\nRelativ farbmetrisch: Behält Farben im Farbraum bei und schneidet die anderen ab (ideal für UI und Symbole).\nHinweis: Visuelle Unterschiede treten nur bei Verwendung fortgeschrittener ICC-Profile mit LUTs (Lookup Tables) auf. Standard-Matrix-Profile fallen automatisch auf 'Relativ farbmetrisch' zurück.", // Settings_Tooltip_CmsIntent
    L"16-Bit-Gleitkomma-Rendering-Pipeline (scRGB) aktivieren.\nWenn aktiviert, werden die Lichter von Fotos auf HDR-fähigen Displays perfekt gerendert, indem das SDR-Limit überschritten wird.\nDeaktivieren erzwingt die Zuordnung zur SDR-Ausgabe.\nHinweis: Aktivieren erhöht die VRAM-Nutzung.", // Settings_Tooltip_AdvancedColor
    L"Auto: 100 % Skalierung, wenn das Bild kleiner als der Bildschirm ist, und an den Bildschirm anpassen, wenn es größer ist.", // Settings_Tooltip_ZoomAuto
    L"Nach Updates suchen", // Settings_Action_CheckUpdates
    L"Update anzeigen", // Settings_Action_ViewUpdate
    L"Prüfe...", // Settings_Status_Checking
    L"Aktuell", // Settings_Status_UpToDate
    L"GitHub-Repository", // Settings_Link_GitHub
    L"Problem melden", // Settings_Link_ReportIssue
    L"Tastenkürzel", // Settings_Link_Hotkeys
    L"Version", // Settings_Label_Version
    L"Build", // Settings_Label_Build
    L"Schwarz", // Settings_Option_Black
    L"Weiß", // Settings_Option_White
    L"Raster", // Settings_Option_Grid
    L"Benutzerdefiniert", // Settings_Option_Custom
    L"Aus", // Settings_Option_Off
    L"Ein", // Settings_Option_On
    L"Kompakt", // Settings_Option_Lite
    L"Vollständig", // Settings_Option_Full
    L"Nur Große", // Settings_Option_LargeOnly
    L"Alle", // Settings_Option_All
    L"Softproof", // Settings_Option_SoftProofing
    L"Fenster", // Settings_Option_Window
    L"Schwenken", // Settings_Option_Pan
    L"Keine", // Settings_Option_None
    L"Beenden", // Settings_Option_Exit
    L"Pfeil", // Settings_Option_Arrow
    L"Cursor", // Settings_Option_Cursor
    L"Manuell", // Settings_Option_Manual
    L"Auto (Explorer)", // Settings_Option_SortAuto
    L"Name", // Settings_Option_SortName
    L"Änderungsdatum", // Settings_Option_SortModified
    L"Aufnahmedatum (EXIF)", // Settings_Option_SortDateTaken
    L"Größe", // Settings_Option_SortSize
    L"Typ", // Settings_Option_SortType
    L"Schleife im Ordner", // Settings_Option_NavLoop
    L"Durch Unterordner", // Settings_Option_NavThrough
    L"Linear: Grundglättung", // Settings_Option_Linear
    L"Nächster: Extreme Schärfe", // Settings_Option_Nearest
    L"HQ Kubisch: Extreme Glättung", // Settings_Option_HighQualityCubic
    L"Auto", // Settings_Option_ZoomAuto
    L"Auto", // Settings_Option_Auto
    L"Öko", // Settings_Option_Eco
    L"Ausgewogen", // Settings_Option_Balanced
    L"Ultra", // Settings_Option_Ultra
    L"Tastenkürzel", // Help_Header_Shortcuts
    L"Mausaktionen", // Help_Header_Mouse
    L"Nächstes/Vorheriges Bild", // Help_Item_NextPrev
    L"Zoomen", // Help_Item_Zoom
    L"Bild schwenken", // Help_Item_Pan
    L"Drehen", // Help_Item_Rotate
    L"An Bildschirm anpassen", // Help_Item_Fit
    L"Bild löschen", // Help_Item_Delete
    L"Vollbild", // Help_Item_Fullscreen
    L"Schließen", // Help_Item_Close
    L"Linke Taste", // Help_Mouse_Left
    L"Mittlere Taste", // Help_Mouse_Middle
    L"Mausrad", // Help_Mouse_Wheel
    L"Rechte Taste", // Help_Mouse_Right
    L"Rechte Taste vertikal ziehen", // Help_Mouse_RightVerticalDrag
    L"Fenster bewegen / Vollbild beenden / Maximierung aufheben", // Help_Action_MoveWindow
    L"Bild schwenken", // Help_Action_PanImage
    L"Kontextmenü", // Help_Action_ContextMenu
    L"Weiter/Zurück", // Help_Action_NextPrev
    L"Zoom", // Help_Action_Zoom
    L"Smart-Zoom (100% / Anpassen / Wiederherstellen)", // Help_Action_SmartZoom
    L"Copy Image", // Help_Desc_Copy
    L"Edit", // Help_Desc_Edit
    L"Tips & Glossary", // Help_Header_Tips
    L"Note: Shortcuts and context menu actions affect the current process " L"only. Settings are permanent.", // Help_Tip_ContextScope
    L"Rotation: 'Edge Adapted' means minor cropping to fit block boundaries " L"(lossless data). 'Lossy' means full re-encoding.", // Help_Tip_Rotation
    L"Video Wall (Ctrl+F11): Spans all monitors. If close button is hidden, " L"double-click to exit.", // Help_Tip_VideoWall
    L"Pausmodus / Folienmodus: Sobald aktiviert, wird das Bild halbtransparent und gibt den Blick auf darunter liegende Elemente frei. Sie können die Größe oder Transparenz anpassen. Klicken Sie auf den Maus-Durchlass-Umschalter in der Symbolleiste, um in den Durchlassmodus zu gelangen, in dem alle Eingaben außer Umschalt+Esc ignoriert werden, wodurch QuickView zu einer transparenten Überlagerung wird.", // Help_Tip_DesignerMode
    L"Farbraumwarnung: Erkennt Farben außerhalb des Farbumfangs für den Zielmonitor oder das Softproof-Profil. Modi: Aus, nur Softproof oder Alle (Standard: Softproof). Umschaltbar über die Symbolleiste.", // Help_Tip_GamutDetection
    L"RAW Button: QuickView shows embedded preview by default. Click to " L"fully decode (may look different due to rendering parameters).", // Help_Tip_Raw
    L"JPEG Quality: Estimated Q value (reverse engineered). May differ " L"slightly from save setting due to algorithm variations.", // Help_Tip_JpegQ
    L"Softproof-Vergleich: Wenn der Vergleichsmodus bei aktiviertem Softproof aufgerufen wird, werden Original und Proof-Bild automatisch verglichen.", // Help_Tip_SoftProofCompare
    L"Neue Version verfügbar!", // Dialog_UpdateTitle
    L"v%s is ready.", // Dialog_UpdateContent
    L"Was ist neu:", // Dialog_UpdateLogHeader
    L"Jetzt aktualisieren", // Dialog_ButtonUpdate
    L"Später", // Dialog_ButtonLater
    L"Stern auf GitHub", // Dialog_ButtonStar
    L"QuickView wird mit Liebe entwickelt", // Dialog_Update_LoveTitle
    L"Ich pflege QuickView in meiner Freizeit, weil ich glaube, dass Windows " L"einen schnelleren und saubereren Viewer verdient. Wenn Ihnen dieses " L"Update gefällt, ist der größte Beitrag, uns auf GitHub einen Stern zu " L"geben.", // Dialog_Update_LoveMessage
    L"Compare Mode", // Help_Item_Compare
    L"Erstes / Letztes Bild", // Help_Item_FirstLast
    L"PHYSICAL ATTRIBUTES", // HUD_Group_Physical
    L"SCIENTIFIC QUALITY", // HUD_Group_Scientific
    L"OPTICS & ENCODING", // HUD_Group_Encoding
    L"Edge definition (Laplacian Variance)", // HUD_Tip_Sharp_Desc
    L"Crisp edges, high detail", // HUD_Tip_Sharp_High
    L"Soft focus or motion blur", // HUD_Tip_Sharp_Low
    L"> 500 is very sharp", // HUD_Tip_Sharp_Ref
    L"Information density (Shannon Entropy)", // HUD_Tip_Ent_Desc
    L"Complex textures or high noise", // HUD_Tip_Ent_High
    L"Flat areas or low detail", // HUD_Tip_Ent_Low
    L"7.0-8.0 is high detail", // HUD_Tip_Ent_Ref
    L"Bits Per Pixel (Compression Efficiency)", // HUD_Tip_BPP_Desc
    L"Lower efficiency (more data preserved)", // HUD_Tip_BPP_High
    L"Higher efficiency (higher compression)", // HUD_Tip_BPP_Low
    L"24.0 (Raw RGB), ~2.0-3.0 (High JPEG), ~0.5-1.5 (WebP/AVIF)", // HUD_Tip_BPP_Ref
    L"High: ", // HUD_Label_High
    L"Low: ", // HUD_Label_Low
    L"Ref: ", // HUD_Label_Ref
    L"Galerie-Filmstreifen (Hover oben)", // Settings_Header_GalleryTrigger
    L"Trigger-Modus", // Settings_Label_GalleryTriggerMode
    L"Automatischer Hover", // Settings_Option_GalleryTriggerAuto
    L"Hotspot-Hover", // Settings_Option_GalleryTriggerDelay
    L"Hotspot-Klick", // Settings_Option_GalleryTriggerClick
    L"Deaktiviert", // Settings_Option_GalleryTriggerDisable
    L"Diese Funktion wird automatisch deaktiviert, wenn das Fenster kleiner als 600x450 ist.", // Settings_Tooltip_GalleryTrigger

    L"Diashow gestartet", // OSD_SlideshowStarted
    L"Diashow gestoppt", // OSD_SlideshowStopped
    L"Diashow fortgesetzt", // OSD_SlideshowResumed
    L"Diashow pausiert", // OSD_SlideshowPaused
    L"Immersiv: Spotlight", // OSD_ImmersiveSpotlight
    L"Immersiv: Normal", // OSD_ImmersiveNormal
    L"Druckauftrag übermittelt. Sie können weiterblättern.", // OSD_PrintJobStarted
    L"Druckauftrag erfolgreich gesendet.", // OSD_PrintJobFinished
    L"Diashow-Modus", // Context_SlideshowMode
    L"Intervall (Sekunden)", // Settings_Label_SlideshowInterval
    L"Immersiver Modus", // Settings_Label_SlideshowImmersive
    L"Kompakte Infoleiste anpassen", // Settings_Label_CustomLiteInfoPanel
    L"Elemente im Normalmodus", // Settings_Label_ItemsInNormalMode
    L"Elemente im Vergleichsmodus", // Settings_Label_ItemsInCompareMode
    L"Trennzeichen-Vorlage", // Settings_Label_SeparatorPreset
    L"Normal", // Settings_Option_SlideshowNormal
    L"Spotlight", // Settings_Option_SlideshowSpotlight
    L"Lupenform", // Settings_Label_LoupeShape
    L"Quadrat", // Settings_Option_LoupeShapeSquare
    L"Kreis", // Settings_Option_LoupeShapeCircle
    L"Halten Sie die Tastenkombination gedrückt und scrollen Sie mit dem Mausrad, um die Lupengröße anzupassen.", // Settings_Tooltip_LoupeHotkey
};

// ----------------------------------------------------------------
// ES Table
// ----------------------------------------------------------------
static const LanguageTable Table_ES = {
    L"No hay imagen cargada", // OSD_NoImage
    L"Sin pérdida", // OSD_Lossless
    L"Recodificado (sin pérdida)", // OSD_ReencodedLossless
    L"Bordes optimizados", // OSD_EdgeAdapted
    L"Recodificado", // OSD_Reencoded
    L"Acceso denegado - el archivo está en uso o es de solo lectura", // OSD_ReadOnly
    L"Transformación no perfecta (bordes optimizados)", // OSD_NotPerfect
    L"Video Wall: ON", // OSD_SpanOn
    L"Video Wall: OFF", // OSD_SpanOff
    L"Antes (Original)", // OSD_CompareBefore
    L"Después (Prueba)", // OSD_CompareAfter
    L"Eliminación deshecha", // OSD_UndoDeleteSuccess
    L"Error al deshacer eliminación", // OSD_UndoDeleteFailed
    L"Nombre cambiado deshecho", // OSD_UndoRenameSuccess
    L"Error al deshacer cambio de nombre", // OSD_UndoRenameFailed
    L"Giro/volteo deshecho", // OSD_UndoTransformSuccess
    L"Error al deshacer giro/volteo", // OSD_UndoTransformFailed
    L"Colores fuera de gama detectados", // OSD_GamutDetected
    L"Gama: Perfil incompatible", // OSD_GamutIncompatible
    L"Gama: Error en el análisis", // OSD_GamutFailed
    L"Girar 90\x00B0 en sentido horario", // Action_RotateCW
    L"Girar 90\x00B0 en sentido antihorario", // Action_RotateCCW
    L"Girar 180\x00B0", // Action_Rotate180
    L"Voltear horizontal", // Action_FlipH
    L"Voltear vertical", // Action_FlipV
    L"¿Guardar cambios?", // Dialog_SaveTitle
    L"La imagen ha sido modificada. ¿Desea guardar los cambios?", // Dialog_SaveContent
    L"Guardar", // Dialog_ButtonSave
    L"Guardar como...", // Dialog_ButtonSaveAs
    L"Descartar", // Dialog_ButtonDiscard
    L"Continuar", // Dialog_ButtonContinue
    L"Siempre guardar sin pérdida", // Checkbox_AlwaysSaveLossless
    L"Siempre guardar con bordes optimizados", // Checkbox_AlwaysSaveEdgeAdapted
    L"Siempre guardar recodificado", // Checkbox_AlwaysSaveLossy
    L"Enviar directamente a la papelera sin confirmar", // Checkbox_NeverConfirmDelete
    L"No se puede decodificar HEIC - Instale la extensión HEVC", // OSD_HEICCodecMissing
    L"No se puede decodificar HEIC", // Dialog_HEICTitle
    L"Su sistema no tiene la extensión HEVC.\\nQuickView usa aceleración de " L"hardware para mejor rendimiento.", // Dialog_HEICContent
    L"Obtener extensión (gratis)", // Dialog_HEICGetExtension
    L"Cancelar", // Dialog_Cancel
    L"General", // Settings_Tab_General
    L"Acerca de", // Settings_Tab_About
    L"Fundamentos", // Settings_Group_Foundation
    L"Inicio", // Settings_Group_Startup
    L"Hábitos", // Settings_Group_Habits
    L"Idioma", // Settings_Label_Language
    L"Instancia única", // Settings_Label_SingleInstance
    L"Buscar actualizaciones", // Settings_Label_CheckUpdates
    L"Canal de actualización", // Settings_Label_UpdateChannel
    L"Estable", // Settings_Option_UpdateStable
    L"Versión preliminar (Pre-release)", // Settings_Option_UpdatePreRelease
    L"Estable: Se publica después de que una versión preliminar demuestre ser estable.\nVersión preliminar (Pre-release): Entrega los últimos cambios de código de inmediato para recopilar sus comentarios. Nota: Estos cambios no han sido probados estrictamente.", // Settings_Tooltip_PreRelease
    L"Navegación en bucle", // Settings_Label_NavLoopMode
    L"Orden de clasificación", // Settings_Label_SortOrder
    L"Descendente", // Settings_Label_SortDescending
    L"Ordenar archivos siempre por nombre", // Settings_Label_SortArchivesByNameAscending
    L"Confirmar antes de eliminar", // Settings_Label_ConfirmDel
    L"Modo portátil / Limpieza", // Settings_Label_Portable
    L"Modo portátil y limpieza del registro:\nCuando está habilitado, QuickView se " L"ejecuta en modo portátil. Limpiará automáticamente las asociaciones de " L"registro existentes, deshabilitará la modificación automática del registro y " L"almacenará los archivos de configuración en el directorio de la aplicación en " L"lugar de AppData.", // Settings_Tooltip_Portable
    L"Span Displays (Video Wall)", // Settings_Label_SpanDisplays
    L"Escala de interfaz", // Settings_Label_UIScale
    L"Reinicio requerido", // Settings_Status_RestartRequired
    L"¡Sin permisos de escritura!", // Settings_Status_NoWritePerm
    L"Habilitado", // Settings_Status_Enabled
    L"Desarrollado con", // Settings_Header_PoweredBy
    L"Abrir...\tCtrl+O", // Context_Open
    L"Abrir con...", // Context_OpenWith
    L"Editar\tE", // Context_Edit
    L"Mostrar en Explorador", // Context_ShowInExplorer
    L"Abrir carpeta", // Context_OpenFolder
    L"Copiar imagen\tCtrl+C", // Context_CopyImage
    L"Copiar ruta\tCtrl+Alt+C", // Context_CopyPath
    L"Imprimir\tCtrl+P", // Context_Print
    L"Girar 90\x00B0 horario\tR", // Context_RotateCW
    L"Girar 90\x00B0 antihorario\tShift+R", // Context_RotateCCW
    L"Voltear horizontal\tH", // Context_FlipH
    L"Voltear vertical\tV", // Context_FlipV
    L"Transformar", // Context_Transform
    L"Tamaño real (100%)\t1 / Z", // Context_ActualSize
    L"Ajustar a pantalla\t0 / F", // Context_FitToScreen
    L"Ajustar a ventana", // Context_FitWindow
    L"Rellenar ventana", // Context_FillWindow
    L"Acercar\t+ / Ctrl +", // Context_ZoomIn
    L"Alejar\t- / Ctrl -", // Context_ZoomOut
    L"Bloquear ventana", // Context_LockWindow
    L"Siempre visible\tCtrl+T", // Context_AlwaysOnTop
    L"Galería HUD\tT", // Context_HUDGallery
    L"Panel de info compacto\tTab", // Context_LiteInfoPanel
    L"Panel de info completo\tI", // Context_FullInfoPanel
    L"Renderizar RAW", // Context_RenderRAW
    L"Modo de Pixel Art", // Context_PixelArtMode
    L"Espacio de color", // Context_ColorSpace
    L"Pantalla completa\tF11", // Context_Fullscreen
    L"Ver", // Context_View
    L"Rellenar", // Context_WallpaperFill
    L"Ajustar", // Context_WallpaperFit
    L"Mosaico", // Context_WallpaperTile
    L"Establecer como fondo", // Context_SetAsWallpaper
    L"Renombrar\tF2", // Context_Rename
    L"Corregir extensión", // Context_FixExtension
    L"Eliminar\tSupr", // Context_Delete
    L"Deshacer eliminación\tCtrl+Z", // Context_UndoDelete
    L"Deshacer cambio de nombre\tCtrl+Z", // Context_UndoRename
    L"Deshacer rotar/voltear\tCtrl+Z", // Context_UndoTransform
    L"Deshacer\tCtrl+Z", // Context_Undo
    L"Ordenar por", // Context_SortBy
    L"Orden de navegación", // Context_NavOrder
    L"Ascendente", // Context_SortAscending
    L"Descendente", // Context_SortDescending
    L"Configuración...", // Context_Settings
    L"Modo comparación\tC", // Context_CompareMode
    L"Modo Calco\tCtrl+Shift+O", // Context_OverlayMode
    L"Abrir en modo de comparación", // Context_GalleryOpenCompare
    L"Abrir en una ventana nueva", // Context_GalleryOpenNewWindow
    L"Salir\tMButton/Esc", // Context_Exit
    L"Salir del modo de paso de ratón", // Menu_ExitPassthrough
    L"Modo de paso de ratón", // Dialog_PassthroughTitle
    L"Los eventos del ratón pasarán a la ventana de abajo.\nSolo se puede salir mediante el atajo global (Shift+Esc) o el menú de la barra de tareas.\n\n¿Continuar?", // Dialog_PassthroughContent
    L"Paso de ratón: ACTIVADO (Shift+Esc para salir)", // OSD_PassthroughOn
    L"Paso de ratón: DESACTIVADO", // OSD_PassthroughOff
    L"Modo calco: ACTIVADO", // OSD_OverlayModeOn
    L"Modo calco: DESACTIVADO", // OSD_OverlayModeOff
    L"Opacidad", // OSD_Opacity
    L"Acercar (Calco)", // Toolbar_Tooltip_OverlayZoomIn
    L"Alejar (Calco)", // Toolbar_Tooltip_OverlayZoomOut
    L"Aumentar opacidad", // Toolbar_Tooltip_OverlayAlphaUp
    L"Disminuir opacidad", // Toolbar_Tooltip_OverlayAlphaDown
    L"Activar paso de ratón", // Toolbar_Tooltip_OverlayPassthroughOn
    L"Desactivar paso de ratón", // Toolbar_Tooltip_OverlayPassthroughOff
    L"Salir del modo calco", // Toolbar_Tooltip_OverlayExit
    L"Error", // Message_SaveErrorTitle
    L"No se pudo guardar el archivo. ¿Está bloqueado?", // Message_SaveErrorContent
    L"Anterior (Izquierda)", // Toolbar_Tooltip_Prev
    L"Siguiente (Derecha)", // Toolbar_Tooltip_Next
    L"Girar izquierda (Shift+R)", // Toolbar_Tooltip_RotateL
    L"Girar derecha (R)", // Toolbar_Tooltip_RotateR
    L"Voltear horizontal (H)", // Toolbar_Tooltip_FlipH
    L"Bloquear ventana (temporal)", // Toolbar_Tooltip_Lock
    L"Desbloquear ventana", // Toolbar_Tooltip_Unlock
    L"Galería (T)", // Toolbar_Tooltip_Gallery
    L"Panel de información", // Toolbar_Tooltip_Info
    L"RAW: Vista previa (Clic para completo)", // Toolbar_Tooltip_RawPreview
    L"RAW: Decodificación completa (Clic para vista previa)", // Toolbar_Tooltip_RawFull
    L"RAW: Ver RAW emparejado (decodificación completa)", // Toolbar_Tooltip_RawPairView
    L"RAW: Volver a la imagen emparejada", // Toolbar_Tooltip_RawPairBack
    L"Extensión no coincide (Reparar)", // Toolbar_Tooltip_FixExtension
    L"Fijar barra de herramientas", // Toolbar_Tooltip_Pin
    L"Soltar barra de herramientas", // Toolbar_Tooltip_Unpin
    L"Mostrar áreas fuera de gama", // Toolbar_Tooltip_GamutWarning
    L"Modo normal", // Toolbar_Tooltip_NormalMode
    L"Modo comparación", // Toolbar_Tooltip_CompareMode
    L"Modo de página única", // Toolbar_Tooltip_SinglePage
    L"Modo de doble página", // Toolbar_Tooltip_DualPage
    L"Abrir nueva imagen en la selección", // Toolbar_Tooltip_CompareOpen
    L"Intercambiar izquierda/derecha", // Toolbar_Tooltip_CompareSwap
    L"Cambiar diseño", // Toolbar_Tooltip_CompareLayout
    L"Información de comparación", // Toolbar_Tooltip_CompareInfo
    L"Eliminar imagen seleccionada", // Toolbar_Tooltip_CompareDelete
    L"Aumentar (Ajuste fino)", // Toolbar_Tooltip_CompareZoomIn
    L"Reducir (Ajuste fino)", // Toolbar_Tooltip_CompareZoomOut
    L"Sincronizar zoom: ACTIVADO", // Toolbar_Tooltip_CompareSyncZoomOn
    L"Sincronizar zoom: DESACTIVADO", // Toolbar_Tooltip_CompareSyncZoomOff
    L"Sincronizar panorámica: ACTIVADO", // Toolbar_Tooltip_CompareSyncPanOn
    L"Sincronizar panorámica: DESACTIVADO", // Toolbar_Tooltip_CompareSyncPanOff
    L"Alternar efecto foco", // Toolbar_Tooltip_SlideshowImmersiveToggle
    L"Salir de la presentación de diapositivas", // Toolbar_Tooltip_SlideshowExit
    L"Salir de comparación", // Toolbar_Tooltip_CompareExit
    L"Reproducir animación", // Toolbar_Tooltip_AnimPlay
    L"Pausar animación", // Toolbar_Tooltip_AnimPause
    L"Marco anterior", // Toolbar_Tooltip_AnimPrev
    L"Marco siguiente", // Toolbar_Tooltip_AnimNext
    L"Depuración de Dirty Rect: ACTIVADO", // Toolbar_Tooltip_AnimDirtyOn
    L"Depuración de Dirty Rect: DESACTIVADO", // Toolbar_Tooltip_AnimDirtyOff
    L"Velocidad de animación", // Toolbar_Tooltip_AnimSpeed
    L"Rendimiento", // Settings_Header_Performance
    L"Herramientas profesionales", // Settings_Header_Professional
    L"Memory Reclaim Strategy:", // Settings_Label_MemoryReclaim
    L"Smart (Auto)", // Settings_Option_MemSmart
    L"Aggressive (Max Perf)", // Settings_Option_MemAggressive
    L"On-Demand (Min RAM)", // Settings_Option_MemOnDemand
    L"Smart: Balance performance and RAM.\nAggressive: Maximize performance, high memory usage.\nOn-Demand: Release memory immediately when idle.", // Settings_Tooltip_MemoryReclaim
    L"Mostrar el botón de rectángulo sucio en el modo de animación", // Settings_Label_ShowDirtyRect
    L"Mostrar botón de depuración de Dirty Rect en la barra de herramientas de animación.", // Settings_Tooltip_ShowDirtyRect
    L"¡Copiado!", // OSD_Copied
    L"¡Coordenadas copiadas!", // OSD_CoordinatesCopied
    L"¡Ruta del archivo copiada!", // OSD_FilePathCopied
    L"Zoom: 100%", // OSD_Zoom100
    L"Zoom: Ajustar a pantalla", // OSD_ZoomFit
    L"Zoom: Ajustar a ventana", // OSD_ZoomFitWindow
    L"Zoom: Rellenar ventana", // OSD_ZoomFill
    L"Imprimir: Use Ctrl+P en la aplicación abierta", // OSD_PrintInstruction
    L"Movido a la papelera", // OSD_MovedToRecycleBin
    L"Ventana bloqueada", // OSD_WindowLocked
    L"Ventana desbloqueada", // OSD_WindowUnlocked
    L"Siempre visible: ACTIVADO", // OSD_AlwaysOnTopOn
    L"Siempre visible: DESACTIVADO", // OSD_AlwaysOnTopOff
    L"Fondo de pantalla establecido", // OSD_WallpaperSet
    L"Error al establecer fondo de pantalla", // OSD_WallpaperFailed
    L"Renombrado", // OSD_Renamed
    L"Error al renombrar", // OSD_RenameFailed
    L"Extensión corregida", // OSD_ExtensionFixed
    L"Restaurado", // OSD_Restored
    L"Primera imagen", // OSD_FirstImage
    L"Última imagen", // OSD_LastImage
    L"HD", // OSD_HD
    L"Zoom: ", // OSD_ZoomPrefix
    L"Reproduciendo", // OSD_AnimPlaying
    L"Pausado (Alt+Izquierda/Derecha para buscar)", // OSD_AnimPaused
    L"Dirty Rect: ACTIVADO", // OSD_AnimDirtyOn
    L"Dirty Rect: DESACTIVADO", // OSD_AnimDirtyOff
    L"Span Displays (Video Wall)\tCtrl+F11", // Context_SpanDisplays
    L"Interfaz", // Settings_Tab_Visuals
    L"Controles", // Settings_Tab_Controls
    L"Imagen", // Settings_Tab_Image
    L"Avanzado", // Settings_Tab_Advanced
    L"Fondo", // Settings_Header_Backdrop
    L"Ventana", // Settings_Header_Window
    L"Bloqueo de ventana", // Settings_Header_WindowLock
    L"Panel", // Settings_Header_Panel
    L"Ratón", // Settings_Header_Mouse
    L"Borde", // Settings_Header_Edge
    L"Renderizado", // Settings_Header_Render
    L"HDR", // Settings_Header_Hdr
    L"Indicaciones", // Settings_Header_Prompts
    L"Sistema", // Settings_Header_System
    L"Funciones", // Settings_Header_Features
    L"Transparencia", // Settings_Header_Transparency
    L"Motor de cristal (GPU)", // Settings_Header_GeekGlass
    L"Efecto de cristal", // Settings_Label_EnableGeekGlass
    L"Animaciones de interfaz", // Settings_Label_GlassUIAnimations
    L"Material", // Settings_Header_CoreMaterial
    L"Radio de desenfoque", // Settings_Label_BlurSigma
    L"Efecto de cristal desactivado (Sistema)", // Settings_Status_GlassDisabled
    L"Capa de tinte", // Settings_Label_TintDensity
    L"Intensidad de color general del efecto de cristal.", // Settings_Tooltip_TintDensity
    L"Brillo (Especular)", // Settings_Label_SpecularOpacity
    L"Brillo de los reflejos de iluminación diagonal.", // Settings_Tooltip_SpecularOpacity
    L"Profundidad de sombra", // Settings_Label_ShadowIntensity
    L"Fuerza de las sombras de oclusión ambiental.", // Settings_Tooltip_ShadowIntensity
    L"Renderizado vectorial", // Settings_Header_VectorAssets
    L"Grosor de trazo de iconos", // Settings_Label_VectorStrokeWeight
    L"Estándar (1.5px)", // Settings_Option_StrokeStandard
    L"Fino (1.0px)", // Settings_Option_StrokeFine
    L"Perfil de tinte", // Settings_Header_GlassTint
    L"Lógica de color", // Settings_Label_TintProfile
    L"Automático (Adaptativo)", // Settings_Option_TintAuto
    L"Color personalizado", // Settings_Option_TintCustom
    L"Tinte manual", // Settings_Label_GlassCustomColor
    L"Densidad de superficie de control (%)", // Settings_Header_DensityMatrix
    L"OSD y HUD", // Settings_Label_OsdDensity
    L"Transparencia para pequeñas superposiciones flotantes.", // Settings_Tooltip_OsdDensity
    L"Barra de herramientas y paneles laterales", // Settings_Label_PanelsDensity
    L"Transparencia para paneles de control persistentes.", // Settings_Tooltip_PanelsDensity
    L"Diálogos y configuración", // Settings_Label_ModalsDensity
    L"Transparencia para ventanas emergentes centradas.", // Settings_Tooltip_ModalsDensity
    L"Menús", // Settings_Label_MenusDensity
    L"Transparencia para menús contextuales.", // Settings_Tooltip_MenusDensity
    L"Tema", // Settings_Tab_Theme
    L"Ajuste", // Settings_Label_ThemeMode
    L"Sistema", // Settings_Option_ThemeAuto
    L"Oscuro", // Settings_Option_ThemeDark
    L"Claro", // Settings_Option_ThemeLight
    L"Diseño", // Settings_Option_ThemeCustom
    L"Atenuador modal", // Settings_Label_AmbientDimmer
    L"Atenuar el fondo cuando hay una ventana modal o de configuración abierta.", // Settings_Tooltip_AmbientDimmer
    L"Color de acento", // Settings_Label_AccentColor
    L"Color de texto", // Settings_Label_TextColor
    L"Gestión de temas", // Settings_Header_ThemeManagement
    L"Exportar", // Settings_Action_ExportTheme
    L"Importar", // Settings_Action_ImportTheme
    L"Color del lienzo", // Settings_Label_CanvasColor
    L"Superposición", // Settings_Label_Overlay
    L"Mostrar cuadrícula", // Settings_Label_ShowGrid
    L"Desvanecimiento de transición de imagen", // Settings_Label_CrossFade
    L"Siempre visible", // Settings_Label_AlwaysOnTop
    L"Bloquear ventana", // Settings_Label_LockWindow
    L"Controla si el programa bloquea el borde de la ventana de forma predeterminada al inicio, sin seguir la escala de la imagen.", // Settings_Tooltip_LockWindow
    L"Ocultar barra de título", // Settings_Label_AutoHideTitle
    L"Esquinas Redondeadas", // Settings_Label_RoundedCorners
    L"Mostrar Bordes de UI", // Settings_Label_UIBorders
    L"Controla las esquinas redondeadas de las ventanas y los menús. Requiere Windows 11.", // Settings_Tooltip_RoundedCorners
    L"Bloquear barra inferior", // Settings_Label_LockToolbar
    L"Anchura mínima de ventana", // Settings_Label_WindowMinSize
    L"Tamaño máximo de inicio (%)", // Settings_Label_WindowMaxSizePercent
    L"Mostrar indicadores de borde", // Settings_Label_ShowBorderIndicator
    L"Muestra una línea indicadora azul en la dirección donde la imagen excede los límites de la ventana.", // Settings_Tooltip_ShowBorderIndicator
    L"Mostrar minimapa", // Settings_Label_ShowNavigator
    L"Auto", // Settings_Option_NavigatorAuto
    L"Activar", // Settings_Option_NavigatorOn
    L"Desactivar", // Settings_Option_NavigatorOff
    L"Mantener el tamaño de la ventana al navegar", // Settings_Label_KeepWindowSizeOnNav
    L"Recordar el último tamaño y posición de la ventana", // Settings_Label_RememberLastWindowSizeAndPosition
    L"Adaptar imágenes pequeñas", // Settings_Label_UpscaleSmallImagesWhenLocked
    L"Escalado suave de ventana (GPU)", // Settings_Label_EnableSmoothScaling
    L"Modo panel EXIF", // Settings_Label_ExifMode
    L"Info de barra por defecto", // Settings_Label_ToolbarInfoDefault
    L"Abrir en pantalla completa", // Settings_Label_OpenFullScreenMode
    L"Modo de zoom a pantalla completa", // Settings_Label_FullScreenZoomMode
    L"Ajustar a pantalla", // Settings_Option_FitScreen
    L"Auto", // Settings_Option_AutoFit
    L"Invertir rueda", // Settings_Label_InvertWheel
    L"Amortiguación de ajuste 100%", // Settings_Label_ZoomSnapDamping
    L"Zoom de ventana anclado al raton", // Settings_Label_MouseAnchorZoom
    L"Zoom con arrastre derecho", // Settings_Label_RightButtonDragZoom
    L"Velocidad de zoom con rueda", // Settings_Label_WheelZoomSpeed
    L"Rueda del pulgar", // Settings_Label_ThumbWheel
    L"Velocidad de zoom con arrastre derecho", // Settings_Label_RightDragZoomSpeed
    L"Velocidad de zoom (temporal): ", // OSD_WheelZoomSpeed
    L"Ajustar velocidad de zoom temporalmente", // Help_Action_AdjustZoomSpeed
    L"Bloquear temporalmente el zoom de la ventana", // Help_Action_LockWindowZoom
    L"Invertir botones laterales", // Settings_Label_InvertButtons
    L"Usar niveles de zoom fijos", // Settings_Label_UseFixedZoom
    L"Cuando está activado, Alt + Rueda realiza un zoom normal en lugar de cambiar la velocidad.", // Settings_Tooltip_UseFixedZoom
    L"  └  Niveles de zoom personalizados", // Settings_Label_FixedZoomLevels
    L"Editar niveles de zoom", // Dialog_FixedZoomTitle
    L"Introduzca niveles de zoom separados por comas (p. ej., 0.5, 1, 2):", // Dialog_FixedZoomMsg
    L"Modo de acercar", // Settings_Label_ZoomModeIn
    L"Modo de alejar", // Settings_Label_ZoomModeOut
    L"Arrastrar izquierdo", // Settings_Label_LeftDrag
    L"Arrastrar central", // Settings_Label_MiddleDrag
    L"Clic central", // Settings_Label_MiddleClick
    L"Clic navegación borde", // Settings_Label_EdgeNavClick
    L"Desactivar en modo de comparación", // Settings_Label_DisableEdgeNavInCompare
    L"Indicador navegación", // Settings_Label_NavIndicator
    L"Rotar automático (EXIF)", // Settings_Label_AutoRotate
    L"Gestión de color (CMS)", // Settings_Label_CMS
    L"Color avanzado (HDR)", // Settings_Label_AdvancedColor
    L"Mapeo de tonos HDR", // Settings_Label_HdrToneMapping
    L"Spline Knee Point", // Settings_Label_HdrSplineKnee
    L"Estrategia de mapeo de tonos (Tone Mapping) de HDR:\nDetermina cómo se " L"muestran las imágenes HDR cuando exceden las capacidades del monitor.\n" L"Spline: Roll-off de luces de alta fidelidad mediante splines por tramos " L"(Recomendado).\nColorimétrico: Mapeo de luminancia estricto; las luces " L"que exceden el límite del monitor se recortan.\nBT.2390 (EETF): Curva ITU-R BT.2390 EETF para mapeo de tonos de alta fidelidad.", // Settings_Tooltip_HdrToneMapping
    L"0 = Auto (Calculated based on image luminance).\nThe value represents the ratio of the monitor's peak luminance. Brightness below this knee point maps 1:1, while brightness above is smoothly compressed using a spline curve.\n(Recommended: 0.4 - 0.75)", // Settings_Tooltip_HdrSplineKnee
    L"Brillo Máximo HDR (Nits)", // Settings_Label_HdrPeakNitsOverride
    L"Ajustar en 0 para usar el brillo detectado por el sistema.", // Settings_Tooltip_HdrPeakNitsOverride
    L"HDR Peak Percentile", // Settings_Label_HdrPeakPercentile
    L"Discard extremely bright pixels to raise overall brightness (mpv default: 99.995%).", // Settings_Tooltip_HdrPeakPercentile
    L"100% (Absolute Peak)", // Settings_Option_HdrPeakPercentile_100
    L"99.995% (Stable)", // Settings_Option_HdrPeakPercentile_99995
    L"99.9% (Aggressive)", // Settings_Option_HdrPeakPercentile_999
    L"Rango de desaturación de brillos HDR", // Settings_Label_HdrDesatThreshold
    L"Umbral de inicio para la desaturación de zonas brillantes. 0.0 significa desaturar todo el brillo, 1.0 significa sin desaturación. El valor predeterminado recomendado es 0.18.", // Settings_Tooltip_HdrDesatThreshold
    L"Fuerza de desaturación de brillos HDR", // Settings_Label_HdrMaxDesat
    L"Intensidad máxima de la desaturación en brillos extremos. 0.0 significa sin desaturación, 1.0 significa completamente desaturado a blanco. El valor predeterminado recomendado es 0.75.", // Settings_Tooltip_HdrMaxDesat
    L"Colorimétrico", // Settings_Option_HdrColorimetric
    L"Spline", // Settings_Option_HdrSpline
    L"BT.2390 (EETF)", // Settings_Option_HdrLegacyReinhard
    L"Perfil alternativo sin etiquetas", // Settings_Label_CmsFallback
    L"Perfil de prueba en pantalla (.icc)", // Settings_Label_CustomProof
    L"Vista previa de prueba en pantalla", // Context_SoftProofing
    L"Perfil de prueba", // Context_SoftProofProfile
    L"Personalizado...", // Context_SoftProofCustom
    L"Próximamente", // Settings_Value_ComingSoon
    L"Forzar decodificación RAW", // Settings_Label_ForceRaw
    L"Emparejamiento RAW+JPEG", // Settings_Label_PairRawJpeg
    L"Mostrar un archivo RAW y la imagen de cámara del mismo nombre (JPEG/HEIF) como una sola foto.\nEn la lista solo queda la imagen revelada, marcada con el formato RAW (p. ej. +CR3); las horas de captura se verifican en segundo plano y las parejas discordantes se separan.\nCambie entre la imagen de cámara y la decodificación RAW con el botón RAW o su atajo, o vea ambas con el atajo de comparación de pareja.", // Settings_Tooltip_PairRawJpeg
    L"Exposición (Brillo)", // Settings_Label_Exposure
    L"Ajusta el brillo de la imagen (compensación de exposición). Rango: 0.18x - 10.0x.", // Settings_Tooltip_Exposure
    L"Añadir a Abrir con", // Settings_Label_AddToOpenWith
    L"Editor de imágenes personalizado", // Settings_Label_CustomEditor
    L"Seleccionar editor", // Context_SelectEditor
    L"No se pudo iniciar el editor. Por favor, configúrelo de nuevo.", // OSD_EditorLaunchFailed
    L"Añadir", // Settings_Action_Add
    L"Añadido", // Settings_Action_Added
    L"Deshabilitado en modo portátil", // Settings_Status_DisabledInPortable
    L"Habilitar HUD de depuración (F12)", // Settings_Label_DebugHUD
    L"Sistema de precarga", // Settings_Label_Prefetch
    L"Panel de información", // Settings_Label_InfoPanelAlpha
    L"Barra de herramientas", // Settings_Label_ToolbarAlpha
    L"Configuración", // Settings_Label_SettingsAlpha
    L"Restablecer toda la configuración", // Settings_Label_Reset
    L"Restaurar", // Settings_Action_Restore
    L"Hecho", // Settings_Action_Done
    L"No gestionado (rápido)", // Settings_Option_CmsUnmanaged
    L"sRGB (estándar)", // Settings_Option_CmssRGB
    L"Display P3 (gama amplia)", // Settings_Option_CmsP3
    L"Adobe RGB (1998)", // Settings_Option_CmsAdobeRGB
    L"Escala de grises (Control de tono)", // Settings_Option_CmsGray
    L"ProPhoto RGB", // Settings_Option_CmsProPhoto
    L"Intención de renderizado", // Settings_Label_CmsIntent
    L"Detección de fuera de gama", // Settings_Label_GamutWarning
    L"Analizar y resaltar áreas fuera de gama. Opciones: Desactivado, solo en modo de prueba en pantalla (predeterminado), o tanto para la prueba como para la gama de la pantalla.", // Settings_Tooltip_GamutWarning
    L"Aviso automático de error de gama", // Settings_Label_GamutAutoPrompt
    L"Mostrar notificación OSD cuando se detecten errores de gama. Los resaltados se pueden activar manualmente en la barra de herramientas.", // Settings_Tooltip_GamutAutoPrompt
    L"Color de resaltado de fuera de gama", // Settings_Label_GamutColor
    L"Relativo colorimétrico", // Settings_Option_CmsIntentRelative
    L"Perceptual", // Settings_Option_CmsIntentPerceptual
    L"Habilitar el Sistema de Gestión de Color (CMS).\nCuando se habilita, aplica una conversión de espacio de color de alta precisión a través de la GPU para restaurar los colores reales.\nDeshabilitarlo reduce la carga de la GPU, pero puede provocar colores sobresaturados en pantallas de amplia gama.", // Settings_Tooltip_CMS
    L"Propósito de representación (Rendering Intent) para la conversión del espacio de color.\nPerceptual: Comprime los colores fuera de gama para preservar los detalles y degradados (ideal para fotos).\nColorimétrico relativo: Preserva los colores dentro de la gama y recorta los que quedan fuera (ideal para UI e íconos).\nNota: Las diferencias visuales solo ocurren cuando se usan perfiles ICC avanzados que contienen LUT (tablas de búsqueda). Los perfiles de matriz estándar volverán automáticamente a Colorimétrico relativo.", // Settings_Tooltip_CmsIntent
    L"Habilitar el pipeline de renderizado de punto flotante de 16 bits (scRGB).\nCuando se habilita, renderiza perfectamente las luces de las fotos en pantallas compatibles con HDR rompiendo el límite SDR.\nDeshabilitarlo fuerza el mapeo a salida SDR.\nNota: Habilitarlo aumenta el uso de VRAM.", // Settings_Tooltip_AdvancedColor
    L"Automático: Escala al 100% cuando la imagen es más pequeña que la pantalla, se ajusta a la pantalla cuando es más grande.", // Settings_Tooltip_ZoomAuto
    L"Buscar actualizaciones", // Settings_Action_CheckUpdates
    L"Ver actualización", // Settings_Action_ViewUpdate
    L"Comprobando...", // Settings_Status_Checking
    L"Actualizado", // Settings_Status_UpToDate
    L"Repositorio GitHub", // Settings_Link_GitHub
    L"Reportar problema", // Settings_Link_ReportIssue
    L"Atajos de teclado", // Settings_Link_Hotkeys
    L"Versión", // Settings_Label_Version
    L"Compilación", // Settings_Label_Build
    L"Negro", // Settings_Option_Black
    L"Blanco", // Settings_Option_White
    L"Cuadrícula", // Settings_Option_Grid
    L"Personalizado", // Settings_Option_Custom
    L"Desactivado", // Settings_Option_Off
    L"Activado", // Settings_Option_On
    L"Compacto", // Settings_Option_Lite
    L"Completo", // Settings_Option_Full
    L"Solo grandes", // Settings_Option_LargeOnly
    L"Todos", // Settings_Option_All
    L"Pruebas en pantalla", // Settings_Option_SoftProofing
    L"Ventana", // Settings_Option_Window
    L"Panorámica", // Settings_Option_Pan
    L"Ninguno", // Settings_Option_None
    L"Salir", // Settings_Option_Exit
    L"Flecha", // Settings_Option_Arrow
    L"Cursor", // Settings_Option_Cursor
    L"Manual", // Settings_Option_Manual
    L"Auto (Explorador)", // Settings_Option_SortAuto
    L"Nombre", // Settings_Option_SortName
    L"Fecha de modificación", // Settings_Option_SortModified
    L"Fecha de captura (EXIF)", // Settings_Option_SortDateTaken
    L"Tamaño", // Settings_Option_SortSize
    L"Tipo", // Settings_Option_SortType
    L"Bucle en carpeta", // Settings_Option_NavLoop
    L"A través de subcarpetas", // Settings_Option_NavThrough
    L"Lineal: Suavizado básico", // Settings_Option_Linear
    L"Cercano: Extrema nitidez", // Settings_Option_Nearest
    L"Cúbico HQ: Extremo suavizado", // Settings_Option_HighQualityCubic
    L"Auto", // Settings_Option_ZoomAuto
    L"Automático", // Settings_Option_Auto
    L"Eco", // Settings_Option_Eco
    L"Equilibrado", // Settings_Option_Balanced
    L"Ultra", // Settings_Option_Ultra
    L"Atajos de teclado", // Help_Header_Shortcuts
    L"Acciones del ratón", // Help_Header_Mouse
    L"Siguiente / Anterior", // Help_Item_NextPrev
    L"Zoom", // Help_Item_Zoom
    L"Mover imagen", // Help_Item_Pan
    L"Girar", // Help_Item_Rotate
    L"Ajustar a pantalla", // Help_Item_Fit
    L"Eliminar imagen", // Help_Item_Delete
    L"Pantalla completa", // Help_Item_Fullscreen
    L"Cerrar", // Help_Item_Close
    L"Botón izquierdo", // Help_Mouse_Left
    L"Botón central", // Help_Mouse_Middle
    L"Rueda", // Help_Mouse_Wheel
    L"Botón derecho", // Help_Mouse_Right
    L"Arrastre vertical con botón derecho", // Help_Mouse_RightVerticalDrag
    L"Mover ventana / Salir de pantalla completa / Salir de maximizado", // Help_Action_MoveWindow
    L"Mover imagen", // Help_Action_PanImage
    L"Menú contextual", // Help_Action_ContextMenu
    L"Sig./Ant.", // Help_Action_NextPrev
    L"Zoom", // Help_Action_Zoom
    L"Zoom inteligente (100% / Ajustar / Restaurar)", // Help_Action_SmartZoom
    L"Copy Image", // Help_Desc_Copy
    L"Edit", // Help_Desc_Edit
    L"Tips & Glossary", // Help_Header_Tips
    L"Note: Shortcuts and context menu actions affect the current process " L"only. Settings are permanent.", // Help_Tip_ContextScope
    L"Rotation: 'Edge Adapted' means minor cropping to fit block boundaries " L"(lossless data). 'Lossy' means full re-encoding.", // Help_Tip_Rotation
    L"Video Wall (Ctrl+F11): Spans all monitors. If close button is hidden, " L"double-click to exit.", // Help_Tip_VideoWall
    L"Modo Calco / Modo Película: Una vez activado, la imagen se vuelve semitransparente, revelando los elementos subyacentes. Puede ajustar su tamaño o transparencia. Haga clic en el interruptor de Paso de Mouse en la barra de herramientas para entrar en el Modo de Paso, donde se ignoran todas las entradas excepto Mayús+Esc, convirtiendo QuickView en una superposición transparente.", // Help_Tip_DesignerMode
    L"Aviso de gama: detecta colores fuera de gama para la pantalla de destino o el perfil de prueba. Modos: Desactivado, solo prueba en pantalla o todo (predeterminado: prueba en pantalla). Alternar mediante la barra de herramientas.", // Help_Tip_GamutDetection
    L"RAW Button: QuickView shows embedded preview by default. Click to " L"fully decode (may look different due to rendering parameters).", // Help_Tip_Raw
    L"JPEG Quality: Estimated Q value (reverse engineered). May differ " L"slightly from save setting due to algorithm variations.", // Help_Tip_JpegQ
    L"Comparación de pruebas: entrar en el modo de comparación mientras la prueba de color está activa comparará automáticamente la imagen original frente a la de prueba.", // Help_Tip_SoftProofCompare
    L"¡Nueva versión disponible!", // Dialog_UpdateTitle
    L"v%s está listo.", // Dialog_UpdateContent
    L"Registro de cambios", // Dialog_UpdateLogHeader
    L"Actualizar ahora", // Dialog_ButtonUpdate
    L"Más tarde", // Dialog_ButtonLater
    L"Estrella en GitHub", // Dialog_ButtonStar
    L"QuickView está hecho con amor", // Dialog_Update_LoveTitle
    L"Yo mantengo QuickView en mi tiempo libre porque creo que Windows " L"merece un visor más rápido y limpio. Si disfrutas de esta " L"actualización, la mayor contribución es darnos una estrella en GitHub.", // Dialog_Update_LoveMessage
    L"Compare Mode", // Help_Item_Compare
    L"Primera / Última imagen", // Help_Item_FirstLast
    L"PHYSICAL ATTRIBUTES", // HUD_Group_Physical
    L"SCIENTIFIC QUALITY", // HUD_Group_Scientific
    L"OPTICS & ENCODING", // HUD_Group_Encoding
    L"Edge definition (Laplacian Variance)", // HUD_Tip_Sharp_Desc
    L"Crisp edges, high detail", // HUD_Tip_Sharp_High
    L"Soft focus or motion blur", // HUD_Tip_Sharp_Low
    L"> 500 is very sharp", // HUD_Tip_Sharp_Ref
    L"Information density (Shannon Entropy)", // HUD_Tip_Ent_Desc
    L"Complex textures or high noise", // HUD_Tip_Ent_High
    L"Flat areas or low detail", // HUD_Tip_Ent_Low
    L"7.0-8.0 is high detail", // HUD_Tip_Ent_Ref
    L"Bits Per Pixel (Compression Efficiency)", // HUD_Tip_BPP_Desc
    L"Lower efficiency (more data preserved)", // HUD_Tip_BPP_High
    L"Higher efficiency (higher compression)", // HUD_Tip_BPP_Low
    L"24.0 (Raw RGB), ~2.0-3.0 (High JPEG), ~0.5-1.5 (WebP/AVIF)", // HUD_Tip_BPP_Ref
    L"High: ", // HUD_Label_High
    L"Low: ", // HUD_Label_Low
    L"Ref: ", // HUD_Label_Ref
    L"Tira de imágenes de galería (Hover superior)", // Settings_Header_GalleryTrigger
    L"Modo de activación", // Settings_Label_GalleryTriggerMode
    L"Hover automático", // Settings_Option_GalleryTriggerAuto
    L"Hover en punto caliente", // Settings_Option_GalleryTriggerDelay
    L"Clic en punto caliente", // Settings_Option_GalleryTriggerClick
    L"Deshabilitado", // Settings_Option_GalleryTriggerDisable
    L"Esta función se desactiva automáticamente cuando la ventana es menor de 600x450.", // Settings_Tooltip_GalleryTrigger

    L"Presentación iniciada", // OSD_SlideshowStarted
    L"Presentación detenida", // OSD_SlideshowStopped
    L"Presentación reanudada", // OSD_SlideshowResumed
    L"Presentación pausada", // OSD_SlideshowPaused
    L"Inmersivo: Foco", // OSD_ImmersiveSpotlight
    L"Inmersivo: Normal", // OSD_ImmersiveNormal
    L"Trabajo de impresión enviado. Puede seguir navegando.", // OSD_PrintJobStarted
    L"Trabajo de impresión enviado con éxito.", // OSD_PrintJobFinished
    L"Modo de presentación", // Context_SlideshowMode
    L"Intervalo (segundos)", // Settings_Label_SlideshowInterval
    L"Modo inmersivo", // Settings_Label_SlideshowImmersive
    L"Personalizar panel de información compacto", // Settings_Label_CustomLiteInfoPanel
    L"Elementos en modo normal", // Settings_Label_ItemsInNormalMode
    L"Elementos en modo de comparación", // Settings_Label_ItemsInCompareMode
    L"Preajuste de separador", // Settings_Label_SeparatorPreset
    L"Normal", // Settings_Option_SlideshowNormal
    L"Foco", // Settings_Option_SlideshowSpotlight
    L"Forma de la lupa", // Settings_Label_LoupeShape
    L"Cuadrado", // Settings_Option_LoupeShapeSquare
    L"Círculo", // Settings_Option_LoupeShapeCircle
    L"Mantenga presionada la tecla de acceso rápido y gire la rueda del mouse para ajustar el tamaño de la lupa.", // Settings_Tooltip_LoupeHotkey
};

// ----------------------------------------------------------------
// FR Table
// ----------------------------------------------------------------
static const LanguageTable Table_FR = {
    L"No image loaded", // OSD_NoImage
    L"Lossless", // OSD_Lossless
    L"Re-encoded (Lossless)", // OSD_ReencodedLossless
    L"Cropped", // OSD_EdgeAdapted
    L"Lossy", // OSD_Reencoded
    L"Access denied - file may be in use or read-only", // OSD_ReadOnly
    L"Transform is not perfect (Edge optimized)", // OSD_NotPerfect
    L"Video Wall: ON", // OSD_SpanOn
    L"Video Wall: OFF", // OSD_SpanOff
    L"Avant (Original)", // OSD_CompareBefore
    L"Après (Preuve)", // OSD_CompareAfter
    L"Suppression annulée", // OSD_UndoDeleteSuccess
    L"Échec de l'annulation", // OSD_UndoDeleteFailed
    L"Renommer annulé", // OSD_UndoRenameSuccess
    L"Échec de l'annulation du renommage", // OSD_UndoRenameFailed
    L"Rotation/retournement annulé", // OSD_UndoTransformSuccess
    L"Échec de l'annulation de la rotation/retournement", // OSD_UndoTransformFailed
    L"Couleurs hors gamme détectées", // OSD_GamutDetected
    L"Gamme : Profil incompatible", // OSD_GamutIncompatible
    L"Gamme : Échec de l'analyse", // OSD_GamutFailed
    L"Rotate 90\x00B0 CW", // Action_RotateCW
    L"Rotate 90\x00B0 CCW", // Action_RotateCCW
    L"Rotate 180\x00B0", // Action_Rotate180
    L"Flip Horizontal", // Action_FlipH
    L"Flip Vertical", // Action_FlipV
    L"Save Changes?", // Dialog_SaveTitle
    L"The image has been modified. Do you want to save changes?", // Dialog_SaveContent
    L"Save", // Dialog_ButtonSave
    L"Save As...", // Dialog_ButtonSaveAs
    L"Discard", // Dialog_ButtonDiscard
    L"Continuer", // Dialog_ButtonContinue
    L"Always save lossless transforms", // Checkbox_AlwaysSaveLossless
    L"Always save edge-adapted", // Checkbox_AlwaysSaveEdgeAdapted
    L"Always save re-encoded", // Checkbox_AlwaysSaveLossy
    L"Envoyer directement à la corbeille sans confirmer", // Checkbox_NeverConfirmDelete
    L"Cannot decode HEIC - Install HEVC Video Extension", // OSD_HEICCodecMissing
    L"Cannot decode HEIC", // Dialog_HEICTitle
    L"Your system is missing the HEVC Video Extension.\nQuickView uses " L"system hardware acceleration for best performance.", // Dialog_HEICContent
    L"Get Extension (Free)", // Dialog_HEICGetExtension
    L"Cancel", // Dialog_Cancel
    L"General", // Settings_Tab_General
    L"About", // Settings_Tab_About
    L"Foundation", // Settings_Group_Foundation
    L"Startup", // Settings_Group_Startup
    L"Habits", // Settings_Group_Habits
    L"Language", // Settings_Label_Language
    L"Single Instance", // Settings_Label_SingleInstance
    L"Check Updates", // Settings_Label_CheckUpdates
    L"Canal de mise à jour", // Settings_Label_UpdateChannel
    L"Stable", // Settings_Option_UpdateStable
    L"Pré-version (Pre-release)", // Settings_Option_UpdatePreRelease
    L"Stable : Publié après qu'une pré-version se soit avérée stable.\nPré-version (Pre-release) : Fournit immédiatement les dernières modifications pour recueillir vos retours. Remarque : Ces modifications ne sont pas strictement testées.", // Settings_Tooltip_PreRelease
    L"Loop", // Settings_Label_NavLoopMode
    L"Sort Order", // Settings_Label_SortOrder
    L"Descending", // Settings_Label_SortDescending
    L"Toujours trier les archives par nom", // Settings_Label_SortArchivesByNameAscending
    L"Confirm Before Delete", // Settings_Label_ConfirmDel
    L"Portable Mode / Cleanup", // Settings_Label_Portable
    L"Portable Mode / Registry Cleanup:\nWhen enabled, QuickView runs in " L"portable mode. It will automatically clean up existing registry " L"associations, disable automatic registry modification, and store " L"configuration files in the application directory instead of AppData.", // Settings_Tooltip_Portable
    L"Span Displays", // Settings_Label_SpanDisplays
    L"UI Scale", // Settings_Label_UIScale
    L"Restart required", // Settings_Status_RestartRequired
    L"No Write Permission!", // Settings_Status_NoWritePerm
    L"Enabled", // Settings_Status_Enabled
    L"Powered by", // Settings_Header_PoweredBy
    L"Open...\tCtrl+O", // Context_Open
    L"Open With...", // Context_OpenWith
    L"Edit\tE", // Context_Edit
    L"Show in Explorer", // Context_ShowInExplorer
    L"Open Folder", // Context_OpenFolder
    L"Copy Image\tCtrl+C", // Context_CopyImage
    L"Copy Path\tCtrl+Alt+C", // Context_CopyPath
    L"Print\tCtrl+P", // Context_Print
    L"Rotate 90\x00B0 CW\tR", // Context_RotateCW
    L"Rotate 90\x00B0 CCW\tShift+R", // Context_RotateCCW
    L"Flip Horizontal\tH", // Context_FlipH
    L"Flip Vertical\tV", // Context_FlipV
    L"Transform", // Context_Transform
    L"Actual Size (100%)\t1 / Z", // Context_ActualSize
    L"Fit to Screen\t0 / F", // Context_FitToScreen
    L"Fit Window", // Context_FitWindow
    L"Fill Window", // Context_FillWindow
    L"Zoom In\t+ / Ctrl +", // Context_ZoomIn
    L"Zoom Out\t- / Ctrl -", // Context_ZoomOut
    L"Lock Window", // Context_LockWindow
    L"Always on Top\tCtrl+T", // Context_AlwaysOnTop
    L"HUD Gallery\tT", // Context_HUDGallery
    L"Lite Info Panel\tTab", // Context_LiteInfoPanel
    L"Full Info Panel\tI", // Context_FullInfoPanel
    L"Render RAW", // Context_RenderRAW
    L"Pixel Art Mode", // Context_PixelArtMode
    L"Color Space", // Context_ColorSpace
    L"Fullscreen\tF11", // Context_Fullscreen
    L"View", // Context_View
    L"Fill", // Context_WallpaperFill
    L"Fit", // Context_WallpaperFit
    L"Tile", // Context_WallpaperTile
    L"Set as Wallpaper", // Context_SetAsWallpaper
    L"Rename\tF2", // Context_Rename
    L"Fix Extension", // Context_FixExtension
    L"Delete\tDel", // Context_Delete
    L"Annuler la suppression\tCtrl+Z", // Context_UndoDelete
    L"Annuler le renommage\tCtrl+Z", // Context_UndoRename
    L"Annuler la rotation/retournement\tCtrl+Z", // Context_UndoTransform
    L"Annuler\tCtrl+Z", // Context_Undo
    L"Sort By", // Context_SortBy
    L"Navigation Order", // Context_NavOrder
    L"Ascending", // Context_SortAscending
    L"Descending", // Context_SortDescending
    L"Settings...", // Context_Settings
    L"Compare Mode\tC", // Context_CompareMode
    L"Mode Décalque\tCtrl+Shift+O", // Context_OverlayMode
    L"Open in Compare Mode", // Context_GalleryOpenCompare
    L"Open in New Window", // Context_GalleryOpenNewWindow
    L"Exit\tMButton/Esc", // Context_Exit
    L"Quitter le mode clic à travers", // Menu_ExitPassthrough
    L"Mode Clic à Travers", // Dialog_PassthroughTitle
    L"Les événements de souris passeront à travers la fenêtre.\nQuitter via le raccourci global (Shift+Esc) ou le menu de la barre des tâches.\n\nContinuer ?", // Dialog_PassthroughContent
    L"Clic à travers : ON (Shift+Esc pour quitter)", // OSD_PassthroughOn
    L"Clic à travers : OFF", // OSD_PassthroughOff
    L"Mode décalque : ON", // OSD_OverlayModeOn
    L"Mode décalque : OFF", // OSD_OverlayModeOff
    L"Opacité", // OSD_Opacity
    L"Zoom avant (décalque)", // Toolbar_Tooltip_OverlayZoomIn
    L"Zoom arrière (décalque)", // Toolbar_Tooltip_OverlayZoomOut
    L"Augmenter l'opacité", // Toolbar_Tooltip_OverlayAlphaUp
    L"Diminuer l'opacité", // Toolbar_Tooltip_OverlayAlphaDown
    L"Activer le clic à travers", // Toolbar_Tooltip_OverlayPassthroughOn
    L"Désactiver le clic à travers", // Toolbar_Tooltip_OverlayPassthroughOff
    L"Quitter le mode décalque", // Toolbar_Tooltip_OverlayExit
    L"Error", // Message_SaveErrorTitle
    L"Failed to save file. File locked?", // Message_SaveErrorContent
    L"Previous (Left)", // Toolbar_Tooltip_Prev
    L"Next (Right)", // Toolbar_Tooltip_Next
    L"Rotate Left (Shift+R)", // Toolbar_Tooltip_RotateL
    L"Rotate Right (R)", // Toolbar_Tooltip_RotateR
    L"Flip Horizontal (H)", // Toolbar_Tooltip_FlipH
    L"Lock Window (Temp)", // Toolbar_Tooltip_Lock
    L"Unlock Window", // Toolbar_Tooltip_Unlock
    L"Gallery (T)", // Toolbar_Tooltip_Gallery
    L"Info Panel", // Toolbar_Tooltip_Info
    L"RAW: Preview (Click for Full)", // Toolbar_Tooltip_RawPreview
    L"RAW: Full Decode (Click for Preview)", // Toolbar_Tooltip_RawFull
    L"RAW : Voir le RAW associé (décodage complet)", // Toolbar_Tooltip_RawPairView
    L"RAW : Revenir à l'image associée", // Toolbar_Tooltip_RawPairBack
    L"Extension Mismatch (Fix)", // Toolbar_Tooltip_FixExtension
    L"Pin Toolbar", // Toolbar_Tooltip_Pin
    L"Unpin Toolbar", // Toolbar_Tooltip_Unpin
    L"Afficher les zones hors gamme", // Toolbar_Tooltip_GamutWarning
    L"Normal Mode", // Toolbar_Tooltip_NormalMode
    L"Compare Mode", // Toolbar_Tooltip_CompareMode
    L"Mode page unique", // Toolbar_Tooltip_SinglePage
    L"Mode double page", // Toolbar_Tooltip_DualPage
    L"Open New Image in Selection", // Toolbar_Tooltip_CompareOpen
    L"Swap Left/Right", // Toolbar_Tooltip_CompareSwap
    L"Toggle Layout", // Toolbar_Tooltip_CompareLayout
    L"Compare Info", // Toolbar_Tooltip_CompareInfo
    L"Delete Selected Image", // Toolbar_Tooltip_CompareDelete
    L"Zoom In (Fine-tune)", // Toolbar_Tooltip_CompareZoomIn
    L"Zoom Out (Fine-tune)", // Toolbar_Tooltip_CompareZoomOut
    L"Zoom Sync: ON", // Toolbar_Tooltip_CompareSyncZoomOn
    L"Zoom Sync: OFF", // Toolbar_Tooltip_CompareSyncZoomOff
    L"Pan Sync: ON", // Toolbar_Tooltip_CompareSyncPanOn
    L"Pan Sync: OFF", // Toolbar_Tooltip_CompareSyncPanOff
    L"Basculer le mode projecteur immersif", // Toolbar_Tooltip_SlideshowImmersiveToggle
    L"Quitter le mode diaporama", // Toolbar_Tooltip_SlideshowExit
    L"Exit Compare", // Toolbar_Tooltip_CompareExit
    L"Play Animation", // Toolbar_Tooltip_AnimPlay
    L"Pause Animation", // Toolbar_Tooltip_AnimPause
    L"Previous Frame", // Toolbar_Tooltip_AnimPrev
    L"Next Frame", // Toolbar_Tooltip_AnimNext
    L"Dirty Rect Debug: ON", // Toolbar_Tooltip_AnimDirtyOn
    L"Dirty Rect Debug: OFF", // Toolbar_Tooltip_AnimDirtyOff
    L"Vitesse d'animation", // Toolbar_Tooltip_AnimSpeed
    L"Performance", // Settings_Header_Performance
    L"Professional Tools", // Settings_Header_Professional
    L"Memory Reclaim Strategy:", // Settings_Label_MemoryReclaim
    L"Smart (Auto)", // Settings_Option_MemSmart
    L"Aggressive (Max Perf)", // Settings_Option_MemAggressive
    L"On-Demand (Min RAM)", // Settings_Option_MemOnDemand
    L"Smart: Balance performance and RAM.\nAggressive: Maximize performance, high memory usage.\nOn-Demand: Release memory immediately when idle.", // Settings_Tooltip_MemoryReclaim
    L"Show update regions button in animation mode", // Settings_Label_ShowDirtyRect
    L"Show the update region debug button in animation mode to visualize which parts of the frame are being redrawn.", // Settings_Tooltip_ShowDirtyRect
    L"Copied!", // OSD_Copied
    L"Coordinates copied!", // OSD_CoordinatesCopied
    L"File path copied!", // OSD_FilePathCopied
    L"Zoom: 100%", // OSD_Zoom100
    L"Zoom: Fit Screen", // OSD_ZoomFit
    L"Zoom: Fit Window", // OSD_ZoomFitWindow
    L"Zoom: Fill Window", // OSD_ZoomFill
    L"Print: Use Ctrl+P in opened app", // OSD_PrintInstruction
    L"Moved to Recycle Bin", // OSD_MovedToRecycleBin
    L"Window Locked", // OSD_WindowLocked
    L"Window Unlocked", // OSD_WindowUnlocked
    L"Always on Top: ON", // OSD_AlwaysOnTopOn
    L"Always on Top: OFF", // OSD_AlwaysOnTopOff
    L"Wallpaper Set", // OSD_WallpaperSet
    L"Failed to set wallpaper", // OSD_WallpaperFailed
    L"Renamed", // OSD_Renamed
    L"Rename Failed", // OSD_RenameFailed
    L"Extension Fixed", // OSD_ExtensionFixed
    L"Restored", // OSD_Restored
    L"First image", // OSD_FirstImage
    L"Last image", // OSD_LastImage
    L"HD", // OSD_HD
    L"Zoom: ", // OSD_ZoomPrefix
    L"Playing", // OSD_AnimPlaying
    L"Paused (Inspector Mode: Alt+Left/Right to Seek)", // OSD_AnimPaused
    L"Dirty Rect: ON", // OSD_AnimDirtyOn
    L"Dirty Rect: OFF", // OSD_AnimDirtyOff
    L"Span Displays (Video Wall)\tCtrl+F11", // Context_SpanDisplays
    L"Interface", // Settings_Tab_Visuals
    L"Controls", // Settings_Tab_Controls
    L"Image", // Settings_Tab_Image
    L"Advanced", // Settings_Tab_Advanced
    L"Backdrop", // Settings_Header_Backdrop
    L"Window", // Settings_Header_Window
    L"Window Lock", // Settings_Header_WindowLock
    L"Panel", // Settings_Header_Panel
    L"Mouse", // Settings_Header_Mouse
    L"Edge", // Settings_Header_Edge
    L"Render", // Settings_Header_Render
    L"HDR", // Settings_Header_Hdr
    L"Prompts", // Settings_Header_Prompts
    L"System", // Settings_Header_System
    L"Features", // Settings_Header_Features
    L"Transparency", // Settings_Header_Transparency
    L"Glass Engine (GPU)", // Settings_Header_GeekGlass
    L"Enable Glass", // Settings_Label_EnableGeekGlass
    L"UI Animations", // Settings_Label_GlassUIAnimations
    L"Component Material", // Settings_Header_CoreMaterial
    L"Glass Blur Sigma", // Settings_Label_BlurSigma
    L"Glass Disabled (System)", // Settings_Status_GlassDisabled
    L"Tint Layer", // Settings_Label_TintDensity
    L"Overall color intensity of the glass frost effect.", // Settings_Tooltip_TintDensity
    L"Reflex (Specular)", // Settings_Label_SpecularOpacity
    L"Brightness of the diagonal lighting reflections.", // Settings_Tooltip_SpecularOpacity
    L"Shadow Depth", // Settings_Label_ShadowIntensity
    L"Strength of the ambient occlusion shadows.", // Settings_Tooltip_ShadowIntensity
    L"Vector Rendering", // Settings_Header_VectorAssets
    L"Icon Stroke weight", // Settings_Label_VectorStrokeWeight
    L"Standard (1.5px)", // Settings_Option_StrokeStandard
    L"Fine (1.0px)", // Settings_Option_StrokeFine
    L"Tint Profile", // Settings_Header_GlassTint
    L"Color logic", // Settings_Label_TintProfile
    L"Auto (Adaptive)", // Settings_Option_TintAuto
    L"Custom Color", // Settings_Option_TintCustom
    L"Manual Tint", // Settings_Label_GlassCustomColor
    L"Control Surface Density (%)", // Settings_Header_DensityMatrix
    L"OSD & HUD", // Settings_Label_OsdDensity
    L"Transparency for small floating overlays.", // Settings_Tooltip_OsdDensity
    L"Toolbar & Sidebars", // Settings_Label_PanelsDensity
    L"Transparency for persistent control panels.", // Settings_Tooltip_PanelsDensity
    L"Dialogs & Settings", // Settings_Label_ModalsDensity
    L"Transparency for centered popups.", // Settings_Tooltip_ModalsDensity
    L"Menus", // Settings_Label_MenusDensity
    L"Transparency for right-click context menus.", // Settings_Tooltip_MenusDensity
    L"Theme", // Settings_Tab_Theme
    L"Preset", // Settings_Label_ThemeMode
    L"System", // Settings_Option_ThemeAuto
    L"Dark", // Settings_Option_ThemeDark
    L"Light", // Settings_Option_ThemeLight
    L"Design", // Settings_Option_ThemeCustom
    L"Modal Dimmer", // Settings_Label_AmbientDimmer
    L"Dim the background when a modal or settings window is open.", // Settings_Tooltip_AmbientDimmer
    L"Accent Color", // Settings_Label_AccentColor
    L"Text Color", // Settings_Label_TextColor
    L"Theme Engine", // Settings_Header_ThemeManagement
    L"Export", // Settings_Action_ExportTheme
    L"Import", // Settings_Action_ImportTheme
    L"Canvas Color", // Settings_Label_CanvasColor
    L"Overlay", // Settings_Label_Overlay
    L"Show Grid Overlay", // Settings_Label_ShowGrid
    L"Image Transition Fade", // Settings_Label_CrossFade
    L"Always on Top", // Settings_Label_AlwaysOnTop
    L"Lock Window", // Settings_Label_LockWindow
    L"Controls whether the program locks the window border by default on startup, rather than following image scaling.", // Settings_Tooltip_LockWindow
    L"Auto-Hide Title Bar", // Settings_Label_AutoHideTitle
    L"Rounded Corners", // Settings_Label_RoundedCorners
    L"Show UI Borders", // Settings_Label_UIBorders
    L"Controls window and context menu rounded corners. Requires Windows 11.", // Settings_Tooltip_RoundedCorners
    L"Lock Bottom Toolbar", // Settings_Label_LockToolbar
    L"Minimum Window Width", // Settings_Label_WindowMinSize
    L"Maximum Start Size (%)", // Settings_Label_WindowMaxSizePercent
    L"Show Edge Indicators", // Settings_Label_ShowBorderIndicator
    L"Shows a blue indicator line in the direction where the image exceeds the window borders.", // Settings_Tooltip_ShowBorderIndicator
    L"Show Minimap", // Settings_Label_ShowNavigator
    L"Auto", // Settings_Option_NavigatorAuto
    L"On", // Settings_Option_NavigatorOn
    L"Off", // Settings_Option_NavigatorOff
    L"Keep window size on navigation", // Settings_Label_KeepWindowSizeOnNav
    L"Remember last window size and position", // Settings_Label_RememberLastWindowSizeAndPosition
    L"Adapt small images", // Settings_Label_UpscaleSmallImagesWhenLocked
    L"Smooth Window Scaling (GPU)", // Settings_Label_EnableSmoothScaling
    L"EXIF Panel Mode", // Settings_Label_ExifMode
    L"Toolbar Info Default", // Settings_Label_ToolbarInfoDefault
    L"Open Fullscreen", // Settings_Label_OpenFullScreenMode
    L"Fullscreen Zoom Mode", // Settings_Label_FullScreenZoomMode
    L"Fit to Screen", // Settings_Option_FitScreen
    L"Auto", // Settings_Option_AutoFit
    L"Invert Wheel", // Settings_Label_InvertWheel
    L"Zoom 100% Snap Damping", // Settings_Label_ZoomSnapDamping
    L"Mouse-Anchored Window Zoom", // Settings_Label_MouseAnchorZoom
    L"Right Button Drag Zoom", // Settings_Label_RightButtonDragZoom
    L"Wheel Zoom Speed", // Settings_Label_WheelZoomSpeed
    L"Thumb Wheel", // Settings_Label_ThumbWheel
    L"Right Drag Zoom Speed", // Settings_Label_RightDragZoomSpeed
    L"Zoom Speed (Temp): ", // OSD_WheelZoomSpeed
    L"Temporarily Adjust Zoom Speed", // Help_Action_AdjustZoomSpeed
    L"Temporarily Lock Window Zoom", // Help_Action_LockWindowZoom
    L"Invert Side Buttons", // Settings_Label_InvertButtons
    L"Use Fixed Zoom Levels", // Settings_Label_UseFixedZoom
    L"When enabled, Alt + Wheel performs regular zoom instead of changing zoom speed.", // Settings_Tooltip_UseFixedZoom
    L"  └  Custom Zoom Levels", // Settings_Label_FixedZoomLevels
    L"Edit Zoom Levels", // Dialog_FixedZoomTitle
    L"Enter comma-separated zoom ratios (e.g. 0.5, 1, 2):", // Dialog_FixedZoomMsg
    L"Zoom Mode (In)", // Settings_Label_ZoomModeIn
    L"Zoom Mode (Out)", // Settings_Label_ZoomModeOut
    L"Left Drag", // Settings_Label_LeftDrag
    L"Middle Drag", // Settings_Label_MiddleDrag
    L"Middle Click", // Settings_Label_MiddleClick
    L"Clic de navigation sur les bords", // Settings_Label_EdgeNavClick
    L"Désactiver en mode comparaison", // Settings_Label_DisableEdgeNavInCompare
    L"Indicateur de navigation", // Settings_Label_NavIndicator
    L"Rotation automatique (EXIF)", // Settings_Label_AutoRotate
    L"Gestion des couleurs (CMS)", // Settings_Label_CMS
    L"Couleurs avancées (HDR)", // Settings_Label_AdvancedColor
    L"Mappage de tons HDR", // Settings_Label_HdrToneMapping
    L"Spline Knee Point", // Settings_Label_HdrSplineKnee
    L"Stratégie de mappage de tons HDR (Tone Mapping) :\nDétermine comment les " L"images HDR sont affichées lorsqu'elles dépassent les capacités du " L"moniteur.\nSpline : Atténuation des hautes lumières haute fidélité " L"utilisant une spline par morceaux (Recommandé).\nColorimétrique : " L"Mappage de luminance strict ; les hautes lumières dépassant la limite " L"du moniteur sont tronquées.\nBT.2390 (EETF) : Courbe ITU-R BT.2390 EETF pour un mappage de tons haute fidélité.", // Settings_Tooltip_HdrToneMapping
    L"0 = Auto (Calculated based on image luminance).\nThe value represents the ratio of the monitor's peak luminance. Brightness below this knee point maps 1:1, while brightness above is smoothly compressed using a spline curve.\n(Recommended: 0.4 - 0.75)", // Settings_Tooltip_HdrSplineKnee
    L"Luminosité maximale HDR (Nits)", // Settings_Label_HdrPeakNitsOverride
    L"Réglez sur 0 pour utiliser la luminosité détectée par le système.", // Settings_Tooltip_HdrPeakNitsOverride
    L"HDR Peak Percentile", // Settings_Label_HdrPeakPercentile
    L"Discard extremely bright pixels to raise overall brightness (mpv default: 99.995%).", // Settings_Tooltip_HdrPeakPercentile
    L"100% (Absolute Peak)", // Settings_Option_HdrPeakPercentile_100
    L"99.995% (Stable)", // Settings_Option_HdrPeakPercentile_99995
    L"99.9% (Aggressive)", // Settings_Option_HdrPeakPercentile_999
    L"Plage de désaturation des hautes lumières HDR", // Settings_Label_HdrDesatThreshold
    L"Seuil de début de désaturation des hautes lumières. 0.0 signifie désaturer toute la luminosité, 1.0 signifie pas de désaturation. Par défaut recommandé : 0.18.", // Settings_Tooltip_HdrDesatThreshold
    L"Force de désaturation des hautes lumières HDR", // Settings_Label_HdrMaxDesat
    L"Intensité maximale de désaturation des hautes lumières extrêmes. 0.0 signifie pas de désaturation, 1.0 signifie désaturé à 100% en blanc. Par défaut recommandé : 0.75.", // Settings_Tooltip_HdrMaxDesat
    L"Colorimétrique", // Settings_Option_HdrColorimetric
    L"Spline", // Settings_Option_HdrSpline
    L"BT.2390 (EETF)", // Settings_Option_HdrLegacyReinhard
    L"Profil de repli pour images non étiquetées", // Settings_Label_CmsFallback
    L"Profil d'épreuvage écran (.icc)", // Settings_Label_CustomProof
    L"Aperçu de l'épreuvage écran", // Context_SoftProofing
    L"Profil de preuve", // Context_SoftProofProfile
    L"Personnalisé...", // Context_SoftProofCustom
    L"Bientôt disponible", // Settings_Value_ComingSoon
    L"Forcer le décodage RAW", // Settings_Label_ForceRaw
    L"Appairage RAW+JPEG", // Settings_Label_PairRawJpeg
    L"Afficher un fichier RAW et l'image boîtier du même nom (JPEG/HEIF) comme une seule photo.\nSeule l'image rendue reste dans la liste, marquée du format RAW (ex. +CR3) ; les heures de prise de vue sont vérifiées en arrière-plan et les paires discordantes sont séparées.\nBasculez entre l'image boîtier et le décodage RAW avec le bouton RAW ou son raccourci, ou affichez les deux avec le raccourci de comparaison de paire.", // Settings_Tooltip_PairRawJpeg
    L"Exposition (Luminosité)", // Settings_Label_Exposure
    L"Ajuste la luminosité de l'image (compensation d'exposition). Plage : 0.18x - 10.0x.", // Settings_Tooltip_Exposure
    L"Ajouter à 'Ouvrir avec'", // Settings_Label_AddToOpenWith
    L"Éditeur d'images personnalisé", // Settings_Label_CustomEditor
    L"Sélectionner l'éditeur", // Context_SelectEditor
    L"Échec du lancement de l'éditeur. Veuillez le reconfigurer.", // OSD_EditorLaunchFailed
    L"Ajouter", // Settings_Action_Add
    L"Ajouté", // Settings_Action_Added
    L"Désactivé en mode portable", // Settings_Status_DisabledInPortable
    L"Activer le HUD de débogage (F12)", // Settings_Label_DebugHUD
    L"Système de préchargement", // Settings_Label_Prefetch
    L"Panneau d'information", // Settings_Label_InfoPanelAlpha
    L"Barre d'outils", // Settings_Label_ToolbarAlpha
    L"Paramètres", // Settings_Label_SettingsAlpha
    L"Réinitialiser tous les paramètres", // Settings_Label_Reset
    L"Restaurer", // Settings_Action_Restore
    L"Terminé", // Settings_Action_Done
    L"Non géré", // Settings_Option_CmsUnmanaged
    L"sRGB", // Settings_Option_CmssRGB
    L"Display P3", // Settings_Option_CmsP3
    L"Adobe RGB (1998)", // Settings_Option_CmsAdobeRGB
    L"Niveaux de gris (Vérification tonale)", // Settings_Option_CmsGray
    L"ProPhoto RGB", // Settings_Option_CmsProPhoto
    L"Intention de rendu", // Settings_Label_CmsIntent
    L"Détection de dépassement de gamme", // Settings_Label_GamutWarning
    L"Analyser et mettre en évidence les zones hors gamme. Options : Désactivé, uniquement en mode épreuvage écran (par défaut), ou pour l'épreuvage et la gamme de l'écran simultanément.", // Settings_Tooltip_GamutWarning
    L"Notification automatique d'erreur de gamme", // Settings_Label_GamutAutoPrompt
    L"Afficher une notification OSD en cas d'erreur de gamme. Les points forts peuvent être activés manuellement via la barre d'outils.", // Settings_Tooltip_GamutAutoPrompt
    L"Couleur de mise en évidence de gamme", // Settings_Label_GamutColor
    L"Colorimétrie relative", // Settings_Option_CmsIntentRelative
    L"Perceptuelle", // Settings_Option_CmsIntentPerceptual
    L"Enable Color Management System.\nWhen enabled, applies high-precision color space conversion via GPU to restore true colors.\nDisabling it reduces GPU load, but may result in oversaturated colors on wide-gamut displays.", // Settings_Tooltip_CMS
    L"Mode de rendu pour la conversion de l'espace colorimétrique (Rendering Intent).\nPerceptuel : Compresse les couleurs hors gamme pour préserver les détails et les dégradés (idéal pour les photos).\nColorimétrique relatif : Préserve les couleurs dans la gamme et coupe celles qui sont en dehors (idéal pour l'interface et les icônes).\nNote : Des différences visuelles n'apparaissent que lors de l'utilisation de profils ICC avancés contenant des LUT (tables de correspondance). Les profils matriciels standard reviendront automatiquement au colorimétrique relatif.", // Settings_Tooltip_CmsIntent
    L"Enable 16-bit floating-point rendering pipeline (scRGB).\nWhen enabled, perfectly renders photo highlights on HDR-capable displays by breaking the SDR limit.\nDisabling it forces mapping to SDR output.\nNote: Enabling increases VRAM usage.", // Settings_Tooltip_AdvancedColor
    L"Auto: 100% scale when image is smaller than screen, fit to screen when larger.", // Settings_Tooltip_ZoomAuto
    L"Rechercher des mises à jour", // Settings_Action_CheckUpdates
    L"Voir la mise à jour", // Settings_Action_ViewUpdate
    L"Vérification...", // Settings_Status_Checking
    L"À jour", // Settings_Status_UpToDate
    L"Dépôt GitHub", // Settings_Link_GitHub
    L"Signaler un problème", // Settings_Link_ReportIssue
    L"Aide (F1)", // Settings_Link_Hotkeys
    L"Version", // Settings_Label_Version
    L"Build", // Settings_Label_Build
    L"Black", // Settings_Option_Black
    L"White", // Settings_Option_White
    L"Grid", // Settings_Option_Grid
    L"Custom", // Settings_Option_Custom
    L"Off", // Settings_Option_Off
    L"On", // Settings_Option_On
    L"Lite", // Settings_Option_Lite
    L"Full", // Settings_Option_Full
    L"Large Only", // Settings_Option_LargeOnly
    L"All", // Settings_Option_All
    L"Épreuvage écran", // Settings_Option_SoftProofing
    L"Window", // Settings_Option_Window
    L"Pan", // Settings_Option_Pan
    L"None", // Settings_Option_None
    L"Exit", // Settings_Option_Exit
    L"Arrow", // Settings_Option_Arrow
    L"Cursor", // Settings_Option_Cursor
    L"Manual", // Settings_Option_Manual
    L"Auto (Explorer)", // Settings_Option_SortAuto
    L"Name", // Settings_Option_SortName
    L"Date Modified", // Settings_Option_SortModified
    L"Date Taken (EXIF)", // Settings_Option_SortDateTaken
    L"Size", // Settings_Option_SortSize
    L"Type", // Settings_Option_SortType
    L"Loop", // Settings_Option_NavLoop
    L"Through subfolders", // Settings_Option_NavThrough
    L"Linear: Basic smoothing", // Settings_Option_Linear
    L"Nearest: Extreme sharpness", // Settings_Option_Nearest
    L"HQ Cubic: Extreme smoothing", // Settings_Option_HighQualityCubic
    L"Auto", // Settings_Option_ZoomAuto
    L"Auto", // Settings_Option_Auto
    L"Eco", // Settings_Option_Eco
    L"Balanced", // Settings_Option_Balanced
    L"Ultra", // Settings_Option_Ultra
    L"Keyboard Shortcuts", // Help_Header_Shortcuts
    L"Mouse Actions", // Help_Header_Mouse
    L"Next / Previous Image", // Help_Item_NextPrev
    L"Zoom In / Out", // Help_Item_Zoom
    L"Pan Image", // Help_Item_Pan
    L"Rotate", // Help_Item_Rotate
    L"Fit to Screen", // Help_Item_Fit
    L"Delete Image", // Help_Item_Delete
    L"Fullscreen", // Help_Item_Fullscreen
    L"Close", // Help_Item_Close
    L"Left Button", // Help_Mouse_Left
    L"Middle Button", // Help_Mouse_Middle
    L"Wheel", // Help_Mouse_Wheel
    L"Right Button", // Help_Mouse_Right
    L"Right Button Vertical Drag", // Help_Mouse_RightVerticalDrag
    L"Move Window / Exit Fullscreen / Exit Maximized", // Help_Action_MoveWindow
    L"Pan Image", // Help_Action_PanImage
    L"Context Menu", // Help_Action_ContextMenu
    L"Next/Prev Image", // Help_Action_NextPrev
    L"Zoom", // Help_Action_Zoom
    L"Smart Zoom (100% / Fit / Restore)", // Help_Action_SmartZoom
    L"Copy Image", // Help_Desc_Copy
    L"Edit", // Help_Desc_Edit
    L"Tips & Glossary", // Help_Header_Tips
    L"Note: Shortcuts apply to the current window only. Settings are global.", // Help_Tip_ContextScope
    L"Rotation: 'Edge Adapted' means minor cropping to align with codec " L"blocks (lossless). 'Lossy' means re-encoding is required.", // Help_Tip_Rotation
    L"Video Wall (Ctrl+F11): Spans all screens. If the close button is " L"hidden, double-click anywhere to exit.", // Help_Tip_VideoWall
    L"Mode Calque / Mode Pellicule : Une fois activé, l'image devient semi-transparente, révélant les éléments sous-jacents. Vous pouvez ajuster sa taille ou sa transparence. Cliquez sur le commutateur de passage de la souris dans la barre d'outils pour passer en mode passage, où toutes les entrées sauf Shift+Esc sont ignorées, transformant QuickView en une superposition transparente.", // Help_Tip_DesignerMode
    L"Alerte de gamme : détecte les couleurs hors gamme pour l'écran cible ou le profil d'épreuvage. Modes : Désactivé, épreuvage écran uniquement, ou tout (par défaut : épreuvage écran). Basculement via la barre d'outils.", // Help_Tip_GamutDetection
    L"RAW: Shows embedded preview by default for speed. Click the RAW button " L"to fully decode (colors may vary).", // Help_Tip_Raw
    L"JPEG Quality: Estimated value (e.g. Photoshop 100% ≈ 98%). May vary " L"slightly from save settings due to encoder variance, which is normal.", // Help_Tip_JpegQ
    L"Comparaison d'épreuves : entrer en mode comparaison alors que le mode épreuvage est actif comparera automatiquement l'image originale et l'image épreuve.", // Help_Tip_SoftProofCompare
    L"New Version Available!", // Dialog_UpdateTitle
    L"v%s is ready.", // Dialog_UpdateContent
    L"Changelog", // Dialog_UpdateLogHeader
    L"Update Now", // Dialog_ButtonUpdate
    L"Later", // Dialog_ButtonLater
    L"Star on GitHub", // Dialog_ButtonStar
    L"QuickView is built with love", // Dialog_Update_LoveTitle
    L"I maintain QuickView in my spare time because I believe Windows " L"deserves a faster, cleaner viewer. I don't have a marketing budget or " L"a team. If you enjoy this update, the biggest contribution you can " L"make is to Star us on GitHub or share it with a friend.", // Dialog_Update_LoveMessage
    L"Compare Mode", // Help_Item_Compare
    L"First / Last Image", // Help_Item_FirstLast
    L"PHYSICAL ATTRIBUTES", // HUD_Group_Physical
    L"SCIENTIFIC QUALITY", // HUD_Group_Scientific
    L"OPTICS & ENCODING", // HUD_Group_Encoding
    L"Edge definition (Laplacian Variance)", // HUD_Tip_Sharp_Desc
    L"Crisp edges, high detail", // HUD_Tip_Sharp_High
    L"Soft focus or motion blur", // HUD_Tip_Sharp_Low
    L"> 500 is very sharp", // HUD_Tip_Sharp_Ref
    L"Information density (Shannon Entropy)", // HUD_Tip_Ent_Desc
    L"Complex textures or high noise", // HUD_Tip_Ent_High
    L"Flat areas or low detail", // HUD_Tip_Ent_Low
    L"7.0-8.0 is high detail", // HUD_Tip_Ent_Ref
    L"Bits Per Pixel (Compression Efficiency)", // HUD_Tip_BPP_Desc
    L"Lower efficiency (more data preserved)", // HUD_Tip_BPP_High
    L"Higher efficiency (higher compression)", // HUD_Tip_BPP_Low
    L"24.0 (Raw RGB), ~2.0-3.0 (High JPEG), ~0.5-1.5 (WebP/AVIF)", // HUD_Tip_BPP_Ref
    L"High: ", // HUD_Label_High
    L"Low: ", // HUD_Label_Low
    L"Ref: ", // HUD_Label_Ref
    L"Film fixe de la galerie (Survol supérieur)", // Settings_Header_GalleryTrigger
    L"Mode de déclenchement", // Settings_Label_GalleryTriggerMode
    L"Survol automatique", // Settings_Option_GalleryTriggerAuto
    L"Survol du point chaud", // Settings_Option_GalleryTriggerDelay
    L"Clic sur le point chaud", // Settings_Option_GalleryTriggerClick
    L"Désactivé", // Settings_Option_GalleryTriggerDisable
    L"Cette fonctionnalité est automatiquement désactivée lorsque la fenêtre est plus petite que 600x450.", // Settings_Tooltip_GalleryTrigger

    L"Diaporama démarré", // OSD_SlideshowStarted
    L"Diaporama arrêté", // OSD_SlideshowStopped
    L"Diaporama repris", // OSD_SlideshowResumed
    L"Diaporama en pause", // OSD_SlideshowPaused
    L"Immersif : Projecteur", // OSD_ImmersiveSpotlight
    L"Immersif : Normal", // OSD_ImmersiveNormal
    L"Tâche d'impression envoyée. Vous pouvez continuer à naviguer.", // OSD_PrintJobStarted
    L"Tâche d'impression envoyée avec succès.", // OSD_PrintJobFinished
    L"Mode diaporama", // Context_SlideshowMode
    L"Intervalle (secondes)", // Settings_Label_SlideshowInterval
    L"Mode immersif", // Settings_Label_SlideshowImmersive
    L"Personnaliser le panneau d'infos compact", // Settings_Label_CustomLiteInfoPanel
    L"Éléments en mode normal", // Settings_Label_ItemsInNormalMode
    L"Éléments en mode comparaison", // Settings_Label_ItemsInCompareMode
    L"Séparateur prédéfini", // Settings_Label_SeparatorPreset
    L"Normal", // Settings_Option_SlideshowNormal
    L"Projecteur", // Settings_Option_SlideshowSpotlight
    L"Forme de la loupe", // Settings_Label_LoupeShape
    L"Carré", // Settings_Option_LoupeShapeSquare
    L"Cercle", // Settings_Option_LoupeShapeCircle
    L"Maintenez la touche de raccourci enfoncée et faites défiler la molette de la souris pour ajuster la taille de la loupe.", // Settings_Tooltip_LoupeHotkey
};

// ----------------------------------------------------------------
// Apply Helper
// ----------------------------------------------------------------
void Apply(const LanguageTable& t) {
  OSD_NoImage = t.OSD_NoImage;
  OSD_Lossless = t.OSD_Lossless;
  OSD_ReencodedLossless = t.OSD_ReencodedLossless;
  OSD_EdgeAdapted = t.OSD_EdgeAdapted;
  OSD_Reencoded = t.OSD_Reencoded;
  OSD_ReadOnly = t.OSD_ReadOnly;
  OSD_NotPerfect = t.OSD_NotPerfect;
  OSD_SpanOn = t.OSD_SpanOn;
  OSD_SpanOff = t.OSD_SpanOff;
  OSD_CompareBefore = t.OSD_CompareBefore;
  OSD_CompareAfter = t.OSD_CompareAfter;
  OSD_UndoDeleteSuccess = t.OSD_UndoDeleteSuccess;
  OSD_UndoDeleteFailed = t.OSD_UndoDeleteFailed;
  OSD_UndoRenameSuccess = t.OSD_UndoRenameSuccess;
  OSD_UndoRenameFailed = t.OSD_UndoRenameFailed;
  OSD_UndoTransformSuccess = t.OSD_UndoTransformSuccess;
  OSD_UndoTransformFailed = t.OSD_UndoTransformFailed;
  OSD_GamutDetected = t.OSD_GamutDetected;
  OSD_GamutIncompatible = t.OSD_GamutIncompatible;
  OSD_GamutFailed = t.OSD_GamutFailed;
  Action_RotateCW = t.Action_RotateCW;
  Action_RotateCCW = t.Action_RotateCCW;
  Action_Rotate180 = t.Action_Rotate180;
  Action_FlipH = t.Action_FlipH;
  Action_FlipV = t.Action_FlipV;
  Dialog_SaveTitle = t.Dialog_SaveTitle;
  Dialog_SaveContent = t.Dialog_SaveContent;
  Dialog_ButtonSave = t.Dialog_ButtonSave;
  Dialog_ButtonSaveAs = t.Dialog_ButtonSaveAs;
  Dialog_ButtonDiscard = t.Dialog_ButtonDiscard;
  Dialog_ButtonContinue = t.Dialog_ButtonContinue;
  Checkbox_AlwaysSaveLossless = t.Checkbox_AlwaysSaveLossless;
  Checkbox_AlwaysSaveEdgeAdapted = t.Checkbox_AlwaysSaveEdgeAdapted;
  Checkbox_AlwaysSaveLossy = t.Checkbox_AlwaysSaveLossy;
  Checkbox_NeverConfirmDelete = t.Checkbox_NeverConfirmDelete;
  OSD_HEICCodecMissing = t.OSD_HEICCodecMissing;
  Dialog_HEICTitle = t.Dialog_HEICTitle;
  Dialog_HEICContent = t.Dialog_HEICContent;
  Dialog_HEICGetExtension = t.Dialog_HEICGetExtension;
  Dialog_Cancel = t.Dialog_Cancel;
  Settings_Tab_General = t.Settings_Tab_General;
  Settings_Tab_About = t.Settings_Tab_About;
  Settings_Group_Foundation = t.Settings_Group_Foundation;
  Settings_Group_Startup = t.Settings_Group_Startup;
  Settings_Group_Habits = t.Settings_Group_Habits;
  Settings_Label_Language = t.Settings_Label_Language;
  Settings_Label_SingleInstance = t.Settings_Label_SingleInstance;
  Settings_Label_CheckUpdates = t.Settings_Label_CheckUpdates;
  Settings_Label_UpdateChannel = t.Settings_Label_UpdateChannel;
  Settings_Option_UpdateStable = t.Settings_Option_UpdateStable;
  Settings_Option_UpdatePreRelease = t.Settings_Option_UpdatePreRelease;
  Settings_Tooltip_PreRelease = t.Settings_Tooltip_PreRelease;
  Settings_Label_NavLoopMode = t.Settings_Label_NavLoopMode;
  Settings_Label_SortOrder = t.Settings_Label_SortOrder;
  Settings_Label_SortDescending = t.Settings_Label_SortDescending;
  Settings_Label_SortArchivesByNameAscending = t.Settings_Label_SortArchivesByNameAscending;
  Settings_Label_ConfirmDel = t.Settings_Label_ConfirmDel;
  Settings_Label_Portable = t.Settings_Label_Portable;
  Settings_Tooltip_Portable = t.Settings_Tooltip_Portable;
  Settings_Label_SpanDisplays = t.Settings_Label_SpanDisplays;
  Settings_Label_UIScale = t.Settings_Label_UIScale;
  Settings_Status_RestartRequired = t.Settings_Status_RestartRequired;
  Settings_Status_NoWritePerm = t.Settings_Status_NoWritePerm;
  Settings_Status_Enabled = t.Settings_Status_Enabled;
  Settings_Header_PoweredBy = t.Settings_Header_PoweredBy;
  Context_Open = t.Context_Open;
  Context_OpenWith = t.Context_OpenWith;
  Context_Edit = t.Context_Edit;
  Context_ShowInExplorer = t.Context_ShowInExplorer;
  Context_OpenFolder = t.Context_OpenFolder;
  Context_CopyImage = t.Context_CopyImage;
  Context_CopyPath = t.Context_CopyPath;
  Context_Print = t.Context_Print;
  Context_RotateCW = t.Context_RotateCW;
  Context_RotateCCW = t.Context_RotateCCW;
  Context_FlipH = t.Context_FlipH;
  Context_FlipV = t.Context_FlipV;
  Context_Transform = t.Context_Transform;
  Context_ActualSize = t.Context_ActualSize;
  Context_FitToScreen = t.Context_FitToScreen;
  Context_FitWindow = t.Context_FitWindow;
  Context_FillWindow = t.Context_FillWindow;
  Context_ZoomIn = t.Context_ZoomIn;
  Context_ZoomOut = t.Context_ZoomOut;
  Context_LockWindow = t.Context_LockWindow;
  Context_AlwaysOnTop = t.Context_AlwaysOnTop;
  Context_HUDGallery = t.Context_HUDGallery;
  Context_LiteInfoPanel = t.Context_LiteInfoPanel;
  Context_FullInfoPanel = t.Context_FullInfoPanel;
  Context_RenderRAW = t.Context_RenderRAW;
  Context_PixelArtMode = t.Context_PixelArtMode;
  Context_ColorSpace = t.Context_ColorSpace;
  Context_Fullscreen = t.Context_Fullscreen;
  Context_View = t.Context_View;
  Context_WallpaperFill = t.Context_WallpaperFill;
  Context_WallpaperFit = t.Context_WallpaperFit;
  Context_WallpaperTile = t.Context_WallpaperTile;
  Context_SetAsWallpaper = t.Context_SetAsWallpaper;
  Context_Rename = t.Context_Rename;
  Context_FixExtension = t.Context_FixExtension;
  Context_Delete = t.Context_Delete;
  Context_UndoDelete = t.Context_UndoDelete;
  Context_UndoRename = t.Context_UndoRename;
  Context_UndoTransform = t.Context_UndoTransform;
  Context_Undo = t.Context_Undo;
  Context_SortBy = t.Context_SortBy;
  Context_NavOrder = t.Context_NavOrder;
  Context_SortAscending = t.Context_SortAscending;
  Context_SortDescending = t.Context_SortDescending;
  Context_Settings = t.Context_Settings;
  Context_CompareMode = t.Context_CompareMode;
  Context_OverlayMode = t.Context_OverlayMode;
  Context_GalleryOpenCompare = t.Context_GalleryOpenCompare;
  Context_GalleryOpenNewWindow = t.Context_GalleryOpenNewWindow;
  Context_Exit = t.Context_Exit;
  Menu_ExitPassthrough = t.Menu_ExitPassthrough;
  Dialog_PassthroughTitle = t.Dialog_PassthroughTitle;
  Dialog_PassthroughContent = t.Dialog_PassthroughContent;
  OSD_PassthroughOn = t.OSD_PassthroughOn;
  OSD_PassthroughOff = t.OSD_PassthroughOff;
  OSD_OverlayModeOn = t.OSD_OverlayModeOn;
  OSD_OverlayModeOff = t.OSD_OverlayModeOff;
  OSD_Opacity = t.OSD_Opacity;
  Toolbar_Tooltip_OverlayZoomIn = t.Toolbar_Tooltip_OverlayZoomIn;
  Toolbar_Tooltip_OverlayZoomOut = t.Toolbar_Tooltip_OverlayZoomOut;
  Toolbar_Tooltip_OverlayAlphaUp = t.Toolbar_Tooltip_OverlayAlphaUp;
  Toolbar_Tooltip_OverlayAlphaDown = t.Toolbar_Tooltip_OverlayAlphaDown;
  Toolbar_Tooltip_OverlayPassthroughOn = t.Toolbar_Tooltip_OverlayPassthroughOn;
  Toolbar_Tooltip_OverlayPassthroughOff = t.Toolbar_Tooltip_OverlayPassthroughOff;
  Toolbar_Tooltip_OverlayExit = t.Toolbar_Tooltip_OverlayExit;
  Message_SaveErrorTitle = t.Message_SaveErrorTitle;
  Message_SaveErrorContent = t.Message_SaveErrorContent;
  Toolbar_Tooltip_Prev = t.Toolbar_Tooltip_Prev;
  Toolbar_Tooltip_Next = t.Toolbar_Tooltip_Next;
  Toolbar_Tooltip_RotateL = t.Toolbar_Tooltip_RotateL;
  Toolbar_Tooltip_RotateR = t.Toolbar_Tooltip_RotateR;
  Toolbar_Tooltip_FlipH = t.Toolbar_Tooltip_FlipH;
  Toolbar_Tooltip_Lock = t.Toolbar_Tooltip_Lock;
  Toolbar_Tooltip_Unlock = t.Toolbar_Tooltip_Unlock;
  Toolbar_Tooltip_Gallery = t.Toolbar_Tooltip_Gallery;
  Toolbar_Tooltip_Info = t.Toolbar_Tooltip_Info;
  Toolbar_Tooltip_RawPreview = t.Toolbar_Tooltip_RawPreview;
  Toolbar_Tooltip_RawFull = t.Toolbar_Tooltip_RawFull;
  Toolbar_Tooltip_RawPairView = t.Toolbar_Tooltip_RawPairView;
  Toolbar_Tooltip_RawPairBack = t.Toolbar_Tooltip_RawPairBack;
  Toolbar_Tooltip_FixExtension = t.Toolbar_Tooltip_FixExtension;
  Toolbar_Tooltip_Pin = t.Toolbar_Tooltip_Pin;
  Toolbar_Tooltip_Unpin = t.Toolbar_Tooltip_Unpin;
  Toolbar_Tooltip_GamutWarning = t.Toolbar_Tooltip_GamutWarning;
  Toolbar_Tooltip_NormalMode = t.Toolbar_Tooltip_NormalMode;
  Toolbar_Tooltip_CompareMode = t.Toolbar_Tooltip_CompareMode;
  Toolbar_Tooltip_SinglePage = t.Toolbar_Tooltip_SinglePage;
  Toolbar_Tooltip_DualPage = t.Toolbar_Tooltip_DualPage;
  Toolbar_Tooltip_CompareOpen = t.Toolbar_Tooltip_CompareOpen;
  Toolbar_Tooltip_CompareSwap = t.Toolbar_Tooltip_CompareSwap;
  Toolbar_Tooltip_CompareLayout = t.Toolbar_Tooltip_CompareLayout;
  Toolbar_Tooltip_CompareInfo = t.Toolbar_Tooltip_CompareInfo;
  Toolbar_Tooltip_CompareDelete = t.Toolbar_Tooltip_CompareDelete;
  Toolbar_Tooltip_CompareZoomIn = t.Toolbar_Tooltip_CompareZoomIn;
  Toolbar_Tooltip_CompareZoomOut = t.Toolbar_Tooltip_CompareZoomOut;
  Toolbar_Tooltip_CompareSyncZoomOn = t.Toolbar_Tooltip_CompareSyncZoomOn;
  Toolbar_Tooltip_CompareSyncZoomOff = t.Toolbar_Tooltip_CompareSyncZoomOff;
  Toolbar_Tooltip_CompareSyncPanOn = t.Toolbar_Tooltip_CompareSyncPanOn;
  Toolbar_Tooltip_CompareSyncPanOff = t.Toolbar_Tooltip_CompareSyncPanOff;
  Toolbar_Tooltip_SlideshowImmersiveToggle = t.Toolbar_Tooltip_SlideshowImmersiveToggle;
  Toolbar_Tooltip_SlideshowExit = t.Toolbar_Tooltip_SlideshowExit;
  Toolbar_Tooltip_CompareExit = t.Toolbar_Tooltip_CompareExit;
  Toolbar_Tooltip_AnimPlay = t.Toolbar_Tooltip_AnimPlay;
  Toolbar_Tooltip_AnimPause = t.Toolbar_Tooltip_AnimPause;
  Toolbar_Tooltip_AnimPrev = t.Toolbar_Tooltip_AnimPrev;
  Toolbar_Tooltip_AnimNext = t.Toolbar_Tooltip_AnimNext;
  Toolbar_Tooltip_AnimDirtyOn = t.Toolbar_Tooltip_AnimDirtyOn;
  Toolbar_Tooltip_AnimDirtyOff = t.Toolbar_Tooltip_AnimDirtyOff;
  Toolbar_Tooltip_AnimSpeed = t.Toolbar_Tooltip_AnimSpeed;
  Settings_Header_Performance = t.Settings_Header_Performance;
  Settings_Header_Professional = t.Settings_Header_Professional;
  Settings_Label_MemoryReclaim = t.Settings_Label_MemoryReclaim;
  Settings_Option_MemSmart = t.Settings_Option_MemSmart;
  Settings_Option_MemAggressive = t.Settings_Option_MemAggressive;
  Settings_Option_MemOnDemand = t.Settings_Option_MemOnDemand;
  Settings_Tooltip_MemoryReclaim = t.Settings_Tooltip_MemoryReclaim;
  Settings_Label_ShowDirtyRect = t.Settings_Label_ShowDirtyRect;
  Settings_Tooltip_ShowDirtyRect = t.Settings_Tooltip_ShowDirtyRect;
  Settings_Label_LoupeShape = t.Settings_Label_LoupeShape;
  Settings_Option_LoupeShapeSquare = t.Settings_Option_LoupeShapeSquare;
  Settings_Option_LoupeShapeCircle = t.Settings_Option_LoupeShapeCircle;
  Settings_Tooltip_LoupeHotkey = t.Settings_Tooltip_LoupeHotkey;
  OSD_Copied = t.OSD_Copied;
  OSD_CoordinatesCopied = t.OSD_CoordinatesCopied;
  OSD_FilePathCopied = t.OSD_FilePathCopied;
  OSD_Zoom100 = t.OSD_Zoom100;
  OSD_ZoomFit = t.OSD_ZoomFit;
  OSD_ZoomFitWindow = t.OSD_ZoomFitWindow;
  OSD_ZoomFill = t.OSD_ZoomFill;
  OSD_PrintInstruction = t.OSD_PrintInstruction;
  OSD_MovedToRecycleBin = t.OSD_MovedToRecycleBin;
  OSD_WindowLocked = t.OSD_WindowLocked;
  OSD_WindowUnlocked = t.OSD_WindowUnlocked;
  OSD_AlwaysOnTopOn = t.OSD_AlwaysOnTopOn;
  OSD_AlwaysOnTopOff = t.OSD_AlwaysOnTopOff;
  OSD_WallpaperSet = t.OSD_WallpaperSet;
  OSD_WallpaperFailed = t.OSD_WallpaperFailed;
  OSD_Renamed = t.OSD_Renamed;
  OSD_RenameFailed = t.OSD_RenameFailed;
  OSD_ExtensionFixed = t.OSD_ExtensionFixed;
  OSD_Restored = t.OSD_Restored;
  OSD_FirstImage = t.OSD_FirstImage;
  OSD_LastImage = t.OSD_LastImage;
  OSD_HD = t.OSD_HD;
  OSD_ZoomPrefix = t.OSD_ZoomPrefix;
  OSD_AnimPlaying = t.OSD_AnimPlaying;
  OSD_AnimPaused = t.OSD_AnimPaused;
  OSD_AnimDirtyOn = t.OSD_AnimDirtyOn;
  OSD_AnimDirtyOff = t.OSD_AnimDirtyOff;
  Context_SpanDisplays = t.Context_SpanDisplays;
  Settings_Tab_Visuals = t.Settings_Tab_Visuals;
  Settings_Tab_Controls = t.Settings_Tab_Controls;
  Settings_Tab_Image = t.Settings_Tab_Image;
  Settings_Tab_Advanced = t.Settings_Tab_Advanced;
  Settings_Header_Backdrop = t.Settings_Header_Backdrop;
  Settings_Header_Window = t.Settings_Header_Window;
  Settings_Header_WindowLock = t.Settings_Header_WindowLock;
  Settings_Header_Panel = t.Settings_Header_Panel;
  Settings_Header_Mouse = t.Settings_Header_Mouse;
  Settings_Header_Edge = t.Settings_Header_Edge;
  Settings_Header_Render = t.Settings_Header_Render;
  Settings_Header_Hdr = t.Settings_Header_Hdr;
  Settings_Header_Prompts = t.Settings_Header_Prompts;
  Settings_Header_System = t.Settings_Header_System;
  Settings_Header_Features = t.Settings_Header_Features;
  Settings_Header_Transparency = t.Settings_Header_Transparency;
  Settings_Header_GeekGlass = t.Settings_Header_GeekGlass;
  Settings_Label_EnableGeekGlass = t.Settings_Label_EnableGeekGlass;
  Settings_Label_GlassUIAnimations = t.Settings_Label_GlassUIAnimations;
  Settings_Header_CoreMaterial = t.Settings_Header_CoreMaterial;
  Settings_Label_BlurSigma = t.Settings_Label_BlurSigma;
  Settings_Status_GlassDisabled = t.Settings_Status_GlassDisabled;
  Settings_Label_TintDensity = t.Settings_Label_TintDensity;
  Settings_Tooltip_TintDensity = t.Settings_Tooltip_TintDensity;
  Settings_Label_SpecularOpacity = t.Settings_Label_SpecularOpacity;
  Settings_Tooltip_SpecularOpacity = t.Settings_Tooltip_SpecularOpacity;
  Settings_Label_ShadowIntensity = t.Settings_Label_ShadowIntensity;
  Settings_Tooltip_ShadowIntensity = t.Settings_Tooltip_ShadowIntensity;
  Settings_Header_VectorAssets = t.Settings_Header_VectorAssets;
  Settings_Label_VectorStrokeWeight = t.Settings_Label_VectorStrokeWeight;
  Settings_Option_StrokeStandard = t.Settings_Option_StrokeStandard;
  Settings_Option_StrokeFine = t.Settings_Option_StrokeFine;
  Settings_Header_GlassTint = t.Settings_Header_GlassTint;
  Settings_Label_TintProfile = t.Settings_Label_TintProfile;
  Settings_Option_TintAuto = t.Settings_Option_TintAuto;
  Settings_Option_TintCustom = t.Settings_Option_TintCustom;
  Settings_Label_GlassCustomColor = t.Settings_Label_GlassCustomColor;
  Settings_Header_DensityMatrix = t.Settings_Header_DensityMatrix;
  Settings_Label_OsdDensity = t.Settings_Label_OsdDensity;
  Settings_Tooltip_OsdDensity = t.Settings_Tooltip_OsdDensity;
  Settings_Label_PanelsDensity = t.Settings_Label_PanelsDensity;
  Settings_Tooltip_PanelsDensity = t.Settings_Tooltip_PanelsDensity;
  Settings_Label_ModalsDensity = t.Settings_Label_ModalsDensity;
  Settings_Tooltip_ModalsDensity = t.Settings_Tooltip_ModalsDensity;
  Settings_Label_MenusDensity = t.Settings_Label_MenusDensity;
  Settings_Tooltip_MenusDensity = t.Settings_Tooltip_MenusDensity;
  Settings_Tab_Theme = t.Settings_Tab_Theme;
  Settings_Label_ThemeMode = t.Settings_Label_ThemeMode;
  Settings_Option_ThemeAuto = t.Settings_Option_ThemeAuto;
  Settings_Option_ThemeDark = t.Settings_Option_ThemeDark;
  Settings_Option_ThemeLight = t.Settings_Option_ThemeLight;
  Settings_Option_ThemeCustom = t.Settings_Option_ThemeCustom;
  Settings_Label_AmbientDimmer = t.Settings_Label_AmbientDimmer;
  Settings_Tooltip_AmbientDimmer = t.Settings_Tooltip_AmbientDimmer;
  Settings_Label_AccentColor = t.Settings_Label_AccentColor;
  Settings_Label_TextColor = t.Settings_Label_TextColor;
  Settings_Header_ThemeManagement = t.Settings_Header_ThemeManagement;
  Settings_Action_ExportTheme = t.Settings_Action_ExportTheme;
  Settings_Action_ImportTheme = t.Settings_Action_ImportTheme;
  Settings_Label_CanvasColor = t.Settings_Label_CanvasColor;
  Settings_Label_Overlay = t.Settings_Label_Overlay;
  Settings_Label_ShowGrid = t.Settings_Label_ShowGrid;
  Settings_Label_CrossFade = t.Settings_Label_CrossFade;
  Settings_Label_AlwaysOnTop = t.Settings_Label_AlwaysOnTop;
  Settings_Label_LockWindow = t.Settings_Label_LockWindow;
  Settings_Tooltip_LockWindow = t.Settings_Tooltip_LockWindow;
  Settings_Label_AutoHideTitle = t.Settings_Label_AutoHideTitle;
  Settings_Label_RoundedCorners = t.Settings_Label_RoundedCorners;
  Settings_Label_UIBorders = t.Settings_Label_UIBorders;
  Settings_Tooltip_RoundedCorners = t.Settings_Tooltip_RoundedCorners;
  Settings_Label_LockToolbar = t.Settings_Label_LockToolbar;
  Settings_Label_WindowMinSize = t.Settings_Label_WindowMinSize;
  Settings_Label_WindowMaxSizePercent = t.Settings_Label_WindowMaxSizePercent;
  Settings_Label_ShowBorderIndicator = t.Settings_Label_ShowBorderIndicator;
  Settings_Tooltip_ShowBorderIndicator = t.Settings_Tooltip_ShowBorderIndicator;
  Settings_Label_ShowNavigator = t.Settings_Label_ShowNavigator;
  Settings_Option_NavigatorAuto = t.Settings_Option_NavigatorAuto;
  Settings_Option_NavigatorOn = t.Settings_Option_NavigatorOn;
  Settings_Option_NavigatorOff = t.Settings_Option_NavigatorOff;
  Settings_Label_KeepWindowSizeOnNav = t.Settings_Label_KeepWindowSizeOnNav;
  Settings_Label_RememberLastWindowSizeAndPosition = t.Settings_Label_RememberLastWindowSizeAndPosition;
  Settings_Label_UpscaleSmallImagesWhenLocked = t.Settings_Label_UpscaleSmallImagesWhenLocked;
  Settings_Label_EnableSmoothScaling = t.Settings_Label_EnableSmoothScaling;
  Settings_Label_ExifMode = t.Settings_Label_ExifMode;
  Settings_Label_ToolbarInfoDefault = t.Settings_Label_ToolbarInfoDefault;
  Settings_Label_OpenFullScreenMode = t.Settings_Label_OpenFullScreenMode;
  Settings_Label_FullScreenZoomMode = t.Settings_Label_FullScreenZoomMode;
  Settings_Option_FitScreen = t.Settings_Option_FitScreen;
  Settings_Option_AutoFit = t.Settings_Option_AutoFit;
  Settings_Label_InvertWheel = t.Settings_Label_InvertWheel;
  Settings_Label_ZoomSnapDamping = t.Settings_Label_ZoomSnapDamping;
  Settings_Label_MouseAnchorZoom = t.Settings_Label_MouseAnchorZoom;
  Settings_Label_RightButtonDragZoom = t.Settings_Label_RightButtonDragZoom;
  Settings_Label_WheelZoomSpeed = t.Settings_Label_WheelZoomSpeed;
  Settings_Label_ThumbWheel = t.Settings_Label_ThumbWheel;
  Settings_Label_RightDragZoomSpeed = t.Settings_Label_RightDragZoomSpeed;
  OSD_WheelZoomSpeed = t.OSD_WheelZoomSpeed;
  Help_Action_AdjustZoomSpeed = t.Help_Action_AdjustZoomSpeed;
  Help_Action_LockWindowZoom = t.Help_Action_LockWindowZoom;
  Settings_Label_InvertButtons = t.Settings_Label_InvertButtons;
  Settings_Label_UseFixedZoom = t.Settings_Label_UseFixedZoom;
  Settings_Tooltip_UseFixedZoom = t.Settings_Tooltip_UseFixedZoom;
  Settings_Label_FixedZoomLevels = t.Settings_Label_FixedZoomLevels;
  Dialog_FixedZoomTitle = t.Dialog_FixedZoomTitle;
  Dialog_FixedZoomMsg = t.Dialog_FixedZoomMsg;
  Settings_Label_ZoomModeIn = t.Settings_Label_ZoomModeIn;
  Settings_Label_ZoomModeOut = t.Settings_Label_ZoomModeOut;
  Settings_Label_LeftDrag = t.Settings_Label_LeftDrag;
  Settings_Label_MiddleDrag = t.Settings_Label_MiddleDrag;
  Settings_Label_MiddleClick = t.Settings_Label_MiddleClick;
  Settings_Label_EdgeNavClick = t.Settings_Label_EdgeNavClick;
  Settings_Label_DisableEdgeNavInCompare = t.Settings_Label_DisableEdgeNavInCompare;
  Settings_Label_NavIndicator = t.Settings_Label_NavIndicator;
  Settings_Label_AutoRotate = t.Settings_Label_AutoRotate;
  Settings_Label_CMS = t.Settings_Label_CMS;
  Settings_Label_AdvancedColor = t.Settings_Label_AdvancedColor;
  Settings_Label_HdrToneMapping = t.Settings_Label_HdrToneMapping;
  Settings_Label_HdrSplineKnee = t.Settings_Label_HdrSplineKnee;
  Settings_Tooltip_HdrToneMapping = t.Settings_Tooltip_HdrToneMapping;
  Settings_Tooltip_HdrSplineKnee = t.Settings_Tooltip_HdrSplineKnee;
  Settings_Label_HdrPeakNitsOverride = t.Settings_Label_HdrPeakNitsOverride;
  Settings_Tooltip_HdrPeakNitsOverride = t.Settings_Tooltip_HdrPeakNitsOverride;
  Settings_Label_HdrPeakPercentile = t.Settings_Label_HdrPeakPercentile;
  Settings_Tooltip_HdrPeakPercentile = t.Settings_Tooltip_HdrPeakPercentile;
  Settings_Option_HdrPeakPercentile_100 = t.Settings_Option_HdrPeakPercentile_100;
  Settings_Option_HdrPeakPercentile_99995 = t.Settings_Option_HdrPeakPercentile_99995;
  Settings_Option_HdrPeakPercentile_999 = t.Settings_Option_HdrPeakPercentile_999;
  Settings_Label_HdrDesatThreshold = t.Settings_Label_HdrDesatThreshold;
  Settings_Tooltip_HdrDesatThreshold = t.Settings_Tooltip_HdrDesatThreshold;
  Settings_Label_HdrMaxDesat = t.Settings_Label_HdrMaxDesat;
  Settings_Tooltip_HdrMaxDesat = t.Settings_Tooltip_HdrMaxDesat;
  Settings_Option_HdrColorimetric = t.Settings_Option_HdrColorimetric;
  Settings_Option_HdrSpline = t.Settings_Option_HdrSpline;
  Settings_Option_HdrLegacyReinhard = t.Settings_Option_HdrLegacyReinhard;
  Settings_Label_CmsFallback = t.Settings_Label_CmsFallback;
  Settings_Label_CustomProof = t.Settings_Label_CustomProof;
  Context_SoftProofing = t.Context_SoftProofing;
  Context_SoftProofProfile = t.Context_SoftProofProfile;
  Context_SoftProofCustom = t.Context_SoftProofCustom;
  Settings_Value_ComingSoon = t.Settings_Value_ComingSoon;
  Settings_Label_ForceRaw = t.Settings_Label_ForceRaw;
  Settings_Label_PairRawJpeg = t.Settings_Label_PairRawJpeg;
  Settings_Tooltip_PairRawJpeg = t.Settings_Tooltip_PairRawJpeg;
  Settings_Label_Exposure = t.Settings_Label_Exposure;
  Settings_Tooltip_Exposure = t.Settings_Tooltip_Exposure;
  Settings_Label_AddToOpenWith = t.Settings_Label_AddToOpenWith;
  Settings_Label_CustomEditor = t.Settings_Label_CustomEditor;
  Context_SelectEditor = t.Context_SelectEditor;
  OSD_EditorLaunchFailed = t.OSD_EditorLaunchFailed;
  Settings_Action_Add = t.Settings_Action_Add;
  Settings_Action_Added = t.Settings_Action_Added;
  Settings_Status_DisabledInPortable = t.Settings_Status_DisabledInPortable;
  Settings_Label_DebugHUD = t.Settings_Label_DebugHUD;
  Settings_Label_Prefetch = t.Settings_Label_Prefetch;
  Settings_Label_InfoPanelAlpha = t.Settings_Label_InfoPanelAlpha;
  Settings_Label_ToolbarAlpha = t.Settings_Label_ToolbarAlpha;
  Settings_Label_SettingsAlpha = t.Settings_Label_SettingsAlpha;
  Settings_Label_Reset = t.Settings_Label_Reset;
  Settings_Action_Restore = t.Settings_Action_Restore;
  Settings_Action_Done = t.Settings_Action_Done;
  Settings_Option_CmsUnmanaged = t.Settings_Option_CmsUnmanaged;
  Settings_Option_CmssRGB = t.Settings_Option_CmssRGB;
  Settings_Option_CmsP3 = t.Settings_Option_CmsP3;
  Settings_Option_CmsAdobeRGB = t.Settings_Option_CmsAdobeRGB;
  Settings_Option_CmsGray = t.Settings_Option_CmsGray;
  Settings_Option_CmsProPhoto = t.Settings_Option_CmsProPhoto;
  Settings_Label_CmsIntent = t.Settings_Label_CmsIntent;
  Settings_Label_GamutWarning = t.Settings_Label_GamutWarning;
  Settings_Tooltip_GamutWarning = t.Settings_Tooltip_GamutWarning;
  Settings_Label_GamutAutoPrompt = t.Settings_Label_GamutAutoPrompt;
  Settings_Tooltip_GamutAutoPrompt = t.Settings_Tooltip_GamutAutoPrompt;
  Settings_Label_GamutColor = t.Settings_Label_GamutColor;
  Settings_Option_CmsIntentRelative = t.Settings_Option_CmsIntentRelative;
  Settings_Option_CmsIntentPerceptual = t.Settings_Option_CmsIntentPerceptual;
  Settings_Tooltip_CMS = t.Settings_Tooltip_CMS;
  Settings_Tooltip_CmsIntent = t.Settings_Tooltip_CmsIntent;
  Settings_Tooltip_AdvancedColor = t.Settings_Tooltip_AdvancedColor;
  Settings_Tooltip_ZoomAuto = t.Settings_Tooltip_ZoomAuto;
  Settings_Action_CheckUpdates = t.Settings_Action_CheckUpdates;
  Settings_Action_ViewUpdate = t.Settings_Action_ViewUpdate;
  Settings_Status_Checking = t.Settings_Status_Checking;
  Settings_Status_UpToDate = t.Settings_Status_UpToDate;
  Settings_Link_GitHub = t.Settings_Link_GitHub;
  Settings_Link_ReportIssue = t.Settings_Link_ReportIssue;
  Settings_Link_Hotkeys = t.Settings_Link_Hotkeys;
  Settings_Label_Version = t.Settings_Label_Version;
  Settings_Label_Build = t.Settings_Label_Build;
  Settings_Option_Black = t.Settings_Option_Black;
  Settings_Option_White = t.Settings_Option_White;
  Settings_Option_Grid = t.Settings_Option_Grid;
  Settings_Option_Custom = t.Settings_Option_Custom;
  Settings_Option_Off = t.Settings_Option_Off;
  Settings_Option_On = t.Settings_Option_On;
  Settings_Option_Lite = t.Settings_Option_Lite;
  Settings_Option_Full = t.Settings_Option_Full;
  Settings_Option_LargeOnly = t.Settings_Option_LargeOnly;
  Settings_Option_All = t.Settings_Option_All;
  Settings_Option_SoftProofing = t.Settings_Option_SoftProofing;
  Settings_Option_Window = t.Settings_Option_Window;
  Settings_Option_Pan = t.Settings_Option_Pan;
  Settings_Option_None = t.Settings_Option_None;
  Settings_Option_Exit = t.Settings_Option_Exit;
  Settings_Option_Arrow = t.Settings_Option_Arrow;
  Settings_Option_Cursor = t.Settings_Option_Cursor;
  Settings_Option_Manual = t.Settings_Option_Manual;
  Settings_Option_SortAuto = t.Settings_Option_SortAuto;
  Settings_Option_SortName = t.Settings_Option_SortName;
  Settings_Option_SortModified = t.Settings_Option_SortModified;
  Settings_Option_SortDateTaken = t.Settings_Option_SortDateTaken;
  Settings_Option_SortSize = t.Settings_Option_SortSize;
  Settings_Option_SortType = t.Settings_Option_SortType;
  Settings_Option_NavLoop = t.Settings_Option_NavLoop;
  Settings_Option_NavThrough = t.Settings_Option_NavThrough;
  Settings_Option_Linear = t.Settings_Option_Linear;
  Settings_Option_Nearest = t.Settings_Option_Nearest;
  Settings_Option_HighQualityCubic = t.Settings_Option_HighQualityCubic;
  Settings_Option_ZoomAuto = t.Settings_Option_ZoomAuto;
  Settings_Option_Auto = t.Settings_Option_Auto;
  Settings_Option_Eco = t.Settings_Option_Eco;
  Settings_Option_Balanced = t.Settings_Option_Balanced;
  Settings_Option_Ultra = t.Settings_Option_Ultra;
  Help_Header_Shortcuts = t.Help_Header_Shortcuts;
  Help_Header_Mouse = t.Help_Header_Mouse;
  Help_Item_NextPrev = t.Help_Item_NextPrev;
  Help_Item_Zoom = t.Help_Item_Zoom;
  Help_Item_Pan = t.Help_Item_Pan;
  Help_Item_Rotate = t.Help_Item_Rotate;
  Help_Item_Fit = t.Help_Item_Fit;
  Help_Item_Delete = t.Help_Item_Delete;
  Help_Item_Fullscreen = t.Help_Item_Fullscreen;
  Help_Item_Close = t.Help_Item_Close;
  Help_Mouse_Left = t.Help_Mouse_Left;
  Help_Mouse_Middle = t.Help_Mouse_Middle;
  Help_Mouse_Wheel = t.Help_Mouse_Wheel;
  Help_Mouse_Right = t.Help_Mouse_Right;
  Help_Mouse_RightVerticalDrag = t.Help_Mouse_RightVerticalDrag;
  Help_Action_MoveWindow = t.Help_Action_MoveWindow;
  Help_Action_PanImage = t.Help_Action_PanImage;
  Help_Action_ContextMenu = t.Help_Action_ContextMenu;
  Help_Action_NextPrev = t.Help_Action_NextPrev;
  Help_Action_Zoom = t.Help_Action_Zoom;
  Help_Action_SmartZoom = t.Help_Action_SmartZoom;
  Help_Desc_Copy = t.Help_Desc_Copy;
  Help_Desc_Edit = t.Help_Desc_Edit;
  Help_Header_Tips = t.Help_Header_Tips;
  Help_Tip_ContextScope = t.Help_Tip_ContextScope;
  Help_Tip_Rotation = t.Help_Tip_Rotation;
  Help_Tip_VideoWall = t.Help_Tip_VideoWall;
  Help_Tip_DesignerMode = t.Help_Tip_DesignerMode;
  Help_Tip_GamutDetection = t.Help_Tip_GamutDetection;
  Help_Tip_Raw = t.Help_Tip_Raw;
  Help_Tip_JpegQ = t.Help_Tip_JpegQ;
  Help_Tip_SoftProofCompare = t.Help_Tip_SoftProofCompare;
  Dialog_UpdateTitle = t.Dialog_UpdateTitle;
  Dialog_UpdateContent = t.Dialog_UpdateContent;
  Dialog_UpdateLogHeader = t.Dialog_UpdateLogHeader;
  Dialog_ButtonUpdate = t.Dialog_ButtonUpdate;
  Dialog_ButtonLater = t.Dialog_ButtonLater;
  Dialog_ButtonStar = t.Dialog_ButtonStar;
  Dialog_Update_LoveTitle = t.Dialog_Update_LoveTitle;
  Dialog_Update_LoveMessage = t.Dialog_Update_LoveMessage;
  Help_Item_Compare = t.Help_Item_Compare;
  Help_Item_FirstLast = t.Help_Item_FirstLast;
  HUD_Group_Physical = t.HUD_Group_Physical;
  HUD_Group_Scientific = t.HUD_Group_Scientific;
  HUD_Group_Encoding = t.HUD_Group_Encoding;
  HUD_Tip_Sharp_Desc = t.HUD_Tip_Sharp_Desc;
  HUD_Tip_Sharp_High = t.HUD_Tip_Sharp_High;
  HUD_Tip_Sharp_Low = t.HUD_Tip_Sharp_Low;
  HUD_Tip_Sharp_Ref = t.HUD_Tip_Sharp_Ref;
  HUD_Tip_Ent_Desc = t.HUD_Tip_Ent_Desc;
  HUD_Tip_Ent_High = t.HUD_Tip_Ent_High;
  HUD_Tip_Ent_Low = t.HUD_Tip_Ent_Low;
  HUD_Tip_Ent_Ref = t.HUD_Tip_Ent_Ref;
  HUD_Tip_BPP_Desc = t.HUD_Tip_BPP_Desc;
  HUD_Tip_BPP_High = t.HUD_Tip_BPP_High;
  HUD_Tip_BPP_Low = t.HUD_Tip_BPP_Low;
  HUD_Tip_BPP_Ref = t.HUD_Tip_BPP_Ref;
  HUD_Label_High = t.HUD_Label_High;
  HUD_Label_Low = t.HUD_Label_Low;
  HUD_Label_Ref = t.HUD_Label_Ref;
  Settings_Header_GalleryTrigger = t.Settings_Header_GalleryTrigger;
  Settings_Label_GalleryTriggerMode = t.Settings_Label_GalleryTriggerMode;
  Settings_Option_GalleryTriggerAuto = t.Settings_Option_GalleryTriggerAuto;
  Settings_Option_GalleryTriggerDelay = t.Settings_Option_GalleryTriggerDelay;
  Settings_Option_GalleryTriggerClick = t.Settings_Option_GalleryTriggerClick;
  Settings_Option_GalleryTriggerDisable = t.Settings_Option_GalleryTriggerDisable;
  Settings_Tooltip_GalleryTrigger = t.Settings_Tooltip_GalleryTrigger;

  OSD_SlideshowStarted = t.OSD_SlideshowStarted;
  OSD_SlideshowStopped = t.OSD_SlideshowStopped;
  OSD_SlideshowResumed = t.OSD_SlideshowResumed;
  OSD_SlideshowPaused = t.OSD_SlideshowPaused;
  OSD_ImmersiveSpotlight = t.OSD_ImmersiveSpotlight;
  OSD_ImmersiveNormal = t.OSD_ImmersiveNormal;
  OSD_PrintJobStarted = t.OSD_PrintJobStarted;
  OSD_PrintJobFinished = t.OSD_PrintJobFinished;
  Context_SlideshowMode = t.Context_SlideshowMode;
  Settings_Label_SlideshowInterval = t.Settings_Label_SlideshowInterval;
  Settings_Label_SlideshowImmersive = t.Settings_Label_SlideshowImmersive;
  Settings_Label_CustomLiteInfoPanel = t.Settings_Label_CustomLiteInfoPanel;
  Settings_Label_ItemsInNormalMode = t.Settings_Label_ItemsInNormalMode;
  Settings_Label_ItemsInCompareMode = t.Settings_Label_ItemsInCompareMode;
  Settings_Label_SeparatorPreset = t.Settings_Label_SeparatorPreset;
  Settings_Option_SlideshowNormal = t.Settings_Option_SlideshowNormal;
  Settings_Option_SlideshowSpotlight = t.Settings_Option_SlideshowSpotlight;
}

void Init() { SetLanguage(Language::Auto); }

void SetLanguage(Language lang) {
  Language target = lang;
  if (target == Language::Auto) {
    LANGID id = GetUserDefaultUILanguage();
    switch (PRIMARYLANGID(id)) {
    case LANG_CHINESE:
      target = Language::ChineseSimplified;
      break;
    case LANG_RUSSIAN:
      target = Language::Russian;
      break;
    case LANG_GERMAN:
      target = Language::German;
      break;
    case LANG_SPANISH:
      target = Language::Spanish;
      break;
    case LANG_JAPANESE:
      target = Language::Japanese;
      break;
    case LANG_FRENCH:
      target = Language::French;
      break;
    default:
      target = Language::English;
      break;
    }
  }

  switch (target) {
  case Language::ChineseSimplified:
    CurrentLocale = L"zh-cn";
    Apply(Table_CN);
    break;
  case Language::ChineseTraditional:
    CurrentLocale = L"zh-tw";
    Apply(Table_TW);
    break;
  case Language::Japanese:
    CurrentLocale = L"ja-jp";
    Apply(Table_JA);
    break;
  case Language::Russian:
    CurrentLocale = L"ru-ru";
    Apply(Table_RU);
    break;
  case Language::German:
    CurrentLocale = L"de-de";
    Apply(Table_DE);
    break;
  case Language::Spanish:
    CurrentLocale = L"es-es";
    Apply(Table_ES);
    break;
  case Language::French:
    CurrentLocale = L"fr-fr";
    Apply(Table_FR);
    break;
  case Language::English:
  default:
    CurrentLocale = L"en-us";
    Apply(Table_EN);
    break;
  }

  switch (target) {
  case Language::ChineseSimplified:
    Settings_Tab_Shortcuts = L"快捷键";
    Settings_Hotkey_PressKey = L"请按下按键...";
    Settings_Hotkey_Conflict = L"快捷键冲突";
    Settings_Hotkey_Restore = L"恢复快捷键默认值";
    Settings_Hotkey_Restored = L"已恢复";
    Settings_Hotkey_MouseTip = L"提示：支持鼠标中键、侧键等鼠标多功能按键。更多侧键请在鼠标驱动中映射为键盘按键后绑定。";
    Settings_Header_KeyboardPan = L"按键平移步长";
    Settings_Label_PanStepNormal = L"按键平移步长 (普通)";
    Settings_Label_PanStepFast = L"按键平移步长 (加速)";
    break;
  case Language::ChineseTraditional:
    Settings_Tab_Shortcuts = L"快捷鍵";
    Settings_Hotkey_PressKey = L"請按下按鍵...";
    Settings_Hotkey_Conflict = L"快捷鍵衝突";
    Settings_Hotkey_Restore = L"恢復快捷鍵預設值";
    Settings_Hotkey_Restored = L"已恢復";
    Settings_Hotkey_MouseTip = L"提示：支持滑鼠中鍵、側鍵等滑鼠多功能按鍵。更多側鍵請在滑鼠驅動中映射為鍵盤按鍵後綁定。";
    Settings_Header_KeyboardPan = L"按鍵平移步長";
    Settings_Label_PanStepNormal = L"按鍵平移步長 (普通)";
    Settings_Label_PanStepFast = L"按鍵平移步長 (加速)";
    break;
  case Language::Japanese:
    Settings_Tab_Shortcuts = L"ショートカット";
    Settings_Hotkey_PressKey = L"キーを押してください...";
    Settings_Hotkey_Conflict = L"キーの競合";
    Settings_Hotkey_Restore = L"既定のショートカットに戻す";
    Settings_Hotkey_Restored = L"復元されました";
    Settings_Hotkey_MouseTip = L"ヒント：中央ボタンやサイドボタンなどのマウス多機能ボタンに対応しています。その他のサイドボタンは、マウスのドライバーでキーボードのキーにマッピングしてからバインドしてください。";
    Settings_Header_KeyboardPan = L"キーボードパン歩幅";
    Settings_Label_PanStepNormal = L"キーボードパン歩幅 (標準)";
    Settings_Label_PanStepFast = L"キーボードパン歩幅 (高速)";
    break;
  case Language::Russian:
    Settings_Tab_Shortcuts = L"Клавиши";
    Settings_Hotkey_PressKey = L"Нажмите клавиши...";
    Settings_Hotkey_Conflict = L"Конфликт клавиш";
    Settings_Hotkey_Restore = L"Сбросить горячие клавиши";
    Settings_Hotkey_Restored = L"Восстановлено";
    Settings_Hotkey_MouseTip = L"Подсказка: поддерживаются средняя кнопка, боковые и другие многофункциональные кнопки мыши. Для сопоставления дополнительных кнопок сначала назначьте их на клавиши клавиатуры в драйвере мыши.";
    Settings_Header_KeyboardPan = L"Шаг клавиатурного сдвига";
    Settings_Label_PanStepNormal = L"Шаг клавиатурного сдвига (Обычный)";
    Settings_Label_PanStepFast = L"Шаг клавиатурного сдвига (Быстрый)";
    break;
  case Language::German:
    Settings_Tab_Shortcuts = L"Kürzel";
    Settings_Hotkey_PressKey = L"Tasten drücken...";
    Settings_Hotkey_Conflict = L"Tastenkonflikt";
    Settings_Hotkey_Restore = L"Standard-Tastenkombinationen wiederherstellen";
    Settings_Hotkey_Restored = L"Wiederhergestellt";
    Settings_Hotkey_MouseTip = L"Tipp: Unterstützt die mittlere Maustaste, Seitentasten und andere Multifunktionstasten. Ordnen Sie weitere Seitentasten im Maustreiber Tastaturtasten zu, um sie zu binden.";
    Settings_Header_KeyboardPan = L"Tastatur-Pan-Schrittweite";
    Settings_Label_PanStepNormal = L"Tastatur-Pan-Schrittweite (Normal)";
    Settings_Label_PanStepFast = L"Tastatur-Pan-Schrittweite (Schnell)";
    break;
  case Language::Spanish:
    Settings_Tab_Shortcuts = L"Atajos";
    Settings_Hotkey_PressKey = L"Presione teclas...";
    Settings_Hotkey_Conflict = L"Conflicto de teclas";
    Settings_Hotkey_Restore = L"Restablecer atajos predeterminados";
    Settings_Hotkey_Restored = L"Restablecido";
    Settings_Hotkey_MouseTip = L"Consejo: Admite el botón central del mouse, los botones laterales y otros botones multifunción. Asigne botones laterales adicionales a teclas del teclado en el controlador del mouse para vincularlos.";
    Settings_Header_KeyboardPan = L"Paso de pan con teclado";
    Settings_Label_PanStepNormal = L"Paso de pan con teclado (Normal)";
    Settings_Label_PanStepFast = L"Paso de pan con teclado (Rápido)";
    break;
  case Language::French:
    Settings_Tab_Shortcuts = L"Raccourcis";
    Settings_Hotkey_PressKey = L"Appuyez sur les touches...";
    Settings_Hotkey_Conflict = L"Conflit de raccourcis";
    Settings_Hotkey_Restore = L"Restaurer les raccourcis par défaut";
    Settings_Hotkey_Restored = L"Restauré";
    Settings_Hotkey_MouseTip = L"Conseil : Prend en charge le bouton central de la souris, les boutons latéraux et autres boutons multifonctions. Mappez les boutons latéraux supplémentaires sur les touches du clavier dans le pilote de votre souris pour les lier.";
    Settings_Header_KeyboardPan = L"Pas de pan clavier";
    Settings_Label_PanStepNormal = L"Pas de pan clavier (Normal)";
    Settings_Label_PanStepFast = L"Pas de pan clavier (Rapide)";
    break;
  case Language::English:
  default:
    Settings_Tab_Shortcuts = L"Shortcuts";
    Settings_Hotkey_PressKey = L"Press keys...";
    Settings_Hotkey_Conflict = L"Hotkey conflict";
    Settings_Hotkey_Restore = L"Restore Default Hotkeys";
    Settings_Hotkey_Restored = L"Restored";
    Settings_Hotkey_MouseTip = L"Tip: Supports middle mouse button, side buttons, and other multi-function mouse keys. Map additional buttons to keyboard keys in your mouse driver to bind them.";
    Settings_Header_KeyboardPan = L"Keyboard Pan Step";
    Settings_Label_PanStepNormal = L"Keyboard Pan Step (Normal)";
    Settings_Label_PanStepFast = L"Keyboard Pan Step (Fast)";
    break;
  }
}

static std::wstring CleanLabel(const wchar_t* label) {
    if (!label) return L"";
    std::wstring s(label);
    
    // Clean tab separator
    size_t pos = s.find(L'\t');
    if (pos != std::wstring::npos) {
        s = s.substr(0, pos);
    }
    
    // Clean (F1) or (F1) variants
    pos = s.find(L" (F1)");
    if (pos != std::wstring::npos) {
        s = s.substr(0, pos);
    }
    pos = s.find(L"(F1)");
    if (pos != std::wstring::npos) {
        s = s.substr(0, pos);
    }

    // Clean parenthesis content unless it contains "100%"
    if (s.find(L"100%") == std::wstring::npos) {
        // Half-width parenthesis
        size_t p = s.find(L'(');
        if (p != std::wstring::npos) {
            while (p > 0 && s[p - 1] == L' ') p--;
            s = s.substr(0, p);
        }
        // Full-width parenthesis
        p = s.find(L'（');
        if (p != std::wstring::npos) {
            while (p > 0 && s[p - 1] == L' ') p--;
            s = s.substr(0, p);
        }
    }
    
    return s;
}

static AppStrings::Language GetActiveLanguage() {
    if (AppStrings::OSD_NoImage == nullptr) return AppStrings::Language::English;
    if (wcscmp(AppStrings::OSD_NoImage, L"没有加载图片") == 0) return AppStrings::Language::ChineseSimplified;
    if (wcscmp(AppStrings::OSD_NoImage, L"沒有載入圖片") == 0) return AppStrings::Language::ChineseTraditional;
    if (wcscmp(AppStrings::OSD_NoImage, L"画像が読み込まれていません") == 0) return AppStrings::Language::Japanese;
    if (wcscmp(AppStrings::OSD_NoImage, L"Изображение не загружено") == 0) return AppStrings::Language::Russian;
    if (wcscmp(AppStrings::OSD_NoImage, L"Kein Bild geladen") == 0) return AppStrings::Language::German;
    if (wcscmp(AppStrings::OSD_NoImage, L"No hay imagen cargada") == 0) return AppStrings::Language::Spanish;
    return AppStrings::Language::English;
}

std::wstring GetHotkeyActionName(HotkeyAction action) {
    const wchar_t* raw = nullptr;
    bool needsCleaning = true;
    
    switch (action) {
    case HotkeyAction::ToggleSlideshow: {
        needsCleaning = false;
        switch (GetActiveLanguage()) {
        case AppStrings::Language::ChineseSimplified:  raw = L"幻灯片模式 (播放/暂停)"; break;
        case AppStrings::Language::ChineseTraditional: raw = L"幻燈片模式 (播放/暫停)"; break;
        case AppStrings::Language::Japanese:           raw = L"スライドショー (再生/一時停止)"; break;
        case AppStrings::Language::Russian:            raw = L"Слайд-шоу (Запуск/Пауза)"; break;
        case AppStrings::Language::German:             raw = L"Diashow (Wiedergabe/Pause)"; break;
        case AppStrings::Language::Spanish:            raw = L"Presentación de diapositivas (Reproducir/Pausar)"; break;
        case AppStrings::Language::French:             raw = L"Diaporama (Lecture/Pause)"; break;
        default:                                       raw = L"Slideshow (Play/Pause)"; break;
        }
        break;
    }
    case HotkeyAction::Loupe: {
        needsCleaning = false;
        switch (GetActiveLanguage()) {
        case AppStrings::Language::ChineseSimplified:  raw = L"放大镜 (按住)"; break;
        case AppStrings::Language::ChineseTraditional: raw = L"放大鏡 (按住)"; break;
        case AppStrings::Language::Japanese:           raw = L"ルーペ (長押し)"; break;
        case AppStrings::Language::Russian:            raw = L"Лупа (удерживать)"; break;
        case AppStrings::Language::German:             raw = L"Lupe (halten)"; break;
        case AppStrings::Language::Spanish:            raw = L"Lupa (mantener)"; break;
        case AppStrings::Language::French:             raw = L"Loupe (maintenir)"; break;
        default:                                       raw = L"Loupe (Hold)"; break;
        }
        break;
    }
    case HotkeyAction::NavNext:
        raw = AppStrings::Toolbar_Tooltip_Next;
        break;
    case HotkeyAction::NavPrev:
        raw = AppStrings::Toolbar_Tooltip_Prev;
        break;
    case HotkeyAction::NavFirst: {
        needsCleaning = false;
        switch (GetActiveLanguage()) {
        case AppStrings::Language::ChineseSimplified:  raw = L"第一张图片"; break;
        case AppStrings::Language::ChineseTraditional: raw = L"第一張圖片"; break;
        case AppStrings::Language::Japanese:           raw = L"最初の画像"; break;
        case AppStrings::Language::Russian:            raw = L"Первое изображение"; break;
        case AppStrings::Language::German:             raw = L"Erstes Bild"; break;
        case AppStrings::Language::Spanish:            raw = L"Primera imagen"; break;
        case AppStrings::Language::French:             raw = L"Première image"; break;
        default:                                       raw = L"First Image"; break;
        }
        break;
    }
    case HotkeyAction::NavLast: {
        needsCleaning = false;
        switch (GetActiveLanguage()) {
        case AppStrings::Language::ChineseSimplified:  raw = L"最后一张"; break;
        case AppStrings::Language::ChineseTraditional: raw = L"最後一張"; break;
        case AppStrings::Language::Japanese:           raw = L"最後の画像"; break;
        case AppStrings::Language::Russian:            raw = L"Последнее изображение"; break;
        case AppStrings::Language::German:             raw = L"Letztes Bild"; break;
        case AppStrings::Language::Spanish:            raw = L"Última imagen"; break;
        case AppStrings::Language::French:             raw = L"Dernière image"; break;
        default:                                       raw = L"Last Image"; break;
        }
        break;
    }
    case HotkeyAction::ZoomIn:
        raw = AppStrings::Context_ZoomIn;
        break;
    case HotkeyAction::ZoomInFine: {
        needsCleaning = false;
        switch (GetActiveLanguage()) {
        case AppStrings::Language::ChineseSimplified:  raw = L"放大微调"; break;
        case AppStrings::Language::ChineseTraditional: raw = L"放大微調"; break;
        case AppStrings::Language::Japanese:           raw = L"ズームイン (微調整)"; break;
        case AppStrings::Language::Russian:            raw = L"Увеличить (точно)"; break;
        case AppStrings::Language::German:             raw = L"Vergrößern (Fein)"; break;
        case AppStrings::Language::Spanish:            raw = L"Acercar (Ajuste fino)"; break;
        case AppStrings::Language::French:             raw = L"Zoom avant (Précis)"; break;
        default:                                       raw = L"Zoom In (Fine)"; break;
        }
        break;
    }
    case HotkeyAction::ZoomOut:
        raw = AppStrings::Context_ZoomOut;
        break;
    case HotkeyAction::ZoomOutFine: {
        needsCleaning = false;
        switch (GetActiveLanguage()) {
        case AppStrings::Language::ChineseSimplified:  raw = L"缩小微调"; break;
        case AppStrings::Language::ChineseTraditional: raw = L"縮小微調"; break;
        case AppStrings::Language::Japanese:           raw = L"ズームアウト (微調整)"; break;
        case AppStrings::Language::Russian:            raw = L"Уменьшить (точно)"; break;
        case AppStrings::Language::German:             raw = L"Verkleinern (Fein)"; break;
        case AppStrings::Language::Spanish:            raw = L"Alejar (Ajuste fino)"; break;
        case AppStrings::Language::French:             raw = L"Zoom arrière (Précis)"; break;
        default:                                       raw = L"Zoom Out (Fine)"; break;
        }
        break;
    }
    case HotkeyAction::Zoom100:
        raw = AppStrings::Context_ActualSize;
        break;
    case HotkeyAction::ZoomFit:
        raw = AppStrings::Context_FitToScreen;
        break;
    case HotkeyAction::ZoomFitWindow:
        raw = AppStrings::Context_FitWindow;
        break;
    case HotkeyAction::ZoomFill:
        raw = AppStrings::Context_FillWindow;
        break;
    case HotkeyAction::RotateCW:
        raw = AppStrings::Context_RotateCW;
        break;
    case HotkeyAction::RotateCCW:
        raw = AppStrings::Context_RotateCCW;
        break;
    case HotkeyAction::FlipH:
        raw = AppStrings::Context_FlipH;
        break;
    case HotkeyAction::FlipV:
        raw = AppStrings::Context_FlipV;
        break;
    case HotkeyAction::RenderRaw:
        raw = AppStrings::Context_RenderRAW;
        break;
    case HotkeyAction::ComparePair: {
        needsCleaning = false;
        switch (GetActiveLanguage()) {
        case AppStrings::Language::ChineseSimplified:  raw = L"配对对比 (直出 vs RAW)"; break;
        case AppStrings::Language::ChineseTraditional: raw = L"配對對比 (直出 vs RAW)"; break;
        case AppStrings::Language::Japanese:           raw = L"ペア比較 (撮って出し vs RAW)"; break;
        case AppStrings::Language::Russian:            raw = L"Сравнение пары (камерный снимок и RAW)"; break;
        case AppStrings::Language::German:             raw = L"Paarvergleich (Kamerabild vs. RAW)"; break;
        case AppStrings::Language::Spanish:            raw = L"Comparar par (imagen de cámara vs RAW)"; break;
        case AppStrings::Language::French:             raw = L"Comparer la paire (image boîtier vs RAW)"; break;
        default:                                       raw = L"Compare Pair (Rendered vs RAW)"; break;
        }
        break;
    }
    case HotkeyAction::ToggleAnimation: {
        // Special case: returns concatenated string
        std::wstring play = CleanLabel(AppStrings::Toolbar_Tooltip_AnimPlay);
        std::wstring pause = CleanLabel(AppStrings::Toolbar_Tooltip_AnimPause);
        return play + L"/" + pause;
    }
    case HotkeyAction::AnimNextFrame:
        raw = AppStrings::Toolbar_Tooltip_AnimNext;
        break;
    case HotkeyAction::AnimPrevFrame:
        raw = AppStrings::Toolbar_Tooltip_AnimPrev;
        break;
    case HotkeyAction::ToggleGallery:
        raw = AppStrings::Context_HUDGallery;
        break;
    case HotkeyAction::ToggleInfoPanel:
        raw = AppStrings::Context_LiteInfoPanel;
        break;
    case HotkeyAction::ToggleExifPanel:
        raw = AppStrings::Context_FullInfoPanel;
        break;
    case HotkeyAction::ToggleFullscreen:
        raw = AppStrings::Context_Fullscreen;
        break;
    case HotkeyAction::ToggleSpan:
        needsCleaning = false;
        raw = AppStrings::Settings_Label_SpanDisplays;
        break;
    case HotkeyAction::OpenFile:
        raw = AppStrings::Context_Open;
        break;
    case HotkeyAction::EditFile:
        raw = AppStrings::Context_Edit;
        break;
    case HotkeyAction::RenameFile:
        raw = AppStrings::Context_Rename;
        break;
    case HotkeyAction::DeleteFile:
        raw = AppStrings::Context_Delete;
        break;
    case HotkeyAction::CopyImage:
        raw = AppStrings::Context_CopyImage;
        break;
    case HotkeyAction::CopyPath:
        raw = AppStrings::Context_CopyPath;
        break;
    case HotkeyAction::ToggleCompare:
        raw = AppStrings::Context_CompareMode;
        break;
    case HotkeyAction::AlwaysOnTop:
        raw = AppStrings::Context_AlwaysOnTop;
        break;
    case HotkeyAction::ToggleDebugHud:
        raw = AppStrings::Settings_Label_DebugHUD;
        break;
    case HotkeyAction::Print:
        raw = AppStrings::Context_Print;
        break;
    case HotkeyAction::ToggleOverlay:
        raw = AppStrings::Context_OverlayMode;
        break;
    case HotkeyAction::OverlayAlphaUp: {
        needsCleaning = false;
        switch (GetActiveLanguage()) {
        case AppStrings::Language::ChineseSimplified:  raw = L"增加透明度"; break;
        case AppStrings::Language::ChineseTraditional: raw = L"增加透明度"; break;
        case AppStrings::Language::Japanese:           raw = L"不透明度を上げる"; break;
        case AppStrings::Language::Russian:            raw = L"Увеличить непрозрачность"; break;
        case AppStrings::Language::German:             raw = L"Deckkraft erhöhen"; break;
        case AppStrings::Language::Spanish:            raw = L"Aumentar opacidad"; break;
        case AppStrings::Language::French:             raw = L"Augmenter l'opacité"; break;
        default:                                       raw = L"Increase Opacity"; break;
        }
        break;
    }
    case HotkeyAction::OverlayAlphaDown: {
        needsCleaning = false;
        switch (GetActiveLanguage()) {
        case AppStrings::Language::ChineseSimplified:  raw = L"减少透明度"; break;
        case AppStrings::Language::ChineseTraditional: raw = L"減少透明度"; break;
        case AppStrings::Language::Japanese:           raw = L"不透明度を下げる"; break;
        case AppStrings::Language::Russian:            raw = L"Уменьшить непрозрачность"; break;
        case AppStrings::Language::German:             raw = L"Deckkraft verringern"; break;
        case AppStrings::Language::Spanish:            raw = L"Disminuir opacidad"; break;
        case AppStrings::Language::French:             raw = L"Diminuer l'opacité"; break;
        default:                                       raw = L"Decrease Opacity"; break;
        }
        break;
    }
    case HotkeyAction::OverlayTogglePassthrough:
        raw = AppStrings::Menu_ExitPassthrough;
        break;
    case HotkeyAction::Help:
        raw = AppStrings::Settings_Link_Hotkeys;
        break;
    case HotkeyAction::Exit:
        raw = AppStrings::Context_Exit;
        break;
    case HotkeyAction::PanUp: {
        needsCleaning = false;
        switch (GetActiveLanguage()) {
        case AppStrings::Language::ChineseSimplified:  raw = L"向上平移"; break;
        case AppStrings::Language::ChineseTraditional: raw = L"向上平移"; break;
        case AppStrings::Language::Japanese:           raw = L"上にパン"; break;
        case AppStrings::Language::Russian:            raw = L"Сдвиг вверх"; break;
        case AppStrings::Language::German:             raw = L"Nach oben verschieben"; break;
        case AppStrings::Language::Spanish:            raw = L"Pan hacia arriba"; break;
        case AppStrings::Language::French:             raw = L"Déplacer vers le haut"; break;
        default:                                       raw = L"Pan Up"; break;
        }
        break;
    }
    case HotkeyAction::PanDown: {
        needsCleaning = false;
        switch (GetActiveLanguage()) {
        case AppStrings::Language::ChineseSimplified:  raw = L"向下平移"; break;
        case AppStrings::Language::ChineseTraditional: raw = L"向下平移"; break;
        case AppStrings::Language::Japanese:           raw = L"下にパン"; break;
        case AppStrings::Language::Russian:            raw = L"Сдвиг вниз"; break;
        case AppStrings::Language::German:             raw = L"Nach unten verschieben"; break;
        case AppStrings::Language::Spanish:            raw = L"Pan hacia abajo"; break;
        case AppStrings::Language::French:             raw = L"Déplacer vers le bas"; break;
        default:                                       raw = L"Pan Down"; break;
        }
        break;
    }
    case HotkeyAction::PanLeft: {
        needsCleaning = false;
        switch (GetActiveLanguage()) {
        case AppStrings::Language::ChineseSimplified:  raw = L"向左平移"; break;
        case AppStrings::Language::ChineseTraditional: raw = L"向左平移"; break;
        case AppStrings::Language::Japanese:           raw = L"左にパン"; break;
        case AppStrings::Language::Russian:            raw = L"Сдвиг влево"; break;
        case AppStrings::Language::German:             raw = L"Nach links verschieben"; break;
        case AppStrings::Language::Spanish:            raw = L"Pan hacia la izquierda"; break;
        case AppStrings::Language::French:             raw = L"Déplacer vers la gauche"; break;
        default:                                       raw = L"Pan Left"; break;
        }
        break;
    }
    case HotkeyAction::PanRight: {
        needsCleaning = false;
        switch (GetActiveLanguage()) {
        case AppStrings::Language::ChineseSimplified:  raw = L"向右平移"; break;
        case AppStrings::Language::ChineseTraditional: raw = L"向右平移"; break;
        case AppStrings::Language::Japanese:           raw = L"右にパン"; break;
        case AppStrings::Language::Russian:            raw = L"Сдвиг вправо"; break;
        case AppStrings::Language::German:             raw = L"Nach rechts verschieben"; break;
        case AppStrings::Language::Spanish:            raw = L"Pan hacia la derecha"; break;
        case AppStrings::Language::French:             raw = L"Déplacer vers la droite"; break;
        default:                                       raw = L"Pan Right"; break;
        }
        break;
    }
    case HotkeyAction::PanUpFast: {
        needsCleaning = false;
        switch (GetActiveLanguage()) {
        case AppStrings::Language::ChineseSimplified:  raw = L"向上平移 (加速)"; break;
        case AppStrings::Language::ChineseTraditional: raw = L"向上平移 (加速)"; break;
        case AppStrings::Language::Japanese:           raw = L"上にパン (高速)"; break;
        case AppStrings::Language::Russian:            raw = L"Быстрый сдвиг вверх"; break;
        case AppStrings::Language::German:             raw = L"Nach oben verschieben (schnell)"; break;
        case AppStrings::Language::Spanish:            raw = L"Pan hacia arriba (rápido)"; break;
        case AppStrings::Language::French:             raw = L"Déplacer vers le haut (rapide)"; break;
        default:                                       raw = L"Pan Up (Fast)"; break;
        }
        break;
    }
    case HotkeyAction::PanDownFast: {
        needsCleaning = false;
        switch (GetActiveLanguage()) {
        case AppStrings::Language::ChineseSimplified:  raw = L"向下平移 (加速)"; break;
        case AppStrings::Language::ChineseTraditional: raw = L"向下平移 (加速)"; break;
        case AppStrings::Language::Japanese:           raw = L"下にパン (高速)"; break;
        case AppStrings::Language::Russian:            raw = L"Быстрый сдвиг вниз"; break;
        case AppStrings::Language::German:             raw = L"Nach unten verschieben (schnell)"; break;
        case AppStrings::Language::Spanish:            raw = L"Pan hacia abajo (rápido)"; break;
        case AppStrings::Language::French:             raw = L"Déplacer vers le bas (rapide)"; break;
        default:                                       raw = L"Pan Down (Fast)"; break;
        }
        break;
    }
    case HotkeyAction::PanLeftFast: {
        needsCleaning = false;
        switch (GetActiveLanguage()) {
        case AppStrings::Language::ChineseSimplified:  raw = L"向左平移 (加速)"; break;
        case AppStrings::Language::ChineseTraditional: raw = L"向左平移 (加速)"; break;
        case AppStrings::Language::Japanese:           raw = L"左にパン (高速)"; break;
        case AppStrings::Language::Russian:            raw = L"Быстрый сдвиг влево"; break;
        case AppStrings::Language::German:             raw = L"Nach links verschieben (schnell)"; break;
        case AppStrings::Language::Spanish:            raw = L"Pan hacia la izquierda (rápido)"; break;
        case AppStrings::Language::French:             raw = L"Déplacer vers la gauche (rapide)"; break;
        default:                                       raw = L"Pan Left (Fast)"; break;
        }
        break;
    }
    case HotkeyAction::PanRightFast: {
        needsCleaning = false;
        switch (GetActiveLanguage()) {
        case AppStrings::Language::ChineseSimplified:  raw = L"向右平移 (加速)"; break;
        case AppStrings::Language::ChineseTraditional: raw = L"向右平移 (加速)"; break;
        case AppStrings::Language::Japanese:           raw = L"右にパン (高速)"; break;
        case AppStrings::Language::Russian:            raw = L"Быстрый сдвиг вправо"; break;
        case AppStrings::Language::German:             raw = L"Nach rechts verschieben (schnell)"; break;
        case AppStrings::Language::Spanish:            raw = L"Pan hacia la derecha (rápido)"; break;
        case AppStrings::Language::French:             raw = L"Déplacer vers la droite (rapide)"; break;
        default:                                       raw = L"Pan Right (Fast)"; break;
        }
        break;
    }
    case HotkeyAction::Undo:
        raw = AppStrings::Context_Undo;
        break;
    default:
        needsCleaning = false;
        raw = L"None";
        break;
    }

    if (!raw) return L"";
    return needsCleaning ? CleanLabel(raw) : std::wstring(raw);
}
}
