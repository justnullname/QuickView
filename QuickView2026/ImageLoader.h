#pragma once
#include "pch.h"

/// <summary>
/// Image Loader
/// Uses WIC to load image files
/// </summary>
class CImageLoader {
public:
    CImageLoader() = default;
    ~CImageLoader() = default;

    /// <summary>
    /// Initialize loader
    /// </summary>
    HRESULT Initialize(IWICImagingFactory* wicFactory);

    /// <summary>
    /// Load WIC bitmap from file
    /// </summary>
    HRESULT LoadFromFile(LPCWSTR filePath, IWICBitmapSource** bitmap);

    /// <summary>
    /// Load WIC bitmap from file and force decode to memory
    /// </summary>
    HRESULT LoadToMemory(LPCWSTR filePath, IWICBitmap** ppBitmap); // Force decode to memory

    /// <summary>
    /// Get image size without full decode
    /// </summary>
    HRESULT GetImageSize(LPCWSTR filePath, UINT* width, UINT* height);

    // Helper: Create WIC bitmap from raw bits
    HRESULT CreateWICBitmapFromMemory(UINT width, UINT height, REFGUID format, UINT stride, UINT size, BYTE* data, IWICBitmap** ppBitmap);

private:
    ComPtr<IWICImagingFactory> m_wicFactory;

    // Specialized High-Performance Loaders
    HRESULT LoadJPEG(LPCWSTR filePath, IWICBitmap** ppBitmap);  // libjpeg-turbo
    HRESULT LoadPNG(LPCWSTR filePath, IWICBitmap** ppBitmap);   // libpng + zlib-ng
    HRESULT LoadWebP(LPCWSTR filePath, IWICBitmap** ppBitmap);  // libwebp
    HRESULT LoadAVIF(LPCWSTR filePath, IWICBitmap** ppBitmap);  // libavif + dav1d
    HRESULT LoadJXL(LPCWSTR filePath, IWICBitmap** ppBitmap);   // libjxl
    HRESULT LoadRaw(LPCWSTR filePath, IWICBitmap** ppBitmap);   // libraw
};
