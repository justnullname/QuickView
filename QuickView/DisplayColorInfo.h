#pragma once

#include "pch.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace QuickView {

enum class TransferFunction : uint8_t {
    Unknown = 0,
    SRGB,
    Linear,
    PQ,
    HLG,
    Gamma22,
    Gamma28,
    Rec709
};

enum class ColorPrimaries : uint8_t {
    Unknown = 0,
    SRGB,
    DisplayP3,
    Rec2020,
    AdobeRGB,
    ProPhotoRGB,
    ACES
};

enum class PixelDataSpace : uint8_t {
    Unknown = 0,
    EncodedSdr,
    EncodedHdr,
    SceneLinear,
    SceneLinearScRgb
};

// Absolute vs relative luminance routing for Advanced Color / scRGB output.
enum class HdrContentKind : uint8_t {
    AbsolutePhotometric,  // PQ/HLG/scRGB/nits metadata or composed gain map
    RelativeSdr,          // Encoded SDR (sRGB, 8-bit)
    RelativeSceneLinear,  // Scene-linear float without absolute calibration
    UltraHdrPending,      // ISO 21496-1 base layer before GPU gain-map compose
};

struct PixelColorInfo {
    PixelDataSpace dataSpace = PixelDataSpace::Unknown;
    TransferFunction transfer = TransferFunction::Unknown;
    ColorPrimaries primaries = ColorPrimaries::Unknown;
    uint8_t nominalBitDepth = 0;
    bool hasEmbeddedIcc = false;

    bool IsSdrEncoded() const {
        return dataSpace == PixelDataSpace::EncodedSdr;
    }

    bool IsSceneLinear() const {
        return dataSpace == PixelDataSpace::SceneLinear ||
               dataSpace == PixelDataSpace::SceneLinearScRgb;
    }

    bool IsScRgb() const {
        return dataSpace == PixelDataSpace::SceneLinearScRgb;
    }

    bool IsSrgb() const {
        return primaries == ColorPrimaries::SRGB &&
               (transfer == TransferFunction::SRGB || transfer == TransferFunction::Rec709 ||
                dataSpace == PixelDataSpace::EncodedSdr);
    }
};

struct HdrStaticMetadata {
    bool isValid = false;
    bool isHdr = false;
    bool isSceneLinear = false;
    bool hasNitsMetadata = false;
    bool hasGainMap = false;
    bool gainMapApplied = false;

    TransferFunction transfer = TransferFunction::Unknown;
    ColorPrimaries primaries = ColorPrimaries::Unknown;

    float maxCLLNits = 0.0f;
    float maxFALLNits = 0.0f;
    float masteringMinNits = 0.0f;
    float masteringMaxNits = 0.0f;
    float gainMapBaseHeadroom = 0.0f;
    float gainMapAlternateHeadroom = 0.0f;
    float gainMapAppliedHeadroom = 0.0f;
};

struct DisplayColorState {
    HMONITOR monitor = nullptr;
    std::wstring gdiDeviceName;

    bool isValid = false;
    bool advancedColorSupported = false;
    bool advancedColorActive = false;
    bool wideColorActive = false;
    bool highDynamicRangeUserEnabled = false;
    bool wideColorUserEnabled = false;

    DXGI_COLOR_SPACE_TYPE colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    float redPrimary[2] = {0.0f, 0.0f};
    float greenPrimary[2] = {0.0f, 0.0f};
    float bluePrimary[2] = {0.0f, 0.0f};
    float whitePoint[2] = {0.0f, 0.0f};
    float minLuminanceNits = 0.0f;
    float maxLuminanceNits = 0.0f;
    float maxFullFrameLuminanceNits = 0.0f;
    float sdrWhiteLevelNits = 80.0f;

    float GetSdrWhiteScale() const {
        const float baseWhite = 80.0f;
        if (sdrWhiteLevelNits <= 0.0f) return 1.0f;
        return sdrWhiteLevelNits / baseWhite;
    }

    float GetEffectivePeakNits(float peakNitsOverride = 0.0f) const;
    float GetHdrHeadroomStops(float peakNitsOverride = 0.0f) const;

    bool ShouldUseScRgbPipeline() const {
        return advancedColorActive;
    }

    bool ShouldBypassMonitorProfileForSdr() const {
        return wideColorActive && !highDynamicRangeUserEnabled;
    }

    bool HasChromaticities() const {
        return redPrimary[0] > 0.0f && redPrimary[1] > 0.0f &&
               greenPrimary[0] > 0.0f && greenPrimary[1] > 0.0f &&
               bluePrimary[0] > 0.0f && bluePrimary[1] > 0.0f &&
               whitePoint[0] > 0.0f && whitePoint[1] > 0.0f;
    }
};

class DisplayColorInfo {
public:
    bool Refresh(HWND hwnd, bool forceHdrSimulation = false);
    static bool QueryMonitorState(HMONITOR monitor, DisplayColorState* stateOut);
    static void InvalidateHardwareCache();
    const DisplayColorState& GetState() const { return m_state; }

private:
    bool QueryForMonitor(HMONITOR monitor, DisplayColorState* stateOut);
    float QuerySdrWhiteLevelNits(const std::wstring& gdiDeviceName) const;

    DisplayColorState m_state;
};

const wchar_t* ToString(TransferFunction value);
const wchar_t* ToString(ColorPrimaries value);

inline void MapCicpCodes(uint8_t primariesCode, uint8_t transferCode,
                         ColorPrimaries* outPrimaries,
                         TransferFunction* outTransfer) {
    ColorPrimaries primaries = ColorPrimaries::Unknown;
    switch (primariesCode) {
        case 1:  primaries = ColorPrimaries::SRGB; break;
        case 9:  primaries = ColorPrimaries::Rec2020; break;
        case 11:
        case 12: primaries = ColorPrimaries::DisplayP3; break;
        default: break;
    }

    TransferFunction transfer = TransferFunction::Unknown;
    switch (transferCode) {
        case 1:
        case 6:
        case 14:
        case 15: transfer = TransferFunction::Rec709; break;
        case 8:  transfer = TransferFunction::Linear; break;
        case 13: transfer = TransferFunction::SRGB; break;
        case 16: transfer = TransferFunction::PQ; break;
        case 18: transfer = TransferFunction::HLG; break;
        default: break;
    }

    if (outPrimaries && primaries != ColorPrimaries::Unknown) {
        *outPrimaries = primaries;
    }
    if (outTransfer && transfer != TransferFunction::Unknown) {
        *outTransfer = transfer;
    }
}

inline bool TryParseIccCicp(const uint8_t* data, size_t size,
                            ColorPrimaries* outPrimaries,
                            TransferFunction* outTransfer) {
    if (!data || size < 132) {
        return false;
    }

    auto readU32BE = [](const uint8_t* p) -> uint32_t {
        return (static_cast<uint32_t>(p[0]) << 24) |
               (static_cast<uint32_t>(p[1]) << 16) |
               (static_cast<uint32_t>(p[2]) << 8) |
               static_cast<uint32_t>(p[3]);
    };

    const uint32_t declaredSize = readU32BE(data);
    if (declaredSize < 132 || static_cast<size_t>(declaredSize) > size) {
        return false;
    }
    const size_t effectiveSize = static_cast<size_t>(declaredSize);

    // ICC header signature "acsp" at offset 36
    if (std::memcmp(data + 36, "acsp", 4) != 0) {
        return false;
    }

    const uint32_t tagCount = readU32BE(data + 128);
    if (static_cast<uint64_t>(tagCount) > (effectiveSize - 132) / 12) {
        return false;
    }

    for (uint32_t i = 0; i < tagCount; ++i) {
        const uint8_t* entry = data + 132 + static_cast<size_t>(i) * 12;
        if (std::memcmp(entry, "cicp", 4) != 0) {
            continue;
        }

        const uint32_t tagOffset = readU32BE(entry + 4);
        const uint32_t tagLength = readU32BE(entry + 8);
        if (tagLength < 12 ||
            static_cast<uint64_t>(tagOffset) + static_cast<uint64_t>(tagLength) > effectiveSize) {
            return false;
        }

        const uint8_t* tagData = data + tagOffset;
        if (std::memcmp(tagData, "cicp", 4) != 0) {
            return false;
        }

        const uint8_t primariesCode = tagData[8];
        const uint8_t transferCode = tagData[9];
        const uint8_t matrixCode = tagData[10];
        const uint8_t fullRangeCode = tagData[11];

        // Accept identity RGB matrix + full range only
        if (matrixCode != 0 || fullRangeCode != 1) {
            return false;
        }

        ColorPrimaries parsedPrimaries = ColorPrimaries::Unknown;
        TransferFunction parsedTransfer = TransferFunction::Unknown;
        MapCicpCodes(primariesCode, transferCode, &parsedPrimaries, &parsedTransfer);
        if (parsedPrimaries == ColorPrimaries::Unknown &&
            parsedTransfer == TransferFunction::Unknown) {
            return false;
        }

        if (outPrimaries && parsedPrimaries != ColorPrimaries::Unknown) {
            *outPrimaries = parsedPrimaries;
        }
        if (outTransfer && parsedTransfer != TransferFunction::Unknown) {
            *outTransfer = parsedTransfer;
        }
        return true;
    }

    return false;
}

inline bool HasJpegGainMapMarkers(const uint8_t* data, size_t size) {
    if (!data || size < 4 || data[0] != 0xFF || data[1] != 0xD8) {
        return false;
    }

    size_t offset = 2;
    while (offset + 1 < size) {
        if (data[offset] != 0xFF) {
            ++offset;
            continue;
        }

        while (offset + 1 < size && data[offset + 1] == 0xFF) {
            ++offset;
        }
        if (offset + 1 >= size) {
            return false;
        }

        const uint8_t marker = data[offset + 1];
        if (marker == 0x00 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8)) {
            offset += 2;
            continue;
        }
        if (marker == 0xDA || marker == 0xD9) { // SOS or EOI: header scan complete
            return false;
        }

        if (offset + 4 > size) {
            return false;
        }
        const uint16_t segLen = static_cast<uint16_t>(
            (static_cast<uint16_t>(data[offset + 2]) << 8) | data[offset + 3]);
        if (segLen < 2 || static_cast<size_t>(segLen) > size - offset - 2) {
            return false;
        }

        const uint8_t* payload = data + offset + 4;
        const size_t payloadLen = static_cast<size_t>(segLen) - 2;

        if (marker == 0xE1) { // APP1: XMP gain map namespace
            constexpr std::string_view kXmpHeader("http://ns.adobe.com/xap/1.0/", 28);
            if (payloadLen >= kXmpHeader.size() &&
                std::memcmp(payload, kXmpHeader.data(), kXmpHeader.size()) == 0) {
                const std::string_view xml(reinterpret_cast<const char*>(payload), payloadLen);
                if (xml.find("hdrgm:") != std::string_view::npos ||
                    xml.find("HDRGainMap") != std::string_view::npos ||
                    xml.find("hdrgainmap") != std::string_view::npos ||
                    xml.find("GContainer") != std::string_view::npos) {
                    return true;
                }
            }
        } else if (marker == 0xE2) { // APP2: ISO 21496-1 gain map metadata
            constexpr std::string_view kIsoGainMapHeader("urn:iso:std:iso:ts:21496:-1", 27);
            if (payloadLen >= kIsoGainMapHeader.size() &&
                std::memcmp(payload, kIsoGainMapHeader.data(), kIsoGainMapHeader.size()) == 0) {
                return true;
            }
        }

        offset += 2 + static_cast<size_t>(segLen);
    }

    return false;
}

} // namespace QuickView
