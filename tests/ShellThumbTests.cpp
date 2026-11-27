#include <gtest/gtest.h>
#include "Plugin/PluginHost.h"
#include "Plugin/PluginRegistry.h"
#include "Plugin/abi/qvx_core.h"
#include "Plugin/abi/qvx_shell_ext.h"
#include "ThumbWorker.h"
#include "ThumbWorkerProtocol.h"
#include "MiniTiff.h"
#include <objbase.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <thumbcache.h>
#include <zlib.h>
#include <cmath>
#include <vector>
#include <string>
#include <type_traits>

namespace {

// {8B4F2A19-6E3C-4D7A-9C51-2E7A9F0B4D31}
constexpr CLSID kTestClsidThumb = {
    0x8B4F2A19u, 0x6E3Cu, 0x4D7Au, {0x9Cu, 0x51u, 0x2Eu, 0x7Au, 0x9Fu, 0x0Bu, 0x4Du, 0x31u}
};

struct ScopedQvxHandle {
    HMODULE hMod = nullptr;
    const QVX_PluginHeader* hdr = nullptr;
    void (*pShutdown)(void) = nullptr;
    ~ScopedQvxHandle() {
        if (hdr) {
            if (hdr->set_param_number) hdr->set_param_number("test_allow_stream", 0.0f);
            if (hdr->set_param_string) hdr->set_param_string("test_override_exts", "");
        }
        if (pShutdown) pShutdown();
        if (hMod) FreeLibrary(hMod);
    }
};

struct ScopedTestExtCleanup {
    const wchar_t* extKey = nullptr;
    ~ScopedTestExtCleanup() {
        if (extKey) RegDeleteTreeW(HKEY_CURRENT_USER, extKey);
    }
};

std::wstring GetExeDirPath() {
    wchar_t exePath[MAX_PATH * 2]{};
    GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(_countof(exePath)));
    PathRemoveFileSpecW(exePath);
    return std::wstring(exePath);
}

std::wstring GetShellThumbDllPath() {
    return GetExeDirPath() + L"\\plugins\\shell\\qv_shell_thumb\\qv_shell_thumb.qvx";
}

bool Write24BppBmpFile(const std::wstring& path, uint32_t w, uint32_t h, uint8_t b, uint8_t g, uint8_t r) {
    const uint32_t rowBytes = ((w * 3u + 3u) & ~3u);
    const uint32_t imgBytes = rowBytes * h;
    BITMAPFILEHEADER bfh{};
    BITMAPINFOHEADER bih{};
    bfh.bfType = 0x4D42;
    bfh.bfOffBits = sizeof(bfh) + sizeof(bih);
    bfh.bfSize = bfh.bfOffBits + imgBytes;
    bih.biSize = sizeof(bih);
    bih.biWidth = static_cast<LONG>(w);
    bih.biHeight = static_cast<LONG>(h);
    bih.biPlanes = 1;
    bih.biBitCount = 24;
    bih.biCompression = BI_RGB;
    bih.biSizeImage = imgBytes;

    std::vector<uint8_t> pixels(imgBytes, 0);
    for (uint32_t y = 0; y < h; ++y) {
        uint8_t* row = pixels.data() + static_cast<size_t>(y) * rowBytes;
        for (uint32_t x = 0; x < w; ++x) {
            row[x * 3 + 0] = b;
            row[x * 3 + 1] = g;
            row[x * 3 + 2] = r;
        }
    }

    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(hFile, &bfh, sizeof(bfh), &written, nullptr) &&
              WriteFile(hFile, &bih, sizeof(bih), &written, nullptr) &&
              WriteFile(hFile, pixels.data(), imgBytes, &written, nullptr);
    CloseHandle(hFile);
    return ok != FALSE;
}

class MemoryStreamWithName final : public IStream {
public:
    MemoryStreamWithName(std::vector<uint8_t> data, std::wstring name)
        : m_data(std::move(data)), m_name(std::move(name)) {}

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ISequentialStream) || IsEqualIID(riid, IID_IStream)) {
            *ppv = static_cast<IStream*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&m_ref)); }
    IFACEMETHODIMP_(ULONG) Release() override {
        ULONG c = static_cast<ULONG>(InterlockedDecrement(&m_ref));
        if (c == 0) delete this;
        return c;
    }
    IFACEMETHODIMP Read(void* pv, ULONG cb, ULONG* pcbRead) override {
        if (!pv) return STG_E_INVALIDPOINTER;
        const size_t avail = (m_pos < m_data.size()) ? (m_data.size() - m_pos) : 0;
        const size_t toRead = (cb < avail) ? cb : avail;
        if (toRead > 0) memcpy(pv, m_data.data() + m_pos, toRead);
        m_pos += toRead;
        if (pcbRead) *pcbRead = static_cast<ULONG>(toRead);
        return (toRead < cb) ? S_FALSE : S_OK;
    }
    IFACEMETHODIMP Write(const void*, ULONG, ULONG*) override { return STG_E_ACCESSDENIED; }
    IFACEMETHODIMP Seek(LARGE_INTEGER dlibMove, DWORD dwOrigin, ULARGE_INTEGER* plibNewPosition) override {
        int64_t next = 0;
        if (dwOrigin == STREAM_SEEK_SET) next = dlibMove.QuadPart;
        else if (dwOrigin == STREAM_SEEK_CUR) next = static_cast<int64_t>(m_pos) + dlibMove.QuadPart;
        else if (dwOrigin == STREAM_SEEK_END) next = static_cast<int64_t>(m_data.size()) + dlibMove.QuadPart;
        else return STG_E_INVALIDFUNCTION;
        if (next < 0) return STG_E_INVALIDFUNCTION;
        m_pos = static_cast<size_t>(next);
        if (plibNewPosition) plibNewPosition->QuadPart = m_pos;
        return S_OK;
    }
    IFACEMETHODIMP SetSize(ULARGE_INTEGER) override { return E_NOTIMPL; }
    IFACEMETHODIMP CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) override { return E_NOTIMPL; }
    IFACEMETHODIMP Commit(DWORD) override { return S_OK; }
    IFACEMETHODIMP Revert() override { return E_NOTIMPL; }
    IFACEMETHODIMP LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }
    IFACEMETHODIMP UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }
    IFACEMETHODIMP Stat(STATSTG* pstatstg, DWORD grfStatFlag) override {
        if (!pstatstg) return STG_E_INVALIDPOINTER;
        ZeroMemory(pstatstg, sizeof(*pstatstg));
        pstatstg->type = STGTY_STREAM;
        pstatstg->cbSize.QuadPart = m_data.size();
        if ((grfStatFlag & STATFLAG_NONAME) == 0 && !m_name.empty()) {
            const size_t bytes = (m_name.size() + 1) * sizeof(wchar_t);
            pstatstg->pwcsName = static_cast<LPOLESTR>(CoTaskMemAlloc(bytes));
            if (pstatstg->pwcsName) memcpy(pstatstg->pwcsName, m_name.c_str(), bytes);
        }
        return S_OK;
    }
    IFACEMETHODIMP Clone(IStream**) override { return E_NOTIMPL; }

private:
    LONG m_ref = 1;
    std::vector<uint8_t> m_data;
    std::wstring m_name;
    size_t m_pos = 0;
};

} // namespace

TEST(ShellThumbTests, AbiAndProtocolLayout) {
    EXPECT_EQ(QVX_SHELL_EXT_INTERFACE_VERSION, 0x00030000u);
    EXPECT_TRUE(std::is_standard_layout_v<QVX_ShellExt_VTable>);
    EXPECT_TRUE(std::is_trivially_copyable_v<QuickView::ThumbIpc::ThumbPipeRequestHeader>);
    EXPECT_TRUE(std::is_trivially_copyable_v<QuickView::ThumbIpc::ThumbPipeResponseHeader>);

    wchar_t pipeA[128]{};
    wchar_t pipeB[128]{};
    EXPECT_TRUE(QuickView::ThumbIpc::BuildSessionScopedPipeName(pipeA, _countof(pipeA)));
    EXPECT_TRUE(QuickView::ThumbIpc::BuildSessionScopedPipeName(pipeB, _countof(pipeB)));
    EXPECT_STREQ(pipeA, pipeB);
    EXPECT_NE(wcsstr(pipeA, L"\\\\.\\pipe\\QuickView_ThumbWorker_S"), nullptr);
}

TEST(ShellThumbTests, RegistryDiscoveryAndDefaultParams) {
    QuickView::PluginRegistry reg;
    reg.Scan();
    const auto* rec = reg.FindById("com.quickview.shell.thumb");
    if (!rec) {
        GTEST_SKIP() << "com.quickview.shell.thumb not staged alongside QuickViewTests.exe";
    }
    EXPECT_TRUE(rec->HasInterface(QVX_IFACE_SHELL_EXT));

    auto& host = QuickView::PluginHost::Instance();
    host.ResetToDefaults();
    host.RefreshRegistry();

    const auto params = host.GetPluginParams("com.quickview.shell.thumb");
    ASSERT_GE(params.size(), 8u);

    auto findDefaultBool = [&](const char* id) -> bool {
        for (const auto& p : params) {
            if (p.id == id) return p.defaultBool;
        }
        return false;
    };
    EXPECT_TRUE(findDefaultBool("grp_modern"));
    EXPECT_TRUE(findDefaultBool("grp_pro"));
    EXPECT_TRUE(findDefaultBool("grp_raw"));
    EXPECT_TRUE(findDefaultBool("grp_comic"));
    EXPECT_FALSE(findDefaultBool("grp_standard"));
    EXPECT_FALSE(findDefaultBool("grp_pdf"));
    EXPECT_TRUE(findDefaultBool("sync_gallery_cache"));
    EXPECT_FALSE(findDefaultBool("respect_native_wic"));

    ScopedTestExtCleanup regCleanup{ L"Software\\Classes\\.qv_shell_unit_test_ext" };
    host.SetPluginParamString("com.quickview.shell.thumb", "test_override_exts", ".qv_shell_unit_test_ext");
    host.SetPluginEnabled("com.quickview.shell.thumb", true);
    // .webp and .heic/.heif must remain OFF by default (handled natively or via Store HEVC extension)
    EXPECT_FALSE(host.ShouldWarmupSystemThumbCache(L"C:\\test\\photo.webp"));
    EXPECT_FALSE(host.ShouldWarmupSystemThumbCache(L"C:\\test\\photo.jpg"));
    EXPECT_FALSE(host.ShouldWarmupSystemThumbCache(L"C:\\test\\photo.heic"));
    EXPECT_FALSE(host.ShouldWarmupSystemThumbCache(L"C:\\test\\photo.heif"));
    EXPECT_TRUE(host.ShouldWarmupSystemThumbCache(L"C:\\test\\photo.jxl"));
    EXPECT_TRUE(host.ShouldWarmupSystemThumbCache(L"C:\\test\\photo.avif"));
    EXPECT_TRUE(host.ShouldWarmupSystemThumbCache(L"C:\\test\\artwork.psd"));
    EXPECT_TRUE(host.ShouldWarmupSystemThumbCache(L"C:\\test\\shot.cr3"));
    EXPECT_TRUE(host.ShouldWarmupSystemThumbCache(L"C:\\test\\manga.cbz"));
    host.SetPluginEnabled("com.quickview.shell.thumb", false);
    host.SetPluginParamString("com.quickview.shell.thumb", "test_override_exts", "");
    host.ResetToDefaults();
}

TEST(ShellThumbTests, DllCanUnloadNowSafetyWhileQvxInitialized) {
    const std::wstring dllPath = GetShellThumbDllPath();
    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        GTEST_SKIP() << "qv_shell_thumb.qvx not found at build output.";
    }
    HMODULE hMod = LoadLibraryW(dllPath.c_str());
    ASSERT_NE(hMod, nullptr);

    using QvxInitFn = bool (*)(const QVX_HostServices*, const QVX_PluginHeader**);
    using QvxShutdownFn = void (*)(void);
    using DllCanUnloadFn = HRESULT (STDAPICALLTYPE*)(void);

    auto pInit = reinterpret_cast<QvxInitFn>(GetProcAddress(hMod, "qvx_init"));
    auto pShutdown = reinterpret_cast<QvxShutdownFn>(GetProcAddress(hMod, "qvx_shutdown"));
    auto pCanUnload = reinterpret_cast<DllCanUnloadFn>(GetProcAddress(hMod, "DllCanUnloadNow"));
    ASSERT_NE(pInit, nullptr);
    ASSERT_NE(pShutdown, nullptr);
    ASSERT_NE(pCanUnload, nullptr);

    const QVX_PluginHeader* hdr = nullptr;
    QVX_HostServices svc{};
    svc.struct_size = sizeof(svc);
    svc.abi_version = QVX_ABI_VERSION;
    ASSERT_TRUE(pInit(&svc, &hdr));
    ASSERT_NE(hdr, nullptr);
    // Even with 0 active COM objects, DllCanUnloadNow MUST return S_FALSE while QVX is alive!
    EXPECT_EQ(pCanUnload(), S_FALSE);

    pShutdown();
    EXPECT_EQ(pCanUnload(), S_OK);
    FreeLibrary(hMod);
}

TEST(ShellThumbTests, AlphaConversionRgbVsStraightArgb) {
    // 1. Opaque pixels -> WTSAT_RGB
    uint8_t opaque[4 * 4] = {
        10, 20, 30, 255,
        40, 50, 60, 255,
        70, 80, 90, 255,
        100, 110, 120, 255
    };
    HBITMAP hOpaque = nullptr;
    WTS_ALPHATYPE alphaOpaque = WTSAT_UNKNOWN;
    ASSERT_EQ(QuickView::ThumbIpc::ConvertBgraToShellDibAndAlpha(
        opaque, 2, 2, 8, true, &hOpaque, &alphaOpaque), S_OK);
    ASSERT_NE(hOpaque, nullptr);
    EXPECT_EQ(alphaOpaque, WTSAT_RGB);
    DeleteObject(hOpaque);

    // 2. Semi-transparent Premultiplied PBGRA -> straight ARGB + WTSAT_ARGB
    // A = 128 (~0.502), Straight BGR = (128, 64, 32) -> Premultiplied BGR ~= (64, 32, 16)
    uint8_t premul[4 * 4] = {
        64, 32, 16, 128,
        0,  0,  0,  0,
        255, 255, 255, 255,
        64, 32, 16, 128
    };
    HBITMAP hAlpha = nullptr;
    WTS_ALPHATYPE alphaType = WTSAT_UNKNOWN;
    ASSERT_EQ(QuickView::ThumbIpc::ConvertBgraToShellDibAndAlpha(
        premul, 2, 2, 8, true, &hAlpha, &alphaType), S_OK);
    ASSERT_NE(hAlpha, nullptr);
    EXPECT_EQ(alphaType, WTSAT_ARGB);

    DIBSECTION ds{};
    ASSERT_EQ(GetObjectW(hAlpha, sizeof(ds), &ds), static_cast<int>(sizeof(ds)));
    ASSERT_NE(ds.dsBm.bmBits, nullptr);
    const uint8_t* outPx = static_cast<const uint8_t*>(ds.dsBm.bmBits);
    EXPECT_NEAR(outPx[0], 128, 2);
    EXPECT_NEAR(outPx[1], 64, 2);
    EXPECT_NEAR(outPx[2], 32, 2);
    EXPECT_EQ(outPx[3], 128);
    EXPECT_EQ(outPx[4], 0);
    EXPECT_EQ(outPx[7], 0);
    DeleteObject(hAlpha);
}

TEST(ShellThumbTests, CloudPlaceholderGuard) {
    using QuickView::ThumbIpc::IsCloudPlaceholderAttributes;
    EXPECT_TRUE(IsCloudPlaceholderAttributes(INVALID_FILE_ATTRIBUTES));
    EXPECT_TRUE(IsCloudPlaceholderAttributes(FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_OFFLINE));
    EXPECT_TRUE(IsCloudPlaceholderAttributes(FILE_ATTRIBUTE_ARCHIVE | 0x00400000u)); // RECALL_ON_DATA_ACCESS
    EXPECT_TRUE(IsCloudPlaceholderAttributes(FILE_ATTRIBUTE_NORMAL | 0x00040000u));  // RECALL_ON_OPEN
    EXPECT_FALSE(IsCloudPlaceholderAttributes(FILE_ATTRIBUTE_NORMAL));
    EXPECT_FALSE(IsCloudPlaceholderAttributes(FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_READONLY));
}

TEST(ShellThumbTests, HotCacheAntiResolutionInversion) {
    QuickView::ThumbHotCache cache;
    std::vector<uint8_t> pixels(300u * 300u * 4u, 0xCCu);
    cache.Insert("c:\\photos\\sample.jxl", 4096, 133000000000000000ull, 300, 300, 1200, false, pixels);

    std::vector<uint8_t> out(512u * 512u * 4u, 0);
    uint32_t w = 0, h = 0, stride = 0;
    uint8_t premul = 1;

    // 256px <= 300px with matching size & mtime -> HIT
    EXPECT_TRUE(cache.Lookup("c:\\photos\\sample.jxl", 4096, 133000000000000000ull, 256,
                             out.data(), static_cast<uint32_t>(out.size()), &w, &h, &stride, &premul));
    EXPECT_EQ(w, 300u);
    EXPECT_EQ(h, 300u);
    EXPECT_EQ(premul, 0u);

    // 320px > 300px -> MUST MISS (prevents upscaling blurry thumbnails into Explorer large icons)
    EXPECT_FALSE(cache.Lookup("c:\\photos\\sample.jxl", 4096, 133000000000000000ull, 320,
                              out.data(), static_cast<uint32_t>(out.size()), &w, &h, &stride, &premul));

    // Stale file size or mtime -> MUST MISS
    EXPECT_FALSE(cache.Lookup("c:\\photos\\sample.jxl", 4097, 133000000000000000ull, 256,
                              out.data(), static_cast<uint32_t>(out.size()), &w, &h, &stride, &premul));
    EXPECT_FALSE(cache.Lookup("c:\\photos\\sample.jxl", 4096, 133000000000000001ull, 256,
                              out.data(), static_cast<uint32_t>(out.size()), &w, &h, &stride, &premul));
}

TEST(ShellThumbTests, HotCacheAllowsSmallOriginalsWithoutInversion) {
    QuickView::ThumbHotCache cache;
    std::vector<uint8_t> smallPx(128u * 128u * 4u, 0xAAu);
    cache.Insert("c:\\photos\\tiny.png", 1024, 133000000000000000ull, 128, 128, 512, true, smallPx, 128);

    std::vector<uint8_t> out(256u * 256u * 4u, 0);
    uint32_t w = 0, h = 0, stride = 0;
    uint8_t premul = 0;

    // Genuinely small image (origMaxEdge == 128 < 256) MUST HIT at requestedCx == 256.
    EXPECT_TRUE(cache.Lookup("c:\\photos\\tiny.png", 1024, 133000000000000000ull, 256,
                             out.data(), static_cast<uint32_t>(out.size()), &w, &h, &stride, &premul));
    EXPECT_EQ(w, 128u);
    EXPECT_EQ(h, 128u);

    // Large original (2000px) cached as undersized 160px MUST MISS at requestedCx == 256.
    std::vector<uint8_t> undersizedPx(160u * 160u * 4u, 0xBBu);
    cache.Insert("c:\\photos\\huge.jxl", 2048, 133000000000000000ull, 160, 160, 640, false, undersizedPx, 2000);
    EXPECT_FALSE(cache.Lookup("c:\\photos\\huge.jxl", 2048, 133000000000000000ull, 256,
                              out.data(), static_cast<uint32_t>(out.size()), &w, &h, &stride, &premul));
}

TEST(ShellThumbTests, PoliteRegistryBackupAndRestore) {
    const std::wstring dllPath = GetShellThumbDllPath();
    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        GTEST_SKIP() << "qv_shell_thumb.qvx not found.";
    }
    HMODULE hMod = LoadLibraryW(dllPath.c_str());
    ASSERT_NE(hMod, nullptr);

    using QvxInitFn = bool (*)(const QVX_HostServices*, const QVX_PluginHeader**);
    using QvxShutdownFn = void (*)(void);
    auto pInit = reinterpret_cast<QvxInitFn>(GetProcAddress(hMod, "qvx_init"));
    auto pShutdown = reinterpret_cast<QvxShutdownFn>(GetProcAddress(hMod, "qvx_shutdown"));
    ASSERT_NE(pInit, nullptr);

    const QVX_PluginHeader* hdr = nullptr;
    QVX_HostServices svc{};
    svc.struct_size = sizeof(svc);
    svc.abi_version = QVX_ABI_VERSION;
    ASSERT_TRUE(pInit(&svc, &hdr));
    ASSERT_NE(hdr, nullptr);
    const auto* shellVt = static_cast<const QVX_ShellExt_VTable*>(
        hdr->get_interface(QVX_IFACE_SHELL_EXT, QVX_SHELL_EXT_INTERFACE_VERSION));
    ASSERT_NE(shellVt, nullptr);

    constexpr wchar_t kExtKey[] = L"Software\\Classes\\.qv_shell_unit_test_ext";
    constexpr wchar_t kShellExKey[] = L"Software\\Classes\\.qv_shell_unit_test_ext\\ShellEx\\{e357fccd-a995-4576-b01f-234630154e96}";
    constexpr wchar_t kFakeThirdPartyClsid[] = L"{11111111-2222-3333-4444-555555555555}";

    ScopedQvxHandle qvxGuard{ hMod, hdr, pShutdown };
    ScopedTestExtCleanup extCleanup{ kExtKey };

    HKEY hKey = nullptr;
    ASSERT_EQ(RegCreateKeyExW(HKEY_CURRENT_USER, kShellExKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hKey, nullptr), ERROR_SUCCESS);
    ASSERT_EQ(RegSetValueExW(hKey, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(kFakeThirdPartyClsid), sizeof(kFakeThirdPartyClsid)), ERROR_SUCCESS);
    RegCloseKey(hKey);

    hdr->set_param_string("test_override_exts", ".qv_shell_unit_test_ext");
    EXPECT_EQ(shellVt->register_shell(""), QVX_OK);

    wchar_t curVal[128]{};
    DWORD cb = sizeof(curVal);
    ASSERT_EQ(RegGetValueW(HKEY_CURRENT_USER, kShellExKey, nullptr, RRF_RT_REG_SZ, nullptr, curVal, &cb), ERROR_SUCCESS);
    EXPECT_STREQ(curVal, L"{8B4F2A19-6E3C-4D7A-9C51-2E7A9F0B4D31}");

    EXPECT_EQ(shellVt->unregister_shell(), QVX_OK);
    ZeroMemory(curVal, sizeof(curVal));
    cb = sizeof(curVal);
    ASSERT_EQ(RegGetValueW(HKEY_CURRENT_USER, kShellExKey, nullptr, RRF_RT_REG_SZ, nullptr, curVal, &cb), ERROR_SUCCESS);
    EXPECT_STREQ(curVal, kFakeThirdPartyClsid);
}

TEST(ShellThumbTests, EndToEndComThumbnailProviderPathAndStream) {
    const std::wstring dllPath = GetShellThumbDllPath();
    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        GTEST_SKIP() << "qv_shell_thumb.qvx not found.";
    }
    HMODULE hMod = LoadLibraryW(dllPath.c_str());
    ASSERT_NE(hMod, nullptr);

    using QvxInitFn = bool (*)(const QVX_HostServices*, const QVX_PluginHeader**);
    using QvxShutdownFn = void (*)(void);
    using DllGetClassObjectFn = HRESULT (STDAPICALLTYPE*)(REFCLSID, REFIID, void**);

    auto pInit = reinterpret_cast<QvxInitFn>(GetProcAddress(hMod, "qvx_init"));
    auto pShutdown = reinterpret_cast<QvxShutdownFn>(GetProcAddress(hMod, "qvx_shutdown"));
    auto pGetClassObj = reinterpret_cast<DllGetClassObjectFn>(GetProcAddress(hMod, "DllGetClassObject"));
    ASSERT_NE(pInit, nullptr);
    ASSERT_NE(pGetClassObj, nullptr);

    const QVX_PluginHeader* hdr = nullptr;
    QVX_HostServices svc{};
    svc.struct_size = sizeof(svc);
    svc.abi_version = QVX_ABI_VERSION;
    ASSERT_TRUE(pInit(&svc, &hdr));
    ScopedQvxHandle qvxGuard{ hMod, hdr, pShutdown };
    const auto* shellVt = static_cast<const QVX_ShellExt_VTable*>(
        hdr->get_interface(QVX_IFACE_SHELL_EXT, QVX_SHELL_EXT_INTERFACE_VERSION));
    ASSERT_NE(shellVt, nullptr);

    wchar_t tempDir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempDir);
    const std::wstring bmpPath = std::wstring(tempDir) + L"qv_shell_thumb_e2e.bmp";
    ASSERT_TRUE(Write24BppBmpFile(bmpPath, 96, 64, 10, 120, 220));

    // Test A: In-process Hot-Cache Fast Path
    std::string canonUtf8;
    uint64_t fsize = 0, mtime = 0;
    ASSERT_TRUE(QuickView::CanonicalizeThumbCacheKey(bmpPath, canonUtf8, fsize, mtime));

    QuickView::ThumbHotCache hotCache;
    std::vector<uint8_t> hotPixels(256u * 192u * 4u, 0xFFu);
    hotCache.Insert(canonUtf8, fsize, mtime, 256, 192, 256 * 4, true, std::move(hotPixels));

    auto lookupThunk = [](void* ud, const char* p, uint64_t sz, uint64_t mt, uint32_t cx,
                          uint8_t* buf, uint32_t maxB, uint32_t* w, uint32_t* h, uint32_t* s, uint8_t* premul) -> bool {
        return static_cast<QuickView::ThumbHotCache*>(ud)->Lookup(p, sz, mt, cx, buf, maxB, w, h, s, premul);
    };
    shellVt->set_inproc_thumb_provider(lookupThunk, &hotCache);

    IClassFactory* pFactory = nullptr;
    ASSERT_EQ(pGetClassObj(kTestClsidThumb, IID_IClassFactory, reinterpret_cast<void**>(&pFactory)), S_OK);
    ASSERT_NE(pFactory, nullptr);

    {
        IInitializeWithFile* pInitFile = nullptr;
        IThumbnailProvider* pThumb = nullptr;
        ASSERT_EQ(pFactory->CreateInstance(nullptr, IID_IInitializeWithFile, reinterpret_cast<void**>(&pInitFile)), S_OK);
        ASSERT_EQ(pInitFile->QueryInterface(IID_IThumbnailProvider, reinterpret_cast<void**>(&pThumb)), S_OK);

        ASSERT_EQ(pInitFile->Initialize(bmpPath.c_str(), STGM_READ), S_OK);
        HBITMAP hBmp = nullptr;
        WTS_ALPHATYPE alpha = WTSAT_UNKNOWN;
        ASSERT_EQ(pThumb->GetThumbnail(256, &hBmp, &alpha), S_OK);
        ASSERT_NE(hBmp, nullptr);
        EXPECT_EQ(alpha, WTSAT_RGB);
        BITMAP bm{};
        GetObjectW(hBmp, sizeof(bm), &bm);
        EXPECT_EQ(bm.bmWidth, 256);
        EXPECT_EQ(bm.bmHeight, 192);
        DeleteObject(hBmp);
        pThumb->Release();
        pInitFile->Release();
    }

    // Test B: Worker Pipe Path & Stream Mode (live QuickView.exe --thumb-worker)
    shellVt->set_inproc_thumb_provider(nullptr, nullptr);
    const std::wstring hostExe = GetExeDirPath() + L"\\QuickView.exe";
    if (GetFileAttributesW(hostExe.c_str()) != INVALID_FILE_ATTRIBUTES) {
        {
            IInitializeWithFile* pInitFile = nullptr;
            IThumbnailProvider* pThumb = nullptr;
            ASSERT_EQ(pFactory->CreateInstance(nullptr, IID_IInitializeWithFile, reinterpret_cast<void**>(&pInitFile)), S_OK);
            ASSERT_EQ(pInitFile->QueryInterface(IID_IThumbnailProvider, reinterpret_cast<void**>(&pThumb)), S_OK);
            ASSERT_EQ(pInitFile->Initialize(bmpPath.c_str(), STGM_READ), S_OK);
            HBITMAP hBmp = nullptr;
            WTS_ALPHATYPE alpha = WTSAT_UNKNOWN;
            EXPECT_EQ(pThumb->GetThumbnail(128, &hBmp, &alpha), S_OK);
            ASSERT_NE(hBmp, nullptr);
            DeleteObject(hBmp);
            pThumb->Release();
            pInitFile->Release();
        }
        {
            HANDLE hFile = CreateFileW(bmpPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            ASSERT_NE(hFile, INVALID_HANDLE_VALUE);
            const DWORD sz = GetFileSize(hFile, nullptr);
            std::vector<uint8_t> bytes(sz);
            DWORD readB = 0;
            ReadFile(hFile, bytes.data(), sz, &readB, nullptr);
            CloseHandle(hFile);

            auto* pMemStream = new MemoryStreamWithName(std::move(bytes), L"sample.bmp");
            hdr->set_param_number("test_allow_stream", 1.0f);
            IInitializeWithStream* pInitStream = nullptr;
            IThumbnailProvider* pThumb = nullptr;
            ASSERT_EQ(pFactory->CreateInstance(nullptr, IID_IInitializeWithStream, reinterpret_cast<void**>(&pInitStream)), S_OK);
            ASSERT_EQ(pInitStream->QueryInterface(IID_IThumbnailProvider, reinterpret_cast<void**>(&pThumb)), S_OK);
            ASSERT_EQ(pInitStream->Initialize(pMemStream, STGM_READ), S_OK);
            HBITMAP hBmp = nullptr;
            WTS_ALPHATYPE alpha = WTSAT_UNKNOWN;
            EXPECT_EQ(pThumb->GetThumbnail(128, &hBmp, &alpha), S_OK);
            ASSERT_NE(hBmp, nullptr);
            DeleteObject(hBmp);
            pThumb->Release();
            pInitStream->Release();
            pMemStream->Release();
            hdr->set_param_number("test_allow_stream", 0.0f);
        }
    }

    pFactory->Release();
    DeleteFileW(bmpPath.c_str());
}

TEST(ShellThumbTests, AvifYuv444ThumbnailDecodesCleanly) {
    constexpr const wchar_t* kAvifFiles[] = {
        L"D:\\Works\\Personal\\Coding\\QuickView\\Local-Files\\test_img\\avif\\avif-yuv444p12le.avif",
        L"D:\\Works\\Personal\\Coding\\QuickView\\Local-Files\\test_img\\avif\\avif-yuv444p.avif"
    };
    const std::wstring dllPath = GetShellThumbDllPath();
    const std::wstring hostExe = GetExeDirPath() + L"\\QuickView.exe";
    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(hostExe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        GTEST_SKIP() << "qv_shell_thumb.qvx or QuickView.exe not staged.";
    }
    for (const wchar_t* f : kAvifFiles) {
        if (GetFileAttributesW(f) == INVALID_FILE_ATTRIBUTES) {
            GTEST_SKIP() << "Local AVIF YUV444 sample not present on disk.";
        }
    }

    HMODULE hMod = LoadLibraryW(dllPath.c_str());
    ASSERT_NE(hMod, nullptr);

    using QvxInitFn = bool (*)(const QVX_HostServices*, const QVX_PluginHeader**);
    using QvxShutdownFn = void (*)(void);
    using DllGetClassObjectFn = HRESULT (STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
    auto pInit = reinterpret_cast<QvxInitFn>(GetProcAddress(hMod, "qvx_init"));
    auto pShutdown = reinterpret_cast<QvxShutdownFn>(GetProcAddress(hMod, "qvx_shutdown"));
    auto pGetClassObj = reinterpret_cast<DllGetClassObjectFn>(GetProcAddress(hMod, "DllGetClassObject"));
    ASSERT_NE(pInit, nullptr);
    ASSERT_NE(pGetClassObj, nullptr);

    const QVX_PluginHeader* hdr = nullptr;
    QVX_HostServices svc{};
    svc.struct_size = sizeof(svc);
    svc.abi_version = QVX_ABI_VERSION;
    ASSERT_TRUE(pInit(&svc, &hdr));
    ScopedQvxHandle qvxGuard{ hMod, hdr, pShutdown };
    hdr->set_param_number("test_allow_stream", 1.0f);

    IClassFactory* pFactory = nullptr;
    ASSERT_EQ(pGetClassObj(kTestClsidThumb, IID_IClassFactory, reinterpret_cast<void**>(&pFactory)), S_OK);
    ASSERT_NE(pFactory, nullptr);

    for (const wchar_t* avifPath : kAvifFiles) {
        // 1. File path path (exercises ImageLoader::LoadThumbnail -> CopyWicBitmapToThumbData 64bppRGBAHalf conversion)
        {
            IInitializeWithFile* pInitFile = nullptr;
            IThumbnailProvider* pThumb = nullptr;
            ASSERT_EQ(pFactory->CreateInstance(nullptr, IID_IInitializeWithFile, reinterpret_cast<void**>(&pInitFile)), S_OK);
            ASSERT_EQ(pInitFile->QueryInterface(IID_IThumbnailProvider, reinterpret_cast<void**>(&pThumb)), S_OK);
            ASSERT_EQ(pInitFile->Initialize(avifPath, STGM_READ), S_OK);
            HBITMAP hBmp = nullptr;
            WTS_ALPHATYPE alpha = WTSAT_UNKNOWN;
            EXPECT_EQ(pThumb->GetThumbnail(256, &hBmp, &alpha), S_OK);
            ASSERT_NE(hBmp, nullptr);
            BITMAP bm{};
            GetObjectW(hBmp, sizeof(bm), &bm);
            EXPECT_GT(bm.bmWidth, 0);
            EXPECT_LE(bm.bmWidth, 256);
            EXPECT_GT(bm.bmHeight, 0);
            EXPECT_LE(bm.bmHeight, 256);
            DeleteObject(hBmp);
            pThumb->Release();
            pInitFile->Release();
        }

        // 2. Stream mode path (exercises ImageLoader::LoadThumbnailFromMemory -> CopyWicBitmapToThumbData 64bppRGBAHalf conversion)
        {
            HANDLE hFile = CreateFileW(avifPath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            ASSERT_NE(hFile, INVALID_HANDLE_VALUE);
            const DWORD sz = GetFileSize(hFile, nullptr);
            std::vector<uint8_t> bytes(sz);
            DWORD readB = 0;
            ASSERT_TRUE(ReadFile(hFile, bytes.data(), sz, &readB, nullptr));
            CloseHandle(hFile);

            auto* pMemStream = new MemoryStreamWithName(std::move(bytes), L"sample.avif");
            IInitializeWithStream* pInitStream = nullptr;
            IThumbnailProvider* pThumb = nullptr;
            ASSERT_EQ(pFactory->CreateInstance(nullptr, IID_IInitializeWithStream, reinterpret_cast<void**>(&pInitStream)), S_OK);
            ASSERT_EQ(pInitStream->QueryInterface(IID_IThumbnailProvider, reinterpret_cast<void**>(&pThumb)), S_OK);
            ASSERT_EQ(pInitStream->Initialize(pMemStream, STGM_READ), S_OK);
            HBITMAP hBmp = nullptr;
            WTS_ALPHATYPE alpha = WTSAT_UNKNOWN;
            EXPECT_EQ(pThumb->GetThumbnail(256, &hBmp, &alpha), S_OK);
            ASSERT_NE(hBmp, nullptr);
            BITMAP bm{};
            GetObjectW(hBmp, sizeof(bm), &bm);
            EXPECT_GT(bm.bmWidth, 0);
            EXPECT_LE(bm.bmWidth, 256);
            EXPECT_GT(bm.bmHeight, 0);
            EXPECT_LE(bm.bmHeight, 256);
            DeleteObject(hBmp);
            pThumb->Release();
            pInitStream->Release();
            pMemStream->Release();
        }
    }

    pFactory->Release();
}

TEST(ShellThumbTests, Max1024ProtocolBoundary) {
    EXPECT_EQ(QuickView::ThumbIpc::kThumbPipeMaxEdge, 1024u);
    EXPECT_EQ(QuickView::ThumbIpc::kThumbPipeMaxPixelsBytes, 4u * 1024u * 1024u);
    EXPECT_EQ(QuickView::ThumbIpc::kThumbPipeTimeoutMs, 4500u);
    EXPECT_EQ(QuickView::ThumbIpc::kThumbWorkerDecodeCapMs, 4000u);
    EXPECT_EQ(QuickView::ThumbIpc::kThumbPipeSvgTimeoutMs, 7000u);
    EXPECT_EQ(QuickView::ThumbIpc::kThumbWorkerSvgDecodeCapMs, 6500u);
    EXPECT_GE(QuickView::ThumbIpc::kThumbWorkerIdleExitMs, 45000u);

    const std::wstring dllPath = GetShellThumbDllPath();
    const std::wstring hostExe = GetExeDirPath() + L"\\QuickView.exe";
    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(hostExe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        GTEST_SKIP() << "qv_shell_thumb.qvx or QuickView.exe not staged.";
    }

    wchar_t tempDir[MAX_PATH]{};
    ASSERT_GT(GetTempPathW(MAX_PATH, tempDir), 0u);
    const std::wstring tmpBmp = std::wstring(tempDir) + L"qv_max1024_test.bmp";
    ASSERT_TRUE(Write24BppBmpFile(tmpBmp, 1200, 1200, 0x20, 0x60, 0xB0));

    HMODULE hMod = LoadLibraryW(dllPath.c_str());
    ASSERT_NE(hMod, nullptr);
    using QvxInitFn = bool (*)(const QVX_HostServices*, const QVX_PluginHeader**);
    using QvxShutdownFn = void (*)(void);
    using DllGetClassObjectFn = HRESULT (STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
    auto pInit = reinterpret_cast<QvxInitFn>(GetProcAddress(hMod, "qvx_init"));
    auto pShutdown = reinterpret_cast<QvxShutdownFn>(GetProcAddress(hMod, "qvx_shutdown"));
    auto pGetClassObj = reinterpret_cast<DllGetClassObjectFn>(GetProcAddress(hMod, "DllGetClassObject"));
    ASSERT_NE(pInit, nullptr);
    ASSERT_NE(pGetClassObj, nullptr);

    const QVX_PluginHeader* hdr = nullptr;
    QVX_HostServices svc{};
    svc.struct_size = sizeof(svc);
    svc.abi_version = QVX_ABI_VERSION;
    ASSERT_TRUE(pInit(&svc, &hdr));
    ScopedQvxHandle qvxGuard{ hMod, hdr, pShutdown };
    hdr->set_param_number("test_allow_stream", 1.0f);

    IClassFactory* pFactory = nullptr;
    ASSERT_EQ(pGetClassObj(kTestClsidThumb, IID_IClassFactory, reinterpret_cast<void**>(&pFactory)), S_OK);
    ASSERT_NE(pFactory, nullptr);

    IInitializeWithFile* pInitFile = nullptr;
    IThumbnailProvider* pThumb = nullptr;
    ASSERT_EQ(pFactory->CreateInstance(nullptr, IID_IInitializeWithFile, reinterpret_cast<void**>(&pInitFile)), S_OK);
    ASSERT_EQ(pInitFile->QueryInterface(IID_IThumbnailProvider, reinterpret_cast<void**>(&pThumb)), S_OK);
    ASSERT_EQ(pInitFile->Initialize(tmpBmp.c_str(), STGM_READ), S_OK);

    HBITMAP hBmp = nullptr;
    WTS_ALPHATYPE alpha = WTSAT_UNKNOWN;
    EXPECT_EQ(pThumb->GetThumbnail(1024, &hBmp, &alpha), S_OK);
    ASSERT_NE(hBmp, nullptr);
    DIBSECTION ds{};
    ASSERT_EQ(GetObjectW(hBmp, sizeof(ds), &ds), static_cast<int>(sizeof(ds)));
    EXPECT_EQ(ds.dsBm.bmWidth, 1024);
    EXPECT_EQ(ds.dsBm.bmHeight, 1024);
    if (ds.dsBm.bmBits) {
        const uint8_t* px = static_cast<const uint8_t*>(ds.dsBm.bmBits);
        const size_t centerOff = (512u * static_cast<size_t>(ds.dsBm.bmWidthBytes)) + 512u * 4u;
        EXPECT_NEAR(px[centerOff + 0], 0x20, 4);
        EXPECT_NEAR(px[centerOff + 1], 0x60, 4);
        EXPECT_NEAR(px[centerOff + 2], 0xB0, 4);
    }
    DeleteObject(hBmp);
    pThumb->Release();
    pInitFile->Release();

    pFactory->Release();
    DeleteFileW(tmpBmp.c_str());
}

TEST(ShellThumbTests, BugFixesRegressionSuite) {
    // 1. MiniTiff::LoadThumbnailSampled cancellation predicate + valid sampled path
    {
        // Minimal 16x16 uncompressed RGB8 Classic TIFF (1 strip at offset 8 + 8 + 10*12 + 4 = 140)
        constexpr uint32_t W = 16, H = 16;
        constexpr uint32_t kPixelOffset = 140;
        constexpr uint32_t kPixelBytes = W * H * 3;
        std::vector<uint8_t> tiff(kPixelOffset + kPixelBytes, 0);
        tiff[0] = 'I'; tiff[1] = 'I'; tiff[2] = 42; tiff[3] = 0;
        tiff[4] = 8; // IFD offset = 8
        uint16_t tagCount = 10;
        memcpy(&tiff[8], &tagCount, 2);
        auto writeTag = [&](size_t idx, uint16_t tag, uint16_t type, uint32_t count, uint32_t val) {
            size_t off = 10 + idx * 12;
            memcpy(&tiff[off + 0], &tag, 2);
            memcpy(&tiff[off + 2], &type, 2);
            memcpy(&tiff[off + 4], &count, 4);
            memcpy(&tiff[off + 8], &val, 4);
        };
        writeTag(0, 256, 4, 1, W);             // ImageWidth
        writeTag(1, 257, 4, 1, H);             // ImageLength
        writeTag(2, 258, 3, 1, 8);             // BitsPerSample
        writeTag(3, 259, 3, 1, 1);             // Compression = None
        writeTag(4, 262, 3, 1, 2);             // Photometric = RGB
        writeTag(5, 273, 4, 1, kPixelOffset);  // StripOffsets
        writeTag(6, 277, 3, 1, 3);             // SamplesPerPixel
        writeTag(7, 278, 4, 1, H);             // RowsPerStrip
        writeTag(8, 279, 4, 1, kPixelBytes);   // StripByteCounts
        writeTag(9, 284, 3, 1, 1);             // PlanarConfig = Chunky
        for (size_t i = kPixelOffset; i < tiff.size(); i += 3) {
            tiff[i + 0] = 0xAA; tiff[i + 1] = 0x55; tiff[i + 2] = 0x22;
        }
        CImageLoader::ThumbData td;
        QuickView::SimplePredicate alwaysCancel{ [](void*) { return true; }, nullptr };
        QuickView::SimplePredicate neverCancel{ [](void*) { return false; }, nullptr };
        EXPECT_EQ(QuickView::MiniTiff::LoadThumbnailSampled(
            tiff.data(), tiff.size(), 16, &td, alwaysCancel), E_ABORT);
        EXPECT_FALSE(td.isValid);
        EXPECT_EQ(QuickView::MiniTiff::LoadThumbnailSampled(
            tiff.data(), tiff.size(), 16, &td, neverCancel), S_OK);
        EXPECT_TRUE(td.isValid);
        EXPECT_EQ(td.width, 16);
        EXPECT_EQ(td.height, 16);
    }

    const std::wstring dllPath = GetShellThumbDllPath();
    const std::wstring hostExe = GetExeDirPath() + L"\\QuickView.exe";
    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(hostExe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        GTEST_SKIP() << "qv_shell_thumb.qvx or QuickView.exe not staged.";
    }

    HMODULE hMod = LoadLibraryW(dllPath.c_str());
    ASSERT_NE(hMod, nullptr);
    using QvxInitFn = bool (*)(const QVX_HostServices*, const QVX_PluginHeader**);
    using QvxShutdownFn = void (*)(void);
    using DllGetClassObjectFn = HRESULT (STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
    auto pInit = reinterpret_cast<QvxInitFn>(GetProcAddress(hMod, "qvx_init"));
    auto pShutdown = reinterpret_cast<QvxShutdownFn>(GetProcAddress(hMod, "qvx_shutdown"));
    auto pGetClassObj = reinterpret_cast<DllGetClassObjectFn>(GetProcAddress(hMod, "DllGetClassObject"));
    ASSERT_NE(pInit, nullptr);
    ASSERT_NE(pGetClassObj, nullptr);

    const QVX_PluginHeader* hdr = nullptr;
    QVX_HostServices svc{};
    svc.struct_size = sizeof(svc);
    svc.abi_version = QVX_ABI_VERSION;
    ASSERT_TRUE(pInit(&svc, &hdr));
    ScopedQvxHandle qvxGuard{ hMod, hdr, pShutdown };

    IClassFactory* pFactory = nullptr;
    ASSERT_EQ(pGetClassObj(kTestClsidThumb, IID_IClassFactory, reinterpret_cast<void**>(&pFactory)), S_OK);
    ASSERT_NE(pFactory, nullptr);

    // 2. Medium-IL COM QI gating: Item OK, Stream denied unless explicitly requested
    {
        IThumbnailProvider* pThumbOnly = nullptr;
        ASSERT_EQ(pFactory->CreateInstance(nullptr, IID_IThumbnailProvider, reinterpret_cast<void**>(&pThumbOnly)), S_OK);
        IInitializeWithItem* pItem = nullptr;
        EXPECT_EQ(pThumbOnly->QueryInterface(IID_IInitializeWithItem, reinterpret_cast<void**>(&pItem)), S_OK);
        if (pItem) pItem->Release();
        IInitializeWithStream* pStream = nullptr;
        EXPECT_EQ(pThumbOnly->QueryInterface(IID_IInitializeWithStream, reinterpret_cast<void**>(&pStream)), E_NOINTERFACE);
        if (pStream) pStream->Release();
        pThumbOnly->Release();
    }

    auto testFileThumb = [&](const wchar_t* path, int targetCx, int* outW = nullptr, int* outH = nullptr) -> HRESULT {
        IInitializeWithFile* pInitFile = nullptr;
        IThumbnailProvider* pThumb = nullptr;
        if (FAILED(pFactory->CreateInstance(nullptr, IID_IInitializeWithFile, reinterpret_cast<void**>(&pInitFile)))) return E_FAIL;
        if (FAILED(pInitFile->QueryInterface(IID_IThumbnailProvider, reinterpret_cast<void**>(&pThumb)))) {
            pInitFile->Release();
            return E_FAIL;
        }
        HRESULT hr = pInitFile->Initialize(path, STGM_READ);
        if (SUCCEEDED(hr)) {
            HBITMAP hBmp = nullptr;
            WTS_ALPHATYPE alpha = WTSAT_UNKNOWN;
            hr = pThumb->GetThumbnail(static_cast<UINT>(targetCx), &hBmp, &alpha);
            if (SUCCEEDED(hr)) {
                if (!hBmp) {
                    hr = E_FAIL;
                } else {
                    BITMAP bm{};
                    GetObjectW(hBmp, sizeof(bm), &bm);
                    if (outW) *outW = bm.bmWidth;
                    if (outH) *outH = bm.bmHeight;
                    if (bm.bmWidth <= 0 || bm.bmHeight <= 0) hr = E_FAIL;
                    DeleteObject(hBmp);
                }
            }
        }
        pThumb->Release();
        pInitFile->Release();
        return hr;
    };

    // 3. Synthetic 00 00 02 00 TGA (must not misclassify as ICO/CUR)
    {
        wchar_t tempDir[MAX_PATH]{};
        ASSERT_GT(GetTempPathW(MAX_PATH, tempDir), 0u);
        const std::wstring tmpTga = std::wstring(tempDir) + L"qv_tga_magic_test.tga";
        std::vector<uint8_t> tga(18u + 16u * 16u * 3u, 0);
        tga[2] = 2; // uncompressed true-color -> 00 00 02 00 magic
        tga[12] = 16; tga[13] = 0; // width = 16
        tga[14] = 16; tga[15] = 0; // height = 16
        tga[16] = 24; tga[17] = 0x20;
        for (size_t i = 18; i < tga.size(); i += 3) {
            tga[i + 0] = 0x10; tga[i + 1] = 0x80; tga[i + 2] = 0xE0;
        }
        HANDLE hf = CreateFileW(tmpTga.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        ASSERT_NE(hf, INVALID_HANDLE_VALUE);
        DWORD written = 0;
        ASSERT_TRUE(WriteFile(hf, tga.data(), static_cast<DWORD>(tga.size()), &written, nullptr));
        CloseHandle(hf);
        int w = 0, h = 0;
        EXPECT_EQ(testFileThumb(tmpTga.c_str(), 128, &w, &h), S_OK);
        EXPECT_EQ(w, 16);
        EXPECT_EQ(h, 16);
        DeleteFileW(tmpTga.c_str());
    }

    // 4. Local repo regression assets (when present)
    const wchar_t* bugSvg = L"D:\\Works\\Personal\\Coding\\QuickView\\Local-Files\\test_img\\svg\\bug260717.svg";
    if (GetFileAttributesW(bugSvg) != INVALID_FILE_ATTRIBUTES) {
        HANDLE hFile = CreateFileW(bugSvg, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        ASSERT_NE(hFile, INVALID_HANDLE_VALUE);
        const DWORD sz = GetFileSize(hFile, nullptr);
        std::vector<uint8_t> bytes(sz);
        DWORD readB = 0;
        ASSERT_TRUE(ReadFile(hFile, bytes.data(), sz, &readB, nullptr));
        CloseHandle(hFile);

        hdr->set_param_number("test_allow_stream", 1.0f);
        auto* pMemStream = new MemoryStreamWithName(std::move(bytes), L"bug260717.svg");
        IInitializeWithStream* pInitStream = nullptr;
        IThumbnailProvider* pThumb = nullptr;
        ASSERT_EQ(pFactory->CreateInstance(nullptr, IID_IInitializeWithStream, reinterpret_cast<void**>(&pInitStream)), S_OK);
        ASSERT_EQ(pInitStream->QueryInterface(IID_IThumbnailProvider, reinterpret_cast<void**>(&pThumb)), S_OK);
        ASSERT_EQ(pInitStream->Initialize(pMemStream, STGM_READ), S_OK);
        HBITMAP hBmp = nullptr;
        WTS_ALPHATYPE alpha = WTSAT_UNKNOWN;
        EXPECT_EQ(pThumb->GetThumbnail(256, &hBmp, &alpha), S_OK);
        ASSERT_NE(hBmp, nullptr);
        DIBSECTION ds{};
        ASSERT_EQ(GetObjectW(hBmp, sizeof(ds), &ds), static_cast<int>(sizeof(ds)));
        EXPECT_GT(ds.dsBm.bmWidth, 128);
        EXPECT_GT(ds.dsBm.bmHeight, 128);
        if (ds.dsBm.bmBits) {
            const uint8_t* bits = static_cast<const uint8_t*>(ds.dsBm.bmBits);
            int maxNonZeroX = -1, maxNonZeroY = -1;
            for (int y = 0; y < ds.dsBm.bmHeight; ++y) {
                const uint8_t* row = bits + static_cast<size_t>(y) * static_cast<size_t>(ds.dsBm.bmWidthBytes);
                for (int x = 0; x < ds.dsBm.bmWidth; ++x) {
                    if (row[x * 4 + 3] > 0 || row[x * 4 + 0] > 0 || row[x * 4 + 1] > 0 || row[x * 4 + 2] > 0) {
                        if (x > maxNonZeroX) maxNonZeroX = x;
                        if (y > maxNonZeroY) maxNonZeroY = y;
                    }
                }
            }
            EXPECT_GT(maxNonZeroX, ds.dsBm.bmWidth / 2);
            EXPECT_GT(maxNonZeroY, ds.dsBm.bmHeight / 2);
        }
        DeleteObject(hBmp);
        pThumb->Release();
        pInitStream->Release();
        pMemStream->Release();
        hdr->set_param_number("test_allow_stream", 0.0f);
    }

    const wchar_t* okFiles[] = {
        L"D:\\Works\\Personal\\Coding\\QuickView\\Local-Files\\test_img\\svg\\Test_Image.svg",
        L"D:\\Works\\Personal\\Coding\\QuickView\\Local-Files\\test_img\\wuffs\\b5-attrib.tga",
        L"D:\\Works\\Personal\\Coding\\QuickView\\Local-Files\\test_img\\格式测试\\grid.wmf",
        L"D:\\Works\\Personal\\Coding\\QuickView\\Local-Files\\test_img\\格式测试\\Memo.emf",
        L"D:\\Works\\Personal\\Coding\\QuickView\\Local-Files\\test_img\\raw\\RAW_CANON_EOS_5DS.CR2",
        L"D:\\Works\\Personal\\Coding\\QuickView\\Local-Files\\test_img\\wuffs\\bug\\Example.png",
        L"D:\\Works\\Personal\\Coding\\QuickView\\Local-Files\\test_img\\psd\\酒店场景2.psd",
        L"D:\\Works\\Personal\\Coding\\QuickView\\Local-Files\\test_img\\动画\\屏幕录制 2026-06-13 111646.gif",
        L"D:\\Works\\Personal\\Coding\\QuickView\\Local-Files\\test_img\\wuffs\\test.cbz",
        L"D:\\Works\\Personal\\Coding\\QuickView\\Local-Files\\test_img\\tile\\noirlab2424a.tif"
    };
    for (const wchar_t* sample : okFiles) {
        if (GetFileAttributesW(sample) == INVALID_FILE_ATTRIBUTES) continue;
        int w = 0, h = 0;
        const ULONGLONG t0 = GetTickCount64();
        const HRESULT hr = testFileThumb(sample, 256, &w, &h);
        const ULONGLONG dt = GetTickCount64() - t0;
        EXPECT_EQ(hr, S_OK) << "Sample failed: " << testing::PrintToString(sample);
        EXPECT_LT(dt, 9000ULL) << "Sample too slow: " << dt << "ms";
    }

    // 4b. gemini.svg (<mask> + <filter> WebView2 STA path): transparent corners + opaque star center
    const wchar_t* geminiSvg = L"D:\\Works\\Personal\\Coding\\QuickView\\Local-Files\\test_img\\svg\\gemini.svg";
    if (GetFileAttributesW(geminiSvg) != INVALID_FILE_ATTRIBUTES) {
        IInitializeWithFile* pInitFile = nullptr;
        IThumbnailProvider* pThumb = nullptr;
        ASSERT_EQ(pFactory->CreateInstance(nullptr, IID_IInitializeWithFile, reinterpret_cast<void**>(&pInitFile)), S_OK);
        ASSERT_EQ(pInitFile->QueryInterface(IID_IThumbnailProvider, reinterpret_cast<void**>(&pThumb)), S_OK);
        ASSERT_EQ(pInitFile->Initialize(geminiSvg, STGM_READ), S_OK);
        HBITMAP hBmp = nullptr;
        WTS_ALPHATYPE alpha = WTSAT_UNKNOWN;
        const HRESULT hr = pThumb->GetThumbnail(256, &hBmp, &alpha);
        EXPECT_EQ(hr, S_OK);
        if (SUCCEEDED(hr) && hBmp) {
            DIBSECTION ds{};
            ASSERT_EQ(GetObjectW(hBmp, sizeof(ds), &ds), static_cast<int>(sizeof(ds)));
            EXPECT_EQ(ds.dsBm.bmWidth, 256);
            EXPECT_EQ(ds.dsBm.bmHeight, 256);
            EXPECT_EQ(alpha, WTSAT_ARGB);
            if (ds.dsBm.bmBits) {
                const uint8_t* bits = static_cast<const uint8_t*>(ds.dsBm.bmBits);
                const uint8_t cornerA = bits[2 * static_cast<size_t>(ds.dsBm.bmWidthBytes) + 2 * 4 + 3];
                const uint8_t centerA = bits[128 * static_cast<size_t>(ds.dsBm.bmWidthBytes) + 128 * 4 + 3];
                EXPECT_LT(cornerA, 16u) << "gemini.svg corner must stay transparent (mask applied)";
                EXPECT_GT(centerA, 200u) << "gemini.svg center must be opaque star body";
            }
            DeleteObject(hBmp);
        }
        pThumb->Release();
        pInitFile->Release();
    }

    // 4c. Giant un-previewed HEIC (space.heic 122.5 MP) must trip 256 MiB pixel budget in < 500ms
    const wchar_t* spaceHeic = L"D:\\Works\\Personal\\Coding\\QuickView\\Local-Files\\test_img\\tile\\space.heic";
    if (GetFileAttributesW(spaceHeic) != INVALID_FILE_ATTRIBUTES) {
        const ULONGLONG t0 = GetTickCount64();
        int w = 0, h = 0;
        const HRESULT hr = testFileThumb(spaceHeic, 256, &w, &h);
        const ULONGLONG dt = GetTickCount64() - t0;
        EXPECT_TRUE(FAILED(hr));
        EXPECT_LT(dt, 500ULL) << "space.heic pixel budget circuit breaker should reject in <500ms";
    }

    // 4c1. 51MP camera HEIF (sample1.heif) must succeed under the 64 MP HEIC ceiling
    const wchar_t* sampleHeif = L"D:\\Works\\Personal\\Coding\\QuickView\\Local-Files\\test_img\\格式测试\\sample1.heif";
    if (GetFileAttributesW(sampleHeif) != INVALID_FILE_ATTRIBUTES) {
        int w = 0, h = 0;
        const HRESULT hr = testFileThumb(sampleHeif, 256, &w, &h);
        EXPECT_EQ(hr, S_OK);
        EXPECT_EQ(w, 256);
        EXPECT_GT(h, 0);
    }

    const wchar_t* hugePng = L"D:\\Works\\Personal\\Coding\\QuickView\\Local-Files\\test_img\\tile\\noirlab2424a 拷贝.png";
    if (GetFileAttributesW(hugePng) != INVALID_FILE_ATTRIBUTES) {
        const ULONGLONG t0 = GetTickCount64();
        int w = 0, h = 0;
        const HRESULT hr = testFileThumb(hugePng, 256, &w, &h);
        const ULONGLONG dt = GetTickCount64() - t0;
        EXPECT_TRUE(FAILED(hr));
        EXPECT_LT(dt, 500ULL) << "Giant PNG streaming circuit breaker should reject in <500ms";
    }

    const wchar_t* hugeJxl = L"D:\\Works\\Personal\\Coding\\QuickView\\Local-Files\\test_img\\tile\\noirlab2424a.jxl";
    if (GetFileAttributesW(hugeJxl) != INVALID_FILE_ATTRIBUTES) {
        const ULONGLONG t0 = GetTickCount64();
        int w = 0, h = 0;
        (void)testFileThumb(hugeJxl, 256, &w, &h);
        const ULONGLONG dt = GetTickCount64() - t0;
        EXPECT_LT(dt, 2500ULL) << "Giant JXL must stay within memory budget without hanging";
    }

    const wchar_t* hugeJpg = L"D:\\Works\\Personal\\Coding\\QuickView\\Local-Files\\test_img\\tile\\noirlab2424a.jpg";
    if (GetFileAttributesW(hugeJpg) != INVALID_FILE_ATTRIBUTES) {
        int w = 0, h = 0;
        const HRESULT hr = testFileThumb(hugeJpg, 256, &w, &h);
        EXPECT_EQ(hr, S_OK);
        EXPECT_EQ(w, 256);
        EXPECT_GT(h, 0);
    }

    // 4c2. Alpha-bearing AVIF (test_webp_lossy.avif) must preserve solid alpha and zero green bleed
    const wchar_t* bleedAvif = L"D:\\Works\\Personal\\Coding\\QuickView\\Local-Files\\test_img\\avif\\test_webp_lossy.avif";
    if (GetFileAttributesW(bleedAvif) != INVALID_FILE_ATTRIBUTES) {
        IInitializeWithFile* pInitFile = nullptr;
        IThumbnailProvider* pThumb = nullptr;
        ASSERT_EQ(pFactory->CreateInstance(nullptr, IID_IInitializeWithFile, reinterpret_cast<void**>(&pInitFile)), S_OK);
        ASSERT_EQ(pInitFile->QueryInterface(IID_IThumbnailProvider, reinterpret_cast<void**>(&pThumb)), S_OK);
        ASSERT_EQ(pInitFile->Initialize(bleedAvif, STGM_READ), S_OK);
        HBITMAP hBmp = nullptr;
        WTS_ALPHATYPE alpha = WTSAT_UNKNOWN;
        const HRESULT hr = pThumb->GetThumbnail(256, &hBmp, &alpha);
        EXPECT_EQ(hr, S_OK);
        if (SUCCEEDED(hr) && hBmp) {
            DIBSECTION ds{};
            ASSERT_EQ(GetObjectW(hBmp, sizeof(ds), &ds), static_cast<int>(sizeof(ds)));
            EXPECT_EQ(ds.dsBm.bmWidth, 256);
            EXPECT_EQ(ds.dsBm.bmHeight, 256);
            EXPECT_EQ(alpha, WTSAT_ARGB);
            if (ds.dsBm.bmBits) {
                const uint8_t* bits = static_cast<const uint8_t*>(ds.dsBm.bmBits);
                int opaqueCount = 0;
                int greenBleedCount = 0;
                for (int y = 0; y < ds.dsBm.bmHeight; ++y) {
                    const uint8_t* row = bits + static_cast<size_t>(y) * static_cast<size_t>(ds.dsBm.bmWidthBytes);
                    for (int x = 0; x < ds.dsBm.bmWidth; ++x) {
                        const int b = row[x * 4 + 0];
                        const int g = row[x * 4 + 1];
                        const int r = row[x * 4 + 2];
                        const int a = row[x * 4 + 3];
                        if (a == 255) ++opaqueCount;
                        if (a > 50 && g > r + 15 && g > b + 15) ++greenBleedCount;
                    }
                }
                EXPECT_GT(opaqueCount, 4000) << "Solid foreground alpha must not degrade from 255 to 254";
                EXPECT_EQ(greenBleedCount, 0) << "Transparent RGB must not bleed green fringes into edges";
            }
            DeleteObject(hBmp);
        }
        pThumb->Release();
        pInitFile->Release();
    }

    // 4d. Malformed JPEG APP1 segLen=2 unsigned-underflow regression through worker pipe
    {
        wchar_t tempDir[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tempDir);
        const std::wstring badJpg = std::wstring(tempDir) + L"qv_bad_app1_seglen2.jpg";
        const uint8_t badBytes[] = { 0xFF, 0xD8, 0xFF, 0xE1, 0x00, 0x02, 0xFF, 0xD9 };
        HANDLE hf = CreateFileW(badJpg.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hf != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(hf, badBytes, sizeof(badBytes), &written, nullptr);
            CloseHandle(hf);
            int w = 0, h = 0;
            EXPECT_TRUE(FAILED(testFileThumb(badJpg.c_str(), 128, &w, &h)));
            DeleteFileW(badJpg.c_str());
        }
    }

    pFactory->Release();
}

TEST(ShellThumbTests, DibAlphaAndMiniTiffOrientationRegressions) {
    // 1. ConvertBgraToShellDibAndAlpha dimension/stride overflow guards
    {
        uint8_t dummy[16]{};
        HBITMAP hBmp = nullptr;
        WTS_ALPHATYPE alpha = WTSAT_UNKNOWN;
        EXPECT_EQ(QuickView::ThumbIpc::ConvertBgraToShellDibAndAlpha(
            dummy, 16385, 1, 16385 * 4, true, &hBmp, &alpha), E_INVALIDARG);
        EXPECT_EQ(hBmp, nullptr);
        EXPECT_EQ(QuickView::ThumbIpc::ConvertBgraToShellDibAndAlpha(
            dummy, 4, 4, 8, true, &hBmp, &alpha), E_INVALIDARG); // stride < width * 4
        EXPECT_EQ(hBmp, nullptr);
    }

    // 2. MiniTiff orientation 5 (Transpose) vs 8 (Rotate 270 CW) distinction on 4x2 asymmetric image
    auto makeOrientedTiff = [](uint16_t orientation) -> std::vector<uint8_t> {
        constexpr uint32_t W = 4, H = 2;
        constexpr uint32_t kPixelOffset = 148;
        constexpr uint32_t kPixelBytes = W * H * 3;
        std::vector<uint8_t> tiff(kPixelOffset + kPixelBytes, 0);
        tiff[0] = 'I'; tiff[1] = 'I'; tiff[2] = 42; tiff[3] = 0;
        tiff[4] = 8;
        uint16_t tagCount = 11;
        memcpy(&tiff[8], &tagCount, 2);
        auto writeTag = [&](size_t idx, uint16_t tag, uint16_t type, uint32_t count, uint32_t val) {
            size_t off = 10 + idx * 12;
            memcpy(&tiff[off + 0], &tag, 2);
            memcpy(&tiff[off + 2], &type, 2);
            memcpy(&tiff[off + 4], &count, 4);
            memcpy(&tiff[off + 8], &val, 4);
        };
        writeTag(0, 256, 4, 1, W);
        writeTag(1, 257, 4, 1, H);
        writeTag(2, 258, 3, 1, 8);
        writeTag(3, 259, 3, 1, 1);
        writeTag(4, 262, 3, 1, 2);
        writeTag(5, 273, 4, 1, kPixelOffset);
        writeTag(6, 274, 3, 1, orientation);
        writeTag(7, 277, 3, 1, 3);
        writeTag(8, 278, 4, 1, H);
        writeTag(9, 279, 4, 1, kPixelBytes);
        writeTag(10, 284, 3, 1, 1);
        // Set unique color at top-right source pixel (x=3, y=0): R=250, G=10, B=20
        const size_t topRight = kPixelOffset + 3 * 3;
        tiff[topRight + 0] = 250; tiff[topRight + 1] = 10; tiff[topRight + 2] = 20;
        return tiff;
    };

    QuickView::SimplePredicate noCancel{ [](void*) { return false; }, nullptr };
    CImageLoader::ThumbData td5, td8;
    const auto t5 = makeOrientedTiff(5);
    const auto t8 = makeOrientedTiff(8);
    ASSERT_EQ(QuickView::MiniTiff::LoadThumbnailSampled(t5.data(), t5.size(), 16, &td5, noCancel), S_OK);
    ASSERT_EQ(QuickView::MiniTiff::LoadThumbnailSampled(t8.data(), t8.size(), 16, &td8, noCancel), S_OK);
    EXPECT_EQ(td5.width, 2);
    EXPECT_EQ(td5.height, 4);
    EXPECT_EQ(td8.width, 2);
    EXPECT_EQ(td8.height, 4);
    // Orientation 5 (transpose): (x=3, y=0) -> (nx=0, ny=3) = bottom-left (row 3, col 0)
    // Orientation 8 (270 CW):    (x=3, y=0) -> (nx=0, ny=0) = top-left (row 0, col 0)
    const size_t px5_bl = 3u * static_cast<size_t>(td5.stride) + 0u;
    const size_t px8_tl = 0u;
    EXPECT_EQ(td5.pixels[px5_bl + 2], 250u); // BGRA red channel
    EXPECT_EQ(td8.pixels[px8_tl + 2], 250u);
    EXPECT_EQ(td5.pixels[0 + 2], 0u);
    EXPECT_EQ(td8.pixels[3u * static_cast<size_t>(td8.stride) + 2], 0u);
}

TEST(ShellThumbTests, TransparentPng16BitAnd8BitNoWhiteFringe) {
    const std::wstring qvxPath = GetShellThumbDllPath();
    const std::wstring hostExe = GetExeDirPath() + L"\\QuickView.exe";
    if (GetFileAttributesW(qvxPath.c_str()) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(hostExe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        GTEST_SKIP() << "qv_shell_thumb.qvx or QuickView.exe not staged.";
    }

    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HMODULE hMod = LoadLibraryW(qvxPath.c_str());
    ASSERT_NE(hMod, nullptr);
    using QvxInitFn = bool (*)(const QVX_HostServices*, const QVX_PluginHeader**);
    using QvxShutdownFn = void (*)(void);
    using DllGetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
    auto pInit = reinterpret_cast<QvxInitFn>(GetProcAddress(hMod, "qvx_init"));
    auto pShutdown = reinterpret_cast<QvxShutdownFn>(GetProcAddress(hMod, "qvx_shutdown"));
    auto pGetClassObj = reinterpret_cast<DllGetClassObjectFn>(GetProcAddress(hMod, "DllGetClassObject"));
    ASSERT_NE(pInit, nullptr);
    ASSERT_NE(pGetClassObj, nullptr);

    const QVX_PluginHeader* hdr = nullptr;
    QVX_HostServices svc{};
    svc.struct_size = sizeof(svc);
    svc.abi_version = QVX_ABI_VERSION;
    ASSERT_TRUE(pInit(&svc, &hdr));
    ScopedQvxHandle qvx{ hMod, hdr, pShutdown };

    IClassFactory* pFactory = nullptr;
    ASSERT_EQ(pGetClassObj(kTestClsidThumb, IID_IClassFactory, reinterpret_cast<void**>(&pFactory)), S_OK);
    ASSERT_NE(pFactory, nullptr);

    auto writePngFile = [](const wchar_t* path, bool is16Bit) -> bool {
        constexpr uint32_t W = 512, H = 512;
        const size_t bpp = is16Bit ? 8u : 4u;
        const size_t rawStride = 1u + static_cast<size_t>(W) * bpp;
        std::vector<uint8_t> raw(rawStride * H, 0);

        for (uint32_t y = 0; y < H; ++y) {
            uint8_t* row = raw.data() + static_cast<size_t>(y) * rawStride;
            row[0] = 0; // filter None
            uint8_t* px = row + 1;
            const double dy = static_cast<double>(y) - 255.5;
            for (uint32_t x = 0; x < W; ++x) {
                const double dx = static_cast<double>(x) - 255.5;
                const double d = std::sqrt(dx * dx + dy * dy);
                double alphaF = 0.0;
                if (d <= 128.0) {
                    alphaF = 1.0;
                } else if (d < 176.0) {
                    alphaF = (176.0 - d) / 48.0;
                }
                if (is16Bit) {
                    const uint16_t a16 = static_cast<uint16_t>(std::lround(alphaF * 65535.0));
                    const uint16_t r16 = 65535u;
                    const uint16_t g16 = (a16 == 0u) ? 65535u : 0u;
                    const uint16_t b16 = (a16 == 0u) ? 65535u : 0u;
                    uint8_t* d8 = px + static_cast<size_t>(x) * 8u;
                    d8[0] = static_cast<uint8_t>(r16 >> 8); d8[1] = static_cast<uint8_t>(r16 & 0xFFu);
                    d8[2] = static_cast<uint8_t>(g16 >> 8); d8[3] = static_cast<uint8_t>(g16 & 0xFFu);
                    d8[4] = static_cast<uint8_t>(b16 >> 8); d8[5] = static_cast<uint8_t>(b16 & 0xFFu);
                    d8[6] = static_cast<uint8_t>(a16 >> 8); d8[7] = static_cast<uint8_t>(a16 & 0xFFu);
                } else {
                    const uint8_t a8 = static_cast<uint8_t>(std::lround(alphaF * 255.0));
                    const uint8_t r8 = 255u;
                    const uint8_t g8 = (a8 == 0u) ? 255u : 0u;
                    const uint8_t b8 = (a8 == 0u) ? 255u : 0u;
                    uint8_t* d8 = px + static_cast<size_t>(x) * 4u;
                    d8[0] = r8; d8[1] = g8; d8[2] = b8; d8[3] = a8;
                }
            }
        }

        uLongf compLen = compressBound(static_cast<uLong>(raw.size()));
        std::vector<uint8_t> comp(compLen);
        if (compress2(comp.data(), &compLen, raw.data(), static_cast<uLong>(raw.size()), Z_BEST_SPEED) != Z_OK) {
            return false;
        }
        comp.resize(compLen);

        std::vector<uint8_t> png;
        png.reserve(128 + comp.size());
        static constexpr uint8_t kSig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
        png.insert(png.end(), kSig, kSig + 8);
        auto appendBE32 = [&](uint32_t v) {
            png.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
            png.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
            png.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
            png.push_back(static_cast<uint8_t>(v & 0xFFu));
        };
        auto appendChunk = [&](const char tag[4], const uint8_t* data, uint32_t len) {
            appendBE32(len);
            const size_t crcStart = png.size();
            png.insert(png.end(), tag, tag + 4);
            if (len > 0 && data) png.insert(png.end(), data, data + len);
            const uLong c = crc32(0L, png.data() + crcStart, 4u + len);
            appendBE32(static_cast<uint32_t>(c));
        };

        uint8_t ihdr[13]{};
        ihdr[0] = (W >> 24) & 0xFFu; ihdr[1] = (W >> 16) & 0xFFu; ihdr[2] = (W >> 8) & 0xFFu; ihdr[3] = W & 0xFFu;
        ihdr[4] = (H >> 24) & 0xFFu; ihdr[5] = (H >> 16) & 0xFFu; ihdr[6] = (H >> 8) & 0xFFu; ihdr[7] = H & 0xFFu;
        ihdr[8] = is16Bit ? 16 : 8;
        ihdr[9] = 6; // RGBA
        appendChunk("IHDR", ihdr, 13);
        appendChunk("IDAT", comp.data(), static_cast<uint32_t>(comp.size()));
        appendChunk("IEND", nullptr, 0);

        HANDLE hFile = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        const BOOL ok = WriteFile(hFile, png.data(), static_cast<DWORD>(png.size()), &written, nullptr);
        CloseHandle(hFile);
        return ok && written == png.size();
    };

    wchar_t tempDir[MAX_PATH]{};
    ASSERT_GT(GetTempPathW(MAX_PATH, tempDir), 0u);
    const std::wstring path8 = std::wstring(tempDir) + L"qv_test_alpha_8bit.png";
    const std::wstring path16 = std::wstring(tempDir) + L"qv_test_alpha_16bit.png";
    ASSERT_TRUE(writePngFile(path8.c_str(), false));
    ASSERT_TRUE(writePngFile(path16.c_str(), true));

    auto verifyNoWhiteFringe = [&](const wchar_t* filePath, const char* label) {
        IInitializeWithFile* pInitFile = nullptr;
        IThumbnailProvider* pThumb = nullptr;
        ASSERT_EQ(pFactory->CreateInstance(nullptr, IID_IInitializeWithFile, reinterpret_cast<void**>(&pInitFile)), S_OK) << label;
        ASSERT_EQ(pInitFile->QueryInterface(IID_IThumbnailProvider, reinterpret_cast<void**>(&pThumb)), S_OK) << label;
        ASSERT_EQ(pInitFile->Initialize(filePath, STGM_READ), S_OK) << label;

        HBITMAP hBmp = nullptr;
        WTS_ALPHATYPE alpha = WTSAT_UNKNOWN;
        ASSERT_EQ(pThumb->GetThumbnail(256, &hBmp, &alpha), S_OK) << label;
        ASSERT_NE(hBmp, nullptr) << label;
        EXPECT_EQ(alpha, WTSAT_ARGB) << label;

        BITMAP bm{};
        ASSERT_GT(GetObjectW(hBmp, sizeof(bm), &bm), 0) << label;
        EXPECT_EQ(bm.bmWidth, 256) << label;
        EXPECT_EQ(bm.bmHeight, 256) << label;

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = bm.bmWidth;
        bmi.bmiHeader.biHeight = -bm.bmHeight;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        std::vector<uint8_t> pixels(static_cast<size_t>(bm.bmWidth) * bm.bmHeight * 4u, 0);
        HDC hdc = GetDC(nullptr);
        ASSERT_EQ(GetDIBits(hdc, hBmp, 0, static_cast<UINT>(bm.bmHeight), pixels.data(), &bmi, DIB_RGB_COLORS), bm.bmHeight) << label;
        ReleaseDC(nullptr, hdc);

        int transparentZeroCount = 0;
        int semiEdgeCount = 0;
        int whiteFringeCount = 0;
        for (size_t i = 0; i < pixels.size(); i += 4) {
            const uint8_t b = pixels[i + 0];
            const uint8_t g = pixels[i + 1];
            const uint8_t r = pixels[i + 2];
            const uint8_t a = pixels[i + 3];
            if (a == 0u) {
                ++transparentZeroCount;
                if (r != 0u || g != 0u || b != 0u) ++whiteFringeCount;
            } else if (a >= 16u && a <= 239u) {
                ++semiEdgeCount;
                if (g > 6u || b > 6u || r < 230u) ++whiteFringeCount;
            }
        }
        EXPECT_GT(transparentZeroCount, 10000) << label;
        EXPECT_GT(semiEdgeCount, 1000) << label;
        EXPECT_EQ(whiteFringeCount, 0) << label;

        DeleteObject(hBmp);
        pThumb->Release();
        pInitFile->Release();
    };

    verifyNoWhiteFringe(path8.c_str(), "8-bit transparent PNG");
    verifyNoWhiteFringe(path16.c_str(), "16-bit transparent PNG");

    DeleteFileW(path8.c_str());
    DeleteFileW(path16.c_str());
    pFactory->Release();
    if (SUCCEEDED(coHr)) CoUninitialize();
}
