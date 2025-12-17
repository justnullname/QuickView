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

HRESULT CImageLoader::LoadToMemory(LPCWSTR filePath, IWICBitmap** ppBitmap) {
    if (!filePath || !ppBitmap) return E_INVALIDARG;
    
    // 1. Load Lazy Source
    ComPtr<IWICBitmapSource> source;
    HRESULT hr = LoadFromFile(filePath, &source);
    if (FAILED(hr)) return hr;

    // 2. Convert to D2D Compatible Format (PBGRA32)
    ComPtr<IWICFormatConverter> converter;
    hr = m_wicFactory->CreateFormatConverter(&converter);
    if (FAILED(hr)) return hr;

    hr = converter->Initialize(
        source.Get(),
        GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone,
        nullptr,
        0.f,
        WICBitmapPaletteTypeMedianCut
    );
    if (FAILED(hr)) return hr;

    // 3. Force Decode to Memory (CreateBitmapFromSource)
    // This reads all pixels and stores them in a memory buffer.
    // Safe to do on background thread.
    return m_wicFactory->CreateBitmapFromSource(
        converter.Get(),
        WICBitmapCacheOnLoad, 
        ppBitmap
    );
}

HRESULT CImageLoader::GetImageSize(LPCWSTR filePath, UINT* width, UINT* height) {
    if (!filePath || !width || !height) return E_INVALIDARG;

    ComPtr<IWICBitmapSource> bitmap;
    HRESULT hr = LoadFromFile(filePath, &bitmap);
    if (FAILED(hr)) return hr;

    return bitmap->GetSize(width, height);
}
