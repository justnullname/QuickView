#include "OffscreenWebView2.h"
#include "WuffsLoader.h"
#include "../third_party/webview2/WebView2.h"

#include <windows.h>
#include <wrl/client.h>
#include <wrl/event.h>
#include <shlwapi.h>
#include <string>
#include <atomic>

using namespace Microsoft::WRL;

namespace QuickView {

bool OffscreenWebView2::NeedsFallback(std::string_view svgContent) {
    if (svgContent.find("<foreignObject") != std::string_view::npos ||
        svgContent.find("<foreignobject") != std::string_view::npos ||
        svgContent.find("<filter") != std::string_view::npos ||
        svgContent.find("<mask") != std::string_view::npos) {
        return true;
    }
    return false;
}

bool OffscreenWebView2::RenderSvgToRgba(
    std::string_view svgContent,
    float targetW,
    float targetH,
    std::vector<uint8_t>& outPixels,
    int& outW,
    int& outH) 
{
    // CoInitialize for STA thread as WebView2 requires STA
    HRESULT hrCo = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    // Prepare temp user data folder in local appdata or temp
    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    std::wstring userDataFolder = std::wstring(tempPath) + L"QuickView_WV2_Temp";

    // Message-only window for hosting offscreen WebView2
    // CRITICAL: We must use a real WS_POPUP window off-screen instead of HWND_MESSAGE.
    // If we use HWND_MESSAGE or set IsVisible(FALSE), the Chromium render pipeline suspends and CapturePreview hangs or captures nothing!
    HWND hWndOffscreen = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, 
        L"STATIC", L"QV_Offscreen", 
        WS_POPUP, -10000, -10000, 10, 10, 
        nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    if (!hWndOffscreen) {
        if (SUCCEEDED(hrCo)) CoUninitialize();
        return false;
    }

    int renderW = (int)(targetW > 0 ? targetW : 1024);
    int renderH = (int)(targetH > 0 ? targetH : 1024);

    std::atomic<bool> isDone = false;
    bool success = false;

    ComPtr<ICoreWebView2Controller> webViewController;
    ComPtr<ICoreWebView2> webView;

    // Html wrapper for rendering SVG crisp & centered
    std::string htmlContent = 
        "<!DOCTYPE html><html><head><style>"
        "html, body { margin:0; padding:0; width:100%; height:100%; overflow:hidden; background-color:#ffffff; display:flex; justify-content:center; align-items:center; }"
        "svg { max-width:100%; max-height:100%; }"
        "</style></head><body>" + std::string(svgContent) + "</body></html>";

    // Convert string to wchar for NavigateToString
    int reqLen = MultiByteToWideChar(CP_UTF8, 0, htmlContent.c_str(), -1, nullptr, 0);
    std::wstring wHtml(reqLen, 0);
    MultiByteToWideChar(CP_UTF8, 0, htmlContent.c_str(), -1, &wHtml[0], reqLen);

    // Call CreateCoreWebView2EnvironmentWithOptions directly (Static Linking)
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

                            // Set size & bounds
                            RECT bounds = {0, 0, renderW, renderH};
                            webViewController->put_Bounds(bounds);
                            // CRITICAL: Must be TRUE, otherwise rendering is suspended!
                            webViewController->put_IsVisible(TRUE);

                            // Add NavigationCompleted handler
                            EventRegistrationToken tokenNav;
                            webView->add_NavigationCompleted(
                                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                    [&](ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                                        (void)args;
                                        // Capture preview as PNG to stream
                                        ComPtr<IStream> imgStream;
                                        CreateStreamOnHGlobal(nullptr, TRUE, &imgStream);

                                        sender->CapturePreview(
                                            COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG,
                                            imgStream.Get(),
                                            Callback<ICoreWebView2CapturePreviewCompletedHandler>(
                                                [&, imgStream](HRESULT hrCap) -> HRESULT {
                                                    if (SUCCEEDED(hrCap)) {
                                                        // Read stream into buffer
                                                        STATSTG stat;
                                                        imgStream->Stat(&stat, STATFLAG_NONAME);
                                                        ULONG bytesRead = 0;
                                                        std::vector<uint8_t> pngData(stat.cbSize.LowPart);
                                                        LARGE_INTEGER zero;
                                                        zero.QuadPart = 0;
                                                        imgStream->Seek(zero, STREAM_SEEK_SET, nullptr);
                                                        imgStream->Read(pngData.data(), stat.cbSize.LowPart, &bytesRead);
                                                        
                                                        uint32_t dw = 0, dh = 0;
                                                        if (WuffsLoader::DecodePNG(pngData.data(), pngData.size(), &dw, &dh, outPixels)) {
                                                            outW = (int)dw;
                                                            outH = (int)dh;
                                                            
                                                            // Wuffs outputs BGRA. Convert to RGBA for consistency with fallback pipeline
                                                            for (size_t i = 0; i < outPixels.size(); i += 4) {
                                                                std::swap(outPixels[i], outPixels[i+2]);
                                                            }
                                                            success = true;
                                                        }
                                                    }
                                                    isDone = true;
                                                    return S_OK;
                                                }).Get());
                                        return S_OK;
                                    }).Get(), &tokenNav);

                            // Navigate to string
                            webView->NavigateToString(wHtml.c_str());
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());

    if (FAILED(hrEnv)) {
        DestroyWindow(hWndOffscreen);
        if (SUCCEEDED(hrCo)) CoUninitialize();
        return false;
    }

    // Pump Win32 messages while waiting for offscreen render completion (timeout 5 sec max)
    DWORD startTicks = GetTickCount();
    MSG msg;
    while (!isDone && (GetTickCount() - startTicks < 5000)) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(5);
    }
    
    // CRITICAL: Immediately close controller and release all COM objects to guarantee ZERO background memory footprint!
    if (webViewController) {
        webViewController->Close();
        webViewController = nullptr;
    }
    webView = nullptr;

    DestroyWindow(hWndOffscreen);
    if (SUCCEEDED(hrCo)) CoUninitialize();

    return success;
}

} // namespace QuickView
