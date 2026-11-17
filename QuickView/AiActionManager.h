#pragma once
// ============================================================================
// AiActionManager.h - High Performance AI Action & Profile Management Engine
// ============================================================================
// Features:
// 1. Separation of Concerns: Model Profiles vs AI Actions
// 2. DPAPI Security: Windows CryptProtectData for local credentials
// 3. Robust WinHTTP: Asynchronous execution, Task ID tracking, instant cancel/fuse
// 4. Inpainting Preprocessing: Adaptive downsampling & Alpha edge feathering
// 5. Zero-Exception Architecture: std::expected / error codes throughout
// ============================================================================

#include "pch.h"
#include <vector>
#include <string>
#include <optional>
#include <memory>
#include <atomic>
#include <mutex>
#include <type_traits>
#include <winhttp.h>
#include <wincrypt.h>
#include <commctrl.h>
#include "AiActionTypes.h"
#include "AppContext.h"

namespace QuickView::AI {

struct ActionCallback {
    void (*pfn)(void* ctx, const ExecutionResult& res) = nullptr;
    void* ctx = nullptr;
    void (*cleanup)(void* ctx) = nullptr;

    constexpr ActionCallback() noexcept = default;

    template <typename F>
        requires std::is_convertible_v<F, void (*)(const ExecutionResult&)>
    constexpr ActionCallback(F fn) noexcept {
        void (*simpleFn)(const ExecutionResult&) = fn;
        if (simpleFn) {
            ctx = reinterpret_cast<void*>(simpleFn);
            pfn = [](void* c, const ExecutionResult& r) {
                if (c) reinterpret_cast<void (*)(const ExecutionResult&)>(c)(r);
            };
            cleanup = nullptr;
        }
    }

    constexpr ActionCallback(void (*fn)(void*, const ExecutionResult&), void* c, void (*cleanFn)(void*) = nullptr) noexcept
        : pfn(fn), ctx(c), cleanup(cleanFn) {}

    ~ActionCallback() { Reset(); }
    ActionCallback(ActionCallback&& o) noexcept : pfn(o.pfn), ctx(o.ctx), cleanup(o.cleanup) {
        o.pfn = nullptr; o.ctx = nullptr; o.cleanup = nullptr;
    }
    ActionCallback(const ActionCallback&) = delete;
    ActionCallback& operator=(ActionCallback&& o) noexcept {
        if (this != &o) {
            Reset();
            pfn = o.pfn; ctx = o.ctx; cleanup = o.cleanup;
            o.pfn = nullptr; o.ctx = nullptr; o.cleanup = nullptr;
        }
        return *this;
    }
    ActionCallback& operator=(const ActionCallback&) = delete;

    void Invoke(const ExecutionResult& res) const {
        if (pfn) pfn(ctx, res);
    }
    void operator()(const ExecutionResult& res) const {
        Invoke(res);
    }
    void Reset() noexcept {
        if (cleanup && ctx) {
            cleanup(ctx);
            ctx = nullptr;
        }
        pfn = nullptr;
        cleanup = nullptr;
    }
    explicit operator bool() const noexcept { return pfn != nullptr; }
};

struct ModelsCallback {
    void (*pfn)(void* ctx, bool success, const std::vector<std::string>& models, const std::wstring& errorMsg) = nullptr;
    void* ctx = nullptr;
    void (*cleanup)(void* ctx) = nullptr;

    constexpr ModelsCallback() noexcept = default;

    template <typename F>
        requires std::is_convertible_v<F, void (*)(bool, const std::vector<std::string>&, const std::wstring&)>
    constexpr ModelsCallback(F fn) noexcept {
        void (*simpleFn)(bool, const std::vector<std::string>&, const std::wstring&) = fn;
        if (simpleFn) {
            ctx = reinterpret_cast<void*>(simpleFn);
            pfn = [](void* c, bool s, const std::vector<std::string>& m, const std::wstring& e) {
                if (c) reinterpret_cast<void (*)(bool, const std::vector<std::string>&, const std::wstring&)>(c)(s, m, e);
            };
            cleanup = nullptr;
        }
    }

    constexpr ModelsCallback(void (*fn)(void*, bool, const std::vector<std::string>&, const std::wstring&), void* c, void (*cleanFn)(void*) = nullptr) noexcept
        : pfn(fn), ctx(c), cleanup(cleanFn) {}

    ~ModelsCallback() { Reset(); }
    ModelsCallback(ModelsCallback&& o) noexcept : pfn(o.pfn), ctx(o.ctx), cleanup(o.cleanup) {
        o.pfn = nullptr; o.ctx = nullptr; o.cleanup = nullptr;
    }
    ModelsCallback(const ModelsCallback&) = delete;
    ModelsCallback& operator=(ModelsCallback&& o) noexcept {
        if (this != &o) {
            Reset();
            pfn = o.pfn; ctx = o.ctx; cleanup = o.cleanup;
            o.pfn = nullptr; o.ctx = nullptr; o.cleanup = nullptr;
        }
        return *this;
    }
    ModelsCallback& operator=(const ModelsCallback&) = delete;

    void Invoke(bool success, const std::vector<std::string>& models, const std::wstring& errorMsg) const {
        if (pfn) pfn(ctx, success, models, errorMsg);
    }
    void operator()(bool success, const std::vector<std::string>& models, const std::wstring& errorMsg) const {
        Invoke(success, models, errorMsg);
    }
    void Reset() noexcept {
        if (cleanup && ctx) {
            cleanup(ctx);
            ctx = nullptr;
        }
        pfn = nullptr;
        cleanup = nullptr;
    }
    explicit operator bool() const noexcept { return pfn != nullptr; }
};

struct LatencyCallback {
    void (*pfn)(void* ctx, bool success, int statusCode, int latencyMs, const std::wstring& message) = nullptr;
    void* ctx = nullptr;
    void (*cleanup)(void* ctx) = nullptr;

    constexpr LatencyCallback() noexcept = default;

    template <typename F>
        requires std::is_convertible_v<F, void (*)(bool, int, int, const std::wstring&)>
    constexpr LatencyCallback(F fn) noexcept {
        void (*simpleFn)(bool, int, int, const std::wstring&) = fn;
        if (simpleFn) {
            ctx = reinterpret_cast<void*>(simpleFn);
            pfn = [](void* c, bool s, int code, int lat, const std::wstring& m) {
                if (c) reinterpret_cast<void (*)(bool, int, int, const std::wstring&)>(c)(s, code, lat, m);
            };
            cleanup = nullptr;
        }
    }

    constexpr LatencyCallback(void (*fn)(void*, bool, int, int, const std::wstring&), void* c, void (*cleanFn)(void*) = nullptr) noexcept
        : pfn(fn), ctx(c), cleanup(cleanFn) {}

    ~LatencyCallback() { Reset(); }
    LatencyCallback(LatencyCallback&& o) noexcept : pfn(o.pfn), ctx(o.ctx), cleanup(o.cleanup) {
        o.pfn = nullptr; o.ctx = nullptr; o.cleanup = nullptr;
    }
    LatencyCallback(const LatencyCallback&) = delete;
    LatencyCallback& operator=(LatencyCallback&& o) noexcept {
        if (this != &o) {
            Reset();
            pfn = o.pfn; ctx = o.ctx; cleanup = o.cleanup;
            o.pfn = nullptr; o.ctx = nullptr; o.cleanup = nullptr;
        }
        return *this;
    }
    LatencyCallback& operator=(const LatencyCallback&) = delete;

    void Invoke(bool success, int statusCode, int latencyMs, const std::wstring& message) const {
        if (pfn) pfn(ctx, success, statusCode, latencyMs, message);
    }
    void operator()(bool success, int statusCode, int latencyMs, const std::wstring& message) const {
        Invoke(success, statusCode, latencyMs, message);
    }
    void Reset() noexcept {
        if (cleanup && ctx) {
            cleanup(ctx);
            ctx = nullptr;
        }
        pfn = nullptr;
        cleanup = nullptr;
    }
    explicit operator bool() const noexcept { return pfn != nullptr; }
};

class AiActionManager {
public:
    static AiActionManager& Instance();

    // --- Profile & Action Configuration Management ---
    bool Init();
    bool LoadConfig();
    bool SaveConfig();
    bool ReloadConfig();
    void ResetToDefaults();
    std::wstring GetConfigFilePath() const;

    std::vector<ModelProfile>& GetProfiles() { return m_profiles; }
    const std::vector<ModelProfile>& GetProfiles() const { return m_profiles; }

    std::vector<ActionDesc>& GetActions() { return m_actions; }
    const std::vector<ActionDesc>& GetActions() const { return m_actions; }

    const ModelProfile* FindProfile(std::string_view profileId) const;
    const ModelProfile* GetDefaultProfile() const;

    void SetDefaultProfileId(std::string_view id);
    const std::string& GetDefaultProfileId() const { return m_defaultProfileId; }

    void SetLastActionId(std::string_view id);
    const std::string& GetLastActionId() const { return m_lastActionId; }

    // --- Security: Windows DPAPI En/Decryption ---
    static std::string EncryptApiKey(std::string_view plainText);
    static std::string DecryptApiKey(std::string_view cipherBase64);

    static void SetVisualFrameProviders(VisualFrameSnapshotFn snapshotFn, VisualBgraProviderFn renderFn);

    // --- Execution & Task Lifecycle ---
    uint64_t ExecuteAction(
        const ActionDesc& action, HWND hwnd,
        ActionCallback onComplete,
        std::wstring_view customPrompt = L"",
        int cropL = 0, int cropT = 0, int cropR = 0, int cropB = 0);
    uint64_t ExecuteInpaint(
        int cropL, int cropT, int cropR, int cropB,
        std::wstring_view customPrompt,
        HWND hwnd,
        ActionCallback onComplete,
        const ActionDesc* sourceAction = nullptr);
    void CancelCurrentTask();
    bool IsRunning() const { return m_isRunning.load(); }
    uint64_t GetCurrentTaskId() const { return m_currentTaskId.load(); }

    // --- Dynamic Model List Fetching ---
    void FetchModelsAsync(
        std::string baseUrl,
        std::string apiKey,
        ApiProtocol protocol,
        ModelsCallback onComplete);

    // --- Connection Testing & Latency Probing ---
    void TestConnectionAsync(
        std::string baseUrl,
        std::string apiKey,
        ApiProtocol protocol,
        LatencyCallback onComplete);

    // --- Error Formatting & Native Dialog Presentation ---
    static void ExtractSemanticError(
        DWORD statusCode, std::string_view responseBody,
        std::wstring& outTitle, std::wstring& outDetail, std::wstring& outAdvice);
    static std::wstring FormatAiErrorMessage(DWORD statusCode, std::string_view responseBody);
    static void ShowAiErrorDialog(HWND hwndParent, const ExecutionResult& result);

    // --- Stable Diffusion Progress Polling Helper ---
    struct SdProgressInfo {
        float progress = 0.0f;
        int currentStep = 0;
        int totalSteps = 0;
        float eta = 0.0f;
    };
    static bool PollSdProgress(std::string_view baseUrl, SdProgressInfo& outInfo);

    // --- Image Processing & Inpainting Helper ---
    // Encodes BGRA raw pixels into PNG format stream in memory via WIC
    static bool EncodeToPngMemory(
        const uint8_t* bgra, int width, int height, int stride,
        std::vector<uint8_t>& outPngBytes);

    // Resamples BGRA image buffer to exact target width and height with bilinear interpolation
    static void ResampleBgraExact(
        const uint8_t* srcBgra, int srcW, int srcH, int srcStride,
        uint8_t* dstBgra, int dstW, int dstH, int dstStride);

    // Blits generated sub-image back onto original buffer strictly within user selection with cosine edge feathering
    static void BlendMaskGuidedFeathered(
        uint8_t* dstBgra, int dstW, int dstH, int dstStride,
        const uint8_t* subBgra, int sliceX0, int sliceY0, int sliceW, int sliceH, int sliceStride,
        int selX0, int selY0, int selX1, int selY1, int featherPixels = 6);

private:
    AiActionManager();
    ~AiActionManager();

    void InitDefaultTemplates();

    // Worker thread function
    void WorkerThread(uint64_t taskId, VisualFrameSnapshot snapshot, ActionDesc action, ModelProfile profile, HWND hwnd, ActionCallback callback);
    void InpaintWorkerThread(
        uint64_t taskId, VisualFrameSnapshot snapshot, int cropL, int cropT, int cropR, int cropB,
        std::wstring prompt, ModelProfile profile, HWND hwnd,
        ActionCallback callback,
        std::optional<ActionDesc> sourceAction = std::nullopt);

    HINTERNET OpenAiHttpRequest(
        uint64_t taskId,
        const wchar_t* userAgent,
        const wchar_t* hostName,
        INTERNET_PORT port,
        bool isHttps,
        bool isLocal,
        const std::wstring& fullPath,
        int timeoutSeconds,
        std::wstring& outError);
    void CloseActiveHttpHandlesIfOwner(uint64_t taskId);

    std::vector<ModelProfile> m_profiles;
    std::vector<ActionDesc> m_actions;
    std::string m_defaultProfileId;
    std::string m_lastActionId;

    bool m_initialized = false;
    std::atomic<bool> m_isRunning{ false };
    std::atomic<uint64_t> m_currentTaskId{ 0 };
    std::mutex m_taskMutex;
    uint64_t m_activeHttpTaskId = 0;
    HINTERNET m_activeSession = nullptr;
    HINTERNET m_activeConnect = nullptr;
    HINTERNET m_activeRequest = nullptr;
};

} // namespace QuickView::AI
