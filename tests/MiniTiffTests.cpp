/*
 * QuickView Mini TIFF Decoder - Unit Tests
 * Copyright (C) 2026-Present QuickView Contributors
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "pch.h"
#include "gtest/gtest.h"
#include "MiniTiff.h"
#include <vector>
#include <string>
#include <cstdio>
#include <filesystem>
#include <cwctype>
#include <cmath>

static std::vector<uint8_t> ReadFileBytes(const std::wstring& path) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) {
        return {};
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        return {};
    }
    std::vector<uint8_t> buf(size);
    size_t readBytes = fread(buf.data(), 1, size, f);
    buf.resize(readBytes);
    fclose(f);
    return buf;
}

TEST(MiniTiffTest, LoadUncompressedRGB) {
    std::wstring path = L"../../../Local-Files/test_img/格式测试/tiff-rgb24.tiff";
    std::vector<uint8_t> bytes = ReadFileBytes(path);
    ASSERT_FALSE(bytes.empty()) << "Failed to read test image tiff-rgb24.tiff";

    QuickView::Codec::DecodeContext ctx;
    ctx.allocator.ctx = nullptr;
    ctx.allocator.pfn = [](void*, size_t s) -> uint8_t* {
        return static_cast<uint8_t*>(_aligned_malloc(s, 64));
    };

    QuickView::Codec::DecodeResult result;
    HRESULT hr = QuickView::MiniTiff::Load(bytes.data(), bytes.size(), ctx, result);

    EXPECT_EQ(hr, S_OK);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.width, 6000);
    EXPECT_EQ(result.height, 4000);
    EXPECT_NE(result.pixels, nullptr);
    EXPECT_EQ(result.metadata.LoaderName, L"MiniTIFF");

    if (result.pixels) {
        _aligned_free(result.pixels);
    }
}

TEST(MiniTiffTest, LoadLzwARGB) {
    std::wstring path = L"../../../Local-Files/test_img/透明度测试/test_tiff_argb.tif";
    std::vector<uint8_t> bytes = ReadFileBytes(path);
    ASSERT_FALSE(bytes.empty()) << "Failed to read test image test_tiff_argb.tif";

    QuickView::Codec::DecodeContext ctx;
    ctx.allocator.ctx = nullptr;
    ctx.allocator.pfn = [](void*, size_t s) -> uint8_t* {
        return static_cast<uint8_t*>(_aligned_malloc(s, 64));
    };

    QuickView::Codec::DecodeResult result;
    HRESULT hr = QuickView::MiniTiff::Load(bytes.data(), bytes.size(), ctx, result);

    EXPECT_EQ(hr, S_OK);
    EXPECT_TRUE(result.success);
    EXPECT_NE(result.pixels, nullptr);
    EXPECT_GT(result.width, 0);
    EXPECT_GT(result.height, 0);
    EXPECT_EQ(result.metadata.LoaderName, L"MiniTIFF");

    if (result.pixels) {
        _aligned_free(result.pixels);
    }
}

TEST(MiniTiffTest, RecursiveConsistencyCheck) {
    namespace fs = std::filesystem;
    std::wstring basePath = L"../../../Local-Files/test_img";
    
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ASSERT_TRUE(SUCCEEDED(hrCo) || hrCo == S_FALSE);

    Microsoft::WRL::ComPtr<IWICImagingFactory> wicFactory;
    HRESULT hrFactory = CoCreateInstance(
        CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&wicFactory)
    );
    ASSERT_TRUE(SUCCEEDED(hrFactory));

    uint32_t testedCount = 0;
    uint32_t supportedCount = 0;

    for (const auto& entry : fs::recursive_directory_iterator(basePath)) {
        if (!entry.is_regular_file()) continue;
        auto path = entry.path();
        auto ext = path.extension().wstring();
        for (auto& c : ext) c = std::towlower(c);
        if (ext != L".tif" && ext != L".tiff") continue;

        std::wstring filename = path.filename().wstring();
        for (auto& c : filename) c = std::towlower(c);
        
        if (path.wstring().find(L"\\raw\\") != std::wstring::npos ||
            path.wstring().find(L"\\raw 配对测试\\") != std::wstring::npos) {
            continue;
        }

        std::vector<uint8_t> bytes = ReadFileBytes(path.wstring());
        if (bytes.empty()) continue;

        testedCount++;

        QuickView::Codec::DecodeContext ctx;
        ctx.allocator.ctx = nullptr;
        ctx.allocator.pfn = [](void*, size_t s) -> uint8_t* {
            return static_cast<uint8_t*>(_aligned_malloc(s, 64));
        };

        QuickView::Codec::DecodeResult result;
        HRESULT hrLoad = QuickView::MiniTiff::Load(bytes.data(), bytes.size(), ctx, result);

        if (hrLoad == S_OK) {
            supportedCount++;
            
            Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
            HRESULT hrWic = wicFactory->CreateDecoderFromFilename(
                path.wstring().c_str(), nullptr, GENERIC_READ,
                WICDecodeMetadataCacheOnDemand, &decoder
            );
            if (SUCCEEDED(hrWic)) {
                Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
                if (SUCCEEDED(decoder->GetFrame(0, &frame))) {
                    UINT wicW = 0, wicH = 0;
                    frame->GetSize(&wicW, &wicH);
                    
                    EXPECT_EQ(result.width, static_cast<int>(wicW)) << "Width mismatch on: " << path.string();
                    EXPECT_EQ(result.height, static_cast<int>(wicH)) << "Height mismatch on: " << path.string();
                    
                    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
                    if (SUCCEEDED(wicFactory->CreateFormatConverter(&converter)) &&
                        SUCCEEDED(converter->Initialize(
                            frame.Get(), GUID_WICPixelFormat32bppBGRA,
                            WICBitmapDitherTypeNone, nullptr, 0.0f,
                            WICBitmapPaletteTypeCustom
                        ))) {
                        
                        int wicStride = ((wicW * 4) + 63) & ~63;
                        size_t wicBufSize = static_cast<size_t>(wicStride) * wicH;
                        std::vector<uint8_t> wicPixels(wicBufSize);
                        
                        if (SUCCEEDED(converter->CopyPixels(
                            nullptr, wicStride, static_cast<UINT>(wicBufSize),
                            wicPixels.data()
                        ))) {
                            bool isCmyk = (result.metadata.FormatDetails.find(L"CMYK") != std::wstring::npos);
                            bool pixelsMatch = true;
                            size_t diffCount = 0;
                            if (!isCmyk) {
                                for (UINT y = 0; y < wicH; ++y) {
                                    const uint8_t* rowMini = result.pixels + y * result.stride;
                                    const uint8_t* rowWic = wicPixels.data() + y * wicStride;
                                    for (UINT x = 0; x < wicW; ++x) {
                                        uint8_t wicA = rowWic[x * 4 + 3];
                                        for (int c = 0; c < 4; ++c) {
                                            uint8_t wicVal = rowWic[x * 4 + c];
                                            if (c < 3) {
                                                wicVal = static_cast<uint8_t>((wicVal * wicA + 127) / 255);
                                            }
                                            int diff = std::abs(static_cast<int>(rowMini[x * 4 + c]) - static_cast<int>(wicVal));
                                            if (diff > 1) {
                                                pixelsMatch = false;
                                                diffCount++;
                                            }
                                        }
                                    }
                                }
                                double diffPercent = (double)diffCount / (wicW * wicH * 4) * 100.0;
                                EXPECT_LT(diffPercent, 1.0) << "Pixel difference too high (" << diffPercent << "%) on: " << path.string();
                                if (diffPercent > 0.0) {
                                    std::printf("[Info] Minor pixel mismatch (%.3f%%) on %S\n", diffPercent, filename.c_str());
                                }
                            } else {
                                std::printf("[Info] Skipped pixel comparison for CMYK image: %S\n", filename.c_str());
                            }
                        }
                    }
                }
            }
        } else if (hrLoad == E_NOTIMPL) {
            std::printf("[Info] Fallback to WIC for: %S\n", filename.c_str());
        } else {
            FAIL() << "MiniTiff failed to load with error code: " << std::hex << hrLoad << " on file: " << path.string();
        }

        if (result.pixels) {
            _aligned_free(result.pixels);
        }
    }

    std::printf("[Summary] Tested %u TIFF files. MiniTiff supported %u, fallback %u\n",
                testedCount, supportedCount, testedCount - supportedCount);
    
    CoUninitialize();
}

TEST(MiniTiffTest, RegionConsistencyCheck) {
    std::wstring path = L"../../../Local-Files/test_img/格式测试/tiff-rgb24.tiff";
    std::vector<uint8_t> bytes = ReadFileBytes(path);
    ASSERT_FALSE(bytes.empty()) << "Failed to read test image tiff-rgb24.tiff";

    // 1. Decode full image
    QuickView::Codec::DecodeContext ctx;
    ctx.allocator.ctx = nullptr;
    ctx.allocator.pfn = [](void*, size_t s) -> uint8_t* {
        return static_cast<uint8_t*>(_aligned_malloc(s, 64));
    };

    QuickView::Codec::DecodeResult fullResult;
    HRESULT hrFull = QuickView::MiniTiff::Load(bytes.data(), bytes.size(), ctx, fullResult);
    ASSERT_EQ(hrFull, S_OK);
    ASSERT_TRUE(fullResult.success);

    // 2. Decode region
    int cropX = 1500;
    int cropY = 1200;
    int cropW = 1000;
    int cropH = 800;

    QuickView::Codec::DecodeResult regionResult;
    HRESULT hrRegion = QuickView::MiniTiff::LoadRegion(bytes.data(), bytes.size(), ctx, regionResult, cropX, cropY, cropW, cropH);
    ASSERT_EQ(hrRegion, S_OK);
    ASSERT_TRUE(regionResult.success);
    EXPECT_EQ(regionResult.width, cropW);
    EXPECT_EQ(regionResult.height, cropH);

    // 3. Compare pixel by pixel
    for (int y = 0; y < cropH; ++y) {
        const uint8_t* fullRow = fullResult.pixels + static_cast<size_t>(cropY + y) * fullResult.stride + cropX * 4;
        const uint8_t* regionRow = regionResult.pixels + static_cast<size_t>(y) * regionResult.stride;
        for (int x = 0; x < cropW; ++x) {
            for (int c = 0; c < 4; ++c) {
                EXPECT_EQ(fullRow[x * 4 + c], regionRow[x * 4 + c])
                    << "Pixel mismatch at output pixel (x=" << x << ", y=" << y << "), channel=" << c;
            }
        }
    }

    if (fullResult.pixels) _aligned_free(fullResult.pixels);
    if (regionResult.pixels) _aligned_free(regionResult.pixels);
}

namespace {

struct TiffIfdEntrySpec {
    uint16_t tag;
    uint16_t type;
    uint32_t count;
    uint32_t valueOrOffset;
};

static void AppendU16LE(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

static void AppendU32LE(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

static std::vector<uint8_t> BuildMinimalTiffLE(const std::vector<TiffIfdEntrySpec>& entries,
                                               const std::vector<uint8_t>& extraData) {
    std::vector<uint8_t> out;
    // TIFF Header (Little-Endian, magic 42, IFD at offset 8)
    out.push_back('I');
    out.push_back('I');
    AppendU16LE(out, 42);
    AppendU32LE(out, 8);

    // IFD
    AppendU16LE(out, static_cast<uint16_t>(entries.size()));
    for (const auto& e : entries) {
        AppendU16LE(out, e.tag);
        AppendU16LE(out, e.type);
        AppendU32LE(out, e.count);
        AppendU32LE(out, e.valueOrOffset);
    }
    AppendU32LE(out, 0); // Next IFD offset

    // Extra data starts immediately after IFD
    out.insert(out.end(), extraData.begin(), extraData.end());
    return out;
}

static QuickView::Codec::DecodeContext MakeTestDecodeContext() {
    QuickView::Codec::DecodeContext ctx;
    ctx.allocator.ctx = nullptr;
    ctx.allocator.pfn = [](void*, size_t s) -> uint8_t* {
        return static_cast<uint8_t*>(_aligned_malloc(s, 64));
    };
    return ctx;
}

} // namespace

TEST(MiniTiffSecurityTest, Tiled16BitGrayscaleWithPredictorNoOob) {
    // Regression test for GHSA-r5qf-q487-gwcq (Bug 1):
    // 32x32 16-bit grayscale tiled TIFF with 16x16 tiles (4 tiles) and Predictor=2.
    // Previously tileBytes was computed as tileW * tileH * samples (256) instead of
    // tileW * tileH * samples * bytesPerSample (512), causing heap OOB read/write.
    constexpr uint32_t kWidth = 32;
    constexpr uint32_t kHeight = 32;
    constexpr uint32_t kTileW = 16;
    constexpr uint32_t kTileH = 16;
    constexpr uint32_t kTileCount = 4;
    constexpr uint32_t kTileBytes = kTileW * kTileH * 2; // 512 bytes per tile

    // IFD has 11 entries: header(8) + count(2) + 11*12(132) + next(4) = 146 bytes
    constexpr uint32_t kExtraBase = 8 + 2 + 11 * 12 + 4;
    const uint32_t offsetsArrayPos = kExtraBase;
    const uint32_t countsArrayPos = offsetsArrayPos + kTileCount * 4;
    const uint32_t tileDataPos = countsArrayPos + kTileCount * 4;

    std::vector<uint8_t> extra;
    for (uint32_t i = 0; i < kTileCount; ++i) {
        AppendU32LE(extra, tileDataPos + i * kTileBytes);
    }
    for (uint32_t i = 0; i < kTileCount; ++i) {
        AppendU32LE(extra, kTileBytes);
    }

    // Fill 4 tiles with horizontal-differenced 16-bit grayscale samples.
    // First pixel of each row has high byte 0x80 (low 0x00), subsequent deltas are 0.
    for (uint32_t t = 0; t < kTileCount; ++t) {
        for (uint32_t y = 0; y < kTileH; ++y) {
            extra.push_back(0x00);
            extra.push_back(0x80);
            for (uint32_t x = 1; x < kTileW; ++x) {
                extra.push_back(0x00);
                extra.push_back(0x00);
            }
        }
    }

    std::vector<TiffIfdEntrySpec> entries = {
        {256, 4, 1, kWidth},          // ImageWidth
        {257, 4, 1, kHeight},         // ImageHeight
        {258, 3, 1, 16},              // BitsPerSample = 16
        {259, 3, 1, 1},               // Compression = Uncompressed
        {262, 3, 1, 1},               // Photometric = BlackIsZero
        {277, 3, 1, 1},               // SamplesPerPixel = 1
        {284, 3, 1, 1},               // PlanarConfiguration = Contiguous
        {317, 3, 1, 2},               // Predictor = Horizontal
        {322, 4, 1, kTileW},          // TileWidth
        {323, 4, 1, kTileH},          // TileLength
        {324, 4, kTileCount, offsetsArrayPos}, // TileOffsets
    };
    // Need TileByteCounts too -> 12 entries total, recalculate kExtraBase
    entries.push_back({325, 4, kTileCount, countsArrayPos});
    const uint32_t actualExtraBase = 8 + 2 + static_cast<uint32_t>(entries.size()) * 12 + 4;
    const uint32_t shift = actualExtraBase - kExtraBase;
    entries[10].valueOrOffset += shift;
    entries[11].valueOrOffset += shift;
    for (uint32_t i = 0; i < kTileCount; ++i) {
        uint32_t newOff = tileDataPos + shift + i * kTileBytes;
        extra[i * 4 + 0] = static_cast<uint8_t>(newOff & 0xFF);
        extra[i * 4 + 1] = static_cast<uint8_t>((newOff >> 8) & 0xFF);
        extra[i * 4 + 2] = static_cast<uint8_t>((newOff >> 16) & 0xFF);
        extra[i * 4 + 3] = static_cast<uint8_t>((newOff >> 24) & 0xFF);
    }

    std::vector<uint8_t> tiff = BuildMinimalTiffLE(entries, extra);
    auto ctx = MakeTestDecodeContext();
    QuickView::Codec::DecodeResult result;

    HRESULT hr = QuickView::MiniTiff::Load(tiff.data(), tiff.size(), ctx, result);
    EXPECT_EQ(hr, S_OK);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.width, static_cast<int>(kWidth));
    EXPECT_EQ(result.height, static_cast<int>(kHeight));
    ASSERT_NE(result.pixels, nullptr);

    // Verify decoded pixels (every pixel should be gray 0x80 with alpha 0xFF)
    for (uint32_t y = 0; y < kHeight; ++y) {
        const uint8_t* row = result.pixels + static_cast<size_t>(y) * result.stride;
        for (uint32_t x = 0; x < kWidth; ++x) {
            EXPECT_EQ(row[x * 4 + 0], 0x80);
            EXPECT_EQ(row[x * 4 + 1], 0x80);
            EXPECT_EQ(row[x * 4 + 2], 0x80);
            EXPECT_EQ(row[x * 4 + 3], 0xFF);
        }
    }

    _aligned_free(result.pixels);
}

TEST(MiniTiffSecurityTest, Tiled16BitRgbSimdDownsampleNoOob) {
    // 16x16 16-bit RGB single-tile TIFF exercising SIMD Pack16to8 path
    constexpr uint32_t kWidth = 16;
    constexpr uint32_t kHeight = 16;
    constexpr uint32_t kTileBytes = kWidth * kHeight * 3 * 2; // 1536 bytes

    constexpr uint32_t kNumEntries = 11;
    constexpr uint32_t kExtraBase = 8 + 2 + kNumEntries * 12 + 4;
    const uint32_t bpsPos = kExtraBase;
    const uint32_t tileDataPos = bpsPos + 6;

    std::vector<uint8_t> extra;
    AppendU16LE(extra, 16);
    AppendU16LE(extra, 16);
    AppendU16LE(extra, 16);

    // Fill with R=0xAA00, G=0x5500, B=0x1100 (LE)
    for (uint32_t i = 0; i < kWidth * kHeight; ++i) {
        AppendU16LE(extra, 0xAA00);
        AppendU16LE(extra, 0x5500);
        AppendU16LE(extra, 0x1100);
    }

    std::vector<TiffIfdEntrySpec> entries = {
        {256, 4, 1, kWidth},
        {257, 4, 1, kHeight},
        {258, 3, 3, bpsPos},          // BitsPerSample = [16, 16, 16]
        {259, 3, 1, 1},               // Uncompressed
        {262, 3, 1, 2},               // RGB
        {277, 3, 1, 3},               // SamplesPerPixel = 3
        {284, 3, 1, 1},               // Contiguous
        {322, 4, 1, kWidth},          // TileWidth
        {323, 4, 1, kHeight},         // TileLength
        {324, 4, 1, tileDataPos},     // TileOffsets (single tile)
        {325, 4, 1, kTileBytes},      // TileByteCounts (single tile)
    };

    std::vector<uint8_t> tiff = BuildMinimalTiffLE(entries, extra);
    auto ctx = MakeTestDecodeContext();
    QuickView::Codec::DecodeResult result;

    HRESULT hr = QuickView::MiniTiff::Load(tiff.data(), tiff.size(), ctx, result);
    EXPECT_EQ(hr, S_OK);
    EXPECT_TRUE(result.success);
    ASSERT_NE(result.pixels, nullptr);

    const uint8_t* p = result.pixels;
    EXPECT_EQ(p[0], 0x11); // B
    EXPECT_EQ(p[1], 0x55); // G
    EXPECT_EQ(p[2], 0xAA); // R
    EXPECT_EQ(p[3], 0xFF); // A

    _aligned_free(result.pixels);
}

TEST(MiniTiffSecurityTest, RejectStripRowBytesOverflow) {
    // Regression test for GHSA-r5qf-q487-gwcq (Bug 2):
    // ImageWidth=65538, SamplesPerPixel=65535, BitsPerSample=8 previously wrapped uint32_t rowBytes.
    constexpr uint32_t kNumEntries = 10;
    constexpr uint32_t kDataPos = 8 + 2 + kNumEntries * 12 + 4;
    std::vector<uint8_t> extra(64, 0);

    std::vector<TiffIfdEntrySpec> entries = {
        {256, 4, 1, 65538},     // ImageWidth
        {257, 4, 1, 1},         // ImageHeight
        {258, 3, 1, 8},         // BitsPerSample
        {259, 3, 1, 1},         // Uncompressed
        {262, 3, 1, 1},         // BlackIsZero
        {273, 4, 1, kDataPos},  // StripOffsets
        {277, 3, 1, 65535},     // SamplesPerPixel (overflows 32-bit width*samples)
        {278, 4, 1, 1},         // RowsPerStrip
        {279, 4, 1, 64},        // StripByteCounts
        {317, 3, 1, 2},         // Predictor = Horizontal
    };

    std::vector<uint8_t> tiff = BuildMinimalTiffLE(entries, extra);
    auto ctx = MakeTestDecodeContext();
    QuickView::Codec::DecodeResult result;

    HRESULT hr = QuickView::MiniTiff::Load(tiff.data(), tiff.size(), ctx, result);
    EXPECT_NE(hr, S_OK);
    EXPECT_FALSE(result.success);
    if (result.pixels) _aligned_free(result.pixels);
}

TEST(MiniTiffSecurityTest, RejectZeroTileDimensions) {
    // Regression test for GHSA-r5qf-q487-gwcq (Bug 3):
    // TileWidth=0 or TileLength=0 previously caused integer division by zero (SIGFPE).
    for (int mode = 0; mode < 2; ++mode) {
        constexpr uint32_t kNumEntries = 10;
        constexpr uint32_t kDataPos = 8 + 2 + kNumEntries * 12 + 4;
        std::vector<uint8_t> extra(64, 0);

        std::vector<TiffIfdEntrySpec> entries = {
            {256, 4, 1, 16},
            {257, 4, 1, 16},
            {258, 3, 1, 8},
            {259, 3, 1, 1},
            {262, 3, 1, 1},
            {277, 3, 1, 1},
            {322, 4, 1, mode == 0 ? 0u : 16u}, // TileWidth
            {323, 4, 1, mode == 0 ? 16u : 0u}, // TileLength
            {324, 4, 1, kDataPos},
            {325, 4, 1, 64},
        };

        std::vector<uint8_t> tiff = BuildMinimalTiffLE(entries, extra);
        auto ctx = MakeTestDecodeContext();
        QuickView::Codec::DecodeResult result;

        HRESULT hr = QuickView::MiniTiff::Load(tiff.data(), tiff.size(), ctx, result);
        EXPECT_EQ(hr, E_FAIL);
        EXPECT_FALSE(result.success);
        if (result.pixels) _aligned_free(result.pixels);
    }
}

TEST(MiniTiffSecurityTest, RejectIfdCountOverflow) {
    // Crafted IFD where count * 2 wraps 32-bit arithmetic (0x80000001 * 2 == 2)
    std::vector<uint8_t> extra(64, 0);
    std::vector<TiffIfdEntrySpec> entries = {
        {256, 4, 1, 16},
        {257, 4, 1, 16},
        {258, 3, 1, 8},
        {259, 3, 1, 1},
        {262, 3, 1, 3},           // Palette
        {273, 3, 0x80000001u, 0}, // StripOffsets with overflowing count * 2
        {277, 3, 1, 1},
        {279, 3, 0x80000001u, 0}, // StripByteCounts
        {320, 3, 0x80000001u, 0}, // ColorMap with overflowing count * 2
    };

    std::vector<uint8_t> tiff = BuildMinimalTiffLE(entries, extra);
    auto ctx = MakeTestDecodeContext();
    QuickView::Codec::DecodeResult result;

    HRESULT hr = QuickView::MiniTiff::Load(tiff.data(), tiff.size(), ctx, result);
    EXPECT_EQ(hr, E_FAIL);
    EXPECT_FALSE(result.success);
    if (result.pixels) _aligned_free(result.pixels);
}

TEST(MiniTiffDirectBufferTest, LoadDirectToBuffer16BitRgbPreservesIcc) {
    constexpr uint32_t kWidth = 37;   // Exercises SIMD vector + scalar tail
    constexpr uint32_t kHeight = 260; // Exercises single-strip row path
    constexpr uint32_t kStripBytes = kWidth * kHeight * 6;
    const std::vector<uint8_t> mockIcc = {'m', 'o', 'c', 'k', 'I', 'C', 'C', '_', 'a', 'd', 'o', 'b', 'e', 'R', 'G', 'B'};

    constexpr uint32_t kNumEntries = 10;
    constexpr uint32_t kExtraBase = 8 + 2 + kNumEntries * 12 + 4;
    const uint32_t bpsPos = kExtraBase;
    const uint32_t iccPos = bpsPos + 6;
    const uint32_t pixelPos = iccPos + static_cast<uint32_t>(mockIcc.size());

    std::vector<uint8_t> extra;
    AppendU16LE(extra, 16);
    AppendU16LE(extra, 16);
    AppendU16LE(extra, 16);
    extra.insert(extra.end(), mockIcc.begin(), mockIcc.end());
    extra.reserve(extra.size() + kStripBytes);
    for (uint32_t y = 0; y < kHeight; ++y) {
        for (uint32_t x = 0; x < kWidth; ++x) {
            const uint8_t rHi = static_cast<uint8_t>((x * 7 + y) & 0xFF);
            const uint8_t gHi = static_cast<uint8_t>((x * 3 + y * 5) & 0xFF);
            const uint8_t bHi = static_cast<uint8_t>((x * 11 + y * 2) & 0xFF);
            AppendU16LE(extra, static_cast<uint16_t>((rHi << 8) | 0x12));
            AppendU16LE(extra, static_cast<uint16_t>((gHi << 8) | 0x34));
            AppendU16LE(extra, static_cast<uint16_t>((bHi << 8) | 0x56));
        }
    }

    std::vector<TiffIfdEntrySpec> entries = {
        {256, 4, 1, kWidth},
        {257, 4, 1, kHeight},
        {258, 3, 3, bpsPos},
        {259, 3, 1, 1}, // Uncompressed
        {262, 3, 1, 2}, // RGB
        {273, 4, 1, pixelPos},
        {277, 3, 1, 3},
        {278, 4, 1, kHeight}, // Single strip
        {279, 4, 1, kStripBytes},
        {34675, 7, static_cast<uint32_t>(mockIcc.size()), iccPos},
    };

    std::vector<uint8_t> tiff = BuildMinimalTiffLE(entries, extra);
    std::vector<uint8_t> outBuf(static_cast<size_t>(kWidth) * kHeight * 4, 0xCC);
    QuickView::Codec::DecodeResult result;

    HRESULT hr = QuickView::MiniTiff::LoadDirectToBuffer(
        tiff.data(), tiff.size(), outBuf.data(), outBuf.size(), 0, {}, result);
    ASSERT_EQ(hr, S_OK);
    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.pixels, outBuf.data());
    EXPECT_EQ(result.stride, static_cast<int>(kWidth * 4));
    EXPECT_EQ(result.width, static_cast<int>(kWidth));
    EXPECT_EQ(result.height, static_cast<int>(kHeight));
    EXPECT_TRUE(result.metadata.HasEmbeddedColorProfile.value_or(false));
    EXPECT_TRUE(result.metadata.colorInfo.hasEmbeddedIcc);
    std::vector<uint8_t> gotIcc(result.metadata.iccProfileData.begin(), result.metadata.iccProfileData.end());
    EXPECT_EQ(gotIcc, mockIcc);

    for (uint32_t y = 0; y < kHeight; ++y) {
        for (uint32_t x = 0; x < kWidth; ++x) {
            const size_t off = (static_cast<size_t>(y) * kWidth + x) * 4;
            EXPECT_EQ(outBuf[off + 0], static_cast<uint8_t>((x * 11 + y * 2) & 0xFF));
            EXPECT_EQ(outBuf[off + 1], static_cast<uint8_t>((x * 3 + y * 5) & 0xFF));
            EXPECT_EQ(outBuf[off + 2], static_cast<uint8_t>((x * 7 + y) & 0xFF));
            EXPECT_EQ(outBuf[off + 3], 0xFF);
        }
    }
}

TEST(MiniTiffDirectBufferTest, FloatTiffReturnsNotImplForHdrFallback) {
    constexpr uint32_t kWidth = 4;
    constexpr uint32_t kHeight = 4;
    constexpr uint32_t kStripBytes = kWidth * kHeight * 12; // 32-bit float RGB
    constexpr uint32_t kNumEntries = 10;
    constexpr uint32_t kExtraBase = 8 + 2 + kNumEntries * 12 + 4;
    const uint32_t bpsPos = kExtraBase;
    const uint32_t pixelPos = bpsPos + 6;

    std::vector<uint8_t> extra;
    AppendU16LE(extra, 32);
    AppendU16LE(extra, 32);
    AppendU16LE(extra, 32);
    extra.resize(extra.size() + kStripBytes, 0);

    std::vector<TiffIfdEntrySpec> entries = {
        {256, 4, 1, kWidth},
        {257, 4, 1, kHeight},
        {258, 3, 3, bpsPos},
        {259, 3, 1, 1},
        {262, 3, 1, 2},
        {273, 4, 1, pixelPos},
        {277, 3, 1, 3},
        {278, 4, 1, kHeight},
        {279, 4, 1, kStripBytes},
        {339, 3, 1, 3}, // SampleFormat = IEEE floating point
    };

    std::vector<uint8_t> tiff = BuildMinimalTiffLE(entries, extra);
    std::vector<uint8_t> outBuf(kWidth * kHeight * 4, 0);
    QuickView::Codec::DecodeResult result;

    HRESULT hr = QuickView::MiniTiff::LoadDirectToBuffer(
        tiff.data(), tiff.size(), outBuf.data(), outBuf.size(), 0, {}, result);
    EXPECT_EQ(hr, E_NOTIMPL);
    EXPECT_FALSE(result.success);
}
