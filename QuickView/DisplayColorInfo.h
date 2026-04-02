#pragma once

#include "pch.h"

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

    DXGI_COLOR_SPACE_TYPE colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    float minLuminanceNits = 0.0f;
    float maxLuminanceNits = 0.0f;
    float maxFullFrameLuminanceNits = 0.0f;
    float sdrWhiteLevelNits = 80.0f;

    float GetSdrWhiteScale() const {
        const float baseWhite = 80.0f;
        if (sdrWhiteLevelNits <= 0.0f) return 1.0f;
        return sdrWhiteLevelNits / baseWhite;
    }

    float GetHdrHeadroomStops() const {
        if (!advancedColorActive || maxLuminanceNits <= 0.0f || sdrWhiteLevelNits <= 0.0f) {
            return 0.0f;
        }

        const float ratio = maxLuminanceNits / sdrWhiteLevelNits;
        if (!(ratio > 1.0f)) {
            return 0.0f;
        }
        return log2f(ratio);
    }

    bool ShouldUseScRgbPipeline() const {
        return advancedColorActive;
    }
};

class DisplayColorInfo {
public:
    bool Refresh(HWND hwnd, bool forceHdrSimulation = false);
    static bool QueryMonitorState(HMONITOR monitor, DisplayColorState* stateOut);
    const DisplayColorState& GetState() const { return m_state; }

private:
    bool QueryForMonitor(HMONITOR monitor, DisplayColorState* stateOut);
    float QuerySdrWhiteLevelNits(const std::wstring& gdiDeviceName) const;

    DisplayColorState m_state;
};

const wchar_t* ToString(TransferFunction value);
const wchar_t* ToString(ColorPrimaries value);

} // namespace QuickView
