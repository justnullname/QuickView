#include <gtest/gtest.h>
#include "Plugin/PluginHost.h"
#include "Plugin/qvx.h"
#include "Plugin/qvx_sr.h"
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

    host.SaveConfig(m_tempIniPath.c_str());

    // Reset in-memory states
    host.SetSrPluginEnabled(false);
    host.SetSrPluginPath(L"");
    host.SetSrModelId("");
    host.SetSrDenoise(0.0f);

    // Reload
    host.LoadConfig(m_tempIniPath.c_str());

    EXPECT_TRUE(host.IsSrPluginEnabled());
    EXPECT_EQ(host.GetSrPluginPath(), L"plugins\\test_custom.qvx");
    EXPECT_EQ(host.GetSrModelId(), "anime_2x");
    EXPECT_NEAR(host.GetSrDenoise(), 0.15f, 0.01f);
}

// 2. Test missing plugin graceful fallback
TEST_F(PluginHostTests, MissingPluginGracefulFallback) {
    auto& host = QuickView::PluginHost::Instance();
    host.SetSrPluginEnabled(true);
    host.SetSrPluginPath(L"plugins\\non_existent_sr_engine.qvx");

    EXPECT_FALSE(host.EnsureSrContext(m_d3dDevice.Get()));
}

// 3. Test Real-ESRGAN Deep Residual Neural Upscale
TEST_F(PluginHostTests, RealESRGANGpuUpscale) {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    PathRemoveFileSpecW(exePath);

    std::wstring pluginPath = std::wstring(exePath) + L"\\plugins\\sr_realesrgan_d3d11.qvx";
    if (GetFileAttributesW(pluginPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        pluginPath = L"plugins\\sr_realesrgan_d3d11.qvx";
        if (GetFileAttributesW(pluginPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            GTEST_SKIP() << "sr_realesrgan_d3d11.qvx not found, skipping Real-ESRGAN test.";
        }
    }

    auto& host = QuickView::PluginHost::Instance();
    host.UnloadSrPlugin();
    host.SetSrPluginEnabled(true);
    host.SetSrPluginPath(pluginPath);
    host.SetSrModelId("realesr-animevideov3-x2");

    ASSERT_TRUE(host.EnsureSrContext(m_d3dDevice.Get()));

    // Verify Model Catalog
    auto models = host.GetCurrentSrModels();
    EXPECT_GE(models.size(), 6u);
    EXPECT_EQ(models[0].modelId, "realesr-animevideov3-auto");
    EXPECT_EQ(models[1].modelId, "realesr-animevideov3-x2");

    // Verify Dynamic Parameters
    auto params = host.GetCurrentSrParams();
    EXPECT_GE(params.size(), 1u);

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

    // Execute Real-ESRGAN Deep Residual GPU Upscale
    int32_t result = host.ExecuteSrUpscaleGpu(
        m_d3dDevice.Get(),
        pSrcTex.Get(), 64, 64,
        pDstTex.Get(), 128, 128,
        nullptr, nullptr
    );

    printf("Real-ESRGAN ExecuteSrUpscaleGpu result = 0x%08X\n", (uint32_t)result);
    EXPECT_EQ(result, (int32_t)S_OK);

    // Also test 0.3.0 General Photo Model (realesr-general-x4v3)
    host.SetSrModelId("realesr-general-x4v3");
    int32_t resultGeneral = host.ExecuteSrUpscaleGpu(
        m_d3dDevice.Get(),
        pSrcTex.Get(), 64, 64,
        pDstTex.Get(), 128, 128,
        nullptr, nullptr
    );
    printf("Real-ESRGAN 0.3.0 General-x4v3 result = 0x%08X\n", (uint32_t)resultGeneral);
    EXPECT_EQ(resultGeneral, (int32_t)S_OK);
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

    host.ResetToDefaults();

    EXPECT_FALSE(host.IsSrPluginEnabled());
    EXPECT_EQ(host.GetSrModelId(), "realesr-animevideov3-auto");
    EXPECT_FALSE(host.IsSrAutoTriggerEnabled());
    EXPECT_TRUE(host.IsSrOpenInCompareMode());
    EXPECT_FALSE(host.IsSrPromptModelOnHotkey());
    EXPECT_NEAR(host.GetSrDenoise(), 0.0f, 0.001f);
}


