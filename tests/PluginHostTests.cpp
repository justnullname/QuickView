#include <gtest/gtest.h>
#include "Plugin/PluginHost.h"
#include "Plugin/qvx.h"
#include "Plugin/qvx_sr.h"
#include "SystemInfo.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <fstream>

using Microsoft::WRL::ComPtr;

class PluginHostTests : public ::testing::Test {
protected:
    ComPtr<ID3D11Device> m_d3dDevice;
    ComPtr<ID3D11DeviceContext> m_d3dContext;
    std::wstring m_tempIniPath;

    void SetUp() override {
        // Create D3D11 Hardware or WARP Device for testing
        D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1 };
        D3D_FEATURE_LEVEL featureLevel;
        HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            featureLevels, 2, D3D11_SDK_VERSION,
            &m_d3dDevice, &featureLevel, &m_d3dContext
        );

        if (FAILED(hr)) {
            // Fallback to WARP (Software Device) if Hardware device is unavailable in CI
            hr = D3D11CreateDevice(
                nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                featureLevels, 2, D3D11_SDK_VERSION,
                &m_d3dDevice, &featureLevel, &m_d3dContext
            );
        }
        ASSERT_TRUE(SUCCEEDED(hr) && m_d3dDevice);

        wchar_t tempPath[MAX_PATH];
        GetTempPathW(MAX_PATH, tempPath);
        m_tempIniPath = std::wstring(tempPath) + L"QVX_Test_Settings.ini";
        DeleteFileW(m_tempIniPath.c_str());
    }

    void TearDown() override {
        QuickView::PluginHost::Instance().UnloadSrPlugin();
        DeleteFileW(m_tempIniPath.c_str());
    }
};

// 1. Test INI Configuration persistence
TEST_F(PluginHostTests, ConfigPersistence) {
    auto& host = QuickView::PluginHost::Instance();
    host.SetSrPluginEnabled(true);
    host.SetSrPluginPath(L"plugins\\test_custom.qvx");
    host.SetSrModelId("anime_2x");
    host.SetSrDenoise(0.15f);
    host.SetSrAutoTriggerMaxSourceMp(2.5f);

    host.SaveConfig(m_tempIniPath.c_str());

    // Reset in-memory states
    host.SetSrPluginEnabled(false);
    host.SetSrPluginPath(L"");
    host.SetSrModelId("");
    host.SetSrDenoise(0.0f);
    host.SetSrAutoTriggerMaxSourceMp(1.0f);

    // Reload
    host.LoadConfig(m_tempIniPath.c_str());

    EXPECT_TRUE(host.IsSrPluginEnabled());
    EXPECT_EQ(host.GetSrPluginPath(), L"plugins\\test_custom.qvx");
    EXPECT_EQ(host.GetSrModelId(), "anime_2x");
    EXPECT_NEAR(host.GetSrDenoise(), 0.15f, 0.01f);
    EXPECT_NEAR(host.GetSrAutoTriggerMaxSourceMp(), 2.5f, 0.01f);
}

// 2. Test missing plugin graceful fallback
TEST_F(PluginHostTests, MissingPluginGracefulFallback) {
    auto& host = QuickView::PluginHost::Instance();
    host.SetSrPluginEnabled(true);
    host.SetSrPluginPath(L"plugins\\non_existent_sr_engine.qvx");

    EXPECT_FALSE(host.EnsureSrContext(m_d3dDevice.Get()));
}

// 3. Test Real-ESRGAN NCNN Vulkan In-Process Super-Resolution
TEST_F(PluginHostTests, NcnnVulkanGpuUpscaleAndProgress) {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    PathRemoveFileSpecW(exePath);

    std::wstring pluginPath = std::wstring(exePath) + L"\\plugins\\sr\\sr_ncnn_vulkan\\sr_ncnn_vulkan.qvx";
    if (GetFileAttributesW(pluginPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        pluginPath = std::wstring(exePath) + L"\\plugins\\sr_ncnn_vulkan.qvx";
        if (GetFileAttributesW(pluginPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            pluginPath = L"plugins\\sr\\sr_ncnn_vulkan\\sr_ncnn_vulkan.qvx";
            if (GetFileAttributesW(pluginPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
                GTEST_SKIP() << "sr_ncnn_vulkan.qvx not found, skipping NCNN Vulkan test.";
            }
        }
    }

    auto& host = QuickView::PluginHost::Instance();
    host.UnloadSrPlugin();
    host.SetSrPluginEnabled(true);
    host.SetSrPluginPath(pluginPath);
    host.SetSrModelId("realesr-animevideov3-auto");

    ASSERT_TRUE(host.EnsureSrContext(m_d3dDevice.Get()));

    // Verify 7 Models Catalog
    auto models = host.GetCurrentSrModels();
    EXPECT_GE(models.size(), 7u);
    EXPECT_EQ(models[0].modelId, "realesr-animevideov3-auto");
    EXPECT_EQ(models[1].modelId, "realesr-animevideov3-x2");

    // Verify Dynamic Parameters (Denoise & Tile Size)
    auto params = host.GetCurrentSrParams();
    EXPECT_GE(params.size(), 2u);
    EXPECT_EQ(params[0].id, "denoise");
    EXPECT_EQ(params[1].id, "tile_size");

    // 64x64 Source Texture
    D3D11_TEXTURE2D_DESC srcDesc{};
    srcDesc.Width = 64;
    srcDesc.Height = 64;
    srcDesc.MipLevels = 1;
    srcDesc.ArraySize = 1;
    srcDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    srcDesc.SampleDesc.Count = 1;
    srcDesc.Usage = D3D11_USAGE_DEFAULT;
    srcDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    ComPtr<ID3D11Texture2D> pSrcTex;
    ASSERT_TRUE(SUCCEEDED(m_d3dDevice->CreateTexture2D(&srcDesc, nullptr, &pSrcTex)));

    // 128x128 Destination Texture
    D3D11_TEXTURE2D_DESC dstDesc = srcDesc;
    dstDesc.Width = 128;
    dstDesc.Height = 128;
    dstDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;

    ComPtr<ID3D11Texture2D> pDstTex;
    ASSERT_TRUE(SUCCEEDED(m_d3dDevice->CreateTexture2D(&dstDesc, nullptr, &pDstTex)));

    std::vector<uint32_t> srcPixels(64 * 64, 0xFF55AAFF);
    m_d3dContext->UpdateSubresource(pSrcTex.Get(), 0, nullptr, srcPixels.data(), 64 * 4, 0);

    std::wstring modelBin = std::wstring(exePath) + L"\\plugins\\sr\\sr_ncnn_vulkan\\models\\realesr-animevideov3-x2.bin";
    if (GetFileAttributesW(modelBin.c_str()) == INVALID_FILE_ATTRIBUTES) {
        modelBin = std::wstring(exePath) + L"\\plugins\\models\\realesr-animevideov3-x2.bin";
        if (GetFileAttributesW(modelBin.c_str()) == INVALID_FILE_ATTRIBUTES) {
            GTEST_SKIP() << "Model weights not downloaded yet, skipping GPU execution.";
        }
    }

    struct ProgressRecord {
        float lastVal = -1.0f;
        int callCount = 0;
    } progRecord;

    auto onProgress = [](float p, void* uData) {
        auto* rec = static_cast<ProgressRecord*>(uData);
        rec->lastVal = p;
        rec->callCount++;
    };

    // Execute Real-ESRGAN Deep Residual GPU Upscale with Progress Callback
    int32_t result = host.ExecuteSrUpscaleGpu(
        m_d3dDevice.Get(),
        pSrcTex.Get(), 64, 64,
        pDstTex.Get(), 128, 128,
        nullptr, nullptr,
        onProgress, &progRecord
    );

    printf("Real-ESRGAN NCNN Vulkan ExecuteSrUpscaleGpu result = 0x%08X (Progress calls: %d, final: %.2f)\n",
           (uint32_t)result, progRecord.callCount, progRecord.lastVal);
    EXPECT_EQ(result, (int32_t)S_OK);
    EXPECT_GT(progRecord.callCount, 0);
    EXPECT_NEAR(progRecord.lastVal, 1.0f, 0.05f);

    // Verify output pixels are valid non-black image with valid alpha
    D3D11_TEXTURE2D_DESC readDesc = dstDesc;
    readDesc.Usage = D3D11_USAGE_STAGING;
    readDesc.BindFlags = 0;
    readDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> pStaging;
    if (SUCCEEDED(m_d3dDevice->CreateTexture2D(&readDesc, nullptr, &pStaging))) {
        m_d3dContext->CopyResource(pStaging.Get(), pDstTex.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(m_d3dContext->Map(pStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            const uint32_t* pPixels = static_cast<const uint32_t*>(mapped.pData);
            // Center pixel must not be pure black (0x00000000)
            uint32_t samplePix = pPixels[64 * (mapped.RowPitch / 4) + 64];
            uint8_t a = (samplePix >> 24) & 0xFF;
            uint8_t r = (samplePix >> 16) & 0xFF;
            uint8_t g = (samplePix >> 8) & 0xFF;
            uint8_t b = samplePix & 0xFF;
            printf("Sample Pixel BGRA: 0x%08X (R=%u, G=%u, B=%u, A=%u)\n", samplePix, r, g, b, a);
            EXPECT_GT(r + g + b, 0); // Must not be black
            EXPECT_EQ(a, 255);       // Must be fully opaque
            m_d3dContext->Unmap(pStaging.Get(), 0);
        }
    }
}

// 4. Test ResetToDefaults and compare mode default value
TEST_F(PluginHostTests, ResetToDefaults) {
    auto& host = QuickView::PluginHost::Instance();
    host.SetSrPluginEnabled(true);
    host.SetSrModelId("realesr-general-x4v3");
    host.SetSrAutoTriggerEnabled(true);
    host.SetSrOpenInCompareMode(false);
    host.SetSrPromptModelOnHotkey(true);
    host.SetSrDenoise(0.5f);
    host.SetSrAutoTriggerMaxSourceMp(8.0f);

    host.ResetToDefaults();

    EXPECT_FALSE(host.IsSrPluginEnabled());
    EXPECT_EQ(host.GetSrModelId(), "realesr-animevideov3-auto");
    EXPECT_FALSE(host.IsSrAutoTriggerEnabled());
    EXPECT_TRUE(host.IsSrOpenInCompareMode());
    EXPECT_FALSE(host.IsSrPromptModelOnHotkey());
    EXPECT_NEAR(host.GetSrDenoise(), 0.0f, 0.001f);
    EXPECT_NEAR(host.GetSrAutoTriggerMaxSourceMp(), 1.0f, 0.001f);
}

// 5. Test native zero-subprocess in-memory ZIP extractor (replaces tar.exe)
#include "ArchiveVFS.h"
TEST_F(PluginHostTests, NativeZipExtraction) {
    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    std::wstring dummyZip = std::wstring(tempPath) + L"QVX_Empty_Test.zip";
    std::wstring extractDir = std::wstring(tempPath) + L"QVX_Extract_Out";

    // Non-existent ZIP should gracefully fail
    EXPECT_FALSE(QuickView::IArchive::ExtractZipToDirectory(dummyZip, extractDir));
}

// 6. Test Transparent Alpha preservation in Neural Upscaling
TEST_F(PluginHostTests, TransparentAlphaPreservation) {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    PathRemoveFileSpecW(exePath);

    std::wstring pluginPath = std::wstring(exePath) + L"\\plugins\\sr\\sr_ncnn_vulkan\\sr_ncnn_vulkan.qvx";
    if (GetFileAttributesW(pluginPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        GTEST_SKIP() << "sr_ncnn_vulkan.qvx not found, skipping Alpha test.";
    }

    std::wstring modelBin = std::wstring(exePath) + L"\\plugins\\sr\\sr_ncnn_vulkan\\models\\realesr-animevideov3-x2.bin";
    if (GetFileAttributesW(modelBin.c_str()) == INVALID_FILE_ATTRIBUTES) {
        GTEST_SKIP() << "Model weights not found, skipping Alpha test.";
    }

    auto& host = QuickView::PluginHost::Instance();
    host.UnloadSrPlugin();
    host.SetSrPluginEnabled(true);
    host.SetSrPluginPath(pluginPath);
    host.SetSrModelId("realesr-animevideov3-x2");

    ASSERT_TRUE(host.EnsureSrContext(m_d3dDevice.Get()));

    // 64x64 Source Texture with transparent corner (Alpha = 0) and opaque center (Alpha = 255)
    D3D11_TEXTURE2D_DESC srcDesc{};
    srcDesc.Width = 64;
    srcDesc.Height = 64;
    srcDesc.MipLevels = 1;
    srcDesc.ArraySize = 1;
    srcDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    srcDesc.SampleDesc.Count = 1;
    srcDesc.Usage = D3D11_USAGE_DEFAULT;
    srcDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    ComPtr<ID3D11Texture2D> pSrcTex;
    ASSERT_TRUE(SUCCEEDED(m_d3dDevice->CreateTexture2D(&srcDesc, nullptr, &pSrcTex)));

    D3D11_TEXTURE2D_DESC dstDesc = srcDesc;
    dstDesc.Width = 128;
    dstDesc.Height = 128;
    dstDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;

    ComPtr<ID3D11Texture2D> pDstTex;
    ASSERT_TRUE(SUCCEEDED(m_d3dDevice->CreateTexture2D(&dstDesc, nullptr, &pDstTex)));

    std::vector<uint32_t> srcPixels(64 * 64, 0x0055AAFF); // Transparent Alpha = 0x00
    // Fill center 32x32 with opaque Alpha = 0xFF
    for (int y = 16; y < 48; ++y) {
        for (int x = 16; x < 48; ++x) {
            srcPixels[y * 64 + x] = 0xFF55AAFF;
        }
    }
    m_d3dContext->UpdateSubresource(pSrcTex.Get(), 0, nullptr, srcPixels.data(), 64 * 4, 0);

    int32_t result = host.ExecuteSrUpscaleGpu(
        m_d3dDevice.Get(),
        pSrcTex.Get(), 64, 64,
        pDstTex.Get(), 128, 128,
        nullptr, nullptr, nullptr, nullptr
    );
    EXPECT_EQ(result, (int32_t)S_OK);

    // Verify Corner Alpha is transparent (< 10) and Center Alpha is opaque (> 240)
    D3D11_TEXTURE2D_DESC readDesc = dstDesc;
    readDesc.Usage = D3D11_USAGE_STAGING;
    readDesc.BindFlags = 0;
    readDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> pStaging;
    if (SUCCEEDED(m_d3dDevice->CreateTexture2D(&readDesc, nullptr, &pStaging))) {
        m_d3dContext->CopyResource(pStaging.Get(), pDstTex.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(m_d3dContext->Map(pStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            const uint32_t* pPixels = static_cast<const uint32_t*>(mapped.pData);
            uint32_t cornerPix = pPixels[0];
            uint32_t centerPix = pPixels[64 * (mapped.RowPitch / 4) + 64];

            uint8_t cornerA = (cornerPix >> 24) & 0xFF;
            uint8_t centerA = (centerPix >> 24) & 0xFF;

            printf("Alpha Test - Corner Alpha: %u (expected 0), Center Alpha: %u (expected 255)\n", cornerA, centerA);
            EXPECT_LE(cornerA, 10);
            EXPECT_GE(centerA, 240);

            m_d3dContext->Unmap(pStaging.Get(), 0);
        }
    }
}

// 7. Test Self-Healing Path Resolution (e.g. Moved directory or imported old absolute path)
TEST_F(PluginHostTests, SelfHealingPathResolution) {
    auto& host = QuickView::PluginHost::Instance();
    // Simulate imported old absolute path from previous device
    host.SetSrPluginPath(L"X:\\OldMachineFolder\\QuickView\\plugins\\sr_ncnn_vulkan.qvx");
    
    // Verify self-healing kicks in and resolves to current executable's relative plugin
    auto installState = host.GetSrPluginInstallState();
    EXPECT_EQ(installState, QuickView::PluginInstallState::Installed);
    
    // Check that configured path is auto-healed to relative
    std::wstring healedPath = host.GetSrPluginPath();
    EXPECT_TRUE(PathIsRelativeW(healedPath.c_str()));
    
    // Verify context initializes cleanly with self-healed path
    host.SetSrPluginEnabled(true);
    EXPECT_TRUE(host.EnsureSrContext(m_d3dDevice.Get()));
}

// 8. Test Missing Plugin Graceful Degradation (Imported backup ini with enabled SR but missing plugin files)
TEST_F(PluginHostTests, MissingPluginGracefulDegradation) {
    auto& host = QuickView::PluginHost::Instance();
    // Simulate configured dummy non-existent plugin
    host.SetSrPluginPath(L"plugins\\non_existent_fake_plugin_12345.qvx");
    host.SetSrPluginEnabled(true);

    // Should gracefully report NotInstalled
    EXPECT_EQ(host.GetSrPluginInstallState(), QuickView::PluginInstallState::NotInstalled);
    // Should gracefully fail context creation without crashing
    EXPECT_FALSE(host.EnsureSrContext(m_d3dDevice.Get()));
}

// 9. Test recursive directory creation and writable models directory resolution
TEST_F(PluginHostTests, EnsureDirectoryRecursiveAndModelsDir) {
    wchar_t tempPath[MAX_PATH]{};
    ASSERT_GT(GetTempPathW(MAX_PATH, tempPath), 0u);
    std::wstring base = std::wstring(tempPath) + L"QuickView_TestNestedDir_" + std::to_wstring(GetCurrentProcessId());
    std::wstring nested = base + L"\\level1\\level2\\models";

    EXPECT_TRUE(SystemInfo::EnsureDirectoryRecursive(nested));
    DWORD attrs = GetFileAttributesW(nested.c_str());
    EXPECT_NE(attrs, INVALID_FILE_ATTRIBUTES);
    EXPECT_TRUE((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0);

    // Clean up nested dirs
    RemoveDirectoryW(nested.c_str());
    RemoveDirectoryW((base + L"\\level1\\level2").c_str());
    RemoveDirectoryW((base + L"\\level1").c_str());
    RemoveDirectoryW(base.c_str());

    std::wstring modelsDir = QuickView::PluginHost::Instance().GetWritableModelsDirectory();
    EXPECT_FALSE(modelsDir.empty());
    EXPECT_NE(modelsDir.find(L"models"), std::wstring::npos);
}

TEST_F(PluginHostTests, VersionComparisonAndManifestMatching) {
    using QuickView::PluginHost;
    EXPECT_GT(PluginHost::ComparePluginVersions("0.2.0", "0.1.0"), 0);
    EXPECT_LT(PluginHost::ComparePluginVersions("0.1.0", "0.2.0"), 0);
    EXPECT_EQ(PluginHost::ComparePluginVersions("v0.2.0", "0.2"), 0);
    EXPECT_GT(PluginHost::ComparePluginVersions("1.0.0", "0.9.12"), 0);

    EXPECT_TRUE(PluginHost::IsOfficialSrPluginMatch("com.quickview.sr.ncnn_vulkan", "sr_ncnn_vulkan_AMD64.zip"));
    EXPECT_TRUE(PluginHost::IsOfficialSrPluginMatch("com.quickview.sr.ncnn_vulkan", "sr_ncnn_vulkan_ARM64.zip"));
    EXPECT_TRUE(PluginHost::IsOfficialSrPluginMatch("sr_ncnn_vulkan", ""));
    EXPECT_TRUE(PluginHost::IsOfficialSrPluginMatch("custom.id", "sr_ncnn_vulkan_AMD64.zip"));
    EXPECT_FALSE(PluginHost::IsOfficialSrPluginMatch("com.other.plugin", "other_plugin.zip"));
}

TEST_F(PluginHostTests, InstalledPluginReportsTargetVersionAndDeduplicatedScan) {
    auto& host = QuickView::PluginHost::Instance();
    host.SetSrPluginPath(L"plugins\\sr\\sr_ncnn_vulkan\\sr_ncnn_vulkan.qvx");
    EXPECT_EQ(host.GetSrPluginInstallState(), QuickView::PluginInstallState::Installed);
    EXPECT_EQ(host.GetInstalledPluginVersion(), QVX_OFFICIAL_SR_PLUGIN_VERSION);

    std::wstring pluginsRoot = host.GetWritablePluginsDirectory();
    EXPECT_FALSE(pluginsRoot.empty());

    auto candidates = host.ScanPluginsDirectory();
    int officialCount = 0;
    for (const auto& c : candidates) {
        if (c.pluginId == "com.quickview.sr.ncnn_vulkan") {
            officialCount++;
            EXPECT_EQ(c.versionStr, QVX_OFFICIAL_SR_PLUGIN_VERSION);
        }
    }
    EXPECT_EQ(officialCount, 1);
}




