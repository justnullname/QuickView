#include "pch.h"
#include "ImageLoader.h"

HRESULT CImageLoader::Initialize(IWICImagingFactory* wicFactory) {
    if (!wicFactory) return E_INVALIDARG;
    m_wicFactory = wicFactory;
    return S_OK;
}

HRESULT CImageLoader::LoadFromFile(LPCWSTR filePath, IWICBitmapSource** bitmap) {
    if (!filePath || !bitmap) return E_INVALIDARG;
    if (!m_wicFactory) return E_FAIL;

    HRESULT hr = S_OK;

    // Use CacheOnDemand for better memory efficiency
    // File handle will be released when bitmap is Reset() before transform
    ComPtr<IWICBitmapDecoder> decoder;
    hr = m_wicFactory->CreateDecoderFromFilename(
        filePath,
        nullptr,
        GENERIC_READ,
        WICDecodeMetadataCacheOnDemand,  // Memory efficient, release before transform
        &decoder
    );
    if (FAILED(hr)) return hr;

    // Get first frame
    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) return hr;

    // Return frame directly - file handle released when this is Reset()
    *bitmap = frame.Detach();
    return S_OK;
}

HRESULT CImageLoader::GetImageSize(LPCWSTR filePath, UINT* width, UINT* height) {
    if (!filePath || !width || !height) return E_INVALIDARG;

    ComPtr<IWICBitmapSource> bitmap;
    HRESULT hr = LoadFromFile(filePath, &bitmap);
    if (FAILED(hr)) return hr;

    return bitmap->GetSize(width, height);
}
