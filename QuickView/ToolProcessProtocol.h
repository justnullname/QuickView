#pragma once

#include <cstdint>
#include <limits>
#include <type_traits>

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
constexpr uint32_t kDecodeWorkerVersion = 2;
constexpr uint64_t kDecodeMapPageSize   = 4096ull;
constexpr uint32_t kDecodeWorkerMaxIccBytes = 8u * 1024u * 1024u;

constexpr uint8_t kHdrFlagValid        = 1u << 0;
constexpr uint8_t kHdrFlagIsHdr        = 1u << 1;
constexpr uint8_t kHdrFlagSceneLinear  = 1u << 2;
constexpr uint8_t kHdrFlagHasNits      = 1u << 3;
constexpr uint8_t kHdrFlagHasGainMap   = 1u << 4;
constexpr uint8_t kHdrFlagGainApplied  = 1u << 5;

struct DecodeResultHeader {
    uint32_t magic   = kDecodeWorkerMagic;
    uint32_t version = kDecodeWorkerVersion;
    int32_t  hr      = 0;               // HRESULT from child
    uint32_t width   = 0;
    uint32_t height  = 0;
    uint32_t stride  = 0;
    uint32_t originalWidth   = 0;       // Source image width (before scaling)
    uint32_t originalHeight  = 0;       // Source image height (before scaling)
    uint32_t exifOrientation = 1;       // EXIF orientation tag (1-8)

    // POD color metadata
    uint8_t pixelFormat     = 0;
    uint8_t colorDataSpace  = 0;
    uint8_t colorTransfer   = 0;
    uint8_t colorPrimaries  = 0;
    uint8_t nominalBitDepth = 0;
    uint8_t hasEmbeddedIcc  = 0;
    uint8_t hdrFlags        = 0;
    uint8_t hdrTransfer     = 0;
    uint8_t hdrPrimaries    = 0;
    uint8_t reservedPad[3]  = {};

    float maxCLLNits               = 0.0f;
    float maxFALLNits              = 0.0f;
    float masteringMinNits         = 0.0f;
    float masteringMaxNits         = 0.0f;
    float gainMapBaseHeadroom      = 0.0f;
    float gainMapAlternateHeadroom = 0.0f;
    float gainMapAppliedHeadroom   = 0.0f;

    // Virtual section layout (SEC_RESERVE + demand commit)
    uint64_t pixelOffset   = 0;
    uint64_t pixelCapacity = 0;
    uint64_t payloadBytes  = 0;         // Actual pixel payload size in bytes
    uint64_t iccOffset     = 0;
    uint32_t iccCapacity   = 0;
    uint32_t iccBytes      = 0;         // Actual ICC profile size in bytes
};

static_assert(std::is_trivially_copyable_v<DecodeResultHeader>,
              "DecodeResultHeader must remain trivially copyable across IPC");
static_assert(sizeof(DecodeResultHeader) <= kDecodeMapPageSize,
              "DecodeResultHeader must fit within the committed header page");

struct DecodeMapLayout {
    uint64_t headerBytes   = 0;
    uint64_t pixelOffset   = 0;
    uint64_t pixelCapacity = 0;
    uint64_t iccOffset     = 0;
    uint32_t iccCapacity   = 0;
    uint64_t totalMapBytes = 0;
};

[[nodiscard]] constexpr uint64_t AlignUpPage(uint64_t value) noexcept {
    constexpr uint64_t kMask = kDecodeMapPageSize - 1ull;
    if (value > (std::numeric_limits<uint64_t>::max)() - kMask) {
        return 0;
    }
    return (value + kMask) & ~kMask;
}

[[nodiscard]] constexpr DecodeMapLayout ComputeDecodeMapLayout(
    uint64_t pixelCapacityBytes,
    uint32_t iccCapacityBytes = kDecodeWorkerMaxIccBytes) noexcept {
    if (pixelCapacityBytes == 0 || iccCapacityBytes > kDecodeWorkerMaxIccBytes) {
        return {};
    }
    const uint64_t headerBytes = AlignUpPage(sizeof(DecodeResultHeader));
    if (headerBytes == 0) {
        return {};
    }
    const uint64_t pixelOffset = headerBytes;
    if (pixelOffset > (std::numeric_limits<uint64_t>::max)() - pixelCapacityBytes) {
        return {};
    }
    const uint64_t iccOffset = AlignUpPage(pixelOffset + pixelCapacityBytes);
    if (iccOffset == 0) {
        return {};
    }
    if (iccOffset > (std::numeric_limits<uint64_t>::max)() - static_cast<uint64_t>(iccCapacityBytes)) {
        return {};
    }
    DecodeMapLayout layout;
    layout.headerBytes   = headerBytes;
    layout.pixelOffset   = pixelOffset;
    layout.pixelCapacity = pixelCapacityBytes;
    layout.iccOffset     = iccOffset;
    layout.iccCapacity   = iccCapacityBytes;
    layout.totalMapBytes = iccOffset + static_cast<uint64_t>(iccCapacityBytes);
    return layout;
}

[[nodiscard]] constexpr uint8_t PackHdrFlags(
    bool isValid, bool isHdr, bool isSceneLinear,
    bool hasNitsMetadata, bool hasGainMap, bool gainMapApplied) noexcept {
    uint8_t flags = 0;
    if (isValid)         flags |= kHdrFlagValid;
    if (isHdr)           flags |= kHdrFlagIsHdr;
    if (isSceneLinear)   flags |= kHdrFlagSceneLinear;
    if (hasNitsMetadata) flags |= kHdrFlagHasNits;
    if (hasGainMap)      flags |= kHdrFlagHasGainMap;
    if (gainMapApplied)  flags |= kHdrFlagGainApplied;
    return flags;
}

constexpr void UnpackHdrFlags(
    uint8_t flags, bool& isValid, bool& isHdr, bool& isSceneLinear,
    bool& hasNitsMetadata, bool& hasGainMap, bool& gainMapApplied) noexcept {
    isValid         = (flags & kHdrFlagValid) != 0;
    isHdr           = (flags & kHdrFlagIsHdr) != 0;
    isSceneLinear   = (flags & kHdrFlagSceneLinear) != 0;
    hasNitsMetadata = (flags & kHdrFlagHasNits) != 0;
    hasGainMap      = (flags & kHdrFlagHasGainMap) != 0;
    gainMapApplied  = (flags & kHdrFlagGainApplied) != 0;
}

[[nodiscard]] inline bool ValidateDecodeResultHeader(
    const DecodeResultHeader& header, const DecodeMapLayout& layout) noexcept {
    if (header.magic != kDecodeWorkerMagic ||
        header.version != kDecodeWorkerVersion) {
        return false;
    }
    if (header.pixelOffset != layout.pixelOffset ||
        header.pixelCapacity != layout.pixelCapacity ||
        header.iccOffset != layout.iccOffset ||
        header.iccCapacity != layout.iccCapacity) {
        return false;
    }
    if (header.width == 0 || header.height == 0 || header.stride == 0) {
        return false;
    }
    const uint64_t minPayload =
        static_cast<uint64_t>(header.stride) * static_cast<uint64_t>(header.height);
    if (minPayload == 0 ||
        header.payloadBytes < minPayload ||
        header.payloadBytes > layout.pixelCapacity) {
        return false;
    }
    if (header.iccBytes > layout.iccCapacity) {
        return false;
    }
    return true;
}

} // namespace QuickView::ToolProcess

