/*
 * QuickView OCR Core, Routing, Cancellation & Interactive Selection Tests
 * Copyright (C) 2026-Present QuickView Contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "pch.h"
#include <gtest/gtest.h>

#include "OcrTypes.h"
#include "WinOcrBackend.h"
#include "OcrManager.h"
#include "OcrOverlay.h"
#include "ToolbarVisibility.h"
#include "EditState.h"
#include "Plugin/PluginHost.h"
#include "ocr_postprocess.h"
#include "Plugin/PluginRegistry.h"
#include "Plugin/abi/qvx_analyzer.h"
#include "SystemInfo.h"

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

std::wstring FindRepoRoot(const std::wstring& startDir) {
    std::filesystem::path cur(startDir);
    for (int i = 0; i < 6; ++i) {
        if (std::filesystem::exists(cur / L"CMakeLists.txt") &&
            std::filesystem::exists(cur / L"plugins" / L"plugins_manifest.json")) {
            return cur.wstring();
        }
        if (!cur.has_parent_path()) break;
        cur = cur.parent_path();
    }
    return {};
}

std::vector<uint8_t> RenderTextCanvasBgra(uint32_t w, uint32_t h, const wchar_t* text, int fontPx, const wchar_t* fontName = L"Arial") {
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4, 0xFF);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = static_cast<LONG>(w);
    bmi.bmiHeader.biHeight = -static_cast<LONG>(h); // Top-down DIB
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC hdc = CreateCompatibleDC(nullptr);
    HBITMAP hbm = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (hdc && hbm && bits) {
        HGDIOBJ oldBmp = SelectObject(hdc, hbm);
        RECT rc{ 0, 0, static_cast<LONG>(w), static_cast<LONG>(h) };
        HBRUSH whiteBrush = CreateSolidBrush(RGB(255, 255, 255));
        FillRect(hdc, &rc, whiteBrush);
        DeleteObject(whiteBrush);

        HFONT hFont = CreateFontW(
            -fontPx, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, FF_SWISS | DEFAULT_PITCH, fontName);
        HGDIOBJ oldFont = SelectObject(hdc, hFont);
        SetTextColor(hdc, RGB(0, 0, 0));
        SetBkColor(hdc, RGB(255, 255, 255));
        DrawTextW(hdc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, oldFont);
        DeleteObject(hFont);
        SelectObject(hdc, oldBmp);
        GdiFlush();
        std::memcpy(out.data(), bits, out.size());
    }
    if (hbm) DeleteObject(hbm);
    if (hdc) DeleteDC(hdc);
    return out;
}

void WriteUtf8File(const std::wstring& path, const std::string& content) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, content.data(), static_cast<DWORD>(content.size()), &written, nullptr);
    CloseHandle(h);
}

std::wstring GetExeDirectory() {
    wchar_t buf[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring path(buf, n);
    const size_t slash = path.find_last_of(L"\\/");
    return (slash != std::wstring::npos) ? path.substr(0, slash) : L".";
}

HWND CreateTestMessageWindow() {
    static bool s_reg = false;
    if (!s_reg) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"QuickView_OcrUnitTestMsg";
        RegisterClassExW(&wc);
        s_reg = true;
    }
    return CreateWindowExW(0, L"QuickView_OcrUnitTestMsg", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
}

bool WaitForPostedMessage(HWND hwnd, UINT targetMsg, WPARAM& outWParam, DWORD timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    MSG msg{};
    while (GetTickCount64() < deadline) {
        if (PeekMessageW(&msg, hwnd, targetMsg, targetMsg, PM_REMOVE)) {
            outWParam = msg.wParam;
            return true;
        }
        Sleep(10);
    }
    return false;
}

QuickView::Ocr::OcrQuad MakeQuad(float x0, float y0, float x1, float y1) {
    QuickView::Ocr::OcrQuad q{};
    q.x[0] = x0; q.y[0] = y0;
    q.x[1] = x1; q.y[1] = y0;
    q.x[2] = x1; q.y[2] = y1;
    q.x[3] = x0; q.y[3] = y1;
    q.minX = x0; q.minY = y0;
    q.maxX = x1; q.maxY = y1;
    return q;
}

std::wstring ToUpperAscii(std::wstring s) {
    for (wchar_t& ch : s) {
        if (ch >= L'a' && ch <= L'z') ch = static_cast<wchar_t>(ch - L'a' + L'A');
    }
    return s;
}

} // namespace

TEST(OcrTests, AnalyzerAbiPodLayout) {
    EXPECT_EQ(QVX_ANALYZER_INTERFACE_VERSION, 0x00030000u);
    EXPECT_TRUE(std::is_standard_layout_v<QVX_NormQuad>);
    EXPECT_TRUE(std::is_standard_layout_v<QVX_AnalyzerPresetInfo>);
    EXPECT_TRUE(std::is_standard_layout_v<QVX_AnalyzerInput>);
    EXPECT_TRUE(std::is_standard_layout_v<QVX_AnalyzerToken>);
    EXPECT_TRUE(std::is_standard_layout_v<QVX_AnalyzerLine>);
    EXPECT_TRUE(std::is_standard_layout_v<QVX_AnalyzerResult>);
    EXPECT_TRUE(std::is_standard_layout_v<QVX_Analyzer_VTable>);
}

TEST(OcrTests, CjkSmartJoinRules) {
    using namespace QuickView::Ocr;

    std::vector<OcrToken> tokens;
    tokens.push_back({ MakeQuad(0, 0, 0.1f, 0.1f), L"你", 1.0f, true });
    tokens.push_back({ MakeQuad(0.1f, 0, 0.2f, 0.1f), L"好", 1.0f, true });
    tokens.push_back({ MakeQuad(0.2f, 0, 0.35f, 0.1f), L"Quick", 1.0f, true });
    tokens.push_back({ MakeQuad(0.35f, 0, 0.5f, 0.1f), L"View", 1.0f, true });
    tokens.push_back({ MakeQuad(0.5f, 0, 0.6f, 0.1f), L"！", 1.0f, false });

    EXPECT_FALSE(ShouldInsertSpaceBetween(L"你", L"好"));
    EXPECT_TRUE(ShouldInsertSpaceBetween(L"好", L"Quick"));
    EXPECT_TRUE(ShouldInsertSpaceBetween(L"Quick", L"View"));
    EXPECT_FALSE(ShouldInsertSpaceBetween(L"View", L"！"));
    EXPECT_FALSE(ShouldInsertSpaceBetween(L"中", L","));
    EXPECT_EQ(JoinTokensSmart(tokens, 0, static_cast<uint32_t>(tokens.size())), L"你好 Quick View！");
}

TEST(OcrTests, WinOcrSyntheticImageAndOversizedRoundTrip) {
    using namespace QuickView::Ocr;

    int32_t statusSmall = QVX_E_FAIL;
    int32_t statusWide = QVX_E_FAIL;
    int32_t statusTall = QVX_E_FAIL;
    OcrDocument docSmall;
    OcrDocument docWide;
    OcrDocument docTall;

    const auto imgSmall = RenderTextCanvasBgra(640, 160, L"QUICKVIEW", 64);
    const auto imgWide = RenderTextCanvasBgra(5200, 400, L"QUICKVIEW", 120);

    // Tall long-screenshot (640x5000): render text near top (y=80) and near bottom (y=4600)
    std::vector<uint8_t> imgTall(static_cast<size_t>(640) * 5000 * 4, 0xFF);
    const auto topBand = RenderTextCanvasBgra(640, 220, L"TOPTEXT", 56);
    const auto botBand = RenderTextCanvasBgra(640, 220, L"BOTTOMTEXT", 56);
    std::memcpy(imgTall.data() + static_cast<size_t>(640) * 80 * 4, topBand.data(), topBand.size());
    std::memcpy(imgTall.data() + static_cast<size_t>(640) * 4600 * 4, botBand.data(), botBand.size());

    std::thread mtaWorker([&]() {
        QVX_CancelToken noCancel{};
        std::wstring err;
        statusSmall = WinOcrBackend::Instance().Recognize(
            imgSmall.data(), 640, 160, 640 * 4, noCancel, docSmall, &err);
        if (statusSmall == HRESULT_FROM_WIN32(ERROR_RESOURCE_LANG_NOT_FOUND)) {
            return;
        }
        statusWide = WinOcrBackend::Instance().Recognize(
            imgWide.data(), 5200, 400, 5200 * 4, noCancel, docWide, &err);
        statusTall = WinOcrBackend::Instance().Recognize(
            imgTall.data(), 640, 5000, 640 * 4, noCancel, docTall, &err);
    });
    mtaWorker.join();

    if (statusSmall == HRESULT_FROM_WIN32(ERROR_RESOURCE_LANG_NOT_FOUND)) {
        GTEST_SKIP() << "No Windows OCR language pack installed on host.";
    }

    ASSERT_EQ(statusSmall, QVX_OK);
    ASSERT_FALSE(docSmall.lines.empty());

    ASSERT_EQ(statusWide, QVX_OK);
    ASSERT_FALSE(docWide.lines.empty());
    for (const auto& line : docWide.lines) {
        EXPECT_GE(line.quad.minX, 0.0f);
        EXPECT_LE(line.quad.maxX, 1.0f);
        EXPECT_GE(line.quad.minY, 0.0f);
        EXPECT_LE(line.quad.maxY, 1.0f);
    }

    // Tall long-screenshot should retain text from both top tile and bottom tile via auto-tiling
    ASSERT_EQ(statusTall, QVX_OK);
    ASSERT_GE(docTall.lines.size(), 2u);
    bool hasTop = false, hasBottom = false;
    for (const auto& line : docTall.lines) {
        EXPECT_GE(line.quad.minY, 0.0f);
        EXPECT_LE(line.quad.maxY, 1.0f);
        if (line.quad.maxY < 0.25f) hasTop = true;
        if (line.quad.minY > 0.75f) hasBottom = true;
    }
    EXPECT_TRUE(hasTop);
    EXPECT_TRUE(hasBottom);
}

TEST(OcrTests, ToolbarOcrModeButtonVisibility) {
    // Normal mode hides OCR action buttons
    EXPECT_FALSE(IsToolbarButtonVisibleInMode(ToolbarButtonID::OcrCopy, false, false, false, false, false, false, false));
    EXPECT_FALSE(IsToolbarButtonVisibleInMode(ToolbarButtonID::OcrSelectAll, false, false, false, false, false, false, false));
    EXPECT_FALSE(IsToolbarButtonVisibleInMode(ToolbarButtonID::OcrExit, false, false, false, false, false, false, false));

    // OCR mode replaces buttons with OcrCopy, OcrSelectAll, OcrExit, and Pin
    EXPECT_TRUE(IsToolbarButtonVisibleInMode(ToolbarButtonID::OcrCopy, false, false, false, false, false, false, false, true, false, false, false, false, true));
    EXPECT_TRUE(IsToolbarButtonVisibleInMode(ToolbarButtonID::OcrSelectAll, false, false, false, false, false, false, false, true, false, false, false, false, true));
    EXPECT_TRUE(IsToolbarButtonVisibleInMode(ToolbarButtonID::OcrExit, false, false, false, false, false, false, false, true, false, false, false, false, true));
    EXPECT_TRUE(IsToolbarButtonVisibleInMode(ToolbarButtonID::Pin, false, false, false, false, false, false, false, true, false, false, false, false, true));
    EXPECT_FALSE(IsToolbarButtonVisibleInMode(ToolbarButtonID::Prev, false, false, false, false, false, false, false, true, false, false, false, false, true));
    EXPECT_FALSE(IsToolbarButtonVisibleInMode(ToolbarButtonID::Ocr, false, false, false, false, false, false, false, true, false, false, false, false, true));

    // OcrText is compatible with click/chord mouse triggers, not drag/wheel triggers
    EXPECT_TRUE(IsMouseTriggerActionCompatible(MouseTrigger::XButton1Click, MouseActionTarget::OcrText));
    EXPECT_FALSE(IsMouseTriggerActionCompatible(MouseTrigger::LeftDrag, MouseActionTarget::OcrText));
}

TEST(OcrTests, MockAnalyzerPluginHotSwapFallbackCancelAndIdleUnload) {
    const std::wstring mockQvxSrc = GetExeDirectory() + L"\\qv_mock_ocr_test.qvx";
    if (GetFileAttributesW(mockQvxSrc.c_str()) == INVALID_FILE_ATTRIBUTES) {
        GTEST_SKIP() << "qv_mock_ocr_test.qvx not found next to QuickViewTests.exe";
    }

    wchar_t tempPath[MAX_PATH]{};
    ASSERT_GT(GetTempPathW(MAX_PATH, tempPath), 0u);
    const std::wstring tempRoot = std::wstring(tempPath) + L"QVX_MockOcrTest_" + std::to_wstring(GetCurrentProcessId());
    const std::wstring pluginDir = tempRoot + L"\\com.quickview.test.mock_ocr";
    ASSERT_TRUE(SystemInfo::EnsureDirectoryRecursive(pluginDir));

    const std::wstring stagedQvx = pluginDir + L"\\qv_mock_ocr_test.qvx";
    ASSERT_TRUE(CopyFileW(mockQvxSrc.c_str(), stagedQvx.c_str(), FALSE));

    WriteUtf8File(pluginDir + L"\\manifest.json", R"({
      "schema_version": 3,
      "abi_version": "3.0.0",
      "id": "com.quickview.test.mock_ocr",
      "version": "1.0.0",
      "entry": "qv_mock_ocr_test.qvx",
      "i18n": { "en-US": { "name": "Mock OCR", "desc": "Test OCR plugin" } },
      "interfaces": ["analyzer"],
      "analyzer": { "kind": "ocr", "caps": ["ocr", "multi_lang", "roi"] }
    })");

    auto& host = QuickView::PluginHost::Instance();
    host.ResetToDefaults();
    host.RefreshRegistry(tempRoot);

    const auto ocrPlugins = host.GetInstalledOcrPlugins();
    bool foundMock = false;
    for (const auto& rec : ocrPlugins) {
        if (rec.id == "com.quickview.test.mock_ocr") {
            foundMock = true;
            EXPECT_TRUE(rec.HasInterface(QVX_IFACE_ANALYZER));
            EXPECT_EQ(rec.analyzerKind, QVX_ANALYZER_KIND_OCR);
        }
    }
    ASSERT_TRUE(foundMock);

    host.SetPluginEnabled("com.quickview.test.mock_ocr", true);
    host.SetActiveOcrPluginId("com.quickview.test.mock_ocr");
    host.SetPluginActivePreset("com.quickview.test.mock_ocr", "mock_fast");
    host.SetPluginParamNumber("com.quickview.test.mock_ocr", "delay_ms", 0.0f);

    HWND msgHwnd = CreateTestMessageWindow();
    ASSERT_NE(msgHwnd, nullptr);
    constexpr UINT kMsgOcrDone = WM_APP + 201;

    std::vector<uint8_t> dummyPixels(64 * 64 * 4, 255);
    QuickView::Ocr::OcrRequest req1{};
    req1.notifyHwnd = msgHwnd;
    req1.notifyMsg = kMsgOcrDone;
    req1.imageId = 101;
    req1.bgraPixels = dummyPixels;
    req1.width = 64;
    req1.height = 64;
    req1.stride = 64 * 4;
    req1.preferredEngineId = "com.quickview.test.mock_ocr";

    const uint64_t gen1 = QuickView::Ocr::OcrManager::Instance().RequestAnalyze(req1);
    ASSERT_GT(gen1, 0u);

    WPARAM wp = 0;
    ASSERT_TRUE(WaitForPostedMessage(msgHwnd, kMsgOcrDone, wp, 5000));
    EXPECT_EQ(static_cast<uint64_t>(wp), gen1);

    QuickView::Ocr::OcrResultPackage pkg1;
    ASSERT_TRUE(QuickView::Ocr::OcrManager::Instance().ConsumeResult(gen1, pkg1));
    EXPECT_EQ(pkg1.statusCode, QVX_OK);
    EXPECT_EQ(pkg1.doc.engineId, "com.quickview.test.mock_ocr");
    ASSERT_EQ(pkg1.doc.lines.size(), 2u);
    EXPECT_EQ(pkg1.doc.lines[0].text, L"Mock OCR Line1");
    EXPECT_EQ(pkg1.doc.lines[1].text, L"测试");

    // Verify idle unload releases analyzer context without unloading DLL
    HMODULE hMockMod = GetModuleHandleW(L"qv_mock_ocr_test.qvx");
    ASSERT_NE(hMockMod, nullptr);
    using CounterFn = int32_t (*)();
    auto pGetCreate = reinterpret_cast<CounterFn>(GetProcAddress(hMockMod, "MockOcrGetCreateCount"));
    auto pGetDestroy = reinterpret_cast<CounterFn>(GetProcAddress(hMockMod, "MockOcrGetDestroyCount"));
    ASSERT_NE(pGetCreate, nullptr);
    ASSERT_NE(pGetDestroy, nullptr);
    const int32_t createsBefore = pGetCreate();
    const int32_t destroysBefore = pGetDestroy();
    host.ReleaseIdleAnalyzerContexts(0);
    EXPECT_EQ(pGetDestroy(), destroysBefore + 1);

    // Context recreates cleanly on next call
    const uint64_t genRecreate = QuickView::Ocr::OcrManager::Instance().RequestAnalyze(req1);
    ASSERT_TRUE(WaitForPostedMessage(msgHwnd, kMsgOcrDone, wp, 5000));
    QuickView::Ocr::OcrResultPackage pkgRecreate;
    ASSERT_TRUE(QuickView::Ocr::OcrManager::Instance().ConsumeResult(genRecreate, pkgRecreate));
    EXPECT_EQ(pkgRecreate.statusCode, QVX_OK);
    EXPECT_EQ(pGetCreate(), createsBefore + 1);

    // Verify generation/cancel race: slow task genSlow superseded/cancelled cleanly
    host.SetPluginParamNumber("com.quickview.test.mock_ocr", "delay_ms", 250.0f);
    const uint64_t genSlow = QuickView::Ocr::OcrManager::Instance().RequestAnalyze(req1);
    Sleep(25);
    QuickView::Ocr::OcrManager::Instance().CancelAll();
    QuickView::Ocr::OcrResultPackage pkgCancelled;
    EXPECT_FALSE(QuickView::Ocr::OcrManager::Instance().ConsumeResult(genSlow, pkgCancelled));

    // Verify missing-asset preset fails gracefully and OcrManager attempts fallback
    host.SetPluginParamNumber("com.quickview.test.mock_ocr", "delay_ms", 0.0f);
    host.SetPluginActivePreset("com.quickview.test.mock_ocr", "mock_missing_asset");
    QVX_AnalyzerInput rawInput{};
    rawInput.struct_size = sizeof(QVX_AnalyzerInput);
    rawInput.pixels = dummyPixels.data();
    rawInput.width = 64;
    rawInput.height = 64;
    rawInput.stride = 64 * 4;
    rawInput.format = QVX_PIXFMT_BGRA8888;
    QuickView::Ocr::OcrDocument directDoc;
    EXPECT_NE(host.ExecuteOcrAnalyze("com.quickview.test.mock_ocr", rawInput, directDoc), QVX_OK);

    // Tear down cleanly without shutting down process-wide singletons
    DestroyWindow(msgHwnd);
    host.SetPluginParamNumber("com.quickview.test.mock_ocr", "delay_ms", 0.0f);
    host.SetPluginActivePreset("com.quickview.test.mock_ocr", "mock_fast");
    host.SetPluginEnabled("com.quickview.test.mock_ocr", false);
    host.SetActiveOcrPluginId("");
    host.ReleaseAnalyzerContextNow();
    host.ResetToDefaults();
    host.RefreshRegistry();
    DeleteFileW((pluginDir + L"\\manifest.json").c_str());
    DeleteFileW(stagedQvx.c_str());
    RemoveDirectoryW(pluginDir.c_str());
    RemoveDirectoryW(tempRoot.c_str());
}

TEST(OcrTests, OverlaySelectionMathAndLineFallback) {
    using namespace QuickView::Ocr;
    using namespace QuickView::UI;

    OcrDocument doc;
    doc.imageId = 777;
    doc.generationId = 1;
    doc.sourceVisualW = 800;
    doc.sourceVisualH = 600;
    doc.engineId = "test";
    doc.engineLabel = L"TestEngine";

    // Line 0: "ColA  ColB"
    doc.tokens.push_back({ MakeQuad(0.10f, 0.10f, 0.25f, 0.16f), L"ColA", 1.0f, true });
    doc.tokens.push_back({ MakeQuad(0.55f, 0.10f, 0.70f, 0.16f), L"ColB", 1.0f, false });
    doc.lines.push_back({ MakeQuad(0.10f, 0.10f, 0.70f, 0.16f), L"ColA ColB", 0, 2, 1.0f, 0 });

    // Line 1: "Row2A Row2B"
    doc.tokens.push_back({ MakeQuad(0.10f, 0.25f, 0.25f, 0.31f), L"Row2A", 1.0f, true });
    doc.tokens.push_back({ MakeQuad(0.55f, 0.25f, 0.70f, 0.31f), L"Row2B", 1.0f, false });
    doc.lines.push_back({ MakeQuad(0.10f, 0.25f, 0.70f, 0.31f), L"Row2A Row2B", 2, 2, 1.0f, 0 });

    auto& overlay = OcrOverlay::Instance();
    overlay.SetDocument(doc, 800.0f, 600.0f);
    EXPECT_EQ(overlay.GetCachedLineRects().size(), 2u);
    EXPECT_EQ(overlay.GetCachedTokenRects().size(), 4u);

    // Hit test inside token 1 ("ColB") in image space (x=0.60*800=480, y=0.13*600=78)
    const OcrTextPosition pos = overlay.HitTestImageSpace(480.0f, 78.0f);
    EXPECT_EQ(pos.lineIdx, 0);
    EXPECT_EQ(pos.tokenIdx, 1);

    // Stream selection across whole token 0 + token 1
    overlay.SetStreamSelection({ 0, 0, 0 }, { 0, 1, 4 });
    EXPECT_EQ(overlay.GetSelectedText(), L"ColA ColB");

    // Shift rectangular box selection isolating left column (x in [70..220], y in [50..200])
    overlay.SetBoxSelection(70.0f, 50.0f, 220.0f, 200.0f);
    EXPECT_EQ(overlay.GetSelectedText(), L"ColA\r\nRow2A");

    // Line-only fallback (no tokens, only lines)
    OcrDocument lineOnly;
    lineOnly.sourceVisualW = 400;
    lineOnly.sourceVisualH = 200;
    lineOnly.lines.push_back({ MakeQuad(0.1f, 0.2f, 0.9f, 0.4f), L"LineOnlyText", 0, 0, 1.0f, 0 });
    overlay.SetDocument(lineOnly, 400.0f, 200.0f);
    EXPECT_EQ(overlay.GetCachedLineRects().size(), 1u);
    EXPECT_EQ(overlay.GetCachedTokenRects().size(), 1u);
    const OcrTextPosition lpos = overlay.HitTestImageSpace(200.0f, 60.0f);
    EXPECT_EQ(lpos.lineIdx, 0);
    overlay.SetBoxSelection(30.0f, 30.0f, 380.0f, 100.0f);
    EXPECT_EQ(overlay.GetSelectedText(), L"LineOnlyText");

    overlay.ClearDocumentKeepHidden();
}

TEST(OcrTests, PpOcrDbNetUnclipAndTileDedup) {
    using namespace QuickView::OcrNcnn;

    // 64x64 synthetic probability map with a horizontal text band and a 2x2 isolated noise dot
    std::vector<float> prob(64 * 64, 0.0f);
    for (int y = 20; y <= 24; ++y) {
        for (int x = 10; x <= 40; ++x) {
            prob[y * 64 + x] = 0.85f;
        }
    }
    // 2x2 noise dot (count=4 < 9 and compW < 3 -> filtered out)
    prob[5 * 64 + 5] = 0.9f; prob[5 * 64 + 6] = 0.9f;
    prob[6 * 64 + 5] = 0.9f; prob[6 * 64 + 6] = 0.9f;

    const auto boxes = ExtractDbBoxesFromProbMap(
        prob.data(), 64, 64, 2.0f, 2.0f, 0.0f, 0.0f, 128.0f, 128.0f,
        0.30f, 0.55f, 1.60f, 0);
    ASSERT_EQ(boxes.size(), 1u);
    // Unclip should expand vertically beyond raw pixels [40..50] to keep stroke margins
    EXPECT_LT(boxes[0].y0, 40.0f);
    EXPECT_GT(boxes[0].y1, 50.0f);
    EXPECT_GE(boxes[0].x0, 0.0f);
    EXPECT_LE(boxes[0].x1, 128.0f);

    // Cross-tile overlap + duplicate suppression + reading-order sort
    std::vector<DbBox> raw = {
        { 100.0f, 80.0f, 300.0f, 110.0f, 0.90f, 1 }, // Row 2 col 1
        { 10.0f,  80.0f, 120.0f, 110.0f, 0.95f, 0 }, // Row 2 col 0 (overlaps horizontally across tile 0 & 1 -> merge)
        { 20.0f,  10.0f, 150.0f, 40.0f,  0.80f, 0 }, // Row 1
        { 22.0f,  11.0f, 148.0f, 39.0f,  0.70f, 0 }, // Duplicate of Row 1 -> suppressed
    };
    const auto merged = MergeAndSortDetectedBoxes(std::move(raw));
    ASSERT_EQ(merged.size(), 2u);
    EXPECT_NEAR(merged[0].y0, 10.0f, 0.1f); // Row 1 first
    EXPECT_NEAR(merged[1].x0, 10.0f, 0.1f); // Merged cross-tile Row 2 second
    EXPECT_NEAR(merged[1].x1, 300.0f, 0.1f);
}

TEST(OcrTests, PpOcrCtcDecodeSmartSpacingAndBothLayouts) {
    using namespace QuickView::OcrNcnn;

    // Dict indices: 1="你", 2="好", 3="Q", 4="u", 5="i", 6="c", 7="k", 8="!" -> numClasses = 10 (0=blank, 9=space)
    const std::vector<std::string> dict = { "\xE4\xBD\xA0", "\xE5\xA5\xBD", "Q", "u", "i", "c", "k", "!" };
    const int numClasses = static_cast<int>(dict.size()) + 2;
    const int steps = 11;
    // Sequence per step (with CTC blanks/repeats): 你 你 blank 好 Q u blank i c k !
    const int seq[steps] = { 1, 1, 0, 2, 3, 4, 0, 5, 6, 7, 8 };

    // 1. TimeMajor [T=steps, C=numClasses] -> dimW=numClasses, dimH=steps
    std::vector<float> logitsTC(static_cast<size_t>(steps) * numClasses, -5.0f);
    for (int t = 0; t < steps; ++t) {
        logitsTC[t * numClasses + seq[t]] = 6.0f;
    }
    const DecodedLine resTC = DecodeCtcLine(logitsTC.data(), numClasses, steps, dict, 0.1f, 0.9f);
    EXPECT_EQ(resTC.utf8, "\xE4\xBD\xA0\xE5\xA5\xBD Quick!");
    ASSERT_EQ(resTC.tokens.size(), 4u); // "你", "好", "Quick", "!"
    EXPECT_EQ(resTC.tokens[2].utf8, "Quick");
    EXPECT_GT(resTC.tokens[2].confidence, 0.95f);
    EXPECT_FLOAT_EQ(resTC.tokens[0].x0Norm, 0.1f);
    EXPECT_FLOAT_EQ(resTC.tokens[3].x1Norm, 0.9f);
    for (size_t i = 0; i + 1 < resTC.tokens.size(); ++i) {
        EXPECT_FLOAT_EQ(resTC.tokens[i].x1Norm, resTC.tokens[i + 1].x0Norm);
        EXPECT_LT(resTC.tokens[i].x0Norm, resTC.tokens[i].x1Norm);
    }

    // 2. ClassMajor [C=numClasses, T=steps] -> dimW=steps, dimH=numClasses
    std::vector<float> logitsCT(static_cast<size_t>(numClasses) * steps, -5.0f);
    for (int t = 0; t < steps; ++t) {
        logitsCT[seq[t] * steps + t] = 6.0f;
    }
    const DecodedLine resCT = DecodeCtcLine(logitsCT.data(), steps, numClasses, dict, 0.1f, 0.9f);
    EXPECT_EQ(resCT.utf8, "\xE4\xBD\xA0\xE5\xA5\xBD Quick!");
}

TEST(OcrTests, PpOcrNcnnPluginDiscoveryParamsAndMissingAssetFallback) {
    const std::wstring pluginQvxSrc = GetExeDirectory() + L"\\plugins\\ocr\\ocr_ppocr_ncnn\\ocr_ppocr_ncnn.qvx";
    const std::wstring pluginManSrc = GetExeDirectory() + L"\\plugins\\ocr\\ocr_ppocr_ncnn\\manifest.json";
    if (GetFileAttributesW(pluginQvxSrc.c_str()) == INVALID_FILE_ATTRIBUTES) {
        GTEST_SKIP() << "ocr_ppocr_ncnn.qvx not found next to QuickViewTests.exe";
    }

    wchar_t tempPath[MAX_PATH]{};
    ASSERT_GT(GetTempPathW(MAX_PATH, tempPath), 0u);
    const std::wstring tempRoot = std::wstring(tempPath) + L"QVX_PpOcrNcnnTest_" + std::to_wstring(GetCurrentProcessId());
    const std::wstring pluginDir = tempRoot + L"\\ocr_ppocr_ncnn";
    const std::wstring modelsDir = pluginDir + L"\\models";
    const std::wstring subPackDir = modelsDir + L"\\custom_pack";
    ASSERT_TRUE(SystemInfo::EnsureDirectoryRecursive(subPackDir));

    const std::wstring stagedQvx = pluginDir + L"\\ocr_ppocr_ncnn.qvx";
    const std::wstring stagedMan = pluginDir + L"\\manifest.json";
    ASSERT_TRUE(CopyFileW(pluginQvxSrc.c_str(), stagedQvx.c_str(), FALSE));
    ASSERT_TRUE(CopyFileW(pluginManSrc.c_str(), stagedMan.c_str(), FALSE));

    auto& host = QuickView::PluginHost::Instance();
    host.ResetToDefaults();
    host.RefreshRegistry(tempRoot);

    const std::string pid = "com.quickview.ocr.ppocr_ncnn";
    const auto presetsInitial = host.GetPluginPresets(pid);
    ASSERT_EQ(presetsInitial.size(), 4u);
    EXPECT_EQ(presetsInitial[0].presetId, "ppocrv5_cjk_en");
    EXPECT_FALSE(presetsInitial[0].assetDownloadUrl.empty());

    std::string missingPresetId;
    for (const auto& p : presetsInitial) {
        if (!p.isInstalled) {
            missingPresetId = p.presetId;
            break;
        }
    }

    const auto params = host.GetPluginParams(pid);
    ASSERT_EQ(params.size(), 6u);
    EXPECT_EQ(params[0].id, "use_gpu");
    EXPECT_EQ(params[1].id, "num_threads");
    EXPECT_EQ(params[2].id, "det_limit_side");
    EXPECT_EQ(params[3].id, "box_thresh");
    EXPECT_EQ(params[4].id, "unclip_ratio");
    EXPECT_EQ(params[5].id, "detect_qr");

    // Drop custom flat triplet + custom subdir pack into models/ and verify dynamic discovery
    WriteUtf8File(modelsDir + L"\\custom_de.param", "7767517\n");
    WriteUtf8File(modelsDir + L"\\custom_de.bin", "BIN");
    WriteUtf8File(modelsDir + L"\\custom_de.dict.txt", "a\nb\n");
    WriteUtf8File(subPackDir + L"\\rec.param", "7767517\n");
    WriteUtf8File(subPackDir + L"\\rec.bin", "BIN");
    WriteUtf8File(subPackDir + L"\\dict.txt", "c\nd\n");
    WriteUtf8File(subPackDir + L"\\model.json", R"({ "display_name": "Deutsch Test Pack", "rec_height": 48 })");

    host.InvalidateModelCache();
    const auto presetsWithCustom = host.GetPluginPresets(pid);
    bool foundFlat = false, foundSub = false;
    for (const auto& p : presetsWithCustom) {
        if (p.presetId == "custom:custom_de" && p.isInstalled) foundFlat = true;
        if (p.presetId == "pack:custom_pack" && p.isInstalled && p.displayName == "Deutsch Test Pack") foundSub = true;
    }
    EXPECT_TRUE(foundFlat);
    EXPECT_TRUE(foundSub);

    // Un-downloaded preset must cleanly return QVX_E_MISSING_ASSET (independent of models installed in host AppData)
    if (!missingPresetId.empty()) {
        host.SetPluginEnabled(pid, true);
        host.SetPluginActivePreset(pid, missingPresetId);
        std::vector<uint8_t> dummyPixels(64 * 64 * 4, 255);
        QVX_AnalyzerInput rawInput{};
        rawInput.struct_size = sizeof(QVX_AnalyzerInput);
        rawInput.pixels = dummyPixels.data();
        rawInput.width = 64;
        rawInput.height = 64;
        rawInput.stride = 64 * 4;
        rawInput.format = QVX_PIXFMT_BGRA8888;
        QuickView::Ocr::OcrDocument doc;
        EXPECT_EQ(host.ExecuteOcrAnalyze(pid, rawInput, doc), QVX_E_MISSING_ASSET);
    }

    // Clean up without calling host.Shutdown()
    host.SetPluginEnabled(pid, false);
    host.ReleaseAnalyzerContextNow();
    host.ResetToDefaults();
    host.RefreshRegistry();
    DeleteFileW((subPackDir + L"\\rec.param").c_str());
    DeleteFileW((subPackDir + L"\\rec.bin").c_str());
    DeleteFileW((subPackDir + L"\\dict.txt").c_str());
    DeleteFileW((subPackDir + L"\\model.json").c_str());
    RemoveDirectoryW(subPackDir.c_str());
    DeleteFileW((modelsDir + L"\\custom_de.param").c_str());
    DeleteFileW((modelsDir + L"\\custom_de.bin").c_str());
    DeleteFileW((modelsDir + L"\\custom_de.dict.txt").c_str());
    RemoveDirectoryW(modelsDir.c_str());
    DeleteFileW(stagedMan.c_str());
    DeleteFileW(stagedQvx.c_str());
    RemoveDirectoryW(pluginDir.c_str());
    RemoveDirectoryW(tempRoot.c_str());
}

TEST(OcrTests, PpOcrNcnnRealModelInferenceCjkAndLatin) {
    const std::wstring exeDir = GetExeDirectory();
    const std::wstring pluginQvxSrc = exeDir + L"\\plugins\\ocr\\ocr_ppocr_ncnn\\ocr_ppocr_ncnn.qvx";
    const std::wstring pluginManSrc = exeDir + L"\\plugins\\ocr\\ocr_ppocr_ncnn\\manifest.json";
    if (GetFileAttributesW(pluginQvxSrc.c_str()) == INVALID_FILE_ATTRIBUTES) {
        GTEST_SKIP() << "ocr_ppocr_ncnn.qvx not built";
    }

    // Resolve repo models directory relative to executable or workspace source
    const std::wstring repoRoot = FindRepoRoot(exeDir);
    if (repoRoot.empty()) {
        GTEST_SKIP() << "Could not locate workspace repo root";
    }
    const std::wstring srcModelsDir = repoRoot + L"\\plugins\\ocr\\ocr_ppocr_ncnn\\models";
    if (GetFileAttributesW((srcModelsDir + L"\\ppocrv5_mobile_det.bin").c_str()) == INVALID_FILE_ATTRIBUTES) {
        GTEST_SKIP() << "Real PP-OCRv5 models not present in repo tree";
    }

    wchar_t tempPath[MAX_PATH]{};
    ASSERT_GT(GetTempPathW(MAX_PATH, tempPath), 0u);
    const std::wstring tempRoot = std::wstring(tempPath) + L"QVX_PpOcrRealModel_" + std::to_wstring(GetCurrentProcessId());
    const std::wstring pluginDir = tempRoot + L"\\ocr_ppocr_ncnn";
    const std::wstring modelsDir = pluginDir + L"\\models";
    ASSERT_TRUE(SystemInfo::EnsureDirectoryRecursive(modelsDir));

    ASSERT_TRUE(CopyFileW(pluginQvxSrc.c_str(), (pluginDir + L"\\ocr_ppocr_ncnn.qvx").c_str(), FALSE));
    ASSERT_TRUE(CopyFileW(pluginManSrc.c_str(), (pluginDir + L"\\manifest.json").c_str(), FALSE));

    const wchar_t* modelFiles[] = {
        L"ppocrv5_mobile_det.param",
        L"ppocrv5_mobile_det.bin",
        L"ppocrv5_mobile_cjk_rec.param",
        L"ppocrv5_mobile_cjk_rec.bin",
        L"ppocrv5_cjk.dict.txt",
        L"ppocrv5_mobile_latin_rec.param",
        L"ppocrv5_mobile_latin_rec.bin",
        L"ppocrv5_latin.dict.txt"
    };
    for (const wchar_t* mf : modelFiles) {
        ASSERT_TRUE(CopyFileW((srcModelsDir + L"\\" + mf).c_str(), (modelsDir + L"\\" + mf).c_str(), FALSE))
            << "Missing real model file: " << mf;
    }

    auto& host = QuickView::PluginHost::Instance();
    host.ResetToDefaults();
    host.RefreshRegistry(tempRoot);

    const std::string pid = "com.quickview.ocr.ppocr_ncnn";
    host.SetPluginEnabled(pid, true);
    const auto presets = host.GetPluginPresets(pid);
    ASSERT_GE(presets.size(), 2u);
    EXPECT_TRUE(presets[0].isInstalled); // ppocrv5_cjk_en
    EXPECT_TRUE(presets[1].isInstalled); // ppocrv5_latin

    // 1. Test real CJK + English inference (CPU fallback or Vulkan GPU depending on host adapter)
    {
        host.SetPluginActivePreset(pid, "ppocrv5_cjk_en");
        constexpr uint32_t W = 760, H = 180;
        const std::vector<uint8_t> img = RenderTextCanvasBgra(W, H, L"\x4F60\x597D\x4E16\x754C QuickView", 48, L"Microsoft YaHei UI");
        QVX_AnalyzerInput rawInput{};
        rawInput.struct_size = sizeof(QVX_AnalyzerInput);
        rawInput.pixels = img.data();
        rawInput.width = W;
        rawInput.height = H;
        rawInput.stride = W * 4;
        rawInput.format = QVX_PIXFMT_BGRA8888;
        QuickView::Ocr::OcrDocument doc;
        ASSERT_EQ(host.ExecuteOcrAnalyze(pid, rawInput, doc), QVX_OK);
        EXPECT_GE(doc.lines.size(), 1u);
        std::wstring allText;
        for (const auto& l : doc.lines) {
            if (!allText.empty()) allText.push_back(L' ');
            allText.append(l.text);
        }
        EXPECT_TRUE(allText.find(L"QuickView") != std::wstring::npos || allText.find(L"\x4F60\x597D") != std::wstring::npos)
            << "Actual CJK+EN OCR output size=" << allText.size();
    }

    // 2. Test real Latin inference
    {
        host.SetPluginActivePreset(pid, "ppocrv5_latin");
        constexpr uint32_t W = 680, H = 180;
        const std::vector<uint8_t> img = RenderTextCanvasBgra(W, H, L"HELLO QUICKVIEW 2026", 48);
        QVX_AnalyzerInput rawInput{};
        rawInput.struct_size = sizeof(QVX_AnalyzerInput);
        rawInput.pixels = img.data();
        rawInput.width = W;
        rawInput.height = H;
        rawInput.stride = W * 4;
        rawInput.format = QVX_PIXFMT_BGRA8888;
        QuickView::Ocr::OcrDocument doc;
        ASSERT_EQ(host.ExecuteOcrAnalyze(pid, rawInput, doc), QVX_OK);
        EXPECT_GE(doc.lines.size(), 1u);
        std::wstring allText;
        for (const auto& l : doc.lines) {
            if (!allText.empty()) allText.push_back(L' ');
            allText.append(l.text);
        }
        EXPECT_TRUE(allText.find(L"HELLO") != std::wstring::npos && allText.find(L"2026") != std::wstring::npos)
            << "Actual Latin OCR output size=" << allText.size();
    }

    host.SetPluginEnabled(pid, false);
    host.ReleaseAnalyzerContextNow();
    host.ResetToDefaults();
    host.RefreshRegistry();

    std::error_code ec;
    std::filesystem::remove_all(tempRoot, ec);
}

TEST(OcrTests, AllPluginsEightLanguagesAndSingleSourceVersion) {
    const std::wstring exeDir = GetExeDirectory();
    const std::wstring repoRoot = FindRepoRoot(exeDir);
    if (repoRoot.empty()) {
        GTEST_SKIP() << "Could not locate workspace repo root";
    }
    const std::wstring repoPluginsManifest = repoRoot + L"\\plugins\\plugins_manifest.json";

    auto readFileStr = [](const std::wstring& path) -> std::string {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return {};
        LARGE_INTEGER sz{};
        if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0) {
            CloseHandle(h);
            return {};
        }
        std::string buf(static_cast<size_t>(sz.QuadPart), '\0');
        DWORD readN = 0;
        ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &readN, nullptr);
        CloseHandle(h);
        return buf;
    };

    const std::string remoteJson = readFileStr(repoPluginsManifest);
    ASSERT_FALSE(remoteJson.empty());

    struct PluginCheckTarget {
        const char* id;
        const wchar_t* manifestRel;
        const wchar_t* qvxRel;
    };
    const PluginCheckTarget targets[] = {
        { "com.quickview.ocr.ppocr_ncnn", L"plugins\\ocr\\ocr_ppocr_ncnn\\manifest.json", L"plugins\\ocr\\ocr_ppocr_ncnn\\ocr_ppocr_ncnn.qvx" },
        { "com.quickview.sr.ncnn_vulkan", L"plugins\\sr\\sr_ncnn_vulkan\\manifest.json",  L"plugins\\sr\\sr_ncnn_vulkan\\sr_ncnn_vulkan.qvx" },
        { "com.quickview.shell.thumb",    L"plugins\\shell\\qv_shell_thumb\\manifest.json", L"plugins\\shell\\qv_shell_thumb\\qv_shell_thumb.qvx" }
    };
    const char* kLangs[8] = { "en-US", "zh-CN", "zh-TW", "ja-JP", "ru-RU", "de-DE", "es-ES", "fr-FR" };

    for (const auto& t : targets) {
        const std::wstring qvxPath = exeDir + L"\\" + t.qvxRel;
        const std::wstring manPath = repoRoot + L"\\" + t.manifestRel;
        if (GetFileAttributesW(qvxPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            GTEST_SKIP() << "Plugin binary not found: " << t.id;
        }

        const std::string manJson = readFileStr(manPath);
        ASSERT_FALSE(manJson.empty()) << t.id;
        for (const char* lang : kLangs) {
            EXPECT_NE(manJson.find(std::string("\"") + lang + "\""), std::string::npos)
                << "manifest.json missing i18n locale " << lang << " for " << t.id;
        }

        // Load binary directly to verify QVX_PluginHeader single-source version & 8-language QVX parameter localization
        HMODULE hMod = LoadLibraryW(qvxPath.c_str());
        ASSERT_NE(hMod, nullptr) << t.id;
        using InitFn = bool(*)(const QVX_HostServices*, const QVX_PluginHeader**);
        using ShutFn = void(*)(void);
        auto initFn = reinterpret_cast<InitFn>(GetProcAddress(hMod, "qvx_init"));
        auto shutFn = reinterpret_cast<ShutFn>(GetProcAddress(hMod, "qvx_shutdown"));
        ASSERT_NE(initFn, nullptr);

        QVX_HostServices hostSvc{};
        hostSvc.struct_size = sizeof(QVX_HostServices);
        hostSvc.abi_version = QVX_ABI_VERSION;
        const QVX_PluginHeader* hdr = nullptr;
        ASSERT_TRUE(initFn(&hostSvc, &hdr));
        ASSERT_NE(hdr, nullptr);
        ASSERT_NE(hdr->version_str, nullptr);
        ASSERT_NE(hdr->set_language, nullptr);
        ASSERT_NE(hdr->get_param_count, nullptr);
        ASSERT_NE(hdr->get_param_desc, nullptr);

        // Verify manifest.json version matches compiled header version
        const std::string verToken = std::string("\"version\": \"") + hdr->version_str + "\"";
        EXPECT_NE(manJson.find(verToken), std::string::npos)
            << "Compiled version " << hdr->version_str << " disagrees with manifest.json for " << t.id;
        // Verify plugins_manifest.json also contains matching plugin id & version
        const size_t idPos = remoteJson.find(t.id);
        ASSERT_NE(idPos, std::string::npos);
        const size_t nextVerPos = remoteJson.find(verToken, idPos);
        EXPECT_NE(nextVerPos, std::string::npos)
            << "plugins_manifest.json disagrees with compiled version " << hdr->version_str << " for " << t.id;

        std::vector<std::string> distinctFirstLabels;
        for (const char* lang : kLangs) {
            EXPECT_EQ(hdr->set_language(lang), QVX_OK);
            ASSERT_GT(hdr->get_param_count(), 0u);
            const QVX_ParamDesc* p0 = hdr->get_param_desc(0);
            ASSERT_NE(p0, nullptr);
            ASSERT_NE(p0->label, nullptr);
            EXPECT_GT(std::strlen(p0->label), 0u);
            if (std::find(distinctFirstLabels.begin(), distinctFirstLabels.end(), p0->label) == distinctFirstLabels.end()) {
                distinctFirstLabels.emplace_back(p0->label);
            }
        }
        EXPECT_GE(distinctFirstLabels.size(), 6u)
            << "Expected rich 8-language param localization for " << t.id;

        if (shutFn) shutFn();
        FreeLibrary(hMod);
    }
}

namespace {

void DrawQr25x25IntoBgra(
    std::vector<uint8_t>& bgra,
    uint32_t imgW,
    uint32_t imgH,
    uint32_t originX,
    uint32_t originY,
    uint32_t modulePx,
    const uint32_t (&rows)[25],
    bool inverted = false)
{
    const uint8_t dark = inverted ? 255u : 0u;
    const uint8_t light = inverted ? 0u : 255u;
    if (inverted) {
        const uint32_t totalPx = 29u * modulePx;
        for (uint32_t dy = 0; dy < totalPx && (originY + dy) < imgH; ++dy) {
            for (uint32_t dx = 0; dx < totalPx && (originX + dx) < imgW; ++dx) {
                const size_t off = (static_cast<size_t>(originY + dy) * imgW + (originX + dx)) * 4u;
                bgra[off + 0] = light;
                bgra[off + 1] = light;
                bgra[off + 2] = light;
                bgra[off + 3] = 255u;
            }
        }
    }
    const uint32_t quiet = 2u * modulePx;
    for (uint32_t r = 0; r < 25u; ++r) {
        for (uint32_t c = 0; c < 25u; ++c) {
            if (((rows[r] >> c) & 1u) == 0u) continue;
            const uint32_t px0 = originX + quiet + c * modulePx;
            const uint32_t py0 = originY + quiet + r * modulePx;
            for (uint32_t dy = 0; dy < modulePx && (py0 + dy) < imgH; ++dy) {
                for (uint32_t dx = 0; dx < modulePx && (px0 + dx) < imgW; ++dx) {
                    const size_t off = (static_cast<size_t>(py0 + dy) * imgW + (px0 + dx)) * 4u;
                    bgra[off + 0] = dark;
                    bgra[off + 1] = dark;
                    bgra[off + 2] = dark;
                    bgra[off + 3] = 255u;
                }
            }
        }
    }
}

constexpr uint32_t kQrUrlRows[25] = {
    0x1FCF27Fu, 0x105DC41u, 0x175975Du, 0x174CE5Du, 0x174E45Du,
    0x104E241u, 0x1FD557Fu, 0x0017700u, 0x047CDF7u, 0x1040C2Eu,
    0x1D82251u, 0x083888Du, 0x1A7114Du, 0x1273B28u, 0x1CD5BE5u,
    0x091F722u, 0x03FCAEDu, 0x1B1CB00u, 0x1B5A37Fu, 0x1310D41u,
    0x13F115Du, 0x078BA5Du, 0x1115D5Du, 0x0B57141u, 0x18FCF7Fu
};

constexpr uint32_t kQrTextRows[25] = {
    0x1FD727Fu, 0x104DC41u, 0x174775Du, 0x175EE5Du, 0x174C45Du,
    0x105A241u, 0x1FD557Fu, 0x0007700u, 0x0464DF7u, 0x1EF4DBEu,
    0x1EA2273u, 0x0018889u, 0x18F11D2u, 0x1213BB8u, 0x1BD5E7Du,
    0x001F692u, 0x11F4DD1u, 0x0714900u, 0x1B5257Fu, 0x0110941u,
    0x17F115Du, 0x0183A5Du, 0x11EDF5Du, 0x083F341u, 0x181497Fu
};

struct LoadedQrAnalyzer {
    HMODULE hMod = nullptr;
    void (*shutFn)(void) = nullptr;
    const QVX_Analyzer_VTable* vt = nullptr;
    QVX_AnalyzerContext ctx = nullptr;

    ~LoadedQrAnalyzer() {
        if (vt && ctx) vt->destroy_context(ctx);
        if (shutFn) shutFn();
        if (hMod) FreeLibrary(hMod);
    }
};

bool LoadOcrPluginForQrTests(LoadedQrAnalyzer& out) {
    const std::wstring qvxPath = GetExeDirectory() + L"\\plugins\\ocr\\ocr_ppocr_ncnn\\ocr_ppocr_ncnn.qvx";
    if (GetFileAttributesW(qvxPath.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    out.hMod = LoadLibraryW(qvxPath.c_str());
    if (!out.hMod) return false;

    using InitFn = bool(*)(const QVX_HostServices*, const QVX_PluginHeader**);
    using ShutFn = void(*)(void);
    auto initFn = reinterpret_cast<InitFn>(GetProcAddress(out.hMod, "qvx_init"));
    out.shutFn = reinterpret_cast<ShutFn>(GetProcAddress(out.hMod, "qvx_shutdown"));
    if (!initFn) return false;

    QVX_HostServices hostSvc{};
    hostSvc.struct_size = sizeof(QVX_HostServices);
    hostSvc.abi_version = QVX_ABI_VERSION;
    const QVX_PluginHeader* hdr = nullptr;
    if (!initFn(&hostSvc, &hdr) || !hdr || !hdr->get_interface) return false;

    out.vt = reinterpret_cast<const QVX_Analyzer_VTable*>(
        hdr->get_interface(QVX_IFACE_ANALYZER, QVX_ANALYZER_INTERFACE_VERSION));
    if (!out.vt || !out.vt->create_context || !out.vt->analyze || !out.vt->free_result) return false;

    out.ctx = out.vt->create_context(nullptr);
    return out.ctx != nullptr;
}

} // namespace

TEST(QrDetectorTest, DetectsMultipleQrCodesAndDistinguishesUrl) {
    LoadedQrAnalyzer plugin;
    if (!LoadOcrPluginForQrTests(plugin)) {
        GTEST_SKIP() << "ocr_ppocr_ncnn.qvx not available";
    }
    EXPECT_TRUE((plugin.vt->get_caps() & QVX_ANALYZER_CAP_BARCODE) != 0u);

    constexpr uint32_t kW = 640u;
    constexpr uint32_t kH = 360u;
    std::vector<uint8_t> bgra(static_cast<size_t>(kW) * kH * 4u, 255u);
    DrawQr25x25IntoBgra(bgra, kW, kH, 40u, 80u, 6u, kQrUrlRows, false);
    DrawQr25x25IntoBgra(bgra, kW, kH, 380u, 80u, 6u, kQrTextRows, false);

    QVX_AnalyzerInput input{};
    input.struct_size = sizeof(QVX_AnalyzerInput);
    input.pixels = bgra.data();
    input.width = kW;
    input.height = kH;
    input.stride = kW * 4u;
    input.format = QVX_PIXFMT_BGRA8888;

    QVX_AnalyzerResult res{};
    res.struct_size = sizeof(QVX_AnalyzerResult);
    ASSERT_EQ(plugin.vt->analyze(plugin.ctx, &input, &res), QVX_OK);

    bool foundUrl = false;
    bool foundText = false;
    for (uint32_t i = 0; i < res.line_count; ++i) {
        const auto& l = res.lines[i];
        if ((l.flags & QVX_ANALYZER_FLAG_BARCODE) == 0u) continue;
        const std::string text(res.utf8_pool + l.utf8_offset, l.utf8_len);
        if (text == "https://example.com") {
            foundUrl = true;
            EXPECT_TRUE((l.flags & QVX_ANALYZER_FLAG_URL) != 0u);
            EXPECT_LT((l.quad.min_x + l.quad.max_x) * 0.5f, 0.5f);
        } else if (text == "QuickView-QR-Test") {
            foundText = true;
            EXPECT_TRUE((l.flags & QVX_ANALYZER_FLAG_URL) == 0u);
            EXPECT_GT((l.quad.min_x + l.quad.max_x) * 0.5f, 0.5f);
        }
    }
    plugin.vt->free_result(plugin.ctx, &res);
    EXPECT_TRUE(foundUrl);
    EXPECT_TRUE(foundText);
}

TEST(QrDetectorTest, DetectsInvertedPolarityQrCode) {
    LoadedQrAnalyzer plugin;
    if (!LoadOcrPluginForQrTests(plugin)) {
        GTEST_SKIP() << "ocr_ppocr_ncnn.qvx not available";
    }

    constexpr uint32_t kW = 320u;
    constexpr uint32_t kH = 320u;
    std::vector<uint8_t> bgra(static_cast<size_t>(kW) * kH * 4u, 255u);
    DrawQr25x25IntoBgra(bgra, kW, kH, 60u, 60u, 6u, kQrUrlRows, true);

    QVX_AnalyzerInput input{};
    input.struct_size = sizeof(QVX_AnalyzerInput);
    input.pixels = bgra.data();
    input.width = kW;
    input.height = kH;
    input.stride = kW * 4u;
    input.format = QVX_PIXFMT_BGRA8888;

    QVX_AnalyzerResult res{};
    res.struct_size = sizeof(QVX_AnalyzerResult);
    ASSERT_EQ(plugin.vt->analyze(plugin.ctx, &input, &res), QVX_OK);

    bool foundUrl = false;
    for (uint32_t i = 0; i < res.line_count; ++i) {
        const auto& l = res.lines[i];
        if ((l.flags & QVX_ANALYZER_FLAG_BARCODE) == 0u) continue;
        const std::string text(res.utf8_pool + l.utf8_offset, l.utf8_len);
        if (text == "https://example.com") {
            foundUrl = true;
            EXPECT_TRUE((l.flags & QVX_ANALYZER_FLAG_URL) != 0u);
        }
    }
    plugin.vt->free_result(plugin.ctx, &res);
    EXPECT_TRUE(foundUrl);
}

TEST(OcrOverlayQrTest, SafeHttpUrlWhitelist) {
    using QuickView::UI::OcrOverlay;
    EXPECT_TRUE(OcrOverlay::IsSafeHttpUrl(L"https://example.com/path?a=1"));
    EXPECT_TRUE(OcrOverlay::IsSafeHttpUrl(L"  HTTP://example.org  "));
    EXPECT_FALSE(OcrOverlay::IsSafeHttpUrl(L"https://"));
    EXPECT_FALSE(OcrOverlay::IsSafeHttpUrl(L"http://"));
    EXPECT_FALSE(OcrOverlay::IsSafeHttpUrl(L"file:///C:/Windows/System32/calc.exe"));
    EXPECT_FALSE(OcrOverlay::IsSafeHttpUrl(L"javascript:alert(1)"));
    EXPECT_FALSE(OcrOverlay::IsSafeHttpUrl(L"cmd.exe"));
    EXPECT_FALSE(OcrOverlay::IsSafeHttpUrl(L"https://example.com\r\n"));
}

TEST(OcrOverlayQrTest, IsolatesBarcodeFromTextSelectionAndHitsBadge) {
    auto& overlay = QuickView::UI::OcrOverlay::Instance();
    overlay.ClearDocumentKeepHidden();
    overlay.SetUIScale(1.0f);

    QuickView::Ocr::OcrDocument doc;
    doc.imageId = 42;
    doc.generationId = 1;
    doc.sourceVisualW = 1000;
    doc.sourceVisualH = 800;

    QuickView::Ocr::OcrLine textLine;
    textLine.quad = { {0.1f, 0.2f, 0.2f, 0.1f}, {0.1f, 0.1f, 0.15f, 0.15f}, 0.1f, 0.1f, 0.2f, 0.15f };
    textLine.text = L"Hello";
    doc.lines.push_back(textLine);

    QuickView::Ocr::OcrLine qrLine;
    qrLine.quad = { {0.4f, 0.6f, 0.6f, 0.4f}, {0.3f, 0.3f, 0.55f, 0.55f}, 0.4f, 0.3f, 0.6f, 0.55f };
    qrLine.text = L"https://example.com";
    qrLine.flags = QVX_ANALYZER_FLAG_BARCODE | QVX_ANALYZER_FLAG_URL;
    doc.lines.push_back(qrLine);

    overlay.SetDocument(std::move(doc), 1000.0f, 800.0f);
    ASSERT_TRUE(overlay.IsActive());

    overlay.SelectAll();
    EXPECT_EQ(overlay.GetSelectedText(), L"Hello");
    EXPECT_EQ(overlay.GetAllText(), L"Hello");

    QuickView::UI::OcrViewportMetrics vp{};
    vp.imgDrawX = 0.0f;
    vp.imgDrawY = 0.0f;
    vp.totalScale = 1.0f;
    vp.visualW = 1000.0f;
    vp.visualH = 800.0f;
    vp.winW = 1000.0f;
    vp.winH = 800.0f;
    vp.valid = true;

    // Circular arrow badge is centered inside the QR rect at (500, 340)
    EXPECT_EQ(overlay.HitTest(500.0f, 340.0f, vp), QuickView::UI::OcrHitZone::Barcode);
    EXPECT_EQ(overlay.HitTestBarcodeScreenSpace(500.0f, 340.0f, vp), 1);
    EXPECT_EQ(overlay.HitTest(420.0f, 260.0f, vp), QuickView::UI::OcrHitZone::Barcode);
    EXPECT_EQ(overlay.HitTest(150.0f, 100.0f, vp), QuickView::UI::OcrHitZone::Text);

    overlay.OnMouseMove(500.0f, 340.0f, vp, false);
    EXPECT_EQ(overlay.GetHoverBarcodeIndex(), 1);

    overlay.ClearDocumentKeepHidden();
}

TEST(QrDetectorTest, DetectsCode128LinearBarcode) {
    LoadedQrAnalyzer plugin;
    if (!LoadOcrPluginForQrTests(plugin)) {
        GTEST_SKIP() << "ocr_ppocr_ncnn.qvx not available";
    }

    // Code 128B encoding of "QV-2026":
    // StartB(104), 'Q'(49), 'V'(54), '-'(13), '2'(18), '0'(16), '2'(18), '6'(22), Checksum(96), Stop(106)+2
    static constexpr int kSeq[][6] = {
        { 2, 1, 1, 2, 1, 4 }, // 104: Start B
        { 2, 1, 1, 3, 3, 1 }, // 49: 'Q'
        { 3, 1, 1, 1, 2, 3 }, // 54: 'V'
        { 1, 2, 2, 1, 3, 2 }, // 13: '-'
        { 2, 2, 3, 2, 1, 1 }, // 18: '2'
        { 1, 2, 3, 1, 2, 2 }, // 16: '0'
        { 2, 2, 3, 2, 1, 1 }, // 18: '2'
        { 2, 2, 3, 1, 1, 2 }, // 22: '6'
        { 1, 1, 4, 3, 1, 1 }, // 96: Checksum
        { 2, 3, 3, 1, 1, 1 }  // 106: Stop
    };

    constexpr uint32_t kModPx = 3u;
    constexpr uint32_t kQuiet = 36u;
    constexpr uint32_t kW = kQuiet * 2u + (11u * 10u + 2u) * kModPx;
    constexpr uint32_t kH = 120u;
    std::vector<uint8_t> bgra(static_cast<size_t>(kW) * kH * 4u, 255u);

    uint32_t curX = kQuiet;
    auto paintBar = [&](uint32_t widthPx) {
        for (uint32_t y = 20u; y < 100u; ++y) {
            for (uint32_t x = curX; x < curX + widthPx && x < kW; ++x) {
                const size_t off = (static_cast<size_t>(y) * kW + x) * 4u;
                bgra[off + 0] = 0u;
                bgra[off + 1] = 0u;
                bgra[off + 2] = 0u;
                bgra[off + 3] = 255u;
            }
        }
    };
    for (const auto& pat : kSeq) {
        for (int j = 0; j < 6; ++j) {
            const uint32_t runPx = static_cast<uint32_t>(pat[j]) * kModPx;
            if ((j & 1) == 0) paintBar(runPx);
            curX += runPx;
        }
    }
    paintBar(2u * kModPx); // Stop termination bar

    QVX_AnalyzerInput input{};
    input.struct_size = sizeof(QVX_AnalyzerInput);
    input.pixels = bgra.data();
    input.width = kW;
    input.height = kH;
    input.stride = kW * 4u;
    input.format = QVX_PIXFMT_BGRA8888;

    QVX_AnalyzerResult res{};
    res.struct_size = sizeof(QVX_AnalyzerResult);
    ASSERT_EQ(plugin.vt->analyze(plugin.ctx, &input, &res), QVX_OK);

    bool foundCode128 = false;
    for (uint32_t i = 0; i < res.line_count; ++i) {
        const auto& l = res.lines[i];
        if ((l.flags & QVX_ANALYZER_FLAG_BARCODE) == 0u) continue;
        const std::string text(res.utf8_pool + l.utf8_offset, l.utf8_len);
        if (text == "QV-2026") {
            foundCode128 = true;
            EXPECT_TRUE((l.flags & QVX_ANALYZER_FLAG_URL) == 0u);
        }
    }
    plugin.vt->free_result(plugin.ctx, &res);
    EXPECT_TRUE(foundCode128);
}
