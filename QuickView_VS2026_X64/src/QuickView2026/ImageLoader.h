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
    /// Get image size without full decode
    /// </summary>
    HRESULT GetImageSize(LPCWSTR filePath, UINT* width, UINT* height);

private:
    ComPtr<IWICImagingFactory> m_wicFactory;
};
