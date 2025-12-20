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

#include <turbojpeg.h>

// High-Performance Library Includes
// libpng REMOVED - replaced by Wuffs
#include <webp/decode.h>     // libwebp
#include <webp/demux.h>
#include <avif/avif.h>       // libavif
#include <jxl/decode.h>      // libjxl
#include <jxl/resizable_parallel_runner.h>
#include <jxl/thread_parallel_runner.h>
#include <libraw/libraw.h>   // libraw

#include <string>
#include <algorithm>
#include <vector>
#include <thread> // For hardware_concurrency

// Wuffs (Google's memory-safe decoder)
// Implementation is in WuffsImpl.cpp with selective module loading
#include "WuffsLoader.h"

// Helper to read file to vector
static bool ReadFileToVector(LPCWSTR filePath, std::vector<uint8_t>& buffer) {
    HANDLE hFile = CreateFileW(filePath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    DWORD fileSize = GetFileSize(hFile, nullptr);
    if (fileSize == INVALID_FILE_SIZE) { CloseHandle(hFile); return false; }

    buffer.resize(fileSize);
    DWORD bytesRead;
    BOOL result = ReadFile(hFile, buffer.data(), fileSize, &bytesRead, nullptr);
    CloseHandle(hFile);
    return result && bytesRead == fileSize;
}

HRESULT CImageLoader::CreateWICBitmapFromMemory(UINT width, UINT height, REFGUID format, UINT stride, UINT size, BYTE* data, IWICBitmap** ppBitmap) {
    if (!m_wicFactory) return E_FAIL;
    return m_wicFactory->CreateBitmapFromMemory(width, height, format, stride, size, data, ppBitmap);
}

// ----------------------------------------------------------------------------
// JPEG (libjpeg-turbo)
// ----------------------------------------------------------------------------
HRESULT CImageLoader::LoadJPEG(LPCWSTR filePath, IWICBitmap** ppBitmap) {
    std::vector<uint8_t> jpegBuf;
    if (!ReadFileToVector(filePath, jpegBuf)) return E_FAIL;

    // Initialize TurboJPEG (Decompressor)
    tjhandle tjInstance = tj3Init(TJINIT_DECOMPRESS);
    if (!tjInstance) return E_FAIL;

    HRESULT hr = E_FAIL;
    
    // Parse header (TurboJPEG v3 API)
    if (tj3DecompressHeader(tjInstance, jpegBuf.data(), jpegBuf.size()) == 0) {
        
        // Get dimensions from handle
        int width = tj3Get(tjInstance, TJPARAM_JPEGWIDTH);
        int height = tj3Get(tjInstance, TJPARAM_JPEGHEIGHT);
        int jpegSubsamp = tj3Get(tjInstance, TJPARAM_SUBSAMP);
        int jpegColorspace = tj3Get(tjInstance, TJPARAM_COLORSPACE);

        if (width > 0 && height > 0) {
            // Decompress to BGRX (compatible with PBGRA/BGRA)
            // Stride must be 4-byte aligned (width * 4 is always 4-byte aligned)
            int pixelFormat = TJPF_BGRX; 
            int stride = width * 4;
            size_t bufSize = (size_t)stride * height;
            
            std::vector<uint8_t> pixelBuf(bufSize);
            
            if (tj3Decompress8(tjInstance, jpegBuf.data(), jpegBuf.size(), pixelBuf.data(), stride, pixelFormat) == 0) {
                 // Create WIC Bitmap from pixels
                 hr = CreateWICBitmapFromMemory(width, height, GUID_WICPixelFormat32bppPBGRA, stride, (UINT)bufSize, pixelBuf.data(), ppBitmap);
            }
        }
    }

    tj3Destroy(tjInstance);
    return hr;
}

// LoadPNG REMOVED - replaced by LoadPngWuffs (Wuffs decoder)

// ----------------------------------------------------------------------------
// WebP (libwebp)
// ----------------------------------------------------------------------------
HRESULT CImageLoader::LoadWebP(LPCWSTR filePath, IWICBitmap** ppBitmap) {
    std::vector<uint8_t> webpBuf;
    if (!ReadFileToVector(filePath, webpBuf)) return E_FAIL;

    // Advanced API for threading support
    WebPDecoderConfig config;
    if (!WebPInitDecoderConfig(&config)) return E_FAIL;

    // Enable multi-threaded decoding
    config.options.use_threads = 1;
    // Set output colorspace to BGRA (WIC compatible)
    config.output.colorspace = MODE_BGRA;

    // Decode directly to buffer managed by WebP? 
    // No, standard flow is:
    // 1. GetFeatures (to determine size)
    // 2. Allocate buffer (optional, or let WebP do it)
    // 3. Decode
    
    if (WebPGetFeatures(webpBuf.data(), webpBuf.size(), &config.input) != VP8_STATUS_OK) return E_FAIL;
    
    // Check dimensions
    int width = config.input.width;
    int height = config.input.height;
    if (width == 0 || height == 0) return E_FAIL;

    // Decode
    if (WebPDecode(webpBuf.data(), webpBuf.size(), &config) != VP8_STATUS_OK) {
        WebPFreeDecBuffer(&config.output);
        return E_FAIL;
    }

    uint8_t* output = config.output.u.RGBA.rgba;
    int stride = config.output.u.RGBA.stride;
    int size = stride * height;

    HRESULT hr = CreateWICBitmapFromMemory(width, height, GUID_WICPixelFormat32bppPBGRA, stride, size, output, ppBitmap);
    
    WebPFreeDecBuffer(&config.output);
    return hr;
}

// ----------------------------------------------------------------------------
// AVIF (libavif + dav1d)
// ----------------------------------------------------------------------------
HRESULT CImageLoader::LoadAVIF(LPCWSTR filePath, IWICBitmap** ppBitmap) {
    if (!filePath || !ppBitmap) return E_INVALIDARG;

    // Read file to memory buffer
    std::vector<uint8_t> avifBuf;
    if (!ReadFileToVector(filePath, avifBuf)) return E_FAIL;

    // Create Decoder
    avifDecoder* decoder = avifDecoderCreate();
    if (!decoder) return E_OUTOFMEMORY;
    
    // Enable multi-threaded decoding
    // Use hardware_concurrency() to maximize throughput on multi-core CPUs
    unsigned int threads = std::thread::hardware_concurrency();
    if (threads > 0) {
        decoder->maxThreads = threads;
    } else {
        decoder->maxThreads = 4; // Fallback sensible default
    }
    
    // Set Memory Source
    avifResult result = avifDecoderSetIOMemory(decoder, avifBuf.data(), avifBuf.size());
    if (result != AVIF_RESULT_OK) {
        avifDecoderDestroy(decoder);
        return E_FAIL;
    }

    // Parse
    result = avifDecoderParse(decoder);
    if (result != AVIF_RESULT_OK) {
        avifDecoderDestroy(decoder);
        return E_FAIL;
    }

    // Next Image (Frame 0)
    result = avifDecoderNextImage(decoder);
    if (result != AVIF_RESULT_OK) {
        avifDecoderDestroy(decoder);
        return E_FAIL;
    }

    // Convert YUV to RGB
    avifRGBImage rgb;
    avifRGBImageSetDefaults(&rgb, decoder->image);
    
    // Configure for WIC (BGRA, 8-bit)
    rgb.format = AVIF_RGB_FORMAT_BGRA;
    rgb.depth = 8;
    
    // Calculate stride and size
    // Note: libavif might want to allocate its own pixels or proper stride
    // Let's allocate our own buffer for safety and control
    rgb.rowBytes = rgb.width * 4;
    std::vector<uint8_t> pixelData(rgb.rowBytes * rgb.height);
    rgb.pixels = pixelData.data();
    
    result = avifImageYUVToRGB(decoder->image, &rgb);
    if (result != AVIF_RESULT_OK) {
        avifDecoderDestroy(decoder);
        return E_FAIL;
    }

    // Create WIC Bitmap
    HRESULT hr = CreateWICBitmapFromMemory(rgb.width, rgb.height, GUID_WICPixelFormat32bppPBGRA, (UINT)rgb.rowBytes, (UINT)pixelData.size(), pixelData.data(), ppBitmap);

    avifDecoderDestroy(decoder);
    return hr;
}

// ----------------------------------------------------------------------------
// JPEG XL (libjxl)
// ----------------------------------------------------------------------------
HRESULT CImageLoader::LoadJXL(LPCWSTR filePath, IWICBitmap** ppBitmap) {
    if (!filePath || !ppBitmap) return E_INVALIDARG;

    std::vector<uint8_t> jxlBuf;
    if (!ReadFileToVector(filePath, jxlBuf)) return E_FAIL;

    // 1. Create Decoder and Runner
    JxlDecoder* dec = JxlDecoderCreate(NULL);
    if (!dec) return E_OUTOFMEMORY;

    // Use max threads (default to system CPU count)
    void* runner = JxlResizableParallelRunnerCreate(NULL);
    if (!runner) {
        JxlDecoderDestroy(dec);
        return E_OUTOFMEMORY;
    }
    
    JxlDecoderSetParallelRunner(dec, JxlResizableParallelRunner, runner);

    // 2. Subscribe to events
    if (JXL_DEC_SUCCESS != JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE)) {
        JxlResizableParallelRunnerDestroy(runner);
        JxlDecoderDestroy(dec);
        return E_FAIL;
    }

    // 3. Set Input
    JxlDecoderSetInput(dec, jxlBuf.data(), jxlBuf.size());

    JxlBasicInfo info;
    JxlPixelFormat format = { 4, JXL_TYPE_UINT8, JXL_LITTLE_ENDIAN, 0 }; // RGBA
    
    std::vector<uint8_t> pixels;
    HRESULT hr = E_FAIL;

    // 4. Decode Loop
    for (;;) {
        JxlDecoderStatus status = JxlDecoderProcessInput(dec);
        
        if (status == JXL_DEC_ERROR) {
            hr = E_FAIL;
            break;
        }
        else if (status == JXL_DEC_SUCCESS) {
            hr = S_OK;
            break;
        }
        else if (status == JXL_DEC_BASIC_INFO) {
            if (JXL_DEC_SUCCESS != JxlDecoderGetBasicInfo(dec, &info)) {
                 hr = E_FAIL; break; 
            }
            // Resize buffer
            size_t stride = info.xsize * 4;
            pixels.resize(stride * info.ysize);
        }
        else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
            size_t bufferSize = pixels.size();
            JxlDecoderSetImageOutBuffer(dec, &format, pixels.data(), bufferSize);
        }
        else if (status == JXL_DEC_FULL_IMAGE) {
            // Nothing to do, just continue
        }
        else {
            // Unknown status or need more input (should not happen with full buffer)
            // break; (Don't break, loop might need to continue)
        }
    }

    if (SUCCEEDED(hr)) {
        // JXL outputs RGBA (by default assumption with 4 channels and standard).
        // WIC needs a GUID. We use GUID_WICPixelFormat32bppRGBA.
        // QuickView's RenderEngine might expect PBGRA. WIC FormatConverter usually runs after loading if needed (fallback uses it).
        // BUT, CImageLoader::LoadToMemory returns a Bitmap. Main loop expects to Draw it.
        // D2D CreateBitmapFromWicBitmap automatically converts if possible.
        // So RGBA is fine.
        hr = CreateWICBitmapFromMemory(info.xsize, info.ysize, GUID_WICPixelFormat32bppRGBA, info.xsize * 4, (UINT)pixels.size(), pixels.data(), ppBitmap);
    }

    JxlResizableParallelRunnerDestroy(runner);
    JxlDecoderDestroy(dec);
    return hr;
}

// ----------------------------------------------------------------------------
// RAW (LibRaw)
// ----------------------------------------------------------------------------
HRESULT CImageLoader::LoadRaw(LPCWSTR filePath, IWICBitmap** ppBitmap) { 
    // Optimization: Try to load embedded JPEG preview first (FAST)
    // Fallback: Full RAW decode (SLOW)

    std::vector<uint8_t> rawBuf;
    if (!ReadFileToVector(filePath, rawBuf)) return E_FAIL;

    LibRaw RawProcessor;
    if (RawProcessor.open_buffer(rawBuf.data(), rawBuf.size()) != LIBRAW_SUCCESS) return E_FAIL;

    // 1. Try Unpack Thumbnail (Embedded Preview) - FASTEST
    if (RawProcessor.unpack_thumb() == LIBRAW_SUCCESS) {
        int err = 0;
        libraw_processed_image_t* thumb = RawProcessor.dcraw_make_mem_thumb(&err);
        
        if (thumb) {
            if (thumb->type == LIBRAW_IMAGE_JPEG) {
                // JPEG Thumbnail
                ComPtr<IWICStream> stream;
                HRESULT hr = m_wicFactory->CreateStream(&stream);
                if (SUCCEEDED(hr)) hr = stream->InitializeFromMemory(thumb->data, thumb->data_size);
                
                ComPtr<IWICBitmapDecoder> decoder;
                if (SUCCEEDED(hr)) hr = m_wicFactory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder);
                
                ComPtr<IWICBitmapFrameDecode> frame;
                if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
                
                ComPtr<IWICFormatConverter> converter;
                if (SUCCEEDED(hr)) hr = m_wicFactory->CreateFormatConverter(&converter);
                if (SUCCEEDED(hr)) hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.f, WICBitmapPaletteTypeMedianCut);
                
                if (SUCCEEDED(hr)) {
                    hr = m_wicFactory->CreateBitmapFromSource(converter.Get(), WICBitmapCacheOnLoad, ppBitmap);
                }
                
                if (SUCCEEDED(hr)) {
                    RawProcessor.dcraw_clear_mem(thumb);
                    return hr; // Success with JPEG Preview!
                }
            } else if (thumb->type == LIBRAW_IMAGE_BITMAP) {
                // Bitmap Thumbnail (RGB)
                if (thumb->bits == 8 && thumb->colors == 3) {
                    UINT width = thumb->width;
                    UINT height = thumb->height;
                    UINT stride = width * 3;
                    HRESULT hr = CreateWICBitmapFromMemory(width, height, GUID_WICPixelFormat24bppRGB, stride, thumb->data_size, thumb->data, ppBitmap);
                    if (SUCCEEDED(hr)) {
                        RawProcessor.dcraw_clear_mem(thumb);
                        return hr; // Success with Bitmap Preview!
                    }
                }
            }
            RawProcessor.dcraw_clear_mem(thumb);
        }
    }
    
    // 2. Fallback: Full Decode (Slow)
    // Optimization: Disable Auto WB (slow), use Camera WB
    RawProcessor.imgdata.params.use_camera_wb = 1;
    RawProcessor.imgdata.params.use_auto_wb = 0; // Speed up
    RawProcessor.imgdata.params.user_qual = 2;   // 0=Linear(fast), 2=AHD(good), 3=AHD+Interpolation
    
    // If you want extreme speed at cost of resolution, uncomment:
    // RawProcessor.imgdata.params.half_size = 1; 

    if (RawProcessor.unpack() != LIBRAW_SUCCESS) return E_FAIL;
    if (RawProcessor.dcraw_process() != LIBRAW_SUCCESS) return E_FAIL;
    
    libraw_processed_image_t* image = RawProcessor.dcraw_make_mem_image();
    if (!image) return E_FAIL;
    
    HRESULT hr = E_FAIL;
    
    if (image->type == LIBRAW_IMAGE_BITMAP) {
        if (image->bits == 8 && image->colors == 3) {
            UINT width = image->width;
            UINT height = image->height;
            UINT stride = width * 3; 
            hr = CreateWICBitmapFromMemory(width, height, GUID_WICPixelFormat24bppRGB, stride, image->data_size, image->data, ppBitmap);
        }
    }
    
    RawProcessor.dcraw_clear_mem(image);
    return hr;
}

HRESULT CImageLoader::LoadToMemory(LPCWSTR filePath, IWICBitmap** ppBitmap, std::wstring* pLoaderName) {
    if (!filePath || !ppBitmap) return E_INVALIDARG;
    
    std::wstring path = filePath;
    std::transform(path.begin(), path.end(), path.begin(), ::towlower);
    
    // -------------------------------------------------------------
    // Architecture Upgrade: Robust Format Detection & Fallback
    // -------------------------------------------------------------
    
    // 1. Read first 16 bytes for Magic Number
    uint8_t magic[16] = {0};
    bool magicRead = false;
    {
        HANDLE hFile = CreateFileW(filePath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile != INVALID_HANDLE_VALUE) {
            DWORD bytesRead = 0;
            if (ReadFile(hFile, magic, 16, &bytesRead, nullptr) && bytesRead >= 4) {
                magicRead = true;
            }
            CloseHandle(hFile);
        }
    }
    
    enum class DetectedFormat { Unknown, JPEG, PNG, GIF, WebP, AVIF, JXL, RAW };
    DetectedFormat detected = DetectedFormat::Unknown;

    if (magicRead) {
        // Check JPEG: FF D8 FF
        if (magic[0] == 0xFF && magic[1] == 0xD8 && magic[2] == 0xFF) 
            detected = DetectedFormat::JPEG;
        
        // Check PNG: 89 50 4E 47
        else if (magic[0] == 0x89 && magic[1] == 0x50 && magic[2] == 0x4E && magic[3] == 0x47) 
            detected = DetectedFormat::PNG;
            
        // Check WebP: RIFF ... WEBP
        else if (magic[0] == 'R' && magic[1] == 'I' && magic[2] == 'F' && magic[3] == 'F' &&
                 magic[8] == 'W' && magic[9] == 'E' && magic[10] == 'B' && magic[11] == 'P')
            detected = DetectedFormat::WebP;
            
        // Check AVIF: ftypavif
        // Usually located at bytes 4-12, e.g., ....ftypavif
        else if (magic[4] == 'f' && magic[5] == 't' && magic[6] == 'y' && magic[7] == 'p' &&
                 magic[8] == 'a' && magic[9] == 'v' && magic[10] == 'i' && magic[11] == 'f')
            detected = DetectedFormat::AVIF;
            
        // Check JXL: FF 0A or 00 00 00 0C JXL 
        else if (magic[0] == 0xFF && magic[1] == 0x0A)
            detected = DetectedFormat::JXL;
        else if (magic[0] == 0x00 && magic[1] == 0x00 && magic[2] == 0x00 && magic[3] == 0x0C &&
                 magic[4] == 'J' && magic[5] == 'X' && magic[6] == 'L' && magic[7] == ' ')
            detected = DetectedFormat::JXL;
            
        // Check GIF: GIF87a or GIF89a
        else if (magic[0] == 'G' && magic[1] == 'I' && magic[2] == 'F' && magic[3] == '8' &&
                 (magic[4] == '7' || magic[4] == '9') && magic[5] == 'a')
            detected = DetectedFormat::GIF;
            
        // RAW is tricky via magic (many formats), rely on extension as secondary hint + WIC fallback
        // But for completeness, check extension if magic is unknown
    }

    // 2. Dispatch Logic
    HRESULT hr = E_FAIL;
    
    switch (detected) {
        case DetectedFormat::JPEG: 
            hr = LoadJPEG(filePath, ppBitmap); 
            if (SUCCEEDED(hr) && pLoaderName) *pLoaderName = L"TurboJPEG (v3 + ASM)"; 
            break;
        case DetectedFormat::PNG:  
            hr = LoadPngWuffs(filePath, ppBitmap);  // Wuffs (Google's memory-safe decoder)
            if (SUCCEEDED(hr) && pLoaderName) *pLoaderName = L"Wuffs PNG (Safe+Fast)"; 
            break;
        case DetectedFormat::GIF:
            hr = LoadGifWuffs(filePath, ppBitmap);  // Wuffs GIF
            if (SUCCEEDED(hr) && pLoaderName) *pLoaderName = L"Wuffs GIF (Chrome)"; 
            break;
        case DetectedFormat::WebP: 
            hr = LoadWebP(filePath, ppBitmap); 
            if (SUCCEEDED(hr) && pLoaderName) *pLoaderName = L"LibWebP (SIMD)"; 
            break;
        case DetectedFormat::AVIF: 
            hr = LoadAVIF(filePath, ppBitmap); 
            if (SUCCEEDED(hr) && pLoaderName) *pLoaderName = L"Dav1d (AV1)"; 
            break;
        case DetectedFormat::JXL:  
            hr = LoadJXL(filePath, ppBitmap); 
            if (SUCCEEDED(hr) && pLoaderName) *pLoaderName = L"LibJXL (Highway)"; 
            break;
        case DetectedFormat::Unknown:
        default:
            if (path.ends_with(L".arw") || path.ends_with(L".cr2") || path.ends_with(L".cr3") || 
                path.ends_with(L".nef") || path.ends_with(L".dng") || path.ends_with(L".orf") || 
                path.ends_with(L".rw2") || path.ends_with(L".raf") || path.ends_with(L".pef") || 
                path.ends_with(L".srw")) {
                 hr = LoadRaw(filePath, ppBitmap);
                 if (SUCCEEDED(hr) && pLoaderName) *pLoaderName = L"LibRaw (Optimized)";
            }
            break;
    }
    
    // If Specialized Loader Succeeded, Return
    if (SUCCEEDED(hr)) return hr;

    // 3. Robust Fallback to WIC (Standard Loading)
    if (pLoaderName) *pLoaderName = L"WIC (Fallback)";
    
    // If High-Perf loader failed (e.g. malformed specific header, unsupported feature) OR format verified but unimplemented (stub),
    // OR format unknown.
    // ---------------------------------------------------------

    // 1. Load Lazy Source
    ComPtr<IWICBitmapSource> source;
    // Note: Can't use this->LoadFromFile nicely if we want to avoid double-open, 
    // but LoadFromFile uses WIC factory directly. Let's just use the WIC path inline or call existing helper.
    // Re-use existing WIC fallback logic:
    
    ComPtr<IWICBitmapDecoder> decoder;
    hr = m_wicFactory->CreateDecoderFromFilename(
        filePath, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder
    );
    if (FAILED(hr)) return hr;

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) return hr;
    
    // 2. Convert to D2D Compatible Format (PBGRA32)
    ComPtr<IWICFormatConverter> converter;
    hr = m_wicFactory->CreateFormatConverter(&converter);
    if (FAILED(hr)) return hr;

    hr = converter->Initialize(
        frame.Get(), // Use frame source
        GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone,
        nullptr,
        0.f,
        WICBitmapPaletteTypeMedianCut
    );
    if (FAILED(hr)) return hr;

    // 3. Force Decode to Memory
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

// ----------------------------------------------------------------------------
// Wuffs PNG Decoder (Google's memory-safe decoder)
// ----------------------------------------------------------------------------
HRESULT CImageLoader::LoadPngWuffs(LPCWSTR filePath, IWICBitmap** ppBitmap) {
    std::vector<uint8_t> pngBuf;
    if (!ReadFileToVector(filePath, pngBuf)) return E_FAIL;

    uint32_t width = 0, height = 0;
    std::vector<uint8_t> pixelData;
    
    if (!WuffsLoader::DecodePNG(pngBuf.data(), pngBuf.size(), &width, &height, pixelData)) {
        return E_FAIL;
    }

    size_t stride = width * 4;
    return CreateWICBitmapFromMemory(width, height, GUID_WICPixelFormat32bppBGRA,
                                      (UINT)stride, (UINT)pixelData.size(), pixelData.data(), ppBitmap);
}

// ----------------------------------------------------------------------------
// Wuffs GIF Decoder (First frame only for now)
// ----------------------------------------------------------------------------
HRESULT CImageLoader::LoadGifWuffs(LPCWSTR filePath, IWICBitmap** ppBitmap) {
    std::vector<uint8_t> gifBuf;
    if (!ReadFileToVector(filePath, gifBuf)) return E_FAIL;

    uint32_t width = 0, height = 0;
    std::vector<uint8_t> pixelData;
    
    if (!WuffsLoader::DecodeGIF(gifBuf.data(), gifBuf.size(), &width, &height, pixelData)) {
        return E_FAIL;
    }

    size_t stride = width * 4;
    return CreateWICBitmapFromMemory(width, height, GUID_WICPixelFormat32bppBGRA,
                                      (UINT)stride, (UINT)pixelData.size(), pixelData.data(), ppBitmap);
}
