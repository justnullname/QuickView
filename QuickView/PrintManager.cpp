#include "PrintManager.h"
#include "RenderEngine.h"
#include "ImageLoader.h"
#include <winspool.h>
#include <memory>
#include <thread>
#include <cmath>
#include <algorithm>
#include <d2d1effects.h>

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
        PRINTER_INFO_4W* pInfo = reinterpret_cast<PRINTER_INFO_4W*>(buffer.data());
        for (DWORD i = 0; i < cReturned; ++i) {
            PrinterInfo info;
            info.name = pInfo[i].pPrinterName;
            
            // Check if default (can use GetDefaultPrinterW for precision)
            DWORD defaultCb = 0;
            ::GetDefaultPrinterW(nullptr, &defaultCb);
            if (defaultCb > 0) {
                std::wstring defPrinter(defaultCb, L'\0');
                if (::GetDefaultPrinterW(defPrinter.data(), &defaultCb)) {
                    // Remove null terminator added by GetDefaultPrinterW
                    defPrinter.resize(defaultCb > 0 ? defaultCb - 1 : 0);
                    if (info.name == defPrinter) {
                        info.isDefault = true;
                    }
                }
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
    ::ClosePrinter(hPrinter);

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

    // 1. Evaluate source dimensions to downscale during decoding if needed (OOM Defense)
    CImageLoader::ImageHeaderInfo headerInfo = g_pImageLoader->PeekHeader(settings.imagePath.c_str());
    int targetW = 0;
    int targetH = 0;
    const int printableWidth = (int)std::lround(metrics.printableWidthPx);
    const int printableHeight = (int)std::lround(metrics.printableHeightPx);
    if (headerInfo.width > 16384 || headerInfo.height > 16384) {
        // Only downscale if the image is astronomically large to prevent memory overflow
        float scale = 16384.0f / std::max(headerInfo.width, headerInfo.height);
        targetW = static_cast<int>(headerInfo.width * scale);
        targetH = static_cast<int>(headerInfo.height * scale);
    }

    // 2. Decode image file to a clean RawImageFrame
    QuickView::RawImageFrame rawFrame;
    HRESULT hr = g_pImageLoader->LoadToFrame(settings.imagePath.c_str(), &rawFrame, nullptr, targetW, targetH);
    if (FAILED(hr)) {
        ::EndDoc(hdcPrint);
        ::DeleteDC(hdcPrint);
        return std::unexpected(hr);
    }
    
    // Apply grayscale manually to guarantee it works on all printers
    if (settings.grayscale && rawFrame.pixels && rawFrame.format == QuickView::PixelFormat::BGRA8888) {
        size_t pixelCount = (size_t)rawFrame.width * rawFrame.height;
        uint32_t* p32 = reinterpret_cast<uint32_t*>(rawFrame.pixels);
        for (size_t i = 0; i < pixelCount; ++i) {
            uint32_t c = p32[i];
            uint32_t b = c & 0xFF;
            uint32_t g = (c >> 8) & 0xFF;
            uint32_t r = (c >> 16) & 0xFF;
            uint32_t a = c & 0xFF000000;
            uint32_t gray = (r * 77 + g * 150 + b * 29) >> 8;
            p32[i] = a | (gray << 16) | (gray << 8) | gray;
        }
    }

    // 3. Setup Memory DC and DIB Section for offscreen rendering
    // D2D cannot reliably BindDC directly to a Printer DC (causes blank output).
    // We render to a Memory DIB first, then BitBlt to the printer.
    HDC hMemDC = ::CreateCompatibleDC(nullptr);
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = printableWidth;
    bmi.bmiHeader.biHeight = -printableHeight; // Top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    
    void* pDibPixels = nullptr;
    HBITMAP hDib = ::CreateDIBSection(hMemDC, &bmi, DIB_RGB_COLORS, &pDibPixels, nullptr, 0);
    if (!hDib) {
        rawFrame.Release();
        ::DeleteDC(hMemDC);
        ::EndDoc(hdcPrint);
        ::DeleteDC(hdcPrint);
        return std::unexpected(E_OUTOFMEMORY);
    }
    HGDIOBJ hOldBmp = ::SelectObject(hMemDC, hDib);

    ComPtr<ID2D1Factory> d2dFactory;
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), &d2dFactory);

    ComPtr<ID2D1DCRenderTarget> dcTarget;
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        0, 0,
        D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE
    );
    hr = d2dFactory->CreateDCRenderTarget(&props, &dcTarget);
    if (FAILED(hr)) {
        rawFrame.Release();
        ::SelectObject(hMemDC, hOldBmp);
        ::DeleteObject(hDib);
        ::DeleteDC(hMemDC);
        ::EndDoc(hdcPrint);
        ::DeleteDC(hdcPrint);
        return std::unexpected(hr);
    }

    RECT rcBind = {0, 0, printableWidth, printableHeight};
    hr = dcTarget->BindDC(hMemDC, &rcBind);
    if (FAILED(hr)) {
        rawFrame.Release();
        ::SelectObject(hMemDC, hOldBmp);
        ::DeleteObject(hDib);
        ::DeleteDC(hMemDC);
        ::EndDoc(hdcPrint);
        ::DeleteDC(hdcPrint);
        return std::unexpected(hr);
    }

    // 4. Create source D2D bitmap
    // DC render targets only support B8G8R8A8_UNORM. For non-BGRA sources,
    // we still create with BGRA (WIC LoadToFrame typically outputs BGRA).
    ComPtr<ID2D1Bitmap> rawBitmap;
    D2D1_BITMAP_PROPERTIES bmpProps = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE)
    );
    hr = dcTarget->CreateBitmap(
        D2D1::SizeU(rawFrame.width, rawFrame.height),
        rawFrame.pixels,
        rawFrame.stride,
        bmpProps,
        &rawBitmap
    );
    if (FAILED(hr)) {
        rawFrame.Release();
        ::SelectObject(hMemDC, hOldBmp);
        ::DeleteObject(hDib);
        ::DeleteDC(hMemDC);
        ::EndDoc(hdcPrint);
        ::DeleteDC(hdcPrint);
        return std::unexpected(hr);
    }

    // 5. Layout uses locked source geometry from the preview dialog when available.
    //    Falls back to decoded frame (and its DPI) so standalone jobs still work.
    //    When decode downscales (>16384), draw into logical source rect so layout stays exact.
    PrintJobSettings layoutSettings = settings;
    float layoutW = settings.sourceWidthPx > 0.0f ? settings.sourceWidthPx : (float)rawFrame.width;
    float layoutH = settings.sourceHeightPx > 0.0f ? settings.sourceHeightPx : (float)rawFrame.height;
    if (layoutSettings.imageDpiX <= 0.0) {
        layoutSettings.imageDpiX = rawFrame.dpiX > 0.0 ? rawFrame.dpiX : 96.0;
    }
    if (layoutSettings.imageDpiY <= 0.0) {
        layoutSettings.imageDpiY = rawFrame.dpiY > 0.0 ? rawFrame.dpiY : 96.0;
    }
    if (layoutSettings.exifOrientation < 1 || layoutSettings.exifOrientation > 8) {
        // Prefer orientation from the decoded frame when settings did not lock it
        int frameExif = rawFrame.exifOrientation;
        layoutSettings.exifOrientation = (frameExif >= 1 && frameExif <= 8) ? frameExif : 1;
    }

    D2D1_SIZE_F imageSize = { layoutW, layoutH };
    
    int cols = 1;
    int rows = 1;
    D2D1_RECT_F marginRect = {};
    float pageWPx = 0;
    float pageHPx = 0;
    
    D2D1::Matrix3x2F m = CalculatePrintTransform(metrics, imageSize, layoutSettings, &cols, &rows, &marginRect, &pageWPx, &pageHPx);

    int totalPages = cols * rows;

    for (int p = 0; p < totalPages; ++p) {
        if (settings.printMultiPage) {
            if (settings.disabledPages.count(p) > 0) continue;
        }

        ::StartPage(hdcPrint);
        
        dcTarget->BeginDraw();
        dcTarget->Clear(D2D1::ColorF(D2D1::ColorF::White));

        D2D1::Matrix3x2F pageTransform = m;
        if (settings.printMultiPage) {
            int row = p / cols;
            int col = p % cols;
            pageTransform = m * D2D1::Matrix3x2F::Translation(-col * pageWPx, -row * pageHPx);
        }

        // Ensure transform is identity before pushing clip, as D2D retains state across frames
        dcTarget->SetTransform(D2D1::Matrix3x2F::Identity());
        
        // 强行剪裁到安全边距内，确保拼贴海报边缘绝对清晰，不会溢出到打印机物理死区或相邻纸张
        dcTarget->PushAxisAlignedClip(marginRect, D2D1_ANTIALIAS_MODE_ALIASED);
        
        dcTarget->SetTransform(pageTransform);

        // Draw into logical source pixel rect so downscaled decodes still match layout size
        dcTarget->DrawBitmap(
            rawBitmap.Get(),
            D2D1::RectF(0.0f, 0.0f, layoutW, layoutH),
            1.0f,
            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR
        );

        dcTarget->PopAxisAlignedClip();

        hr = dcTarget->EndDraw();
        
        // Blast the memory DC to the actual printer DC
        ::BitBlt(hdcPrint, 0, 0, printableWidth, printableHeight, hMemDC, 0, 0, SRCCOPY);
        
        ::EndPage(hdcPrint);
    }
    
    // Cleanup Memory DC
    ::SelectObject(hMemDC, hOldBmp);
    ::DeleteObject(hDib);
    ::DeleteDC(hMemDC);

    // 7. Spooler complete
    ::EndDoc(hdcPrint);
    ::DeleteDC(hdcPrint);
    rawFrame.Release();

    return {};
}

HRESULT PrintManager::CreatePrinterColorContext(
    ID2D1DeviceContext* d2dContext,
    HDC printerDC,
    const std::wstring& customPrinterIcc,
    ID2D1ColorContext** outColorContext
) {
    if (!outColorContext) return E_INVALIDARG;
    *outColorContext = nullptr;

    if (!customPrinterIcc.empty()) {
        const HRESULT hr = d2dContext->CreateColorContextFromFilename(customPrinterIcc.c_str(), outColorContext);
        if (SUCCEEDED(hr)) return hr;
    }

    if (printerDC) {
        DWORD dwLen = 0;
        GetICMProfileW(printerDC, &dwLen, nullptr);
        if (dwLen > 0) {
            std::wstring profilePath(dwLen, L'\0');
            if (GetICMProfileW(printerDC, &dwLen, &profilePath[0])) {
                profilePath.resize(dwLen - 1);
                const HRESULT hr = d2dContext->CreateColorContextFromFilename(profilePath.c_str(), outColorContext);
                if (SUCCEEDED(hr)) return hr;
            }
        }
    }

    return E_FAIL;
}

} // namespace QuickView
