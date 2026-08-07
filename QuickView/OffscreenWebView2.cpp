/*
 * QuickView - WebView2 SVG Fallback Detection & Offscreen Rasterizer
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

#include "pch.h"
#include "OffscreenWebView2.h"
#include "WuffsLoader.h"
#include "../third_party/webview2/WebView2.h"

#include <wrl/client.h>
#include <wrl/event.h>
#include <shlwapi.h>
#include <string>
#include <atomic>
#include <algorithm>

using namespace Microsoft::WRL;

namespace QuickView {

bool OffscreenWebView2::NeedsFallback(std::string_view svgContent) {
    // D2D native SVG lacks these features. Case-cover common spellings.
    // Keep this a pure scan — no allocations, safe on loader threads.
    auto has = [&](std::string_view tag) {
        return svgContent.find(tag) != std::string_view::npos;
    };
    return has("<foreignObject") || has("<foreignobject") ||
           has("<filter") || has("<Filter") ||
           has("<mask") || has("<Mask");
}

bool OffscreenWebView2::RenderSvgToRgba(
    std::string_view svgContent,
    float targetW,
    float targetH,
    std::vector<uint8_t>& outPixels,
    int& outW,
    int& outH)
{
    outPixels.clear();
    outW = 0;
    outH = 0;
    if (svgContent.empty()) return false;

    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool coInitedHere = (hrCo == S_OK || hrCo == S_FALSE);
    // RPC_E_CHANGED_MODE: already on MTA — WebView2 requires STA; abort.
    if (hrCo == RPC_E_CHANGED_MODE) {
        return false;
    }

    wchar_t tempPath[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempPath);
    const std::wstring userDataFolder = std::wstring(tempPath) + L"QuickView_WV2_Temp";

    // Real off-screen popup — HWND_MESSAGE / IsVisible(FALSE) suspends Chromium paint.
    HWND hWndOffscreen = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"STATIC", L"QV_Offscreen",
        WS_POPUP, -10000, -10000, 10, 10,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hWndOffscreen) {
        if (coInitedHere) CoUninitialize();
        return false;
    }

    const int renderW = (std::max)(1, static_cast<int>(targetW > 0.0f ? targetW : 1024.0f));
    const int renderH = (std::max)(1, static_cast<int>(targetH > 0.0f ? targetH : 1024.0f));

    std::atomic<bool> isDone{false};
    bool success = false;

    ComPtr<ICoreWebView2Controller> webViewController;
    ComPtr<ICoreWebView2> webView;

    const std::string htmlContent =
        "<!DOCTYPE html><html><head><style>"
        "html,body{margin:0;padding:0;width:100%;height:100%;overflow:hidden;"
        "background:transparent;display:flex;justify-content:center;align-items:center;}"
        "svg{max-width:100%;max-height:100%;}"
        "</style></head><body>" + std::string(svgContent) + "</body></html>";

    int reqLen = MultiByteToWideChar(CP_UTF8, 0, htmlContent.c_str(), -1, nullptr, 0);
    if (reqLen <= 0) {
        DestroyWindow(hWndOffscreen);
        if (coInitedHere) CoUninitialize();
        return false;
    }
    std::wstring wHtml(static_cast<size_t>(reqLen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, htmlContent.c_str(), -1, wHtml.data(), reqLen);
    // Drop the embedded null from MultiByteToWideChar(-1).
    if (!wHtml.empty() && wHtml.back() == L'\0') wHtml.pop_back();

    HRESULT hrEnv = CreateCoreWebView2EnvironmentWithOptions(
        nullptr,
        userDataFolder.c_str(),
        nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [&](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(hr) || !env) {
                    isDone = true;
                    return S_OK;
                }

                env->CreateCoreWebView2Controller(
                    hWndOffscreen,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [&](HRESULT hrController, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(hrController) || !controller) {
                                isDone = true;
                                return S_OK;
                            }

                            webViewController = controller;
                            webViewController->get_CoreWebView2(&webView);
                            if (!webView) {
                                isDone = true;
                                return S_OK;
                            }

                            RECT bounds = { 0, 0, renderW, renderH };
                            webViewController->put_Bounds(bounds);
                            webViewController->put_IsVisible(TRUE);

                            ComPtr<ICoreWebView2Controller2> c2;
                            if (SUCCEEDED(webViewController.As(&c2))) {
                                COREWEBVIEW2_COLOR color = { 0, 0, 0, 0 };
                                c2->put_DefaultBackgroundColor(color);
                            }

                            EventRegistrationToken tokenNav{};
                            webView->add_NavigationCompleted(
                                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                    [&](ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                                        BOOL navOk = FALSE;
                                        if (args) args->get_IsSuccess(&navOk);
                                        if (!navOk) {
                                            isDone = true;
                                            return S_OK;
                                        }

                                        ComPtr<IStream> imgStream;
                                        if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &imgStream)) || !imgStream) {
                                            isDone = true;
                                            return S_OK;
                                        }

                                        sender->CapturePreview(
                                            COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG,
                                            imgStream.Get(),
                                            Callback<ICoreWebView2CapturePreviewCompletedHandler>(
                                                [&, imgStream](HRESULT hrCap) -> HRESULT {
                                                    if (SUCCEEDED(hrCap)) {
                                                        STATSTG stat = {};
                                                        imgStream->Stat(&stat, STATFLAG_NONAME);
                                                        if (stat.cbSize.LowPart > 0) {
                                                            std::vector<uint8_t> pngData(stat.cbSize.LowPart);
                                                            LARGE_INTEGER zero{};
                                                            imgStream->Seek(zero, STREAM_SEEK_SET, nullptr);
                                                            ULONG bytesRead = 0;
                                                            imgStream->Read(pngData.data(), stat.cbSize.LowPart, &bytesRead);

                                                            uint32_t dw = 0, dh = 0;
                                                            if (WuffsLoader::DecodePNG(pngData.data(), pngData.size(), &dw, &dh, outPixels)) {
                                                                outW = static_cast<int>(dw);
                                                                outH = static_cast<int>(dh);
                                                                // Wuffs PNG path yields BGRA; pipeline expects RGBA here.
                                                                for (size_t i = 0; i + 3 < outPixels.size(); i += 4) {
                                                                    std::swap(outPixels[i], outPixels[i + 2]);
                                                                }
                                                                success = true;
                                                            }
                                                        }
                                                    }
                                                    isDone = true;
                                                    return S_OK;
                                                }).Get());
                                        return S_OK;
                                    }).Get(), &tokenNav);

                            webView->NavigateToString(wHtml.c_str());
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());

    if (FAILED(hrEnv)) {
        DestroyWindow(hWndOffscreen);
        if (coInitedHere) CoUninitialize();
        return false;
    }

    const DWORD startTicks = GetTickCount();
    MSG msg = {};
    while (!isDone.load() && (GetTickCount() - startTicks < 5000)) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(5);
    }

    if (webViewController) {
        webViewController->Close();
        webViewController.Reset();
    }
    webView.Reset();
    DestroyWindow(hWndOffscreen);
    if (coInitedHere) CoUninitialize();

    return success;
}

} // namespace QuickView
