

// === Module Selection ===
#define WUFFS_CONFIG__MODULES
#define WUFFS_CONFIG__MODULE__BASE
#define WUFFS_CONFIG__MODULE__CRC32
#define WUFFS_CONFIG__MODULE__ADLER32
#define WUFFS_CONFIG__MODULE__DEFLATE
#define WUFFS_CONFIG__MODULE__ZLIB
#define WUFFS_CONFIG__MODULE__PNG
#define WUFFS_CONFIG__MODULE__GIF
#define WUFFS_CONFIG__MODULE__LZW
#define WUFFS_CONFIG__MODULE__BMP
#define WUFFS_CONFIG__MODULE__TARGA
#define WUFFS_CONFIG__MODULE__WBMP
#define WUFFS_CONFIG__MODULE__NETPBM
#define WUFFS_CONFIG__MODULE__QOI

// === Performance Optimizations ===
#define WUFFS_CONFIG__STATIC_FUNCTIONS

#ifdef _MSC_VER
#if defined(_M_X64) || defined(_M_IX86)
#define WUFFS_CONFIG__I_KNOW_THAT_WUFFS_MSVC_PERFORMS_BEST_WITH_ARCH_AVX2
#endif
#endif

#define WUFFS_CONFIG__DST_PIXEL_FORMAT__ENABLE_ALLOWLIST
#define WUFFS_CONFIG__DST_PIXEL_FORMAT__ALLOW_BGRA_NONPREMUL
#define WUFFS_CONFIG__DST_PIXEL_FORMAT__ALLOW_BGRA_NONPREMUL_4X16LE
#define WUFFS_CONFIG__DST_PIXEL_FORMAT__ALLOW_BGRA_PREMUL

#define WUFFS_IMPLEMENTATION
#include "../third_party/wuffs/release/c/wuffs-v0.4.c"

#include "WuffsLoader.h"
#include "AnimationDecoder.h"
#include "ImageLoaderSimd.h"
#include "MappedFile.h"
#include <zlib.h>

#ifndef Bytef
typedef unsigned char Bytef;
#endif
#ifndef uLongf
typedef unsigned long uLongf;
#endif

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <malloc.h>
#include <memory>
#include <vector>
#include <thread>
#include <shared_mutex>
#include <stop_token>

namespace WuffsLoader {

    struct BufferAdapter {
        AllocatorFunc allocFunc;
        uint8_t* ptr = nullptr;
        using allocator_type = std::allocator<uint8_t>;
        BufferAdapter(AllocatorFunc f) : allocFunc(f) {}
        void resize(size_t s) { ptr = allocFunc(s); }
        uint8_t* data() { return ptr; }
        allocator_type get_allocator() const { return allocator_type(); }
    };

    struct BufferSink {
        void* container = nullptr;
        uint8_t* (*allocate)(void* ctx, size_t size) = nullptr;
        std::pmr::memory_resource* memRes = nullptr;

        uint8_t* Allocate(size_t size) {
            return allocate ? allocate(container, size) : nullptr;
        }
    };

    template <typename Vec>
    inline BufferSink MakeBufferSink(Vec& outPixels) {
        if constexpr (std::is_same_v<Vec, std::pmr::vector<uint8_t>>) {
            return BufferSink{
                &outPixels,
                [](void* ctx, size_t sz) -> uint8_t* {
                    auto* v = static_cast<std::pmr::vector<uint8_t>*>(ctx);
                    v->resize(sz);
                    return v->data();
                },
                outPixels.get_allocator().resource()
            };
        } else if constexpr (std::is_same_v<Vec, std::vector<uint8_t>>) {
            return BufferSink{
                &outPixels,
                [](void* ctx, size_t sz) -> uint8_t* {
                    auto* v = static_cast<std::vector<uint8_t>*>(ctx);
                    v->resize(sz);
                    return v->data();
                },
                nullptr
            };
        } else {
            return BufferSink{
                &outPixels,
                [](void* ctx, size_t sz) -> uint8_t* {
                    auto* a = static_cast<BufferAdapter*>(ctx);
                    a->resize(sz);
                    return a->data();
                },
                nullptr
            };
        }
    }

#define WUFFS_TRY(expr) \
    do { \
        while(true) { \
            if (checkCancel && checkCancel()) return false; \
            status = (expr); \
            if (wuffs_base__status__is_ok(&status)) break; \
            if (status.repr == wuffs_base__suspension__short_read && !src.meta.closed) { \
                 size_t next = std::min(size, src.meta.wi + 16777216); \
                 src.meta.wi = next; \
                 src.meta.closed = (next == size); \
                 continue; \
            } \
            return false; \
        } \
    } while(0)

// ------------------------------------------------------------
// PNG Decoder (zlib-ng streaming + Highway SIMD fast path, Wuffs fallback)
// ------------------------------------------------------------
struct PngPreScanInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    uint8_t bitDepth = 0;
    uint8_t colorType = 0;
    uint8_t compression = 0;
    uint8_t filter = 0;
    uint8_t interlace = 0;
    bool hasIhdr = false;
    bool hasTrns = false;
    bool hasActl = false;
    size_t firstIdatOffset = 0;
};

static inline uint32_t ReadU32BE_Png(const uint8_t* ptr) noexcept {
    return (static_cast<uint32_t>(ptr[0]) << 24) |
           (static_cast<uint32_t>(ptr[1]) << 16) |
           (static_cast<uint32_t>(ptr[2]) << 8) |
           static_cast<uint32_t>(ptr[3]);
}

static PngPreScanInfo PreScanPngHeaderChunks(const uint8_t* data, size_t size,
                                             WuffsImageInfo* pInfo) {
    PngPreScanInfo scan;
    static constexpr uint8_t kPngSignature[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    if (size < 8 || std::memcmp(data, kPngSignature, 8) != 0) {
        return scan;
    }

    size_t offset = 8; // skip PNG signature
    bool hasTopLevelCicp = false;
    bool hasIccCicp = false;
    bool hasSrgbChunk = false;
    QuickView::ColorPrimaries iccPrimaries = QuickView::ColorPrimaries::Unknown;
    QuickView::TransferFunction iccTransfer = QuickView::TransferFunction::Unknown;

    while (offset + 12 <= size) {
        const uint32_t chunk_len = ReadU32BE_Png(data + offset);
        const uint32_t chunk_type = ReadU32BE_Png(data + offset + 4);
        if (static_cast<size_t>(chunk_len) > size - offset - 12) {
            break;
        }
        const uint8_t* payload = data + offset + 8;

        if (chunk_type == 0x49484452 && chunk_len >= 13) { // IHDR
            scan.width = ReadU32BE_Png(payload);
            scan.height = ReadU32BE_Png(payload + 4);
            scan.bitDepth = payload[8];
            scan.colorType = payload[9];
            scan.compression = payload[10];
            scan.filter = payload[11];
            scan.interlace = payload[12];
            scan.hasIhdr = true;
            if (pInfo) {
                pInfo->bitDepth = payload[8];
            }
        } else if (chunk_type == 0x74524E53) { // tRNS
            scan.hasTrns = true;
        } else if (chunk_type == 0x6163544C) { // acTL
            scan.hasActl = true;
        } else if (pInfo && chunk_type == 0x63494350 && chunk_len >= 4) { // cICP
            const uint8_t primaries = payload[0];
            const uint8_t transfer = payload[1];
            const uint8_t matrix = payload[2];
            if (matrix == 0) {
                QuickView::ColorPrimaries cicpPrim = QuickView::ColorPrimaries::Unknown;
                QuickView::TransferFunction cicpTransfer = QuickView::TransferFunction::Unknown;
                QuickView::MapCicpCodes(primaries, transfer, &cicpPrim, &cicpTransfer);
                if (cicpPrim != QuickView::ColorPrimaries::Unknown ||
                    cicpTransfer != QuickView::TransferFunction::Unknown) {
                    hasTopLevelCicp = true;
                    if (cicpPrim != QuickView::ColorPrimaries::Unknown) {
                        pInfo->primaries = cicpPrim;
                    }
                    if (cicpTransfer != QuickView::TransferFunction::Unknown) {
                        pInfo->transfer = cicpTransfer;
                    }
                }
            }
        } else if (pInfo && chunk_type == 0x73524742 && chunk_len >= 1) { // sRGB
            hasSrgbChunk = true;
        } else if (pInfo && chunk_type == 0x69434350 && pInfo->iccProfile.empty()) { // iCCP
            // iCCP format: Name(1-79B) + Null(1B) + CompressionFlag(1B=0) + zlib_data
            size_t null_idx = 0;
            while (null_idx < 80 && null_idx < chunk_len && payload[null_idx] != 0) null_idx++;
            if (chunk_len >= 2 && null_idx + 2 <= chunk_len && payload[null_idx] == 0 && payload[null_idx + 1] == 0) {
                const uint8_t* zlib_data = payload + null_idx + 2;
                size_t zlib_len = static_cast<size_t>(chunk_len) - (null_idx + 2);

                uLongf destLen = 1048576; // Start with 1MB for ICC
                pInfo->iccProfile.resize(destLen);
                int ret = uncompress(pInfo->iccProfile.data(), &destLen, zlib_data, static_cast<uLong>(zlib_len));
                if (ret == Z_OK) {
                    pInfo->iccProfile.resize(destLen);
                } else if (ret == Z_BUF_ERROR) {
                    destLen = 1048576 * 4; // 4MB
                    pInfo->iccProfile.resize(destLen);
                    ret = uncompress(pInfo->iccProfile.data(), &destLen, zlib_data, static_cast<uLong>(zlib_len));
                    if (ret == Z_OK) pInfo->iccProfile.resize(destLen);
                    else pInfo->iccProfile.clear();
                } else {
                    pInfo->iccProfile.clear();
                }

                if (!pInfo->iccProfile.empty()) {
                    if (QuickView::TryParseIccCicp(pInfo->iccProfile.data(),
                                                   pInfo->iccProfile.size(),
                                                   &iccPrimaries,
                                                   &iccTransfer)) {
                        hasIccCicp = true;
                    }
                }
            }
        } else if (pInfo && chunk_type == 0x634C4C69 && chunk_len >= 8) { // cLLi
            const uint32_t maxCllScaled = ReadU32BE_Png(payload);
            const uint32_t maxFallScaled = ReadU32BE_Png(payload + 4);
            pInfo->maxCLLNits = static_cast<float>(maxCllScaled) / 10000.0f;
            pInfo->maxFALLNits = static_cast<float>(maxFallScaled) / 10000.0f;
            if (pInfo->maxCLLNits > 0.0f || pInfo->maxFALLNits > 0.0f) {
                pInfo->hasNitsMetadata = true;
            }
        } else if (pInfo && chunk_type == 0x6D444376 && chunk_len >= 24) { // mDCv
            const uint32_t maxLumScaled = ReadU32BE_Png(payload + 16);
            const uint32_t minLumScaled = ReadU32BE_Png(payload + 20);
            pInfo->masteringMaxNits = static_cast<float>(maxLumScaled) / 10000.0f;
            pInfo->masteringMinNits = static_cast<float>(minLumScaled) / 10000.0f;
            if (pInfo->masteringMaxNits > 0.0f || pInfo->masteringMinNits > 0.0f) {
                pInfo->hasNitsMetadata = true;
            }
        } else if (chunk_type == 0x49444154) { // IDAT
            scan.firstIdatOffset = offset;
            break;
        } else if (chunk_type == 0x49454E44) { // IEND
            break;
        }
        offset += 12 + static_cast<size_t>(chunk_len);
    }

    if (pInfo) {
        // Priority: top-level PNG cICP > ICC cicp > sRGB chunk
        if (hasIccCicp) {
            if (pInfo->primaries == QuickView::ColorPrimaries::Unknown &&
                iccPrimaries != QuickView::ColorPrimaries::Unknown) {
                pInfo->primaries = iccPrimaries;
            }
            if (pInfo->transfer == QuickView::TransferFunction::Unknown &&
                iccTransfer != QuickView::TransferFunction::Unknown) {
                pInfo->transfer = iccTransfer;
            }
        }
        if (hasSrgbChunk && !hasTopLevelCicp && !hasIccCicp) {
            if (pInfo->transfer == QuickView::TransferFunction::Unknown) {
                pInfo->transfer = QuickView::TransferFunction::SRGB;
            }
            if (pInfo->primaries == QuickView::ColorPrimaries::Unknown) {
                pInfo->primaries = QuickView::ColorPrimaries::SRGB;
            }
        }
    }
    return scan;
}

static inline uint16_t ReadU16BE_Png(const uint8_t* p) noexcept {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]));
}

static void ConvertPngRowToTarget(const uint8_t* src, uint8_t* dst, uint32_t width,
                                  uint8_t colorType, uint8_t bitDepth, bool output16Bit) {
    if (output16Bit) {
        // Output matches Wuffs + SwizzleBGRA16ToRGBA16: non-premultiplied RGBA16 little-endian
        uint16_t* dst16 = reinterpret_cast<uint16_t*>(dst);
        if (colorType == 2) { // RGB16 BE -> RGBA16 LE
            for (uint32_t x = 0; x < width; ++x) {
                const uint8_t* s = src + static_cast<size_t>(x) * 6;
                uint16_t* d = dst16 + static_cast<size_t>(x) * 4;
                d[0] = ReadU16BE_Png(s + 0);
                d[1] = ReadU16BE_Png(s + 2);
                d[2] = ReadU16BE_Png(s + 4);
                d[3] = 0xFFFFu;
            }
        } else if (colorType == 6) { // RGBA16 BE -> RGBA16 LE
            for (uint32_t x = 0; x < width; ++x) {
                const uint8_t* s = src + static_cast<size_t>(x) * 8;
                uint16_t* d = dst16 + static_cast<size_t>(x) * 4;
                d[0] = ReadU16BE_Png(s + 0);
                d[1] = ReadU16BE_Png(s + 2);
                d[2] = ReadU16BE_Png(s + 4);
                d[3] = ReadU16BE_Png(s + 6);
            }
        } else if (colorType == 0) { // Gray16 BE -> RGBA16 LE
            for (uint32_t x = 0; x < width; ++x) {
                const uint16_t g = ReadU16BE_Png(src + static_cast<size_t>(x) * 2);
                uint16_t* d = dst16 + static_cast<size_t>(x) * 4;
                d[0] = g;
                d[1] = g;
                d[2] = g;
                d[3] = 0xFFFFu;
            }
        } else { // colorType == 4: GrayAlpha16 BE -> RGBA16 LE
            for (uint32_t x = 0; x < width; ++x) {
                const uint8_t* s = src + static_cast<size_t>(x) * 4;
                const uint16_t g = ReadU16BE_Png(s + 0);
                const uint16_t a = ReadU16BE_Png(s + 2);
                uint16_t* d = dst16 + static_cast<size_t>(x) * 4;
                d[0] = g;
                d[1] = g;
                d[2] = g;
                d[3] = a;
            }
        }
        return;
    }

    // Output matches Wuffs BGRA_PREMUL (BGRA8888 premultiplied)
    if (colorType == 2) {
        if (bitDepth == 16) {
            ImageLoaderSimd::ConvertRGB16ToBGRARow(src, dst, static_cast<int>(width), false);
        } else {
            ImageLoaderSimd::ConvertRGBToBGRA(src, dst, static_cast<int>(width), 1, static_cast<int>(width) * 4);
        }
    } else if (colorType == 6) {
        if (bitDepth == 8) {
            std::memcpy(dst, src, static_cast<size_t>(width) * 4);
            ImageLoaderSimd::SwizzleRGBAToBGRA(dst, static_cast<size_t>(width));
        } else {
            for (uint32_t x = 0; x < width; ++x) {
                const uint8_t* s = src + static_cast<size_t>(x) * 8;
                const uint32_t r16 = ReadU16BE_Png(s + 0);
                const uint32_t g16 = ReadU16BE_Png(s + 2);
                const uint32_t b16 = ReadU16BE_Png(s + 4);
                const uint32_t a16 = ReadU16BE_Png(s + 6);
                uint8_t* d = dst + static_cast<size_t>(x) * 4;
                d[0] = static_cast<uint8_t>(((b16 * a16) / 0xFFFFu) >> 8);
                d[1] = static_cast<uint8_t>(((g16 * a16) / 0xFFFFu) >> 8);
                d[2] = static_cast<uint8_t>(((r16 * a16) / 0xFFFFu) >> 8);
                d[3] = static_cast<uint8_t>(a16 >> 8);
            }
        }
    } else if (colorType == 0) {
        if (bitDepth == 8) {
            for (uint32_t x = 0; x < width; ++x) {
                const uint8_t g = src[x];
                uint8_t* d = dst + static_cast<size_t>(x) * 4;
                d[0] = g; d[1] = g; d[2] = g; d[3] = 255;
            }
        } else {
            for (uint32_t x = 0; x < width; ++x) {
                const uint8_t g = src[static_cast<size_t>(x) * 2];
                uint8_t* d = dst + static_cast<size_t>(x) * 4;
                d[0] = g; d[1] = g; d[2] = g; d[3] = 255;
            }
        }
    } else { // colorType == 4 (GrayAlpha)
        if (bitDepth == 8) {
            for (uint32_t x = 0; x < width; ++x) {
                const uint32_t g = src[static_cast<size_t>(x) * 2 + 0];
                const uint32_t a = src[static_cast<size_t>(x) * 2 + 1];
                const uint8_t gp = static_cast<uint8_t>((g * a + 127u) / 255u);
                uint8_t* d = dst + static_cast<size_t>(x) * 4;
                d[0] = gp; d[1] = gp; d[2] = gp; d[3] = static_cast<uint8_t>(a);
            }
        } else {
            for (uint32_t x = 0; x < width; ++x) {
                const uint8_t* s = src + static_cast<size_t>(x) * 4;
                const uint32_t g16 = ReadU16BE_Png(s + 0);
                const uint32_t a16 = ReadU16BE_Png(s + 2);
                const uint8_t gp = static_cast<uint8_t>(((g16 * a16) / 0xFFFFu) >> 8);
                uint8_t* d = dst + static_cast<size_t>(x) * 4;
                d[0] = gp; d[1] = gp; d[2] = gp; d[3] = static_cast<uint8_t>(a16 >> 8);
            }
        }
    }
}

static bool FeedNextIdatChunk(const uint8_t* data, size_t size,
                                size_t& curOffset, uint32_t& curChunkLen, z_stream& strm) noexcept {
    curOffset += 12 + static_cast<size_t>(curChunkLen);
    curChunkLen = 0;
    while (curOffset + 12 <= size) {
        const uint32_t len = ReadU32BE_Png(data + curOffset);
        const uint32_t type = ReadU32BE_Png(data + curOffset + 4);
        if (static_cast<size_t>(len) > size - curOffset - 12 || type != 0x49444154) {
            return false;
        }
        if (len > 0) {
            curChunkLen = len;
            strm.next_in = const_cast<Bytef*>(data + curOffset + 8);
            strm.avail_in = static_cast<uInt>(curChunkLen);
            return true;
        }
        curOffset += 12;
    }
    return false;
}

static bool DecodePngSingleThreadRows(
    const uint8_t* data, size_t size, const PngPreScanInfo& scan,
    uint8_t* outPtr, size_t rowBytes, size_t lineBytes, int bpp,
    size_t dstRowBytes, bool output16Bit, CancelPredicate checkCancel,
    size_t& curOffset, uint32_t& curChunkLen, z_stream& strm, bool& outCancelled) {
    // Allocate two uninitialized rolling scanline buffers (~rowBytes each, zero memset overhead).
    std::unique_ptr<uint8_t[]> rowBufA(new (std::nothrow) uint8_t[lineBytes]);
    std::unique_ptr<uint8_t[]> rowBufB(new (std::nothrow) uint8_t[lineBytes]);
    if (!rowBufA || !rowBufB) return false;
    uint8_t* currLine = rowBufA.get();
    uint8_t* prevLine = rowBufB.get();

    size_t rowFill = 0;
    uint32_t y = 0;
    while (y < scan.height) {
        if (strm.avail_in == 0 && !FeedNextIdatChunk(data, size, curOffset, curChunkLen, strm)) {
            return false;
        }

        const size_t rem = lineBytes - rowFill;
        strm.next_out = currLine + rowFill;
        strm.avail_out = static_cast<uInt>((std::min)(rem, static_cast<size_t>(0x7FFFFFFFu)));

        const int ret = inflate(&strm, Z_NO_FLUSH);
        rowFill = static_cast<size_t>(strm.next_out - currLine);

        if (rowFill == lineBytes) {
            const uint8_t filterType = currLine[0];
            if (filterType > 4) return false;
            if ((y & 63u) == 0u && checkCancel && checkCancel()) {
                outCancelled = true;
                return false;
            }

            ImageLoaderSimd::PngUnfilterRow(
                filterType, currLine + 1, (y > 0) ? (prevLine + 1) : nullptr, rowBytes, bpp);
            ConvertPngRowToTarget(currLine + 1, outPtr + static_cast<size_t>(y) * dstRowBytes,
                                  scan.width, scan.colorType, scan.bitDepth, output16Bit);

            std::swap(currLine, prevLine);
            rowFill = 0;
            ++y;
        }

        if (ret == Z_STREAM_END) break;
        if (ret != Z_OK && !(ret == Z_BUF_ERROR && strm.avail_in == 0)) return false;
    }
    return y == scan.height;
}

// Dual-thread lock-free SPSC ring pipeline: main thread feeds large contiguous avail_out chunks
// to zlib-ng inflate while a consumer thread performs SIMD unfilter + pixel conversion in parallel.
static bool DecodePngPipelinedRing(
    const uint8_t* data, size_t size, const PngPreScanInfo& scan,
    uint8_t* outPtr, size_t rowBytes, size_t lineBytes, int bpp,
    size_t dstRowBytes, bool output16Bit, CancelPredicate checkCancel,
    size_t& curOffset, uint32_t& curChunkLen, z_stream& strm, bool& outCancelled) {
    uint64_t ringRows = 64;
    while (ringRows > 8 && ringRows * static_cast<uint64_t>(lineBytes) > 16ULL * 1024ULL * 1024ULL) {
        ringRows >>= 1;
    }
    const uint64_t mask = ringRows - 1u;
    std::unique_ptr<uint8_t[]> ringBuf(new (std::nothrow) uint8_t[static_cast<size_t>(ringRows) * lineBytes]);
    if (!ringBuf) return false;

    constexpr uint64_t kPipelineStop = (std::numeric_limits<uint64_t>::max)();
    std::atomic<uint64_t> producedAtomic{0};
    std::atomic<uint64_t> consumedAtomic{0};
    std::atomic<bool> failed{false};

    std::thread consumer([&]() {
        uint64_t y = 0;
        while (y < scan.height) {
            const uint64_t avail = producedAtomic.load(std::memory_order_acquire);
            if (avail == kPipelineStop || failed.load(std::memory_order_acquire)) break;
            if (y >= avail) {
                producedAtomic.wait(avail, std::memory_order_acquire);
                continue;
            }

            const uint64_t batchEnd = (std::min)(avail, static_cast<uint64_t>(scan.height));
            for (; y < batchEnd; ++y) {
                if (failed.load(std::memory_order_relaxed)) break;
                uint8_t* curr = ringBuf.get() + static_cast<size_t>(y & mask) * lineBytes;
                const uint8_t* prev = (y > 0)
                    ? (ringBuf.get() + static_cast<size_t>((y - 1u) & mask) * lineBytes + 1u)
                    : nullptr;
                const uint8_t filterType = curr[0];
                if (filterType > 4) {
                    failed.store(true, std::memory_order_release);
                    consumedAtomic.store(kPipelineStop, std::memory_order_release);
                    consumedAtomic.notify_one();
                    return;
                }
                ImageLoaderSimd::PngUnfilterRow(filterType, curr + 1u, prev, rowBytes, bpp);
                ConvertPngRowToTarget(curr + 1u, outPtr + static_cast<size_t>(y) * dstRowBytes,
                                      scan.width, scan.colorType, scan.bitDepth, output16Bit);
            }
            consumedAtomic.store(y, std::memory_order_release);
            consumedAtomic.notify_one();
        }
    });

    uint64_t produced = 0;
    uint64_t lastPublished = 0;
    size_t partialBytes = 0;
    bool streamOk = true;

    while (produced < scan.height) {
        if (checkCancel && checkCancel()) {
            outCancelled = true;
            streamOk = false;
            break;
        }

        const uint64_t c = consumedAtomic.load(std::memory_order_acquire);
        if (c == kPipelineStop || failed.load(std::memory_order_acquire)) {
            streamOk = false;
            break;
        }

        // Leave 1 slot safety gap so consumer can still read slot[(c - 1) & mask] as prevRow.
        const uint64_t maxRowLimit = (std::min)(
            static_cast<uint64_t>(scan.height), c + (ringRows - 1u));
        if (produced >= maxRowLimit) {
            if (produced > lastPublished) {
                producedAtomic.store(produced, std::memory_order_release);
                producedAtomic.notify_one();
                lastPublished = produced;
            }
            consumedAtomic.wait(c, std::memory_order_acquire);
            continue;
        }

        if (strm.avail_in == 0 && !FeedNextIdatChunk(data, size, curOffset, curChunkLen, strm)) {
            streamOk = false;
            break;
        }

        const uint64_t maxContigRows = (std::min)(
            ringRows - (produced & mask), maxRowLimit - produced);
        const uint64_t burstRows = (std::min)(maxContigRows, uint64_t{16});
        const size_t availBytes = static_cast<size_t>(burstRows) * lineBytes - partialBytes;
        uint8_t* writeStart = ringBuf.get() + static_cast<size_t>(produced & mask) * lineBytes + partialBytes;

        strm.next_out = writeStart;
        strm.avail_out = static_cast<uInt>((std::min)(availBytes, static_cast<size_t>(0x7FFFFFFFu)));

        const int ret = inflate(&strm, Z_NO_FLUSH);
        const size_t inflatedBytes = static_cast<size_t>(strm.next_out - writeStart);
        const size_t totalBytes = partialBytes + inflatedBytes;
        const uint64_t completedNewRows = totalBytes / lineBytes;
        partialBytes = totalBytes % lineBytes;
        produced += completedNewRows;

        if (produced > lastPublished &&
            (produced - lastPublished >= 8 || produced == scan.height ||
             ret == Z_STREAM_END || produced >= maxRowLimit)) {
            producedAtomic.store(produced, std::memory_order_release);
            producedAtomic.notify_one();
            lastPublished = produced;
        }

        if (ret == Z_STREAM_END) break;
        if (ret != Z_OK && !(ret == Z_BUF_ERROR && strm.avail_in == 0)) {
            streamOk = false;
            break;
        }
    }

    if (!streamOk || produced != scan.height) {
        failed.store(true, std::memory_order_release);
        producedAtomic.store(kPipelineStop, std::memory_order_release);
        producedAtomic.notify_one();
        streamOk = false;
    } else if (produced > lastPublished) {
        producedAtomic.store(produced, std::memory_order_release);
        producedAtomic.notify_one();
    }

    if (consumer.joinable()) consumer.join();
    if (failed.load(std::memory_order_acquire)) streamOk = false;
    return streamOk;
}

// Streams non-interlaced 8/16-bit PNG via zlib-ng Raw Inflate (-15) and rolling row buffers,
// eliminating full-frame workbuf commits while invoking Highway SIMD unfilter + pixel conversion.
static bool TryDecodePngStreamingZlibNg(
    const uint8_t* data, size_t size, const PngPreScanInfo& scan,
    BufferSink& outSink, CancelPredicate checkCancel, WuffsImageInfo* pInfo, bool force8BitBgra,
    uint8_t*& inOutAllocatedPtr, size_t& inOutAllocatedBytes, bool& outCancelled) {
    outCancelled = false;
    if (!scan.hasIhdr || scan.width == 0 || scan.height == 0 ||
        scan.width > 0x7FFFFFFFu || scan.height > 0x7FFFFFFFu ||
        scan.width > static_cast<uint32_t>((std::numeric_limits<int>::max)() / 4) ||
        scan.compression != 0 || scan.filter != 0 || scan.interlace != 0 ||
        scan.hasTrns || scan.hasActl || scan.firstIdatOffset == 0) {
        return false;
    }
    if (scan.bitDepth != 8 && scan.bitDepth != 16) return false;

    int channels = 0;
    switch (scan.colorType) {
    case 0: channels = 1; break;
    case 2: channels = 3; break;
    case 4: channels = 2; break;
    case 6: channels = 4; break;
    default: return false; // Palette (colorType == 3) falls back to Wuffs
    }

    const int bpp = channels * (scan.bitDepth / 8);
    if (static_cast<uint64_t>(scan.width) * static_cast<uint64_t>(bpp) > (std::numeric_limits<size_t>::max)() - 1u) {
        return false;
    }
    const size_t rowBytes = static_cast<size_t>(scan.width) * static_cast<size_t>(bpp);
    const size_t lineBytes = rowBytes + 1u;

    const bool output16Bit = (pInfo != nullptr && scan.bitDepth == 16 && !force8BitBgra);
    const size_t outBpp = output16Bit ? 8u : 4u;
    const size_t pixelCount = static_cast<size_t>(scan.width) * static_cast<size_t>(scan.height);
    if (pixelCount / scan.width != scan.height ||
        pixelCount > (std::numeric_limits<size_t>::max)() / outBpp) {
        return false;
    }
    const size_t pixelSize = pixelCount * outBpp;

    // Locate first non-empty IDAT chunk and validate 2-byte RFC 1950 zlib header for raw inflate.
    size_t curOffset = scan.firstIdatOffset;
    uint32_t curChunkLen = 0;
    while (curOffset + 12 <= size) {
        const uint32_t len = ReadU32BE_Png(data + curOffset);
        const uint32_t type = ReadU32BE_Png(data + curOffset + 4);
        if (static_cast<size_t>(len) > size - curOffset - 12 || type != 0x49444154) {
            return false;
        }
        if (len > 0) {
            curChunkLen = len;
            break;
        }
        curOffset += 12;
    }
    if (curChunkLen < 2) return false;

    const uint8_t* firstPayload = data + curOffset + 8;
    const uint8_t cmf = firstPayload[0];
    const uint8_t flg = firstPayload[1];
    if ((cmf & 0x0F) != 8 || (cmf >> 4) > 7 ||
        ((((static_cast<uint32_t>(cmf) << 8) | static_cast<uint32_t>(flg)) % 31u) != 0u) ||
        (flg & 0x20) != 0) {
        return false; // Invalid header or preset dictionary -> fallback to Wuffs
    }

    if (checkCancel && checkCancel()) {
        outCancelled = true;
        return false;
    }

    uint8_t* outPtr = outSink.Allocate(pixelSize);
    if (!outPtr) {
        outCancelled = true; // Do not retry Wuffs when destination allocation fails
        return false;
    }
    inOutAllocatedPtr = outPtr;
    inOutAllocatedBytes = pixelSize;

    z_stream strm = {};
    if (inflateInit2(&strm, -15) != Z_OK) {
        return false;
    }

    strm.next_in = const_cast<Bytef*>(firstPayload + 2);
    strm.avail_in = static_cast<uInt>(curChunkLen - 2);

    const size_t dstRowBytes = static_cast<size_t>(scan.width) * outBpp;
    const uint64_t totalScanlineBytes = static_cast<uint64_t>(lineBytes) * static_cast<uint64_t>(scan.height);
    const bool usePipelinedRing = (scan.height >= 256 && totalScanlineBytes >= 16ULL * 1024ULL * 1024ULL);

    const bool streamOk = usePipelinedRing
        ? DecodePngPipelinedRing(data, size, scan, outPtr, rowBytes, lineBytes, bpp,
                                 dstRowBytes, output16Bit, checkCancel, curOffset, curChunkLen, strm, outCancelled)
        : DecodePngSingleThreadRows(data, size, scan, outPtr, rowBytes, lineBytes, bpp,
                                    dstRowBytes, output16Bit, checkCancel, curOffset, curChunkLen, strm, outCancelled);

    inflateEnd(&strm);
    if (!streamOk) {
        return false;
    }

    if (pInfo) {
        pInfo->hasAlpha = (scan.colorType == 4 || scan.colorType == 6);
        if (pInfo->bitDepth <= 0) pInfo->bitDepth = 8;
    }
    return true;
}

static bool DecodePNG_Impl(const uint8_t* data, size_t size, 
               uint32_t* outWidth, uint32_t* outHeight,
               BufferSink& outSink,
               CancelPredicate checkCancel,
               WuffsImageInfo* pInfo,
               bool force8BitBgra) {
    if (!data || size == 0) return false;

    const PngPreScanInfo scan = PreScanPngHeaderChunks(data, size, pInfo);
    uint8_t* preallocatedPtr = nullptr;
    size_t preallocatedBytes = 0;
    bool cancelled = false;

    if (TryDecodePngStreamingZlibNg(data, size, scan, outSink, checkCancel, pInfo,
                                    force8BitBgra, preallocatedPtr, preallocatedBytes, cancelled)) {
        *outWidth = scan.width;
        *outHeight = scan.height;
        return true;
    }
    if (cancelled) {
        return false;
    }

    wuffs_png__decoder dec;
    wuffs_base__status status = wuffs_png__decoder__initialize(
        &dec, sizeof(dec), WUFFS_VERSION,
        WUFFS_INITIALIZE__LEAVE_INTERNAL_BUFFERS_UNINITIALIZED);
    if (!wuffs_base__status__is_ok(&status)) return false;
    wuffs_png__decoder__set_quirk(&dec, WUFFS_BASE__QUIRK_IGNORE_CHECKSUM, 1);

    wuffs_base__io_buffer src = {};
    src.data.ptr = const_cast<uint8_t*>(data);
    src.data.len = size;
    src.meta.wi = std::min(size, (size_t)16777216); 
    src.meta.ri = 0;
    src.meta.closed = (src.meta.wi == size);

    wuffs_base__image_config ic = {};
    WUFFS_TRY(wuffs_png__decoder__decode_image_config(&dec, &ic, &src));

    uint32_t width = wuffs_base__pixel_config__width(&ic.pixcfg);
    uint32_t height = wuffs_base__pixel_config__height(&ic.pixcfg);
    if (width == 0 || height == 0) return false;

    // [v6.3] Extract Metadata (Zero Cost) - Before we override format for decoding
    if (pInfo) {
        pInfo->hasAlpha = !wuffs_base__image_config__first_frame_is_opaque(&ic);
        if (pInfo->bitDepth <= 0) pInfo->bitDepth = 8;
    }

    uint32_t targetFmt = WUFFS_BASE__PIXEL_FORMAT__BGRA_PREMUL;
    uint32_t bpp = 4;
    if (pInfo && pInfo->bitDepth == 16 && !force8BitBgra) {
        // Wuffs v0.4 only supports BGRA_NONPREMUL_4X16LE in its 16-bit PNG swizzler allowlist.
        targetFmt = WUFFS_BASE__PIXEL_FORMAT__BGRA_NONPREMUL_4X16LE;
        bpp = 8;
    }
    wuffs_base__pixel_config__set(&ic.pixcfg, targetFmt, WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, width, height);

    size_t pixelSize = (size_t)width * height * bpp;
    // Reuse preallocated buffer when falling back from fast path to prevent duplicate CapturingAlloc calls.
    uint8_t* outPtr = (preallocatedPtr && preallocatedBytes == pixelSize)
                          ? preallocatedPtr
                          : outSink.Allocate(pixelSize);
    if (!outPtr) return false;

    wuffs_base__pixel_buffer pb;
    status = wuffs_base__pixel_buffer__set_from_slice(&pb, &ic.pixcfg, wuffs_base__make_slice_u8(outPtr, pixelSize));
    if (!wuffs_base__status__is_ok(&status)) return false;

    uint64_t workbuf_len = wuffs_png__decoder__workbuf_len(&dec).max_incl;
    std::pmr::vector<uint8_t> workbuf(workbuf_len, outSink.memRes ? outSink.memRes : std::pmr::get_default_resource());

    wuffs_base__frame_config fc = {};
    WUFFS_TRY(wuffs_png__decoder__decode_frame_config(&dec, &fc, &src));

    WUFFS_TRY(wuffs_png__decoder__decode_frame(&dec, &pb, &src, WUFFS_BASE__PIXEL_BLEND__SRC, wuffs_base__make_slice_u8(workbuf.data(), workbuf.size()), nullptr));

    if (targetFmt == WUFFS_BASE__PIXEL_FORMAT__BGRA_NONPREMUL_4X16LE) {
        ImageLoaderSimd::SwizzleBGRA16ToRGBA16(
            reinterpret_cast<uint16_t*>(outPtr),
            static_cast<size_t>(width) * static_cast<size_t>(height));
    }

    *outWidth = width;
    *outHeight = height;
    return true;
}
bool DecodePNG(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, std::pmr::vector<uint8_t>& out, CancelPredicate c, WuffsImageInfo* p, bool force8BitBgra) { auto sink = MakeBufferSink(out); return DecodePNG_Impl(d, s, w, h, sink, c, p, force8BitBgra); }
bool DecodePNG(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, std::vector<uint8_t>& out, CancelPredicate c, WuffsImageInfo* p, bool force8BitBgra) { auto sink = MakeBufferSink(out); return DecodePNG_Impl(d, s, w, h, sink, c, p, force8BitBgra); }
bool DecodePNG(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, AllocatorFunc alloc, CancelPredicate c, WuffsImageInfo* p, bool force8BitBgra) { BufferAdapter a(alloc); auto sink = MakeBufferSink(a); return DecodePNG_Impl(d, s, w, h, sink, c, p, force8BitBgra); }

// ------------------------------------------------------------
// GIF Decoder
// ------------------------------------------------------------
// ------------------------------------------------------------
// GIF Decoder
// ------------------------------------------------------------
static bool DecodeGIF_Impl(const uint8_t* data, size_t size,
               uint32_t* outWidth, uint32_t* outHeight,
               BufferSink& outSink,
               CancelPredicate checkCancel) {
    wuffs_gif__decoder dec;
    wuffs_base__status status = wuffs_gif__decoder__initialize(
        &dec, sizeof(dec), WUFFS_VERSION,
        WUFFS_INITIALIZE__LEAVE_INTERNAL_BUFFERS_UNINITIALIZED);
    if (!wuffs_base__status__is_ok(&status)) return false;

    wuffs_base__io_buffer src = {};
    src.data.ptr = const_cast<uint8_t*>(data);
    src.data.len = size;
    src.meta.wi = std::min(size, (size_t)1048576);
    src.meta.ri = 0;
    src.meta.closed = (src.meta.wi == size);

    wuffs_base__image_config ic = {};
    WUFFS_TRY(wuffs_gif__decoder__decode_image_config(&dec, &ic, &src));

    uint32_t width = wuffs_base__pixel_config__width(&ic.pixcfg);
    uint32_t height = wuffs_base__pixel_config__height(&ic.pixcfg);
    if (width == 0 || height == 0) return false;

    wuffs_base__pixel_config__set(&ic.pixcfg, WUFFS_BASE__PIXEL_FORMAT__BGRA_PREMUL, WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, width, height);

    size_t pixelSize = (size_t)width * height * 4;
    uint8_t* outPtr = outSink.Allocate(pixelSize);
    if (!outPtr) return false;

    wuffs_base__pixel_buffer pb;
    status = wuffs_base__pixel_buffer__set_from_slice(&pb, &ic.pixcfg, wuffs_base__make_slice_u8(outPtr, pixelSize));
    if (!wuffs_base__status__is_ok(&status)) return false;

    uint64_t workbuf_len = wuffs_gif__decoder__workbuf_len(&dec).max_incl;
    std::pmr::vector<uint8_t> workbuf(workbuf_len, outSink.memRes ? outSink.memRes : std::pmr::get_default_resource());

    wuffs_base__frame_config fc = {};
    WUFFS_TRY(wuffs_gif__decoder__decode_frame_config(&dec, &fc, &src));

    WUFFS_TRY(wuffs_gif__decoder__decode_frame(&dec, &pb, &src, WUFFS_BASE__PIXEL_BLEND__SRC, wuffs_base__make_slice_u8(workbuf.data(), workbuf.size()), nullptr));

    *outWidth = width;
    *outHeight = height;
    return true;
}
bool DecodeGIF(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, std::pmr::vector<uint8_t>& out, CancelPredicate c) { auto sink = MakeBufferSink(out); return DecodeGIF_Impl(d, s, w, h, sink, c); }
bool DecodeGIF(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, std::vector<uint8_t>& out, CancelPredicate c) { auto sink = MakeBufferSink(out); return DecodeGIF_Impl(d, s, w, h, sink, c); }
bool DecodeGIF(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, AllocatorFunc alloc, CancelPredicate c) { BufferAdapter a(alloc); auto sink = MakeBufferSink(a); return DecodeGIF_Impl(d, s, w, h, sink, c); }

// ------------------------------------------------------------
// BMP Decoder
// ------------------------------------------------------------
static bool DecodeBMP_Impl(const uint8_t* data, size_t size,
               uint32_t* outWidth, uint32_t* outHeight,
               BufferSink& outSink,
               CancelPredicate checkCancel) {
    wuffs_bmp__decoder dec;
    wuffs_base__status status = wuffs_bmp__decoder__initialize(
        &dec, sizeof(dec), WUFFS_VERSION,
        WUFFS_INITIALIZE__LEAVE_INTERNAL_BUFFERS_UNINITIALIZED);
    if (!wuffs_base__status__is_ok(&status)) return false;

    wuffs_base__io_buffer src = {};
    src.data.ptr = const_cast<uint8_t*>(data);
    src.data.len = size;
    src.meta.wi = std::min(size, (size_t)1048576);
    src.meta.ri = 0;
    src.meta.closed = (src.meta.wi == size);

    wuffs_base__image_config ic = {};
    WUFFS_TRY(wuffs_bmp__decoder__decode_image_config(&dec, &ic, &src));

    uint32_t width = wuffs_base__pixel_config__width(&ic.pixcfg);
    uint32_t height = wuffs_base__pixel_config__height(&ic.pixcfg);
    if (width == 0 || height == 0) return false;

    wuffs_base__pixel_config__set(&ic.pixcfg, WUFFS_BASE__PIXEL_FORMAT__BGRA_PREMUL, WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, width, height);

    size_t pixelSize = (size_t)width * height * 4;
    uint8_t* outPtr = outSink.Allocate(pixelSize);
    if (!outPtr) return false;

    wuffs_base__pixel_buffer pb;
    status = wuffs_base__pixel_buffer__set_from_slice(&pb, &ic.pixcfg, wuffs_base__make_slice_u8(outPtr, pixelSize));
    if (!wuffs_base__status__is_ok(&status)) return false;

    uint64_t workbuf_len = wuffs_bmp__decoder__workbuf_len(&dec).max_incl;
    std::pmr::vector<uint8_t> workbuf(workbuf_len, outSink.memRes ? outSink.memRes : std::pmr::get_default_resource());

    wuffs_base__frame_config fc = {};
    WUFFS_TRY(wuffs_bmp__decoder__decode_frame_config(&dec, &fc, &src));

    WUFFS_TRY(wuffs_bmp__decoder__decode_frame(&dec, &pb, &src, WUFFS_BASE__PIXEL_BLEND__SRC, wuffs_base__make_slice_u8(workbuf.data(), workbuf.size()), nullptr));

    *outWidth = width;
    *outHeight = height;
    return true;
}
bool DecodeBMP(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, std::pmr::vector<uint8_t>& out, CancelPredicate c) { auto sink = MakeBufferSink(out); return DecodeBMP_Impl(d, s, w, h, sink, c); }
bool DecodeBMP(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, std::vector<uint8_t>& out, CancelPredicate c) { auto sink = MakeBufferSink(out); return DecodeBMP_Impl(d, s, w, h, sink, c); }
bool DecodeBMP(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, AllocatorFunc alloc, CancelPredicate c) { BufferAdapter a(alloc); auto sink = MakeBufferSink(a); return DecodeBMP_Impl(d, s, w, h, sink, c); }

// ------------------------------------------------------------
// TGA Decoder
// ------------------------------------------------------------
static bool DecodeTGA_Impl(const uint8_t* data, size_t size,
               uint32_t* outWidth, uint32_t* outHeight,
               BufferSink& outSink,
               CancelPredicate checkCancel) {
    wuffs_targa__decoder dec;
    wuffs_base__status status = wuffs_targa__decoder__initialize(
        &dec, sizeof(dec), WUFFS_VERSION,
        WUFFS_INITIALIZE__LEAVE_INTERNAL_BUFFERS_UNINITIALIZED);
    if (!wuffs_base__status__is_ok(&status)) return false;

    wuffs_base__io_buffer src = {};
    src.data.ptr = const_cast<uint8_t*>(data);
    src.data.len = size;
    src.meta.wi = std::min(size, (size_t)1048576);
    src.meta.ri = 0;
    src.meta.closed = (src.meta.wi == size);

    wuffs_base__image_config ic = {};
    WUFFS_TRY(wuffs_targa__decoder__decode_image_config(&dec, &ic, &src));

    uint32_t width = wuffs_base__pixel_config__width(&ic.pixcfg);
    uint32_t height = wuffs_base__pixel_config__height(&ic.pixcfg);
    if (width == 0 || height == 0) return false;

    wuffs_base__pixel_config__set(&ic.pixcfg, WUFFS_BASE__PIXEL_FORMAT__BGRA_PREMUL, WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, width, height);

    size_t pixelSize = (size_t)width * height * 4;
    uint8_t* outPtr = outSink.Allocate(pixelSize);
    if (!outPtr) return false;

    wuffs_base__pixel_buffer pb;
    status = wuffs_base__pixel_buffer__set_from_slice(&pb, &ic.pixcfg, wuffs_base__make_slice_u8(outPtr, pixelSize));
    if (!wuffs_base__status__is_ok(&status)) return false;

    uint64_t workbuf_len = wuffs_targa__decoder__workbuf_len(&dec).max_incl;
    std::pmr::vector<uint8_t> workbuf(workbuf_len, outSink.memRes ? outSink.memRes : std::pmr::get_default_resource());

    wuffs_base__frame_config fc = {};
    WUFFS_TRY(wuffs_targa__decoder__decode_frame_config(&dec, &fc, &src));

    WUFFS_TRY(wuffs_targa__decoder__decode_frame(&dec, &pb, &src, WUFFS_BASE__PIXEL_BLEND__SRC, wuffs_base__make_slice_u8(workbuf.data(), workbuf.size()), nullptr));

    *outWidth = width;
    *outHeight = height;
    return true;
}
bool DecodeTGA(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, std::pmr::vector<uint8_t>& out, CancelPredicate c) { auto sink = MakeBufferSink(out); return DecodeTGA_Impl(d, s, w, h, sink, c); }
bool DecodeTGA(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, std::vector<uint8_t>& out, CancelPredicate c) { auto sink = MakeBufferSink(out); return DecodeTGA_Impl(d, s, w, h, sink, c); }
bool DecodeTGA(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, AllocatorFunc alloc, CancelPredicate c) { BufferAdapter a(alloc); auto sink = MakeBufferSink(a); return DecodeTGA_Impl(d, s, w, h, sink, c); }

// ------------------------------------------------------------
// WBMP Decoder
// ------------------------------------------------------------
static bool DecodeWBMP_Impl(const uint8_t* data, size_t size,
               uint32_t* outWidth, uint32_t* outHeight,
               BufferSink& outSink,
               CancelPredicate checkCancel) {
    wuffs_wbmp__decoder dec;
    wuffs_base__status status = wuffs_wbmp__decoder__initialize(
        &dec, sizeof(dec), WUFFS_VERSION,
        WUFFS_INITIALIZE__LEAVE_INTERNAL_BUFFERS_UNINITIALIZED);
    if (!wuffs_base__status__is_ok(&status)) return false;

    wuffs_base__io_buffer src = {};
    src.data.ptr = const_cast<uint8_t*>(data);
    src.data.len = size;
    src.meta.wi = std::min(size, (size_t)1048576);
    src.meta.ri = 0;
    src.meta.closed = (src.meta.wi == size);

    wuffs_base__image_config ic = {};
    WUFFS_TRY(wuffs_wbmp__decoder__decode_image_config(&dec, &ic, &src));

    uint32_t width = wuffs_base__pixel_config__width(&ic.pixcfg);
    uint32_t height = wuffs_base__pixel_config__height(&ic.pixcfg);
    if (width == 0 || height == 0) return false;

    wuffs_base__pixel_config__set(&ic.pixcfg, WUFFS_BASE__PIXEL_FORMAT__BGRA_PREMUL, WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, width, height);

    size_t pixelSize = (size_t)width * height * 4;
    uint8_t* outPtr = outSink.Allocate(pixelSize);
    if (!outPtr) return false;

    wuffs_base__pixel_buffer pb;
    status = wuffs_base__pixel_buffer__set_from_slice(&pb, &ic.pixcfg, wuffs_base__make_slice_u8(outPtr, pixelSize));
    if (!wuffs_base__status__is_ok(&status)) return false;

    uint64_t workbuf_len = wuffs_wbmp__decoder__workbuf_len(&dec).max_incl;
    std::pmr::vector<uint8_t> workbuf(workbuf_len, outSink.memRes ? outSink.memRes : std::pmr::get_default_resource());

    wuffs_base__frame_config fc = {};
    WUFFS_TRY(wuffs_wbmp__decoder__decode_frame_config(&dec, &fc, &src));

    WUFFS_TRY(wuffs_wbmp__decoder__decode_frame(&dec, &pb, &src, WUFFS_BASE__PIXEL_BLEND__SRC, wuffs_base__make_slice_u8(workbuf.data(), workbuf.size()), nullptr));

    *outWidth = width;
    *outHeight = height;
    return true;
}
bool DecodeWBMP(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, std::pmr::vector<uint8_t>& out, CancelPredicate c) { auto sink = MakeBufferSink(out); return DecodeWBMP_Impl(d, s, w, h, sink, c); }
bool DecodeWBMP(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, std::vector<uint8_t>& out, CancelPredicate c) { auto sink = MakeBufferSink(out); return DecodeWBMP_Impl(d, s, w, h, sink, c); }
bool DecodeWBMP(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, AllocatorFunc alloc, CancelPredicate c) { BufferAdapter a(alloc); auto sink = MakeBufferSink(a); return DecodeWBMP_Impl(d, s, w, h, sink, c); }

// ------------------------------------------------------------
// NetPBM Decoder
// ------------------------------------------------------------
static bool DecodeNetpbm_Impl(const uint8_t* data, size_t size,
               uint32_t* outWidth, uint32_t* outHeight,
               BufferSink& outSink,
               CancelPredicate checkCancel) {
    wuffs_netpbm__decoder dec;
    wuffs_base__status status = wuffs_netpbm__decoder__initialize(
        &dec, sizeof(dec), WUFFS_VERSION,
        WUFFS_INITIALIZE__LEAVE_INTERNAL_BUFFERS_UNINITIALIZED);
    if (!wuffs_base__status__is_ok(&status)) return false;

    wuffs_base__io_buffer src = {};
    src.data.ptr = const_cast<uint8_t*>(data);
    src.data.len = size;
    src.meta.wi = std::min(size, (size_t)1048576);
    src.meta.ri = 0;
    src.meta.closed = (src.meta.wi == size);

    wuffs_base__image_config ic = {};
    WUFFS_TRY(wuffs_netpbm__decoder__decode_image_config(&dec, &ic, &src));

    uint32_t width = wuffs_base__pixel_config__width(&ic.pixcfg);
    uint32_t height = wuffs_base__pixel_config__height(&ic.pixcfg);
    if (width == 0 || height == 0) return false;

    wuffs_base__pixel_config__set(&ic.pixcfg, WUFFS_BASE__PIXEL_FORMAT__BGRA_PREMUL, WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, width, height);

    size_t pixelSize = (size_t)width * height * 4;
    uint8_t* outPtr = outSink.Allocate(pixelSize);
    if (!outPtr) return false;

    wuffs_base__pixel_buffer pb;
    status = wuffs_base__pixel_buffer__set_from_slice(&pb, &ic.pixcfg, wuffs_base__make_slice_u8(outPtr, pixelSize));
    if (!wuffs_base__status__is_ok(&status)) return false;

    uint64_t workbuf_len = wuffs_netpbm__decoder__workbuf_len(&dec).max_incl;
    std::pmr::vector<uint8_t> workbuf(workbuf_len, outSink.memRes ? outSink.memRes : std::pmr::get_default_resource());

    wuffs_base__frame_config fc = {};
    WUFFS_TRY(wuffs_netpbm__decoder__decode_frame_config(&dec, &fc, &src));

    WUFFS_TRY(wuffs_netpbm__decoder__decode_frame(&dec, &pb, &src, WUFFS_BASE__PIXEL_BLEND__SRC, wuffs_base__make_slice_u8(workbuf.data(), workbuf.size()), nullptr));

    *outWidth = width;
    *outHeight = height;
    return true;
}
bool DecodeNetpbm(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, std::pmr::vector<uint8_t>& out, CancelPredicate c) { auto sink = MakeBufferSink(out); return DecodeNetpbm_Impl(d, s, w, h, sink, c); }
bool DecodeNetpbm(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, std::vector<uint8_t>& out, CancelPredicate c) { auto sink = MakeBufferSink(out); return DecodeNetpbm_Impl(d, s, w, h, sink, c); }
bool DecodeNetpbm(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, AllocatorFunc alloc, CancelPredicate c) { BufferAdapter a(alloc); auto sink = MakeBufferSink(a); return DecodeNetpbm_Impl(d, s, w, h, sink, c); }

// ------------------------------------------------------------
// QOI Decoder
// ------------------------------------------------------------
static bool DecodeQOI_Impl(const uint8_t* data, size_t size,
               uint32_t* outWidth, uint32_t* outHeight,
               BufferSink& outSink,
               CancelPredicate checkCancel) {
    wuffs_qoi__decoder dec;
    wuffs_base__status status = wuffs_qoi__decoder__initialize(
        &dec, sizeof(dec), WUFFS_VERSION,
        WUFFS_INITIALIZE__LEAVE_INTERNAL_BUFFERS_UNINITIALIZED);
    if (!wuffs_base__status__is_ok(&status)) return false;

    wuffs_base__io_buffer src = {};
    src.data.ptr = const_cast<uint8_t*>(data);
    src.data.len = size;
    src.meta.wi = std::min(size, (size_t)1048576);
    src.meta.ri = 0;
    src.meta.closed = (src.meta.wi == size);

    wuffs_base__image_config ic = {};
    WUFFS_TRY(wuffs_qoi__decoder__decode_image_config(&dec, &ic, &src));

    uint32_t width = wuffs_base__pixel_config__width(&ic.pixcfg);
    uint32_t height = wuffs_base__pixel_config__height(&ic.pixcfg);
    if (width == 0 || height == 0) return false;

    wuffs_base__pixel_config__set(&ic.pixcfg, WUFFS_BASE__PIXEL_FORMAT__BGRA_PREMUL, WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, width, height);

    size_t pixelSize = (size_t)width * height * 4;
    uint8_t* outPtr = outSink.Allocate(pixelSize);
    if (!outPtr) return false;

    wuffs_base__pixel_buffer pb;
    status = wuffs_base__pixel_buffer__set_from_slice(&pb, &ic.pixcfg, wuffs_base__make_slice_u8(outPtr, pixelSize));
    if (!wuffs_base__status__is_ok(&status)) return false;

    uint64_t workbuf_len = wuffs_qoi__decoder__workbuf_len(&dec).max_incl;
    std::pmr::vector<uint8_t> workbuf(workbuf_len, outSink.memRes ? outSink.memRes : std::pmr::get_default_resource());

    wuffs_base__frame_config fc = {};
    WUFFS_TRY(wuffs_qoi__decoder__decode_frame_config(&dec, &fc, &src));

    WUFFS_TRY(wuffs_qoi__decoder__decode_frame(&dec, &pb, &src, WUFFS_BASE__PIXEL_BLEND__SRC, wuffs_base__make_slice_u8(workbuf.data(), workbuf.size()), nullptr));

    *outWidth = width;
    *outHeight = height;
    return true;
}
bool DecodeQOI(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, std::pmr::vector<uint8_t>& out, CancelPredicate c) { auto sink = MakeBufferSink(out); return DecodeQOI_Impl(d, s, w, h, sink, c); }
bool DecodeQOI(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, std::vector<uint8_t>& out, CancelPredicate c) { auto sink = MakeBufferSink(out); return DecodeQOI_Impl(d, s, w, h, sink, c); }
bool DecodeQOI(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, AllocatorFunc alloc, CancelPredicate c) { BufferAdapter a(alloc); auto sink = MakeBufferSink(a); return DecodeQOI_Impl(d, s, w, h, sink, c); }

#undef WUFFS_TRY

} // namespace WuffsLoader

// ============================================================================
// Wuffs Animator (GIF/APNG)
// ============================================================================

namespace QuickView {

class WuffsAnimator : public IAnimationDecoder {
public:
    WuffsAnimator() {
        memset(&m_gifDec, 0, sizeof(m_gifDec));
        memset(&m_pngDec, 0, sizeof(m_pngDec));
        memset(&m_src, 0, sizeof(m_src));
    }
    
    ~WuffsAnimator() override = default;

    bool Initialize(std::shared_ptr<QuickView::MappedFile> file,
                    QuickView::PixelFormat /*preferredFormat*/) override {
      if (!file || !file->IsValid() || !file->data() || file->size() == 0) {
        return false;
      }
      m_mappedFile = file;
      const uint8_t *data = file->data();
      size_t size = file->size();

      m_src.data.ptr = const_cast<uint8_t *>(data);
      m_src.data.len = size;
      m_src.meta.wi = std::min(size, (size_t)1048576);
      m_src.meta.ri = 0;
      m_src.meta.closed = (m_src.meta.wi == size);

      // Try GIF first
      wuffs_base__status status = wuffs_gif__decoder__initialize(
          &m_gifDec, sizeof(m_gifDec), WUFFS_VERSION,
          WUFFS_INITIALIZE__LEAVE_INTERNAL_BUFFERS_UNINITIALIZED);

      if (wuffs_base__status__is_ok(&status)) {
        status = DecodeConfigWithShortRead(true, m_gifDec, m_pngDec, m_ic, m_src);
        if (wuffs_base__status__is_ok(&status)) {
          // Verify actual animated GIF via multiple Image Descriptors.
          bool hasAnim = false;
          uint32_t image_count = 0;
          const uint8_t *p = data;
          size_t offset = 13; // Header (6) + LSD (7)
          if (size > 13) {
            uint8_t lsd_packed = p[10];
            if (lsd_packed & 0x80) { // Global Color Table Flag
              size_t gctSize = 3u * (2u << (lsd_packed & 0x07));
              offset = (gctSize <= size - offset) ? (offset + gctSize) : size;
            }
            while (offset < size) {
              uint8_t block_type = p[offset++];
              if (block_type == 0x3B) { // Trailer
                break;
              } else if (block_type == 0x2C) { // Image Descriptor
                image_count++;
                if (image_count > 1) {
                  hasAnim = true;
                }
                if (offset + 9 > size)
                  break;
                uint8_t id_packed = p[offset + 8];
                offset += 9;
                if (id_packed & 0x80) { // Local Color Table Flag
                  size_t lctSize = 3u * (2u << (id_packed & 0x07));
                  if (lctSize > size - offset)
                    break;
                  offset += lctSize;
                }
                if (offset >= size)
                  break;
                offset++; // LZW minimum code size
                while (offset < size) {
                  uint8_t sub_len = p[offset++];
                  if (sub_len == 0)
                    break;
                  if (sub_len > size - offset) {
                    offset = size;
                    break;
                  }
                  offset += sub_len;
                }
              } else if (block_type == 0x21) { // Extension
                if (offset >= size)
                  break;
                offset++; // Skip ext_type
                // Skip extension sub-blocks
                while (offset < size) {
                  uint8_t sub_len = p[offset++];
                  if (sub_len == 0)
                    break;
                  if (sub_len > size - offset) {
                    offset = size;
                    break;
                  }
                  offset += sub_len;
                }
              } else {
                break; // Unknown block, stop
              }
            }
          }
          if (hasAnim) {
            m_isGif = true;
            m_knownTotalFrames = image_count;
          } else {
            return false; // Fallback to static GIF fast-path
          }
        }
      }

      if (!m_isGif) {
        // Try PNG (APNG) with full short-read config expansion support (>1MB metadata)
        m_src.meta.ri = 0;
        m_src.meta.wi = std::min(size, (size_t)1048576);
        m_src.meta.closed = (m_src.meta.wi == size);
        status = wuffs_png__decoder__initialize(
            &m_pngDec, sizeof(m_pngDec), WUFFS_VERSION,
            WUFFS_INITIALIZE__LEAVE_INTERNAL_BUFFERS_UNINITIALIZED);
        if (wuffs_base__status__is_ok(&status)) {
          wuffs_png__decoder__set_quirk(&m_pngDec, WUFFS_BASE__QUIRK_IGNORE_CHECKSUM, 1);
          status = DecodeConfigWithShortRead(false, m_gifDec, m_pngDec, m_ic, m_src);
          if (wuffs_base__status__is_ok(&status)) {
            const uint32_t apng_frames =
                m_pngDec.private_impl.f_seen_actl
                    ? m_pngDec.private_impl.f_num_animation_frames_value
                    : 1u;
            if (apng_frames > 1) {
              m_isPng = true;
              m_knownTotalFrames = apng_frames;
            } else {
              return false; // Fallback to static PNG fast-path
            }
          } else {
            return false;
          }
        } else {
          return false;
        }
      }

      m_width = wuffs_base__pixel_config__width(&m_ic.pixcfg);
      m_height = wuffs_base__pixel_config__height(&m_ic.pixcfg);
      if (m_width == 0 || m_height == 0)
        return false;

      // Is it actually animated?
      uint32_t num_loops = 0;
      (void)num_loops;
      (void)num_loops;
      if (m_isGif)
        num_loops = wuffs_gif__decoder__num_animation_loops(&m_gifDec);
      if (m_isPng)
        num_loops = wuffs_png__decoder__num_animation_loops(&m_pngDec);
      // Note: num_loops can be 0 (infinite) and still be animated. A better
      // check is if first frame config implies more.

      // Let's assume initialized successfully
      wuffs_base__pixel_config__set(
          &m_ic.pixcfg, WUFFS_BASE__PIXEL_FORMAT__BGRA_PREMUL,
          WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, m_width, m_height);

      m_pixelBytes = (size_t)m_width * m_height * 4;
      m_canvas.resize(m_pixelBytes);

      status = wuffs_base__pixel_buffer__set_from_slice(
          &m_pb, &m_ic.pixcfg,
          wuffs_base__make_slice_u8(m_canvas.data(), m_pixelBytes));
      if (!wuffs_base__status__is_ok(&status))
        return false;

      uint64_t workbuf_len = 0;
      if (m_isGif)
        workbuf_len = wuffs_gif__decoder__workbuf_len(&m_gifDec).max_incl;
      if (m_isPng)
        workbuf_len = wuffs_png__decoder__workbuf_len(&m_pngDec).max_incl;
      m_workbuf.resize((size_t)workbuf_len);

      m_lastDisposal = FrameDisposalMode::Unspecified;
      m_lastRect = {};

      m_isAnimated = true; // For now.
      // Defer background indexer until non-zero seek to avoid cold-boot CPU/memory contention.
      return true;
    }

    bool DecodeFrameInternal(bool copyPixels, std::shared_ptr<RawImageFrame>& outFrame) {
        wuffs_base__status status;
        wuffs_base__frame_config fc = {};

        // Process previous frame disposalBEFORE decoding new frame config
        if (m_currentIndex > 0) {
            if (m_lastDisposal == FrameDisposalMode::RestoreBackground) {
                // Clear the previous frame's rect to transparent (BGRA 0)
                int safeLeft = std::max(0, m_lastRect.left);
                int safeTop = std::max(0, m_lastRect.top);
                int safeRight = std::min((int)m_width, m_lastRect.right);
                int safeBottom = std::min((int)m_height, m_lastRect.bottom);
                for (int y = safeTop; y < safeBottom; y++) {
                    uint8_t* row = m_canvas.data() + (y * m_width * 4) + safeLeft * 4;
                    memset(row, 0, (safeRight - safeLeft) * 4);
                }
            } else if (m_lastDisposal == FrameDisposalMode::RestorePrevious) {
                if (m_restoreBuffer.size() == m_canvas.size()) {
                    memcpy(m_canvas.data(), m_restoreBuffer.data(), m_canvas.size());
                }
            }
        }

        // Save snapshot is now handled by the background indexer

        // Decode Frame Config
        while (true) {
            if (m_isGif) status = wuffs_gif__decoder__decode_frame_config(&m_gifDec, &fc, &m_src);
            else if (m_isPng) status = wuffs_png__decoder__decode_frame_config(&m_pngDec, &fc, &m_src);
            
            if (wuffs_base__status__is_ok(&status)) break;
            if (status.repr == wuffs_base__suspension__short_read && !m_src.meta.closed) {
                size_t next = std::min(m_mappedFile->size(), m_src.meta.wi + 1048576);
                m_src.meta.wi = next;
                m_src.meta.closed = (next == m_mappedFile->size());
                continue;
            }
            // End of animation or error
            return false;
        }

        // Determine blend mode from frame config
        wuffs_base__pixel_blend blendMode = WUFFS_BASE__PIXEL_BLEND__SRC_OVER;
        if (wuffs_base__frame_config__overwrite_instead_of_blend(&fc)) {
            blendMode = WUFFS_BASE__PIXEL_BLEND__SRC;
        }

        uint8_t currentDisposal = wuffs_base__frame_config__disposal(&fc);
        if (currentDisposal == WUFFS_BASE__ANIMATION_DISPOSAL__RESTORE_PREVIOUS) {
            if (m_restoreBuffer.size() != m_canvas.size()) m_restoreBuffer.resize(m_canvas.size());
            memcpy(m_restoreBuffer.data(), m_canvas.data(), m_canvas.size());
        }

        // Decode Frame Pixels into Canvas
        while (true) {
            if (m_isGif) {
                status = wuffs_gif__decoder__decode_frame(&m_gifDec, &m_pb, &m_src, 
                    blendMode, wuffs_base__make_slice_u8(m_workbuf.data(), m_workbuf.size()), nullptr);
            } else if (m_isPng) {
                status = wuffs_png__decoder__decode_frame(&m_pngDec, &m_pb, &m_src, 
                    blendMode, wuffs_base__make_slice_u8(m_workbuf.data(), m_workbuf.size()), nullptr);
            }
            
            if (wuffs_base__status__is_ok(&status)) break;
            if (status.repr == wuffs_base__suspension__short_read && !m_src.meta.closed) {
                size_t next = std::min(m_mappedFile->size(), m_src.meta.wi + 1048576);
                m_src.meta.wi = next;
                m_src.meta.closed = (next == m_mappedFile->size());
                continue;
            }
            return false;
        }

        // Extract Meta and Disposal
        uint8_t d = wuffs_base__frame_config__disposal(&fc);
        FrameDisposalMode nextDisposal = FrameDisposalMode::Keep;
        if (d == WUFFS_BASE__ANIMATION_DISPOSAL__RESTORE_BACKGROUND) nextDisposal = FrameDisposalMode::RestoreBackground;
        else if (d == WUFFS_BASE__ANIMATION_DISPOSAL__RESTORE_PREVIOUS) nextDisposal = FrameDisposalMode::RestorePrevious;
        
        wuffs_base__rect_ie_u32 bounds = wuffs_base__frame_config__bounds(&fc);
        m_lastDisposal = nextDisposal;
        m_lastRect.left = bounds.min_incl_x;
        m_lastRect.top = bounds.min_incl_y;
        m_lastRect.right = bounds.max_excl_x;
        m_lastRect.bottom = bounds.max_excl_y;

        if (copyPixels) {
            // Build shared frame wrapper
            auto frame = std::make_shared<RawImageFrame>();
            
            size_t bufSize = m_canvas.size();
            frame->pixels = (uint8_t*)_aligned_malloc(bufSize, 64);
            frame->memoryDeleter = QuickView::MemoryDeleter::FromAlignedFree();
            memcpy(frame->pixels, m_canvas.data(), bufSize);
            
            frame->width = m_width;
            frame->height = m_height;
            frame->stride = m_width * 4; // Canvas stride is always width * 4
            frame->format = PixelFormat::BGRA8888;
            frame->quality = DecodeQuality::Full;
            
            // Extract Meta
            auto& meta = frame->frameMeta;
            meta.index = m_currentIndex;
            
            wuffs_base__flicks ticks = wuffs_base__frame_config__duration(&fc);
            meta.delayMs = (uint32_t)(ticks / (WUFFS_BASE__FLICKS_PER_SECOND / 1000));
            if (meta.delayMs == 0) meta.delayMs = 100; // Fallback
            meta.disposal = nextDisposal;
            meta.rcLeft = bounds.min_incl_x;
            meta.rcTop = bounds.min_incl_y;
            meta.rcRight = bounds.max_excl_x;
            meta.rcBottom = bounds.max_excl_y;
            meta.isDelta = true;
            
            outFrame = frame;
        }

        m_currentIndex++;
        uint32_t known = m_knownTotalFrames.load(std::memory_order_relaxed);
        if (m_currentIndex > known) {
            m_knownTotalFrames.store(m_currentIndex, std::memory_order_relaxed);
        }
        
        return true;
    }

    std::shared_ptr<RawImageFrame> GetNextFrame() override {
        std::shared_ptr<RawImageFrame> frame;
        if (DecodeFrameInternal(true, frame)) {
            return frame;
        }
        return nullptr;
    }

    std::shared_ptr<RawImageFrame> SeekToFrame(uint32_t targetIndex) override {
        if (targetIndex > 0) {
            EnsureIndexerStarted();
        }
        if (targetIndex == m_currentIndex) {
            // Already there
            return GetNextFrame(); // wait, GetNextFrame advances to current+1
        }
        
        uint32_t bestIndex = 0;
        if (m_currentIndex <= targetIndex) {
            bestIndex = m_currentIndex;
        }

        // Find closest snapshot <= targetIndex
        int bestSnap = -1;
        std::shared_lock<std::shared_mutex> lock(m_snapshotMutex);
        for (int i = (int)m_snapshots.size() - 1; i >= 0; i--) {
            if (m_snapshots[i].index <= targetIndex) {
                bestSnap = i;
                break;
            }
        }
        
        if (bestSnap >= 0 && m_snapshots[bestSnap].index > bestIndex) {
            // Restore snapshot
            auto& snap = m_snapshots[bestSnap];
            m_currentIndex = snap.index;
            memcpy(m_canvas.data(), snap.canvas.data(), m_canvas.size());
            m_src = snap.srcState;
            m_lastDisposal = snap.lastDisposal;
            m_lastRect = snap.lastRect;
            if (m_isGif) memcpy(&m_gifDec, snap.decoderState.data(), sizeof(m_gifDec));
            else if (m_isPng) memcpy(&m_pngDec, snap.decoderState.data(), sizeof(m_pngDec));
        } else if (m_currentIndex > targetIndex) {
            // We need to rewind, but no snapshot was closer than 0.
            // Re-initialize foreground decoder only (do not clear background indexer)
            wuffs_base__status status = {};
            m_src.meta.ri = 0; // Rewind IO
            m_src.meta.wi = std::min(m_mappedFile->size(), (size_t)1048576);
            m_src.meta.closed = (m_src.meta.wi == m_mappedFile->size());
            if (m_isGif) {
                status = wuffs_gif__decoder__initialize(&m_gifDec, sizeof(m_gifDec), WUFFS_VERSION, WUFFS_INITIALIZE__LEAVE_INTERNAL_BUFFERS_UNINITIALIZED);
                if (!wuffs_base__status__is_ok(&status)) return nullptr;
                status = DecodeConfigWithShortRead(true, m_gifDec, m_pngDec, m_ic, m_src);
                if (!wuffs_base__status__is_ok(&status)) return nullptr;
            } else if (m_isPng) {
                status = wuffs_png__decoder__initialize(&m_pngDec, sizeof(m_pngDec), WUFFS_VERSION, WUFFS_INITIALIZE__LEAVE_INTERNAL_BUFFERS_UNINITIALIZED);
                if (!wuffs_base__status__is_ok(&status)) return nullptr;
                status = DecodeConfigWithShortRead(false, m_gifDec, m_pngDec, m_ic, m_src);
                if (!wuffs_base__status__is_ok(&status)) return nullptr;
            }
            wuffs_base__pixel_config__set(
                &m_ic.pixcfg, WUFFS_BASE__PIXEL_FORMAT__BGRA_PREMUL,
                WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, m_width, m_height);
            memset(m_canvas.data(), 0, m_canvas.size());
            m_currentIndex = 0;
            m_lastDisposal = FrameDisposalMode::Unspecified;
            m_lastRect = {};
        }
        lock.unlock();
        
        // Fast-forward WITHOUT memory allocation & pixel copying
        while (m_currentIndex < targetIndex) {
            std::shared_ptr<RawImageFrame> dummy;
            if (!DecodeFrameInternal(false, dummy)) break; // Error or EOF
        }
        
        // Return target frame WITH memory allocation
        return GetNextFrame();
    }

    uint32_t GetTotalFrames() const override { return m_knownTotalFrames.load(std::memory_order_relaxed); }
    bool IsAnimated() const override { return m_isAnimated; }
    bool SupportsDirtyRect() const override { return true; }

private:
    void EnsureIndexerStarted() {
        std::call_once(m_indexerOnce, [this]() {
            m_indexerThread = std::jthread([this](std::stop_token st) {
                BackgroundIndexer(st);
            });
        });
    }
    struct Rect { int left = 0; int top = 0; int right = 0; int bottom = 0; };

    struct Snapshot {
        uint32_t index;
        std::vector<uint8_t> canvas;
        std::vector<uint8_t> decoderState;
        wuffs_base__io_buffer srcState;
        FrameDisposalMode lastDisposal;
        Rect lastRect;
    };
    
    void BackgroundIndexer(std::stop_token st) {
        __try {
            BackgroundIndexerInternal(st);
        }
        __except (GetExceptionCode() == STATUS_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
            // [Stability Fix] Safely exit if MMF page fault fails (e.g. temporary file deleted, disk removed).
        }
    }

    void BackgroundIndexerInternal(std::stop_token st) {
        if (!m_mappedFile || !m_mappedFile->IsValid()) return;
        const size_t fileSize = m_mappedFile->size();
        wuffs_gif__decoder bgGifDec;
        wuffs_png__decoder bgPngDec;
        wuffs_base__io_buffer bgSrc = {};
        bgSrc.data.ptr = const_cast<uint8_t*>(m_mappedFile->data());
        bgSrc.data.len = fileSize;
        bgSrc.meta.wi = std::min(fileSize, (size_t)1048576);
        bgSrc.meta.ri = 0;
        bgSrc.meta.closed = (bgSrc.meta.wi == fileSize);

        wuffs_base__status status = {};
        wuffs_base__image_config ic = {};
        if (m_isGif) {
            status = wuffs_gif__decoder__initialize(&bgGifDec, sizeof(bgGifDec), WUFFS_VERSION, WUFFS_INITIALIZE__LEAVE_INTERNAL_BUFFERS_UNINITIALIZED);
            if (!wuffs_base__status__is_ok(&status)) return;
            status = DecodeConfigWithShortRead(true, bgGifDec, bgPngDec, ic, bgSrc);
            if (!wuffs_base__status__is_ok(&status)) return;
        } else if (m_isPng) {
            status = wuffs_png__decoder__initialize(&bgPngDec, sizeof(bgPngDec), WUFFS_VERSION, WUFFS_INITIALIZE__LEAVE_INTERNAL_BUFFERS_UNINITIALIZED);
            if (!wuffs_base__status__is_ok(&status)) return;
            wuffs_png__decoder__set_quirk(&bgPngDec, WUFFS_BASE__QUIRK_IGNORE_CHECKSUM, 1);
            status = DecodeConfigWithShortRead(false, bgGifDec, bgPngDec, ic, bgSrc);
            if (!wuffs_base__status__is_ok(&status)) return;
        } else {
            return;
        }

        std::vector<uint8_t> bgCanvas(m_canvas.size(), 0);
        std::vector<uint8_t> bgWorkbuf(m_workbuf.size());
        wuffs_base__pixel_buffer bgPb = {};
        wuffs_base__pixel_config bgIc = {};
        wuffs_base__pixel_config__set(
            &bgIc, WUFFS_BASE__PIXEL_FORMAT__BGRA_PREMUL,
            WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, m_width, m_height);
        status = wuffs_base__pixel_buffer__set_from_slice(&bgPb, &bgIc, wuffs_base__make_slice_u8(bgCanvas.data(), bgCanvas.size()));
        if (!wuffs_base__status__is_ok(&status)) return;

        uint32_t bgCurrentIndex = 0;
        FrameDisposalMode bgLastDisposal = FrameDisposalMode::Unspecified;
        Rect bgLastRect = {};
        
        uint32_t snapshotInterval = 20;
        size_t memoryLimit = 256 * 1024 * 1024; // 256MB
        size_t currentMemory = 0;
        
        std::vector<uint8_t> bgRestoreBuffer;

        while (!st.stop_requested()) {
            if (bgCurrentIndex > 0) {
                if (bgLastDisposal == FrameDisposalMode::RestoreBackground) {
                    int safeLeft = std::max(0, bgLastRect.left);
                    int safeTop = std::max(0, bgLastRect.top);
                    int safeRight = std::min((int)m_width, bgLastRect.right);
                    int safeBottom = std::min((int)m_height, bgLastRect.bottom);
                    for (int y = safeTop; y < safeBottom; y++) {
                        uint8_t* row = bgCanvas.data() + (y * m_width * 4) + safeLeft * 4;
                        memset(row, 0, (safeRight - safeLeft) * 4);
                    }
                } else if (bgLastDisposal == FrameDisposalMode::RestorePrevious) {
                    if (bgRestoreBuffer.size() == bgCanvas.size()) {
                        memcpy(bgCanvas.data(), bgRestoreBuffer.data(), bgCanvas.size());
                    }
                }
            }

            if (bgCurrentIndex % snapshotInterval == 0 && bgLastDisposal != FrameDisposalMode::RestorePrevious) {
                Snapshot snap;
                snap.index = bgCurrentIndex;
                snap.canvas = bgCanvas;
                snap.srcState = bgSrc;
                snap.lastDisposal = bgLastDisposal;
                snap.lastRect = bgLastRect;
                if (m_isGif) {
                    snap.decoderState.resize(sizeof(wuffs_gif__decoder));
                    memcpy(snap.decoderState.data(), &bgGifDec, sizeof(wuffs_gif__decoder));
                } else if (m_isPng) {
                    snap.decoderState.resize(sizeof(wuffs_png__decoder));
                    memcpy(snap.decoderState.data(), &bgPngDec, sizeof(wuffs_png__decoder));
                }
                
                size_t snapSize = snap.canvas.size() + snap.decoderState.size();
                
                std::unique_lock<std::shared_mutex> lock(m_snapshotMutex);
                m_snapshots.push_back(std::move(snap));
                currentMemory += snapSize;
                
                if (currentMemory > memoryLimit) {
                    std::vector<Snapshot> newSnapshots;
                    currentMemory = 0;
                    for (size_t i = 0; i < m_snapshots.size(); i += 2) {
                        newSnapshots.push_back(std::move(m_snapshots[i]));
                        currentMemory += newSnapshots.back().canvas.size() + newSnapshots.back().decoderState.size();
                    }
                    m_snapshots = std::move(newSnapshots);
                    snapshotInterval *= 2;
                }
                lock.unlock();
            }

            wuffs_base__frame_config fc = {};
            while (true) {
                if (st.stop_requested()) return;
                if (m_isGif) status = wuffs_gif__decoder__decode_frame_config(&bgGifDec, &fc, &bgSrc);
                else if (m_isPng) status = wuffs_png__decoder__decode_frame_config(&bgPngDec, &fc, &bgSrc);
                
                if (wuffs_base__status__is_ok(&status)) break;
                if (status.repr == wuffs_base__suspension__short_read && !bgSrc.meta.closed) {
                    size_t next = std::min(m_mappedFile->size(), bgSrc.meta.wi + 1048576);
                    bgSrc.meta.wi = next;
                    bgSrc.meta.closed = (next == m_mappedFile->size());
                    continue;
                }
                return;
            }

            wuffs_base__pixel_blend blendMode = WUFFS_BASE__PIXEL_BLEND__SRC_OVER;
            if (wuffs_base__frame_config__overwrite_instead_of_blend(&fc)) blendMode = WUFFS_BASE__PIXEL_BLEND__SRC;

            uint8_t currentDisposal = wuffs_base__frame_config__disposal(&fc);
            if (currentDisposal == WUFFS_BASE__ANIMATION_DISPOSAL__RESTORE_PREVIOUS) {
                if (bgRestoreBuffer.size() != bgCanvas.size()) bgRestoreBuffer.resize(bgCanvas.size());
                memcpy(bgRestoreBuffer.data(), bgCanvas.data(), bgCanvas.size());
            }

            while (true) {
                if (st.stop_requested()) return;
                if (m_isGif) status = wuffs_gif__decoder__decode_frame(&bgGifDec, &bgPb, &bgSrc, blendMode, wuffs_base__make_slice_u8(bgWorkbuf.data(), bgWorkbuf.size()), nullptr);
                else if (m_isPng) status = wuffs_png__decoder__decode_frame(&bgPngDec, &bgPb, &bgSrc, blendMode, wuffs_base__make_slice_u8(bgWorkbuf.data(), bgWorkbuf.size()), nullptr);
                
                if (wuffs_base__status__is_ok(&status)) break;
                if (status.repr == wuffs_base__suspension__short_read && !bgSrc.meta.closed) {
                    size_t next = std::min(m_mappedFile->size(), bgSrc.meta.wi + 1048576);
                    bgSrc.meta.wi = next;
                    bgSrc.meta.closed = (next == m_mappedFile->size());
                    continue;
                }
                return;
            }

            uint8_t d = wuffs_base__frame_config__disposal(&fc);
            if (d == WUFFS_BASE__ANIMATION_DISPOSAL__RESTORE_BACKGROUND) bgLastDisposal = FrameDisposalMode::RestoreBackground;
            else if (d == WUFFS_BASE__ANIMATION_DISPOSAL__RESTORE_PREVIOUS) bgLastDisposal = FrameDisposalMode::RestorePrevious;
            else bgLastDisposal = FrameDisposalMode::Keep;
            
            wuffs_base__rect_ie_u32 bounds = wuffs_base__frame_config__bounds(&fc);
            bgLastRect.left = bounds.min_incl_x;
            bgLastRect.top = bounds.min_incl_y;
            bgLastRect.right = bounds.max_excl_x;
            bgLastRect.bottom = bounds.max_excl_y;

            bgCurrentIndex++;
            uint32_t known = m_knownTotalFrames.load(std::memory_order_relaxed);
            if (bgCurrentIndex > known) {
                m_knownTotalFrames.store(bgCurrentIndex, std::memory_order_relaxed);
            }
        }
    }

    wuffs_base__status DecodeConfigWithShortRead(bool isGif,
                                                 wuffs_gif__decoder& gifDec,
                                                 wuffs_png__decoder& pngDec,
                                                 wuffs_base__image_config& ic,
                                                 wuffs_base__io_buffer& src) const {
        const size_t total = m_mappedFile ? m_mappedFile->size() : 0;
        wuffs_base__status status = {};
        while (true) {
            status = isGif
                ? wuffs_gif__decoder__decode_image_config(&gifDec, &ic, &src)
                : wuffs_png__decoder__decode_image_config(&pngDec, &ic, &src);
            if (wuffs_base__status__is_ok(&status)) return status;
            if (status.repr == wuffs_base__suspension__short_read && !src.meta.closed) {
                const size_t next = std::min(total, src.meta.wi + 1048576);
                src.meta.wi = next;
                src.meta.closed = (next == total);
                continue;
            }
            return status;
        }
    }

    std::shared_ptr<QuickView::MappedFile> m_mappedFile;
    
    wuffs_gif__decoder m_gifDec = {};
    wuffs_png__decoder m_pngDec = {};
    bool m_isGif = false;
    bool m_isPng = false;
    
    wuffs_base__io_buffer m_src = {};
    wuffs_base__image_config m_ic = {};
    wuffs_base__pixel_buffer m_pb = {};
    
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    size_t m_pixelBytes = 0;
    
    std::vector<uint8_t> m_workbuf;
    std::vector<uint8_t> m_canvas;
    
    uint32_t m_currentIndex = 0;
    std::atomic<uint32_t> m_knownTotalFrames{0};
    bool m_isAnimated = false;
    
    // Disposal State
    FrameDisposalMode m_lastDisposal = FrameDisposalMode::Unspecified;
    Rect m_lastRect;
    std::vector<uint8_t> m_restoreBuffer;

    std::vector<Snapshot> m_snapshots;
    std::shared_mutex m_snapshotMutex;
    std::once_flag m_indexerOnce;
    std::jthread m_indexerThread;
};

std::unique_ptr<IAnimationDecoder> CreateWuffsAnimator() {
    return std::make_unique<WuffsAnimator>();
}

} // namespace QuickView
