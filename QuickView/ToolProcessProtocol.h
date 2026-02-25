#pragma once

#include <cstdint>

namespace QuickView::ToolProcess {

// --- JXL LOD (Legacy) ---
constexpr uint32_t kJxlLodDecodeMagic = 0x514A4C44; // "QJLD"
constexpr uint32_t kJxlLodDecodeVersion = 1;

struct JxlLodDecodeResultHeader {
    uint32_t magic = kJxlLodDecodeMagic;
    uint32_t version = kJxlLodDecodeVersion;
    int32_t hr = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    uint32_t reserved = 0;
    uint64_t payloadBytes = 0;
};

// --- Generic Decode Worker ---
constexpr uint32_t kDecodeWorkerMagic   = 0x51564457; // "QVDW"
constexpr uint32_t kDecodeWorkerVersion = 1;

struct DecodeResultHeader {
    uint32_t magic   = kDecodeWorkerMagic;
    uint32_t version = kDecodeWorkerVersion;
    int32_t  hr      = 0;               // HRESULT from child
    uint32_t width   = 0;
    uint32_t height  = 0;
    uint32_t stride  = 0;
    uint32_t exifOrientation = 0;        // EXIF orientation tag (1-8)
    uint64_t payloadBytes    = 0;        // pixel data size in bytes
    // Pixel data (BGRA8888) follows immediately after this header
};

} // namespace QuickView::ToolProcess

