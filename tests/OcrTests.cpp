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
#include "Plugin/PluginRegistry.h"
#include "Plugin/abi/qvx_analyzer.h"
#include "SystemInfo.h"

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

std::vector<uint8_t> RenderTextCanvasBgra(uint32_t w, uint32_t h, const wchar_t* text, int fontPx) {
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
            ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, FF_SWISS | DEFAULT_PITCH, L"Arial");
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
