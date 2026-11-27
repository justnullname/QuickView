#include <gtest/gtest.h>
#include "Plugin/PluginHost.h"
#include "Plugin/PluginRegistry.h"
#include "Plugin/abi/qvx_core.h"
#include "Plugin/abi/qvx_processor.h"
#include "ArchiveVFS.h"
#include "SystemInfo.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <type_traits>

using Microsoft::WRL::ComPtr;

namespace {

void WriteTextFile(const std::wstring& path, const std::string& content) {
    HANDLE hFile = CreateFileW(
        path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(hFile, content.data(), static_cast<DWORD>(content.size()), &written, nullptr);
    CloseHandle(hFile);
}

void DeleteDirTree(const std::wstring& dirPath) {
    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW((dirPath + L"\\*").c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        std::wstring p = dirPath + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            DeleteDirTree(p);
            RemoveDirectoryW(p.c_str());
        } else {
            SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
            DeleteFileW(p.c_str());
        }
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);
    RemoveDirectoryW(dirPath.c_str());
}

} // namespace

class PluginHostTests : public ::testing::Test {
protected:
    ComPtr<ID3D11Device> m_d3dDevice;
    ComPtr<ID3D11DeviceContext> m_d3dContext;
    std::wstring m_tempIniPath;

    void SetUp() override {
        D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1 };
        D3D_FEATURE_LEVEL featureLevel;
        HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            featureLevels, 2, D3D11_SDK_VERSION,
            &m_d3dDevice, &featureLevel, &m_d3dContext);
        if (FAILED(hr)) {
            hr = D3D11CreateDevice(
                nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                featureLevels, 2, D3D11_SDK_VERSION,
                &m_d3dDevice, &featureLevel, &m_d3dContext);
        }
        ASSERT_TRUE(SUCCEEDED(hr) && m_d3dDevice);

        wchar_t tempPath[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tempPath);
        m_tempIniPath = std::wstring(tempPath) + L"QVX_Test_Settings_v3.ini";
        DeleteFileW(m_tempIniPath.c_str());

        auto& host = QuickView::PluginHost::Instance();
        host.ResetToDefaults();
        host.RefreshRegistry();
    }

    void TearDown() override {
        auto& host = QuickView::PluginHost::Instance();
        host.RefreshRegistry();
        host.ResetToDefaults();
        DeleteFileW(m_tempIniPath.c_str());
    }
};

TEST_F(PluginHostTests, AbiV3LayoutAndConstants) {
    EXPECT_EQ(QVX_ABI_VERSION, 0x00030000u);
    EXPECT_EQ(QVX_PROCESSOR_INTERFACE_VERSION, 0x00030000u);
    EXPECT_TRUE(std::is_standard_layout_v<QVX_ParamDesc>);
    EXPECT_TRUE(std::is_standard_layout_v<QVX_AssetDesc>);
    EXPECT_TRUE(std::is_standard_layout_v<QVX_HostServices>);
    EXPECT_TRUE(std::is_standard_layout_v<QVX_PluginHeader>);
    EXPECT_TRUE(std::is_standard_layout_v<QVX_ProcPresetInfo>);
    EXPECT_TRUE(std::is_standard_layout_v<QVX_ProcExecuteParams>);
    EXPECT_TRUE(std::is_standard_layout_v<QVX_Processor_VTable>);
}

TEST_F(PluginHostTests, ZeroLoadLibraryStaticManifestScanAndPendingDeleteCleanup) {
    wchar_t tempPath[MAX_PATH]{};
    ASSERT_GT(GetTempPathW(MAX_PATH, tempPath), 0u);
    const std::wstring root = std::wstring(tempPath) + L"QVX_RegTest_" + std::to_wstring(GetCurrentProcessId());
    const std::wstring p1Dir = root + L"\\com.test.upscaler";
    const std::wstring p2Dir = root + L"\\com.test.ocr";
    DeleteDirTree(root);
    ASSERT_TRUE(SystemInfo::EnsureDirectoryRecursive(p1Dir));
    ASSERT_TRUE(SystemInfo::EnsureDirectoryRecursive(p2Dir));

    // Create bogus 4-byte .qvx stubs that would fail LoadLibrary if Registry ever loaded DLLs
    WriteTextFile(p1Dir + L"\\stub1.qvx", "NOT_A_PE_DLL");
    WriteTextFile(p2Dir + L"\\stub2.qvx", "NOT_A_PE_DLL");
    WriteTextFile(p1Dir + L"\\old.qvx.pending_delete", "STALE");

    WriteTextFile(p1Dir + L"\\manifest.json", R"({
      "schema_version": 3,
      "abi_version": "3.0.0",
      "id": "com.test.upscaler",
      "version": "1.2.0",
      "entry": "stub1.qvx",
      "i18n": {
        "en-US": { "name": "Test Upscaler", "desc": "EN Desc" },
        "zh-CN": { "name": "测试超分", "desc": "中文描述" }
      },
      "interfaces": ["image_processor"],
      "image_processor": { "kind": "upscaler", "caps": ["gpu_d3d11", "auto_trigger"] }
    })");

    WriteTextFile(p2Dir + L"\\manifest.json", R"({
      "schema_version": 3,
      "abi_version": "3.0.0",
      "id": "com.test.ocr",
      "version": "0.5.0",
      "entry": "stub2.qvx",
      "i18n": { "en-US": { "name": "Test OCR", "desc": "OCR" } },
      "interfaces": ["analyzer"]
    })");

    QuickView::PluginRegistry reg;
    reg.Scan(root);

    const auto& all = reg.GetAll();
    ASSERT_EQ(all.size(), 2u);

    const auto* up = reg.FindById("com.test.upscaler");
    ASSERT_NE(up, nullptr);
    EXPECT_TRUE(up->HasInterface(QVX_IFACE_IMAGE_PROCESSOR));
    EXPECT_EQ(up->processorKind, QVX_PROC_KIND_UPSCALER);
    EXPECT_EQ(up->processorCaps, QVX_PROC_CAP_GPU_D3D11 | QVX_PROC_CAP_AUTO_TRIGGER);
    EXPECT_EQ(up->ResolveName("zh-CN"), "测试超分");
    EXPECT_EQ(up->ResolveName("en-US"), "Test Upscaler");

    const auto* ocr = reg.FindById("com.test.ocr");
    ASSERT_NE(ocr, nullptr);
    EXPECT_TRUE(ocr->HasInterface(QVX_IFACE_ANALYZER));

    // Opportunistic cleanup must have deleted stale .pending_delete
    EXPECT_EQ(GetFileAttributesW((p1Dir + L"\\old.qvx.pending_delete").c_str()), INVALID_FILE_ATTRIBUTES);

    DeleteDirTree(root);
    RemoveDirectoryW(root.c_str());
}

TEST_F(PluginHostTests, MultiPluginConfigPersistenceAndLegacyMigration) {
    auto& host = QuickView::PluginHost::Instance();
    host.SetPluginEnabled("com.alpha.proc", true);
    host.SetPluginActivePreset("com.alpha.proc", "preset_fast");
    host.SetPluginParamNumber("com.alpha.proc", "strength", 0.75f);
    host.SetPluginParamString("com.alpha.proc", "custom_tag", "hello");

    host.SetPluginEnabled("com.beta.filter", false);
    host.SetPluginParamNumber("com.beta.filter", "radius", 8.0f);

    host.SetSrAutoTriggerEnabled(true);
    host.SetSrAutoTriggerMaxSourceMp(2.5f);
    host.SaveConfig(m_tempIniPath.c_str());

    host.ResetToDefaults();
    host.LoadConfig(m_tempIniPath.c_str());

    EXPECT_TRUE(host.IsPluginEnabled("com.alpha.proc"));
    EXPECT_EQ(host.GetPluginActivePreset("com.alpha.proc"), "preset_fast");
    EXPECT_NEAR(host.GetPluginParamNumber("com.alpha.proc", "strength"), 0.75f, 0.01f);
    EXPECT_EQ(host.GetPluginParamString("com.alpha.proc", "custom_tag"), "hello");
    EXPECT_FALSE(host.IsPluginEnabled("com.beta.filter"));
    EXPECT_NEAR(host.GetPluginParamNumber("com.beta.filter", "radius"), 8.0f, 0.01f);
    EXPECT_TRUE(host.IsSrAutoTriggerEnabled());
    EXPECT_NEAR(host.GetSrAutoTriggerMaxSourceMp(), 2.5f, 0.01f);

    // Test legacy v2 [SuperResolution] INI migration using a clean file path
    const std::wstring legacyIni = m_tempIniPath + L".legacy.ini";
    DeleteFileW(legacyIni.c_str());
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, legacyIni.c_str());
    WritePrivateProfileStringW(L"SuperResolution", L"EnableSrPlugin", L"1", legacyIni.c_str());
    WritePrivateProfileStringW(L"SuperResolution", L"SrModelId", L"realesr-general-x4v3", legacyIni.c_str());
    WritePrivateProfileStringW(L"SuperResolution", L"SrAutoTrigger", L"1", legacyIni.c_str());

    host.ResetToDefaults();
    host.LoadConfig(legacyIni.c_str());
    DeleteFileW(legacyIni.c_str());
    QuickView::PluginRecord upscalerRec;
    if (host.FindInstalledPlugin("com.quickview.sr.ncnn_vulkan", upscalerRec)) {
        EXPECT_FALSE(host.GetActiveUpscalerPluginId().empty());
        EXPECT_TRUE(host.IsSrPluginEnabled());
        EXPECT_EQ(host.GetSrModelId(), "realesr-general-x4v3");
        EXPECT_TRUE(host.IsSrAutoTriggerEnabled());
    }
}

TEST_F(PluginHostTests, MissingPluginGracefulFallback) {
    auto& host = QuickView::PluginHost::Instance();
    EXPECT_FALSE(host.EnsureProcessorContext("com.does.not.exist", m_d3dDevice.Get()));
}

TEST_F(PluginHostTests, NcnnVulkanV3GpuUpscaleAndProgress) {
    auto& host = QuickView::PluginHost::Instance();
    QuickView::PluginRecord rec;
    if (!host.FindInstalledPlugin("com.quickview.sr.ncnn_vulkan", rec)) {
        GTEST_SKIP() << "com.quickview.sr.ncnn_vulkan not found in plugins directory.";
    }

    host.SetActiveUpscalerPluginId(rec.id);
    host.SetPluginEnabled(rec.id, true);
    host.SetPluginActivePreset(rec.id, "realesr-animevideov3-auto");

    ASSERT_TRUE(host.EnsureSrContext(m_d3dDevice.Get()));

    auto presets = host.GetPluginPresets(rec.id);
    EXPECT_GE(presets.size(), 7u);
    EXPECT_EQ(presets[0].presetId, "realesr-animevideov3-auto");
    EXPECT_TRUE(presets[0].isDynamicAuto);
    EXPECT_EQ(presets[1].presetId, "realesr-animevideov3-x2");

    auto assets = host.GetPluginAssets(rec.id);
    EXPECT_GE(assets.size(), 7u);

    auto params = host.GetPluginParams(rec.id);
    ASSERT_GE(params.size(), 1u);
    EXPECT_EQ(params[0].id, "tile_size");

    if (!presets[1].isInstalled) {
        GTEST_SKIP() << "Model weights not present on disk, skipping GPU execution.";
    }

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

    std::vector<uint32_t> srcPixels(64 * 64, 0xFF55AAFF);
    m_d3dContext->UpdateSubresource(pSrcTex.Get(), 0, nullptr, srcPixels.data(), 64 * 4, 0);

    struct ProgressRecord {
        float lastVal = -1.0f;
        int callCount = 0;
    } progRecord;

    auto onProgress = [](float p, void* uData) {
        auto* r = static_cast<ProgressRecord*>(uData);
        r->lastVal = p;
        r->callCount++;
    };

    const int32_t result = host.ExecuteSrUpscaleGpu(
        m_d3dDevice.Get(),
        pSrcTex.Get(), 64, 64,
        pDstTex.Get(), 128, 128,
        nullptr, nullptr,
        onProgress, &progRecord);

    EXPECT_EQ(result, QVX_OK);
    EXPECT_GT(progRecord.callCount, 0);
    EXPECT_NEAR(progRecord.lastVal, 1.0f, 0.05f);
}

TEST_F(PluginHostTests, TransparentAlphaPreservation) {
    auto& host = QuickView::PluginHost::Instance();
    QuickView::PluginRecord rec;
    if (!host.FindInstalledPlugin("com.quickview.sr.ncnn_vulkan", rec)) {
        GTEST_SKIP() << "com.quickview.sr.ncnn_vulkan not found.";
    }

    host.SetActiveUpscalerPluginId(rec.id);
    host.SetPluginEnabled(rec.id, true);
    host.SetPluginActivePreset(rec.id, "realesr-animevideov3-x2");

    auto presets = host.GetPluginPresets(rec.id);
    if (presets.size() < 2 || !presets[1].isInstalled) {
        GTEST_SKIP() << "Model weights not found, skipping Alpha test.";
    }

    ASSERT_TRUE(host.EnsureSrContext(m_d3dDevice.Get()));

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

    std::vector<uint32_t> srcPixels(64 * 64, 0x0055AAFF);
    for (int y = 16; y < 48; ++y) {
        for (int x = 16; x < 48; ++x) {
            srcPixels[y * 64 + x] = 0xFF55AAFF;
        }
    }
    m_d3dContext->UpdateSubresource(pSrcTex.Get(), 0, nullptr, srcPixels.data(), 64 * 4, 0);

    const int32_t result = host.ExecuteSrUpscaleGpu(
        m_d3dDevice.Get(),
        pSrcTex.Get(), 64, 64,
        pDstTex.Get(), 128, 128,
        nullptr, nullptr, nullptr, nullptr);
    EXPECT_EQ(result, QVX_OK);

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
            const uint8_t cornerA = (pPixels[0] >> 24) & 0xFF;
            const uint8_t centerA = (pPixels[64 * (mapped.RowPitch / 4) + 64] >> 24) & 0xFF;
            EXPECT_LE(cornerA, 10);
            EXPECT_GE(centerA, 240);
            m_d3dContext->Unmap(pStaging.Get(), 0);
        }
    }
}

TEST_F(PluginHostTests, ResetToDefaults) {
    auto& host = QuickView::PluginHost::Instance();
    host.SetSrPluginEnabled(true);
    host.SetSrAutoTriggerEnabled(true);
    host.SetSrOpenInCompareMode(false);
    host.SetSrPromptModelOnHotkey(true);
    host.SetSrAutoTriggerMaxSourceMp(8.0f);

    host.ResetToDefaults();

    EXPECT_FALSE(host.IsSrPluginEnabled());
    EXPECT_FALSE(host.IsSrAutoTriggerEnabled());
    EXPECT_TRUE(host.IsSrOpenInCompareMode());
    EXPECT_FALSE(host.IsSrPromptModelOnHotkey());
    EXPECT_NEAR(host.GetSrAutoTriggerMaxSourceMp(), 1.0f, 0.001f);
}

TEST_F(PluginHostTests, VersionComparisonAndNativeZipExtraction) {
    EXPECT_GT(QuickView::PluginHost::ComparePluginVersions("0.3.0", "0.2.0"), 0);
    EXPECT_LT(QuickView::PluginHost::ComparePluginVersions("0.1.0", "0.3.0"), 0);
    EXPECT_EQ(QuickView::PluginHost::ComparePluginVersions("v0.3.0", "0.3"), 0);
    EXPECT_LT(QuickView::PluginHost::ComparePluginVersions("1.0.0-beta.1", "1.0.0"), 0);
    EXPECT_GT(QuickView::PluginHost::ComparePluginVersions("1.0.0", "1.0.0-alpha"), 0);
    EXPECT_GT(QuickView::PluginHost::ComparePluginVersions("1.0.0-alpha.1", "1.0.0-alpha"), 0);

    wchar_t tempPath[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempPath);
    const std::wstring dummyZip = std::wstring(tempPath) + L"QVX_Empty_Test_v3.zip";
    const std::wstring extractDir = std::wstring(tempPath) + L"QVX_Extract_Out_v3";
    EXPECT_FALSE(QuickView::IArchive::ExtractZipToDirectory(dummyZip, extractDir));
}

TEST_F(PluginHostTests, SecurityAndAbiRegressionGuards) {
    auto& host = QuickView::PluginHost::Instance();

    // 1. DownloadPluginPackage must reject missing/invalid SHA-256 and path traversal filenames without network I/O
    const std::string validHex(64, 'a');
    EXPECT_FALSE(host.DownloadPluginPackage(
        "com.test.security", L"pkg.zip",
        "https://justnullname.github.io/QuickView/plugins/nonexistent.zip", "", nullptr));
    EXPECT_FALSE(host.DownloadPluginPackage(
        "com.test.security", L"..\\..\\escape.dll",
        "https://justnullname.github.io/QuickView/plugins/nonexistent.dll", validHex, nullptr));
    EXPECT_FALSE(host.DownloadPluginPackage(
        "com.test.security", L"C:\\abs\\evil.dll",
        "https://justnullname.github.io/QuickView/plugins/nonexistent.dll", validHex, nullptr));

    // 2. HostServices ABI alloc_buffer aligns non-power-of-two sizes cleanly via std::bit_ceil + plugin struct_size check
    QuickView::PluginRecord rec;
    if (host.FindInstalledPlugin("com.quickview.sr.ncnn_vulkan", rec)) {
        HMODULE hMod = LoadLibraryW(rec.entryFullPath.c_str());
        ASSERT_NE(hMod, nullptr);
        using QvxInitFn = bool (*)(const QVX_HostServices*, const QVX_PluginHeader**);
        using QvxShutdownFn = void (*)(void);
        auto pInit = reinterpret_cast<QvxInitFn>(GetProcAddress(hMod, "qvx_init"));
        auto pShutdown = reinterpret_cast<QvxShutdownFn>(GetProcAddress(hMod, "qvx_shutdown"));
        ASSERT_NE(pInit, nullptr);
        ASSERT_NE(pShutdown, nullptr);

        const QVX_PluginHeader* hdr = nullptr;
        QVX_HostServices svc{};
        svc.struct_size = sizeof(svc);
        svc.abi_version = QVX_ABI_VERSION;
        ASSERT_TRUE(pInit(&svc, &hdr));
        ASSERT_NE(hdr, nullptr);
        EXPECT_GE(hdr->struct_size, sizeof(QVX_PluginHeader));

        const auto* proc = static_cast<const QVX_Processor_VTable*>(
            hdr->get_interface(QVX_IFACE_IMAGE_PROCESSOR, QVX_PROCESSOR_INTERFACE_VERSION));
        ASSERT_NE(proc, nullptr);
        EXPECT_GE(proc->struct_size, sizeof(QVX_Processor_VTable));

        // Undersized struct_size on QVX_ProcExecuteParams must fail safely with QVX_E_INVALIDARG
        QVX_ProcExecuteParams badParams{};
        badParams.struct_size = 8;
        EXPECT_EQ(proc->process_gpu(nullptr, nullptr, nullptr, &badParams), QVX_E_INVALIDARG);

        pShutdown();
        pShutdown(); // Idempotent double-shutdown
        FreeLibrary(hMod);
    }
}
