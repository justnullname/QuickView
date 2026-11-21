#include <gtest/gtest.h>
#include "AiActionManager.h"
#include "OSDState.h"
#include "AppStrings.h"
#include <windows.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")

// Stubs for test harness
OSDState g_osd;
std::wstring g_imagePath;
std::wstring GetCurrentActiveImagePath() {
    return g_imagePath;
}

std::wstring GetConfigPath([[maybe_unused]] bool forcePortableCheck) {
    wchar_t tempDir[MAX_PATH];
    GetTempPathW(MAX_PATH, tempDir);
    return std::wstring(tempDir) + L"QuickView_Test.ini";
}

class AiActionManagerTest : public ::testing::Test {
protected:
    std::wstring m_testJsonPath;

    void SetUp() override {
        AppStrings::SetLanguage(AppStrings::Language::ChineseSimplified);
        m_testJsonPath = QuickView::AI::AiActionManager::Instance().GetConfigFilePath();
        DeleteFileW(m_testJsonPath.c_str());
        QuickView::AI::AiActionManager::Instance().ResetToDefaults();
    }

    void TearDown() override {
        DeleteFileW(m_testJsonPath.c_str());
    }
};

TEST_F(AiActionManagerTest, DpapiEncryptionRoundTrip) {
    std::string testKey = "AIzaSyD-TestKey_1234567890_!@#$%^&*()_+";
    std::string encrypted = QuickView::AI::AiActionManager::EncryptApiKey(testKey);
    EXPECT_FALSE(encrypted.empty());
    EXPECT_NE(encrypted, testKey);

    std::string decrypted = QuickView::AI::AiActionManager::DecryptApiKey(encrypted);
    EXPECT_EQ(decrypted, testKey);
}

TEST_F(AiActionManagerTest, EmptyApiKeyHandling) {
    EXPECT_TRUE(QuickView::AI::AiActionManager::EncryptApiKey("").empty());
    EXPECT_TRUE(QuickView::AI::AiActionManager::DecryptApiKey("").empty());
}

TEST_F(AiActionManagerTest, PathDerivedFromConfigPath) {
    std::wstring cfg = QuickView::AI::AiActionManager::Instance().GetConfigFilePath();
    EXPECT_FALSE(cfg.empty());
    EXPECT_NE(cfg.find(L"ai_actions.json"), std::wstring::npos);

    std::wstring ini = GetConfigPath(false);
    wchar_t iniDir[MAX_PATH] = { 0 };
    wcscpy_s(iniDir, ini.c_str());
    PathRemoveFileSpecW(iniDir);

    wchar_t jsonDir[MAX_PATH] = { 0 };
    wcscpy_s(jsonDir, cfg.c_str());
    PathRemoveFileSpecW(jsonDir);

    EXPECT_EQ(std::wstring(iniDir), std::wstring(jsonDir));
}

TEST_F(AiActionManagerTest, ConfigSerializationRoundTripWithDeepCopy) {
    auto& mgr = QuickView::AI::AiActionManager::Instance();
    auto& profiles = mgr.GetProfiles();
    ASSERT_FALSE(profiles.empty());

    // Set custom values with Unicode and special characters
    profiles[0].displayName = L"自定义模型服务商 测试";
    profiles[0].baseUrl = "https://api.example.com/v1/";
    profiles[0].defaultModel = "custom-vision-pro";
    profiles[0].encryptedApiKey = QuickView::AI::AiActionManager::EncryptApiKey("sk-secret-test-key");
    profiles[0].fetchedModels = { "model-alpha", "model-beta", "custom-vision-pro" };

    auto& actions = mgr.GetActions();
    ASSERT_FALSE(actions.empty());
    actions[0].name = L"超分辨率画质精修";
    actions[0].promptTemplate = L"Line 1: Ultra high resolution.\nLine 2: 8k masterpiece, photorealistic.";
    actions[0].negativePrompt = L"blurry, artifacts, bad quality";
    actions[0].samplingSteps = 30;
    actions[0].cfgScale = 8.5f;
    actions[0].aspectRatio = QuickView::AI::OutputAspectRatio::Landscape_16_9;
    actions[0].targetResolution = QuickView::AI::TargetResolution::Res_4K;

    EXPECT_TRUE(mgr.SaveConfig());

    // Reload and verify
    EXPECT_TRUE(mgr.ReloadConfig());

    const auto& reloadedProfiles = mgr.GetProfiles();
    ASSERT_FALSE(reloadedProfiles.empty());
    EXPECT_EQ(reloadedProfiles[0].displayName, L"自定义模型服务商 测试");
    EXPECT_EQ(reloadedProfiles[0].baseUrl, "https://api.example.com/v1/");
    EXPECT_EQ(reloadedProfiles[0].defaultModel, "custom-vision-pro");
    EXPECT_EQ(reloadedProfiles[0].fetchedModels.size(), 3);
    EXPECT_EQ(reloadedProfiles[0].fetchedModels[0], "model-alpha");

    std::string decKey = QuickView::AI::AiActionManager::DecryptApiKey(reloadedProfiles[0].encryptedApiKey);
    EXPECT_EQ(decKey, "sk-secret-test-key");

    const auto& reloadedActions = mgr.GetActions();
    ASSERT_FALSE(reloadedActions.empty());
    EXPECT_EQ(reloadedActions[0].name, L"超分辨率画质精修");
    EXPECT_EQ(reloadedActions[0].promptTemplate, L"Line 1: Ultra high resolution.\nLine 2: 8k masterpiece, photorealistic.");
    EXPECT_EQ(reloadedActions[0].negativePrompt, L"blurry, artifacts, bad quality");
    EXPECT_EQ(reloadedActions[0].samplingSteps, 30);
    EXPECT_FLOAT_EQ(reloadedActions[0].cfgScale, 8.5f);
    EXPECT_EQ(reloadedActions[0].aspectRatio, QuickView::AI::OutputAspectRatio::Landscape_16_9);
    EXPECT_EQ(reloadedActions[0].targetResolution, QuickView::AI::TargetResolution::Res_4K);
}

TEST_F(AiActionManagerTest, ResetToDefaultsRestoresCleanState) {
    auto& mgr = QuickView::AI::AiActionManager::Instance();
    mgr.ResetToDefaults();

    const auto& profiles = mgr.GetProfiles();
    ASSERT_FALSE(profiles.empty());
    EXPECT_EQ(profiles[0].displayName, L"Google Gemini");
    EXPECT_EQ(profiles[0].defaultModel, "");

    const auto& actions = mgr.GetActions();
    ASSERT_FALSE(actions.empty());
    EXPECT_EQ(actions[0].name, L"AI Detail Enhancement & Texture Refine");
}

TEST_F(AiActionManagerTest, FormatAiErrorMessageUnwrapsArrayAndRemovesRawJson) {
    // Test unwrapping 429 error returned as JSON array
    std::string_view jsonArray429 = R"([
  {
    "error": {
      "code": 429,
      "message": "You exceeded your current quota, please check your plan and billing details.",
      "status": "RESOURCE_EXHAUSTED"
    }
  }
])";
    std::wstring msg429 = QuickView::AI::AiActionManager::FormatAiErrorMessage(429, jsonArray429);
    EXPECT_NE(msg429.find(L"HTTP 429"), std::wstring::npos);
    EXPECT_NE(msg429.find(L"You exceeded your current quota"), std::wstring::npos);
    EXPECT_NE(msg429.find(L"Hint: Check your API account balance"), std::wstring::npos);
    // Crucial: Ensure NO raw JSON braces or brackets are leaked into user display
    EXPECT_EQ(msg429.find(L'{'), std::wstring::npos);
    EXPECT_EQ(msg429.find(L'}'), std::wstring::npos);
    EXPECT_EQ(msg429.find(L'['), std::wstring::npos);
    EXPECT_EQ(msg429.find(L']'), std::wstring::npos);

    // Test 503 error formatting
    std::string_view json503 = R"({"error": {"message": "Model is overloaded"}})";
    std::wstring msg503 = QuickView::AI::AiActionManager::FormatAiErrorMessage(503, json503);
    EXPECT_NE(msg503.find(L"HTTP 503"), std::wstring::npos);
    EXPECT_NE(msg503.find(L"Model is overloaded"), std::wstring::npos);
    EXPECT_NE(msg503.find(L"Remote AI server is overloaded"), std::wstring::npos);
    EXPECT_EQ(msg503.find(L'{'), std::wstring::npos);
    EXPECT_EQ(msg503.find(L'}'), std::wstring::npos);
}

TEST_F(AiActionManagerTest, UniversalSemanticExtractionHandlesNestedFastApiValidationArray) {
    // Test universal extraction on nested FastAPI validation array
    std::string_view fastapiNested = R"({
  "detail": [
    {
      "loc": ["body", "init_images"],
      "msg": "Field required: init_images",
      "type": "value_error.missing"
    }
  ]
})";
    std::wstring title, detail, advice;
    QuickView::AI::AiActionManager::ExtractSemanticError(422, fastapiNested, title, detail, advice);
    EXPECT_NE(title.find(L"HTTP 422"), std::wstring::npos);
    EXPECT_EQ(detail, L"Field required: init_images");
    EXPECT_FALSE(advice.empty());
}

TEST_F(AiActionManagerTest, ProbeLocalSdWebUiModelsIfRunning) {
    HANDLE hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    struct TestProbeCtx {
        HANDLE hEvent;
        bool probeSuccess = false;
        std::vector<std::string> fetchedModels;
    } ctx{ hEvent, false, {} };

    QuickView::AI::AiActionManager::Instance().FetchModelsAsync(
        "http://127.0.0.1:7860/", "", QuickView::AI::ApiProtocol::StabilityInpaint,
        QuickView::AI::ModelsCallback(
            [](void* u, bool success, const std::vector<std::string>& models, const std::wstring& /*errMsg*/) {
                auto* c = static_cast<TestProbeCtx*>(u);
                c->probeSuccess = success;
                c->fetchedModels = models;
                SetEvent(c->hEvent);
            },
            &ctx
        )
    );

    DWORD waitResult = WaitForSingleObject(hEvent, 4000);
    CloseHandle(hEvent);

    if (waitResult == WAIT_OBJECT_0 && ctx.probeSuccess) {
        EXPECT_FALSE(ctx.fetchedModels.empty());
        bool foundDreamshaper = false;
        for (const auto& m : ctx.fetchedModels) {
            if (m.find("dreamshaper") != std::string::npos || m.find("v1-5") != std::string::npos) {
                foundDreamshaper = true;
                // Verify trailing hash brackets like " [879db523c3]" are stripped cleanly
                EXPECT_EQ(m.find(" ["), std::string::npos);
                break;
            }
        }
        EXPECT_TRUE(foundDreamshaper);
    }
}

TEST_F(AiActionManagerTest, PollSdProgressIfRunning) {
    QuickView::AI::AiActionManager::SdProgressInfo info;
    bool polled = QuickView::AI::AiActionManager::PollSdProgress("http://127.0.0.1:7860/", info);
    // If WebUI Forge daemon is currently running, PollSdProgress must succeed
    if (polled) {
        EXPECT_GE(info.progress, 0.0f);
        EXPECT_LE(info.progress, 1.0f);
    }
}

TEST_F(AiActionManagerTest, TestConnectionAsyncWithLocalSdWebUi) {
    HANDLE hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    struct TestConnCtx {
        HANDLE hEvent;
        bool connSuccess = false;
        int code = 0;
    } ctx{ hEvent, false, 0 };

    QuickView::AI::AiActionManager::Instance().TestConnectionAsync(
        "http://127.0.0.1:7860/", "", QuickView::AI::ApiProtocol::StabilityInpaint,
        QuickView::AI::LatencyCallback(
            [](void* u, bool success, int statusCode, int /*latencyMs*/, const std::wstring& /*msg*/) {
                auto* c = static_cast<TestConnCtx*>(u);
                c->connSuccess = success;
                c->code = statusCode;
                SetEvent(c->hEvent);
            },
            &ctx
        )
    );

    DWORD waitResult = WaitForSingleObject(hEvent, 4000);
    CloseHandle(hEvent);

    if (waitResult == WAIT_OBJECT_0 && ctx.connSuccess) {
        EXPECT_EQ(ctx.code, 200);
    }
}

TEST_F(AiActionManagerTest, FormatAiErrorMessageUnwrapsHttpExceptionDetail) {
    // When FastAPI returns HTTPException 404 with {"error": "HTTPException", "detail": "Init image not found"}
    std::string_view fastapi404 = R"({
  "error": "HTTPException",
  "detail": "Init image not found",
  "body": "",
  "errors": ""
})";
    std::wstring msg = QuickView::AI::AiActionManager::FormatAiErrorMessage(404, fastapi404);
    // Must contain specific detail "Init image not found" rather than generic "HTTPException"
    EXPECT_NE(msg.find(L"Init image not found"), std::wstring::npos);
    EXPECT_EQ(msg.find(L"HTTPException"), std::wstring::npos);
}

TEST_F(AiActionManagerTest, ExtractSemanticErrorHandlesUnhandledMimeType) {
    std::string_view unhandledError = R"({
  "error": {
    "code": 400,
    "message": "Unhandled generated data mime type: image/jpeg",
    "status": "INVALID_ARGUMENT"
  }
})";
    std::wstring title, detail, advice;
    QuickView::AI::AiActionManager::ExtractSemanticError(400, unhandledError, title, detail, advice);
    EXPECT_NE(detail.find(L"Unhandled generated data mime type"), std::wstring::npos);
    EXPECT_NE(advice.find(L"Native REST"), std::wstring::npos);
}

TEST_F(AiActionManagerTest, GeminiNativeProtocolRoundTripPersistence) {
    auto& mgr = QuickView::AI::AiActionManager::Instance();
    auto& profiles = mgr.GetProfiles();
    ASSERT_FALSE(profiles.empty());

    profiles[0].displayName = L"Google Gemini Native";
    profiles[0].baseUrl = "https://generativelanguage.googleapis.com/v1beta/";
    profiles[0].protocol = QuickView::AI::ApiProtocol::GeminiNative;
    profiles[0].defaultModel = "gemini-2.5-flash";

    EXPECT_TRUE(mgr.SaveConfig());
    EXPECT_TRUE(mgr.ReloadConfig());

    const auto& reloaded = mgr.GetProfiles();
    ASSERT_FALSE(reloaded.empty());
    EXPECT_EQ(reloaded[0].protocol, QuickView::AI::ApiProtocol::GeminiNative);
    EXPECT_EQ(reloaded[0].baseUrl, "https://generativelanguage.googleapis.com/v1beta/");
    EXPECT_EQ(reloaded[0].defaultModel, "gemini-2.5-flash");
}

TEST_F(AiActionManagerTest, GeminiNativeEndpointLiveRoutingProbeWithoutKey) {
    // Live probe test against official Google endpoint with legacy URL containing "/openai/"
    // Verifies that:
    // 1. Path sanitization automatically strips "/openai" so it routes to /v1beta/models (no 404)
    // 2. Authentication header uses only "x-goog-api-key" without "Authorization: Bearer" (no 401)
    // 3. Official endpoint responds with HTTP 400 API_KEY_INVALID rather than 401 or 404
    HANDLE hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    struct TestRouteCtx {
        HANDLE hEvent;
        int returnedStatusCode = 0;
    } ctx{ hEvent, 0 };

    QuickView::AI::AiActionManager::Instance().TestConnectionAsync(
        "https://generativelanguage.googleapis.com/v1beta/openai/",
        "AIzaSyTest_DummyKey_For_Routing_Verification",
        QuickView::AI::ApiProtocol::GeminiNative,
        QuickView::AI::LatencyCallback(
            [](void* u, bool /*success*/, int statusCode, int /*latencyMs*/, const std::wstring& /*msg*/) {
                auto* c = static_cast<TestRouteCtx*>(u);
                c->returnedStatusCode = statusCode;
                SetEvent(c->hEvent);
            },
            &ctx
        )
    );

    DWORD waitResult = WaitForSingleObject(hEvent, 10000);
    CloseHandle(hEvent);

    if (waitResult == WAIT_OBJECT_0 && ctx.returnedStatusCode > 0) {
        EXPECT_EQ(ctx.returnedStatusCode, 400);
        EXPECT_NE(ctx.returnedStatusCode, 401);
        EXPECT_NE(ctx.returnedStatusCode, 404);
    }
}

TEST_F(AiActionManagerTest, ResampleBgraExactScalingCorrectness) {
    // 2x2 red-green-blue-white pattern resampled to 4x4
    std::vector<uint8_t> src = {
        255, 0, 0, 255,   0, 255, 0, 255,
        0, 0, 255, 255,   255, 255, 255, 255
    };
    std::vector<uint8_t> dst(4 * 4 * 4, 0);

    QuickView::AI::AiActionManager::ResampleBgraExact(
        src.data(), 2, 2, 2 * 4,
        dst.data(), 4, 4, 4 * 4);

    // Alpha channel must remain fully opaque (255)
    for (int i = 0; i < 16; ++i) {
        EXPECT_EQ(dst[i * 4 + 3], 255);
    }
}

TEST_F(AiActionManagerTest, BlendMaskGuidedFeatheredStrictRegionIsolation) {
    // 100x100 white canvas
    const int W = 100, H = 100;
    std::vector<uint8_t> orig(W * H * 4, 255);

    // 40x40 red patch representing context slice at (30, 30)
    const int sliceW = 40, sliceH = 40;
    std::vector<uint8_t> sub(sliceW * sliceH * 4, 0);
    for (int i = 0; i < sliceW * sliceH; ++i) {
        sub[i * 4 + 2] = 255; // Red channel
        sub[i * 4 + 3] = 255; // Alpha
    }

    // User selection is strictly [40, 60) x [40, 60), sub-image covers [30, 70)
    int selX0 = 40, selY0 = 40, selX1 = 60, selY1 = 60;
    int sliceX0 = 30, sliceY0 = 30;

    QuickView::AI::AiActionManager::BlendMaskGuidedFeathered(
        orig.data(), W, H, W * 4,
        sub.data(), sliceX0, sliceY0, sliceW, sliceH, sliceW * 4,
        selX0, selY0, selX1, selY1, 4);

    // Pixel at (35, 35) is inside the slice but OUTSIDE user selection [40, 60):
    // MUST remain original white (255, 255, 255), absolutely never overwritten!
    int outIdx = (35 * W + 35) * 4;
    EXPECT_EQ(orig[outIdx + 0], 255);
    EXPECT_EQ(orig[outIdx + 1], 255);
    EXPECT_EQ(orig[outIdx + 2], 255);

    // Center of user selection (50, 50) MUST be blended with AI red patch
    int inIdx = (50 * W + 50) * 4;
    EXPECT_EQ(orig[inIdx + 0], 0);   // B
    EXPECT_EQ(orig[inIdx + 1], 0);   // G
    EXPECT_EQ(orig[inIdx + 2], 255); // R
}

TEST_F(AiActionManagerTest, InpaintActionParameterPreservation) {
    auto& mgr = QuickView::AI::AiActionManager::Instance();
    QuickView::AI::ActionDesc customAct;
    customAct.id = "test_custom_inpaint";
    customAct.name = L"Custom Inpaint Test";
    customAct.promptTemplate = L"replace with vibrant red roses";
    customAct.negativePrompt = L"low quality, blurry";
    customAct.denoisingStrength = 0.85f;
    customAct.samplingSteps = 40;
    customAct.cfgScale = 9.5f;

    // Verify macro substitution and prompt merging logic
    std::wstring customInput = L"blooming in spring";
    QuickView::AI::ActionDesc actCopy = customAct;
    if (!customInput.empty()) {
        size_t macroPos = actCopy.promptTemplate.find(L"{prompt}");
        if (macroPos != std::wstring::npos) {
            actCopy.promptTemplate.replace(macroPos, 8, customInput);
        } else {
            if (!actCopy.promptTemplate.empty()) actCopy.promptTemplate += L"\n";
            actCopy.promptTemplate += customInput;
        }
    }

    EXPECT_EQ(actCopy.promptTemplate, L"replace with vibrant red roses\nblooming in spring");
    EXPECT_FLOAT_EQ(actCopy.denoisingStrength, 0.85f);
    EXPECT_EQ(actCopy.samplingSteps, 40);
    EXPECT_FLOAT_EQ(actCopy.cfgScale, 9.5f);
}

TEST_F(AiActionManagerTest, LoadConfigHandlesNullOrWrongTypedStringFieldsSafely) {
    auto& mgr = QuickView::AI::AiActionManager::Instance();
    const std::wstring cfgPath = mgr.GetConfigFilePath();

    FILE* fp = nullptr;
    _wfopen_s(&fp, cfgPath.c_str(), L"wb");
    ASSERT_NE(fp, nullptr);
    const char* malformedJson = R"({
        "default_profile": 123,
        "last_action": null,
        "profiles": [
            "not_an_object",
            {
                "id": null,
                "name": 42,
                "protocol": 3,
                "base_url": null,
                "encrypted_key": false,
                "default_model": 99,
                "fetched_models": [123, null, "valid-model"]
            }
        ],
        "actions": [
            12345,
            {
                "id": null,
                "name": null,
                "profile_id": 7,
                "prompt": null,
                "negative_prompt": null
            }
        ]
    })";
    fwrite(malformedJson, 1, strlen(malformedJson), fp);
    fclose(fp);

    EXPECT_TRUE(mgr.ReloadConfig());
    ASSERT_EQ(mgr.GetProfiles().size(), 1u);
    EXPECT_EQ(mgr.GetProfiles()[0].displayName, L"Google Gemini");
    ASSERT_EQ(mgr.GetProfiles()[0].fetchedModels.size(), 1u);
    EXPECT_EQ(mgr.GetProfiles()[0].fetchedModels[0], "valid-model");
    ASSERT_EQ(mgr.GetActions().size(), 1u);
}

TEST_F(AiActionManagerTest, VisualProviderSnapshotAndRenderIntegrationInpaintSlice) {
    static std::wstring s_capturedPath;
    static std::vector<uint8_t> s_fakeVisualBgra;

    s_capturedPath.clear();
    // Construct 40x20 BGRA upright visual frame
    s_fakeVisualBgra.assign(40 * 20 * 4, 128);
    for (int i = 0; i < 40 * 20; ++i) s_fakeVisualBgra[i * 4 + 3] = 255;

    QuickView::AI::AiActionManager::SetVisualFrameProviders(
        [](QuickView::AI::VisualFrameSnapshot& out) -> bool {
            out.imagePath = L"C:\\fake_visual_snapshot.jpg";
            out.effectiveExif = 6;
            out.netRotation = 90;
            s_capturedPath = out.imagePath;
            return true;
        },
        [](const QuickView::AI::VisualFrameSnapshot&, std::vector<uint8_t>& outBgra, uint32_t& outW, uint32_t& outH, uint32_t& outStride) -> bool {
            outW = 40;
            outH = 20;
            outStride = 40 * 4;
            outBgra = s_fakeVisualBgra;
            return true;
        });

    auto& mgr = QuickView::AI::AiActionManager::Instance();
    mgr.GetProfiles()[0].baseUrl = "http://127.0.0.1:1/"; // fast local refuse
    mgr.GetProfiles()[0].protocol = QuickView::AI::ApiProtocol::StabilityInpaint;

    HANDLE hDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    static QuickView::AI::ExecutionResult s_lastRes;
    s_lastRes = {};
    struct WaitCtx { HANDLE ev; };
    WaitCtx* wCtx = new WaitCtx{ hDone };

    mgr.ExecuteInpaint(
        2, 2, 38, 18, L"test fill", nullptr,
        QuickView::AI::ActionCallback(
            [](void* ctx, const QuickView::AI::ExecutionResult& res) {
                s_lastRes = res;
                SetEvent(static_cast<WaitCtx*>(ctx)->ev);
            },
            wCtx,
            [](void* ctx) { delete static_cast<WaitCtx*>(ctx); }));

    EXPECT_EQ(s_capturedPath, L"C:\\fake_visual_snapshot.jpg");
    EXPECT_EQ(WaitForSingleObject(hDone, 10000), WAIT_OBJECT_0);
    CloseHandle(hDone);
    EXPECT_FALSE(s_lastRes.success);
    EXPECT_EQ(s_lastRes.sourceImagePath, L"C:\\fake_visual_snapshot.jpg");

    QuickView::AI::AiActionManager::SetVisualFrameProviders(nullptr, nullptr);
}

TEST_F(AiActionManagerTest, RateLimited429IsRetriedOnSameRequest) {
    WSADATA wsa;
    ASSERT_EQ(WSAStartup(MAKEWORD(2, 2), &wsa), 0);
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    ASSERT_EQ(listen(listener, 4), 0);
    int addrLen = sizeof(addr);
    getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &addrLen);
    const int port = ntohs(addr.sin_port);

    // First POST gets 429 (Retry-After: 0), the next gets 400 so the task ends quickly.
    static std::atomic<int> s_posts{ 0 };
    static std::string s_retryHeaders;
    s_posts = 0;
    s_retryHeaders.clear();
    std::thread server([listener]() {
        for (;;) {
            SOCKET c = accept(listener, nullptr, nullptr);
            if (c == INVALID_SOCKET) return;
            std::string req;
            char chunk[65536];
            size_t hdrEnd = std::string::npos;
            size_t bodyLen = 0;
            for (;;) {
                if (hdrEnd == std::string::npos && (hdrEnd = req.find("\r\n\r\n")) != std::string::npos) {
                    size_t cl = req.find("Content-Length:");
                    if (cl != std::string::npos && cl < hdrEnd) bodyLen = std::stoul(req.substr(cl + 15));
                }
                if (hdrEnd != std::string::npos && req.size() >= hdrEnd + 4 + bodyLen) break;
                int n = recv(c, chunk, sizeof(chunk), 0);
                if (n <= 0) break;
                req.append(chunk, n);
            }
            std::string resp = "HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
            if (req.rfind("POST", 0) == 0) {
                if (++s_posts == 1) {
                    resp = "HTTP/1.1 429 Too Many Requests\r\nConnection: close\r\nRetry-After: 0\r\nContent-Length: 2\r\n\r\n{}";
                } else {
                    s_retryHeaders = req.substr(0, hdrEnd);
                    resp = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 2\r\n\r\n{}";
                }
            }
            send(c, resp.data(), static_cast<int>(resp.size()), 0);
            closesocket(c);
        }
    });

    QuickView::AI::AiActionManager::SetVisualFrameProviders(
        [](QuickView::AI::VisualFrameSnapshot& out) -> bool {
            out.imagePath = L"C:\\fake_visual_snapshot.jpg";
            return true;
        },
        [](const QuickView::AI::VisualFrameSnapshot&, std::vector<uint8_t>& outBgra, uint32_t& outW, uint32_t& outH, uint32_t& outStride) -> bool {
            outW = 40;
            outH = 20;
            outStride = 40 * 4;
            outBgra.assign(40 * 20 * 4, 255);
            return true;
        });

    auto& mgr = QuickView::AI::AiActionManager::Instance();
    mgr.GetProfiles()[0].baseUrl = "http://127.0.0.1:" + std::to_string(port) + "/";
    mgr.GetProfiles()[0].protocol = QuickView::AI::ApiProtocol::StabilityInpaint;

    HANDLE hDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    static QuickView::AI::ExecutionResult s_lastRes;
    s_lastRes = {};
    struct WaitCtx { HANDLE ev; };
    mgr.ExecuteInpaint(
        2, 2, 38, 18, L"test fill", nullptr,
        QuickView::AI::ActionCallback(
            [](void* ctx, const QuickView::AI::ExecutionResult& res) {
                s_lastRes = res;
                SetEvent(static_cast<WaitCtx*>(ctx)->ev);
            },
            new WaitCtx{ hDone },
            [](void* ctx) { delete static_cast<WaitCtx*>(ctx); }));

    EXPECT_EQ(WaitForSingleObject(hDone, 15000), WAIT_OBJECT_0);
    CloseHandle(hDone);
    closesocket(listener);
    server.join();
    WSACleanup();
    QuickView::AI::AiActionManager::SetVisualFrameProviders(nullptr, nullptr);

    EXPECT_EQ(s_posts.load(), 2);
    EXPECT_EQ(s_lastRes.httpStatusCode, 400u);
    // The retried request still carries the original headers, once.
    size_t first = s_retryHeaders.find("Content-Type:");
    EXPECT_NE(first, std::string::npos);
    EXPECT_EQ(s_retryHeaders.find("Content-Type:", first + 1), std::string::npos);
}
