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
