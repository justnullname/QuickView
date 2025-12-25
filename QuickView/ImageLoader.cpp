#include "pch.h"

// NanoSVG
#pragma warning(push)
#pragma warning(disable: 4244) 
#pragma warning(disable: 4702)
#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "../third_party/nanosvg/nanosvg.h"
#include "../third_party/nanosvg/nanosvgrast.h"
#pragma warning(pop)
#include "ImageLoader.h"
#include <jxl/decode.h> // JXL
#include "WuffsLoader.h"
#include "StbLoader.h"
#include "TinyExrLoader.h"
#include <immintrin.h> // SIMD
#include "SIMDUtils.h"
#include <thread>
#include "PreviewExtractor.h"

// Helper to detect format from file content (Magic Bytes)
static std::wstring DetectFormatFromContent(LPCWSTR filePath) {
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
    
    if (!magicRead) return L"Unknown";

    // Check JPEG: FF D8 FF
    if (magic[0] == 0xFF && magic[1] == 0xD8 && magic[2] == 0xFF) return L"JPEG";
    
    // Check PNG: 89 50 4E 47
    if (magic[0] == 0x89 && magic[1] == 0x50 && magic[2] == 0x4E && magic[3] == 0x47) return L"PNG";
        
    // Check WebP: RIFF ... WEBP
    if (magic[0] == 'R' && magic[1] == 'I' && magic[2] == 'F' && magic[3] == 'F' &&
             magic[8] == 'W' && magic[9] == 'E' && magic[10] == 'B' && magic[11] == 'P') return L"WebP";
        
    // Check AVIF: ftypavif OR ftypavis (AVIF Sequence)
    if (magic[4] == 'f' && magic[5] == 't' && magic[6] == 'y' && magic[7] == 'p') {
        bool isAvif = (magic[8] == 'a' && magic[9] == 'v' && magic[10] == 'i' && magic[11] == 'f');
        bool isAvis = (magic[8] == 'a' && magic[9] == 'v' && magic[10] == 'i' && magic[11] == 's');
        if (isAvif || isAvis) return L"AVIF";
    }

    // Check HEIC/HEIF: ftyp + brand
    // Common brands: heic, heix, hevc, heim, heis, hevm, hevs, mif1, msf1
    if (magic[4] == 'f' && magic[5] == 't' && magic[6] == 'y' && magic[7] == 'p') {
        // Check brand at offset 8
        if ((magic[8] == 'h' && magic[9] == 'e' && magic[10] == 'i' && (magic[11] == 'c' || magic[11] == 'x' || magic[11] == 's' || magic[11] == 'm')) || // heic, heix, heis, heim
            (magic[8] == 'h' && magic[9] == 'e' && magic[10] == 'v' && (magic[11] == 'c' || magic[11] == 'm' || magic[11] == 's')) || // hevc, hevm, hevs
            (magic[8] == 'm' && magic[9] == 'i' && magic[10] == 'f' && magic[11] == '1') || // mif1
             (magic[8] == 'm' && magic[9] == 's' && magic[10] == 'f' && magic[11] == '1'))   // msf1
        {
             return L"HEIC"; // Unified as HEIC/HEIF
        }
    }
        
    // Check JXL: FF 0A or 00 00 00 0C JXL 
    if (magic[0] == 0xFF && magic[1] == 0x0A) return L"JXL";
    if (magic[0] == 0x00 && magic[1] == 0x00 && magic[2] == 0x00 && magic[3] == 0x0C &&
             magic[4] == 'J' && magic[5] == 'X' && magic[6] == 'L' && magic[7] == ' ') return L"JXL";
        
    // Check GIF: GIF87a or GIF89a
    if (magic[0] == 'G' && magic[1] == 'I' && magic[2] == 'F' && magic[3] == '8' &&
             (magic[4] == '7' || magic[4] == '9') && magic[5] == 'a') return L"GIF";

    // Check BMP: BM
    if (magic[0] == 'B' && magic[1] == 'M') return L"BMP";

    // Check PSD: 8BPS
    if (magic[0] == '8' && magic[1] == 'B' && magic[2] == 'P' && magic[3] == 'S') return L"PSD";

    // Check HDR: #?RADIANCE or #?RGBE
    if (magic[0] == '#' && magic[1] == '?') return L"HDR";

    // Check EXR: v/1\x01 (0x76 0x2f 0x31 0x01)
    if (magic[0] == 0x76 && magic[1] == 0x2F && magic[2] == 0x31 && magic[3] == 0x01) return L"EXR";
        
    // Check PIC: 0x53 0x80 ...
    if (magic[0] == 0x53 && magic[1] == 0x80 && magic[2] == 0xF6 && magic[3] == 0x34) return L"PIC";
        
    // Check PNM: P1-P7
    if (magic[0] == 'P' && magic[1] >= '1' && magic[1] <= '7') return L"PNM";
    
    // Check QOI: qoif
    if (magic[0] == 'q' && magic[1] == 'o' && magic[2] == 'i' && magic[3] == 'f') return L"QOI";
    
    // Check PCX: 0x0A ...
    if (magic[0] == 0x0A) return L"PCX";
    
    // Check ICO: 00 00 01 00
    if (magic[0] == 0x00 && magic[1] == 0x00 && magic[2] == 0x01 && magic[3] == 0x00) return L"ICO";

    // Check TIFF: II (49 49 2A 00) or MM (4D 4D 00 2A)
    if (magic[0] == 0x49 && magic[1] == 0x49 && magic[2] == 0x2A && magic[3] == 0x00) return L"TIFF";
    if (magic[0] == 0x4D && magic[1] == 0x4D && magic[2] == 0x00 && magic[3] == 0x2A) return L"TIFF";
    
    return L"Unknown";
}

HRESULT CImageLoader::Initialize(IWICImagingFactory* wicFactory) {
    if (!wicFactory) return E_INVALIDARG;
    m_wicFactory = wicFactory;
    return S_OK;
}

HRESULT CImageLoader::LoadFromFile(LPCWSTR filePath, IWICBitmapSource** bitmap) {
    if (!filePath || !bitmap) return E_INVALIDARG;

    // Delegate to the new architecture (Detected Logic + Specialized Loaders + WIC Fallback)
    IWICBitmap* pBitmap = nullptr;
    std::wstring loaderName; // Optional, or ignore
    
    // We cast IWICBitmap** to IWICBitmapSource**? 
    // No, LoadToMemory returns IWICBitmap. We want IWICBitmapSource.
    // IWICBitmap* supports IWICBitmapSource interface.
    // But pointers are different? No, inheritance.
    
    // Actually LoadToMemory signature: 
    // HRESULT LoadToMemory(LPCWSTR filePath, IWICBitmap** ppBitmap, std::wstring* pLoaderName = nullptr);
    
    ComPtr<IWICBitmap> ptrBitmap;
    HRESULT hr = LoadToMemory(filePath, &ptrBitmap, nullptr);
    
    if (SUCCEEDED(hr)) {
        *bitmap = ptrBitmap.Detach(); // Detach gives strictly the pointer.
        return S_OK;
    }
    
    return hr;
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

// Global storage for format details (set by loaders, read by main)
static std::wstring g_lastFormatDetails;

// Helper to read file to vector
bool ReadFileToVector(LPCWSTR filePath, std::vector<uint8_t>& buffer) {
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

// Standard JPEG luminance quantization table (Q=50)
static const int std_luminance_qtable[64] = {
    16, 11, 10, 16, 24, 40, 51, 61,
    12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56,
    14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68, 109, 103, 77,
    24, 35, 55, 64, 81, 104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101,
    72, 92, 95, 98, 112, 100, 103, 99
};

// Estimate JPEG quality from quantization table in buffer
// Returns 0-100, or -1 if unable to parse
static int GetJpegQualityFromBuffer(const uint8_t* data, size_t size) {
    if (!data || size < 20) return -1;
    
    // Search for DQT marker (0xFF, 0xDB)
    for (size_t i = 0; i < size - 70; i++) {
        if (data[i] == 0xFF && data[i + 1] == 0xDB) {
            // Found DQT marker
            size_t offset = i + 4; // Skip marker + length
            if (offset + 64 >= size) return -1;
            
            // Read first 64 quantization values
            int qtable[64];
            for (int j = 0; j < 64; j++) {
                qtable[j] = data[offset + j];
            }
            
            // Calculate average scale compared to standard table
            double total_scale = 0.0;
            for (int j = 0; j < 64; j++) {
                if (std_luminance_qtable[j] > 0) {
                    total_scale += (double)qtable[j] / std_luminance_qtable[j];
                }
            }
            double avg_scale = total_scale / 64.0;
            
            // Convert scale to quality (0-100)
            int quality;
            if (avg_scale <= 0.0) quality = 100;
            else if (avg_scale >= 1.0) {
                quality = (int)(50.0 / avg_scale);
            } else {
                quality = (int)(100.0 - (avg_scale * 50.0));
            }
            
            if (quality > 100) quality = 100;
            if (quality < 1) quality = 1;
            
            return quality;
        }
    }
    return -1;
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
                 
                 // Extract format details with quality estimation
                 if (SUCCEEDED(hr)) {
                     std::wstring subsamp;
                     switch (jpegSubsamp) {
                         case TJSAMP_444: subsamp = L"4:4:4"; break;
                         case TJSAMP_422: subsamp = L"4:2:2"; break;
                         case TJSAMP_420: subsamp = L"4:2:0"; break;
                         case TJSAMP_GRAY: subsamp = L"Gray"; break;
                         case TJSAMP_440: subsamp = L"4:4:0"; break;
                         default: subsamp = L""; break;
                     }
                     
                     // Estimate quality from quantization table
                     int quality = GetJpegQualityFromBuffer(jpegBuf.data(), jpegBuf.size());
                     if (quality > 0) {
                         wchar_t buf[64];
                         swprintf_s(buf, L"%s Q~%d", subsamp.c_str(), quality);
                         g_lastFormatDetails = buf;
                     } else {
                         g_lastFormatDetails = subsamp;
                     }
                 }
            }
        }
    }

    tj3Destroy(tjInstance);
    return hr;
}

// ----------------------------------------------------------------------------
// Step 2: Optimized Thumbnail Loading
// ----------------------------------------------------------------------------

HRESULT CImageLoader::LoadThumbJPEGFromMemory(const uint8_t* pBuf, size_t size, int targetSize, ThumbData* pData) {
    if (!pData || !pBuf || size == 0) return E_INVALIDARG;

    tjhandle tj = tj3Init(TJINIT_DECOMPRESS);
    if (!tj) return E_FAIL;

    // Helper to auto-destroy handle
    struct TjGuard { tjhandle h; ~TjGuard() { if(h) tj3Destroy(h); } } guard{tj};

    int width = 0, height = 0;
    if (tj3DecompressHeader(tj, pBuf, size) < 0) {
        return E_FAIL;
    }
    width = tj3Get(tj, TJPARAM_JPEGWIDTH);
    height = tj3Get(tj, TJPARAM_JPEGHEIGHT);
    pData->origWidth = width;
    pData->origHeight = height;
    pData->fileSize = size;
    
    if (width <= 0 || height <= 0) return E_FAIL;

    // Calculate Scaling Factor
    int numFactors;
    tjscalingfactor* factors = tj3GetScalingFactors(&numFactors);
    tjscalingfactor chosenFactor = { 1, 1 };
    
    // Find smallest factor that produces a dimension >= targetSize
    int bestMetric = 999999;
    for (int i = 0; i < numFactors; i++) {
        int sW = TJSCALED(width, factors[i]);
        int sH = TJSCALED(height, factors[i]);
        // Ideally we want >= targetSize.
        // If all are smaller, pick largest (1/1).
        if (sW >= targetSize && sH >= targetSize) {
            int metric = sW; 
            if (metric < bestMetric) {
                bestMetric = metric;
                chosenFactor = factors[i];
            }
        }
    }

    if (tj3SetScalingFactor(tj, chosenFactor) < 0) {
        // Fallback to 1/1
        tjscalingfactor one = {1, 1};
        tj3SetScalingFactor(tj, one);
    }

    int finalW = TJSCALED(width, chosenFactor);
    int finalH = TJSCALED(height, chosenFactor);

    // Decode directly to target buffer
    pData->width = finalW;
    pData->height = finalH;
    pData->stride = finalW * 4;
    pData->pixels.resize(pData->stride * finalH);

    // Use TJPF_BGRA for D2D compatibility
    // Note: TurboJPEG BGRA is straight alpha. If extracted thumb has no alpha (JPEG doesn't), it's opaque (A=255).
    if (tj3Decompress8(tj, pBuf, size, pData->pixels.data(), pData->stride, TJPF_BGRA) < 0) {
        return E_FAIL;
    }

    pData->isValid = true;
    return S_OK;
}

HRESULT CImageLoader::LoadThumbWebPFromMemory(const uint8_t* pBuf, size_t size, int targetSize, ThumbData* pData) {
    if (!pData || !pBuf || size == 0) return E_INVALIDARG;

    // Advanced API for threading + scaling support
    WebPDecoderConfig config;
    if (!WebPInitDecoderConfig(&config)) return E_FAIL;
    
    if (WebPGetFeatures(pBuf, size, &config.input) != VP8_STATUS_OK) return E_FAIL;
    
    int w = config.input.width;
    int h = config.input.height;
    pData->origWidth = w;
    pData->origHeight = h;
    pData->fileSize = size;
    if (w <= 0 || h <= 0) return E_FAIL;

    // Calculate Ratio aiming for targetSize in smallest dimension (cover)? 
    // Or fit?
    // TurboJPEG logic above tries to find a metric >= targetSize.
    // Let's match typical scaler logic: Match one dimension to target size?
    // Actually WebP scaling is arbitrary (unlike JPEG which is 1/2, 1/4, 1/8).
    // So we can request exact size?
    // WebP documentation says "scaled_width/height".
    // "The output buffer will be scaled to these dimensions".
    // So we can request exactly targetSize X something.
    
    // Let's compute exact target size maintaining aspect ratio.
    float ratio = std::max((float)targetSize/w, (float)targetSize/h);
    // If ratio >= 1.0 (Image smaller than target), keep original.
    int newW, newH;
    if (ratio >= 1.0f) {
        newW = w;
        newH = h;
    } else {
        newW = (int)(w * ratio);
        newH = (int)(h * ratio);
        if (newW < 1) newW = 1; 
        if (newH < 1) newH = 1;
    }

    config.options.use_threads = 1;
    config.options.use_scaling = 1;
    config.options.scaled_width = newW;
    config.options.scaled_height = newH;
    config.options.no_fancy_upsampling = 1; // Speed up
    config.output.colorspace = MODE_BGRA;   // Direct BGRA
    
    // Decode
    if (WebPDecode(pBuf, size, &config) != VP8_STATUS_OK) {
         WebPFreeDecBuffer(&config.output);
         return E_FAIL;
    }
    
    // Copy/Move to ThumbData
    // We could have decoded directly to pData->pixels if we resized it first and used external memory.
    // But stride handling: WebP stride might differ?
    // config.output.u.RGBA.stride usually is width*4 for MODE_BGRA.
    
    uint8_t* output = config.output.u.RGBA.rgba;
    int stride = config.output.u.RGBA.stride;
    int dataSize = config.output.u.RGBA.size;
    
    pData->width = newW;
    pData->height = newH;
    pData->stride = newW * 4; // Our target stride
    
    if (stride == pData->stride) {
        pData->pixels.assign(output, output + dataSize);
    } else {
        // Copy row by row
        pData->pixels.resize(pData->stride * newH);
        for(int y=0; y<newH; ++y) {
            BYTE* src = output + y * stride;
            BYTE* dst = pData->pixels.data() + y * pData->stride;
            memcpy(dst, src, std::min(stride, pData->stride));
        }
    }
    
    pData->isValid = true;
    WebPFreeDecBuffer(&config.output);
    return S_OK;
}

HRESULT CImageLoader::LoadThumbJPEG(LPCWSTR filePath, int targetSize, ThumbData* pData) {
    if (!pData) return E_INVALIDARG;

    std::vector<uint8_t> jpegBuf;
    if (!ReadFileToVector(filePath, jpegBuf)) return E_FAIL;
    
    return LoadThumbJPEGFromMemory(jpegBuf.data(), jpegBuf.size(), targetSize, pData);
}

HRESULT CImageLoader::LoadThumbnail(LPCWSTR filePath, int targetSize, ThumbData* pData) {
    if (!pData) return E_INVALIDARG;
    pData->isValid = false;
    
    // Get file size (cheap)
    // Actually we iterate file path anyway. But if we read to vector, we know size.
    // If we use WIC, we might not know size unless we query file.
    // Let's use std::filesystem for non-vector paths logic
    // But most paths read vector.
    
    // Detect format
    std::wstring format = DetectFormatFromContent(filePath);

    // 1. Optimized Path (JPEG)
    if (format == L"JPEG") {
        if (SUCCEEDED(LoadThumbJPEG(filePath, targetSize, pData))) {
            return S_OK;
        }
    } else if (format == L"WebP") {
        // 1b. Optimized WebP (Scaled & Multithreaded)
        std::vector<uint8_t> buf;
        if (ReadFileToVector(filePath, buf)) {
            if (SUCCEEDED(LoadThumbWebPFromMemory(buf.data(), buf.size(), targetSize, pData))) {
                return S_OK;
            }
        }
    }
    
    // 1b. Optimized Extraction (RAW / HEIC / PSD) via PreviewExtractor
    // Check extension logic or format details?
    // DetectFormatFromContent usually returns "RAW (ARW)" or "HEIC".
    // Let's use extension check for speed first (since Detect might read file).
    // Actually LoadThumbnail passed filePath.
    
    std::wstring ext = filePath;
    size_t dot = ext.find_last_of(L'.');
    if (dot != std::wstring::npos) {
        std::wstring e = ext.substr(dot);
        std::transform(e.begin(), e.end(), e.begin(), ::towlower);
        
        bool tryExtract = false;
        bool isRaw = (e == L".arw" || e == L".cr2" || e == L".nef" || e == L".dng" || e == L".orf" || e == L".rw2" || e == L".raf");
        bool isHeic = (e == L".heic" || e == L".heif" || e == L".avif"); // AVIF might not have exif thumb, but worth try?
        bool isPsd = (e == L".psd" || e == L".psb");
        
        if (isRaw || isHeic || isPsd) {
            // Read file header/content
            // Note: RAW/PSD extraction might need significant portion of file.
            // For now, read whole file. (Memory Mapped would be better, but Read works).
            std::vector<uint8_t> buf;
            if (ReadFileToVector(filePath, buf)) {
                PreviewExtractor::ExtractedData exData;
                bool extracted = false;
                
                if (isRaw) extracted = PreviewExtractor::ExtractFromRAW(buf.data(), buf.size(), exData);
                else if (isHeic) extracted = PreviewExtractor::ExtractFromHEIC(buf.data(), buf.size(), exData);
                else if (isPsd) extracted = PreviewExtractor::ExtractFromPSD(buf.data(), buf.size(), exData);
                
                if (extracted && exData.IsValid()) {
                    // It's a JPEG buffer!
                    if (SUCCEEDED(LoadThumbJPEGFromMemory(exData.pData, exData.size, targetSize, pData))) {
                        // For extracted previews, we might not know the FULL RAW dimensions here unless parsed.
                        // However, preview dimensions (pData->origWidth) are set by LoadThumbJPEGFromMemory.
                        // That's the preview dimension, not RAW dimension.
                        // User might want RAW dimension.
                        // But getting RAW dimension requires parsing TIFF/HEIC headers fully.
                        // PreviewExtractor might have that info?
                        // For now, let's accept Preview Size or File Size.
                        // But File Size should be the RAW file size.
                        // buf.size() is the RAW file size.
                        pData->fileSize = buf.size();
                        return S_OK;
                    }
                }
            }
        }
    }

    // 2. Fallback Path (WIC Scaler for everything else)
    
    // Fail Fast: Check Dimensions to prevent OOM on massive files (e.g. 20k x 20k)
    // Only needed for fallback path which likely does full decode.
    {
        ComPtr<IWICBitmapDecoder> decoder;
        // Used GENERIC_READ. WIC Decoder is fast (reads header only).
        if (SUCCEEDED(m_wicFactory->CreateDecoderFromFilename(filePath, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder))) {
            ComPtr<IWICBitmapFrameDecode> frame;
            if (SUCCEEDED(decoder->GetFrame(0, &frame))) {
                UINT w = 0, h = 0;
                frame->GetSize(&w, &h);
                // Limit: 16384x16384 (268 MP). 
                // RGBA = 268 * 4 = 1GB per image. 
                // 1GB is too much for a thumbnail thread.
                if (w > 16384 || h > 16384) {
                     return E_OUTOFMEMORY;
                }
            }
        }
    }

    ComPtr<IWICBitmapSource> source;
    // Use LoadFromFile to leverage specialized loaders where possible (e.g. Wuffs for PNG)
    HRESULT hr = LoadFromFile(filePath, &source); 
    
    if (FAILED(hr) || !source) return hr;

    // Scale
    UINT origW, origH;
    source->GetSize(&origW, &origH);
    if (origW == 0 || origH == 0) return E_FAIL;

    // Compute ratio to fill targetSize (Center Crop style needs slightly larger coverage? 
    // Or just fit? Gallery uses Center Crop. So we should scale such that MIN(w,h) >= targetSize.
    // Let's ensure both dims >= targetSize if possible, or at least one matches?
    // Actually for center crop, we need the *smaller* dimension to be at least targetSize.
    // wait, if we request targetSize, we usually mean the cell size.
    // let's aim for the image covering targetSize x targetSize.
    // So scale factor = max(targetSize/w, targetSize/h).
    
    double ratio = std::max((double)targetSize / origW, (double)targetSize / origH);
    
    UINT newW, newH;
    if (ratio >= 1.0) {
        // Image is smaller than thumbnail slot? Keep original (don't upscale bloat)
        // Or upscale if needed? Let's just keep original effectively.
        // Actually WIC Scaler handles upscaling too.
        // For thumbnails, usually we are downscaling.
        // If image is tiny, maybe just use it.
        newW = origW; 
        newH = origH;
    } else {
        newW = (UINT)(origW * ratio);
        newH = (UINT)(origH * ratio);
        if (newW < 1) newW = 1;
        if (newH < 1) newH = 1;
    }
    
    // WIC Scaler
    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(m_wicFactory->CreateBitmapScaler(&scaler))) return E_FAIL;
    
    if (FAILED(scaler->Initialize(source.Get(), newW, newH, WICBitmapInterpolationModeFant))) return E_FAIL;

    // Format Converter (Ensure PBGRA/BGRA for D2D)
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(m_wicFactory->CreateFormatConverter(&converter))) return E_FAIL;
    
    // D2D prefers PBGRA usually, but we used BGRA for TurboJPEG. 
    // CreateBitmap functions usually accept both if specified correctly.
    // Let's use PBGRA (Premultiplied BGRA) which works best with D2D alpha blending.
    // TurboJPEG TJPF_BGRA is technically straight alpha (not premultiplied).
    // D2D supports IgnoreAlpha or StraightAlpha via different flags, but PBGRA is standard.
    // If JPG has no alpha, BGRA == PBGRA. So it's fine.
    // But PNG might have alpha. WIC converter to PBGRA handles premultiplication.
    
    if (FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.f, WICBitmapPaletteTypeMedianCut))) return E_FAIL;

    // Copy to raw buffer
    pData->width = newW;
    pData->height = newH;
    pData->stride = newW * 4;
    pData->pixels.resize(pData->stride * newH);

    hr = converter->CopyPixels(nullptr, pData->stride, (UINT)pData->pixels.size(), pData->pixels.data());
    hr = converter->CopyPixels(nullptr, pData->stride, (UINT)pData->pixels.size(), pData->pixels.data());
    if (SUCCEEDED(hr)) {
        pData->origWidth = origW;
        pData->origHeight = origH;
        // FileSize?
        // Check file manually
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesEx(filePath, GetFileExInfoStandard, &fad)) {
            pData->fileSize = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
        }
        pData->isValid = true;
    }
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

    // Manual Premultiplication (SIMD)
    SIMDUtils::PremultiplyAlpha_BGRA(output, width, height);

    HRESULT hr = CreateWICBitmapFromMemory(width, height, GUID_WICPixelFormat32bppPBGRA, stride, size, output, ppBitmap);
    
    // Extract format details (VP8 = Lossy, VP8L/Lossless Flag = Lossless)
    if (SUCCEEDED(hr)) {
        // Check if lossless format (VP8L)
        // WebP format: RIFF header + VP8/VP8L chunk
        // VP8L signature: 0x2F at offset 15 (after RIFF+WEBP headers)
        if (config.input.format == 2) { // VP8L = lossless
            g_lastFormatDetails = L"Lossless";
        } else {
            g_lastFormatDetails = L"Lossy";
        }
        if (config.input.has_alpha) g_lastFormatDetails += L" +Alpha";
    }
    
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
    unsigned int threads = std::thread::hardware_concurrency();
    if (threads > 0) {
        decoder->maxThreads = threads;
    } else {
        decoder->maxThreads = 4; // Fallback sensible default
    }
    
    // Disable strict flags to improve compatibility with non-compliant or experimental files
    decoder->strictFlags = AVIF_STRICT_DISABLED;
    
    // Set Memory Source
    if (avifDecoderSetIOMemory(decoder, avifBuf.data(), avifBuf.size()) != AVIF_RESULT_OK) {
        avifDecoderDestroy(decoder);
        return E_FAIL;
    }

    // Parse
    if (avifDecoderParse(decoder) != AVIF_RESULT_OK) {
        avifDecoderDestroy(decoder);
        return E_FAIL;
    }

    // Next Image (Frame 0)
    if (avifDecoderNextImage(decoder) != AVIF_RESULT_OK) {
        avifDecoderDestroy(decoder);
        return E_FAIL;
    }

    // Convert YUV to RGB
    avifRGBImage rgb;
    avifRGBImageSetDefaults(&rgb, decoder->image);
    
    // Configure for WIC (BGRA, 8-bit)
    rgb.format = AVIF_RGB_FORMAT_BGRA;
    rgb.depth = 8;
    rgb.alphaPremultiplied = AVIF_TRUE; // Re-enabled native premul as per user request
    
    // Calculate stride and size
    rgb.rowBytes = rgb.width * 4;
    std::vector<uint8_t> pixelData(rgb.rowBytes * rgb.height);
    rgb.pixels = pixelData.data();
    
    if (avifImageYUVToRGB(decoder->image, &rgb) != AVIF_RESULT_OK) {
        avifDecoderDestroy(decoder);
        return E_FAIL;
    }

    // Create WIC Bitmap
    HRESULT hr = CreateWICBitmapFromMemory(rgb.width, rgb.height, GUID_WICPixelFormat32bppPBGRA, (UINT)rgb.rowBytes, (UINT)pixelData.size(), pixelData.data(), ppBitmap);

    // Extract format details (bit depth)
    if (SUCCEEDED(hr)) {
        int depth = decoder->image->depth;
        wchar_t buf[32];
        swprintf_s(buf, L"%d-bit", depth);
        g_lastFormatDetails = buf;
        if (decoder->image->alphaPlane != nullptr) g_lastFormatDetails += L" +Alpha";
    }

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
HRESULT CImageLoader::LoadRaw(LPCWSTR filePath, IWICBitmap** ppBitmap, bool forceFullDecode) { 
    // Optimization: Try to load embedded JPEG preview first (FAST)
    // Fallback: Full RAW decode (SLOW)

    std::vector<uint8_t> rawBuf;
    if (!ReadFileToVector(filePath, rawBuf)) return E_FAIL;

    LibRaw RawProcessor;
    if (RawProcessor.open_buffer(rawBuf.data(), rawBuf.size()) != LIBRAW_SUCCESS) return E_FAIL;

    // 1. Try Unpack Thumbnail (Embedded Preview) - FASTEST
    if (!forceFullDecode && RawProcessor.unpack_thumb() == LIBRAW_SUCCESS) {
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

HRESULT CImageLoader::LoadToMemory(LPCWSTR filePath, IWICBitmap** ppBitmap, std::wstring* pLoaderName, bool forceFullDecode) {
    if (!filePath || !ppBitmap) return E_INVALIDARG;
    
    // Clear previous format details to avoid residue when switching formats
    g_lastFormatDetails.clear();
    
    std::wstring path = filePath;
    std::transform(path.begin(), path.end(), path.begin(), ::towlower);
    
    // -------------------------------------------------------------
    // Architecture Upgrade: Robust Format Detection & Fallback
    // -------------------------------------------------------------
    
    // 1. Detect Format
    std::wstring detectedFmt = DetectFormatFromContent(filePath);
    
    // -------------------------------------------------------------
    // Architecture Upgrade: Priority-Based Loading
    // -------------------------------------------------------------
    
    // 1. High-Performance Special Format Loaders (Bypass WIC)
    if (detectedFmt == L"JPEG") {
        HRESULT hr = LoadJPEG(filePath, ppBitmap);
        if (SUCCEEDED(hr)) { if (pLoaderName) *pLoaderName = L"libjpeg-turbo"; return S_OK; }
    }
    else if (detectedFmt == L"PNG") {
        HRESULT hr = LoadPngWuffs(filePath, ppBitmap); // Wuffs is faster/safer
        if (SUCCEEDED(hr)) { if (pLoaderName) *pLoaderName = L"Wuffs PNG"; return S_OK; }
    }
    else if (detectedFmt == L"WebP") {
        HRESULT hr = LoadWebP(filePath, ppBitmap);
        if (SUCCEEDED(hr)) { if (pLoaderName) *pLoaderName = L"libwebp"; return S_OK; }
    }
    else if (detectedFmt == L"AVIF" || 
             ((detectedFmt == L"HEIC" || detectedFmt == L"Unknown") && (path.ends_with(L".avif") || path.ends_with(L".avifs")))) {
        HRESULT hr = LoadAVIF(filePath, ppBitmap);
        if (SUCCEEDED(hr)) { if (pLoaderName) *pLoaderName = L"libavif"; return S_OK; }
    }
    else if (detectedFmt == L"JXL") {
        HRESULT hr = LoadJXL(filePath, ppBitmap);
        if (SUCCEEDED(hr)) { if (pLoaderName) *pLoaderName = L"libjxl"; return S_OK; }
    }
    else if (detectedFmt == L"GIF") {
        HRESULT hr = LoadGifWuffs(filePath, ppBitmap);
        if (SUCCEEDED(hr)) { if (pLoaderName) *pLoaderName = L"Wuffs GIF"; return S_OK; }
    }
    else if (detectedFmt == L"BMP") {
         HRESULT hr = LoadBmpWuffs(filePath, ppBitmap);
         if (SUCCEEDED(hr)) { if (pLoaderName) *pLoaderName = L"Wuffs BMP"; return S_OK; }
    }
    else if (detectedFmt == L"TGA") {
         HRESULT hr = LoadTgaWuffs(filePath, ppBitmap);
         if (SUCCEEDED(hr)) { if (pLoaderName) *pLoaderName = L"Wuffs TGA"; return S_OK; }
    }
     else if (detectedFmt == L"WBMP") {
         HRESULT hr = LoadWbmpWuffs(filePath, ppBitmap);
         if (SUCCEEDED(hr)) { if (pLoaderName) *pLoaderName = L"Wuffs WBMP"; return S_OK; }
    }
    else if (detectedFmt == L"PSD") {
        HRESULT hr = LoadStbImage(filePath, ppBitmap);
        if (SUCCEEDED(hr)) { if (pLoaderName) *pLoaderName = L"Stb Image (PSD)"; return S_OK; }
    }
    else if (detectedFmt == L"HDR") {
        HRESULT hr = LoadStbImage(filePath, ppBitmap, true); // float
        if (SUCCEEDED(hr)) { if (pLoaderName) *pLoaderName = L"Stb Image (HDR)"; return S_OK; }
    }
    else if (detectedFmt == L"PIC") {
         HRESULT hr = LoadStbImage(filePath, ppBitmap);
        if (SUCCEEDED(hr)) { if (pLoaderName) *pLoaderName = L"Stb Image (PIC)"; return S_OK; }
    }
    else if (detectedFmt == L"PNM") {
        // Try Wuffs first (safer), fallback to Stb? Not needed, Wuffs covers well.
        HRESULT hr = LoadNetpbmWuffs(filePath, ppBitmap);
        if (SUCCEEDED(hr)) { if (pLoaderName) *pLoaderName = L"Wuffs NetPBM"; return S_OK; }
    }
    else if (detectedFmt == L"EXR") {
        HRESULT hr = LoadTinyExrImage(filePath, ppBitmap);
        if (SUCCEEDED(hr)) { if (pLoaderName) *pLoaderName = L"TinyEXR"; return S_OK; }
    }
    else if (detectedFmt == L"SVG") {
        HRESULT hr = LoadSVG(filePath, ppBitmap);
        if (SUCCEEDED(hr)) { if (pLoaderName) *pLoaderName = L"NanoSVG"; return S_OK; }
    }
    else if (detectedFmt == L"QOI") {
         HRESULT hr = LoadQoiWuffs(filePath, ppBitmap);
         if (SUCCEEDED(hr)) { if (pLoaderName) *pLoaderName = L"Wuffs QOI"; return S_OK; }
    }
    else if (detectedFmt == L"PCX") {
         HRESULT hr = LoadPCX(filePath, ppBitmap);
         if (SUCCEEDED(hr)) { if (pLoaderName) *pLoaderName = L"Custom PCX"; return S_OK; }
    }
    

    
    // 2. Handle TIFF (CR2, NEF, ARW often identify as TIFF via Magic Bytes)
    else if (detectedFmt == L"TIFF") {
         // Check if it's actually a RAW file based on extension
         if (path.ends_with(L".arw") || path.ends_with(L".cr2") || path.ends_with(L".nef") || 
             path.ends_with(L".dng") || path.ends_with(L".orf") || path.ends_with(L".rw2") || 
             path.ends_with(L".raf") || path.ends_with(L".pef") || path.ends_with(L".srw") || path.ends_with(L".cr3") || path.ends_with(L".nrw")) {
             HRESULT hr = LoadRaw(filePath, ppBitmap, forceFullDecode);
             if (SUCCEEDED(hr)) { 
                 if (pLoaderName) *pLoaderName = forceFullDecode ? L"LibRaw (Full)" : L"LibRaw (Preview)"; 
                 return S_OK; 
             }
         }
         // If no RAW extension, fall through to WIC (Standard TIFF)
    }

    // RAW Check (no magic bytes usually reliable OR falls into Unknown)
    if (detectedFmt == L"Unknown") { 
         // Check extension
         if (path.ends_with(L".arw") || path.ends_with(L".cr2") || path.ends_with(L".nef") || 
             path.ends_with(L".dng") || path.ends_with(L".orf") || path.ends_with(L".rw2") || 
             path.ends_with(L".raf") || path.ends_with(L".pef") || path.ends_with(L".srw") || path.ends_with(L".cr3") || path.ends_with(L".nrw")) {
             HRESULT hr = LoadRaw(filePath, ppBitmap, forceFullDecode);
             if (SUCCEEDED(hr)) { 
                 if (pLoaderName) *pLoaderName = forceFullDecode ? L"LibRaw (Full)" : L"LibRaw (Preview)"; 
                 return S_OK; 
             }
         }
         else if (path.ends_with(L".svg")) {
             HRESULT hr = LoadSVG(filePath, ppBitmap);
             if (SUCCEEDED(hr)) { if (pLoaderName) *pLoaderName = L"NanoSVG"; return S_OK; }
         }
         else if (path.ends_with(L".tga")) {
             HRESULT hr = LoadTgaWuffs(filePath, ppBitmap);
             if (SUCCEEDED(hr)) { if (pLoaderName) *pLoaderName = L"Wuffs TGA"; return S_OK; }
         }
         else if (path.ends_with(L".avif")) {
             HRESULT hr = LoadAVIF(filePath, ppBitmap);
             if (SUCCEEDED(hr)) { if (pLoaderName) *pLoaderName = L"libavif"; return S_OK; }
         }
         else if (path.ends_with(L".hdr") || path.ends_with(L".pic")) {
             HRESULT hr = LoadStbImage(filePath, ppBitmap, true);
             if (SUCCEEDED(hr)) { if (pLoaderName) *pLoaderName = L"Stb Image (HDR)"; return S_OK; }
         }
         else if (path.ends_with(L".exr")) {
             HRESULT hr = LoadTinyExrImage(filePath, ppBitmap);
             if (SUCCEEDED(hr)) { if (pLoaderName) *pLoaderName = L"TinyEXR"; return S_OK; }
         }
         else if (path.ends_with(L".psd")) {
             HRESULT hr = LoadStbImage(filePath, ppBitmap);
             if (SUCCEEDED(hr)) { if (pLoaderName) *pLoaderName = L"Stb Image (PSD)"; return S_OK; }
         }
    }


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
    HRESULT hr = m_wicFactory->CreateDecoderFromFilename(
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

std::wstring CImageLoader::GetLastFormatDetails() const {
    return g_lastFormatDetails;
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

// ----------------------------------------------------------------------------
// Wuffs BMP Decoder
// ----------------------------------------------------------------------------
HRESULT CImageLoader::LoadBmpWuffs(LPCWSTR filePath, IWICBitmap** ppBitmap) {
    std::vector<uint8_t> buf;
    if (!ReadFileToVector(filePath, buf)) return E_FAIL;

    uint32_t width = 0, height = 0;
    std::vector<uint8_t> pixelData;

    if (!WuffsLoader::DecodeBMP(buf.data(), buf.size(), &width, &height, pixelData)) {
        return E_FAIL;
    }

    size_t stride = width * 4;
    return CreateWICBitmapFromMemory(width, height, GUID_WICPixelFormat32bppBGRA,
                                      (UINT)stride, (UINT)pixelData.size(), pixelData.data(), ppBitmap);
}

// ----------------------------------------------------------------------------
// Wuffs TGA Decoder
// ----------------------------------------------------------------------------
HRESULT CImageLoader::LoadTgaWuffs(LPCWSTR filePath, IWICBitmap** ppBitmap) {
    std::vector<uint8_t> buf;
    if (!ReadFileToVector(filePath, buf)) return E_FAIL;

    uint32_t width = 0, height = 0;
    std::vector<uint8_t> pixelData;

    if (!WuffsLoader::DecodeTGA(buf.data(), buf.size(), &width, &height, pixelData)) {
        return E_FAIL;
    }

    size_t stride = width * 4;
    return CreateWICBitmapFromMemory(width, height, GUID_WICPixelFormat32bppBGRA,
                                      (UINT)stride, (UINT)pixelData.size(), pixelData.data(), ppBitmap);
}

// ----------------------------------------------------------------------------
// Wuffs WBMP Decoder
// ----------------------------------------------------------------------------
HRESULT CImageLoader::LoadWbmpWuffs(LPCWSTR filePath, IWICBitmap** ppBitmap) {
    std::vector<uint8_t> buf;
    if (!ReadFileToVector(filePath, buf)) return E_FAIL;

    uint32_t width = 0, height = 0;
    std::vector<uint8_t> pixelData;

    if (!WuffsLoader::DecodeWBMP(buf.data(), buf.size(), &width, &height, pixelData)) {
        return E_FAIL;
    }

    size_t stride = width * 4;
    return CreateWICBitmapFromMemory(width, height, GUID_WICPixelFormat32bppBGRA,
                                      (UINT)stride, (UINT)pixelData.size(), pixelData.data(), ppBitmap);
}

// ----------------------------------------------------------------------------
// Stb Image Decoder (PSD, HDR, PIC, PNM)
// ----------------------------------------------------------------------------
HRESULT CImageLoader::LoadStbImage(LPCWSTR filePath, IWICBitmap** ppBitmap, bool floatFormat) {
    // Read file to memory first (Solves Windows Unicode Path issues reliably)
    std::vector<uint8_t> buf;
    if (!ReadFileToVector(filePath, buf)) return E_FAIL;

    int width = 0, height = 0, channels = 0;
    std::vector<uint8_t> pixelData;
    
    // Use Memory Loader
    if (!StbLoader::LoadImageFromMemory(buf.data(), buf.size(), &width, &height, &channels, pixelData, floatFormat)) {
        return E_FAIL;
    }

    if (floatFormat) {
        // HDR: Create float bitmap (128bpp RGBA Float)
        size_t stride = width * 4 * sizeof(float);
        return CreateWICBitmapFromMemory(width, height, GUID_WICPixelFormat128bppRGBAFloat,
                                          (UINT)stride, (UINT)pixelData.size(), (BYTE*)pixelData.data(), ppBitmap);
    } else {
        // Standard 8-bit RGBA
        // WIC prefers BGRA for 32bpp
        // Swap R and B
        uint8_t* p = pixelData.data();
        size_t pixelCount = (size_t)width * height;
        for (size_t i = 0; i < pixelCount; i++) {
            std::swap(p[i*4], p[i*4+2]); // Swap R and B
        }
        
        size_t stride = width * 4;
        return CreateWICBitmapFromMemory(width, height, GUID_WICPixelFormat32bppBGRA,
                                          (UINT)stride, (UINT)pixelData.size(), pixelData.data(), ppBitmap);
    }
}

// ----------------------------------------------------------------------------
// TinyEXR Decoder
// ----------------------------------------------------------------------------
HRESULT CImageLoader::LoadTinyExrImage(LPCWSTR filePath, IWICBitmap** ppBitmap) {
    // Read file to memory (Solves Path and Locking issues)
    std::vector<uint8_t> buf;
    if (!ReadFileToVector(filePath, buf)) return E_FAIL;

    int width = 0, height = 0;
    std::vector<float> pixelData;

    // Use Memory Loader
    if (!TinyExrLoader::LoadEXRFromMemory(buf.data(), buf.size(), &width, &height, pixelData)) {
         return E_FAIL;
    }

    // TinyEXR returns RGBA floats.
    // Create WIC Bitmap (128bpp RGBA Float)
    size_t stride = width * 4 * sizeof(float);
    // Note: pixelData.size() is count of floats. Size in bytes is size() * sizeof(float).
    return CreateWICBitmapFromMemory(width, height, GUID_WICPixelFormat128bppRGBAFloat,
                                      (UINT)stride, (UINT)(pixelData.size() * sizeof(float)), (BYTE*)pixelData.data(), ppBitmap);
}

// ----------------------------------------------------------------------------
// NanoSVG Decoder (SVG Support)
// ----------------------------------------------------------------------------
HRESULT CImageLoader::LoadSVG(LPCWSTR filePath, IWICBitmap** ppBitmap) {
    std::vector<uint8_t> fileData;
    if (!ReadFileToVector(filePath, fileData)) return E_FAIL;

    // NanoSVG parses char* string (null terminated recommended)
    std::vector<char> xmlData(fileData.begin(), fileData.end());
    xmlData.push_back('\0'); 

    // Parse (96 DPI default units)
    // Note: nsvgParse modifies the input string content during parsing (destructive)
    NSVGimage* image = nsvgParse(xmlData.data(), "px", 96.0f);
    if (!image) return E_FAIL;

    // Scale Logic: 2.0x for crisp rendering
    float scale = 2.0f; 
    
    // Safety size limit (e.g. 8k)
    float maxDim = 8192.0f;
    if (image->width * scale > maxDim || image->height * scale > maxDim) {
         float aspect = image->width / image->height;
         if (aspect > 1.0f) {
             scale = maxDim / image->width;
         } else {
             scale = maxDim / image->height;
         }
    }

    int width = (int)(image->width * scale);
    int height = (int)(image->height * scale);
    
    if (width <= 0 || height <= 0) {
        nsvgDelete(image);
        return E_FAIL;
    }

    // Rasterize
    NSVGrasterizer* rast = nsvgCreateRasterizer();
    if (!rast) {
        nsvgDelete(image);
        return E_OUTOFMEMORY;
    }

    // NanoSVG generates RGBA (32-bit)
    size_t stride = width * 4;
    size_t size = stride * height;
    std::vector<uint8_t> imgData(size);

    nsvgRasterize(rast, image, 0, 0, scale, imgData.data(), width, height, (int)stride);

    nsvgDeleteRasterizer(rast);
    nsvgDelete(image);

    // Create WIC Bitmap (GUID_WICPixelFormat32bppRGBA)
    return CreateWICBitmapFromMemory(width, height, GUID_WICPixelFormat32bppRGBA,
                                     (UINT)stride, (UINT)size, imgData.data(), ppBitmap);
}

// ----------------------------------------------------------------------------
// Wuffs NetPBM (PAM, PBM, PGM, PPM)
// ----------------------------------------------------------------------------
HRESULT CImageLoader::LoadNetpbmWuffs(LPCWSTR filePath, IWICBitmap** ppBitmap) {
    std::vector<uint8_t> buf;
    if (!ReadFileToVector(filePath, buf)) return E_FAIL;

    uint32_t width = 0, height = 0;
    std::vector<uint8_t> pixelData;

    if (!WuffsLoader::DecodeNetpbm(buf.data(), buf.size(), &width, &height, pixelData)) {
        return E_FAIL;
    }

    size_t stride = width * 4;
    return CreateWICBitmapFromMemory(width, height, GUID_WICPixelFormat32bppBGRA,
                                     (UINT)stride, (UINT)(stride * height), pixelData.data(), ppBitmap);
}

// ----------------------------------------------------------------------------
// Wuffs QOI (Quite OK Image)
// ----------------------------------------------------------------------------
HRESULT CImageLoader::LoadQoiWuffs(LPCWSTR filePath, IWICBitmap** ppBitmap) {
    std::vector<uint8_t> buf;
    if (!ReadFileToVector(filePath, buf)) return E_FAIL;

    uint32_t width = 0, height = 0;
    std::vector<uint8_t> pixelData;

    if (!WuffsLoader::DecodeQOI(buf.data(), buf.size(), &width, &height, pixelData)) {
        return E_FAIL;
    }

    size_t stride = width * 4;
    return CreateWICBitmapFromMemory(width, height, GUID_WICPixelFormat32bppBGRA,
                                     (UINT)stride, (UINT)(stride * height), pixelData.data(), ppBitmap);
}

// ----------------------------------------------------------------------------
// Custom PCX Decoder (Since stb_image lacks support)
// ----------------------------------------------------------------------------
#pragma pack(push, 1)
struct PCXHeader {
    uint8_t manufacturer; // 0x0A
    uint8_t version;
    uint8_t encoding;     // 1 = RLE
    uint8_t bitsPerPixel;
    uint16_t xmin, ymin, xmax, ymax;
    uint16_t hDpi, vDpi;
    uint8_t palette[48];
    uint8_t reserved;
    uint8_t colorPlanes;
    uint16_t bytesPerLine;
    uint16_t paletteType;
    uint16_t hScreenSize, vScreenSize;
    uint8_t filler[54];
};
#pragma pack(pop)

HRESULT CImageLoader::LoadPCX(LPCWSTR filePath, IWICBitmap** ppBitmap) {
    std::vector<uint8_t> data;
    if (!ReadFileToVector(filePath, data)) return E_FAIL;

    if (data.size() < sizeof(PCXHeader)) return E_FAIL;

    const PCXHeader* header = (const PCXHeader*)data.data();
    if (header->manufacturer != 0x0A) return E_FAIL;
    if (header->encoding != 1) return E_FAIL; // Only RLE supported

    int width = header->xmax - header->xmin + 1;
    int height = header->ymax - header->ymin + 1;
    if (width <= 0 || height <= 0) return E_FAIL;
    if (width > 32768 || height > 32768) return E_FAIL;

    // Decode RLE
    size_t scanlineBytes = header->bytesPerLine * header->colorPlanes;
    std::vector<uint8_t> decodedBytes(scanlineBytes * height);
    uint8_t* dst = decodedBytes.data();
    uint8_t* dstEnd = dst + decodedBytes.size();

    const uint8_t* ptr = data.data() + 128;
    const uint8_t* end = data.data() + data.size();

    while (dst < dstEnd && ptr < end) {
        uint8_t byte = *ptr++;
        if ((byte & 0xC0) == 0xC0) {
            uint8_t count = byte & 0x3F;
            if (ptr >= end) break;
            uint8_t val = *ptr++;
            if (dst + count > dstEnd) count = (uint8_t)(dstEnd - dst);
            memset(dst, val, count);
            dst += count;
        } else {
            *dst++ = byte;
        }
    }

    // Convert to RGBA
    std::vector<uint8_t> rgba(width * height * 4);
    
    if (header->colorPlanes == 3 && header->bitsPerPixel == 8) {
        // 24-bit RGB
        for (int y = 0; y < height; y++) {
            uint8_t* rowSrc = decodedBytes.data() + y * scanlineBytes;
            uint8_t* rSrc = rowSrc;
            uint8_t* gSrc = rowSrc + header->bytesPerLine;
            uint8_t* bSrc = rowSrc + header->bytesPerLine * 2;
            uint8_t* dstRow = rgba.data() + y * width * 4;

            for (int x = 0; x < width; x++) {
                if (x < header->bytesPerLine) {
                    dstRow[x * 4 + 0] = rSrc[x];
                    dstRow[x * 4 + 1] = gSrc[x];
                    dstRow[x * 4 + 2] = bSrc[x];
                    dstRow[x * 4 + 3] = 255;
                }
            }
        }
    } else if (header->colorPlanes == 1 && header->bitsPerPixel == 8) {
        // 256 color palette (at end of file)
        if (data.size() < 769) return E_FAIL;
        const uint8_t* palPtr = data.data() + data.size() - 768;
        
        for (int y = 0; y < height; y++) {
            uint8_t* rowSrc = decodedBytes.data() + y * scanlineBytes;
            uint8_t* dstRow = rgba.data() + y * width * 4;
            for (int x = 0; x < width; x++) {
                if (x < header->bytesPerLine) {
                    uint8_t idx = rowSrc[x];
                    dstRow[x * 4 + 0] = palPtr[idx * 3 + 0];
                    dstRow[x * 4 + 1] = palPtr[idx * 3 + 1];
                    dstRow[x * 4 + 2] = palPtr[idx * 3 + 2];
                    dstRow[x * 4 + 3] = 255;
                }
            }
        }
    } else {
        return E_FAIL; // Unsupported format
    }

    return CreateWICBitmapFromMemory(width, height, GUID_WICPixelFormat32bppRGBA,
                                     width * 4, width * height * 4, rgba.data(), ppBitmap);
}

// ----------------------------------------------------------------------------
// Metadata Implementation
// ----------------------------------------------------------------------------

#include <propsys.h>
#include <propvarutil.h>
#include <cmath>
#pragma comment(lib, "propsys.lib")

static HRESULT GetMetadataString(IWICMetadataQueryReader* reader, LPCWSTR query, std::wstring& out) {
    if (!reader) return E_INVALIDARG;
    PROPVARIANT val; PropVariantInit(&val);
    HRESULT hr = reader->GetMetadataByName(query, &val);
    if (SUCCEEDED(hr)) {
        WCHAR buf[512] = {};
        if (SUCCEEDED(PropVariantToString(val, buf, 512))) {
             out = buf;
        }
        PropVariantClear(&val);
    }
    return hr;
}

static double DecodeRational(unsigned __int64 val) {
    uint32_t num = (uint32_t)(val & 0xFFFFFFFF);
    uint32_t den = (uint32_t)(val >> 32);
    if (den == 0) return 0.0;
    return (double)num / (double)den;
}

static double DecodeSignedRational(unsigned __int64 val) {
    // EXIF SRATIONAL: Low 32 bits = signed numerator, High 32 bits = signed denominator
    int32_t num = (int32_t)(val & 0xFFFFFFFF);
    int32_t den = (int32_t)(val >> 32);
    if (den == 0) return 0.0;
    return (double)num / (double)den;
}

static HRESULT GetMetadataSignedRational(IWICMetadataQueryReader* reader, LPCWSTR query, double* out) {
    if (!reader || !out) return E_INVALIDARG;
    PROPVARIANT val; PropVariantInit(&val);
    HRESULT hr = reader->GetMetadataByName(query, &val);
    if (SUCCEEDED(hr)) {
        if (val.vt == VT_UI8) {
            *out = DecodeSignedRational(val.uhVal.QuadPart);
        } else if (val.vt == VT_I8) {
            *out = DecodeSignedRational((unsigned __int64)val.hVal.QuadPart);
        } else if (val.vt == VT_I4) {
            *out = (double)val.lVal; // Already a ratio?
        } else if (val.vt == VT_R8) {
            *out = val.dblVal;
        } else {
            // Fallback: Try standard conversion but it may fail for RATIONAL
            hr = E_FAIL; // Skip if unknown type
        }
        PropVariantClear(&val);
    }
    return hr;
}

static HRESULT GetMetadataRational(IWICMetadataQueryReader* reader, LPCWSTR query, double* out) {
    if (!reader || !out) return E_INVALIDARG;
    PROPVARIANT val; PropVariantInit(&val);
    HRESULT hr = reader->GetMetadataByName(query, &val);
    if (SUCCEEDED(hr)) {
        if (val.vt == VT_UI8) {
            *out = DecodeRational(val.uhVal.QuadPart);
        } else {
            PropVariantToDouble(val, out);
        }
        PropVariantClear(&val);
    }
    return hr;
}



static HRESULT GetMetadataGPS(IWICMetadataQueryReader* reader, LPCWSTR coordQuery, LPCWSTR refQuery, double* outVal) {
    if (!reader || !outVal) return E_INVALIDARG;
    
    // 1. Read Reference (N/S/E/W)
    WCHAR refBuf[16] = {};
    PROPVARIANT varRef; PropVariantInit(&varRef);
    if (SUCCEEDED(reader->GetMetadataByName(refQuery, &varRef))) {
        PropVariantToString(varRef, refBuf, 16);
        PropVariantClear(&varRef);
    }
    
    // 2. Read Coordinate (Vector of 3 UI8s)
    PROPVARIANT varCoord; PropVariantInit(&varCoord);
    HRESULT hr = reader->GetMetadataByName(coordQuery, &varCoord);
    if (FAILED(hr)) return hr;
    
    double result = 0.0;
    
    if (varCoord.vt == (VT_UI8 | VT_VECTOR)) {
        if (varCoord.cauh.cElems == 3) {
            double deg = DecodeRational(varCoord.cauh.pElems[0].QuadPart);
            double min = DecodeRational(varCoord.cauh.pElems[1].QuadPart);
            double sec = DecodeRational(varCoord.cauh.pElems[2].QuadPart);
            result = deg + min/60.0 + sec/3600.0;
        }
    }
    PropVariantClear(&varCoord);
    
    // Apply Ref (S or W means negative)
    if (refBuf[0] == 'S' || refBuf[0] == 's' || refBuf[0] == 'W' || refBuf[0] == 'w') {
        result = -result;
    }
    
    *outVal = result;
    return S_OK;
}

HRESULT CImageLoader::ReadMetadata(LPCWSTR filePath, ImageMetadata* pMetadata) {
    if (!filePath || !pMetadata) return E_INVALIDARG;
    *pMetadata = {}; // Clear

    if (!m_wicFactory) return E_FAIL;

    // 1. Detect Format (New robust logic)
    pMetadata->Format = DetectFormatFromContent(filePath);
    
    // 2. Create Decoder based on file (Safe fallback)
    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = m_wicFactory->CreateDecoderFromFilename(filePath, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
    
    if (FAILED(hr)) {
        // If WIC failed but we detected a format (e.g. JXL, Custom RAW), return S_OK so we at least have the Format field
        if (pMetadata->Format != L"Unknown") return S_OK;
        return hr; 
    }

    // WIC Format check (Secondary verification or detail)
    // If our magic byte detector returned generic or WIC has better info?
    // Actually our detector is better for JXL/AVIF/WebP often. 
    // Just keep the Magic Byte result as primary.
    /* 
    GUID containerFormat;
    if (SUCCEEDED(decoder->GetContainerFormat(&containerFormat))) {
        // ... WIC detection fallback if needed ...
    }
    */

    // 2. Get Frame 0
    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) return S_FALSE;

    // 3. Basic Info
    frame->GetSize(&pMetadata->Width, &pMetadata->Height);
    
    // File Size & Time Fallback
    HANDLE hFile = CreateFileW(filePath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size;
        if (GetFileSizeEx(hFile, &size)) pMetadata->FileSize = size.QuadPart;
        
        // Get File Modify Time (Fallback for Date)
        FILETIME ftWrite;
        if (GetFileTime(hFile, nullptr, nullptr, &ftWrite)) {
             SYSTEMTIME st; FileTimeToSystemTime(&ftWrite, &st);
             wchar_t buf[64];
             swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
             // We store it, but if EXIF exists, we overwrite
             pMetadata->Date = buf;
        }
        CloseHandle(hFile);
    }

    // 4. Metadata Query
    ComPtr<IWICMetadataQueryReader> reader;
    if (FAILED(frame->GetMetadataQueryReader(&reader))) return S_OK;

    // 5. Read Tags
    // 5. Read Tags (Try multiple paths for robustness)
    const wchar_t* makeQueries[] = { L"/app1/ifd/{ushort=271}", L"/ifd/{ushort=271}" };
    for (auto q : makeQueries) if (SUCCEEDED(GetMetadataString(reader.Get(), q, pMetadata->Make)) && !pMetadata->Make.empty()) break;

    const wchar_t* modelQueries[] = { L"/app1/ifd/{ushort=272}", L"/ifd/{ushort=272}" };
    for (auto q : modelQueries) if (SUCCEEDED(GetMetadataString(reader.Get(), q, pMetadata->Model)) && !pMetadata->Model.empty()) break;

    const wchar_t* dateQueries[] = { L"/app1/ifd/exif/{ushort=36867}", L"/app1/ifd/{ushort=306}", L"/ifd/exif/{ushort=36867}", L"/ifd/{ushort=306}" };
    for (auto q : dateQueries) if (SUCCEEDED(GetMetadataString(reader.Get(), q, pMetadata->Date)) && !pMetadata->Date.empty()) break;

    // ISO (34855)
    std::wstring iso;
    const wchar_t* isoQueries[] = { L"/app1/ifd/exif/{ushort=34855}", L"/ifd/exif/{ushort=34855}", L"/app1/ifd/{ushort=34855}" };
    for (auto q : isoQueries) {
        if (SUCCEEDED(GetMetadataString(reader.Get(), q, iso)) && !iso.empty()) {
            pMetadata->ISO = iso; break;
        }
    }

    // Aperture (FNumber 33437)
    double fnum = 0.0;
    const wchar_t* fnumQueries[] = { L"/app1/ifd/exif/{ushort=33437}", L"/ifd/exif/{ushort=33437}", L"/app1/ifd/{ushort=33437}" };
    for (auto q : fnumQueries) {
        if (SUCCEEDED(GetMetadataRational(reader.Get(), q, &fnum))) {
             wchar_t buf[32]; swprintf_s(buf, L"f/%.1f", fnum);
             pMetadata->Aperture = buf;
             break;
        }
    }

    // ExposureTime (33434)
    double expTime = 0.0;
    const wchar_t* expQueries[] = { L"/app1/ifd/exif/{ushort=33434}", L"/ifd/exif/{ushort=33434}", L"/app1/ifd/{ushort=33434}" };
    for (auto q : expQueries) {
        if (SUCCEEDED(GetMetadataRational(reader.Get(), q, &expTime))) {
            if (expTime > 0) {
                if (expTime < 1.0) {
                    wchar_t buf[32]; swprintf_s(buf, L"1/%.0f", 1.0 / expTime);
                    pMetadata->Shutter = buf;
                } else {
                     wchar_t buf[32]; swprintf_s(buf, L"%.1f\"", expTime);
                     pMetadata->Shutter = buf;
                }
            }
            break;
        }
    }

    // FocalLength (37386)
    double focal = 0.0;
    const wchar_t* focalQueries[] = { L"/app1/ifd/exif/{ushort=37386}", L"/ifd/exif/{ushort=37386}", L"/app1/ifd/{ushort=37386}" };
    for (auto q : focalQueries) {
        if (SUCCEEDED(GetMetadataRational(reader.Get(), q, &focal))) {
             wchar_t buf[64];
             // Try to get 35mm equivalent (0xa405)
             double focal35 = 0.0;
             const wchar_t* focal35Queries[] = { L"/app1/ifd/exif/{ushort=41989}", L"/ifd/exif/{ushort=41989}" };
             for (auto q35 : focal35Queries) {
                 if (SUCCEEDED(GetMetadataRational(reader.Get(), q35, &focal35)) && focal35 > 0) break;
             }
             if (focal35 > 0) {
                 swprintf_s(buf, L"%.0fmm (%.0fmm)", focal, focal35);
             } else {
                 swprintf_s(buf, L"%.0fmm", focal);
             }
             pMetadata->Focal = buf;
             break;
        }
    }
    
    // Lens Model (42036 or 0xA434)
    const wchar_t* lensQueries[] = { L"/app1/ifd/exif/{ushort=42036}", L"/ifd/exif/{ushort=42036}" };
    for (auto q : lensQueries) if (SUCCEEDED(GetMetadataString(reader.Get(), q, pMetadata->Lens)) && !pMetadata->Lens.empty()) break;

    // Software (305) - Try multiple paths
    const wchar_t* softQueries[] = { L"/app1/ifd/{ushort=305}", L"/ifd/{ushort=305}" };
    for (auto q : softQueries) if (SUCCEEDED(GetMetadataString(reader.Get(), q, pMetadata->Software)) && !pMetadata->Software.empty()) break;

    // ColorSpace Detection:
    // 1. Try to get ICC Profile embedded in image for accurate color space name
    // 2. Fall back to EXIF ColorSpace tag (0xa001)
    
    // Try ICC Profile first - Get color context from frame
    ComPtr<IWICBitmapFrameDecode> iccFrame;
    if (SUCCEEDED(decoder->GetFrame(0, &iccFrame))) {
        ComPtr<IWICColorContext> colorContext;
        if (SUCCEEDED(m_wicFactory->CreateColorContext(&colorContext))) {
            UINT count = 0;
            IWICColorContext* contexts[1] = { colorContext.Get() };
            hr = iccFrame->GetColorContexts(1, contexts, &count);
            if (SUCCEEDED(hr) && count > 0) {
                // Get profile bytes and parse description
                UINT profileSize = 0;
                colorContext->GetProfileBytes(0, nullptr, &profileSize);
                if (profileSize > 128) { // ICC profile header is 128 bytes
                    std::vector<BYTE> profileData(profileSize);
                    if (SUCCEEDED(colorContext->GetProfileBytes(profileSize, profileData.data(), &profileSize))) {
                        // ICC Profile structure: 
                        // Offset 12-15: Profile/Device class (signature)
                        // We need to find 'desc' tag for profile description
                        // Tag table starts at offset 128
                        UINT tagCount = (profileData[128] << 24) | (profileData[129] << 16) | (profileData[130] << 8) | profileData[131];
                        for (UINT i = 0; i < tagCount && i < 50; i++) {
                            UINT tagOffset = 132 + i * 12;
                            if (tagOffset + 12 > profileSize) break;
                            // Check for 'desc' tag signature
                            if (profileData[tagOffset] == 'd' && profileData[tagOffset+1] == 'e' && 
                                profileData[tagOffset+2] == 's' && profileData[tagOffset+3] == 'c') {
                                UINT descOffset = (profileData[tagOffset+4] << 24) | (profileData[tagOffset+5] << 16) | 
                                                  (profileData[tagOffset+6] << 8) | profileData[tagOffset+7];
                                UINT descSize = (profileData[tagOffset+8] << 24) | (profileData[tagOffset+9] << 16) | 
                                                (profileData[tagOffset+10] << 8) | profileData[tagOffset+11];
                                if (descOffset + 12 < profileSize && descSize > 12) {
                                    // Skip type signature (4 bytes) and reserved (4 bytes)
                                    // ASCII count at offset+8
                                    UINT strLen = (profileData[descOffset+8] << 24) | (profileData[descOffset+9] << 16) |
                                                  (profileData[descOffset+10] << 8) | profileData[descOffset+11];
                                    if (strLen > 0 && strLen < 128 && descOffset + 12 + strLen <= profileSize) {
                                        std::string desc((char*)&profileData[descOffset + 12], strLen - 1);
                                        // Common profiles
                                        if (desc.find("Display P3") != std::string::npos) pMetadata->ColorSpace = L"Display P3";
                                        else if (desc.find("sRGB") != std::string::npos) pMetadata->ColorSpace = L"sRGB";
                                        else if (desc.find("Adobe RGB") != std::string::npos) pMetadata->ColorSpace = L"Adobe RGB";
                                        else if (desc.find("DCI-P3") != std::string::npos) pMetadata->ColorSpace = L"DCI-P3";
                                        else if (desc.find("Rec. 2020") != std::string::npos || desc.find("BT.2020") != std::string::npos) pMetadata->ColorSpace = L"Rec.2020";
                                        else {
                                            // Use first 20 chars of description
                                            std::wstring wdesc(desc.begin(), desc.end());
                                            if (wdesc.length() > 20) wdesc = wdesc.substr(0, 20) + L"...";
                                            pMetadata->ColorSpace = wdesc;
                                        }
                                    }
                                }
                                break;
                            }
                        }
                    }
                }
            }
        }
    }
    
    // Fallback to EXIF ColorSpace if ICC not found
    if (pMetadata->ColorSpace.empty()) {
        PROPVARIANT csVar; PropVariantInit(&csVar);
        const wchar_t* csQueries[] = { L"/app1/ifd/exif/{ushort=40961}", L"/ifd/exif/{ushort=40961}" };
        for (auto q : csQueries) {
            if (SUCCEEDED(reader->GetMetadataByName(q, &csVar))) {
                UINT csVal = 0;
                if (csVar.vt == VT_UI2) csVal = csVar.uiVal;
                else if (csVar.vt == VT_UI4) csVal = csVar.ulVal;
                
                if (csVal == 1) pMetadata->ColorSpace = L"sRGB";
                else if (csVal == 2) pMetadata->ColorSpace = L"Adobe RGB";
                else if (csVal == 65535) pMetadata->ColorSpace = L"Wide Gamut";
                PropVariantClear(&csVar);
                break;
            }
        }
    }

    // Exposure Bias (37380) - Signed Rational
    double bias = 0.0;
    const wchar_t* biasQueries[] = { L"/app1/ifd/exif/{ushort=37380}", L"/ifd/exif/{ushort=37380}" };
    for (auto q : biasQueries) if (SUCCEEDED(GetMetadataSignedRational(reader.Get(), q, &bias))) {
        if (std::abs(bias) > 0.0001) { // Check non-zero with epsilon
            wchar_t buf[32]; swprintf_s(buf, L"%+.1f ev", bias);
            pMetadata->ExposureBias = buf;
        }
        break;
    }

    // Flash (37377)
    PROPVARIANT flashVar; PropVariantInit(&flashVar);
    const wchar_t* flashQueries[] = { L"/app1/ifd/exif/{ushort=37377}", L"/ifd/exif/{ushort=37377}" };
    for (auto q : flashQueries) {
        if (SUCCEEDED(reader->GetMetadataByName(q, &flashVar))) {
            UINT flashVal = 0;
            if (flashVar.vt == VT_UI2) flashVal = flashVar.uiVal;
            else if (flashVar.vt == VT_UI4) flashVal = flashVar.ulVal;
            
            pMetadata->Flash = (flashVal & 1) ? L"Flash: On" : L"Flash: Off"; // Simple logic
            if ((flashVal & 1) == 0) pMetadata->Flash = L""; // Don't show if Off? Or show "Flash: Off"? User said "Whether there is flash". "Off" is info.
            // But usually clean UI hides "Off". "Windows Photo" shows it.
            // I'll show it if it was explicitly recorded.
            PropVariantClear(&flashVar);
            break;
        }
    }

    // GPS
    double lat = 0, lon = 0;
    bool foundGPS = false;
    
    // Block 1: Try /app1/ifd/gps/...
    if (SUCCEEDED(GetMetadataGPS(reader.Get(), L"/app1/ifd/gps/{ushort=2}", L"/app1/ifd/gps/{ushort=1}", &lat)) &&
        SUCCEEDED(GetMetadataGPS(reader.Get(), L"/app1/ifd/gps/{ushort=4}", L"/app1/ifd/gps/{ushort=3}", &lon))) {
        foundGPS = true;
    }
    // Block 2: Try /ifd/gps/... (Fallback)
    else if (SUCCEEDED(GetMetadataGPS(reader.Get(), L"/ifd/gps/{ushort=2}", L"/ifd/gps/{ushort=1}", &lat)) &&
             SUCCEEDED(GetMetadataGPS(reader.Get(), L"/ifd/gps/{ushort=4}", L"/ifd/gps/{ushort=3}", &lon))) {
        foundGPS = true;
    }

    if (foundGPS) {
        pMetadata->HasGPS = true;
        pMetadata->Latitude = lat;
        pMetadata->Longitude = lon;
        
        // Altitude - Try multiple paths for HEIC/JPEG compatibility
        double alt = 0;
        const wchar_t* altQueries[] = { 
            L"/app1/ifd/gps/{ushort=6}", 
            L"/ifd/gps/{ushort=6}",
            L"/ifd/{ushort=34853}/{ushort=6}"  // GPS IFD pointer method
        };
        for (auto q : altQueries) {
            if (SUCCEEDED(GetMetadataRational(reader.Get(), q, &alt))) {
                pMetadata->Altitude = alt;
                // Check Ref (5): 0=Above, 1=Below
                PROPVARIANT altRef; PropVariantInit(&altRef);
                // Try corresponding ref path
                std::wstring refPath = q;
                size_t pos = refPath.rfind(L"6}");
                if (pos != std::wstring::npos) refPath.replace(pos, 2, L"5}");
                if (SUCCEEDED(reader->GetMetadataByName(refPath.c_str(), &altRef))) {
                    if (altRef.vt == VT_UI1 && altRef.bVal == 1) pMetadata->Altitude = -alt;
                    PropVariantClear(&altRef);
                }
                break;
            }
        }
    }

    return S_OK;
}

HRESULT CImageLoader::ComputeHistogram(IWICBitmapSource* source, ImageMetadata* pMetadata) {
    if (!source || !pMetadata) return E_INVALIDARG;
    
    // Reset
    pMetadata->HistR.assign(256, 0);
    pMetadata->HistG.assign(256, 0);
    pMetadata->HistB.assign(256, 0);
    pMetadata->HistL.assign(256, 0);

    ComPtr<IWICBitmap> bitmap;
    if (FAILED(source->QueryInterface(IID_PPV_ARGS(&bitmap)))) {
        return E_NOINTERFACE; 
    }
    
    ComPtr<IWICBitmapLock> lock;
    if (FAILED(bitmap->Lock(nullptr, WICBitmapLockRead, &lock))) return E_FAIL;
    
    UINT width = 0, height = 0;
    bitmap->GetSize(&width, &height);
    
    UINT stride = 0;
    lock->GetStride(&stride);

    UINT bufSize = 0;
    BYTE* ptr = nullptr;
    if (FAILED(lock->GetDataPointer(&bufSize, &ptr))) return E_FAIL;
    
    WICPixelFormatGUID format;
    source->GetPixelFormat(&format);
    
    // Determine offsets
    int offsetR = 0, offsetG = 1, offsetB = 2; // Default RGBA
    if (IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA) || 
        IsEqualGUID(format, GUID_WICPixelFormat32bppPBGRA)) {
        offsetR = 2; offsetG = 1; offsetB = 0;
    }
    
    // Optimized Histogram Calculation
    // For small images (< 2MP), use Step=1 (Exact).
    // For large images, use Step > 1 (Sparse).
    
    UINT64 totalPixels = (UINT64)width * height;
    
    UINT step = 1;
    if (totalPixels > 2000000) { // 2MP threshold
         // Target ~250k samples
         step = (UINT)sqrt(totalPixels / 250000.0);
         if (step < 1) step = 1;
    }
    
    // Sampling Loop
    for (UINT y = 0; y < height; y += step) {
        BYTE* row = ptr + (UINT64)y * stride; // Use 64-bit math for stride offset safety
        for (UINT x = 0; x < width; x += step) {
            // Safety check for last pixel in row? 
            // x*4 should be < stride. x < width implies x*4 < width*4 <= stride.
            // But if stride is tight...
            
            BYTE r = row[x * 4 + offsetR];
            BYTE g = row[x * 4 + offsetG];
            BYTE b = row[x * 4 + offsetB];
            
            pMetadata->HistR[r]++;
            pMetadata->HistG[g]++;
            pMetadata->HistB[b]++;
            
            // Luma (Integer approx: 0.2126R + 0.7152G + 0.0722B)
            // (54*R + 183*G + 18*B) / 256
            BYTE l = (BYTE)((54 * r + 183 * g + 19 * b) >> 8);
            pMetadata->HistL[l]++;
        }
    }
    
    return S_OK;
}
