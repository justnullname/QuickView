#pragma once
#include "pch.h"
#include <string>
#include <wincodec.h>
#include <wrl/client.h>
#include <expected>

#define WM_CLIPBOARD_PRERENDER_READY (WM_USER + 102)

namespace QuickView {

struct ExportOptions {
    std::wstring InputPath;
    std::wstring OutputPath;
    
    // Crop region in original pixels
    int CropX = 0;
    int CropY = 0;
    int CropWidth = 0;
    int CropHeight = 0;
    
    // Target resolution (0 means use crop width/height)
    int TargetWidth = 0;
    int TargetHeight = 0;
    
    // JPEG / WebP / JXR Quality (1-100)
    int JpegQuality = 90;
    
    // Lossless encoding mode for WebP, JXR, HEIF etc.
    bool Lossless = false;
    
    // Rotation and Flip transform
    int Rotation = 0; // 0, 90, 180, 270
    bool FlipH = false;
    bool FlipV = false;

    // Display zoom for true vector crop rasterization
    float DisplayZoom = 1.0f;
    // RAW decode mode (false = smart embedded JPEG preview ~10ms; true = full demosaic)
    bool RawForceFullDecode = false;

    // Metadata & ICC Profile embedding
    bool PreserveMetadata = true; // Copy EXIF / IPTC / XMP metadata blocks
    bool EmbedIcc = true;
    std::wstring IccProfilePath;
    std::vector<uint8_t> CustomIccData;
    // Optional in-memory source frame (e.g. for animations or memory bitmaps)
    std::shared_ptr<struct RawImageFrame> SourceFrame = nullptr;
};

struct PendingClipboardSnapshot {
    std::wstring filePath;
    ExportOptions options;
    std::shared_ptr<struct RawImageFrame> memoryFrame = nullptr;
    bool isValid = false;
};

struct ExportFormatDesc {
    std::wstring DisplayName;
    std::wstring Ext; // Primary extension e.g. ".jpg"
    GUID ContainerGuid = {};
    bool SupportsLosslessSwitch = false;
    bool SupportsQuality = false;
};

class ImageExporter {
public:
    // Returns available export formats supported by WIC on current OS
    static std::vector<ExportFormatDesc> GetSupportedExportFormats();

    // Exports the cropped and scaled image to the specified path
    static std::expected<void, std::wstring> Export(const ExportOptions& options);
    
    // Delayed Rendering Windows Clipboard API (0ms latency on copy)
    static UINT GetPngClipboardFormat();
    static std::expected<void, std::wstring> SetupDelayedClipboard(HWND hwnd, const PendingClipboardSnapshot& snapshot);
    static HGLOBAL RenderClipboardFormat(UINT uFormat, const PendingClipboardSnapshot& snapshot);
    static void RenderAllClipboardFormats(HWND hwnd, const PendingClipboardSnapshot& snapshot);
    static void CancelPreRendering();

    // Direct synchronous copy fallback
    static std::expected<void, std::wstring> CopyToClipboard(const ExportOptions& options, HWND hwnd);

    // Asynchronously or synchronously estimates output file size in bytes
    static std::expected<uint64_t, std::wstring> EstimateSize(const ExportOptions& options);

    static bool FormatSupportsExifOrientation(const wchar_t* ext);

private:
    static HRESULT CreateWICPipeline(const ExportOptions& options,
                                     Microsoft::WRL::ComPtr<IWICBitmapSource>& outSource,
                                     Microsoft::WRL::ComPtr<IWICImagingFactory>& factory,
                                     Microsoft::WRL::ComPtr<IWICColorContext>& outColorContext,
                                     Microsoft::WRL::ComPtr<IWICBitmapFrameDecode>& outFrameDecode);
    static HGLOBAL RenderClipboardFormatDirect(UINT uFormat, const PendingClipboardSnapshot& snapshot);
    static void RenderClipboardDual(const PendingClipboardSnapshot& snapshot, HGLOBAL& outPng, HGLOBAL& outDib);
    static GUID GetContainerFormatFromExtension(const wchar_t* ext);
};

extern bool g_isSettingUpClipboard;

} // namespace QuickView
