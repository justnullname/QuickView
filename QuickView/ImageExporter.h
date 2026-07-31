#pragma once
#include "pch.h"
#include <string>
#include <wincodec.h>
#include <wrl/client.h>
#include <expected>

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
    
    // JPEG Quality (1-100)
    int JpegQuality = 90;
    
    // Rotation and Flip transform
    int Rotation = 0; // 0, 90, 180, 270
    bool FlipH = false;
    bool FlipV = false;

    // ICC Profile embedding
    bool EmbedIcc = true;
    std::wstring IccProfilePath;
};

class ImageExporter {
public:
    // Exports the cropped and scaled image to the specified path
    static std::expected<void, std::wstring> Export(const ExportOptions& options);
    
    // Copies the cropped and scaled image to the clipboard
    static std::expected<void, std::wstring> CopyToClipboard(const ExportOptions& options, HWND hwnd);

    // Asynchronously or synchronously estimates output file size in bytes
    static std::expected<uint64_t, std::wstring> EstimateSize(const ExportOptions& options);

private:
    static HRESULT CreateWICPipeline(const ExportOptions& options,
                                     Microsoft::WRL::ComPtr<IWICBitmapSource>& outSource,
                                     Microsoft::WRL::ComPtr<IWICImagingFactory>& factory,
                                     Microsoft::WRL::ComPtr<IWICColorContext>& outColorContext);
};

} // namespace QuickView
