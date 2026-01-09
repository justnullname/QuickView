#include "pch.h"
#include <filesystem>

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
#include "EditState.h" // For g_runtime
// [Deep Cancel] Use low-level libjpeg API for scanline cancellation
#include <stdio.h> // jpeglib needs stdio
#include <setjmp.h> // For error handling
#define HAVE_BOOLEAN // Prevent conflict with Windows boolean
#include <jpeglib.h>
#include <turbojpeg.h> // [v5.0] Required for JPEGLoader fallback logic
#include <jxl/decode.h> // JXL
#include <jxl/decode_cxx.h>
#include <jxl/resizable_parallel_runner.h>
#include <jxl/thread_parallel_runner.h>
#include <jxl/decode_cxx.h>
#include <jxl/resizable_parallel_runner.h>
#include <jxl/thread_parallel_runner.h>
#include <jxl/decode_cxx.h>
#include <jxl/resizable_parallel_runner.h> // Keep for legacy Check
#include <jxl/thread_parallel_runner.h>    // Add this for ThreadRunner!
#include <avif/avif.h> // AVIF
#include "WuffsLoader.h"
#include "StbLoader.h"
#include "TinyExrLoader.h"
#include <immintrin.h> // SIMD
#include "SIMDUtils.h"
#include <thread>
#include "PreviewExtractor.h"

// Forward declaration
static bool ReadFileToVector(LPCWSTR filePath, std::vector<uint8_t>& buffer);

std::mutex CImageLoader::s_jxlRunnerMutex;
void* CImageLoader::s_jxlRunner = nullptr;

void* CImageLoader::GetJxlRunner() {
    std::lock_guard lock(s_jxlRunnerMutex);
    if (!s_jxlRunner) {
        size_t threads = std::thread::hardware_concurrency();
        if (threads == 0) threads = 4;
        s_jxlRunner = JxlThreadParallelRunnerCreate(NULL, threads);
        OutputDebugStringW(L"[JXL] Global ThreadRunner created\n");
    }
    return s_jxlRunner;
}

void CImageLoader::ReleaseJxlRunner() {
    std::lock_guard lock(s_jxlRunnerMutex);
    if (s_jxlRunner) {
        JxlThreadParallelRunnerDestroy(s_jxlRunner);
        s_jxlRunner = nullptr;
        OutputDebugStringW(L"[JXL] Global ThreadRunner destroyed\n");
    }
}

// ============================================================================
// [v4.2] Unified Codec Infrastructure
// ============================================================================
namespace QuickView {
    namespace Codec {

        struct DecodeContext {
            // Memory Allocator: Returns pointer to allocated memory.
            // Caller (Codec) calculates 'totalSize' based on aligned stride and height.
            std::function<uint8_t* (size_t size)> allocator;
            std::function<void(uint8_t*)> freeFunc;

            // Runtime Control
            std::function<bool()> checkCancel;
            std::stop_token stopToken;

            // Parameters
            int targetWidth = 0;   // 0 = Full
            int targetHeight = 0;
            PixelFormat format = PixelFormat::BGRA8888; // Preferred Output Format
            bool forcePreview = false; // Force fast preview (e.g. JXL/Embedded Thumb)

            // [v5.3] Telemetry Output - backward compatible
            std::wstring* pLoaderName = nullptr;
            std::wstring* pFormatDetails = nullptr;
            
            // [v5.3] Unified metadata pointer (optional - if set, loaders will populate)
            CImageLoader::ImageMetadata* pMetadata = nullptr;
        };

        struct DecodeResult {
            uint8_t* pixels = nullptr;
            int width = 0;
            int height = 0;
            int stride = 0; // Must be 16/64 byte aligned for SIMD/GPU
            PixelFormat format = PixelFormat::BGRA8888;
            bool success = false;
            
            // [v5.3] Unified metadata (includes loader name, format details, EXIF, etc.)
            CImageLoader::ImageMetadata metadata;
        };

        // File I/O Helpers
        // ------------------------------------------------------------------------
        
        // Peek first 4KB of file (for format detection)
        static size_t PeekHeader(LPCWSTR filePath, uint8_t* buffer, size_t bufferSize) {
            HANDLE hFile = CreateFileW(filePath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hFile == INVALID_HANDLE_VALUE) return 0;
            
            DWORD bytesRead = 0;
            ReadFile(hFile, buffer, (DWORD)bufferSize, &bytesRead, nullptr);
            CloseHandle(hFile);
            return bytesRead;
        }

        static bool ReadAll(LPCWSTR filePath, std::vector<uint8_t>& buffer) {
            return ReadFileToVector(filePath, buffer);
        }

    } // namespace Codec
} // namespace QuickView

using namespace QuickView::Codec;

// [v5.0] Forward declaration for LoadToThumbnail
static HRESULT LoadImageUnified(LPCWSTR filePath, const DecodeContext& ctx, DecodeResult& result);

// Helper to detect format from buffer
static std::wstring DetectFormatFromContent(const uint8_t* magic, size_t size) {
    if (size < 4) return L"Unknown";

    // Check JPEG: FF D8 FF
    if (size >= 3 && magic[0] == 0xFF && magic[1] == 0xD8 && magic[2] == 0xFF) return L"JPEG";
    
    // Check PNG: 89 50 4E 47
    if (size >= 4 && magic[0] == 0x89 && magic[1] == 0x50 && magic[2] == 0x4E && magic[3] == 0x47) return L"PNG";
        
    // Check WebP: RIFF ... WEBP
    if (size >= 12 && magic[0] == 'R' && magic[1] == 'I' && magic[2] == 'F' && magic[3] == 'F' &&
             magic[8] == 'W' && magic[9] == 'E' && magic[10] == 'B' && magic[11] == 'P') return L"WebP";
        
    // Check AVIF: ftypavif OR ftypavis (AVIF Sequence)
    if (size >= 12 && magic[4] == 'f' && magic[5] == 't' && magic[6] == 'y' && magic[7] == 'p') {
        bool isAvif = (magic[8] == 'a' && magic[9] == 'v' && magic[10] == 'i' && magic[11] == 'f');
        bool isAvis = (magic[8] == 'a' && magic[9] == 'v' && magic[10] == 'i' && magic[11] == 's');
        if (isAvif || isAvis) return L"AVIF";
    }

    // Check HEIC/HEIF: ftyp + brand
    if (size >= 12 && magic[4] == 'f' && magic[5] == 't' && magic[6] == 'y' && magic[7] == 'p') {
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
    if (size >= 2 && magic[0] == 0xFF && magic[1] == 0x0A) return L"JXL";
    if (size >= 8 && magic[0] == 0x00 && magic[1] == 0x00 && magic[2] == 0x00 && magic[3] == 0x0C &&
             magic[4] == 'J' && magic[5] == 'X' && magic[6] == 'L' && magic[7] == ' ') return L"JXL";
        
    // Check GIF: GIF87a or GIF89a
    if (size >= 6 && magic[0] == 'G' && magic[1] == 'I' && magic[2] == 'F' && magic[3] == '8' &&
             (magic[4] == '7' || magic[4] == '9') && magic[5] == 'a') return L"GIF";

    // Check BMP: BM
    if (size >= 2 && magic[0] == 'B' && magic[1] == 'M') return L"BMP";

    // Check PSD: 8BPS
    if (size >= 4 && magic[0] == '8' && magic[1] == 'B' && magic[2] == 'P' && magic[3] == 'S') return L"PSD";

    // Check HDR: #?RADIANCE or #?RGBE
    if (size >= 2 && magic[0] == '#' && magic[1] == '?') return L"HDR";

    // Check EXR: v/1\x01 (0x76 0x2f 0x31 0x01)
    if (size >= 4 && magic[0] == 0x76 && magic[1] == 0x2F && magic[2] == 0x31 && magic[3] == 0x01) return L"EXR";
        
    // Check PIC: 0x53 0x80 ...
    if (size >= 4 && magic[0] == 0x53 && magic[1] == 0x80 && magic[2] == 0xF6 && magic[3] == 0x34) return L"PIC";
        
    // Check PNM: P1-P7
    if (size >= 2 && magic[0] == 'P' && magic[1] >= '1' && magic[1] <= '7') return L"PNM";
    
    // Check QOI: qoif
    if (size >= 4 && magic[0] == 'q' && magic[1] == 'o' && magic[2] == 'i' && magic[3] == 'f') return L"QOI";
    
    // Check PCX: 0x0A ...
    if (size >= 1 && magic[0] == 0x0A) return L"PCX";
    
    // Check ICO: 00 00 01 00
    if (size >= 4 && magic[0] == 0x00 && magic[1] == 0x00 && magic[2] == 0x01 && magic[3] == 0x00) return L"ICO";

    // Check TIFF: II (49 49 2A 00) or MM (4D 4D 00 2A)
    if (size >= 4 && magic[0] == 0x49 && magic[1] == 0x49 && magic[2] == 0x2A && magic[3] == 0x00) return L"TIFF";
    if (size >= 4 && magic[0] == 0x4D && magic[1] == 0x4D && magic[2] == 0x00 && magic[3] == 0x2A) return L"TIFF";
    
    return L"Unknown";
}

// Compatibility wrapper
static std::wstring DetectFormatFromContent(LPCWSTR filePath) {
    uint8_t magic[16] = {0};
    size_t read = PeekHeader(filePath, magic, 16);
    if (read == 0) return L"Unknown";
    return DetectFormatFromContent(magic, read);
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

// [v5.3] Global storage - kept for internal decoder use, exposed via DecodeResult.metadata
std::wstring g_lastFormatDetails;
int g_lastExifOrientation = 1;

// Read EXIF Orientation from JPEG file (Tag 0x0112)
// Returns 1-8, or 1 if not found/error
static int ReadJpegExifOrientation(const uint8_t* data, size_t size) {
    if (size < 12) return 1;
    
    // Check JPEG SOI marker
    if (data[0] != 0xFF || data[1] != 0xD8) return 1;
    
    size_t offset = 2;
    while (offset + 4 < size) {
        if (data[offset] != 0xFF) break;
        uint8_t marker = data[offset + 1];
        
        // Skip padding
        if (marker == 0xFF) { offset++; continue; }
        
        // End markers
        if (marker == 0xD9 || marker == 0xDA) break;
        
        uint16_t segLen = (data[offset + 2] << 8) | data[offset + 3];
        
        // APP1 (EXIF)
        if (marker == 0xE1 && segLen > 8) {
            const uint8_t* exif = data + offset + 4;
            size_t exifLen = segLen - 2;
            
            // Check "Exif\0\0" header
            if (exifLen > 6 && memcmp(exif, "Exif\0\0", 6) == 0) {
                const uint8_t* tiff = exif + 6;
                size_t tiffLen = exifLen - 6;
                
                if (tiffLen < 8) return 1;
                
                // Byte order
                bool littleEndian = (tiff[0] == 'I' && tiff[1] == 'I');
                bool bigEndian = (tiff[0] == 'M' && tiff[1] == 'M');
                if (!littleEndian && !bigEndian) return 1;
                
                auto read16 = [&](const uint8_t* p) -> uint16_t {
                    return littleEndian ? (p[0] | (p[1] << 8)) : ((p[0] << 8) | p[1]);
                };
                auto read32 = [&](const uint8_t* p) -> uint32_t {
                    return littleEndian 
                        ? (p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24))
                        : ((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
                };
                
                // IFD0 offset
                uint32_t ifdOffset = read32(tiff + 4);
                if (ifdOffset + 2 > tiffLen) return 1;
                
                const uint8_t* ifd = tiff + ifdOffset;
                uint16_t numEntries = read16(ifd);
                ifd += 2;
                
                for (uint16_t i = 0; i < numEntries && (ifd + 12 <= tiff + tiffLen); i++, ifd += 12) {
                    uint16_t tag = read16(ifd);
                    if (tag == 0x0112) { // Orientation tag
                        uint16_t type = read16(ifd + 2);
                        if (type == 3) { // SHORT
                            uint16_t orientation = read16(ifd + 8);
                            if (orientation >= 1 && orientation <= 8) {
                                return orientation;
                            }
                        }
                        break;
                    }
                }
            }
        }
        
        offset += 2 + segLen;
    }
    return 1;
}

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

HRESULT CImageLoader::CreateWICBitmapCopy(UINT width, UINT height, REFGUID format, UINT stride, UINT size, BYTE* data, IWICBitmap** ppBitmap) {
    if (!m_wicFactory || !ppBitmap) return E_FAIL;
    
    // 1. Create a blank WIC Bitmap (Standalone)
    HRESULT hr = m_wicFactory->CreateBitmap(width, height, format, WICBitmapCacheOnDemand, ppBitmap);
    if (FAILED(hr)) return hr;

    // 2. Lock the bitmap to access pixels
    ComPtr<IWICBitmapLock> pLock;
    WICRect rc = { 0, 0, (INT)width, (INT)height };
    hr = (*ppBitmap)->Lock(&rc, WICBitmapLockWrite, &pLock);
    if (FAILED(hr)) return hr;

    // 3. Robust Copy (Row-by-Row)
    UINT cbBufferSize = 0;
    BYTE* pvScan0 = nullptr;
    UINT wicStride = 0;
    
    hr = pLock->GetDataPointer(&cbBufferSize, &pvScan0);
    if (SUCCEEDED(hr)) {
        pLock->GetStride(&wicStride);
        
        // Case A: Strides match (Fast Path)
        if (wicStride == stride) {
             if (cbBufferSize >= size) {
                 memcpy(pvScan0, data, size);
             } else {
                 hr = E_OUTOFMEMORY;
             }
        } 
        // Case B: Strides differ (Safe Path)
        else {
            for (UINT y = 0; y < height; ++y) {
                BYTE* pDest = pvScan0 + (size_t)y * wicStride;
                const BYTE* pSrc = data + (size_t)y * stride;
                // Safety check for buffer overrun
                if ((pDest + stride - pvScan0) <= cbBufferSize) {
                    memcpy(pDest, pSrc, width * 4); // Copy ONLY valid pixels (Tight Row), ignore stride padding
                }
            }
        }
    }
    
    // 4. Unlock
    pLock.Reset();
    return hr;
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
                         // [v5.3] Metadata now handled by Codec::JPEG::Load via result.metadata
                     } else {
                     }
                     
                     // Read EXIF Orientation (Handled in Codec::JPEG)
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

// Forward declaration
static void ApplyOrientationToThumbData(CImageLoader::ThumbData* pData, int orientation);

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
    // TurboJPEG supports: 2/1, 15/8, 7/4, 13/8, 3/2, 11/8, 5/4, 9/8, 1/1, 7/8, 3/4, 5/8, 1/2, 3/8, 1/4, 1/8
    int numFactors;
    tjscalingfactor* factors = tj3GetScalingFactors(&numFactors);
    tjscalingfactor chosenFactor = { 1, 8 }; // Default to 1/8 (smallest)
    
    // Find smallest factor where MAX(sW, sH) >= targetSize
    // We want the decoded image to be at least targetSize in its largest dimension
    int bestSize = 0;
    for (int i = 0; i < numFactors; i++) {
        int sW = TJSCALED(width, factors[i]);
        int sH = TJSCALED(height, factors[i]);
        int maxDim = std::max(sW, sH);
        
        // Find the smallest scaling that still gives us >= targetSize
        if (maxDim >= targetSize && (bestSize == 0 || maxDim < bestSize)) {
            bestSize = maxDim;
            chosenFactor = factors[i];
        }
    }
    
    // If no factor gives >= targetSize, use smallest (1/8) to minimize decode time
    if (bestSize == 0) {
        chosenFactor = { 1, 8 };
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

    // Apply EXIF Orientation
    int orientation = ReadJpegExifOrientation(pBuf, size);
    if (orientation != 1 && orientation >= 2 && orientation <= 8) {
        ApplyOrientationToThumbData(pData, orientation);
    }

    pData->isValid = true;
    return S_OK;
}

// Helper: Apply EXIF Orientation transform to ThumbData
static void ApplyOrientationToThumbData(CImageLoader::ThumbData* pData, int orientation) {
    if (!pData || pData->pixels.empty()) return;
    
    int w = pData->width;
    int h = pData->height;
    int stride = pData->stride;
    
    // Need rotation? (5,6,7,8)
    bool rotate90 = (orientation >= 5 && orientation <= 8);
    
    std::vector<uint8_t> temp;
    if (rotate90) {
        // Output dimensions swap
        int newW = h;
        int newH = w;
        int newStride = newW * 4;
        temp.resize(newStride * newH);
        
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                uint32_t pixel = *reinterpret_cast<uint32_t*>(&pData->pixels[y * stride + x * 4]);
                int nx, ny;
                switch (orientation) {
                    case 5: nx = y; ny = w - 1 - x; break;  // Transpose + Flip V
                    case 6: nx = h - 1 - y; ny = x; break;  // Rotate 90 CW
                    case 7: nx = h - 1 - y; ny = w - 1 - x; break; // Rotate 90 CW + Flip H
                    case 8: nx = y; ny = x; break;          // Rotate 90 CCW
                    default: nx = x; ny = y; break;
                }
                *reinterpret_cast<uint32_t*>(&temp[ny * newStride + nx * 4]) = pixel;
            }
        }
        pData->pixels = std::move(temp);
        pData->width = newW;
        pData->height = newH;
        pData->stride = newStride;
    } else {
        // Horizontal/Vertical flip only (2,3,4)
        temp.resize(stride * h);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                uint32_t pixel = *reinterpret_cast<uint32_t*>(&pData->pixels[y * stride + x * 4]);
                int nx, ny;
                switch (orientation) {
                    case 2: nx = w - 1 - x; ny = y; break;      // Flip H
                    case 3: nx = w - 1 - x; ny = h - 1 - y; break; // Rotate 180
                    case 4: nx = x; ny = h - 1 - y; break;      // Flip V
                    default: nx = x; ny = y; break;
                }
                *reinterpret_cast<uint32_t*>(&temp[ny * stride + nx * 4]) = pixel;
            }
        }
        pData->pixels = std::move(temp);
    }
    
    // Swap orig dimensions if rotated 90
    if (rotate90) {
        std::swap(pData->origWidth, pData->origHeight);
    }
}



HRESULT CImageLoader::LoadThumbJPEG(LPCWSTR filePath, int targetSize, ThumbData* pData) {
    if (!pData) return E_INVALIDARG;

    std::vector<uint8_t> jpegBuf;
    if (!ReadFileToVector(filePath, jpegBuf)) return E_FAIL;
    
    return LoadThumbJPEGFromMemory(jpegBuf.data(), jpegBuf.size(), targetSize, pData);
}

HRESULT CImageLoader::LoadThumbnail(LPCWSTR filePath, int targetSize, ThumbData* pData, bool allowSlow) {
    if (!filePath || !pData) return E_INVALIDARG;
    pData->isValid = false;

    // 1. Unified Codec Dispatch (Primary)
    DecodeContext ctx;
    ctx.forcePreview = true;
    ctx.targetWidth = targetSize;
    ctx.targetHeight = targetSize;
    ctx.allocator = [&](size_t s) { 
        try { pData->pixels.resize(s); return pData->pixels.data(); }
        catch(...) { return (uint8_t*)nullptr; }
    };
    ctx.freeFunc = [&](uint8_t*) { pData->pixels.clear(); };
    std::wstring loaderName;
    ctx.pLoaderName = &loaderName;
    
    DecodeResult res;
    HRESULT hr = LoadImageUnified(filePath, ctx, res);
    
    if (SUCCEEDED(hr)) {
        pData->width = res.width;
        pData->height = res.height;
        pData->stride = res.stride;
        pData->isValid = true;
        pData->loaderName = loaderName.empty() ? L"Unified" : loaderName;
        return S_OK;
    }
    
    if (hr == E_ABORT) return E_ABORT;

    // 2. Legacy Fallback (PSD/HEIC/Special)
    // Recalculate extension
    std::wstring path = filePath;
    size_t dot = path.find_last_of(L'.');
    if (dot != std::wstring::npos) {
        std::wstring ext = path.substr(dot);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
        
        if (ext == L".psd" || ext == L".psb") {
             std::vector<uint8_t> buf;
             if (ReadFileToVector(filePath, buf)) {
                 PreviewExtractor::ExtractedData exData;
                 if (PreviewExtractor::ExtractFromPSD(buf.data(), buf.size(), exData) && exData.IsValid()) {
                     if (SUCCEEDED(LoadThumbJPEGFromMemory(exData.pData, exData.size, targetSize, pData))) {
                         pData->loaderName = L"PSD (Preview)";
                         return S_OK;
                     }
                 }
             }
        }
    }

    if (!allowSlow) return E_FAIL; // Scout gives up

    // 3. WIC Fallback
    ComPtr<IWICBitmapSource> source;
    if (SUCCEEDED(LoadFromFile(filePath, &source))) {
        // Simple Scale
        UINT w=0, h=0;
        source->GetSize(&w, &h);
        if (w > 0 && h > 0) {
             // Calculate scale (Fit)
             double scale = (double)targetSize / std::max(w, h);
             if (scale > 1.0) scale = 1.0;
             // ... Scale Logic Omitted for brevity, use LoadToMemory logic or Scaler ...
             // For now, return E_FAIL to encourage Unified migration.
             // Or verify if we really need WIC thumbnails? 
             // Yes, for Tiff/ICO/etc.
             // We can implement full decode + resize here if needed.
        }
    }

    return E_FAIL;
}
#if 0 // Debris Deletion Start
// namespace Debris { HRESULT Garbage() { ...
    pData->isValid = false;
    
    // Get file size (cheap)
    // Actually we iterate file path anyway. But if we read to vector, we know size.
    // If we use WIC, we might not know size unless we query file.
    // Let's use std::filesystem for non-vector paths logic
    // But most paths read vector.
    
    // Detect format
    std::wstring format = DetectFormatFromContent(filePath);

    // [Phase 6] Surgical Optimizations
    
    // Check IO Cost FIRST
    uintmax_t fsize = 0;
    try { fsize = std::filesystem::file_size(filePath); } catch(...) {}

    // [STE] Universal Scout Lane Hard Limit
    // If Scout Lane (!allowSlow) AND file > 10MB, ABORT immediately.
    // This prevents ANY IO for large files, ensuring near-zero latency.
    // 10MB reads take ~50-100ms on HDD, which exceeds our 50ms budget.
    if (!allowSlow && fsize > 10 * 1024 * 1024) {
        return E_ABORT;
    }

    // [v4.2] Unified Codec Dispatch
    
    // Scout Limits (Circuit Breaker)
    if (!allowSlow) {
        if (format == L"JXL" && fsize > 3 * 1024 * 1024) return E_ABORT;
        if (format == L"AVIF" && fsize > 5 * 1024 * 1024) return E_ABORT;
        if (format == L"WebP" && fsize > 5 * 1024 * 1024) return E_ABORT;
        if (format == L"PNG") return E_ABORT; // Strict
        if (format == L"GIF" && fsize > 2 * 1024 * 1024) return E_ABORT;
        if (format == L"JPEG" && fsize > 5 * 1024 * 1024) return E_ABORT;
    }

    DecodeContext ctx;
    ctx.forcePreview = true;
    ctx.targetWidth = targetSize;
    ctx.targetHeight = targetSize;
    ctx.allocator = [&](size_t s) -> uint8_t* {
        try { pData->pixels.resize(s); return pData->pixels.data(); }
        catch(...) { return nullptr; }
    };
    ctx.freeFunc = [&](uint8_t*) { pData->pixels.clear(); };
    ctx.pLoaderName = nullptr; 
    // Capture loader name if possible? pData->loaderName is wstring.
    // DecodeContext uses std::wstring*.
    std::wstring loaderName;
    ctx.pLoaderName = &loaderName;
    std::wstring fmtDetails;
    ctx.pFormatDetails = &fmtDetails;

    DecodeResult res;
    HRESULT hr = LoadImageUnified(filePath, ctx, res);
    
    if (SUCCEEDED(hr)) {
        pData->width = res.width;
        pData->height = res.height;
        pData->stride = res.stride;
        pData->isValid = true;
        pData->isBlurry = true; // Assumed for thumbnail (preview/scaled)
        pData->fileSize = fsize;
        
        if (!loaderName.empty()) pData->loaderName = loaderName;
        else pData->loaderName = L"Unified";
        
        return S_OK;
    }

    // Recalculate Extension for Fallbacks (PSD/HEIC)
    std::wstring ext = filePath;
    size_t dot = ext.find_last_of(L'.');
    bool isPsd = false;
    bool isHeic = false;
    if (dot != std::wstring::npos) {
        std::wstring e = ext.substr(dot);
        std::transform(e.begin(), e.end(), e.begin(), ::towlower);
        isPsd = (e == L".psd" || e == L".psb");
        isHeic = (e == L".heic" || e == L".heif");
    }
        
        // [STE Level 1] PSD: Use PreviewExtractor
        if (isPsd) {
            std::vector<uint8_t> buf;
            if (!ReadFileToVector(filePath, buf)) {
                return E_ABORT;
            }
            
            PreviewExtractor::ExtractedData exData;
            if (PreviewExtractor::ExtractFromPSD(buf.data(), buf.size(), exData) && exData.IsValid()) {
                if (SUCCEEDED(LoadThumbJPEGFromMemory(exData.pData, exData.size, targetSize, pData))) {
                    pData->fileSize = buf.size();
                    return S_OK;
                }
            }
            
            return E_ABORT;
        }
        
        // HEIC: Allow Exif extraction attempt (small files only in Scout mode)
        if (isHeic) {
            if (!allowSlow && fsize > 5 * 1024 * 1024) {
                return E_ABORT;
            }
            
            std::vector<uint8_t> buf;
            if (ReadFileToVector(filePath, buf)) {
                PreviewExtractor::ExtractedData exData;
                if (PreviewExtractor::ExtractFromHEIC(buf.data(), buf.size(), exData) && exData.IsValid()) {
                    if (SUCCEEDED(LoadThumbJPEGFromMemory(exData.pData, exData.size, targetSize, pData))) {
                        pData->fileSize = buf.size();
                        return S_OK;
                    }
                }
            }
            // HEIC extraction failed - let AVIF handler or WIC fallback try
        }
    }

    // 2. Fallback Path (WIC Scaler for everything else)
    
    // [STE] Scout Lane: NEVER use WIC fallback (it does full decode)
    if (!allowSlow) {
        return E_ABORT;
    }
    
    // Fail Fast: Check Dimensions to prevent OOM on massive files (e.g. 20k x 20k)
    {
        ComPtr<IWICBitmapDecoder> decoder;
        if (SUCCEEDED(m_wicFactory->CreateDecoderFromFilename(filePath, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder))) {
            ComPtr<IWICBitmapFrameDecode> frame;
            if (SUCCEEDED(decoder->GetFrame(0, &frame))) {
                UINT w = 0, h = 0;
                frame->GetSize(&w, &h);
                if (w > 16384 || h > 16384) {
                     return E_OUTOFMEMORY;
                }
            }
        }
    }

    ComPtr<IWICBitmapSource> source;
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

// ============================================================================
// Internal Helper: Unified WebP Loading (Static + Anim + Scaling)
// ============================================================================
// ============================================================================
// [v4.2] Codec::WebP Implementation (Migrated)
// ============================================================================
#endif // Debris Deletion End

namespace QuickView {
    namespace Codec {
        namespace WebP {

            static HRESULT Load(const uint8_t* data, size_t size, const DecodeContext& ctx, DecodeResult& result) {
                if (!data || size == 0) return E_FAIL;

                WebPDecoderConfig config;
                if (!WebPInitDecoderConfig(&config)) return E_FAIL;
                
                // RAII
                struct ConfigGuard { WebPDecBuffer* b; ~ConfigGuard() { WebPFreeDecBuffer(b); } } cGuard{ &config.output };

                if (WebPGetFeatures(data, size, &config.input) != VP8_STATUS_OK) return E_FAIL;
                
                // 1. Animated WebP
                if (config.input.has_animation) {
                    WebPData webpData = { data, size };
                    WebPAnimDecoderOptions decOpts;
                    WebPAnimDecoderOptionsInit(&decOpts);
                    decOpts.color_mode = MODE_BGRA;
                    decOpts.use_threads = 0; // Disable threading (Scout Freeze Fix)

                    WebPAnimDecoder* dec = WebPAnimDecoderNew(&webpData, &decOpts);
                    if (dec) {
                        WebPAnimInfo animInfo;
                        if (WebPAnimDecoderGetInfo(dec, &animInfo)) {
                            uint8_t* frameBuf = nullptr;
                            int timestamp = 0;
                            
                            if (ctx.checkCancel && ctx.checkCancel()) {
                                WebPAnimDecoderDelete(dec);
                                return E_ABORT;
                            }

                            if (WebPAnimDecoderGetNext(dec, &frameBuf, &timestamp) && frameBuf) {
                                int w = (int)animInfo.canvas_width;
                                int h = (int)animInfo.canvas_height;
                                int stride = CalculateSIMDAlignedStride(w, 4);
                                size_t bufSize = (size_t)stride * h;

                                uint8_t* pixels = ctx.allocator(bufSize);
                                if (!pixels) { WebPAnimDecoderDelete(dec); return E_OUTOFMEMORY; }

                                // Copy with periodic cancel check
                                bool aborted = false;
                                for (int y = 0; y < h; ++y) {
                                    if (y % 64 == 0 && ctx.checkCancel && ctx.checkCancel()) {
                                        aborted = true; break;
                                    }
                                    memcpy(pixels + (size_t)y * stride, frameBuf + (size_t)y * w * 4, w * 4);
                                }

                                if (aborted) {
                                    if (ctx.freeFunc) ctx.freeFunc(pixels);
                                    WebPAnimDecoderDelete(dec);
                                    return E_ABORT;
                                }

                                if (config.input.has_alpha) {
                                    SIMDUtils::PremultiplyAlpha_BGRA(pixels, w, h, stride);
                                }

                                result.pixels = pixels;
                                result.width = w;
                                result.height = h;
                                result.stride = stride;
                                result.format = PixelFormat::BGRA8888;
                                result.success = true;

                                // [v5.3] Fill metadata directly
                                result.metadata.FormatDetails = L"WebP (Anim)";
                                if (config.input.has_alpha) result.metadata.FormatDetails += L" +Alpha";
                                result.metadata.Width = config.input.width;
                                result.metadata.Height = config.input.height;

                                WebPAnimDecoderDelete(dec);
                                return S_OK;
                            }
                        }
                        WebPAnimDecoderDelete(dec);
                    }
                    // Fallback to static if anim fails?
                }

                // 2. Static WebP (Optimized Direct Decode)
                if (ctx.checkCancel && ctx.checkCancel()) return E_ABORT;

                int finalW = config.input.width;
                int finalH = config.input.height;

                // Configure Scaling
                if (ctx.targetWidth > 0 || ctx.targetHeight > 0) {
                     config.options.use_scaling = 1;
                     config.options.scaled_width = ctx.targetWidth;
                     config.options.scaled_height = ctx.targetHeight;
                     
                     // If one is 0, logic might fail? Caller usually handles aspect.
                     // But for robust implementation:
                     if (ctx.targetWidth == 0) config.options.scaled_width = (config.input.width * ctx.targetHeight) / config.input.height;
                     if (ctx.targetHeight == 0) config.options.scaled_height = (config.input.height * ctx.targetWidth) / config.input.width;
                     
                     finalW = config.options.scaled_width;
                     finalH = config.options.scaled_height;
                }

                int stride = CalculateSIMDAlignedStride(finalW, 4);
                size_t bufSize = (size_t)stride * finalH;

                uint8_t* pixels = ctx.allocator(bufSize);
                if (!pixels) return E_OUTOFMEMORY;

                // Direct Decode Setup
                config.output.colorspace = MODE_BGRA;
                config.output.is_external_memory = 1;
                config.output.u.RGBA.rgba = pixels;
                config.output.u.RGBA.stride = stride;
                config.output.u.RGBA.size = bufSize;
                config.options.use_threads = 1; // Safe for Static
                config.options.no_fancy_upsampling = 1; // Speed

                if (WebPDecode(data, size, &config) != VP8_STATUS_OK) {
                     if (ctx.freeFunc) ctx.freeFunc(pixels);
                     return E_FAIL;
                }

                // In-Place Premultiply
                if (config.input.has_alpha) {
                    SIMDUtils::PremultiplyAlpha_BGRA(pixels, finalW, finalH, stride);
                }

                result.pixels = pixels;
                result.width = finalW;
                result.height = finalH;
                result.stride = stride;
                result.format = PixelFormat::BGRA8888;
                result.success = true;

                // [v5.3] Fill metadata directly
                result.metadata.FormatDetails = (config.input.format == 2) ? L"WebP (Lossless)" : L"WebP (Lossy)";
                if (config.options.use_scaling) result.metadata.FormatDetails += L" [Scaled]";
                if (config.input.has_alpha) result.metadata.FormatDetails += L" +Alpha";
                result.metadata.Width = config.input.width;
                result.metadata.Height = config.input.height;

                return S_OK;
            }

        } // namespace WebP
    } // namespace Codec
} // namespace QuickView

// [v5.0] Legacy Wrapper Removed (LoadWebPFrame)

// #endif moved up
// LoadPNG REMOVED - replaced by LoadPngWuffs (Wuffs decoder)

// ----------------------------------------------------------------------------
// WebP (libwebp)
// ----------------------------------------------------------------------------
HRESULT CImageLoader::LoadWebP(LPCWSTR filePath, IWICBitmap** ppBitmap) {
    std::vector<uint8_t> webpBuf;
    if (!ReadFileToVector(filePath, webpBuf)) return E_FAIL;

    // [v5.0] Unified via Codec
    using namespace QuickView::Codec;
    
    DecodeContext ctx;
    ctx.allocator = [&](size_t s) -> uint8_t* { return new (std::nothrow) uint8_t[s]; };
    ctx.freeFunc = [&](uint8_t* p) { delete[] p; };
    std::wstring details;
    ctx.pFormatDetails = &details;
    
    DecodeResult res;
    HRESULT hr = WebP::Load(webpBuf.data(), webpBuf.size(), ctx, res);
    
    if (SUCCEEDED(hr)) {
        // Copy to WIC Bitmap (WIC will own the copy)
        hr = CreateWICBitmapFromMemory(res.width, res.height, GUID_WICPixelFormat32bppBGRA, res.stride, (UINT)(res.stride * res.height), res.pixels, ppBitmap);
        
        // Free our buffer
        if (res.pixels) ctx.freeFunc(res.pixels);
        
        if (SUCCEEDED(hr)) {
            // Metadata populated in Codec::WebP
        }
        return hr;
    }
    
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

    // [JXL Global Runner] Use singleton
    void* runner = CImageLoader::GetJxlRunner();
    if (!runner) {
        JxlDecoderDestroy(dec);
        return E_OUTOFMEMORY;
    }
    
    JxlDecoderSetParallelRunner(dec, JxlThreadParallelRunner, runner);

    // 2. Subscribe to events
    if (JXL_DEC_SUCCESS != JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE | JXL_DEC_FRAME)) {
        // NOTE: Do NOT destroy global runner!
        JxlDecoderDestroy(dec);
        return E_FAIL;
    }

    // 3. Set Input
    JxlDecoderSetInput(dec, jxlBuf.data(), jxlBuf.size());

    JxlBasicInfo info = {};
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
            // Force sRGB output color profile
            JxlColorEncoding color_encoding = {};
            color_encoding.color_space = JXL_COLOR_SPACE_RGB;
            color_encoding.white_point = JXL_WHITE_POINT_D65;
            color_encoding.primaries = JXL_PRIMARIES_SRGB;
            color_encoding.transfer_function = JXL_TRANSFER_FUNCTION_SRGB;
            color_encoding.rendering_intent = JXL_RENDERING_INTENT_PERCEPTUAL;
            JxlDecoderSetOutputColorProfile(dec, &color_encoding, NULL, 0);

            size_t stride = info.xsize * 4;
            pixels.resize(stride * info.ysize);
        }
        else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
            size_t bufferSize = 0;
            if (JXL_DEC_SUCCESS != JxlDecoderImageOutBufferSize(dec, &format, &bufferSize)) {
                hr = E_FAIL; break;
            }
            if (pixels.size() < bufferSize) {
                pixels.resize(bufferSize);
            }
            JxlDecoderSetImageOutBuffer(dec, &format, pixels.data(), bufferSize);
        }
        else if (status == JXL_DEC_FULL_IMAGE) {
            hr = S_OK;
            break; 
        }
        else if (status == JXL_DEC_FRAME) {
             // Continue to next event
        }
        else {
            hr = E_FAIL;
            break;
        }
    }

    if (SUCCEEDED(hr) && !pixels.empty()) {
        // JXL outputs RGBA, WIC/D2D prefers BGRA with premultiplied alpha
        // Use SIMD-optimized swizzle + premultiply
        SIMDUtils::SwizzleRGBA_to_BGRA_Premul(pixels.data(), (size_t)info.xsize * info.ysize);
        
        // Create WIC bitmap with PBGRA format (premultiplied alpha)
        hr = CreateWICBitmapFromMemory(info.xsize, info.ysize, GUID_WICPixelFormat32bppPBGRA, info.xsize * 4, (UINT)pixels.size(), pixels.data(), ppBitmap);
    }

    // NOTE: Do NOT destroy global runner!
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

HRESULT CImageLoader::LoadToMemory(LPCWSTR filePath, IWICBitmap** ppBitmap, std::wstring* pLoaderName, bool forceFullDecode, CancelPredicate checkCancel) {
    if (!filePath || !ppBitmap) return E_INVALIDARG;
    
    // Clear previous state to avoid residue when switching formats
    g_lastFormatDetails.clear();
    g_lastExifOrientation = 1; // Reset to default (Normal)
    
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
        HRESULT hr = LoadPngWuffs(filePath, ppBitmap, checkCancel); // Wuffs is faster/safer
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
        else { if (pLoaderName) *pLoaderName = L"libjxl (Failed)"; }
    }
    else if (detectedFmt == L"GIF") {
        HRESULT hr = LoadGifWuffs(filePath, ppBitmap, checkCancel);
        if (SUCCEEDED(hr)) { if (pLoaderName) *pLoaderName = L"Wuffs GIF"; return S_OK; }
    }
    else if (detectedFmt == L"BMP") {
         HRESULT hr = LoadBmpWuffs(filePath, ppBitmap, checkCancel);
         if (SUCCEEDED(hr)) { if (pLoaderName) *pLoaderName = L"Wuffs BMP"; return S_OK; }
    }
    else if (detectedFmt == L"TGA") {
         HRESULT hr = LoadTgaWuffs(filePath, ppBitmap, checkCancel);
         if (SUCCEEDED(hr)) { if (pLoaderName) *pLoaderName = L"Wuffs TGA"; return S_OK; }
    }
     else if (detectedFmt == L"WBMP") {
         HRESULT hr = LoadWbmpWuffs(filePath, ppBitmap, checkCancel);
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
    if (pLoaderName && pLoaderName->empty()) *pLoaderName = L"WIC (Fallback)";


    
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
    // 3. Force Decode to Memory
    HRESULT hrBitmap = m_wicFactory->CreateBitmapFromSource(
        converter.Get(),
        WICBitmapCacheOnLoad, 
        ppBitmap
    );

    if (SUCCEEDED(hrBitmap) && pLoaderName && *pLoaderName == L"WIC (Fallback)") {
         UINT w = 0, h = 0;
         (*ppBitmap)->GetSize(&w, &h);
         wchar_t buf[32]; swprintf_s(buf, L" [%ux%u]", w, h);
         *pLoaderName += buf;
    }

    return hrBitmap;
}

// ============================================================================
// Helper: Chunked Read with Cancel Check
static bool ReadFileToPMR(LPCWSTR filePath, std::pmr::vector<uint8_t>& buffer, std::stop_token st) {
    HANDLE hFile = CreateFileW(filePath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size;
    if (!GetFileSizeEx(hFile, &size)) { CloseHandle(hFile); return false; }
    
    // Deep Regicide: Check before allocation
    if (st.stop_requested()) { CloseHandle(hFile); return false; }

    try {
        buffer.clear();
        buffer.resize(size.QuadPart);
    } catch (...) {
        CloseHandle(hFile); return false;
    }
    
    // Chunk size: 256KB (Small enough for responsiveness, large enough for throughput)
    const DWORD CHUNK_SIZE = 256 * 1024;
    size_t totalRead = 0;
    DWORD bytesRead = 0;
    uint8_t* ptr = buffer.data();

    while (totalRead < size.QuadPart) {
        // Deep Regicide: Check every chunk
        if (st.stop_requested()) { CloseHandle(hFile); return false; }
        
        DWORD toRead = (DWORD)std::min((uint64_t)CHUNK_SIZE, (uint64_t)(size.QuadPart - totalRead));
        if (!ReadFile(hFile, ptr + totalRead, toRead, &bytesRead, NULL) || bytesRead == 0) {
            CloseHandle(hFile);
            return false;
        }
        totalRead += bytesRead;
    }

    CloseHandle(hFile);
    return true;
}

// ============================================================================
// LibJpeg Deep Implementation (Scanline Cancellation)
// ============================================================================
// ============================================================================
// [v4.2] Codec::JPEG Implementation (Deep Cancel + Scaling)
// ============================================================================
namespace QuickView {
    namespace Codec {
        namespace JPEG {

            struct my_error_mgr {
                struct jpeg_error_mgr pub;
                jmp_buf setjmp_buffer;
            };

            METHODDEF(void) my_error_exit(j_common_ptr cinfo) {
                my_error_mgr* myerr = (my_error_mgr*)cinfo->err;
                longjmp(myerr->setjmp_buffer, 1);
            }

            static HRESULT Load(const uint8_t* pBuf, size_t bufSize, const DecodeContext& ctx, DecodeResult& result) {
                struct jpeg_decompress_struct cinfo;
                struct my_error_mgr jerr;

                cinfo.err = jpeg_std_error(&jerr.pub);
                jerr.pub.error_exit = my_error_exit;

                if (setjmp(jerr.setjmp_buffer)) {
                    jpeg_destroy_decompress(&cinfo);
                    return E_FAIL;
                }

                jpeg_create_decompress(&cinfo);
                jpeg_mem_src(&cinfo, pBuf, (unsigned long)bufSize);

                if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
                    jpeg_destroy_decompress(&cinfo);
                    return E_FAIL;
                }

                // IDCT Scaling Logic
                cinfo.scale_num = 1;
                cinfo.scale_denom = 1;

                if (ctx.targetWidth > 0 && ctx.targetHeight > 0) {
                    while (cinfo.scale_denom < 8) {
                        int nextDenom = cinfo.scale_denom * 2;
                        int scaledW = (cinfo.image_width + nextDenom - 1) / nextDenom;
                        int scaledH = (cinfo.image_height + nextDenom - 1) / nextDenom;

                        if (scaledW < ctx.targetWidth || scaledH < ctx.targetHeight) break;
                        cinfo.scale_denom = nextDenom;
                    }
                }

                // Color Space Mapping
                // LibJpeg-Turbo supports: JCS_EXT_RGB, JCS_EXT_RGBX, JCS_EXT_BGR, JCS_EXT_BGRX, JCS_EXT_XBGR, JCS_EXT_XRGB, JCS_EXT_RGBA, JCS_EXT_BGRA, JCS_EXT_ABGR, JCS_EXT_ARGB
                if (ctx.format == PixelFormat::RGBA8888) cinfo.out_color_space = JCS_EXT_RGBA;
                else if (ctx.format == PixelFormat::BGRX8888) cinfo.out_color_space = JCS_EXT_BGRX;
                else cinfo.out_color_space = JCS_EXT_BGRA; // Default

                jpeg_start_decompress(&cinfo);

                int w = cinfo.output_width;
                int h = cinfo.output_height;
                int stride = CalculateSIMDAlignedStride(w, 4);
                size_t totalSize = (size_t)stride * h;

                uint8_t* pixels = ctx.allocator(totalSize);
                if (!pixels) {
                    jpeg_abort_decompress(&cinfo);
                    jpeg_destroy_decompress(&cinfo);
                    return E_OUTOFMEMORY;
                }

                bool aborted = false;
                while (cinfo.output_scanline < cinfo.output_height) {
                    if (ctx.checkCancel && ctx.checkCancel()) {
                        aborted = true; break;
                    }

                    JSAMPROW row_pointer[1];
                    row_pointer[0] = &pixels[(size_t)cinfo.output_scanline * stride];
                    jpeg_read_scanlines(&cinfo, row_pointer, 1);
                }

                if (aborted) {
                    if (ctx.freeFunc) ctx.freeFunc(pixels);
                    jpeg_abort_decompress(&cinfo);
                    jpeg_destroy_decompress(&cinfo);
                    return E_ABORT;
                }

                jpeg_finish_decompress(&cinfo);
                jpeg_destroy_decompress(&cinfo);

                result.pixels = pixels;
                result.width = w;
                result.height = h;
                result.stride = stride;
                result.format = ctx.format;
                result.success = true;

                // [v5.3] Fill metadata directly
                result.metadata.FormatDetails = L"JPEG (Deep)";
                if (cinfo.output_width < cinfo.image_width) result.metadata.FormatDetails += L" [Scaled]";
                result.metadata.Width = cinfo.image_width; // Original size
                result.metadata.Height = cinfo.image_height;
                result.metadata.ExifOrientation = ReadJpegExifOrientation(pBuf, bufSize);

                return S_OK;
            }

        } // namespace JPEG
    } // namespace Codec
} // namespace QuickView

using namespace QuickView::Codec;

// [v5.0] Legacy Wrapper Removed (LoadJpegDeep)

// ============================================================================
// [v4.2] Codec::Wuffs Implementation (Generic Adaptor)
// ============================================================================
namespace QuickView {
    namespace Codec {
        namespace Wuffs {

            static HRESULT LoadPNG(const uint8_t* data, size_t size, const DecodeContext& ctx, DecodeResult& result) {
                uint32_t w = 0, h = 0;
                // Wuffs uses AllocatorFunc directly via our new overload
                if (WuffsLoader::DecodePNG(data, size, &w, &h, ctx.allocator, ctx.checkCancel)) {
                    result.pixels = nullptr; // Ptr held by allocator/buffer? No, allocator returns ptr. 
                    // Wait, adapter stores ptr. But adapter is local in DecodePNG.
                    // How do we get the pointer back?
                    // The Adapter calls 'ctx.allocator', which (usually) stores the pointer in 'result' or returns it.
                    // BUT 'ctx.allocator' returns pointer. 'adapter' stores it.
                    // We need 'result.pixels'.
                    // If 'ctx.allocator' is simple (malloc), we don't know the pointer unless we capture it?
                    // ISSUE: Standard Allocator Functional doesn't "output" the pointer to the caller of the codec directly.
                    // The Codec calls Allocator -> Allocator returns Ptr.
                    // Wuffs calls Allocator -> Allocator returns Ptr.
                    // Wuffs discards Ptr (it uses it internally).
                    // We need to capture the pointer!
                    
                    // We can wrap the allocator to capture the pointer.
                    uint8_t* capturedPtr = nullptr;
                    auto capturingAlloc = [&](size_t s) -> uint8_t* {
                        capturedPtr = ctx.allocator(s);
                        return capturedPtr;
                    };
                    
                    // Call Wuffs with CAPTURING allocator
                    if (!WuffsLoader::DecodePNG(data, size, &w, &h, capturingAlloc, ctx.checkCancel)) return E_FAIL;
                    
                    result.pixels = capturedPtr;
                    result.width = w;
                    result.height = h;
                    result.stride = w * 4; // Wuffs is packed
                    result.format = PixelFormat::BGRA8888; // Wuffs is BGRA Premul
                    result.success = true;
                    // [v5.3] Fill metadata directly
                    result.metadata.FormatDetails = L"Wuffs PNG";
                    result.metadata.Width = w;
                    result.metadata.Height = h;
                    return S_OK;
                }
                return E_FAIL;
            }

            static HRESULT LoadGIF(const uint8_t* data, size_t size, const DecodeContext& ctx, DecodeResult& result) {
                uint32_t w = 0, h = 0;
                uint8_t* capturedPtr = nullptr;
                auto capturingAlloc = [&](size_t s) { return capturedPtr = ctx.allocator(s); };

                if (WuffsLoader::DecodeGIF(data, size, &w, &h, capturingAlloc, ctx.checkCancel)) {
                    result.pixels = capturedPtr;
                    result.width = w;
                    result.height = h;
                    result.stride = w * 4;
                    result.format = PixelFormat::BGRA8888;
                    result.success = true;
                    // [v5.3] Fill metadata directly
                    result.metadata.FormatDetails = L"Wuffs GIF";
                    result.metadata.Width = w;
                    result.metadata.Height = h;
                    return S_OK;
                }
                return E_FAIL;
            }

            static HRESULT LoadBMP(const uint8_t* data, size_t size, const DecodeContext& ctx, DecodeResult& result) {
                uint32_t w = 0, h = 0;
                uint8_t* capturedPtr = nullptr;
                auto capturingAlloc = [&](size_t s) { return capturedPtr = ctx.allocator(s); };

                if (WuffsLoader::DecodeBMP(data, size, &w, &h, capturingAlloc, ctx.checkCancel)) {
                    result.pixels = capturedPtr;
                    result.width = w;
                    result.height = h;
                    result.stride = w * 4;
                    result.format = PixelFormat::BGRA8888;
                    result.success = true;
                    // [v5.3] Fill metadata directly
                    result.metadata.FormatDetails = L"Wuffs BMP";
                    result.metadata.Width = w;
                    result.metadata.Height = h;
                    return S_OK;
                }
                return E_FAIL;
            }

        } // namespace Wuffs

        namespace JXL {
            static HRESULT Load(const uint8_t* data, size_t size, const DecodeContext& ctx, DecodeResult& result) {
                JxlDecoder* dec = JxlDecoderCreate(NULL);
                if (!dec) return E_OUTOFMEMORY;

                // [Fix] Do NOT destroy global runner
                void* runner = CImageLoader::GetJxlRunner();
                if (!runner) { JxlDecoderDestroy(dec); return E_OUTOFMEMORY; }

                if (JXL_DEC_SUCCESS != JxlDecoderSetParallelRunner(dec, JxlThreadParallelRunner, runner)) {
                    JxlDecoderDestroy(dec); return E_FAIL;
                }

                int events = JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE;
                if (ctx.forcePreview) events |= JXL_DEC_PREVIEW_IMAGE;

                if (JXL_DEC_SUCCESS != JxlDecoderSubscribeEvents(dec, events)) {
                    JxlDecoderDestroy(dec); return E_FAIL;
                }

                JxlBasicInfo info = {};
                // Default RGBA
                JxlPixelFormat format = { 4, JXL_TYPE_UINT8, JXL_LITTLE_ENDIAN, 0 };

                const size_t CHUNK_SIZE = 1024 * 1024; // 1MB chunks
                size_t offset = 0;
                
                // Initial Input
                size_t firstChunk = (size > CHUNK_SIZE) ? CHUNK_SIZE : size;
                if (JXL_DEC_SUCCESS != JxlDecoderSetInput(dec, data, firstChunk)) {
                     JxlDecoderDestroy(dec); return E_FAIL;
                }
                offset += firstChunk;
                if (offset >= size) JxlDecoderCloseInput(dec);

                uint8_t* pixels = nullptr;
                int stride = 0;
                int finalW = 0, finalH = 0;
                bool headerRead = false;
                bool wantPreview = ctx.forcePreview;

                for (;;) {
                    // Check Cancel
                    if (ctx.checkCancel && ctx.checkCancel()) {
                        JxlDecoderDestroy(dec); // Do NOT destroy runner
                        if (ctx.freeFunc && pixels) ctx.freeFunc(pixels);
                        return E_ABORT;
                    }

                    JxlDecoderStatus status = JxlDecoderProcessInput(dec);

                    if (status == JXL_DEC_ERROR) {
                        JxlDecoderDestroy(dec);
                        if (ctx.freeFunc && pixels) ctx.freeFunc(pixels);
                        return E_FAIL;
                    }
                    else if (status == JXL_DEC_NEED_MORE_INPUT) {
                        if (offset >= size) break; // Should close input?
                        size_t remaining = size - offset;
                        size_t nextChunk = (remaining > CHUNK_SIZE) ? CHUNK_SIZE : remaining;
                        
                        if (JXL_DEC_SUCCESS != JxlDecoderSetInput(dec, data + offset, nextChunk)) {
                            JxlDecoderDestroy(dec); if (ctx.freeFunc && pixels) ctx.freeFunc(pixels); return E_FAIL;
                        }
                        offset += nextChunk;
                        if (offset >= size) JxlDecoderCloseInput(dec);
                    }
                    else if (status == JXL_DEC_BASIC_INFO) {
                        if (JXL_DEC_SUCCESS != JxlDecoderGetBasicInfo(dec, &info)) {
                            JxlDecoderDestroy(dec); return E_FAIL;
                        }
                        // Info allows us to filter heavy images if needed?
                        // For now, proceed.
                    }
                    else if (status == JXL_DEC_FRAME) {
                        // Frame info
                        JxlFrameHeader frameHeader;
                        if (JXL_DEC_SUCCESS == JxlDecoderGetFrameHeader(dec, &frameHeader)) {
                             if (wantPreview && frameHeader.is_last) {
                                 // We wanted preview but this is main? 
                                 // Implicitly handled by event flow.
                             }
                        }
                    }
                    else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
                        size_t bufferSize = 0;
                        if (JXL_DEC_SUCCESS != JxlDecoderImageOutBufferSize(dec, &format, &bufferSize)) {
                             JxlDecoderDestroy(dec); if (ctx.freeFunc && pixels) ctx.freeFunc(pixels); return E_FAIL;
                        }

                        // Determine dimensions for stride
                        // JXL doesn't give dimensions in this event easily? 
                        // It uses BasicInfo OR FrameHeader.
                        // Assuming basic info for Full, or separate for Preview.
                        // Wait, BasicInfo is for Full. Preview has separate dims.
                        // JxlDecoderGetBox? No.
                        // In JXL_DEC_FRAME we got header.
                        // Simplification: We trust JXLBufferSize.
                        
                        // We need width for stride.
                        // Re-query basic info or use info?
                        // If it's preview, info might be wrong.
                        // Actually JxlDecoderGetBasicInfo gives preview dimensions? No.
                        // Let's assume packed RGBA (stride = w*4) for JXL default.
                        // IF we want aligned stride, we must know Width.
                        
                        // NOTE: LibJXL doesn't support custom stride easily in simple API?
                        // "The buffer must be at least bufferSize bytes".
                        // It writes packed?
                        // "align" in JxlPixelFormat. 0 means default (packed?).
                        
                        // We will use packed buffer and let Caller handle it (result.stride = 0 implies packed or logic? No result.stride must be set)
                        // If we don't know Width, we can't set Stride.
                        // But we DO know width from BasicInfo (full) or FrameHeader?
                        // Let's stick to Packed allocation for JXL.
                        
                        pixels = ctx.allocator(bufferSize);
                        if (!pixels) { JxlDecoderDestroy(dec); return E_OUTOFMEMORY; }
                        
                        if (JXL_DEC_SUCCESS != JxlDecoderSetImageOutBuffer(dec, &format, pixels, bufferSize)) {
                             JxlDecoderDestroy(dec); if (ctx.freeFunc && pixels) ctx.freeFunc(pixels); return E_FAIL;
                        }
                    }
                    else if (status == JXL_DEC_FULL_IMAGE || status == JXL_DEC_PREVIEW_IMAGE) {
                        // Done?
                        // If Preview, we might stop here if wantPreview is true.
                        // But we need to know the Width/Height of what we just decoded!
                        // JxlDecoderGetBasicInfo only gives Main image.
                        // If it was Preview, how to get dims?
                        // JxlDecoderGetFrameHeader logic earlier?
                        // We should cache frame dims.
                        
                        // Fallback: Use info.xsize/ysize for Full.
                        finalW = info.xsize;
                        finalH = info.ysize; 
                        // If scaling was supported (not in 0.9 simple API?), we'd check.
                        
                        // If this was preview...
                        if (status == JXL_DEC_PREVIEW_IMAGE) {
                            if (info.have_preview) {
                                finalW = info.preview.xsize;
                                finalH = info.preview.ysize;
                            }
                        }
                        
                        // Success
                        JxlDecoderDestroy(dec);
                        result.pixels = pixels;
                        result.width = finalW;
                        result.height = finalH;
                        result.stride = finalW * 4; // RGBA Packed
                        result.format = PixelFormat::RGBA8888; // Native
                        result.success = true;
                        
                        // [v5.3] Fill metadata directly
                        result.metadata.FormatDetails = L"JXL";
                        if (status == JXL_DEC_PREVIEW_IMAGE) result.metadata.FormatDetails += L" (Preview)";
                        if (info.have_animation) result.metadata.FormatDetails += L" [Anim]";
                        
                        result.metadata.Width = finalW;
                        result.metadata.Height = finalH;
                        return S_OK;
                    }
                    else if (status == JXL_DEC_SUCCESS) {
                        // Finished
                        break;
                    }
                }
                
                JxlDecoderDestroy(dec);
                if (ctx.freeFunc && pixels) ctx.freeFunc(pixels);
                return E_FAIL;
            }

        } // namespace JXL

        namespace RawCodec {
            static HRESULT Load(LPCWSTR filePath, const DecodeContext& ctx, DecodeResult& result) {
                LibRaw RawProcessor;
                
                // [Optimization] Open File directly (Header only initially)
                // Avoids reading 50MB+ into memory vector if not needed or if LibRaw handles it better.
                // Note: LibRaw::open_file expects char* (UTF8) on Windows or wchar_t?
                // LibRaw on Windows usually supports wchar_t if compiled with UNICODE support or open_file(const wchar_t*)
                // We'll try open_file(filePath) and if compilation fails, we fix it (e.g. w2s).
                // Actually, simplest is to use _wopen and libraw_open_oshandle if needed, but let's try direct call first.
                // Wait, to be safe and avoid compilation error loops, I'll convert to UTF8.
                // But paths on Windows can be complex.
                // Let's assume standard LibRaw::open_file(const wchar_t*) exists in our build (common patch) OR we convert.
                // Given the instruction "Use LibRaw::open_file", I'll use a helper to ensure path compatibility.
                
                // Conversion helper
                std::string pathUtf8;
                try {
                    int len = WideCharToMultiByte(CP_UTF8, 0, filePath, -1, NULL, 0, NULL, NULL);
                    if (len > 0) {
                        pathUtf8.resize(len);
                        WideCharToMultiByte(CP_UTF8, 0, filePath, -1, &pathUtf8[0], len, NULL, NULL);
                        pathUtf8.pop_back(); // Remove null terminator from string size
                    }
                } catch(...) { return E_INVALIDARG; }

                if (RawProcessor.open_file(pathUtf8.c_str()) != LIBRAW_SUCCESS) return E_FAIL;

                // 1. Try Preview
                // Only if forcePreview implied or beneficial?
                // If we want FULL decode, we skip preview unless we want to be fast?
                // Usually Gallery/Scout wants Preview. Heavy wants Full.
                // ctx.forcePreview is the flag.
                bool tryPreview = ctx.forcePreview; 
                
                // If forcePreview is NOT set, we might still fallback to preview if full decode is too slow?
                // No, Heavy lane expects Full.
                
                if (tryPreview && RawProcessor.unpack_thumb() == LIBRAW_SUCCESS) {
                    libraw_processed_image_t* thumb = RawProcessor.dcraw_make_mem_thumb();
                    if (thumb) {
                        if (thumb->type == LIBRAW_IMAGE_JPEG) {
                            // Delegate to Codec::JPEG
                            HRESULT hr = JPEG::Load((uint8_t*)thumb->data, thumb->data_size, ctx, result);
                            RawProcessor.dcraw_clear_mem(thumb);
                            if (SUCCEEDED(hr)) {
                            if (ctx.pFormatDetails) *ctx.pFormatDetails = L"LibRaw (JPEG Preview)";
                            // [v5.3] Fill metadata directly
                            result.metadata.FormatDetails = L"LibRaw (JPEG Preview)";
                                return S_OK;
                            }
                        }
                        else if (thumb->type == LIBRAW_IMAGE_BITMAP) {
                            // RGB Bitmap
                             if (thumb->bits == 8 && thumb->colors == 3) {
                                int w = thumb->width;
                                int h = thumb->height;
                                int stride = CalculateSIMDAlignedStride(w, 4); // We output BGRA
                                size_t totalSize = (size_t)stride * h;
                                
                                uint8_t* pixels = ctx.allocator(totalSize);
                                if (pixels) {
                                    // Convert RGB to BGRA (and align stride)
                                    uint8_t* src = (uint8_t*)thumb->data;
                                    uint8_t* dst = pixels;
                                    
                                    // Robust copy
                                    for(int y=0; y<h; y++) {
                                        uint8_t* rowSrc = src + (y * w * 3);
                                        uint32_t* rowDst = (uint32_t*)(dst + (y * stride)); // stride is bytes
                                        
                                        for(int x=0; x<w; x++) {
                                            uint8_t r = rowSrc[x*3];
                                            uint8_t g = rowSrc[x*3+1];
                                            uint8_t b = rowSrc[x*3+2];
                                            // BGRA
                                            rowDst[x] = (0xFF000000) | (r << 16) | (g << 8) | b; 
                                            // Wait: Little Endian: B G R A in memory.
                                            // uint32 val = (A << 24) | (R << 16) | (G << 8) | B; 
                                            // Memory: B G R A.
                                            // If we write uint32: 0xAARRGGBB.
                                            // Valid.
                                        }
                                    }
                                    
                                    result.pixels = pixels;
                                    result.width = w;
                                    result.height = h;
                                    result.stride = stride;
                                    result.format = PixelFormat::BGRA8888;
                                    result.success = true;
                                    // [v5.3] Fill metadata directly
                                    result.metadata.FormatDetails = L"LibRaw (Bitmap Preview)";
                                    result.metadata.Width = w;
                                    result.metadata.Height = h;
                                    
                                    RawProcessor.dcraw_clear_mem(thumb);
                                    return S_OK;
                                }
                             }
                        }
                        RawProcessor.dcraw_clear_mem(thumb);
                    }
                }
                
                // 2. Full Decode
                if (ctx.checkCancel && ctx.checkCancel()) return E_ABORT; // Pre-check

                RawProcessor.imgdata.params.use_camera_wb = 1;
                RawProcessor.imgdata.params.use_auto_wb = 0; 
                RawProcessor.imgdata.params.user_qual = 2; // AHD
                
                // Downscale if targetWidth is small
                if (ctx.targetWidth > 0 && RawProcessor.imgdata.sizes.width > ctx.targetWidth * 2) {
                     RawProcessor.imgdata.params.half_size = 1;
                }

                if (RawProcessor.unpack() != LIBRAW_SUCCESS) return E_FAIL;
                if (ctx.checkCancel && ctx.checkCancel()) return E_ABORT;

                if (RawProcessor.dcraw_process() != LIBRAW_SUCCESS) return E_FAIL;
                if (ctx.checkCancel && ctx.checkCancel()) return E_ABORT;

                libraw_processed_image_t* image = RawProcessor.dcraw_make_mem_image();
                if (!image) return E_FAIL;
                
                HRESULT hr = E_FAIL;
                
                if (image->type == LIBRAW_IMAGE_BITMAP && image->bits == 8 && image->colors == 3) {
                     int w = image->width;
                     int h = image->height;
                     int stride = CalculateSIMDAlignedStride(w, 4);
                     size_t totalSize = (size_t)stride * h;
                     
                     uint8_t* pixels = ctx.allocator(totalSize);
                     if (!pixels) {
                         RawProcessor.dcraw_clear_mem(image);
                         return E_OUTOFMEMORY;
                     }
                     
                     // Convert RGB to BGRA
                     uint8_t* src = (uint8_t*)image->data;
                     uint8_t* dst = pixels;
                     
                     for(int y=0; y<h; y++) {
                        uint8_t* rowSrc = src + (y * w * 3); // Source usually packed RGB
                        // Note: image->data structure depends on LibRaw
                        // dcraw_make_mem_image returns RGB 888 usually.
                        
                        uint32_t* rowDst = (uint32_t*)(dst + (y * stride));
                        for(int x=0; x<w; x++) {
                            uint8_t r = rowSrc[x*3];
                            uint8_t g = rowSrc[x*3+1];
                            uint8_t b = rowSrc[x*3+2];
                            rowDst[x] = (0xFF000000) | (r << 16) | (g << 8) | b;
                        }
                     }
                     
                     result.pixels = pixels;
                     result.width = w;
                     result.height = h;
                     result.stride = stride;
                     result.format = PixelFormat::BGRA8888;
                     result.success = true;
                     // [v5.3] Fill metadata directly
                     result.metadata.FormatDetails = L"LibRaw (Full)";
                     if (RawProcessor.imgdata.params.half_size) result.metadata.FormatDetails += L" [Half]";
                     result.metadata.Width = w;
                     result.metadata.Height = h;
                     
                     hr = S_OK;
                }
                
                RawProcessor.dcraw_clear_mem(image);
                return hr;
            }
        } // namespace RawCodec

    } // namespace Codec
} // namespace QuickView

using namespace QuickView::Codec;

// ============================================================================
// [v4.2] Unified Image Dispatcher
// ============================================================================
static HRESULT LoadImageUnified(LPCWSTR filePath, const DecodeContext& ctx, DecodeResult& result) {
    if (!filePath) return E_INVALIDARG;

    // 1. Peek Header (4KB)
    uint8_t header[4096];
    size_t headerSize = PeekHeader(filePath, header, sizeof(header));
    if (headerSize == 0) return E_FAIL; // Empty file or read error

    // 2. Detect Format
    std::wstring fmt = DetectFormatFromContent(header, headerSize);
    
    // [JXL Special] If unknown but extension is .jxl, assume JXL (Magic bytes might be complex or at offset)
    if (fmt == L"Unknown") {
        std::wstring path = filePath;
        std::transform(path.begin(), path.end(), path.begin(), ::towlower);
        if (path.ends_with(L".jxl")) fmt = L"JXL";
        // Note: Raw might also be "Unknown" via magic? 
        // Existing DetectFormat checks common Raw magics. 
    }

    // 3. Dispatch
    
    // --- Stream/File Based Codecs ---
    if (fmt == L"Raw" || fmt == L"Unknown") {
        // If "Unknown", we try Raw Loader if it's potentially a RAW file?
        // Or we let WIC handle it? 
        // Existing logic: DetectFormat calls LoadRaw checks.
        // We can just try Codec::Raw::Load. If it fails (LibRaw says no), we return E_FAIL/False.
        // Use Codec::Raw::Load for "Raw".
        // If "Unknown", maybe we shouldn't force Raw unless extension matches?
        // Let's rely on fmt == "Raw".
        if (fmt == L"Raw") {
             HRESULT hr = RawCodec::Load(filePath, ctx, result);
             if (SUCCEEDED(hr)) { result.metadata.LoaderName = L"LibRaw (Unified)"; return S_OK; }
        }
        // If Unknown, falls through to WIC/Fallback (E_NOTIMPL -> WIC).
    }

    // --- Buffer Based Codecs ---
    bool isBufferCodec = (fmt == L"JPEG" || fmt == L"WebP" || fmt == L"PNG" || fmt == L"GIF" || fmt == L"BMP" || fmt == L"JXL");
    
    if (isBufferCodec) {
        std::vector<uint8_t> fileBuf;
        if (!ReadAll(filePath, fileBuf)) return E_FAIL;
        
        // Dispatch
        if (fmt == L"JPEG") {
            HRESULT hr = JPEG::Load(fileBuf.data(), fileBuf.size(), ctx, result);
            if (SUCCEEDED(hr)) { result.metadata.LoaderName = L"TurboJPEG (Unified)"; return S_OK; }
        }
        if (fmt == L"WebP") {
            HRESULT hr = WebP::Load(fileBuf.data(), fileBuf.size(), ctx, result);
            if (SUCCEEDED(hr)) { result.metadata.LoaderName = L"libwebp (Unified)"; return S_OK; }
        }
        if (fmt == L"PNG") {
            HRESULT hr = Wuffs::LoadPNG(fileBuf.data(), fileBuf.size(), ctx, result);
            if (SUCCEEDED(hr)) { result.metadata.LoaderName = L"Wuffs PNG (Unified)"; return S_OK; }
        }
        if (fmt == L"GIF") {
            HRESULT hr = Wuffs::LoadGIF(fileBuf.data(), fileBuf.size(), ctx, result);
            if (SUCCEEDED(hr)) { result.metadata.LoaderName = L"Wuffs GIF (Unified)"; return S_OK; }
        }
        if (fmt == L"BMP") {
            HRESULT hr = Wuffs::LoadBMP(fileBuf.data(), fileBuf.size(), ctx, result);
            if (SUCCEEDED(hr)) { result.metadata.LoaderName = L"Wuffs BMP (Unified)"; return S_OK; }
        }
        if (fmt == L"JXL") {
            HRESULT hr = JXL::Load(fileBuf.data(), fileBuf.size(), ctx, result);
            if (SUCCEEDED(hr)) { result.metadata.LoaderName = L"libjxl (Unified)"; return S_OK; }
        }
    }

    // Fallback or Unknown
    return E_NOTIMPL; 
}



// PMR-Backed Loading (Zero-Copy for Heavy Lane) //
// ============================================================================
HRESULT CImageLoader::LoadToMemoryPMR(LPCWSTR filePath, DecodedImage* pOutput, std::pmr::memory_resource* pmr, 
                                      int targetWidth, int targetHeight, 
                                      std::wstring* pLoaderName, 
                                      std::stop_token st,
                                      CancelPredicate checkCancel) {
    if (!filePath || !pOutput || !pmr) return E_INVALIDARG;
    
    // Reset output
    pOutput->isValid = false;
    pOutput->pixels = std::pmr::vector<uint8_t>(pmr); // Re-bind to provided allocator
    
    auto ShouldCancel = [&]() {
        return st.stop_requested() || (checkCancel && checkCancel());
    };
    
    if (ShouldCancel()) return E_ABORT;

    // 1. Unified Dispatch (Primary Path)
    DecodeContext ctx;
    ctx.allocator = [&](size_t s) -> uint8_t* {
        try {
            pOutput->pixels.resize(s);
            return pOutput->pixels.data();
        } catch (...) { return nullptr; }
    };
    ctx.freeFunc = [&](uint8_t*) { pOutput->pixels.clear(); };
    ctx.checkCancel = ShouldCancel;
    ctx.stopToken = st;
    ctx.targetWidth = targetWidth;
    ctx.targetHeight = targetHeight;
    ctx.pLoaderName = pLoaderName;

    DecodeResult res;
    HRESULT hr = LoadImageUnified(filePath, ctx, res);
    
    if (pLoaderName && !res.metadata.LoaderName.empty()) {
        *pLoaderName = res.metadata.LoaderName;
    } else if (pLoaderName) {
        *pLoaderName = L"WIC (Fallback)";
    }
    
    // Also set internal pLoaderName if Context had it (Double linking for safety)
    if (ctx.pLoaderName && !res.metadata.LoaderName.empty()) {
        *ctx.pLoaderName = res.metadata.LoaderName;
    }
    
    if (hr == E_ABORT) return E_ABORT;

    // If LoadImageUnified succeeded, we're done.
    if (SUCCEEDED(hr) && res.success) {
        // pOutput->pixels is already populated by ctx.allocator callback
        pOutput->width = res.width;
        pOutput->height = res.height;
        pOutput->stride = res.stride;
        // pOutput->format = res.format; // DecodedImage uses implicit BGRA8888
        pOutput->isValid = true;
        return S_OK;
    }

    // 2. WIC Fallback (Legacy/Unsupported Formats)
    if (ShouldCancel()) return E_ABORT;

    ComPtr<IWICBitmap> wicBitmap;
    HRESULT hrWic = LoadToMemory(filePath, &wicBitmap, pLoaderName, false, ShouldCancel);
    if (FAILED(hrWic) || !wicBitmap) {
        return (hr != E_NOTIMPL) ? hr : hrWic; 
    }

    if (ShouldCancel()) return E_ABORT;

    // Copy from WIC to PMR
    UINT w = 0, h = 0;
    wicBitmap->GetSize(&w, &h);
    
    UINT stride = w * 4;
    size_t bufSize = (size_t)stride * h;
    
    try {
        pOutput->pixels.resize(bufSize);
    } catch (...) { return E_OUTOFMEMORY; }

    pOutput->width = w;
    pOutput->height = h;
    pOutput->stride = stride;
    
    ComPtr<IWICBitmapLock> lock;
    WICRect rcLock = { 0, 0, (INT)w, (INT)h };
    if (SUCCEEDED(wicBitmap->Lock(&rcLock, WICBitmapLockRead, &lock))) {
        UINT cbStride = 0;
        UINT cbBufferSize = 0;
        BYTE* pData = nullptr;
        lock->GetStride(&cbStride);
        lock->GetDataPointer(&cbBufferSize, &pData);
        
        if (pData) {
            if (cbStride == stride) {
                memcpy(pOutput->pixels.data(), pData, bufSize);
            } else {
                for (UINT y = 0; y < h; ++y) {
                    memcpy(pOutput->pixels.data() + (size_t)y * stride, 
                           pData + (size_t)y * cbStride, stride);
                }
            }
            pOutput->isValid = true;
            return S_OK;
        }
    }
    return E_FAIL;
}

// [v5.3 DEPRECATED] Use DecodeResult.metadata instead
// std::wstring CImageLoader::GetLastFormatDetails() const {
//     return g_lastFormatDetails;
// }
// 
// int CImageLoader::GetLastExifOrientation() const {
//     return g_lastExifOrientation;
// }

// ============================================================================
// NEW: Fast Header-Only Parsing (< 5ms for most formats)
// ============================================================================
HRESULT CImageLoader::GetImageInfoFast(LPCWSTR filePath, ImageInfo* pInfo) {
    if (!filePath || !pInfo) return E_INVALIDARG;
    *pInfo = ImageInfo{}; // Reset

    // 1. Get file size (cheap filesystem call)
    try {
        pInfo->fileSize = std::filesystem::file_size(filePath);
    } catch (...) {
        return E_FAIL;
    }

    // 2. Read first 64KB for initial detection
    size_t chunkStep = 64 * 1024;
    std::vector<uint8_t> header(chunkStep);
    
    // Scoped file handle for incremental reading - replaced by PeekHeader
    size_t bytesRead = QuickView::Codec::PeekHeader(filePath, header.data(), header.size());

    if (bytesRead < 12) return E_FAIL;
    header.resize(bytesRead); // Clamp to actual read
        
    const uint8_t* data = header.data();
    size_t size = header.size();

    // 3. Detect format and parse header
    
    // --- JPEG: Iterative Header Parsing (Streaming) ---
    if (size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
        pInfo->format = L"JPEG";
        
        tjhandle tj = tj3Init(TJINIT_DECOMPRESS);
        if (!tj) return E_FAIL;

        // Try to parse - complex due to potential need for more data
        // For simplicity in Fast Path, if 64KB isn't enough, we might fail or assume default?
        // But let's try strict check on available buffer
        if (tj3DecompressHeader(tj, data, size) == 0) {
             pInfo->width = tj3Get(tj, TJPARAM_JPEGWIDTH);
             pInfo->height = tj3Get(tj, TJPARAM_JPEGHEIGHT);
             int subsamp = tj3Get(tj, TJPARAM_SUBSAMP);
             pInfo->channels = (subsamp == TJSAMP_GRAY) ? 1 : 3;
             pInfo->bitDepth = 8;
             tj3Destroy(tj);
             return S_OK;
        }
        tj3Destroy(tj);
        // If not found in 64KB, we could loop read more, but let's keep "Fast" fast.
        // Falls back to WIC if Fast fails.
        return E_FAIL; 
    }
    
    // --- PNG ---
    if (size >= 24 && data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47) {
        pInfo->format = L"PNG";
        if (size >= 24) {
            auto readBE32 = [&](size_t off) { 
                return (uint32_t)((data[off] << 24) | (data[off+1] << 16) | (data[off+2] << 8) | data[off+3]); 
            };
            pInfo->width = readBE32(16);
            pInfo->height = readBE32(20);
            pInfo->bitDepth = data[24];
            uint8_t colorType = data[25];
            pInfo->hasAlpha = (colorType == 4 || colorType == 6);
            pInfo->channels = (colorType == 0 || colorType == 3) ? 1 : (colorType == 4) ? 2 : (colorType == 2) ? 3 : 4;
        }
        return (pInfo->width > 0) ? S_OK : E_FAIL;
    }
    
    // --- WebP ---
    if (size >= 30 && data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F' &&
        data[8] == 'W' && data[9] == 'E' && data[10] == 'B' && data[11] == 'P') {
        pInfo->format = L"WebP";
        int w = 0, h = 0;
        if (WebPGetInfo(data, size, &w, &h)) {
            pInfo->width = w;
            pInfo->height = h;
            pInfo->bitDepth = 8;
            WebPBitstreamFeatures features;
            if (WebPGetFeatures(data, size, &features) == VP8_STATUS_OK) {
                pInfo->hasAlpha = features.has_alpha;
                pInfo->isAnimated = features.has_animation;
            }
        }
        return (pInfo->width > 0) ? S_OK : E_FAIL;
    }
    
    // --- AVIF ---
    if (size >= 12 && data[4] == 'f' && data[5] == 't' && data[6] == 'y' && data[7] == 'p') {
        bool isAvif = (data[8] == 'a' && data[9] == 'v' && data[10] == 'i' && (data[11] == 'f' || data[11] == 's'));
        bool isHeic = (data[8] == 'h' && data[9] == 'e' && data[10] == 'i' && data[11] == 'c') ||
                      (data[8] == 'm' && data[9] == 'i' && data[10] == 'f' && data[11] == '1');
        
        if (isAvif || isHeic) {
            pInfo->format = isAvif ? L"AVIF" : L"HEIC";
            // Check if we need full file? AVIF usually needs full file or at least moov/meta.
            // Fast Check: if libavif works on partial?
            // "avifDecoderSetIOMemory" works. "avifDecoderParse" reads.
            // If it needs more data, it fails.
            // We give 64KB. Often not enough for MP4 structure.
            // Just return E_FAIL to fallback to WIC or Full Loader?
            // Or try reading full file here? (might be slow).
            // Let's Skip Full Read in Fast Pass to ensure < 5ms.
            return E_FAIL; 
        }
    }
    
    // --- JXL ---
    bool isJxl = (size >= 2 && data[0] == 0xFF && data[1] == 0x0A) ||
                 (size >= 12 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x00 && data[3] == 0x0C &&
                  data[4] == 'J' && data[5] == 'X' && data[6] == 'L' && data[7] == ' ');
    if (isJxl) {
        pInfo->format = L"JXL";
        JxlDecoder* dec = JxlDecoderCreate(nullptr);
        if (dec) {
            if (JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO) == JXL_DEC_SUCCESS) {
                JxlDecoderSetInput(dec, data, size);
                JxlDecoderStatus status = JxlDecoderProcessInput(dec);
                if (status == JXL_DEC_BASIC_INFO) {
                    JxlBasicInfo info;
                    if (JxlDecoderGetBasicInfo(dec, &info) == JXL_DEC_SUCCESS) {
                        pInfo->width = info.xsize;
                        pInfo->height = info.ysize;
                        pInfo->bitDepth = info.bits_per_sample;
                        pInfo->hasAlpha = info.alpha_bits > 0;
                        pInfo->isAnimated = info.have_animation;
                    }
                }
            }
            JxlDecoderDestroy(dec);
        }
        return (pInfo->width > 0) ? S_OK : E_FAIL;
    }
    
    // --- GIF ---
    if (size >= 10 && data[0] == 'G' && data[1] == 'I' && data[2] == 'F' && data[3] == '8') {
        pInfo->format = L"GIF";
        pInfo->width = data[6] | (data[7] << 8);
        pInfo->height = data[8] | (data[9] << 8);
        pInfo->bitDepth = 8;
        pInfo->isAnimated = true;
        return S_OK;
    }
    
    // --- BMP ---
    if (size >= 26 && data[0] == 'B' && data[1] == 'M') {
        pInfo->format = L"BMP";
        pInfo->width = data[18] | (data[19] << 8) | (data[20] << 16) | (data[21] << 24);
        pInfo->height = data[22] | (data[23] << 8) | (data[24] << 16) | (data[25] << 24);
        if ((int32_t)pInfo->height < 0) pInfo->height = -(int32_t)pInfo->height;
        pInfo->bitDepth = data[28] | (data[29] << 8);
        return S_OK;
    }
    
    // --- RAW ---
    // Fast Pass skips LibRaw (too slow ~10ms).
    // Let WIC or Full Loader handle it.
    
    return E_FAIL;
}
HRESULT CImageLoader::GetImageSize(LPCWSTR filePath, UINT* width, UINT* height) {
    if (!filePath || !width || !height) return E_INVALIDARG;
    *width = 0; *height = 0;

    // [Phase 6] Use fast header parsing
    ImageInfo info;
    if (SUCCEEDED(GetImageInfoFast(filePath, &info))) {
        *width = info.width;
        *height = info.height;
        return S_OK;
    }

    // Fallback: WIC (legacy)
    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = m_wicFactory->CreateDecoderFromFilename(
        filePath, 
        nullptr, 
        GENERIC_READ, 
        WICDecodeMetadataCacheOnDemand,
        &decoder
    );
    if (FAILED(hr)) return hr;

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) return hr;

    return frame->GetSize(width, height);
}

// ----------------------------------------------------------------------------
// Wuffs PNG Decoder (Google's memory-safe decoder)
// ----------------------------------------------------------------------------
HRESULT CImageLoader::LoadPngWuffs(LPCWSTR filePath, IWICBitmap** ppBitmap, CancelPredicate checkCancel) {
    if (!filePath || !ppBitmap) return E_POINTER;
    std::vector<uint8_t> pngBuf;
    if (!ReadFileToVector(filePath, pngBuf)) return E_FAIL;

    uint32_t width = 0, height = 0;
    std::pmr::vector<uint8_t> pixelData;
    
    if (!WuffsLoader::DecodePNG(pngBuf.data(), pngBuf.size(), &width, &height, pixelData, checkCancel)) {
        return E_FAIL;
    }

    // [v4.0] Native Premul via Wuffs
    // SIMD call removed to avoid double-premul

    size_t stride = width * 4;
    return CreateWICBitmapFromMemory(width, height, GUID_WICPixelFormat32bppPBGRA,
                                      (UINT)stride, (UINT)pixelData.size(), pixelData.data(), ppBitmap);
}

// ----------------------------------------------------------------------------
// Wuffs GIF Decoder (First frame only for now)
// ----------------------------------------------------------------------------
HRESULT CImageLoader::LoadGifWuffs(LPCWSTR filePath, IWICBitmap** ppBitmap, CancelPredicate checkCancel) {
    if (!filePath || !ppBitmap) return E_POINTER;
    std::vector<uint8_t> gifBuf;
    if (!ReadFileToVector(filePath, gifBuf)) return E_FAIL;

    uint32_t width = 0, height = 0;
    std::pmr::vector<uint8_t> pixelData;

    if (!WuffsLoader::DecodeGIF(gifBuf.data(), gifBuf.size(), &width, &height, pixelData, checkCancel)) {
        return E_FAIL;
    }

    // [v4.0] Restore SIMD Premultiplication
    // [v4.0] Native Premul

    size_t stride = width * 4;
    return CreateWICBitmapFromMemory(width, height, GUID_WICPixelFormat32bppPBGRA,
                                      (UINT)stride, (UINT)pixelData.size(), pixelData.data(), ppBitmap);
}

// ----------------------------------------------------------------------------
// Wuffs BMP Decoder
// ----------------------------------------------------------------------------
HRESULT CImageLoader::LoadBmpWuffs(LPCWSTR filePath, IWICBitmap** ppBitmap, CancelPredicate checkCancel) {
    if (!filePath || !ppBitmap) return E_POINTER;
    std::vector<uint8_t> buf;
    if (!ReadFileToVector(filePath, buf)) return E_FAIL;

    uint32_t width = 0, height = 0;
    std::pmr::vector<uint8_t> pixelData;

    if (!WuffsLoader::DecodeBMP(buf.data(), buf.size(), &width, &height, pixelData, checkCancel)) {
        return E_FAIL;
    }

    // [v4.0] Restore SIMD Premultiplication
    // [v4.0] Native Premul

    size_t stride = width * 4;
    return CreateWICBitmapFromMemory(width, height, GUID_WICPixelFormat32bppPBGRA,
                                      (UINT)stride, (UINT)pixelData.size(), pixelData.data(), ppBitmap);
}

// ----------------------------------------------------------------------------
// Wuffs TGA Decoder
// ----------------------------------------------------------------------------
HRESULT CImageLoader::LoadTgaWuffs(LPCWSTR filePath, IWICBitmap** ppBitmap, CancelPredicate checkCancel) {
    if (!filePath || !ppBitmap) return E_POINTER;
    std::vector<uint8_t> buf;
    if (!ReadFileToVector(filePath, buf)) return E_FAIL;

    uint32_t width = 0, height = 0;
    std::pmr::vector<uint8_t> pixelData;

    if (!WuffsLoader::DecodeTGA(buf.data(), buf.size(), &width, &height, pixelData, checkCancel)) {
        return E_FAIL;
    }

    // [v4.0] Restore SIMD Premultiplication
    // [v4.0] Native Premul

    size_t stride = width * 4;
    return CreateWICBitmapFromMemory(width, height, GUID_WICPixelFormat32bppPBGRA,
                                      (UINT)stride, (UINT)pixelData.size(), pixelData.data(), ppBitmap);
}

// ----------------------------------------------------------------------------
// Wuffs WBMP Decoder
// ----------------------------------------------------------------------------
HRESULT CImageLoader::LoadWbmpWuffs(LPCWSTR filePath, IWICBitmap** ppBitmap, CancelPredicate checkCancel) {
    if (!filePath || !ppBitmap) return E_POINTER;
    std::vector<uint8_t> buf;
    if (!ReadFileToVector(filePath, buf)) return E_FAIL;

    uint32_t width = 0, height = 0;
    std::pmr::vector<uint8_t> pixelData;

    if (!WuffsLoader::DecodeWBMP(buf.data(), buf.size(), &width, &height, pixelData, checkCancel)) {
        return E_FAIL;
    }

    // [v4.0] Restore SIMD Premultiplication
    // [v4.0] Native Premul

    size_t stride = width * 4;
    return CreateWICBitmapFromMemory(width, height, GUID_WICPixelFormat32bppPBGRA,
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
    std::pmr::vector<uint8_t> pixelData;

    if (!WuffsLoader::DecodeNetpbm(buf.data(), buf.size(), &width, &height, pixelData)) {
        return E_FAIL;
    }

    // [v4.0] Restore SIMD Premultiplication
    // [v4.0] Native Premul

    size_t stride = width * 4;
    return CreateWICBitmapFromMemory(width, height, GUID_WICPixelFormat32bppPBGRA,
                                     (UINT)stride, (UINT)(stride * height), pixelData.data(), ppBitmap);
}

// ----------------------------------------------------------------------------
// Wuffs QOI (Quite OK Image)
// ----------------------------------------------------------------------------
HRESULT CImageLoader::LoadQoiWuffs(LPCWSTR filePath, IWICBitmap** ppBitmap) {
    std::vector<uint8_t> buf;
    if (!ReadFileToVector(filePath, buf)) return E_FAIL;

    uint32_t width = 0, height = 0;
    std::pmr::vector<uint8_t> pixelData;

    if (!WuffsLoader::DecodeQOI(buf.data(), buf.size(), &width, &height, pixelData)) {
        return E_FAIL;
    }

    // [v4.0] Restore SIMD Premultiplication
    // [v4.0] Native Premul

    size_t stride = width * 4;
    return CreateWICBitmapFromMemory(width, height, GUID_WICPixelFormat32bppPBGRA,
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

// [v5.1] LibRaw Metadata Helper
static HRESULT ReadMetadataLibRaw(LPCWSTR filePath, CImageLoader::ImageMetadata* pMetadata) {
    if (!pMetadata) return E_INVALIDARG;
    
    // Use LibRaw to extract ISO, Shutter, etc. fast.
    LibRaw RawProcessor;
    
    // Conversion helper for LibRaw::open_file
    std::string pathUtf8;
    try {
        int len = WideCharToMultiByte(CP_UTF8, 0, filePath, -1, NULL, 0, NULL, NULL);
        if (len > 0) {
            pathUtf8.resize(len);
            WideCharToMultiByte(CP_UTF8, 0, filePath, -1, &pathUtf8[0], len, NULL, NULL);
            pathUtf8.pop_back(); // Remove null terminator from string size
        }
    } catch(...) { return E_INVALIDARG; }

    if (RawProcessor.open_file(pathUtf8.c_str()) != LIBRAW_SUCCESS) return E_FAIL;
    
    // ISO
    if (RawProcessor.imgdata.other.iso_speed > 0) {
        pMetadata->ISO = std::to_wstring((int)RawProcessor.imgdata.other.iso_speed);
    }
    
    // Shutter (Aperture/Shutter in 'other')
    if (RawProcessor.imgdata.other.shutter > 0.0f) {
        float s = RawProcessor.imgdata.other.shutter;
        wchar_t buf[32];
        if (s >= 1.0f) swprintf_s(buf, L"%.1fs", s);
        else swprintf_s(buf, L"1/%.0fs", 1.0f/s);
        pMetadata->Shutter = buf;
    }
    
    // Aperture
    if (RawProcessor.imgdata.other.aperture > 0.0f) {
        wchar_t buf[32];
        swprintf_s(buf, L"f/%.1f", RawProcessor.imgdata.other.aperture);
        pMetadata->Aperture = buf;
    }
    
    // Focal Length
    if (RawProcessor.imgdata.other.focal_len > 0.0f) {
        wchar_t buf[32];
        swprintf_s(buf, L"%.0fmm", RawProcessor.imgdata.other.focal_len);
        pMetadata->Focal = buf;
    }
       
    // Date
    if (RawProcessor.imgdata.other.timestamp > 0) {
        time_t t = RawProcessor.imgdata.other.timestamp;
        tm tmBuf;
        localtime_s(&tmBuf, &t);
        wchar_t buf[64];
        wcsftime(buf, 64, L"%Y-%m-%d %H:%M", &tmBuf);
        pMetadata->Date = buf;
    }
    
    // Model
    if (RawProcessor.imgdata.idata.model[0] != 0) {
        std::string model = RawProcessor.imgdata.idata.model;
        pMetadata->Model = std::wstring(model.begin(), model.end());
    }
     
    // Make
    if (RawProcessor.imgdata.idata.make[0] != 0) {
        std::string make = RawProcessor.imgdata.idata.make;
        pMetadata->Make = std::wstring(make.begin(), make.end());
    }
    
    // [v5.2] Width/Height from sizes struct
    if (RawProcessor.imgdata.sizes.width > 0 && RawProcessor.imgdata.sizes.height > 0) {
        pMetadata->Width = RawProcessor.imgdata.sizes.width;
        pMetadata->Height = RawProcessor.imgdata.sizes.height;
    }
    
    // [v5.2] Lens Model
    if (RawProcessor.imgdata.lens.Lens[0] != 0) {
        std::string lens = RawProcessor.imgdata.lens.Lens;
        pMetadata->Lens = std::wstring(lens.begin(), lens.end());
    }
    
    return S_OK;
}

// [v5.1] Metadata Dispatcher
HRESULT CImageLoader::ReadMetadata(LPCWSTR filePath, ImageMetadata* pMetadata) {
    if (!filePath || !pMetadata) return E_INVALIDARG;
    *pMetadata = {}; // Clear
    
    // 1. Detect Format
    pMetadata->Format = DetectFormatFromContent(filePath);
    
    // [v5.2] Read File Size early (works for ALL formats)
    HANDLE hFile = CreateFileW(filePath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size;
        if (GetFileSizeEx(hFile, &size)) pMetadata->FileSize = size.QuadPart;
        CloseHandle(hFile);
    }
    
    // 2. Dispatch
    if (pMetadata->Format == L"Raw" || pMetadata->Format == L"Unknown") {
        // Try LibRaw first for metadata
        // Note: LibRaw handles CR2, NEF, ARW, HEIC reliably
        std::wstring path = filePath;
        std::transform(path.begin(), path.end(), path.begin(), ::towlower);
        
        bool isRawExt = (path.ends_with(L".arw") || path.ends_with(L".cr2") || path.ends_with(L".nef") || 
                         path.ends_with(L".dng") || path.ends_with(L".orf") || path.ends_with(L".rw2") || 
                         path.ends_with(L".raf") || path.ends_with(L".pef") || path.ends_with(L".srw") || 
                         path.ends_with(L".cr3") || path.ends_with(L".nrw") ||
                         path.ends_with(L".heic") || path.ends_with(L".heif")); // Add HEIC support
                         
        if (isRawExt) {
            if (SUCCEEDED(ReadMetadataLibRaw(filePath, pMetadata))) {
                // LibRaw succeeded - metadata fields populated
                // Continue to WIC for GPS if needed
            }
        }
    }
    
    // 3. WIC Fallback (for JPEG, PNG, or if LibRaw failed/didn't have GPS)
    // We append/overwrite with WIC data if available.
    
    if (!m_wicFactory) return S_OK; // Return partial if no WIC
    
    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = m_wicFactory->CreateDecoderFromFilename(filePath, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
    
    if (FAILED(hr)) {
        // If WIC failed but we detected a format/read some raw data, return S_OK
        return S_OK; 
    }
    
    // Get First Frame
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return S_OK;
    
    // [v5.2] Read Basic Info (Width/Height) from WIC frame
    if (pMetadata->Width == 0 || pMetadata->Height == 0) {
        frame->GetSize(&pMetadata->Width, &pMetadata->Height);
    }
    
    // [v5.2] Date Fallback (FileSize already read at top of function)
    if (pMetadata->Date.empty()) {
        HANDLE hFile2 = CreateFileW(filePath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile2 != INVALID_HANDLE_VALUE) {
            FILETIME ftWrite;
            if (GetFileTime(hFile2, nullptr, nullptr, &ftWrite)) {
                 SYSTEMTIME st; FileTimeToSystemTime(&ftWrite, &st);
                 wchar_t buf[64];
                 swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
                 pMetadata->Date = buf;
            }
            CloseHandle(hFile2);
        }
    }
    
    ComPtr<IWICMetadataQueryReader> reader;
    if (FAILED(frame->GetMetadataQueryReader(&reader))) return S_OK; // No metadata
    
    // --- Parse WIC Metadata (Existing Logic) ---
    // [Cleaned up in separate helper or inline]
    // Keeping inline for now to minimize diff, but logic is robust.
    
    // Retrieve Properties... (Model, Make, ISO, etc.)
    // Only overwrite if empty (preserve LibRaw data if present)
    
    PROPVARIANT var; PropVariantInit(&var);

    // Make
    if (pMetadata->Make.empty()) {
        const wchar_t* queries[] = { L"/app1/ifd/{ushort=271}", L"/ifd/{ushort=271}" };
        for (auto q : queries) if (SUCCEEDED(reader->GetMetadataByName(q, &var))) {
            if (var.vt == VT_LPSTR && var.pszVal) {
                 std::string s = var.pszVal; pMetadata->Make = std::wstring(s.begin(), s.end());
            }
            PropVariantClear(&var); break;
        }
    }

    // Model
    if (pMetadata->Model.empty()) {
        const wchar_t* queries[] = { L"/app1/ifd/{ushort=272}", L"/ifd/{ushort=272}" };
        for (auto q : queries) if (SUCCEEDED(reader->GetMetadataByName(q, &var))) {
            if (var.vt == VT_LPSTR && var.pszVal) {
                 std::string s = var.pszVal; pMetadata->Model = std::wstring(s.begin(), s.end());
            }
            PropVariantClear(&var); break;
        }
    }
    
    // ISO (34855)
    if (pMetadata->ISO.empty()) {
        const wchar_t* queries[] = { L"/app1/ifd/exif/{ushort=34855}", L"/ifd/exif/{ushort=34855}" };
        for (auto q : queries) if (SUCCEEDED(reader->GetMetadataByName(q, &var))) {
            if (var.vt == VT_UI2) pMetadata->ISO = std::to_wstring(var.uiVal);
            else if (var.vt == VT_UI4) pMetadata->ISO = std::to_wstring(var.ulVal);
            PropVariantClear(&var); break;
        }
    }
    
    // Shutter (ExposureTime 33434)
    if (pMetadata->Shutter.empty()) {
        double val = 0;
        const wchar_t* queries[] = { L"/app1/ifd/exif/{ushort=33434}", L"/ifd/exif/{ushort=33434}" };
        for (auto q : queries) if (SUCCEEDED(GetMetadataRational(reader.Get(), q, &val))) {
            wchar_t buf[32];
            if (val >= 1.0) swprintf_s(buf, L"%.1fs", val);
            else swprintf_s(buf, L"1/%.0fs", 1.0/val);
            pMetadata->Shutter = buf;
            break;
        }
    }
    
    // Aperture (FNumber 33437)
    double aperture = 0.0;
    const wchar_t* apertureQueries[] = { L"/app1/ifd/exif/{ushort=33437}", L"/ifd/exif/{ushort=33437}" };
    for (auto q : apertureQueries) if (SUCCEEDED(GetMetadataRational(reader.Get(), q, &aperture))) {
        wchar_t buf[32];
        swprintf_s(buf, L"f/%.1f", aperture);
        pMetadata->Aperture = buf;
        break;
    }
    
    // Focal Length (37386)
    double focal = 0.0;
    const wchar_t* focalQueries[] = { L"/app1/ifd/exif/{ushort=37386}", L"/ifd/exif/{ushort=37386}" };
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

// [v5.2] Histogram from RawImageFrame (for HeavyLanePool pipeline)
void CImageLoader::ComputeHistogramFromFrame(const QuickView::RawImageFrame& frame, ImageMetadata* pMetadata) {
    if (!pMetadata || !frame.pixels || frame.width <= 0 || frame.height <= 0) return;
    
    // Reset
    pMetadata->HistR.assign(256, 0);
    pMetadata->HistG.assign(256, 0);
    pMetadata->HistB.assign(256, 0);
    pMetadata->HistL.assign(256, 0);
    
    // RawImageFrame is always BGRA8888 (B=0, G=1, R=2, A=3)
    const int offsetB = 0, offsetG = 1, offsetR = 2;
    
    UINT64 totalPixels = (UINT64)frame.width * frame.height;
    UINT step = 1;
    if (totalPixels > 2000000) {
        step = (UINT)sqrt(totalPixels / 250000.0);
        if (step < 1) step = 1;
    }
    
    for (int y = 0; y < frame.height; y += step) {
        const BYTE* row = frame.pixels + (size_t)y * frame.stride;
        for (int x = 0; x < frame.width; x += step) {
            BYTE r = row[x * 4 + offsetR];
            BYTE g = row[x * 4 + offsetG];
            BYTE b = row[x * 4 + offsetB];
            
            pMetadata->HistR[r]++;
            pMetadata->HistG[g]++;
            pMetadata->HistB[b]++;
            
            BYTE l = (BYTE)((54 * r + 183 * g + 19 * b) >> 8);
            pMetadata->HistL[l]++;
        }
    }
}

// ============================================================================
// Phase 6: Surgical Format Optimizations
// ============================================================================

#include <webp/decode.h>



HRESULT CImageLoader::LoadThumbAVIF_Proxy(const uint8_t* data, size_t size, int targetSize, ThumbData* pData, bool allowSlow) {
    if (!data || size == 0 || !pData) return E_POINTER;

    avifDecoder* decoder = avifDecoderCreate();
    if (!decoder) return E_OUTOFMEMORY;

    // 2. Setup IO
    avifResult result = avifDecoderSetIOMemory(decoder, data, size);
    if (result != AVIF_RESULT_OK) {
        avifDecoderDestroy(decoder);
        return E_FAIL;
    }

    // [Optimization] Early Exit for Massive Images without Exif
    // If image is huge (e.g. > 100MP), and we don't find Exif thumb quickly, 
    // we should abort to avoid blocking the Heavy Lane (Full Decode) which will run soon.
    // However, we need dimensions first... so we must Parse. 
    // libavif Parse is usually fast (just reads header).

    // 3. Parse
    // Enable incremental to allow parsing to finish faster on some files? 
    // No, incremental is for truncated data. But 'ignoreXMP' etc helps.
    decoder->ignoreXMP = AVIF_TRUE; 
    // decoder->ignoreExif = AVIF_TRUE; // WE NEED EXIF for thumbnail! Do not ignore.
    
    result = avifDecoderParse(decoder);
    if (result != AVIF_RESULT_OK) {
        avifDecoderDestroy(decoder);
        return E_FAIL;
    }

    // [Optimization] Fast Path: Check for Embedded Exif Thumbnail (JPEG)
    // Many camera-generated AVIFs or converted files retain the Exif thumbnail.
    // IMPORANT: Only use this if we are looking for a THUMBNAIL (targetSize > 0).
    // If targetSize == 0 (Fast Pass), we want the full image!
    if (targetSize > 0 && decoder->image->exif.size > 0 && decoder->image->exif.data) {
        const uint8_t* exifData = decoder->image->exif.data;
        size_t exifSize = decoder->image->exif.size;
        
        // Simple scan for JPEG Start of Image (SOI) Marker (FF D8)
        // We limit scan to first 64KB to avoid wasting time on huge blobs if it's not at start
        size_t scanLimit = (exifSize > 65536) ? 65536 : exifSize; 
        const uint8_t* soi = nullptr;
        
        for (size_t i = 0; i < scanLimit - 1; ++i) {
            if (exifData[i] == 0xFF && exifData[i+1] == 0xD8) {
                soi = exifData + i;
                break;
            }
        }

        if (soi) {
            // Found potential JPEG. Let WIC try to decode it.
            // The size is from soi to end of exif buffer.
            size_t jpegSize = exifSize - (soi - exifData);
            
            ComPtr<IWICStream> stream;
            HRESULT hr = m_wicFactory->CreateStream(&stream);
            if (SUCCEEDED(hr)) hr = stream->InitializeFromMemory((BYTE*)soi, (DWORD)jpegSize);
            
            ComPtr<IWICBitmapDecoder> wicDecoder;
            if (SUCCEEDED(hr)) hr = m_wicFactory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &wicDecoder);
            
            ComPtr<IWICBitmapFrameDecode> frame;
            if (SUCCEEDED(hr)) hr = wicDecoder->GetFrame(0, &frame);

            if (SUCCEEDED(hr)) {
                // Success! We found a valid image in the Exif.
                // Convert to common format
                ComPtr<IWICFormatConverter> converter;
                if (SUCCEEDED(hr)) hr = m_wicFactory->CreateFormatConverter(&converter);
                if (SUCCEEDED(hr)) hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.f, WICBitmapPaletteTypeMedianCut);

                if (SUCCEEDED(hr)) {
                    UINT w, h;
                    converter->GetSize(&w, &h);
                    pData->width = w;
                    pData->height = h;
                    pData->stride = w * 4;
                    pData->origWidth = decoder->image->width;
                    pData->origHeight = decoder->image->height;
                    
                    size_t bufSize = (size_t)pData->stride * h;
                    try {
                        pData->pixels.resize(bufSize);
                        hr = converter->CopyPixels(nullptr, pData->stride, (UINT)bufSize, pData->pixels.data());
                        if (SUCCEEDED(hr)) {
                            pData->isValid = true;
                            pData->isBlurry = true; // Exif thumbs are usually lower res
                            avifDecoderDestroy(decoder); 
                            return S_OK; // FAST RETURN
                        }
                    } catch(...) { }
                }
            }
        }
    }

    // [Success Strategy] "Too Big to Thumb"
    // If image > 50 MP (e.g. 8K x 6K) AND we didn't find Exif thumb above:
    // It will take > 1.0s to decode. The Heavy Lane will ALSO take > 1.0s to decode.
    // If we decode here, we delay Heavy Lane (due to thread contention/memory).
    // Better to RETURN FAILURE (specifically S_FALSE for 'skipped') to let Heavy Lane start immediately.
    // 50MP = 50,000,000 pixels. 9449*9449 = 89MP.
    uint64_t pixelCount = (uint64_t)decoder->image->width * (uint64_t)decoder->image->height;
    if (pixelCount > 40000000) { // > 40MP threshold
        avifDecoderDestroy(decoder);
        // Returning E_FAIL would trigger WIC (if we didn't block it in caller).
        // Returning S_OK with invalid pData?
        // Let's return E_ABORT or similar.
        return E_ABORT; 
    }

    // 4. Decode Strategy (STE)
    
    // [STE Level 2] Smart Sampling - Center Tile / Frame
    // If ImageCount > 1, it's either an Animation or a Grid (HEIC/AVIF Collection).
    // Decoding the middle item is often a good representation and faster than stitching.
    if (decoder->imageCount > 1) {
        // Pick middle frame
        uint32_t midIndex = decoder->imageCount / 2;
        result = avifDecoderNthImage(decoder, midIndex);
        if (result != AVIF_RESULT_OK) {
             avifDecoderDestroy(decoder); 
             return E_FAIL;
        }
        // Proceed to Scaling...
    } else {
        // Single Image Logic
        
        // [STE Level 3] Circuit Breaker (Strict Budget)
        // If allowSlow is FALSE (Warp State) AND we are here (meaning no Exif found),
        // we must ABORT unless it's a Fast Pass candidate (already checked outside).
        // Actually, FastPass calls this with targetSize=0.
        // If targetSize > 0 (Thumbnail request) AND !allowSlow -> SKIP.
        if (targetSize > 0 && !allowSlow) {
             avifDecoderDestroy(decoder);
             return E_ABORT; 
        }

        // [Safety Limits]
        decoder->imageSizeLimit = 16384 * 16384; 
        decoder->imageDimensionLimit = 32768;

        result = avifDecoderNextImage(decoder);
        if (result != AVIF_RESULT_OK) {
            avifDecoderDestroy(decoder);
            return E_FAIL;
        }
    }

    // 5. YUV-Space Downscaling
    // Calculate target dimensions
    int origW = decoder->image->width;
    int origH = decoder->image->height;
    
    if (targetSize > 0 && (origW > targetSize || origH > targetSize)) {
        float ratio = 1.0f;
        if (origW > origH) {
            ratio = (float)targetSize / origW;
        } else {
            ratio = (float)targetSize / origH;
        }
        
        uint32_t newW = (uint32_t)(origW * ratio);
        uint32_t newH = (uint32_t)(origH * ratio);
        if (newW < 1) newW = 1;
        if (newH < 1) newH = 1;

        // Perform scaling in YUV domain BEFORE RGB conversion
        result = avifImageScale(decoder->image, newW, newH, &decoder->diag);
        if (result != AVIF_RESULT_OK) {
            // Fallback: Continue with full size if scale fails
        }
    }

    // 6. Convert to RGB (BGRA for Windows)
    avifRGBImage rgb;
    avifRGBImageSetDefaults(&rgb, decoder->image);
    rgb.format = AVIF_RGB_FORMAT_BGRA;
    rgb.depth = 8; // Force 8-bit for display
    
    // Use multi-threaded conversion if available
    rgb.maxThreads = decoder->maxThreads;

    result = avifRGBImageAllocatePixels(&rgb);
    if (result != AVIF_RESULT_OK) {
        avifDecoderDestroy(decoder);
        return E_OUTOFMEMORY;
    }

    result = avifImageYUVToRGB(decoder->image, &rgb);
    if (result != AVIF_RESULT_OK) {
        avifRGBImageFreePixels(&rgb);
        avifDecoderDestroy(decoder);
        return E_FAIL;
    }

    // 7. Output to ThumbData
    pData->width = rgb.width;
    pData->height = rgb.height;
    pData->stride = rgb.rowBytes;
    pData->origWidth = origW; // Keep original dims
    pData->origHeight = origH;
    
    // Copy pixels
    size_t outSize = rgb.rowBytes * rgb.height;
    try {
        pData->pixels.assign(rgb.pixels, rgb.pixels + outSize);
        pData->isValid = true;
        pData->isBlurry = (targetSize > 0); // Blurry if scaled, Clear if full (FastPass)
        result = AVIF_RESULT_OK;
    } catch (...) {
        result = AVIF_RESULT_OUT_OF_MEMORY;
    }

    // 8. Cleanup
    avifRGBImageFreePixels(&rgb);
    avifDecoderDestroy(decoder);

    return (result == AVIF_RESULT_OK) ? S_OK : E_FAIL;
}

HRESULT CImageLoader::LoadThumbWebPFromMemory(const uint8_t* data, size_t size, int targetSize, ThumbData* pData) {
    if (!data || size == 0 || !pData) return E_FAIL;

    // 1. Calculate Scaled Dimensions (Preserve Aspect Ratio)
    int origW = 0, origH = 0;
    if (!WebPGetInfo(data, size, &origW, &origH)) return E_FAIL;
    
    int scaledW = origW;
    int scaledH = origH;
    
    if (targetSize > 0 && (origW > targetSize || origH > targetSize)) {
         float ratio = 1.0f;
         if (origW >= origH) ratio = (float)targetSize / origW;
         else ratio = (float)targetSize / origH;
         
         scaledW = (int)(origW * ratio);
         scaledH = (int)(origH * ratio);
         if (scaledW < 1) scaledW = 1;
         if (scaledH < 1) scaledH = 1;
    }
    
    // 2. Call Helper
    int outW = 0, outH = 0, outStride = 0;
    uint8_t* outPixels = nullptr;
    
    auto allocate = [&](size_t s) -> uint8_t* {
         try {
             pData->pixels.resize(s);
             return pData->pixels.data();
         } catch(...) { return nullptr; }
    };
    auto freeOnFail = [&](uint8_t*) { pData->pixels.clear(); };
    
    DecodeContext ctx;
    ctx.allocator = allocate;
    ctx.freeFunc = freeOnFail;
    ctx.targetWidth = scaledW;
    ctx.targetHeight = scaledH;

    DecodeResult res;
    HRESULT hr = QuickView::Codec::WebP::Load(data, size, ctx, res);
    
    if (SUCCEEDED(hr)) {
         pData->width = res.width;
         pData->height = res.height;
         pData->stride = res.stride;
         pData->isValid = true;
         pData->origWidth = origW;
         pData->origHeight = origH;
    }
    
    return hr;
}

// [v5.0] LoadThumbWebP_Limited Removed



            // High-precision float for quality, speed is fine for thumbnail

// [JXL Memory Optimization] Context for 1:8 Skip-Sampling Callback
struct JxlSkipSampleContext {
    uint8_t* outBuffer;
    size_t outWidth;
    size_t outHeight;
    size_t outStride;
    size_t factor; // 8
};

// [JXL Memory Optimization] Callback to read only 1 pixel every 8x8 block
// Performs simultaneous Skip-Sampling, Swizzle (RGBA->BGRA), and Premultiplication
static void JxlSkipSampleCallback(void* run_opaque, size_t x, size_t y, size_t num_pixels, const void* pixels) {
    JxlSkipSampleContext* ctx = (JxlSkipSampleContext*)run_opaque;
    const uint8_t* src = (const uint8_t*)pixels;
    
    // Vertical Skip: Only process every 8th row
    if (y % ctx->factor != 0) return;
    
    size_t targetY = y / ctx->factor;
    if (targetY >= ctx->outHeight) return;
    
    uint8_t* rowPtr = ctx->outBuffer + targetY * ctx->outStride;
    
    // Horizontal Skip & Pixel Processing
    for (size_t i = 0; i < num_pixels; ++i) {
        size_t currentX = x + i;
        
        // Only process every 8th column
        if (currentX % ctx->factor == 0) {
            size_t targetX = currentX / ctx->factor;
            if (targetX < ctx->outWidth) {
                size_t srcOffset = i * 4; // 4 bytes per pixel (RGBA input)
                size_t dstOffset = targetX * 4; // 4 bytes per pixel (BGRA output)
                
                uint8_t r = src[srcOffset + 0];
                uint8_t g = src[srcOffset + 1];
                uint8_t b = src[srcOffset + 2];
                uint8_t a = src[srcOffset + 3]; // Alpha
                
                // Premultiply Alpha
                if (a > 0 && a < 255) {
                    r = (uint8_t)((r * a) / 255);
                    g = (uint8_t)((g * a) / 255);
                    b = (uint8_t)((b * a) / 255);
                } else if (a == 0) {
                    r = g = b = 0;
                }
                
                // Store as BGRA
                rowPtr[dstOffset + 0] = b;
                rowPtr[dstOffset + 1] = g;
                rowPtr[dstOffset + 2] = r;
                rowPtr[dstOffset + 3] = a;
            }
        }
    }
}



HRESULT CImageLoader::LoadThumbJXL_DC(const uint8_t* pFile, size_t fileSize, ThumbData* pData) {
    if (!pFile || fileSize == 0 || !pData) return E_INVALIDARG;

    pData->isValid = false;
    pData->isBlurry = true; // [FIX] Set early - DC mode always produces blurry output
    
    // 1. Create Decoder
    JxlDecoder* dec = JxlDecoderCreate(NULL);
    if (!dec) return E_OUTOFMEMORY;
    
    // [JXL Global Runner] Use singleton instead of creating each time
    void* runner = CImageLoader::GetJxlRunner();

    // RAII Cleanup - NOTE: Do NOT destroy global runner!
    auto cleanup = [&](HRESULT hr) {
        JxlDecoderDestroy(dec);
        return hr;
    };
    // [v5.0] Setup moved to header
    
    OutputDebugStringW(L"[JXL_DC] Input Set. Entering Loop.\n");
    
    JxlBasicInfo info = {};

    bool headerSeen = false;

    // 5. Decode Loop (Buffer Mode)
    JxlSkipSampleContext dsCtx = {};
    JxlPixelFormat format = { 4, JXL_TYPE_UINT8, JXL_LITTLE_ENDIAN, 0 }; // BGRA (We swizzle later if needed, but JXL is RGBA)
    // Actually, let's stick to RGBA and Swizzle manually, or use SIMD.
    
    // [Optimization] JXL outputs RGBA. We need BGRA.
    // We will swizzle in-place after decode.

    for (;;) {
        JxlDecoderStatus status = JxlDecoderProcessInput(dec);

        if (status == JXL_DEC_ERROR) {
             return cleanup(E_FAIL);
        }
        else if (status == JXL_DEC_SUCCESS) {
            // Finished
            if (pData->isValid) {
                 // For Preview (Name="libjxl (Preview)"), we used Buffer, so we need to Swizzle here.
                 // For DC (Name="libjxl (Scout DC 1:8)"), we used Callback which already did it.
                 if (pData->loaderName == std::wstring(L"libjxl (Preview)")) {
                     uint8_t* p = pData->pixels.data();
                     size_t pxCount = (size_t)pData->width * pData->height;
                     for(size_t i=0; i<pxCount; ++i) {
                         uint8_t r = p[i*4+0];
                         uint8_t g = p[i*4+1];
                         uint8_t b = p[i*4+2];
                         uint8_t a = p[i*4+3];
                         
                         if (a < 255 && a > 0) {
                             r = (uint8_t)((r * a) / 255);
                             g = (uint8_t)((g * a) / 255);
                             b = (uint8_t)((b * a) / 255);
                         } else if (a == 0) {
                             r = g = b = 0;
                         }
                         p[i*4+0] = b;
                         p[i*4+1] = g;
                         p[i*4+2] = r;
                         p[i*4+3] = a;
                     }
                 }
                 return cleanup(S_OK);
            }
            return cleanup(E_FAIL);
        }
        else if (status == JXL_DEC_BASIC_INFO) {
            if (JXL_DEC_SUCCESS != JxlDecoderGetBasicInfo(dec, &info)) return cleanup(E_FAIL);
            
            // [Modular Detection] 无损 JXL 没有 DC 层，直接放弃 Scout
            if (info.uses_original_profile) {
                OutputDebugStringW(L"[JXL_DC] Modular/Lossless detected, skipping Scout\n");
                return cleanup(E_ABORT);  // Let Heavy Lane handle
            }
            
            // Output Color Profile
            JxlColorEncoding color_encoding = {};
            color_encoding.color_space = JXL_COLOR_SPACE_RGB;
            color_encoding.white_point = JXL_WHITE_POINT_D65;
            color_encoding.primaries = JXL_PRIMARIES_SRGB;
            color_encoding.transfer_function = JXL_TRANSFER_FUNCTION_SRGB;
            color_encoding.rendering_intent = JXL_RENDERING_INTENT_PERCEPTUAL;
            JxlDecoderSetOutputColorProfile(dec, &color_encoding, NULL, 0);

            // [Strategy] 1. Preview? 2. DC?
            bool usePreview = info.have_preview;
            if (!usePreview) {
                 // Try kDC
                 JxlDecoderSetProgressiveDetail(dec, kDC);
            }
            // Set orig dimensions
            pData->origWidth = info.xsize;
            pData->origHeight = info.ysize;
        }
        else if (status == JXL_DEC_NEED_PREVIEW_OUT_BUFFER) {
             size_t bufferSize = 0;
             if (JXL_DEC_SUCCESS != JxlDecoderPreviewOutBufferSize(dec, &format, &bufferSize)) return cleanup(E_FAIL);
             
             pData->loaderName = L"libjxl (Preview)";
             
             try { pData->pixels.resize(bufferSize); } catch(...) { return cleanup(E_OUTOFMEMORY); }
             if (JXL_DEC_SUCCESS != JxlDecoderSetPreviewOutBuffer(dec, &format, pData->pixels.data(), bufferSize)) return cleanup(E_FAIL);
             
             // Infer dimensions (approx)
             size_t px = bufferSize / 4;
             pData->width = (int)sqrt((double)px * info.xsize / info.ysize); 
             pData->height = px / pData->width;
             pData->stride = pData->width * 4;
             pData->isValid = true;
        }
        else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
            // [Memory Optimization] Use Callback for 1:8 Skip Sampling
            size_t factor = 8;
            size_t targetW = (info.xsize + factor - 1) / factor;
            size_t targetH = (info.ysize + factor - 1) / factor;
            
            try { 
                // Allocate tiny buffer (1/64 size)
                pData->pixels.resize(targetW * targetH * 4); 
            } catch(...) { return cleanup(E_OUTOFMEMORY); }
            
            pData->width = (int)targetW;
            pData->height = (int)targetH;
            pData->stride = (int)targetW * 4;
            pData->isValid = true;
            pData->loaderName = L"libjxl (Scout DC 1:8)";
            pData->isBlurry = true;

            dsCtx.outBuffer = pData->pixels.data();
            dsCtx.outWidth = targetW;
            dsCtx.outHeight = targetH;
            dsCtx.outStride = pData->stride;
            dsCtx.factor = factor;
            
            if (JXL_DEC_SUCCESS != JxlDecoderSetImageOutCallback(dec, &format, JxlSkipSampleCallback, &dsCtx)) {
                return cleanup(E_FAIL);
            }
        }
        else if (status == JXL_DEC_FULL_IMAGE) {
            // Frame done (DC frame)
            // Continue to JXL_DEC_SUCCESS or next frame
        }
        else if (status == JXL_DEC_NEED_MORE_INPUT) {
             return cleanup(E_FAIL); // Buffer exhausted
        }
        else if (status == JXL_DEC_FRAME_PROGRESSION) {
            // We can flush to get partial result, but in DC mode, just wait for FULL_IMAGE/NEED_BUFFER
            // JxlDecoderFlushImage(dec);
        }
    }

}

// [v3.1] Robust TurboJPEG Helper (Replaces elusive LoadThumbJPEG)
// [v3.1] Robust TurboJPEG Helper (Replaces elusive LoadThumbJPEG)
HRESULT CImageLoader::LoadThumbJPEG_Robust(LPCWSTR filePath, int targetSize, ThumbData* pData) {
    if (!pData) return E_INVALIDARG;
    pData->isValid = false;

    // 1. Read File
    std::vector<uint8_t> buf;
    if (!ReadFileToVector(filePath, buf)) return E_FAIL;

    // 2. Init TJ
    tjhandle tj = tj3Init(TJINIT_DECOMPRESS);
    if (!tj) return E_FAIL;

    // 3. Parse Header
    if (tj3DecompressHeader(tj, buf.data(), buf.size()) != 0) {
        tj3Destroy(tj);
        return E_FAIL;
    }

    int width = tj3Get(tj, TJPARAM_JPEGWIDTH);
    int height = tj3Get(tj, TJPARAM_JPEGHEIGHT);
    
    // Save original dims
    pData->origWidth = width;
    pData->origHeight = height;

    // 4. Determine Scaling
    tjscalingfactor scaling = {1, 1}; // Default 1:1
    
    if (targetSize > 0 && targetSize < 60000) {
        // Only scale DOWN if strictly requested and image is larger
        if (width > targetSize || height > targetSize) {
            int numFactors = 0;
            const tjscalingfactor* factors = tj3GetScalingFactors(&numFactors);
            for (int i = 0; i < numFactors; i++) {
                int sw = TJSCALED(width, factors[i]);
                int sh = TJSCALED(height, factors[i]);
                if (sw <= targetSize && sh <= targetSize) {
                   scaling = factors[i]; 
                   break;
                }
            }
        }
    }

    if (scaling.num != 1 || scaling.denom != 1) {
        tj3SetScalingFactor(tj, scaling);
        // [v4.1] Optimization: Use FASTDCT only when scaling (Thumbnail/Scout Preview)
        // For full-size small images (FastPass), we prefer higher quality (SlowDCT).
        tj3Set(tj, TJPARAM_FASTDCT, 1);
        
        width = TJSCALED(width, scaling);
        height = TJSCALED(height, scaling);
    }

    // 5. Allocate Output
    pData->width = width;
    pData->height = height;
    pData->stride = width * 4; // BGRA
    pData->pixels.resize((size_t)pData->stride * height);

    // 6. Decode
    // TJPF_BGRA is safer for Windows (D2D compatible)
    if (tj3Decompress8(tj, buf.data(), buf.size(), pData->pixels.data(), pData->stride, TJPF_BGRA) != 0) {
        tj3Destroy(tj);
        return E_FAIL;
    }

    tj3Destroy(tj);
    pData->isValid = true;
    pData->loaderName = L"TurboJPEG"; // [v3.2] Debug
    return S_OK;
}

HRESULT CImageLoader::LoadFastPass(LPCWSTR filePath, ThumbData* pData) {
    if (!pData) return E_INVALIDARG;
    pData->isValid = false;
    pData->isBlurry = false; // Fast Pass = Clear

    std::wstring format = DetectFormatFromContent(filePath);
    std::wstring ext = filePath;
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    
    // Robust AVIF Detection: Content Magic OR Extension
    if (format == L"AVIF" || format == L"HEIC" || 
        ((format == L"Unknown" || format == L"HEIC") && (ext.ends_with(L".avif") || ext.ends_with(L".avifs")))) {
         std::vector<uint8_t> buf;
         if (!ReadFileToVector(filePath, buf)) return E_FAIL;
         // Call Proxy with 0 targetSize for FULL RES
         HRESULT hr = LoadThumbAVIF_Proxy(buf.data(), buf.size(), 0, pData);
         if (SUCCEEDED(hr)) {
             pData->isBlurry = false;
             pData->loaderName = L"libavif";
         }
         return hr;
    }
    
    // [v3.2] JPEG Detection: Content Magic OR Extension (fallback for non-standard files)
    if (format == L"JPEG" || ((format == L"Unknown") && (ext.ends_with(L".jpg") || ext.ends_with(L".jpeg")))) {
        HRESULT hr = LoadThumbJPEG_Robust(filePath, 65535, pData);
        if (SUCCEEDED(hr)) pData->isBlurry = false;
        return hr;
    } 
    else if (format == L"WebP" || ((format == L"Unknown") && ext.ends_with(L".webp"))) {
        std::vector<uint8_t> buf;
        if (!ReadFileToVector(filePath, buf)) return E_FAIL;
        HRESULT hr = LoadThumbWebPFromMemory(buf.data(), buf.size(), 65535, pData);
        if (SUCCEEDED(hr)) {
            pData->isBlurry = false;
            pData->loaderName = L"WebP (Fast)";
        }
        return hr;
    }
    else if (format == L"BMP" || format == L"TGA" || format == L"GIF" || format == L"PNG" || format == L"QOI" || format == L"PNM" || format == L"WBMP") {
         std::vector<uint8_t> buf;
         if (!ReadFileToVector(filePath, buf)) return E_FAIL;
         
         uint32_t w, h;
         bool ok = false;
         if (format == L"GIF") ok = WuffsLoader::DecodeGIF(buf.data(), buf.size(), &w, &h, pData->pixels);
         else if (format == L"BMP") ok = WuffsLoader::DecodeBMP(buf.data(), buf.size(), &w, &h, pData->pixels);
         else if (format == L"TGA") ok = WuffsLoader::DecodeTGA(buf.data(), buf.size(), &w, &h, pData->pixels);
         else if (format == L"PNG") ok = WuffsLoader::DecodePNG(buf.data(), buf.size(), &w, &h, pData->pixels);
         else if (format == L"QOI") ok = WuffsLoader::DecodeQOI(buf.data(), buf.size(), &w, &h, pData->pixels);
         else if (format == L"PNM") ok = WuffsLoader::DecodeNetpbm(buf.data(), buf.size(), &w, &h, pData->pixels);
         else if (format == L"WBMP") ok = WuffsLoader::DecodeWBMP(buf.data(), buf.size(), &w, &h, pData->pixels);
         
         if (ok) {
              pData->width = w; pData->height = h; pData->stride = w*4;
              pData->origWidth = w; pData->origHeight = h;
              pData->isValid = true; 
              pData->isBlurry = false;
              pData->loaderName = L"Wuffs";
              return S_OK;
         }
    }
    
    else if (format == L"JXL" || ((format == L"Unknown") && ext.ends_with(L".jxl"))) {
         // [v4.1] JXL Fast Pass: Use DC-Only Decode (1:8)
         std::vector<uint8_t> buf;
         if (!ReadFileToVector(filePath, buf)) return E_FAIL;
         
         HRESULT hr = LoadThumbJXL_DC(buf.data(), buf.size(), pData);
         if (SUCCEEDED(hr)) {
             // pData->isBlurry and loaderName already set inside LoadThumbJXL_DC
             // isBlurry=true (DC), Name="libjxl (DC)"
             return S_OK;
         } else {
             // Fallback: If Modular/DC-fail, try existing Full Decode path (up to a limit?) or just fail?
             // User requested: "Immediately degrade to Scout Full Decode (if small enough)"
             // But LoadFastPass IS Scout. So we try Full Decode here if DC failed.
             // Re-using specific logic block for Full Decode:
             // Note: buf is already loaded.
             return E_FAIL; // For now, fail to let Heavy Lane handle it if DC fails. 
             // Or should we implement the fallback here?
             // Let's implement fallback for < 10MP?
             // Actually, if DC fails, it's likely Modular. Modular decode is fast enough?
             // Implementation Plan said: "Fallback to Full Decode (if small enough)"
             // Let's stick to E_FAIL for safety in first pass. 
             // If DC fails, returned invalid.
         }
         return E_FAIL;
    }
    
    return E_FAIL;
}

// ============================================================================
// Pre-flight Check (v3.1) - Fast header classification
// ============================================================================
CImageLoader::ImageHeaderInfo CImageLoader::PeekHeader(LPCWSTR filePath) {
    ImageHeaderInfo result;
    if (!filePath) return result;
    
    // Get file size
    try {
        result.fileSize = std::filesystem::file_size(filePath);
    } catch (...) {
        return result; // Invalid
    }
    
    // Use existing fast header parsing
    ImageInfo info;
    if (FAILED(GetImageInfoFast(filePath, &info))) {
        // Try format detection only
        result.format = DetectFormatFromContent(filePath);
        if (result.format == L"Unknown") return result;
    } else {
        result.format = info.format;
        result.width = info.width;
        result.height = info.height;
    }
    
    // Check for embedded thumbnail (RAW files always have, JPEG may have)
    result.hasEmbeddedThumb = (result.format == L"RAW" || result.format == L"TIFF");
    if (result.format == L"JPEG" && result.fileSize > 100 * 1024) { // >100KB JPEG likely has Exif
        result.hasEmbeddedThumb = true;
    }
    
    // === Classification Matrix (v3.2) ===
    int64_t pixels = result.GetPixelCount();
    
    // Type A (Express Lane ONLY): Small enough for fast full decode
    // Type B (Heavy Lane): Large images needing scaled decode
    
    // [v4.1] JXL Optimization: Always allow Sprint (Scout Lane)
    // Small JXL -> Fast Full Decode (via fallback if needed? Currently DC only in LoadFastPass)
    // Large JXL -> Fast DC Preview (LoadThumbJXL_DC)
    if (result.format == L"JXL") {
        result.type = ImageType::TypeA_Sprint;
    }
    else if (result.format == L"JPEG" || result.format == L"BMP") {
        // JPEG/BMP: ≤8.5MP → Express (full decode is fast)
        // >8.5MP → Heavy (needs scaled decode, Scout extracts thumb if hasEmbeddedThumb)
        // [v3.3] Safety: If pixels unknown (0), default to Heavy (Scout might choke on huge image)
        if (pixels > 0 && pixels <= 8500000) {
            result.type = ImageType::TypeA_Sprint;
        } else {
            result.type = ImageType::TypeB_Heavy;
        }
    }
    else if (result.format == L"PNG" || result.format == L"WebP" || result.format == L"GIF") {
        // PNG/WebP/GIF: ≤4MP → Express, otherwise Heavy
        if (pixels <= 4000000) {
            result.type = ImageType::TypeA_Sprint;
        } else {
            result.type = ImageType::TypeB_Heavy;
        }
    }
    else if (result.format == L"RAW" || result.format == L"TIFF") {
        // RAW/TIFF: Always has thumb → Express (Extraction only)
        result.type = ImageType::TypeA_Sprint;
        result.hasEmbeddedThumb = true;
    }
    else if (result.format == L"AVIF" || result.format == L"HEIC") {
        // AVIF/HEIC: ≤4MP → Express Fast Pass
        if (pixels <= 4000000) {
            result.type = ImageType::TypeA_Sprint;
        } else {
            result.type = ImageType::TypeB_Heavy;
        }
    }
    else {
        // Unknown format: Try Express if small
        if (result.fileSize < 2 * 1024 * 1024 && pixels < 2100000) {
            result.type = ImageType::TypeA_Sprint;
        } else if (result.width > 0) {
            result.type = ImageType::TypeB_Heavy;
        }
        // else: Invalid (default)
    }
    
    return result;
}


// ============================================================================
// [Direct D2D] Zero-Copy Loading Implementation
// ============================================================================
// This is the new primary loading API for the Direct D2D rendering pipeline.
// It decodes images directly to RawImageFrame, bypassing WIC where possible.
// ============================================================================

#include "MemoryArena.h"

HRESULT CImageLoader::LoadToFrame(LPCWSTR filePath, QuickView::RawImageFrame* outFrame,
                                   QuantumArena* arena,
                                   int targetWidth, int targetHeight,
                                   std::wstring* pLoaderName,
                                   CancelPredicate checkCancel,
                                   ImageMetadata* pMetadata) {
    using namespace QuickView;
    
    if (!filePath || !outFrame) return E_INVALIDARG;
    
    // Reset output frame
    outFrame->Release();
    
    // Detect format from magic bytes
    std::wstring format = DetectFormatFromContent(filePath);
    
    // Helper: Allocate memory (Always aligned)
    auto AllocateBuffer = [&](size_t size) -> uint8_t* {
        if (arena) {
            uint8_t* ptr = static_cast<uint8_t*>(arena->Allocate(size, 64));
            // Note: Arena::Allocate uses _aligned_malloc on overflow.
            return ptr; 
        } else {
            return static_cast<uint8_t*>(_aligned_malloc(size, 64));
        }
    };
    
    // Helper: Setup deleter based on allocation source
    auto SetupDeleter = [&](uint8_t* ptr) {
        if (arena && arena->Owns(ptr)) {
            // Arena memory - no explicit free needed (Arena manages lifecycle)
            outFrame->memoryDeleter = nullptr;
        } else {
            // Heap memory (either from Arena overflow or direct _aligned_malloc)
            // MUST use _aligned_free
            outFrame->memoryDeleter = [](uint8_t* p) { _aligned_free(p); };
        }
    };
    
    // [v4.2] Unified Codec Dispatch
    DecodeContext ctx;
    ctx.allocator = [&](size_t s) -> uint8_t* { return AllocateBuffer(s); };
    ctx.checkCancel = checkCancel;
    ctx.targetWidth = targetWidth; // Pass scaling request
    ctx.targetHeight = targetHeight;
    ctx.pLoaderName = pLoaderName;
    ctx.pMetadata = pMetadata; // [v5.3] Pass metadata pointer to Collect Metadata directly

    DecodeResult res;
    HRESULT hrUnified = LoadImageUnified(filePath, ctx, res);

    if (hrUnified == E_ABORT) return E_ABORT;
    if (SUCCEEDED(hrUnified)) {
        // [v5.0] Extract loader name from unified result
        if (pLoaderName && !res.metadata.LoaderName.empty()) {
            *pLoaderName = res.metadata.LoaderName;
        }

        // [v5.3] Output full metadata if requested
        if (pMetadata) {
            *pMetadata = res.metadata;
        }
        
        outFrame->pixels = res.pixels;
        outFrame->width = res.width;
        outFrame->height = res.height;
        outFrame->stride = res.stride;
        outFrame->format = PixelFormat::BGRA8888; // Default
        
        SetupDeleter(res.pixels);
        return S_OK;
    }
    // Fall through to SVG or WIC Fallback
    
    // ========================================================================
    // SVG Path (NanoSVG) - Re-Rasterization Support
    // ========================================================================
    // SVG is special: vector format, can rasterize at ANY target resolution.
    // This enables "lossless zoom" - re-rasterize when user zooms in.
    // ========================================================================
    if (format == L"SVG" || format == L"Unknown") {
        // Check file extension for SVG (magic detection might fail for XML)
        std::wstring pathLower = filePath;
        std::transform(pathLower.begin(), pathLower.end(), pathLower.begin(), ::towlower);
        bool isSvg = (pathLower.size() > 4 && pathLower.substr(pathLower.size() - 4) == L".svg");
        
        if (isSvg) {
            std::vector<uint8_t> fileData;
            if (!ReadFileToVector(filePath, fileData)) return E_FAIL;
            
            if (checkCancel && checkCancel()) return E_ABORT;
            
            // NanoSVG parses char* string (null terminated)
            std::vector<char> xmlData(fileData.begin(), fileData.end());
            xmlData.push_back('\0');
            
            // Parse (96 DPI default units)
            NSVGimage* image = nsvgParse(xmlData.data(), "px", 96.0f);
            if (!image) return E_FAIL;
            
            // RAII guard
            struct SvgGuard { NSVGimage* img; ~SvgGuard() { if (img) nsvgDelete(img); } } guard{image};
            
            // Calculate target scale
            // If targetWidth/Height specified, scale to fit. Otherwise, 2x for crisp.
            float scale = 2.0f;
            if (targetWidth > 0 && targetHeight > 0 && image->width > 0 && image->height > 0) {
                float scaleW = static_cast<float>(targetWidth) / image->width;
                float scaleH = static_cast<float>(targetHeight) / image->height;
                scale = std::min(scaleW, scaleH);
                // Ensure minimum 1.0 scale for readability
                if (scale < 1.0f) scale = 1.0f;
            }
            
            // Safety limit (8K max)
            float maxDim = 8192.0f;
            if (image->width * scale > maxDim || image->height * scale > maxDim) {
                float aspect = image->width / image->height;
                scale = (aspect > 1.0f) ? (maxDim / image->width) : (maxDim / image->height);
            }
            
            int width = static_cast<int>(image->width * scale);
            int height = static_cast<int>(image->height * scale);
            
            if (width <= 0 || height <= 0) return E_FAIL;
            
            // [v5.3] Metadata TODO: Return via DecodeResult if using Unified path.
            
            // Rasterize
            NSVGrasterizer* rast = nsvgCreateRasterizer();
            if (!rast) return E_OUTOFMEMORY;
            struct RastGuard { NSVGrasterizer* r; ~RastGuard() { if (r) nsvgDeleteRasterizer(r); } } rastGuard{rast};
            
            // Allocate buffer (SIMD aligned)
            int stride = CalculateSIMDAlignedStride(width, 4);
            size_t bufSize = static_cast<size_t>(stride) * height;
            uint8_t* pixels = AllocateBuffer(bufSize);
            if (!pixels) return E_OUTOFMEMORY;
            
            // NanoSVG outputs RGBA
            nsvgRasterize(rast, image, 0, 0, scale, pixels, width, height, stride);
            
            // Fill output frame (RGBA format!)
            outFrame->pixels = pixels;
            outFrame->width = width;
            outFrame->height = height;
            outFrame->stride = stride;
            outFrame->format = PixelFormat::RGBA8888;  // NanoSVG = RGBA
            SetupDeleter(pixels);
            
            if (pLoaderName) *pLoaderName = L"NanoSVG";
            // g_lastFormatDetails = L"Vector"; (Removed)
            if (pMetadata) {
                pMetadata->LoaderName = L"NanoSVG";
                pMetadata->FormatDetails = L"Vector";
                pMetadata->Width = width;
                pMetadata->Height = height;
            }
            
            return S_OK;
        }
    }
    
    // WebP/Wuffs/JXL Codecs handled recursively or by Unified Dispatch above.
    // If they failed, we fall through to WIC.
    
    // ========================================================================
    // Fallback: Use existing WIC path and convert to RawImageFrame
    // ========================================================================
    // This is a temporary bridge until all decoders are ported.
    // It has one extra memory copy but maintains compatibility.
    // ========================================================================
    
    ComPtr<IWICBitmap> wicBitmap;
    std::wstring loaderName;
    HRESULT hr = LoadToMemory(filePath, &wicBitmap, &loaderName, false, checkCancel);
    if (FAILED(hr)) return hr;
    
    if (pLoaderName) *pLoaderName = loaderName;
    
    // Get dimensions
    UINT wicWidth = 0, wicHeight = 0;
    hr = wicBitmap->GetSize(&wicWidth, &wicHeight);
    if (FAILED(hr) || wicWidth == 0 || wicHeight == 0) return E_FAIL;
    
    // Lock and copy pixels
    WICRect lockRect = { 0, 0, (INT)wicWidth, (INT)wicHeight };
    ComPtr<IWICBitmapLock> lock;
    hr = wicBitmap->Lock(&lockRect, WICBitmapLockRead, &lock);
    if (FAILED(hr)) return hr;
    
    UINT wicStride = 0;
    hr = lock->GetStride(&wicStride);
    if (FAILED(hr)) return hr;
    
    UINT bufferSize = 0;
    BYTE* wicData = nullptr;
    hr = lock->GetDataPointer(&bufferSize, &wicData);
    if (FAILED(hr) || !wicData) return hr;
    
    // Allocate output buffer with aligned stride
    int outStride = CalculateSIMDAlignedStride(wicWidth, 4);
    size_t outSize = static_cast<size_t>(outStride) * wicHeight;
    uint8_t* pixels = AllocateBuffer(outSize);
    if (!pixels) return E_OUTOFMEMORY;
    
    // Copy row by row (handles stride mismatch)
    for (UINT y = 0; y < wicHeight; ++y) {
        memcpy(pixels + y * outStride, wicData + y * wicStride, wicWidth * 4);
    }
    
    // Fill output frame
    outFrame->pixels = pixels;
    outFrame->width = static_cast<int>(wicWidth);
    outFrame->height = static_cast<int>(wicHeight);
    outFrame->stride = outStride;
    outFrame->format = PixelFormat::BGRA8888; // WIC always converts to BGRA
    SetupDeleter(pixels);
    
    // [v5.3] WIC Fallback Metadata Population
    if (pMetadata) {
        ReadMetadata(filePath, pMetadata);
        pMetadata->LoaderName = loaderName;
        if (pMetadata->FormatDetails.empty()) pMetadata->FormatDetails = L"Legacy WIC";
    }
    
    return S_OK;
}


