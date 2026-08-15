#include "pch.h"
#include "ImageExporter.h"
#include "ImageLoader.h"
#include "ImageTypes.h"
#include "AppContext.h"
#include "MappedFile.h"
#include "MetafileCodec.h"
#include "SupportedExtensions.h"
#include <wincodec.h>
#include <wincodecsdk.h>
#include <shlwapi.h>
#include <windows.h>
#include <ole2.h>
#include <algorithm>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>

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

HRESULT ImageExporter::CreateWICPipeline(const ExportOptions& options,
                                         ComPtr<IWICBitmapSource>& outSource,
                                         ComPtr<IWICImagingFactory>& factory,
                                         ComPtr<IWICColorContext>& outColorContext,
                                         ComPtr<IWICBitmapFrameDecode>& outFrameDecode) {
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;

    ComPtr<IWICBitmapSource> currentSource;
    bool isSvgAlreadyCropped = false;

    // Check if in-memory source frame was supplied (e.g. animated frame or canvas buffer)
    if (options.SourceFrame && options.SourceFrame->pixels && options.SourceFrame->width > 0 && options.SourceFrame->height > 0) {
        WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppBGRA;
        UINT stride = options.SourceFrame->stride > 0 ? options.SourceFrame->stride : (options.SourceFrame->width * 4);
        UINT totalBytes = stride * options.SourceFrame->height;
        ComPtr<IWICBitmap> memBmp;
        hr = factory->CreateBitmapFromMemory(options.SourceFrame->width, options.SourceFrame->height, pixelFormat, stride, totalBytes, options.SourceFrame->pixels, &memBmp);
        if (SUCCEEDED(hr) && memBmp) {
            currentSource = memBmp;
        }
    }

    // 1. Camera RAW Direct Bypass (Bypass flawed/incomplete Windows WIC RAW Codecs)
    if (!currentSource && !options.InputPath.empty() && QuickView::IsRawPath(options.InputPath)) {
        ComPtr<IWICBitmap> rawBmp;
        CImageLoader loader;
        loader.Initialize(factory.Get());
        hr = loader.LoadRaw(options.InputPath.c_str(), rawBmp.GetAddressOf(), options.RawForceFullDecode);
        if (SUCCEEDED(hr) && rawBmp) {
            currentSource = rawBmp;
        }
    }

    // 2. Standard WIC Decoder (for JPEG, PNG, TIFF, BMP, GIF, etc.)
    if (!currentSource && !options.InputPath.empty()) {
        ComPtr<IWICBitmapDecoder> decoder;
        hr = factory->CreateDecoderFromFilename(options.InputPath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
        if (SUCCEEDED(hr) && decoder) {
            ComPtr<IWICBitmapFrameDecode> frame;
            if (SUCCEEDED(decoder->GetFrame(0, &frame)) && frame) {
                outFrameDecode = frame;
                currentSource = frame;

                // Extract ICC Color Context from frame if requested
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
            }
        }
    }

    // 3. Universal Fallback: Unified loaders (SVG, EMF/WMF, AVIF, JXL, PSD, HDR, EXR, etc.)
    if (!currentSource && !options.InputPath.empty()) {
        QuickView::MappedFile fileMap(options.InputPath.c_str());
        if (fileMap.IsValid() && fileMap.size() > 0) {
            std::wstring pathLower = options.InputPath;
            std::transform(pathLower.begin(), pathLower.end(), pathLower.begin(), ::towlower);

            // 3.1 Vector SVG: True-vector sub-region rasterization
            if (pathLower.ends_with(L".svg") || pathLower.ends_with(L".svgz") || 
                (fileMap.size() >= 5 && (memcmp(fileMap.data(), "<?xml", 5) == 0 || memcmp(fileMap.data(), "<svg", 4) == 0))) {
                CImageLoader::ThumbData svgData;
                CImageLoader::ImageHeaderInfo hdr = ::g_imageLoader ? ::g_imageLoader->PeekHeader(options.InputPath.c_str()) : CImageLoader::ImageHeaderInfo{};
                
                float cropX = (float)options.CropX;
                float cropY = (float)options.CropY;
                float cropW = (float)options.CropWidth;
                float cropH = (float)options.CropHeight;
                float zoom = options.DisplayZoom > 0.0f ? options.DisplayZoom : 1.0f;

                if (SUCCEEDED(CImageLoader::RasterizeSvgToPixels(
                        fileMap.data(), fileMap.size(), (float)hdr.width, (float)hdr.height, 0, &svgData,
                        cropX, cropY, cropW, cropH, zoom)) && svgData.isValid && !svgData.pixels.empty()) {
                    ComPtr<IWICBitmap> memBmp;
                    hr = factory->CreateBitmapFromMemory(svgData.width, svgData.height, GUID_WICPixelFormat32bppBGRA, svgData.stride, (UINT)svgData.pixels.size(), svgData.pixels.data(), &memBmp);
                    if (SUCCEEDED(hr) && memBmp) {
                        currentSource = memBmp;
                        if (cropW > 0.0f && cropH > 0.0f) {
                            isSvgAlreadyCropped = true;
                        }
                    }
                }
            }
            // 3.2 Metafile (EMF / WMF)
            else if (QuickView::Metafile::Detect(fileMap.data(), fileMap.size()) != QuickView::Metafile::Kind::None) {
                HENHMETAFILE hemf = QuickView::Metafile::OpenHemf(fileMap.data(), fileMap.size());
                if (hemf) {
                    auto logical = QuickView::Metafile::MeasureHeader(fileMap.data(), fileMap.size(), 96, 96);
                    int w = (logical.width > 0) ? logical.width : 1024;
                    int h = (logical.height > 0) ? logical.height : 768;
                    std::vector<uint8_t> pixels((size_t)w * h * 4);
                    if (QuickView::Metafile::Rasterize(hemf, w, h, pixels.data(), w * 4)) {
                        ComPtr<IWICBitmap> memBmp;
                        hr = factory->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppBGRA, w * 4, (UINT)pixels.size(), pixels.data(), &memBmp);
                        if (SUCCEEDED(hr) && memBmp) {
                            currentSource = memBmp;
                        }
                    }
                    DeleteEnhMetaFile(hemf);
                }
            }
            // 3.3 Memory-based unified formats (AVIF, JXL, PSD, EXR, HDR, WebP, etc.)
            else {
                QuickView::RawImageFrame rawFrame;
                if (SUCCEEDED(CImageLoader::FullDecodeFromMemory(fileMap.data(), fileMap.size(), &rawFrame)) && rawFrame.pixels && rawFrame.width > 0 && rawFrame.height > 0) {
                    WICPixelFormatGUID pixelFormat = (rawFrame.format == PixelFormat::RGBA8888) 
                        ? GUID_WICPixelFormat32bppRGBA : GUID_WICPixelFormat32bppBGRA;
                    UINT stride = rawFrame.stride > 0 ? rawFrame.stride : (rawFrame.width * 4);
                    UINT totalBytes = stride * rawFrame.height;
                    ComPtr<IWICBitmap> memBmp;
                    hr = factory->CreateBitmapFromMemory(rawFrame.width, rawFrame.height, pixelFormat, stride, totalBytes, rawFrame.pixels, &memBmp);
                    if (SUCCEEDED(hr) && memBmp) {
                        currentSource = memBmp;
                    }
                }
            }
        }

        // 3.4 Secondary fallback to CImageLoader::LoadToMemory (full decode)
        if (!currentSource) {
            ComPtr<IWICBitmap> fallbackBmp;
            CImageLoader loader;
            loader.Initialize(factory.Get());
            hr = loader.LoadToMemory(options.InputPath.c_str(), fallbackBmp.GetAddressOf(), nullptr, true /* forceFullDecode */);
            if (SUCCEEDED(hr) && fallbackBmp) {
                currentSource = fallbackBmp;
            }
        }
    }

    if (!currentSource) {
        return FAILED(hr) ? hr : E_FAIL;
    }

    // Explicit ICC Profile Injection if provided
    if (options.EmbedIcc && !outColorContext) {
        if (!options.IccProfilePath.empty()) {
            if (SUCCEEDED(factory->CreateColorContext(&outColorContext))) {
                outColorContext->InitializeFromFilename(options.IccProfilePath.c_str());
            }
        } else if (!options.CustomIccData.empty()) {
            if (SUCCEEDED(factory->CreateColorContext(&outColorContext))) {
                outColorContext->InitializeFromMemory(options.CustomIccData.data(), (UINT)options.CustomIccData.size());
            }
        }
    }

    // Get Original Frame Size for Boundary Clamp
    UINT origWidth = 0, origHeight = 0;
    currentSource->GetSize(&origWidth, &origHeight);

    // RAM CACHE: Decode frame into IWICBitmap to eliminate O(N^2) random-access
    // thrashing during WIC FlipRotator.
    bool needsRandomAccess = (options.Rotation != 0 || options.FlipH || options.FlipV);
    if (needsRandomAccess && outFrameDecode) {
        WICPixelFormatGUID srcPF = {};
        outFrameDecode->GetPixelFormat(&srcPF);
        UINT bpp = 32;
        ComPtr<IWICComponentInfo> compInfo;
        if (SUCCEEDED(factory->CreateComponentInfo(srcPF, &compInfo))) {
            ComPtr<IWICPixelFormatInfo> pfInfo;
            if (SUCCEEDED(compInfo.As(&pfInfo))) {
                pfInfo->GetBitsPerPixel(&bpp);
            }
        }

        uint64_t rawBytes = static_cast<uint64_t>(origWidth) * static_cast<uint64_t>(origHeight) * static_cast<uint64_t>(bpp) / 8;
        MEMORYSTATUSEX memInfo = {};
        memInfo.dwLength = sizeof(memInfo);
        uint64_t maxCacheBytes = 512ULL * 1024 * 1024;
        if (GlobalMemoryStatusEx(&memInfo)) {
            uint64_t avail = memInfo.ullAvailPhys;
            uint64_t total = memInfo.ullTotalPhys;
            uint64_t reserve = 2ULL * 1024 * 1024 * 1024;
            uint64_t usable = (avail > reserve) ? (avail - reserve) : 0;
            maxCacheBytes = std::min(usable / 2, total / 4);
        }

        if (rawBytes > 0 && rawBytes <= maxCacheBytes) {
            ComPtr<IWICBitmap> ramBitmap;
            if (SUCCEEDED(factory->CreateBitmapFromSource(outFrameDecode.Get(), WICBitmapCacheOnLoad, &ramBitmap))) {
                currentSource = ramBitmap;
            }
        }
    }

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

    // Apply Crop with strict boundary clamp (Skip if SVG was already true-vector cropped)
    if (!isSvgAlreadyCropped && options.CropWidth > 0 && options.CropHeight > 0) {
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

bool ImageExporter::FormatSupportsExifOrientation(const wchar_t* ext) {
    if (!ext || !*ext) return false;
    return (_wcsicmp(ext, L".jpg") == 0 || _wcsicmp(ext, L".jpeg") == 0 ||
            _wcsicmp(ext, L".tif") == 0 || _wcsicmp(ext, L".tiff") == 0 ||
            _wcsicmp(ext, L".heic") == 0 || _wcsicmp(ext, L".heif") == 0 ||
            _wcsicmp(ext, L".avif") == 0);
}

static void EmbedMetadataAndResetOrientation(IWICImagingFactory* factory,
                                              IWICBitmapFrameDecode* metaFrame,
                                              IWICBitmapFrameEncode* frameEncode,
                                              const ExportOptions& options) {
    if (!factory || !frameEncode) return;

    if (options.PreserveMetadata && metaFrame) {
        ComPtr<IWICMetadataBlockReader> blockReader;
        if (SUCCEEDED(metaFrame->QueryInterface(IID_PPV_ARGS(&blockReader)))) {
            ComPtr<IWICMetadataBlockWriter> blockWriter;
            if (SUCCEEDED(frameEncode->QueryInterface(IID_PPV_ARGS(&blockWriter)))) {
                blockWriter->InitializeFromBlockReader(blockReader.Get());
            }
        }
        

    }

    // Neutralize EXIF Orientation tag to 1 ONLY when transforms are applied and format supports EXIF.
    // Skip entirely for non-EXIF formats (PNG/BMP/GIF/WebP) and for unmodified exports.
    bool hasTransform = (options.Rotation != 0 || options.FlipH || options.FlipV);
    if (hasTransform) {
        const wchar_t* ext = PathFindExtensionW(options.OutputPath.c_str());
        if (ImageExporter::FormatSupportsExifOrientation(ext)) {
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
    }
}

std::expected<void, std::wstring> ImageExporter::Export(const ExportOptions& options) {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapSource> source;
    ComPtr<IWICColorContext> colorContext;
    ComPtr<IWICBitmapFrameDecode> frameDecode;
    
    HRESULT hr = CreateWICPipeline(options, source, factory, colorContext, frameDecode);
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
    EmbedMetadataAndResetOrientation(factory.Get(), frameDecode.Get(), frameEncode.Get(), options);

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

    // Release WIC handles (both input and output) before moving file
    frameEncode.Reset();
    encoder.Reset();
    stream.Reset();
    finalSource.Reset();
    source.Reset();
    frameDecode.Reset();
    colorContext.Reset();
    factory.Reset();

    if (isOverwrite) {
        bool moveOk = false;
        // 1. Retry MoveFileExW with MOVEFILE_COPY_ALLOWED (5 retries with 80ms delay)
        for (int i = 0; i < 5; ++i) {
            if (MoveFileExW(tempPath.c_str(), options.OutputPath.c_str(), 
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH | MOVEFILE_COPY_ALLOWED)) {
                moveOk = true;
                break;
            }
            Sleep(80);
        }

        // 2. Fallback: CopyFileW if Move fails
        if (!moveOk) {
            if (CopyFileW(tempPath.c_str(), options.OutputPath.c_str(), FALSE)) {
                moveOk = true;
            }
        }

        DeleteFileW(tempPath.c_str());

        if (!moveOk) {
            return std::unexpected(L"Failed to overwrite target file.");
        }
    }

    return {};
}

std::expected<uint64_t, std::wstring> ImageExporter::EstimateSize(const ExportOptions& options) {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapSource> source;
    ComPtr<IWICColorContext> colorContext;
    ComPtr<IWICBitmapFrameDecode> frameDecode;
    
    HRESULT hr = CreateWICPipeline(options, source, factory, colorContext, frameDecode);
    if (FAILED(hr) || !source || !factory) return std::unexpected(L"Pipeline error.");

    const wchar_t* ext = PathFindExtensionW(options.OutputPath.c_str());
    GUID containerFormat = GetContainerFormatFromExtension(ext);

    ComPtr<IStream> memStream;
    CountingStream* countingStream = new (std::nothrow) CountingStream();
    if (!countingStream) return std::unexpected(L"Out of memory creating measurement stream.");
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
    EmbedMetadataAndResetOrientation(factory.Get(), frameDecode.Get(), frameEncode.Get(), options);

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
    if (FAILED(hr)) return std::unexpected(L"Write source error.");

    hr = frameEncode->Commit();
    if (FAILED(hr)) return std::unexpected(L"Commit error.");

    hr = encoder->Commit();
    if (FAILED(hr)) return std::unexpected(L"Encoder commit error.");

    return countingStream->GetSize();
}

bool g_isSettingUpClipboard = false;

struct PreRenderSession {
    uint64_t generationId = 0;
    std::atomic<bool> isCancelled{false};
    std::atomic<bool> isCompleted{false};
    HGLOBAL hPng = nullptr;
    HGLOBAL hDib = nullptr;
    std::mutex mtx;
    std::condition_variable cv;

    ~PreRenderSession() {
        if (hPng) { GlobalFree(hPng); hPng = nullptr; }
        if (hDib) { GlobalFree(hDib); hDib = nullptr; }
    }
};

static std::shared_ptr<PreRenderSession> g_activePreRenderSession;
static std::mutex g_preRenderSessionMutex;
static uint64_t g_preRenderGeneration = 0;

UINT ImageExporter::GetPngClipboardFormat() {
    static const UINT s_cfPng = RegisterClipboardFormatW(L"PNG");
    return s_cfPng;
}

void ImageExporter::CancelPreRendering() {
    std::lock_guard<std::mutex> lock(g_preRenderSessionMutex);
    if (g_activePreRenderSession) {
        g_activePreRenderSession->isCancelled = true;
        g_activePreRenderSession->cv.notify_all();
        g_activePreRenderSession.reset();
    }
}

std::expected<void, std::wstring> ImageExporter::SetupDelayedClipboard(HWND hwnd, const PendingClipboardSnapshot& snapshot) {
    if (!snapshot.isValid) {
        return std::unexpected(L"Invalid clipboard snapshot.");
    }
    if (!OpenClipboard(hwnd)) {
        return std::unexpected(L"Failed to open clipboard.");
    }

    g_isSettingUpClipboard = true;
    EmptyClipboard();
    g_isSettingUpClipboard = false;

    // Register delayed rendering formats (NULL data)
    // NOTE: Only register PNG + CF_DIB. Do NOT register CF_DIBV5 here!
    // Windows auto-synthesizes CF_DIBV5/CF_BITMAP from CF_DIB. Registering CF_DIBV5
    // explicitly causes the auto-synthesis engine to interfere with our CF_DIB
    // delayed rendering, producing corrupt 16x16 bitmaps in MS Paint.
    SetClipboardData(GetPngClipboardFormat(), nullptr);
    SetClipboardData(CF_DIB, nullptr);

    CloseClipboard();

    // Hybrid Pre-rendering: Trigger background worker to decode & compress in advance (0ms Ctrl+V latency)
    {
        std::lock_guard<std::mutex> lock(g_preRenderSessionMutex);
        if (g_activePreRenderSession) {
            g_activePreRenderSession->isCancelled = true;
            g_activePreRenderSession->cv.notify_all();
        }
        auto session = std::make_shared<PreRenderSession>();
        session->generationId = ++g_preRenderGeneration;
        g_activePreRenderSession = session;

        std::thread preRenderWorker([session, snapshot, hwnd]() {
            HGLOBAL hPng = nullptr;
            HGLOBAL hDib = nullptr;
            RenderClipboardDual(snapshot, hPng, hDib);

            std::lock_guard<std::mutex> sLock(session->mtx);
            if (session->isCancelled) {
                if (hPng) GlobalFree(hPng);
                if (hDib) GlobalFree(hDib);
            } else {
                session->hPng = hPng;
                session->hDib = hDib;
                session->isCompleted = true;
                session->cv.notify_all();
                if (hwnd && IsWindow(hwnd)) {
                    PostMessageW(hwnd, WM_CLIPBOARD_PRERENDER_READY, 0, 0);
                }
            }
        });
        preRenderWorker.detach();
    }

    return {};
}

void ImageExporter::RenderClipboardDual(const PendingClipboardSnapshot& snapshot, HGLOBAL& outPng, HGLOBAL& outDib) {
    outPng = nullptr;
    outDib = nullptr;
    if (!snapshot.isValid) return;

    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapSource> source;
    ComPtr<IWICColorContext> colorContext;
    ComPtr<IWICBitmapFrameDecode> frameDecode;

    HRESULT hr = ImageExporter::CreateWICPipeline(snapshot.options, source, factory, colorContext, frameDecode);
    if (FAILED(hr) || !source || !factory) return;

    ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(&converter);
    if (FAILED(hr)) return;

    hr = converter->Initialize(source.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return;

    UINT width = 0, height = 0;
    hr = converter->GetSize(&width, &height);
    if (FAILED(hr) || width == 0 || height == 0) return;

    const UINT stride = width * 4;
    const UINT imageSize = stride * height;
    const UINT dibSize = sizeof(BITMAPINFOHEADER) + imageSize;

    // 1. Single-pass Pull of all BGRA pixels into RAM buffer
    std::vector<uint8_t> topDownPixels((size_t)imageSize);
    HRESULT copyHr = converter->CopyPixels(nullptr, stride, imageSize, topDownPixels.data());
    if (FAILED(copyHr)) return;

    // 2. Fast CF_DIB Construction (2ms parallel row flip)
    HGLOBAL hDib = GlobalAlloc(GMEM_MOVEABLE, dibSize);
    if (hDib) {
        uint8_t* pData = static_cast<uint8_t*>(GlobalLock(hDib));
        if (pData) {
            BITMAPINFOHEADER* bmi = reinterpret_cast<BITMAPINFOHEADER*>(pData);
            ZeroMemory(bmi, sizeof(BITMAPINFOHEADER));
            bmi->biSize = sizeof(BITMAPINFOHEADER);
            bmi->biWidth = static_cast<LONG>(width);
            bmi->biHeight = static_cast<LONG>(height);
            bmi->biPlanes = 1;
            bmi->biBitCount = 32;
            bmi->biCompression = BI_RGB;
            bmi->biSizeImage = imageSize;
            bmi->biXPelsPerMeter = 0;
            bmi->biYPelsPerMeter = 0;

            uint8_t* pixels = pData + sizeof(BITMAPINFOHEADER);
            for (UINT y = 0; y < height; ++y) {
                const uint8_t* srcRow = topDownPixels.data() + static_cast<size_t>(y) * stride;
                uint8_t* dstRow = pixels + static_cast<size_t>(height - 1 - y) * stride;
                memcpy(dstRow, srcRow, stride);
            }
            GlobalUnlock(hDib);
            outDib = hDib;
        } else {
            GlobalFree(hDib);
        }
    }

    // 3. Fast PNG Encoding (Direct from already initialized converter)
    IStream* pStream = nullptr;
    hr = CreateStreamOnHGlobal(nullptr, FALSE, &pStream);
    if (SUCCEEDED(hr) && pStream) {
        ComPtr<IWICBitmapEncoder> pngEncoder;
        hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &pngEncoder);
        if (SUCCEEDED(hr)) {
            hr = pngEncoder->Initialize(pStream, WICBitmapEncoderNoCache);
            if (SUCCEEDED(hr)) {
                ComPtr<IWICBitmapFrameEncode> frameEncode;
                ComPtr<IPropertyBag2> props;
                hr = pngEncoder->CreateNewFrame(&frameEncode, &props);
                if (SUCCEEDED(hr)) {
                    if (props) {
                        PROPBAG2 optFilter = {};
                        optFilter.pstrName = (LPOLESTR)L"FilterOption";
                        VARIANT varFilter; VariantInit(&varFilter);
                        varFilter.vt = VT_UI1;
                        varFilter.bVal = 1; // WICPngFilterNone = 1
                        props->Write(1, &optFilter, &varFilter);

                        PROPBAG2 optInterlace = {};
                        optInterlace.pstrName = (LPOLESTR)L"InterlaceOption";
                        VARIANT varInterlace; VariantInit(&varInterlace);
                        varInterlace.vt = VT_BOOL;
                        varInterlace.boolVal = VARIANT_FALSE;
                        props->Write(1, &optInterlace, &varInterlace);
                    }
                    if (SUCCEEDED(frameEncode->Initialize(props.Get())) &&
                        SUCCEEDED(frameEncode->SetSize(width, height))) {
                        frameEncode->SetResolution(96.0, 96.0);
                        WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppBGRA;
                        frameEncode->SetPixelFormat(&pixelFormat);
                        if (colorContext && snapshot.options.EmbedIcc) {
                            IWICColorContext* pCtx = colorContext.Get();
                            frameEncode->SetColorContexts(1, &pCtx);
                        }
                        if (SUCCEEDED(frameEncode->WriteSource(converter.Get(), nullptr)) &&
                            SUCCEEDED(frameEncode->Commit()) &&
                            SUCCEEDED(pngEncoder->Commit())) {
                            STATSTG stat = {};
                            pStream->Stat(&stat, STATFLAG_NONAME);
                            ULARGE_INTEGER streamSize = stat.cbSize;
                            HGLOBAL hStreamMem = nullptr;
                            GetHGlobalFromStream(pStream, &hStreamMem);
                            if (hStreamMem && streamSize.QuadPart > 0) {
                                HGLOBAL hFinalMem = GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(streamSize.QuadPart));
                                if (hFinalMem) {
                                    void* pSrc = GlobalLock(hStreamMem);
                                    void* pDst = GlobalLock(hFinalMem);
                                    if (pSrc && pDst) {
                                        memcpy(pDst, pSrc, static_cast<size_t>(streamSize.QuadPart));
                                    }
                                    if (pSrc) GlobalUnlock(hStreamMem);
                                    if (pDst) GlobalUnlock(hFinalMem);
                                    outPng = hFinalMem;
                                }
                            }
                            if (hStreamMem) GlobalFree(hStreamMem);
                        }
                    }
                }
            }
        }
        pStream->Release();
    }
}

HGLOBAL ImageExporter::RenderClipboardFormat(UINT uFormat, const PendingClipboardSnapshot& snapshot) {
    if (!snapshot.isValid) return nullptr;

    const UINT pngFormat = GetPngClipboardFormat();

    // Check Hybrid Pre-rendering cache
    std::shared_ptr<PreRenderSession> session;
    {
        std::lock_guard<std::mutex> lock(g_preRenderSessionMutex);
        session = g_activePreRenderSession;
    }

    if (session) {
        std::unique_lock<std::mutex> sLock(session->mtx);
        if (!session->isCompleted && !session->isCancelled) {
            // Wait up to 3 seconds for background worker to complete
            session->cv.wait_for(sLock, std::chrono::milliseconds(3000), [&]() {
                return session->isCompleted.load() || session->isCancelled.load();
            });
        }

        if (session->isCompleted && !session->isCancelled) {
            if (uFormat == pngFormat && session->hPng) {
                HGLOBAL res = session->hPng;
                session->hPng = nullptr; // Transfer ownership to Windows Clipboard
                return res;
            } else if (uFormat == CF_DIB && session->hDib) {
                HGLOBAL res = session->hDib;
                session->hDib = nullptr; // Transfer ownership to Windows Clipboard
                return res;
            }
        }
    }

    // Direct synchronous fallback
    return RenderClipboardFormatDirect(uFormat, snapshot);
}

HGLOBAL ImageExporter::RenderClipboardFormatDirect(UINT uFormat, const PendingClipboardSnapshot& snapshot) {
    if (!snapshot.isValid) return nullptr;
    HGLOBAL hPng = nullptr;
    HGLOBAL hDib = nullptr;
    RenderClipboardDual(snapshot, hPng, hDib);

    const UINT pngFormat = ImageExporter::GetPngClipboardFormat();
    if (uFormat == pngFormat) {
        if (hDib) GlobalFree(hDib);
        return hPng;
    } else if (uFormat == CF_DIB) {
        if (hPng) GlobalFree(hPng);
        return hDib;
    }
    if (hPng) GlobalFree(hPng);
    if (hDib) GlobalFree(hDib);
    return nullptr;
}

void ImageExporter::RenderAllClipboardFormats(HWND /*hwnd*/, const PendingClipboardSnapshot& snapshot) {
    if (!snapshot.isValid) return;

    UINT pngFormat = GetPngClipboardFormat();
    HGLOBAL hPng = RenderClipboardFormat(pngFormat, snapshot);
    if (hPng) {
        if (!SetClipboardData(pngFormat, hPng)) {
            GlobalFree(hPng);
        }
    }

    HGLOBAL hDib = RenderClipboardFormat(CF_DIB, snapshot);
    if (hDib) {
        if (!SetClipboardData(CF_DIB, hDib)) {
            GlobalFree(hDib);
        }
    }
}

std::expected<void, std::wstring> ImageExporter::CopyToClipboard(const ExportOptions& options, HWND hwnd) {
    PendingClipboardSnapshot snapshot;
    snapshot.filePath = options.InputPath;
    snapshot.options = options;
    snapshot.memoryFrame = options.SourceFrame;
    snapshot.isValid = true;

    if (!OpenClipboard(hwnd)) {
        return std::unexpected(L"Failed to open clipboard.");
    }
    EmptyClipboard();

    RenderAllClipboardFormats(hwnd, snapshot);

    CloseClipboard();
    return {};
}

} // namespace QuickView
