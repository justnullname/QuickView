#pragma once
// ============================================================================
// ThumbWorker.h - Pooled --thumb-worker Daemon & In-Process Hot Thumbnail Cache
// ============================================================================

#include "ThumbWorkerProtocol.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace QuickView {

struct ThumbDecodeResult {
    std::vector<uint8_t> pixels;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    bool isPremultiplied = true;
};

struct ThumbWorkerDecodeOps {
    void* (*createLoader)() = nullptr;
    void  (*destroyLoader)(void* loader) = nullptr;
    HRESULT (*decodeFile)(void* loader, LPCWSTR filePath, int targetSize, ThumbDecodeResult& out, const std::atomic<bool>* cancelFlag) = nullptr;
    HRESULT (*decodeMemory)(void* loader, const uint8_t* data, size_t size, LPCWSTR extHint, int targetSize, ThumbDecodeResult& out, const std::atomic<bool>* cancelFlag) = nullptr;
};

// Returns true for heavy formats (RAW, PSD, PSB, EXR, HDR, TIFF, PDF, Archives)
// that should be throttled by a concurrency semaphore in gallery & worker pools.
[[nodiscard]] bool IsHeavyThumbnailExtension(std::wstring_view pathOrExt) noexcept;

// Canonicalizes a Win32 path to lower-case UTF-8 and reads (fileSize, mtime100ns).
// Returns false for cloud placeholders, missing files, virtual paths, or directories.
[[nodiscard]] bool CanonicalizeThumbCacheKey(
    std::wstring_view filePath,
    std::string& outCanonicalPathUtf8,
    uint64_t& outFileSize,
    uint64_t& outFileMtime100ns);

// Bounded 64-slot / 24 MiB flat DOD LRU ring for zero-redecode in-process Shell warm-up.
class ThumbHotCache {
public:
    static constexpr size_t kMaxEntries = 64;
    static constexpr size_t kMaxBytes = 24u * 1024u * 1024u;

    void Insert(
        std::string canonicalPathUtf8,
        uint64_t fileSize,
        uint64_t fileMtime100ns,
        uint32_t width,
        uint32_t height,
        uint32_t stride,
        bool isPremultiplied,
        std::vector<uint8_t> pixels,
        uint32_t origMaxEdge = 0);

    [[nodiscard]] bool Lookup(
        const char* canonicalPathUtf8,
        uint64_t fileSize,
        uint64_t fileMtime100ns,
        uint32_t requestedCx,
        uint8_t* outBgraBuf,
        uint32_t maxOutBytes,
        uint32_t* outWidth,
        uint32_t* outHeight,
        uint32_t* outStride,
        uint8_t* outIsPremultiplied);

    void Clear();

private:
    struct Slot {
        bool occupied = false;
        uint64_t pathHash = 0;
        uint64_t lruSeq = 0;
        std::string canonicalPathUtf8;
        uint64_t fileSize = 0;
        uint64_t fileMtime100ns = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t stride = 0;
        uint32_t origMaxEdge = 0;
        bool isPremultiplied = true;
        std::vector<uint8_t> pixels;
    };

    void EvictIfNeededLocked();

    std::mutex m_mutex;
    std::array<Slot, kMaxEntries> m_slots{};
    uint64_t m_lruCounter = 0;
    size_t m_totalBytes = 0;
};

// Pooled overlapped Named Pipe server entry point for `QuickView.exe --thumb-worker`.
int RunThumbWorker(int argc, LPWSTR* argv, const ThumbWorkerDecodeOps& ops);

} // namespace QuickView
