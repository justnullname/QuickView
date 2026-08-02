#include "pch.h"
#include "ImageExporter.h"
#include <wincodec.h>
#include <wincodecsdk.h>
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

    // Extract or Create ICC Color Context if requested
    if (options.EmbedIcc) {
        if (!options.CustomIccData.empty()) {
            if (SUCCEEDED(factory->CreateColorContext(&outColorContext))) {
                outColorContext->InitializeFromMemory(options.CustomIccData.data(), static_cast<UINT>(options.CustomIccData.size()));
            }
        } else if (!options.IccProfilePath.empty()) {
            if (SUCCEEDED(factory->CreateColorContext(&outColorContext))) {
                outColorContext->InitializeFromFilename(options.IccProfilePath.c_str());
            }
        } else {
            UINT count = 0;
            if (SUCCEEDED(frame->GetColorContexts(0, nullptr, &count)) && count > 0) {
                if (SUCCEEDED(factory->CreateColorContext(&outColorContext))) {
                    UINT actualCount = 0;
                    IWICColorContext* pContext = outColorContext.Get();
                    frame->GetColorContexts(1, &pContext, &actualCount);
                }
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

static bool ProbeAvifEncoderSupport(IWICImagingFactory* factory, const GUID& containerGuid) {
    if (!factory) return false;

    IStream* pMemStream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(NULL, TRUE, &pMemStream))) return false;
    
    ComPtr<IWICStream> stream;
    HRESULT hr = factory->CreateStream(&stream);
    if (FAILED(hr)) {
        pMemStream->Release();
        return false;
    }
    
    hr = stream->InitializeFromIStream(pMemStream);
    if (FAILED(hr)) {
        pMemStream->Release();
        return false;
    }

    ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(containerGuid, nullptr, &encoder);
    if (FAILED(hr)) {
        pMemStream->Release();
        return false;
    }

    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) {
        pMemStream->Release();
        return false;
    }

    ComPtr<IWICBitmapFrameEncode> frameEncode;
    ComPtr<IPropertyBag2> props;
    hr = encoder->CreateNewFrame(&frameEncode, &props);
    if (FAILED(hr)) {
        pMemStream->Release();
        return false;
    }

    if (props) {
        PROPBAG2 optM = {};
        optM.pstrName = (LPOLESTR)L"HeifCompressionMethod";
        VARIANT varM; VariantInit(&varM);
        varM.vt = VT_UI1; varM.bVal = 3; // Enforce WICHeifCompressionAV1 (0x3)
        props->Write(1, &optM, &varM);

        PROPBAG2 optQ = {};
        optQ.pstrName = (LPOLESTR)L"ImageQuality";
        VARIANT varQ; VariantInit(&varQ);
        varQ.vt = VT_R4; varQ.fltVal = 0.9f;
        props->Write(1, &optQ, &varQ);
    }

    hr = frameEncode->Initialize(props.Get());
    if (FAILED(hr)) {
        pMemStream->Release();
        return false;
    }

    ComPtr<IWICBitmap> bitmap;
    hr = factory->CreateBitmap(1, 1, GUID_WICPixelFormat32bppBGRA, WICBitmapCacheOnLoad, &bitmap);
    if (FAILED(hr)) {
        pMemStream->Release();
        return false;
    }

    frameEncode->SetSize(1, 1);
    WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
    frameEncode->SetPixelFormat(&pf);
    if (FAILED(frameEncode->WriteSource(bitmap.Get(), nullptr))) {
        pMemStream->Release();
        return false;
    }

    if (FAILED(frameEncode->Commit())) {
        pMemStream->Release();
        return false;
    }

    bool success = SUCCEEDED(encoder->Commit());
    pMemStream->Release();
    return success;
}

std::vector<ExportFormatDesc> ImageExporter::GetSupportedExportFormats() {
    std::vector<ExportFormatDesc> result;

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
        return result;
    }

    ComPtr<IEnumUnknown> enumComponents;
    HRESULT hr = factory->CreateComponentEnumerator(WICEncoder, WICComponentEnumerateDefault, &enumComponents);
    if (FAILED(hr)) return result;

    ComPtr<IUnknown> element;
    ULONG fetched = 0;

    while (enumComponents->Next(1, &element, &fetched) == S_OK && fetched == 1) {
        ComPtr<IWICBitmapEncoderInfo> encoderInfo;
        if (FAILED(element.As(&encoderInfo))) continue;

        GUID containerGuid = {};
        if (FAILED(encoderInfo->GetContainerFormat(&containerGuid))) continue;

        ComPtr<IWICBitmapEncoder> encoder;
        if (FAILED(factory->CreateEncoder(containerGuid, nullptr, &encoder))) continue;

        wchar_t friendlyName[256] = {};
        UINT actualLen = 0;
        encoderInfo->GetFriendlyName(256, friendlyName, &actualLen);

        wchar_t extensions[256] = {};
        encoderInfo->GetFileExtensions(256, extensions, &actualLen);

        std::wstring extList = extensions;
        size_t commaPos = extList.find_first_of(L",; ");
        std::wstring primaryExt = (commaPos != std::wstring::npos) ? extList.substr(0, commaPos) : extList;
        
        if (primaryExt.empty()) continue;
        if (primaryExt[0] != L'.') primaryExt = L"." + primaryExt;

        std::wstring displayName = friendlyName;
        displayName += L" (*" + primaryExt + L")";

        bool supportsLossless = (_wcsicmp(primaryExt.c_str(), L".webp") == 0 ||
                                 _wcsicmp(primaryExt.c_str(), L".jxl") == 0 ||
                                 _wcsicmp(primaryExt.c_str(), L".jxr") == 0 ||
                                 _wcsicmp(primaryExt.c_str(), L".wdp") == 0 ||
                                 _wcsicmp(primaryExt.c_str(), L".heic") == 0 ||
                                 _wcsicmp(primaryExt.c_str(), L".heif") == 0);

        bool supportsQuality = (_wcsicmp(primaryExt.c_str(), L".jpg") == 0 ||
                                _wcsicmp(primaryExt.c_str(), L".jpeg") == 0 ||
                                _wcsicmp(primaryExt.c_str(), L".webp") == 0 ||
                                _wcsicmp(primaryExt.c_str(), L".jxl") == 0 ||
                                _wcsicmp(primaryExt.c_str(), L".jxr") == 0 ||
                                _wcsicmp(primaryExt.c_str(), L".wdp") == 0 ||
                                _wcsicmp(primaryExt.c_str(), L".heic") == 0 ||
                                _wcsicmp(primaryExt.c_str(), L".heif") == 0);

        ExportFormatDesc desc;
        desc.DisplayName = displayName;
        desc.Ext = primaryExt;
        desc.ContainerGuid = containerGuid;
        desc.SupportsLosslessSwitch = supportsLossless;
        desc.SupportsQuality = supportsQuality;

        result.push_back(std::move(desc));

        // If HEIF container is available, probe whether TRUE AVIF (.avif) encoding can actually commit via WICHeifCompressionAV1 (3)
        if (_wcsicmp(primaryExt.c_str(), L".heic") == 0 || _wcsicmp(primaryExt.c_str(), L".heif") == 0) {
            static bool s_probedAvif = false;
            static bool s_canEncodeAvif = false;
            if (!s_probedAvif) {
                s_canEncodeAvif = ProbeAvifEncoderSupport(factory.Get(), containerGuid);
                s_probedAvif = true;
            }

            if (s_canEncodeAvif) {
                ExportFormatDesc avifDesc;
                avifDesc.DisplayName = L"AVIF Image (*.avif)";
                avifDesc.Ext = L".avif";
                avifDesc.ContainerGuid = containerGuid;
                avifDesc.SupportsLosslessSwitch = false; // AVIF WIC does not support Lossless=TRUE
                avifDesc.SupportsQuality = true;         // AVIF WIC supports Quality slider!
                result.push_back(std::move(avifDesc));
            }
        }
    }

    std::sort(result.begin(), result.end(), [](const ExportFormatDesc& a, const ExportFormatDesc& b) {
        return _wcsicmp(a.DisplayName.c_str(), b.DisplayName.c_str()) < 0;
    });

    return result;
}

GUID ImageExporter::GetContainerFormatFromExtension(const wchar_t* ext) {
    if (!ext || !*ext) return GUID_ContainerFormatJpeg;
    auto formats = GetSupportedExportFormats();
    for (const auto& fmt : formats) {
        if (_wcsicmp(fmt.Ext.c_str(), ext) == 0) {
            return fmt.ContainerGuid;
        }
    }
    return GUID_ContainerFormatJpeg;
}

static void ConfigureEncoderProperties(IPropertyBag2* props, const GUID& containerFormat, const ExportOptions& options) {
    (void)containerFormat;
    if (!props) return;

    const wchar_t* ext = PathFindExtensionW(options.OutputPath.c_str());
    bool isAvif = (ext && _wcsicmp(ext, L".avif") == 0);

    if (isAvif) {
        PROPBAG2 optHeif = {};
        optHeif.pstrName = (LPOLESTR)L"HeifCompressionMethod";
        VARIANT varHeif; VariantInit(&varHeif);
        varHeif.vt = VT_UI1; varHeif.bVal = 3; // Enforce WICHeifCompressionAV1 (0x3)
        props->Write(1, &optHeif, &varHeif);
    }

    if (options.Lossless && !isAvif) {
        PROPBAG2 optLossless = {};
        optLossless.pstrName = (LPOLESTR)L"Lossless";
        VARIANT varLossless;
        VariantInit(&varLossless);
        varLossless.vt = VT_BOOL;
        varLossless.boolVal = VARIANT_TRUE;
        props->Write(1, &optLossless, &varLossless);
    } else {
        float qualityVal = std::clamp(options.JpegQuality, 1, 100) / 100.0f;
        PROPBAG2 optQuality = {};
        optQuality.pstrName = (LPOLESTR)L"ImageQuality";
        VARIANT varQuality;
        VariantInit(&varQuality);
        varQuality.vt = VT_R4;
        varQuality.fltVal = qualityVal;
        if (FAILED(props->Write(1, &optQuality, &varQuality))) {
            optQuality.pstrName = (LPOLESTR)L"Quality";
            props->Write(1, &optQuality, &varQuality);
        }
    }
}

static void EmbedMetadataAndResetOrientation(IWICImagingFactory* factory,
                                              IWICBitmapFrameEncode* frameEncode,
                                              const ExportOptions& options) {
    if (!factory || !frameEncode) return;

    if (options.PreserveMetadata && !options.InputPath.empty()) {
        ComPtr<IWICBitmapDecoder> metaDecoder;
        if (SUCCEEDED(factory->CreateDecoderFromFilename(options.InputPath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &metaDecoder))) {
            ComPtr<IWICBitmapFrameDecode> metaFrame;
            if (SUCCEEDED(metaDecoder->GetFrame(0, &metaFrame))) {
                ComPtr<IWICMetadataBlockReader> blockReader;
                if (SUCCEEDED(metaFrame.As(&blockReader))) {
                    ComPtr<IWICMetadataBlockWriter> blockWriter;
                    if (SUCCEEDED(frameEncode->QueryInterface(IID_PPV_ARGS(&blockWriter)))) {
                        blockWriter->InitializeFromBlockReader(blockReader.Get());
                    }
                }
            }
        }
    }

    // Always neutralize EXIF Orientation tag to 1 (Normal)
    ComPtr<IWICMetadataQueryWriter> queryWriter;
    if (SUCCEEDED(frameEncode->GetMetadataQueryWriter(&queryWriter))) {
        PROPVARIANT var;
        PropVariantInit(&var);
        var.vt = VT_UI2;
        var.uiVal = 1; // Set Orientation = 1 (Normal)
        queryWriter->SetMetadataByName(L"/app1/ifd/{short=274}", &var);
        queryWriter->SetMetadataByName(L"/ifd/{short=274}", &var);
        PropVariantClear(&var);
    }
}

std::expected<void, std::wstring> ImageExporter::Export(const ExportOptions& options) {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapSource> source;
    ComPtr<IWICColorContext> colorContext;
    
    HRESULT hr = CreateWICPipeline(options, source, factory, colorContext);
    if (FAILED(hr)) return std::unexpected(L"Failed to create image pipeline.");

    // Check if writing to input file directly (overwrite)
    bool isOverwrite = false;
    std::wstring actualOutputPath = options.OutputPath;
    std::wstring tempPath;

    if (!options.InputPath.empty() && 
        _wcsicmp(options.OutputPath.c_str(), options.InputPath.c_str()) == 0) {
        isOverwrite = true;
        tempPath = options.OutputPath + L".tmp";
        actualOutputPath = tempPath;
    }

    // Determine encoder by extension
    const wchar_t* ext = PathFindExtensionW(options.OutputPath.c_str());
    GUID containerFormat = GetContainerFormatFromExtension(ext);

    ComPtr<IWICStream> stream;
    hr = factory->CreateStream(&stream);
    if (FAILED(hr)) return std::unexpected(L"Failed to create output stream.");

    hr = stream->InitializeFromFilename(actualOutputPath.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) {
        if (isOverwrite) DeleteFileW(tempPath.c_str());
        return std::unexpected(L"Failed to open output file.");
    }

    ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(containerFormat, nullptr, &encoder);
    if (FAILED(hr)) {
        if (isOverwrite) DeleteFileW(tempPath.c_str());
        return std::unexpected(L"Failed to create encoder.");
    }

    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) {
        if (isOverwrite) DeleteFileW(tempPath.c_str());
        return std::unexpected(L"Failed to initialize encoder.");
    }

    ComPtr<IWICBitmapFrameEncode> frameEncode;
    ComPtr<IPropertyBag2> props;
    hr = encoder->CreateNewFrame(&frameEncode, &props);
    if (FAILED(hr)) {
        if (isOverwrite) DeleteFileW(tempPath.c_str());
        return std::unexpected(L"Failed to create encoder frame.");
    }

    ConfigureEncoderProperties(props.Get(), containerFormat, options);

    hr = frameEncode->Initialize(props.Get());
    if (FAILED(hr)) {
        if (isOverwrite) DeleteFileW(tempPath.c_str());
        return std::unexpected(L"Failed to initialize frame encode.");
    }

    // Embed Metadata Blocks & Reset EXIF Orientation to 1
    EmbedMetadataAndResetOrientation(factory.Get(), frameEncode.Get(), options);

    // Embed Color Context
    if (colorContext && options.EmbedIcc) {
        IWICColorContext* pCtx = colorContext.Get();
        frameEncode->SetColorContexts(1, &pCtx);
    }

    UINT w = 0, h = 0;
    source->GetSize(&w, &h);
    frameEncode->SetSize(w, h);

    WICPixelFormatGUID pixelFormat = {};
    source->GetPixelFormat(&pixelFormat);
    WICPixelFormatGUID origPixelFormat = pixelFormat;
    frameEncode->SetPixelFormat(&pixelFormat);

    ComPtr<IWICBitmapSource> finalSource = source;
    if (pixelFormat != origPixelFormat) {
        ComPtr<IWICFormatConverter> converter;
        if (SUCCEEDED(factory->CreateFormatConverter(&converter))) {
            if (SUCCEEDED(converter->Initialize(source.Get(), pixelFormat, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
                finalSource = converter;
            }
        }
    }

    hr = frameEncode->WriteSource(finalSource.Get(), nullptr);
    if (FAILED(hr)) {
        if (isOverwrite) DeleteFileW(tempPath.c_str());
        return std::unexpected(L"Failed to write image data.");
    }

    hr = frameEncode->Commit();
    if (FAILED(hr)) {
        if (isOverwrite) DeleteFileW(tempPath.c_str());
        return std::unexpected(L"Failed to commit frame.");
    }

    hr = encoder->Commit();
    if (FAILED(hr)) {
        if (isOverwrite) DeleteFileW(tempPath.c_str());
        return std::unexpected(L"Failed to commit encoder.");
    }

    // Release WIC handles before moving file
    frameEncode.Reset();
    encoder.Reset();
    stream.Reset();
    finalSource.Reset();
    source.Reset();
    factory.Reset();

    if (isOverwrite) {
        if (!MoveFileExW(tempPath.c_str(), options.OutputPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(tempPath.c_str());
            return std::unexpected(L"Failed to overwrite target file.");
        }
    }

    return {};
}

std::expected<uint64_t, std::wstring> ImageExporter::EstimateSize(const ExportOptions& options) {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapSource> source;
    ComPtr<IWICColorContext> colorContext;
    
    HRESULT hr = CreateWICPipeline(options, source, factory, colorContext);
    if (FAILED(hr)) return std::unexpected(L"Pipeline error.");

    const wchar_t* ext = PathFindExtensionW(options.OutputPath.c_str());
    GUID containerFormat = GetContainerFormatFromExtension(ext);

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

    ConfigureEncoderProperties(props.Get(), containerFormat, options);

    hr = frameEncode->Initialize(props.Get());
    if (FAILED(hr)) return std::unexpected(L"Frame init error.");

    // Embed Metadata Blocks & Reset EXIF Orientation to 1 for accurate size estimation
    EmbedMetadataAndResetOrientation(factory.Get(), frameEncode.Get(), options);

    if (colorContext && options.EmbedIcc) {
        IWICColorContext* pCtx = colorContext.Get();
        frameEncode->SetColorContexts(1, &pCtx);
    }

    UINT estW = 0, estH = 0;
    source->GetSize(&estW, &estH);
    frameEncode->SetSize(estW, estH);

    WICPixelFormatGUID estPixelFormat = {};
    source->GetPixelFormat(&estPixelFormat);
    WICPixelFormatGUID estOrigPixelFormat = estPixelFormat;
    frameEncode->SetPixelFormat(&estPixelFormat);

    ComPtr<IWICBitmapSource> estFinalSource = source;
    if (estPixelFormat != estOrigPixelFormat) {
        ComPtr<IWICFormatConverter> converter;
        if (SUCCEEDED(factory->CreateFormatConverter(&converter))) {
            if (SUCCEEDED(converter->Initialize(source.Get(), estPixelFormat, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
                estFinalSource = converter;
            }
        }
    }

    hr = frameEncode->WriteSource(estFinalSource.Get(), nullptr);
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
