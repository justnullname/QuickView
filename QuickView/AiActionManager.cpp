#include "pch.h"
#include "AiActionManager.h"
#include "yyjson.h"
#include <wincrypt.h>
#include <winhttp.h>
#include <wincodec.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <thread>
#include <cmath>
#include <algorithm>
#include <limits>
#include "EditState.h"
#include "OSDState.h"
#include "AppStrings.h"

#include "StringUtils.h"
#include "AsyncJob.h"
#include "SystemInfo.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shlwapi.lib")

extern OSDState g_osd;
extern std::wstring GetCurrentActiveImagePath();

namespace QuickView::AI {

using Microsoft::WRL::ComPtr;

static VisualFrameSnapshotFn s_visualSnapshotFn = nullptr;
static VisualBgraProviderFn s_visualRenderFn = nullptr;

struct ScopedComInit {
    HRESULT hr = E_FAIL;
    ScopedComInit() { hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
    ~ScopedComInit() {
        if (SUCCEEDED(hr)) {
            CoUninitialize();
        }
    }
    ScopedComInit(const ScopedComInit&) = delete;
    ScopedComInit& operator=(const ScopedComInit&) = delete;
};

struct ScopedWinHttpHandle {
    HINTERNET h = nullptr;
    ScopedWinHttpHandle() = default;
    explicit ScopedWinHttpHandle(HINTERNET handle) : h(handle) {}
    ~ScopedWinHttpHandle() { Reset(); }
    void Reset(HINTERNET newHandle = nullptr) {
        if (h) WinHttpCloseHandle(h);
        h = newHandle;
    }
    HINTERNET Release() {
        HINTERNET old = h;
        h = nullptr;
        return old;
    }
    explicit operator bool() const noexcept { return h != nullptr; }
    ScopedWinHttpHandle(const ScopedWinHttpHandle&) = delete;
    ScopedWinHttpHandle& operator=(const ScopedWinHttpHandle&) = delete;
};

static bool ReadJsonStr(yyjson_val* obj, const char* key, std::string& out) {
    if (!obj || !yyjson_is_obj(obj) || !key) return false;
    yyjson_val* v = yyjson_obj_get(obj, key);
    if (v && yyjson_is_str(v)) {
        const char* s = yyjson_get_str(v);
        if (s) {
            out = s;
            return true;
        }
    }
    return false;
}

static const char* SelectClosestGeminiAspectRatio(OutputAspectRatio ratio, uint32_t w, uint32_t h) {
    switch (ratio) {
        case OutputAspectRatio::Landscape_16_9: return "16:9";
        case OutputAspectRatio::Portrait_9_16:  return "9:16";
        case OutputAspectRatio::Standard_4_3:   return "4:3";
        case OutputAspectRatio::Vertical_3_4:   return "3:4";
        case OutputAspectRatio::Square_1_1:     return "1:1";
        case OutputAspectRatio::Auto:
        default: {
            if (w == 0 || h == 0) return "1:1";
            struct AspectChoice { const char* label; float ratio; };
            static constexpr AspectChoice kChoices[] = {
                { "21:9", 21.0f / 9.0f },
                { "16:9", 16.0f / 9.0f },
                { "3:2",  3.0f / 2.0f },
                { "4:3",  4.0f / 3.0f },
                { "5:4",  5.0f / 4.0f },
                { "1:1",  1.0f },
                { "4:5",  4.0f / 5.0f },
                { "3:4",  3.0f / 4.0f },
                { "2:3",  2.0f / 3.0f },
                { "9:16", 9.0f / 16.0f },
            };
            const float target = static_cast<float>(w) / static_cast<float>(h);
            const char* bestLabel = "1:1";
            float bestDiff = (std::numeric_limits<float>::max)();
            for (const auto& c : kChoices) {
                const float diff = std::abs(std::log(target / c.ratio));
                if (diff < bestDiff) {
                    bestDiff = diff;
                    bestLabel = c.label;
                }
            }
            return bestLabel;
        }
    }
}

static std::string BuildOpenAiMultipartEditPayload(
    const std::string& boundary,
    const std::string& targetModel,
    const std::string& utf8Prompt,
    const std::vector<uint8_t>& imagePngBytes,
    const std::vector<uint8_t>* maskPngBytes)
{
    std::string body;
    auto addFormField = [&](const std::string& name, const std::string& value) {
        body += "--" + boundary + "\r\n";
        body += "Content-Disposition: form-data; name=\"" + name + "\"\r\n\r\n";
        body += value + "\r\n";
    };
    auto addFileField = [&](const std::string& name, const std::string& filename, const std::vector<uint8_t>& fileBytes) {
        body += "--" + boundary + "\r\n";
        body += "Content-Disposition: form-data; name=\"" + name + "\"; filename=\"" + filename + "\"\r\n";
        body += "Content-Type: image/png\r\n\r\n";
        body.append(reinterpret_cast<const char*>(fileBytes.data()), fileBytes.size());
        body += "\r\n";
    };

    addFormField("model", targetModel);
    addFormField("prompt", utf8Prompt);
    addFormField("response_format", "b64_json");
    if (!imagePngBytes.empty()) {
        addFileField("image", "image.png", imagePngBytes);
    }
    if (maskPngBytes && !maskPngBytes->empty()) {
        addFileField("mask", "mask.png", *maskPngBytes);
    }
    body += "--" + boundary + "--\r\n";
    return body;
}

struct HttpExchangeResult {
    bool ok = false;
    bool interrupted = false;
    DWORD statusCode = 0;
    std::string body;
    std::wstring mainTitle;
    std::wstring detailMessage;
    std::wstring actionAdvice;
    std::wstring errorMessage;
};

static HttpExchangeResult SendRequestAndReadResponse(
    HINTERNET hRequest,
    const std::wstring& headers,
    const void* payloadData,
    DWORD payloadLen,
    const wchar_t* hostName,
    INTERNET_PORT port,
    bool isLocal,
    uint64_t expectedTaskId,
    const std::atomic<uint64_t>& currentTaskId)
{
    HttpExchangeResult res;
    if (!hRequest || expectedTaskId != currentTaskId.load()) {
        res.interrupted = true;
        return res;
    }

    BOOL bSend = WinHttpSendRequest(hRequest, headers.c_str(), static_cast<DWORD>(headers.size()),
                                    const_cast<void*>(payloadData), payloadLen, payloadLen, 0);

    if (!bSend || !WinHttpReceiveResponse(hRequest, nullptr)) {
        if (expectedTaskId != currentTaskId.load()) {
            res.interrupted = true;
            return res;
        }
        DWORD dwErr = GetLastError();
        if (dwErr == ERROR_WINHTTP_OPERATION_CANCELLED) {
            res.interrupted = true;
            return res;
        }
        if (dwErr == ERROR_WINHTTP_CANNOT_CONNECT) {
            res.mainTitle = L"连接失败 (12029)";
            wchar_t buf[256];
            swprintf_s(buf, L"无法连接到目标服务 [%s:%d]，连接被拒绝。", hostName ? hostName : L"", port);
            res.detailMessage = buf;
            if (isLocal) {
                res.actionAdvice = L"提示：检测到当前使用的是本地模型服务 (127.0.0.1)，请确认本地 SD WebUI / Forge / ComfyUI 是否已启动并开启 API；或者在设置中切换为在线云端模型 (如 Google Gemini)。";
            } else {
                res.actionAdvice = L"提示：请检查服务器地址、网络连接或代理设置。";
            }
        } else if (dwErr == ERROR_WINHTTP_TIMEOUT) {
            res.mainTitle = L"请求超时 (12002)";
            res.detailMessage = L"服务器未在配置的超时时间内响应。";
            res.actionAdvice = L"提示：可尝试在模型配置中增加超时时间，或框选稍小的区域/降低步数重试。";
        } else if (dwErr == ERROR_WINHTTP_NAME_NOT_RESOLVED) {
            res.mainTitle = L"DNS 解析失败 (12007)";
            res.detailMessage = L"无法解析 Base URL 中的主机名。";
            res.actionAdvice = L"提示：请检查网络连接、DNS 或系统代理配置。";
        } else {
            wchar_t buf[128];
            swprintf_s(buf, L"网络传输错误 (%lu)", dwErr);
            res.mainTitle = buf;
            wchar_t detail[256];
            swprintf_s(detail, L"与目标服务器 [%s:%d] 通信时发生底层 WinHTTP 传输错误。", hostName ? hostName : L"", port);
            res.detailMessage = detail;
            res.actionAdvice = L"提示：请检查网络连接、VPN 或防火墙状态。";
        }
        res.errorMessage = res.mainTitle + L"\n" + res.detailMessage + L"\n" + res.actionAdvice;
        return res;
    }

    DWORD statusCode = 0;
    DWORD dwSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &dwSize, WINHTTP_NO_HEADER_INDEX);
    res.statusCode = statusCode;

    DWORD bytesAvailable = 0;
    while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0) {
        if (expectedTaskId != currentTaskId.load()) {
            res.interrupted = true;
            return res;
        }
        std::vector<char> buf(bytesAvailable);
        DWORD bytesRead = 0;
        if (!WinHttpReadData(hRequest, buf.data(), bytesAvailable, &bytesRead)) {
            if (expectedTaskId != currentTaskId.load()) {
                res.interrupted = true;
                return res;
            }
            break;
        }
        if (bytesRead > 0) {
            res.body.append(buf.data(), bytesRead);
        } else {
            break;
        }
    }
    if (expectedTaskId != currentTaskId.load()) {
        res.interrupted = true;
        return res;
    }

    if (statusCode != 200) {
        AiActionManager::ExtractSemanticError(statusCode, res.body, res.mainTitle, res.detailMessage, res.actionAdvice);
        res.errorMessage = AiActionManager::FormatAiErrorMessage(statusCode, res.body);
        return res;
    }

    res.ok = true;
    return res;
}

static std::string BinaryToBase64(const uint8_t* data, size_t len) {
    if (!data || len == 0) return "";
    DWORD b64Len = 0;
    CryptBinaryToStringA(data, static_cast<DWORD>(len), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &b64Len);
    if (b64Len == 0) return "";
    std::string b64(b64Len, '\0');
    CryptBinaryToStringA(data, static_cast<DWORD>(len), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, b64.data(), &b64Len);
    while (!b64.empty() && (b64.back() == '\0' || b64.back() == '\r' || b64.back() == '\n')) {
        b64.pop_back();
    }
    return b64;
}

static bool DownloadImageFromUrl(const std::wstring& wUrl, std::vector<uint8_t>& outBytes) {
    if (wUrl.empty()) return false;
    URL_COMPONENTS urlComp{};
    urlComp.dwStructSize = sizeof(urlComp);
    urlComp.dwSchemeLength = static_cast<DWORD>(-1);
    urlComp.dwHostNameLength = static_cast<DWORD>(-1);
    urlComp.dwUrlPathLength = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(wUrl.c_str(), static_cast<DWORD>(wUrl.size()), 0, &urlComp)) return false;

    std::wstring host(urlComp.lpszHostName, urlComp.dwHostNameLength);
    std::wstring path(urlComp.lpszUrlPath, urlComp.dwUrlPathLength);

    ScopedWinHttpHandle hSession(WinHttpOpen(L"QuickView-AI-Download/2.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!hSession) return false;

    ScopedWinHttpHandle hConnect(WinHttpConnect(hSession.h, host.c_str(), urlComp.nPort, 0));
    if (!hConnect) return false;

    DWORD flags = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    ScopedWinHttpHandle hReq(WinHttpOpenRequest(hConnect.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!hReq) return false;

    WinHttpSetTimeouts(hReq.h, 5000, 5000, 30000, 30000);

    bool ok = false;
    if (WinHttpSendRequest(hReq.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hReq.h, nullptr)) {
        DWORD statusCode = 0;
        DWORD dwSize = sizeof(statusCode);
        WinHttpQueryHeaders(hReq.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &dwSize, WINHTTP_NO_HEADER_INDEX);
        if (statusCode == 200) {
            DWORD bytesAvailable = 0;
            while (WinHttpQueryDataAvailable(hReq.h, &bytesAvailable) && bytesAvailable > 0) {
                size_t curSize = outBytes.size();
                outBytes.resize(curSize + bytesAvailable);
                DWORD bytesRead = 0;
                if (WinHttpReadData(hReq.h, outBytes.data() + curSize, bytesAvailable, &bytesRead) && bytesRead > 0) {
                    outBytes.resize(curSize + bytesRead);
                } else {
                    outBytes.resize(curSize);
                    break;
                }
            }
            ok = !outBytes.empty();
        }
    }

    return ok;
}

static bool Base64ToBinary(const char* b64, size_t b64Len, std::vector<uint8_t>& outBytes) {
    if (!b64 || b64Len == 0) return false;
    const char* comma = static_cast<const char*>(memchr(b64, ',', b64Len));
    if (comma) {
        size_t skip = static_cast<size_t>((comma + 1) - b64);
        b64 += skip;
        b64Len -= skip;
    }
    if (b64Len == 0) return false;
    DWORD binLen = 0;
    if (!CryptStringToBinaryA(b64, static_cast<DWORD>(b64Len), CRYPT_STRING_BASE64, nullptr, &binLen, nullptr, nullptr) || binLen == 0) {
        return false;
    }
    outBytes.resize(binLen);
    if (!CryptStringToBinaryA(b64, static_cast<DWORD>(b64Len), CRYPT_STRING_BASE64, outBytes.data(), &binLen, nullptr, nullptr)) {
        outBytes.clear();
        return false;
    }
    outBytes.resize(binLen);
    return !outBytes.empty();
}

static bool CrackEndpointUrl(
    const std::string& baseUrl,
    wchar_t hostName[256],
    INTERNET_PORT& outPort,
    bool& outIsHttps,
    std::wstring& outPath,
    std::wstring& outError)
{
    std::wstring wUrl = Utf8ToWide(baseUrl);
    URL_COMPONENTS urlComp{};
    urlComp.dwStructSize = sizeof(urlComp);
    wchar_t localPath[1024] = { 0 };
    urlComp.lpszHostName = hostName;
    urlComp.dwHostNameLength = 256;
    urlComp.lpszUrlPath = localPath;
    urlComp.dwUrlPathLength = 1024;

    if (!WinHttpCrackUrl(wUrl.c_str(), static_cast<DWORD>(wUrl.size()), 0, &urlComp)) {
        outError = AppStrings::g.AiError_InvalidUrl;
        return false;
    }
    outPort = urlComp.nPort;
    outIsHttps = (urlComp.nScheme == INTERNET_SCHEME_HTTPS);
    outPath = localPath;
    if (outPath.empty() || outPath.back() != L'/') outPath += L'/';
    return true;
}

static void ResolveGeminiPath(std::wstring& fullPath, const std::string& targetModel) {
    size_t openaiPos = fullPath.find(L"openai");
    if (openaiPos != std::wstring::npos) fullPath = fullPath.substr(0, openaiPos);
    size_t pos = fullPath.find(L"models");
    if (pos != std::wstring::npos) fullPath = fullPath.substr(0, pos);
    if (fullPath.empty() || fullPath.back() != L'/') fullPath += L'/';
    if (fullPath == L"/") fullPath = L"/v1beta/";

    fullPath += L"models/" + Utf8ToWide(targetModel);
    if (targetModel.rfind("imagen-", 0) == 0) {
        fullPath += L":predict";
    } else {
        fullPath += L":generateContent";
    }
}

static void ResolveSdPath(std::wstring& fullPath, bool isImg2Img) {
    size_t pos = fullPath.find(L"sdapi");
    if (pos != std::wstring::npos) fullPath = fullPath.substr(0, pos);
    if (fullPath.empty() || fullPath.back() != L'/') fullPath += L'/';
    fullPath += isImg2Img ? L"sdapi/v1/img2img" : L"sdapi/v1/txt2img";
}

static bool ExtractImageFromYyjson(yyjson_val* root, std::vector<uint8_t>& outImgBytes) {
    if (!root) return false;

    // 1. Try Gemini Native candidates[0].content.parts[*].inlineData.data
    yyjson_val* candidates = yyjson_obj_get(root, "candidates");
    if (candidates && yyjson_is_arr(candidates) && yyjson_arr_size(candidates) > 0) {
        yyjson_val* firstCand = yyjson_arr_get(candidates, 0);
        yyjson_val* candContent = yyjson_obj_get(firstCand, "content");
        if (candContent) {
            yyjson_val* parts = yyjson_obj_get(candContent, "parts");
            if (parts && yyjson_is_arr(parts)) {
                size_t partCount = yyjson_arr_size(parts);
                for (size_t pIdx = 0; pIdx < partCount; ++pIdx) {
                    yyjson_val* part = yyjson_arr_get(parts, pIdx);
                    yyjson_val* inData = yyjson_obj_get(part, "inlineData");
                    if (!inData) inData = yyjson_obj_get(part, "inline_data");
                    if (inData) {
                        yyjson_val* b64Val = yyjson_obj_get(inData, "data");
                        if (b64Val && yyjson_is_str(b64Val)) {
                            const char* s = yyjson_get_str(b64Val);
                            if (Base64ToBinary(s, strlen(s), outImgBytes)) return true;
                        }
                    }
                }
            }
        }
    }

    // 2. Try Google Imagen 3 predict structure: predictions[0].bytesBase64Encoded
    yyjson_val* vPreds = yyjson_obj_get(root, "predictions");
    if (vPreds && yyjson_is_arr(vPreds) && yyjson_arr_size(vPreds) > 0) {
        yyjson_val* firstPred = yyjson_arr_get(vPreds, 0);
        yyjson_val* b64Val = yyjson_obj_get(firstPred, "bytesBase64Encoded");
        if (b64Val && yyjson_is_str(b64Val)) {
            const char* s = yyjson_get_str(b64Val);
            if (Base64ToBinary(s, strlen(s), outImgBytes)) return true;
        }
    }

    // 3. Try SD WebUI / Forge structure: images[0] (base64 string or obj with url)
    yyjson_val* imagesArr = yyjson_obj_get(root, "images");
    if (imagesArr && yyjson_is_arr(imagesArr) && yyjson_arr_size(imagesArr) > 0) {
        yyjson_val* firstImg = yyjson_arr_get(imagesArr, 0);
        if (firstImg) {
            if (yyjson_is_str(firstImg)) {
                const char* s = yyjson_get_str(firstImg);
                if (Base64ToBinary(s, strlen(s), outImgBytes)) return true;
            } else if (yyjson_is_obj(firstImg)) {
                yyjson_val* vUrl = yyjson_obj_get(firstImg, "url");
                if (vUrl && yyjson_is_str(vUrl)) {
                    std::wstring imgUrl = Utf8ToWide(yyjson_get_str(vUrl));
                    if (DownloadImageFromUrl(imgUrl, outImgBytes)) return true;
                }
            }
        }
    }

    // 4. Try OpenAI / SiliconFlow structure: data[0].b64_json or url
    yyjson_val* dataArr = yyjson_obj_get(root, "data");
    if (dataArr && yyjson_is_arr(dataArr) && yyjson_arr_size(dataArr) > 0) {
        yyjson_val* firstObj = yyjson_arr_get(dataArr, 0);
        if (firstObj && yyjson_is_obj(firstObj)) {
            yyjson_val* b64Val = yyjson_obj_get(firstObj, "b64_json");
            if (b64Val && yyjson_is_str(b64Val)) {
                const char* s = yyjson_get_str(b64Val);
                if (Base64ToBinary(s, strlen(s), outImgBytes)) return true;
            } else {
                yyjson_val* vUrl = yyjson_obj_get(firstObj, "url");
                if (vUrl && yyjson_is_str(vUrl)) {
                    std::wstring imgUrl = Utf8ToWide(yyjson_get_str(vUrl));
                    if (DownloadImageFromUrl(imgUrl, outImgBytes)) return true;
                }
            }
        }
    }

    return false;
}

static bool EncodePngBytesToBase64OrBytes(
    const std::vector<uint8_t>& pngBytes,
    std::vector<uint8_t>* outPngBytes,
    std::string& outBase64)
{
    if (pngBytes.empty()) return false;
    outBase64 = BinaryToBase64(pngBytes.data(), pngBytes.size());
    if (outPngBytes) {
        *outPngBytes = pngBytes;
    }
    return !outBase64.empty();
}

static bool LoadVisualImageAsPng(
    const VisualFrameSnapshot& snapshot,
    MaxResolution maxRes,
    std::string& outBase64,
    std::string& outMimeType,
    std::vector<uint8_t>* outPngBytes,
    uint32_t& outWidth,
    uint32_t& outHeight)
{
    outWidth = 0;
    outHeight = 0;
    outBase64.clear();
    if (outPngBytes) outPngBytes->clear();

    const uint32_t limit = (maxRes == MaxResolution::NoLimit) ? 0 : static_cast<uint32_t>(maxRes);
    const std::wstring& filePath = snapshot.imagePath;
    const bool hasVisualEdits = snapshot.effectiveExif != 1 || snapshot.netRotation != 0 ||
                                snapshot.netFlipH || snapshot.flipV || snapshot.hasCrop ||
                                !snapshot.memoryBgra.empty();

    std::wstring ext;
    if (!filePath.empty()) {
        ext = PathFindExtensionW(filePath.c_str());
        for (auto& c : ext) c = towlower(c);
    }

    const bool canPassThroughRawFile = !hasVisualEdits && !outPngBytes && !filePath.empty() &&
        PathFileExistsW(filePath.c_str()) &&
        (ext == L".jpg" || ext == L".jpeg" || ext == L".png" || ext == L".webp") &&
        (limit == 0 || limit >= 2048);

    if (canPassThroughRawFile) {
        HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER fSize{};
            if (GetFileSizeEx(hFile, &fSize) && fSize.QuadPart > 0 && fSize.QuadPart <= 4 * 1024 * 1024) {
                std::vector<uint8_t> rawBuf(static_cast<size_t>(fSize.QuadPart));
                DWORD bytesRead = 0;
                if (ReadFile(hFile, rawBuf.data(), static_cast<DWORD>(fSize.QuadPart), &bytesRead, nullptr) && bytesRead == fSize.QuadPart) {
                    CloseHandle(hFile);
                    outBase64 = BinaryToBase64(rawBuf.data(), rawBuf.size());
                    if (ext == L".jpg" || ext == L".jpeg") outMimeType = "image/jpeg";
                    else if (ext == L".webp") outMimeType = "image/webp";
                    else outMimeType = "image/png";

                    ComPtr<IWICImagingFactory> pFactory;
                    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFactory)))) {
                        ComPtr<IWICBitmapDecoder> pDec;
                        if (SUCCEEDED(pFactory->CreateDecoderFromFilename(filePath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &pDec))) {
                            ComPtr<IWICBitmapFrameDecode> pFr;
                            if (SUCCEEDED(pDec->GetFrame(0, &pFr))) {
                                UINT w = 0, h = 0;
                                if (SUCCEEDED(pFr->GetSize(&w, &h))) {
                                    outWidth = w;
                                    outHeight = h;
                                }
                            }
                        }
                    }
                    if (!outBase64.empty()) return true;
                }
            }
            CloseHandle(hFile);
        }
    }

    // Prefer unified visual renderer when registered (handles RAW/AVIF/JXL/HEIC/SVG/EXIF/crops/prior edits)
    if (s_visualRenderFn) {
        std::vector<uint8_t> bgra;
        uint32_t w = 0, h = 0, stride = 0;
        if (s_visualRenderFn(snapshot, bgra, w, h, stride) && !bgra.empty() && w > 0 && h > 0) {
            outWidth = w;
            outHeight = h;

            uint32_t targetW = w;
            uint32_t targetH = h;
            if (limit > 0 && (w > limit || h > limit)) {
                if (w >= h) {
                    targetW = limit;
                    targetH = (std::max)(1u, static_cast<uint32_t>(std::lround(static_cast<double>(h) * limit / w)));
                } else {
                    targetH = limit;
                    targetW = (std::max)(1u, static_cast<uint32_t>(std::lround(static_cast<double>(w) * limit / h)));
                }
            }

            std::vector<uint8_t> pngBytes;
            if (targetW != w || targetH != h) {
                std::vector<uint8_t> scaled(static_cast<size_t>(targetW) * targetH * 4);
                AiActionManager::ResampleBgraExact(bgra.data(), static_cast<int>(w), static_cast<int>(h), static_cast<int>(stride),
                                                 scaled.data(), static_cast<int>(targetW), static_cast<int>(targetH), static_cast<int>(targetW * 4));
                if (!AiActionManager::EncodeToPngMemory(scaled.data(), static_cast<int>(targetW), static_cast<int>(targetH), static_cast<int>(targetW * 4), pngBytes)) {
                    return false;
                }
            } else {
                if (!AiActionManager::EncodeToPngMemory(bgra.data(), static_cast<int>(w), static_cast<int>(h), static_cast<int>(stride), pngBytes)) {
                    return false;
                }
            }
            outMimeType = "image/png";
            return EncodePngBytesToBase64OrBytes(pngBytes, outPngBytes, outBase64);
        }
    }

    if (filePath.empty() || !PathFileExistsW(filePath.c_str())) return false;

    // WIC fallback for standalone tests
    ComPtr<IWICImagingFactory> pFactory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFactory)))) {
        return false;
    }

    ComPtr<IWICBitmapDecoder> pDecoder;
    if (FAILED(pFactory->CreateDecoderFromFilename(filePath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &pDecoder))) {
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> pFrame;
    if (FAILED(pDecoder->GetFrame(0, &pFrame))) {
        return false;
    }

    UINT origW = 0, origH = 0;
    if (FAILED(pFrame->GetSize(&origW, &origH)) || origW == 0 || origH == 0) {
        return false;
    }
    outWidth = origW;
    outHeight = origH;

    UINT targetW = origW;
    UINT targetH = origH;
    if (limit > 0 && (origW > limit || origH > limit)) {
        if (origW >= origH) {
            targetW = limit;
            targetH = (std::max)(1u, static_cast<UINT>(std::lround(static_cast<double>(origH) * limit / origW)));
        } else {
            targetH = limit;
            targetW = (std::max)(1u, static_cast<UINT>(std::lround(static_cast<double>(origW) * limit / origH)));
        }
    }

    IWICBitmapSource* pSource = pFrame.Get();
    ComPtr<IWICBitmapScaler> pScaler;
    if (targetW != origW || targetH != origH) {
        if (SUCCEEDED(pFactory->CreateBitmapScaler(&pScaler)) &&
            SUCCEEDED(pScaler->Initialize(pFrame.Get(), targetW, targetH, WICBitmapInterpolationModeHighQualityCubic))) {
            pSource = pScaler.Get();
        }
    }

    ComPtr<IStream> pStream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &pStream))) return false;
    ComPtr<IWICBitmapEncoder> pEncoder;
    if (FAILED(pFactory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &pEncoder))) return false;
    if (FAILED(pEncoder->Initialize(pStream.Get(), WICBitmapEncoderNoCache))) return false;
    ComPtr<IWICBitmapFrameEncode> pDstFrame;
    if (FAILED(pEncoder->CreateNewFrame(&pDstFrame, nullptr))) return false;
    if (FAILED(pDstFrame->Initialize(nullptr)) ||
        FAILED(pDstFrame->SetSize(targetW, targetH))) return false;
    WICPixelFormatGUID pixelFormat = GUID_WICPixelFormatDontCare;
    if (FAILED(pDstFrame->SetPixelFormat(&pixelFormat)) ||
        FAILED(pDstFrame->WriteSource(pSource, nullptr)) ||
        FAILED(pDstFrame->Commit()) ||
        FAILED(pEncoder->Commit())) {
        return false;
    }

    STATSTG stat{};
    if (FAILED(pStream->Stat(&stat, STATFLAG_NONAME)) || stat.cbSize.QuadPart == 0 || stat.cbSize.QuadPart > UINT_MAX) {
        return false;
    }
    LARGE_INTEGER liZero{};
    if (FAILED(pStream->Seek(liZero, STREAM_SEEK_SET, nullptr))) return false;
    std::vector<uint8_t> pngBytes(static_cast<size_t>(stat.cbSize.QuadPart));
    ULONG read = 0;
    if (FAILED(pStream->Read(pngBytes.data(), static_cast<ULONG>(pngBytes.size()), &read)) || read != pngBytes.size()) {
        return false;
    }
    outMimeType = "image/png";
    return EncodePngBytesToBase64OrBytes(pngBytes, outPngBytes, outBase64);
}

void AiActionManager::ExtractSemanticError(
    DWORD statusCode, std::string_view responseBody,
    std::wstring& outTitle, std::wstring& outDetail, std::wstring& outAdvice) {

    // 1. Standardized English Status Code Titles & Actionable Advice
    switch (statusCode) {
    case 400:
        outTitle = L"Bad Request (HTTP 400)";
        outAdvice = L"Hint: Check request parameters, prompt syntax, or image dimensions.";
        break;
    case 401:
        outTitle = L"Invalid or Expired API Key (HTTP 401)";
        outAdvice = L"Hint: Check and update your API key in Model Profiles settings.";
        break;
    case 403:
        outTitle = L"Access Denied / Forbidden (HTTP 403)";
        outAdvice = L"Hint: Your account or key lacks permission to call this model.";
        break;
    case 404:
        outTitle = L"Model or Endpoint Not Found (HTTP 404)";
        outAdvice = L"Hint: Verify endpoint Base URL, route path, or selected model identifier.";
        break;
    case 413:
        outTitle = L"Payload Too Large (HTTP 413)";
        outAdvice = L"Hint: Lower Max Resolution in Model Profiles to 1024px or 2048px.";
        break;
    case 429:
        outTitle = L"Rate Limit Exceeded or Quota Exhausted (HTTP 429)";
        outAdvice = L"Hint: Check your API account balance/billing or wait before retrying.";
        break;
    case 500:
        outTitle = L"Internal Server Error (HTTP 500)";
        outAdvice = L"Hint: An internal crash or exception occurred inside the AI backend.";
        break;
    case 503:
        outTitle = L"Service Temporarily Unavailable (HTTP 503)";
        outAdvice = L"Hint: Remote AI server is overloaded or undergoing maintenance.";
        break;
    default: {
        wchar_t buf[64] = { 0 };
        swprintf_s(buf, L"AI Request Failed (HTTP %lu)", statusCode);
        outTitle = buf;
        outAdvice = L"Hint: Check endpoint connectivity, proxy settings, or firewall.";
        break;
    }
    }

    outDetail.clear();
    if (responseBody.empty()) return;

    // 2. Universal Semantic Heuristic Extraction (Language- & Framework-Agnostic)
    yyjson_doc* doc = yyjson_read(responseBody.data(), responseBody.size(), 0);
    if (doc) {
        yyjson_val* root = yyjson_doc_get_root(doc);
        if (root && yyjson_is_arr(root) && yyjson_arr_size(root) > 0) {
            root = yyjson_arr_get(root, 0);
        }

        if (root && yyjson_is_obj(root)) {
            const char* bestDetail = nullptr;
            const char* fallbackGeneric = nullptr;

            auto evaluateCandidate = [&](const char* candidate) {
                if (!candidate || candidate[0] == '\0') return;
                bool isGeneric = (strstr(candidate, "Exception") != nullptr ||
                                  strstr(candidate, "Error") != nullptr ||
                                  strcmp(candidate, "Not Found") == 0 ||
                                  strcmp(candidate, "Internal Server Error") == 0);
                if (isGeneric) {
                    if (!fallbackGeneric) fallbackGeneric = candidate;
                } else {
                    if (!bestDetail || strlen(candidate) > strlen(bestDetail)) {
                        bestDetail = candidate;
                    }
                }
            };

            // Heuristic Key Candidates with Priority
            static const char* const kPriorityKeys[] = {
                "message", "detail", "msg", "description", "reason", "error_description"
            };

            for (const char* key : kPriorityKeys) {
                yyjson_val* v = yyjson_obj_get(root, key);
                if (!v) continue;
                if (yyjson_is_str(v)) {
                    evaluateCandidate(yyjson_get_str(v));
                } else if (yyjson_is_arr(v) && yyjson_arr_size(v) > 0) {
                    // e.g. FastAPI validation errors: "detail": [{"msg": "...", "loc": ...}]
                    yyjson_val* first = yyjson_arr_get(v, 0);
                    if (yyjson_is_obj(first)) {
                        for (const char* subKey : kPriorityKeys) {
                            yyjson_val* subV = yyjson_obj_get(first, subKey);
                            if (subV && yyjson_is_str(subV)) {
                                evaluateCandidate(yyjson_get_str(subV));
                                break;
                            }
                        }
                    } else if (yyjson_is_str(first)) {
                        evaluateCandidate(yyjson_get_str(first));
                    }
                }
            }

            // Inspect nested "error" object (e.g. OpenAI / Anthropic / Gemini: {"error": {"message": "..."}})
            yyjson_val* vErr = yyjson_obj_get(root, "error");
            if (vErr) {
                if (yyjson_is_obj(vErr)) {
                    for (const char* key : kPriorityKeys) {
                        yyjson_val* subV = yyjson_obj_get(vErr, key);
                        if (subV && yyjson_is_str(subV)) {
                            evaluateCandidate(yyjson_get_str(subV));
                            break;
                        }
                    }
                } else if (yyjson_is_str(vErr)) {
                    evaluateCandidate(yyjson_get_str(vErr));
                }
            }

            const char* finalStr = bestDetail ? bestDetail : fallbackGeneric;
            if (finalStr) {
                outDetail = Utf8ToWide(finalStr);
                if (outDetail.find(L"Unhandled generated data mime type") != std::wstring::npos) {
                    outTitle = L"Gemini Compatibility Layer Bug (HTTP 400)";
                    outAdvice = L"Hint: Gemini's /chat/completions cannot return raw image data. Set Provider Protocol to 'Google Gemini (Native REST)' in Settings.";
                }
            }
        }
        yyjson_doc_free(doc);
    }

    // 3. Fallback: Plaintext snippet (e.g. Nginx 502 HTML / Gateway text)
    if (outDetail.empty()) {
        std::string snippet;
        for (char c : responseBody) {
            if (c == '\r' || c == '\n' || c == '\t') snippet += ' ';
            else if (static_cast<unsigned char>(c) >= 32 && static_cast<unsigned char>(c) < 127) snippet += c;
            if (snippet.size() >= 160) break;
        }
        if (!snippet.empty() && snippet.find('{') == std::string::npos && snippet.find('<') == std::string::npos) {
            outDetail = Utf8ToWide(snippet);
        }
    }
}

std::wstring AiActionManager::FormatAiErrorMessage(DWORD statusCode, std::string_view responseBody) {
    std::wstring title, detail, advice;
    ExtractSemanticError(statusCode, responseBody, title, detail, advice);

    std::wstring result = title;
    if (!detail.empty()) {
        result += L"\nDetail: " + detail;
    }
    if (!advice.empty()) {
        result += L"\n" + advice;
    }
    return result;
}

void AiActionManager::ShowAiErrorDialog(HWND hwndParent, const ExecutionResult& result) {
    TASKDIALOGCONFIG tc{};
    tc.cbSize = sizeof(tc);
    tc.hwndParent = hwndParent ? hwndParent : GetActiveWindow();
    tc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT | TDF_EXPAND_FOOTER_AREA;
    tc.pszWindowTitle = L"QuickView - AI Action Failure";
    tc.pszMainIcon = TD_ERROR_ICON;

    std::wstring mainTitle = result.mainTitle.empty() ? L"AI Action Execution Failed" : result.mainTitle;
    tc.pszMainInstruction = mainTitle.c_str();

    std::wstring content;
    if (!result.detailMessage.empty()) {
        content = L"Error Details:\n" + result.detailMessage;
    }
    if (!result.actionAdvice.empty()) {
        if (!content.empty()) content += L"\n\n";
        content += result.actionAdvice;
    }
    if (content.empty()) {
        content = result.errorMessage.empty() ? L"An unexpected error occurred during AI execution." : result.errorMessage;
    }
    tc.pszContent = content.c_str();

    std::wstring expandedInfo;
    if (!result.rawResponseBody.empty()) {
        std::string rawTrunc = result.rawResponseBody;
        if (rawTrunc.size() > 4096) {
            rawTrunc.resize(4096);
            rawTrunc += "\n... [Server response truncated, 4096 bytes shown]";
        }
        expandedInfo = Utf8ToWide(rawTrunc);
    } else {
        expandedInfo = L"(No response body returned from server)";
    }
    tc.pszExpandedInformation = expandedInfo.c_str();
    tc.pszCollapsedControlText = L"Show Raw Server Response (Ctrl+C to copy all)";
    tc.pszExpandedControlText = L"Hide Raw Server Response";
    tc.dwCommonButtons = TDCBF_CLOSE_BUTTON;

    TaskDialogIndirect(&tc, nullptr, nullptr, nullptr);
}

AiActionManager& AiActionManager::Instance() {
    static AiActionManager instance;
    return instance;
}

AiActionManager::AiActionManager() {
    Init();
}

AiActionManager::~AiActionManager() {
    CancelCurrentTask();
}

void AiActionManager::SetVisualFrameProviders(VisualFrameSnapshotFn snapshotFn, VisualBgraProviderFn renderFn) {
    s_visualSnapshotFn = snapshotFn;
    s_visualRenderFn = renderFn;
}

HINTERNET AiActionManager::OpenAiHttpRequest(
    uint64_t taskId,
    const wchar_t* userAgent,
    const wchar_t* hostName,
    INTERNET_PORT port,
    bool isHttps,
    bool isLocal,
    const std::wstring& fullPath,
    int timeoutSeconds,
    std::wstring& outError)
{
    ScopedWinHttpHandle hSession(WinHttpOpen(userAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!hSession) {
        outError = AppStrings::g.AiError_InitWinHttpFailed;
        return nullptr;
    }

    DWORD recvTimeout = (timeoutSeconds > 0) ? (static_cast<DWORD>(timeoutSeconds) * 1000) : (isLocal ? 0 : 600000);
    WinHttpSetTimeouts(hSession.h, 5000, 10000, 30000, recvTimeout);

    ScopedWinHttpHandle hConnect(WinHttpConnect(hSession.h, hostName, port, 0));
    if (!hConnect) {
        outError = AppStrings::g.AiError_ConnectFailed;
        return nullptr;
    }

    DWORD flags = isHttps ? WINHTTP_FLAG_SECURE : 0;
    ScopedWinHttpHandle hRequest(WinHttpOpenRequest(hConnect.h, L"POST", fullPath.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!hRequest) {
        outError = AppStrings::g.AiError_CreateReqFailed;
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(m_taskMutex);
    if (taskId != m_currentTaskId.load()) {
        return nullptr;
    }
    if (m_activeRequest) {
        WinHttpCloseHandle(m_activeRequest);
        m_activeRequest = nullptr;
    }
    if (m_activeConnect) {
        WinHttpCloseHandle(m_activeConnect);
        m_activeConnect = nullptr;
    }
    if (m_activeSession) {
        WinHttpCloseHandle(m_activeSession);
        m_activeSession = nullptr;
    }

    m_activeHttpTaskId = taskId;
    m_activeSession = hSession.Release();
    m_activeConnect = hConnect.Release();
    m_activeRequest = hRequest.Release();
    return m_activeRequest;
}

void AiActionManager::CloseActiveHttpHandlesIfOwner(uint64_t taskId) {
    std::lock_guard<std::mutex> lock(m_taskMutex);
    if (taskId == 0 || m_activeHttpTaskId == taskId) {
        if (m_activeRequest) {
            WinHttpCloseHandle(m_activeRequest);
            m_activeRequest = nullptr;
        }
        if (m_activeConnect) {
            WinHttpCloseHandle(m_activeConnect);
            m_activeConnect = nullptr;
        }
        if (m_activeSession) {
            WinHttpCloseHandle(m_activeSession);
            m_activeSession = nullptr;
        }
        m_activeHttpTaskId = 0;
    }
}

bool AiActionManager::Init() {
    if (m_initialized) return true;
    m_initialized = true;
    if (!LoadConfig()) {
        InitDefaultTemplates();
        SaveConfig();
    }
    return true;
}

bool AiActionManager::ReloadConfig() {
    return LoadConfig();
}

void AiActionManager::ResetToDefaults() {
    InitDefaultTemplates();
    SaveConfig();
}

void AiActionManager::InitDefaultTemplates() {
    m_profiles.clear();
    m_actions.clear();

    // Default: Single Profile (Starts with Google Gemini, customizable via dropdown)
    ModelProfile primary;
    primary.id = "primary_profile";
    primary.displayName = L"Google Gemini";
    primary.protocol = ApiProtocol::GeminiNative;
    primary.baseUrl = "https://generativelanguage.googleapis.com/v1beta/";
    primary.defaultModel = "";
    primary.maxResolution = MaxResolution::Original_4K;
    primary.timeoutSeconds = 600;
    primary.isCustom = false;
    m_profiles.push_back(primary);

    m_defaultProfileId = primary.id;

    // --- Default Actions (Numbered 1..5 for Professional Designers) ---
    // 1. AI 智能细节增强与画质精修 (AI Enhance & Texture Refine)
    ActionDesc act1;
    act1.id = "action_ai_enhance";
    act1.name = L"AI Detail Enhancement & Texture Refine";
    act1.modelProfileId = "";
    act1.promptTemplate = L"masterpiece, highly detailed, sharp focus, pristine textures, professional photography, natural lighting, crystal clear details, 8k uhd, masterwork";
    act1.negativePrompt = L"blurry, noise, low quality, artifacts, distorted, deformed, oversaturated, watermark, bad quality, grainy";
    act1.scopeMode = ScopeMode::FullImage;
    act1.denoisingStrength = 0.35f; // Crucial: 0.35 preserves exact shapes & subject geometry!
    act1.samplingSteps = 25;
    act1.cfgScale = 7.0f;
    m_actions.push_back(act1);

    // 2. 无痕消除与智能去水印 (Smart Inpaint & Watermark Removal)
    ActionDesc act2;
    act2.id = "action_inpaint_watermark";
    act2.name = L"Smart Inpaint & Object Removal";
    act2.modelProfileId = "";
    act2.promptTemplate = L"flawless seamless background fill, natural continuation of texture, pristine clean surface, smooth transition, high quality restoration, uninterrupted surface";
    act2.negativePrompt = L"text, watermark, logo, signature, letters, numbers, copyright, visible seams, blur, smear, artifacts, boundary lines";
    act2.scopeMode = ScopeMode::CropAndBlend;
    act2.denoisingStrength = 0.70f; // High inpaint strength within the selected crop bounding box
    act2.samplingSteps = 30;
    act2.cfgScale = 7.5f;
    m_actions.push_back(act2);

    // 3. 商业摄影影棚布光 (Commercial Studio Lighting & Render)
    ActionDesc act3;
    act3.id = "action_studio_lighting";
    act3.name = L"Studio Lighting & Commercial Render";
    act3.modelProfileId = "";
    act3.promptTemplate = L"commercial product photography, professional studio softbox lighting, octane render, soft ambient occlusion, elegant shadows, raytracing reflections, premium presentation";
    act3.negativePrompt = L"harsh flash, flat lighting, amateur snapshot, bad lighting, cluttered background, underexposed, overexposed, noise";
    act3.scopeMode = ScopeMode::FullImage;
    act3.denoisingStrength = 0.40f;
    act3.samplingSteps = 25;
    act3.cfgScale = 7.0f;
    m_actions.push_back(act3);

    // 4. 概念设计草图真实化渲染 (Design Sketch to Photorealism)
    ActionDesc act4;
    act4.id = "action_sketch_to_photo";
    act4.name = L"Sketch & Concept to Photorealism";
    act4.modelProfileId = "";
    act4.promptTemplate = L"photorealistic industrial and architectural prototype rendering, physical based rendering, realistic materials, metal and glass textures, pristine finish, canon 5d photography";
    act4.negativePrompt = L"sketch, lineart, drawing, cartoon, unrealistic, low resolution, 2d, illustration";
    act4.scopeMode = ScopeMode::FullImage;
    act4.denoisingStrength = 0.55f;
    act4.samplingSteps = 30;
    act4.cfgScale = 8.0f;
    m_actions.push_back(act4);

    // 5. 日漫与插画风格化转换 (Anime & Illustration Stylization)
    ActionDesc act5;
    act5.id = "action_anime_style";
    act5.name = L"Anime & Illustration Stylization";
    act5.modelProfileId = "";
    act5.promptTemplate = L"masterpiece anime illustration, Makoto Shinkai aesthetic, vibrant clean colors, beautiful atmospheric lighting, crisp lineart, cel shading, delicate details";
    act5.negativePrompt = L"photo, photorealistic, 3d render, messy lines, bad anatomy, bad quality, realistic skin";
    act5.scopeMode = ScopeMode::FullImage;
    act5.denoisingStrength = 0.50f;
    act5.samplingSteps = 25;
    act5.cfgScale = 7.5f;
    m_actions.push_back(act5);

    m_lastActionId = act1.id;
}

const ModelProfile* AiActionManager::FindProfile(std::string_view profileId) const {
    for (const auto& p : m_profiles) {
        if (p.id == profileId) return &p;
    }
    return nullptr;
}

const ModelProfile* AiActionManager::GetDefaultProfile() const {
    const ModelProfile* p = FindProfile(m_defaultProfileId);
    if (p) return p;
    return m_profiles.empty() ? nullptr : &m_profiles[0];
}

void AiActionManager::SetDefaultProfileId(std::string_view id) {
    m_defaultProfileId = id;
}

void AiActionManager::SetLastActionId(std::string_view id) {
    m_lastActionId = id;
}

std::wstring AiActionManager::GetConfigFilePath() const {
    std::wstring iniPath = GetConfigPath();
    wchar_t szDir[MAX_PATH * 2] = { 0 };
    wcscpy_s(szDir, iniPath.c_str());
    PathRemoveFileSpecW(szDir);
    if (szDir[0] != L'\0') {
        SystemInfo::EnsureDirectoryRecursive(szDir);
    }

    wchar_t szPath[MAX_PATH * 2] = { 0 };
    wcscpy_s(szPath, szDir);
    PathAppendW(szPath, L"ai_actions.json");

    // Seamless migration: If target does not exist yet, check if legacy exeDir version exists
    if (!SystemInfo::IsPackagedApp() && GetFileAttributesW(szPath) == INVALID_FILE_ATTRIBUTES) {
        wchar_t exePath[MAX_PATH] = { 0 };
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
            PathRemoveFileSpecW(exePath);
            PathAppendW(exePath, L"ai_actions.json");
            if (_wcsicmp(exePath, szPath) != 0 && GetFileAttributesW(exePath) != INVALID_FILE_ATTRIBUTES) {
                CopyFileW(exePath, szPath, FALSE);
            }
        }
    }
    return szPath;
}

// --- Security: Windows DPAPI En/Decryption ---
std::string AiActionManager::EncryptApiKey(std::string_view plainText) {
    if (plainText.empty()) return "";

    DATA_BLOB dataIn;
    dataIn.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plainText.data()));
    dataIn.cbData = static_cast<DWORD>(plainText.size());

    DATA_BLOB dataOut{};
    if (!CryptProtectData(&dataIn, L"QuickView_AI_Key", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &dataOut)) {
        return "";
    }

    DWORD base64Len = 0;
    CryptBinaryToStringA(dataOut.pbData, dataOut.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &base64Len);
    std::string result;
    if (base64Len > 0) {
        result.resize(base64Len);
        CryptBinaryToStringA(dataOut.pbData, dataOut.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, result.data(), &base64Len);
        while (!result.empty() && (result.back() == '\0' || result.back() == '\r' || result.back() == '\n')) {
            result.pop_back();
        }
    }
    LocalFree(dataOut.pbData);
    return result;
}

std::string AiActionManager::DecryptApiKey(std::string_view cipherBase64) {
    if (cipherBase64.empty()) return "";

    std::vector<uint8_t> cipherBinary;
    if (!Base64ToBinary(cipherBase64.data(), cipherBase64.size(), cipherBinary)) {
        return "";
    }

    DATA_BLOB dataIn;
    dataIn.pbData = cipherBinary.data();
    dataIn.cbData = static_cast<DWORD>(cipherBinary.size());

    DATA_BLOB dataOut{};
    if (!CryptUnprotectData(&dataIn, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &dataOut)) {
        return "";
    }

    std::string plain(reinterpret_cast<char*>(dataOut.pbData), dataOut.cbData);
    LocalFree(dataOut.pbData);
    return plain;
}

// --- Config Persistence via yyjson ---
bool AiActionManager::LoadConfig() {
    std::wstring cfgPath = GetConfigFilePath();
    FILE* fp = nullptr;
    _wfopen_s(&fp, cfgPath.c_str(), L"rb");
    if (!fp) return false;

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (size <= 0) {
        fclose(fp);
        return false;
    }

    std::vector<char> buf(size + 1, 0);
    fread(buf.data(), 1, size, fp);
    fclose(fp);

    yyjson_doc* doc = yyjson_read(buf.data(), size, 0);
    if (!doc) return false;

    yyjson_val* root = yyjson_doc_get_root(doc);
    if (!root) {
        yyjson_doc_free(doc);
        return false;
    }

    yyjson_val* vDefaultProfile = yyjson_obj_get(root, "default_profile");
    if (vDefaultProfile && yyjson_is_str(vDefaultProfile)) {
        m_defaultProfileId = yyjson_get_str(vDefaultProfile);
    }
    yyjson_val* vLastAction = yyjson_obj_get(root, "last_action");
    if (vLastAction && yyjson_is_str(vLastAction)) {
        m_lastActionId = yyjson_get_str(vLastAction);
    }

    // Load Profiles
    yyjson_val* vProfiles = yyjson_obj_get(root, "profiles");
    if (vProfiles && yyjson_is_arr(vProfiles)) {
        m_profiles.clear();
        size_t idx, maxIdx;
        yyjson_val* pVal;
        yyjson_arr_foreach(vProfiles, idx, maxIdx, pVal) {
            if (!yyjson_is_obj(pVal)) continue;
            ModelProfile p;
            ReadJsonStr(pVal, "id", p.id);
            std::string nameUtf8;
            if (ReadJsonStr(pVal, "name", nameUtf8)) {
                p.displayName = Utf8ToWide(nameUtf8);
            }
            if (p.displayName.empty() || p.displayName[0] < 32) {
                p.displayName = L"Google Gemini";
            }
            yyjson_val* proto = yyjson_obj_get(pVal, "protocol");
            if (proto && yyjson_is_int(proto)) p.protocol = static_cast<ApiProtocol>(yyjson_get_int(proto));
            ReadJsonStr(pVal, "base_url", p.baseUrl);
            ReadJsonStr(pVal, "encrypted_key", p.encryptedApiKey);
            ReadJsonStr(pVal, "default_model", p.defaultModel);
            yyjson_val* maxR = yyjson_obj_get(pVal, "max_resolution");
            if (maxR && yyjson_is_uint(maxR)) p.maxResolution = static_cast<MaxResolution>(yyjson_get_uint(maxR));
            yyjson_val* timeout = yyjson_obj_get(pVal, "timeout_seconds");
            if (timeout && yyjson_is_int(timeout)) p.timeoutSeconds = yyjson_get_int(timeout);
            yyjson_val* custom = yyjson_obj_get(pVal, "is_custom");
            if (custom && yyjson_is_bool(custom)) p.isCustom = yyjson_get_bool(custom);

            yyjson_val* vFetched = yyjson_obj_get(pVal, "fetched_models");
            if (vFetched && yyjson_is_arr(vFetched)) {
                p.fetchedModels.clear();
                size_t mIdx, mMax;
                yyjson_val* mVal;
                yyjson_arr_foreach(vFetched, mIdx, mMax, mVal) {
                    if (yyjson_is_str(mVal)) {
                        const char* mStr = yyjson_get_str(mVal);
                        if (mStr) p.fetchedModels.push_back(mStr);
                    }
                }
            }

            m_profiles.push_back(p);
        }
    }

    // Load Actions
    yyjson_val* vActions = yyjson_obj_get(root, "actions");
    if (vActions && yyjson_is_arr(vActions)) {
        m_actions.clear();
        size_t idx, maxIdx;
        yyjson_val* aVal;
        yyjson_arr_foreach(vActions, idx, maxIdx, aVal) {
            if (!yyjson_is_obj(aVal)) continue;
            ActionDesc a;
            ReadJsonStr(aVal, "id", a.id);
            std::string actNameUtf8;
            if (ReadJsonStr(aVal, "name", actNameUtf8)) {
                a.name = Utf8ToWide(actNameUtf8);
            }

            // Smooth migration for legacy Chinese default action names to standard English
            if (a.id == "action_ai_enhance" && (a.name.empty() || a.name == L"AI 画质精修与细节增强")) {
                a.name = L"AI Detail Enhancement & Texture Refine";
            } else if (a.id == "action_inpaint_watermark" && (a.name.empty() || a.name == L"无痕消除与智能去水印")) {
                a.name = L"Smart Inpaint & Object Removal";
            } else if (a.id == "action_studio_lighting" && (a.name.empty() || a.name == L"商业摄影影棚布光渲染")) {
                a.name = L"Studio Lighting & Commercial Render";
            } else if (a.id == "action_sketch_to_photo" && (a.name.empty() || a.name == L"设计线稿与草图写实渲染")) {
                a.name = L"Sketch & Concept to Photorealism";
            } else if (a.id == "action_anime_style" && (a.name.empty() || a.name == L"日漫插画与艺术风格化")) {
                a.name = L"Anime & Illustration Stylization";
            }
            ReadJsonStr(aVal, "profile_id", a.modelProfileId);

            std::string promptUtf8;
            if (ReadJsonStr(aVal, "prompt", promptUtf8)) {
                a.promptTemplate = Utf8ToWide(promptUtf8);
            }
            std::string negUtf8;
            if (ReadJsonStr(aVal, "negative_prompt", negUtf8)) {
                a.negativePrompt = Utf8ToWide(negUtf8);
            }
            yyjson_val* scope = yyjson_obj_get(aVal, "scope_mode");
            if (scope) {
                int sVal = yyjson_get_int(scope);
                if (sVal == 2 || (sVal == 1 && a.id == "action_inpaint_watermark")) {
                    a.scopeMode = ScopeMode::CropAndBlend;
                } else {
                    a.scopeMode = (sVal == 1 && (a.id == "action_studio_lighting" || a.id == "action_sketch_to_photo" || a.id == "action_anime_style"))
                        ? ScopeMode::FullImage : static_cast<ScopeMode>(sVal);
                }
            }

            yyjson_val* denoise = yyjson_obj_get(aVal, "denoising_strength");
            if (denoise) {
                if (yyjson_is_real(denoise)) a.denoisingStrength = static_cast<float>(yyjson_get_real(denoise));
                else if (yyjson_is_int(denoise)) a.denoisingStrength = static_cast<float>(yyjson_get_int(denoise));
            }
            yyjson_val* steps = yyjson_obj_get(aVal, "sampling_steps");
            if (steps) a.samplingSteps = yyjson_get_int(steps);
            yyjson_val* cfg = yyjson_obj_get(aVal, "cfg_scale");
            if (cfg) {
                if (yyjson_is_real(cfg)) a.cfgScale = static_cast<float>(yyjson_get_real(cfg));
                else if (yyjson_is_int(cfg)) a.cfgScale = static_cast<float>(yyjson_get_int(cfg));
            }
            yyjson_val* aspect = yyjson_obj_get(aVal, "aspect_ratio");
            if (aspect) a.aspectRatio = static_cast<OutputAspectRatio>(yyjson_get_int(aspect));

            yyjson_val* res = yyjson_obj_get(aVal, "target_resolution");
            if (res) a.targetResolution = static_cast<TargetResolution>(yyjson_get_int(res));

            m_actions.push_back(a);
        }
    }

    yyjson_doc_free(doc);
    return true;
}

bool AiActionManager::SaveConfig() {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    if (!doc) return false;

    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_strcpy(doc, root, "default_profile", m_defaultProfileId.c_str());
    yyjson_mut_obj_add_strcpy(doc, root, "last_action", m_lastActionId.c_str());

    // Serialize Profiles
    yyjson_mut_val* vProfiles = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, root, "profiles", vProfiles);
    for (const auto& p : m_profiles) {
        yyjson_mut_val* pVal = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, pVal, "id", p.id.c_str());

        std::string utf8Name = WideToUtf8(p.displayName);
        yyjson_mut_obj_add_strcpy(doc, pVal, "name", utf8Name.c_str());

        yyjson_mut_obj_add_int(doc, pVal, "protocol", static_cast<int>(p.protocol));
        yyjson_mut_obj_add_strcpy(doc, pVal, "base_url", p.baseUrl.c_str());
        yyjson_mut_obj_add_strcpy(doc, pVal, "encrypted_key", p.encryptedApiKey.c_str());
        yyjson_mut_obj_add_strcpy(doc, pVal, "default_model", p.defaultModel.c_str());
        yyjson_mut_obj_add_uint(doc, pVal, "max_resolution", static_cast<uint32_t>(p.maxResolution));
        yyjson_mut_obj_add_int(doc, pVal, "timeout_seconds", p.timeoutSeconds);
        yyjson_mut_obj_add_bool(doc, pVal, "is_custom", p.isCustom);

        if (!p.fetchedModels.empty()) {
            yyjson_mut_val* vModels = yyjson_mut_arr(doc);
            yyjson_mut_obj_add_val(doc, pVal, "fetched_models", vModels);
            for (const auto& mName : p.fetchedModels) {
                yyjson_mut_arr_add_strcpy(doc, vModels, mName.c_str());
            }
        }

        yyjson_mut_arr_append(vProfiles, pVal);
    }

    // Serialize Actions
    yyjson_mut_val* vActions = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, root, "actions", vActions);
    for (const auto& a : m_actions) {
        yyjson_mut_val* aVal = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, aVal, "id", a.id.c_str());

        std::string utf8Name = WideToUtf8(a.name);
        yyjson_mut_obj_add_strcpy(doc, aVal, "name", utf8Name.c_str());

        yyjson_mut_obj_add_strcpy(doc, aVal, "profile_id", a.modelProfileId.c_str());

        std::string utf8Prompt = WideToUtf8(a.promptTemplate);
        yyjson_mut_obj_add_strcpy(doc, aVal, "prompt", utf8Prompt.c_str());

        std::string utf8Neg = WideToUtf8(a.negativePrompt);
        yyjson_mut_obj_add_strcpy(doc, aVal, "negative_prompt", utf8Neg.c_str());

        yyjson_mut_obj_add_int(doc, aVal, "scope_mode", static_cast<int>(a.scopeMode));
        yyjson_mut_obj_add_int(doc, aVal, "aspect_ratio", static_cast<int>(a.aspectRatio));
        yyjson_mut_obj_add_int(doc, aVal, "target_resolution", static_cast<int>(a.targetResolution));
        yyjson_mut_obj_add_real(doc, aVal, "denoising_strength", a.denoisingStrength);
        yyjson_mut_obj_add_int(doc, aVal, "sampling_steps", a.samplingSteps);
        yyjson_mut_obj_add_real(doc, aVal, "cfg_scale", a.cfgScale);
        yyjson_mut_arr_append(vActions, aVal);
    }

    size_t jsonLen = 0;
    char* jsonStr = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY, &jsonLen);
    if (!jsonStr) {
        yyjson_mut_doc_free(doc);
        return false;
    }

    std::wstring cfgPath = GetConfigFilePath();

    FILE* fp = nullptr;
    _wfopen_s(&fp, cfgPath.c_str(), L"wb");
    bool ok = false;
    if (fp) {
        fwrite(jsonStr, 1, jsonLen, fp);
        fclose(fp);
        ok = true;
    }
    free(jsonStr);
    yyjson_mut_doc_free(doc);
    return ok;
}

// --- Image Processing & Inpainting Helper ---
bool AiActionManager::EncodeToPngMemory(
    const uint8_t* bgra, int width, int height, int stride,
    std::vector<uint8_t>& outPngBytes) {

    if (!bgra || width <= 0 || height <= 0 || stride < width * 4) return false;
    const size_t totalBytes = static_cast<size_t>(stride) * static_cast<size_t>(height);
    if (totalBytes > static_cast<size_t>((std::numeric_limits<UINT>::max)())) return false;

    ComPtr<IStream> pStream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &pStream)) || !pStream) {
        return false;
    }

    ComPtr<IWICImagingFactory> pFactory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFactory))) || !pFactory) {
        return false;
    }

    ComPtr<IWICBitmapEncoder> pEncoder;
    if (FAILED(pFactory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &pEncoder)) || !pEncoder) {
        return false;
    }

    if (FAILED(pEncoder->Initialize(pStream.Get(), WICBitmapEncoderNoCache))) return false;
    ComPtr<IWICBitmapFrameEncode> pFrame;
    if (FAILED(pEncoder->CreateNewFrame(&pFrame, nullptr)) || !pFrame) return false;
    if (FAILED(pFrame->Initialize(nullptr))) return false;
    if (FAILED(pFrame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height)))) return false;

    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(pFrame->SetPixelFormat(&format))) return false;
    if (FAILED(pFrame->WritePixels(static_cast<UINT>(height), static_cast<UINT>(stride), static_cast<UINT>(totalBytes), const_cast<BYTE*>(bgra)))) {
        return false;
    }
    if (FAILED(pFrame->Commit()) || FAILED(pEncoder->Commit())) return false;

    STATSTG stat{};
    if (FAILED(pStream->Stat(&stat, STATFLAG_NONAME)) || stat.cbSize.QuadPart == 0 ||
        stat.cbSize.QuadPart > static_cast<ULONGLONG>((std::numeric_limits<ULONG>::max)())) {
        return false;
    }
    const ULONG streamSize = static_cast<ULONG>(stat.cbSize.QuadPart);
    outPngBytes.resize(streamSize);

    LARGE_INTEGER liZero{};
    if (FAILED(pStream->Seek(liZero, STREAM_SEEK_SET, nullptr))) {
        outPngBytes.clear();
        return false;
    }
    ULONG bytesRead = 0;
    if (FAILED(pStream->Read(outPngBytes.data(), streamSize, &bytesRead)) || bytesRead == 0) {
        outPngBytes.clear();
        return false;
    }
    outPngBytes.resize(bytesRead);
    return true;
}

void AiActionManager::ResampleBgraExact(
    const uint8_t* srcBgra, int srcW, int srcH, int srcStride,
    uint8_t* dstBgra, int dstW, int dstH, int dstStride) {

    if (!srcBgra || !dstBgra || srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return;

    const float xRatio = static_cast<float>(srcW) / static_cast<float>(dstW);
    const float yRatio = static_cast<float>(srcH) / static_cast<float>(dstH);

    for (int y = 0; y < dstH; ++y) {
        float srcY = (static_cast<float>(y) + 0.5f) * yRatio - 0.5f;
        int yLow = std::clamp(static_cast<int>(std::floor(srcY)), 0, srcH - 1);
        int yHigh = std::clamp(yLow + 1, 0, srcH - 1);
        float yWeight = srcY - std::floor(srcY);

        const uint8_t* rowLow = srcBgra + yLow * srcStride;
        const uint8_t* rowHigh = srcBgra + yHigh * srcStride;
        uint8_t* dstRow = dstBgra + y * dstStride;

        for (int x = 0; x < dstW; ++x) {
            float srcX = (static_cast<float>(x) + 0.5f) * xRatio - 0.5f;
            int xLow = std::clamp(static_cast<int>(std::floor(srcX)), 0, srcW - 1);
            int xHigh = std::clamp(xLow + 1, 0, srcW - 1);
            float xWeight = srcX - std::floor(srcX);

            const uint8_t* p00 = rowLow + xLow * 4;
            const uint8_t* p10 = rowLow + xHigh * 4;
            const uint8_t* p01 = rowHigh + xLow * 4;
            const uint8_t* p11 = rowHigh + xHigh * 4;

            float w00 = (1.0f - xWeight) * (1.0f - yWeight);
            float w10 = xWeight * (1.0f - yWeight);
            float w01 = (1.0f - xWeight) * yWeight;
            float w11 = xWeight * yWeight;

            for (int c = 0; c < 4; ++c) {
                float val = w00 * p00[c] + w10 * p10[c] + w01 * p01[c] + w11 * p11[c];
                dstRow[x * 4 + c] = static_cast<uint8_t>(std::clamp(val, 0.0f, 255.0f));
            }
        }
    }
}

void AiActionManager::BlendMaskGuidedFeathered(
    uint8_t* dstBgra, int dstW, int dstH, int dstStride,
    const uint8_t* subBgra, int sliceX0, int sliceY0, int sliceW, int sliceH, int sliceStride,
    int selX0, int selY0, int selX1, int selY1, int featherPixels) {

    if (!dstBgra || !subBgra || dstW <= 0 || dstH <= 0 || sliceW <= 0 || sliceH <= 0) return;
    featherPixels = (std::max)(1, featherPixels);

    // Strictly limit blending to the user's selected region.
    // Pixels outside [selX0, selX1) x [selY0, selY1) are NEVER touched, perfectly protecting original image!
    int blendX0 = (std::max)(sliceX0, (std::max)(0, selX0));
    int blendY0 = (std::max)(sliceY0, (std::max)(0, selY0));
    int blendX1 = (std::min)(sliceX0 + sliceW, (std::min)(dstW, selX1));
    int blendY1 = (std::min)(sliceY0 + sliceH, (std::min)(dstH, selY1));

    for (int y = blendY0; y < blendY1; ++y) {
        uint8_t* dRow = dstBgra + y * dstStride;
        int subY = y - sliceY0;
        if (subY < 0 || subY >= sliceH) continue;
        const uint8_t* sRow = subBgra + subY * sliceStride;

        int distY = (std::min)(y - selY0, selY1 - 1 - y);

        for (int x = blendX0; x < blendX1; ++x) {
            int subX = x - sliceX0;
            if (subX < 0 || subX >= sliceW) continue;

            int distX = (std::min)(x - selX0, selX1 - 1 - x);
            int minDist = (std::min)(distX, distY);

            float alpha = 1.0f;
            if (minDist < featherPixels) {
                float norm = static_cast<float>(minDist) / static_cast<float>(featherPixels);
                // Cosine smooth transition
                alpha = 0.5f * (1.0f - std::cos(norm * 3.141592653589793f));
            }

            uint8_t* dPixel = dRow + x * 4;
            const uint8_t* sPixel = sRow + subX * 4;

            float srcA = (sPixel[3] / 255.0f) * alpha;
            float invA = 1.0f - srcA;

            dPixel[0] = static_cast<uint8_t>(std::clamp(sPixel[0] * srcA + dPixel[0] * invA, 0.0f, 255.0f));
            dPixel[1] = static_cast<uint8_t>(std::clamp(sPixel[1] * srcA + dPixel[1] * invA, 0.0f, 255.0f));
            dPixel[2] = static_cast<uint8_t>(std::clamp(sPixel[2] * srcA + dPixel[2] * invA, 0.0f, 255.0f));
            dPixel[3] = static_cast<uint8_t>(std::clamp(255.0f * srcA + dPixel[3] * invA, 0.0f, 255.0f));
        }
    }
}

// --- Execution & Task Lifecycle ---
uint64_t AiActionManager::ExecuteAction(
    const ActionDesc& action, HWND hwnd,
    ActionCallback onComplete,
    std::wstring_view customPrompt,
    int cropL, int cropT, int cropR, int cropB) {

    CancelCurrentTask();

    const ModelProfile* profile = nullptr;
    if (!action.modelProfileId.empty()) {
        profile = FindProfile(action.modelProfileId);
    }
    if (!profile) {
        profile = GetDefaultProfile();
    }
    if (!profile) {
        if (onComplete) {
            ExecutionResult err;
            err.success = false;
            err.errorMessage = AppStrings::g.AiError_NoAvailableProfile;
            onComplete(err);
        }
        return 0;
    }

    uint64_t taskId = ++m_currentTaskId;
    m_isRunning.store(true);
    m_lastActionId = action.id;

    VisualFrameSnapshot snapshot;
    if (!s_visualSnapshotFn || !s_visualSnapshotFn(snapshot)) {
        snapshot.imagePath = GetCurrentActiveImagePath();
    }

    ModelProfile profCopy = *profile;
    ActionDesc actCopy = action;

    // Macro replacement for {prompt} or customPrompt injection
    if (!customPrompt.empty()) {
        size_t macroPos = actCopy.promptTemplate.find(L"{prompt}");
        if (macroPos != std::wstring::npos) {
            actCopy.promptTemplate.replace(macroPos, 8, customPrompt);
        } else {
            if (!actCopy.promptTemplate.empty()) actCopy.promptTemplate += L"\n";
            actCopy.promptTemplate += customPrompt;
        }
    } else {
        size_t macroPos = actCopy.promptTemplate.find(L"{prompt}");
        if (macroPos != std::wstring::npos) {
            actCopy.promptTemplate.erase(macroPos, 8);
        }
    }

    // Check if we should execute inpaint / region selection
    bool hasSelection = (std::abs(cropR - cropL) >= 8 && std::abs(cropB - cropT) >= 8);
    bool shouldInpaint = (actCopy.scopeMode == ScopeMode::CropAndBlend);

    if (shouldInpaint && hasSelection) {
        std::wstring promptCopy = actCopy.promptTemplate;
        QuickView::RunDetached([this, taskId, snapshot = std::move(snapshot), cropL, cropT, cropR, cropB, promptCopy, profCopy, hwnd, actCopy, onComplete = std::move(onComplete)]() mutable {
            InpaintWorkerThread(taskId, std::move(snapshot), cropL, cropT, cropR, cropB, promptCopy, profCopy, hwnd, std::move(onComplete), actCopy);
        });
        return taskId;
    }

    QuickView::RunDetached([this, taskId, snapshot = std::move(snapshot), actCopy, profCopy, hwnd, onComplete = std::move(onComplete)]() mutable {
        WorkerThread(taskId, std::move(snapshot), actCopy, profCopy, hwnd, std::move(onComplete));
    });

    return taskId;
}

void AiActionManager::CancelCurrentTask() {
    m_currentTaskId.fetch_add(1);
    m_isRunning.store(false);
    CloseActiveHttpHandlesIfOwner(0);
}

static void ComputeTargetDimensions(
    OutputAspectRatio ratio, TargetResolution targetRes, uint32_t srcW, uint32_t srcH,
    int& outW, int& outH) {
    int baseSize = 1024;
    if (targetRes == TargetResolution::Res_2K) baseSize = 2048;
    else if (targetRes == TargetResolution::Res_4K) baseSize = 3840;

    switch (ratio) {
        case OutputAspectRatio::Square_1_1:
            outW = (targetRes == TargetResolution::Res_4K) ? 4096 : baseSize;
            outH = (targetRes == TargetResolution::Res_4K) ? 4096 : baseSize;
            break;
        case OutputAspectRatio::Landscape_16_9:
            outW = (baseSize == 3840) ? 3840 : ((baseSize == 2048) ? 2560 : 1344);
            outH = (baseSize == 3840) ? 2160 : ((baseSize == 2048) ? 1440 : 768);
            break;
        case OutputAspectRatio::Portrait_9_16:
            outW = (baseSize == 3840) ? 2160 : ((baseSize == 2048) ? 1440 : 768);
            outH = (baseSize == 3840) ? 3840 : ((baseSize == 2048) ? 2560 : 1344);
            break;
        case OutputAspectRatio::Standard_4_3:
            outW = (baseSize == 3840) ? 3840 : ((baseSize == 2048) ? 2304 : 1152);
            outH = (baseSize == 3840) ? 2880 : ((baseSize == 2048) ? 1728 : 864);
            break;
        case OutputAspectRatio::Vertical_3_4:
            outW = (baseSize == 3840) ? 2880 : ((baseSize == 2048) ? 1728 : 864);
            outH = (baseSize == 3840) ? 3840 : ((baseSize == 2048) ? 2304 : 1152);
            break;
        case OutputAspectRatio::Auto:
        default:
            if (srcW > 0 && srcH > 0) {
                float aspect = static_cast<float>(srcW) / static_cast<float>(srcH);
                if (aspect >= 1.0f) {
                    outW = baseSize;
                    outH = static_cast<int>(std::round(outW / aspect));
                } else {
                    outH = baseSize;
                    outW = static_cast<int>(std::round(outH * aspect));
                }
                outW = (outW / 64) * 64;
                outH = (outH / 64) * 64;
                outW = (std::max)(256, (std::min)(outW, 4096));
                outH = (std::max)(256, (std::min)(outH, 4096));
            } else {
                outW = baseSize;
                outH = baseSize;
            }
            break;
    }
}

void AiActionManager::WorkerThread(
    uint64_t taskId, VisualFrameSnapshot snapshot, ActionDesc action, ModelProfile profile,
    HWND /*hwnd*/, ActionCallback callback) {

    ScopedComInit comGuard;
    struct HttpHandleGuard {
        AiActionManager* mgr;
        uint64_t tid;
        ~HttpHandleGuard() { mgr->CloseActiveHttpHandlesIfOwner(tid); }
    } httpGuard{ this, taskId };

    ExecutionResult result;
    result.success = false;
    result.taskId = taskId;
    result.sourceImagePath = snapshot.imagePath;

    auto finishTask = [&](bool invokeCb) {
        if (taskId == m_currentTaskId.load()) {
            m_isRunning.store(false);
            if (invokeCb && callback) {
                callback(result);
            }
        }
    };

    // Check cancellation
    if (taskId != m_currentTaskId.load()) {
        return;
    }

    bool isLocal = (profile.protocol == ApiProtocol::ComfyUI ||
                    profile.protocol == ApiProtocol::StabilityInpaint ||
                    profile.baseUrl.find("127.0.0.1") != std::string::npos ||
                    profile.baseUrl.find("localhost") != std::string::npos);

    std::string apiKey = DecryptApiKey(profile.encryptedApiKey);
    if (!isLocal && apiKey.empty()) {
        result.errorMessage = AppStrings::g.AiError_ApiKeyEmpty;
        finishTask(true);
        return;
    }

    // Determine target model
    std::string targetModel = profile.defaultModel;
    if (targetModel.empty() && profile.protocol != ApiProtocol::StabilityInpaint && profile.protocol != ApiProtocol::ComfyUI) {
        result.errorMessage = AppStrings::g.AiError_ModelEmpty;
        finishTask(true);
        return;
    }

    // Load and encode active visual image (if any)
    std::string imageBase64;
    std::string imageMimeType;
    std::vector<uint8_t> editPngBytes;
    const bool wantsRawPngBytes = (profile.protocol == ApiProtocol::OpenAiImagesEdit);
    uint32_t imgW = 0, imgH = 0;
    bool hasImage = LoadVisualImageAsPng(
        snapshot, profile.maxResolution, imageBase64, imageMimeType,
        wantsRawPngBytes ? &editPngBytes : nullptr, imgW, imgH);

    // Parse URL & Endpoint
    wchar_t hostName[256] = { 0 };
    INTERNET_PORT port = 0;
    bool isHttps = false;
    std::wstring fullPath;
    if (!CrackEndpointUrl(profile.baseUrl, hostName, port, isHttps, fullPath, result.errorMessage)) {
        finishTask(true);
        return;
    }

    bool isAnthropic = (profile.baseUrl.find("api.anthropic.com") != std::string::npos);
    ApiProtocol effectiveProtocol = profile.protocol;

    if (effectiveProtocol == ApiProtocol::StabilityInpaint) {
        ResolveSdPath(fullPath, hasImage);
    } else if (effectiveProtocol == ApiProtocol::GeminiNative) {
        ResolveGeminiPath(fullPath, targetModel);
    } else if (effectiveProtocol == ApiProtocol::OpenAiChat) {
        if (isAnthropic) {
            if (fullPath.find(L"messages") == std::wstring::npos) fullPath += L"messages";
        } else {
            if (fullPath.find(L"chat/completions") == std::wstring::npos) fullPath += L"chat/completions";
        }
    } else if (effectiveProtocol == ApiProtocol::OpenAiImagesGenerate) {
        if (fullPath.find(L"images/generations") == std::wstring::npos) fullPath += L"images/generations";
    } else if (effectiveProtocol == ApiProtocol::OpenAiImagesEdit) {
        if (fullPath.find(L"images/edits") == std::wstring::npos) fullPath += L"images/edits";
    }

    HINTERNET hRequest = OpenAiHttpRequest(taskId, L"QuickView-AI/2.0", hostName, port, isHttps, isLocal, fullPath, profile.timeoutSeconds, result.errorMessage);
    if (!hRequest) {
        if (taskId == m_currentTaskId.load()) {
            finishTask(true);
        }
        return;
    }

    // Prompt UTF-8
    std::string utf8Prompt = WideToUtf8(action.promptTemplate);
    if (effectiveProtocol != ApiProtocol::StabilityInpaint && !action.negativePrompt.empty()) {
        std::string utf8Neg = WideToUtf8(action.negativePrompt);
        utf8Prompt += "\n[Negative constraints: please strictly avoid the following: " + utf8Neg + "]";
    }
    if (effectiveProtocol != ApiProtocol::StabilityInpaint && effectiveProtocol != ApiProtocol::OpenAiImagesGenerate && action.aspectRatio != OutputAspectRatio::Auto) {
        const char* ratioDesc = nullptr;
        switch (action.aspectRatio) {
            case OutputAspectRatio::Square_1_1: ratioDesc = "1:1 square"; break;
            case OutputAspectRatio::Landscape_16_9: ratioDesc = "16:9 widescreen landscape"; break;
            case OutputAspectRatio::Portrait_9_16: ratioDesc = "9:16 vertical portrait"; break;
            case OutputAspectRatio::Standard_4_3: ratioDesc = "4:3 standard landscape"; break;
            case OutputAspectRatio::Vertical_3_4: ratioDesc = "3:4 vertical standard"; break;
            default: break;
        }
        if (ratioDesc) {
            utf8Prompt += "\n[Composition constraint: framing and aspect ratio must strictly be " + std::string(ratioDesc) + " format.]";
        }
    }

    std::wstring headers;
    std::string postPayload;

    if (effectiveProtocol == ApiProtocol::OpenAiImagesEdit && hasImage && !editPngBytes.empty()) {
        const std::string boundary = "----QuickViewEditBoundary9A2C4E";
        headers = L"Content-Type: multipart/form-data; boundary=" + Utf8ToWide(boundary) + L"\r\n";
        if (!apiKey.empty()) {
            headers += L"Authorization: Bearer " + Utf8ToWide(apiKey) + L"\r\n";
        }
        postPayload = BuildOpenAiMultipartEditPayload(boundary, targetModel, utf8Prompt, editPngBytes, nullptr);
    } else {
        // Build JSON Payload via yyjson
        yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
        yyjson_mut_val* root = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, root);

        if (effectiveProtocol == ApiProtocol::StabilityInpaint) {
            yyjson_mut_obj_add_str(doc, root, "prompt", utf8Prompt.c_str());
            if (!action.negativePrompt.empty()) {
                std::string utf8Neg = WideToUtf8(action.negativePrompt);
                yyjson_mut_obj_add_str(doc, root, "negative_prompt", utf8Neg.c_str());
            }
            int actualSteps = (action.samplingSteps >= 5 && action.samplingSteps <= 150) ? action.samplingSteps : 25;
            yyjson_mut_obj_add_int(doc, root, "steps", actualSteps);
            double actualCfg = (action.cfgScale >= 1.0f && action.cfgScale <= 30.0f) ? static_cast<double>(action.cfgScale) : 7.0;
            yyjson_mut_obj_add_real(doc, root, "cfg_scale", actualCfg);

            int targetW = 1024;
            int targetH = 1024;
            ComputeTargetDimensions(action.aspectRatio, action.targetResolution, imgW, imgH, targetW, targetH);
            yyjson_mut_obj_add_int(doc, root, "width", targetW);
            yyjson_mut_obj_add_int(doc, root, "height", targetH);

            if (hasImage) {
                yyjson_mut_val* initArr = yyjson_mut_arr(doc);
                yyjson_mut_arr_append(initArr, yyjson_mut_str(doc, imageBase64.c_str()));
                yyjson_mut_obj_add_val(doc, root, "init_images", initArr);
                double actualDenoise = (action.denoisingStrength > 0.0f && action.denoisingStrength <= 1.0f) ?
                                       static_cast<double>(action.denoisingStrength) : 0.35;
                yyjson_mut_obj_add_real(doc, root, "denoising_strength", actualDenoise);
            }

            if (!targetModel.empty() && targetModel != "default") {
                std::string cleanModel = targetModel;
                size_t bracketPos = cleanModel.rfind(" [");
                if (bracketPos != std::string::npos && cleanModel.back() == ']') {
                    cleanModel = cleanModel.substr(0, bracketPos);
                }
                yyjson_mut_val* overrideSettings = yyjson_mut_obj(doc);
                yyjson_mut_obj_add_str(doc, overrideSettings, "sd_model_checkpoint", cleanModel.c_str());
                yyjson_mut_obj_add_val(doc, root, "override_settings", overrideSettings);
            }
        } else if (isAnthropic) {
            yyjson_mut_obj_add_str(doc, root, "model", targetModel.c_str());
            yyjson_mut_obj_add_int(doc, root, "max_tokens", 2048);

            yyjson_mut_val* msgs = yyjson_mut_arr(doc);
            yyjson_mut_obj_add_val(doc, root, "messages", msgs);

            yyjson_mut_val* userMsg = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, userMsg, "role", "user");

            if (hasImage) {
                yyjson_mut_val* contentArr = yyjson_mut_arr(doc);
                yyjson_mut_obj_add_val(doc, userMsg, "content", contentArr);

                yyjson_mut_val* imgObj = yyjson_mut_obj(doc);
                yyjson_mut_obj_add_str(doc, imgObj, "type", "image");
                yyjson_mut_val* srcObj = yyjson_mut_obj(doc);
                yyjson_mut_obj_add_str(doc, srcObj, "type", "base64");
                yyjson_mut_obj_add_str(doc, srcObj, "media_type", imageMimeType.c_str());
                yyjson_mut_obj_add_str(doc, srcObj, "data", imageBase64.c_str());
                yyjson_mut_obj_add_val(doc, imgObj, "source", srcObj);
                yyjson_mut_arr_append(contentArr, imgObj);

                yyjson_mut_val* txtObj = yyjson_mut_obj(doc);
                yyjson_mut_obj_add_str(doc, txtObj, "type", "text");
                yyjson_mut_obj_add_str(doc, txtObj, "text", utf8Prompt.c_str());
                yyjson_mut_arr_append(contentArr, txtObj);
            } else {
                yyjson_mut_obj_add_str(doc, userMsg, "content", utf8Prompt.c_str());
            }
            yyjson_mut_arr_append(msgs, userMsg);
        } else if (effectiveProtocol == ApiProtocol::GeminiNative) {
            const char* geminiSizeStr = "1K";
            if (action.targetResolution == TargetResolution::Res_2K) geminiSizeStr = "2K";
            else if (action.targetResolution == TargetResolution::Res_4K) geminiSizeStr = "4K";

            const char* geminiAspect = SelectClosestGeminiAspectRatio(action.aspectRatio, imgW, imgH);

            if (targetModel.rfind("imagen-", 0) == 0) {
                yyjson_mut_val* instancesArr = yyjson_mut_arr(doc);
                yyjson_mut_val* instObj = yyjson_mut_obj(doc);
                yyjson_mut_obj_add_str(doc, instObj, "prompt", utf8Prompt.c_str());
                yyjson_mut_arr_append(instancesArr, instObj);
                yyjson_mut_obj_add_val(doc, root, "instances", instancesArr);

                yyjson_mut_val* paramsObj = yyjson_mut_obj(doc);
                yyjson_mut_obj_add_int(doc, paramsObj, "sampleCount", 1);
                yyjson_mut_obj_add_str(doc, paramsObj, "aspectRatio", geminiAspect);
                yyjson_mut_obj_add_str(doc, paramsObj, "imageSize", geminiSizeStr);
                if (!action.negativePrompt.empty()) {
                    std::string utf8Neg = WideToUtf8(action.negativePrompt);
                    yyjson_mut_obj_add_str(doc, paramsObj, "negative_prompt", utf8Neg.c_str());
                }
                yyjson_mut_obj_add_val(doc, root, "parameters", paramsObj);
            } else {
                yyjson_mut_val* contentsArr = yyjson_mut_arr(doc);
                yyjson_mut_val* contentItem = yyjson_mut_obj(doc);
                yyjson_mut_obj_add_str(doc, contentItem, "role", "user");

                yyjson_mut_val* partsArr = yyjson_mut_arr(doc);

                yyjson_mut_val* textPart = yyjson_mut_obj(doc);
                yyjson_mut_obj_add_str(doc, textPart, "text", utf8Prompt.c_str());
                yyjson_mut_arr_append(partsArr, textPart);

                if (hasImage) {
                    yyjson_mut_val* imgPart = yyjson_mut_obj(doc);
                    yyjson_mut_val* inlineData = yyjson_mut_obj(doc);
                    yyjson_mut_obj_add_str(doc, inlineData, "mimeType", imageMimeType.c_str());
                    yyjson_mut_obj_add_str(doc, inlineData, "data", imageBase64.c_str());
                    yyjson_mut_obj_add_val(doc, imgPart, "inlineData", inlineData);
                    yyjson_mut_arr_append(partsArr, imgPart);
                }

                yyjson_mut_obj_add_val(doc, contentItem, "parts", partsArr);
                yyjson_mut_arr_append(contentsArr, contentItem);
                yyjson_mut_obj_add_val(doc, root, "contents", contentsArr);

                yyjson_mut_val* genConfig = yyjson_mut_obj(doc);
                yyjson_mut_val* respModalities = yyjson_mut_arr(doc);
                yyjson_mut_arr_add_strcpy(doc, respModalities, "IMAGE");
                yyjson_mut_arr_add_strcpy(doc, respModalities, "TEXT");
                yyjson_mut_obj_add_val(doc, genConfig, "responseModalities", respModalities);

                yyjson_mut_val* imgConfig = yyjson_mut_obj(doc);
                yyjson_mut_obj_add_str(doc, imgConfig, "aspectRatio", geminiAspect);
                yyjson_mut_obj_add_str(doc, imgConfig, "imageSize", geminiSizeStr);
                yyjson_mut_obj_add_val(doc, genConfig, "imageConfig", imgConfig);

                yyjson_mut_obj_add_val(doc, root, "generationConfig", genConfig);
            }
        } else if (effectiveProtocol == ApiProtocol::OpenAiImagesGenerate) {
            yyjson_mut_obj_add_str(doc, root, "model", targetModel.c_str());
            yyjson_mut_obj_add_str(doc, root, "prompt", utf8Prompt.c_str());
            yyjson_mut_obj_add_int(doc, root, "n", 1);
            yyjson_mut_obj_add_str(doc, root, "response_format", "b64_json");

            const char* dalleQuality = (action.targetResolution == TargetResolution::Res_1K) ? "standard" : "hd";
            yyjson_mut_obj_add_str(doc, root, "quality", dalleQuality);

            const char* dalleSize = "1024x1024";
            switch (action.aspectRatio) {
                case OutputAspectRatio::Landscape_16_9:
                case OutputAspectRatio::Standard_4_3:
                    dalleSize = "1792x1024";
                    break;
                case OutputAspectRatio::Portrait_9_16:
                case OutputAspectRatio::Vertical_3_4:
                    dalleSize = "1024x1792";
                    break;
                case OutputAspectRatio::Square_1_1:
                    dalleSize = "1024x1024";
                    break;
                case OutputAspectRatio::Auto:
                default:
                    if (imgW > 0 && imgH > 0) {
                        float aspect = static_cast<float>(imgW) / static_cast<float>(imgH);
                        if (aspect >= 1.3f) dalleSize = "1792x1024";
                        else if (aspect <= 0.77f) dalleSize = "1024x1792";
                        else dalleSize = "1024x1024";
                    }
                    break;
            }
            yyjson_mut_obj_add_str(doc, root, "size", dalleSize);

            if (!action.negativePrompt.empty()) {
                std::string utf8Neg = WideToUtf8(action.negativePrompt);
                yyjson_mut_obj_add_str(doc, root, "negative_prompt", utf8Neg.c_str());
            }
            if (action.samplingSteps > 0) {
                yyjson_mut_obj_add_int(doc, root, "steps", action.samplingSteps);
                yyjson_mut_obj_add_int(doc, root, "num_inference_steps", action.samplingSteps);
            }
            if (action.cfgScale > 0.0f) {
                yyjson_mut_obj_add_real(doc, root, "guidance_scale", static_cast<double>(action.cfgScale));
            }
        } else {
            yyjson_mut_obj_add_str(doc, root, "model", targetModel.c_str());
            yyjson_mut_val* msgs = yyjson_mut_arr(doc);
            yyjson_mut_obj_add_val(doc, root, "messages", msgs);

            yyjson_mut_val* userMsg = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, userMsg, "role", "user");

            if (hasImage) {
                yyjson_mut_val* contentArr = yyjson_mut_arr(doc);
                yyjson_mut_obj_add_val(doc, userMsg, "content", contentArr);

                yyjson_mut_val* textObj = yyjson_mut_obj(doc);
                yyjson_mut_obj_add_str(doc, textObj, "type", "text");
                yyjson_mut_obj_add_str(doc, textObj, "text", utf8Prompt.c_str());
                yyjson_mut_arr_append(contentArr, textObj);

                yyjson_mut_val* imgObj = yyjson_mut_obj(doc);
                yyjson_mut_obj_add_str(doc, imgObj, "type", "image_url");
                yyjson_mut_val* urlChild = yyjson_mut_obj(doc);
                std::string dataUrl = "data:" + imageMimeType + ";base64," + imageBase64;
                yyjson_mut_obj_add_str(doc, urlChild, "url", dataUrl.c_str());
                yyjson_mut_obj_add_val(doc, imgObj, "image_url", urlChild);
                yyjson_mut_arr_append(contentArr, imgObj);
            } else {
                yyjson_mut_obj_add_str(doc, userMsg, "content", utf8Prompt.c_str());
            }
            yyjson_mut_arr_append(msgs, userMsg);
        }

        size_t payloadLen = 0;
        char* payloadStr = yyjson_mut_write(doc, 0, &payloadLen);
        yyjson_mut_doc_free(doc);
        if (!payloadStr) {
            result.errorMessage = AppStrings::g.AiError_JsonParseFailed;
            finishTask(true);
            return;
        }
        postPayload.assign(payloadStr, payloadLen);
        free(payloadStr);

        // Build Headers
        headers = L"Content-Type: application/json\r\n";
        if (isAnthropic) {
            if (!apiKey.empty()) {
                headers += L"x-api-key: " + Utf8ToWide(apiKey) + L"\r\n";
            }
            headers += L"anthropic-version: 2023-06-01\r\n";
        } else if (effectiveProtocol == ApiProtocol::GeminiNative) {
            if (!apiKey.empty()) {
                headers += L"x-goog-api-key: " + Utf8ToWide(apiKey) + L"\r\n";
            }
        } else if (!apiKey.empty()) {
            headers += L"Authorization: Bearer " + Utf8ToWide(apiKey) + L"\r\n";
        }
    }

    // Send Request & Read Response via unified helper
    auto httpRes = SendRequestAndReadResponse(
        hRequest, headers, postPayload.data(), static_cast<DWORD>(postPayload.size()),
        hostName, port, isLocal, taskId, m_currentTaskId);

    if (httpRes.interrupted) {
        return;
    }

    result.httpStatusCode = httpRes.statusCode;

    if (!httpRes.ok) {
        result.rawResponseBody = std::move(httpRes.body);
        result.mainTitle = std::move(httpRes.mainTitle);
        result.detailMessage = std::move(httpRes.detailMessage);
        result.actionAdvice = std::move(httpRes.actionAdvice);
        result.errorMessage = std::move(httpRes.errorMessage);
        finishTask(true);
        return;
    }

    std::string responseBody = std::move(httpRes.body);

    // Success (HTTP 200): Parse Image Data (Base64 or URL) or Text Content
    yyjson_doc* respDoc = yyjson_read(responseBody.data(), responseBody.size(), 0);
    if (respDoc) {
        yyjson_val* rRoot = yyjson_doc_get_root(respDoc);
        if (rRoot) {
            if (ExtractImageFromYyjson(rRoot, result.resultImageData)) {
                result.success = true;
            } else {
                yyjson_val* vCandidates = yyjson_obj_get(rRoot, "candidates");
                if (vCandidates && yyjson_is_arr(vCandidates) && yyjson_arr_size(vCandidates) > 0) {
                    yyjson_val* firstCand = yyjson_arr_get(vCandidates, 0);
                    yyjson_val* cContent = yyjson_obj_get(firstCand, "content");
                    if (cContent) {
                        yyjson_val* cParts = yyjson_obj_get(cContent, "parts");
                        if (cParts && yyjson_is_arr(cParts)) {
                            size_t pIdx, pMax;
                            yyjson_val* part;
                            yyjson_arr_foreach(cParts, pIdx, pMax, part) {
                                yyjson_val* tVal = yyjson_obj_get(part, "text");
                                if (tVal && yyjson_is_str(tVal)) {
                                    result.textContent = Utf8ToWide(yyjson_get_str(tVal));
                                    result.success = true;
                                    break;
                                }
                            }
                        }
                    }
                }
            }

            if (!result.success && isAnthropic) {
                yyjson_val* vContent = yyjson_obj_get(rRoot, "content");
                if (vContent && yyjson_is_arr(vContent) && yyjson_arr_size(vContent) > 0) {
                    yyjson_val* firstBlock = yyjson_arr_get(vContent, 0);
                    yyjson_val* vText = yyjson_obj_get(firstBlock, "text");
                    if (vText && yyjson_is_str(vText)) {
                        result.textContent = Utf8ToWide(yyjson_get_str(vText));
                        result.success = true;
                    }
                }
            }

            if (!result.success) {
                yyjson_val* vChoices = yyjson_obj_get(rRoot, "choices");
                if (vChoices && yyjson_is_arr(vChoices) && yyjson_arr_size(vChoices) > 0) {
                    yyjson_val* firstChoice = yyjson_arr_get(vChoices, 0);
                    yyjson_val* msgObj = yyjson_obj_get(firstChoice, "message");
                    if (msgObj) {
                        yyjson_val* cVal = yyjson_obj_get(msgObj, "content");
                        if (cVal && yyjson_is_str(cVal)) {
                            const char* cStr = yyjson_get_str(cVal);
                            std::string contentStr = cStr ? cStr : "";

                            size_t b64Pos = contentStr.find(";base64,");
                            if (b64Pos != std::string::npos) {
                                size_t startPos = b64Pos + 8;
                                size_t endPos = contentStr.find_first_of(")\"'\r\n \t", startPos);
                                std::string subB64 = (endPos == std::string::npos) ? contentStr.substr(startPos) : contentStr.substr(startPos, endPos - startPos);

                                if (Base64ToBinary(subB64.c_str(), subB64.size(), result.resultImageData)) {
                                    result.success = true;
                                }
                            }

                            if (!result.success && !contentStr.empty()) {
                                result.textContent = Utf8ToWide(contentStr);
                                result.success = true;
                            }
                        } else if (cVal && yyjson_is_arr(cVal)) {
                            size_t cIdx = 0, cMax = 0;
                            yyjson_val* block = nullptr;
                            yyjson_arr_foreach(cVal, cIdx, cMax, block) {
                                if (!yyjson_is_obj(block)) continue;
                                yyjson_val* imgUrlObj = yyjson_obj_get(block, "image_url");
                                if (imgUrlObj && yyjson_is_obj(imgUrlObj)) {
                                    std::string uStr;
                                    if (ReadJsonStr(imgUrlObj, "url", uStr)) {
                                        if (uStr.find(";base64,") != std::string::npos) {
                                            if (Base64ToBinary(uStr.c_str(), uStr.size(), result.resultImageData)) {
                                                result.success = true;
                                                break;
                                            }
                                        } else if (DownloadImageFromUrl(Utf8ToWide(uStr), result.resultImageData)) {
                                            result.success = true;
                                            break;
                                        }
                                    }
                                }
                                std::string txt;
                                if (ReadJsonStr(block, "text", txt) && !txt.empty()) {
                                    if (!result.textContent.empty()) result.textContent += L"\n";
                                    result.textContent += Utf8ToWide(txt);
                                    result.success = true;
                                }
                            }
                        }
                    }
                }
            }
        }
        yyjson_doc_free(respDoc);
    }

    if (!result.success) {
        result.errorMessage = AppStrings::g.AiError_NoValidDataReturned;
        result.rawResponseBody = std::move(responseBody);
    }

    finishTask(true);
}

void AiActionManager::FetchModelsAsync(
    std::string baseUrl,
    std::string apiKey,
    ApiProtocol protocol,
    ModelsCallback onComplete)
{
    QuickView::RunDetached([baseUrl = std::move(baseUrl), apiKey = std::move(apiKey), protocol, onComplete = std::move(onComplete)]() mutable {
        std::vector<std::string> models;

        if (baseUrl.empty()) {
            if (onComplete) onComplete(false, {}, AppStrings::g.AiError_InvalidUrl);
            return;
        }

        URL_COMPONENTS urlComp{};
        urlComp.dwStructSize = sizeof(urlComp);
        urlComp.dwSchemeLength = static_cast<DWORD>(-1);
        urlComp.dwHostNameLength = static_cast<DWORD>(-1);
        urlComp.dwUrlPathLength = static_cast<DWORD>(-1);

        std::wstring wUrl = Utf8ToWide(baseUrl);
        if (!WinHttpCrackUrl(wUrl.c_str(), static_cast<DWORD>(wUrl.length()), 0, &urlComp)) {
            if (onComplete) onComplete(false, {}, AppStrings::g.AiError_InvalidUrl);
            return;
        }

        std::wstring host(urlComp.lpszHostName, urlComp.dwHostNameLength);
        std::wstring path(urlComp.lpszUrlPath, urlComp.dwUrlPathLength);
        if (path.empty() || path.back() != L'/') path += L'/';

        bool isAnthropic = (host.find(L"api.anthropic.com") != std::wstring::npos);

        ApiProtocol effectiveProtocol = protocol;

        if (effectiveProtocol == ApiProtocol::StabilityInpaint) {
            size_t pos = path.find(L"sdapi");
            if (pos != std::wstring::npos) {
                path = path.substr(0, pos);
            }
            if (path.empty() || path.back() != L'/') path += L'/';
            path += L"sdapi/v1/sd-models";
        } else if (effectiveProtocol == ApiProtocol::ComfyUI) {
            size_t pos = path.find(L"object_info");
            if (pos != std::wstring::npos) {
                path = path.substr(0, pos);
            }
            if (path.empty() || path.back() != L'/') path += L'/';
            path += L"object_info/CheckpointLoaderSimple";
        } else if (effectiveProtocol == ApiProtocol::GeminiNative) {
            size_t openaiPos = path.find(L"openai");
            if (openaiPos != std::wstring::npos) {
                path = path.substr(0, openaiPos);
            }
            size_t modelsPos = path.find(L"models");
            if (modelsPos != std::wstring::npos) {
                path = path.substr(0, modelsPos);
            }
            if (path.empty() || path.back() != L'/') path += L'/';
            if (path == L"/") {
                path = L"/v1beta/";
            }
            path += L"models";
        } else {
            if (path.empty() || path.back() != L'/') path += L'/';
            if (path.find(L"models") == std::wstring::npos) {
                path += L"models";
            }
        }

        ScopedWinHttpHandle hSession(WinHttpOpen(L"QuickView-AI-Fetch/2.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        if (!hSession) {
            if (onComplete) onComplete(false, {}, AppStrings::g.AiError_InitWinHttpFailed);
            return;
        }

        ScopedWinHttpHandle hConnect(WinHttpConnect(hSession.h, host.c_str(), urlComp.nPort, 0));
        if (!hConnect) {
            if (onComplete) onComplete(false, {}, AppStrings::g.AiError_ConnectFailed);
            return;
        }

        DWORD flags = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
        ScopedWinHttpHandle hReq(WinHttpOpenRequest(hConnect.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
        if (!hReq) {
            if (onComplete) onComplete(false, {}, AppStrings::g.AiError_CreateReqFailed);
            return;
        }

        // 15s timeout
        WinHttpSetTimeouts(hReq.h, 5000, 5000, 15000, 15000);

        std::wstring headers;
        if (isAnthropic) {
            if (!apiKey.empty()) {
                headers = L"x-api-key: " + Utf8ToWide(apiKey) + L"\r\n";
            }
            headers += L"anthropic-version: 2023-06-01\r\n";
        } else if (effectiveProtocol == ApiProtocol::GeminiNative) {
            if (!apiKey.empty()) {
                headers = L"x-goog-api-key: " + Utf8ToWide(apiKey) + L"\r\n";
            }
        } else if (!apiKey.empty()) {
            headers = L"Authorization: Bearer " + Utf8ToWide(apiKey) + L"\r\n";
        }

        BOOL sent = WinHttpSendRequest(hReq.h,
            headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
            static_cast<DWORD>(headers.length()),
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0);

        if (!sent || !WinHttpReceiveResponse(hReq.h, nullptr)) {
            if (onComplete) onComplete(false, {}, AppStrings::g.AiError_RequestTimeout);
            return;
        }

        DWORD statusCode = 0;
        DWORD dwSize = sizeof(statusCode);
        WinHttpQueryHeaders(hReq.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &dwSize, WINHTTP_NO_HEADER_INDEX);

        std::string respBody;
        DWORD bytesAvailable = 0;
        while (WinHttpQueryDataAvailable(hReq.h, &bytesAvailable) && bytesAvailable > 0) {
            std::vector<char> buf(bytesAvailable);
            DWORD bytesRead = 0;
            if (WinHttpReadData(hReq.h, buf.data(), bytesAvailable, &bytesRead) && bytesRead > 0) {
                respBody.append(buf.data(), bytesRead);
            } else {
                break;
            }
        }

        if (statusCode != 200) {
            if (onComplete) onComplete(false, {}, std::wstring(AppStrings::g.AiError_HttpStatusPrefix) + std::to_wstring(statusCode) + L")");
            return;
        }

        // Parse JSON via yyjson
        yyjson_doc* doc = yyjson_read(respBody.c_str(), respBody.size(), 0);
        if (!doc) {
            if (onComplete) onComplete(false, {}, AppStrings::g.AiError_JsonParseFailed);
            return;
        }

        yyjson_val* root = yyjson_doc_get_root(doc);
        if (root) {
            if (effectiveProtocol == ApiProtocol::StabilityInpaint && yyjson_is_arr(root)) {
                // SD WebUI / Forge format: array of objects with "title" or "model_name"
                size_t idx, max = yyjson_arr_size(root);
                yyjson_val* item;
                yyjson_arr_foreach(root, idx, max, item) {
                    if (yyjson_is_obj(item)) {
                        yyjson_val* tVal = yyjson_obj_get(item, "title");
                        if (!tVal || !yyjson_is_str(tVal)) {
                            tVal = yyjson_obj_get(item, "model_name");
                        }
                        if (tVal && yyjson_is_str(tVal)) {
                            std::string mName = yyjson_get_str(tVal);
                            // Clean up optional trailing hash like " [879db523c3]" for clean, user-friendly display
                            size_t bracketPos = mName.rfind(" [");
                            if (bracketPos != std::string::npos && mName.back() == ']') {
                                mName = mName.substr(0, bracketPos);
                            }
                            models.emplace_back(std::move(mName));
                        }
                    }
                }
            } else if (effectiveProtocol == ApiProtocol::ComfyUI) {
                yyjson_val* cpNode = yyjson_obj_get(root, "CheckpointLoaderSimple");
                if (cpNode) {
                    yyjson_val* inObj = yyjson_obj_get(cpNode, "input");
                    if (inObj) {
                        yyjson_val* reqObj = yyjson_obj_get(inObj, "required");
                        if (reqObj) {
                            yyjson_val* ckptArr = yyjson_obj_get(reqObj, "ckpt_name");
                            if (ckptArr && yyjson_is_arr(ckptArr) && yyjson_arr_size(ckptArr) > 0) {
                                yyjson_val* namesArr = yyjson_arr_get(ckptArr, 0);
                                if (namesArr && yyjson_is_arr(namesArr)) {
                                    size_t idx, max = yyjson_arr_size(namesArr);
                                    yyjson_val* item;
                                    yyjson_arr_foreach(namesArr, idx, max, item) {
                                        if (yyjson_is_str(item)) {
                                            models.emplace_back(yyjson_get_str(item));
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            } else {
                yyjson_val* dataArr = yyjson_obj_get(root, "data");
                if (!dataArr || !yyjson_is_arr(dataArr)) {
                    dataArr = yyjson_obj_get(root, "models");
                }

                if (dataArr && yyjson_is_arr(dataArr)) {
                    size_t idx, max;
                    yyjson_val* item;
                    yyjson_arr_foreach(dataArr, idx, max, item) {
                        yyjson_val* idVal = yyjson_obj_get(item, "id");
                        if (!idVal) idVal = yyjson_obj_get(item, "name");
                        if (idVal && yyjson_is_str(idVal)) {
                            std::string idStr = yyjson_get_str(idVal);
                            if (idStr.rfind("models/", 0) == 0) {
                                idStr = idStr.substr(7);
                            }
                            models.push_back(std::move(idStr));
                        }
                    }
                }
            }
        }
        yyjson_doc_free(doc);

        std::sort(models.begin(), models.end());
        models.erase(std::unique(models.begin(), models.end()), models.end());

        if (models.empty()) {
            if (onComplete) onComplete(false, {}, AppStrings::g.AiError_NoModelsFound);
        } else {
            if (onComplete) onComplete(true, models, L"");
        }
    });
}

void AiActionManager::TestConnectionAsync(
    std::string baseUrl,
    std::string apiKey,
    ApiProtocol protocol,
    LatencyCallback onComplete)
{
    QuickView::RunDetached([baseUrl = std::move(baseUrl), apiKey = std::move(apiKey), protocol, onComplete = std::move(onComplete)]() mutable {
        if (baseUrl.empty()) {
            if (onComplete) onComplete(false, 0, 0, AppStrings::g.AiError_InvalidUrl);
            return;
        }

        URL_COMPONENTS urlComp{};
        urlComp.dwStructSize = sizeof(urlComp);
        urlComp.dwSchemeLength = static_cast<DWORD>(-1);
        urlComp.dwHostNameLength = static_cast<DWORD>(-1);
        urlComp.dwUrlPathLength = static_cast<DWORD>(-1);

        std::wstring wUrl = Utf8ToWide(baseUrl);
        if (!WinHttpCrackUrl(wUrl.c_str(), static_cast<DWORD>(wUrl.length()), 0, &urlComp)) {
            if (onComplete) onComplete(false, 0, 0, AppStrings::g.AiError_InvalidUrl);
            return;
        }

        std::wstring host(urlComp.lpszHostName, urlComp.dwHostNameLength);
        std::wstring path(urlComp.lpszUrlPath, urlComp.dwUrlPathLength);
        if (path.empty() || path.back() != L'/') path += L'/';

        bool isAnthropic = (host.find(L"api.anthropic.com") != std::wstring::npos);

        ApiProtocol effectiveProtocol = protocol;

        if (effectiveProtocol == ApiProtocol::StabilityInpaint) {
            size_t pos = path.find(L"sdapi");
            if (pos != std::wstring::npos) {
                path = path.substr(0, pos);
            }
            if (path.empty() || path.back() != L'/') path += L'/';
            path += L"sdapi/v1/sd-models";
        } else if (effectiveProtocol == ApiProtocol::ComfyUI) {
            size_t pos = path.find(L"system_stats");
            if (pos != std::wstring::npos) {
                path = path.substr(0, pos);
            }
            if (path.empty() || path.back() != L'/') path += L'/';
            path += L"system_stats";
        } else if (effectiveProtocol == ApiProtocol::GeminiNative) {
            size_t openaiPos = path.find(L"openai");
            if (openaiPos != std::wstring::npos) {
                path = path.substr(0, openaiPos);
            }
            size_t modelsPos = path.find(L"models");
            if (modelsPos != std::wstring::npos) {
                path = path.substr(0, modelsPos);
            }
            if (path.empty() || path.back() != L'/') path += L'/';
            if (path == L"/") {
                path = L"/v1beta/";
            }
            path += L"models";
        } else {
            if (path.empty() || path.back() != L'/') path += L'/';
            if (path.find(L"models") == std::wstring::npos) {
                path += L"models";
            }
        }

        auto startTime = std::chrono::steady_clock::now();

        ScopedWinHttpHandle hSession(WinHttpOpen(L"QuickView-AI-Probe/2.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        if (!hSession) {
            if (onComplete) onComplete(false, 0, 0, AppStrings::g.AiError_InitWinHttpFailed);
            return;
        }

        ScopedWinHttpHandle hConnect(WinHttpConnect(hSession.h, host.c_str(), urlComp.nPort, 0));
        if (!hConnect) {
            if (onComplete) onComplete(false, 0, 0, AppStrings::g.AiError_ConnectFailed);
            return;
        }

        DWORD flags = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
        ScopedWinHttpHandle hReq(WinHttpOpenRequest(hConnect.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
        if (!hReq) {
            if (onComplete) onComplete(false, 0, 0, AppStrings::g.AiError_CreateReqFailed);
            return;
        }

        // 10s probe timeout
        WinHttpSetTimeouts(hReq.h, 3000, 4000, 10000, 10000);

        std::wstring headers;
        if (isAnthropic) {
            if (!apiKey.empty()) {
                headers = L"x-api-key: " + Utf8ToWide(apiKey) + L"\r\n";
            }
            headers += L"anthropic-version: 2023-06-01\r\n";
        } else if (effectiveProtocol == ApiProtocol::GeminiNative) {
            if (!apiKey.empty()) {
                headers = L"x-goog-api-key: " + Utf8ToWide(apiKey) + L"\r\n";
            }
        } else if (!apiKey.empty()) {
            headers = L"Authorization: Bearer " + Utf8ToWide(apiKey) + L"\r\n";
        }

        BOOL sent = WinHttpSendRequest(hReq.h,
            headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
            static_cast<DWORD>(headers.length()),
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0);

        if (!sent || !WinHttpReceiveResponse(hReq.h, nullptr)) {
            if (onComplete) onComplete(false, 0, 0, AppStrings::g.AiError_RequestTimeout);
            return;
        }

        auto endTime = std::chrono::steady_clock::now();
        int latencyMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count());

        DWORD statusCode = 0;
        DWORD dwSize = sizeof(statusCode);
        WinHttpQueryHeaders(hReq.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &dwSize, WINHTTP_NO_HEADER_INDEX);

        if (statusCode >= 200 && statusCode < 300) {
            if (onComplete) onComplete(true, static_cast<int>(statusCode), latencyMs, AppStrings::g.AiTest_ConnSuccess);
        } else if (statusCode == 401 || statusCode == 403) {
            if (onComplete) onComplete(false, static_cast<int>(statusCode), latencyMs, std::wstring(AppStrings::g.AiTest_AuthFailedPrefix) + std::to_wstring(statusCode) + L")");
        } else {
            if (onComplete) onComplete(false, static_cast<int>(statusCode), latencyMs, std::wstring(AppStrings::g.AiTest_HttpErrorPrefix) + std::to_wstring(statusCode));
        }
    });
}

bool AiActionManager::PollSdProgress(std::string_view baseUrl, SdProgressInfo& outInfo) {
    if (baseUrl.empty()) return false;
    URL_COMPONENTS urlComp{};
    urlComp.dwStructSize = sizeof(urlComp);
    urlComp.dwHostNameLength = static_cast<DWORD>(-1);
    urlComp.dwUrlPathLength = static_cast<DWORD>(-1);

    std::wstring wUrl = Utf8ToWide(baseUrl);
    if (!WinHttpCrackUrl(wUrl.c_str(), static_cast<DWORD>(wUrl.length()), 0, &urlComp)) {
        return false;
    }

    std::wstring host(urlComp.lpszHostName, urlComp.dwHostNameLength);
    std::wstring path(urlComp.lpszUrlPath, urlComp.dwUrlPathLength);
    if (path.empty() || path.back() != L'/') path += L'/';
    path += L"sdapi/v1/progress?skip_current_image=true";

    ScopedWinHttpHandle hSession(WinHttpOpen(L"QuickView-Poll/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!hSession) return false;

    // Fast timeouts for polling: 500ms resolve, 500ms connect, 500ms send, 1000ms receive
    WinHttpSetTimeouts(hSession.h, 500, 500, 500, 1000);

    ScopedWinHttpHandle hConnect(WinHttpConnect(hSession.h, host.c_str(), urlComp.nPort, 0));
    if (!hConnect) return false;

    DWORD flags = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    ScopedWinHttpHandle hReq(WinHttpOpenRequest(hConnect.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!hReq) return false;

    BOOL sent = WinHttpSendRequest(hReq.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (!sent || !WinHttpReceiveResponse(hReq.h, nullptr)) return false;

    DWORD statusCode = 0;
    DWORD dwSize = sizeof(statusCode);
    WinHttpQueryHeaders(hReq.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &dwSize, WINHTTP_NO_HEADER_INDEX);
    if (statusCode != 200) return false;

    std::string resp;
    DWORD dwDownloaded = 0;
    char buf[2048];
    while (WinHttpReadData(hReq.h, buf, sizeof(buf), &dwDownloaded) && dwDownloaded > 0) {
        resp.append(buf, dwDownloaded);
        if (resp.size() > 65536) break; // sanity limit
    }

    if (resp.empty()) return false;

    yyjson_doc* doc = yyjson_read(resp.data(), resp.size(), 0);
    if (!doc) return false;
    yyjson_val* root = yyjson_doc_get_root(doc);
    if (root && yyjson_is_obj(root)) {
        yyjson_val* vProg = yyjson_obj_get(root, "progress");
        if (vProg && yyjson_is_real(vProg)) {
            outInfo.progress = static_cast<float>(yyjson_get_real(vProg));
        } else if (vProg && yyjson_is_int(vProg)) {
            outInfo.progress = static_cast<float>(yyjson_get_int(vProg));
        }

        yyjson_val* vEta = yyjson_obj_get(root, "eta_relative");
        if (vEta && yyjson_is_real(vEta)) {
            outInfo.eta = static_cast<float>(yyjson_get_real(vEta));
        }

        yyjson_val* vState = yyjson_obj_get(root, "state");
        if (vState && yyjson_is_obj(vState)) {
            yyjson_val* vStep = yyjson_obj_get(vState, "sampling_step");
            if (vStep && yyjson_is_int(vStep)) {
                outInfo.currentStep = yyjson_get_int(vStep);
            }
            yyjson_val* vSteps = yyjson_obj_get(vState, "sampling_steps");
            if (vSteps && yyjson_is_int(vSteps)) {
                outInfo.totalSteps = yyjson_get_int(vSteps);
            }
        }
    }
    yyjson_doc_free(doc);
    return true;
}

static bool LoadActiveImageAsBgra(const std::wstring& filePath, std::vector<uint8_t>& outBgra, uint32_t& outWidth, uint32_t& outHeight, uint32_t& outStride) {
    outWidth = outHeight = outStride = 0;
    if (filePath.empty() || !PathFileExistsW(filePath.c_str())) return false;

    ComPtr<IWICImagingFactory> pFactory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFactory))) || !pFactory) return false;

    ComPtr<IWICBitmapDecoder> pDecoder;
    if (FAILED(pFactory->CreateDecoderFromFilename(filePath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &pDecoder)) || !pDecoder) {
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> pFrame;
    if (FAILED(pDecoder->GetFrame(0, &pFrame)) || !pFrame) {
        return false;
    }

    UINT w = 0, h = 0;
    if (FAILED(pFrame->GetSize(&w, &h)) || w == 0 || h == 0) {
        return false;
    }

    ComPtr<IWICFormatConverter> pConv;
    if (FAILED(pFactory->CreateFormatConverter(&pConv)) || !pConv) {
        return false;
    }

    if (FAILED(pConv->Initialize(pFrame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0f, WICBitmapPaletteTypeCustom))) {
        return false;
    }

    UINT stride = w * 4;
    outBgra.resize(static_cast<size_t>(stride) * h);
    HRESULT hr = pConv->CopyPixels(nullptr, stride, static_cast<UINT>(outBgra.size()), outBgra.data());

    if (FAILED(hr)) {
        outBgra.clear();
        return false;
    }

    outWidth = w;
    outHeight = h;
    outStride = stride;
    return true;
}

static bool DecodeMemoryToBgra(const uint8_t* data, size_t size, std::vector<uint8_t>& outBgra, uint32_t& outW, uint32_t& outH, uint32_t& outStride) {
    outW = outH = outStride = 0;
    if (!data || size == 0 || size > static_cast<size_t>((std::numeric_limits<UINT>::max)())) return false;
    ComPtr<IWICImagingFactory> pFactory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFactory))) || !pFactory) return false;

    ComPtr<IStream> pStream;
    pStream.Attach(SHCreateMemStream(data, static_cast<UINT>(size)));
    if (!pStream) return false;

    ComPtr<IWICBitmapDecoder> pDecoder;
    if (FAILED(pFactory->CreateDecoderFromStream(pStream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &pDecoder)) || !pDecoder) {
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> pFrame;
    if (FAILED(pDecoder->GetFrame(0, &pFrame)) || !pFrame) {
        return false;
    }

    UINT w = 0, h = 0;
    if (FAILED(pFrame->GetSize(&w, &h)) || w == 0 || h == 0) {
        return false;
    }
    ComPtr<IWICFormatConverter> pConv;
    if (FAILED(pFactory->CreateFormatConverter(&pConv)) || !pConv ||
        FAILED(pConv->Initialize(pFrame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0f, WICBitmapPaletteTypeCustom))) {
        return false;
    }

    UINT stride = w * 4;
    outBgra.resize(static_cast<size_t>(stride) * h);
    HRESULT hr = pConv->CopyPixels(nullptr, stride, static_cast<UINT>(outBgra.size()), outBgra.data());

    if (FAILED(hr)) {
        outBgra.clear();
        return false;
    }

    outW = w;
    outH = h;
    outStride = stride;
    return true;
}

uint64_t AiActionManager::ExecuteInpaint(
    int cropL, int cropT, int cropR, int cropB,
    std::wstring_view customPrompt,
    HWND hwnd,
    ActionCallback onComplete,
    const ActionDesc* sourceAction) {

    CancelCurrentTask();

    // Strictly respect the profile: if sourceAction specifies one, use it, else default profile
    const ModelProfile* profile = nullptr;
    if (sourceAction && !sourceAction->modelProfileId.empty()) {
        profile = FindProfile(sourceAction->modelProfileId);
    }
    if (!profile) {
        profile = GetDefaultProfile();
    }
    if (!profile) {
        for (const auto& p : m_profiles) {
            if (p.protocol == ApiProtocol::GeminiNative || p.protocol == ApiProtocol::StabilityInpaint || p.protocol == ApiProtocol::OpenAiImagesEdit) {
                profile = &p;
                break;
            }
        }
    }
    if (!profile && !m_profiles.empty()) {
        profile = &m_profiles[0];
    }

    if (!profile) {
        if (onComplete) {
            ExecutionResult err;
            err.success = false;
            err.errorMessage = AppStrings::g.AiError_NoAvailableProfile;
            onComplete(err);
        }
        return 0;
    }

    uint64_t taskId = ++m_currentTaskId;
    m_isRunning.store(true);

    VisualFrameSnapshot snapshot;
    if (!s_visualSnapshotFn || !s_visualSnapshotFn(snapshot)) {
        snapshot.imagePath = GetCurrentActiveImagePath();
    }

    ModelProfile profCopy = *profile;
    std::wstring promptCopy(customPrompt);
    std::optional<ActionDesc> actionOpt;
    if (sourceAction) actionOpt = *sourceAction;

    QuickView::RunDetached([this, taskId, snapshot = std::move(snapshot), cropL, cropT, cropR, cropB, promptCopy, profCopy, hwnd, actionOpt, onComplete = std::move(onComplete)]() mutable {
        InpaintWorkerThread(taskId, std::move(snapshot), cropL, cropT, cropR, cropB, promptCopy, profCopy, hwnd, std::move(onComplete), actionOpt);
    });

    return taskId;
}

void AiActionManager::InpaintWorkerThread(
    uint64_t taskId, VisualFrameSnapshot snapshot, int cropL, int cropT, int cropR, int cropB,
    std::wstring prompt, ModelProfile profile, HWND /*hwnd*/,
    ActionCallback callback,
    std::optional<ActionDesc> sourceAction) {

    ScopedComInit comGuard;
    struct HttpHandleGuard {
        AiActionManager* mgr;
        uint64_t tid;
        ~HttpHandleGuard() { mgr->CloseActiveHttpHandlesIfOwner(tid); }
    } httpGuard{ this, taskId };

    ExecutionResult result;
    result.success = false;
    result.taskId = taskId;
    result.sourceImagePath = snapshot.imagePath;

    auto finishTask = [&](bool invokeCb) {
        if (taskId == m_currentTaskId.load()) {
            m_isRunning.store(false);
            if (invokeCb && callback) {
                callback(result);
            }
        }
    };

    if (taskId != m_currentTaskId.load()) {
        return;
    }

    // 1. Load active visual image as raw BGRA (respecting EXIF orientation, transforms, crops, and prior AI edits)
    std::vector<uint8_t> origBgra;
    uint32_t origW = 0, origH = 0, origStride = 0;
    bool loadedVisual = false;
    if (s_visualRenderFn) {
        loadedVisual = s_visualRenderFn(snapshot, origBgra, origW, origH, origStride);
    }
    if (!loadedVisual || origBgra.empty()) {
        loadedVisual = LoadActiveImageAsBgra(snapshot.imagePath, origBgra, origW, origH, origStride);
    }
    if (!loadedVisual || origBgra.empty() || origW == 0 || origH == 0) {
        result.errorMessage = L"读取当前图像像素数据失败";
        finishTask(true);
        return;
    }

    // 2. Normalize Selection Coordinates
    int selX0 = std::clamp((std::min)(cropL, cropR), 0, static_cast<int>(origW));
    int selY0 = std::clamp((std::min)(cropT, cropB), 0, static_cast<int>(origH));
    int selX1 = std::clamp((std::max)(cropL, cropR), 0, static_cast<int>(origW));
    int selY1 = std::clamp((std::max)(cropT, cropB), 0, static_cast<int>(origH));
    int selW = selX1 - selX0;
    int selH = selY1 - selY0;

    if (selW < 8 || selH < 8) {
        result.errorMessage = L"选区尺寸过小，请重新框选";
        finishTask(true);
        return;
    }

    // 3. Smart Context Margin (cover 100% of selection + round UP to 8px multiple within image bounds)
    int padX = (std::max)(48, selW / 3);
    int padY = (std::max)(48, selH / 3);
    int sliceX0 = (std::max)(0, selX0 - padX);
    int sliceY0 = (std::max)(0, selY0 - padY);
    int sliceX1 = (std::min)(static_cast<int>(origW), selX1 + padX);
    int sliceY1 = (std::min)(static_cast<int>(origH), selY1 + padY);

    auto alignSliceSpan = [](int sel0, int sel1, int imgDim, int& s0, int& s1) {
        int rawSpan = s1 - s0;
        int aligned = ((rawSpan + 7) / 8) * 8;
        if (aligned <= imgDim) {
            int extra = aligned - rawSpan;
            int addRight = (std::min)(extra, imgDim - s1);
            s1 += addRight;
            extra -= addRight;
            int addLeft = (std::min)(extra, s0);
            s0 -= addLeft;
            if (s1 - s0 > aligned) {
                s1 = s0 + aligned;
            }
        } else {
            s0 = 0;
            s1 = imgDim;
        }
        // Guarantee selection is strictly contained inside the slice
        if (s0 > sel0) s0 = sel0;
        if (s1 < sel1) s1 = sel1;
    };

    alignSliceSpan(selX0, selX1, static_cast<int>(origW), sliceX0, sliceX1);
    alignSliceSpan(selY0, selY1, static_cast<int>(origH), sliceY0, sliceY1);

    int sliceW = sliceX1 - sliceX0;
    int sliceH = sliceY1 - sliceY0;
    if (sliceW <= 0 || sliceH <= 0) {
        result.errorMessage = L"生成选区切片尺寸无效";
        finishTask(true);
        return;
    }

    const bool isGemini = (profile.protocol == ApiProtocol::GeminiNative);
    const bool isSd = (profile.protocol == ApiProtocol::StabilityInpaint);
    const bool isOpenAiChat = (profile.protocol == ApiProtocol::OpenAiChat || profile.protocol == ApiProtocol::CustomJsonPost);
    const bool isOpenAiTransparentMask = (!isGemini && !isSd && !isOpenAiChat);

    // 4. Extract Slice Image and Generate Binary Mask
    std::vector<uint8_t> sliceBgra(static_cast<size_t>(sliceW) * sliceH * 4, 0);
    std::vector<uint8_t> maskBgra(static_cast<size_t>(sliceW) * sliceH * 4, 0);

    for (int y = 0; y < sliceH; ++y) {
        int srcY = sliceY0 + y;
        const uint8_t* srcRow = origBgra.data() + static_cast<size_t>(srcY) * origStride;
        uint8_t* sliceRow = sliceBgra.data() + static_cast<size_t>(y) * sliceW * 4;
        uint8_t* maskRow = maskBgra.data() + static_cast<size_t>(y) * sliceW * 4;

        for (int x = 0; x < sliceW; ++x) {
            int srcX = sliceX0 + x;
            const uint8_t* sPx = srcRow + static_cast<size_t>(srcX) * 4;
            uint8_t* dSlicePx = sliceRow + static_cast<size_t>(x) * 4;
            uint8_t* dMaskPx = maskRow + static_cast<size_t>(x) * 4;

            dSlicePx[0] = sPx[0];
            dSlicePx[1] = sPx[1];
            dSlicePx[2] = sPx[2];
            dSlicePx[3] = sPx[3];

            const bool insideSel = (srcX >= selX0 && srcX < selX1 && srcY >= selY0 && srcY < selY1);
            if (isOpenAiTransparentMask) {
                // OpenAI /images/edits uses alpha == 0 for the editable area and alpha == 255 to preserve
                dMaskPx[0] = sPx[0];
                dMaskPx[1] = sPx[1];
                dMaskPx[2] = sPx[2];
                dMaskPx[3] = insideSel ? 0 : 255;
            } else if (insideSel) {
                dMaskPx[0] = 255;
                dMaskPx[1] = 255;
                dMaskPx[2] = 255;
                dMaskPx[3] = 255;
            } else {
                dMaskPx[0] = 0;
                dMaskPx[1] = 0;
                dMaskPx[2] = 0;
                dMaskPx[3] = 255;
            }
        }
    }

    std::vector<uint8_t> slicePngBytes, maskPngBytes;
    if (!EncodeToPngMemory(sliceBgra.data(), sliceW, sliceH, sliceW * 4, slicePngBytes) ||
        !EncodeToPngMemory(maskBgra.data(), sliceW, sliceH, sliceW * 4, maskPngBytes)) {
        result.errorMessage = L"编码选区图像切片失败";
        finishTask(true);
        return;
    }

    std::string sliceB64 = BinaryToBase64(slicePngBytes.data(), slicePngBytes.size());
    std::string maskB64 = BinaryToBase64(maskPngBytes.data(), maskPngBytes.size());

    // 5. Build Hyperparameters & Prompts
    int actualSteps = 25;
    double actualCfg = 7.0;
    double actualDenoise = 0.75;
    std::string utf8Negative = "text, watermark, logo, signature, letters, numbers, blur, smear, artifacts, boundary lines, seam, defect";

    if (sourceAction.has_value()) {
        if (sourceAction->samplingSteps >= 5 && sourceAction->samplingSteps <= 150) {
            actualSteps = sourceAction->samplingSteps;
        }
        if (sourceAction->cfgScale >= 1.0f && sourceAction->cfgScale <= 30.0f) {
            actualCfg = static_cast<double>(sourceAction->cfgScale);
        }
        if (sourceAction->denoisingStrength > 0.0f && sourceAction->denoisingStrength <= 1.0f) {
            actualDenoise = static_cast<double>(sourceAction->denoisingStrength);
        }
        if (!sourceAction->negativePrompt.empty()) {
            utf8Negative = WideToUtf8(sourceAction->negativePrompt);
        }
    }

    std::string utf8Prompt;
    bool isCustomUserPrompt = !prompt.empty();
    if (isCustomUserPrompt) {
        utf8Prompt = WideToUtf8(prompt);
    } else {
        utf8Prompt = "flawless seamless background fill, natural continuation of texture, pristine clean surface, smooth transition, high quality restoration, uninterrupted surface";
    }

    // 6. Network Dispatch
    bool isLocal = (profile.protocol == ApiProtocol::ComfyUI ||
                    profile.protocol == ApiProtocol::StabilityInpaint ||
                    profile.baseUrl.find("127.0.0.1") != std::string::npos ||
                    profile.baseUrl.find("localhost") != std::string::npos);

    std::string apiKey = DecryptApiKey(profile.encryptedApiKey);
    if (!isLocal && apiKey.empty()) {
        result.errorMessage = AppStrings::g.AiError_ApiKeyEmpty;
        finishTask(true);
        return;
    }

    wchar_t hostName[256] = { 0 };
    INTERNET_PORT port = 0;
    bool isHttps = false;
    std::wstring fullPath;
    if (!CrackEndpointUrl(profile.baseUrl, hostName, port, isHttps, fullPath, result.errorMessage)) {
        finishTask(true);
        return;
    }

    std::string targetModel = profile.defaultModel;
    if (targetModel.empty()) {
        if (isGemini) targetModel = "nano-banana-pro-preview";
        else if (isSd) targetModel = "default";
        else targetModel = "dall-e-2";
    }

    if (isGemini) {
        ResolveGeminiPath(fullPath, targetModel);
    } else if (isSd) {
        ResolveSdPath(fullPath, true);
    } else if (isOpenAiChat) {
        if (fullPath.find(L"chat/completions") == std::wstring::npos) {
            fullPath += L"chat/completions";
        }
    } else {
        if (fullPath.find(L"images/edits") == std::wstring::npos) {
            fullPath += L"images/edits";
        }
    }

    HINTERNET hRequest = OpenAiHttpRequest(taskId, L"QuickView-AI-Inpaint/2.0", hostName, port, isHttps, isLocal, fullPath, profile.timeoutSeconds, result.errorMessage);
    if (!hRequest) {
        if (taskId == m_currentTaskId.load()) {
            finishTask(true);
        }
        return;
    }

    std::wstring headers;
    std::string postPayload;

    std::string inpaintTaskDesc;
    if (isCustomUserPrompt) {
        inpaintTaskDesc = "Task: Inpainting & Generative Fill. Image 1 is the context slice, Image 2 is the binary mask (white = region to modify, black = keep completely unchanged). Modify only the white masked area to match: " +
            utf8Prompt + ". Seamlessly match surrounding lighting, texture, and colors. Output only the complete restored image.";
    } else {
        inpaintTaskDesc = "Task: Object Removal & Inpainting. Image 1 is the context slice, Image 2 is the binary mask (white = region to remove/fill, black = keep completely unchanged). Remove what is inside the white masked region and fill seamlessly matching surrounding background texture and lighting. Output only the complete restored image.";
    }

    if (isGemini) {
        headers = L"Content-Type: application/json\r\n";
        if (!apiKey.empty()) {
            headers += L"x-goog-api-key: " + Utf8ToWide(apiKey) + L"\r\n";
        }

        yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
        yyjson_mut_val* root = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, root);

        yyjson_mut_val* contentsArr = yyjson_mut_arr(doc);
        yyjson_mut_val* contentItem = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, contentItem, "role", "user");

        yyjson_mut_val* partsArr = yyjson_mut_arr(doc);

        yyjson_mut_val* textPart = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, textPart, "text", inpaintTaskDesc.c_str());
        yyjson_mut_arr_append(partsArr, textPart);

        yyjson_mut_val* imgPart = yyjson_mut_obj(doc);
        yyjson_mut_val* imgData = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, imgData, "mimeType", "image/png");
        yyjson_mut_obj_add_str(doc, imgData, "data", sliceB64.c_str());
        yyjson_mut_obj_add_val(doc, imgPart, "inlineData", imgData);
        yyjson_mut_arr_append(partsArr, imgPart);

        yyjson_mut_val* maskPart = yyjson_mut_obj(doc);
        yyjson_mut_val* maskData = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, maskData, "mimeType", "image/png");
        yyjson_mut_obj_add_str(doc, maskData, "data", maskB64.c_str());
        yyjson_mut_obj_add_val(doc, maskPart, "inlineData", maskData);
        yyjson_mut_arr_append(partsArr, maskPart);

        yyjson_mut_obj_add_val(doc, contentItem, "parts", partsArr);
        yyjson_mut_arr_append(contentsArr, contentItem);
        yyjson_mut_obj_add_val(doc, root, "contents", contentsArr);

        yyjson_mut_val* genConfig = yyjson_mut_obj(doc);
        yyjson_mut_val* respModalities = yyjson_mut_arr(doc);
        yyjson_mut_arr_add_strcpy(doc, respModalities, "IMAGE");
        yyjson_mut_obj_add_val(doc, genConfig, "responseModalities", respModalities);

        yyjson_mut_val* imgConfig = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, imgConfig, "aspectRatio",
            SelectClosestGeminiAspectRatio(OutputAspectRatio::Auto, static_cast<uint32_t>(sliceW), static_cast<uint32_t>(sliceH)));
        yyjson_mut_obj_add_val(doc, genConfig, "imageConfig", imgConfig);
        yyjson_mut_obj_add_val(doc, root, "generationConfig", genConfig);

        size_t pLen = 0;
        char* pStr = yyjson_mut_write(doc, 0, &pLen);
        yyjson_mut_doc_free(doc);
        if (!pStr) {
            result.errorMessage = AppStrings::g.AiError_JsonParseFailed;
            finishTask(true);
            return;
        }
        postPayload.assign(pStr, pLen);
        free(pStr);
    } else if (isSd) {
        headers = L"Content-Type: application/json\r\n";
        yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
        yyjson_mut_val* root = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, root);

        yyjson_mut_obj_add_str(doc, root, "prompt", utf8Prompt.c_str());
        yyjson_mut_obj_add_str(doc, root, "negative_prompt", utf8Negative.c_str());
        yyjson_mut_obj_add_int(doc, root, "steps", actualSteps);
        yyjson_mut_obj_add_real(doc, root, "cfg_scale", actualCfg);
        yyjson_mut_obj_add_real(doc, root, "denoising_strength", actualDenoise);
        yyjson_mut_obj_add_int(doc, root, "mask_blur", 4);
        yyjson_mut_obj_add_int(doc, root, "inpainting_fill", 1);
        yyjson_mut_obj_add_int(doc, root, "inpaint_full_res", 0);
        yyjson_mut_obj_add_int(doc, root, "width", sliceW);
        yyjson_mut_obj_add_int(doc, root, "height", sliceH);

        yyjson_mut_val* initArr = yyjson_mut_arr(doc);
        yyjson_mut_arr_append(initArr, yyjson_mut_str(doc, sliceB64.c_str()));
        yyjson_mut_obj_add_val(doc, root, "init_images", initArr);
        yyjson_mut_obj_add_str(doc, root, "mask", maskB64.c_str());

        size_t pLen = 0;
        char* pStr = yyjson_mut_write(doc, 0, &pLen);
        yyjson_mut_doc_free(doc);
        if (!pStr) {
            result.errorMessage = AppStrings::g.AiError_JsonParseFailed;
            finishTask(true);
            return;
        }
        postPayload.assign(pStr, pLen);
        free(pStr);
    } else if (isOpenAiChat) {
        headers = L"Content-Type: application/json\r\n";
        if (!apiKey.empty()) {
            headers += L"Authorization: Bearer " + Utf8ToWide(apiKey) + L"\r\n";
        }
        yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
        yyjson_mut_val* root = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, root);

        yyjson_mut_obj_add_str(doc, root, "model", targetModel.c_str());
        yyjson_mut_val* msgs = yyjson_mut_arr(doc);
        yyjson_mut_obj_add_val(doc, root, "messages", msgs);

        yyjson_mut_val* userMsg = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, userMsg, "role", "user");
        yyjson_mut_val* contentArr = yyjson_mut_arr(doc);
        yyjson_mut_obj_add_val(doc, userMsg, "content", contentArr);

        auto addText = [&](const std::string& txt) {
            yyjson_mut_val* obj = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, obj, "type", "text");
            yyjson_mut_obj_add_str(doc, obj, "text", txt.c_str());
            yyjson_mut_arr_append(contentArr, obj);
        };
        auto addPngUrl = [&](const std::string& b64) {
            yyjson_mut_val* obj = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, obj, "type", "image_url");
            yyjson_mut_val* urlObj = yyjson_mut_obj(doc);
            std::string url = "data:image/png;base64," + b64;
            yyjson_mut_obj_add_str(doc, urlObj, "url", url.c_str());
            yyjson_mut_obj_add_val(doc, obj, "image_url", urlObj);
            yyjson_mut_arr_append(contentArr, obj);
        };

        addText(inpaintTaskDesc);
        addPngUrl(sliceB64);
        addPngUrl(maskB64);
        yyjson_mut_arr_append(msgs, userMsg);

        size_t pLen = 0;
        char* pStr = yyjson_mut_write(doc, 0, &pLen);
        yyjson_mut_doc_free(doc);
        if (!pStr) {
            result.errorMessage = AppStrings::g.AiError_JsonParseFailed;
            finishTask(true);
            return;
        }
        postPayload.assign(pStr, pLen);
        free(pStr);
    } else {
        std::string boundary = "----QuickViewBoundary7MA4YWxkTrZu0gW";
        headers = L"Content-Type: multipart/form-data; boundary=" + Utf8ToWide(boundary) + L"\r\n";
        if (!apiKey.empty()) {
            headers += L"Authorization: Bearer " + Utf8ToWide(apiKey) + L"\r\n";
        }
        postPayload = BuildOpenAiMultipartEditPayload(boundary, targetModel, utf8Prompt, slicePngBytes, &maskPngBytes);
    }

    auto httpRes = SendRequestAndReadResponse(
        hRequest, headers, postPayload.data(), static_cast<DWORD>(postPayload.size()),
        hostName, port, isLocal, taskId, m_currentTaskId);

    if (httpRes.interrupted) {
        return;
    }

    result.httpStatusCode = httpRes.statusCode;

    if (!httpRes.ok) {
        result.rawResponseBody = std::move(httpRes.body);
        result.mainTitle = std::move(httpRes.mainTitle);
        result.detailMessage = std::move(httpRes.detailMessage);
        result.actionAdvice = std::move(httpRes.actionAdvice);
        result.errorMessage = std::move(httpRes.errorMessage);
        finishTask(true);
        return;
    }

    std::string respBody = std::move(httpRes.body);

    std::vector<uint8_t> aiImgBytes;
    yyjson_doc* doc = yyjson_read(respBody.c_str(), respBody.size(), 0);
    if (doc) {
        yyjson_val* root = yyjson_doc_get_root(doc);
        ExtractImageFromYyjson(root, aiImgBytes);
        if (aiImgBytes.empty() && isOpenAiChat && root) {
            yyjson_val* vChoices = yyjson_obj_get(root, "choices");
            if (vChoices && yyjson_is_arr(vChoices) && yyjson_arr_size(vChoices) > 0) {
                yyjson_val* msgObj = yyjson_obj_get(yyjson_arr_get(vChoices, 0), "message");
                yyjson_val* cVal = msgObj ? yyjson_obj_get(msgObj, "content") : nullptr;
                if (cVal && yyjson_is_str(cVal)) {
                    const char* cStr = yyjson_get_str(cVal);
                    if (cStr) {
                        std::string contentStr = cStr;
                        size_t b64Pos = contentStr.find(";base64,");
                        if (b64Pos != std::string::npos) {
                            size_t startPos = b64Pos + 8;
                            size_t endPos = contentStr.find_first_of(")\"'\r\n \t", startPos);
                            std::string subB64 = (endPos == std::string::npos) ? contentStr.substr(startPos) : contentStr.substr(startPos, endPos - startPos);
                            Base64ToBinary(subB64.c_str(), subB64.size(), aiImgBytes);
                        }
                    }
                }
            }
        }
        yyjson_doc_free(doc);
    }

    if (aiImgBytes.empty()) {
        result.errorMessage = L"解析 AI 重绘结果图像数据为空";
        result.rawResponseBody = std::move(respBody);
        finishTask(true);
        return;
    }

    // 7. Decode AI sub-image & Seamless Feathered Re-injection
    std::vector<uint8_t> aiSubBgra;
    uint32_t aiW = 0, aiH = 0, aiStride = 0;
    if (!DecodeMemoryToBgra(aiImgBytes.data(), aiImgBytes.size(), aiSubBgra, aiW, aiH, aiStride) || aiSubBgra.empty()) {
        result.errorMessage = L"解码 AI 返回的切片图像失败";
        finishTask(true);
        return;
    }

    std::vector<uint8_t> alignedSubBgra;
    if (static_cast<int>(aiW) != sliceW || static_cast<int>(aiH) != sliceH) {
        alignedSubBgra.resize(static_cast<size_t>(sliceW) * sliceH * 4);
        ResampleBgraExact(aiSubBgra.data(), static_cast<int>(aiW), static_cast<int>(aiH), static_cast<int>(aiStride),
                          alignedSubBgra.data(), sliceW, sliceH, sliceW * 4);
    } else {
        alignedSubBgra = std::move(aiSubBgra);
    }

    // 8. Mask-Guided Feathered Blend strictly onto the user's selection region
    std::vector<uint8_t> mergedBgra = std::move(origBgra);
    BlendMaskGuidedFeathered(
        mergedBgra.data(), static_cast<int>(origW), static_cast<int>(origH), static_cast<int>(origStride),
        alignedSubBgra.data(), sliceX0, sliceY0, sliceW, sliceH, sliceW * 4,
        selX0, selY0, selX1, selY1, 6);

    // 9. Encode full merged frame to PNG
    std::vector<uint8_t> finalMergedPng;
    if (!EncodeToPngMemory(mergedBgra.data(), static_cast<int>(origW), static_cast<int>(origH), static_cast<int>(origStride), finalMergedPng)) {
        result.errorMessage = L"合成最终图像失败";
        finishTask(true);
        return;
    }

    result.success = true;
    result.resultImageData = std::move(finalMergedPng);
    result.imageWidth = origW;
    result.imageHeight = origH;

    finishTask(true);
}

} // namespace QuickView::AI
 
