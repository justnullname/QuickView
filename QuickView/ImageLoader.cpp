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

// [JXL Global Runner] Static singleton initialization
void* CImageLoader::s_jxlRunner = nullptr;
std::mutex CImageLoader::s_jxlRunnerMutex;

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
static int g_lastExifOrientation = 1; // EXIF Orientation (1-8, 1=Normal)

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
                         g_lastFormatDetails = buf;
                     } else {
                         g_lastFormatDetails = subsamp;
                     }
                     
                     // Read EXIF Orientation
                     g_lastExifOrientation = ReadJpegExifOrientation(jpegBuf.data(), jpegBuf.size());
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
    

    
    // MANUAL PREMULTIPLY (Required for D2D/WIC compatibility to checkboard correctly)
    SIMDUtils::PremultiplyAlpha_BGRA(pData->pixels.data(), pData->width, pData->height, pData->stride);

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

HRESULT CImageLoader::LoadThumbnail(LPCWSTR filePath, int targetSize, ThumbData* pData, bool allowSlow) {
    if (!pData) return E_INVALIDARG;
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

    if (format == L"JXL") {
        // JXL DC layer decoding still requires parsing the entire file stream.
        // For 4K+ JXL (typically > 2MB), this can take 100-200ms.
        // Scout Lane limit: 3MB for JXL.
        if (!allowSlow && fsize > 3 * 1024 * 1024) {
            OutputDebugStringW(L"[Loader] JXL > 3MB, Aborting Scout\n");
            return E_ABORT;
        }
        
        OutputDebugStringW(L"[Loader] Reading JXL for Scout...\n");
        std::vector<uint8_t> buf;
        if (ReadFileToVector(filePath, buf)) {
            HRESULT hr = LoadThumbJXL_DC(buf.data(), buf.size(), pData);
             if (SUCCEEDED(hr)) OutputDebugStringW(L"[Loader] JXL Scout Success\n");
             else { wchar_t b[64]; swprintf_s(b, L"[Loader] JXL Scout Failed hr=0x%X\n", hr); OutputDebugStringW(b); }
             return hr;
        }
        OutputDebugStringW(L"[Loader] ReadFile Failed\n");
    }
    else if (format == L"AVIF") {
         // [STE] Circuit Breaker: AVIF decode is CPU-heavy, limit to 5MB for Scout
         if (!allowSlow && fsize > 5 * 1024 * 1024) return E_ABORT; 

         std::vector<uint8_t> buf;
         if (ReadFileToVector(filePath, buf)) {
             HRESULT hr = LoadThumbAVIF_Proxy(buf.data(), buf.size(), targetSize, pData, allowSlow);
             // CRITICAL: Do NOT fall back to WIC for AVIF/HEIC if libavif failed. 
             // Windows WIC is notoriously slow/buggy or hangs on complex AV1 streams.
             // If our optimized loader fails, accept defeat to avoid "Infinite Spinner".
             if (FAILED(hr)) return hr; 
             return S_OK;
         }
         return E_FAIL; // Read failed
    }
    else if (format == L"WebP") {
         // [STE] Circuit Breaker: WebP > 5MB is likely large animated or high-res
         if (!allowSlow && fsize > 5 * 1024 * 1024) return E_ABORT;

         std::vector<uint8_t> buf;
         // Heavy Lane (allowSlow=true) can proceed (assuming dimensions are safe)
         if (ReadFileToVector(filePath, buf)) {
             int timeout = allowSlow ? 200 : 15; // Increased timeout for Heavy Lane WebP
             return LoadThumbWebP_Limited(buf.data(), buf.size(), targetSize, pData, timeout);
         }
    }
    else if (format == L"PNG") {
        // [STE] Scout Lane: Abort immediately (PNG decode is slow for large files)
        // FastPass (in ImageEngine) already handled small PNGs.
        if (!allowSlow) return E_ABORT; 

        // Heavy Lane: Use Wuffs for fast PNG decode
        std::vector<uint8_t> buf;
        if (!ReadFileToVector(filePath, buf)) return E_FAIL;
        
        // Limit to 10MB for Heavy Lane (larger files are rare edge cases)
        if (buf.size() > 10 * 1024 * 1024) return E_ABORT;

        uint32_t w, h;
        if (WuffsLoader::DecodePNG(buf.data(), buf.size(), &w, &h, pData->pixels)) {
             // Limit to 4K for thumbnail use
             if (w > 3840 || h > 2160) {
                 pData->pixels.clear();
                 return E_ABORT; 
             }
             pData->width = w; pData->height = h; pData->stride = w*4;
             pData->origWidth = w; pData->origHeight = h;
             pData->fileSize = buf.size();
             pData->isValid = true; pData->isBlurry = true; 
             return S_OK;
        }
    }
    else if (format == L"GIF") {
        // [v4.1] GIF Support: Use Wuffs for fast decode (First Frame Only)
        // Scout Lane: Allow small GIFs (<2MB), Heavy Lane: Allow all
        if (!allowSlow && fsize > 2 * 1024 * 1024) return E_ABORT;
        
        std::vector<uint8_t> buf;
        if (!ReadFileToVector(filePath, buf)) return E_FAIL;
        
        uint32_t w, h;
        if (WuffsLoader::DecodeGIF(buf.data(), buf.size(), &w, &h, pData->pixels)) {
            // Sanity check for extremely large GIFs
            if (w > 4096 || h > 4096) {
                pData->pixels.clear();
                return E_ABORT;
            }
            pData->width = w; pData->height = h; pData->stride = w * 4;
            pData->origWidth = w; pData->origHeight = h;
            pData->fileSize = buf.size();
            pData->isValid = true; pData->isBlurry = true;
            pData->loaderName = L"Wuffs GIF";
            return S_OK;
        }
    }
    // 1. Optimized Path (JPEG)
    if (format == L"JPEG") {
        // [STE] Scout Lane Circuit Breaker for Large JPEG
        if (!allowSlow && fsize > 5 * 1024 * 1024) {
            return E_ABORT;
        }
        
        // Read file ONCE
        std::vector<uint8_t> buf;
        if (!ReadFileToVector(filePath, buf)) {
            return E_FAIL;
        }
        
        // [STE Level 0] Try EXIF Thumbnail First (< 5ms)
        // Most camera JPEGs have 160x120 or 320x240 embedded thumbnails
        if (!allowSlow) {
            PreviewExtractor::ExtractedData exData;
            if (PreviewExtractor::ExtractFromJPEG(buf.data(), buf.size(), exData) && exData.IsValid()) {
                // Found EXIF thumbnail - decode it (tiny, < 2ms)
                if (SUCCEEDED(LoadThumbJPEGFromMemory(exData.pData, exData.size, targetSize, pData))) {
                    pData->fileSize = buf.size();
                    pData->isBlurry = true; // EXIF thumb is low-res
                    return S_OK;
                }
            }
        }
        
        // Fallback: Full scaled decode (reuse buffer, no double IO)
        if (SUCCEEDED(LoadThumbJPEGFromMemory(buf.data(), buf.size(), targetSize, pData))) {
            pData->fileSize = buf.size();
            return S_OK;
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
        
        // [STE] Complete RAW Extension List (LibRaw supported formats)
        bool isRaw = (
            e == L".arw" ||  // Sony
            e == L".cr2" ||  // Canon
            e == L".cr3" ||  // Canon (new)
            e == L".nef" ||  // Nikon
            e == L".nrw" ||  // Nikon (compact)
            e == L".dng" ||  // Adobe DNG
            e == L".orf" ||  // Olympus
            e == L".rw2" ||  // Panasonic
            e == L".raf" ||  // Fujifilm
            e == L".pef" ||  // Pentax
            e == L".srw" ||  // Samsung
            e == L".erf" ||  // Epson
            e == L".kdc" ||  // Kodak
            e == L".dcr" ||  // Kodak
            e == L".mrw" ||  // Minolta
            e == L".3fr" ||  // Hasselblad
            e == L".fff" ||  // Hasselblad
            e == L".iiq" ||  // Phase One
            e == L".rwl" ||  // Leica
            e == L".raw" ||  // Generic RAW
            e == L".x3f"     // Sigma
        );
        
        // [STE] PSD/PSB (Photoshop)
        bool isPsd = (e == L".psd" || e == L".psb");
        
        // [STE] HEIC/HEIF (handled separately by AVIF path earlier, but keep for extraction)
        bool isHeic = (e == L".heic" || e == L".heif");
        
        // [STE Level 1] RAW: Use LibRaw for embedded JPEG extraction (FAST)
        // LibRaw's unpack_thumb() is the correct way to get RAW previews.
        if (isRaw) {
            std::vector<uint8_t> rawBuf;
            if (!ReadFileToVector(filePath, rawBuf)) {
                return E_ABORT;
            }
            
            LibRaw RawProcessor;
            if (RawProcessor.open_buffer(rawBuf.data(), rawBuf.size()) != LIBRAW_SUCCESS) {
                return E_ABORT;
            }
            
            // Try to find and extract the best thumbnail
            libraw_processed_image_t* thumb = nullptr;
            int err = 0;
            
            // Strategy 1: Try thumbs_list (multiple thumbnails available in some formats)
            int thumbCount = RawProcessor.imgdata.thumbs_list.thumbcount;
            if (thumbCount > 0) {
                // Find the largest thumbnail by area
                int bestIdx = -1;
                int bestArea = 0;
                for (int i = 0; i < thumbCount && i < LIBRAW_THUMBNAIL_MAXCOUNT; i++) {
                    auto& ti = RawProcessor.imgdata.thumbs_list.thumblist[i];
                    int area = ti.twidth * ti.theight;
                    if (area > bestArea) {
                        bestArea = area;
                        bestIdx = i;
                    }
                }
                
                if (bestIdx >= 0) {
                    if (RawProcessor.unpack_thumb_ex(bestIdx) == LIBRAW_SUCCESS) {
                        thumb = RawProcessor.dcraw_make_mem_thumb(&err);
                    }
                }
            }
            
            // Strategy 2: Fallback to default unpack_thumb() (gets largest available)
            if (!thumb) {
                if (RawProcessor.unpack_thumb() == LIBRAW_SUCCESS) {
                    thumb = RawProcessor.dcraw_make_mem_thumb(&err);
                }
            }
            
            // Process extracted thumbnail
            if (thumb && thumb->data_size > 0) {
                if (thumb->type == LIBRAW_IMAGE_JPEG) {
                    // 情况 A: 标准 JPEG (90% 的情况)
                    HRESULT hr = LoadThumbJPEGFromMemory(thumb->data, thumb->data_size, targetSize, pData);
                    pData->fileSize = rawBuf.size();
                    RawProcessor.dcraw_clear_mem(thumb);
                    if (SUCCEEDED(hr)) {
                        return S_OK;
                    }
                } else if (thumb->type == LIBRAW_IMAGE_BITMAP) {
                    // 情况 B: 未压缩位图 (RGB/RGBA)
                    pData->origWidth = thumb->width;
                    pData->origHeight = thumb->height;
                    pData->width = thumb->width;
                    pData->height = thumb->height;
                    pData->stride = thumb->width * 4;
                    pData->fileSize = rawBuf.size();
                    
                    size_t pixelCount = (size_t)thumb->width * thumb->height;
                    pData->pixels.resize(pixelCount * 4);
                    uint8_t* dst = pData->pixels.data();
                    
                    if (thumb->bits == 8 && thumb->colors == 3) {
                        // RGB 8-bit (常见)
                        const uint8_t* src = thumb->data;
                        for (size_t i = 0; i < pixelCount; i++) {
                            dst[i*4+0] = src[i*3+2]; // B
                            dst[i*4+1] = src[i*3+1]; // G
                            dst[i*4+2] = src[i*3+0]; // R
                            dst[i*4+3] = 255;        // A
                        }
                        pData->isValid = true;
                    } else if (thumb->bits == 8 && thumb->colors == 4) {
                        // RGBA 8-bit
                        const uint8_t* src = thumb->data;
                        for (size_t i = 0; i < pixelCount; i++) {
                            dst[i*4+0] = src[i*4+2]; // B
                            dst[i*4+1] = src[i*4+1]; // G
                            dst[i*4+2] = src[i*4+0]; // R
                            dst[i*4+3] = src[i*4+3]; // A
                        }
                        pData->isValid = true;
                    } else if (thumb->bits == 16 && thumb->colors == 3) {
                        // RGB 16-bit -> 转换为 8-bit
                        const uint16_t* src = (const uint16_t*)thumb->data;
                        for (size_t i = 0; i < pixelCount; i++) {
                            dst[i*4+0] = (uint8_t)(src[i*3+2] >> 8); // B
                            dst[i*4+1] = (uint8_t)(src[i*3+1] >> 8); // G
                            dst[i*4+2] = (uint8_t)(src[i*3+0] >> 8); // R
                            dst[i*4+3] = 255;                         // A
                        }
                        pData->isValid = true;
                    } else {
                        // 未知格式 - 尝试当作 RGB 8-bit 处理
                        if (thumb->data_size >= pixelCount * 3) {
                            const uint8_t* src = thumb->data;
                            for (size_t i = 0; i < pixelCount; i++) {
                                dst[i*4+0] = src[i*3+2]; // B
                                dst[i*4+1] = src[i*3+1]; // G
                                dst[i*4+2] = src[i*3+0]; // R
                                dst[i*4+3] = 255;        // A
                            }
                            pData->isValid = true;
                        }
                    }
                    
                    if (pData->isValid) {
                        pData->isBlurry = true;
                        RawProcessor.dcraw_clear_mem(thumb);
                        return S_OK;
                    }
                }
                // 情况 C: 其他格式 (PPM 等) -> 放弃
                RawProcessor.dcraw_clear_mem(thumb);
            }
            
            // LibRaw extraction failed - show icon, never full decode in Scout
            return E_ABORT;
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
    // Pass stride to ensure correct row alignment
    SIMDUtils::PremultiplyAlpha_BGRA(output, width, height, stride);

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
struct my_error_mgr {
  struct jpeg_error_mgr pub;
  jmp_buf setjmp_buffer;
};

METHODDEF(void) my_error_exit(j_common_ptr cinfo) {
  my_error_mgr* myerr = (my_error_mgr*)cinfo->err;
  longjmp(myerr->setjmp_buffer, 1);
}

// Low-level decode with scanline cancellation
static HRESULT LoadJpegDeep(const uint8_t* pBuf, size_t bufSize, 
                            CImageLoader::DecodedImage* pOut, 
                            int targetW, int targetH, 
                            std::wstring* pLoaderName,
                            CImageLoader::CancelPredicate checkCancel) 
{
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
    
    if (targetW > 0 && targetH > 0) {
        // Calculate scale factor (M/8)
        while (cinfo.scale_denom < 8) {
             int nextDenom = cinfo.scale_denom * 2;
             int scaledW = (cinfo.image_width + nextDenom - 1) / nextDenom;
             int scaledH = (cinfo.image_height + nextDenom - 1) / nextDenom;
             
             if (scaledW < targetW || scaledH < targetH) break; 
             cinfo.scale_denom = nextDenom;
        }
    }
    
    // Output BGRA (libjpeg-turbo extension)
    cinfo.out_color_space = JCS_EXT_BGRA; 
    
    jpeg_start_decompress(&cinfo);
    
    int w = cinfo.output_width;
    int h = cinfo.output_height;
    UINT stride = w * 4;
    
    try {
        pOut->pixels.resize((size_t)stride * h);
    } catch(...) {
        jpeg_abort_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        return E_OUTOFMEMORY;
    }
    
    pOut->width = w;
    pOut->height = h;
    pOut->stride = stride;
    
    while (cinfo.output_scanline < cinfo.output_height) {
        // [Deep Check]
        if (checkCancel && checkCancel()) {
            jpeg_abort_decompress(&cinfo);
            jpeg_destroy_decompress(&cinfo);
            return E_ABORT;
        }
        
        JSAMPROW row_pointer[1];
        row_pointer[0] = &pOut->pixels[(size_t)cinfo.output_scanline * stride];
        jpeg_read_scanlines(&cinfo, row_pointer, 1);
    }
    
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    
    if (pLoaderName) {
        *pLoaderName = L"FASTJPEG (Deep Cancel)";
        if (cinfo.output_width < cinfo.image_width) *pLoaderName += L" [Scaled]";
    }
    
    pOut->isValid = true;
    return S_OK;
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
    
    // Convenience lambda combining all cancel sources
    auto ShouldCancel = [&]() {
        return st.stop_requested() || (checkCancel && checkCancel());
    };
    
    if (ShouldCancel()) return E_ABORT;
    
    // Detect format
    std::wstring detectedFmt = DetectFormatFromContent(filePath);
    
    // --- JPEG Path (Highest Priority / Most Common) ---
    if (detectedFmt == L"JPEG") {
        // Use PMR vector for Input Buffer (Scratchpad optimization)
        std::pmr::vector<uint8_t> jpegBuf(pmr);
        if (!ReadFileToPMR(filePath, jpegBuf, st)) return E_ABORT; // Use Chunked Read
        
        if (st.stop_requested()) return E_ABORT;
        
        // [v4.0] DEEP CANCELLATION: Replace TurboJPEG monolithic call with Scanline Loop
        // This allows aborting decode instantly if user navigates away.
        // Direct PMR Loading + IDCT Scaling + Cancel Check
        HRESULT hr = LoadJpegDeep(jpegBuf.data(), jpegBuf.size(), 
                                 pOutput, targetWidth, targetHeight, 
                                 pLoaderName, ShouldCancel);
                                 
        if (hr == E_ABORT) return E_ABORT;
        if (SUCCEEDED(hr)) return hr;


    }

    // --- WebP Path (New Optimized PMR with Scaling) ---
    // --- WebP Path (New Optimized PMR with Scaling + Deep Cancel) ---
    if (detectedFmt == L"WebP") {
         std::pmr::vector<uint8_t> webpBuf(pmr);
         if (!ReadFileToPMR(filePath, webpBuf, st)) return E_ABORT;

         WebPDecoderConfig config;
         if (WebPInitDecoderConfig(&config)) {
             if (WebPGetFeatures(webpBuf.data(), webpBuf.size(), &config.input) == VP8_STATUS_OK) {
                  int w = config.input.width;
                  int h = config.input.height;

                  // [Optimization] WebP Scaling Disabled (Final Decision)
                  // Smart Scaling (Lossy=Scale, Lossless=NoScale) was considered, 
                  // but user testing revealed Phase A (Scaled) took 2100ms+ while Phase B (Full) took 1800ms.
                  // Scaling provides NO speed benefit for this use case, only artifacting and delay.
                  // Strategy: Direct Full Decode (Atomic + Multithreaded).
                  
                  /*
                  if (config.input.format == 1) { // 1 = Lossy (VP8)
                      if (targetWidth > 0 && targetHeight > 0 && (w > targetWidth || h > targetHeight)) {
                          config.options.use_scaling = 1;
                         // ...
                      }
                  }
                  */

                  // [v4.2] Optimization:
                  // 1. Use MODE_bgrA for Native Premultiplied Alpha (Matches D2D PBGRA) - Correctness + Speed
                  // 2. Disable Fancy Upsampling (Nearest Neighbor for Chroma) - Faster decode
                  config.output.colorspace = MODE_bgrA; 
                  config.options.no_fancy_upsampling = 1;
                  
                  config.output.is_external_memory = 1; // We allocate!
                  config.options.use_threads = 1;       // Enable multithreading

                  UINT stride = w * 4;
                  size_t bufSize = (size_t)stride * h;
                  
                  try {
                      pOutput->pixels.resize(bufSize);
                      config.output.u.RGBA.rgba = pOutput->pixels.data();
                      config.output.u.RGBA.stride = stride;
                      config.output.u.RGBA.size = bufSize;

                      // [Performance] Revert to Atomic Decode (WebPDecode) to match v2.1 speed (1829ms vs 2069ms)
                      // Deep Cancel (Incremental) adds ~13% overhead. Speed is priority.
                      if (WebPDecode(webpBuf.data(), webpBuf.size(), &config) == VP8_STATUS_OK) {
                           pOutput->width = w;
                           pOutput->height = h;
                           pOutput->stride = stride;
                           pOutput->isValid = true;
                           if (pLoaderName) {
                               *pLoaderName = L"WebP (PMR)";
                               if (config.options.use_scaling) *pLoaderName += L" [Scaled]";
                           }
                           WebPFreeDecBuffer(&config.output);
                           return S_OK;
                      }
                  } catch(...) { }
             }
             WebPFreeDecBuffer(&config.output);
         }
    }
    
    // --- PNG Path (Wuffs - Safety & Speed) ---
    if (detectedFmt == L"PNG") {
        std::pmr::vector<uint8_t> pngBuf(pmr);
        if (!ReadFileToPMR(filePath, pngBuf, st)) return E_ABORT;

        uint32_t w = 0, h = 0;
        // Direct decode into PMR (Zero Copy)
        if (WuffsLoader::DecodePNG(pngBuf.data(), pngBuf.size(), &w, &h, pOutput->pixels, ShouldCancel)) {
             UINT stride = w * 4;
             pOutput->width = w;
             pOutput->height = h;
             pOutput->stride = stride;
             pOutput->isValid = true;
             
             if (pLoaderName) *pLoaderName = L"Wuffs PNG (PMR ZeroCopy)";
             return S_OK;
        }
    
    // --- JXL Path (Streaming Deep Cancel + PMR) ---
    // [v4.1] Robust check: Magic Bytes OR .jxl extension (Priority to Extension)
    bool isJXL = (detectedFmt == L"JXL");
    if (!isJXL) {
        std::wstring path = filePath;
        std::transform(path.begin(), path.end(), path.begin(), ::towlower);
        if (path.ends_with(L".jxl")) isJXL = true;
    }

    if (isJXL) {
        std::pmr::vector<uint8_t> jxlBuf(pmr);
        if (!ReadFileToPMR(filePath, jxlBuf, st)) return E_ABORT;

        JxlDecoder* dec = JxlDecoderCreate(NULL);
        if (!dec) return E_OUTOFMEMORY;

        // [JXL Global Runner] Use singleton
        void* runner = CImageLoader::GetJxlRunner();
        if (!runner) { JxlDecoderDestroy(dec); return E_OUTOFMEMORY; }
        
        if (JXL_DEC_SUCCESS != JxlDecoderSetParallelRunner(dec, JxlThreadParallelRunner, runner)) {
            JxlDecoderDestroy(dec); return E_FAIL; // NOTE: Do NOT destroy global runner
        }

        if (JXL_DEC_SUCCESS != JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE)) {
            JxlDecoderDestroy(dec); return E_FAIL;
        }
        
        JxlBasicInfo info = {};
        JxlPixelFormat format = { 4, JXL_TYPE_UINT8, JXL_LITTLE_ENDIAN, 0 }; // RGBA
        
        bool success = false;
        const size_t CHUNK_SIZE = 1024 * 1024; // 1MB chunks to force yielding for cancellation
        size_t offset = 0;
        size_t totalSize = jxlBuf.size();
        
        // Streaming Loop
        // We feed chunks to force the decoder to return control periodically.
        
        // Initial Input (First Chunk)
        size_t firstChunk = (totalSize > CHUNK_SIZE) ? CHUNK_SIZE : totalSize;
        if (JXL_DEC_SUCCESS != JxlDecoderSetInput(dec, jxlBuf.data(), firstChunk)) {
             JxlDecoderDestroy(dec); return E_FAIL; // NOTE: Do NOT destroy global runner
        }
        offset += firstChunk;
        
        bool inputClosed = (offset >= totalSize);
        if (inputClosed) JxlDecoderCloseInput(dec);

        for (;;) {
            // Check Cancel on every iteration (roughly every chunk or state change)
            if (ShouldCancel()) {
                JxlThreadParallelRunnerDestroy(runner);
                JxlDecoderDestroy(dec);
                return E_ABORT;
            }

            JxlDecoderStatus status = JxlDecoderProcessInput(dec);
            
            if (status == JXL_DEC_ERROR) {
                break;
            }
            else if (status == JXL_DEC_NEED_MORE_INPUT) {
                if (offset >= totalSize) {
                    // We gave everything but it wants more? Unexpected for valid file.
                    break;
                }
                size_t remaining = totalSize - offset;
                size_t nextChunk = (remaining > CHUNK_SIZE) ? CHUNK_SIZE : remaining;
                
                // Release previous input is automatic when SetInput is called?
                // JXL API requires us to release? No, SetInput replaces.
                
                // CRITICAL: We must provide the pointer to the NEXT chunk.
                if (JXL_DEC_SUCCESS != JxlDecoderSetInput(dec, jxlBuf.data() + offset, nextChunk)) {
                    break;
                }
                offset += nextChunk;
                if (offset >= totalSize) JxlDecoderCloseInput(dec);
            }
            else if (status == JXL_DEC_BASIC_INFO) {
                if (JXL_DEC_SUCCESS != JxlDecoderGetBasicInfo(dec, &info)) break;
                
                // Allocate Output (PMR)
                // JXL is RGBA. WIC needs 4 channels.
                JxlColorEncoding color_encoding = {};
                color_encoding.color_space = JXL_COLOR_SPACE_RGB;
                color_encoding.white_point = JXL_WHITE_POINT_D65;
                color_encoding.primaries = JXL_PRIMARIES_SRGB;
                color_encoding.transfer_function = JXL_TRANSFER_FUNCTION_SRGB;
                color_encoding.rendering_intent = JXL_RENDERING_INTENT_PERCEPTUAL;
                JxlDecoderSetOutputColorProfile(dec, &color_encoding, NULL, 0);

                UINT w = info.xsize;
                UINT h = info.ysize;
                UINT stride = w * 4;
                try {
                    pOutput->pixels.resize((size_t)stride * h);
                } catch(...) { break; } // OOM
                
                pOutput->width = w;
                pOutput->height = h;
                pOutput->stride = stride;
                
                // Using RGBA, but we are BGRA environment?
                // TurboJPEG uses BGRA.
                // JXL default is RGBA.
                // We should check if JXL supports BGRA output.
                // JxlPixelFormat does NOT have a BGR type. It defines endianness.
                // We might need to Swap RB processing or use FormatConverter later.
                // OR: RenderEngine/D2D supports RGBA?
                // D2D CreateBitmapFromMemory supports GUID_WICPixelFormat32bppRGBA.
                // So RGBA is fine!
                
            }
            else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
                size_t bufferSize = 0;
                if (JXL_DEC_SUCCESS != JxlDecoderImageOutBufferSize(dec, &format, &bufferSize)) break;
                if (pOutput->pixels.size() != bufferSize) {
                     // Should match Basic Info calculation, but safety resize
                     try { pOutput->pixels.resize(bufferSize); } catch(...) { break; }
                }
                if (JXL_DEC_SUCCESS != JxlDecoderSetImageOutBuffer(dec, &format, pOutput->pixels.data(), bufferSize)) break;
            }
            else if (status == JXL_DEC_SUCCESS || status == JXL_DEC_FULL_IMAGE) {
                success = true;
                break;
            }
        }
        
        // NOTE: Do NOT destroy global runner!
        JxlDecoderDestroy(dec);

        if (success) {
            pOutput->isValid = true;
            
            // JXL outputs RGBA straight alpha, D2D needs BGRA premultiplied
            // Use SIMD-optimized swizzle + premultiply
            SIMDUtils::SwizzleRGBA_to_BGRA_Premul(pOutput->pixels.data(), (size_t)pOutput->width * pOutput->height);
            
            if (pLoaderName) *pLoaderName = L"libjxl";

            return S_OK;
        }
        return E_FAIL; // JXL Decode Failed
    }
    }

    // --- RAW Path (LibRaw - Performance & Thumbnails) ---
    if (detectedFmt == L"RAW" || detectedFmt == L"TIFF") {
        // Double check extension to ensure it is a Camera RAW
        std::wstring path = filePath;
        std::transform(path.begin(), path.end(), path.begin(), ::towlower);
        
        bool isRaw = path.ends_with(L".arw") || path.ends_with(L".cr2") || path.ends_with(L".nef") || 
                     path.ends_with(L".dng") || path.ends_with(L".orf") || path.ends_with(L".rw2") || 
                     path.ends_with(L".raf") || path.ends_with(L".pef") || path.ends_with(L".srw") || 
                     path.ends_with(L".cr3") || path.ends_with(L".nrw");
                     
        if (isRaw) {
             std::pmr::vector<uint8_t> rawBuf(pmr);
             if (!ReadFileToPMR(filePath, rawBuf, st)) return E_ABORT;
             
             LibRaw processor;
             if (processor.open_buffer(rawBuf.data(), rawBuf.size()) == LIBRAW_SUCCESS) {
                 // 1. Try Embedded JPEG (Fastest)
                 bool thumbSuccess = false;
                 if (processor.unpack_thumb() == LIBRAW_SUCCESS) {
                     int err = 0;
                     libraw_processed_image_t* thumb = processor.dcraw_make_mem_thumb(&err);
                     if (thumb) {
                         if (thumb->type == LIBRAW_IMAGE_JPEG) {
                             // Decode the embedded JPEG using TurboJPEG Logic!
                             tjhandle tj = tj3Init(TJINIT_DECOMPRESS);
                             if (tj) {
                                 int width = 0, height = 0;
                                 if (tj3DecompressHeader(tj, (uint8_t*)thumb->data, thumb->data_size) == 0) {
                                     width = tj3Get(tj, TJPARAM_JPEGWIDTH);
                                     height = tj3Get(tj, TJPARAM_JPEGHEIGHT);
                                     
                                     // IDCT Scaling for Thumb
                                     tjscalingfactor bestFactor = {1, 1};
                                     if (targetWidth > 0 && targetHeight > 0) {
                                          int numFactors = 0;
                                          const tjscalingfactor* factors = tj3GetScalingFactors(&numFactors);
                                          for (int i = 0; i < numFactors; ++i) {
                                              int scaledW = TJSCALED(width, factors[i]);
                                              int scaledH = TJSCALED(height, factors[i]);
                                              if (scaledW >= targetWidth && scaledH >= targetHeight) {
                                                  if (scaledW < TJSCALED(width, bestFactor)) bestFactor = factors[i];
                                              }
                                          }
                                     }
                                     
                                     if (bestFactor.num != 1) {
                                         tj3SetScalingFactor(tj, bestFactor);
                                         width = TJSCALED(width, bestFactor);
                                         height = TJSCALED(height, bestFactor);
                                     }
                                     
                                     UINT stride = width * 4;
                                     try {
                                         pOutput->pixels.resize((size_t)stride * height);
                                         if (tj3Decompress8(tj, (uint8_t*)thumb->data, thumb->data_size, 
                                                            pOutput->pixels.data(), stride, TJPF_BGRX) == 0) {
                                             pOutput->width = width;
                                             pOutput->height = height;
                                             pOutput->stride = stride;
                                             pOutput->isValid = true;
                                             if (pLoaderName) {
                                                 *pLoaderName = L"LibRaw (Embedded JPEG)";
                                                 if (bestFactor.num != 1) *pLoaderName += L" [Scaled]";
                                             }
                                             thumbSuccess = true;
                                         }
                                     } catch(...) {}
                                 }
                                 tj3Destroy(tj);
                             }
                         }
                         processor.dcraw_clear_mem(thumb);
                     }
                 }
                 
                 // 2. Fallback to Full Decode (Slow)
                 if (!thumbSuccess && !st.stop_requested()) {
                     processor.imgdata.params.use_camera_wb = 1;
                     processor.imgdata.params.use_auto_wb = 0;
                     processor.imgdata.params.user_qual = 2; // AHD
                     
                     if (processor.unpack() == LIBRAW_SUCCESS && processor.dcraw_process() == LIBRAW_SUCCESS) {
                         libraw_processed_image_t* image = processor.dcraw_make_mem_image();
                         if (image) {
                             if (image->type == LIBRAW_IMAGE_BITMAP && image->bits == 8 && image->colors == 3) {
                                 // Convert RGB to BGRA
                                 UINT w = image->width;
                                 UINT h = image->height;
                                 UINT stride = w * 4;
                                 try {
                                     pOutput->pixels.resize((size_t)stride * h);
                                     uint8_t* src = image->data;
                                     uint8_t* dst = pOutput->pixels.data();
                                     for (UINT i = 0; i < w * h; ++i) {
                                         dst[i*4+0] = src[i*3+2]; // B
                                         dst[i*4+1] = src[i*3+1]; // G
                                         dst[i*4+2] = src[i*3+0]; // R
                                         dst[i*4+3] = 255;        // A
                                     }
                                     pOutput->width = w;
                                     pOutput->height = h;
                                     pOutput->stride = stride;
                                     pOutput->isValid = true;
                                     if (pLoaderName) *pLoaderName = L"LibRaw (Full PMR)";
                                 } catch(...) {}
                             }
                             processor.dcraw_clear_mem(image);
                         }
                     }
                 }
                 
                 if (pOutput->isValid) return S_OK;
             }
        }
    }
    if (st.stop_requested()) return E_ABORT;

    ComPtr<IWICBitmapSource> source;
    // ... comments ...
    ComPtr<IWICBitmap> wicBitmap;
    
    // Check cancel before expensive LoadToMemory
    if (st.stop_requested()) return E_ABORT;
    
    HRESULT hr = LoadToMemory(filePath, &wicBitmap, pLoaderName, false, ShouldCancel);
    if (FAILED(hr) || !wicBitmap) return hr;
    
    if (st.stop_requested()) return E_ABORT;
    
    UINT w, h;
    wicBitmap->GetSize(&w, &h);
    
    // Check pixel format for Swizzle need
    WICPixelFormatGUID fmt = {0};
    wicBitmap->GetPixelFormat(&fmt);
    bool needsSwizzle = (fmt == GUID_WICPixelFormat32bppRGBA);

    UINT stride = w * 4;
    size_t bufSize = (size_t)stride * h;
    
    // Ensure 16-byte alignment usage if needed, but vector manages its data.
    pOutput->pixels.resize(bufSize);
    pOutput->width = w;
    pOutput->height = h;
    pOutput->stride = stride;
    
    // Lock and copy
    ComPtr<IWICBitmapLock> lock;
    WICRect rcLock = { 0, 0, (INT)w, (INT)h };
    hr = wicBitmap->Lock(&rcLock, WICBitmapLockRead, &lock);
    if (SUCCEEDED(hr)) {
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
                    memcpy(pOutput->pixels.data() + y * stride, 
                           pData + y * cbStride, 
                           std::min(stride, cbStride));
                }
            }
            pOutput->isValid = true;
            
            // [v4.1] Universal Swizzle Fix for Fallback Loaders (e.g. JXL via LoadToMemory)
            if (needsSwizzle) {
                 uint8_t* p = pOutput->pixels.data();
                 size_t pxCount = (size_t)w * h;
                 for (size_t i = 0; i < pxCount; ++i) {
                     std::swap(p[i*4+0], p[i*4+2]);
                 }
                 if (pLoaderName) *pLoaderName += L" [Swizzled]";
            }
            
            // [v3.1] Append resolution to confirm we got the real deal
            if (pLoaderName) {
                wchar_t buf[32]; swprintf_s(buf, L" [%ux%u]", w, h);
                *pLoaderName += buf;
            }
        }
    }
    
    return pOutput->isValid ? S_OK : E_FAIL;
}

std::wstring CImageLoader::GetLastFormatDetails() const {
    return g_lastFormatDetails;
}

int CImageLoader::GetLastExifOrientation() const {
    return g_lastExifOrientation;
}

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
    
    // Scoped file handle for incremental reading
    {
        HANDLE hFile = CreateFileW(filePath, GENERIC_READ, FILE_SHARE_READ, nullptr, 
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) return E_FAIL;
        
        DWORD bytesRead = 0;
        DWORD toRead = (DWORD)std::min((uint64_t)header.size(), pInfo->fileSize);
        if (!ReadFile(hFile, header.data(), toRead, &bytesRead, nullptr)) {
            CloseHandle(hFile);
            return E_FAIL;
        }
        
        if (bytesRead < 12) {
            CloseHandle(hFile);
            return E_FAIL;
        }
        header.resize(bytesRead); // Clamp to actual read
        
        const uint8_t* data = header.data();
        size_t size = header.size();

        // 3. Detect format and parse header
        
        // --- JPEG: Iterative Header Parsing (Streaming) ---
        if (size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
            pInfo->format = L"JPEG";
            
            tjhandle tj = tj3Init(TJINIT_DECOMPRESS);
            if (!tj) { CloseHandle(hFile); return E_FAIL; }

            bool found = false;
            // Loop until found or limit reached (e.g. 16MB safety)
            while (!found && header.size() < 16 * 1024 * 1024) {
                 // Try to parse current buffer
                 if (tj3DecompressHeader(tj, header.data(), header.size()) == 0) {
                     pInfo->width = tj3Get(tj, TJPARAM_JPEGWIDTH);
                     pInfo->height = tj3Get(tj, TJPARAM_JPEGHEIGHT);
                     int subsamp = tj3Get(tj, TJPARAM_SUBSAMP);
                     pInfo->channels = (subsamp == TJSAMP_GRAY) ? 1 : 3;
                     pInfo->bitDepth = 8;
                     found = true;
                     break;
                 }
                 
                 // Not found yet. Scan for SOS (Start of Scan - FF DA) manually?
                 // If we hit SOS, and TurboJPEG still hasn't found headers, then it's effectively corrupted or unsupported.
                 // TurboJPEG parses markers internally. If it returns -1, it means "Error" or "Incomplete".
                 // We will trust reading MORE data might help.
                 
                 // Read next chunk
                 if (header.size() >= pInfo->fileSize) break; // EOF
                 
                 size_t oldSize = header.size();
                 size_t nextRead = std::min((size_t)chunkStep, (size_t)(pInfo->fileSize - oldSize));
                 if (nextRead == 0) break;
                 
                 header.resize(oldSize + nextRead);
                 // Need to seek? ReadFile advances pointer automatically.
                 DWORD chunkBytes = 0;
                 if (!ReadFile(hFile, header.data() + oldSize, (DWORD)nextRead, &chunkBytes, nullptr) || chunkBytes == 0) {
                     break;
                 }
                 header.resize(oldSize + chunkBytes);
            }
            
            tj3Destroy(tj);
            CloseHandle(hFile); // Done with file
            return (pInfo->width > 0) ? S_OK : E_FAIL;
        }
        
        // For other formats, close handle now (buffer is sufficient)
        CloseHandle(hFile); 
    }
    
    // Re-check pointers since vector might have moved during resize (if inside loop, but here okay)
    const uint8_t* data = header.data();
    size_t size = header.size();
    
    // --- PNG: Parse IHDR chunk directly (< 1ms) ---
    if (size >= 24 && data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47) {
        pInfo->format = L"PNG";
        // PNG signature (8) + IHDR length (4) + "IHDR" (4) + Width (4) + Height (4) + BitDepth (1) + ColorType (1)
        if (size >= 24) {
            pInfo->width = (data[16] << 24) | (data[17] << 16) | (data[18] << 8) | data[19];
            pInfo->height = (data[20] << 24) | (data[21] << 16) | (data[22] << 8) | data[23];
            pInfo->bitDepth = data[24];
            uint8_t colorType = data[25];
            pInfo->hasAlpha = (colorType == 4 || colorType == 6);
            pInfo->channels = (colorType == 0 || colorType == 3) ? 1 : (colorType == 4) ? 2 : (colorType == 2) ? 3 : 4;
        }
        return (pInfo->width > 0) ? S_OK : E_FAIL;
    }
    
    // --- WebP: Parse VP8/VP8L/VP8X header (< 1ms) ---
    if (size >= 30 && data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F' &&
        data[8] == 'W' && data[9] == 'E' && data[10] == 'B' && data[11] == 'P') {
        pInfo->format = L"WebP";
        // Use libwebp's fast info parser
        int w = 0, h = 0;
        if (WebPGetInfo(data, size, &w, &h)) {
            pInfo->width = w;
            pInfo->height = h;
            pInfo->bitDepth = 8;
            // Check for alpha
            WebPBitstreamFeatures features;
            if (WebPGetFeatures(data, size, &features) == VP8_STATUS_OK) {
                pInfo->hasAlpha = features.has_alpha;
                pInfo->isAnimated = features.has_animation;
            }
        }
        return (pInfo->width > 0) ? S_OK : E_FAIL;
    }
    
    // --- AVIF: Parse with libavif (< 5ms) ---
    if (size >= 12 && data[4] == 'f' && data[5] == 't' && data[6] == 'y' && data[7] == 'p') {
        bool isAvif = (data[8] == 'a' && data[9] == 'v' && data[10] == 'i' && (data[11] == 'f' || data[11] == 's'));
        bool isHeic = (data[8] == 'h' && data[9] == 'e' && data[10] == 'i' && data[11] == 'c') ||
                      (data[8] == 'm' && data[9] == 'i' && data[10] == 'f' && data[11] == '1');
        
        if (isAvif || isHeic) {
            pInfo->format = isAvif ? L"AVIF" : L"HEIC";
            // Need full file for libavif parsing - read more if needed
            std::vector<uint8_t> fullFile;
            if (ReadFileToVector(filePath, fullFile)) {
                avifDecoder* decoder = avifDecoderCreate();
                if (decoder) {
                    avifResult result = avifDecoderSetIOMemory(decoder, fullFile.data(), fullFile.size());
                    if (result == AVIF_RESULT_OK) {
                        result = avifDecoderParse(decoder);
                        if (result == AVIF_RESULT_OK) {
                            pInfo->width = decoder->image->width;
                            pInfo->height = decoder->image->height;
                            pInfo->bitDepth = decoder->image->depth;
                            pInfo->hasAlpha = decoder->alphaPresent;
                            pInfo->isAnimated = decoder->imageCount > 1;
                        }
                    }
                    avifDecoderDestroy(decoder);
                }
            }
            return (pInfo->width > 0) ? S_OK : E_FAIL;
        }
    }
    
    // --- JXL: Parse with libjxl (< 2ms) ---
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
    
    // --- GIF: Parse header directly (< 1ms) ---
    if (size >= 10 && data[0] == 'G' && data[1] == 'I' && data[2] == 'F' && data[3] == '8') {
        pInfo->format = L"GIF";
        pInfo->width = data[6] | (data[7] << 8);
        pInfo->height = data[8] | (data[9] << 8);
        pInfo->bitDepth = 8;
        pInfo->isAnimated = true; // Assume animated (checking requires more parsing)
        return S_OK;
    }
    
    // --- BMP: Parse header directly (< 1ms) ---
    if (size >= 26 && data[0] == 'B' && data[1] == 'M') {
        pInfo->format = L"BMP";
        pInfo->width = data[18] | (data[19] << 8) | (data[20] << 16) | (data[21] << 24);
        pInfo->height = data[22] | (data[23] << 8) | (data[24] << 16) | (data[25] << 24);
        if ((int32_t)pInfo->height < 0) pInfo->height = -(int32_t)pInfo->height; // Bottom-up BMP
        pInfo->bitDepth = data[28] | (data[29] << 8);
        return S_OK;
    }
    
    // --- RAW (TIFF-based): Use LibRaw (~10ms) ---
    if (size >= 4 && ((data[0] == 'I' && data[1] == 'I') || (data[0] == 'M' && data[1] == 'M'))) {
        pInfo->format = L"RAW";
        std::vector<uint8_t> fullFile;
        if (ReadFileToVector(filePath, fullFile)) {
            LibRaw processor;
            if (processor.open_buffer(fullFile.data(), fullFile.size()) == LIBRAW_SUCCESS) {
                pInfo->width = processor.imgdata.sizes.width;
                pInfo->height = processor.imgdata.sizes.height;
                pInfo->bitDepth = processor.imgdata.params.output_bps;
            }
        }
        return (pInfo->width > 0) ? S_OK : E_FAIL;
    }

    // --- Fallback: WIC (slow but universal) ---
    pInfo->format = L"Unknown";
    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = m_wicFactory->CreateDecoderFromFilename(filePath, nullptr, GENERIC_READ, 
                                                          WICDecodeMetadataCacheOnDemand, &decoder);
    if (SUCCEEDED(hr)) {
        ComPtr<IWICBitmapFrameDecode> frame;
        if (SUCCEEDED(decoder->GetFrame(0, &frame))) {
            UINT w = 0, h = 0;
            frame->GetSize(&w, &h);
            pInfo->width = w;
            pInfo->height = h;
        }
    }
    return (pInfo->width > 0) ? S_OK : E_FAIL;
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

HRESULT CImageLoader::LoadThumbWebP_Scaled(const uint8_t* data, size_t size, int targetSize, ThumbData* pData) {
    WebPDecoderConfig config;
    if (!WebPInitDecoderConfig(&config)) return E_FAIL;
    
    if (WebPGetFeatures(data, size, &config.input) != VP8_STATUS_OK) return E_FAIL;
    
    // Scaling
    config.options.use_scaling = 1;
    
    // Calc ratio
    float ratio = (float)targetSize / std::max(config.input.width, config.input.height);
    config.options.scaled_width = (int)(config.input.width * ratio);
    config.options.scaled_height = (int)(config.input.height * ratio);
    if (config.options.scaled_width < 1) config.options.scaled_width = 1;
    if (config.options.scaled_height < 1) config.options.scaled_height = 1;

    config.output.colorspace = MODE_BGRA;
    
    if (WebPDecode(data, size, &config) != VP8_STATUS_OK) {
        WebPFreeDecBuffer(&config.output); return E_FAIL;
    }
    
    pData->width = config.output.width;
    pData->height = config.output.height;
    pData->stride = config.output.u.RGBA.stride;
    
    size_t outSize = (size_t)pData->stride * pData->height;
    pData->pixels.assign(config.output.u.RGBA.rgba, config.output.u.RGBA.rgba + outSize);
    
    pData->isValid = true;
    pData->isBlurry = true;
    pData->origWidth = config.input.width;
    pData->origHeight = config.input.height;
    
    WebPFreeDecBuffer(&config.output);
    return S_OK;
}

HRESULT CImageLoader::LoadThumbWebP_Limited(const uint8_t* data, size_t size, int targetSize, ThumbData* pData, int timeoutMs) {
    // 1. Header Check
    WebPBitstreamFeatures features;
    if (WebPGetFeatures(data, size, &features) != VP8_STATUS_OK) return E_FAIL;
    
    if ((uint64_t)features.width * features.height > 16 * 1024 * 1024) return E_FAIL; // Limit 16MP
    
    // 2. Call Scaled (which calls LoadThumbWebP_Scaled internally, oh wait, LoadThumbWebPFromMemory does?)
    // Actually LoadThumbWebP_Scaled IS the scaled implementation I added.
    return LoadThumbWebP_Scaled(data, size, targetSize, pData);
}

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
    
    // Set Parallel Runner
    if (runner) {
        JxlDecoderSetParallelRunner(dec, JxlThreadParallelRunner, runner);
    }

    // 2. Subscribe Events: Basic Info + Frame + Full Image + Preview
    // We subscribe to everything potentially useful; we will decide what to decode based on Basic Info.
    int events = JXL_DEC_BASIC_INFO | JXL_DEC_FRAME_PROGRESSION | JXL_DEC_FULL_IMAGE | JXL_DEC_PREVIEW_IMAGE;
    if (JXL_DEC_SUCCESS != JxlDecoderSubscribeEvents(dec, events)) {
        return cleanup(E_FAIL);
    }
    
    // 3. Request DC Only (Lazy: Defer kDC until we know if preview exists)
    // JxlDecoderSetProgressiveDetail(dec, kDC); // Moved to BASIC_INFO handler

    // 4. Input & Config
    JxlDecoderSetInput(dec, pFile, fileSize);
    JxlDecoderCloseInput(dec); // Signal EOF
    
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
static HRESULT LoadThumbJPEG_Robust(LPCWSTR filePath, int targetSize, CImageLoader::ThumbData* pData) {
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
                                   CancelPredicate checkCancel) {
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
    
    // ========================================================================
    // JPEG Path (libjpeg-turbo) - Zero-Copy
    // ========================================================================
    if (format == L"JPEG") {
        // [Revert] Ctrl+4 (DisableDirectD2D) logic moved to main.cpp (Bypass Upload).
        // ImageLoader should ALWAYS try TurboJPEG first for performance.
        
        bool success = false;
        std::vector<uint8_t> jpegBuf;
        
        // Attempt fast load + decode
        if (ReadFileToVector(filePath, jpegBuf)) {
            // ... RAII ...
            tjhandle tjInstance = tj3Init(TJINIT_DECOMPRESS);
            struct TjGuard { tjhandle h; ~TjGuard() { if (h) tj3Destroy(h); } } guard{tjInstance};

            if (tjInstance && tj3DecompressHeader(tjInstance, jpegBuf.data(), jpegBuf.size()) == 0) {
                int origWidth = tj3Get(tjInstance, TJPARAM_JPEGWIDTH);
                int origHeight = tj3Get(tjInstance, TJPARAM_JPEGHEIGHT);
                
                if (origWidth > 0 && origHeight > 0) {
                     // [Two-Stage Decode] Calculate IDCT scaling factor
                     int finalW = origWidth, finalH = origHeight;
                     
                     if (targetWidth > 0 && targetHeight > 0) {
                         // Find best scaling factor that fits screen
                         int numFactors;
                         tjscalingfactor* factors = tj3GetScalingFactors(&numFactors);
                         tjscalingfactor bestFactor = {1, 1}; // Default: full size
                         
                         // Find smallest factor where result >= max(targetWidth, targetHeight)
                         int targetSize = std::max(targetWidth, targetHeight);
                         int bestSize = origWidth * origHeight; // Start with full size
                         
                         for (int i = 0; i < numFactors; i++) {
                             int sW = TJSCALED(origWidth, factors[i]);
                             int sH = TJSCALED(origHeight, factors[i]);
                             int maxDim = std::max(sW, sH);
                             int pixels = sW * sH;
                             
                             // Find the smallest size that still covers target
                             if (maxDim >= targetSize && pixels < bestSize) {
                                 bestSize = pixels;
                                 bestFactor = factors[i];
                             }
                         }
                         
                         // Apply scaling factor
                         if (tj3SetScalingFactor(tjInstance, bestFactor) == 0) {
                             finalW = TJSCALED(origWidth, bestFactor);
                             finalH = TJSCALED(origHeight, bestFactor);
                         }
                     }
                     
                     int stride = CalculateSIMDAlignedStride(finalW, 4);
                     size_t bufSize = static_cast<size_t>(stride) * finalH;
                     
                     // Allocate buffer (using _aligned_malloc via helper)
                     uint8_t* pixels = AllocateBuffer(bufSize);
                     
                     if (pixels) {
                         // [Clean] Use TJPF_BGRX - fastest path, D2D ignores alpha via X8 format
                         if (tj3Decompress8(tjInstance, jpegBuf.data(), jpegBuf.size(), pixels, stride, TJPF_BGRX) == 0) {
                             // Success!
                             outFrame->pixels = pixels;
                             outFrame->width = finalW;
                             outFrame->height = finalH;
                             outFrame->stride = stride;
                             outFrame->format = PixelFormat::BGRX8888; // X8 = Ignore Alpha
                             SetupDeleter(pixels);
                             
                             if (pLoaderName) *pLoaderName = L"TurboJPEG";
                             
                             // Subsampling / Quality info (Preserved)
                             // ... (omitted specifics for brevity, preserved in logic) ...
                             int jpegSubsamp = tj3Get(tjInstance, TJPARAM_SUBSAMP);
                             std::wstring subsamp;
                             switch (jpegSubsamp) {
                                case TJSAMP_444: subsamp = L"4:4:4"; break;
                                case TJSAMP_422: subsamp = L"4:2:2"; break;
                                case TJSAMP_420: subsamp = L"4:2:0"; break;
                                case TJSAMP_GRAY: subsamp = L"Gray"; break;
                                case TJSAMP_440: subsamp = L"4:4:0"; break;
                                default: subsamp = L""; break;
                            }
                            int quality = GetJpegQualityFromBuffer(jpegBuf.data(), jpegBuf.size());
                            if (quality > 0) {
                                wchar_t buf[64];
                                swprintf_s(buf, L"%s Q~%d", subsamp.c_str(), quality);
                                g_lastFormatDetails = buf;
                            } else {
                                g_lastFormatDetails = subsamp;
                            }
                            g_lastExifOrientation = ReadJpegExifOrientation(jpegBuf.data(), jpegBuf.size());
                            
                            success = true;
                         } else {
                             // Decompress failed
                             if (!arena || !arena->Owns(pixels)) _aligned_free(pixels); // Corrected Deleter
                         }
                     }
                }
            }
        }
        
        if (success) return S_OK;
        // If we reach here, TurboJPEG failed. Fall through to WIC.
    }
    
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
            g_lastFormatDetails = L"Vector";
            
            return S_OK;
        }
    }
    
    // ========================================================================
    // WebP Path (libwebp) - Zero-Copy with SIMD Premultiply
    // ========================================================================
    if (format == L"WebP") {
        std::vector<uint8_t> webpBuf;
        if (!ReadFileToVector(filePath, webpBuf)) return E_FAIL;
        
        if (checkCancel && checkCancel()) return E_ABORT;
        
        // Advanced API for threading support
        WebPDecoderConfig config;
        if (!WebPInitDecoderConfig(&config)) return E_FAIL;
        
        // RAII guard for WebP buffer
        struct WebPGuard { 
            WebPDecBuffer* buf; 
            ~WebPGuard() { if (buf) WebPFreeDecBuffer(buf); } 
        } wpGuard{&config.output};
        
        // Enable multi-threaded decoding, output BGRA
        config.options.use_threads = 1;
        config.output.colorspace = MODE_BGRA;
        
        if (WebPGetFeatures(webpBuf.data(), webpBuf.size(), &config.input) != VP8_STATUS_OK) {
            return E_FAIL;
        }
        
        int width = config.input.width;
        int height = config.input.height;
        if (width <= 0 || height <= 0) return E_FAIL;
        
        // Decode
        if (WebPDecode(webpBuf.data(), webpBuf.size(), &config) != VP8_STATUS_OK) {
            return E_FAIL;
        }
        
        uint8_t* output = config.output.u.RGBA.rgba;
        int webpStride = config.output.u.RGBA.stride;
        
        // Allocate our own buffer (SIMD aligned) and copy
        int stride = CalculateSIMDAlignedStride(width, 4);
        size_t bufSize = static_cast<size_t>(stride) * height;
        uint8_t* pixels = AllocateBuffer(bufSize);
        if (!pixels) return E_OUTOFMEMORY;
        
        // Copy row by row (handles stride mismatch)
        for (int y = 0; y < height; ++y) {
            memcpy(pixels + y * stride, output + y * webpStride, width * 4);
        }
        
        // SIMD Premultiply Alpha (Required for D2D/WIC compatibility)
        if (config.input.has_alpha) {
            SIMDUtils::PremultiplyAlpha_BGRA(pixels, width, height, stride);
        }
        
        // Fill output frame
        outFrame->pixels = pixels;
        outFrame->width = width;
        outFrame->height = height;
        outFrame->stride = stride;
        outFrame->format = PixelFormat::BGRA8888;
        SetupDeleter(pixels);
        
        if (pLoaderName) *pLoaderName = L"libwebp";
        
        // Format details
        if (config.input.format == 2) {  // VP8L = lossless
            g_lastFormatDetails = L"Lossless";
        } else {
            g_lastFormatDetails = L"Lossy";
        }
        if (config.input.has_alpha) g_lastFormatDetails += L" +Alpha";
        
        return S_OK;
    }
    
    // ========================================================================
    // PNG/GIF/BMP Path (Wuffs) - Zero-Copy, Already Premultiplied
    // ========================================================================
    if (format == L"PNG" || format == L"GIF" || format == L"BMP") {
        std::vector<uint8_t> fileBuf;
        if (!ReadFileToVector(filePath, fileBuf)) return E_FAIL;
        
        if (checkCancel && checkCancel()) return E_ABORT;
        
        uint32_t width = 0, height = 0;
        std::vector<uint8_t> pixelBuf;
        bool ok = false;
        
        // Wuffs cancel predicate wrapper
        auto wuffsCancel = [&checkCancel]() -> bool {
            return checkCancel && checkCancel();
        };
        
        // Select appropriate decoder
        if (format == L"PNG") {
            ok = WuffsLoader::DecodePNG(fileBuf.data(), fileBuf.size(), &width, &height, pixelBuf, wuffsCancel);
            g_lastFormatDetails = L"Lossless";
        } else if (format == L"GIF") {
            ok = WuffsLoader::DecodeGIF(fileBuf.data(), fileBuf.size(), &width, &height, pixelBuf, wuffsCancel);
            g_lastFormatDetails = L"Indexed";
        } else if (format == L"BMP") {
            ok = WuffsLoader::DecodeBMP(fileBuf.data(), fileBuf.size(), &width, &height, pixelBuf, wuffsCancel);
            g_lastFormatDetails = L"";
        }
        
        if (!ok || width == 0 || height == 0 || pixelBuf.empty()) return E_FAIL;
        
        // Wuffs outputs tight stride (width * 4), copy to aligned buffer
        int wuffsStride = static_cast<int>(width * 4);
        int stride = CalculateSIMDAlignedStride(static_cast<int>(width), 4);
        size_t bufSize = static_cast<size_t>(stride) * height;
        uint8_t* pixels = AllocateBuffer(bufSize);
        if (!pixels) return E_OUTOFMEMORY;
        
        // Copy row by row (handles stride alignment)
        for (uint32_t y = 0; y < height; ++y) {
            memcpy(pixels + y * stride, pixelBuf.data() + y * wuffsStride, wuffsStride);
        }
        
        // Fill output frame (BGRA_PREMUL from Wuffs)
        outFrame->pixels = pixels;
        outFrame->width = static_cast<int>(width);
        outFrame->height = static_cast<int>(height);
        outFrame->stride = stride;
        outFrame->format = PixelFormat::BGRA8888;
        SetupDeleter(pixels);
        
        if (pLoaderName) {
            if (format == L"PNG") *pLoaderName = L"Wuffs/PNG";
            else if (format == L"GIF") *pLoaderName = L"Wuffs/GIF";
            else if (format == L"BMP") *pLoaderName = L"Wuffs/BMP";
        }
        
        return S_OK;
    }
    
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
    
    return S_OK;
}


