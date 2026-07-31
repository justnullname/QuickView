#include "pch.h"
#include "ImageExporter.h"
#include <wincodec.h>
#include <shlwapi.h>
#include <windows.h>
#include <algorithm>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shlwapi.lib")

using namespace Microsoft::WRL;

namespace QuickView {

class CountingStream : public IStream {
    uint64_t m_size = 0;
    ULONG m_ref = 1;
public:
    virtual ~CountingStream() = default;
    uint64_t GetSize() const { return m_size; }
    
    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (riid == IID_IUnknown || riid == IID_IStream || riid == IID_ISequentialStream) {
            *ppvObject = this;
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override { 
        ULONG res = InterlockedDecrement(&m_ref);
        if (res == 0) delete this;
        return res;
    }

    // ISequentialStream
    HRESULT STDMETHODCALLTYPE Read(void* pv, ULONG cb, ULONG* pcbRead) override {
        (void)pv; (void)cb; (void)pcbRead;
        return E_NOTIMPL; 
    }
    HRESULT STDMETHODCALLTYPE Write(const void* pv, ULONG cb, ULONG* pcbWritten) override {
        (void)pv;
        m_size += cb;
        if (pcbWritten) *pcbWritten = cb;
        return S_OK;
    }

    // IStream
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER dlibMove, DWORD dwOrigin, ULARGE_INTEGER* plibNewPosition) override {
        (void)dlibMove; (void)dwOrigin;
        if (plibNewPosition) plibNewPosition->QuadPart = m_size;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER libNewSize) override { 
        (void)libNewSize;
        return E_NOTIMPL; 
    }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream* pstm, ULARGE_INTEGER cb, ULARGE_INTEGER* pcbRead, ULARGE_INTEGER* pcbWritten) override { 
        (void)pstm; (void)cb; (void)pcbRead; (void)pcbWritten;
        return E_NOTIMPL; 
    }
    HRESULT STDMETHODCALLTYPE Commit(DWORD grfCommitFlags) override { 
        (void)grfCommitFlags;
        return S_OK; 
    }
    HRESULT STDMETHODCALLTYPE Revert() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER libOffset, ULARGE_INTEGER cb, DWORD dwLockType) override { 
        (void)libOffset; (void)cb; (void)dwLockType;
        return E_NOTIMPL; 
    }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER libOffset, ULARGE_INTEGER cb, DWORD dwLockType) override { 
        (void)libOffset; (void)cb; (void)dwLockType;
        return E_NOTIMPL; 
    }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* pstatstg, DWORD grfStatFlag) override {
        (void)grfStatFlag;
        if (pstatstg) pstatstg->cbSize.QuadPart = m_size;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IStream** ppstm) override { 
        (void)ppstm;
        return E_NOTIMPL; 
    }
};

HRESULT ImageExporter::CreateWICPipeline(const ExportOptions& options,
                                         ComPtr<IWICBitmapSource>& outSource,
                                         ComPtr<IWICImagingFactory>& factory,
                                         ComPtr<IWICColorContext>& outColorContext) {
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;

    ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromFilename(options.InputPath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr)) return hr;

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) return hr;

    // Extract ICC Color Context if requested
    if (options.EmbedIcc) {
        UINT count = 0;
        if (SUCCEEDED(frame->GetColorContexts(0, nullptr, &count)) && count > 0) {
            if (SUCCEEDED(factory->CreateColorContext(&outColorContext))) {
                UINT actualCount = 0;
                IWICColorContext* pContext = outColorContext.Get();
                frame->GetColorContexts(1, &pContext, &actualCount);
            }
        }
    }

    ComPtr<IWICBitmapSource> currentSource = frame;

    // Get Original Frame Size for Boundary Clamp
    UINT origWidth = 0, origHeight = 0;
    currentSource->GetSize(&origWidth, &origHeight);

    // Apply Flip / Rotation Transforms before cropping if specified
    if (options.Rotation != 0 || options.FlipH || options.FlipV) {
        ComPtr<IWICBitmapFlipRotator> flipRotator;
        hr = factory->CreateBitmapFlipRotator(&flipRotator);
        if (SUCCEEDED(hr)) {
            WICBitmapTransformOptions transformOpt = WICBitmapTransformRotate0;
            int rot = (options.Rotation % 360 + 360) % 360;
            if (rot == 90) transformOpt = WICBitmapTransformRotate90;
            else if (rot == 180) transformOpt = WICBitmapTransformRotate180;
            else if (rot == 270) transformOpt = WICBitmapTransformRotate270;

            if (options.FlipH) transformOpt = static_cast<WICBitmapTransformOptions>(transformOpt | WICBitmapTransformFlipHorizontal);
            if (options.FlipV) transformOpt = static_cast<WICBitmapTransformOptions>(transformOpt | WICBitmapTransformFlipVertical);

            if (SUCCEEDED(flipRotator->Initialize(currentSource.Get(), transformOpt))) {
                currentSource = flipRotator;
                currentSource->GetSize(&origWidth, &origHeight);
            }
        }
    }

    // Apply Crop with strict boundary clamp
    if (options.CropWidth > 0 && options.CropHeight > 0) {
        int cx = std::clamp(options.CropX, 0, (int)origWidth - 1);
        int cy = std::clamp(options.CropY, 0, (int)origHeight - 1);
        int cw = std::clamp(options.CropWidth, 1, (int)origWidth - cx);
        int ch = std::clamp(options.CropHeight, 1, (int)origHeight - cy);

        ComPtr<IWICBitmapClipper> clipper;
        hr = factory->CreateBitmapClipper(&clipper);
        if (FAILED(hr)) return hr;

        WICRect rect = { cx, cy, cw, ch };
        hr = clipper->Initialize(currentSource.Get(), &rect);
        if (FAILED(hr)) return hr;
        currentSource = clipper;
    }

    // Apply Scaling
    if (options.TargetWidth > 0 && options.TargetHeight > 0 && 
        (options.TargetWidth != options.CropWidth || options.TargetHeight != options.CropHeight)) {
        ComPtr<IWICBitmapScaler> scaler;
        hr = factory->CreateBitmapScaler(&scaler);
        if (FAILED(hr)) return hr;

        hr = scaler->Initialize(currentSource.Get(), options.TargetWidth, options.TargetHeight, WICBitmapInterpolationModeHighQualityCubic);
        if (FAILED(hr)) return hr;
        currentSource = scaler;
    }

    outSource = currentSource;
    return S_OK;
}

std::expected<void, std::wstring> ImageExporter::Export(const ExportOptions& options) {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapSource> source;
    ComPtr<IWICColorContext> colorContext;
    
    HRESULT hr = CreateWICPipeline(options, source, factory, colorContext);
    if (FAILED(hr)) return std::unexpected(L"Failed to create image pipeline.");

    // Determine encoder by extension
    const wchar_t* ext = PathFindExtensionW(options.OutputPath.c_str());
    GUID containerFormat = GUID_ContainerFormatJpeg;
    if (_wcsicmp(ext, L".png") == 0) {
        containerFormat = GUID_ContainerFormatPng;
    } else if (_wcsicmp(ext, L".bmp") == 0) {
        containerFormat = GUID_ContainerFormatBmp;
    } else if (_wcsicmp(ext, L".tif") == 0 || _wcsicmp(ext, L".tiff") == 0) {
        containerFormat = GUID_ContainerFormatTiff;
    }

    ComPtr<IWICStream> stream;
    hr = factory->CreateStream(&stream);
    if (FAILED(hr)) return std::unexpected(L"Failed to create output stream.");

    hr = stream->InitializeFromFilename(options.OutputPath.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) return std::unexpected(L"Failed to open output file.");

    ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(containerFormat, nullptr, &encoder);
    if (FAILED(hr)) return std::unexpected(L"Failed to create encoder.");

    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) return std::unexpected(L"Failed to initialize encoder.");

    ComPtr<IWICBitmapFrameEncode> frameEncode;
    ComPtr<IPropertyBag2> props;
    hr = encoder->CreateNewFrame(&frameEncode, &props);
    if (FAILED(hr)) return std::unexpected(L"Failed to create encoder frame.");

    // Set JPEG Quality
    if (containerFormat == GUID_ContainerFormatJpeg) {
        PROPBAG2 option = {};
        option.pstrName = (LPOLESTR)L"ImageQuality";
        VARIANT varValue;
        VariantInit(&varValue);
        varValue.vt = VT_R4;
        varValue.fltVal = std::clamp(options.JpegQuality, 1, 100) / 100.0f;
        props->Write(1, &option, &varValue);
    }

    hr = frameEncode->Initialize(props.Get());
    if (FAILED(hr)) return std::unexpected(L"Failed to initialize frame encode.");

    // Embed Color Context
    if (colorContext && options.EmbedIcc) {
        IWICColorContext* pCtx = colorContext.Get();
        frameEncode->SetColorContexts(1, &pCtx);
    }

    hr = frameEncode->WriteSource(source.Get(), nullptr);
    if (FAILED(hr)) return std::unexpected(L"Failed to write image data.");

    hr = frameEncode->Commit();
    if (FAILED(hr)) return std::unexpected(L"Failed to commit frame.");

    hr = encoder->Commit();
    if (FAILED(hr)) return std::unexpected(L"Failed to commit encoder.");

    return {};
}

std::expected<uint64_t, std::wstring> ImageExporter::EstimateSize(const ExportOptions& options) {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapSource> source;
    ComPtr<IWICColorContext> colorContext;
    
    HRESULT hr = CreateWICPipeline(options, source, factory, colorContext);
    if (FAILED(hr)) return std::unexpected(L"Pipeline error.");

    const wchar_t* ext = PathFindExtensionW(options.OutputPath.c_str());
    GUID containerFormat = GUID_ContainerFormatJpeg;
    if (_wcsicmp(ext, L".png") == 0) containerFormat = GUID_ContainerFormatPng;
    else if (_wcsicmp(ext, L".bmp") == 0) containerFormat = GUID_ContainerFormatBmp;
    else if (_wcsicmp(ext, L".tif") == 0 || _wcsicmp(ext, L".tiff") == 0) containerFormat = GUID_ContainerFormatTiff;

    ComPtr<IStream> memStream;
    CountingStream* countingStream = new CountingStream();
    memStream.Attach(countingStream); // memStream will manage the ref count

    ComPtr<IWICStream> wicStream;
    hr = factory->CreateStream(&wicStream);
    if (FAILED(hr)) return std::unexpected(L"WIC Stream error.");

    hr = wicStream->InitializeFromIStream(memStream.Get());
    if (FAILED(hr)) return std::unexpected(L"WIC Stream init error.");

    ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(containerFormat, nullptr, &encoder);
    if (FAILED(hr)) return std::unexpected(L"Encoder error.");

    hr = encoder->Initialize(wicStream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) return std::unexpected(L"Encoder init error.");

    ComPtr<IWICBitmapFrameEncode> frameEncode;
    ComPtr<IPropertyBag2> props;
    hr = encoder->CreateNewFrame(&frameEncode, &props);
    if (FAILED(hr)) return std::unexpected(L"Frame error.");

    if (containerFormat == GUID_ContainerFormatJpeg) {
        PROPBAG2 option = {};
        option.pstrName = (LPOLESTR)L"ImageQuality";
        VARIANT varValue;
        VariantInit(&varValue);
        varValue.vt = VT_R4;
        varValue.fltVal = std::clamp(options.JpegQuality, 1, 100) / 100.0f;
        props->Write(1, &option, &varValue);
    }

    hr = frameEncode->Initialize(props.Get());
    if (FAILED(hr)) return std::unexpected(L"Frame init error.");

    if (colorContext && options.EmbedIcc) {
        IWICColorContext* pCtx = colorContext.Get();
        frameEncode->SetColorContexts(1, &pCtx);
    }

    hr = frameEncode->WriteSource(source.Get(), nullptr);
    if (FAILED(hr)) return std::unexpected(L"Write error.");

    hr = frameEncode->Commit();
    if (FAILED(hr)) return std::unexpected(L"Commit error.");

    hr = encoder->Commit();
    if (FAILED(hr)) return std::unexpected(L"Commit encoder error.");

    return countingStream->GetSize();
}

std::expected<void, std::wstring> ImageExporter::CopyToClipboard(const ExportOptions& options, HWND hwnd) {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapSource> source;
    ComPtr<IWICColorContext> colorContext;
    
    HRESULT hr = CreateWICPipeline(options, source, factory, colorContext);
    if (FAILED(hr)) return std::unexpected(L"Failed to create image pipeline.");

    // Convert to BGRA for DIB
    ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(&converter);
    if (FAILED(hr)) return std::unexpected(L"Failed to create format converter.");

    hr = converter->Initialize(source.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return std::unexpected(L"Failed to initialize format converter.");

    UINT width, height;
    converter->GetSize(&width, &height);

    const UINT stride = width * 4;
    const UINT imageSize = stride * height;
    
    // Allocate global memory for DIB (BITMAPV5HEADER)
    const UINT dibSize = sizeof(BITMAPV5HEADER) + imageSize;
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, dibSize);
    if (!hMem) return std::unexpected(L"Failed to allocate clipboard memory.");

    uint8_t* pData = static_cast<uint8_t*>(GlobalLock(hMem));
    if (!pData) {
        GlobalFree(hMem);
        return std::unexpected(L"Failed to lock clipboard memory.");
    }

    BITMAPV5HEADER* bmi = reinterpret_cast<BITMAPV5HEADER*>(pData);
    ZeroMemory(bmi, sizeof(BITMAPV5HEADER));
    bmi->bV5Size = sizeof(BITMAPV5HEADER);
    bmi->bV5Width = width;
    bmi->bV5Height = -static_cast<LONG>(height); // Top-down
    bmi->bV5Planes = 1;
    bmi->bV5BitCount = 32;
    bmi->bV5Compression = BI_BITFIELDS;
    bmi->bV5RedMask   = 0x00FF0000;
    bmi->bV5GreenMask = 0x0000FF00;
    bmi->bV5BlueMask  = 0x000000FF;
    bmi->bV5AlphaMask = 0xFF000000;

    uint8_t* pixels = pData + sizeof(BITMAPV5HEADER);
    hr = converter->CopyPixels(nullptr, stride, imageSize, pixels);
    GlobalUnlock(hMem);

    if (FAILED(hr)) {
        GlobalFree(hMem);
        return std::unexpected(L"Failed to copy pixels.");
    }

    if (!OpenClipboard(hwnd)) {
        GlobalFree(hMem);
        return std::unexpected(L"Failed to open clipboard.");
    }

    EmptyClipboard();
    SetClipboardData(CF_DIBV5, hMem);
    CloseClipboard();

    return {};
}

} // namespace QuickView
