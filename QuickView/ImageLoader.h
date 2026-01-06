#pragma once
#include "pch.h"
#include <vector>
#include <stop_token>
#include <functional>
#include "WuffsLoader.h"
#include <memory_resource>

/// <summary>
/// Image Loader
/// Uses WIC to load image files
/// </summary>
class CImageLoader {
public:
    CImageLoader() = default;
    ~CImageLoader() = default;

    /// <summary>
    /// Initialize loader
    /// </summary>
    HRESULT Initialize(IWICImagingFactory* wicFactory);
    
    // [v4.0] Infrastructure: Atomic Cancellation Predicate
    using CancelPredicate = std::function<bool()>;
    
    // --- Metadata Structure ---
    struct ImageMetadata {
        std::wstring Make;
        std::wstring Model;
        std::wstring Lens;
        std::wstring ISO;      // e.g. "ISO 100"
        std::wstring Aperture; // e.g. "f/2.8"
        std::wstring Shutter;  // e.g. "1/500s"
        std::wstring Focal;    // e.g. "50mm"
        std::wstring ExposureBias; // e.g. "+0.3 EV"
        std::wstring Flash;        // New: Flash status
        std::wstring Date;         // EXIF Date or File Date fallback
        std::wstring Software;     // New: Software/Firmware
        
        UINT Width = 0;
        UINT Height = 0;
        UINT64 FileSize = 0;
        std::wstring Format;        // e.g. "JPEG", "RAW (ARW)"
        std::wstring FormatDetails; // e.g. "4:2:0", "10-bit", "Lossy"
        std::wstring ColorSpace;    // e.g. "sRGB", "Display P3", "Adobe RGB"
        
        // Decoder Info
        std::wstring LoaderName;    // e.g. "TurboJPEG", "libavif"
        DWORD LoadTimeMs = 0;       // Load time in milliseconds
        
        // GPS
        bool HasGPS = false;
        double Latitude = 0.0;
        double Longitude = 0.0;
        double Altitude = 0.0;       // New: Altitude

        // Histogram (256 bins)
        std::vector<uint32_t> HistR;
        std::vector<uint32_t> HistG;
        std::vector<uint32_t> HistB;
        std::vector<uint32_t> HistL; // Luminance
        
        bool IsEmpty() const { return Make.empty() && Model.empty() && ISO.empty() && Date.empty(); }

        std::wstring GetCompactString() const {
            std::wstring s;
            if (!Make.empty()) s += Make + L" ";
            if (!Model.empty()) s += Model;
            if (!Focal.empty()) s += (s.empty() ? L"" : L"  ") + Focal;
            if (!ISO.empty()) s += (s.empty() ? L"" : L"  ") + (L"ISO " + ISO);
            if (!Aperture.empty()) s += L"  " + Aperture;
            if (!Shutter.empty()) s += L"  " + Shutter;
            if (!ExposureBias.empty()) s += L"  " + ExposureBias;
            return s;
        }
    };

    // --- NEW: Raw Thumbnail Data (Zero-Copy flow) ---
    struct ThumbData {
        std::vector<uint8_t> pixels; // Raw BGRA (Compatible with PBGRA/BGRX)
        int width;
        int height;
        int stride;
        bool isValid = false;
        
        // Metadata for Hover
        int origWidth = 0;
        int origHeight = 0;
        uint64_t fileSize = 0;
        bool isBlurry = true; // Phase 6: Fast Pass (false = Clear, true = Blur)
        
        // [v3.2] Debug: 记录实际使用的 Loader
        std::wstring loaderName;
    };

    // --- NEW: PMR-backed Decoded Image (Zero-Copy) ---
    struct DecodedImage {
        std::pmr::vector<uint8_t> pixels;  // BGRA/BGRX pixels in PMR memory
        UINT width = 0;
        UINT height = 0;
        UINT stride = 0;
        bool isValid = false;
        
        DecodedImage() : pixels(std::pmr::get_default_resource()) {}
        explicit DecodedImage(std::pmr::memory_resource* mr) : pixels(mr) {}
    };

    // --- NEW: Pre-flight Check Types (v3.1) ---
    enum class ImageType {
        TypeA_Sprint,  // Express Lane: Small files, embedded thumbs
        TypeB_Heavy,   // Main Lane: Large files requiring Fit decode
        Invalid        // Unsupported or corrupt
    };

    // --- NEW: Header Info for Pre-flight Check ---
    struct ImageHeaderInfo {
        std::wstring format;      // JPEG/PNG/WEBP/RAW/JXL/AVIF
        int width = 0;
        int height = 0;
        uintmax_t fileSize = 0;
        bool hasEmbeddedThumb = false;
        ImageType type = ImageType::Invalid;
        
        int64_t GetPixelCount() const { return (int64_t)width * height; }
        // [v3.1] Reverted to 2MB/2.1MP per user request
        bool IsSmall() const { return width > 0 && fileSize < 2 * 1024 * 1024 && GetPixelCount() < 2100000; }
    };

    // --- Pre-flight Check API ---
    /// <summary>
    /// Fast header peek (reads ~512 bytes) to classify image without full decode.
    /// </summary>
    ImageHeaderInfo PeekHeader(LPCWSTR filePath);

    /// <summary>
    /// Read metadata from file using WIC
    /// </summary>
    HRESULT ReadMetadata(LPCWSTR filePath, ImageMetadata* pMetadata);

    /// <summary>
    /// Compute Histogram from bitmap (Sparse Sampling supported)
    /// </summary>
    HRESULT ComputeHistogram(IWICBitmapSource* source, ImageMetadata* pMetadata);

    /// <summary>
    /// Load WIC bitmap from file
    /// </summary>
    HRESULT LoadFromFile(LPCWSTR filePath, IWICBitmapSource** bitmap);

    /// <summary>
    /// Load WIC bitmap from file and force decode to memory
    /// </summary>
    HRESULT LoadToMemory(LPCWSTR filePath, IWICBitmap** ppBitmap, std::wstring* pLoaderName = nullptr, bool forceFullDecode = false, CancelPredicate checkCancel = nullptr);

    /// <summary>
    /// NEW: Load image directly into PMR-backed buffer (Zero-Copy for Heavy Lane)
    /// Support specialized "Decode-to-Scale" based on target dimensions.
    /// </summary>
    HRESULT LoadToMemoryPMR(LPCWSTR filePath, DecodedImage* pOutput, std::pmr::memory_resource* pmr, 
                            int targetWidth, int targetHeight, /* 0 for full decode */
                            std::wstring* pLoaderName = nullptr, 
                            std::stop_token st = {},
                            CancelPredicate checkCancel = nullptr);

    /// <summary>
    /// NEW: Load Thumbnail (Raw Data)
    /// Optimizes for speed using TurboJPEG scaling where possible.
    /// Returns raw BGRA buffer.
    /// </summary>



    /// <summary>
    /// Get format details from last load (e.g. "4:2:0", "10-bit")
    /// </summary>
    std::wstring GetLastFormatDetails() const;

    /// <summary>
    /// Get EXIF Orientation from last load (1-8, 1=Normal)
    /// </summary>
    int GetLastExifOrientation() const;

    // --- NEW: Fast Image Info (Header-Only Parsing) ---
    struct ImageInfo {
        uint32_t width = 0;
        uint32_t height = 0;
        uint64_t fileSize = 0;
        std::wstring format;      // "JPEG", "PNG", "WebP", "AVIF", "JXL", "RAW", etc.
        std::wstring colorSpace;  // Optional: "sRGB", "Display P3", etc.
        int bitDepth = 0;         // Optional: 8, 10, 12, 16
        int channels = 0;         // Optional: 3 (RGB), 4 (RGBA)
        bool hasAlpha = false;
        bool isAnimated = false;  // For GIF, WebP, AVIF animations
    };

    /// <summary>
    /// Fast header-only parsing using native decoders (< 5ms for most formats)
    /// Only reads first ~64KB of file, uses format-specific optimized parsers.
    /// </summary>
    HRESULT GetImageInfoFast(LPCWSTR filePath, ImageInfo* pInfo);

    /// <summary>
    /// Get image size without full decode (Legacy, uses WIC)
    /// </summary>
    HRESULT GetImageSize(LPCWSTR filePath, UINT* width, UINT* height);

    // Helper: Create WIC bitmap from raw bits
    HRESULT CreateWICBitmapFromMemory(UINT width, UINT height, REFGUID format, UINT stride, UINT size, BYTE* data, IWICBitmap** ppBitmap);
    
    /// <summary>
    /// Creates a WIC Bitmap by DEEP COPYING memory. Safe for cross-thread Arena usage.
    /// </summary>
    HRESULT CreateWICBitmapCopy(UINT width, UINT height, REFGUID format, UINT stride, UINT size, BYTE* data, IWICBitmap** ppBitmap);

    /// <summary>
    /// Try to load a "Fast Pass" image (Full Decode, Timeout 15ms)
    /// </summary>
    HRESULT LoadFastPass(LPCWSTR filePath, ThumbData* pData);
    
    // Core Thumbnail API
    HRESULT LoadThumbnail(LPCWSTR filePath, int targetSize, ThumbData* pData, bool allowSlow = true);

    // [JXL Global Runner] 全局线程池单例，避免每次解码创建开销
    static void* GetJxlRunner();
    static void ReleaseJxlRunner();

private:
    ComPtr<IWICImagingFactory> m_wicFactory;
    
    // [JXL Global Runner] Static singleton
    static void* s_jxlRunner;
    static std::mutex s_jxlRunnerMutex;

    // Specialized High-Performance Loaders
    HRESULT LoadJPEG(LPCWSTR filePath, IWICBitmap** ppBitmap);  // libjpeg-turbo
    HRESULT LoadThumbJPEG(LPCWSTR filePath, int targetSize, ThumbData* pData); // New TurboJPEG Scaled Loader
    HRESULT LoadThumbJPEGFromMemory(const uint8_t* pBuf, size_t size, int targetSize, ThumbData* pData); // Helper for in-memory buffers
    HRESULT LoadThumbWebPFromMemory(const uint8_t* pBuf, size_t size, int targetSize, ThumbData* pData); // Helper for WebP buffers

    /// <summary>
    /// Specialized Thumbnail Loaders (Phase 6)
    /// </summary>
    HRESULT LoadThumbJXL_DC(const uint8_t* data, size_t size, ThumbData* pData);
    HRESULT LoadThumbAVIF_Proxy(const uint8_t* data, size_t size, int targetSize, ThumbData* pData, bool allowSlow = true);
    HRESULT LoadThumbWebP_Limited(const uint8_t* data, size_t size, int targetSize, ThumbData* pData, int timeoutMs);
    HRESULT LoadThumbWebP_Scaled(const uint8_t* data, size_t size, int targetSize, ThumbData* pData);





    // LoadPNG REMOVED - replaced by LoadPngWuffs
    HRESULT LoadWebP(LPCWSTR filePath, IWICBitmap** ppBitmap);  // libwebp
    HRESULT LoadAVIF(LPCWSTR filePath, IWICBitmap** ppBitmap);  // libavif + dav1d
    HRESULT LoadJXL(LPCWSTR filePath, IWICBitmap** ppBitmap);   // libjxl
    HRESULT LoadRaw(LPCWSTR filePath, IWICBitmap** ppBitmap, bool forceFullDecode);   // libraw
    
    // Wuffs (Google's memory-safe decoder) - Ultimate Performance
    // [v4.0] Cancellation Support
    HRESULT LoadPngWuffs(LPCWSTR filePath, IWICBitmap** ppBitmap, CancelPredicate checkCancel = nullptr);
    HRESULT LoadGifWuffs(LPCWSTR filePath, IWICBitmap** ppBitmap, CancelPredicate checkCancel = nullptr);
    HRESULT LoadBmpWuffs(LPCWSTR filePath, IWICBitmap** ppBitmap, CancelPredicate checkCancel = nullptr);
    HRESULT LoadTgaWuffs(LPCWSTR filePath, IWICBitmap** ppBitmap, CancelPredicate checkCancel = nullptr);
    HRESULT LoadWbmpWuffs(LPCWSTR filePath, IWICBitmap** ppBitmap, CancelPredicate checkCancel = nullptr);
    
    // Stb Image (Legacy/Special Formats: PSD, HDR, PIC, PNM)
    HRESULT LoadStbImage(LPCWSTR filePath, IWICBitmap** ppBitmap, bool floatFormat = false);

    // TinyEXR (OpenEXR)
    HRESULT LoadTinyExrImage(LPCWSTR filePath, IWICBitmap** ppBitmap);

    // NanoSVG (SVG)
    HRESULT LoadSVG(LPCWSTR filePath, IWICBitmap** ppBitmap);
    
    // NetPBM via Wuffs (PAM, PBM, PGM, PPM)
    HRESULT LoadNetpbmWuffs(LPCWSTR filePath, IWICBitmap** ppBitmap);
    
    // QOI via Wuffs
    HRESULT LoadQoiWuffs(LPCWSTR filePath, IWICBitmap** ppBitmap);
    
    // Custom PCX Decoder
    HRESULT LoadPCX(LPCWSTR filePath, IWICBitmap** ppBitmap);
};
