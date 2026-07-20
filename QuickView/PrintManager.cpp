#include "PrintManager.h"
#include "RenderEngine.h"
#include "ImageLoader.h"
#include <winspool.h>
#include <memory>
#include <thread>
#include <cmath>
#include <algorithm>
#include <wincodec.h>

#pragma comment(lib, "winspool.lib")

extern CRenderEngine* g_pRenderEngine;
extern CImageLoader* g_pImageLoader;

namespace QuickView {

std::vector<PrinterInfo> PrintManager::EnumLocalPrinters() const {
    std::vector<PrinterInfo> printers;
    
    DWORD flags = PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS;
    DWORD cbNeeded = 0;
    DWORD cReturned = 0;
    
    // First call to get required buffer size
    ::EnumPrintersW(flags, nullptr, 4, nullptr, 0, &cbNeeded, &cReturned);
    if (cbNeeded == 0) return printers;

    std::vector<uint8_t> buffer(cbNeeded);
    if (::EnumPrintersW(flags, nullptr, 4, buffer.data(), cbNeeded, &cbNeeded, &cReturned)) {
        std::wstring defPrinter;
        DWORD defaultCb = 0;
        ::GetDefaultPrinterW(nullptr, &defaultCb);
        if (defaultCb > 0) {
            defPrinter.resize(defaultCb);
            if (::GetDefaultPrinterW(&defPrinter[0], &defaultCb)) {
                defPrinter.resize(defaultCb > 0 ? defaultCb - 1 : 0);
            } else {
                defPrinter.clear();
            }
        }

        PRINTER_INFO_4W* pInfo = reinterpret_cast<PRINTER_INFO_4W*>(buffer.data());
        for (DWORD i = 0; i < cReturned; ++i) {
            PrinterInfo info;
            info.name = pInfo[i].pPrinterName;
            if (!defPrinter.empty() && info.name == defPrinter) {
                info.isDefault = true;
            }
            printers.push_back(info);
        }
    }
    return printers;
}

bool PrintManager::GetPrinterCapabilities([[maybe_unused]] const std::wstring& printerName) {
    // 预留接口：可使用 DeviceCapabilitiesW 获取支持的纸张等
    return true;
}

bool PrintManager::ShowPrinterProperties(HWND hwndOwner, const std::wstring& printerName, std::vector<uint8_t>& inOutDevModeData) {
    if (printerName.empty()) return false;
    
    bool changed = false;
    HANDLE hPrinter = nullptr;
    if (::OpenPrinterW(const_cast<LPWSTR>(printerName.c_str()), &hPrinter, nullptr)) {
        // Fetch current devmode size
        LONG size = ::DocumentPropertiesW(hwndOwner, hPrinter, const_cast<LPWSTR>(printerName.c_str()), nullptr, nullptr, 0);
        if (size > 0) {
            std::vector<uint8_t> newDevMode(size);
            DEVMODEW* pDevModeOut = reinterpret_cast<DEVMODEW*>(newDevMode.data());
            
            DEVMODEW* pDevModeIn = nullptr;
            if (!inOutDevModeData.empty()) {
                pDevModeIn = reinterpret_cast<DEVMODEW*>(inOutDevModeData.data());
            }

            // DocumentProperties requires us to populate the output buffer first if we are passing DM_OUT_BUFFER but no DM_IN_BUFFER, 
            // but actually we should just pass DM_OUT_BUFFER to get defaults if we have no input.
            // Wait, DM_IN_PROMPT | DM_IN_BUFFER | DM_OUT_BUFFER
            DWORD flags = DM_IN_PROMPT | DM_OUT_BUFFER;
            if (pDevModeIn) {
                flags |= DM_IN_BUFFER;
            }

            if (::DocumentPropertiesW(hwndOwner, hPrinter, const_cast<LPWSTR>(printerName.c_str()), pDevModeOut, pDevModeIn, flags) == IDOK) {
                inOutDevModeData = newDevMode;
                changed = true;
            }
        }
        ::ClosePrinter(hPrinter);
    }
    return changed;
}

bool PrintManager::BuildConfiguredDevMode(
    const PrintJobSettings& settings,
    std::vector<uint8_t>& outBuf,
    DEVMODEW** outDevMode
) {
    if (!outDevMode || settings.printerName.empty()) return false;
    *outDevMode = nullptr;

    HANDLE hPrinter = nullptr;
    if (!::OpenPrinterW(const_cast<LPWSTR>(settings.printerName.c_str()), &hPrinter, nullptr)) {
        return false;
    }

    if (!settings.devModeData.empty()) {
        outBuf = settings.devModeData;
    } else {
        LONG size = ::DocumentPropertiesW(nullptr, hPrinter, const_cast<LPWSTR>(settings.printerName.c_str()), nullptr, nullptr, 0);
        if (size <= 0) {
            ::ClosePrinter(hPrinter);
            return false;
        }

        outBuf.resize(static_cast<size_t>(size));
        DEVMODEW* pDevMode = reinterpret_cast<DEVMODEW*>(outBuf.data());
        if (::DocumentPropertiesW(nullptr, hPrinter, const_cast<LPWSTR>(settings.printerName.c_str()), pDevMode, nullptr, DM_OUT_BUFFER) != IDOK) {
            ::ClosePrinter(hPrinter);
            return false;
        }
    }

    DEVMODEW* pDevMode = reinterpret_cast<DEVMODEW*>(outBuf.data());

    // Keep preview and print on the same DEVMODE fields.
    pDevMode->dmFields |= DM_COPIES;
    pDevMode->dmCopies = settings.copies > 0 ? settings.copies : 1;

    pDevMode->dmFields |= DM_ORIENTATION;
    pDevMode->dmOrientation = settings.isLandscape ? DMORIENT_LANDSCAPE : DMORIENT_PORTRAIT;

    pDevMode->dmFields |= DM_COLOR;
    pDevMode->dmColor = settings.grayscale ? DMCOLOR_MONOCHROME : DMCOLOR_COLOR;

    if (settings.paperSize > 0) {
        pDevMode->dmFields |= DM_PAPERSIZE;
        pDevMode->dmPaperSize = settings.paperSize;
    }

    ::ClosePrinter(hPrinter);
    *outDevMode = pDevMode;
    return true;
}

PrintManager::PrintDeviceMetrics PrintManager::MetricsFromHdc(HDC hdc) {
    PrintDeviceMetrics metrics = {};
    if (!hdc) return metrics;

    metrics.dpiX = (float)::GetDeviceCaps(hdc, LOGPIXELSX);
    metrics.dpiY = (float)::GetDeviceCaps(hdc, LOGPIXELSY);
    
    if (metrics.dpiX <= 0.0f) metrics.dpiX = 600.0f;
    if (metrics.dpiY <= 0.0f) metrics.dpiY = 600.0f;
    
    // HORZSIZE is the printable width in mm, NOT the physical paper width!
    // To get the absolute physical paper size, we must use PHYSICALWIDTH in device pixels.
    metrics.physicalWidthMm = ((float)::GetDeviceCaps(hdc, PHYSICALWIDTH) / metrics.dpiX) * 25.4f;
    metrics.physicalHeightMm = ((float)::GetDeviceCaps(hdc, PHYSICALHEIGHT) / metrics.dpiY) * 25.4f;
    
    metrics.printableWidthPx = (float)::GetDeviceCaps(hdc, HORZRES);
    metrics.printableHeightPx = (float)::GetDeviceCaps(hdc, VERTRES);
    metrics.physicalOffsetX_Px = (float)::GetDeviceCaps(hdc, PHYSICALOFFSETX);
    metrics.physicalOffsetY_Px = (float)::GetDeviceCaps(hdc, PHYSICALOFFSETY);

    if (metrics.physicalWidthMm <= 0.0f) metrics.physicalWidthMm = 210.0f;
    if (metrics.physicalHeightMm <= 0.0f) metrics.physicalHeightMm = 297.0f;
    if (metrics.printableWidthPx <= 0.0f) {
        metrics.printableWidthPx = (metrics.physicalWidthMm / 25.4f) * metrics.dpiX;
    }
    if (metrics.printableHeightPx <= 0.0f) {
        metrics.printableHeightPx = (metrics.physicalHeightMm / 25.4f) * metrics.dpiY;
    }
    return metrics;
}

bool PrintManager::QueryPrintDeviceMetrics(
    const PrintJobSettings& settings,
    PrintDeviceMetrics& outMetrics,
    HDC* outDc
) {
    outMetrics = {};
    if (outDc) *outDc = nullptr;

    if (settings.printerName.empty()) {
        // Fallback A4 @ 600 DPI
        outMetrics.dpiX = 600.0f;
        outMetrics.dpiY = 600.0f;
        outMetrics.physicalWidthMm = settings.isLandscape ? 297.0f : 210.0f;
        outMetrics.physicalHeightMm = settings.isLandscape ? 210.0f : 297.0f;
        outMetrics.printableWidthPx = (outMetrics.physicalWidthMm / 25.4f) * outMetrics.dpiX;
        outMetrics.printableHeightPx = (outMetrics.physicalHeightMm / 25.4f) * outMetrics.dpiY;
        outMetrics.physicalOffsetX_Px = 0.0f;
        outMetrics.physicalOffsetY_Px = 0.0f;
        return false;
    }

    std::vector<uint8_t> devModeBuf;
    DEVMODEW* pDevMode = nullptr;
    const bool hasDevMode = BuildConfiguredDevMode(settings, devModeBuf, &pDevMode);

    HDC hdc = nullptr;
    if (outDc) {
        // Real printer DC for ExecutePrintJob
        if (hasDevMode) {
            hdc = ::CreateDCW(L"WINSPOOL", settings.printerName.c_str(), nullptr, pDevMode);
        }
        if (!hdc) {
            hdc = ::CreateDCW(L"WINSPOOL", settings.printerName.c_str(), nullptr, nullptr);
        }
        if (!hdc) {
            // Same fallback as preview so page counts still have a baseline
            outMetrics.dpiX = 600.0f;
            outMetrics.dpiY = 600.0f;
            outMetrics.physicalWidthMm = settings.isLandscape ? 297.0f : 210.0f;
            outMetrics.physicalHeightMm = settings.isLandscape ? 210.0f : 297.0f;
            outMetrics.printableWidthPx = (outMetrics.physicalWidthMm / 25.4f) * outMetrics.dpiX;
            outMetrics.printableHeightPx = (outMetrics.physicalHeightMm / 25.4f) * outMetrics.dpiY;
            return false;
        }
        *outDc = hdc;
        outMetrics = MetricsFromHdc(hdc);
        return true;
    }

    // Information context for preview — same DEVMODE fields as the real DC path
    if (hasDevMode) {
        hdc = ::CreateICW(L"WINSPOOL", settings.printerName.c_str(), nullptr, pDevMode);
    }
    if (!hdc) {
        hdc = ::CreateICW(L"WINSPOOL", settings.printerName.c_str(), nullptr, nullptr);
    }
    if (!hdc) {
        outMetrics.dpiX = 600.0f;
        outMetrics.dpiY = 600.0f;
        outMetrics.physicalWidthMm = settings.isLandscape ? 297.0f : 210.0f;
        outMetrics.physicalHeightMm = settings.isLandscape ? 210.0f : 297.0f;
        outMetrics.printableWidthPx = (outMetrics.physicalWidthMm / 25.4f) * outMetrics.dpiX;
        outMetrics.printableHeightPx = (outMetrics.physicalHeightMm / 25.4f) * outMetrics.dpiY;
        return false;
    }

    outMetrics = MetricsFromHdc(hdc);
    ::DeleteDC(hdc);
    return true;
}

D2D1::Matrix3x2F PrintManager::BuildExifOrientationMatrix(int orientation) {
    // Sensor pixels are assumed centered at origin before this matrix is applied.
    // Matches main viewer GPU pre-rotation (g_renderExifOrientation).
    switch (orientation) {
    case 2: return D2D1::Matrix3x2F::Scale(-1.0f, 1.0f); // Mirror horizontal
    case 3: return D2D1::Matrix3x2F::Rotation(180.0f);
    case 4: return D2D1::Matrix3x2F::Scale(1.0f, -1.0f); // Mirror vertical
    case 5: return D2D1::Matrix3x2F::Scale(-1.0f, 1.0f) * D2D1::Matrix3x2F::Rotation(270.0f);
    case 6: return D2D1::Matrix3x2F::Rotation(90.0f);
    case 7: return D2D1::Matrix3x2F::Scale(-1.0f, 1.0f) * D2D1::Matrix3x2F::Rotation(90.0f);
    case 8: return D2D1::Matrix3x2F::Rotation(270.0f);
    default: return D2D1::Matrix3x2F::Identity();
    }
}

D2D1_SIZE_F PrintManager::GetOrientedSourceSize(float sourceWidthPx, float sourceHeightPx, int exifOrientation) {
    float w = sourceWidthPx;
    float h = sourceHeightPx;
    if (exifOrientation >= 5 && exifOrientation <= 8) {
        std::swap(w, h);
    }
    return D2D1::SizeF(w, h);
}

D2D1::Matrix3x2F PrintManager::CalculatePrintTransform(
    const PrintDeviceMetrics& metrics, 
    const D2D1_SIZE_F& imageSize, 
    const PrintJobSettings& settings, 
    int* outCols, 
    int* outRows,
    D2D1_RECT_F* outMarginRectPx,
    float* outPageWPx,
    float* outPageHPx
) {
    // Keep internal calculations in double precision to prevent floating-point cumulative errors in layout
    // 1. Calculate physical unprintable (hard) margins in mm
    double hardMarginLeftMm = (metrics.physicalOffsetX_Px / metrics.dpiX) * 25.4;
    double hardMarginTopMm = (metrics.physicalOffsetY_Px / metrics.dpiY) * 25.4;
    double hardMarginRightMm = metrics.physicalWidthMm - ((metrics.printableWidthPx + metrics.physicalOffsetX_Px) / metrics.dpiX) * 25.4;
    double hardMarginBottomMm = metrics.physicalHeightMm - ((metrics.printableHeightPx + metrics.physicalOffsetY_Px) / metrics.dpiY) * 25.4;

    // 2. Logical page size (physical page minus user margins)
    double logicalPageWMm = metrics.physicalWidthMm - settings.marginLeft - settings.marginRight;
    double logicalPageHMm = metrics.physicalHeightMm - settings.marginTop - settings.marginBottom;

    if (logicalPageWMm <= 0 || logicalPageHMm <= 0) {
        if (outCols) *outCols = 1;
        if (outRows) *outRows = 1;
        if (outPageWPx) *outPageWPx = 0;
        if (outPageHPx) *outPageHPx = 0;
        if (outMarginRectPx) *outMarginRectPx = D2D1::RectF(0, 0, 0, 0);
        return D2D1::Matrix3x2F::Identity();
    }

    // 3. Safe area for 'Fit' mode (intersection of user margins and hard margins)
    double safeLeftMm = std::max((double)settings.marginLeft, hardMarginLeftMm);
    double safeTopMm = std::max((double)settings.marginTop, hardMarginTopMm);
    double safeRightMm = std::max((double)settings.marginRight, hardMarginRightMm);
    double safeBottomMm = std::max((double)settings.marginBottom, hardMarginBottomMm);
    
    double safeWidthMm = std::max(0.0, metrics.physicalWidthMm - safeLeftMm - safeRightMm);
    double safeHeightMm = std::max(0.0, metrics.physicalHeightMm - safeTopMm - safeBottomMm);

    // 4. Calculate clipping rect in Printer DC coordinates (starts at hardMarginLeft/Top)
    if (outMarginRectPx) {
        double clipLeftPx = (safeLeftMm - hardMarginLeftMm) * (metrics.dpiX / 25.4);
        double clipTopPx = (safeTopMm - hardMarginTopMm) * (metrics.dpiY / 25.4);
        double clipRightPx = (metrics.physicalWidthMm - safeRightMm - hardMarginLeftMm) * (metrics.dpiX / 25.4);
        double clipBottomPx = (metrics.physicalHeightMm - safeBottomMm - hardMarginTopMm) * (metrics.dpiY / 25.4);
        
        *outMarginRectPx = D2D1::RectF(
            (float)clipLeftPx,
            (float)clipTopPx,
            (float)clipRightPx,
            (float)clipBottomPx
        );
    }
    
    // outPageWPx/HPx is used for tiling translation in printer pixels.
    // For physical tiling, each page cell is exactly the physical width/height.
    if (outPageWPx) *outPageWPx = (float)(metrics.physicalWidthMm * metrics.dpiX / 25.4);
    if (outPageHPx) *outPageHPx = (float)(metrics.physicalHeightMm * metrics.dpiY / 25.4);

    // 5. Convert Image to physical MM using embedded DPI (layout-locked in settings)
    // imageSize is sensor/decode pixel size (unoriented).
    double rawW = imageSize.width;
    double rawH = imageSize.height;

    int exif = settings.exifOrientation;
    if (exif < 1 || exif > 8) exif = 1;

    // Oriented size after EXIF (before user rotation)
    double orientedW = rawW;
    double orientedH = rawH;
    if (exif >= 5 && exif <= 8) {
        std::swap(orientedW, orientedH);
    }

    // Final layout size after user rotation
    double layoutW = orientedW;
    double layoutH = orientedH;
    if (settings.rotationAngle == 90 || settings.rotationAngle == 270) {
        std::swap(layoutW, layoutH);
    }
    
    // Fallback to 96 if zero; prefer settings-locked DPI so preview == print
    double dpiX = (settings.imageDpiX > 0.0) ? settings.imageDpiX : 96.0;
    double dpiY = (settings.imageDpiY > 0.0) ? settings.imageDpiY : 96.0;

    double imagePhysicalWidthMm = (layoutW / dpiX) * 25.4;
    double imagePhysicalHeightMm = (layoutH / dpiY) * 25.4;

    // 6. Apply user's layout/scale settings to the physical image size
    double scaleX = logicalPageWMm / imagePhysicalWidthMm;
    double scaleY = logicalPageHMm / imagePhysicalHeightMm;
    double userScale = 1.0;

    switch (settings.layoutMode) {
    case PrintLayoutMode::Fit:
        // For Fit, we must ensure it doesn't get clipped by the dead zones, so use safe bounds
        if (safeWidthMm > 0 && safeHeightMm > 0) {
            double safeScaleX = safeWidthMm / imagePhysicalWidthMm;
            double safeScaleY = safeHeightMm / imagePhysicalHeightMm;
            userScale = std::min(safeScaleX, safeScaleY);
        } else {
            userScale = std::min(scaleX, scaleY);
        }
        break;
    case PrintLayoutMode::Fill:
        userScale = std::max(scaleX, scaleY);
        break;
    case PrintLayoutMode::Original:
        userScale = 1.0;
        break;
    case PrintLayoutMode::Custom:
        userScale = settings.customScale;
        break;
    }

    // 7. Calculate final image physical dimensions and grid size
    double finalImageWidthMm = imagePhysicalWidthMm * userScale;
    double finalImageHeightMm = imagePhysicalHeightMm * userScale;

    int cols = 1;
    int rows = 1;
    if (settings.printMultiPage) {
        cols = (int)std::ceil(finalImageWidthMm / logicalPageWMm - 0.001);
        rows = (int)std::ceil(finalImageHeightMm / logicalPageHMm - 0.001);
        if (cols < 1) cols = 1;
        if (rows < 1) rows = 1;
    }
    
    if (outCols) *outCols = cols;
    if (outRows) *outRows = rows;

    // 8. Align the image inside the poster grid
    double posterWMm = cols * logicalPageWMm;
    double posterHMm = rows * logicalPageHMm;
    
    // Default TopLeft offsets in mm (relative to the logical page grid)
    double offsetX_Mm = 0.0;
    double offsetY_Mm = 0.0;

    switch (settings.alignment) {
    case PrintAlignment::TopCenter:
    case PrintAlignment::Center:
    case PrintAlignment::BottomCenter:
        offsetX_Mm = (posterWMm - finalImageWidthMm) * 0.5;
        break;
    case PrintAlignment::TopRight:
    case PrintAlignment::CenterRight:
    case PrintAlignment::BottomRight:
        offsetX_Mm = posterWMm - finalImageWidthMm;
        break;
    default: break;
    }

    switch (settings.alignment) {
    case PrintAlignment::CenterLeft:
    case PrintAlignment::Center:
    case PrintAlignment::CenterRight:
        offsetY_Mm = (posterHMm - finalImageHeightMm) * 0.5;
        break;
    case PrintAlignment::BottomLeft:
    case PrintAlignment::BottomCenter:
    case PrintAlignment::BottomRight:
        offsetY_Mm = posterHMm - finalImageHeightMm;
        break;
    default: break;
    }

    // 9. Build the Transform Matrix (From Sensor Image Pixels -> Printer Device Pixels)
    // Scale is isotropic in physical mm space using each axis DPI.
    double imageToMmScaleX = (25.4 / dpiX) * userScale;
    double imageToMmScaleY = (25.4 / dpiY) * userScale;

    double mmToPrinterScaleX = metrics.dpiX / 25.4;
    double mmToPrinterScaleY = metrics.dpiY / 25.4;

    double overallScaleX = imageToMmScaleX * mmToPrinterScaleX;
    double overallScaleY = imageToMmScaleY * mmToPrinterScaleY;

    // Convert absolute physical position to Printer DC coordinates
    double absoluteLeftMm = settings.marginLeft + offsetX_Mm;
    double absoluteTopMm = settings.marginTop + offsetY_Mm;

    double finalOffsetX_Px = (absoluteLeftMm - hardMarginLeftMm) * mmToPrinterScaleX;
    double finalOffsetY_Px = (absoluteTopMm - hardMarginTopMm) * mmToPrinterScaleY;

    // Order: center sensor pixels → EXIF orient → user rotation → physical scale → place on page
    D2D1::Matrix3x2F m =
        D2D1::Matrix3x2F::Translation(-(float)rawW / 2.0f, -(float)rawH / 2.0f) *
        BuildExifOrientationMatrix(exif) *
        D2D1::Matrix3x2F::Rotation((float)settings.rotationAngle) *
        D2D1::Matrix3x2F::Scale((float)overallScaleX, (float)overallScaleY) *
        D2D1::Matrix3x2F::Translation(
            (float)(finalOffsetX_Px + (finalImageWidthMm * mmToPrinterScaleX) / 2.0),
            (float)(finalOffsetY_Px + (finalImageHeightMm * mmToPrinterScaleY) / 2.0)
        );

    return m;
}

std::expected<void, HRESULT> PrintManager::ExecutePrintJob(const PrintJobSettings& settings) {
    if (settings.printerName.empty()) {
        return std::unexpected(E_INVALIDARG);
    }

    PrintDeviceMetrics metrics = {};
    HDC hdcPrint = nullptr;
    if (!QueryPrintDeviceMetrics(settings, metrics, &hdcPrint) || !hdcPrint) {
        return std::unexpected(HRESULT_FROM_WIN32(GetLastError()));
    }

    DOCINFOW docInfo = {};
    docInfo.cbSize = sizeof(DOCINFOW);
    docInfo.lpszDocName = L"QuickView Print Job";
    
    if (::StartDocW(hdcPrint, &docInfo) <= 0) {
        ::DeleteDC(hdcPrint);
        return std::unexpected(HRESULT_FROM_WIN32(GetLastError()));
    }

    // Initialize thread-local WIC imaging factory
    ComPtr<IWICImagingFactory> wicFactory;
    HRESULT hr = ::CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&wicFactory)
    );
    if (FAILED(hr)) {
        ::EndDoc(hdcPrint);
        ::DeleteDC(hdcPrint);
        return std::unexpected(hr);
    }

    // Load decoder for target image file
    ComPtr<IWICBitmapDecoder> decoder;
    hr = wicFactory->CreateDecoderFromFilename(
        settings.imagePath.c_str(),
        nullptr,
        GENERIC_READ,
        WICDecodeMetadataCacheOnDemand,
        &decoder
    );
    if (FAILED(hr)) {
        ::EndDoc(hdcPrint);
        ::DeleteDC(hdcPrint);
        return std::unexpected(hr);
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) {
        ::EndDoc(hdcPrint);
        ::DeleteDC(hdcPrint);
        return std::unexpected(hr);
    }

    uint32_t rawW = 0, rawH = 0;
    frame->GetSize(&rawW, &rawH);

    double dpiX = 96.0, dpiY = 96.0;
    frame->GetResolution(&dpiX, &dpiY);

    PrintJobSettings layoutSettings = settings;
    float layoutW = settings.sourceWidthPx > 0.0f ? settings.sourceWidthPx : static_cast<float>(rawW);
    float layoutH = settings.sourceHeightPx > 0.0f ? settings.sourceHeightPx : static_cast<float>(rawH);
    if (layoutSettings.imageDpiX <= 0.0) {
        layoutSettings.imageDpiX = dpiX > 0.0 ? dpiX : 96.0;
    }
    if (layoutSettings.imageDpiY <= 0.0) {
        layoutSettings.imageDpiY = dpiY > 0.0 ? dpiY : 96.0;
    }
    if (layoutSettings.exifOrientation < 1 || layoutSettings.exifOrientation > 8) {
        layoutSettings.exifOrientation = 1;
    }

    D2D1_SIZE_F imageSize = { layoutW, layoutH };
    
    int cols = 1;
    int rows = 1;
    D2D1_RECT_F marginRect = {};
    float pageWPx = 0;
    float pageHPx = 0;
    
    D2D1::Matrix3x2F m = CalculatePrintTransform(metrics, imageSize, layoutSettings, &cols, &rows, &marginRect, &pageWPx, &pageHPx);
    int totalPages = cols * rows;

    // Build the core WIC transformation pipeline
    ComPtr<IWICBitmapSource> currentSource = frame;

    // 1. EXIF Orientation correction
    WICBitmapTransformOptions exifOpt = WICBitmapTransformRotate0;
    switch (layoutSettings.exifOrientation) {
    case 2: exifOpt = WICBitmapTransformFlipHorizontal; break;
    case 3: exifOpt = WICBitmapTransformRotate180; break;
    case 4: exifOpt = WICBitmapTransformFlipVertical; break;
    case 5: exifOpt = static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal); break;
    case 6: exifOpt = WICBitmapTransformRotate90; break;
    case 7: exifOpt = static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal); break;
    case 8: exifOpt = WICBitmapTransformRotate270; break;
    default: break;
    }
    if (exifOpt != WICBitmapTransformRotate0) {
        ComPtr<IWICBitmapFlipRotator> rotator;
        hr = wicFactory->CreateBitmapFlipRotator(&rotator);
        if (SUCCEEDED(hr)) {
            hr = rotator->Initialize(currentSource.Get(), exifOpt);
            if (SUCCEEDED(hr)) {
                currentSource = rotator;
            }
        }
    }

    // 2. User-configured custom rotation
    WICBitmapTransformOptions userOpt = WICBitmapTransformRotate0;
    if (settings.rotationAngle == 90) userOpt = WICBitmapTransformRotate90;
    else if (settings.rotationAngle == 180) userOpt = WICBitmapTransformRotate180;
    else if (settings.rotationAngle == 270) userOpt = WICBitmapTransformRotate270;

    if (userOpt != WICBitmapTransformRotate0) {
        ComPtr<IWICBitmapFlipRotator> rotator;
        hr = wicFactory->CreateBitmapFlipRotator(&rotator);
        if (SUCCEEDED(hr)) {
            hr = rotator->Initialize(currentSource.Get(), userOpt);
            if (SUCCEEDED(hr)) {
                currentSource = rotator;
            }
        }
    }

    // 3. High fidelity ICC color profile translation
    UINT colorContextCount = 0;
    frame->GetColorContexts(0, nullptr, &colorContextCount);
    if (colorContextCount > 0) {
        std::vector<IWICColorContext*> rawContexts(colorContextCount, nullptr);
        if (SUCCEEDED(frame->GetColorContexts(colorContextCount, rawContexts.data(), &colorContextCount))) {
            std::vector<ComPtr<IWICColorContext>> sourceColorContexts(colorContextCount);
            for (UINT i = 0; i < colorContextCount; ++i) {
                sourceColorContexts[i].Attach(rawContexts[i]);
            }
            std::wstring printerIccPath;
            DWORD dwLen = 0;
            ::GetICMProfileW(hdcPrint, &dwLen, nullptr);
            if (dwLen > 0) {
                printerIccPath.resize(dwLen);
                if (::GetICMProfileW(hdcPrint, &dwLen, &printerIccPath[0])) {
                    printerIccPath.resize(dwLen > 0 ? dwLen - 1 : 0);
                }
            }
            if (!printerIccPath.empty()) {
                ComPtr<IWICColorContext> destContext;
                if (SUCCEEDED(wicFactory->CreateColorContext(&destContext))) {
                    if (SUCCEEDED(destContext->InitializeFromFilename(printerIccPath.c_str()))) {
                        ComPtr<IWICColorTransform> colorTransform;
                        if (SUCCEEDED(wicFactory->CreateColorTransformer(&colorTransform))) {
                            if (SUCCEEDED(colorTransform->Initialize(currentSource.Get(), sourceColorContexts[0].Get(), destContext.Get(), GUID_WICPixelFormat32bppBGRA))) {
                                currentSource = colorTransform;
                            }
                        }
                    }
                }
            }
        }
    }

    // 4. Grayscale processing (BGR -> 8bppGray -> BGR to avoid GDI palettes)
    if (settings.grayscale) {
        ComPtr<IWICFormatConverter> grayConverter;
        hr = wicFactory->CreateFormatConverter(&grayConverter);
        if (SUCCEEDED(hr)) {
            hr = grayConverter->Initialize(
                currentSource.Get(),
                GUID_WICPixelFormat8bppGray,
                WICBitmapDitherTypeNone,
                nullptr,
                0.0f,
                WICBitmapPaletteTypeCustom
            );
            if (SUCCEEDED(hr)) {
                currentSource = grayConverter;
            }
        }
    }

    // 5. Unify output format to BGRA
    ComPtr<IWICFormatConverter> finalBgraConverter;
    hr = wicFactory->CreateFormatConverter(&finalBgraConverter);
    if (FAILED(hr)) {
        ::EndDoc(hdcPrint);
        ::DeleteDC(hdcPrint);
        return std::unexpected(hr);
    }
    hr = finalBgraConverter->Initialize(
        currentSource.Get(),
        GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone,
        nullptr,
        0.0f,
        WICBitmapPaletteTypeCustom
    );
    if (FAILED(hr)) {
        ::EndDoc(hdcPrint);
        ::DeleteDC(hdcPrint);
        return std::unexpected(hr);
    }
    currentSource = finalBgraConverter;

    uint32_t orientedW = 0, orientedH = 0;
    currentSource->GetSize(&orientedW, &orientedH);

    // Query printer capabilities to determine whether to copy pages manually
    int copyCount = 1;
    DWORD maxCopies = ::DeviceCapabilitiesW(settings.printerName.c_str(), nullptr, DC_COPIES, nullptr, nullptr);
    if (maxCopies <= 1) {
        copyCount = settings.copies > 0 ? settings.copies : 1;
    }

    for (int c = 0; c < copyCount; ++c) {
        // Render page by page
        for (int p = 0; p < totalPages; ++p) {
            if (settings.printMultiPage) {
                if (settings.disabledPages.count(p) > 0) continue;
            }

            D2D1::Matrix3x2F pageTransform = m;
            if (settings.printMultiPage) {
                int row = p / cols;
                int col = p % cols;
                pageTransform = m * D2D1::Matrix3x2F::Translation(-col * pageWPx, -row * pageHPx);
            }

            // Map oriented dimensions to printer device pixels
            D2D1_POINT_2F p1 = pageTransform.TransformPoint(D2D1::Point2F(0.0f, 0.0f));
            D2D1_POINT_2F p2 = pageTransform.TransformPoint(D2D1::Point2F(layoutW, 0.0f));
            D2D1_POINT_2F p3 = pageTransform.TransformPoint(D2D1::Point2F(0.0f, layoutH));
            D2D1_POINT_2F p4 = pageTransform.TransformPoint(D2D1::Point2F(layoutW, layoutH));

            float minX = std::min({ p1.x, p2.x, p3.x, p4.x });
            float maxX = std::max({ p1.x, p2.x, p3.x, p4.x });
            float minY = std::min({ p1.y, p2.y, p3.y, p4.y });
            float maxY = std::max({ p1.y, p2.y, p3.y, p4.y });

            float destWidth = maxX - minX;
            float destHeight = maxY - minY;
            float destX = minX;
            float destY = minY;

            if (destWidth <= 0.1f || destHeight <= 0.1f) {
                continue;
            }

            ComPtr<IWICBitmapSource> pageSource = currentSource;
            uint32_t finalW = orientedW;
            uint32_t finalH = orientedH;

            // Downscale protective scaling for massive pictures to defend memory
            bool needScale = false;
            if (orientedW > static_cast<uint32_t>(destWidth) || orientedH > static_cast<uint32_t>(destHeight)) {
                needScale = true;
                finalW = static_cast<uint32_t>(std::round(destWidth));
                finalH = static_cast<uint32_t>(std::round(destHeight));
                if (finalW == 0) finalW = 1;
                if (finalH == 0) finalH = 1;
            }

            if (needScale) {
                ComPtr<IWICBitmapScaler> scaler;
                if (SUCCEEDED(wicFactory->CreateBitmapScaler(&scaler))) {
                    if (SUCCEEDED(scaler->Initialize(pageSource.Get(), finalW, finalH, WICBitmapInterpolationModeFant))) {
                        pageSource = scaler;
                    }
                }
            }

            // Copy raw pixel stream
            uint32_t stride = finalW * 4;
            size_t bufferSize = static_cast<size_t>(stride) * finalH;
            std::vector<uint8_t> pixelBuffer(bufferSize);
            hr = pageSource->CopyPixels(nullptr, stride, static_cast<UINT>(bufferSize), pixelBuffer.data());
            if (FAILED(hr)) {
                continue;
            }

            ::StartPage(hdcPrint);

            // Apply native GDI clipping boundary
            HRGN hClipRgn = ::CreateRectRgn(
                static_cast<int>(std::round(marginRect.left)),
                static_cast<int>(std::round(marginRect.top)),
                static_cast<int>(std::round(marginRect.right)),
                static_cast<int>(std::round(marginRect.bottom))
            );
            ::SelectClipRgn(hdcPrint, hClipRgn);

            // Configure stretching and alignment characteristics
            ::SetStretchBltMode(hdcPrint, HALFTONE);
            ::SetBrushOrgEx(hdcPrint, 0, 0, nullptr);

            BITMAPINFO bmi = {};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = static_cast<LONG>(finalW);
            bmi.bmiHeader.biHeight = -static_cast<LONG>(finalH); // Top-down
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;

            ::StretchDIBits(
                hdcPrint,
                static_cast<int>(std::round(destX)),
                static_cast<int>(std::round(destY)),
                static_cast<int>(std::round(destWidth)),
                static_cast<int>(std::round(destHeight)),
                0, 0,
                static_cast<int>(finalW),
                static_cast<int>(finalH),
                pixelBuffer.data(),
                &bmi,
                DIB_RGB_COLORS,
                SRCCOPY
            );

            ::SelectClipRgn(hdcPrint, nullptr);
            ::DeleteObject(hClipRgn);

            ::EndPage(hdcPrint);
        }
    }

    ::EndDoc(hdcPrint);
    ::DeleteDC(hdcPrint);

    return {};
}

} // namespace QuickView
