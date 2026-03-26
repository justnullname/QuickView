#include "pch.h"
#include <filesystem>
#include <fstream> 
#include <memory>
#include <map>
#include <DirectXPackedVector.h>
#include <wincodec.h>
#include <wincodecsdk.h>

#include "ImageLoader.h"
#include "MemoryArena.h" // [Fix] Include for QuantumArena definition
#include "EditState.h" // For g_runtime
#include "TileMemoryManager.h" // [Titan]
// [Deep Cancel] Use low-level libjpeg API for scanline cancellation
#include <stdio.h> // jpeglib needs stdio
#include <setjmp.h> // For error handling
#define HAVE_BOOLEAN // Prevent conflict with Windows boolean
#include <jpeglib.h>
#include <turbojpeg.h> // [v5.0] Required for JPEGLoader fallback logic
#include <webp/demux.h> // [Phase 18] Required for Native WebP ICC extraction
#include <jxl/decode.h> // JXL
#include <jxl/decode_cxx.h>
#include <jxl/resizable_parallel_runner.h>
#include <jxl/thread_parallel_runner.h>
#include <avif/avif.h> // AVIF
#include "WuffsLoader.h"
#include "StbLoader.h"
#include "PreviewExtractor.h" // [Titan]

using namespace QuickView;
#include "TinyExrLoader.h"
#include <immintrin.h> // SIMD
#include "SIMDUtils.h"
#include <thread>
#include <shobjidl.h> // [Add] for IShellItemImageFactory
#include "MappedFile.h" // [Opt]
#if defined(__has_include)
#if __has_include(<simd>)
#include <simd>
#define QUICKVIEW_HAS_STD_SIMD 1
#else
#define QUICKVIEW_HAS_STD_SIMD 0
#endif
#else
#define QUICKVIEW_HAS_STD_SIMD 0
#endif

#if QUICKVIEW_HAS_STD_SIMD && defined(__cpp_lib_simd) && (__cpp_lib_simd >= 202207L)
#define QUICKVIEW_USE_STD_SIMD_HIST 1
#else
#define QUICKVIEW_USE_STD_SIMD_HIST 0
#endif

// Forward declaration
static bool ReadFileToVector(LPCWSTR filePath, std::vector<uint8_t>& buffer);

// [CMS/PMR] Forward declaration
static bool ReadFileToPMR(LPCWSTR filePath, std::pmr::vector<uint8_t>& buffer, std::pmr::memory_resource* mr = nullptr);

// [v6.3] Forward Declaration for JXL Helper
static std::wstring ParseJXLColorEncoding(const JxlColorEncoding& c);
static QuickView::TransferFunction MapJxlTransferFunction(JxlTransferFunction tf);
static QuickView::ColorPrimaries MapJxlPrimaries(JxlPrimaries primaries);
static QuickView::TransferFunction MapAvifTransferFunction(uint8_t tf);
static QuickView::ColorPrimaries MapAvifPrimaries(uint16_t primaries);
static void PopulateAvifHdrStaticMetadata(const avifImage* image, QuickView::HdrStaticMetadata* hdrMetadata);
static float DecodePqToLinearScRgb(float value);
static float DecodeHlgToLinear(float value);
static float DecodeSrgbToLinear(float value);
static float DecodeRec709ToLinear(float value);
static float DecodeTransferToLinear(float value, QuickView::TransferFunction tf);
static bool TryDecodeAvifGainMappedLinearRGBA(
    avifDecoder* decoder,
    const QuickView::Codec::DecodeContext& ctx,
    uint8_t** outPixels,
    int* outWidth,
    int* outHeight,
    int* outStride,
    QuickView::HdrStaticMetadata* hdrMetadata,
    std::wstring* formatDetails);

// [Fast Header] AVIF/HEIC dimension probes (implemented later in this file)
static bool GetAVIFDimensions(LPCWSTR filePath, uint32_t* width, uint32_t* height);
static bool GetISOBMFFDimensions(LPCWSTR filePath, uint32_t* width, uint32_t* height);



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
            float targetHdrHeadroomStops = -1.0f; // < 0 = codec default

            // [v5.3] Telemetry Output - backward compatible
            std::wstring* pLoaderName = nullptr;
            std::wstring* pFormatDetails = nullptr;
            
            // [v5.3] Unified metadata pointer (optional - if set, loaders will populate)
            CImageLoader::ImageMetadata* pMetadata = nullptr;
            
            // [v10] Fallback control for Titan Base Layer
            bool forceRenderFull = false;
            
            // [v10.1] Override to prevent 1x1 Fake Base generation (e.g. for specific LODs)
            bool allowFakeBase = true;
            bool isTitanMode = false;
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

// [v5.0] Forward// Forward declarations
struct IStream;
struct IWICBitmap;
namespace QuickView {
    namespace Codec {
        namespace PsdComposite {
            static bool IsPSB(const uint8_t* data, size_t size);
            static HRESULT Load(const uint8_t* data, size_t size, const DecodeContext& ctx, DecodeResult& result);
        }
    }
}

// Helper to detect format from buffer
static std::wstring DetectFormatFromContent(const uint8_t* magic, size_t size) {
    if (size < 4) return L"Unknown";

    struct MagicSignature {
        const wchar_t* name;
        size_t offset;
        const uint8_t* signature;
        size_t sig_len;
    };

    static const uint8_t sig_jpeg[] = {0xFF, 0xD8, 0xFF};
    static const uint8_t sig_png[]  = {0x89, 0x50, 0x4E, 0x47};
    static const uint8_t sig_webp[] = {'W', 'E', 'B', 'P'};
    static const uint8_t sig_jxl1[] = {0xFF, 0x0A};
    static const uint8_t sig_jxl2[] = {'J', 'X', 'L', ' '};
    static const uint8_t sig_dds[]  = {'D', 'D', 'S', ' '};
    static const uint8_t sig_bmp[]  = {'B', 'M'};
    static const uint8_t sig_psd[]  = {'8', 'B', 'P', 'S'};
    static const uint8_t sig_hdr[]  = {'#', '?'};
    static const uint8_t sig_exr[]  = {0x76, 0x2F, 0x31, 0x01};
    static const uint8_t sig_pic[]  = {0x53, 0x80, 0xF6, 0x34};
    static const uint8_t sig_qoi[]  = {'q', 'o', 'i', 'f'};
    static const uint8_t sig_pcx[]  = {0x0A};
    static const uint8_t sig_ico[]  = {0x00, 0x00, 0x01, 0x00};
    static const uint8_t sig_tif1[] = {0x49, 0x49, 0x2A, 0x00};
    static const uint8_t sig_tif2[] = {0x4D, 0x4D, 0x00, 0x2A};

    static const MagicSignature signatures[] = {
        { L"JPEG", 0, sig_jpeg, sizeof(sig_jpeg) },
        { L"PNG",  0, sig_png,  sizeof(sig_png) },
        { L"WebP", 8, sig_webp, sizeof(sig_webp) },
        { L"JXL",  0, sig_jxl1, sizeof(sig_jxl1) },
        { L"JXL",  4, sig_jxl2, sizeof(sig_jxl2) },
        { L"DDS",  0, sig_dds,  sizeof(sig_dds) },
        { L"BMP",  0, sig_bmp,  sizeof(sig_bmp) },
        { L"PSD",  0, sig_psd,  sizeof(sig_psd) },
        { L"HDR",  0, sig_hdr,  sizeof(sig_hdr) },
        { L"EXR",  0, sig_exr,  sizeof(sig_exr) },
        { L"PIC",  0, sig_pic,  sizeof(sig_pic) },
        { L"QOI",  0, sig_qoi,  sizeof(sig_qoi) },
        { L"PCX",  0, sig_pcx,  sizeof(sig_pcx) },
        { L"ICO",  0, sig_ico,  sizeof(sig_ico) },
        { L"TIFF", 0, sig_tif1, sizeof(sig_tif1) },
        { L"TIFF", 0, sig_tif2, sizeof(sig_tif2) }
    };

    for (const auto& sig : signatures) {
        if (size >= sig.offset + sig.sig_len) {
            bool match = true;
            size_t i = 0;
            for (size_t k = 0; k < sig.sig_len; ++k) {
                uint8_t byte = sig.signature[k];
                if (magic[sig.offset + i] != byte) {
                    match = false;
                    break;
                }
                i++;
            }
            // Additional context rules for WebP / JXL box
            if (match) {
                if (wcscmp(sig.name, L"WebP") == 0 && !(magic[0] == 'R' && magic[1] == 'I' && magic[2] == 'F' && magic[3] == 'F')) match = false;
                if (sig.offset == 4 && wcscmp(sig.name, L"JXL") == 0 && !(magic[0] == 0x00 && magic[1] == 0x00 && magic[2] == 0x00 && magic[3] == 0x0C)) match = false;
            }
            if (match) return sig.name;
        }
    }

    // Complex / Heuristic checks
    if (size >= 12 && magic[4] == 'f' && magic[5] == 't' && magic[6] == 'y' && magic[7] == 'p') {
        if (magic[8] == 'a' && magic[9] == 'v' && magic[10] == 'i' && (magic[11] == 'f' || magic[11] == 's')) return L"AVIF";
        if (magic[8] == 'c' && magic[9] == 'r' && magic[10] == 'x') return L"RAW";
        if ((magic[8] == 'h' && magic[9] == 'e' && magic[10] == 'i' && (magic[11] == 'c' || magic[11] == 'x' || magic[11] == 's' || magic[11] == 'm')) ||
            (magic[8] == 'h' && magic[9] == 'e' && magic[10] == 'v' && (magic[11] == 'c' || magic[11] == 'm' || magic[11] == 's')) ||
            (magic[8] == 'm' && magic[9] == 'i' && magic[10] == 'f' && magic[11] == '1') ||
            (magic[8] == 'm' && magic[9] == 's' && magic[10] == 'f' && magic[11] == '1')) return L"HEIC";
    }

    if (size >= 6 && magic[0] == 'G' && magic[1] == 'I' && magic[2] == 'F' && magic[3] == '8' && (magic[4] == '7' || magic[4] == '9') && magic[5] == 'a') return L"GIF";
    if (size >= 2 && magic[0] == 'P' && magic[1] >= '1' && magic[1] <= '7') return L"PNM";

    if (size >= 18) {
        bool validColorMap = (magic[1] == 0 || magic[1] == 1);
        bool validType = (magic[2] == 1 || magic[2] == 2 || magic[2] == 3 || magic[2] == 9 || magic[2] == 10 || magic[2] == 11);
        bool validBpp = (magic[16] == 8 || magic[16] == 15 || magic[16] == 16 || magic[16] == 24 || magic[16] == 32);
        if (validColorMap && validType && validBpp) return L"TGA";
    }

    if (size >= 2 && magic[0] == 0x00 && magic[1] == 0x00) {
        if (size >= 8 && magic[4] == 'f' && magic[5] == 't' && magic[6] == 'y' && magic[7] == 'p') return L"Unknown";
        return L"WBMP";
    }

    return L"Unknown";
}

// [Optimization] Load full image from memory pointer (for MMF Preload)
HRESULT CImageLoader::LoadToFrameFromMemory(const uint8_t* data, size_t size, 
                                            QuickView::RawImageFrame* outFrame,
                                            class QuantumArena* arena,
                                            int targetWidth, int targetHeight,
                                            std::wstring* pLoaderName,
                                            ImageMetadata* pMetadata) 
{
    if (!data || !size || !outFrame) return E_INVALIDARG;

    // 1. Detect Format
    std::wstring fmt = DetectFormatFromContent(data, size);

    // 2. TurboJPEG Fast Path
    if (fmt == L"JPEG") {
        if (pLoaderName) *pLoaderName = L"TurboJPEG (MMF)";
        
        // [Opt] Thread-Local TurboJPEG Handle to avoid init overhead
        // 5-10ms overhead per init avoided.
        struct TjCtx { 
            tjhandle h; 
            TjCtx() { h = tj3Init(TJINIT_DECOMPRESS); } 
            ~TjCtx() { if(h) tj3Destroy(h); } 
        };
        static thread_local TjCtx t_ctx;
        
        tjhandle handle = t_ctx.h;
        if (!handle) return E_FAIL;
        
        // No Guard needed, handle persists with thread
        // Resetting to defaults might be needed? 
        // tj3DecompressHeader/tj3Decompress8 overrides params anyway usually.
        // But scaling factor persists? 
        // tj3SetScalingFactor sets a property. We should reset it or set it explicitly every time.
        // We do set it below if targetWidth > 0.
        // If not, we should reset it?
        // Default is 1/1. 
        tjscalingfactor sf = {1, 1};
        tj3SetScalingFactor(handle, sf);

        // [Fix] Use TurboJPEG v3 API correctly (Header -> Get)
        if (tj3DecompressHeader(handle, data, size) != 0) {
             return E_FAIL;
        }

        int width = tj3Get(handle, TJPARAM_JPEGWIDTH);
        int height = tj3Get(handle, TJPARAM_JPEGHEIGHT);

        // [CMS Fix] If the JPEG is CMYK, TurboJPEG's TJPF_BGRA is naive.
        // Fall back to WIC which now uses IWICColorTransform for high-fidelity CMS.
        int colorspace = tj3Get(handle, TJPARAM_COLORSPACE);
        if (colorspace == TJCS_CMYK || colorspace == TJCS_YCCK) {
             goto WIC_FALLBACK;
        }

        // [Metadata] Populate Original Dimensions BEFORE Scaling
        if (pMetadata) {
            pMetadata->Width = width;
            pMetadata->Height = height;
            pMetadata->FileSize = size;
            pMetadata->Format = L"JPEG (MMF)";
            
            int subsamp = tj3Get(handle, TJPARAM_SUBSAMP);
            switch(subsamp) {
                case TJSAMP_444: pMetadata->FormatDetails = L"4:4:4"; break;
                case TJSAMP_422: pMetadata->FormatDetails = L"4:2:2"; break;
                case TJSAMP_420: pMetadata->FormatDetails = L"4:2:0"; break;
                case TJSAMP_GRAY: pMetadata->FormatDetails = L"Gray"; break;
                case TJSAMP_440: pMetadata->FormatDetails = L"4:4:0"; break;
                default: pMetadata->FormatDetails = L"Unknown"; break;
            }
        }
        
        // Calculate Scaling
        tjscalingfactor chosenFactor = {1, 1};
        if (targetWidth > 0 || targetHeight > 0) {
            int numFactors;
            tjscalingfactor* factors = tj3GetScalingFactors(&numFactors);
            
            // Default target if one dimension is missing (preserve aspect ratio)
            // But usually caller passes both (screen size).
            // Actually, for "Fit", we usually want the image to be <= target dimensions.
            // Let's emulate LoadThumbJPEGFromMemory logic: find factor that keeps image LARGER than target?
            // "Fit" strategy usually implies we downscale so that image fits INSIDE target.
            // But IDCT selection usually looks for "smallest factor >= target" to avoid software upscaling, 
            // OR "largest factor <= target" if we want purely hardware?
            // TurboJPEG docs say: "choose the smallest scaling factor that generates an image larger than or equal to the desired size".
            // That's for thumbnails where you crop/resize later.
            // For full image view, we want the closest match.
            // Let's use simple logic: find factor that makes image definitely >= target (for quality) or closest?
            // Let's pick Closest match to target size (Fit).
            
            int neededW = (targetWidth > 0) ? targetWidth : width;
            int neededH = (targetHeight > 0) ? targetHeight : height;
            
            double targetScale = std::min((double)neededW / width, (double)neededH / height);
            if (targetScale > 1.0) targetScale = 1.0;
            
            // Find scaling factor closest to targetScale
            double bestDiff = 100.0;
            for (int i = 0; i < numFactors; i++) {
                double f = (double)factors[i].num / factors[i].denom;
                double diff = std::abs(f - targetScale);
                if (diff < bestDiff) {
                     bestDiff = diff;
                     chosenFactor = factors[i];
                }
            }
            tj3SetScalingFactor(handle, chosenFactor);
        }

        // Get Scaled Dimensions
        int finalW = TJSCALED(width, chosenFactor);
        int finalH = TJSCALED(height, chosenFactor);

        // Allocate Pixel Buffer
        outFrame->width = finalW;
        outFrame->height = finalH;
        outFrame->stride = QuickView::CalculateAlignedStride(finalW, 4);
        outFrame->format = PixelFormat::BGRA8888;
        outFrame->formatDetails = L"8-bit JPEG (MMF)";
        if (chosenFactor.num != chosenFactor.denom) {
            outFrame->formatDetails += L" Scaled";
            outFrame->srcWidth = width;   // [v10.1] Preserve original resolution
            outFrame->srcHeight = height;
        }
        
        size_t bufSize = outFrame->GetBufferSize();
        
        if (arena) {
            // [Fix] Use Allocate instead of Alloc
            outFrame->pixels = (uint8_t*)arena->Allocate(bufSize, 64);
            outFrame->memoryDeleter = nullptr; // Arena owned
        } else {
             outFrame->pixels = (uint8_t*)_aligned_malloc(bufSize, 64);
             outFrame->memoryDeleter = [](uint8_t* p) { _aligned_free(p); };
        }
        
        if (!outFrame->pixels) {
            return E_OUTOFMEMORY;
        }
        
        // Decompress
        // NOTE: This will trigger PAGE FAULTS for the entire file sequentially!
        // This effectively "Warms Up" the OS Cache for random access later.
        if (tj3Decompress8(handle, data, size, outFrame->pixels, outFrame->stride, TJPF_BGRA) != 0) {
             return E_FAIL;
        }
        
        // [CMS] Extract ICC Profile from TurboJPEG
        uint8_t* iccBuf = nullptr;
        size_t iccSize = 0;
        if (tj3GetICCProfile(handle, &iccBuf, &iccSize) == 0 && iccSize > 0) {
            outFrame->iccProfile.assign(iccBuf, iccBuf + iccSize);
            
            // [v10.1] Sync back to metadata for unified Info Panel & CMS check
            if (pMetadata) {
                pMetadata->iccProfileData.assign(iccBuf, iccBuf + iccSize);
                pMetadata->HasEmbeddedColorProfile = true;
            }
            
            tj3Free(iccBuf);
        }

        return S_OK;
    }
    
WIC_FALLBACK:
    // For other formats (WIC), we need a Stream.
    // Create IStream from Memory
    ComPtr<IStream> stream = SHCreateMemStream(data, (UINT)size);
    if (!stream) return E_FAIL;
    
    // Use WIC
    if (pLoaderName) *pLoaderName = L"WIC (MMF)";
    
    // Create Decoder
    ComPtr<IWICBitmapDecoder> decoder;
    if (!m_wicFactory) return E_FAIL;
    
    HRESULT hr = m_wicFactory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr)) return hr;
    
    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) return hr;

    UINT origW, origH;
    frame->GetSize(&origW, &origH);

    // [Metadata] Populate Original Dimensions BEFORE Scaling
    if (pMetadata) {
        pMetadata->Width = (uint32_t)origW;
        pMetadata->Height = (uint32_t)origH;
        pMetadata->FileSize = (uint64_t)size;
        pMetadata->Format = fmt + L" (MMF)";
    }

    // Use WIC Scaler if target size is specified
    ComPtr<IWICBitmapSource> sourceWithScaler;
    
    if (targetWidth > 0 || targetHeight > 0) {
        // Calculate factor
        double scale = std::min((double)targetWidth / origW, (double)targetHeight / origH);
        if (scale < 1.0) {
            UINT newW = (UINT)(origW * scale);
            UINT newH = (UINT)(origH * scale);
            if (newW < 1) newW = 1; 
            if (newH < 1) newH = 1;
            
            ComPtr<IWICBitmapScaler> scaler;
            if (SUCCEEDED(m_wicFactory->CreateBitmapScaler(&scaler))) {
                if (SUCCEEDED(scaler->Initialize(frame.Get(), newW, newH, WICBitmapInterpolationModeFant))) {
                    sourceWithScaler = scaler;
                }
            }
        }
    }
    
    // Fallback if no scaler
    ComPtr<IWICBitmapSource> finalSource = sourceWithScaler ? sourceWithScaler : frame;
    
    // 3. Convert to BitmapSource (Format)
    ComPtr<IWICBitmapSource> sourceConverted;
    WICConvertBitmapSource(GUID_WICPixelFormat32bppPBGRA, finalSource.Get(), &sourceConverted);
    if (!sourceConverted) sourceConverted = finalSource;

    // 4. [CMS] Extract ICC Profile
    UINT count = 0;
    if (SUCCEEDED(frame->GetColorContexts(0, nullptr, &count)) && count > 0) {
        std::vector<ComPtr<IWICColorContext>> contexts(count);
        std::vector<IWICColorContext*> rawContexts(count);
        for (UINT i = 0; i < count; i++) {
            m_wicFactory->CreateColorContext(&contexts[i]);
            rawContexts[i] = contexts[i].Get();
        }

        UINT actual = 0;
        if (SUCCEEDED(frame->GetColorContexts(count, rawContexts.data(), &actual))) {
            for (UINT i = 0; i < actual; i++) {
                WICColorContextType type;
                contexts[i]->GetType(&type);
                if (type == WICColorContextProfile) {
                    UINT cbProfile = 0;
                    contexts[i]->GetProfileBytes(0, nullptr, &cbProfile);
                    if (cbProfile > 0) {
                        outFrame->iccProfile.resize(cbProfile);
                        if (FAILED(contexts[i]->GetProfileBytes(cbProfile, outFrame->iccProfile.data(), &cbProfile))) {
                            outFrame->iccProfile.clear();
                        } else {
                            break; // Got it
                        }
                    }
                }
            }
        }
    }

    // 5. Build Final Frame
    UINT w, h;
    sourceConverted->GetSize(&w, &h);
    outFrame->width = w;
    outFrame->height = h;
    outFrame->stride = QuickView::CalculateAlignedStride(w, 4);
    outFrame->format = PixelFormat::BGRA8888;
    outFrame->formatDetails = L"WIC Cache";
    
    if (pMetadata) {
        pMetadata->Width = (uint32_t)origW;
        pMetadata->Height = (uint32_t)origH;
    }
    
    size_t bufSize = outFrame->GetBufferSize();
    if (arena) {
        outFrame->pixels = (uint8_t*)arena->Allocate(bufSize, 64);
        outFrame->memoryDeleter = nullptr; 
    } else {
        outFrame->pixels = (uint8_t*)_aligned_malloc(bufSize, 64);
        outFrame->memoryDeleter = [](uint8_t* p) { _aligned_free(p); };
    }
    
    if (!outFrame->pixels) return E_OUTOFMEMORY;
    
    hr = sourceConverted->CopyPixels(nullptr, outFrame->stride, (UINT)bufSize, outFrame->pixels);
    return hr;
}

// [v9.2] Redesigned Format Detection: Extension-First for RAW
// RAW formats are too varied (40+ types using TIFF/ISOBMFF/proprietary containers).
// Extension is the ONLY reliable indicator for RAW. Magic bytes are unreliable.
static std::wstring DetectFormatFromContent(LPCWSTR filePath) {
    if (!filePath) return L"Unknown";
    
    // === STEP 1: Extension-based RAW Detection (Highest Priority) ===
    // This MUST run first. RAW formats cannot be reliably detected by magic bytes.
    std::wstring pathLower = filePath;
    std::transform(pathLower.begin(), pathLower.end(), pathLower.begin(), ::towlower);
    
    // Comprehensive LibRaw Extension List (40+ formats) + SVG
    static const wchar_t* rawExts[] = {
        L".3fr", L".ari", L".arw", L".bay", L".braw", L".cr2", L".cr3", L".crw", L".cap", L".data", L".dcs", L".dcr", 
        L".dng", L".drf", L".eip", L".erf", L".fff", L".gpr", L".iiq", L".k25", L".kdc", L".mdc", L".mef", 
        L".mos", L".mrw", L".nef", L".nrw", L".obm", L".orf", L".pef", L".ptx", L".pxn", L".r3d", L".raf", 
        L".raw", L".rwl", L".rw2", L".rwz", L".sr2", L".srf", L".srw", L".sti", L".x3f",
        L".svg" // [v9.5] SVG detection
        // Note: .tif/.tiff excluded - standard TIFFs should use WIC, not LibRaw
    };
    
    for (const auto* ext : rawExts) {
        if (pathLower.ends_with(ext)) {
            if (wcscmp(ext, L".svg") == 0) return L"SVG"; // Special case for SVG
            return L"RAW"; // Definitive RAW identification by extension
        }
    }
    
    // === STEP 2: Magic Bytes Detection (for non-RAW formats) ===
    uint8_t magic[32] = {0}; 
    size_t read = PeekHeader(filePath, magic, 32);
    if (read == 0) return L"Unknown";
    
    std::wstring fmt = DetectFormatFromContent(magic, read);
    
    // === STEP 3: Extension Fallbacks (for problematic formats) ===
    // TGA: Some TGAs have no reliable magic signature
    if (fmt == L"Unknown") {
        if (pathLower.ends_with(L".tga")) return L"TGA";
        if (pathLower.ends_with(L".jxl")) return L"JXL";
        if (pathLower.ends_with(L".dds")) return L"DDS";
    }
    
    return fmt;
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


// High-Performance Library Includes

#include <webp/decode.h>     // libwebp
#include <libraw/libraw.h>   // libraw
#include <wincodec.h>

#include <string>
#include <algorithm>
#include <vector>
#include <sstream>
#include <iomanip>
#include "exif.h"
 // [v6.0] EasyExif
#include "ImageEngine.h"

// Wuffs (Google's memory-safe decoder)
// Implementation is in WuffsImpl.cpp with selective module loading

// [v5.3] Global storage - kept for internal decoder use, exposed via DecodeResult.metadata
std::wstring g_lastFormatDetails;
int g_lastExifOrientation = 1;

// Read EXIF Orientation from JPEG file (Tag 0x0112)
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

// [v6.2] Refined EasyExif Populator (Fixes Date Priority & 35mm)
// Moved to Top for visibility to JXL/AVIF loaders
static void PopulateMetadataFromEasyExif_Refined(const easyexif::EXIFInfo& exif, CImageLoader::ImageMetadata& meta) {
    if (!exif.Make.empty()) meta.Make = std::wstring(exif.Make.begin(), exif.Make.end());
    if (!exif.Model.empty()) meta.Model = std::wstring(exif.Model.begin(), exif.Model.end());
    if (!exif.Software.empty()) meta.Software = std::wstring(exif.Software.begin(), exif.Software.end());
    if (!exif.LensInfo.Model.empty()) meta.Lens = std::wstring(exif.LensInfo.Model.begin(), exif.LensInfo.Model.end());
    
    // Date: FORCE OVERWRITE (Priority: EXIF > FileSystem)
    if (!exif.DateTimeOriginal.empty()) {
        meta.Date = std::wstring(exif.DateTimeOriginal.begin(), exif.DateTimeOriginal.end());
    } else if (!exif.DateTime.empty()) {
        meta.Date = std::wstring(exif.DateTime.begin(), exif.DateTime.end());
    }

    // Exposure
    if (exif.ISOSpeedRatings > 0) meta.ISO = std::to_wstring(exif.ISOSpeedRatings);
    
    if (exif.ExposureTime > 0) {
        wchar_t buf[32];
        if (exif.ExposureTime >= 1.0) swprintf_s(buf, L"%.1fs", exif.ExposureTime);
        else swprintf_s(buf, L"1/%.0fs", 1.0 / exif.ExposureTime);
        meta.Shutter = buf;
    }
    
    if (exif.FNumber > 0) {
        wchar_t buf[32]; swprintf_s(buf, L"f/%.1f", exif.FNumber);
        meta.Aperture = buf;
    }
    
    // Focal Length & 35mm Equivalent
    if (exif.FocalLength > 0) {
        wchar_t buf[64];
        if (exif.LensInfo.FocalLengthIn35mm > 0) {
             swprintf_s(buf, L"%.0fmm (%.0fmm)", exif.FocalLength, exif.LensInfo.FocalLengthIn35mm);
             meta.Focal35mm = std::to_wstring((int)exif.LensInfo.FocalLengthIn35mm) + L"mm";
        } else {
             swprintf_s(buf, L"%.0fmm", exif.FocalLength);
        }
        meta.Focal = buf;
    }
    
    if (std::abs(exif.ExposureBiasValue) > 0.001) {
        wchar_t buf[32]; swprintf_s(buf, L"%+.1f ev", exif.ExposureBiasValue);
        meta.ExposureBias = buf;
    }
    
    // Flash
    meta.Flash = (exif.Flash & 1) ? L"Flash: On" : L"Flash: Off";
    
    // GPS
    if (exif.GeoLocation.Latitude != 0 || exif.GeoLocation.Longitude != 0) {
        meta.HasGPS = true;
        meta.Latitude = exif.GeoLocation.Latitude;
        meta.Longitude = exif.GeoLocation.Longitude;
        meta.Altitude = exif.GeoLocation.Altitude; 
    }

    // [v6.2] Level 1: EXIF Color Space (Fastest)
    if (exif.ColorSpace == 1) meta.ColorSpace = L"sRGB";
    else if (exif.ColorSpace == 2) meta.ColorSpace = L"Adobe RGB";
    else if (exif.ColorSpace == 65535) meta.ColorSpace = L"Uncalibrated";

    // [Fix] Populate Orientation!
    if (exif.Orientation >= 1 && exif.Orientation <= 8) {
        meta.ExifOrientation = exif.Orientation;
    }
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

// [CMS/PMR] PMR-aware file reader
static bool ReadFileToPMR(LPCWSTR filePath, std::pmr::vector<uint8_t>& buffer, std::pmr::memory_resource* mr) {
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
// [Titan Engine] Region Decoding Implementation
// ----------------------------------------------------------------------------

// Forward declaration


// Logic: Robust TurboJPEG 3 Region Decoding
// Logic: Robust TurboJPEG 3 Region Decoding with Software Resize Support
static HRESULT LoadJpegRegion_V3(const uint8_t* buf, size_t bufSize, QuickView::RegionRect rect, float scale, 
                               QuickView::RawImageFrame* outFrame, QuickView::TileMemoryManager* tileManager, QuantumArena* arena,
                               int explicitTargetW = 0, int explicitTargetH = 0)
{
    // Initialize v3 Decompressor
    tjhandle tj = tj3Init(TJINIT_DECOMPRESS);
    if (!tj) return E_FAIL;
    struct TjGuard { tjhandle h; ~TjGuard() { if(h) tj3Destroy(h); } } guard{tj};

    if (tj3DecompressHeader(tj, buf, bufSize) != 0) return E_FAIL;

    int width = tj3Get(tj, TJPARAM_JPEGWIDTH);
    int height = tj3Get(tj, TJPARAM_JPEGHEIGHT);

    // 1. Calculate Scaling Factor
    int numFactors;
    tjscalingfactor* factors = tj3GetScalingFactors(&numFactors);
    tjscalingfactor chosen = {1, 1};

    // [Safety] Bounds Check Input
    if (rect.w <= 0 || rect.h <= 0) return E_INVALIDARG;
    
    // TurboJPEG supports max 1/8.
    float bestDiff = 100.0f;
    for (int i = 0; i < numFactors; i++) {
        float f = (float)factors[i].num / factors[i].denom;
        float diff = std::abs(f - scale);
        if (diff < bestDiff) {
            bestDiff = diff;
            chosen = factors[i];
        }
    }
    
    if (tj3SetScalingFactor(tj, chosen) != 0) return E_FAIL;

    // 2. Calculate Cropping in SCALED coordinates (TurboJPEG space)
    int tjScdX = TJSCALED(rect.x, chosen);
    int tjScdY = TJSCALED(rect.y, chosen);
    int tjScdW = TJSCALED(rect.w, chosen);
    int tjScdH = TJSCALED(rect.h, chosen);
    
    // Bounds check
    int fullScaledW = TJSCALED(width, chosen);
    int fullScaledH = TJSCALED(height, chosen);
    if (tjScdX + tjScdW > fullScaledW) tjScdW = fullScaledW - tjScdX;
    if (tjScdY + tjScdH > fullScaledH) tjScdH = fullScaledH - tjScdY;
    
    if (tjScdW <= 0 || tjScdH <= 0 || tjScdX < 0 || tjScdY < 0) return E_FAIL;

    // 3. Set Cropping Region
    tjregion cropRegion = { tjScdX, tjScdY, tjScdW, tjScdH };
    if (tj3SetCroppingRegion(tj, cropRegion) != 0) return E_FAIL;

    // 4. Determine Input vs Output Size
    // 4. Determine Input vs Output Size
    
    // [Fix] Calculate Content Size based on ACTUAL decoded data (clipped)
    // If we use rect.w/h directly, we might define a 32x32 target for a 512x512 request
    // even if we only have 20x20 pixels of data available (clipped edge).
    // This causes the resizer to stretch 20px -> 32px.
    // We must scale relative to the TurboJPEG output.
    
    float tjScale = (float)chosen.num / chosen.denom;
    float relativeScale = scale / tjScale;
    
    int contentW = (int)(tjScdW * relativeScale + 0.5f);
    int contentH = (int)(tjScdH * relativeScale + 0.5f);
    
    if (contentW <= 0) contentW = 1;
    if (contentH <= 0) contentH = 1;
    
    // Frame Dimensions (Allocation size - can be larger for padding)
    // If explicit target is provided, use it. Otherwise match content.
    int frameW = (explicitTargetW > 0) ? explicitTargetW : contentW;
    int frameH = (explicitTargetH > 0) ? explicitTargetH : contentH;
    
    // Ensure we don't allocate smaller than content (clipping is okay? No, usually expands)
    // Actually if frameW < contentW, we might clip content. But usually we use this for padding.

    // Check if we need software downscaling
    // Note: If relativeScale < 1.0, we are downscaling further from the TJ output
    bool needResize = (tjScdW > (int)(contentW * 1.2f)) || (tjScdH > (int)(contentH * 1.2f));

    // Allocate Pixel Buffer
    uint8_t* pDecodeBuf = nullptr;
    int decodeStride = 0;
    
    if (needResize) {
        // [Intermediate Decode]
        // We decode to a temporary buffer (potentially large)
        decodeStride = tjScdW * 4;
        size_t decodeSize = (size_t)decodeStride * tjScdH;
        
        if (arena) {
            pDecodeBuf = (uint8_t*)arena->Allocate(decodeSize, 64);
        }
        if (!pDecodeBuf) {
            pDecodeBuf = (uint8_t*)_aligned_malloc(decodeSize, 64);
        }
        if (!pDecodeBuf) return E_OUTOFMEMORY;
        
        // Decompress to Temp
        if (tj3Decompress8(tj, buf, bufSize, pDecodeBuf, decodeStride, TJPF_BGRX) != 0) {
            if (!arena || !arena->Owns(pDecodeBuf)) _aligned_free(pDecodeBuf);
            return E_FAIL;
        }

        // [Final Output]
        outFrame->width = frameW;
        outFrame->height = frameH;
        outFrame->stride = CalculateAlignedStride(frameW, 4);
        outFrame->format = PixelFormat::BGRA8888;
        outFrame->formatDetails = L"JPEG Region (Resized+Pad)";

        size_t finalSize = outFrame->GetBufferSize();
        
        if (tileManager && finalSize <= TILE_SLAB_SIZE) {
            outFrame->pixels = (uint8_t*)tileManager->Allocate();
            if (outFrame->pixels) {
                outFrame->memoryDeleter = [tileManager](uint8_t* p) { tileManager->Free(p); };
            }
        }
        if (!outFrame->pixels) {
            outFrame->pixels = (uint8_t*)_aligned_malloc(finalSize, 64);
            outFrame->memoryDeleter = [](uint8_t* p) { _aligned_free(p); };
        }
        
        if (!outFrame->pixels) {
            if (!arena || !arena->Owns(pDecodeBuf)) _aligned_free(pDecodeBuf);
            return E_OUTOFMEMORY;
        }

        // Default Init (for padding)
        // If content < frame, we must zero out the whole buffer or just the padding?
        // Safest: Zero entire buffer if padding needed.
        if (contentH < frameH || contentW < frameW) {
             memset(outFrame->pixels, 0, finalSize);
        }

        // Software Resize
        // We resize into the top-left 'contentW x contentH' of the buffer.
        SIMDUtils::ResizeBilinear(pDecodeBuf, tjScdW, tjScdH, 0 /*stride*/, outFrame->pixels, contentW, contentH, outFrame->stride);

        // [CMS] Extract ICC Profile from TurboJPEG (Software Resized)
        uint8_t* iccBuf = nullptr;
        size_t iccSize = 0;
        if (tj3GetICCProfile(tj, &iccBuf, &iccSize) == 0 && iccSize > 0) {
            outFrame->iccProfile.assign(iccBuf, iccBuf + iccSize);
            tj3Free(iccBuf);
        }

        // Free temp
        if (!arena || !arena->Owns(pDecodeBuf)) _aligned_free(pDecodeBuf);
        
    } else {
        // [Direct Decode] - No resize needed (or close enough)
        // We decode directly into the final buffer.
        
        outFrame->width = frameW;
        outFrame->height = frameH;
        outFrame->stride = CalculateAlignedStride(frameW, 4);
        outFrame->format = PixelFormat::BGRA8888;
        outFrame->formatDetails = L"JPEG Region";
        
        size_t totalSize = outFrame->GetBufferSize();
        
        if (tileManager && totalSize <= TILE_SLAB_SIZE) {
            outFrame->pixels = (uint8_t*)tileManager->Allocate();
            if (outFrame->pixels) {
                outFrame->memoryDeleter = [tileManager](uint8_t* p) { tileManager->Free(p); };
            }
        }
        
        if (!outFrame->pixels) {
             if (arena) {
                outFrame->pixels = (uint8_t*)arena->Allocate(totalSize, 64);
                outFrame->memoryDeleter = nullptr; // Arena managed
            } else {
                outFrame->pixels = (uint8_t*)_aligned_malloc(totalSize, 64);
                outFrame->memoryDeleter = [](uint8_t* p) { _aligned_free(p); };
            }
        }
        
        if (!outFrame->pixels) return E_OUTOFMEMORY;
        
        // If padding required, zero init first (or memset the gap later)
        // Since TurboJPEG writes row by row, pre-zeroing is safest/easiest.
        if (frameH > tjScdH || frameW > tjScdW) {
            memset(outFrame->pixels, 0, totalSize);
        }
        
        // Decompress (writes only tjScdW x tjScdH)
        // Stride handles the width.
        if (tj3Decompress8(tj, buf, bufSize, outFrame->pixels, outFrame->stride, TJPF_BGRX) != 0) {
            return E_FAIL;
        }

        // [CMS] Extract ICC Profile from TurboJPEG (Direct Decode)
        uint8_t* iccBuf = nullptr;
        size_t iccSize = 0;
        if (tj3GetICCProfile(tj, &iccBuf, &iccSize) == 0 && iccSize > 0) {
            outFrame->iccProfile.assign(iccBuf, iccBuf + iccSize);
            tj3Free(iccBuf);
        }
    }

    return S_OK;
}

// [Safety] SEH Helper to isolate C++ Unwinding from __try/__except (Fix C2712)
// This function must NOT have any local C++ objects with destructors.
static HRESULT SafeLoadJpegRegion(
    const uint8_t* data, size_t size, 
    QuickView::RegionRect rect, float scale, 
    QuickView::RawImageFrame* out, 
    QuickView::TileMemoryManager* tileManager, QuantumArena* arena,
    int explicitTargetW = 0, int explicitTargetH = 0) 
{
    __try {
        return LoadJpegRegion_V3(data, size, rect, scale, out, tileManager, arena, explicitTargetW, explicitTargetH);
    }
    __except (GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        OutputDebugStringW(L"[ImageLoader] CRITICAL: ReadFile fault (Network lost?)\n");
        return HRESULT_FROM_WIN32(ERROR_READ_FAULT);
    }
}

// [Optimization] FileMappingCache REMOVED - using MappedFile locally or pass-through


// [Infinity Engine] Zero-Copy Tile Loader Implementation
// [Infinity Engine] Zero-Copy Tile Loader Implementation
// ----------------------------------------------------------------------------
// Internal Implementation (Contains C++ Destructors, so NO SEH here!)
// ----------------------------------------------------------------------------
// [Infinity Engine] Zero-Copy Tile Loader Implementation
// ----------------------------------------------------------------------------
// SEH Wrapper (No Destructors Allowed Here)
// ----------------------------------------------------------------------------
HRESULT CImageLoader::LoadTileFromMemory(
    const uint8_t* sourceData, size_t sourceSize,
    QuickView::RegionRect region, float scale,
    QuickView::RawImageFrame* outFrame,
    QuickView::TileMemoryManager* tileManager,
    int targetWidth, int targetHeight) 
{
    if (!sourceData || !outFrame) return E_INVALIDARG;

    // [Safety] Wrap everything in __try/__except to catch MMF read faults
    __try {
        // [Fix] Unified Logic: Use V3 Loader which supports:
        // 1. Software Downscaling (fixing Jigsaw puzzle 1/16 vs 1/8 issue)
        // 2. Proper Padding (fixing Edge Smearing)
        // 3. Tile Slab Allocation
        return LoadJpegRegion_V3(sourceData, sourceSize, region, scale, outFrame, tileManager, nullptr /*arena*/, targetWidth, targetHeight);
    } 
    __except (GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        OutputDebugStringW(L"[ImageLoader] CRITICAL: ReadFile fault during Tile Decode\n");
        return HRESULT_FROM_WIN32(ERROR_READ_FAULT);
    }
}

// ============================================================================
// [P15] Format-Agnostic Full Decode from Memory
// ============================================================================
// Decodes entire image from memory to BGRA pixels (heap-allocated).
// Used by HeavyLanePool::FullDecodeAndCacheLOD for non-JPEG formats.
HRESULT CImageLoader::FullDecodeFromMemory(const uint8_t* data, size_t size,
                                            QuickView::RawImageFrame* outFrame) {
    if (!data || size < 4 || !outFrame) return E_INVALIDARG;

    std::wstring fmt = DetectFormatFromContent(data, size);

    // ---------------------------------------------------------------
    // JPEG path: delegate to existing TurboJPEG loader
    // ---------------------------------------------------------------
    if (fmt == L"JPEG") {
        QuickView::RegionRect fullRegion = { 0, 0, 0x7fffffff, 0x7fffffff };
        return LoadTileFromMemory(data, size, fullRegion, 1.0f, outFrame, nullptr, 0, 0);
    }

    // ---------------------------------------------------------------
    // PNG path: Wuffs (AVX2 accelerated, outputs BGRA directly)
    // ---------------------------------------------------------------
    if (fmt == L"PNG") {
        uint32_t w = 0, h = 0;
        uint8_t* directBuf = nullptr;
        
        // Use AllocatorFunc overload: Wuffs writes directly into our _aligned_malloc buffer.
        // This eliminates the intermediate std::vector<uint8_t> (4.8GB for 40000x30000).
        auto allocator = [&](size_t sz) -> uint8_t* {
            directBuf = (uint8_t*)_aligned_malloc(sz, 32);
            return directBuf;
        };
        
        if (!WuffsLoader::DecodePNG(data, size, &w, &h, allocator, nullptr)) {
            if (directBuf) _aligned_free(directBuf);
            wchar_t dbg[128];
            size_t neededMB = ((size_t)w * h * 4) / (1024 * 1024);
            swprintf_s(dbg, L"[P15] Wuffs PNG decode failed (need ~%zu MB BGRA)\n", neededMB);
            OutputDebugStringW(dbg);
            return E_FAIL;
        }

        if (!directBuf) return E_OUTOFMEMORY;
        
        outFrame->pixels = directBuf;
        outFrame->width = (int)w;
        outFrame->height = (int)h;
        outFrame->stride = (int)((size_t)w * 4);
        outFrame->memoryDeleter = [](uint8_t* p) { _aligned_free(p); };

        OutputDebugStringW(L"[P15] PNG decoded via Wuffs OK\n");
        return S_OK;
    }

    // ---------------------------------------------------------------
    // JXL path: libjxl (RGBA output → SIMD BGRA swizzle)
    // ---------------------------------------------------------------
    if (fmt == L"JXL") {
        JxlDecoder* dec = JxlDecoderCreate(NULL);
        if (!dec) return E_OUTOFMEMORY;

        void* runner = CImageLoader::GetJxlRunner();
        if (!runner) { JxlDecoderDestroy(dec); return E_OUTOFMEMORY; }
        JxlDecoderSetParallelRunner(dec, JxlThreadParallelRunner, runner);

        if (JXL_DEC_SUCCESS != JxlDecoderSubscribeEvents(dec,
            JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE)) {
            JxlDecoderDestroy(dec);
            return E_FAIL;
        }
        JxlDecoderSetInput(dec, data, size);
        JxlDecoderCloseInput(dec);

        JxlBasicInfo info = {};
        JxlPixelFormat pixFmt = { 4, JXL_TYPE_UINT8, JXL_LITTLE_ENDIAN, 0 };
        uint8_t* outBuf = nullptr;
        size_t outBufSize = 0;
        HRESULT hr = E_FAIL;

        for (;;) {
            JxlDecoderStatus st = JxlDecoderProcessInput(dec);
            if (st == JXL_DEC_ERROR) break;
            if (st == JXL_DEC_SUCCESS) { hr = S_OK; break; }

            if (st == JXL_DEC_BASIC_INFO) {
                if (JXL_DEC_SUCCESS != JxlDecoderGetBasicInfo(dec, &info)) break;

                // Force sRGB output
                JxlColorEncoding ce = {};
                ce.color_space = JXL_COLOR_SPACE_RGB;
                ce.white_point = JXL_WHITE_POINT_D65;
                ce.primaries = JXL_PRIMARIES_SRGB;
                ce.transfer_function = JXL_TRANSFER_FUNCTION_SRGB;
                ce.rendering_intent = JXL_RENDERING_INTENT_PERCEPTUAL;
                JxlDecoderSetOutputColorProfile(dec, &ce, NULL, 0);
            }
            else if (st == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
                if (JXL_DEC_SUCCESS != JxlDecoderImageOutBufferSize(dec, &pixFmt, &outBufSize)) break;

                // [Fix JXL Titan] Memory safety: check available RAM before attempting massive allocation.
                // Without this, _aligned_malloc(4.8GB) for a 40000x30000 image causes ACCESS_VIOLATION
                // in the subprocess, which then cascades into infinite retry loops.
                {
                    MEMORYSTATUSEX ms = { sizeof(ms) };
                    if (GlobalMemoryStatusEx(&ms)) {
                        if (outBufSize > (ms.ullAvailPhys * 6 / 10)) { // > 60% of available RAM
                            wchar_t dbg[192];
                            swprintf_s(dbg, L"[P15] JXL decode aborted: need %zu MB but only %llu MB available\n",
                                       outBufSize / (1024*1024), ms.ullAvailPhys / (1024*1024));
                            OutputDebugStringW(dbg);
                            JxlDecoderDestroy(dec);
                            return E_OUTOFMEMORY;
                        }
                    }
                }

                outBuf = (uint8_t*)_aligned_malloc(outBufSize, 32);
                if (!outBuf) { hr = E_OUTOFMEMORY; break; }

                if (JXL_DEC_SUCCESS != JxlDecoderSetImageOutBuffer(dec, &pixFmt, outBuf, outBufSize)) {
                    _aligned_free(outBuf); outBuf = nullptr;
                    break;
                }
            }
            else if (st == JXL_DEC_FULL_IMAGE) {
                hr = S_OK;
                break;
            }
        }

        JxlDecoderDestroy(dec);

        if (FAILED(hr) || !outBuf) {
            if (outBuf) _aligned_free(outBuf);
            OutputDebugStringW(L"[P15] JXL decode failed\n");
            return FAILED(hr) ? hr : E_FAIL;
        }

        // RGBA → BGRA swizzle (SIMD)
        SIMDUtils::SwizzleRGBA_to_BGRA_Premul(outBuf, (size_t)info.xsize * info.ysize);

        outFrame->pixels = outBuf;
        outFrame->width = (int)info.xsize;
        outFrame->height = (int)info.ysize;
        outFrame->stride = (int)(info.xsize * 4);
        outFrame->memoryDeleter = [](uint8_t* p) { _aligned_free(p); };

        wchar_t dbg[128];
        swprintf_s(dbg, L"[P15] JXL decoded via libjxl OK (%ux%u)\n", info.xsize, info.ysize);
        OutputDebugStringW(dbg);
        return S_OK;
    }


    // ---------------------------------------------------------------
    // Fallback: unsupported format for tile decode
    // ---------------------------------------------------------------
    OutputDebugStringW(L"[P15] FullDecodeFromMemory: unsupported format\n");
    return E_NOTIMPL;
}

// ============================================================================
// [Direct-to-MMF] Zero-Heap Full Decode into External Buffer
// ============================================================================
// Decodes entire image from memory, writing output pixels DIRECTLY into
// the caller-provided buffer (typically a MMF view). This eliminates all
// heap allocations for the pixel data — the OS VMM handles page faults
// and disk-backed paging, so even 4.8GB outputs work on 8GB RAM machines.
//
// Supported formats: JXL, PNG. Others fall back to FullDecodeFromMemory + memcpy.
// Returns: S_OK on success, dimensions written to outW/outH/outStride.
HRESULT CImageLoader::FullDecodeToMMF(const uint8_t* data, size_t size,
                                       uint8_t* mmfBuf, size_t mmfBufSize,
                                       int* outW, int* outH, int* outStride,
                                       std::function<void()> dcReadyCallback) {
    if (!data || size < 4 || !mmfBuf || !outW || !outH || !outStride) return E_INVALIDARG;

    std::wstring fmt = DetectFormatFromContent(data, size);

    // ---------------------------------------------------------------
    // JXL: libjxl natively supports external output buffer via
    // JxlDecoderSetImageOutBuffer — zero-copy into MMF view.
    // [Progressive DC] When libjxl reaches the DC (1:8) stage, we flush
    // the upsampled DC data into the MMF and call dcReadyCallback so
    // tiles can start immediately from the blurry preview.
    // ---------------------------------------------------------------
    if (fmt == L"JXL") {
        JxlDecoder* dec = JxlDecoderCreate(NULL);
        if (!dec) return E_OUTOFMEMORY;

        // [Fix] Create a DEDICATED runner for this decode session.
        // The global GetJxlRunner() is shared with metadata reads (LoadMetadata),
        // LoadJXL, and other consumers across threads. JxlThreadParallelRunner
        // is NOT thread-safe for concurrent use — corrupts internal state and
        // causes JXL_DEC_ERROR on subsequent decodes.
        size_t runnerThreads = std::thread::hardware_concurrency();
        if (runnerThreads == 0) runnerThreads = 4;
        void* runner = JxlThreadParallelRunnerCreate(NULL, runnerThreads);
        if (!runner) { JxlDecoderDestroy(dec); return E_OUTOFMEMORY; }
        JxlDecoderSetParallelRunner(dec, JxlThreadParallelRunner, runner);

        // Subscribe to FRAME_PROGRESSION to catch the DC (1:8) stage
        int events = JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE | JXL_DEC_FRAME_PROGRESSION;
        if (JXL_DEC_SUCCESS != JxlDecoderSubscribeEvents(dec, events)) {
            JxlDecoderDestroy(dec);
            JxlThreadParallelRunnerDestroy(runner);
            return E_FAIL;
        }
        // Request DC-level progressive detail
        JxlDecoderSetProgressiveDetail(dec, JxlProgressiveDetail::kDC);

        JxlDecoderSetInput(dec, data, size);
        JxlDecoderCloseInput(dec);

        JxlBasicInfo info = {};
        JxlPixelFormat pixFmt = { 4, JXL_TYPE_UINT8, JXL_LITTLE_ENDIAN, 0 };
        bool bufferSet = false;
        HRESULT hr = E_FAIL;

        for (;;) {
            JxlDecoderStatus st = JxlDecoderProcessInput(dec);
            if (st == JXL_DEC_ERROR) break;
            if (st == JXL_DEC_SUCCESS) { hr = S_OK; break; }

            if (st == JXL_DEC_BASIC_INFO) {
                if (JXL_DEC_SUCCESS != JxlDecoderGetBasicInfo(dec, &info)) break;

                // Force sRGB output
                JxlColorEncoding ce = {};
                ce.color_space = JXL_COLOR_SPACE_RGB;
                ce.white_point = JXL_WHITE_POINT_D65;
                ce.primaries = JXL_PRIMARIES_SRGB;
                ce.transfer_function = JXL_TRANSFER_FUNCTION_SRGB;
                ce.rendering_intent = JXL_RENDERING_INTENT_PERCEPTUAL;
                JxlDecoderSetOutputColorProfile(dec, &ce, NULL, 0);
            }
            else if (st == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
                size_t needed = 0;
                if (JXL_DEC_SUCCESS != JxlDecoderImageOutBufferSize(dec, &pixFmt, &needed)) break;

                if (needed > mmfBufSize) {
                    wchar_t dbg[192];
                    swprintf_s(dbg, L"[MMF] JXL needs %zu MB but MMF is %zu MB\n",
                               needed / (1024*1024), mmfBufSize / (1024*1024));
                    OutputDebugStringW(dbg);
                    JxlDecoderDestroy(dec);
                    JxlThreadParallelRunnerDestroy(runner);
                    return E_OUTOFMEMORY;
                }

                // Direct-to-MMF: libjxl writes directly into the MMF view!
                if (JXL_DEC_SUCCESS != JxlDecoderSetImageOutBuffer(dec, &pixFmt, mmfBuf, needed)) {
                    JxlDecoderDestroy(dec);
                    JxlThreadParallelRunnerDestroy(runner);
                    return E_FAIL;
                }
                bufferSet = true;
            }
            else if (st == JXL_DEC_FRAME_PROGRESSION) {
                // [Progressive DC] libjxl has decoded the DC (1:8) layer.
                // NOTE: We intentionally do NOT call FlushImage or SwizzleRGBA_to_BGRA here.
                // Reason: After FlushImage, libjxl continues writing RGBA AC data into the
                // same MMF buffer, overwriting the BGRA-swizzled DC data. This causes LOD=3/2
                // to read a mix of BGRA (unoverwritten DC) and RGBA (new AC) = color corruption.
                // The safe path: let the full decode complete, swizzle once at the end.
                size_t ratio = JxlDecoderGetIntendedDownsamplingRatio(dec);

                wchar_t dbg[192];
                swprintf_s(dbg, L"[MMF] JXL FRAME_PROGRESSION: ratio=%zu, image=%ux%u (logged only, no flush)\n",
                           ratio, info.xsize, info.ysize);
                OutputDebugStringW(dbg);
                // Continue decode loop for full quality
            }
            else if (st == JXL_DEC_FULL_IMAGE) {
                hr = S_OK;
                break;
            }
        }

        JxlDecoderDestroy(dec);
        JxlThreadParallelRunnerDestroy(runner);

        if (FAILED(hr) || !bufferSet) {
            OutputDebugStringW(L"[MMF] JXL decode to MMF failed\n");
            return FAILED(hr) ? hr : E_FAIL;
        }

        // Final RGBA → BGRA swizzle for the full-quality image
        // (DC was already swizzled above, but full decode overwrites with RGBA again)
        SIMDUtils::SwizzleRGBA_to_BGRA_Premul(mmfBuf, (size_t)info.xsize * info.ysize);

        *outW = (int)info.xsize;
        *outH = (int)info.ysize;
        *outStride = (int)(info.xsize * 4);

        wchar_t dbg[128];
        swprintf_s(dbg, L"[MMF] JXL decoded direct-to-MMF OK (%ux%u)\n", info.xsize, info.ysize);
        OutputDebugStringW(dbg);
        return S_OK;
    }

    // ---------------------------------------------------------------
    // PNG: Wuffs AllocatorFunc redirect to MMF view
    // ---------------------------------------------------------------
    if (fmt == L"PNG") {
        uint32_t w = 0, h = 0;
        bool allocOK = true;

        auto allocator = [&](size_t sz) -> uint8_t* {
            if (sz > mmfBufSize) { allocOK = false; return nullptr; }
            return mmfBuf; // Direct-to-MMF: Wuffs writes directly into MMF view
        };

        if (!WuffsLoader::DecodePNG(data, size, &w, &h, allocator, nullptr) || !allocOK) {
            OutputDebugStringW(L"[MMF] Wuffs PNG decode to MMF failed\n");
            return E_FAIL;
        }

        *outW = (int)w;
        *outH = (int)h;
        *outStride = (int)((size_t)w * 4);

        wchar_t dbg[128];
        swprintf_s(dbg, L"[MMF] PNG decoded direct-to-MMF OK (%ux%u)\n", w, h);
        OutputDebugStringW(dbg);
        return S_OK;
    }

    // ---------------------------------------------------------------
    // Fallback: Decode to heap, memcpy into MMF view
    // Works for AVIF, HEIC, TIFF, and any other format.
    // ---------------------------------------------------------------
    QuickView::RawImageFrame heapFrame;
    HRESULT hr = FullDecodeFromMemory(data, size, &heapFrame);
    if (FAILED(hr) || !heapFrame.IsValid()) return FAILED(hr) ? hr : E_FAIL;

    size_t needed = (size_t)heapFrame.stride * (size_t)heapFrame.height;
    if (needed > mmfBufSize) {
        wchar_t dbg[192];
        swprintf_s(dbg, L"[MMF] Fallback: need %zu MB but MMF is %zu MB\n",
                   needed / (1024*1024), mmfBufSize / (1024*1024));
        OutputDebugStringW(dbg);
        return E_OUTOFMEMORY;
    }

    memcpy(mmfBuf, heapFrame.pixels, needed);

    *outW = heapFrame.width;
    *outH = heapFrame.height;
    *outStride = heapFrame.stride;

    wchar_t dbg[128];
    swprintf_s(dbg, L"[MMF] %ls decoded via fallback+copy OK (%dx%d)\n",
               fmt.c_str(), heapFrame.width, heapFrame.height);
    OutputDebugStringW(dbg);
    return S_OK;
}




namespace {
struct RegionScalePlan {
    int cropX = 0;
    int cropY = 0;
    int cropW = 0;
    int cropH = 0;
    int contentW = 0;
    int contentH = 0;
    int frameW = 0;
    int frameH = 0;
};

static bool BuildRegionScalePlan(QuickView::RegionRect srcRect,
                                 int imageW, int imageH,
                                 float scale,
                                 int explicitTargetW, int explicitTargetH,
                                 RegionScalePlan* out) {
    if (!out || imageW <= 0 || imageH <= 0) return false;

    if (!(scale > 0.0f)) scale = 1.0f;

    if (srcRect.x < 0) { srcRect.w += srcRect.x; srcRect.x = 0; }
    if (srcRect.y < 0) { srcRect.h += srcRect.y; srcRect.y = 0; }
    if (srcRect.x > imageW) srcRect.x = imageW;
    if (srcRect.y > imageH) srcRect.y = imageH;
    if (srcRect.x + srcRect.w > imageW) srcRect.w = imageW - srcRect.x;
    if (srcRect.y + srcRect.h > imageH) srcRect.h = imageH - srcRect.y;
    if (srcRect.w <= 0 || srcRect.h <= 0) return false;

    const int contentW = (std::max)(1, (int)(srcRect.w * scale + 0.5f));
    const int contentH = (std::max)(1, (int)(srcRect.h * scale + 0.5f));
    const int frameW = (explicitTargetW > 0) ? explicitTargetW : contentW;
    const int frameH = (explicitTargetH > 0) ? explicitTargetH : contentH;

    out->cropX = srcRect.x;
    out->cropY = srcRect.y;
    out->cropW = srcRect.w;
    out->cropH = srcRect.h;
    out->contentW = contentW;
    out->contentH = contentH;
    out->frameW = frameW;
    out->frameH = frameH;
    return true;
}
}

HRESULT CImageLoader::LoadRegionToFrame(LPCWSTR filePath, QuickView::RegionRect srcRect, float scale,
                          QuickView::RawImageFrame* outFrame,
                          QuickView::TileMemoryManager* tileManager,
                          class QuantumArena* arena,
                          std::wstring* pLoaderName,
                          CImageLoader::CancelPredicate checkCancel,
                          int targetWidth, int targetHeight) 
{
    if (!filePath || !outFrame) return E_INVALIDARG;

    // Detect format to choose the best strategy
    std::wstring format = DetectFormatFromContent(filePath);
    
    // --- Strategy 1: TurboJPEG Region Decoding (The most optimized path) ---
    if (format == L"JPEG") {
        if (pLoaderName) *pLoaderName = L"TurboJPEG Region";
        
        QuickView::MappedFile mapping(filePath); // [Opt] Local map (OS handles caching)
        if (!mapping.IsValid()) return E_FAIL;
        
        if (checkCancel && checkCancel()) return E_ABORT;
        return SafeLoadJpegRegion(mapping.data(), mapping.size(), srcRect, scale, outFrame, tileManager, arena, targetWidth, targetHeight);
    }

    // --- Strategy 1b: WebP Native Region Decoding ---
    if (format == L"WebP") {
        if (pLoaderName) *pLoaderName = L"WebP Native Region";
        return LoadWebPRegionToFrame(filePath, srcRect, scale, outFrame, tileManager, arena, checkCancel, targetWidth, targetHeight);
    }

    // --- Strategy 2: Strategy B (Load Full & Crop/Resize) ---
    // For formats without native region decoding support (PNG, etc.)
    if (format == L"PNG") {
        if (pLoaderName) *pLoaderName = format + L" Strategy-B";
        return LoadRegionGeneric_StrategyB(filePath, srcRect, scale, outFrame, tileManager, arena, checkCancel, targetWidth, targetHeight);
    }
    
    // --- [P15] JXL: Callback-based Region Decode (Avoids massive allocation) ---
    if (format == L"JXL") {
        if (pLoaderName) *pLoaderName = L"JXL Region Callback";
        return LoadJxlRegionToFrame(filePath, srcRect, scale, outFrame, tileManager, arena, checkCancel, targetWidth, targetHeight);
    }

    return E_NOTIMPL; 
}

HRESULT CImageLoader::LoadRegionGeneric_StrategyB(LPCWSTR filePath, QuickView::RegionRect srcRect, float scale, 
                                               QuickView::RawImageFrame* outFrame, 
                                               QuickView::TileMemoryManager* tileManager, 
                                               QuantumArena* arena, 
                                               CancelPredicate checkCancel,
                                               int explicitTargetW, int explicitTargetH) 
{
    // 1. Load the full image first
    QuickView::RawImageFrame fullFrame;
    HRESULT hr = LoadToFrame(filePath, &fullFrame, arena, 0, 0, nullptr, checkCancel);
    if (FAILED(hr)) return hr;

    RegionScalePlan plan{};
    if (!BuildRegionScalePlan(srcRect, fullFrame.width, fullFrame.height, scale,
                              explicitTargetW, explicitTargetH, &plan)) {
        return E_INVALIDARG;
    }

    outFrame->width = plan.frameW;
    outFrame->height = plan.frameH;
    outFrame->stride = CalculateAlignedStride(plan.frameW, 4);
    outFrame->format = fullFrame.format;
    
    size_t totalSize = outFrame->GetBufferSize();
    if (tileManager && totalSize <= TILE_SLAB_SIZE) {
        outFrame->pixels = (uint8_t*)tileManager->Allocate();
        if (outFrame->pixels) {
            outFrame->memoryDeleter = [tileManager](uint8_t* p) { tileManager->Free(p); };
        }
    }
    
    if (!outFrame->pixels) {
        outFrame->pixels = (uint8_t*)_aligned_malloc(totalSize, 64);
        outFrame->memoryDeleter = [](uint8_t* p) { _aligned_free(p); };
    }
    
    if (!outFrame->pixels) return E_OUTOFMEMORY;

    // Zero-fill padding if needed
    if (plan.contentH < plan.frameH || plan.contentW < plan.frameW) {
        memset(outFrame->pixels, 0, totalSize);
    }

    // Resize/Crop from Full Image to Out Frame
    SIMDUtils::ResizeBilinear(
        fullFrame.pixels + (size_t)plan.cropY * fullFrame.stride + (size_t)plan.cropX * 4,
        plan.cropW, plan.cropH, (int)fullFrame.stride,
        outFrame->pixels, plan.contentW, plan.contentH, outFrame->stride);

    return S_OK;
}

HRESULT CImageLoader::LoadWebPRegionToFrame(LPCWSTR filePath, QuickView::RegionRect srcRect, float scale,
                                            QuickView::RawImageFrame* outFrame,
                                            QuickView::TileMemoryManager* tileManager,
                                            QuantumArena* arena,
                                            CancelPredicate checkCancel,
                                            int explicitTargetW, int explicitTargetH,
                                            const uint8_t* mappedData, size_t mappedSize)
{
    // [WebP Native Region] Reuse caller-provided MMF view when available.
    const uint8_t* srcData = mappedData;
    size_t srcSize = mappedSize;
    std::unique_ptr<QuickView::MappedFile> mappingOwner;
    if (!srcData || srcSize == 0) {
        mappingOwner = std::make_unique<QuickView::MappedFile>(filePath);
        if (!mappingOwner->IsValid()) return E_FAIL;
        srcData = mappingOwner->data();
        srcSize = mappingOwner->size();
    }

    if (checkCancel && checkCancel()) return E_ABORT;

    WebPDecoderConfig config;
    if (!WebPInitDecoderConfig(&config)) return E_FAIL;
    
    // RAII for decoder output buffer
    struct ConfigGuard { WebPDecBuffer* b; ~ConfigGuard() { WebPFreeDecBuffer(b); } } cGuard{ &config.output };

    if (WebPGetFeatures(srcData, srcSize, &config.input) != VP8_STATUS_OK) return E_FAIL;

    RegionScalePlan plan{};
    if (!BuildRegionScalePlan(srcRect, config.input.width, config.input.height, scale,
                              explicitTargetW, explicitTargetH, &plan)) {
        return E_FAIL;
    }

    // 1. Cropping
    config.options.use_cropping = 1;
    config.options.crop_left = plan.cropX;
    config.options.crop_top = plan.cropY;
    config.options.crop_width = plan.cropW;
    config.options.crop_height = plan.cropH;

    // 2. Scaling
    if (plan.contentW != plan.cropW || plan.contentH != plan.cropH) {
        config.options.use_scaling = 1;
        config.options.scaled_width = plan.contentW;
        config.options.scaled_height = plan.contentH;
    }

    outFrame->width = plan.frameW;
    outFrame->height = plan.frameH;
    outFrame->stride = CalculateAlignedStride(plan.frameW, 4);
    outFrame->format = PixelFormat::BGRA8888;
    outFrame->formatDetails = L"WebP Region";

    size_t totalSize = outFrame->GetBufferSize();

    if (tileManager && totalSize <= TILE_SLAB_SIZE) {
        outFrame->pixels = (uint8_t*)tileManager->Allocate();
        if (outFrame->pixels) {
            outFrame->memoryDeleter = [tileManager](uint8_t* p) { tileManager->Free(p); };
        }
    }
    
    if (!outFrame->pixels) {
        if (arena) {
             outFrame->pixels = (uint8_t*)arena->Allocate(totalSize, 64);
             outFrame->memoryDeleter = nullptr; 
        } else {
             outFrame->pixels = (uint8_t*)_aligned_malloc(totalSize, 64);
             outFrame->memoryDeleter = [](uint8_t* p) { _aligned_free(p); };
        }
    }
    
    if (!outFrame->pixels) return E_OUTOFMEMORY;

    // Padding
    if (plan.contentH < plan.frameH || plan.contentW < plan.frameW) {
        memset(outFrame->pixels, 0, totalSize);
    }

    // Direct Decode setup
    config.output.colorspace = MODE_BGRA;
    config.output.is_external_memory = 1;
    config.output.u.RGBA.rgba = outFrame->pixels;
    config.output.u.RGBA.stride = outFrame->stride;
    config.output.u.RGBA.size = totalSize;
    config.options.use_threads = 1; 

    if (WebPDecode(srcData, srcSize, &config) != VP8_STATUS_OK) {
        return E_FAIL;
    }

    // WebP returns un-premultiplied. We use Premultiplied visually.
    if (config.input.has_alpha) {
        SIMDUtils::PremultiplyAlpha_BGRA(outFrame->pixels, plan.contentW, plan.contentH, outFrame->stride);
    }

    return S_OK;
}

// Struct to track state during JXL Callback
struct JxlCropCtx {
    uint8_t* tempBuf;
    int tempStride;
    int cropX, cropY, cropW, cropH;
};

static void JxlCropCallback(void* opaque, size_t x, size_t y, size_t num_pixels, const void* pixels) {
    JxlCropCtx* ctx = (JxlCropCtx*)opaque;
    if (!ctx->tempBuf) return;
    
    if (y < (size_t)ctx->cropY || y >= (size_t)(ctx->cropY + ctx->cropH)) return;
    if (x >= (size_t)(ctx->cropX + ctx->cropW) || x + num_pixels <= (size_t)ctx->cropX) return;
    
    size_t startX = std::max(x, (size_t)ctx->cropX);
    size_t endX = std::min(x + num_pixels, (size_t)(ctx->cropX + ctx->cropW));
    
    size_t copyPixels = endX - startX;
    size_t srcOffset = startX - x;
    
    size_t dstX = startX - ctx->cropX;
    size_t dstY = y - ctx->cropY;
    
    uint8_t* dst = ctx->tempBuf + dstY * ctx->tempStride + dstX * 4;
    const uint8_t* src = (const uint8_t*)pixels + srcOffset * 4;
    
    memcpy(dst, src, copyPixels * 4);
}

HRESULT CImageLoader::LoadJxlRegionToFrame(LPCWSTR filePath, QuickView::RegionRect srcRect, float scale,
                                           QuickView::RawImageFrame* outFrame,
                                           QuickView::TileMemoryManager* tileManager,
                                           QuantumArena* arena,
                                           CancelPredicate checkCancel,
                                           int explicitTargetW, int explicitTargetH,
                                           const uint8_t* mappedData, size_t mappedSize)
{
    // Reuse caller-provided MMF view when available.
    const uint8_t* srcData = mappedData;
    size_t srcSize = mappedSize;
    std::unique_ptr<QuickView::MappedFile> mappingOwner;
    if (!srcData || srcSize == 0) {
        mappingOwner = std::make_unique<QuickView::MappedFile>(filePath);
        if (!mappingOwner->IsValid()) return E_FAIL;
        srcData = mappingOwner->data();
        srcSize = mappingOwner->size();
    }

    if (checkCancel && checkCancel()) return E_ABORT;

    JxlDecoder* dec = JxlDecoderCreate(NULL);
    if (!dec) return E_OUTOFMEMORY;

    // RAII for decoder
    struct DecGuard { JxlDecoder* d; ~DecGuard() { JxlDecoderDestroy(d); } } dGuard{ dec };

    void* runner = CImageLoader::GetJxlRunner();
    if (runner) {
        JxlDecoderSetParallelRunner(dec, JxlThreadParallelRunner, runner);
    }

    if (JXL_DEC_SUCCESS != JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE)) {
        return E_FAIL;
    }

    JxlDecoderSetInput(dec, srcData, srcSize);
    JxlDecoderCloseInput(dec);

    JxlBasicInfo info = {};
    JxlPixelFormat pixFmt = { 4, JXL_TYPE_UINT8, JXL_LITTLE_ENDIAN, 0 };
    
    HRESULT hr = E_FAIL;
    RegionScalePlan plan{};
    bool planReady = false;

    uint8_t* tempBuf = nullptr;
    int tempStride = 0;
    
    JxlCropCtx ctx = {0};

    for (;;) {
        if (checkCancel && checkCancel()) {
            if (tempBuf && (!arena || !arena->Owns(tempBuf))) _aligned_free(tempBuf);
            return E_ABORT;
        }

        JxlDecoderStatus st = JxlDecoderProcessInput(dec);
        if (st == JXL_DEC_ERROR) {
             OutputDebugStringW(L"[JXL_ROI] Decoder Error during ProcessInput\n");
             break;
        }
        if (st == JXL_DEC_SUCCESS) { hr = S_OK; break; }

        if (st == JXL_DEC_BASIC_INFO) {
            if (JXL_DEC_SUCCESS != JxlDecoderGetBasicInfo(dec, &info)) {
                 OutputDebugStringW(L"[JXL_ROI] Failed to get Basic Info\n");
                 break;
            }

            // Force sRGB output to match full-decode color behavior.
            JxlColorEncoding ce = {};
            ce.color_space = JXL_COLOR_SPACE_RGB;
            ce.white_point = JXL_WHITE_POINT_D65;
            ce.primaries = JXL_PRIMARIES_SRGB;
            ce.transfer_function = JXL_TRANSFER_FUNCTION_SRGB;
            ce.rendering_intent = JXL_RENDERING_INTENT_PERCEPTUAL;
            // [Fix] Non-fatal: match full-decode path (line ~6865) which ignores this return value.
            // libjxl may reject sRGB override for some images but still decode correctly with native profile.
            JxlDecoderSetOutputColorProfile(dec, &ce, NULL, 0);

            if (!BuildRegionScalePlan(srcRect, (int)info.xsize, (int)info.ysize, scale,
                                      explicitTargetW, explicitTargetH, &plan)) {
                OutputDebugStringW(L"[JXL_ROI] Failed to Build Region Scale Plan\n");
                hr = E_FAIL;
                break;
            }
            planReady = true;

            // Allocate temp buffer for 1:1 cropped region
            tempStride = CalculateAlignedStride(plan.cropW, 4);
            size_t tempSize = (size_t)tempStride * plan.cropH;
            
            if (arena) {
                tempBuf = (uint8_t*)arena->Allocate(tempSize, 64);
            }
            if (!tempBuf) {
                tempBuf = (uint8_t*)_aligned_malloc(tempSize, 64);
            }
            if (!tempBuf) { hr = E_OUTOFMEMORY; break; }

            // Initialize context
            ctx.tempBuf = tempBuf;
            ctx.tempStride = tempStride;
            ctx.cropX = plan.cropX;
            ctx.cropY = plan.cropY;
            ctx.cropW = plan.cropW;
            ctx.cropH = plan.cropH;

            if (JXL_DEC_SUCCESS != JxlDecoderSetImageOutCallback(dec, &pixFmt, JxlCropCallback, &ctx)) {
                OutputDebugStringW(L"[JXL_ROI] Failed to Set Image Out Callback\n");
                hr = E_FAIL; break;
            }
        }
        else if (st == JXL_DEC_FULL_IMAGE) {
            hr = S_OK;
            break;
        }
        else if (st == JXL_DEC_NEED_MORE_INPUT) {
            OutputDebugStringW(L"[JXL_ROI] Need more input but input is closed!\n");
            break;
        }
    }

    if (FAILED(hr) || !tempBuf || !planReady) {
        wchar_t errObj[256];
        swprintf_s(errObj, L"[JXL_ROI] Failure Exit: hr=0x%08X, tempBuf=%p, planReady=%d\n", hr, tempBuf, planReady);
        OutputDebugStringW(errObj);
        if (tempBuf && (!arena || !arena->Owns(tempBuf))) _aligned_free(tempBuf);
        return hr == S_OK ? E_FAIL : hr;
    }

    // Allocate Output Frame
    outFrame->width = plan.frameW;
    outFrame->height = plan.frameH;
    outFrame->stride = CalculateAlignedStride(plan.frameW, 4);
    outFrame->format = PixelFormat::BGRA8888;
    outFrame->formatDetails = L"JXL Region Callback";

    size_t totalSize = outFrame->GetBufferSize();

    if (tileManager && totalSize <= TILE_SLAB_SIZE) {
        outFrame->pixels = (uint8_t*)tileManager->Allocate();
        if (outFrame->pixels) {
            outFrame->memoryDeleter = [tileManager](uint8_t* p) { tileManager->Free(p); };
        }
    }
    
    if (!outFrame->pixels) {
        if (arena) {
             outFrame->pixels = (uint8_t*)arena->Allocate(totalSize, 64);
             outFrame->memoryDeleter = nullptr; 
        } else {
             outFrame->pixels = (uint8_t*)_aligned_malloc(totalSize, 64);
             outFrame->memoryDeleter = [](uint8_t* p) { _aligned_free(p); };
        }
    }
    
    if (!outFrame->pixels) {
        if (tempBuf && (!arena || !arena->Owns(tempBuf))) _aligned_free(tempBuf);
        return E_OUTOFMEMORY;
    }

    // Zero-fill padding
    if (plan.contentH < plan.frameH || plan.contentW < plan.frameW) {
        memset(outFrame->pixels, 0, totalSize);
    }

    // Resize (or direct copy if scale == 1.0)
    // Remember libjxl outputs RGBA, we need BGRA
    SIMDUtils::SwizzleRGBA_to_BGRA_Premul(tempBuf, (size_t)plan.cropW * plan.cropH);
    
    SIMDUtils::ResizeBilinear(tempBuf, plan.cropW, plan.cropH, tempStride, 
                              outFrame->pixels, plan.contentW, plan.contentH, outFrame->stride);

    // Cleanup
    if (tempBuf && (!arena || !arena->Owns(tempBuf))) {
        _aligned_free(tempBuf);
    }

    return S_OK;
}

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

    // [Opt] Thread-Local Handle
    struct TjCtx { 
        tjhandle h; 
        TjCtx() { h = tj3Init(TJINIT_DECOMPRESS); } 
        ~TjCtx() { if(h) tj3Destroy(h); } 
    };
    static thread_local TjCtx t_ctx;
    
    tjhandle tj = t_ctx.h;
    if (!tj) return E_FAIL;
    
    // Reset defaults
    tjscalingfactor sf = {1, 1};
    tj3SetScalingFactor(tj, sf);

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



HRESULT CImageLoader::LoadShellThumbnail(LPCWSTR filePath, int targetSize, ThumbData* pData) {
    if (!filePath || !pData || !m_wicFactory) return E_INVALIDARG;

    ComPtr<IShellItemImageFactory> imageFactory;
    HRESULT hr = SHCreateItemFromParsingName(filePath, nullptr, IID_PPV_ARGS(&imageFactory));
    if (FAILED(hr) || !imageFactory) return hr;

    SIZE size = { targetSize, targetSize };
    HBITMAP hBitmap = nullptr;
    
    // Step 1: Request from shell cache (exactly target size) and strictly NO Icon fallback (SIIGBF_THUMBNAILONLY)
    hr = imageFactory->GetImage(size, static_cast<SIIGBF>(SIIGBF_THUMBNAILONLY | SIIGBF_INCACHEONLY), &hBitmap);
    
    // Step 2: Fallback to System Large Icon Cache size (256) if the requested size failed
    if (FAILED(hr) || !hBitmap) {
        SIZE fallbackSize = { 256, 256 };
        hr = imageFactory->GetImage(fallbackSize, static_cast<SIIGBF>(SIIGBF_THUMBNAILONLY | SIIGBF_INCACHEONLY), &hBitmap);
    }
    
    if (FAILED(hr) || !hBitmap) return E_FAIL;

    // Reject standard icon sizes to avoid meaningless fallback
    BITMAP bm;
    if (GetObject(hBitmap, sizeof(bm), &bm)) {
        if (bm.bmWidth == bm.bmHeight && (bm.bmWidth == 16 || bm.bmWidth == 32 || bm.bmWidth == 48 || bm.bmWidth == 64 || bm.bmWidth == 128)) {
            DeleteObject(hBitmap);
            return E_FAIL; // It's an icon, reject it
        }
    } else {
        DeleteObject(hBitmap);
        return E_FAIL;
    }

    ComPtr<IWICBitmap> wicBitmap;
    hr = m_wicFactory->CreateBitmapFromHBITMAP(hBitmap, nullptr, WICBitmapUsePremultipliedAlpha, &wicBitmap);
    DeleteObject(hBitmap);
    
    if (FAILED(hr) || !wicBitmap) return hr;
    
    ComPtr<IWICBitmapSource> sourceToCopy = wicBitmap;
    WICPixelFormatGUID srcFormat = {};
    if (SUCCEEDED(wicBitmap->GetPixelFormat(&srcFormat)) && !IsEqualGUID(srcFormat, GUID_WICPixelFormat32bppPBGRA)) {
        ComPtr<IWICFormatConverter> converter;
        if (SUCCEEDED(m_wicFactory->CreateFormatConverter(&converter)) && converter) {
            hr = converter->Initialize(wicBitmap.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0f, WICBitmapPaletteTypeCustom);
            if (SUCCEEDED(hr)) {
                sourceToCopy = converter;
            }
        }
    }

    UINT w = 0, h = 0;
    if (FAILED(sourceToCopy->GetSize(&w, &h)) || w == 0 || h == 0) return E_FAIL;

    UINT stride = QuickView::CalculateAlignedStride(w, 4);
    size_t byteCount = static_cast<size_t>(stride) * h;
    
    try {
        pData->pixels.resize(byteCount);
    } catch (...) {
        return E_OUTOFMEMORY;
    }
    
    hr = sourceToCopy->CopyPixels(nullptr, stride, static_cast<UINT>(byteCount), pData->pixels.data());
    if (FAILED(hr)) {
        pData->pixels.clear();
        return hr;
    }
    
    pData->width = static_cast<int>(w);
    pData->height = static_cast<int>(h);
    pData->stride = static_cast<int>(stride);
    pData->isValid = true;
    pData->loaderName = L"Shell Cache";
    
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(filePath, GetFileExInfoStandard, &fad)) {
        pData->fileSize = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    }
    pData->origWidth = pData->width;
    pData->origHeight = pData->height;

    return S_OK;
}

HRESULT CImageLoader::LoadThumbJPEG(LPCWSTR filePath, int targetSize, ThumbData* pData) {
    if (!pData) return E_INVALIDARG;

    std::vector<uint8_t> jpegBuf;
    if (!ReadFileToVector(filePath, jpegBuf)) return E_FAIL;
    
    return LoadThumbJPEGFromMemory(jpegBuf.data(), jpegBuf.size(), targetSize, pData);
}

static void PopulateThumbOriginalInfo(const CImageLoader::ImageHeaderInfo& headerInfo,
                                      uint64_t fallbackFileSize,
                                      CImageLoader::ThumbData* pData) {
    if (!pData) return;

    if (headerInfo.width > 0 && headerInfo.height > 0) {
        pData->origWidth = headerInfo.width;
        pData->origHeight = headerInfo.height;
    } else if (pData->origWidth <= 0 || pData->origHeight <= 0) {
        pData->origWidth = pData->width;
        pData->origHeight = pData->height;
    }

    if (headerInfo.fileSize > 0) {
        pData->fileSize = static_cast<uint64_t>(headerInfo.fileSize);
    } else if (pData->fileSize == 0) {
        pData->fileSize = fallbackFileSize;
    }
}

static HRESULT DownscaleThumbDataIfNeeded(CImageLoader::ThumbData* pData, int targetSize) {
    if (!pData || !pData->isValid || targetSize <= 0) return S_OK;
    if (pData->width <= targetSize && pData->height <= targetSize) return S_OK;

    float scaleW = static_cast<float>(targetSize) / static_cast<float>(pData->width);
    float scaleH = static_cast<float>(targetSize) / static_cast<float>(pData->height);
    float scale = (std::min)(scaleW, scaleH);
    if (scale >= 1.0f) return S_OK;

    int finalW = (std::max)(1, static_cast<int>(pData->width * scale + 0.5f));
    int finalH = (std::max)(1, static_cast<int>(pData->height * scale + 0.5f));
    int finalStride = CalculateAlignedStride(finalW, 4);
    std::vector<uint8_t> resized(static_cast<size_t>(finalStride) * finalH);

    SIMDUtils::ResizeBilinear(
        pData->pixels.data(),
        pData->width,
        pData->height,
        pData->stride,
        resized.data(),
        finalW,
        finalH,
        finalStride);

    pData->pixels = std::move(resized);
    pData->width = finalW;
    pData->height = finalH;
    pData->stride = finalStride;
    pData->isBlurry = true;
    return S_OK;
}

static HRESULT CopyWicBitmapToThumbData(IWICBitmapSource* source, CImageLoader::ThumbData* pData) {
    if (!source || !pData) return E_INVALIDARG;

    UINT width = 0;
    UINT height = 0;
    HRESULT hr = source->GetSize(&width, &height);
    if (FAILED(hr) || width == 0 || height == 0) return FAILED(hr) ? hr : E_FAIL;

    pData->width = static_cast<int>(width);
    pData->height = static_cast<int>(height);
    pData->stride = static_cast<int>(width) * 4;
    pData->pixels.resize(static_cast<size_t>(pData->stride) * height);

    hr = source->CopyPixels(nullptr,
                            static_cast<UINT>(pData->stride),
                            static_cast<UINT>(pData->pixels.size()),
                            pData->pixels.data());
    if (FAILED(hr)) {
        pData->pixels.clear();
        return hr;
    }

    pData->isValid = true;
    return S_OK;
}

struct JxlThumbSampleCtx {
    CImageLoader::ThumbData* thumb = nullptr;
    std::vector<int>* sampleX = nullptr;
    std::unordered_map<int, std::vector<int>>* sampleRows = nullptr;
    std::atomic<size_t>* touchedPixels = nullptr;
};

static void JxlThumbSampleCallback(void* opaque, size_t x, size_t y, size_t num_pixels, const void* pixels) {
    auto* ctx = static_cast<JxlThumbSampleCtx*>(opaque);
    if (!ctx || !ctx->thumb || !ctx->sampleX || !ctx->sampleRows) return;

    auto rowIt = ctx->sampleRows->find(static_cast<int>(y));
    if (rowIt == ctx->sampleRows->end()) return;

    const uint8_t* src = static_cast<const uint8_t*>(pixels);
    for (int dstY : rowIt->second) {
        uint8_t* dstRow = ctx->thumb->pixels.data() + (size_t)dstY * ctx->thumb->stride;
        for (int dstX = 0; dstX < ctx->thumb->width; ++dstX) {
            int srcX = (*ctx->sampleX)[dstX];
            if (srcX < (int)x || srcX >= (int)(x + num_pixels)) continue;

            const uint8_t* s = src + (size_t)(srcX - (int)x) * 4;
            uint8_t r = s[0];
            uint8_t g = s[1];
            uint8_t b = s[2];
            uint8_t a = s[3];
            if (a < 255) {
                r = (uint8_t)((r * a) / 255);
                g = (uint8_t)((g * a) / 255);
                b = (uint8_t)((b * a) / 255);
            }
            uint8_t* d = dstRow + (size_t)dstX * 4;
            d[0] = b;
            d[1] = g;
            d[2] = r;
            d[3] = a;
            if (ctx->touchedPixels) {
                ctx->touchedPixels->fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

static HRESULT LoadThumbJXL_Sampled(const uint8_t* pFile,
                                    size_t fileSize,
                                    int targetSize,
                                    CImageLoader::ThumbData* pData,
                                    CImageLoader::ImageMetadata* pMetadata) {
    if (!pFile || fileSize == 0 || !pData) return E_INVALIDARG;

    JxlDecoder* dec = JxlDecoderCreate(NULL);
    if (!dec) return E_OUTOFMEMORY;

    auto cleanup = [&](HRESULT hr) {
        JxlDecoderDestroy(dec);
        return hr;
    };

    void* runner = CImageLoader::GetJxlRunner();
    if (!runner) return cleanup(E_OUTOFMEMORY);
    if (JXL_DEC_SUCCESS != JxlDecoderSetParallelRunner(dec, JxlThreadParallelRunner, runner)) {
        return cleanup(E_FAIL);
    }

    const int events = JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE | JXL_DEC_COLOR_ENCODING;
    if (JXL_DEC_SUCCESS != JxlDecoderSubscribeEvents(dec, events)) {
        return cleanup(E_FAIL);
    }

    if (JXL_DEC_SUCCESS != JxlDecoderSetInput(dec, pFile, fileSize)) {
        return cleanup(E_FAIL);
    }
    JxlDecoderCloseInput(dec);

    JxlBasicInfo info = {};
    JxlPixelFormat format = { 4, JXL_TYPE_UINT8, JXL_LITTLE_ENDIAN, 0 };
    std::vector<int> sampleX;
    std::unordered_map<int, std::vector<int>> sampleRows;
    JxlThumbSampleCtx sampleCtx = {};
    std::atomic<size_t> touchedPixels{ 0 };
    bool callbackSet = false;

    for (;;) {
        JxlDecoderStatus status = JxlDecoderProcessInput(dec);
        if (status == JXL_DEC_ERROR) return cleanup(E_FAIL);
        if (status == JXL_DEC_SUCCESS) break;

        if (status == JXL_DEC_BASIC_INFO) {
            if (JXL_DEC_SUCCESS != JxlDecoderGetBasicInfo(dec, &info)) {
                return cleanup(E_FAIL);
            }

            JxlColorEncoding colorEncoding = {};
            colorEncoding.color_space = JXL_COLOR_SPACE_RGB;
            colorEncoding.white_point = JXL_WHITE_POINT_D65;
            colorEncoding.primaries = JXL_PRIMARIES_SRGB;
            colorEncoding.transfer_function = JXL_TRANSFER_FUNCTION_SRGB;
            colorEncoding.rendering_intent = JXL_RENDERING_INTENT_PERCEPTUAL;
            JxlDecoderSetOutputColorProfile(dec, &colorEncoding, NULL, 0);

            int origW = static_cast<int>(info.xsize);
            int origH = static_cast<int>(info.ysize);
            int dstW = origW;
            int dstH = origH;
            if (targetSize > 0 && (origW > targetSize || origH > targetSize)) {
                float ratio = (origW >= origH) ? ((float)targetSize / (float)origW)
                                               : ((float)targetSize / (float)origH);
                dstW = (std::max)(1, (int)std::lround(origW * ratio));
                dstH = (std::max)(1, (int)std::lround(origH * ratio));
            }

            pData->width = dstW;
            pData->height = dstH;
            pData->stride = CalculateAlignedStride(dstW, 4);
            pData->origWidth = origW;
            pData->origHeight = origH;
            pData->pixels.assign((size_t)pData->stride * dstH, 0);
            pData->isValid = false;
            pData->isBlurry = (dstW != origW || dstH != origH);
            pData->loaderName = L"libjxl (Sampled)";

            sampleX.resize(dstW);
            for (int dx = 0; dx < dstW; ++dx) {
                sampleX[dx] = (std::min)(origW - 1, (int)(((double)dx + 0.5) * origW / dstW));
            }
            sampleRows.clear();
            for (int dy = 0; dy < dstH; ++dy) {
                int sy = (std::min)(origH - 1, (int)(((double)dy + 0.5) * origH / dstH));
                sampleRows[sy].push_back(dy);
            }

            if (pMetadata) {
                pMetadata->Width = origW;
                pMetadata->Height = origH;
            }

            sampleCtx.thumb = pData;
            sampleCtx.sampleX = &sampleX;
            sampleCtx.sampleRows = &sampleRows;
            sampleCtx.touchedPixels = &touchedPixels;
            if (JXL_DEC_SUCCESS != JxlDecoderSetImageOutCallback(dec, &format, JxlThumbSampleCallback, &sampleCtx)) {
                return cleanup(E_FAIL);
            }
            callbackSet = true;
        } else if (status == JXL_DEC_COLOR_ENCODING) {
            if (pMetadata) {
                size_t iccSize = 0;
                if (JXL_DEC_SUCCESS == JxlDecoderGetICCProfileSize(dec, JXL_COLOR_PROFILE_TARGET_DATA, &iccSize) && iccSize > 0) {
                    std::vector<uint8_t> icc(iccSize);
                    if (JXL_DEC_SUCCESS == JxlDecoderGetColorAsICCProfile(dec, JXL_COLOR_PROFILE_TARGET_DATA, icc.data(), iccSize)) {
                        std::wstring desc = CImageLoader::ParseICCProfileName(icc.data(), iccSize);
                        if (!desc.empty()) pMetadata->ColorSpace = desc;
                    }
                }
            }
        } else if (status == JXL_DEC_FULL_IMAGE) {
            break;
        }
    }

    if (!callbackSet || touchedPixels.load(std::memory_order_relaxed) == 0 || pData->pixels.empty()) {
        pData->isValid = false;
        pData->pixels.clear();
        return cleanup(E_FAIL);
    }
    pData->isValid = true;
    return cleanup(S_OK);
}

static HRESULT RasterizeSvgThumbnail(const std::vector<uint8_t>& xmlData,
                                     float viewBoxW,
                                     float viewBoxH,
                                     int targetSize,
                                     CImageLoader::ThumbData* pData) {
    if (!pData || xmlData.empty()) return E_INVALIDARG;

    const float safeW = viewBoxW > 0.0f ? viewBoxW : 512.0f;
    const float safeH = viewBoxH > 0.0f ? viewBoxH : 512.0f;
    const int maxDim = targetSize > 0 ? targetSize : 512;
    const float scale = (std::min)(1.0f, (float)maxDim / (std::max)(safeW, safeH));
    const UINT outW = (UINT)(std::max)(1, (int)std::lround(safeW * scale));
    const UINT outH = (UINT)(std::max)(1, (int)std::lround(safeH * scale));

    UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    ComPtr<ID3D11Device> d3dDevice;
    ComPtr<ID3D11DeviceContext> d3dContext;
    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_WARP,
        nullptr,
        creationFlags,
        &fl,
        1,
        D3D11_SDK_VERSION,
        &d3dDevice,
        nullptr,
        &d3dContext);
    if (FAILED(hr)) return hr;

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = outW;
    texDesc.Height = outH;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    ComPtr<ID3D11Texture2D> renderTex;
    hr = d3dDevice->CreateTexture2D(&texDesc, nullptr, &renderTex);
    if (FAILED(hr)) return hr;

    ComPtr<IDXGISurface> dxgiSurface;
    hr = renderTex.As(&dxgiSurface);
    if (FAILED(hr)) return hr;

    D2D1_FACTORY_OPTIONS options = {};
    ComPtr<ID2D1Factory1> d2dFactory;
    hr = D2D1CreateFactory(
        D2D1_FACTORY_TYPE_SINGLE_THREADED,
        __uuidof(ID2D1Factory1),
        &options,
        reinterpret_cast<void**>(d2dFactory.GetAddressOf()));
    if (FAILED(hr)) return hr;

    ComPtr<IDXGIDevice> dxgiDevice;
    hr = d3dDevice.As(&dxgiDevice);
    if (FAILED(hr)) return hr;

    ComPtr<ID2D1Device> d2dDevice;
    hr = d2dFactory->CreateDevice(dxgiDevice.Get(), &d2dDevice);
    if (FAILED(hr)) return hr;

    ComPtr<ID2D1DeviceContext> d2dContext;
    hr = d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dContext);
    if (FAILED(hr)) return hr;

    ComPtr<ID2D1DeviceContext5> d2dContext5;
    hr = d2dContext.As(&d2dContext5);
    if (FAILED(hr)) return hr;

    D2D1_BITMAP_PROPERTIES1 targetProps = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f,
        96.0f);

    ComPtr<ID2D1Bitmap1> targetBitmap;
    hr = d2dContext5->CreateBitmapFromDxgiSurface(dxgiSurface.Get(), &targetProps, &targetBitmap);
    if (FAILED(hr)) return hr;

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, xmlData.size());
    if (!hMem) return E_OUTOFMEMORY;

    ComPtr<IStream> stream;
    void* mem = GlobalLock(hMem);
    if (!mem) {
        GlobalFree(hMem);
        return E_OUTOFMEMORY;
    }
    memcpy(mem, xmlData.data(), xmlData.size());
    GlobalUnlock(hMem);
    hr = CreateStreamOnHGlobal(hMem, TRUE, &stream);
    if (FAILED(hr)) {
        GlobalFree(hMem);
        return hr;
    }

    ComPtr<ID2D1SvgDocument> svgDoc;
    hr = d2dContext5->CreateSvgDocument(stream.Get(), D2D1::SizeF(safeW, safeH), &svgDoc);
    if (FAILED(hr)) return hr;

    d2dContext5->SetTarget(targetBitmap.Get());
    d2dContext5->BeginDraw();
    d2dContext5->Clear(D2D1::ColorF(0, 0, 0, 0));
    d2dContext5->SetTransform(D2D1::Matrix3x2F::Scale((float)outW / safeW, (float)outH / safeH));
    d2dContext5->DrawSvgDocument(svgDoc.Get());
    hr = d2dContext5->EndDraw();
    if (FAILED(hr)) return hr;

    D3D11_TEXTURE2D_DESC stagingDesc = texDesc;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> stagingTex;
    hr = d3dDevice->CreateTexture2D(&stagingDesc, nullptr, &stagingTex);
    if (FAILED(hr)) return hr;

    d3dContext->CopyResource(stagingTex.Get(), renderTex.Get());

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = d3dContext->Map(stagingTex.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return hr;

    pData->width = (int)outW;
    pData->height = (int)outH;
    pData->stride = CalculateAlignedStride((int)outW, 4);
    pData->pixels.resize((size_t)pData->stride * outH);

    for (UINT y = 0; y < outH; ++y) {
        memcpy(pData->pixels.data() + (size_t)y * pData->stride,
               static_cast<const uint8_t*>(mapped.pData) + (size_t)y * mapped.RowPitch,
               outW * 4);
    }

    d3dContext->Unmap(stagingTex.Get(), 0);
    pData->isValid = true;
    pData->isBlurry = false;
    return S_OK;
}

HRESULT CImageLoader::LoadThumbnail(LPCWSTR filePath, int targetSize, ThumbData* pData, bool allowSlow) {
    if (!filePath || !pData) return E_INVALIDARG;
    pData->isValid = false;
    pData->pixels.clear();
    
    // 0. Highest Priority: Windows Shell Thumbnail Cache (Insanely fast for pre-cached heavy RAWs)
    if (SUCCEEDED(LoadShellThumbnail(filePath, targetSize, pData))) {
        return S_OK;
    }

    const ImageHeaderInfo headerInfo = PeekHeader(filePath);
    const uint64_t fallbackFileSize = static_cast<uint64_t>(headerInfo.fileSize);
    std::wstring format = headerInfo.format.empty() ? DetectFormatFromContent(filePath) : headerInfo.format;
    std::wstring pathLower = filePath;
    std::transform(pathLower.begin(), pathLower.end(), pathLower.begin(), ::towlower);

    QuickView::MappedFile mapping(filePath);
    const uint8_t* mappedData = mapping.IsValid() ? mapping.data() : nullptr;
    size_t mappedSize = mapping.IsValid() ? mapping.size() : 0;

    auto tryEmbeddedPreview = [&](const wchar_t* loaderName,
                                  auto extractor) -> bool {
        if (!mappedData || mappedSize == 0) return false;
        PreviewExtractor::ExtractedData exData;
        if (!extractor(mappedData, mappedSize, exData) || !exData.IsValid()) {
            return false;
        }
        if (FAILED(LoadThumbJPEGFromMemory(exData.pData, exData.size, targetSize, pData)) || !pData->isValid) {
            return false;
        }
        pData->loaderName = loaderName;
        pData->isBlurry = false;
        PopulateThumbOriginalInfo(headerInfo, static_cast<uint64_t>(mappedSize), pData);
        return true;
    };

    if (format == L"RAW" && tryEmbeddedPreview(L"RAW Preview", PreviewExtractor::ExtractFromRAW)) {
        return S_OK;
    }
    if (format == L"TIFF" && tryEmbeddedPreview(L"TIFF Preview", PreviewExtractor::ExtractFromTIFF)) {
        return S_OK;
    }
    if ((format == L"HEIC" || pathLower.ends_with(L".heic") || pathLower.ends_with(L".heif")) &&
        tryEmbeddedPreview(L"HEIC Preview", PreviewExtractor::ExtractFromHEIC)) {
        return S_OK;
    }
    if ((format == L"PSD" || pathLower.ends_with(L".psb")) &&
        tryEmbeddedPreview(L"PSD Preview", PreviewExtractor::ExtractFromPSD)) {
        return S_OK;
    }

    if ((format == L"AVIF" || format == L"HEIC" ||
         pathLower.ends_with(L".avif") || pathLower.ends_with(L".avifs") ||
         pathLower.ends_with(L".heic") || pathLower.ends_with(L".heif")) &&
        mappedData && mappedSize > 0) {
        ImageMetadata meta;
        HRESULT hr = LoadThumbAVIF_Proxy(mappedData, mappedSize, targetSize, pData, allowSlow, &meta);
        if (SUCCEEDED(hr) && pData->isValid) {
            DownscaleThumbDataIfNeeded(pData, targetSize);
            if (meta.Width > 0 && meta.Height > 0) {
                pData->origWidth = meta.Width;
                pData->origHeight = meta.Height;
            }
            pData->fileSize = meta.FileSize > 0 ? meta.FileSize : static_cast<uint64_t>(mappedSize);
            pData->loaderName = format == L"HEIC" ? L"libavif/HEIF" : L"libavif";
            return S_OK;
        }
    }

    if ((format == L"JXL" || pathLower.ends_with(L".jxl")) && mappedData && mappedSize > 0) {
        ImageMetadata meta;
        // Gallery thumbnails must never use Titan's transparent fake base.
        // If a cheap real thumbnail is unavailable, we fall through to the
        // normal decode path instead of returning an empty/transparent result.
        HRESULT hr = LoadThumbJXL_DC(mappedData, mappedSize, pData, &meta, false, false);
        if (SUCCEEDED(hr) && pData->isValid) {
            const bool isFakeThumb =
                pData->width <= 1 &&
                pData->height <= 1 &&
                pData->pixels.size() == 4 &&
                std::all_of(pData->pixels.begin(), pData->pixels.end(), [](uint8_t v) { return v == 0; });
            if (isFakeThumb || pData->loaderName.contains(L"Fake Base")) {
                pData->isValid = false;
                pData->pixels.clear();
                hr = E_FAIL;
            }
        }
        if (SUCCEEDED(hr) && pData->isValid) {
            DownscaleThumbDataIfNeeded(pData, targetSize);
            if (meta.Width > 0 && meta.Height > 0) {
                pData->origWidth = meta.Width;
                pData->origHeight = meta.Height;
            }
            PopulateThumbOriginalInfo(headerInfo, static_cast<uint64_t>(mappedSize), pData);
            return S_OK;
        }
        if (allowSlow) {
            pData->isValid = false;
            pData->pixels.clear();
            hr = LoadThumbJXL_Sampled(mappedData, mappedSize, targetSize, pData, &meta);
            if (SUCCEEDED(hr) && pData->isValid) {
                PopulateThumbOriginalInfo(headerInfo, static_cast<uint64_t>(mappedSize), pData);
                return S_OK;
            }
        }
        return E_FAIL;
    }

    if (format == L"SVG") {
        using namespace QuickView;
        RawImageFrame* pFrame = new RawImageFrame();
        HRESULT hr = LoadToFrame(filePath, pFrame, nullptr, targetSize, targetSize, &pData->loaderName, nullptr, nullptr);
        if (SUCCEEDED(hr) && pFrame->IsSvg() && pFrame->svg) {
            hr = RasterizeSvgThumbnail(pFrame->svg->xmlData, pFrame->svg->viewBoxW, pFrame->svg->viewBoxH, targetSize, pData);
        } else if (SUCCEEDED(hr) && pFrame->pixels && pFrame->width > 0 && pFrame->height > 0) {
            pData->width = pFrame->width;
            pData->height = pFrame->height;
            pData->stride = pFrame->stride;
            pData->pixels.resize(static_cast<size_t>(pFrame->stride) * pFrame->height);
            memcpy(pData->pixels.data(), pFrame->pixels, pData->pixels.size());
            pData->isValid = true;
            pData->isBlurry = false;
        }
        if (SUCCEEDED(hr) && pData->isValid) {
            PopulateThumbOriginalInfo(headerInfo, fallbackFileSize, pData);
            pFrame->Release();
            delete pFrame;
            return S_OK;
        }
        if (pFrame) {
            pFrame->Release();
            delete pFrame;
        }
    }

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
        
        // [BugFix] Populate original dimensions and file size for Gallery tooltip
        // Use metadata if available, otherwise use decoded dimensions (for scaled previews)
        pData->origWidth = (res.metadata.Width > 0) ? res.metadata.Width : res.width;
        pData->origHeight = (res.metadata.Height > 0) ? res.metadata.Height : res.height;
        pData->fileSize = res.metadata.FileSize;
        
        // If FileSize not populated by codec, get it from file system
        if (pData->fileSize == 0) {
            WIN32_FILE_ATTRIBUTE_DATA fad;
            if (GetFileAttributesExW(filePath, GetFileExInfoStandard, &fad)) {
                pData->fileSize = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
            }
        }
        DownscaleThumbDataIfNeeded(pData, targetSize);
        PopulateThumbOriginalInfo(headerInfo, fallbackFileSize, pData);
        return S_OK;
    }
    
    if (hr == E_ABORT) return E_ABORT;

    if (!allowSlow) return E_FAIL; // Scout gives up

    // 2. Full compatibility fallback through the main loader chain.
    ComPtr<IWICBitmap> wicBitmap;
    std::wstring wicLoader;
    HRESULT wicHr = LoadToMemory(filePath, &wicBitmap, &wicLoader, false, nullptr, targetSize, targetSize);
    if (SUCCEEDED(wicHr) && wicBitmap) {
        HRESULT copyHr = CopyWicBitmapToThumbData(wicBitmap.Get(), pData);
        if (SUCCEEDED(copyHr)) {
            pData->loaderName = wicLoader.empty() ? L"WIC Thumbnail" : wicLoader;
            PopulateThumbOriginalInfo(headerInfo, fallbackFileSize, pData);
            pData->isBlurry = (pData->origWidth > pData->width || pData->origHeight > pData->height);
            return S_OK;
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
// [v6.0] Helper for EasyExif population
static void PopulateMetadataFromEasyExif(const easyexif::EXIFInfo& exif, CImageLoader::ImageMetadata& meta) {
     auto toW = [](const std::string& s) -> std::wstring {
         if (s.empty()) return L"";
         int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, NULL, 0);
         if (len <= 0) return L"";
         std::wstring w(len - 1, 0);
         MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], len);
         return w;
     };
     
     if (!exif.Make.empty()) meta.Make = toW(exif.Make);
     if (!exif.Model.empty()) meta.Model = toW(exif.Model);
     if (!exif.DateTimeOriginal.empty()) meta.Date = toW(exif.DateTimeOriginal);
     else if (!exif.DateTime.empty()) meta.Date = toW(exif.DateTime);
     
     if (exif.ISOSpeedRatings > 0) meta.ISO = std::to_wstring(exif.ISOSpeedRatings);
     
     if (exif.ExposureTime > 0.0) {
         wchar_t buf[32];
         if (exif.ExposureTime >= 1.0) swprintf_s(buf, L"%.1fs", exif.ExposureTime);
         else swprintf_s(buf, L"1/%.0fs", 1.0/exif.ExposureTime);
         meta.Shutter = buf;
     }
     
     if (exif.FNumber > 0.0) {
         wchar_t buf[32]; swprintf_s(buf, L"f/%.1f", exif.FNumber);
         meta.Aperture = buf;
     }
     
     if (exif.FocalLength > 0.0) {
         wchar_t buf[32]; swprintf_s(buf, L"%.0fmm", exif.FocalLength);
         meta.Focal = buf;
     }
     
     if (exif.Flash & 1) meta.Flash = L"Flash: On";
     else if (exif.Flash == 0) meta.Flash = L"Flash: Off";
     
     if (!exif.Software.empty()) meta.Software = toW(exif.Software);
     if (!exif.LensInfo.Model.empty()) meta.Lens = toW(exif.LensInfo.Model);

     // [v6.0] Color Space
     if (exif.ColorSpace == 1) meta.ColorSpace = L"sRGB";
     else if (exif.ColorSpace == 2) meta.ColorSpace = L"Adobe RGB";
     else if (exif.ColorSpace == 65535) meta.ColorSpace = L"Uncalibrated";

     // [Fix] Populate Orientation! (Crucial for TurboJPEG/Buffer Loaders)
     if (exif.Orientation >= 1 && exif.Orientation <= 8) {
         meta.ExifOrientation = exif.Orientation;
     }
}

    namespace Codec {
        namespace WebP {
            // Needs: #include <webp/demux.h> if not already included via header chain? 
            // Actually QuickView uses libwebp/src/webp/demux.h usually or similar.
            // Let's assume headers are available via pch or similar.
            // If not, we might need to include it.
            
            static HRESULT Load(const uint8_t* data, size_t size, const DecodeContext& ctx, DecodeResult& result) {
                if (!data || size == 0) return E_FAIL;

                // [v6.0] WebP Metadata Extraction using WebPDemux (Exif + ICC)
                {
                    WebPData webpData = { data, size };
                    WebPDemuxer* demux = WebPDemux(&webpData);
                    if (demux) {
                        // 1. EXIF
                        WebPChunkIterator chunk;
                        if (WebPDemuxGetChunk(demux, "EXIF", 1, &chunk)) {
                             easyexif::EXIFInfo exif;
                             if (exif.parseFromEXIFSegment(chunk.chunk.bytes, static_cast<unsigned>(chunk.chunk.size)) != 0) {
                                 std::vector<unsigned char> tempBuf(chunk.chunk.size + 6);
                                 memcpy(tempBuf.data(), "Exif\0\0", 6);
                                 memcpy(tempBuf.data() + 6, chunk.chunk.bytes, chunk.chunk.size);
                                 if (exif.parseFromEXIFSegment(tempBuf.data(), static_cast<unsigned>(tempBuf.size())) == 0) {
                                     PopulateMetadataFromEasyExif(exif, result.metadata);
                                 }
                             } else {
                                 PopulateMetadataFromEasyExif(exif, result.metadata);
                             }
                             WebPDemuxReleaseChunkIterator(&chunk);
                        }
                        
                        // 2. ICCP (CMS)
                        if (WebPDemuxGetChunk(demux, "ICCP", 1, &chunk)) {
                            result.metadata.iccProfileData.assign(chunk.chunk.bytes, chunk.chunk.bytes + chunk.chunk.size);
                            result.metadata.HasEmbeddedColorProfile = true;
                            WebPDemuxReleaseChunkIterator(&chunk);
                        }
                        
                        WebPDemuxDelete(demux);
                    }
                }

                WebPDecoderConfig config;
                if (!WebPInitDecoderConfig(&config)) return E_FAIL;
                
                // RAII
                struct ConfigGuard { WebPDecBuffer* b; ~ConfigGuard() { WebPFreeDecBuffer(b); } } cGuard{ &config.output };

                if (WebPGetFeatures(data, size, &config.input) != VP8_STATUS_OK) return E_FAIL;
                
                // [v6.0] Format Details
                if (config.input.format == 2) result.metadata.FormatDetails = L"Lossless";
                else if (config.input.format == 1) result.metadata.FormatDetails = L"Lossy YUV";
                
                // [v7.0] Append Color Mode details
                // VP8 (Lossy) is YUV 4:2:0. VP8L (Lossless) is ARGB.
                // User Request: Show YUV.
                // Already added "YUV" above for lossy.
                // For Lossless, we can add ARGB?
                if (config.input.format == 2) result.metadata.FormatDetails += L" ARGB";
                // else result.metadata.FormatDetails += L""; 

                // [CR Logic moved to specific decoder paths to ensure correct frame count/size]
                
                if (config.input.has_animation) result.metadata.FormatDetails += L" Anim";
                if (config.input.has_alpha) result.metadata.FormatDetails += L" Alpha";

                // 1. Animated WebP
                if (config.input.has_animation) {
                    WebPData webpData = { data, size };
                    WebPAnimDecoderOptions decOpts;
                    WebPAnimDecoderOptionsInit(&decOpts);
                    decOpts.color_mode = MODE_BGRA;
                    decOpts.use_threads = 1; // [v7.1] Re-enabled per user request

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

                                // [v7.2] Animated WebP Info
                                result.metadata.FormatDetails = L"Anim";
                                bool isLossless = (config.input.format == 2);
                                
                                // Anim usually mixes frames, but base format is indicative
                                if (isLossless) result.metadata.FormatDetails += L" Lossless ARGB";
                                else result.metadata.FormatDetails += L" Lossy YUV";

                                if (config.input.has_alpha && !isLossless) result.metadata.FormatDetails += L" +Alpha";
                                
                                // [CR Removed per user request]
                                
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
                     
                     // 修复 Bug 1: 必须保持长宽比缩放
                     int tw = ctx.targetWidth > 0 ? ctx.targetWidth : config.input.width;
                     int th = ctx.targetHeight > 0 ? ctx.targetHeight : config.input.height;
                     
                     float scaleX = (float)tw / config.input.width;
                     float scaleY = (float)th / config.input.height;
                     float scale = (std::min)(scaleX, scaleY);
                     
                     if (scale > 1.0f) scale = 1.0f; // Only downscale
                     
                     config.options.scaled_width = (std::max)(1, (int)(config.input.width * scale));
                     config.options.scaled_height = (std::max)(1, (int)(config.input.height * scale));
                     
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

                // [v7.2] Refined WebP Info (Short & Accurate)
                // Static WebP Path
                bool isLossless = (config.input.format == 2);
                result.metadata.FormatDetails = isLossless ? L"Lossless" : L"Lossy";
                
                if (config.options.use_scaling) result.metadata.FormatDetails += L" [Scaled]";
                
                // Append Color Mode
                if (isLossless) result.metadata.FormatDetails += L" ARGB"; 
                else result.metadata.FormatDetails += L" YUV";
                
                if (config.input.has_alpha && !isLossless) result.metadata.FormatDetails += L" +Alpha";
                
                // [CR Removed per user request]
                
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
HRESULT CImageLoader::LoadWebP(LPCWSTR filePath, IWICBitmap** ppBitmap, ImageMetadata* pMetadata) {
    std::pmr::vector<uint8_t> webpBuf(std::pmr::get_default_resource());
    if (!ReadFileToPMR(filePath, webpBuf, {})) return E_FAIL;

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
            // [CMS] Use WebPDemux to extract ICC Profile natively
            if (pMetadata) {
                WebPData webp_data = { webpBuf.data(), webpBuf.size() };
                WebPDemuxer* demux = WebPDemux(&webp_data);
                if (demux) {
                    WebPChunkIterator chunk_iter;
                    if (WebPDemuxGetChunk(demux, "ICCP", 1, &chunk_iter)) {
                        pMetadata->iccProfileData.assign(chunk_iter.chunk.bytes, chunk_iter.chunk.bytes + chunk_iter.chunk.size);
                        WebPDemuxReleaseChunkIterator(&chunk_iter);
                    }
                    WebPDemuxDelete(demux);
                }
            }
        }
        return hr;
    }
    
    return hr;
}

// ----------------------------------------------------------------------------
// AVIF (libavif + dav1d)
// ----------------------------------------------------------------------------
HRESULT CImageLoader::LoadAVIF(LPCWSTR filePath, IWICBitmap** ppBitmap, ImageMetadata* pMetadata) {
    if (!filePath || !ppBitmap) return E_INVALIDARG;

    // Read file to memory buffer
    std::pmr::vector<uint8_t> avifBuf(std::pmr::get_default_resource());
    if (!ReadFileToPMR(filePath, avifBuf, {})) return E_FAIL;

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
    avifResult result = avifDecoderSetIOMemory(decoder, avifBuf.data(), avifBuf.size());
    if (result != AVIF_RESULT_OK) {
        wchar_t buf[128]; swprintf_s(buf, L"[LoadAVIF] SetIOMemory failed: %hs\n", avifResultToString(result));
        OutputDebugStringW(buf);
        avifDecoderDestroy(decoder);
        return E_FAIL;
    }

    // Parse
    result = avifDecoderParse(decoder);
    if (result != AVIF_RESULT_OK) {
        wchar_t buf[256]; 
        swprintf_s(buf, L"[LoadAVIF] Parse failed: %hs | Diag: %hs\n", avifResultToString(result), decoder->diag.error);
        OutputDebugStringW(buf);
        avifDecoderDestroy(decoder);
        return E_FAIL;
    }

    // Next Image (Frame 0)
    result = avifDecoderNextImage(decoder);
    if (result != AVIF_RESULT_OK) {
        wchar_t buf[128]; swprintf_s(buf, L"[LoadAVIF] NextImage failed: %hs\n", avifResultToString(result));
        OutputDebugStringW(buf);
        avifDecoderDestroy(decoder);
        return E_FAIL;
    }

    // Convert YUV to RGB directly into WIC Bitmap
    
    // 1. Create target WIC Bitmap
    ComPtr<IWICBitmap> pWicBitmap;
    HRESULT hr = m_wicFactory->CreateBitmap(decoder->image->width, decoder->image->height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand, &pWicBitmap);
    if (FAILED(hr)) {
        avifDecoderDestroy(decoder);
        return hr;
    }

    // 2. Lock buffer
    ComPtr<IWICBitmapLock> pLock;
    WICRect rc = { 0, 0, (INT)decoder->image->width, (INT)decoder->image->height };
    hr = pWicBitmap->Lock(&rc, WICBitmapLockWrite, &pLock);
    if (FAILED(hr)) { avifDecoderDestroy(decoder); return hr; }

    UINT bufSize = 0;
    BYTE* pBuf = nullptr;
    UINT stride = 0;
    pLock->GetDataPointer(&bufSize, &pBuf);
    pLock->GetStride(&stride);

    // 3. Decode to Buffer
    avifRGBImage rgb;
    avifRGBImageSetDefaults(&rgb, decoder->image);
    rgb.format = AVIF_RGB_FORMAT_BGRA;
    rgb.depth = 8;
    rgb.alphaPremultiplied = AVIF_TRUE; 
    rgb.pixels = pBuf;
    rgb.rowBytes = stride;
    
    // Validate buffer size safe-guard
    if (bufSize < (size_t)stride * rgb.height) {
        pLock.Reset(); avifDecoderDestroy(decoder); return E_OUTOFMEMORY;
    }
    
    result = avifImageYUVToRGB(decoder->image, &rgb);
    pLock.Reset(); // Unlock

    if (result != AVIF_RESULT_OK) {
        wchar_t buf[128]; swprintf_s(buf, L"[LoadAVIF] YUVToRGB failed: %hs\n", avifResultToString(result));
        OutputDebugStringW(buf);
        avifDecoderDestroy(decoder);
        return E_FAIL;
    }

    *ppBitmap = pWicBitmap.Detach();

    // Extract format details (bit depth)
    if (SUCCEEDED(hr)) {
        int depth = decoder->image->depth;
        wchar_t buf[32];
        swprintf_s(buf, L"%d-bit", depth);
        g_lastFormatDetails = buf;
        if (decoder->image->alphaPlane != nullptr) g_lastFormatDetails += L" +Alpha";

        // [CMS] Extract ICC profile
        if (pMetadata && decoder->image->icc.data && decoder->image->icc.size > 0) {
            pMetadata->iccProfileData.assign(decoder->image->icc.data, decoder->image->icc.data + decoder->image->icc.size);
        }
        if (pMetadata) {
            PopulateAvifHdrStaticMetadata(decoder->image, &pMetadata->hdrMetadata);
            if (decoder->image->gainMap) {
                g_lastFormatDetails += L" [GainMap]";
            }
            if (decoder->image->clli.maxCLL > 0 || decoder->image->clli.maxPALL > 0) {
                g_lastFormatDetails += L" [CLLI]";
            }
        }
    }

    avifDecoderDestroy(decoder);
    return hr;
}

// ----------------------------------------------------------------------------
// JPEG XL (libjxl)
// ----------------------------------------------------------------------------
HRESULT CImageLoader::LoadJXL(LPCWSTR filePath, IWICBitmap** ppBitmap, ImageMetadata* pMetadata) {
    if (!filePath || !ppBitmap) return E_INVALIDARG;

    std::pmr::vector<uint8_t> jxlBuf(std::pmr::get_default_resource());
    if (!ReadFileToPMR(filePath, jxlBuf, {})) return E_FAIL;

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

    // 2. Subscribe to events (Drop output color profile override, extract ICC instead)
    if (JXL_DEC_SUCCESS != JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_FULL_IMAGE | JXL_DEC_FRAME)) {
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
    ComPtr<IWICBitmap> pWicBitmap = nullptr;
    ComPtr<IWICBitmapLock> pLock = nullptr;
    BYTE* pWicBuf = nullptr;
    UINT wicStride = 0;
    
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
            // Removed Force sRGB output color profile so original color gets preserved.

            // [Optimization] Create WIC Bitmap Early and Lock
            hr = m_wicFactory->CreateBitmap(info.xsize, info.ysize, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand, &pWicBitmap);
            if (SUCCEEDED(hr)) {
                WICRect rc = { 0, 0, (INT)info.xsize, (INT)info.ysize };
                hr = pWicBitmap->Lock(&rc, WICBitmapLockWrite, &pLock);
                if (SUCCEEDED(hr)) {
                    UINT bufSize = 0;
                    pLock->GetDataPointer(&bufSize, &pWicBuf);
                    pLock->GetStride(&wicStride);
                }
            }
        }
        else if (status == JXL_DEC_COLOR_ENCODING) {
            // [CMS] Extract exact original ICC Profile from JXL Stream
            if (pMetadata) {
                size_t icc_size = 0;
                if (JXL_DEC_SUCCESS == JxlDecoderGetICCProfileSize(dec, JXL_COLOR_PROFILE_TARGET_DATA, &icc_size) && icc_size > 0) {
                    pMetadata->iccProfileData.resize(icc_size);
                    JxlDecoderGetColorAsICCProfile(dec, JXL_COLOR_PROFILE_TARGET_DATA, pMetadata->iccProfileData.data(), icc_size);
                }

                JxlColorEncoding colorEncoding = {};
                if (JXL_DEC_SUCCESS == JxlDecoderGetColorAsEncodedProfile(dec, JXL_COLOR_PROFILE_TARGET_ORIGINAL, &colorEncoding)) {
                    pMetadata->hdrMetadata.isValid = true;
                    pMetadata->hdrMetadata.transfer = MapJxlTransferFunction(colorEncoding.transfer_function);
                    pMetadata->hdrMetadata.primaries = MapJxlPrimaries(colorEncoding.primaries);
                    pMetadata->hdrMetadata.isSceneLinear =
                        (pMetadata->hdrMetadata.transfer == QuickView::TransferFunction::Linear);
                    pMetadata->hdrMetadata.isHdr =
                        pMetadata->hdrMetadata.transfer == QuickView::TransferFunction::PQ ||
                        pMetadata->hdrMetadata.transfer == QuickView::TransferFunction::HLG ||
                        pMetadata->hdrMetadata.transfer == QuickView::TransferFunction::Linear;
                }
            }
        }
        else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
            size_t bufferSize = 0;
            if (JXL_DEC_SUCCESS != JxlDecoderImageOutBufferSize(dec, &format, &bufferSize)) {
                hr = E_FAIL; break;
            }
            
            // [Optimization] Use WIC buffer if stride matches
            if (pWicBuf && wicStride == info.xsize * 4) {
                 if (JXL_DEC_SUCCESS != JxlDecoderSetImageOutBuffer(dec, &format, pWicBuf, bufferSize)) {
                      hr = E_FAIL; break;
                 }
            } else {
                 if (pixels.size() < bufferSize) {
                    pixels.resize(bufferSize);
                 }
                 if (JXL_DEC_SUCCESS != JxlDecoderSetImageOutBuffer(dec, &format, pixels.data(), bufferSize)) {
                     hr = E_FAIL; break;
                 }
            }
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

    if (SUCCEEDED(hr)) {
        if (!pixels.empty()) {
             // Fallback: Intermediate buffer used
             SIMDUtils::SwizzleRGBA_to_BGRA_Premul(pixels.data(), (size_t)info.xsize * info.ysize);
             hr = CreateWICBitmapFromMemory(info.xsize, info.ysize, GUID_WICPixelFormat32bppPBGRA, info.xsize * 4, (UINT)pixels.size(), pixels.data(), ppBitmap);
        } else if (pWicBitmap) {
             // Optimization: Direct WIC buffer used
             // In-place Swizzle
             SIMDUtils::SwizzleRGBA_to_BGRA_Premul(pWicBuf, (size_t)info.xsize * info.ysize);
             pLock.Reset(); // Unlock
             *ppBitmap = pWicBitmap.Detach();
        }
    }
    
    // Clean up if something failed mid-way or if we detached
    pLock.Reset();
    pWicBitmap.Reset();

    // NOTE: Do NOT destroy global runner!
    JxlDecoderDestroy(dec);
    return hr;
}

// ----------------------------------------------------------------------------
// RAW (LibRaw)
// ----------------------------------------------------------------------------
HRESULT CImageLoader::LoadRaw(LPCWSTR filePath, IWICBitmap** ppBitmap, bool forceFullDecode, ImageMetadata* pMetadata) { 
    // Optimization: Try to load embedded JPEG preview first (FAST)
    // Fallback: Full RAW decode (SLOW)

    std::pmr::vector<uint8_t> rawBuf(std::pmr::get_default_resource());
    if (!ReadFileToPMR(filePath, rawBuf, {})) return E_FAIL;

    LibRaw RawProcessor;
    if (RawProcessor.open_buffer(rawBuf.data(), rawBuf.size()) != LIBRAW_SUCCESS) return E_FAIL;

    // [Fix] Capture RAW orientation early (same mapping as LoadImageUnified RAW Codec)
    int flip = RawProcessor.imgdata.sizes.flip;
    int exifOrientation = 1;
    if (flip == 3) exifOrientation = 3;      // 180
    else if (flip == 6) exifOrientation = 6; // 90 CW
    else if (flip == 5) exifOrientation = 8; // 90 CCW
    else if (flip == 1) exifOrientation = 2; // Flip X
    else if (flip == 2) exifOrientation = 4; // Flip Y
    else if (flip == 4) exifOrientation = 5; // Transpose
    else if (flip == 7) exifOrientation = 7; // Transverse

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
                    // [Fix] Embedded JPEG preview is pre-rotated by camera firmware.
                    // Set orientation to 1 to prevent double-rotation in compare mode.
                    g_lastExifOrientation = 1;
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
                        // [Fix] Bitmap thumbnails may NOT be pre-rotated.
                        // Preserve orientation so renderer can apply it.
                        g_lastExifOrientation = exifOrientation;
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
    // [Fix] Disable auto-rotation so bitmap stays in sensor orientation.
    // This matches LoadImageUnified RAW Codec behavior.
    // Rotation is handled by the renderer using g_lastExifOrientation.
    RawProcessor.imgdata.params.user_flip = 0;
    
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
    
    if (SUCCEEDED(hr)) {
        // [Fix] Bitmap is un-rotated (user_flip=0). Store real orientation.
        g_lastExifOrientation = exifOrientation;
    }
    
    RawProcessor.dcraw_clear_mem(image);
    return hr;
}

HRESULT CImageLoader::LoadToMemory(LPCWSTR filePath, IWICBitmap** ppBitmap, std::wstring* pLoaderName, bool forceFullDecode, CancelPredicate checkCancel, int targetWidth, int targetHeight) {
    if (!filePath || !ppBitmap) return E_INVALIDARG;
    
    // Clear previous state to avoid residue when switching formats
    g_lastFormatDetails.clear();
    g_lastExifOrientation = 0; // Reset to 0 ("not set by decoder")
    
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
             detectedFmt == L"HEIC" ||
             ((detectedFmt == L"Unknown") &&
              (path.ends_with(L".avif") || path.ends_with(L".avifs") ||
               path.ends_with(L".heic") || path.ends_with(L".heif")))) {
        HRESULT hr = LoadAVIF(filePath, ppBitmap);
        if (SUCCEEDED(hr)) {
            if (pLoaderName) {
                *pLoaderName = (detectedFmt == L"HEIC" || path.ends_with(L".heic") || path.ends_with(L".heif"))
                    ? L"libavif/HEIF"
                    : L"libavif";
            }
            return S_OK;
        }
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

        // PSB path: try native composite decoder (Raw/RLE).
        std::vector<uint8_t> psdBuf;
        if (ReadFileToVector(filePath, psdBuf)) {
            DecodeContext decodeCtx;
            decodeCtx.allocator = [](size_t s) -> uint8_t* { return new (std::nothrow) uint8_t[s]; };
            decodeCtx.freeFunc = [](uint8_t* p) { delete[] p; };
            decodeCtx.checkCancel = checkCancel;

            DecodeResult decodeRes;
            hr = QuickView::Codec::PsdComposite::Load(psdBuf.data(), psdBuf.size(), decodeCtx, decodeRes);
            if (SUCCEEDED(hr) && decodeRes.success && decodeRes.pixels) {
                const UINT cb = static_cast<UINT>(decodeRes.stride * decodeRes.height);
                hr = CreateWICBitmapFromMemory(
                    decodeRes.width,
                    decodeRes.height,
                    GUID_WICPixelFormat32bppBGRA,
                    decodeRes.stride,
                    cb,
                    decodeRes.pixels,
                    ppBitmap);
                decodeCtx.freeFunc(decodeRes.pixels);
                if (SUCCEEDED(hr)) {
                    if (pLoaderName) *pLoaderName = decodeRes.metadata.LoaderName.empty() ? L"PSB Composite" : decodeRes.metadata.LoaderName;
                    return S_OK;
                }
            } else if (decodeRes.pixels) {
                decodeCtx.freeFunc(decodeRes.pixels);
            }

            // Fallback to embedded JPEG preview if full composite decode fails.
            PreviewExtractor::ExtractedData exData;
            if (PreviewExtractor::ExtractFromPSD(psdBuf.data(), psdBuf.size(), exData) &&
                exData.IsValid() &&
                exData.size <= static_cast<size_t>(MAXDWORD)) {
                ComPtr<IWICStream> stream;
                hr = m_wicFactory->CreateStream(&stream);
                if (SUCCEEDED(hr)) {
                    hr = stream->InitializeFromMemory(const_cast<BYTE*>(exData.pData), static_cast<DWORD>(exData.size));
                }

                ComPtr<IWICBitmapDecoder> decoder;
                if (SUCCEEDED(hr)) {
                    hr = m_wicFactory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
                }

                ComPtr<IWICBitmapFrameDecode> frame;
                if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);

                ComPtr<IWICFormatConverter> converter;
                if (SUCCEEDED(hr)) hr = m_wicFactory->CreateFormatConverter(&converter);
                if (SUCCEEDED(hr)) {
                    hr = converter->Initialize(
                        frame.Get(),
                        GUID_WICPixelFormat32bppPBGRA,
                        WICBitmapDitherTypeNone,
                        nullptr,
                        0.f,
                        WICBitmapPaletteTypeMedianCut);
                }
                if (SUCCEEDED(hr)) {
                    hr = m_wicFactory->CreateBitmapFromSource(converter.Get(), WICBitmapCacheOnLoad, ppBitmap);
                }
                if (SUCCEEDED(hr)) {
                    if (pLoaderName) *pLoaderName = L"PSD Preview (Embedded JPEG)";
                    return S_OK;
                }
            }
        }
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
    // NanoSVG Removed
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
         // NanoSVG Removed
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
         else if (path.ends_with(L".psd") || path.ends_with(L".psb")) {
             HRESULT hr = LoadStbImage(filePath, ppBitmap);
             if (SUCCEEDED(hr)) { if (pLoaderName) *pLoaderName = L"Stb Image (PSD)"; return S_OK; }
         }
    }


    // 3. Robust Fallback to WIC (Standard Loading)
    if (pLoaderName && pLoaderName->empty()) *pLoaderName = L"WIC (Fallback)";

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

    // [v4.1] Native WIC Downscaling Support (IWICBitmapScaler)
    // For HEIC, HEIF, and TIFF, if Titan demands an LOD=1 base scale, we scale natively here.
    ComPtr<IWICBitmapSource> finalSource = frame;
    
    // First, try fast native decoder-level scaling (IWICBitmapSourceTransform)
    bool usedNativeScaling = false;
    if (targetWidth > 0 && targetHeight > 0) {
        UINT width, height;
        if (SUCCEEDED(frame->GetSize(&width, &height)) && (width > (UINT)targetWidth || height > (UINT)targetHeight)) {
            // Find proportional size
            float scaleW = (float)targetWidth / width;
            float scaleH = (float)targetHeight / height;
            float scale = (std::min)(scaleW, scaleH);
            
            UINT finalW = (UINT)(width * scale + 0.5f);
            UINT finalH = (UINT)(height * scale + 0.5f);
            if (finalW < 1) finalW = 1;
            if (finalH < 1) finalH = 1;
            
            // Try IWICBitmapSourceTransform for ultra-fast codec-level scale (HEVC natively supports this)
            ComPtr<IWICBitmapSourceTransform> pTransform;
            if (SUCCEEDED(frame.As(&pTransform))) {
                // Not copying here because we need an IWICBitmapSource for the converter.
                // But wait, IWICBitmapSourceTransform is used via CopyPixels, making it hard to plug into converter pipeline.
                // WIC's standard IWICBitmapScaler handles requesting native scale from the decoder underneath anyway!
                // So using Scaler is mathematically optimal.
            }

            ComPtr<IWICBitmapScaler> scaler;
            if (SUCCEEDED(m_wicFactory->CreateBitmapScaler(&scaler))) {
                if (SUCCEEDED(scaler->Initialize(frame.Get(), finalW, finalH, WICBitmapInterpolationModeFant))) {
                    finalSource = scaler;
                    if (pLoaderName) *pLoaderName += L" [WIC Scaler]";
                    usedNativeScaling = true;
                }
            }
        }
    }
    
    // 2. Convert to D2D Compatible Format (PBGRA32 or 128bppRGBAFloat for HDR)
    ComPtr<IWICFormatConverter> converter;
    hr = m_wicFactory->CreateFormatConverter(&converter);
    if (FAILED(hr)) return hr;

    WICPixelFormatGUID srcFormat;
    hr = frame->GetPixelFormat(&srcFormat);

    // Check if the source format is a high-precision/HDR format
    bool isHighPrecision = false;
    if (SUCCEEDED(hr)) {
        if (IsEqualGUID(srcFormat, GUID_WICPixelFormat128bppRGBAFloat) ||
            IsEqualGUID(srcFormat, GUID_WICPixelFormat128bppPRGBAFloat) ||
            IsEqualGUID(srcFormat, GUID_WICPixelFormat128bppRGBFloat) ||
            IsEqualGUID(srcFormat, GUID_WICPixelFormat64bppRGBAHalf) ||
            IsEqualGUID(srcFormat, GUID_WICPixelFormat64bppPRGBAHalf) ||
            IsEqualGUID(srcFormat, GUID_WICPixelFormat64bppRGBHalf) ||
            IsEqualGUID(srcFormat, GUID_WICPixelFormat64bppRGBA) ||
            IsEqualGUID(srcFormat, GUID_WICPixelFormat64bppPRGBA) ||
            IsEqualGUID(srcFormat, GUID_WICPixelFormat64bppRGB) ||
            IsEqualGUID(srcFormat, GUID_WICPixelFormat48bppRGB) ||
            IsEqualGUID(srcFormat, GUID_WICPixelFormat48bppRGBHalf) ||
            IsEqualGUID(srcFormat, GUID_WICPixelFormat32bppRGBE)) {
            isHighPrecision = true;
        }
    }

    WICPixelFormatGUID targetFormat = isHighPrecision ? GUID_WICPixelFormat128bppRGBAFloat : GUID_WICPixelFormat32bppPBGRA;

    hr = converter->Initialize(
        finalSource.Get(), // Use frame source
        targetFormat,
        WICBitmapDitherTypeNone,
        nullptr,
        0.f,
        WICBitmapPaletteTypeMedianCut
    );
    if (FAILED(hr)) {
        // Fallback to PBGRA if float conversion is not supported
        targetFormat = GUID_WICPixelFormat32bppPBGRA;
        hr = converter->Initialize(
            finalSource.Get(),
            targetFormat,
            WICBitmapDitherTypeNone,
            nullptr,
            0.f,
            WICBitmapPaletteTypeMedianCut
        );
        if (FAILED(hr)) return hr;
    }

    // 3. Force Decode to Memory
    HRESULT hrBitmap = m_wicFactory->CreateBitmapFromSource(
        converter.Get(),
        WICBitmapCacheOnLoad, 
        ppBitmap
    );

    if (SUCCEEDED(hrBitmap) && pLoaderName && pLoaderName->contains(L"WIC") && !usedNativeScaling) {
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

                // [v6.0] Exif Marker Persistence for EasyExif
                jpeg_save_markers(&cinfo, JPEG_APP0 + 1, 0xFFFF);
                
                // [CMS] Save APP2 markers for ICC Profile extraction
                jpeg_save_markers(&cinfo, JPEG_APP0 + 2, 0xFFFF);

                if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
                    jpeg_destroy_decompress(&cinfo);
                    return E_FAIL;
                }
                
                // [CMS] Extract and Merge Multi-segment ICC Profile from APP2 markers
                {
                    size_t iccTotalSize = 0;
                    int iccNumMarkers = 0;
                    
                    // First pass: identify markers and calculate size
                    for (jpeg_saved_marker_ptr marker = cinfo.marker_list; marker; marker = marker->next) {
                        if (marker->marker == 0xE2 && marker->data_length >= 14) {
                             if (memcmp(marker->data, "ICC_PROFILE", 11) == 0) {
                                 iccTotalSize += (marker->data_length - 14);
                                 iccNumMarkers++;
                             }
                        }
                    }
                    
                    {
                        wchar_t logBuf[128];
                        swprintf_s(logBuf, L"[Loader] JPEG Markers Check: Found %d ICC segments. Total size: %zu\n", iccNumMarkers, iccTotalSize);
                        OutputDebugStringW(logBuf);
                    }
                    
                    if (iccNumMarkers > 0) {
                        result.metadata.iccProfileData.resize(iccTotalSize);
                        uint8_t* pIcc = result.metadata.iccProfileData.data();
                        size_t currentOffset = 0;
                        int markersmerged = 0;
                        
                        // Second pass: Merge segments in correct sequence
                        for (int seq = 1; seq <= 255; ++seq) {
                             bool found = false;
                             for (jpeg_saved_marker_ptr marker = cinfo.marker_list; marker; marker = marker->next) {
                                 if (marker->marker == JPEG_APP0 + 2 && marker->data_length >= 14 && memcmp(marker->data, "ICC_PROFILE\0", 12) == 0) {
                                      if (marker->data[12] == seq) {
                                           size_t partLen = marker->data_length - 14;
                                           if (currentOffset + partLen <= iccTotalSize) {
                                               memcpy(pIcc + currentOffset, marker->data + 14, partLen);
                                               currentOffset += partLen;
                                               markersmerged++;
                                           }
                                           found = true;
                                           break;
                                      }
                                 }
                             }
                             if (!found) break; // Finished or missing segment
                        }
                        
                        if (markersmerged > 0) {
                            result.metadata.HasEmbeddedColorProfile = true;
                        } else {
                            result.metadata.iccProfileData.clear();
                        }
                    }
                }
                
                // [v6.0] Parse EXIF Marker (Native)
                for (jpeg_saved_marker_ptr marker = cinfo.marker_list; marker; marker = marker->next) {
                    if (marker->marker == JPEG_APP0 + 1 && marker->data_length >= 6 && memcmp(marker->data, "Exif\0\0", 6) == 0) {
                        easyexif::EXIFInfo exif;
                        if (exif.parseFromEXIFSegment(marker->data, marker->data_length) == 0) {
                             PopulateMetadataFromEasyExif(exif, result.metadata);
                        }
                        break; // Found it
                    }
                }

                // [v6.0] Fill FormatDetails (Q + Subsampling)
                {
                    // Estimate Quality
                    int q = GetJpegQualityFromBuffer(pBuf, bufSize);
                    
                    // Determine Subsampling
                    std::wstring sub = L"";
                    if (cinfo.num_components == 3) {
                        int h0 = cinfo.comp_info[0].h_samp_factor;
                        int v0 = cinfo.comp_info[0].v_samp_factor;
                        int h1 = cinfo.comp_info[1].h_samp_factor;
                        int v1 = cinfo.comp_info[1].v_samp_factor;
                        int h2 = cinfo.comp_info[2].h_samp_factor;
                        int v2 = cinfo.comp_info[2].v_samp_factor;

                        if (h0 == 2 && v0 == 2 && h1 == 1 && v1 == 1 && h2 == 1 && v2 == 1) sub = L"4:2:0";
                        else if (h0 == 2 && v0 == 1 && h1 == 1 && v1 == 1 && h2 == 1 && v2 == 1) sub = L"4:2:2";
                        else if (h0 == 1 && v0 == 1 && h1 == 1 && v1 == 1 && h2 == 1 && v2 == 1) sub = L"4:4:4";
                        else sub = L"4:4:0"; // Approximation for others
                    } else if (cinfo.num_components == 1) {
                        sub = L"Gray";
                    }

                    wchar_t fmtBuf[64];
                    if (q > 0) swprintf_s(fmtBuf, L"%s Q=%d", sub.c_str(), q);
                    else swprintf_s(fmtBuf, L"%s", sub.c_str());
                    result.metadata.FormatDetails = fmtBuf;
                    
                    // Progressive?
                    if (jpeg_has_multiple_scans(&cinfo)) {
                         result.metadata.FormatDetails += L" Prog";
                    }
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
                
                // [v5.4 Quality Metrics] Extract JPEG Details
                std::wstring details;
                
                // 1. Progressive vs Baseline
                if (jpeg_has_multiple_scans(&cinfo)) details += L"Progressive, ";
                else details += L"Baseline, ";
                
                // 2. Chroma Subsampling
                // cinfo.comp_info[0] is usually Y. Check sampling factors relative to Max.
                // Standard: MaxH=2 MaxV=2. 
                // 4:4:4 -> Y=1x1 (relative) or Y=MaxH x MaxV
                // Actually easier: Check Sum of H*V? 
                // Common check:
                int h0 = cinfo.comp_info[0].h_samp_factor;
                int v0 = cinfo.comp_info[0].v_samp_factor;
                int maxH = cinfo.max_h_samp_factor;
                int maxV = cinfo.max_v_samp_factor;
                int numComps = cinfo.num_components;
                
                if (numComps == 1) {
                    details += L"Grayscale";
                } else if (numComps == 3) {
                    if (h0 == maxH && v0 == maxV) {
                        int h1 = cinfo.comp_info[1].h_samp_factor;
                        int v1 = cinfo.comp_info[1].v_samp_factor;
                        
                        if (h1 == h0 && v1 == v0) details += L"4:4:4";
                        else if (h1 * 2 == h0 && v1 == v0) details += L"4:2:2";
                        else if (h1 * 2 == h0 && v1 * 2 == v0) details += L"4:2:0";
                        else if (h1 == h0 && v1 * 2 == v0) details += L"4:4:0";
                        else details += L"Subsampled (Custom)";
                    } else {
                        details += L"Subsampled";
                    }
                } else if (numComps == 4) {
                    details += L"CMYK";
                }
                
                // 3. Bit Depth (if available in this struct version, usually 8)
                // if (cinfo.data_precision != 8) details += L" " + std::to_wstring(cinfo.data_precision) + L"bpp";
                
                // [v5.7] Append Estimated Q-Value
                int q = GetJpegQualityFromBuffer(pBuf, bufSize);
                if (q > 0) {
                    details += L" Q~" + std::to_wstring(q);
                }
                
                result.metadata.FormatDetails = details;

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

                // [v5.3] Fill metadata directly
                // result.metadata.FormatDetails already populated above
                if (cinfo.output_width < cinfo.image_width) result.metadata.FormatDetails += L" [Scaled]";
                result.metadata.Width = cinfo.image_width; // Original size
                result.metadata.Height = cinfo.image_height;

                jpeg_finish_decompress(&cinfo);
                jpeg_destroy_decompress(&cinfo);

                result.pixels = pixels;
                result.width = w;
                result.height = h;
                result.stride = stride;
                result.format = ctx.format;
                result.success = true;
                
                // [Fix] Respect EasyExif result if available. Fallback to Raw Scan only if needed.
                if (result.metadata.ExifOrientation == 1) {
                    result.metadata.ExifOrientation = ReadJpegExifOrientation(pBuf, bufSize);
                }

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
                
                // [v6.3] Capture metadata
                WuffsLoader::WuffsImageInfo info;
                
                // We can wrap the allocator to capture the pointer.
                uint8_t* capturedPtr = nullptr;
                auto capturingAlloc = [&](size_t s) -> uint8_t* {
                    capturedPtr = ctx.allocator(s);
                    return capturedPtr;
                };
                
                // Call Wuffs with CAPTURING allocator AND Metadata Info
                if (!WuffsLoader::DecodePNG(data, size, &w, &h, capturingAlloc, ctx.checkCancel, &info)) return E_FAIL;
                
                result.pixels = capturedPtr;
                result.width = w;
                result.height = h;
                result.stride = w * 4; // Wuffs is packed
                result.format = PixelFormat::BGRA8888; // Wuffs is BGRA Premul
                result.success = true;

                // [v5.3] Fill metadata directly
                // [v6.3] Use PopulateFormatDetails helper for consistent formatting
                // "Wuffs PNG" is the base format name.
                // Info struct gives us: bitDepth, hasAlpha, isAnim.
                // We can use CImageLoader::PopulateFormatDetails(..., L"Wuffs PNG", info.bitDepth, true (lossless), info.hasAlpha, info.isAnim);
                // Wait, PopulateFormatDetails helper is static member.
                // We are inside CImageLoader (friend/nested)? No, Codec namespace.
                // But PopulateFormatDetails is public static.
                
                // [v6.3] Manual formatting for FormatDetails using extracted info
                // Always show bit depth (User preference)
                bool showBitDepth = (info.bitDepth > 0);
                
                std::wstring details = L"Wuffs PNG";
                if (showBitDepth) details += L" " + std::to_wstring(info.bitDepth) + L"-bit";
                if (info.hasAlpha) details += L" Alpha";
                if (info.isAnim) details += L" [Anim]";
                
                result.metadata.FormatDetails = details;
                result.metadata.Width = w;
                result.metadata.Height = h;
                
                // [CMS] Extract ICC Profile from Wuffs result
                if (!info.iccProfile.empty()) {
                    result.metadata.iccProfileData = std::move(info.iccProfile);
                    result.metadata.HasEmbeddedColorProfile = true;
                }
                
                return S_OK;
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

            static HRESULT LoadTGA(const uint8_t* data, size_t size, const DecodeContext& ctx, DecodeResult& result) {
                uint32_t w = 0, h = 0;
                uint8_t* capturedPtr = nullptr;
                auto capturingAlloc = [&](size_t s) { return capturedPtr = ctx.allocator(s); };

                if (WuffsLoader::DecodeTGA(data, size, &w, &h, capturingAlloc, ctx.checkCancel)) {
                    result.pixels = capturedPtr;
                    result.width = w;
                    result.height = h;
                    result.stride = w * 4;
                    result.format = PixelFormat::BGRA8888;
                    result.success = true;
                    // [v5.3] Fill metadata directly
                    result.metadata.FormatDetails = L"Wuffs TGA";
                    result.metadata.Width = w;
                    result.metadata.Height = h;
                    return S_OK;
                }
                return E_FAIL;
            }

            static HRESULT LoadQOI(const uint8_t* data, size_t size, const DecodeContext& ctx, DecodeResult& result) {
                uint32_t w = 0, h = 0;
                uint8_t* capturedPtr = nullptr;
                auto capturingAlloc = [&](size_t s) { return capturedPtr = ctx.allocator(s); };

                if (WuffsLoader::DecodeQOI(data, size, &w, &h, capturingAlloc, ctx.checkCancel)) {
                    result.pixels = capturedPtr;
                    result.width = w;
                    result.height = h;
                    result.stride = w * 4;
                    result.format = PixelFormat::BGRA8888;
                    result.success = true;
                    // [v5.3] Fill metadata directly
                    result.metadata.FormatDetails = L"Wuffs QOI";
                    result.metadata.Width = w;
                    result.metadata.Height = h;
                    return S_OK;
                }
                return E_FAIL;
            }
            
            static HRESULT LoadWBMP(const uint8_t* data, size_t size, const DecodeContext& ctx, DecodeResult& result) {
                uint32_t w = 0, h = 0;
                uint8_t* capturedPtr = nullptr;
                auto capturingAlloc = [&](size_t s) { return capturedPtr = ctx.allocator(s); };

                if (WuffsLoader::DecodeWBMP(data, size, &w, &h, capturingAlloc, ctx.checkCancel)) {
                    result.pixels = capturedPtr;
                    result.width = w;
                    result.height = h;
                    result.stride = w * 4;
                    result.format = PixelFormat::BGRA8888;
                    result.success = true;
                    // [v5.3] Fill metadata directly
                    result.metadata.FormatDetails = L"Wuffs WBMP";
                    result.metadata.Width = w;
                    result.metadata.Height = h;
                    return S_OK;
                }
                return E_FAIL;
            }

            static HRESULT LoadNetPBM(const uint8_t* data, size_t size, const DecodeContext& ctx, DecodeResult& result) {
                uint32_t w = 0, h = 0;
                uint8_t* capturedPtr = nullptr;
                auto capturingAlloc = [&](size_t s) { return capturedPtr = ctx.allocator(s); };

                if (WuffsLoader::DecodeNetpbm(data, size, &w, &h, capturingAlloc, ctx.checkCancel)) {
                    result.pixels = capturedPtr;
                    result.width = w;
                    result.height = h;
                    result.stride = w * 4;
                    result.format = PixelFormat::BGRA8888;
                    result.success = true;
                    // [v5.3] Fill metadata directly
                    result.metadata.FormatDetails = L"Wuffs NetPBM";
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

                int events = JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE | JXL_DEC_FRAME | JXL_DEC_COLOR_ENCODING | JXL_DEC_FRAME_PROGRESSION;
                if (ctx.forcePreview) events |= JXL_DEC_PREVIEW_IMAGE;
                // [v6.2] Parse EXIF if metadata requested
                if (ctx.pMetadata) events |= JXL_DEC_BOX;

                if (JXL_DEC_SUCCESS != JxlDecoderSubscribeEvents(dec, events)) {
                    JxlDecoderDestroy(dec); return E_FAIL;
                }

                JxlBasicInfo info = {};
                JxlColorEncoding encodedColor = {};
                bool useFloatOutput = false;
                QuickView::TransferFunction transfer = QuickView::TransferFunction::Unknown;
                QuickView::ColorPrimaries primaries = QuickView::ColorPrimaries::Unknown;
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
                // [v6.2] Buffer for EXIF data
                std::vector<uint8_t> jxlExifBuffer;

                int finalW = 0, finalH = 0;
                bool headerRead = false;
                bool wantPreview = ctx.forcePreview;
                
                // [v6.2] Metadata Details (Lossy/Lossless, Bit Depth)
                // We need to query BasicInfo even if not strictly required for decode
                // But JXL decode event flow provides it.
                // We should subscribe to BASIC_INFO.

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
                        if (offset >= size) {
                             JxlDecoderCloseInput(dec);
                             continue;
                        }
                        size_t remain = JxlDecoderReleaseInput(dec);
                        offset -= remain; // Backtrack to include unconsumed bytes

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
                        
                        // [v6.2] Populate Metadata
                        if (ctx.pMetadata) {
                             CImageLoader::PopulateFormatDetails(
                                 ctx.pMetadata, 
                                 L"JXL", 
                                 info.bits_per_sample, 
                                 info.uses_original_profile, // likely lossless
                                 info.alpha_bits > 0, 
                                 (info.have_animation == JXL_TRUE)
                             );
                        }
                     }
                     
                     // [v6.2] Handle Color Encoding (ICC)
                     else if (status == JXL_DEC_COLOR_ENCODING) {
                         if (ctx.pMetadata) {
                             size_t iccSize = 0;
                             if (JXL_DEC_SUCCESS == JxlDecoderGetICCProfileSize(dec, JXL_COLOR_PROFILE_TARGET_DATA, &iccSize)) {
                                 if (iccSize > 0) {
                                     std::vector<uint8_t> icc(iccSize);
                                     if (JXL_DEC_SUCCESS == JxlDecoderGetColorAsICCProfile(dec, JXL_COLOR_PROFILE_TARGET_DATA, icc.data(), iccSize)) {
                                         std::wstring desc = CImageLoader::ParseICCProfileName(icc.data(), iccSize);
                                         if (!desc.empty()) ctx.pMetadata->ColorSpace = desc;
                                     }
                                 }
                             }
                         }

                         if (JXL_DEC_SUCCESS == JxlDecoderGetColorAsEncodedProfile(dec, JXL_COLOR_PROFILE_TARGET_ORIGINAL, &encodedColor)) {
                             transfer = MapJxlTransferFunction(encodedColor.transfer_function);
                             primaries = MapJxlPrimaries(encodedColor.primaries);
                             useFloatOutput =
                                 !ctx.forcePreview &&
                                 (transfer == QuickView::TransferFunction::Linear ||
                                  transfer == QuickView::TransferFunction::PQ ||
                                  transfer == QuickView::TransferFunction::HLG ||
                                  info.bits_per_sample > 8);

                             if (ctx.pMetadata) {
                                 ctx.pMetadata->hdrMetadata.isValid = true;
                                 ctx.pMetadata->hdrMetadata.transfer = transfer;
                                 ctx.pMetadata->hdrMetadata.primaries = primaries;
                                 ctx.pMetadata->hdrMetadata.isSceneLinear = (transfer == QuickView::TransferFunction::Linear);
                                 ctx.pMetadata->hdrMetadata.isHdr =
                                     transfer == QuickView::TransferFunction::Linear ||
                                     transfer == QuickView::TransferFunction::PQ ||
                                     transfer == QuickView::TransferFunction::HLG ||
                                     info.bits_per_sample > 8;
                             }
                         }
                     }
                     
                     // [v6.2] Handle EXIF Box
                    else if (status == JXL_DEC_BOX) {
                        JxlBoxType type;
                        if (JXL_DEC_SUCCESS == JxlDecoderGetBoxType(dec, type, JXL_TRUE)) {
                            if (memcmp(type, "Exif", 4) == 0 && ctx.pMetadata) {
                                uint64_t boxSize = 0;
                                if (JXL_DEC_SUCCESS == JxlDecoderGetBoxSizeRaw(dec, &boxSize)) {
                                    // Header is 4 (size 4) + 4 (type 4) = 8? No, Raw size includes header?
                                    // JXL API: Raw size is full box size.
                                    // We need to capture the payload? 
                                    // JxlDecoderSetBoxBuffer captures *payload* if we want?
                                    // No, it captures remaining data in box.
                                    // Start position?
                                    // We use a large buffer to be safe or exact size?
                                    // Exact size if known.
                                    // Note: boxSize might be "0" (until end) or 1 (large).
                                    // Assuming standard box.
                                    
                                    // Safety cap 1MB
                                    if (boxSize < 1024 * 1024 * 5) { // 5MB limit
                                         jxlExifBuffer.resize((size_t)boxSize);
                                         JxlDecoderSetBoxBuffer(dec, jxlExifBuffer.data(), jxlExifBuffer.size());
                                    }
                                }
                            }
                        }
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
                    else if (status == JXL_DEC_FRAME_PROGRESSION) {
                        // [v8.3] Catch the Rabbit: 1:8 Progressive DC Preview
                        // [Fix] Only return DC preview early if forcePreview is set (Titan base layer).
                        // For normal SubmitFullDecode, we MUST continue to full decode.
                        if (ctx.forcePreview) {
                        size_t ratio = JxlDecoderGetIntendedDownsamplingRatio(dec);
                        if (ratio >= 4 && ratio <= 16) {
                            size_t downW = info.xsize / ratio;
                            size_t downH = info.ysize / ratio;
                            if (downW == 0) downW = 1;
                            if (downH == 0) downH = 1;
                            
                            size_t bufferSize = downW * downH * 4;
                            pixels = ctx.allocator(bufferSize);
                            if (!pixels) { JxlDecoderDestroy(dec); return E_OUTOFMEMORY; }
                            
                            if (JXL_DEC_SUCCESS != JxlDecoderSetImageOutBuffer(dec, &format, pixels, bufferSize)) {
                                JxlDecoderDestroy(dec); if (ctx.freeFunc && pixels) ctx.freeFunc(pixels); return E_FAIL;
                            }
                            
                            if (JXL_DEC_SUCCESS != JxlDecoderFlushImage(dec)) {
                                JxlDecoderDestroy(dec); if (ctx.freeFunc && pixels) ctx.freeFunc(pixels); return E_FAIL;
                            }
                            
                            JxlDecoderDestroy(dec);
                            SIMDUtils::SwizzleRGBA_to_BGRA_Premul(pixels, downW * downH);
                            
                            result.pixels = pixels;
                            result.width = (int)downW;
                            result.height = (int)downH;
                            result.stride = (int)downW * 4;
                            result.format = PixelFormat::BGRA8888;
                            result.success = true;
                            
                            // Fill metadata
                            result.metadata.FormatDetails = L"JXL";
                            if (info.have_animation) result.metadata.FormatDetails += L" [Anim]";
                            result.metadata.LoaderName = L"libjxl (Prog DC)"; // Success marker
                            
                            return S_OK;
                        }
                        }
                        // else: not forcePreview, ignore DC event and continue to full decode
                    }
                    else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
                        if (useFloatOutput) {
                            format.data_type = JXL_TYPE_FLOAT;
                        }

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
                        
                        // [Fix] Prevent massive memory allocation stalls (e.g. 4.8GB for 1.2 GigaPixel JXL)
                        // This prevents the OS from thrashing the page file for 15 seconds before crashing.
                        if (ctx.allowFakeBase && bufferSize > 1024ULL * 1024ULL * 1024ULL) { // 1 GB limit
                            OutputDebugStringW(L"[JXL_DC] No 1:8 found and image too large. Faking 1x1 base layer for Region Decoder.\n");
                            // We fake a 1x1 transparent pixel to satisfy the pipeline and unlock Titan Mode.
                            pixels = ctx.allocator(4); // 1 pixel
                            if (!pixels) { JxlDecoderDestroy(dec); return E_OUTOFMEMORY; }
                            
                            memset(pixels, 0, 4); // Transparent pixel
                            
                            JxlDecoderDestroy(dec);
                            
                            result.pixels = pixels;
                            result.width = 1;
                            result.height = 1;
                            result.stride = 4;
                            result.format = PixelFormat::BGRA8888;
                            result.success = true;
                            
                            result.metadata.FormatDetails = L"JXL";
                            if (info.have_animation) result.metadata.FormatDetails += L" [Anim]";
                            result.metadata.LoaderName = L"libjxl (Fake Base)"; 
                            
                            return S_OK;
                        }
                        
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
                        if (useFloatOutput) {
                            float* pf = reinterpret_cast<float*>(pixels);
                            const size_t pixelCount = static_cast<size_t>(finalW) * finalH;
                            for (size_t i = 0; i < pixelCount; ++i) {
                                pf[i * 4 + 0] = DecodeTransferToLinear(pf[i * 4 + 0], transfer);
                                pf[i * 4 + 1] = DecodeTransferToLinear(pf[i * 4 + 1], transfer);
                                pf[i * 4 + 2] = DecodeTransferToLinear(pf[i * 4 + 2], transfer);
                            }
                            result.stride = finalW * 16;
                            result.format = PixelFormat::R32G32B32A32_FLOAT;
                            result.metadata.is_Linear_sRGB = true;
                            result.metadata.hdrMetadata.isSceneLinear = true;
                        } else {
                            // [v8.6] Fix: JXL outputs RGBA Straight, D2D needs BGRA Premultiplied.
                            SIMDUtils::SwizzleRGBA_to_BGRA_Premul(pixels, (size_t)finalW * finalH);
                            result.stride = finalW * 4;
                            result.format = PixelFormat::BGRA8888;
                        }
                        result.success = true;
                        
                        // [v5.3] Fill metadata directly
                        result.metadata.FormatDetails = L"JXL";
                        if (status == JXL_DEC_PREVIEW_IMAGE) result.metadata.FormatDetails += L" (Preview)";
                        if (info.have_animation) result.metadata.FormatDetails += L" [Anim]";
                        result.metadata.hdrMetadata.isValid = true;
                        result.metadata.hdrMetadata.transfer = transfer;
                        result.metadata.hdrMetadata.primaries = primaries;
                        result.metadata.hdrMetadata.isHdr = useFloatOutput;
                        
                        // [v6.2] Parse Captured EXIF
                        if (!jxlExifBuffer.empty() && result.metadata.IsEmpty()) {
                             // JXL Exif box usually starts with 4 byte offset? Or straight TIFF?
                             // 00 00 00 06 (big endian) + "Exif\0\0" ?
                             // Or just Raw TIFF?
                             // Spec says "Exif" box contains Exif data.
                             // Usually it's: [4 bytes offset] [Exif\0\0] [TIFF...] ?
                             // Or just TIFF?
                             // EasyExif expects standard Exif header or TIFF.
                             // Let's try direct first, then offset.
                             easyexif::EXIFInfo exif;
                             if (exif.parseFromEXIFSegment(jxlExifBuffer.data(), (unsigned)jxlExifBuffer.size()) == 0) {
                                 PopulateMetadataFromEasyExif_Refined(exif, result.metadata);
                             } else {
                                 // Try skipping 4 bytes (offset?)
                                 if (jxlExifBuffer.size() > 4) {
                                      if (exif.parseFromEXIFSegment(jxlExifBuffer.data() + 4, (unsigned)(jxlExifBuffer.size() - 4)) == 0) {
                                         PopulateMetadataFromEasyExif_Refined(exif, result.metadata);
                                      }
                                 }
                             }
                        }
                        
                        result.metadata.Width = finalW;
                        result.metadata.Height = finalH;
                        return S_OK;
                    }
                    else if (status == JXL_DEC_FRAME_PROGRESSION) {
                        // [Deep Cancel] Progression event triggers loop to CheckCancel
                        continue;
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

        namespace AVIF {
            static HRESULT Load(const uint8_t* data, size_t size, const DecodeContext& ctx, DecodeResult& result) {
                avifDecoder* decoder = avifDecoderCreate();
                if (!decoder) return E_OUTOFMEMORY;

                decoder->strictFlags = AVIF_STRICT_DISABLED;
                const unsigned int threads = std::thread::hardware_concurrency();
                decoder->maxThreads = threads > 0 ? threads : 4;

                avifResult avifHr = avifDecoderSetIOMemory(decoder, data, size);
                if (avifHr != AVIF_RESULT_OK) {
                    avifDecoderDestroy(decoder);
                    return E_FAIL;
                }

                avifHr = avifDecoderParse(decoder);
                if (avifHr != AVIF_RESULT_OK) {
                    avifDecoderDestroy(decoder);
                    return E_FAIL;
                }

                avifHr = avifDecoderNextImage(decoder);
                if (avifHr != AVIF_RESULT_OK) {
                    avifDecoderDestroy(decoder);
                    return E_FAIL;
                }

                const int origW = decoder->image->width;
                const int origH = decoder->image->height;

                if (ctx.targetWidth > 0 && ctx.targetHeight > 0 &&
                    (origW > ctx.targetWidth || origH > ctx.targetHeight)) {
                    const double scale = (std::min)(
                        static_cast<double>(ctx.targetWidth) / (std::max)(1, origW),
                        static_cast<double>(ctx.targetHeight) / (std::max)(1, origH));
                    if (scale > 0.0 && scale < 1.0) {
                        const uint32_t scaledW = (std::max)(1u, static_cast<uint32_t>(origW * scale + 0.5));
                        const uint32_t scaledH = (std::max)(1u, static_cast<uint32_t>(origH * scale + 0.5));
                        avifImageScale(decoder->image, scaledW, scaledH, &decoder->diag);
                    }
                }

                const QuickView::TransferFunction transfer = MapAvifTransferFunction(decoder->image->transferCharacteristics);
                const QuickView::ColorPrimaries primaries = MapAvifPrimaries(decoder->image->colorPrimaries);
                const bool useFloatOutput =
                    decoder->image->gainMap != nullptr ||
                    transfer == QuickView::TransferFunction::Linear ||
                    transfer == QuickView::TransferFunction::PQ ||
                    transfer == QuickView::TransferFunction::HLG ||
                    decoder->image->depth > 8;

                if (useFloatOutput) {
                    if (decoder->image->gainMap) {
                        uint8_t* gainMappedPixels = nullptr;
                        int gainMappedWidth = 0;
                        int gainMappedHeight = 0;
                        int gainMappedStride = 0;
                        std::wstring gainMapFormatDetails;
                        if (TryDecodeAvifGainMappedLinearRGBA(
                                decoder,
                                ctx,
                                &gainMappedPixels,
                                &gainMappedWidth,
                                &gainMappedHeight,
                                &gainMappedStride,
                                &result.metadata.hdrMetadata,
                                &gainMapFormatDetails)) {
                            result.pixels = gainMappedPixels;
                            result.width = gainMappedWidth;
                            result.height = gainMappedHeight;
                            result.stride = gainMappedStride;
                            result.format = PixelFormat::R32G32B32A32_FLOAT;
                            result.success = true;
                            result.metadata.LoaderName = L"libavif (Unified HDR GainMap)";
                            result.metadata.Width = origW;
                            result.metadata.Height = origH;
                            result.metadata.is_Linear_sRGB = true;
                            if (decoder->image->icc.data && decoder->image->icc.size > 0) {
                                result.metadata.iccProfileData.assign(
                                    decoder->image->icc.data,
                                    decoder->image->icc.data + decoder->image->icc.size);
                            }
                            result.metadata.FormatDetails = gainMapFormatDetails;
                            avifDecoderDestroy(decoder);
                            return S_OK;
                        }
                    }

                    avifRGBImage rgb;
                    avifRGBImageSetDefaults(&rgb, decoder->image);
                    rgb.format = AVIF_RGB_FORMAT_RGBA;
                    rgb.depth = 16;
                    rgb.alphaPremultiplied = AVIF_FALSE;
                    rgb.maxThreads = decoder->maxThreads;

                    avifHr = avifRGBImageAllocatePixels(&rgb);
                    if (avifHr != AVIF_RESULT_OK) {
                        avifDecoderDestroy(decoder);
                        return E_OUTOFMEMORY;
                    }

                    avifHr = avifImageYUVToRGB(decoder->image, &rgb);
                    if (avifHr != AVIF_RESULT_OK) {
                        avifRGBImageFreePixels(&rgb);
                        avifDecoderDestroy(decoder);
                        return E_FAIL;
                    }

                    const int width = rgb.width;
                    const int height = rgb.height;
                    const int stride = CalculateSIMDAlignedStride(width, 16);
                    uint8_t* pixels = ctx.allocator(static_cast<size_t>(stride) * height);
                    if (!pixels) {
                        avifRGBImageFreePixels(&rgb);
                        avifDecoderDestroy(decoder);
                        return E_OUTOFMEMORY;
                    }

                    for (int y = 0; y < height; ++y) {
                        float* dst = reinterpret_cast<float*>(pixels + static_cast<size_t>(y) * stride);
                        const uint16_t* src = reinterpret_cast<const uint16_t*>(rgb.pixels + static_cast<size_t>(y) * rgb.rowBytes);
                        for (int x = 0; x < width; ++x) {
                            const float r = src[x * 4 + 0] / 65535.0f;
                            const float g = src[x * 4 + 1] / 65535.0f;
                            const float b = src[x * 4 + 2] / 65535.0f;
                            const float a = src[x * 4 + 3] / 65535.0f;
                            dst[x * 4 + 0] = DecodeTransferToLinear(r, transfer);
                            dst[x * 4 + 1] = DecodeTransferToLinear(g, transfer);
                            dst[x * 4 + 2] = DecodeTransferToLinear(b, transfer);
                            dst[x * 4 + 3] = a;
                        }
                    }

                    result.pixels = pixels;
                    result.width = width;
                    result.height = height;
                    result.stride = stride;
                    result.format = PixelFormat::R32G32B32A32_FLOAT;
                    result.success = true;
                    result.metadata.LoaderName = L"libavif (Unified HDR)";
                    result.metadata.Width = origW;
                    result.metadata.Height = origH;
                    result.metadata.is_Linear_sRGB = true;
                    PopulateAvifHdrStaticMetadata(decoder->image, &result.metadata.hdrMetadata);
                    result.metadata.hdrMetadata.isSceneLinear = true;
                    result.metadata.hdrMetadata.isHdr = true;
                    if (decoder->image->icc.data && decoder->image->icc.size > 0) {
                        result.metadata.iccProfileData.assign(
                            decoder->image->icc.data,
                            decoder->image->icc.data + decoder->image->icc.size);
                    }

                    wchar_t fmtBuf[128];
                    swprintf_s(fmtBuf, L"%d-bit HDR AVIF", decoder->image->depth);
                    result.metadata.FormatDetails = fmtBuf;
                    if (decoder->image->clli.maxCLL > 0 || decoder->image->clli.maxPALL > 0) {
                        result.metadata.FormatDetails += L" [CLLI]";
                    }
                    if (decoder->image->gainMap) {
                        result.metadata.FormatDetails += L" [GainMap]";
                    }

                    avifRGBImageFreePixels(&rgb);
                    avifDecoderDestroy(decoder);
                    return S_OK;
                }

                avifRGBImage rgb;
                avifRGBImageSetDefaults(&rgb, decoder->image);
                rgb.format = AVIF_RGB_FORMAT_BGRA;
                rgb.depth = 8;
                rgb.alphaPremultiplied = AVIF_TRUE;
                rgb.maxThreads = decoder->maxThreads;

                avifHr = avifRGBImageAllocatePixels(&rgb);
                if (avifHr != AVIF_RESULT_OK) {
                    avifDecoderDestroy(decoder);
                    return E_OUTOFMEMORY;
                }

                avifHr = avifImageYUVToRGB(decoder->image, &rgb);
                if (avifHr != AVIF_RESULT_OK) {
                    avifRGBImageFreePixels(&rgb);
                    avifDecoderDestroy(decoder);
                    return E_FAIL;
                }

                const int width = rgb.width;
                const int height = rgb.height;
                const int stride = static_cast<int>(rgb.rowBytes);
                uint8_t* pixels = ctx.allocator(static_cast<size_t>(stride) * height);
                if (!pixels) {
                    avifRGBImageFreePixels(&rgb);
                    avifDecoderDestroy(decoder);
                    return E_OUTOFMEMORY;
                }

                memcpy(pixels, rgb.pixels, static_cast<size_t>(stride) * height);

                result.pixels = pixels;
                result.width = width;
                result.height = height;
                result.stride = stride;
                result.format = PixelFormat::BGRA8888;
                result.success = true;
                result.metadata.LoaderName = L"libavif (Unified)";
                result.metadata.Width = origW;
                result.metadata.Height = origH;
                PopulateAvifHdrStaticMetadata(decoder->image, &result.metadata.hdrMetadata);
                result.metadata.hdrMetadata.isHdr = false;
                if (decoder->image->icc.data && decoder->image->icc.size > 0) {
                    result.metadata.iccProfileData.assign(
                        decoder->image->icc.data,
                        decoder->image->icc.data + decoder->image->icc.size);
                }
                wchar_t fmtBuf[128];
                swprintf_s(fmtBuf, L"%d-bit AVIF", decoder->image->depth);
                result.metadata.FormatDetails = fmtBuf;
                if (decoder->image->clli.maxCLL > 0 || decoder->image->clli.maxPALL > 0) {
                    result.metadata.FormatDetails += L" [CLLI]";
                }
                if (decoder->image->gainMap) {
                    result.metadata.FormatDetails += L" [GainMap]";
                }

                avifRGBImageFreePixels(&rgb);
                avifDecoderDestroy(decoder);
                return S_OK;
            }
        } // namespace AVIF

        namespace RawCodec {
            // [v7.5] Optimized RAW Decoder (LibRaw)
            // Strategy: Prio Embedded Preview -> Fallback Half-Size -> Full Size (if Forced)
            static HRESULT Load(LPCWSTR filePath, const DecodeContext& ctx, DecodeResult& result) {
                LibRaw RawProcessor;

                // Debug: Entry point
                OutputDebugStringW(L"[RawCodec] Entering RawCodec::Load\n");

                // [v9.3] Use wide-char path directly on Windows (fixes -100009 error)
                // LibRaw on Windows supports open_file(const wchar_t*) overload
#ifdef _WIN32
                int openResult = RawProcessor.open_file(filePath);
#else
                // Fallback to UTF-8 for non-Windows
                std::string pathUtf8;
                int len = WideCharToMultiByte(CP_UTF8, 0, filePath, -1, NULL, 0, NULL, NULL);
                if (len > 0) {
                    pathUtf8.resize(len);
                    WideCharToMultiByte(CP_UTF8, 0, filePath, -1, &pathUtf8[0], len, NULL, NULL);
                    pathUtf8.pop_back(); 
                }
                int openResult = RawProcessor.open_file(pathUtf8.c_str());
#endif
                if (openResult != LIBRAW_SUCCESS) {
                    wchar_t dbg[128];
                    swprintf_s(dbg, L"[RawCodec] open_file FAILED: %d\n", openResult);
                    OutputDebugStringW(dbg);
                    return E_FAIL;
                }
                OutputDebugStringW(L"[RawCodec] open_file OK\n");

                // [v10.1] Capture RAW orientation early
                int flip = RawProcessor.imgdata.sizes.flip;
                int exifOrientation = 1;
                // Mapping: LibRaw internal flip enum -> EXIF orientation tag
                if (flip == 3) exifOrientation = 3;      // 180
                else if (flip == 6) exifOrientation = 6; // 90 CW (Rotate 90)
                else if (flip == 5) exifOrientation = 8; // 90 CCW (Rotate 270)
                else if (flip == 1) exifOrientation = 2; // Flipped
                else if (flip == 2) exifOrientation = 4;
                else if (flip == 4) exifOrientation = 5;
                else if (flip == 7) exifOrientation = 7;
                else exifOrientation = 1;

                result.metadata.ExifOrientation = exifOrientation;

                // [v10.1] Force NOT flipping in LibRaw so UI Renderer can handle it with zero-copy/fast-path
                // This ensures consistency with JPEG/WebP/PNG loaders which don't rotate pixels.
                RawProcessor.imgdata.params.user_flip = 0; 

                // 2. Strategy Check
                // Force Raw Logic: If enabled, we skip embedded preview and force full process. // [USER REQUEST]
                bool forceRawStart = g_runtime.ForceRawDecode; 
                
                // If NOT forced, prioritize embedded preview extraction
                if (!forceRawStart) {
                    int unpackResult = RawProcessor.unpack_thumb();
                    
                    // Debug logging
                    wchar_t dbg[256];
                    swprintf_s(dbg, L"[RawCodec] unpack_thumb: %d\n", unpackResult);
                    OutputDebugStringW(dbg);
                    
                    if (unpackResult == LIBRAW_SUCCESS) {
                        libraw_processed_image_t* thumb = RawProcessor.dcraw_make_mem_thumb();
                        
                        swprintf_s(dbg, L"[RawCodec] thumb=%p, type=%d, size=%d\n", 
                                   thumb, thumb ? thumb->type : -1, thumb ? thumb->data_size : 0);
                        OutputDebugStringW(dbg);
                        
                        if (thumb) {
                            if (thumb->type == LIBRAW_IMAGE_JPEG) {
                                // Delegate to Codec::JPEG
                                HRESULT hr = JPEG::Load((uint8_t*)thumb->data, thumb->data_size, ctx, result);
                                if (SUCCEEDED(hr)) {
                                    RawProcessor.dcraw_clear_mem(thumb);
                                    // Override Metadata
                                    result.metadata.LoaderName = L"LibRaw (Preview)";
                                    // We keep the ExifOrientation from JPEG::Load or the one we captured above
                                    if (result.metadata.ExifOrientation <= 1) result.metadata.ExifOrientation = exifOrientation;
                                    OutputDebugStringW(L"[RawCodec] Preview JPEG decoded OK\n");
                                    return S_OK;
                                }
                                OutputDebugStringW(L"[RawCodec] Preview JPEG decode FAILED\n");
                            }
                            else if (thumb->type == LIBRAW_IMAGE_BITMAP) {
                                // RGB Bitmap
                                if (thumb->bits == 8 && thumb->colors == 3) {
                                    int w = thumb->width;
                                    int h = thumb->height;
                                    int stride = CalculateSIMDAlignedStride(w, 4); 
                                    size_t totalSize = (size_t)stride * h;
                                    
                                    uint8_t* pixels = ctx.allocator(totalSize);
                                    if (pixels) {
                                        uint8_t* src = (uint8_t*)thumb->data;
                                        uint8_t* dst = pixels;
                                        
                                        for(int y=0; y<h; y++) {
                                            uint8_t* rowSrc = src + (y * w * 3);
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
                                        result.metadata.LoaderName = L"LibRaw (PreviewBmp)";
                                        result.metadata.FormatDetails = L"Embedded Bitmap";
                                        result.metadata.Width = w;
                                        result.metadata.Height = h;
                                        result.metadata.ExifOrientation = exifOrientation; // [Fix] Preserve orientation
                                        
                                        RawProcessor.dcraw_clear_mem(thumb);
                                        return S_OK;
                                    }
                                }
                            }
                            RawProcessor.dcraw_clear_mem(thumb);
                        }
                    }
                } // End embedded preview attempt

                // 3. Decode Fallback (LibRaw Processing)
                // "Extract failed then half size. Not active full size... unless forced."
                
                if (ctx.checkCancel && ctx.checkCancel()) return E_ABORT;

                RawProcessor.imgdata.params.use_camera_wb = 1;
                RawProcessor.imgdata.params.use_auto_wb = 0; 
                
                // [v9.9] Optimize demosaic algorithm based on decode mode
                if (g_runtime.ForceRawDecode) {
                     // Full Decode: Use AHD for quality
                     RawProcessor.imgdata.params.user_qual = 2; // AHD (Standard Quality)
                     RawProcessor.imgdata.params.half_size = 0;
                } else {
                     // Preview Mode: Use faster Linear demosaic + Half Size
                     RawProcessor.imgdata.params.user_qual = 0; // Linear (Fastest)
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
                     
                     // [v9.9] Convert RGB to BGRA with OpenMP parallelization
                     uint8_t* src = (uint8_t*)image->data;
                     uint8_t* dst = pixels;
                     
                     #pragma omp parallel for schedule(dynamic, 32)
                     for(int y=0; y<h; y++) {
                        uint8_t* rowSrc = src + (y * w * 3);
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
                     result.metadata.LoaderName = L"LibRaw";
                     result.metadata.IsRawFullDecode = true; // [Compare RAW] mark as full decode
                     result.metadata.FormatDetails = L"RAW Developed";
                     if (RawProcessor.imgdata.params.half_size) result.metadata.FormatDetails += L" (Half)";
                     else result.metadata.FormatDetails += L" (Full)";
                     
                     result.metadata.Width = w;
                     result.metadata.Height = h;
                     
                     hr = S_OK;
                }
                
                RawProcessor.dcraw_clear_mem(image);
                return hr;
            }
        } // namespace RawCodec

        // NanoSVG namespace removed


        namespace TinyEXR {
            static HRESULT Load(const uint8_t* data, size_t size, const DecodeContext& ctx, DecodeResult& result) {
                int w = 0, h = 0;
                std::vector<float> floatPixels;
                
                if (!TinyExrLoader::LoadEXRFromMemory(data, size, &w, &h, floatPixels)) {
                    return E_FAIL;
                }

                if (w <= 0 || h <= 0 || floatPixels.empty()) return E_FAIL;

                int outW = w;
                int outH = h;
                if (ctx.targetWidth > 0 || ctx.targetHeight > 0) {
                    const double tw = (ctx.targetWidth > 0) ? static_cast<double>(ctx.targetWidth) : static_cast<double>(w);
                    const double th = (ctx.targetHeight > 0) ? static_cast<double>(ctx.targetHeight) : static_cast<double>(h);
                    double scale = (std::min)(tw / static_cast<double>(w), th / static_cast<double>(h));
                    if (scale > 1.0) scale = 1.0;
                    if (scale > 0.0 && scale < 1.0) {
                        outW = (std::max)(1, static_cast<int>(w * scale + 0.5));
                        outH = (std::max)(1, static_cast<int>(h * scale + 0.5));
                    }
                }

                const int bytesPerPixel = 16;
                int stride = CalculateSIMDAlignedStride(outW, bytesPerPixel);
                size_t totalSize = (size_t)stride * outH;
                uint8_t* pixels = ctx.allocator(totalSize);
                if (!pixels) return E_OUTOFMEMORY;

                std::vector<int> srcXForOut(outW);
                for (int ox = 0; ox < outW; ++ox) {
                    int sx = static_cast<int>((static_cast<int64_t>(ox) * w) / outW);
                    if (sx >= w) sx = w - 1;
                    srcXForOut[ox] = sx;
                }

                std::vector<int> srcYToOut(h, -1);
                for (int oy = 0; oy < outH; ++oy) {
                    int sy = static_cast<int>((static_cast<int64_t>(oy) * h) / outH);
                    if (sy >= h) sy = h - 1;
                    srcYToOut[sy] = oy;
                }

                #pragma omp parallel for schedule(dynamic, 16)
                for (int oy = 0; oy < outH; ++oy) {
                    int sy = static_cast<int>((static_cast<int64_t>(oy) * h) / outH);
                    if (sy >= h) sy = h - 1;

                    float* rowDst = reinterpret_cast<float*>(pixels + (size_t)oy * stride);
                    const float* rowSrc = floatPixels.data() + (size_t)sy * w * 4;

                    for (int ox = 0; ox < outW; ++ox) {
                        int sx = srcXForOut[ox];
                        rowDst[ox*4+0] = rowSrc[sx*4+0];
                        rowDst[ox*4+1] = rowSrc[sx*4+1];
                        rowDst[ox*4+2] = rowSrc[sx*4+2];
                        rowDst[ox*4+3] = rowSrc[sx*4+3];
                    }
                }

                result.pixels = pixels;
                result.width = outW;
                result.height = outH;
                result.stride = stride;
                result.metadata.Width = w;
                result.metadata.Height = h;
                result.format = PixelFormat::R32G32B32A32_FLOAT;
                result.success = true;
                result.metadata.LoaderName = L"TinyEXR";
                result.metadata.FormatDetails = L"TinyEXR";
                result.metadata.Width = w;
                result.metadata.Height = h;
                result.metadata.is_Linear_sRGB = true;
                result.metadata.hdrMetadata.isValid = true;
                result.metadata.hdrMetadata.isHdr = true;
                result.metadata.hdrMetadata.isSceneLinear = true;
                result.metadata.hdrMetadata.transfer = QuickView::TransferFunction::Linear;
                return S_OK;
            }
        }

        namespace PsdComposite {
            struct HeaderInfo {
                uint16_t version = 0;   // 1=PSD, 2=PSB
                uint16_t channels = 0;
                uint32_t width = 0;
                uint32_t height = 0;
                uint16_t depth = 0;     // 8/16 supported
                uint16_t colorMode = 0; // 1=Gray, 3=RGB supported
                uint16_t compression = 0; // 0=Raw, 1=RLE
                size_t imageDataOffset = 0;
            };

            static inline uint16_t ReadBE16(const uint8_t* p) {
                return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
            }

            static inline uint32_t ReadBE32(const uint8_t* p) {
                return (static_cast<uint32_t>(p[0]) << 24) |
                       (static_cast<uint32_t>(p[1]) << 16) |
                       (static_cast<uint32_t>(p[2]) << 8) |
                       static_cast<uint32_t>(p[3]);
            }

            static inline uint64_t ReadBE64(const uint8_t* p) {
                return (static_cast<uint64_t>(p[0]) << 56) |
                       (static_cast<uint64_t>(p[1]) << 48) |
                       (static_cast<uint64_t>(p[2]) << 40) |
                       (static_cast<uint64_t>(p[3]) << 32) |
                       (static_cast<uint64_t>(p[4]) << 24) |
                       (static_cast<uint64_t>(p[5]) << 16) |
                       (static_cast<uint64_t>(p[6]) << 8) |
                       static_cast<uint64_t>(p[7]);
            }

            static inline bool FitsRange(size_t offset, size_t need, size_t size) {
                return offset <= size && need <= (size - offset);
            }

            static bool ParseHeader(const uint8_t* data, size_t size, HeaderInfo& out) {
                if (!data || size < 26) return false;
                if (std::memcmp(data, "8BPS", 4) != 0) return false;

                out.version = ReadBE16(data + 4);
                if (out.version != 1 && out.version != 2) return false;

                out.channels = ReadBE16(data + 12);
                out.height = ReadBE32(data + 14);
                out.width = ReadBE32(data + 18);
                out.depth = ReadBE16(data + 22);
                out.colorMode = ReadBE16(data + 24);

                if (out.channels == 0 || out.width == 0 || out.height == 0) return false;

                size_t off = 26;

                if (!FitsRange(off, 4, size)) return false;
                const uint32_t colorModeLen = ReadBE32(data + off);
                off += 4;
                if (!FitsRange(off, colorModeLen, size)) return false;
                off += static_cast<size_t>(colorModeLen);

                if (!FitsRange(off, 4, size)) return false;
                const uint32_t imageResLen = ReadBE32(data + off);
                off += 4;
                if (!FitsRange(off, imageResLen, size)) return false;
                off += static_cast<size_t>(imageResLen);

                uint64_t layerMaskLen = 0;
                if (out.version == 1) {
                    if (!FitsRange(off, 4, size)) return false;
                    layerMaskLen = ReadBE32(data + off);
                    off += 4;
                } else {
                    if (!FitsRange(off, 8, size)) return false;
                    layerMaskLen = ReadBE64(data + off);
                    off += 8;
                }

                if (layerMaskLen > static_cast<uint64_t>(size - off)) return false;
                off += static_cast<size_t>(layerMaskLen);

                if (!FitsRange(off, 2, size)) return false;
                out.compression = ReadBE16(data + off);
                out.imageDataOffset = off;
                return true;
            }

            static bool IsPSB(const uint8_t* data, size_t size) {
                HeaderInfo header;
                return ParseHeader(data, size, header) && header.version == 2;
            }

            static bool DecodePackBitsRow(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t dstSize) {
                size_t si = 0;
                size_t di = 0;

                while (si < srcSize && di < dstSize) {
                    const int8_t n = static_cast<int8_t>(src[si++]);
                    if (n >= 0) {
                        const size_t count = static_cast<size_t>(n) + 1;
                        if (count > srcSize - si || count > dstSize - di) return false;
                        std::memcpy(dst + di, src + si, count);
                        si += count;
                        di += count;
                    } else if (n >= -127) {
                        if (si >= srcSize) return false;
                        const uint8_t value = src[si++];
                        const size_t count = static_cast<size_t>(1 - n);
                        if (count > dstSize - di) return false;
                        std::memset(dst + di, value, count);
                        di += count;
                    } else {
                        // n == -128: no-op
                    }
                }

                return di == dstSize;
            }

            static inline uint8_t SampleTo8Bit(const uint8_t* row, uint32_t srcX, uint16_t depth) {
                if (depth == 16) {
                    return row[static_cast<size_t>(srcX) * 2]; // use high byte
                }
                return row[srcX];
            }

            static void WriteChannelToBgraRow(
                uint8_t* dstRow,
                const std::vector<uint32_t>& srcXForOut,
                const uint8_t* srcRow,
                uint16_t depth,
                uint16_t colorMode,
                uint16_t channelIndex)
            {
                const size_t outW = srcXForOut.size();
                if (colorMode == 3) {
                    for (size_t ox = 0; ox < outW; ++ox) {
                        const uint8_t v = SampleTo8Bit(srcRow, srcXForOut[ox], depth);
                        uint8_t* px = dstRow + ox * 4;
                        if (channelIndex == 0) px[2] = v;      // R
                        else if (channelIndex == 1) px[1] = v; // G
                        else if (channelIndex == 2) px[0] = v; // B
                        else if (channelIndex == 3) px[3] = v; // A
                    }
                } else if (colorMode == 1) {
                    for (size_t ox = 0; ox < outW; ++ox) {
                        const uint8_t v = SampleTo8Bit(srcRow, srcXForOut[ox], depth);
                        uint8_t* px = dstRow + ox * 4;
                        if (channelIndex == 0) {
                            px[0] = v;
                            px[1] = v;
                            px[2] = v;
                        } else if (channelIndex == 1) {
                            px[3] = v; // gray alpha if present
                        }
                    }
                }
            }

            static HRESULT Load(const uint8_t* data, size_t size, const DecodeContext& ctx, DecodeResult& result) {
                HeaderInfo header;
                if (!ParseHeader(data, size, header)) return E_FAIL;

                const bool supportedColorMode = (header.colorMode == 3 || header.colorMode == 1);
                const bool supportedDepth = (header.depth == 8 || header.depth == 16);
                const bool supportedCompression = (header.compression == 0 || header.compression == 1);
                if (!supportedColorMode || !supportedDepth || !supportedCompression) return E_NOTIMPL;

                if (header.colorMode == 3 && header.channels < 3) return E_FAIL;
                if (header.colorMode == 1 && header.channels < 1) return E_FAIL;

                uint32_t outW = header.width;
                uint32_t outH = header.height;
                if (ctx.targetWidth > 0 || ctx.targetHeight > 0) {
                    const double tw = (ctx.targetWidth > 0) ? static_cast<double>(ctx.targetWidth) : static_cast<double>(header.width);
                    const double th = (ctx.targetHeight > 0) ? static_cast<double>(ctx.targetHeight) : static_cast<double>(header.height);
                    double scale = (std::min)(tw / static_cast<double>(header.width), th / static_cast<double>(header.height));
                    if (scale > 1.0) scale = 1.0;
                    if (scale > 0.0 && scale < 1.0) {
                        outW = (std::max)(1u, static_cast<uint32_t>(header.width * scale + 0.5));
                        outH = (std::max)(1u, static_cast<uint32_t>(header.height * scale + 0.5));
                    }
                }

                const int stride = CalculateSIMDAlignedStride(static_cast<int>(outW), 4);
                if (stride <= 0) return E_FAIL;

                const size_t totalSize = static_cast<size_t>(stride) * static_cast<size_t>(outH);
                uint8_t* pixels = ctx.allocator(totalSize);
                if (!pixels) return E_OUTOFMEMORY;

                std::memset(pixels, 0, totalSize);
                for (uint32_t y = 0; y < outH; ++y) {
                    uint8_t* row = pixels + static_cast<size_t>(y) * stride;
                    for (uint32_t x = 0; x < outW; ++x) {
                        row[static_cast<size_t>(x) * 4 + 3] = 255;
                    }
                }

                std::vector<uint32_t> srcXForOut(outW);
                for (uint32_t ox = 0; ox < outW; ++ox) {
                    uint32_t sx = static_cast<uint32_t>((static_cast<uint64_t>(ox) * header.width) / outW);
                    if (sx >= header.width) sx = header.width - 1;
                    srcXForOut[ox] = sx;
                }

                std::vector<int32_t> srcYToOut(header.height, -1);
                for (uint32_t oy = 0; oy < outH; ++oy) {
                    uint32_t sy = static_cast<uint32_t>((static_cast<uint64_t>(oy) * header.height) / outH);
                    if (sy >= header.height) sy = header.height - 1;
                    srcYToOut[sy] = static_cast<int32_t>(oy);
                }

                const size_t rowBytes = static_cast<size_t>(header.width) * ((header.depth == 16) ? 2u : 1u);
                const size_t compressionOffset = header.imageDataOffset + 2;

                const auto isRelevantChannel = [&](uint16_t c) -> bool {
                    if (header.colorMode == 3) return c < 4; // RGB + optional alpha
                    if (header.colorMode == 1) return c < 2; // Gray + optional alpha
                    return false;
                };

                if (header.compression == 0) {
                    if (!FitsRange(compressionOffset, 0, size)) return E_FAIL;
                    const uint8_t* ptr = data + compressionOffset;
                    const uint8_t* end = data + size;

                    for (uint16_t c = 0; c < header.channels; ++c) {
                        for (uint32_t sy = 0; sy < header.height; ++sy) {
                            if (ctx.checkCancel && (sy % 128 == 0) && ctx.checkCancel()) return E_ABORT;
                            if (ptr > end || rowBytes > static_cast<size_t>(end - ptr)) return E_FAIL;

                            const int32_t oy = srcYToOut[sy];
                            if (oy >= 0 && isRelevantChannel(c)) {
                                uint8_t* dstRow = pixels + static_cast<size_t>(oy) * stride;
                                WriteChannelToBgraRow(dstRow, srcXForOut, ptr, header.depth, header.colorMode, c);
                            }
                            ptr += rowBytes;
                        }
                    }
                } else {
                    const uint64_t numRows = static_cast<uint64_t>(header.channels) * static_cast<uint64_t>(header.height);
                    const size_t rleLenField = (header.version == 2) ? 4u : 2u;
                    if (numRows > (std::numeric_limits<size_t>::max() / rleLenField)) return E_FAIL;
                    const size_t rleTableBytes = static_cast<size_t>(numRows) * rleLenField;
                    if (!FitsRange(compressionOffset, rleTableBytes, size)) return E_FAIL;

                    const uint8_t* rleLenTable = data + compressionOffset;
                    const uint8_t* compPtr = rleLenTable + rleTableBytes;
                    const uint8_t* end = data + size;
                    std::vector<uint8_t> decodedRow(rowBytes);

                    for (uint16_t c = 0; c < header.channels; ++c) {
                        for (uint32_t sy = 0; sy < header.height; ++sy) {
                            if (ctx.checkCancel && (sy % 128 == 0) && ctx.checkCancel()) return E_ABORT;

                            const uint64_t idx = static_cast<uint64_t>(c) * header.height + sy;
                            const uint8_t* lenPtr = rleLenTable + static_cast<size_t>(idx) * rleLenField;
                            const uint32_t packedLen = (rleLenField == 2) ? ReadBE16(lenPtr) : ReadBE32(lenPtr);

                            if (compPtr > end || packedLen > static_cast<uint32_t>(end - compPtr)) return E_FAIL;

                            const int32_t oy = srcYToOut[sy];
                            if (oy >= 0 && isRelevantChannel(c)) {
                                if (!DecodePackBitsRow(compPtr, packedLen, decodedRow.data(), rowBytes)) return E_FAIL;
                                uint8_t* dstRow = pixels + static_cast<size_t>(oy) * stride;
                                WriteChannelToBgraRow(dstRow, srcXForOut, decodedRow.data(), header.depth, header.colorMode, c);
                            }

                            compPtr += packedLen;
                        }
                    }
                }

                result.pixels = pixels;
                result.width = static_cast<int>(outW);
                result.height = static_cast<int>(outH);
                result.stride = stride;
                result.format = PixelFormat::BGRA8888;
                result.success = true;
                result.metadata.Format = (header.version == 2) ? L"PSB" : L"PSD";
                result.metadata.LoaderName = (header.version == 2) ? L"PSB Composite" : L"PSD Composite";
                result.metadata.Width = header.width;
                result.metadata.Height = header.height;

                std::wstring details = (header.version == 2) ? L"PSB v2 Composite" : L"PSD Composite";
                details += (header.compression == 0) ? L" Raw" : L" RLE";
                if (header.depth == 16) details += L" 16bpc";
                if (outW != header.width || outH != header.height) details += L" [Scaled]";
                result.metadata.FormatDetails = details;
                return S_OK;
            }
        }



        namespace Stb {
            static HRESULT Load(const uint8_t* data, size_t size, const DecodeContext& ctx, DecodeResult& result) {
                int w = 0, h = 0, c = 0;
                std::pmr::vector<uint8_t> out(std::pmr::get_default_resource());
                // STB uses malloc, we use pmr wrapper
                if (StbLoader::LoadImageFromMemory(data, size, &w, &h, &c, out, false)) {
                     // Check if RGB or RGBA? StbLoader wrapper usually forces RGBA (4 channels) if possible or returns raw?
                     // Verify StbLoader implementation?
                     // It says "LoadImage" - usually wrapping stbi_load_from_memory forcing 4 components?
                     // Assuming RGBA output from StbLoader wrapper.
                     
                     int stride = CalculateSIMDAlignedStride(w, 4);
                     size_t totalSize = (size_t)stride * h;
                     uint8_t* pixels = ctx.allocator(totalSize);
                     if (!pixels) return E_OUTOFMEMORY;
                     
                     // [v9.9] Copy and Convert RGBA to BGRA with OpenMP parallelization
                     uint8_t* src = out.data();
                     
                     #pragma omp parallel for schedule(dynamic, 32)
                     for(int y=0; y<h; y++) {
                         uint32_t* rowDst = (uint32_t*)(pixels + (size_t)y * stride);
                         uint8_t* rowSrc = src + (size_t)y * w * 4; // Assuming 4 channels packed
                         
                         for(int x=0; x<w; x++) {
                             uint8_t r = rowSrc[x*4+0]; // R
                             uint8_t g = rowSrc[x*4+1]; // G
                             uint8_t b = rowSrc[x*4+2]; // B
                             uint8_t a = rowSrc[x*4+3]; // A
                             
                             // BGRA Output
                             rowDst[x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
                         }
                     }
                     
                    result.pixels = pixels;
                    result.width = w;
                    result.height = h;
                    result.stride = stride;
                    result.format = PixelFormat::BGRA8888;
                    result.success = true;
                    result.metadata.LoaderName = L"StbImage";
                    result.metadata.FormatDetails = L"StbImage";
                    result.metadata.Width = w;
                    result.metadata.Height = h;
                    return S_OK;
                }
                return E_FAIL;
            }

            static HRESULT LoadHdr(const uint8_t* data, size_t size, const DecodeContext& ctx, DecodeResult& result) {
                int w = 0, h = 0, c = 0;
                std::pmr::vector<uint8_t> out(std::pmr::get_default_resource());
                if (!StbLoader::LoadImageFromMemory(data, size, &w, &h, &c, out, true)) {
                    return E_FAIL;
                }

                const int bytesPerPixel = 16;
                const int stride = CalculateSIMDAlignedStride(w, bytesPerPixel);
                const size_t totalSize = static_cast<size_t>(stride) * h;
                uint8_t* pixels = ctx.allocator(totalSize);
                if (!pixels) return E_OUTOFMEMORY;

                for (int y = 0; y < h; ++y) {
                    float* dstRow = reinterpret_cast<float*>(pixels + static_cast<size_t>(y) * stride);
                    const float* srcRow = reinterpret_cast<const float*>(out.data() + static_cast<size_t>(y) * w * bytesPerPixel);
                    memcpy(dstRow, srcRow, static_cast<size_t>(w) * bytesPerPixel);
                }

                result.pixels = pixels;
                result.width = w;
                result.height = h;
                result.stride = stride;
                result.format = PixelFormat::R32G32B32A32_FLOAT;
                result.success = true;
                result.metadata.LoaderName = L"StbImage (HDR)";
                result.metadata.FormatDetails = L"Radiance HDR Float";
                result.metadata.Width = w;
                result.metadata.Height = h;
                result.metadata.is_Linear_sRGB = true;
                result.metadata.hdrMetadata.isValid = true;
                result.metadata.hdrMetadata.isHdr = true;
                result.metadata.hdrMetadata.isSceneLinear = true;
                result.metadata.hdrMetadata.transfer = QuickView::TransferFunction::Linear;
                return S_OK;
            }
        }

    } // namespace Codec
} // namespace QuickView

using namespace QuickView::Codec;

// ============================================================================
// [v4.2] Unified Image Dispatcher
// ============================================================================
HRESULT CImageLoader::LoadImageUnified(LPCWSTR filePath, const DecodeContext& ctx, DecodeResult& result) {
    if (!filePath) return E_INVALIDARG;

    // [v9.2] Use path-based detection directly (handles RAW extensions properly)
    std::wstring fmt = DetectFormatFromContent(filePath);
    
    // [Fix] Ensure Format is stored in metadata for extension mismatch detection (main.cpp)
    result.metadata.Format = fmt;

    // 3. Dispatch
    
    // --- Stream/File Based Codecs ---
    // [v9.1 Fix] Accept both "Raw" and "RAW" (case insensitive)
    if (fmt == L"Raw" || fmt == L"RAW" || fmt == L"Unknown" || fmt == L"DDS") {
        if (fmt == L"Raw" || fmt == L"RAW") {
             HRESULT hr = RawCodec::Load(filePath, ctx, result);
             if (SUCCEEDED(hr)) { result.metadata.LoaderName = L"LibRaw (Unified)"; return S_OK; }
        }
        // If Unknown or DDS, falls through to WIC/Fallback (E_NOTIMPL -> WIC).
    }

    // --- Buffer Based Codecs ---
    // [v6.7] Expanded: Include ALL integrated specialized formats
    bool isBufferCodec = (
        fmt == L"JPEG" || fmt == L"WebP" || fmt == L"PNG" || fmt == L"GIF" || fmt == L"BMP" || fmt == L"JXL" ||
        fmt == L"AVIF" || fmt == L"HEIC" ||
        fmt == L"QOI" || fmt == L"TGA" || fmt == L"PNM" || fmt == L"WBMP" ||
        fmt == L"PSD" || fmt == L"HDR" || fmt == L"PIC" || fmt == L"PCX" ||
        fmt == L"SVG" || fmt == L"EXR"
    );
    
    if (isBufferCodec) {
        QuickView::MappedFile mapping(filePath);
        if (!mapping.IsValid()) return E_FAIL;
        
        const uint8_t* mappedData = mapping.data();
        size_t mappedSize = mapping.size();
        
        // Dispatch
        if (fmt == L"AVIF" || fmt == L"HEIC") {
            HRESULT hr = AVIF::Load(mappedData, mappedSize, ctx, result);
            if (SUCCEEDED(hr)) {
                if (fmt == L"HEIC" && result.metadata.LoaderName == L"libavif (Unified)") {
                    result.metadata.LoaderName = L"libavif/HEIF (Unified)";
                } else if (fmt == L"HEIC" && result.metadata.LoaderName == L"libavif (Unified HDR)") {
                    result.metadata.LoaderName = L"libavif/HEIF (Unified HDR)";
                }
                return S_OK;
            }
        }
        else if (fmt == L"JXL") {
            // [JXL DC] Only use DC Preview if scaling is requested (Stage 1)
             // or if explicitly forcing preview.
             // [Fix] User Requirement: 提取缩略图逻辑仅在 titan 模式下进行，普通模式下不进行缩略图逻辑
             if (ctx.isTitanMode && (ctx.targetWidth > 0 || ctx.targetHeight > 0 || ctx.forceRenderFull)) {
                 CImageLoader::ThumbData tmp;
                 HRESULT hr = CImageLoader::LoadThumbJXL_DC(mappedData, mappedSize, &tmp, &result.metadata, ctx.forceRenderFull, ctx.allowFakeBase);
                 
                 // [v8.4] Critical Fix: Respect E_ABORT from FastLane
                 // If JXL loader explicitly aborts (e.g. Large Modular Image in FastLane), 
                 // we MUST NOT fall through to full decode on this thread.
                 if (hr == E_ABORT) {
                     // FastLane requests 256x256. HeavyLane Base Layer requests screen size (e.g. 1920x1080).
                     // If it's FastLane, abort. If it's HeavyLane, fall through to Full Decode.
                     if (ctx.targetWidth < 1000 && ctx.targetHeight < 1000) {
                         return E_ABORT;
                     }
                     // Else fall through
                 }
                 else if (SUCCEEDED(hr) && tmp.isValid) {
                     result.width = tmp.width;
                     result.height = tmp.height;
                     result.stride = tmp.stride;
                     size_t bufSize = (size_t)tmp.stride * tmp.height;
                     result.pixels = ctx.allocator(bufSize);
                     if (result.pixels) {
                         memcpy(result.pixels, tmp.pixels.data(), bufSize);
                         result.metadata.LoaderName = tmp.loaderName; 
                         return S_OK;
                     }
                     // If alloc failed, fall through to full decode? No, E_OUTOFMEMORY.
                     return E_OUTOFMEMORY;
                 }
             }
             // Fall through to JXL::Load (Full) if DC failed or not requested
        }
        else if (fmt == L"WebP") {
             // WebP Loader already accepts DecodeResult directly
             HRESULT hr = QuickView::Codec::WebP::Load(mappedData, mappedSize, ctx, result);
             if (SUCCEEDED(hr)) {
                 result.metadata.LoaderName = L"WebP (Unified)";
                 return S_OK;
             }
        }
        else if (fmt == L"JPEG") {
            HRESULT hr = JPEG::Load(mappedData, mappedSize, ctx, result);
            if (SUCCEEDED(hr)) { result.metadata.LoaderName = L"TurboJPEG (Unified)"; return S_OK; }
        }
        else if (fmt == L"PNG") {
            HRESULT hr = Wuffs::LoadPNG(mappedData, mappedSize, ctx, result);
            if (SUCCEEDED(hr)) { result.metadata.LoaderName = L"Wuffs PNG (Unified)"; return S_OK; }
        }
        else if (fmt == L"GIF") {
            HRESULT hr = Wuffs::LoadGIF(mappedData, mappedSize, ctx, result);
            if (SUCCEEDED(hr)) { result.metadata.LoaderName = L"Wuffs GIF (Unified)"; return S_OK; }
        }
        else if (fmt == L"BMP") {
            HRESULT hr = Wuffs::LoadBMP(mappedData, mappedSize, ctx, result);
            if (SUCCEEDED(hr)) { result.metadata.LoaderName = L"Wuffs BMP (Unified)"; return S_OK; }
        }
        else if (fmt == L"QOI") {
            HRESULT hr = Wuffs::LoadQOI(mappedData, mappedSize, ctx, result);
            if (SUCCEEDED(hr)) { result.metadata.LoaderName = L"Wuffs QOI (Unified)"; return S_OK; }
        }
        else if (fmt == L"TGA") {
            HRESULT hr = Wuffs::LoadTGA(mappedData, mappedSize, ctx, result);
            if (SUCCEEDED(hr)) { 
                result.metadata.LoaderName = L"Wuffs TGA (Unified)"; 
                return S_OK; 
            }
            // [v6.9.7] Fallback: If Wuffs fails (Strict), try Stb (Robust)
            // This ensures FastLane doesn't fail silently for valid TGA files.
            hr = Stb::Load(mappedData, mappedSize, ctx, result);
            if (SUCCEEDED(hr)) {
                // Stb sets LoaderName to "StbImage"
                return S_OK;
            }
        }
        else if (fmt == L"PNM") {
            HRESULT hr = Wuffs::LoadNetPBM(mappedData, mappedSize, ctx, result);
            if (SUCCEEDED(hr)) { result.metadata.LoaderName = L"Wuffs NetPBM (Unified)"; return S_OK; }
        }
        else if (fmt == L"WBMP") {
            HRESULT hr = Wuffs::LoadWBMP(mappedData, mappedSize, ctx, result);
            if (SUCCEEDED(hr)) { result.metadata.LoaderName = L"Wuffs WBMP (Unified)"; return S_OK; }
        }
        /* NanoSVG Removed */
        else if (fmt == L"EXR") {
             HRESULT hr = TinyEXR::Load(mappedData, mappedSize, ctx, result);
             if (SUCCEEDED(hr)) { 
                 return S_OK; 
             }
        }
        else if (fmt == L"PSD") {
             // Prefer native composite decode for PSB (version 2).
             const bool isPsb = PsdComposite::IsPSB(mappedData, mappedSize);
             if (isPsb) {
                 HRESULT hr = PsdComposite::Load(mappedData, mappedSize, ctx, result);
                 if (hr == E_ABORT || hr == E_OUTOFMEMORY) return hr;
                 if (SUCCEEDED(hr)) return S_OK;
             } else {
                 HRESULT hr = Stb::Load(mappedData, mappedSize, ctx, result);
                 if (SUCCEEDED(hr)) {
                     // Stb::Load sets LoaderName to StbImage
                     return S_OK;
                 }
             }

             // Fallback to embedded JPEG preview when full decode is unavailable.
             PreviewExtractor::ExtractedData exData;
             if (PreviewExtractor::ExtractFromPSD(mappedData, mappedSize, exData) && exData.IsValid()) {
                 HRESULT previewHr = JPEG::Load(exData.pData, exData.size, ctx, result);
                 if (SUCCEEDED(previewHr)) {
                     result.metadata.Format = L"PSD";
                     result.metadata.LoaderName = L"PSD Preview (Embedded JPEG)";
                     if (result.metadata.FormatDetails.empty()) {
                         result.metadata.FormatDetails = L"PSD Preview";
                     } else {
                         result.metadata.FormatDetails += L" / PSD Preview";
                     }
                     return S_OK;
                 }
             }
        }
        else if (fmt == L"HDR") {
             HRESULT hr = Stb::LoadHdr(mappedData, mappedSize, ctx, result);
             if (SUCCEEDED(hr)) {
                 return S_OK;
             }
        }
        else if (fmt == L"PIC" || fmt == L"PCX") {
             HRESULT hr = Stb::Load(mappedData, mappedSize, ctx, result);
             if (SUCCEEDED(hr)) { 
                 return S_OK; 
             }
        }
        if (fmt == L"JXL") {
            HRESULT hr = JXL::Load(mappedData, mappedSize, ctx, result);
            if (hr == E_OUTOFMEMORY || hr == E_ABORT) return hr;
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
    
    if (hr == E_ABORT || hr == E_OUTOFMEMORY) return hr;

    // If LoadImageUnified succeeded, we're done.
    if (SUCCEEDED(hr) && res.success) {
        // pOutput->pixels is already populated by ctx.allocator callback
        pOutput->width = res.width;
        pOutput->height = res.height;
        pOutput->stride = res.stride;
        // pOutput->format = res.format; // DecodedImage uses implicit BGRA8888
        // [v5.4] Metadata
        pOutput->FormatDetails = res.metadata.FormatDetails;
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
    
    WICPixelFormatGUID srcWicFmt;
    wicBitmap->GetPixelFormat(&srcWicFmt);
    bool isFloat = IsEqualGUID(srcWicFmt, GUID_WICPixelFormat128bppRGBAFloat);
    int bpp = isFloat ? 16 : 4;

    UINT stride = w * bpp;
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
                           pData + (size_t)y * cbStride, w * bpp);
                }
            }
            pOutput->isValid = true;
            return S_OK;
        }
    }
    return E_FAIL;
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
             int w = tj3Get(tj, TJPARAM_JPEGWIDTH);
             int h = tj3Get(tj, TJPARAM_JPEGHEIGHT);
             if (w > 0 && h > 0) {
                 pInfo->width = w;
                 pInfo->height = h;
                 int subsamp = tj3Get(tj, TJPARAM_SUBSAMP);
                 pInfo->channels = (subsamp == TJSAMP_GRAY) ? 1 : 3;
                 pInfo->bitDepth = 8;
                 tj3Destroy(tj);
                 return S_OK;
             }
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

            // Try robust file-based box parsing first. It handles cases where 64KB head is insufficient.
            uint32_t boxW = 0, boxH = 0;
            if (isAvif && GetAVIFDimensions(filePath, &boxW, &boxH)) {
                pInfo->width = boxW;
                pInfo->height = boxH;
                pInfo->bitDepth = 10; // Conservative default; exact value may require full decode.
                return S_OK;
            }
            if (GetISOBMFFDimensions(filePath, &boxW, &boxH)) {
                pInfo->width = boxW;
                pInfo->height = boxH;
                pInfo->bitDepth = 10;
                return S_OK;
            }

            // [v6.9] Try Parsing AVIF/HEIC Header
            // We give 64KB (caller usually reads 64KB). Often enough for mp4/avif header (moov).
            avifDecoder* decoder = avifDecoderCreate();
            if (decoder) {
                if (avifDecoderSetIOMemory(decoder, data, size) == AVIF_RESULT_OK) {
                    if (avifDecoderParse(decoder) == AVIF_RESULT_OK) {
                        pInfo->width = decoder->image->width;
                        pInfo->height = decoder->image->height;
                        pInfo->bitDepth = decoder->image->depth;
                        pInfo->hasAlpha = decoder->image->alphaPlane != nullptr; // or just assume?
                        // Actually decoder->image might not be fully populated until NextImage?
                        // avifDecoderParse parses the sequence header. It populates width/height.
                        // For alpha, we check if alphaPresent?
                        pInfo->hasAlpha = (decoder->alphaPresent == AVIF_TRUE);
                        pInfo->isAnimated = (decoder->imageCount > 1);
                        
                        avifDecoderDestroy(decoder);
                        return S_OK;
                    }
                }
                avifDecoderDestroy(decoder);
            }
            // Keep format info even if dimensions are unresolved. Caller can still route to Titan via
            // file-size fallback and/or slow GetImageSize probe.
            return S_OK; 
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
    
    // --- [v6.9.6] TGA (Heuristic) ---
    // Offset 1: ColorMapType (0/1). Offset 2: ImageType.
    if (size >= 18 && (data[1] <= 1) && (data[2]==1 || data[2]==2 || data[2]==3 || data[2]==9 || data[2]==10 || data[2]==11)) {
        // TGA Width at 12, Height at 14 (Little Endian)
        uint16_t w = data[12] | (data[13] << 8);
        uint16_t h = data[14] | (data[15] << 8);
        uint8_t depth = data[16]; // Offset 16
        
        if (w > 0 && h > 0) {
            pInfo->format = L"TGA";
            pInfo->width = w;
            pInfo->height = h;
            pInfo->bitDepth = depth;
            return S_OK;
        }
    }

    // --- [v6.9.6] WBMP ---
    // Header: Type(0), Fixed(0), Width(MBI), Height(MBI)
    if (size >= 4 && data[0] == 0x00 && data[1] == 0x00) {
        size_t off = 2;
        auto ReadMBI = [&](uint32_t& outVal) -> bool {
            outVal = 0;
            while (off < size) {
                uint8_t byte = data[off++];
                outVal = (outVal << 7) | (byte & 0x7F);
                if ((byte & 0x80) == 0) return true;
                if (off >= size) return false;
            }
            return false;
        };
        
        uint32_t w = 0, h = 0;
        if (ReadMBI(w) && ReadMBI(h)) {
            pInfo->format = L"WBMP";
            pInfo->width = w;
            pInfo->height = h;
            pInfo->bitDepth = 1;
            return S_OK;
        }
    }

    // --- [v7.0] PNM/PAM (Netpbm) ---
    // P1-P7 magic bytes. Header is ASCII (except P7 logic).
    // Parsing PNM header is tricky due to whitespace/comments, but usually simple enough for 'Fast' check.
    if (size >= 2 && data[0] == 'P' && data[1] >= '1' && data[1] <= '7') {
         pInfo->format = L"PNM";
         // Simple parser: skip magic, skip whitespace/comments, read width, read height
         size_t i = 2;
         auto skip = [&]() {
             while (i < size) {
                 if (isspace(data[i])) { i++; continue; }
                 if (data[i] == '#') { 
                     while (i < size && data[i] != '\n' && data[i] != '\r') i++; 
                     continue; 
                 }
                 break;
             }
         };
         auto readInt = [&]() -> int {
             skip();
             int val = 0;
             bool found = false;
             while (i < size && isdigit(data[i])) {
                 val = val * 10 + (data[i] - '0');
                 i++;
                 found = true;
             }
             return found ? val : -1;
         };
         
         // P7 (PAM) has diff header: "WIDTH 123\nHEIGHT 456\n..."
         if (data[1] == '7') {
             // Robust Parsing for PAM Header
             // Use simple buffer scan to avoid stream overhead issues
             const char* p = (const char*)data + 2; // Skip P7
             const char* end = (const char*)data + std::min(size, (size_t)4096); // Check first 4KB
             
             auto nextToken = [&](std::string& outToken) -> bool {
                 outToken.clear();
                 // Skip non-graphical
                 while (p < end && (unsigned char)*p <= 32) p++;
                 // Skip comments
                 while (p < end && *p == '#') {
                     while (p < end && *p != '\n' && *p != '\r') p++;
                     while (p < end && (unsigned char)*p <= 32) p++;
                 }
                 if (p >= end) return false;
                 
                 // Read token
                 while (p < end && (unsigned char)*p > 32) {
                     outToken += *p++;
                 }
                 return !outToken.empty();
             };
             
             int w = 0, h = 0;
             std::string tok;
             while (nextToken(tok)) {
                 if (tok == "WIDTH") {
                     std::string val; 
                     if (nextToken(val)) w = std::atoi(val.c_str());
                 }
                 else if (tok == "HEIGHT") {
                     std::string val;
                     if (nextToken(val)) h = std::atoi(val.c_str());
                 }
                 else if (tok == "ENDHDR") {
                     break;
                 }
             }
             
             if (w > 0 && h > 0) {
                 pInfo->width = w; pInfo->height = h;
                 return S_OK;
             }
         } else {
             // P1-P6: width height maxval(for some)
             int w = readInt();
             int h = readInt();
            if (w > 0 && h > 0) {
                 pInfo->width = w; pInfo->height = h;
                 return S_OK;
             }
         }
         // If parsing fails, E_FAIL (will fall back to slow load if configured, or fail)
    }

    // --- [v9.9] QOI (Quite OK Image) ---
    // Magic: "qoif", W at 4-7 (BE), H at 8-11 (BE)
    if (size >= 14 && data[0] == 'q' && data[1] == 'o' && data[2] == 'i' && data[3] == 'f') {
        pInfo->format = L"QOI";
        pInfo->width = ((uint32_t)data[4] << 24) | ((uint32_t)data[5] << 16) | 
                       ((uint32_t)data[6] << 8) | data[7];
        pInfo->height = ((uint32_t)data[8] << 24) | ((uint32_t)data[9] << 16) | 
                        ((uint32_t)data[10] << 8) | data[11];
        pInfo->channels = data[12]; // 3=RGB, 4=RGBA
        pInfo->bitDepth = 8;
        return S_OK;
    }

    // --- [v9.9] PSD/PSB (Photoshop) ---
    // Signature: "8BPS", Version: 1=PSD, 2=PSB
    if (size >= 26 && data[0] == '8' && data[1] == 'B' && data[2] == 'P' && data[3] == 'S') {
        const uint16_t version = (static_cast<uint16_t>(data[4]) << 8) | data[5];
        if (version == 1 || version == 2) {
            // Keep format as PSD for existing dispatch thresholds.
            pInfo->format = L"PSD";
            pInfo->channels = (static_cast<uint16_t>(data[12]) << 8) | data[13];
            pInfo->height = ((uint32_t)data[14] << 24) | ((uint32_t)data[15] << 16) |
                            ((uint32_t)data[16] << 8) | data[17];
            pInfo->width = ((uint32_t)data[18] << 24) | ((uint32_t)data[19] << 16) |
                           ((uint32_t)data[20] << 8) | data[21];
            pInfo->bitDepth = (static_cast<uint16_t>(data[22]) << 8) | data[23];
            pInfo->hasAlpha = (pInfo->channels >= 4);
            return S_OK;
        }
    }

    // --- [v9.9] HDR (Radiance RGBE) ---
    // Magic: "#?RADIANCE" or "#?RGBE", dimensions in ASCII header "-Y H +X W"
    if (size >= 11 && data[0] == '#' && data[1] == '?') {
        pInfo->format = L"HDR";
        // Search for resolution string in first 4KB
        std::string header((const char*)data, std::min(size, (size_t)4096));
        size_t resPos = header.find("-Y ");
        if (resPos != std::string::npos) {
            int h = 0, w = 0;
            if (sscanf_s(header.c_str() + resPos, "-Y %d +X %d", &h, &w) == 2 ||
                sscanf_s(header.c_str() + resPos, "-Y %d -X %d", &h, &w) == 2) {
                pInfo->height = h;
                pInfo->width = w;
                pInfo->bitDepth = 32; // RGBE is 32-bit float equivalent
                return S_OK;
            }
        }
        // Alternate format: "+Y H +X W"
        resPos = header.find("+Y ");
        if (resPos != std::string::npos) {
            int h = 0, w = 0;
            if (sscanf_s(header.c_str() + resPos, "+Y %d +X %d", &h, &w) == 2) {
                pInfo->height = h;
                pInfo->width = w;
                pInfo->bitDepth = 32;
                return S_OK;
            }
        }
        // Parsing failed, return format only
        return S_OK;
    }

    // --- [v9.9] ICO (Icon) ---
    // Magic: 00 00 01 00, ICONDIRENTRY at offset 6, use largest entry
    if (size >= 22 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01 && data[3] == 0x00) {
        pInfo->format = L"ICO";
        uint16_t count = data[4] | ((uint16_t)data[5] << 8);
        int maxW = 0, maxH = 0;
        for (uint16_t i = 0; i < count && (6 + i * 16 + 4) <= size; ++i) {
            size_t off = 6 + i * 16;
            int w = data[off];     // 0 means 256
            int h = data[off + 1]; // 0 means 256
            if (w == 0) w = 256;
            if (h == 0) h = 256;
            if (w > maxW) { maxW = w; maxH = h; }
        }
        pInfo->width = maxW;
        pInfo->height = maxH;
        return S_OK;
    }

    // --- RAW ---
    // Fast Pass skips LibRaw (too slow ~10ms).
    // Let WIC or Full Loader handle it.
    
    // --- [v9.9] TIFF Dimension Extraction (Fast IFD Parse) ---
    // TIFF magic: II (49 49 2A 00) or MM (4D 4D 00 2A)
    bool isTiffLE = (size >= 8 && data[0] == 0x49 && data[1] == 0x49 && data[2] == 0x2A && data[3] == 0x00);
    bool isTiffBE = (size >= 8 && data[0] == 0x4D && data[1] == 0x4D && data[2] == 0x00 && data[3] == 0x2A);
    
    if (isTiffLE || isTiffBE) {
        pInfo->format = L"TIFF";
        
        auto read16 = [&](size_t off) -> uint16_t {
            if (off + 2 > size) return 0;
            if (isTiffLE) return data[off] | ((uint16_t)data[off+1] << 8);
            else return ((uint16_t)data[off] << 8) | data[off+1];
        };
        
        auto read32 = [&](size_t off) -> uint32_t {
            if (off + 4 > size) return 0;
            if (isTiffLE) return data[off] | ((uint32_t)data[off+1] << 8) | 
                                 ((uint32_t)data[off+2] << 16) | ((uint32_t)data[off+3] << 24);
            else return ((uint32_t)data[off] << 24) | ((uint32_t)data[off+1] << 16) | 
                        ((uint32_t)data[off+2] << 8) | data[off+3];
        };
        
        // Read IFD0 offset (at byte 4-7)
        uint32_t ifdOffset = read32(4);
        if (ifdOffset > 0 && ifdOffset + 2 < size) {
            uint16_t numEntries = read16(ifdOffset);
            
            int w = 0, h = 0;
            for (uint16_t i = 0; i < numEntries && (ifdOffset + 2 + i * 12 + 12) <= size; ++i) {
                size_t entryOff = ifdOffset + 2 + i * 12;
                uint16_t tag = read16(entryOff);
                uint16_t type = read16(entryOff + 2);
                uint32_t count = read32(entryOff + 4);
                
                // Value or offset: for small values (1-4 bytes), value is inline at entryOff+8
                uint32_t val = 0;
                if (type == 3 && count == 1) val = read16(entryOff + 8); // SHORT
                else if (type == 4 && count == 1) val = read32(entryOff + 8); // LONG
                
                if (tag == 256) w = (int)val; // ImageWidth
                if (tag == 257) h = (int)val; // ImageHeight
                
                if (w > 0 && h > 0) break;
            }
            
            if (w > 0 && h > 0) {
                pInfo->width = w;
                pInfo->height = h;
                return S_OK;
            }
        }
        // [Fix] If parsing fails (e.g. IFD is past 64KB), return S_OK with format set 
        // to prevent caller from rejecting the entire file. WIC/HeavyLane will figure out dimensions later.
        return S_OK;
    }
    
    // --- [v9.9] EXR Dimension Extraction (Fast Header Parse) ---
    // EXR magic: 0x76 0x2F 0x31 0x01
    if (size >= 8 && data[0] == 0x76 && data[1] == 0x2F && data[2] == 0x31 && data[3] == 0x01) {
        pInfo->format = L"EXR";
        int w = 0, h = 0;
        if (TinyExrLoader::GetEXRDimensionsFromMemory(data, size, &w, &h)) {
            pInfo->width = w;
            pInfo->height = h;
            pInfo->bitDepth = 16; // EXR typically uses float16/32
            return S_OK;
        }
        // If header parse fails, return with format set but dimensions unknown
        return S_OK; // Will use 0x0, dispatch will treat as "Large"
    }

    // --- [Module B] SVG Detection & Dimensions ---
    bool isSvgExt = false;
    std::wstring pathStr = filePath;
    if (pathStr.length() > 4) {
        std::wstring ext = pathStr.substr(pathStr.length() - 4);
        if (_wcsicmp(ext.c_str(), L".svg") == 0) isSvgExt = true;
    }

    // Check content (XML/SVG) or extension
    if (isSvgExt || (size >= 5 && data[0] == '<' && (data[1] == '?' || data[1] == 's'))) {
        pInfo->format = L"SVG";
        
        // Default size if parsing fails
        pInfo->width = 512;
        pInfo->height = 512; 
        
        // Parse Dimensions (approximate from first 64KB)
        std::string xml(data, data + size);
        
        try {
            auto GetAttrVal = [](const std::string& s, const char* attr) -> std::string {
                std::string key = attr;
                key += "=\"";
                size_t pos = s.find(key);
                if (pos == std::string::npos) return "";
                pos += key.length();
                size_t end = s.find("\"", pos);
                if (end == std::string::npos) return "";
                return s.substr(pos, end - pos);
            };

            float pw = 0, ph = 0;
            std::string wStr = GetAttrVal(xml, "width");
            std::string hStr = GetAttrVal(xml, "height");
            if (!wStr.empty()) try { pw = std::stof(wStr); } catch (...) {}
            if (!hStr.empty()) try { ph = std::stof(hStr); } catch (...) {}

            if (pw > 0 && ph > 0) {
                pInfo->width = (int)std::lround(pw);
                pInfo->height = (int)std::lround(ph);
                return S_OK;
            } else {
                std::string vbStr = GetAttrVal(xml, "viewBox");
                if (!vbStr.empty()) {
                    size_t p = 0;
                    int skipped = 0;
                    while (skipped < 2 && p < vbStr.length()) {
                        if (isspace((unsigned char)vbStr[p]) || vbStr[p] == ',') {
                            while (p < vbStr.length() && (isspace((unsigned char)vbStr[p]) || vbStr[p] == ',')) p++;
                            skipped++;
                        } else p++;
                    }
                    if (p < vbStr.length()) {
                        char* endptr = nullptr;
                        float vw = strtof(vbStr.c_str() + p, &endptr);
                        if (endptr && endptr > vbStr.c_str() + p) {
                            float vh = strtof(endptr, nullptr);
                            if (vw > 0 && vh > 0) {
                                pInfo->width = (int)std::lround(vw);
                                pInfo->height = (int)std::lround(vh);
                            }
                        }
                    }
                }
            }
        } catch (...) {}
        return S_OK;
    }

    return E_FAIL;
}
HRESULT CImageLoader::GetImageSize(LPCWSTR filePath, UINT* width, UINT* height) {
    if (!filePath || !width || !height) return E_INVALIDARG;
    *width = 0; *height = 0;

    // [Phase 6] Use fast header parsing
    ImageInfo info;
    if (SUCCEEDED(GetImageInfoFast(filePath, &info))) {
        if (info.width > 0 && info.height > 0) {
            *width = info.width;
            *height = info.height;
            return S_OK;
        }
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
HRESULT CImageLoader::LoadStbImage(LPCWSTR filePath, IWICBitmap** ppBitmap, bool floatFormat, ImageMetadata* pMetadata) {
    // Read file to memory first (Solves Windows Unicode Path issues reliably)
    std::pmr::vector<uint8_t> buf(std::pmr::get_default_resource());
    if (!ReadFileToPMR(filePath, buf, {})) return E_FAIL;

    int width = 0, height = 0, channels = 0;
    std::pmr::vector<uint8_t> pixelData(std::pmr::get_default_resource());
    
    // Use Memory Loader
    if (!StbLoader::LoadImageFromMemory(buf.data(), buf.size(), &width, &height, &channels, pixelData, floatFormat)) {
        return E_FAIL;
    }

    if (pMetadata) {
        if (floatFormat) pMetadata->is_Linear_sRGB = true;
        else pMetadata->is_sRGB = true;

        if (floatFormat) {
            pMetadata->hdrMetadata.isValid = true;
            pMetadata->hdrMetadata.isHdr = true;
            pMetadata->hdrMetadata.isSceneLinear = true;
            pMetadata->hdrMetadata.transfer = QuickView::TransferFunction::Linear;
        }
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
// LoadSVG removed (NanoSVG cleanup)


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

template <bool IsSigned>
static double DecodeRationalT(unsigned __int64 val) {
    if constexpr (IsSigned) {
        int32_t num = (int32_t)(val & 0xFFFFFFFF);
        int32_t den = (int32_t)(val >> 32);
        if (den == 0) return 0.0;
        return (double)num / (double)den;
    } else {
        uint32_t num = (uint32_t)(val & 0xFFFFFFFF);
        uint32_t den = (uint32_t)(val >> 32);
        if (den == 0) return 0.0;
        return (double)num / (double)den;
    }
}

template <bool IsSigned>
static HRESULT GetMetadataRationalT(IWICMetadataQueryReader* reader, LPCWSTR query, double* out) {
    if (!reader || !out) return E_INVALIDARG;
    PROPVARIANT val; PropVariantInit(&val);
    HRESULT hr = reader->GetMetadataByName(query, &val);
    if (SUCCEEDED(hr)) {
        if (val.vt == VT_UI8) {
            *out = DecodeRationalT<IsSigned>(val.uhVal.QuadPart);
        } else if (val.vt == VT_I8) {
            *out = DecodeRationalT<IsSigned>((unsigned __int64)val.hVal.QuadPart);
        } else if (val.vt == VT_I4) {
            *out = (double)val.lVal;
        } else if (val.vt == VT_R8) {
            *out = val.dblVal;
        } else if constexpr (!IsSigned) {
            PropVariantToDouble(val, out);
        } else {
            hr = E_FAIL;
        }
        PropVariantClear(&val);
    }
    return hr;
}

static HRESULT GetMetadataRational(IWICMetadataQueryReader* reader, LPCWSTR query, double* out) {
    return GetMetadataRationalT<false>(reader, query, out);
}

static HRESULT GetMetadataSignedRational(IWICMetadataQueryReader* reader, LPCWSTR query, double* out) {
    return GetMetadataRationalT<true>(reader, query, out);
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
            double deg = DecodeRationalT<false>(varCoord.cauh.pElems[0].QuadPart);
            double min = DecodeRationalT<false>(varCoord.cauh.pElems[1].QuadPart);
            double sec = DecodeRationalT<false>(varCoord.cauh.pElems[2].QuadPart);
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
    
    // [v5.8] Dimensions
    if (RawProcessor.imgdata.sizes.width > 0 && RawProcessor.imgdata.sizes.height > 0) {
        pMetadata->Width = RawProcessor.imgdata.sizes.width;
        pMetadata->Height = RawProcessor.imgdata.sizes.height;
    }

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
// [v5.3] Shared EXIF Extraction logic for WIC (File & Memory)
static void PopulateExifFromQueryReader(IWICMetadataQueryReader* reader, CImageLoader::ImageMetadata* pMetadata) {
    if (!reader || !pMetadata) return;
    
    PROPVARIANT var; PropVariantInit(&var);

    // Helper to try multiple paths
    auto TryGetText = [&](const std::initializer_list<const wchar_t*>& queries, std::wstring& target, bool force = false) {
        if (!target.empty() && !force) return;
        for (const auto& q : queries) {
            if (SUCCEEDED(reader->GetMetadataByName(q, &var))) {
                if (var.vt == VT_LPSTR && var.pszVal) {
                     std::string s = var.pszVal; target = std::wstring(s.begin(), s.end());
                } else if (var.vt == VT_LPWSTR && var.pwszVal) {
                     target = var.pwszVal;
                } else if (var.vt == VT_BSTR && var.bstrVal) {
                     target = var.bstrVal;
                }
                PropVariantClear(&var);
                if (!target.empty()) break;
            }
        }
    };

    // Make - Try standard EXIF, TIFF, XMP, and root fallbacks
    TryGetText({ 
        L"/app1/ifd/{ushort=271}", 
        L"/ifd/{ushort=271}", 
        L"/xmp/tiff:Make",
        L"/{ushort=271}"
    }, pMetadata->Make);

    // Model
    TryGetText({ 
        L"/app1/ifd/{ushort=272}", 
        L"/ifd/{ushort=272}", 
        L"/xmp/tiff:Model", 
        L"/xmp/exif:Model",
        L"/{ushort=272}"
    }, pMetadata->Model);

    // Date (DateTimeOriginal 36867 or DateTime 306)
    // Date (DateTimeOriginal 36867 or DateTime 306) -> FORCE OVERWRITE
    TryGetText({ 
        L"/app1/ifd/exif/{ushort=36867}", 
        L"/ifd/exif/{ushort=36867}", 
        L"/xmp/exif:DateTimeOriginal",
        L"/app1/ifd/{ushort=306}", 
        L"/ifd/{ushort=306}",
        L"/xmp/tiff:DateTime",
        L"/xmp/xmp:CreateDate"
    }, pMetadata->Date, true);

    // Software
    TryGetText({ 
        L"/app1/ifd/{ushort=305}", 
        L"/ifd/{ushort=305}",
        L"/xmp/tiff:Software",
        L"/xmp/xmp:CreatorTool"
    }, pMetadata->Software);

    // ISO
    if (pMetadata->ISO.empty()) {
        const wchar_t* queries[] = { L"/app1/ifd/exif/{ushort=34855}", L"/ifd/exif/{ushort=34855}", L"/xmp/exif:ISOSpeedRatings/xmpSeq:li" };
        for (const auto& q : queries) if (SUCCEEDED(reader->GetMetadataByName(q, &var))) {
            if (var.vt == VT_UI2) pMetadata->ISO = std::to_wstring(var.uiVal);
            else if (var.vt == VT_UI4) pMetadata->ISO = std::to_wstring(var.ulVal);
            else if (var.vt == VT_LPSTR && var.pszVal) {
                // XMP often returns string
                std::string s = var.pszVal; pMetadata->ISO = std::wstring(s.begin(), s.end());
            } else if (var.vt == VT_LPWSTR && var.pwszVal) {
                pMetadata->ISO = var.pwszVal;
            }
            PropVariantClear(&var); break;
        }
    }

    // Shutter (ExposureTime)
    if (pMetadata->Shutter.empty()) {
        double val = 0;
        const wchar_t* queries[] = { L"/app1/ifd/exif/{ushort=33434}", L"/ifd/exif/{ushort=33434}" };
        for (const auto& q : queries) if (SUCCEEDED(GetMetadataRational(reader, q, &val))) {
            wchar_t buf[32];
            if (val >= 1.0) swprintf_s(buf, L"%.1fs", val);
            else swprintf_s(buf, L"1/%.0fs", 1.0/val);
            pMetadata->Shutter = buf;
            break;
        }
    }

    // Aperture (FNumber)
    if (pMetadata->Aperture.empty()) {
        double val = 0;
        const wchar_t* queries[] = { L"/app1/ifd/exif/{ushort=33437}", L"/ifd/exif/{ushort=33437}" };
        for (const auto& q : queries) if (SUCCEEDED(GetMetadataRational(reader, q, &val))) {
            wchar_t buf[32]; swprintf_s(buf, L"f/%.1f", val);
            pMetadata->Aperture = buf;
            break;
        }
    }

    // Focal Length
    if (pMetadata->Focal.empty()) {
        double val = 0;
        const wchar_t* queries[] = { L"/app1/ifd/exif/{ushort=37386}", L"/ifd/exif/{ushort=37386}" };
        for (const auto& q : queries) if (SUCCEEDED(GetMetadataRational(reader, q, &val))) {
            // Try 35mm equivalent
            double val35 = 0;
            const wchar_t* q35[] = { L"/app1/ifd/exif/{ushort=41989}", L"/ifd/exif/{ushort=41989}" };
            bool found35 = false;
            for(const auto& q : q35) if(SUCCEEDED(GetMetadataRational(reader, q, &val35)) && val35 > 0) { found35 = true; break; }
            
            wchar_t buf[64];
            if (found35) {
                 swprintf_s(buf, L"%.0fmm (%.0fmm)", val, val35);
                 pMetadata->Focal35mm = std::to_wstring((int)val35) + L"mm"; 
            } else {
                 swprintf_s(buf, L"%.0fmm", val);
            }
            pMetadata->Focal = buf;
            break;
        }
    }
    
    // Exposure Bias
    double bias = 0.0;
    const wchar_t* biasQueries[] = { L"/app1/ifd/exif/{ushort=37380}", L"/ifd/exif/{ushort=37380}" };
    for (const auto& q : biasQueries) if (SUCCEEDED(GetMetadataSignedRational(reader, q, &bias))) {
        if (std::abs(bias) > 0.0001) {
            wchar_t buf[32]; swprintf_s(buf, L"%+.1f ev", bias);
            pMetadata->ExposureBias = buf;
        }
        break;
    }

    // Flash
    if (pMetadata->Flash.empty()) {
         const wchar_t* flashQueries[] = { L"/app1/ifd/exif/{ushort=37377}", L"/ifd/exif/{ushort=37377}" };
         for (const auto& q : flashQueries) if (SUCCEEDED(reader->GetMetadataByName(q, &var))) {
             UINT fVal = (var.vt == VT_UI2) ? var.uiVal : (var.vt == VT_UI4 ? var.ulVal : 0);
             pMetadata->Flash = (fVal & 1) ? L"Flash: On" : L"Flash: Off";
             PropVariantClear(&var); break;
         }
    }
    
    // Lens Model
    TryGetText({ 
        L"/app1/ifd/exif/{ushort=42036}", 
        L"/ifd/exif/{ushort=42036}", 
        L"/app1/ifd/exif/{ushort=0xA434}", 
        L"/ifd/exif/{ushort=0xA434}",
        L"/xmp/exif:LensModel",
        L"/xmp/aux:Lens"
    }, pMetadata->Lens);

    // [v5.5] White Balance
    if (pMetadata->WhiteBalance.empty()) {
        const wchar_t* queries[] = { L"/app1/ifd/exif/{ushort=41987}", L"/ifd/exif/{ushort=41987}" };
        for (const auto& q : queries) if (SUCCEEDED(reader->GetMetadataByName(q, &var))) {
             UINT val = (var.vt == VT_UI2) ? var.uiVal : (var.vt == VT_UI4 ? var.ulVal : 99);
             if (val == 0) pMetadata->WhiteBalance = L"Auto";
             else if (val == 1) pMetadata->WhiteBalance = L"Manual";
             PropVariantClear(&var); break;
        }
    }

    // [v5.5] Metering Mode
    if (pMetadata->MeteringMode.empty()) {
        const wchar_t* queries[] = { L"/app1/ifd/exif/{ushort=37383}", L"/ifd/exif/{ushort=37383}" };
        for (const auto& q : queries) if (SUCCEEDED(reader->GetMetadataByName(q, &var))) {
             UINT val = (var.vt == VT_UI2) ? var.uiVal : (var.vt == VT_UI4 ? var.ulVal : 0);
             switch (val) {
                 case 1: pMetadata->MeteringMode = L"Average"; break;
                 case 2: pMetadata->MeteringMode = L"Center Weighted"; break;
                 case 3: pMetadata->MeteringMode = L"Spot"; break;
                 case 4: pMetadata->MeteringMode = L"MultiSpot"; break;
                 case 5: pMetadata->MeteringMode = L"Pattern"; break;
                 case 6: pMetadata->MeteringMode = L"Partial"; break;
                 case 255: pMetadata->MeteringMode = L"Other"; break;
                 default: break; // Unknown
             }
             PropVariantClear(&var); break;
        }
    }

    // [v5.5] Exposure Program
    if (pMetadata->ExposureProgram.empty()) {
        const wchar_t* queries[] = { L"/app1/ifd/exif/{ushort=34850}", L"/ifd/exif/{ushort=34850}" };
        for (const auto& q : queries) if (SUCCEEDED(reader->GetMetadataByName(q, &var))) {
             UINT val = (var.vt == VT_UI2) ? var.uiVal : (var.vt == VT_UI4 ? var.ulVal : 0);
             switch (val) {
                 case 1: pMetadata->ExposureProgram = L"Manual"; break;
                 case 2: pMetadata->ExposureProgram = L"Normal"; break;
                 case 3: pMetadata->ExposureProgram = L"Aperture Priority"; break;
                 case 4: pMetadata->ExposureProgram = L"Shutter Priority"; break;
                 case 5: pMetadata->ExposureProgram = L"Creative"; break;
                 case 6: pMetadata->ExposureProgram = L"Action"; break;
                 case 7: pMetadata->ExposureProgram = L"Portrait"; break;
                 case 8: pMetadata->ExposureProgram = L"Landscape"; break;
                 default: break;
             }
             PropVariantClear(&var); break;
        }
    }

    // Color Space
    if (pMetadata->ColorSpace.empty()) {
         const wchar_t* csQueries[] = { L"/app1/ifd/exif/{ushort=40961}", L"/ifd/exif/{ushort=40961}" };
         for (const auto& q : csQueries) if (SUCCEEDED(reader->GetMetadataByName(q, &var))) {
             UINT csVal = (var.vt == VT_UI2) ? var.uiVal : (var.vt == VT_UI4 ? var.ulVal : 0);
             if (csVal == 1) pMetadata->ColorSpace = L"sRGB";
             else if (csVal == 2) pMetadata->ColorSpace = L"Adobe RGB";
             else if (csVal == 65535) pMetadata->ColorSpace = L"Uncalibrated";
             PropVariantClear(&var); break;
         }
    }

    // GPS
    double lat = 0, lon = 0;
    bool foundGPS = false;
    if (SUCCEEDED(GetMetadataGPS(reader, L"/app1/ifd/gps/{ushort=2}", L"/app1/ifd/gps/{ushort=1}", &lat)) &&
        SUCCEEDED(GetMetadataGPS(reader, L"/app1/ifd/gps/{ushort=4}", L"/app1/ifd/gps/{ushort=3}", &lon))) { foundGPS = true; }
    else if (SUCCEEDED(GetMetadataGPS(reader, L"/ifd/gps/{ushort=2}", L"/ifd/gps/{ushort=1}", &lat)) &&
             SUCCEEDED(GetMetadataGPS(reader, L"/ifd/gps/{ushort=4}", L"/ifd/gps/{ushort=3}", &lon))) { foundGPS = true; }
             
    if (foundGPS) {
        pMetadata->HasGPS = true;
        pMetadata->Latitude = lat;
        pMetadata->Longitude = lon;
        
        // Altitude
        double alt = 0;
        const wchar_t* altQueries[] = { L"/app1/ifd/gps/{ushort=6}", L"/ifd/gps/{ushort=6}" };
        for (const auto& q : altQueries) if (SUCCEEDED(GetMetadataRational(reader, q, &alt))) {
            pMetadata->Altitude = alt;
            // Ref
            std::wstring refPath = q;
            size_t pos = refPath.rfind(L"6}");
            if (pos != std::wstring::npos) {
                refPath.replace(pos, 2, L"5}");
                PROPVARIANT rVar; PropVariantInit(&rVar);
                if (SUCCEEDED(reader->GetMetadataByName(refPath.c_str(), &rVar))) {
                    if (rVar.vt == VT_UI1 && rVar.bVal == 1) pMetadata->Altitude = -alt;
                    PropVariantClear(&rVar);
                }
            }
            break;
        }
    }
}

// [v5.3] File Stats
static void PopulateFileStats(LPCWSTR filePath, CImageLoader::ImageMetadata* meta) {
    if (!filePath || !meta) return;
    
    // File Size
    std::error_code ec;
    uintmax_t size = std::filesystem::file_size(filePath, ec);
    if (!ec) meta->FileSize = size;
    
    // Date (Fallback to Modify Date) + [v5.3] Timestamps
    HANDLE hFile = CreateFileW(filePath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
         FILETIME ftCreate, ftAccess, ftWrite;
         if (GetFileTime(hFile, &ftCreate, &ftAccess, &ftWrite)) {
             meta->CreationTime = ftCreate;
             meta->LastWriteTime = ftWrite;
             
             if (meta->Date.empty()) {
                 SYSTEMTIME st; FileTimeToSystemTime(&ftWrite, &st);
                 wchar_t buf[64];
                 swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
                 meta->Date = buf; 
             }
         }
         CloseHandle(hFile);
    }
}





// [v6.0] Async Native Helper (Header-Only, No Pixel Decode)
static bool TryReadMetadataNative(LPCWSTR filePath, CImageLoader::ImageMetadata* pMetadata) {
    if (!filePath || !pMetadata) return false;
    
    // Read Header (256KB should cover most Exif)
    std::vector<uint8_t> header(256 * 1024); 
    size_t actualSize = PeekHeader(filePath, header.data(), header.size());
    if (actualSize == 0) return false;
    header.resize(actualSize);
    
    // 1. JPEG
    // 1. JPEG
    if (header.size() > 2 && header[0] == 0xFF && header[1] == 0xD8) {
        // [Safety] Use setjmp to catch libjpeg errors (defaults to exit())
        struct my_error_mgr { struct jpeg_error_mgr pub; jmp_buf setjmp_buffer; };
        auto my_error_exit = [](j_common_ptr cinfo) {
            my_error_mgr* myerr = (my_error_mgr*)cinfo->err;
            longjmp(myerr->setjmp_buffer, 1);
        };
        
        struct jpeg_decompress_struct cinfo;
        struct my_error_mgr jerr;
        
        cinfo.err = jpeg_std_error(&jerr.pub);
        jerr.pub.error_exit = my_error_exit;
        
        if (setjmp(jerr.setjmp_buffer)) {
            jpeg_destroy_decompress(&cinfo);
            return false;
        }
        
        jpeg_create_decompress(&cinfo);
        
        // [v6.3] Enable Marker Saving for APP1 (Exif) and APP2 (ICC)
        jpeg_save_markers(&cinfo, JPEG_APP0 + 1, 0xFFFF); 
        jpeg_save_markers(&cinfo, JPEG_APP0 + 2, 0xFFFF); // ICC Profile
        
        jpeg_mem_src(&cinfo, header.data(), (unsigned long)header.size());
        
        bool success = false;
        if (jpeg_read_header(&cinfo, TRUE) == JPEG_HEADER_OK) {
             pMetadata->Format = L"JPEG";
             // [v10.0] Fix: Always extract dimensions natively to prevent WIC 2633px preview regression
             pMetadata->Width = cinfo.image_width;
             pMetadata->Height = cinfo.image_height;

             // Fill JPEG format details even when there is no EXIF, so the Info Panel can show chroma/Q/progressive.
             {
                 int q = GetJpegQualityFromBuffer(header.data(), header.size());

                 std::wstring sub;
                 if (cinfo.num_components == 3) {
                     int h0 = cinfo.comp_info[0].h_samp_factor;
                     int v0 = cinfo.comp_info[0].v_samp_factor;
                     int h1 = cinfo.comp_info[1].h_samp_factor;
                     int v1 = cinfo.comp_info[1].v_samp_factor;
                     int h2 = cinfo.comp_info[2].h_samp_factor;
                     int v2 = cinfo.comp_info[2].v_samp_factor;

                     if (h0 == 2 && v0 == 2 && h1 == 1 && v1 == 1 && h2 == 1 && v2 == 1) sub = L"4:2:0";
                     else if (h0 == 2 && v0 == 1 && h1 == 1 && v1 == 1 && h2 == 1 && v2 == 1) sub = L"4:2:2";
                     else if (h0 == 1 && v0 == 1 && h1 == 1 && v1 == 1 && h2 == 1 && v2 == 1) sub = L"4:4:4";
                     else sub = L"4:4:0";
                 } else if (cinfo.num_components == 1) {
                     sub = L"Gray";
                 }

                 wchar_t fmtBuf[64]{};
                 if (q > 0 && !sub.empty()) swprintf_s(fmtBuf, L"%s Q=%d", sub.c_str(), q);
                 else if (q > 0) swprintf_s(fmtBuf, L"Q=%d", q);
                 else if (!sub.empty()) swprintf_s(fmtBuf, L"%s", sub.c_str());

                 pMetadata->FormatDetails = fmtBuf;
                 if (jpeg_has_multiple_scans(&cinfo)) {
                     if (!pMetadata->FormatDetails.empty()) pMetadata->FormatDetails += L" ";
                     pMetadata->FormatDetails += L"Prog";
                 }
             }

             std::vector<uint8_t> iccData;
             
             for (jpeg_saved_marker_ptr marker = cinfo.marker_list; marker; marker = marker->next) {
                 // Exif (APP1)
                 if (marker->marker == JPEG_APP0 + 1 && marker->data_length >= 6 && memcmp(marker->data, "Exif\0\0", 6) == 0) {
                     easyexif::EXIFInfo exif;
                     if (exif.parseFromEXIFSegment(marker->data, marker->data_length) == 0) {
                         PopulateMetadataFromEasyExif_Refined(exif, *pMetadata);
                         success = true;
                     }
                 }
                 // ICC (APP2)
                 else if (marker->marker == JPEG_APP0 + 2 && marker->data_length > 14 && memcmp(marker->data, "ICC_PROFILE\0", 12) == 0) {
                     iccData.insert(iccData.end(), marker->data + 14, marker->data + marker->data_length);
                 }
             }
             
             if (!iccData.empty()) {
                 pMetadata->HasEmbeddedColorProfile = true;
                 std::wstring desc = CImageLoader::ParseICCProfileName(iccData.data(), iccData.size());
                 if (!desc.empty()) pMetadata->ColorSpace = desc;
             }
        }
        jpeg_destroy_decompress(&cinfo);
        return success || !pMetadata->FormatDetails.empty() || pMetadata->Width > 0;
    }
    
    // 2. WebP
    else if (header.size() > 12 && memcmp(header.data(), "RIFF", 4) == 0 && memcmp(header.data() + 8, "WEBP", 4) == 0) {
        WebPData webpData = { header.data(), header.size() };
        WebPDemuxer* demux = WebPDemux(&webpData);
        bool success = false;
        if (demux) {
            WebPChunkIterator chunk;
            // EXIF
            if (WebPDemuxGetChunk(demux, "EXIF", 1, &chunk)) {
                 easyexif::EXIFInfo exif;
                 if (exif.parseFromEXIFSegment(chunk.chunk.bytes, static_cast<unsigned>(chunk.chunk.size)) == 0) {
                     QuickView::PopulateMetadataFromEasyExif(exif, *pMetadata); success = true;
                 } else {
                     std::vector<unsigned char> t(chunk.chunk.size + 6);
                     memcpy(t.data(), "Exif\0\0", 6); memcpy(t.data() + 6, chunk.chunk.bytes, chunk.chunk.size);
                     if (exif.parseFromEXIFSegment(t.data(), static_cast<unsigned>(t.size())) == 0) { QuickView::PopulateMetadataFromEasyExif(exif, *pMetadata); success = true; }
                 }
                 WebPDemuxReleaseChunkIterator(&chunk);
            }
            // ICC
            if (WebPDemuxGetChunk(demux, "ICCP", 1, &chunk)) {
                pMetadata->HasEmbeddedColorProfile = true;
                std::wstring desc = CImageLoader::ParseICCProfileName(chunk.chunk.bytes, chunk.chunk.size);
                if (!desc.empty()) pMetadata->ColorSpace = desc;
                WebPDemuxReleaseChunkIterator(&chunk);
            }
            WebPDemuxDelete(demux);
        }
        return success;
    }
    
    // 3. AVIF (ftyp avif/heic) - libavif header parse
    else if (header.size() > 12 && memcmp(header.data() + 4, "ftyp", 4) == 0) {
        avifDecoder* decoder = avifDecoderCreate();
        if (decoder) {
            bool success = false;
            // Note: avifDecoderSetIOMemory uses reader which might try to read more than available?
            // If we only provide 256KB, parser might fail if file is larger and boxes are far?
            // "rodata.size" is buffer size. AVIF parser respects it. 
            // If it returns AVIF_RESULT_TRUNCATED_DATA, we fail gracefully.
            if (avifDecoderSetIOMemory(decoder, header.data(), header.size()) == AVIF_RESULT_OK) {
                decoder->ignoreXMP = AVIF_TRUE;
                if (avifDecoderParse(decoder) == AVIF_RESULT_OK) {
                    if (decoder->image->exif.size > 0 && decoder->image->exif.data) {
                        easyexif::EXIFInfo exif;
                        if (exif.parseFromEXIFSegment(decoder->image->exif.data, static_cast<unsigned>(decoder->image->exif.size)) == 0) {
                            QuickView::PopulateMetadataFromEasyExif(exif, *pMetadata); success = true;
                        } else {
                            std::vector<unsigned char> t(decoder->image->exif.size + 6);
                            memcpy(t.data(), "Exif\0\0", 6); memcpy(t.data() + 6, decoder->image->exif.data, decoder->image->exif.size);
                            if (exif.parseFromEXIFSegment(t.data(), static_cast<unsigned>(t.size())) == 0) { QuickView::PopulateMetadataFromEasyExif(exif, *pMetadata); success = true; }
                        }
                    }
                    
                    // ICC
                   if (decoder->image->icc.size > 0) {
                       pMetadata->HasEmbeddedColorProfile = true;
                       std::wstring desc = CImageLoader::ParseICCProfileName(decoder->image->icc.data, decoder->image->icc.size);
                       if (!desc.empty()) pMetadata->ColorSpace = desc;
                   }
                }
            }
            avifDecoderDestroy(decoder);
            return success;
        }
    }
    
    // 4. JXL (Signature check FF 0A or 00 00 00 0C JXL)
    else if (header.size() > 12 && ((header[0]==0xFF && header[1]==0x0A) || (header.size()>12 && memcmp(header.data()+4, "JXL ", 4)==0))) {
        JxlDecoder* dec = JxlDecoderCreate(NULL);
        if (!dec) return false;
        
        bool success = false;
        auto runner = CImageLoader::GetJxlRunner();
        JxlDecoderSetParallelRunner(dec, JxlThreadParallelRunner, runner);
        
        if (JXL_DEC_SUCCESS == JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO | JXL_DEC_BOX | JXL_DEC_COLOR_ENCODING)) {
            if (JXL_DEC_SUCCESS == JxlDecoderSetInput(dec, header.data(), header.size())) {
                std::vector<uint8_t> ex;
                bool reading = false;
                for (;;) {
                    JxlDecoderStatus status = JxlDecoderProcessInput(dec);
                    if (status == JXL_DEC_ERROR || status == JXL_DEC_SUCCESS) break;
                    if (status == JXL_DEC_NEED_MORE_INPUT) break; // 256KB exhausted
                    
                    if (status == JXL_DEC_BASIC_INFO) {
                        JxlBasicInfo info = {};
                        if (JXL_DEC_SUCCESS == JxlDecoderGetBasicInfo(dec, &info)) {
                             // Debug logging
                             wchar_t buf[128];
                             swprintf_s(buf, L"[JXL] BasicInfo: %ux%u, Bits=%u, Exp=%u, Alpha=%u, P3=%u\n", 
                                 info.xsize, info.ysize, info.bits_per_sample, 
                                 info.exponent_bits_per_sample, info.num_extra_channels, 
                                 info.uses_original_profile);
                             OutputDebugStringW(buf);
                             pMetadata->Width = info.xsize;
                             pMetadata->Height = info.ysize;
                             
                             // Detect Bit Depth (approximate, JXL stores bits_per_sample)
                             int bitDepth = info.bits_per_sample;
                             if (info.exponent_bits_per_sample > 0) bitDepth = 32; // Float usually 32-bit container
                             
                             bool hasAlpha = (info.num_extra_channels > 0); // Simplification: extra channels *usually* alpha
                             bool isLossless = (info.uses_original_profile == JXL_TRUE); 
                             // Not strictly true (Modular Squeeze can use original profile but be lossy), 
                             // but "uses_original_profile" is the best proxy for "Not transformed to XYB".
                             // True Lossless in JXL is complex. For UI, "Lossless" usually implies "Arithmetic/Modular mode without XYB".
                             
                             CImageLoader::PopulateFormatDetails(pMetadata, L"JXL", bitDepth, isLossless, hasAlpha, false); 
                             success = true; // [v6.3] Mark as successful
                        }
                    }
                    else if (status == JXL_DEC_COLOR_ENCODING) {
                         size_t iccSize = 0;
                         if (JXL_DEC_SUCCESS == JxlDecoderGetICCProfileSize(dec, JXL_COLOR_PROFILE_TARGET_DATA, &iccSize) && iccSize > 0) {
                             std::vector<uint8_t> icc(iccSize);
                             if (JXL_DEC_SUCCESS == JxlDecoderGetColorAsICCProfile(dec, JXL_COLOR_PROFILE_TARGET_DATA, icc.data(), iccSize)) {
                                 pMetadata->HasEmbeddedColorProfile = true;
                                 std::wstring desc = CImageLoader::ParseICCProfileName(icc.data(), icc.size());
                                 
                                 // [v6.3] Fallback: Parse Structured Color Encoding (e.g. for P3/HLG/PQ where ICC name is missing)
                                 if (desc.empty()) {
                                      JxlColorEncoding enc;
                                      if (JXL_DEC_SUCCESS == JxlDecoderGetColorAsEncodedProfile(dec, JXL_COLOR_PROFILE_TARGET_DATA, &enc)) {
                                          desc = ParseJXLColorEncoding(enc);
                                      }
                                 }
                                 
                                 if (!desc.empty()) pMetadata->ColorSpace = desc;
                             }
                         }
                    }
                    else if (status == JXL_DEC_BOX) {
                        JxlBoxType t;
                        if (JXL_DEC_SUCCESS == JxlDecoderGetBoxType(dec, t, JXL_TRUE)) {
                            if (memcmp(t, "Exif", 4) == 0) {
                                ex.resize(65536);
                                JxlDecoderSetBoxBuffer(dec, ex.data(), ex.size());
                                reading = true;
                            } else reading = false;
                        }
                    } else if (reading && status != JXL_DEC_BOX_NEED_MORE_OUTPUT) {
                        // Finished box
                        size_t rem = JxlDecoderReleaseBoxBuffer(dec);
                        size_t size = ex.size() - rem;
                        if (size > 0) {
                            easyexif::EXIFInfo exif;
                            if (exif.parseFromEXIFSegment(ex.data(), static_cast<unsigned>(size)) == 0) { QuickView::PopulateMetadataFromEasyExif(exif, *pMetadata); success = true; }
                            else {
                                std::vector<unsigned char> tmp(size + 6);
                                memcpy(tmp.data(), "Exif\0\0", 6); memcpy(tmp.data()+6, ex.data(), size);
                                if (exif.parseFromEXIFSegment(tmp.data(), static_cast<unsigned>(tmp.size())) == 0) { QuickView::PopulateMetadataFromEasyExif(exif, *pMetadata); success = true; }
                            }
                        }
                        break; // Found it
                    }
                }
            }
        }
        JxlDecoderDestroy(dec);
        return success;
    }

    return false;
}

HRESULT CImageLoader::ReadMetadata(LPCWSTR filePath, ImageMetadata* pMetadata, bool clear) {
    if (!filePath || !pMetadata) return E_INVALIDARG;
    if (clear) *pMetadata = {}; 
    
    // [v5.3] Read File Stats (Size/Date) - ALWAYS read this first
    PopulateFileStats(filePath, pMetadata);

    // [v6.0] Try Native parsers first (Faster & No WIC dependency for JXL/AVIF)
    if (TryReadMetadataNative(filePath, pMetadata)) {
        return S_OK;
    }
    
    // 1. Detect Format (if missing)

    if (pMetadata->Format.empty()) {
        pMetadata->Format = DetectFormatFromContent(filePath);
    }
    
    // File Stats already populated above
    
    // 2. Dispatch
    if (pMetadata->Format == L"Raw" || pMetadata->Format == L"Unknown") {
        // Try LibRaw first for metadata
        std::wstring path = filePath;
        std::transform(path.begin(), path.end(), path.begin(), ::towlower);
        
        bool isRawExt = (path.ends_with(L".arw") || path.ends_with(L".cr2") || path.ends_with(L".nef") || 
                         path.ends_with(L".dng") || path.ends_with(L".orf") || path.ends_with(L".rw2") || 
                         path.ends_with(L".raf") || path.ends_with(L".pef") || path.ends_with(L".srw") || 
                         path.ends_with(L".cr3") || path.ends_with(L".nrw") ||
                         path.ends_with(L".heic") || path.ends_with(L".heif"));
                         
        if (isRawExt) {
            if (SUCCEEDED(ReadMetadataLibRaw(filePath, pMetadata))) {
                // LibRaw succeeded - metadata fields populated
                // Continue to WIC for GPS if needed (or other missing fields)
            }
        }
    }
    
    // 3. WIC Fallback (for JPEG, PNG, or if LibRaw failed/didn't have GPS)
    if (!m_wicFactory) return S_OK; 
    
    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = m_wicFactory->CreateDecoderFromFilename(filePath, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
    
    if (FAILED(hr)) return S_OK;
    
    // Get First Frame
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return S_OK;
    
    // Dimensions (if missing)
    if (pMetadata->Width == 0 || pMetadata->Height == 0) {
        frame->GetSize(&pMetadata->Width, &pMetadata->Height);
    }
    
    // 4. Metadata Reader (Standard EXIF + GPS)
    ComPtr<IWICMetadataQueryReader> reader;
    
    // [v6.9.5] Safety: Skip EXIF for BMP/ICO to prevent WIC crashes (unsupported op)
    bool skipExif = (pMetadata->Format == L"BMP" || pMetadata->Format == L"ICO" || pMetadata->Format == L"CUR");

    if (!skipExif && SUCCEEDED(frame->GetMetadataQueryReader(&reader))) {
        PopulateExifFromQueryReader(reader.Get(), pMetadata);
        
        // [v6.2] Level 1: EXIF Color Space (Fastest)
        // Check Tag 40961 (ColorSpace)
        if (pMetadata->ColorSpace.empty()) {
            PROPVARIANT csVar; PropVariantInit(&csVar);
            const wchar_t* csQueries[] = { L"/app1/ifd/exif/{ushort=40961}", L"/ifd/exif/{ushort=40961}" };
            for (const auto& q : csQueries) {
                if (SUCCEEDED(reader->GetMetadataByName(q, &csVar))) {
                    UINT csVal = 0;
                    if (csVar.vt == VT_UI2) csVal = csVar.uiVal;
                    else if (csVar.vt == VT_UI4) csVal = csVar.ulVal;
                    
                    if (csVal == 1) pMetadata->ColorSpace = L"sRGB";
                    else if (csVal == 2) pMetadata->ColorSpace = L"Adobe RGB";
                    else if (csVal == 65535) pMetadata->ColorSpace = L"Uncalibrated";
                    PropVariantClear(&csVar);
                    break;
                }
            }
        }
    }

    // [v6.2] Level 2: ICC Profile (Most Accurate)
    // Only if ColorSpace is empty or "Uncalibrated"
    if (pMetadata->ColorSpace.empty() || pMetadata->ColorSpace == L"Uncalibrated") {
        UINT count = 0;
        if (SUCCEEDED(frame->GetColorContexts(0, nullptr, &count)) && count > 0) {
            std::vector<IWICColorContext*> contexts(count);
            for (UINT i = 0; i < count; i++) m_wicFactory->CreateColorContext(&contexts[i]);
            
            UINT actual = 0;
            if (SUCCEEDED(frame->GetColorContexts(count, contexts.data(), &actual))) {
                bool found = false;
                for (UINT i = 0; i < actual; i++) {
                    if (!found) { 
                        WICColorContextType type;
                        contexts[i]->GetType(&type);
                        if (type == WICColorContextExifColorSpace) {
                            // WIC's wrapper for EXIF ColorSpace tag - usually redundant with above
                            UINT val = 0; contexts[i]->GetExifColorSpace(&val);
                            if (val == 1) { pMetadata->ColorSpace = L"sRGB"; found = true; }
                            else if (val == 2) { pMetadata->ColorSpace = L"Adobe RGB"; found = true; }
                        } else if (type == WICColorContextProfile) {
                            // Deep inspect profile
                            UINT cbProfile = 0;
                            contexts[i]->GetProfileBytes(0, nullptr, &cbProfile);
                            if (cbProfile > 0) {
                                std::vector<BYTE> profile(cbProfile);
                                if (SUCCEEDED(contexts[i]->GetProfileBytes(cbProfile, profile.data(), &cbProfile))) {
                                    // Search for signatures
                                    // [v6.2] Professional Parsing
                                    std::wstring desc = CImageLoader::ParseICCProfileName(profile.data(), profile.size());
                                    if (!desc.empty()) {
                                        pMetadata->ColorSpace = desc;
                                        found = true;
                                        pMetadata->HasEmbeddedColorProfile = true; // [Phase 18] Flag it
                                    } 
                                }
                            }
                        }
                    }
                }
            }
            // Release ALL created contexts
            for (UINT i = 0; i < count; i++) {
                if (contexts[i]) contexts[i]->Release();
            }
        }
    }
    
    // [v6.2] Level 3: Implicit (Fallback)
    if (pMetadata->ColorSpace.empty() || pMetadata->ColorSpace == L"Uncalibrated") {
        // Check Channels / Pixel Format
        WICPixelFormatGUID fmt;
        if (SUCCEEDED(frame->GetPixelFormat(&fmt))) {
             if (IsEqualGUID(fmt, GUID_WICPixelFormat16bppGray) || 
                 IsEqualGUID(fmt, GUID_WICPixelFormat8bppGray)) {
                 pMetadata->ColorSpace = L"Grayscale";
             } else if (IsEqualGUID(fmt, GUID_WICPixelFormat32bppCMYK) ||
                        IsEqualGUID(fmt, GUID_WICPixelFormat64bppCMYK)) {
                 pMetadata->ColorSpace = L"CMYK";
             } else {
                  // [v6.3] Do NOT label as "Untagged" if we know there is an embedded profile whose name we just couldn't parse.
                  if (pMetadata->HasEmbeddedColorProfile) {
                      pMetadata->ColorSpace = L"Embedded Profile"; 
                  } else {
                      pMetadata->ColorSpace = L"sRGB (Untagged)";
                  }
              }
        }
    }

    // [v6.9.5] Redundant code removed. 
    // Exposure, Flash, and GPS are already populated by PopulateExifFromQueryReader above.
    // This avoids crashing when 'reader' is NULL (e.g. BMP/ICO).

    pMetadata->IsFullMetadataLoaded = true;
    return S_OK;
}

HRESULT CImageLoader::ComputeHistogram(IWICBitmapSource* source, ImageMetadata* pMetadata) {
    if (!source || !pMetadata) return E_INVALIDARG;

    UINT width = 0, height = 0;
    source->GetSize(&width, &height);
    if (width == 0 || height == 0) return E_FAIL;

    ComPtr<IWICBitmap> pBitmap;
    if (SUCCEEDED(source->QueryInterface(IID_PPV_ARGS(&pBitmap)))) {
        WICRect rcLock = { 0, 0, (INT)width, (INT)height };
        ComPtr<IWICBitmapLock> pLock;
        if (SUCCEEDED(pBitmap->Lock(&rcLock, WICBitmapLockRead, &pLock))) {
            UINT cbStride = 0;
            UINT cbBufferSize = 0;
            BYTE* pPixels = nullptr;
            pLock->GetStride(&cbStride);
            pLock->GetDataPointer(&cbBufferSize, &pPixels);

            WICPixelFormatGUID format;
            pLock->GetPixelFormat(&format);

            if (IsEqualGUID(format, GUID_WICPixelFormat32bppPBGRA) || IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA)) {
                QuickView::RawImageFrame frame = {};
                frame.pixels = pPixels;
                frame.width = width;
                frame.height = height;
                frame.stride = cbStride;

                ComputeHistogramFromFrame(frame, pMetadata);
                return S_OK;
            }
        }
    }

    // Fallback if not IWICBitmap or not BGRA/PBGRA: Convert to memory and delegate
    ComPtr<IWICFormatConverter> converter;
    if (SUCCEEDED(m_wicFactory->CreateFormatConverter(&converter))) {
        if (SUCCEEDED(converter->Initialize(source, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.f, WICBitmapPaletteTypeCustom))) {
            UINT stride = width * 4;
            std::vector<BYTE> buffer(stride * height);
            if (SUCCEEDED(converter->CopyPixels(nullptr, stride, (UINT)buffer.size(), buffer.data()))) {
                QuickView::RawImageFrame frame = {};
                frame.pixels = buffer.data();
                frame.width = width;
                frame.height = height;
                frame.stride = stride;

                ComputeHistogramFromFrame(frame, pMetadata);
                return S_OK;
            }
        }
    }

    return E_FAIL;
}
// [v5.2] Histogram from RawImageFrame (for HeavyLanePool pipeline)
void CImageLoader::ComputeHistogramFromFrame(const QuickView::RawImageFrame& frame, ImageMetadata* pMetadata) {
    if (!frame.pixels || frame.width == 0 || frame.height == 0 || !pMetadata) return;

    pMetadata->HistR.assign(256, 0);
    pMetadata->HistG.assign(256, 0);
    pMetadata->HistB.assign(256, 0);
    pMetadata->HistL.assign(256, 0);
    
    // Skip Sampling (Target ~2MP samples)
    UINT64 totalPixels = (UINT64)frame.width * frame.height;
    UINT stepY = 1;
    
    if (totalPixels > 2000000) {
        // Calculate step to keep roughly 2M pixels
        // total / step = 2M  => step = total / 2M
        stepY = (UINT)(totalPixels / 2000000);
        if (stepY < 1) stepY = 1;
    }

    const uint8_t* ptr = frame.pixels;
    int stride = frame.stride;
    const UINT lapStep = stepY;
    double lapSumSq = 0.0;
    uint64_t lapCount = 0;
    
    // Assume BGRA8888 (standard for RawImageFrame)
    // Assume BGRA8888 (standard for RawImageFrame)
    for (UINT y = 0; y < frame.height; y += stepY) {
        const uint8_t* row = ptr + (UINT64)y * stride;
        UINT x = 0;
    
        // Process 8 pixels per iteration.
        const UINT width8 = frame.width & ~7u;

#if QUICKVIEW_USE_STD_SIMD_HIST
        using u32x8 = std::simd<uint32_t, std::simd_abi::fixed_size<8>>;
        alignas(32) uint32_t bLane[8];
        alignas(32) uint32_t gLane[8];
        alignas(32) uint32_t rLane[8];
        alignas(32) uint32_t lLane[8];

        for (; x < width8; x += 8) {
            const uint8_t* p = row + x * 4;
            for (int i = 0; i < 8; ++i) {
                const uint8_t* px = p + i * 4;
                const uint32_t b = px[0];
                const uint32_t g = px[1];
                const uint32_t r = px[2];
                bLane[i] = b;
                gLane[i] = g;
                rLane[i] = r;
                pMetadata->HistB[b]++;
                pMetadata->HistG[g]++;
                pMetadata->HistR[r]++;
            }

            u32x8 vb;
            u32x8 vg;
            u32x8 vr;
            vb.copy_from(bLane, std::element_aligned);
            vg.copy_from(gLane, std::element_aligned);
            vr.copy_from(rLane, std::element_aligned);

            const u32x8 vl = (vr * 299u + vg * 587u + vb * 114u + 500u) / 1000u;
            vl.copy_to(lLane, std::element_aligned);

            for (int i = 0; i < 8; ++i) {
                pMetadata->HistL[lLane[i]]++;
            }
        }
#else
        // [AVX2] SIMD Optimization for Luminance Calculation
        const __m256i vCoeffs = _mm256_set1_epi64x(0x0000012B024B0072);
        // Multiply by 8389 and shift by 23: 8389 / 2^23 = 8389 / 8388608 ≈ 0.000999999
        // Max Luma = 255 * 1000 = 255000. Max mult = 255000 * 8389 = 2,139,195,000 < 2^31
        const __m256i vMul = _mm256_set1_epi32(8389);

        for (; x < width8; x += 8) {
            __m256i vPixels = _mm256_loadu_si256((const __m256i*)(row + x * 4));
            __m256i vPix03 = _mm256_cvtepu8_epi16(_mm256_castsi256_si128(vPixels));
            __m256i vPix47 = _mm256_cvtepu8_epi16(_mm256_extracti128_si256(vPixels, 1));
            __m256i vSum03 = _mm256_madd_epi16(vPix03, vCoeffs);
            __m256i vSum47 = _mm256_madd_epi16(vPix47, vCoeffs);
            __m256i vLuma03 = _mm256_hadd_epi32(vSum03, vSum03);
            __m256i vLuma47 = _mm256_hadd_epi32(vSum47, vSum47);
            __m256i vDiv03 = _mm256_srli_epi32(_mm256_mullo_epi32(vLuma03, vMul), 23);
            __m256i vDiv47 = _mm256_srli_epi32(_mm256_mullo_epi32(vLuma47, vMul), 23);

            uint32_t l0 = _mm256_cvtsi256_si32(vDiv03);
            uint32_t l1 = _mm256_extract_epi32(vDiv03, 1);
            uint32_t l2 = _mm256_extract_epi32(vDiv03, 4);
            uint32_t l3 = _mm256_extract_epi32(vDiv03, 5);
            uint32_t l4 = _mm256_cvtsi256_si32(vDiv47);
            uint32_t l5 = _mm256_extract_epi32(vDiv47, 1);
            uint32_t l6 = _mm256_extract_epi32(vDiv47, 4);
            uint32_t l7 = _mm256_extract_epi32(vDiv47, 5);

            const uint8_t* p = row + x * 4;
            pMetadata->HistB[p[0]]++; pMetadata->HistG[p[1]]++; pMetadata->HistR[p[2]]++; pMetadata->HistL[l0]++;
            pMetadata->HistB[p[4]]++; pMetadata->HistG[p[5]]++; pMetadata->HistR[p[6]]++; pMetadata->HistL[l1]++;
            pMetadata->HistB[p[8]]++; pMetadata->HistG[p[9]]++; pMetadata->HistR[p[10]]++; pMetadata->HistL[l2]++;
            pMetadata->HistB[p[12]]++; pMetadata->HistG[p[13]]++; pMetadata->HistR[p[14]]++; pMetadata->HistL[l3]++;
            pMetadata->HistB[p[16]]++; pMetadata->HistG[p[17]]++; pMetadata->HistR[p[18]]++; pMetadata->HistL[l4]++;
            pMetadata->HistB[p[20]]++; pMetadata->HistG[p[21]]++; pMetadata->HistR[p[22]]++; pMetadata->HistL[l5]++;
            pMetadata->HistB[p[24]]++; pMetadata->HistG[p[25]]++; pMetadata->HistR[p[26]]++; pMetadata->HistL[l6]++;
            pMetadata->HistB[p[28]]++; pMetadata->HistG[p[29]]++; pMetadata->HistR[p[30]]++; pMetadata->HistL[l7]++;
        }
#endif

    for (; x < frame.width; x++) {
        // Unrolling or SIMD could be added here, but scalar is fast enough with skip sampling.
        // Layout: B, G, R, A
        uint8_t b = row[x * 4 + 0];
        uint8_t g = row[x * 4 + 1];
        uint8_t r = row[x * 4 + 2];
        
        pMetadata->HistB[b]++;
        pMetadata->HistG[g]++;
        pMetadata->HistR[r]++;
            
            // Luminance (approx)
            uint8_t l = (uint8_t)((r * 299 + g * 587 + b * 114) / 1000);
            pMetadata->HistL[l]++;
        }
    }

    // Laplacian Sharpness (sampled)
    if (frame.width > lapStep * 2 && frame.height > lapStep * 2) {
        auto getLumaAt = [&](const uint8_t* rowPtr, UINT x) -> int {
            const uint8_t b = rowPtr[x * 4 + 0];
            const uint8_t g = rowPtr[x * 4 + 1];
            const uint8_t r = rowPtr[x * 4 + 2];
            return (int)((54 * r + 183 * g + 19 * b) >> 8);
        };

        for (UINT y = lapStep; y + lapStep < frame.height; y += lapStep) {
            const uint8_t* rowPrev = ptr + (UINT64)(y - lapStep) * stride;
            const uint8_t* rowCurr = ptr + (UINT64)y * stride;
            const uint8_t* rowNext = ptr + (UINT64)(y + lapStep) * stride;

            for (UINT x = lapStep; x + lapStep < frame.width; x += lapStep) {
                const int center = getLumaAt(rowCurr, x);
                const int left = getLumaAt(rowCurr, x - lapStep);
                const int right = getLumaAt(rowCurr, x + lapStep);
                const int up = getLumaAt(rowPrev, x);
                const int down = getLumaAt(rowNext, x);
                const int lap = (up + down + left + right) - 4 * center;
                lapSumSq += (double)lap * (double)lap;
                lapCount++;
            }
        }
    }

    if (lapCount > 0) {
        pMetadata->Sharpness = lapSumSq / (double)lapCount;
        pMetadata->HasSharpness = true;
    } else {
        pMetadata->Sharpness = 0.0;
        pMetadata->HasSharpness = false;
    }

    {
        uint64_t total = 0;
        for (uint32_t v : pMetadata->HistL) total += v;
        if (total > 0) {
            double entropy = 0.0;
            const double invTotal = 1.0 / (double)total;
            for (uint32_t v : pMetadata->HistL) {
                if (v == 0) continue;
                const double p = (double)v * invTotal;
                entropy -= p * std::log2(p);
            }
            pMetadata->Entropy = entropy;
            pMetadata->HasEntropy = true;
        } else {
            pMetadata->Entropy = 0.0;
            pMetadata->HasEntropy = false;
        }
    }
}

// ============================================================================
// Phase 6: Surgical Format Optimizations
// ============================================================================




HRESULT CImageLoader::LoadThumbAVIF_Proxy(const uint8_t* data, size_t size, int targetSize, ThumbData* pData, bool allowSlow, ImageMetadata* pMetadata) {
    if (!data || size == 0 || !pData) return E_POINTER;

    avifDecoder* decoder = avifDecoderCreate();
    if (!decoder) return E_OUTOFMEMORY;
    
    // [v6.9] Robustness: Disable strict compliance checks to accept slightly malformed files
    decoder->strictFlags = AVIF_STRICT_DISABLED;
    
    // [v6.9.1] Maximize Tolerance
    decoder->ignoreXMP = AVIF_TRUE; 
    decoder->ignoreExif = AVIF_TRUE; // We extract Exif via EasyExif later anyway
    decoder->maxThreads = (std::max)(1u, std::thread::hardware_concurrency());
    decoder->imageSizeLimit = 32768ULL * 32768ULL;
    decoder->imageDimensionLimit = 32768;
    
    // Force Primary Item (Single Image) decoding if possible to avoid complex sequence issues?
    // decoder->requestedSource = AVIF_DECODER_SOURCE_PRIMARY_ITEM; 
    // Actually, keep AUTO, as some files might be sequences.

    // 2. Setup IO
    avifResult result = avifDecoderSetIOMemory(decoder, data, size);
    if (result != AVIF_RESULT_OK) {
        avifDecoderDestroy(decoder);
        return E_FAIL;
    }

    // 3. Parse
    result = avifDecoderParse(decoder);
    // [v6.9.2] Diagnostic Logs CLEANED UP for Production
    if (result != AVIF_RESULT_OK) {
        avifDecoderDestroy(decoder);
        return E_FAIL;
    }
    
    // [v6.0] Native EXIF Extraction
    // Extract early (after Parse) to support both FastPath and Main path
    if (pMetadata && decoder->image->exif.data && decoder->image->exif.size > 0) {
        easyexif::EXIFInfo exif;
        if (exif.parseFromEXIFSegment(decoder->image->exif.data, static_cast<unsigned>(decoder->image->exif.size)) == 0) {
            // QuickView::PopulateMetadataFromEasyExif(exif, *pMetadata);
            PopulateMetadataFromEasyExif_Refined(exif, *pMetadata);
        } else {
             // Try prepend Exif header for raw TIFF payloads
             std::vector<unsigned char> tempBuf(decoder->image->exif.size + 6);
             memcpy(tempBuf.data(), "Exif\0\0", 6);
             memcpy(tempBuf.data() + 6, decoder->image->exif.data, decoder->image->exif.size);
             if (exif.parseFromEXIFSegment(tempBuf.data(), static_cast<unsigned>(tempBuf.size())) == 0) {
                 // QuickView::PopulateMetadataFromEasyExif(exif, *pMetadata);
                 PopulateMetadataFromEasyExif_Refined(exif, *pMetadata);
             }
        }
    }
    
    // [v6.2] AVIF Bit Depth & Details
    if (pMetadata) {
        // AVIF usually 8, 10, or 12.
        // decoder->image->depth is reliable after decoding.
        // Lossless? AVIF doesn't have a simple flag. Assuming Lossy unless user profile overrides?
        // We will default to Lossy for AVIF unless we check qMin/qMax? Too complex.
        // Alpha? yes if decoder->image->alphaPlane != NULL or image->alphaRowBytes > 0
        bool hasAlpha = (decoder->image->alphaPlane != nullptr && decoder->image->alphaRowBytes > 0);
        
        // Wait, 'decoder->image' might be null if destroyed? No, we destroy after.
        
        CImageLoader::PopulateFormatDetails(
            pMetadata, 
            L"AVIF", 
            decoder->image->depth, 
            false, // Assume Lossy for AVIF usually
            hasAlpha, 
            (decoder->imageCount > 1) // Animation
        );
        
        if (size > 1024 * 1024 * 5) pMetadata->FormatDetails += L" (Deep)"; // cosmetic
    }


    // [Success Strategy] "Too Big to Thumb"
    // If image > 50 MP (e.g. 8K x 6K) AND we didn't find Exif thumb above:
    // It will take > 1.0s to decode. The Heavy Lane will ALSO take > 1.0s to decode.
    // If we decode here, we delay Heavy Lane (due to thread contention/memory).
    // Better to RETURN FAILURE (specifically S_FALSE for 'skipped') to let Heavy Lane start immediately.
    // 50MP = 50,000,000 pixels. 9449*9449 = 89MP.
    uint64_t pixelCount = (uint64_t)decoder->image->width * (uint64_t)decoder->image->height;
    if (pixelCount > 40000000 && !allowSlow) { // > 40MP threshold
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
        if (targetSize > 0 && !allowSlow) {
             avifDecoderDestroy(decoder);
             return E_ABORT; 
        }

        result = avifDecoderNextImage(decoder);
        if (result != AVIF_RESULT_OK) {
            avifDecoderDestroy(decoder);
            return E_FAIL;
        }

        if (decoder->image->icc.size > 0 && pMetadata) {
             std::wstring desc = CImageLoader::ParseICCProfileName(decoder->image->icc.data, decoder->image->icc.size);
             if (!desc.empty()) pMetadata->ColorSpace = desc;
        }

        if (pMetadata) {
            const wchar_t* subsamp = L"";
            switch (decoder->image->yuvFormat) {
                 case AVIF_PIXEL_FORMAT_YUV444: subsamp = L"4:4:4"; break;
                 case AVIF_PIXEL_FORMAT_YUV422: subsamp = L"4:2:2"; break;
                 case AVIF_PIXEL_FORMAT_YUV420: subsamp = L"4:2:0"; break;
                 case AVIF_PIXEL_FORMAT_YUV400: subsamp = L"Gray"; break;
                 default: subsamp = L""; break;
            }
            
            // Heuristic for Lossless: Matrix=Identity (RGB) + 4:4:4 usually implies lossless (or high quality)
            bool isIdentity = (decoder->image->matrixCoefficients == AVIF_MATRIX_COEFFICIENTS_IDENTITY);
            const wchar_t* coding = isIdentity ? L"RGB" : L"YUV";
            
            // Animation
            bool isAnim = (decoder->imageCount > 1);
            
            // Alpha
            bool hasAlpha = (decoder->image->alphaPlane != nullptr) || decoder->alphaPresent;
            
            wchar_t fmtBuf[128];
            // Format: "10-bit YUV 4:2:0 (Alpha) Seq"
            swprintf_s(fmtBuf, L"%d-bit %s%s%s%s%s", 
                decoder->image->depth, 
                coding,
                (subsamp[0] ? L" " : L""), subsamp,
                (hasAlpha ? L" (Alpha)" : L""),
                (isAnim ? L" Seq" : L"")
            );
            pMetadata->FormatDetails = fmtBuf;
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
    rgb.alphaPremultiplied = AVIF_TRUE; // [v6.9.3] Fix: Direct2D requires Premultiplied Alpha
    
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

// [JXL Memory Optimization] Callback REMOVED (Replaced by JxlDecoderSetPreferredDownsampling)




HRESULT CImageLoader::LoadThumbJXL_DC(const uint8_t* pFile, size_t fileSize, ThumbData* pData, ImageMetadata* pMetadata, bool forceRenderFull, bool allowFakeBase) {
    if (!pFile || fileSize == 0 || !pData) return E_INVALIDARG;

    pData->isValid = false;
    // Default to true. If we fall back to Full Decode (Small), we set it to false.
    pData->isBlurry = true; 

    // 1. Create Decoder
    JxlDecoder* dec = JxlDecoderCreate(NULL);
    if (!dec) return E_OUTOFMEMORY;
    
    // [JXL Global Runner] Use singleton instead of creating each time
    void* runner = CImageLoader::GetJxlRunner();
    if (JXL_DEC_SUCCESS != JxlDecoderSetParallelRunner(dec, JxlThreadParallelRunner, runner)) {
        JxlDecoderDestroy(dec);
        return E_FAIL;
    }

    // RAII Cleanup
    auto cleanup = [&](HRESULT hr) {
        JxlDecoderDestroy(dec);
        return hr;
    };
    
    // 2. Setup Input
    if (JXL_DEC_SUCCESS != JxlDecoderSetInput(dec, pFile, fileSize)) return cleanup(E_FAIL);
    
    // 3. Subscribe Events
    // Must subscribe to FRAME_PROGRESSION to catch 1:8.
    // Also subscribe PREVIEW_IMAGE as fallback when DC is missing.
    int events = JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE | JXL_DEC_FRAME | JXL_DEC_FRAME_PROGRESSION | JXL_DEC_PREVIEW_IMAGE;
    if (pMetadata) events |= JXL_DEC_BOX;
    
    if (JXL_DEC_SUCCESS != JxlDecoderSubscribeEvents(dec, events)) return cleanup(E_FAIL);

    // [v8.4] Critical: In libjxl 0.8+, JXL_DEC_DC_IMAGE was removed. 
    // We MUST set ProgressiveDetail to kDC to force the decoder to emit 
    // JXL_DEC_FRAME_PROGRESSION at the 1:8 DC layer stage!
    JxlDecoderSetProgressiveDetail(dec, JxlProgressiveDetail::kDC);

    JxlBasicInfo info = {};
    JxlPixelFormat format = { 4, JXL_TYPE_UINT8, JXL_LITTLE_ENDIAN, 0 }; 

    // EXIF State
    std::vector<uint8_t> exifBuffer;
    bool readingExif = false;
    bool foundDC = false; // Flag if we caught the rabbit
    bool isProgressive = false;

    for (;;) {
        JxlDecoderStatus status = JxlDecoderProcessInput(dec);

        if (status == JXL_DEC_ERROR) {
             return cleanup(E_FAIL);
        }
        else if (status == JXL_DEC_SUCCESS) {
            return cleanup(foundDC ? S_OK : E_FAIL);
        }
        else if (status == JXL_DEC_BASIC_INFO) {
            if (JXL_DEC_SUCCESS != JxlDecoderGetBasicInfo(dec, &info)) return cleanup(E_FAIL);
            
            JxlColorEncoding color_encoding = {};
            color_encoding.color_space = JXL_COLOR_SPACE_RGB;
            color_encoding.white_point = JXL_WHITE_POINT_D65;
            color_encoding.primaries = JXL_PRIMARIES_SRGB;
            color_encoding.transfer_function = JXL_TRANSFER_FUNCTION_SRGB;
            color_encoding.rendering_intent = JXL_RENDERING_INTENT_PERCEPTUAL;
            JxlDecoderSetOutputColorProfile(dec, &color_encoding, NULL, 0);

            pData->origWidth = static_cast<int>(info.xsize);
            pData->origHeight = static_cast<int>(info.ysize);
            
            if (pMetadata) {
                pMetadata->Width = static_cast<int>(info.xsize);
                pMetadata->Height = static_cast<int>(info.ysize);
                wchar_t fmtBuf[64];
                const wchar_t* mode = info.uses_original_profile ? L"Lossless" : L"Lossy";
                swprintf_s(fmtBuf, L"%d-bit %s", info.bits_per_sample, mode);
                pMetadata->FormatDetails = fmtBuf;
                if (info.have_animation) pMetadata->FormatDetails += L" Anim";
            }

            // [Diagnostic] Log encoding mode to help debug DC detection failures
            {
                wchar_t diagBuf[256];
                swprintf_s(diagBuf, L"[JXL_DC] BasicInfo: %ux%u, %d-bit, uses_original_profile=%d (Modular=%s), have_preview=%d\n",
                    info.xsize, info.ysize, info.bits_per_sample,
                    info.uses_original_profile,
                    info.uses_original_profile ? L"YES→No DC Layer" : L"NO→VarDCT→DC possible",
                    info.have_preview);
                OutputDebugStringW(diagBuf);
            }
        }
        else if (status == JXL_DEC_FRAME_PROGRESSION) {
            isProgressive = true;
            // [v8.3] Catch the Rabbit
            size_t ratio = JxlDecoderGetIntendedDownsamplingRatio(dec);
            
            if (ratio >= 4 && ratio <= 16) {
                // Caught it!
                size_t downW = info.xsize / ratio;
                size_t downH = info.ysize / ratio;
                if (downW == 0) downW = 1;
                if (downH == 0) downH = 1;
                
                size_t bufferSize = downW * downH * 4;
                try {
                    pData->pixels.resize(bufferSize);
                } catch(...) { return cleanup(E_OUTOFMEMORY); }
                
                pData->width = (UINT)downW;
                pData->height = (UINT)downH;
                pData->stride = (UINT)downW * 4;
                pData->isValid = true;
                pData->loaderName = L"libjxl (Prog DC)"; // Success marker

                if (JXL_DEC_SUCCESS != JxlDecoderSetImageOutBuffer(dec, &format, pData->pixels.data(), bufferSize)) {
                    return cleanup(E_FAIL);
                }
                
                // Force Flush
                if (JXL_DEC_SUCCESS != JxlDecoderFlushImage(dec)) {
                     return cleanup(E_FAIL);
                }
                
                // Optimized Premultiply & Swizzle (RGBA -> BGRA) using SIMD
                uint8_t* p = pData->pixels.data();
                size_t pxCount = bufferSize / 4;
                SIMDUtils::SwizzleRGBA_to_BGRA_Premul(p, pxCount);
                 
                foundDC = true;
                return cleanup(S_OK); // Success!
            }
        }
        else if (status == JXL_DEC_BOX) {
            JxlBoxType type;
            if (JXL_DEC_SUCCESS == JxlDecoderGetBoxType(dec, type, JXL_TRUE)) {
                if (memcmp(type, "Exif", 4) == 0 && pMetadata) {
                    exifBuffer.resize(256 * 1024); 
                    JxlDecoderSetBoxBuffer(dec, exifBuffer.data(), exifBuffer.size());
                    readingExif = true;
                } else {
                    readingExif = false;
                }
            }
        }
        else if (status == JXL_DEC_BOX_NEED_MORE_OUTPUT) {
            if (readingExif) JxlDecoderReleaseBoxBuffer(dec);
        }
        else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
            size_t outSize = 0;
            if (JXL_DEC_SUCCESS != JxlDecoderImageOutBufferSize(dec, &format, &outSize)) {
                return cleanup(E_FAIL);
            }

            // [Fallback A] If no DC found but preview exists, prefer preview over 1x1 fake.
            // This improves first-frame latency for large JXL files that ship preview but no 1:8 DC.
            if (!foundDC && info.have_preview) {
                size_t previewSize = (size_t)info.preview.xsize * (size_t)info.preview.ysize * 4;
                if (previewSize > 0 && outSize == previewSize) {
                    // Safety cap: preview buffer should remain practical for base-layer display.
                    const size_t kPreviewCap = 512ULL * 1024ULL * 1024ULL; // 512 MB
                    if (previewSize <= kPreviewCap) {
                        try {
                            pData->pixels.resize(previewSize);
                        } catch (...) {
                            return cleanup(E_OUTOFMEMORY);
                        }

                        pData->width = (UINT)info.preview.xsize;
                        pData->height = (UINT)info.preview.ysize;
                        pData->stride = (UINT)info.preview.xsize * 4;
                        pData->isValid = true;
                        pData->isBlurry = true;
                        pData->loaderName = L"libjxl (Preview)";

                        if (JXL_DEC_SUCCESS != JxlDecoderSetImageOutBuffer(dec, &format, pData->pixels.data(), previewSize)) {
                            return cleanup(E_FAIL);
                        }

                        wchar_t dbg[160];
                        swprintf_s(dbg, L"[JXL_DC] Using Preview fallback: %ux%u (no 1:8 DC)\n",
                                   info.preview.xsize, info.preview.ysize);
                        OutputDebugStringW(dbg);
                        continue;
                    }
                }
            }

            // [v8.3] Fallback Logic
            // Reached here means NO 1:8 FRAME_PROGRESSION event occurred.
            // Common reasons:
            //   1. Lossless (uses_original_profile=1) → Modular codec → NO DC layer exists
            //   2. Non-progressive lossy encoding → No progressive passes at all
            //   3. Progressive but ratio not in [4,16] range
            {
                wchar_t diagBuf[256];
                swprintf_s(diagBuf, L"[JXL_DC] No 1:8 DC. isProgressive=%d, uses_original_profile=%d, have_preview=%d, image=%ux%u\n",
                    isProgressive ? 1 : 0, info.uses_original_profile, info.have_preview,
                    info.xsize, info.ysize);
                OutputDebugStringW(diagBuf);
            }
            
            bool isSmallEnough = ((uint64_t)info.xsize * info.ysize) < 2000000; // 2MP

             // [Fix] Removed `|| forceRenderFull` from this condition. 
             // We MUST NOT decode massive JXLs into `std::vector` inside LoadThumbJXL_DC as it causes EXCEPTION_ACCESS_VIOLATION 
             // due to allocating double the RAM (e.g., 9.6GB for a 1.2Gpx image).
             // If a massive image needs a full decode, it will fall through to `LoadImageUnified` and use `ctx.allocator` directly safely.
             if (isSmallEnough) {
                  // FastLane Full Decode (or HeavyLane forced full decode within reasonable limits)
                  size_t stride = info.xsize * 4;
                  size_t bufSize = stride * info.ysize;
                  try { pData->pixels.resize(bufSize); } catch(...) { return cleanup(E_OUTOFMEMORY); }
                  
                  pData->width = (UINT)info.xsize;
                  pData->height = (UINT)info.ysize;
                  pData->stride = (UINT)stride;
                  pData->isValid = true;
                  pData->isBlurry = false; // Full decode is sharp
                  pData->loaderName = isSmallEnough ? L"libjxl (Fast Full)" : L"libjxl (Heavy Full)";

                  JxlDecoderSetImageOutBuffer(dec, &format, pData->pixels.data(), bufSize);
                  // No Flush, just continue loop
             } else if (allowFakeBase) {
                  // Too large for FastLane full decode or too massive for HeavyLane CPU decoding.
                  // We fake a 1x1 transparent pixel to satisfy the pipeline and unlock Titan Mode.
                  OutputDebugStringW(L"[JXL_DC] No 1:8 found and image too large. Faking 1x1 base layer for Region Decoder.\n");
                  
                  try {
                      pData->pixels.assign(4, 0); // 1 pixel, 4 channels (RGBA), all 0 (transparent)
                  } catch(...) { return cleanup(E_OUTOFMEMORY); }
                  
                  pData->width = 1;
                  pData->height = 1;
                  pData->stride = 4;
                  pData->isValid = true;
                  pData->isBlurry = true;
                  pData->loaderName = isProgressive ? L"libjxl (Fake Base Prog)" : L"libjxl (Fake Base)";
                  
                  return cleanup(S_OK);
             } else {
                  // Image is massive, AND we are not allowed to fake a 1x1 base (e.g. LOD extraction).
                  // We MUST NOT decode it into a std::vector.
                  // Abort the DC fast-pass and let the outer `LoadImageUnified` handle the massive Full Decode!
                  return cleanup(E_FAIL);
             }
        }
        else if (status == JXL_DEC_FULL_IMAGE) {
             if (pData->isValid) {
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
                     } else if (a == 0) r = g = b = 0;
                     p[i*4+0] = b;
                     p[i*4+1] = g;
                     p[i*4+2] = r;
                     p[i*4+3] = a;
                 }
                 return cleanup(S_OK);
             }
        }
        else if (status == JXL_DEC_PREVIEW_IMAGE) {
            if (pData->isValid) {
                uint8_t* p = pData->pixels.data();
                size_t pxCount = (size_t)pData->width * pData->height;
                for (size_t i = 0; i < pxCount; ++i) {
                    uint8_t r = p[i * 4 + 0];
                    uint8_t g = p[i * 4 + 1];
                    uint8_t b = p[i * 4 + 2];
                    uint8_t a = p[i * 4 + 3];
                    if (a < 255 && a > 0) {
                        r = (uint8_t)((r * a) / 255);
                        g = (uint8_t)((g * a) / 255);
                        b = (uint8_t)((b * a) / 255);
                    } else if (a == 0) {
                        r = g = b = 0;
                    }
                    p[i * 4 + 0] = b;
                    p[i * 4 + 1] = g;
                    p[i * 4 + 2] = r;
                    p[i * 4 + 3] = a;
                }
                return cleanup(S_OK);
            }
        }
        // Exif Box Handling
        if (readingExif && status != JXL_DEC_BOX && status != JXL_DEC_BOX_NEED_MORE_OUTPUT) {
             size_t remaining = JxlDecoderReleaseBoxBuffer(dec);
             size_t validSize = exifBuffer.size() - remaining;
             if (validSize > 0) {
                 easyexif::EXIFInfo exif;
                 if (exif.parseFromEXIFSegment(exifBuffer.data(), static_cast<unsigned>(validSize)) == 0) {
                     QuickView::PopulateMetadataFromEasyExif(exif, *pMetadata);
                 } else {
                      std::vector<unsigned char> tmp(validSize + 6);
                      memcpy(tmp.data(), "Exif\0\0", 6);
                      memcpy(tmp.data()+6, exifBuffer.data(), validSize);
                      if (exif.parseFromEXIFSegment(tmp.data(), static_cast<unsigned>(tmp.size())) == 0) {
                          QuickView::PopulateMetadataFromEasyExif(exif, *pMetadata);
                      }
                 }
             }
             readingExif = false;
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

    // 2. Init TJ (Optimized)
    struct TjCtx { 
        tjhandle h; 
        TjCtx() { h = tj3Init(TJINIT_DECOMPRESS); } 
        ~TjCtx() { if(h) tj3Destroy(h); } 
    };
    static thread_local TjCtx t_ctx;
    
    tjhandle tj = t_ctx.h;
    if (!tj) return E_FAIL;
    
    // No Guard needed
    
    // 3. Parse Header
    if (tj3DecompressHeader(tj, buf.data(), buf.size()) != 0) {
        // Don't destroy handle! 
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

// [Module B] Forward Declaration for Regex Helper
static bool GetSvgDimensions(LPCWSTR filePath, uint32_t* width, uint32_t* height);



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
         
         // FastPass is user-visible too, so do not allow Titan's fake base here.
         HRESULT hr = LoadThumbJXL_DC(buf.data(), buf.size(), pData, nullptr, false, false);
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
    else if (format == L"RAW") {
        // [v9.0] RAW Fast Pass: Use Embedded Preview
        QuickView::Codec::DecodeContext ctx;
        ctx.forcePreview = true; 
        
        ctx.allocator = [&](size_t s) -> uint8_t* {
             try { pData->pixels.resize(s); return pData->pixels.data(); }
             catch (...) { return nullptr; }
        };
        
        QuickView::Codec::DecodeResult res;
        HRESULT hr = QuickView::Codec::RawCodec::Load(filePath, ctx, res);
        
        if (SUCCEEDED(hr)) {
             pData->width = res.width;
             pData->height = res.height;
             pData->stride = res.stride;
             pData->isValid = true;
             pData->loaderName = res.metadata.LoaderName; 
             pData->isBlurry = false; // Previews are reasonably sharp
             return S_OK;
        }
        // If fail, fall through to E_FAIL (HeavyLane will pick it up)
        return E_FAIL;
    }
    
    // [Module B] Fast SVG Path
    else if (format == L"SVG") {
        using namespace QuickView;
        RawImageFrame* pFrame = new RawImageFrame();
        
        // Smart Sizing (Delegate to LoadToFrame logic via -1)
        
        // We restore w, h variables because they are used later for pData->origWidth
        uint32_t w = 0, h = 0;
        GetSvgDimensions(filePath, &w, &h);
        
        int targetW = -1;
        int targetH = -1;
        
        // Use LoadToFrame (Hybrid D2D Path) on Current Thread
        HRESULT hr = LoadToFrame(filePath, pFrame, nullptr, targetW, targetH, &pData->loaderName, nullptr, nullptr);
        
        if (SUCCEEDED(hr)) {
             pData->width = pFrame->width;
             pData->height = pFrame->height;
             pData->stride = pFrame->stride;
             pData->origWidth = (w > 0) ? w : pFrame->width;
             pData->origHeight = (h > 0) ? h : pFrame->height;
             
             size_t size = pData->stride * pData->height;
             pData->pixels.resize(size);
             if (pFrame->pixels) memcpy(pData->pixels.data(), pFrame->pixels, size);
             
             // pData->format = DXGI_FORMAT_B8G8R8A8_UNORM; // Not a member
             
             pData->isValid = true;
             pData->isBlurry = false;
        }
        
        pFrame->Release();
        delete pFrame;
        return hr;
    }

    return E_FAIL;
}


// [Optimization] Fast Multi-Replacement Utility
// Avoids O(N^2) complexity of iterative std::string::replace by using single-pass construction
template <typename Container>
static void MultiReplace(std::string& str, const Container& replacements) {
    if (replacements.empty() || str.empty()) return;

    for (auto const& [oldVal, newVal] : replacements) {
        if (oldVal.empty()) continue;

        size_t firstMatch = str.find(oldVal);
        if (firstMatch == std::string::npos) continue;

        std::string result;
        // Pre-allocate to avoid reallocations
        result.reserve(str.size() + (newVal.size() > oldVal.size() ? 1024 : 0));

        // Append part before first match
        result.append(str, 0, firstMatch);
        result.append(newVal);

        size_t pos = firstMatch + oldVal.length();
        size_t matchPos;
        while ((matchPos = str.find(oldVal, pos)) != std::string::npos) {
            result.append(str, pos, matchPos - pos);
            result.append(newVal);
            pos = matchPos + oldVal.length();
        }

        result.append(str, pos, str.length() - pos);
        str = std::move(result);
    }
}

// Helper: Parse SVG dimensions using Regex (Header only)
static bool GetSvgDimensions(LPCWSTR filePath, uint32_t* width, uint32_t* height) {
    if (!width || !height) return false;
    
    // Read first 4KB (usually contains header)
    HANDLE hFile = CreateFileW(filePath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    
    char buffer[4096];
    DWORD bytesRead = 0;
    ReadFile(hFile, buffer, sizeof(buffer), &bytesRead, nullptr);
    CloseHandle(hFile);
    
    if (bytesRead == 0) return false;
    
    std::string header(buffer, bytesRead);
    
    // 1. Try width="..." height="..."
    auto GetAttrVal = [](const std::string& s, const char* attr) -> std::string {
        std::string key = attr;
        key += "=\"";
        size_t pos = s.find(key);
        if (pos == std::string::npos) return "";
        pos += key.length();
        size_t end = s.find("\"", pos);
        if (end == std::string::npos) return "";
        return s.substr(pos, end - pos);
    };

    float w = 0, h = 0;
    std::string wStr = GetAttrVal(header, "width");
    std::string hStr = GetAttrVal(header, "height");
    if (!wStr.empty()) try { w = std::stof(wStr); } catch (...) {}
    if (!hStr.empty()) try { h = std::stof(hStr); } catch (...) {}

    if (w > 0 && h > 0) {
        *width = (uint32_t)std::lround(w);
        *height = (uint32_t)std::lround(h);
        return true;
    }

    std::string vbStr = GetAttrVal(header, "viewBox");
    if (!vbStr.empty()) {
        size_t p = 0;
        int skipped = 0;
        while (skipped < 2 && p < vbStr.length()) {
            if (isspace((unsigned char)vbStr[p]) || vbStr[p] == ',') {
                while (p < vbStr.length() && (isspace((unsigned char)vbStr[p]) || vbStr[p] == ',')) p++;
                skipped++;
            } else p++;
        }
        if (p < vbStr.length()) {
            char* endptr = nullptr;
            float vw = strtof(vbStr.c_str() + p, &endptr);
            if (endptr && endptr > vbStr.c_str() + p) {
                float vh = strtof(endptr, nullptr);
                if (vw > 0 && vh > 0) {
                    *width = (uint32_t)std::lround(vw);
                    *height = (uint32_t)std::lround(vh);
                    return true;
                }
            }
        }
    }
    
    return false;
}

// Helper: Parse AVIF dimensions using libavif (Header only)
static bool GetAVIFDimensions(LPCWSTR filePath, uint32_t* width, uint32_t* height) {
    if (!width || !height) return false;
    
    // [v6.9.4] Robust Probe: Read FULL file if small (<1MB) to ensure 'moov'/'meta' are found.
    // Otherwise read 64KB probe (increased from 16KB).
    HANDLE hFile = CreateFileW(filePath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    
    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFile, &fileSize)) {
        CloseHandle(hFile);
        return false;
    }
    
    DWORD bytesToRead = 65536; // Default Probe 64KB
    if (fileSize.QuadPart < 1024 * 1024) {
        bytesToRead = (DWORD)fileSize.QuadPart;
    }
    
    std::vector<uint8_t> buffer(bytesToRead);
    DWORD bytesRead = 0;
    ReadFile(hFile, buffer.data(), bytesToRead, &bytesRead, nullptr);
    CloseHandle(hFile);
    
    if (bytesRead < 100) return false;

    avifDecoder* decoder = avifDecoderCreate();
    if (!decoder) return false;
    
    bool success = false;
    if (avifDecoderSetIOMemory(decoder, buffer.data(), bytesRead) == AVIF_RESULT_OK) {
        // [v6.9] Robust Header Parsing
        decoder->strictFlags = AVIF_STRICT_DISABLED;
        decoder->ignoreXMP = AVIF_TRUE;
        decoder->ignoreExif = AVIF_TRUE;
        
        if (avifDecoderParse(decoder) == AVIF_RESULT_OK) {
            *width = decoder->image->width;
            *height = decoder->image->height;
            success = true;
        }
    }
    avifDecoderDestroy(decoder);
    return success;
}

// Helper: Read Big-Endian uint32
static uint32_t ReadBE32(const uint8_t* p) {
    return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

// Helper: Parse ISOBMFF to find HEIC/AVIF dimensions (ispe box)
// [v9.8] Custom Parser. Robust fallback for both HEIC and AVIF when native decoders fail to parse header.
static bool GetISOBMFFDimensions(LPCWSTR filePath, uint32_t* width, uint32_t* height) {
    if (!width || !height) return false;
    
    HANDLE hFile = CreateFileW(filePath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    
    // Read 64KB. Typically enough to reach 'ispe' in 'ipco'.
    DWORD bytesToRead = 65536; 
    std::vector<uint8_t> buffer(bytesToRead);
    DWORD bytesRead = 0;
    
    if (!ReadFile(hFile, buffer.data(), bytesToRead, &bytesRead, nullptr)) {
        CloseHandle(hFile);
        return false;
    }
    CloseHandle(hFile);
    
    if (bytesRead < 12) return false;
    
    const uint8_t* p = buffer.data();
    const uint8_t* end = p + bytesRead;
    
    // Simple robust scan: Look for 'meta' -> 'iprp' -> 'ipco' -> 'ispe'
    // Recursive scanning is best, but for 64KB head, we can just linear scan top-level and recurse or Flatten.
    // For simplicity and robustness against malformed offsets, we will look for 'ipco' then 'ispe'.
    // Actually, simply scanning for ALL 'ispe' boxes within the buffer is fairly safe because 'ispe' shouldn't appear randomly.
    // But to be correct, let's parse boxes.

    const uint8_t* ptr = buffer.data();
    // Helper to peek boxes
    uint64_t maxPixels = 0;
    uint32_t maxW = 0;
    uint32_t maxH = 0;

    auto parse_boxes = [&](auto&& self, const uint8_t* start, const uint8_t* end) -> void {
        const uint8_t* curr = start;
        while (curr + 8 <= end) {
            uint32_t size = ReadBE32(curr);
            uint32_t type = ReadBE32(curr + 4);
            
            if (size == 1) { // 64-bit large size
                 if (curr + 16 > end) break;
                 uint64_t largeSize = ((uint64_t)ReadBE32(curr + 8) << 32) | ReadBE32(curr + 12);
                 // We can't really skip vast sizes in a 64KB buffer, so we treat it as container boundary or skip.
                 // For header parsing, we usually don't need 64-bit boxes unless it's mdat.
                 // Just check type.
                 if (type == 0x6970636f) { // ipco
                     self(self, curr + 16, end); // Recurse into body (truncated to buffer end)
                 } else if (type == 0x6d657461 || type == 0x69707270) { // meta, iprp
                     self(self, curr + 16, end);
                 }
                 // Skip
                 curr += (largeSize > 0xFFFFFFFF) ? 0xFFFFFFFF : (size_t)largeSize; 
                 // (Actually pointer math might wrap/overflow if wrong, but we check bounds in loop)
                 continue; // Break inner loop if overflow?
            } else if (size < 8) break; // Invalid
            
            // Boundary check
            if (curr + size > end) {
                // Partial box? If it's a container we care about, we might still parse what we have.
                // If it's ipco/meta/iprp, proceed.
                if (type == 0x6970636f || type == 0x6d657461 || type == 0x69707270) {
                     // Recurse into the rest of buffer
                     self(self, curr + 8, end);
                }
                break;
            }
            
            if (type == 0x69737065) { // ispe
                if (size >= 20) {
                     uint32_t w = ReadBE32(curr + 12);
                     uint32_t h = ReadBE32(curr + 16);
                     uint64_t px = (uint64_t)w * h;
                     if (px > maxPixels) {
                         maxPixels = px; maxW = w; maxH = h;
                     }
                }
            }
            else if (type == 0x6d657461) { // meta
                self(self, curr + 8 + 4, curr + size); // meta often has 4 byte version/flags after header
            }
            else if (type == 0x69707270 || type == 0x6970636f) { // iprp, ipco
                self(self, curr + 8, curr + size);
            }
            
            curr += size;
        }
    };
    
    parse_boxes(parse_boxes, p, end);
    
    if (maxPixels > 0) {
        *width = maxW;
        *height = maxH;
        return true;
    }
    
    return false;
}


    

    
    // Simple robust scan: Look for 'meta' -> 'iprp' -> 'ipco' -> 'ispe'
    

    

    

    


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
    
    // [v9.3] Use extension-first format detection (handles RAW correctly)
    result.format = DetectFormatFromContent(filePath);
    if (result.format == L"Unknown") return result;
    
    // Try to get dimensions from fast header parsing
    ImageInfo info;
    if (SUCCEEDED(GetImageInfoFast(filePath, &info))) {
        result.width = info.width;
        result.height = info.height;
    }
    
    // [v6.6 Fix] AVIF/HEIC: Try to get real dimensions using libavif (for AVIF) or ISOBMFF parser (for HEIC)
    if (result.width == 0) {
        if (result.format == L"AVIF") {
            uint32_t w = 0, h = 0;
            // Try native AVIF decoder first (supports some files)
            if (GetAVIFDimensions(filePath, &w, &h)) {
                result.width = w;
                result.height = h;
            }
            // Fallback: Generic ISOBMFF parser (handles .avifs or other containers where libavif might fail)
            else if (GetISOBMFFDimensions(filePath, &w, &h)) {
                result.width = w;
                result.height = h;
            }
        }
        else if (result.format == L"HEIC") {
            uint32_t w = 0, h = 0;
            if (GetISOBMFFDimensions(filePath, &w, &h)) {
                result.width = w;
                result.height = h;
            }
        }
    }
    
    // Check for embedded thumbnail (RAW files always have, JPEG may have)
    result.hasEmbeddedThumb = (result.format == L"RAW" || result.format == L"TIFF");
    if (result.format == L"JPEG" && result.fileSize > 100 * 1024) { // >100KB JPEG likely has Exif
        result.hasEmbeddedThumb = true;
    }
    
    // === Classification Matrix (v3.2) ===
    int64_t pixels = result.GetPixelCount();
    
    // Type A (Express Lane ONLY): Small enough for fast full decode
    // Type B (Heavy Lane): Large images routed to Heavy Lane (full decode for non-Titan)
    
    // [v4.1] JXL Optimization: Always allow Sprint (Scout Lane)
    // Small JXL -> Fast Full Decode (via fallback if needed? Currently DC only in LoadFastPass)
    // Large JXL -> Fast DC Preview (LoadThumbJXL_DC)
    if (result.format == L"JXL") {
        // [v9.3] Fix: Large JXL should be TypeB (Heavy) to align with Dispatch properties.
        // Previously unconditional TypeA caused FastLane Choke on 89MP images.
        bool isSmall = (result.fileSize < 1048576) && (pixels < 2000000);
        
        if (isSmall) {
            result.type = ImageType::TypeA_Sprint;
        } else {
            result.type = ImageType::TypeB_Heavy;
        }
    }
    else if (result.format == L"JPEG" || result.format == L"BMP") {
        // JPEG/BMP: ≤8.5MP → Express (full decode is fast)
        // >8.5MP → Heavy (Scout may extract thumb if hasEmbeddedThumb)
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
        // [v9.1] Smart RAW Dispatch: Based on Embedded Preview Size
        result.hasEmbeddedThumb = true;
        
        // Try to get embedded preview dimensions
        int previewW = 0, previewH = 0;
        if (SUCCEEDED(GetEmbeddedPreviewInfo(filePath, &previewW, &previewH))) {
            result.embeddedPreviewWidth = previewW;
            result.embeddedPreviewHeight = previewH;
            result.embeddedPreviewIsJpeg = true; // LibRaw unpack_thumb usually returns JPEG
            
            int64_t previewPixels = (int64_t)previewW * previewH;
            
            // Dispatch Logic:
            // - Small preview (< 2MP, e.g. 160x120 thumb): FastLane (fast extraction)
            // - Large preview (>= 2MP, e.g. 6000x4000 full JPEG): HeavyLane (TurboJPEG decode)
            if (previewPixels > 0 && previewPixels < 2'000'000) {
                result.type = ImageType::TypeA_Sprint;
            } else {
                result.type = ImageType::TypeB_Heavy;
            }
        } else {
            // No embedded preview or extraction failed -> HeavyLane (LibRaw half-size)
            result.type = ImageType::TypeB_Heavy;
        }
    }
    else if (result.format == L"AVIF" || result.format == L"HEIC") {
        // AVIF/HEIC: ≤4MP → Express Fast Pass
        // [v6.6] Safety: If pixels == 0 (Unknown), Assume Large (Heavy Lane)
        // This prevents huge AVIFs (where header Peek failed) from crashing FastLane.
        if (pixels > 0 && pixels <= 4000000) {
            result.type = ImageType::TypeA_Sprint;
        } else {
            result.type = ImageType::TypeB_Heavy;
        }
    }
    else if (result.format == L"SVG") {
        // [Module B] Smart Dispatch & Measure
        // 1. Extract Dimensions (Regex)
        uint32_t w = 0, h = 0;
        if (GetSvgDimensions(filePath, &w, &h)) {
             result.width = w;
             result.height = h;
        } else {
             // Fallback
             result.width = 512;
             result.height = 512;
        }
        
        // 2. Dispatch Logic
        // <= 512KB: FastLane (In-place Render)
        // > 512KB: HeavyLane (Background Render)
        if (result.fileSize <= 512 * 1024) {
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


HRESULT CImageLoader::LoadToFrame(LPCWSTR filePath, QuickView::RawImageFrame* outFrame,
                                   QuantumArena* arena,
                                   int targetWidth, int targetHeight,
                                   std::wstring* pLoaderName,
                                   CancelPredicate checkCancel,
                                   ImageMetadata* pMetadata,
                                   bool allowFakeBase,
                                   bool isTitanMode,
                                   float targetHdrHeadroomStops) {
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
    ctx.forceRenderFull = true; // [v10] Ensure HeavyLane base layer decode does not abort for large non-DC JXLs
    ctx.allowFakeBase = allowFakeBase; // [v10.1] Pass the fallback behavior override
    ctx.isTitanMode = isTitanMode;
    ctx.targetHdrHeadroomStops = targetHdrHeadroomStops;

    DecodeResult res;
    HRESULT hrUnified = LoadImageUnified(filePath, ctx, res);

    if (hrUnified == E_ABORT || hrUnified == E_OUTOFMEMORY) return hrUnified;
    if (SUCCEEDED(hrUnified)) {
        // [v5.0] Extract loader name from unified result
        if (pLoaderName && !res.metadata.LoaderName.empty()) {
            *pLoaderName = res.metadata.LoaderName;
        }

        if (pMetadata) {
            *pMetadata = res.metadata;
            // [v5.3 Fix] Ensure FileSize is populated for Info Panel "Disk" row
            if (pMetadata->FileSize == 0) {
                 PopulateFileStats(filePath, pMetadata);
            }
            // [Fix] Final Guarantee: Format must not be empty for extension mismatch check
            if (pMetadata->Format.empty()) {
                pMetadata->Format = DetectFormatFromContent(filePath);
            }
        }
        
        // [Fix] Software Downscale if Unified Codec ignored target dimensions
        if (targetWidth > 0 && targetHeight > 0 && 
           ((int)res.width > targetWidth || (int)res.height > targetHeight)) {
            
            // Calculate proportional scale to fit within target
            float scaleW = (float)targetWidth / res.width;
            float scaleH = (float)targetHeight / res.height;
            float scale = (std::min)(scaleW, scaleH);
            
            int finalW = (int)(res.width * scale + 0.5f);
            int finalH = (int)(res.height * scale + 0.5f);
            if (finalW < 1) finalW = 1;
            if (finalH < 1) finalH = 1;
            
            int finalStride = CalculateAlignedStride(finalW, 4);
            size_t finalSize = (size_t)finalStride * finalH;
            uint8_t* resizedPixels = AllocateBuffer(finalSize);
            
            if (resizedPixels) {
                // Perform Resize
                SIMDUtils::ResizeBilinear(res.pixels, res.width, res.height, res.stride, 
                                          resizedPixels, finalW, finalH, finalStride);
                                          
                // Free the original huge buffer
                if (arena && arena->Owns(res.pixels)) {
                    // It will be reclaimed when arena resets
                } else {
                    _aligned_free(res.pixels);
                }
                
                res.pixels = resizedPixels;
                res.width = finalW;
                res.height = finalH;
                res.stride = finalStride;
                // res.metadata.FormatDetails += L" (SwRescaled)";
            }
        }
        
        outFrame->pixels = res.pixels;
        outFrame->width = res.width;
        outFrame->height = res.height;
        outFrame->stride = res.stride;
        outFrame->format = res.format;
        // [v5.4] Metadata
        outFrame->formatDetails = res.metadata.FormatDetails;
        outFrame->exifOrientation = res.metadata.ExifOrientation; // [v8.7] Propagate Orientation
        outFrame->iccProfile.assign(res.metadata.iccProfileData.begin(), res.metadata.iccProfileData.end());

        outFrame->is_sRGB = res.metadata.is_sRGB;
        outFrame->is_Linear_sRGB = res.metadata.is_Linear_sRGB;
        outFrame->hdrMetadata = res.metadata.hdrMetadata;
        
        // [v10.1] Preserve original resolution if frame is scaled
        if (res.metadata.Width > 0 && res.metadata.Height > 0 &&
            (res.metadata.Width != (UINT)outFrame->width || res.metadata.Height != (UINT)outFrame->height)) {
            outFrame->srcWidth = (int)res.metadata.Width;
            outFrame->srcHeight = (int)res.metadata.Height;
        }
        
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

            // Hybrid D2D SVG Path (D3D11 WARP Device Architecture)
            // Debugging: Added detailed HRESULT logs.

            HRESULT hr = S_OK;
            
            // [Fix] Universal SVG Sanitizer & Style Inliner
            // Issues: 
            // 1. Direct2D fails on non-ASCII IDs (Paint Server Resolution Failure).
            // 2. Direct2D ignores <style> in <defs>, causing missing fills (Black Silhouette/Invisibility).
            if (!fileData.empty()) {
                std::string svgContent(fileData.begin(), fileData.end());

                // =========================================================
                // PHASE 1: ID SANITIZATION (Fix Broken Links)
                // =========================================================
                try {
                    std::map<std::string, std::string> replacements;
                    int count = 0;
                    
                    size_t pos_id = 0;
                    while ((pos_id = svgContent.find("id=\"", pos_id)) != std::string::npos) {
                        pos_id += 4;
                        size_t end_id = svgContent.find("\"", pos_id);
                        if (end_id == std::string::npos) break;
                        
                        std::string val = svgContent.substr(pos_id, end_id - pos_id);
                        pos_id = end_id + 1;
                        
                        bool unsafe = false;
                        for (unsigned char c : val) {
                            if (c > 127) { unsafe = true; break; }
                        }
                        // Also consider spaces or weird symbols as unsafe for D2D validation
                        if (!unsafe && val.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.") != std::string::npos) {
                             unsafe = true; 
                        }

                        if (unsafe && replacements.find(val) == replacements.end()) {
                             char buf[64];
                             sprintf_s(buf, "qv_fix_%d", count++);
                             replacements[val] = std::string(buf);
                        }
                    }
                    
                    // Apply ID Replacements
                    if (!replacements.empty()) {
                        MultiReplace(svgContent, replacements);
                        wchar_t msg[128];
                        swprintf_s(msg, L"[SVG] Sanitized %d Unsafe IDs.\n", (int)replacements.size());
                        OutputDebugStringW(msg);
                    }
                } catch (...) {
                    OutputDebugStringW(L"[SVG] ID Sanitizer Pattern Failure\n");
                }

                // =========================================================
                // PHASE 2: STYLE INLINER (Fix Black Silhouette)
                // =========================================================
                // D2D ignores <style> in <defs>. We extract CSS rules from
                // <style> blocks and inline fill/stroke directly onto elements.
                // Uses string_view zero-copy parsing. No std::regex.
                // =========================================================
                try {
                    int inlinedCount = 0;
                    
                    // --- Utility lambdas (string_view based) ---
                    auto SkipWS = [](std::string_view sv, size_t p) -> size_t {
                        while (p < sv.size() && (sv[p] == ' ' || sv[p] == '\t' || sv[p] == '\n' || sv[p] == '\r')) ++p;
                        return p;
                    };
                    auto TrimSV = [](std::string_view sv) -> std::string_view {
                        size_t s = 0, e = sv.size();
                        while (s < e && (sv[s] == ' ' || sv[s] == '\t' || sv[s] == '\n' || sv[s] == '\r')) ++s;
                        while (e > s && (sv[e-1] == ' ' || sv[e-1] == '\t' || sv[e-1] == '\n' || sv[e-1] == '\r')) --e;
                        return sv.substr(s, e - s);
                    };
                    // Extract "prop: value" from a CSS body string_view
                    auto ExtractCSSProp = [&](std::string_view body, const char* prop) -> std::string_view {
                        size_t propLen = strlen(prop);
                        size_t p = 0;
                        while (p < body.size()) {
                            size_t found = body.find(prop, p);
                            if (found == std::string_view::npos) return {};
                            // Ensure it's at property boundary (start of body or after ; or whitespace)
                            bool atBound = (found == 0) || body[found - 1] == ';' ||
                                           body[found - 1] == ' ' || body[found - 1] == '\t' ||
                                           body[found - 1] == '\n' || body[found - 1] == '\r' ||
                                           body[found - 1] == '{';
                            size_t afterKey = found + propLen;
                            if (atBound && afterKey < body.size() && body[afterKey] == ':') {
                                size_t valStart = afterKey + 1;
                                // Skip whitespace after ':'
                                while (valStart < body.size() && (body[valStart] == ' ' || body[valStart] == '\t')) ++valStart;
                                size_t valEnd = body.find(';', valStart);
                                if (valEnd == std::string_view::npos) valEnd = body.size();
                                return TrimSV(body.substr(valStart, valEnd - valStart));
                            }
                            p = found + 1;
                        }
                        return {};
                    };
                    // Check if a multi-class attribute value contains a specific class token
                    auto HasClassToken = [](std::string_view classVal, std::string_view token) -> bool {
                        size_t p = 0;
                        while (p < classVal.size()) {
                            // Skip leading whitespace
                            while (p < classVal.size() && (classVal[p] == ' ' || classVal[p] == '\t')) ++p;
                            if (p >= classVal.size()) return false;
                            size_t end = p;
                            while (end < classVal.size() && classVal[end] != ' ' && classVal[end] != '\t') ++end;
                            if (classVal.substr(p, end - p) == token) return true;
                            p = end;
                        }
                        return false;
                    };
                    
                    // Collected CSS rules: className -> {fill, stroke}
                    struct CSSRule { std::string fill; std::string stroke; };
                    std::map<std::string, CSSRule> cssRules;
                    
                    // --- Pass 1: Parse all <style> blocks ---
                    size_t styleSearch = 0;
                    while (true) {
                        size_t styleStart = svgContent.find("<style", styleSearch);
                        if (styleStart == std::string::npos) break;
                        // Find end of opening tag '>'
                        size_t tagClose = svgContent.find('>', styleStart);
                        if (tagClose == std::string::npos) break;
                        size_t contentStart = tagClose + 1;
                        size_t styleEnd = svgContent.find("</style>", contentStart);
                        if (styleEnd == std::string::npos) styleEnd = svgContent.find("</style", contentStart);
                        if (styleEnd == std::string::npos) break;
                        
                        std::string_view styleBlock(svgContent.data() + contentStart, styleEnd - contentStart);
                        
                        // Parse CSS rules within this <style> block
                        size_t rp = 0;
                        while (rp < styleBlock.size()) {
                            // Find '.' that starts a class selector
                            size_t dot = styleBlock.find('.', rp);
                            if (dot == std::string_view::npos) break;
                            
                            size_t nameStart = dot + 1;
                            if (nameStart >= styleBlock.size()) break;
                            
                            // Validate: class name must start with letter, underscore, or hyphen
                            char firstCh = styleBlock[nameStart];
                            if (!isalpha((unsigned char)firstCh) && firstCh != '_' && firstCh != '-') {
                                rp = nameStart;
                                continue;
                            }
                            
                            // Read class name (alphanumeric, underscore, hyphen)
                            size_t nameEnd = nameStart;
                            while (nameEnd < styleBlock.size()) {
                                char c = styleBlock[nameEnd];
                                if (isalnum((unsigned char)c) || c == '_' || c == '-') ++nameEnd;
                                else break;
                            }
                            if (nameEnd == nameStart) { rp = nameEnd; continue; }
                            
                            std::string_view clsName = styleBlock.substr(nameStart, nameEnd - nameStart);
                            
                            // Find opening brace
                            size_t bo = styleBlock.find('{', nameEnd);
                            if (bo == std::string_view::npos) break;
                            size_t bc = styleBlock.find('}', bo);
                            if (bc == std::string_view::npos) break;
                            
                            std::string_view body = styleBlock.substr(bo + 1, bc - bo - 1);
                            
                            // Extract fill and stroke
                            std::string_view fillSV = ExtractCSSProp(body, "fill");
                            std::string_view strokeSV = ExtractCSSProp(body, "stroke");
                            
                            if (!fillSV.empty() || !strokeSV.empty()) {
                                std::string key(clsName);
                                auto& rule = cssRules[key];
                                if (!fillSV.empty() && rule.fill.empty()) rule.fill = std::string(fillSV);
                                if (!strokeSV.empty() && rule.stroke.empty()) rule.stroke = std::string(strokeSV);
                            }
                            
                            rp = bc + 1;
                        }
                        
                        styleSearch = styleEnd + 8; // skip "</style>"
                    }
                    
                    // --- Pass 2: Inline extracted rules onto elements ---
                    if (!cssRules.empty()) {
                        // Build injection map: for each class, build the attribute prefix string
                        // e.g. fill="url(#ID)" stroke="#000" 
                        struct Injection { std::string className; std::string attrPrefix; };
                        std::vector<Injection> injections;
                        injections.reserve(cssRules.size());
                        for (auto& [cls, rule] : cssRules) {
                            std::string prefix;
                            if (!rule.fill.empty()) { prefix += "fill=\""; prefix += rule.fill; prefix += "\" "; }
                            if (!rule.stroke.empty()) { prefix += "stroke=\""; prefix += rule.stroke; prefix += "\" "; }
                            if (!prefix.empty()) {
                                injections.push_back({cls, std::move(prefix)});
                            }
                        }
                        
                        // Scan for class= attributes and inject
                        // We rebuild the string once, processing all class= occurrences
                        std::string result;
                        result.reserve(svgContent.size() + svgContent.size() / 8);
                        size_t lastCopy = 0;
                        size_t scanPos = 0;
                        
                        while (scanPos < svgContent.size()) {
                            // Find "class" keyword
                            size_t classPos = svgContent.find("class", scanPos);
                            if (classPos == std::string::npos) break;
                            
                            // Skip whitespace and '='
                            size_t eq = SkipWS(svgContent, classPos + 5);
                            if (eq >= svgContent.size() || svgContent[eq] != '=') {
                                scanPos = classPos + 5;
                                continue;
                            }
                            size_t qStart = SkipWS(svgContent, eq + 1);
                            if (qStart >= svgContent.size()) break;
                            
                            // Determine quote character (" or ' or none)
                            char quote = svgContent[qStart];
                            size_t valStart, valEnd;
                            if (quote == '"' || quote == '\'') {
                                valStart = qStart + 1;
                                valEnd = svgContent.find(quote, valStart);
                                if (valEnd == std::string::npos) { scanPos = valStart; continue; }
                            } else {
                                // No quote: value runs until whitespace or '>' or '/'
                                valStart = qStart;
                                valEnd = valStart;
                                while (valEnd < svgContent.size() && svgContent[valEnd] != ' ' && 
                                       svgContent[valEnd] != '\t' && svgContent[valEnd] != '>' && 
                                       svgContent[valEnd] != '/' && svgContent[valEnd] != '\n') ++valEnd;
                            }
                            
                            std::string_view classVal(svgContent.data() + valStart, valEnd - valStart);
                            
                            // Check each injection rule
                            std::string prefixToInject;
                            for (auto& inj : injections) {
                                if (HasClassToken(classVal, inj.className)) {
                                    prefixToInject += inj.attrPrefix;
                                    inlinedCount++;
                                }
                            }
                            
                            if (!prefixToInject.empty()) {
                                // Copy everything up to "class", inject prefix, then continue
                                result.append(svgContent, lastCopy, classPos - lastCopy);
                                result.append(prefixToInject);
                                // Copy the class attribute itself unchanged
                                size_t attrEnd = (quote == '"' || quote == '\'') ? valEnd + 1 : valEnd;
                                result.append(svgContent, classPos, attrEnd - classPos);
                                lastCopy = attrEnd;
                            }
                            
                            scanPos = (quote == '"' || quote == '\'') ? valEnd + 1 : valEnd;
                        }
                        
                        if (inlinedCount > 0) {
                            result.append(svgContent, lastCopy, svgContent.size() - lastCopy);
                            svgContent = std::move(result);
                        }
                    }
                    
                    if (inlinedCount > 0) {
                         wchar_t msg[128];
                         swprintf_s(msg, L"[SVG] Inlined %d Styles (Fixed Black Silhouette).\n", inlinedCount);
                         OutputDebugStringW(msg);
                    }
                } catch (...) {
                    OutputDebugStringW(L"[SVG] Style Inliner Failure\n");
                }
                
                // Commit changes
                fileData.assign(svgContent.begin(), svgContent.end());
            }

            // [D2D Native] Simplified Loading: Store XML only
            // =========================================================
            // No rendering here! We just pass the sanitized XML to UI thread.
            // =========================================================

            // 2. Parse Dimensions (Manual Search)
            float svgW = 512.0f;
            float svgH = 512.0f;
            
            std::string svgContent(fileData.begin(), fileData.end());

            try {
                auto GetAttrVal = [](const std::string& s, const char* attr) -> std::string {
                    std::string key = attr;
                    key += "=\"";
                    size_t pos = s.find(key);
                    if (pos == std::string::npos) return "";
                    pos += key.length();
                    size_t end = s.find("\"", pos);
                    if (end == std::string::npos) return "";
                    return s.substr(pos, end - pos);
                };

                float pw = 0, ph = 0;
                std::string wStr = GetAttrVal(svgContent, "width");
                std::string hStr = GetAttrVal(svgContent, "height");
                if (!wStr.empty()) try { pw = std::stof(wStr); } catch (...) {}
                if (!hStr.empty()) try { ph = std::stof(hStr); } catch (...) {}
                
                if (pw > 0 && ph > 0) {
                    svgW = pw;
                    svgH = ph;
                } else {
                    std::string vbStr = GetAttrVal(svgContent, "viewBox");
                    if (!vbStr.empty()) {
                        // viewBox="x y w h" - need w and h (3rd and 4th)
                        size_t p = 0;
                        int skipped = 0;
                        while (skipped < 2 && p < vbStr.length()) {
                            if (isspace((unsigned char)vbStr[p]) || vbStr[p] == ',') {
                                while (p < vbStr.length() && (isspace((unsigned char)vbStr[p]) || vbStr[p] == ',')) p++;
                                skipped++;
                            } else p++;
                        }
                        if (p < vbStr.length()) {
                            char* endptr = nullptr;
                            float vw = strtof(vbStr.c_str() + p, &endptr);
                            if (endptr && endptr > vbStr.c_str() + p) {
                                float vh = strtof(endptr, nullptr);
                                if (vw > 0 && vh > 0) {
                                    svgW = vw; svgH = vh;
                                }
                            }
                        }
                    }
                }
            } catch (...) {
                OutputDebugStringW(L"[SVG] Dimension parse failed, using default\n");
            }
            
            if (svgW <= 0) svgW = 512;
            if (svgH <= 0) svgH = 512;

            // 3. Populate RawImageFrame
            outFrame->format = PixelFormat::SVG_XML;
            outFrame->width = (int)std::lround(svgW);
            outFrame->height = (int)std::lround(svgH);
            outFrame->stride = 0; // No pixels
            outFrame->pixels = nullptr;
            
            // Allocate SvgData
            outFrame->svg = std::make_unique<RawImageFrame::SvgData>();
            outFrame->svg->xmlData.assign(svgContent.begin(), svgContent.end());
            outFrame->svg->viewBoxW = svgW;
            outFrame->svg->viewBoxH = svgH;
             
            if (pLoaderName) *pLoaderName = L"SVG XML";
            if (pMetadata) {
                pMetadata->LoaderName = L"SVG XML"; 
                pMetadata->Format = L"SVG"; // [Fix] Transparency detection depends on this!
                pMetadata->FormatDetails = L"Vector (Native)";
                pMetadata->Width = (UINT)svgW;
                pMetadata->Height = (UINT)svgH;
            }
            outFrame->formatDetails = L"SVG"; // [Fix] Ensure frame carries this info too
            
            return S_OK;
        } // End if (isSvg)
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
    HRESULT hr = LoadToMemory(filePath, &wicBitmap, &loaderName, false, checkCancel, targetWidth, targetHeight);
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
    
    // [Fix] WIC Fallback Software Downscale
    bool needWicResize = targetWidth > 0 && targetHeight > 0 && 
                        ((int)wicWidth > targetWidth || (int)wicHeight > targetHeight);

    int finalW = (int)wicWidth;
    int finalH = (int)wicHeight;
    
    if (needWicResize) {
        float scaleW = (float)targetWidth / wicWidth;
        float scaleH = (float)targetHeight / wicHeight;
        float scale = (std::min)(scaleW, scaleH);
        
        finalW = (int)(wicWidth * scale + 0.5f);
        finalH = (int)(wicHeight * scale + 0.5f);
        if (finalW < 1) finalW = 1;
        if (finalH < 1) finalH = 1;
    }

    WICPixelFormatGUID outWicFormat;
    wicBitmap->GetPixelFormat(&outWicFormat);
    bool isFloat = IsEqualGUID(outWicFormat, GUID_WICPixelFormat128bppRGBAFloat);
    int bpp = isFloat ? 16 : 4;
    QuickView::PixelFormat outPixelFormat = isFloat ? QuickView::PixelFormat::R32G32B32A32_FLOAT : QuickView::PixelFormat::BGRA8888;

    // Allocate output buffer with aligned stride
    int outStride = CalculateSIMDAlignedStride(finalW, bpp);
    size_t outSize = static_cast<size_t>(outStride) * finalH;
    uint8_t* pixels = AllocateBuffer(outSize);
    if (!pixels) return E_OUTOFMEMORY;
    
    if (needWicResize) {
        // Resize directly from WIC memory lock
        if (isFloat) {
            // Very simple fallback for resizing float buffers if needed.
            // Ideally SIMDUtils should support it, but for WIC fallback resize we can just nearest-neighbor or skip.
            // Using a simple row/col mapping for floats:
            float* dst = (float*)pixels;
            float* src = (float*)wicData;
            for (int y = 0; y < finalH; ++y) {
                int srcY = y * wicHeight / finalH;
                for (int x = 0; x < finalW; ++x) {
                    int srcX = x * wicWidth / finalW;
                    int dstIdx = y * (outStride/4) + x * 4;
                    int srcIdx = srcY * (wicStride/4) + srcX * 4;
                    dst[dstIdx+0] = src[srcIdx+0];
                    dst[dstIdx+1] = src[srcIdx+1];
                    dst[dstIdx+2] = src[srcIdx+2];
                    dst[dstIdx+3] = src[srcIdx+3];
                }
            }
        } else {
            SIMDUtils::ResizeBilinear(wicData, wicWidth, wicHeight, wicStride,
                                      pixels, finalW, finalH, outStride);
        }
    } else {
        // Copy row by row (handles stride mismatch)
        for (UINT y = 0; y < wicHeight; ++y) {
            memcpy(pixels + y * outStride, wicData + y * wicStride, wicWidth * bpp);
        }
    }
    
    // Fill output frame
    outFrame->pixels = pixels;
    outFrame->width = finalW;
    outFrame->height = finalH;
    outFrame->stride = outStride;
    outFrame->format = outPixelFormat; // Handle float correctly
    SetupDeleter(pixels);
    
    // [v5.3] WIC Fallback Metadata Population
    if (pMetadata) {
        ReadMetadata(filePath, pMetadata);
        pMetadata->LoaderName = loaderName;
        if (pMetadata->FormatDetails.empty()) pMetadata->FormatDetails = L"Legacy WIC";
    }
    
    return S_OK;
}



// ============================================================================
// [v6.2] Static Helper Implementations
// ============================================================================

// Helper: Big-Endian to Native
inline uint32_t ReadU32BE(const uint8_t* ptr) {
    return (ptr[0] << 24) | (ptr[1] << 16) | (ptr[2] << 8) | ptr[3];
}

std::wstring CImageLoader::ParseICCProfileName(const uint8_t* data, size_t size) {
    if (!data || size < 128) return L"";
    
    // 1. Signature Check "acsp" at offset 36
    if (size > 40 && memcmp(data + 36, "acsp", 4) != 0) return L"";
    
    // 2. Tag Count at offset 128
    uint32_t tagCount = ReadU32BE(data + 128);
    if (128 + 4 + tagCount * 12 > size) return L"";
    
    // 3. Scan Tags
    const uint8_t* tags = data + 132;
    
    for (uint32_t i = 0; i < tagCount; ++i) {
        const uint8_t* entry = tags + i * 12;
        
        // Look for 'desc'
        if (memcmp(entry, "desc", 4) == 0) {
            uint32_t offset = ReadU32BE(entry + 4);
            uint32_t length = ReadU32BE(entry + 8);
            
            if (offset + length > size || length < 12) continue;
            
            const uint8_t* tagData = data + offset;
            uint32_t asciiLen = ReadU32BE(tagData + 8);
            
            if (asciiLen > 0 && asciiLen < length && offset + 12 + asciiLen <= size) {
                 std::string ascii((const char*)(tagData + 12), asciiLen);
                 // Remove nulls
                 ascii.erase(std::remove(ascii.begin(), ascii.end(), '\0'), ascii.end());
                 return std::wstring(ascii.begin(), ascii.end());
            }
             }
        }

    return L""; // Not found
}

// Helper: Parse JXL Color Encoding enum
static std::wstring ParseJXLColorEncoding(const JxlColorEncoding& c) {
    if (c.color_space == JXL_COLOR_SPACE_GRAY) return L"Grayscale";
    if (c.color_space == JXL_COLOR_SPACE_XYB) return L"XYB";
    if (c.color_space != JXL_COLOR_SPACE_RGB) return L""; // Unknown

    std::wstring primaries;
    switch (c.primaries) {
        case JXL_PRIMARIES_SRGB: primaries = L"sRGB"; break;
        case JXL_PRIMARIES_P3: primaries = L"Display P3"; break;
        case JXL_PRIMARIES_2100: primaries = L"Rec.2100"; break;
        default: break;
    }

    std::wstring transfer;
    switch (c.transfer_function) {
        case JXL_TRANSFER_FUNCTION_SRGB: transfer = L"sRGB"; break;
        case JXL_TRANSFER_FUNCTION_LINEAR: transfer = L"Linear"; break;
        case JXL_TRANSFER_FUNCTION_PQ: transfer = L"PQ"; break;
        case JXL_TRANSFER_FUNCTION_HLG: transfer = L"HLG"; break;
        case JXL_TRANSFER_FUNCTION_709: transfer = L"Rec.709"; break;
        case JXL_TRANSFER_FUNCTION_DCI: transfer = L"DCI"; break;
        default: break;
    }

    // Combine logic
    if (primaries == L"sRGB" && transfer == L"sRGB") return L"sRGB";
    if (primaries == L"Display P3" && transfer == L"sRGB") return L"Display P3";
    if (primaries.empty() && transfer.empty()) return L"";

    // Generic formatting
    std::wstring result = primaries.empty() ? L"Custom" : primaries;
    if (!transfer.empty() && transfer != L"sRGB") { // Assume sRGB transfer is default/implied for many
        result += L" (" + transfer + L")";
    }
    return result;
}

static QuickView::TransferFunction MapJxlTransferFunction(JxlTransferFunction tf) {
    switch (tf) {
        case JXL_TRANSFER_FUNCTION_SRGB: return QuickView::TransferFunction::SRGB;
        case JXL_TRANSFER_FUNCTION_LINEAR: return QuickView::TransferFunction::Linear;
        case JXL_TRANSFER_FUNCTION_PQ: return QuickView::TransferFunction::PQ;
        case JXL_TRANSFER_FUNCTION_HLG: return QuickView::TransferFunction::HLG;
        case JXL_TRANSFER_FUNCTION_709: return QuickView::TransferFunction::Rec709;
        default: return QuickView::TransferFunction::Unknown;
    }
}

static QuickView::ColorPrimaries MapJxlPrimaries(JxlPrimaries primaries) {
    switch (primaries) {
        case JXL_PRIMARIES_SRGB: return QuickView::ColorPrimaries::SRGB;
        case JXL_PRIMARIES_P3: return QuickView::ColorPrimaries::DisplayP3;
        case JXL_PRIMARIES_2100: return QuickView::ColorPrimaries::Rec2020;
        default: return QuickView::ColorPrimaries::Unknown;
    }
}

static QuickView::TransferFunction MapAvifTransferFunction(uint8_t tf) {
    switch (tf) {
        case AVIF_TRANSFER_CHARACTERISTICS_SRGB: return QuickView::TransferFunction::SRGB;
        case AVIF_TRANSFER_CHARACTERISTICS_LINEAR: return QuickView::TransferFunction::Linear;
        case AVIF_TRANSFER_CHARACTERISTICS_PQ: return QuickView::TransferFunction::PQ;
        case AVIF_TRANSFER_CHARACTERISTICS_HLG: return QuickView::TransferFunction::HLG;
        case AVIF_TRANSFER_CHARACTERISTICS_BT709: return QuickView::TransferFunction::Rec709;
        default: return QuickView::TransferFunction::Unknown;
    }
}

static QuickView::ColorPrimaries MapAvifPrimaries(uint16_t primaries) {
    switch (primaries) {
        case AVIF_COLOR_PRIMARIES_BT709: return QuickView::ColorPrimaries::SRGB;
        case AVIF_COLOR_PRIMARIES_BT2020: return QuickView::ColorPrimaries::Rec2020;
        case AVIF_COLOR_PRIMARIES_SMPTE432: return QuickView::ColorPrimaries::DisplayP3;
        default: return QuickView::ColorPrimaries::Unknown;
    }
}

static float DecodePqToLinearScRgb(float value) {
    value = (std::clamp)(value, 0.0f, 1.0f);
    static constexpr float m1 = 2610.0f / 16384.0f;
    static constexpr float m2 = 2523.0f / 32.0f;
    static constexpr float c1 = 3424.0f / 4096.0f;
    static constexpr float c2 = 2413.0f / 128.0f;
    static constexpr float c3 = 2392.0f / 128.0f;

    const float vp = powf(value, 1.0f / m2);
    const float num = (std::max)(vp - c1, 0.0f);
    const float den = c2 - c3 * vp;
    if (den <= 0.0f) return 0.0f;
    const float nits = 10000.0f * powf(num / den, 1.0f / m1);
    return nits / 80.0f;
}

static float DecodeHlgToLinear(float value) {
    value = (std::clamp)(value, 0.0f, 1.0f);
    if (value <= 0.5f) {
        return (value * value) / 3.0f;
    }
    static constexpr float a = 0.17883277f;
    static constexpr float b = 0.28466892f;
    static constexpr float c = 0.55991073f;
    return (expf((value - c) / a) + b) / 12.0f;
}

static float DecodeSrgbToLinear(float value) {
    value = (std::clamp)(value, 0.0f, 1.0f);
    if (value <= 0.04045f) return value / 12.92f;
    return powf((value + 0.055f) / 1.055f, 2.4f);
}

static float DecodeRec709ToLinear(float value) {
    value = (std::clamp)(value, 0.0f, 1.0f);
    if (value < 0.081f) return value / 4.5f;
    return powf((value + 0.099f) / 1.099f, 1.0f / 0.45f);
}

static float AvifUnsignedFractionToFloat(const avifUnsignedFraction& value, float fallback = 0.0f) {
    if (value.d == 0) return fallback;
    return static_cast<float>(value.n) / static_cast<float>(value.d);
}

static avifColorPrimaries GetAvifGainMapOutputPrimaries(const avifImage* image) {
    if (!image || !image->gainMap) {
        return image ? image->colorPrimaries : AVIF_COLOR_PRIMARIES_UNSPECIFIED;
    }

    if (image->gainMap->useBaseColorSpace || image->gainMap->altColorPrimaries == AVIF_COLOR_PRIMARIES_UNSPECIFIED) {
        return image->colorPrimaries;
    }
    return image->gainMap->altColorPrimaries;
}

static float GetAvifGainMapAlternateHeadroom(const avifImage* image) {
    if (!image || !image->gainMap) return 0.0f;
    return AvifUnsignedFractionToFloat(image->gainMap->alternateHdrHeadroom);
}

static float GetAvifGainMapBaseHeadroom(const avifImage* image) {
    if (!image || !image->gainMap) return 0.0f;
    return AvifUnsignedFractionToFloat(image->gainMap->baseHdrHeadroom);
}

static float ResolveAvifGainMapTargetHeadroom(const avifImage* image, float requestedHeadroom) {
    const float baseHeadroom = GetAvifGainMapBaseHeadroom(image);
    const float alternateHeadroom = GetAvifGainMapAlternateHeadroom(image);

    if (requestedHeadroom >= 0.0f) {
        return requestedHeadroom;
    }
    if (alternateHeadroom > 0.0f) {
        return alternateHeadroom;
    }
    return (std::max)(baseHeadroom, 0.0f);
}

static bool TryDecodeAvifGainMappedLinearRGBA(
    avifDecoder* decoder,
    const QuickView::Codec::DecodeContext& ctx,
    uint8_t** outPixels,
    int* outWidth,
    int* outHeight,
    int* outStride,
    QuickView::HdrStaticMetadata* hdrMetadata,
    std::wstring* formatDetails)
{
    if (!decoder || !decoder->image || !decoder->image->gainMap || !outPixels || !outWidth || !outHeight || !outStride) {
        return false;
    }

    const float hdrHeadroom = ResolveAvifGainMapTargetHeadroom(decoder->image, ctx.targetHdrHeadroomStops);
    if (hdrHeadroom < 0.0f) {
        return false;
    }

    avifRGBImage toneMappedRgb;
    avifRGBImageSetDefaults(&toneMappedRgb, decoder->image);
    toneMappedRgb.format = AVIF_RGB_FORMAT_RGBA;
    toneMappedRgb.depth = 16;
    toneMappedRgb.isFloat = AVIF_TRUE;
    toneMappedRgb.alphaPremultiplied = AVIF_FALSE;
    toneMappedRgb.maxThreads = decoder->maxThreads;

    avifResult avifHr = avifRGBImageAllocatePixels(&toneMappedRgb);
    if (avifHr != AVIF_RESULT_OK) {
        return false;
    }

    avifContentLightLevelInformationBox gainMapClli = {};
    avifHr = avifImageApplyGainMap(
        decoder->image,
        decoder->image->gainMap,
        hdrHeadroom,
        GetAvifGainMapOutputPrimaries(decoder->image),
        AVIF_TRANSFER_CHARACTERISTICS_LINEAR,
        &toneMappedRgb,
        &gainMapClli,
        &decoder->diag);

    if (avifHr != AVIF_RESULT_OK) {
        avifRGBImageFreePixels(&toneMappedRgb);
        return false;
    }

    const int width = static_cast<int>(toneMappedRgb.width);
    const int height = static_cast<int>(toneMappedRgb.height);
    const int stride = CalculateSIMDAlignedStride(width, 16);
    uint8_t* pixels = ctx.allocator(static_cast<size_t>(stride) * height);
    if (!pixels) {
        avifRGBImageFreePixels(&toneMappedRgb);
        return false;
    }

    for (int y = 0; y < height; ++y) {
        float* dst = reinterpret_cast<float*>(pixels + static_cast<size_t>(y) * stride);
        const uint16_t* src = reinterpret_cast<const uint16_t*>(toneMappedRgb.pixels + static_cast<size_t>(y) * toneMappedRgb.rowBytes);
        for (int x = 0; x < width; ++x) {
            dst[x * 4 + 0] = DirectX::PackedVector::XMConvertHalfToFloat(src[x * 4 + 0]);
            dst[x * 4 + 1] = DirectX::PackedVector::XMConvertHalfToFloat(src[x * 4 + 1]);
            dst[x * 4 + 2] = DirectX::PackedVector::XMConvertHalfToFloat(src[x * 4 + 2]);
            dst[x * 4 + 3] = DirectX::PackedVector::XMConvertHalfToFloat(src[x * 4 + 3]);
        }
    }

    if (hdrMetadata) {
        PopulateAvifHdrStaticMetadata(decoder->image, hdrMetadata);
        hdrMetadata->isValid = true;
        hdrMetadata->isHdr = true;
        hdrMetadata->isSceneLinear = true;
        hdrMetadata->gainMapApplied = true;
        hdrMetadata->gainMapAppliedHeadroom = hdrHeadroom;
        hdrMetadata->transfer = QuickView::TransferFunction::Linear;
        hdrMetadata->primaries = MapAvifPrimaries(GetAvifGainMapOutputPrimaries(decoder->image));
        if (gainMapClli.maxCLL > 0 || gainMapClli.maxPALL > 0) {
            hdrMetadata->hasNitsMetadata = true;
            hdrMetadata->maxCLLNits = static_cast<float>(gainMapClli.maxCLL);
            hdrMetadata->maxFALLNits = static_cast<float>(gainMapClli.maxPALL);
        }
    }

    if (formatDetails) {
        wchar_t fmtBuf[160];
        swprintf_s(fmtBuf, L"%d-bit HDR AVIF [GainMap Applied]", decoder->image->depth);
        *formatDetails = fmtBuf;
        if (gainMapClli.maxCLL > 0 || gainMapClli.maxPALL > 0) {
            *formatDetails += L" [CLLI]";
        }
    }

    *outPixels = pixels;
    *outWidth = width;
    *outHeight = height;
    *outStride = stride;
    avifRGBImageFreePixels(&toneMappedRgb);
    return true;
}

static void PopulateAvifHdrStaticMetadata(const avifImage* image, QuickView::HdrStaticMetadata* hdrMetadata) {
    if (!image || !hdrMetadata) return;

    hdrMetadata->isValid = true;
    hdrMetadata->transfer = MapAvifTransferFunction(image->transferCharacteristics);
    hdrMetadata->primaries = MapAvifPrimaries(image->colorPrimaries);
    hdrMetadata->isSceneLinear = (hdrMetadata->transfer == QuickView::TransferFunction::Linear);
    hdrMetadata->isHdr =
        hdrMetadata->transfer == QuickView::TransferFunction::PQ ||
        hdrMetadata->transfer == QuickView::TransferFunction::HLG ||
        image->depth > 8;
    hdrMetadata->hasGainMap = (image->gainMap != nullptr);
    hdrMetadata->gainMapApplied = false;
    hdrMetadata->gainMapAppliedHeadroom = 0.0f;

    if (image->gainMap) {
        hdrMetadata->gainMapBaseHeadroom = GetAvifGainMapBaseHeadroom(image);
        hdrMetadata->gainMapAlternateHeadroom = GetAvifGainMapAlternateHeadroom(image);
    }

    if (image->clli.maxCLL > 0 || image->clli.maxPALL > 0) {
        hdrMetadata->hasNitsMetadata = true;
        hdrMetadata->maxCLLNits = static_cast<float>(image->clli.maxCLL);
        hdrMetadata->maxFALLNits = static_cast<float>(image->clli.maxPALL);
    }

    if (!hdrMetadata->hasNitsMetadata && image->gainMap) {
        const auto& altCLLI = image->gainMap->altCLLI;
        if (altCLLI.maxCLL > 0 || altCLLI.maxPALL > 0) {
            hdrMetadata->hasNitsMetadata = true;
            hdrMetadata->maxCLLNits = static_cast<float>(altCLLI.maxCLL);
            hdrMetadata->maxFALLNits = static_cast<float>(altCLLI.maxPALL);
        }
    }
}

static float DecodeTransferToLinear(float value, QuickView::TransferFunction tf) {
    switch (tf) {
        case QuickView::TransferFunction::Linear: return value;
        case QuickView::TransferFunction::PQ: return DecodePqToLinearScRgb(value);
        case QuickView::TransferFunction::HLG: return DecodeHlgToLinear(value);
        case QuickView::TransferFunction::Rec709: return DecodeRec709ToLinear(value);
        case QuickView::TransferFunction::SRGB:
        default:
            return DecodeSrgbToLinear(value);
    }
}

// ============================================================================

void CImageLoader::PopulateFormatDetails(struct ImageMetadata* meta, const wchar_t* formatName, int bitDepth, bool isLossless, bool hasAlpha, bool isAnim) {
    if (!meta) return;
    
    wchar_t buf[128];
    meta->FormatDetails = L"";
    
    // Bit Depth
    if (bitDepth > 0) {
        swprintf_s(buf, L"%d-bit ", bitDepth);
        meta->FormatDetails += buf;
    }
    
    // Name
    meta->FormatDetails += formatName;
    
    // Lossless/Lossy
    if (isLossless) meta->FormatDetails += L" Lossless";
    // else meta->FormatDetails += L" Lossy"; // Optional: Don't show "Lossy" for standard JPG? User asked for ALL formats.
    else if (_wcsicmp(formatName, L"JPG") != 0 && _wcsicmp(formatName, L"JPEG") != 0) {
         // Show Lossy for non-JPGs (AVIF, WebP, JXL)
         // For JPG it's implied? User said "in ALL supported formats... show Lossy/Lossless".
         meta->FormatDetails += L" Lossy"; 
    } 
    else {
        // For JPG, maybe Q-value covers it?
        // Let's explicitly add "Lossy" to satisfy "ALL formats" request.
        meta->FormatDetails += L" Lossy";
    }

    // Alpha
    if (hasAlpha) meta->FormatDetails += L" Alpha";
    
    // Anim
    if (isAnim) meta->FormatDetails += L" Anim";
}
// [v6.5 Recursor] Get Embedded Thumbnail/Preview dimensions for RAW files
HRESULT CImageLoader::GetEmbeddedPreviewInfo(LPCWSTR filePath, int* width, int* height) {
    if (!filePath || !width || !height) return E_INVALIDARG;
    *width = 0; *height = 0;

    // Use LibRaw to extract thumbnail Dimensions FAST.
    LibRaw RawProcessor;

    // Conversion helper (same as ReadMetadataLibRaw)
    std::string pathUtf8;
    try {
        int len = WideCharToMultiByte(CP_UTF8, 0, filePath, -1, NULL, 0, NULL, NULL);
        if (len > 0) {
            pathUtf8.resize(len);
            WideCharToMultiByte(CP_UTF8, 0, filePath, -1, &pathUtf8[0], len, NULL, NULL);
            pathUtf8.pop_back(); 
        }
    } catch(...) { return E_INVALIDARG; }

    if (RawProcessor.open_file(pathUtf8.c_str()) != LIBRAW_SUCCESS) return E_FAIL;
    
    // Try unpack_thumb to get dimensions (unfortunately needed for some formats)
    // Optimization: Check if sizes are already available in imgdata.thumbnail without unpack?
    // Usually twidth/theight are 0 until unpack_thumb.
    if (RawProcessor.unpack_thumb() != LIBRAW_SUCCESS) return E_FAIL;

    *width = RawProcessor.imgdata.thumbnail.twidth;
    *height = RawProcessor.imgdata.thumbnail.theight;
    
    return S_OK;
}
