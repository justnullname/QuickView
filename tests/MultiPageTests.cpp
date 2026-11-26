/*
 * QuickView Multi-Page, ICO Decoder & Comic/Book Mode Unit Tests
 * Copyright (C) 2026-Present QuickView Contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "pch.h"
#include "gtest/gtest.h"
#include "IcoDecoder.h"
#include "FileNavigator.h"
#include "MiniTiff.h"
#include "ToolbarVisibility.h"
#include <wincodec.h>
#include <filesystem>
#include <cstdio>
#include <vector>
#include <cstring>

namespace {

void PushU16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void PushU32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

// Build a synthetic 32-bit DIB ICO frame (BITMAPINFOHEADER + XOR BGRA + AND mask)
std::vector<uint8_t> MakeBmpIcoPayload(uint16_t w, uint16_t h, uint8_t b, uint8_t g, uint8_t r) {
    const uint32_t xorSize = static_cast<uint32_t>(w) * h * 4;
    const uint32_t andStride = ((w + 31) / 32) * 4;
    const uint32_t andSize = andStride * h;

    std::vector<uint8_t> dib;
    dib.reserve(40 + xorSize + andSize);
    PushU32(dib, 40);                           // biSize
    PushU32(dib, w);                            // biWidth
    PushU32(dib, static_cast<uint32_t>(h) * 2); // biHeight (XOR + AND)
    PushU16(dib, 1);                            // biPlanes
    PushU16(dib, 32);                           // biBitCount
    PushU32(dib, 0);                            // BI_RGB
    PushU32(dib, xorSize);
    PushU32(dib, 0);
    PushU32(dib, 0);
    PushU32(dib, 0);
    PushU32(dib, 0);

    for (uint32_t i = 0; i < static_cast<uint32_t>(w) * h; ++i) {
        dib.push_back(b);
        dib.push_back(g);
        dib.push_back(r);
        dib.push_back(0xFF);
    }
    dib.resize(dib.size() + andSize, 0);
    return dib;
}

// Build a complete ICO file from multiple (width, height, bpp, payload) entries
std::vector<uint8_t> MakeSyntheticIco(const std::vector<std::pair<uint16_t, std::vector<uint8_t>>>& frames,
                                      const std::vector<uint16_t>& bpps) {
    const uint16_t count = static_cast<uint16_t>(frames.size());
    std::vector<uint8_t> ico;
    PushU16(ico, 0);     // reserved
    PushU16(ico, 1);     // type = ICON
    PushU16(ico, count); // image count

    uint32_t dataOffset = 6 + count * 16;
    for (uint16_t i = 0; i < count; ++i) {
        const uint16_t dim = frames[i].first;
        const uint8_t bDim = (dim >= 256) ? 0 : static_cast<uint8_t>(dim);
        ico.push_back(bDim);
        ico.push_back(bDim);
        ico.push_back(0); // color count
        ico.push_back(0); // reserved
        PushU16(ico, 1);  // planes
        PushU16(ico, bpps[i]);
        PushU32(ico, static_cast<uint32_t>(frames[i].second.size()));
        PushU32(ico, dataOffset);
        dataOffset += static_cast<uint32_t>(frames[i].second.size());
    }
    for (const auto& f : frames) {
        ico.insert(ico.end(), f.second.begin(), f.second.end());
    }
    return ico;
}

} // namespace

TEST(IcoDecoderTest, ParsesSortsAndDecodesEntries) {
    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // Input order: 16x16@32, 32x32@32, 48x48@32 -> sorted should be 48, 32, 16
    auto p16 = MakeBmpIcoPayload(16, 16, 0x11, 0x22, 0x33);
    auto p32 = MakeBmpIcoPayload(32, 32, 0x44, 0x55, 0x66);
    auto p48 = MakeBmpIcoPayload(48, 48, 0x77, 0x88, 0x99);
    auto ico = MakeSyntheticIco({{16, p16}, {32, p32}, {48, p48}}, {32, 32, 32});

    std::vector<QuickView::IcoDecoder::IcoEntryInfo> entries;
    ASSERT_TRUE(QuickView::IcoDecoder::ParseDirectory(ico.data(), ico.size(), entries));
    ASSERT_EQ(entries.size(), 3u);
    EXPECT_EQ(entries[0].width, 48);
    EXPECT_EQ(entries[1].width, 32);
    EXPECT_EQ(entries[2].width, 16);

    const std::wstring summary = QuickView::IcoDecoder::BuildSummary(entries);
    EXPECT_NE(summary.find(L"3 sizes"), std::wstring::npos);
    EXPECT_NE(summary.find(L"48"), std::wstring::npos);

    ComPtr<IWICImagingFactory> wicFactory;
    ASSERT_EQ(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wicFactory)), S_OK);

    QuickView::RawImageFrame frame48;
    ASSERT_EQ(QuickView::IcoDecoder::DecodeEntry(ico.data(), ico.size(), entries[0], wicFactory.Get(), frame48), S_OK);
    EXPECT_EQ(frame48.width, 48u);
    EXPECT_EQ(frame48.height, 48u);
    ASSERT_NE(frame48.pixels, nullptr);
    EXPECT_EQ(frame48.pixels[0], 0x77);
    EXPECT_EQ(frame48.pixels[1], 0x88);
    EXPECT_EQ(frame48.pixels[2], 0x99);

    QuickView::RawImageFrame sheet;
    ASSERT_EQ(QuickView::IcoDecoder::RenderContactSheet(ico.data(), ico.size(), entries, wicFactory.Get(), 1.0f, sheet), S_OK);
    EXPECT_GT(sheet.width, 48u);
    EXPECT_GT(sheet.height, 48u);
    ASSERT_NE(sheet.pixels, nullptr);

    if (SUCCEEDED(hrCom)) CoUninitialize();
}

TEST(MultiPageNavigatorTest, VirtualPathRoundTripAndBookModeTransitions) {
    const std::wstring host = L"C:\\Images\\sample.tif";
    const std::wstring vp = FileNavigator::MakeMultiPagePath(host, 2, 5);
    EXPECT_TRUE(FileNavigator::IsMultiPageVirtualPath(vp));
    EXPECT_EQ(FileNavigator::GetPhysicalPath(vp), host);

    const std::wstring icoVp = FileNavigator::MakeIcoFramePath(L"C:\\Icons\\app.ico", 1, 64, 64, 32);
    EXPECT_TRUE(FileNavigator::IsMultiPageVirtualPath(icoVp));
    EXPECT_EQ(FileNavigator::GetPhysicalPath(icoVp), L"C:\\Icons\\app.ico");

    FileNavigator nav;
    EXPECT_FALSE(nav.IsBookModeActive());
    ASSERT_TRUE(nav.EnterMultiPageBookMode(host, 5, 2));
    EXPECT_TRUE(nav.IsBookModeActive());
    EXPECT_EQ(nav.Count(), 5u);
    EXPECT_EQ(nav.Index(), 2);
    EXPECT_EQ(nav.GetFile(2), vp);

    std::wstring restoredTarget;
    ASSERT_TRUE(nav.ExitBookMode(restoredTarget));
    EXPECT_FALSE(nav.IsBookModeActive());
    EXPECT_EQ(restoredTarget, host);
}

TEST(MultiPageNavigatorTest, IcoBookModeWithCustomPagePathsAndRawImageFramePreservation) {
    const std::wstring host = L"C:\\Icons\\app.ico";
    std::vector<std::wstring> frames = {
        FileNavigator::MakeIcoFramePath(host, 0, 256, 256, 32),
        FileNavigator::MakeIcoFramePath(host, 1, 64, 64, 32),
        FileNavigator::MakeIcoFramePath(host, 2, 16, 16, 32),
    };
    FileNavigator nav;
    ASSERT_TRUE(nav.EnterBookModeWithPages(host, frames, 1));
    EXPECT_TRUE(nav.IsBookModeActive());
    EXPECT_EQ(nav.Count(), 3u);
    EXPECT_EQ(nav.Index(), 1);
    EXPECT_EQ(nav.GetFile(0), frames[0]);
    EXPECT_EQ(nav.GetFile(2), frames[2]);

    std::wstring restored;
    ASSERT_TRUE(nav.ExitBookMode(restored));
    EXPECT_FALSE(nav.IsBookModeActive());
    EXPECT_EQ(restored, host);

    // Verify RawImageFrame carries multi-page and ICO overview state across Clone/Promote (cache-hit safety)
    QuickView::RawImageFrame frame;
    frame.width = 2;
    frame.height = 2;
    frame.stride = 8;
    frame.pixels = static_cast<uint8_t*>(_aligned_malloc(16, 64));
    ASSERT_NE(frame.pixels, nullptr);
    std::memset(frame.pixels, 0x7F, 16);
    frame.memoryDeleter = QuickView::MemoryDeleter::FromAlignedFree();
    frame.pageCount = 10;
    frame.currentPage = 4;
    frame.isIcoOverview = true;
    frame.icoFrameSummary = L"10 sizes";

    auto cloned = frame.CloneToStandalone();
    ASSERT_NE(cloned, nullptr);
    EXPECT_EQ(cloned->pageCount, 10u);
    EXPECT_EQ(cloned->currentPage, 4u);
    EXPECT_TRUE(cloned->isIcoOverview);
    EXPECT_EQ(cloned->icoFrameSummary, L"10 sizes");

    auto promoted = frame.PromoteToStandalone();
    ASSERT_NE(promoted, nullptr);
    EXPECT_EQ(promoted->pageCount, 10u);
    EXPECT_EQ(promoted->currentPage, 4u);
    EXPECT_TRUE(promoted->isIcoOverview);
    EXPECT_EQ(promoted->icoFrameSummary, L"10 sizes");
    // Original frame must reset multi-page fields on move
    EXPECT_EQ(frame.pageCount, 1u);
    EXPECT_EQ(frame.currentPage, 0u);
    EXPECT_FALSE(frame.isIcoOverview);
    EXPECT_TRUE(frame.icoFrameSummary.empty());
}

TEST(MultiPageNavigatorTest, RealMultiPageTiffFixtureIfdCountIsTen) {
    const std::filesystem::path sourceDir = std::filesystem::path(__FILE__).parent_path();
    const std::filesystem::path candidates[] = {
        std::filesystem::current_path() / "Local-Files" / "test_img" / "tif" / "multipage_tif_example2.tif",
        sourceDir.parent_path() / "Local-Files" / "test_img" / "tif" / "multipage_tif_example2.tif",
    };
    std::filesystem::path assetPath;
    for (const auto& c : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(c, ec)) {
            assetPath = c;
            break;
        }
    }
    if (assetPath.empty()) {
        GTEST_SKIP() << "multipage_tif_example2.tif fixture not found";
    }
    FILE* f = nullptr;
    ASSERT_EQ(_wfopen_s(&f, assetPath.wstring().c_str(), L"rb"), 0);
    ASSERT_NE(f, nullptr);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    ASSERT_GT(sz, 0);
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    ASSERT_EQ(fread(buf.data(), 1, buf.size(), f), buf.size());
    fclose(f);

    // Verify QuickView::MiniTiff detects all 10 IFD pages on real JPEG-in-TIFF fixture
    EXPECT_EQ(QuickView::MiniTiff::GetPageCount(buf.data(), buf.size()), 10u);
}

TEST(ToolbarVisibilityTest, ComicAndBookModeStrictWhitelistHidesCropButtons) {
    // Regression check: Comic / Book mode must strictly hide Crop/Anim/Overlay/LockSize controls
    EXPECT_TRUE(IsToolbarButtonVisibleInMode(ToolbarButtonID::RotateL,     false, false, false, false, false, true, true));
    EXPECT_TRUE(IsToolbarButtonVisibleInMode(ToolbarButtonID::RotateR,     false, false, false, false, false, true, true));
    EXPECT_FALSE(IsToolbarButtonVisibleInMode(ToolbarButtonID::LockSize,   false, false, false, false, false, true, true));
    EXPECT_FALSE(IsToolbarButtonVisibleInMode(ToolbarButtonID::FlipH,      false, false, false, false, false, true, true));
    EXPECT_FALSE(IsToolbarButtonVisibleInMode(ToolbarButtonID::CropCopy,   false, false, false, false, false, true, true));
    EXPECT_FALSE(IsToolbarButtonVisibleInMode(ToolbarButtonID::CropApply,  false, false, false, false, false, true, true));
    EXPECT_FALSE(IsToolbarButtonVisibleInMode(ToolbarButtonID::CropSave,   false, false, false, false, false, true, true, true, false, false, true));
    EXPECT_FALSE(IsToolbarButtonVisibleInMode(ToolbarButtonID::CropCancel, false, false, false, false, false, true, true));
    EXPECT_FALSE(IsToolbarButtonVisibleInMode(ToolbarButtonID::AnimPlayPause, false, false, false, false, false, true, true));
    EXPECT_FALSE(IsToolbarButtonVisibleInMode(ToolbarButtonID::OverlayExit,   false, false, false, false, false, true, true));

    // Whitelisted navigation & reading controls remain visible
    EXPECT_TRUE(IsToolbarButtonVisibleInMode(ToolbarButtonID::Prev,          false, false, false, false, false, true, true));
    EXPECT_TRUE(IsToolbarButtonVisibleInMode(ToolbarButtonID::Next,          false, false, false, false, false, true, true));
    EXPECT_TRUE(IsToolbarButtonVisibleInMode(ToolbarButtonID::Gallery,       false, false, false, false, false, true, true));
    EXPECT_TRUE(IsToolbarButtonVisibleInMode(ToolbarButtonID::Exif,          false, false, false, false, false, true, true));
    EXPECT_TRUE(IsToolbarButtonVisibleInMode(ToolbarButtonID::CompareToggle, false, false, false, false, false, true, true));
    EXPECT_TRUE(IsToolbarButtonVisibleInMode(ToolbarButtonID::Pin,           false, false, false, false, false, true, true));
    EXPECT_TRUE(IsToolbarButtonVisibleInMode(ToolbarButtonID::BookMode,      false, false, false, false, false, true, true));
    EXPECT_FALSE(IsToolbarButtonVisibleInMode(ToolbarButtonID::BookMode,     false, false, false, false, false, true, false));
}

TEST(ToolbarVisibilityTest, DualPageReadingKeepsBookToolbarOverCompareToolbar) {
    // Regression check (#DualPage): When CompareMode + Comic/BookMode are both active,
    // Dual-Page reading must keep the reading toolbar instead of normal CompareMode buttons.
    EXPECT_TRUE(IsToolbarButtonVisibleInMode(ToolbarButtonID::Prev,          false, false, false, true, false, true, true));
    EXPECT_TRUE(IsToolbarButtonVisibleInMode(ToolbarButtonID::Next,          false, false, false, true, false, true, true));
    EXPECT_TRUE(IsToolbarButtonVisibleInMode(ToolbarButtonID::RotateL,       false, false, false, true, false, true, true));
    EXPECT_TRUE(IsToolbarButtonVisibleInMode(ToolbarButtonID::RotateR,       false, false, false, true, false, true, true));
    EXPECT_TRUE(IsToolbarButtonVisibleInMode(ToolbarButtonID::Gallery,       false, false, false, true, false, true, true));
    EXPECT_TRUE(IsToolbarButtonVisibleInMode(ToolbarButtonID::Exif,          false, false, false, true, false, true, true));
    EXPECT_TRUE(IsToolbarButtonVisibleInMode(ToolbarButtonID::CompareToggle, false, false, false, true, false, true, true));
    EXPECT_TRUE(IsToolbarButtonVisibleInMode(ToolbarButtonID::BookMode,      false, false, false, true, false, true, true));
    EXPECT_TRUE(IsToolbarButtonVisibleInMode(ToolbarButtonID::Pin,           false, false, false, true, false, true, true));

    EXPECT_FALSE(IsToolbarButtonVisibleInMode(ToolbarButtonID::CompareOpen,   false, false, false, true, false, true, true));
    EXPECT_FALSE(IsToolbarButtonVisibleInMode(ToolbarButtonID::CompareSwap,   false, false, false, true, false, true, true));
    EXPECT_FALSE(IsToolbarButtonVisibleInMode(ToolbarButtonID::CompareLayout, false, false, false, true, false, true, true));
    EXPECT_FALSE(IsToolbarButtonVisibleInMode(ToolbarButtonID::CompareExit,   false, false, false, true, false, true, true));
}
