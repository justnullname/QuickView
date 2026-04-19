#include "pch.h"
#include "DisplayColorInfo.h"
#include <icm.h>
#include <cstdlib>

#include <vector>
#include <windows.graphics.display.interop.h>
#include <windows.graphics.display.h>
#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>

#pragma comment(lib, "runtimeobject.lib")

namespace QuickView {

namespace {

bool IsHdrColorSpace(DXGI_COLOR_SPACE_TYPE colorSpace) {
    return colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
           colorSpace == DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020 ||
           colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
}

HMONITOR GetWindowCenterMonitor(HWND hwnd) {
    if (!hwnd) {
        return nullptr;
    }

    RECT windowRect = {};
    if (!GetWindowRect(hwnd, &windowRect)) {
        return MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    }

    const POINT center = {
        windowRect.left + (windowRect.right - windowRect.left) / 2,
        windowRect.top + (windowRect.bottom - windowRect.top) / 2
    };
    return MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST);
}

// Interop helper to fetch true OS-calibrated HDR characteristics via classic COM/WRL (avoids C++/WinRT overhead).
bool QueryAdvancedColorInfoWinRT(HMONITOR hMon, float& outMaxLuminance, float& outSdrWhiteLevel) {
    using namespace ABI::Windows::Graphics::Display;
    using namespace Microsoft::WRL;
    using namespace Microsoft::WRL::Wrappers;

    // Defensively ensure WinRT/COM is initialized for this thread.
    // If it's already initialized (e.g. Main UI thread STA), RoInitialize returns S_FALSE or RPC_E_CHANGED_MODE.
    // We only uninitialize if we were the ones to successfully initialize it (S_OK/S_FALSE).
    struct RoInitWrapper {
        HRESULT hrInit;
        RoInitWrapper() { hrInit = RoInitialize(RO_INIT_MULTITHREADED); }
        ~RoInitWrapper() { if (SUCCEEDED(hrInit)) RoUninitialize(); }
    } roInit;

    ComPtr<IDisplayInformationStaticsInterop> interop;
    HRESULT hr = RoGetActivationFactory(
        HStringReference(RuntimeClass_Windows_Graphics_Display_DisplayInformation).Get(),
        IID_PPV_ARGS(&interop)
    );
    if (FAILED(hr) || !interop) return false;

    ComPtr<IDisplayInformation> displayInfo;
    hr = interop->GetForMonitor(hMon, IID_PPV_ARGS(&displayInfo));
    if (FAILED(hr) || !displayInfo) return false;

    ComPtr<IDisplayInformation5> displayInfo5;
    if (SUCCEEDED(displayInfo.As(&displayInfo5))) {
        ComPtr<IAdvancedColorInfo> advancedColor;
        if (SUCCEEDED(displayInfo5->GetAdvancedColorInfo(&advancedColor)) && advancedColor) {
            float maxLuminance = 0.0f;
            float sdrWhite = 0.0f;
            
            // Windows typically scales raw EDID based on user calibration / slider 
            if (SUCCEEDED(advancedColor->get_MaxLuminanceInNits(&maxLuminance)) && maxLuminance > 0.0f) {
                outMaxLuminance = maxLuminance;
                if (SUCCEEDED(advancedColor->get_SdrWhiteLevelInNits(&sdrWhite)) && sdrWhite > 0.0f) {
                    outSdrWhiteLevel = sdrWhite;
                }
                return true; 
            }
        }
    }

    return false;
}

float QueryIccPeakLuminance(const std::wstring& gdiDeviceName) {
    if (gdiDeviceName.empty()) return 0.0f;

    HDC hdcMon = CreateDCW(L"DISPLAY", gdiDeviceName.c_str(), NULL, NULL);
    if (!hdcMon) return 0.0f;

    DWORD dwLen = 0;
    GetICMProfileW(hdcMon, &dwLen, NULL);
    if (dwLen == 0) {
        DeleteDC(hdcMon);
        return 0.0f;
    }

    std::wstring profilePath(dwLen, L'\0');
    if (!GetICMProfileW(hdcMon, &dwLen, profilePath.data())) {
        DeleteDC(hdcMon);
        return 0.0f;
    }
    DeleteDC(hdcMon);

    profilePath.resize(wcsnlen(profilePath.c_str(), dwLen));

    PROFILE profile = {};
    profile.dwType = PROFILE_FILENAME;
    profile.pProfileData = const_cast<void*>(static_cast<const void*>(profilePath.c_str()));
    profile.cbDataSize = static_cast<DWORD>(profilePath.size() * sizeof(wchar_t));

    HPROFILE hProfile = OpenColorProfileW(&profile, PROFILE_READ, FILE_SHARE_READ, OPEN_EXISTING);
    if (!hProfile) return 0.0f;

    DWORD tagSize = 0;
    BOOL bHasTag = GetColorProfileElementSize(hProfile, 'lumi', &tagSize);
    float peakNits = 0.0f;
    if (bHasTag && tagSize >= 20) {
        std::vector<BYTE> buffer(tagSize);
        if (GetColorProfileElement(hProfile, 'lumi', 0, &tagSize, buffer.data(), nullptr)) {
            int32_t yFixed = *reinterpret_cast<int32_t*>(buffer.data() + 12);
            yFixed = _byteswap_ulong(yFixed);
            peakNits = static_cast<float>(yFixed) / 65536.0f;
        }
    }
    CloseColorProfile(hProfile);
    return peakNits;
}

} // namespace


bool DisplayColorInfo::Refresh(HWND hwnd, bool forceHdrSimulation) {
    HMONITOR monitor = GetWindowCenterMonitor(hwnd);
    DisplayColorState nextState = {};
    if (!QueryMonitorState(monitor, &nextState)) {
        nextState.monitor = monitor;
        nextState.sdrWhiteLevelNits = 80.0f;
    }

    if (forceHdrSimulation && !nextState.advancedColorActive) {
        nextState.advancedColorActive = true;
        nextState.advancedColorSupported = true;
        nextState.colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;

        // Provide enough headroom for testing without totally crushing the image on a real SDR display.
        // A max luminance of 2x the SDR white level gives exactly 1.0 stop of HDR headroom.
        if (nextState.maxLuminanceNits <= nextState.sdrWhiteLevelNits) {
            nextState.maxLuminanceNits = nextState.sdrWhiteLevelNits * 2.0f;
        }
        if (nextState.maxFullFrameLuminanceNits <= nextState.sdrWhiteLevelNits) {
            nextState.maxFullFrameLuminanceNits = nextState.sdrWhiteLevelNits * 2.0f;
        }
    }

    const bool changed =
        m_state.monitor != nextState.monitor ||
        m_state.advancedColorActive != nextState.advancedColorActive ||
        m_state.advancedColorSupported != nextState.advancedColorSupported ||
        m_state.colorSpace != nextState.colorSpace ||
        std::abs(m_state.maxLuminanceNits - nextState.maxLuminanceNits) > 0.01f ||
        std::abs(m_state.maxFullFrameLuminanceNits - nextState.maxFullFrameLuminanceNits) > 0.01f ||
        std::abs(m_state.sdrWhiteLevelNits - nextState.sdrWhiteLevelNits) > 0.01f ||
        m_state.gdiDeviceName != nextState.gdiDeviceName;

    m_state = std::move(nextState);
    return changed;
}

bool DisplayColorInfo::QueryMonitorState(HMONITOR monitor, DisplayColorState* stateOut) {
    DisplayColorInfo helper;
    return helper.QueryForMonitor(monitor, stateOut);
}

bool DisplayColorInfo::QueryForMonitor(HMONITOR monitor, DisplayColorState* stateOut) {
    if (!stateOut) return false;

    *stateOut = {};
    stateOut->monitor = monitor;
    stateOut->sdrWhiteLevelNits = 80.0f;

    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return false;
    }

    for (UINT adapterIndex = 0;; ++adapterIndex) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(adapterIndex, &adapter) == DXGI_ERROR_NOT_FOUND) {
            break;
        }

        for (UINT outputIndex = 0;; ++outputIndex) {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(outputIndex, &output) == DXGI_ERROR_NOT_FOUND) {
                break;
            }

            DXGI_OUTPUT_DESC outputDesc = {};
            if (FAILED(output->GetDesc(&outputDesc)) || outputDesc.Monitor != monitor) {
                continue;
            }

            stateOut->isValid = true;
            stateOut->gdiDeviceName = outputDesc.DeviceName;

            ComPtr<IDXGIOutput6> output6;
            if (SUCCEEDED(output.As(&output6))) {
                DXGI_OUTPUT_DESC1 desc1 = {};
                if (SUCCEEDED(output6->GetDesc1(&desc1))) {
                    stateOut->colorSpace = desc1.ColorSpace;
                    stateOut->minLuminanceNits = desc1.MinLuminance;
                    stateOut->maxLuminanceNits = desc1.MaxLuminance;
                    stateOut->maxFullFrameLuminanceNits = desc1.MaxFullFrameLuminance;
                    stateOut->advancedColorActive = IsHdrColorSpace(desc1.ColorSpace);
                    stateOut->advancedColorSupported =
                        stateOut->advancedColorActive || desc1.MaxLuminance > 0.0f;
                }
            }

            // Get accurate Peak Luminance and SDR white via a multi-tier fallback pipeline.
            float winrtMaxNits = 0.0f;
            float winrtSdrWhite = 0.0f;
            const bool hasWinRT = QueryAdvancedColorInfoWinRT(monitor, winrtMaxNits, winrtSdrWhite);

            // Tier 1: ICC Profile (Highest precision, user-calibrated via Windows HDR Calibration app)
            float iccPeakNits = QueryIccPeakLuminance(stateOut->gdiDeviceName);
            if (iccPeakNits > 0.0f) {
                stateOut->maxLuminanceNits = iccPeakNits;
            }
            // Tier 2: OS Advanced Color Info (Respects system scale but may occasionally falter)
            else if (hasWinRT && winrtMaxNits > 0.0f) {
                stateOut->maxLuminanceNits = winrtMaxNits;
            }
            // Tier 3: DXGI desc1.MaxLuminance (Raw EDID, already populated above).

            if (hasWinRT && winrtSdrWhite > 0.0f) {
                stateOut->sdrWhiteLevelNits = winrtSdrWhite;
            } else {
                stateOut->sdrWhiteLevelNits = QuerySdrWhiteLevelNits(stateOut->gdiDeviceName);
            }

            return true;
        }
    }

    return false;
}

float DisplayColorInfo::QuerySdrWhiteLevelNits(const std::wstring& gdiDeviceName) const {
    if (gdiDeviceName.empty()) {
        return 80.0f;
    }

    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) {
        return 80.0f;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS) {
        return 80.0f;
    }

    for (UINT32 i = 0; i < pathCount; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName = {};
        sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sourceName.header.size = sizeof(sourceName);
        sourceName.header.adapterId = paths[i].sourceInfo.adapterId;
        sourceName.header.id = paths[i].sourceInfo.id;

        if (DisplayConfigGetDeviceInfo(&sourceName.header) != ERROR_SUCCESS) {
            continue;
        }

        if (gdiDeviceName != sourceName.viewGdiDeviceName) {
            continue;
        }

        DISPLAYCONFIG_SDR_WHITE_LEVEL whiteLevel = {};
        whiteLevel.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        whiteLevel.header.size = sizeof(whiteLevel);
        whiteLevel.header.adapterId = paths[i].targetInfo.adapterId;
        whiteLevel.header.id = paths[i].targetInfo.id;

        if (DisplayConfigGetDeviceInfo(&whiteLevel.header) == ERROR_SUCCESS) {
            return 80.0f * static_cast<float>(whiteLevel.SDRWhiteLevel) / 1000.0f;
        }

        break;
    }

    return 80.0f;
}

const wchar_t* ToString(TransferFunction value) {
    switch (value) {
        case TransferFunction::SRGB: return L"sRGB";
        case TransferFunction::Linear: return L"Linear";
        case TransferFunction::PQ: return L"PQ";
        case TransferFunction::HLG: return L"HLG";
        case TransferFunction::Gamma22: return L"Gamma 2.2";
        case TransferFunction::Gamma28: return L"Gamma 2.8";
        case TransferFunction::Rec709: return L"Rec.709";
        default: return L"Unknown";
    }
}

const wchar_t* ToString(ColorPrimaries value) {
    switch (value) {
        case ColorPrimaries::SRGB: return L"sRGB";
        case ColorPrimaries::DisplayP3: return L"Display P3";
        case ColorPrimaries::Rec2020: return L"Rec.2020";
        case ColorPrimaries::AdobeRGB: return L"Adobe RGB";
        case ColorPrimaries::ProPhotoRGB: return L"ProPhoto RGB";
        case ColorPrimaries::ACES: return L"ACES";
        default: return L"Unknown";
    }
}

} // namespace QuickView
