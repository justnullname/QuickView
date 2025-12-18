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
#include <png.h>             // libpng
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

// ----------------------------------------------------------------------------
// PNG (libpng)
// ----------------------------------------------------------------------------
struct MemReaderState {
    const uint8_t* data;
    size_t size;
    size_t offset;
};

static void PngReadCallback(png_structp png_ptr, png_bytep data, png_size_t length) {
    MemReaderState* state = (MemReaderState*)png_get_io_ptr(png_ptr);
    if (state->offset + length > state->size) {
        png_error(png_ptr, "Read Error");
    }
    memcpy(data, state->data + state->offset, length);
    state->offset += length;
}

HRESULT CImageLoader::LoadPNG(LPCWSTR filePath, IWICBitmap** ppBitmap) { 
    std::vector<uint8_t> pngBuf;
    if (!ReadFileToVector(filePath, pngBuf)) return E_FAIL;

    // Check PNG signature
    if (pngBuf.size() < 8 || png_sig_cmp(pngBuf.data(), 0, 8)) return E_FAIL;

    png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png_ptr) return E_FAIL;

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        png_destroy_read_struct(&png_ptr, nullptr, nullptr);
        return E_FAIL;
    }

    if (setjmp(png_jmpbuf(png_ptr))) {
        png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
        return E_FAIL;
    }

    MemReaderState state = { pngBuf.data(), pngBuf.size(), 0 };
    png_set_read_fn(png_ptr, &state, PngReadCallback);

    png_read_info(png_ptr, info_ptr);

    int bitDepth, colorType;
    png_uint_32 width, height;
    png_get_IHDR(png_ptr, info_ptr, &width, &height, &bitDepth, &colorType, nullptr, nullptr, nullptr);

    // Transforms to ensure BGRA output
    if (colorType == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png_ptr);

    if (colorType == PNG_COLOR_TYPE_GRAY && bitDepth < 8)
        png_set_expand_gray_1_2_4_to_8(png_ptr);

    if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png_ptr);

    if (bitDepth == 16)
        png_set_strip_16(png_ptr);

    if (colorType == PNG_COLOR_TYPE_GRAY || colorType == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png_ptr);

    // QuickView uses PBGRA usually, but for WIC creation from memory, standard BGRA is fine depending on the GUID used.
    // GUID_WICPixelFormat32bppBGRA is standard. GUID_WICPixelFormat32bppPBGRA is premultiplied.
    // D2D prefers PBGRA.
    // libpng doesn't support premultiplication out of the box easily without manual processing.
    // Let's output BGRA (Add alpha if missing) and let WIC converter handle premultiplication if needed eventually,
    // OR just use BGRA and create a bitmap.
    
    // Force Alpha channel
    if (!(colorType & PNG_COLOR_MASK_ALPHA))
        png_set_add_alpha(png_ptr, 0xFF, PNG_FILLER_AFTER);

    // Swap RGB to BGR
    png_set_bgr(png_ptr);

    png_read_update_info(png_ptr, info_ptr);

    // Read rows
    size_t rowBytes = png_get_rowbytes(png_ptr, info_ptr);
    size_t imgSize = rowBytes * height;
    std::vector<uint8_t> pixelData(imgSize);
    
    std::vector<png_bytep> rowPointers(height);
    for (uint32_t i = 0; i < height; ++i) {
        rowPointers[i] = pixelData.data() + i * rowBytes;
    }

    png_read_image(png_ptr, rowPointers.data());
    png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);

    // We have BGRA now.
    // Note: Creating WIC bitmap with GUID_WICPixelFormat32bppBGRA.
    return CreateWICBitmapFromMemory(width, height, GUID_WICPixelFormat32bppBGRA, (UINT)rowBytes, (UINT)imgSize, pixelData.data(), ppBitmap);
}

// ----------------------------------------------------------------------------
// WebP (libwebp)
// ----------------------------------------------------------------------------
HRESULT CImageLoader::LoadWebP(LPCWSTR filePath, IWICBitmap** ppBitmap) {
    std::vector<uint8_t> webpBuf;
    if (!ReadFileToVector(filePath, webpBuf)) return E_FAIL;

    int width, height;
    if (!WebPGetInfo(webpBuf.data(), webpBuf.size(), &width, &height)) return E_FAIL;

    // Decode to BGRA
    uint8_t* output = WebPDecodeBGRA(webpBuf.data(), webpBuf.size(), &width, &height);
    if (!output) return E_FAIL;

    // Create WIC Bitmap
    UINT stride = width * 4;
    UINT size = stride * height;
    HRESULT hr = CreateWICBitmapFromMemory(width, height, GUID_WICPixelFormat32bppPBGRA, stride, size, output, ppBitmap);
    
    // Free WebP buffer (WebPDecodeBGRA allocates memory using internal malloc)
    WebPFree(output);
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
    // Uses standard LibRaw processing
    // Warning: extensive memory usage

    std::wstring pathStr = filePath;
    std::string pathUtf8(pathStr.begin(), pathStr.end()); // Simple conversion for now. LibRaw supports wstring on Windows?
    // LibRaw::open_file accepts wchar_t* on Windows if compiled with unicode support?
    // Let's check headers or try open_file(filePath).
    // Safest is open_buffer to avoid path encoding issues if API differs.
    
    std::vector<uint8_t> rawBuf;
    if (!ReadFileToVector(filePath, rawBuf)) return E_FAIL;

    LibRaw RawProcessor;
    
    if (RawProcessor.open_buffer(rawBuf.data(), rawBuf.size()) != LIBRAW_SUCCESS) return E_FAIL;
    if (RawProcessor.unpack() != LIBRAW_SUCCESS) return E_FAIL;
    
    // Standard processing (auto-brightness, etc)
    // dcraw_process() can be slow.
    if (RawProcessor.dcraw_process() != LIBRAW_SUCCESS) return E_FAIL;
    
    // Get memory image
    libraw_processed_image_t* image = RawProcessor.dcraw_make_mem_image();
    if (!image) return E_FAIL;
    
    HRESULT hr = E_FAIL;
    
    if (image->type == LIBRAW_IMAGE_BITMAP) {
        // usually only 16-bit support later, but dcraw_make_mem_image usually returns 8-bit RGB by default process settings?
        // Check depth
        if (image->bits == 8 && image->colors == 3) {
            // RGB structure
            UINT width = image->width;
            UINT height = image->height;
            UINT stride = width * 3; 
            
            // WIC needs a GUID. GUID_WICPixelFormat24bppRGB.
            // Data in image->data.
            hr = CreateWICBitmapFromMemory(width, height, GUID_WICPixelFormat24bppRGB, stride, image->data_size, image->data, ppBitmap);
        }
    }
    
    RawProcessor.dcraw_clear_mem(image);
    return hr;
}

HRESULT CImageLoader::LoadToMemory(LPCWSTR filePath, IWICBitmap** ppBitmap) {
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
    
    enum class DetectedFormat { Unknown, JPEG, PNG, WebP, AVIF, JXL, RAW };
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
            
        // RAW is tricky via magic (many formats), rely on extension as secondary hint + WIC fallback
        // But for completeness, check extension if magic is unknown
    }

    // 2. Dispatch Logic
    HRESULT hr = E_FAIL;
    
    switch (detected) {
        case DetectedFormat::JPEG: hr = LoadJPEG(filePath, ppBitmap); break;
        case DetectedFormat::PNG:  hr = LoadPNG(filePath, ppBitmap); break;
        case DetectedFormat::WebP: hr = LoadWebP(filePath, ppBitmap); break;
        case DetectedFormat::AVIF: hr = LoadAVIF(filePath, ppBitmap); break;
        case DetectedFormat::JXL:  hr = LoadJXL(filePath, ppBitmap); break;
        case DetectedFormat::Unknown:
        default:
            // Fallback to extension check for RAW or generic
            if (path.ends_with(L".arw") || path.ends_with(L".cr2") || path.ends_with(L".nef") || path.ends_with(L".dng")) {
                 hr = LoadRaw(filePath, ppBitmap);
            }
            break;
    }
    
    // If Specialized Loader Succeeded, Return
    if (SUCCEEDED(hr)) return hr;

    // 3. Robust Fallback to WIC (Standard Loading)
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
