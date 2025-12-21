#pragma once
#include "pch.h"

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
        std::wstring Format;   // e.g. "JPEG", "RAW (ARW)"
        
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
    HRESULT LoadToMemory(LPCWSTR filePath, IWICBitmap** ppBitmap, std::wstring* pLoaderName = nullptr); // Force decode to memory

    /// <summary>
    /// Get image size without full decode
    /// </summary>
    HRESULT GetImageSize(LPCWSTR filePath, UINT* width, UINT* height);

    // Helper: Create WIC bitmap from raw bits
    HRESULT CreateWICBitmapFromMemory(UINT width, UINT height, REFGUID format, UINT stride, UINT size, BYTE* data, IWICBitmap** ppBitmap);

private:
    ComPtr<IWICImagingFactory> m_wicFactory;

    // Specialized High-Performance Loaders
    HRESULT LoadJPEG(LPCWSTR filePath, IWICBitmap** ppBitmap);  // libjpeg-turbo
    // LoadPNG REMOVED - replaced by LoadPngWuffs
    HRESULT LoadWebP(LPCWSTR filePath, IWICBitmap** ppBitmap);  // libwebp
    HRESULT LoadAVIF(LPCWSTR filePath, IWICBitmap** ppBitmap);  // libavif + dav1d
    HRESULT LoadJXL(LPCWSTR filePath, IWICBitmap** ppBitmap);   // libjxl
    HRESULT LoadRaw(LPCWSTR filePath, IWICBitmap** ppBitmap);   // libraw
    
    // Wuffs (Google's memory-safe decoder) - Ultimate Performance
    HRESULT LoadPngWuffs(LPCWSTR filePath, IWICBitmap** ppBitmap);  // Wuffs PNG (replaces libpng)
    HRESULT LoadGifWuffs(LPCWSTR filePath, IWICBitmap** ppBitmap);  // Wuffs GIF (replaces WIC)
    HRESULT LoadBmpWuffs(LPCWSTR filePath, IWICBitmap** ppBitmap);  // Wuffs BMP (Safer than WIC)
    HRESULT LoadTgaWuffs(LPCWSTR filePath, IWICBitmap** ppBitmap);  // Wuffs TGA (New support)
    HRESULT LoadWbmpWuffs(LPCWSTR filePath, IWICBitmap** ppBitmap); // Wuffs WBMP (Fix sample)
    
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
