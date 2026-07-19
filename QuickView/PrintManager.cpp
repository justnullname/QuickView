/*
 * QuickView - Modern D2D Print Engine
 * Copyright (C) 2026-Present QuickView Contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "PrintManager.h"
#include "RenderEngine.h"
#include "ImageLoader.h"
#include <commdlg.h>
#include <shlwapi.h>
#include <d2d1effects.h>

#pragma comment(lib, "shlwapi.lib")

extern CRenderEngine* g_pRenderEngine;
extern CImageLoader* g_pImageLoader;

namespace QuickView {

// RAII guard for PrintDlgExW resources to eliminate cleanup duplication
struct PrintDialogGuard {
    PRINTDLGEXW& pdx;
    explicit PrintDialogGuard(PRINTDLGEXW& p) : pdx(p) {}
    ~PrintDialogGuard() {
        if (pdx.hDevMode) GlobalFree(pdx.hDevMode);
        if (pdx.hDevNames) GlobalFree(pdx.hDevNames);
        if (pdx.hDC) DeleteDC(pdx.hDC);
    }
    PrintDialogGuard(const PrintDialogGuard&) = delete;
    PrintDialogGuard& operator=(const PrintDialogGuard&) = delete;
};

HRESULT PrintManager::PrintImage(HWND hwndOwner, const std::wstring& imagePath, const std::wstring& customPrinterIcc) {
    if (imagePath.empty() || !g_pRenderEngine || !g_pImageLoader) {
        return E_INVALIDARG;
    }

    // 1. Show standard Win32 Print Dialog (Redirects to Modern Print UI on Win11 22H2+)
    PRINTDLGEXW pdx{};
    pdx.lStructSize = sizeof(PRINTDLGEXW);
    pdx.hwndOwner = hwndOwner;
    pdx.Flags = PD_RETURNDC | PD_USEDEVMODECOPIESANDCOLLATE | PD_NOPAGENUMS | PD_NOSELECTION;
    pdx.nStartPage = START_PAGE_GENERAL;
    pdx.nMinPage = 1;
    pdx.nMaxPage = 1;
    pdx.nCopies = 1;

    HRESULT hr = PrintDlgExW(&pdx);
    if (hr != S_OK || pdx.dwResultAction != PD_RESULT_PRINT) {
        // User cancelled or dialog failed
        if (pdx.hDevMode) GlobalFree(pdx.hDevMode);
        if (pdx.hDevNames) GlobalFree(pdx.hDevNames);
        if (pdx.hDC) DeleteDC(pdx.hDC);
        return (hr == S_OK) ? S_FALSE : hr;
    }

    // RAII guard for all subsequent early-return paths
    PrintDialogGuard guard(pdx);

    // 2. Lock DEVMODE and enforce Driver Color Management Bypass
    if (pdx.hDevMode) {
        LPDEVMODEW lpDevMode = static_cast<LPDEVMODEW>(GlobalLock(pdx.hDevMode));
        if (lpDevMode) {
            lpDevMode->dmFields |= DM_ICMMETHOD;
            lpDevMode->dmICMMethod = DMICMMETHOD_NONE;

            HDC newDC = ResetDCW(pdx.hDC, lpDevMode);
            if (newDC) {
                pdx.hDC = newDC;
            }
            GlobalUnlock(pdx.hDevMode);
        }
    }

    // 3. Query printer physical properties and DPI
    const int dpiX = GetDeviceCaps(pdx.hDC, LOGPIXELSX);
    const int dpiY = GetDeviceCaps(pdx.hDC, LOGPIXELSY);
    const int pageWidth = GetDeviceCaps(pdx.hDC, PHYSICALWIDTH);
    const int pageHeight = GetDeviceCaps(pdx.hDC, PHYSICALHEIGHT);
    const int offsetX = GetDeviceCaps(pdx.hDC, PHYSICALOFFSETX);
    const int offsetY = GetDeviceCaps(pdx.hDC, PHYSICALOFFSETY);
    const int printableWidth = GetDeviceCaps(pdx.hDC, HORZRES);
    const int printableHeight = GetDeviceCaps(pdx.hDC, VERTRES);

    if (dpiX <= 0 || dpiY <= 0 || printableWidth <= 0 || printableHeight <= 0) {
        return E_UNEXPECTED;
    }

    // 4. OOM Defense: Evaluate source dimensions to downscale during decoding if needed.
    CImageLoader::ImageHeaderInfo headerInfo = g_pImageLoader->PeekHeader(imagePath.c_str());
    int targetW = 0;
    int targetH = 0;
    if (headerInfo.width > 0 && headerInfo.height > 0) {
        if (headerInfo.width > static_cast<int>(printableWidth * 1.2f) ||
            headerInfo.height > static_cast<int>(printableHeight * 1.2f)) {
            const float scaleW = static_cast<float>(printableWidth) / headerInfo.width;
            const float scaleH = static_cast<float>(printableHeight) / headerInfo.height;
            const float scale = (scaleW < scaleH) ? scaleW : scaleH;
            targetW = static_cast<int>(headerInfo.width * scale);
            targetH = static_cast<int>(headerInfo.height * scale);
        }
    }

    // 5. Decode image file to a clean RawImageFrame
    QuickView::RawImageFrame rawFrame;
    hr = g_pImageLoader->LoadToFrame(imagePath.c_str(), &rawFrame, nullptr, targetW, targetH);
    if (FAILED(hr)) {
        return hr;
    }

    // 6. Extract Printer Name
    std::wstring printerName;
    if (pdx.hDevNames) {
        LPDEVNAMES lpDevNames = static_cast<LPDEVNAMES>(GlobalLock(pdx.hDevNames));
        if (lpDevNames) {
            printerName = reinterpret_cast<wchar_t*>(lpDevNames) + lpDevNames->wDeviceOffset;
            GlobalUnlock(pdx.hDevNames);
        }
    }

    // 7. Initialize Modern XPS Document Package Target
    ComPtr<IPrintDocumentPackageTargetFactory> packageTargetFactory;
    hr = CoCreateInstance(
        __uuidof(PrintDocumentPackageTargetFactory),
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&packageTargetFactory)
    );

    ComPtr<IPrintDocumentPackageTarget> packageTarget;
    if (SUCCEEDED(hr)) {
        hr = packageTargetFactory->CreateDocumentPackageTargetForPrintJob(
            printerName.c_str(),
            L"QuickView Modern Print Job",
            nullptr, // outputStream (nullptr routes directly to spooler)
            nullptr, // jobProperties (nullptr defaults to spooler settings)
            &packageTarget
        );
    }

    if (FAILED(hr)) {
        rawFrame.Release();
        return hr;
    }

    // 8. Fetch global D2D handles
    ID2D1Device* d2dDevice = g_pRenderEngine->GetD2DDevice();
    IWICImagingFactory* wicFactory = g_pRenderEngine->GetWICFactory();
    if (!d2dDevice || !wicFactory) {
        rawFrame.Release();
        return E_FAIL;
    }

    // 9. Create a DEDICATED DeviceContext for printing (CRITICAL FIX)
    //    Must NOT reuse the screen-bound DeviceContext to avoid state conflicts.
    ComPtr<ID2D1DeviceContext> printContext;
    hr = d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &printContext);
    if (FAILED(hr)) {
        rawFrame.Release();
        return hr;
    }

    // 10. Instantiate D2D Print Control
    ComPtr<ID2D1PrintControl> printControl;
    D2D1_PRINT_CONTROL_PROPERTIES printControlProps = {
        D2D1_PRINT_FONT_SUBSET_MODE_DEFAULT,
        static_cast<float>(dpiX),
        D2D1_COLOR_SPACE_SRGB
    };

    hr = d2dDevice->CreatePrintControl(
        wicFactory,
        packageTarget.Get(),
        &printControlProps,
        &printControl
    );

    if (FAILED(hr)) {
        rawFrame.Release();
        return hr;
    }

    // 11. Process D2D Drawing into CommandList
    ComPtr<ID2D1CommandList> commandList;
    hr = printContext->CreateCommandList(&commandList);
    if (FAILED(hr)) {
        rawFrame.Release();
        return hr;
    }

    printContext->SetTarget(commandList.Get());
    printContext->BeginDraw();
    printContext->Clear(D2D1::ColorF(D2D1::ColorF::White)); // White canvas background

    // Convert page margins to DIPs (96 DPI coordinate system)
    const float printL_Dips = static_cast<float>(offsetX) * 96.0f / dpiX;
    const float printT_Dips = static_cast<float>(offsetY) * 96.0f / dpiY;
    const float printW_Dips = static_cast<float>(printableWidth) * 96.0f / dpiX;
    const float printH_Dips = static_cast<float>(printableHeight) * 96.0f / dpiY;

    if (rawFrame.IsSvg()) {
        // SVG Vector Printing Path
        ComPtr<ID2D1DeviceContext5> printContext5;
        ComPtr<IStream> svgStream;
        if (SUCCEEDED(printContext->QueryInterface(IID_PPV_ARGS(&printContext5))) && rawFrame.svg) {
            svgStream.Attach(SHCreateMemStream(rawFrame.svg->xmlData.data(), static_cast<UINT>(rawFrame.svg->xmlData.size())));
            if (svgStream) {
                ComPtr<ID2D1SvgDocument> svgDoc;
                D2D1_SIZE_F vpSize = D2D1::SizeF(printW_Dips, printH_Dips);
                hr = printContext5->CreateSvgDocument(svgStream.Get(), vpSize, &svgDoc);
                if (SUCCEEDED(hr) && svgDoc) {
                    // Calculate aspect ratio fit for SVG
                    const float viewW = rawFrame.svg->viewBoxW > 0.0f ? rawFrame.svg->viewBoxW : printW_Dips;
                    const float viewH = rawFrame.svg->viewBoxH > 0.0f ? rawFrame.svg->viewBoxH : printH_Dips;

                    const float scaleW = printW_Dips / viewW;
                    const float scaleH = printH_Dips / viewH;
                    const float scale = (scaleW < scaleH) ? scaleW : scaleH;

                    const float finalW = viewW * scale;
                    const float finalH = viewH * scale;

                    const float destL = printL_Dips + (printW_Dips - finalW) / 2.0f;
                    const float destT = printT_Dips + (printH_Dips - finalH) / 2.0f;

                    D2D1_MATRIX_3X2_F finalTransform =
                        D2D1::Matrix3x2F::Scale(scale, scale) *
                        D2D1::Matrix3x2F::Translation(destL, destT);

                    printContext5->SetTransform(finalTransform);
                    printContext5->DrawSvgDocument(svgDoc.Get());
                }
            }
        }
    } else {
        // Bitmap Printing Path with CMS Proofing
        // Upload raw frame to a D2D bitmap using the dedicated print context
        ComPtr<ID2D1Bitmap> rawBitmap;
        if (rawFrame.pixels && rawFrame.width > 0 && rawFrame.height > 0) {
            // Map RawImageFrame pixel format to DXGI format
            DXGI_FORMAT dxgiFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
            switch (rawFrame.format) {
                case QuickView::PixelFormat::BGRA8888:
                case QuickView::PixelFormat::BGRX8888:
                    dxgiFormat = DXGI_FORMAT_B8G8R8A8_UNORM; break;
                case QuickView::PixelFormat::RGBA8888:
                    dxgiFormat = DXGI_FORMAT_R8G8B8A8_UNORM; break;
                case QuickView::PixelFormat::R32G32B32A32_FLOAT:
                    dxgiFormat = DXGI_FORMAT_R32G32B32A32_FLOAT; break;
                case QuickView::PixelFormat::R16G16B16A16_UNORM:
                    dxgiFormat = DXGI_FORMAT_R16G16B16A16_UNORM; break;
                case QuickView::PixelFormat::R16G16B16A16_FLOAT:
                    dxgiFormat = DXGI_FORMAT_R16G16B16A16_FLOAT; break;
                default: break;
            }

            D2D1_BITMAP_PROPERTIES bmpProps = D2D1::BitmapProperties(
                D2D1::PixelFormat(dxgiFormat, D2D1_ALPHA_MODE_PREMULTIPLIED)
            );
            hr = printContext->CreateBitmap(
                D2D1::SizeU(rawFrame.width, rawFrame.height),
                rawFrame.pixels,
                rawFrame.stride,
                bmpProps,
                &rawBitmap
            );
        }

        if (SUCCEEDED(hr) && rawBitmap) {
            // Initialize Source and Destination Color Contexts
            ComPtr<ID2D1ColorContext> srcColorContext;
            if (!rawFrame.iccProfile.empty()) {
                printContext->CreateColorContext(
                    D2D1_COLOR_SPACE_CUSTOM, rawFrame.iccProfile.data(),
                    static_cast<UINT32>(rawFrame.iccProfile.size()), &srcColorContext);
            }
            if (!srcColorContext) {
                printContext->CreateColorContext(D2D1_COLOR_SPACE_SRGB, nullptr, 0, &srcColorContext);
            }

            ComPtr<ID2D1ColorContext> dstColorContext;
            hr = CreatePrinterColorContext(printContext.Get(), pdx.hDC, customPrinterIcc, &dstColorContext);
            if (FAILED(hr) || !dstColorContext) {
                printContext->CreateColorContext(D2D1_COLOR_SPACE_SRGB, nullptr, 0, &dstColorContext);
            }

            // Setup CMS Color Management Effect
            ComPtr<ID2D1Effect> colorCmsEffect;
            hr = printContext->CreateEffect(CLSID_D2D1ColorManagement, &colorCmsEffect);
            if (SUCCEEDED(hr)) {
                colorCmsEffect->SetInput(0, rawBitmap.Get());
                colorCmsEffect->SetValue(D2D1_COLORMANAGEMENT_PROP_SOURCE_COLOR_CONTEXT, srcColorContext.Get());
                colorCmsEffect->SetValue(D2D1_COLORMANAGEMENT_PROP_DESTINATION_COLOR_CONTEXT, dstColorContext.Get());
                colorCmsEffect->SetValue(D2D1_COLORMANAGEMENT_PROP_QUALITY, D2D1_COLORMANAGEMENT_QUALITY_BEST);

                const D2D1_SIZE_F imgSize = rawBitmap->GetSize();
                const float imgW = imgSize.width;
                const float imgH = imgSize.height;

                // Apply EXIF rotation to viewport calculations
                const int orientation = rawFrame.exifOrientation;
                const bool swapped = (orientation >= 5 && orientation <= 8);
                const float drawW = swapped ? imgH : imgW;
                const float drawH = swapped ? imgW : imgH;

                const float scaleW = printW_Dips / drawW;
                const float scaleH = printH_Dips / drawH;
                const float scale = (scaleW < scaleH) ? scaleW : scaleH;

                const float finalW = drawW * scale;
                const float finalH = drawH * scale;

                const float destL = printL_Dips + (printW_Dips - finalW) / 2.0f;
                const float destT = printT_Dips + (printH_Dips - finalH) / 2.0f;

                // Construct affine transform tree for EXIF & alignment
                const float cx = imgW / 2.0f;
                const float cy = imgH / 2.0f;
                D2D1_MATRIX_3X2_F rotationTransform = D2D1::Matrix3x2F::Identity();

                if (orientation == 2) {
                    rotationTransform = D2D1::Matrix3x2F::Scale(-1.0f, 1.0f, D2D1::Point2F(cx, cy));
                } else if (orientation == 3) {
                    rotationTransform = D2D1::Matrix3x2F::Rotation(180.0f, D2D1::Point2F(cx, cy));
                } else if (orientation == 4) {
                    rotationTransform = D2D1::Matrix3x2F::Scale(1.0f, -1.0f, D2D1::Point2F(cx, cy));
                } else if (orientation == 5) {
                    rotationTransform = D2D1::Matrix3x2F::Scale(-1.0f, 1.0f, D2D1::Point2F(cx, cy)) *
                                        D2D1::Matrix3x2F::Rotation(270.0f, D2D1::Point2F(cx, cy));
                } else if (orientation == 6) {
                    rotationTransform = D2D1::Matrix3x2F::Rotation(90.0f, D2D1::Point2F(cx, cy));
                } else if (orientation == 7) {
                    rotationTransform = D2D1::Matrix3x2F::Scale(-1.0f, 1.0f, D2D1::Point2F(cx, cy)) *
                                        D2D1::Matrix3x2F::Rotation(90.0f, D2D1::Point2F(cx, cy));
                } else if (orientation == 8) {
                    rotationTransform = D2D1::Matrix3x2F::Rotation(270.0f, D2D1::Point2F(cx, cy));
                }

                const float targetCx = destL + finalW / 2.0f;
                const float targetCy = destT + finalH / 2.0f;

                const D2D1_MATRIX_3X2_F finalTransform =
                    D2D1::Matrix3x2F::Translation(-cx, -cy) *
                    rotationTransform *
                    D2D1::Matrix3x2F::Scale(scale, scale) *
                    D2D1::Matrix3x2F::Translation(targetCx, targetCy);

                printContext->SetTransform(finalTransform);

                // High-quality bicubic resampling during print rasterization
                printContext->DrawImage(
                    colorCmsEffect.Get(),
                    D2D1::Point2F(0.0f, 0.0f),
                    D2D1::RectF(0.0f, 0.0f, imgW, imgH),
                    D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC
                );
            }
        }
    }

    // Reset transform before closing
    printContext->SetTransform(D2D1::Matrix3x2F::Identity());
    hr = printContext->EndDraw();
    if (FAILED(hr)) {
        rawFrame.Release();
        return hr;
    }

    hr = commandList->Close();
    if (FAILED(hr)) {
        rawFrame.Release();
        return hr;
    }

    // 12. Write command list page to the print pipeline and flush
    const float paperW_Dips = static_cast<float>(pageWidth) * 96.0f / dpiX;
    const float paperH_Dips = static_cast<float>(pageHeight) * 96.0f / dpiY;

    hr = printControl->AddPage(commandList.Get(), D2D1::SizeF(paperW_Dips, paperH_Dips), nullptr);
    if (SUCCEEDED(hr)) {
        hr = printControl->Close();
    }

    // 13. Cleanup frame data (RAII guard handles pdx)
    rawFrame.Release();

    return hr;
}

HRESULT PrintManager::CreatePrinterColorContext(
    ID2D1DeviceContext* d2dContext,
    HDC printerDC,
    const std::wstring& customPrinterIcc,
    ID2D1ColorContext** outColorContext
) {
    if (!outColorContext) return E_INVALIDARG;
    *outColorContext = nullptr;

    // Use custom user-defined proofing ICC profile if provided
    if (!customPrinterIcc.empty()) {
        const HRESULT hr = d2dContext->CreateColorContextFromFilename(customPrinterIcc.c_str(), outColorContext);
        if (SUCCEEDED(hr)) return hr;
    }

    // Automatically resolve system-associated ICC profile path from printer device capabilities
    if (printerDC) {
        DWORD dwLen = 0;
        GetICMProfileW(printerDC, &dwLen, nullptr);
        if (dwLen > 0) {
            std::wstring profilePath(dwLen, L'\0');
            if (GetICMProfileW(printerDC, &dwLen, &profilePath[0])) {
                profilePath.resize(dwLen - 1); // Truncate trailing null terminator
                const HRESULT hr = d2dContext->CreateColorContextFromFilename(profilePath.c_str(), outColorContext);
                if (SUCCEEDED(hr)) return hr;
            }
        }
    }

    return E_FAIL;
}

} // namespace QuickView
