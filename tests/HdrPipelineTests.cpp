#include <gtest/gtest.h>

#include "pch.h"
#include "ColorMath.h"
#include "ComputeEngine.h"
#include "DisplayColorInfo.h"
#include "ImageLoaderSimd.h"
#include "ImageTypes.h"
#include "MiniTiff.h"
#include "ToolProcessProtocol.h"
#include "WuffsLoader.h"

#include <d2d1_1.h>
#include <d2d1effects.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <jxl/encode.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

std::filesystem::path FindHdrTestAsset(const wchar_t* filename) {
    const std::filesystem::path sourceDir = std::filesystem::path(__FILE__).parent_path();
    const std::filesystem::path candidates[] = {
        std::filesystem::current_path() / "Local-Files" / "test_img" / "Hdr" / filename,
        sourceDir.parent_path() / "Local-Files" / "test_img" / "Hdr" / filename,
    };
    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
    }
    return {};
}

bool ReadFileBytes(const std::filesystem::path& path, std::vector<uint8_t>* outBytes) {
    if (!outBytes || path.empty()) return false;
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) return false;
    const std::streamoff size = ifs.tellg();
    if (size <= 0) return false;
    outBytes->resize(static_cast<size_t>(size));
    ifs.seekg(0, std::ios::beg);
    return static_cast<bool>(ifs.read(reinterpret_cast<char*>(outBytes->data()), size));
}

void WriteU32BE(uint8_t* dst, uint32_t v) {
    dst[0] = static_cast<uint8_t>((v >> 24) & 0xFF);
    dst[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
    dst[2] = static_cast<uint8_t>((v >> 8) & 0xFF);
    dst[3] = static_cast<uint8_t>(v & 0xFF);
}

uint16_t FloatToHalf(float f) {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    const uint32_t sign = (bits >> 16) & 0x8000u;
    int32_t exp = static_cast<int32_t>((bits >> 23) & 0xFFu) - 127 + 15;
    uint32_t mant = bits & 0x7FFFFFu;
    if (exp <= 0) {
        if (exp < -10) return static_cast<uint16_t>(sign);
        mant |= 0x800000u;
        const uint32_t shift = static_cast<uint32_t>(14 - exp);
        return static_cast<uint16_t>(sign | (mant >> shift));
    }
    if (exp >= 31) {
        return static_cast<uint16_t>(sign | 0x7C00u | (mant ? 0x0200u : 0u));
    }
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10) | (mant >> 13));
}

float HalfToFloat(uint16_t h) {
    const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
    const uint32_t exp = (h >> 10) & 0x1Fu;
    uint32_t mant = h & 0x03FFu;
    uint32_t out = 0;
    if (exp == 0) {
        if (mant == 0) {
            out = sign;
        } else {
            int e = -14;
            while ((mant & 0x0400u) == 0) {
                mant <<= 1;
                --e;
            }
            mant &= 0x03FFu;
            const uint32_t outExp = static_cast<uint32_t>(e + 127) << 23;
            out = sign | outExp | (mant << 13);
        }
    } else if (exp == 31) {
        out = sign | 0x7F800000u | (mant << 13);
    } else {
        out = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float f = 0.0f;
    std::memcpy(&f, &out, sizeof(f));
    return f;
}

float NitsToPqSignal(float nits) {
    const float y = std::clamp(nits / 10000.0f, 0.0f, 1.0f);
    if (y <= 0.0f) return 0.0f;
    constexpr float m1 = 2610.0f / 16384.0f;
    constexpr float m2 = (2523.0f / 4096.0f) * 128.0f;
    constexpr float c1 = 3424.0f / 4096.0f;
    constexpr float c2 = (2413.0f / 4096.0f) * 32.0f;
    constexpr float c3 = (2392.0f / 4096.0f) * 32.0f;
    const float ym = std::pow(y, m1);
    return std::pow((c1 + c2 * ym) / (1.0f + c3 * ym), m2);
}

std::vector<uint8_t> BuildSyntheticIccCicp(
    uint8_t primaries, uint8_t transfer, uint8_t matrix, uint8_t fullRange,
    bool validSignature = true, uint32_t overrideTagCount = 1,
    uint32_t overrideDeclaredSize = 0) {
    // 128-byte header + 4-byte tagCount + 12-byte tagEntry + 12-byte cicp tag = 156 bytes
    std::vector<uint8_t> icc(156, 0);
    WriteU32BE(icc.data(), overrideDeclaredSize ? overrideDeclaredSize : static_cast<uint32_t>(icc.size()));
    std::memcpy(icc.data() + 36, validSignature ? "acsp" : "BAD!", 4);
    WriteU32BE(icc.data() + 128, overrideTagCount);
    std::memcpy(icc.data() + 132, "cicp", 4);
    WriteU32BE(icc.data() + 136, 144);
    WriteU32BE(icc.data() + 140, 12);
    std::memcpy(icc.data() + 144, "cicp", 4);
    icc[148 + 4] = primaries;
    icc[148 + 5] = transfer;
    icc[148 + 6] = matrix;
    icc[148 + 7] = fullRange;
    return icc;
}

void AppendPngChunk(std::vector<uint8_t>& png, const char type[4], const uint8_t* payload, size_t len) {
    uint8_t header[8];
    WriteU32BE(header, static_cast<uint32_t>(len));
    std::memcpy(header + 4, type, 4);
    png.insert(png.end(), header, header + 8);
    if (len > 0 && payload) {
        png.insert(png.end(), payload, payload + len);
    }
    uint32_t crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, reinterpret_cast<const Bytef*>(type), 4);
    if (len > 0 && payload) {
        crc = crc32(crc, payload, static_cast<uInt>(len));
    }
    uint8_t crcBytes[4];
    WriteU32BE(crcBytes, crc);
    png.insert(png.end(), crcBytes, crcBytes + 4);
}

std::vector<uint8_t> BuildSynthetic1x1Png(
    bool hasTopCicp, uint8_t topPrimaries, uint8_t topTransfer,
    const std::vector<uint8_t>* iccProfile,
    bool hasSrgbChunk,
    bool hasClliChunk, uint32_t maxCllScaled, uint32_t maxFallScaled,
    bool hasMdcvChunk, uint32_t mdcvMaxScaled, uint32_t mdcvMinScaled) {
    static constexpr uint8_t kPngSig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> png(kPngSig, kPngSig + 8);

    // 1x1 8-bit RGBA IHDR
    uint8_t ihdr[13] = {};
    WriteU32BE(ihdr, 1);
    WriteU32BE(ihdr + 4, 1);
    ihdr[8] = 8;  // bit depth
    ihdr[9] = 6;  // RGBA
    AppendPngChunk(png, "IHDR", ihdr, sizeof(ihdr));

    if (hasTopCicp) {
        const uint8_t cicp[4] = {topPrimaries, topTransfer, 0, 1};
        AppendPngChunk(png, "cICP", cicp, sizeof(cicp));
    }
    if (iccProfile && !iccProfile->empty()) {
        std::vector<uint8_t> compressed(compressBound(static_cast<uLong>(iccProfile->size())));
        uLong compLen = static_cast<uLong>(compressed.size());
        if (compress2(compressed.data(), &compLen, iccProfile->data(),
                      static_cast<uLong>(iccProfile->size()), Z_BEST_SPEED) == Z_OK) {
            std::vector<uint8_t> iccp;
            const char name[] = "test";
            iccp.insert(iccp.end(), name, name + sizeof(name)); // includes NUL
            iccp.push_back(0); // compression method: deflate
            iccp.insert(iccp.end(), compressed.data(), compressed.data() + compLen);
            AppendPngChunk(png, "iCCP", iccp.data(), iccp.size());
        }
    }
    if (hasSrgbChunk) {
        const uint8_t intent = 0;
        AppendPngChunk(png, "sRGB", &intent, 1);
    }
    if (hasClliChunk) {
        uint8_t clli[8] = {};
        WriteU32BE(clli, maxCllScaled);
        WriteU32BE(clli + 4, maxFallScaled);
        AppendPngChunk(png, "cLLi", clli, sizeof(clli));
    }
    if (hasMdcvChunk) {
        uint8_t mdcv[24] = {};
        WriteU32BE(mdcv + 16, mdcvMaxScaled);
        WriteU32BE(mdcv + 20, mdcvMinScaled);
        AppendPngChunk(png, "mDCv", mdcv, sizeof(mdcv));
    }

    // 1x1 filter byte (0) + RGBA pixel
    const uint8_t rawScanline[5] = {0, 128, 64, 255, 255};
    uint8_t idat[64] = {};
    uLong idatLen = sizeof(idat);
    compress2(idat, &idatLen, rawScanline, sizeof(rawScanline), Z_BEST_SPEED);
    AppendPngChunk(png, "IDAT", idat, idatLen);
    AppendPngChunk(png, "IEND", nullptr, 0);
    return png;
}

HRESULT CreateD3D11DeviceWithWarpFallback(
    ComPtr<ID3D11Device>* outDevice,
    ComPtr<ID3D11DeviceContext>* outContext,
    D3D_DRIVER_TYPE* outDriverType = nullptr) {
    if (!outDevice || !outContext) return E_INVALIDARG;
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    static constexpr D3D_FEATURE_LEVEL kLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    const D3D_DRIVER_TYPE types[] = {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP};
    HRESULT lastHr = E_FAIL;
    for (D3D_DRIVER_TYPE driverType : types) {
        D3D_FEATURE_LEVEL achieved = D3D_FEATURE_LEVEL_11_0;
        ComPtr<ID3D11Device> dev;
        ComPtr<ID3D11DeviceContext> ctx;
        lastHr = D3D11CreateDevice(
            nullptr, driverType, nullptr, flags,
            kLevels, static_cast<UINT>(std::size(kLevels)),
            D3D11_SDK_VERSION, &dev, &achieved, &ctx);
        if (SUCCEEDED(lastHr) && dev && ctx) {
            *outDevice = std::move(dev);
            *outContext = std::move(ctx);
            if (outDriverType) *outDriverType = driverType;
            return S_OK;
        }
    }
    return lastHr;
}

bool ReadbackFp16Texture(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    ID3D11Texture2D* srcTex,
    std::vector<uint16_t>* outHalfRgba,
    UINT* outWidth = nullptr,
    UINT* outHeight = nullptr) {
    if (!device || !context || !srcTex || !outHalfRgba) return false;
    D3D11_TEXTURE2D_DESC desc = {};
    srcTex->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) return false;

    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &staging)) || !staging) {
        return false;
    }

    context->CopyResource(staging.Get(), srcTex);
    context->Flush();

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return false;
    }

    outHalfRgba->resize(static_cast<size_t>(desc.Width) * static_cast<size_t>(desc.Height) * 4u);
    const uint8_t* srcBytes = static_cast<const uint8_t*>(mapped.pData);
    for (UINT y = 0; y < desc.Height; ++y) {
        std::memcpy(
            outHalfRgba->data() + static_cast<size_t>(y) * desc.Width * 4u,
            srcBytes + static_cast<size_t>(y) * mapped.RowPitch,
            static_cast<size_t>(desc.Width) * 8u);
    }
    context->Unmap(staging.Get(), 0);
    if (outWidth) *outWidth = desc.Width;
    if (outHeight) *outHeight = desc.Height;
    return true;
}

bool ReadbackRgba8Texture(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    ID3D11Texture2D* srcTex,
    std::vector<uint8_t>* outRgba8,
    UINT* outWidth = nullptr,
    UINT* outHeight = nullptr) {
    if (!device || !context || !srcTex || !outRgba8) return false;
    D3D11_TEXTURE2D_DESC desc = {};
    srcTex->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
        desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        return false;
    }

    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &staging)) || !staging) {
        return false;
    }

    context->CopyResource(staging.Get(), srcTex);
    context->Flush();

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return false;
    }

    outRgba8->resize(static_cast<size_t>(desc.Width) * static_cast<size_t>(desc.Height) * 4u);
    const uint8_t* srcBytes = static_cast<const uint8_t*>(mapped.pData);
    for (UINT y = 0; y < desc.Height; ++y) {
        std::memcpy(
            outRgba8->data() + static_cast<size_t>(y) * desc.Width * 4u,
            srcBytes + static_cast<size_t>(y) * mapped.RowPitch,
            static_cast<size_t>(desc.Width) * 4u);
    }
    context->Unmap(staging.Get(), 0);
    if (outWidth) *outWidth = desc.Width;
    if (outHeight) *outHeight = desc.Height;
    return true;
}

float LinearSrgbToOkLabHueDeg(float r, float g, float b) {
    const float l = 0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b;
    const float m = 0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b;
    const float s = 0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b;
    const float l_ = std::cbrt(l);
    const float m_ = std::cbrt(m);
    const float s_ = std::cbrt(s);
    const float a = 1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_;
    const float b_ok = 0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_;
    return std::atan2(b_ok, a) * (180.0f / 3.14159265358979323846f);
}

} // namespace

TEST(HdrPipelineTests, SwizzleBGRA16ToRGBA16) {
    const size_t kLengths[] = {0, 1, 3, 7, 16, 33, 1025};
    for (size_t count : kLengths) {
        std::vector<uint16_t> pixels(count * 4u);
        for (size_t i = 0; i < count; ++i) {
            pixels[i * 4u + 0u] = static_cast<uint16_t>((i * 3u + 1u) & 0xFFFFu);   // B
            pixels[i * 4u + 1u] = static_cast<uint16_t>((i * 5u + 2u) & 0xFFFFu);   // G
            pixels[i * 4u + 2u] = static_cast<uint16_t>((i * 7u + 3u) & 0xFFFFu);   // R
            pixels[i * 4u + 3u] = static_cast<uint16_t>((i * 11u + 4u) & 0xFFFFu);  // A
        }

        ImageLoaderSimd::SwizzleBGRA16ToRGBA16(pixels.data(), count);
        for (size_t i = 0; i < count; ++i) {
            EXPECT_EQ(pixels[i * 4u + 0u], static_cast<uint16_t>((i * 7u + 3u) & 0xFFFFu));
            EXPECT_EQ(pixels[i * 4u + 1u], static_cast<uint16_t>((i * 5u + 2u) & 0xFFFFu));
            EXPECT_EQ(pixels[i * 4u + 2u], static_cast<uint16_t>((i * 3u + 1u) & 0xFFFFu));
            EXPECT_EQ(pixels[i * 4u + 3u], static_cast<uint16_t>((i * 11u + 4u) & 0xFFFFu));
        }

        // Second invocation must restore original BGRA order cleanly.
        ImageLoaderSimd::SwizzleBGRA16ToRGBA16(pixels.data(), count);
        for (size_t i = 0; i < count; ++i) {
            EXPECT_EQ(pixels[i * 4u + 0u], static_cast<uint16_t>((i * 3u + 1u) & 0xFFFFu));
            EXPECT_EQ(pixels[i * 4u + 2u], static_cast<uint16_t>((i * 7u + 3u) & 0xFFFFu));
        }
    }
}

TEST(HdrPipelineTests, TryParseIccCicp) {
    // 1. Valid Rec.2020 + PQ
    {
        auto icc = BuildSyntheticIccCicp(9, 16, 0, 1);
        QuickView::ColorPrimaries p = QuickView::ColorPrimaries::Unknown;
        QuickView::TransferFunction tf = QuickView::TransferFunction::Unknown;
        EXPECT_TRUE(QuickView::TryParseIccCicp(icc.data(), icc.size(), &p, &tf));
        EXPECT_EQ(p, QuickView::ColorPrimaries::Rec2020);
        EXPECT_EQ(tf, QuickView::TransferFunction::PQ);
    }

    // 2. Valid Display P3 + HLG
    {
        auto icc = BuildSyntheticIccCicp(12, 18, 0, 1);
        QuickView::ColorPrimaries p = QuickView::ColorPrimaries::Unknown;
        QuickView::TransferFunction tf = QuickView::TransferFunction::Unknown;
        EXPECT_TRUE(QuickView::TryParseIccCicp(icc.data(), icc.size(), &p, &tf));
        EXPECT_EQ(p, QuickView::ColorPrimaries::DisplayP3);
        EXPECT_EQ(tf, QuickView::TransferFunction::HLG);
    }

    // 3. Reject malformed / unsafe inputs
    {
        QuickView::ColorPrimaries p = QuickView::ColorPrimaries::Unknown;
        QuickView::TransferFunction tf = QuickView::TransferFunction::Unknown;
        EXPECT_FALSE(QuickView::TryParseIccCicp(nullptr, 156, &p, &tf));

        auto icc = BuildSyntheticIccCicp(9, 16, 0, 1);
        EXPECT_FALSE(QuickView::TryParseIccCicp(icc.data(), 100, &p, &tf));

        auto badSig = BuildSyntheticIccCicp(9, 16, 0, 1, false);
        EXPECT_FALSE(QuickView::TryParseIccCicp(badSig.data(), badSig.size(), &p, &tf));

        auto overflowCount = BuildSyntheticIccCicp(9, 16, 0, 1, true, 0x40000000u);
        EXPECT_FALSE(QuickView::TryParseIccCicp(overflowCount.data(), overflowCount.size(), &p, &tf));

        auto truncatedSize = BuildSyntheticIccCicp(9, 16, 0, 1, true, 1, 9999u);
        EXPECT_FALSE(QuickView::TryParseIccCicp(truncatedSize.data(), truncatedSize.size(), &p, &tf));

        auto nonZeroMatrix = BuildSyntheticIccCicp(9, 16, 9, 1);
        EXPECT_FALSE(QuickView::TryParseIccCicp(nonZeroMatrix.data(), nonZeroMatrix.size(), &p, &tf));

        auto limitedRange = BuildSyntheticIccCicp(9, 16, 0, 0);
        EXPECT_FALSE(QuickView::TryParseIccCicp(limitedRange.data(), limitedRange.size(), &p, &tf));
    }
}

TEST(HdrPipelineTests, Wuffs16BitPngAndCicp) {
    // 1. Synthetic chunk priority: cICP > ICC cicp > sRGB, plus cLLi/mDCv
    {
        auto icc = BuildSyntheticIccCicp(12, 18, 0, 1); // P3 HLG
        auto topWins = BuildSynthetic1x1Png(true, 9, 16, &icc, true, true, 100000000u, 4000000u, true, 10000000u, 1u);
        uint32_t w = 0, h = 0;
        std::vector<uint8_t> px;
        WuffsLoader::WuffsImageInfo info;
        ASSERT_TRUE(WuffsLoader::DecodePNG(topWins.data(), topWins.size(), &w, &h, px, {}, &info));
        EXPECT_EQ(w, 1u);
        EXPECT_EQ(h, 1u);
        EXPECT_EQ(info.primaries, QuickView::ColorPrimaries::Rec2020);
        EXPECT_EQ(info.transfer, QuickView::TransferFunction::PQ);
        EXPECT_TRUE(info.hasNitsMetadata);
        EXPECT_NEAR(info.maxCLLNits, 10000.0f, 0.01f);
        EXPECT_NEAR(info.maxFALLNits, 400.0f, 0.01f);
        EXPECT_NEAR(info.masteringMaxNits, 1000.0f, 0.01f);
        EXPECT_NEAR(info.masteringMinNits, 0.0001f, 1e-5f);

        auto iccWins = BuildSynthetic1x1Png(false, 0, 0, &icc, true, false, 0, 0, false, 0, 0);
        info = {};
        ASSERT_TRUE(WuffsLoader::DecodePNG(iccWins.data(), iccWins.size(), &w, &h, px, {}, &info));
        EXPECT_EQ(info.primaries, QuickView::ColorPrimaries::DisplayP3);
        EXPECT_EQ(info.transfer, QuickView::TransferFunction::HLG);

        auto srgbOnly = BuildSynthetic1x1Png(false, 0, 0, nullptr, true, false, 0, 0, false, 0, 0);
        info = {};
        ASSERT_TRUE(WuffsLoader::DecodePNG(srgbOnly.data(), srgbOnly.size(), &w, &h, px, {}, &info));
        EXPECT_EQ(info.primaries, QuickView::ColorPrimaries::SRGB);
        EXPECT_EQ(info.transfer, QuickView::TransferFunction::SRGB);
    }

    // 2. Real 16-bit HDR PNGs if present in workspace
    const auto hellraiser = FindHdrTestAsset(L"Hellraiser-Win64-Shipping_2026-10-09_00-07-23_7.png");
    if (!hellraiser.empty()) {
        std::vector<uint8_t> bytes;
        ASSERT_TRUE(ReadFileBytes(hellraiser, &bytes));
        uint32_t w = 0, h = 0;
        std::vector<uint8_t> wuffsPixels;
        WuffsLoader::WuffsImageInfo info;
        ASSERT_TRUE(WuffsLoader::DecodePNG(bytes.data(), bytes.size(), &w, &h, wuffsPixels, {}, &info));
        EXPECT_EQ(w, 3840u);
        EXPECT_EQ(h, 2160u);
        EXPECT_EQ(info.bitDepth, 16);
        EXPECT_EQ(info.primaries, QuickView::ColorPrimaries::Rec2020);
        EXPECT_EQ(info.transfer, QuickView::TransferFunction::PQ);
        EXPECT_FALSE(info.iccProfile.empty());
        ASSERT_EQ(wuffsPixels.size(), static_cast<size_t>(w) * static_cast<size_t>(h) * 8u);

        // Cross-check Wuffs 16-bit RGBA decode byte-for-byte against WIC 64bppRGBA reference
        const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        {
            ComPtr<IWICImagingFactory> wicFactory;
            ASSERT_HRESULT_SUCCEEDED(CoCreateInstance(
                CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wicFactory)));
            ComPtr<IWICStream> stream;
            ASSERT_HRESULT_SUCCEEDED(wicFactory->CreateStream(&stream));
            ASSERT_HRESULT_SUCCEEDED(stream->InitializeFromMemory(bytes.data(), static_cast<DWORD>(bytes.size())));
            ComPtr<IWICBitmapDecoder> decoder;
            ASSERT_HRESULT_SUCCEEDED(wicFactory->CreateDecoderFromStream(
                stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder));
            ComPtr<IWICBitmapFrameDecode> frame;
            ASSERT_HRESULT_SUCCEEDED(decoder->GetFrame(0, &frame));
            ComPtr<IWICFormatConverter> converter;
            ASSERT_HRESULT_SUCCEEDED(wicFactory->CreateFormatConverter(&converter));
            ASSERT_HRESULT_SUCCEEDED(converter->Initialize(
                frame.Get(), GUID_WICPixelFormat64bppRGBA, WICBitmapDitherTypeNone,
                nullptr, 0.0, WICBitmapPaletteTypeCustom));

            std::vector<uint8_t> wicPixels(static_cast<size_t>(w) * static_cast<size_t>(h) * 8u);
            ASSERT_HRESULT_SUCCEEDED(converter->CopyPixels(
                nullptr, w * 8u, static_cast<UINT>(wicPixels.size()), wicPixels.data()));

            // Compare sampled rows and strided pixels across corners, middle, and highlight region
            const UINT sampleYs[] = {0, 1, 540, 1080, 1620, 2159};
            for (UINT y : sampleYs) {
                for (UINT x = 0; x < w; x += 97) {
                    const size_t off = (static_cast<size_t>(y) * w + x) * 8u;
                    EXPECT_EQ(std::memcmp(wuffsPixels.data() + off, wicPixels.data() + off, 8u), 0)
                        << "Mismatch at (" << x << ", " << y << ")";
                }
            }
        }
        if (SUCCEEDED(hrCo)) CoUninitialize();
    }

    const auto sunrisePq = FindHdrTestAsset(L"sunrise-PQ.png");
    if (!sunrisePq.empty()) {
        std::vector<uint8_t> bytes;
        ASSERT_TRUE(ReadFileBytes(sunrisePq, &bytes));
        uint32_t w = 0, h = 0;
        std::vector<uint8_t> px;
        WuffsLoader::WuffsImageInfo info;
        ASSERT_TRUE(WuffsLoader::DecodePNG(bytes.data(), bytes.size(), &w, &h, px, {}, &info));
        EXPECT_EQ(w, 944u);
        EXPECT_EQ(h, 944u);
        EXPECT_EQ(info.bitDepth, 16);
        EXPECT_EQ(info.primaries, QuickView::ColorPrimaries::Rec2020);
        EXPECT_EQ(info.transfer, QuickView::TransferFunction::PQ);
        EXPECT_TRUE(info.hasNitsMetadata);
        EXPECT_NEAR(info.maxCLLNits, 10000.0f, 1.0f);
    }

    const auto hlgPattern = FindHdrTestAsset(L"test_pattern-HLG.png");
    if (!hlgPattern.empty()) {
        std::vector<uint8_t> bytes;
        ASSERT_TRUE(ReadFileBytes(hlgPattern, &bytes));
        uint32_t w = 0, h = 0;
        std::vector<uint8_t> px;
        WuffsLoader::WuffsImageInfo info;
        ASSERT_TRUE(WuffsLoader::DecodePNG(bytes.data(), bytes.size(), &w, &h, px, {}, &info));
        EXPECT_EQ(w, 1024u);
        EXPECT_EQ(h, 1024u);
        EXPECT_EQ(info.bitDepth, 16);
        EXPECT_EQ(info.primaries, QuickView::ColorPrimaries::Rec2020);
        EXPECT_EQ(info.transfer, QuickView::TransferFunction::HLG);
    }
}

TEST(HdrPipelineTests, JpegGainMapMarkerProbe) {
    // Malformed and truncated inputs
    EXPECT_FALSE(QuickView::HasJpegGainMapMarkers(nullptr, 100));
    const uint8_t notJpeg[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A};
    EXPECT_FALSE(QuickView::HasJpegGainMapMarkers(notJpeg, sizeof(notJpeg)));
    const uint8_t truncJpeg[] = {0xFF, 0xD8, 0xFF, 0xE1, 0x00, 0x10, 'h', 't'};
    EXPECT_FALSE(QuickView::HasJpegGainMapMarkers(truncJpeg, sizeof(truncJpeg)));
    const uint8_t badSegLen[] = {0xFF, 0xD8, 0xFF, 0xE1, 0x00, 0x01, 0x00};
    EXPECT_FALSE(QuickView::HasJpegGainMapMarkers(badSegLen, sizeof(badSegLen)));

    // Fake marker string after SOS must not be treated as a real APP segment
    const std::string afterSos = std::string("\xFF\xD8\xFF\xDA\x00\x02", 6) + "http://ns.adobe.com/xap/1.0/\0hdrgm:Version";
    EXPECT_FALSE(QuickView::HasJpegGainMapMarkers(
        reinterpret_cast<const uint8_t*>(afterSos.data()), afterSos.size()));

    // Valid synthetic APP1 XMP segment before SOS
    const std::string xmpPayload = std::string("http://ns.adobe.com/xap/1.0/\0<hdrgm:Version=\"1.0\"/>", 49);
    std::vector<uint8_t> xmpJpeg = {0xFF, 0xD8, 0xFF, 0xE1};
    const uint16_t segLen = static_cast<uint16_t>(xmpPayload.size() + 2);
    xmpJpeg.push_back(static_cast<uint8_t>(segLen >> 8));
    xmpJpeg.push_back(static_cast<uint8_t>(segLen & 0xFF));
    xmpJpeg.insert(xmpJpeg.end(), xmpPayload.begin(), xmpPayload.end());
    xmpJpeg.push_back(0xFF);
    xmpJpeg.push_back(0xDA);
    EXPECT_TRUE(QuickView::HasJpegGainMapMarkers(xmpJpeg.data(), xmpJpeg.size()));

    // Real Ultra HDR / HDR JPEG assets if present in workspace
    const auto uhdr1 = FindHdrTestAsset(L"ultra_hdr.jpg");
    if (!uhdr1.empty()) {
        std::vector<uint8_t> bytes;
        ASSERT_TRUE(ReadFileBytes(uhdr1, &bytes));
        EXPECT_TRUE(QuickView::HasJpegGainMapMarkers(bytes.data(), bytes.size()));
    }
    const auto uhdr2 = FindHdrTestAsset(L"Ultra_HDR_Samples_Originals_05.jpg");
    if (!uhdr2.empty()) {
        std::vector<uint8_t> bytes;
        ASSERT_TRUE(ReadFileBytes(uhdr2, &bytes));
        EXPECT_TRUE(QuickView::HasJpegGainMapMarkers(bytes.data(), bytes.size()));
    }
    const auto pqJpg = FindHdrTestAsset(L"sunrise-PQ.jpg");
    if (!pqJpg.empty()) {
        std::vector<uint8_t> bytes;
        ASSERT_TRUE(ReadFileBytes(pqJpg, &bytes));
        EXPECT_FALSE(QuickView::HasJpegGainMapMarkers(bytes.data(), bytes.size()));
    }
}

TEST(HdrPipelineTests, ComputeEngineGainMapAndFsrNoCrash) {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ASSERT_HRESULT_SUCCEEDED(CreateD3D11DeviceWithWarpFallback(&device, &context));

    QuickView::ComputeEngine compute;
    ASSERT_HRESULT_SUCCEEDED(compute.Initialize(device.Get()));

    // 1. ComposeGainMap: 4x4 white SDR BGRA8 + 2x2 R8 gain map (row0=255, row1=0)
    {
        std::vector<uint8_t> sdrPixels(4 * 4 * 4, 255);
        std::array<uint8_t, 4> gainPixels = {255, 255, 0, 0};

        QuickView::GpuShaderPayload payload = {};
        payload.gainMapMin[0] = payload.gainMapMin[1] = payload.gainMapMin[2] = 0.0f;
        payload.gainMapMax[0] = payload.gainMapMax[1] = payload.gainMapMax[2] = 2.0f;
        payload.gamma[0] = payload.gamma[1] = payload.gamma[2] = 1.0f;
        payload.hdrCapacityMin = 0.0f;
        payload.hdrCapacityMax = 2.0f;
        payload.targetHeadroom = 2.0f;
        payload.baseIsHdr = 0.0f;
        payload.sdrWidth = 4;
        payload.sdrHeight = 4;

        ComPtr<ID3D11Texture2D> outTex;
        ASSERT_HRESULT_SUCCEEDED(compute.ComposeGainMap(
            sdrPixels.data(), 4, 4, 4 * 4,
            QuickView::PixelFormat::BGRA8888,
            gainPixels.data(), 2, 2, 2,
            payload, &outTex));
        ASSERT_NE(outTex, nullptr);

        std::vector<uint16_t> fp16;
        UINT w = 0, h = 0;
        ASSERT_TRUE(ReadbackFp16Texture(device.Get(), context.Get(), outTex.Get(), &fp16, &w, &h));
        ASSERT_EQ(w, 4u);
        ASSERT_EQ(h, 4u);

        // Top row samples gain = 1.0 -> 2^2 = 4.0 linear SDR white
        EXPECT_NEAR(HalfToFloat(fp16[0]), 4.0f, 0.08f);
        EXPECT_NEAR(HalfToFloat(fp16[1]), 4.0f, 0.08f);
        EXPECT_NEAR(HalfToFloat(fp16[2]), 4.0f, 0.08f);
        // Bottom row samples gain = 0.0 -> 2^0 = 1.0 linear SDR white
        const size_t row3 = 3u * 4u * 4u;
        EXPECT_NEAR(HalfToFloat(fp16[row3 + 0]), 1.0f, 0.03f);
        EXPECT_NEAR(HalfToFloat(fp16[row3 + 1]), 1.0f, 0.03f);
        EXPECT_NEAR(HalfToFloat(fp16[row3 + 2]), 1.0f, 0.03f);
    }

    // 2. ExecuteFsr1Upscale: verify null constant-buffer clear fix does not crash
    {
        std::vector<uint8_t> srcPixels(8 * 8 * 4, 180);
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = 8;
        desc.Height = 8;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA initData = {srcPixels.data(), 8 * 4, 0};
        ComPtr<ID3D11Texture2D> srcTex;
        ASSERT_HRESULT_SUCCEEDED(device->CreateTexture2D(&desc, &initData, &srcTex));

        ComPtr<ID3D11Texture2D> upscaledTex;
        ASSERT_HRESULT_SUCCEEDED(compute.ExecuteFsr1Upscale(srcTex.Get(), 8, 8, 16, 16, 0.5f, &upscaledTex));
        ASSERT_NE(upscaledTex, nullptr);

        D3D11_TEXTURE2D_DESC outDesc = {};
        upscaledTex->GetDesc(&outDesc);
        EXPECT_EQ(outDesc.Width, 16u);
        EXPECT_EQ(outDesc.Height, 16u);
    }
}

TEST(HdrPipelineTests, ComputeEngineToneMapHdrWideGamutAndAlpha) {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ASSERT_HRESULT_SUCCEEDED(CreateD3D11DeviceWithWarpFallback(&device, &context));

    QuickView::ComputeEngine compute;
    ASSERT_HRESULT_SUCCEEDED(compute.Initialize(device.Get()));

    auto makeIdentitySettings = [](QuickView::TransferFunction tf, uint32_t mode) {
        QuickView::ToneMapSettings s = {};
        s.contentPeakScRgb = 12.5f;
        s.displayPeakScRgb = 12.5f;
        s.paperWhiteScRgb = 1.0f;
        s.exposure = 1.0f;
        s.exposureGain = 1.0f;
        s.mode = mode;
        s.isHdrOutput = 1;
        s.realHardwarePeakScRgb = 12.5f;
        s.transferFunction = static_cast<uint32_t>(tf);
        s.colorMatrix[0] = s.colorMatrix[5] = s.colorMatrix[10] = s.colorMatrix[15] = 1.0f;
        return s;
    };

    // 1. Rec.2020 pure green PQ with Rec.2020->scRGB matrix preserves negative R/B channels
    {
        const ColorMath::ColorMatrix3 rec2020ToXyz = ColorMath::GetRgbToXyzMatrix(QuickView::ColorPrimaries::Rec2020);
        const ColorMath::ColorMatrix3 xyzToSrgb = ColorMath::GetXyzToRgbMatrix(QuickView::ColorPrimaries::SRGB);
        const ColorMath::ColorMatrix3 gamut = ColorMath::MultiplyColorMatrices(xyzToSrgb, rec2020ToXyz);

        auto s = makeIdentitySettings(QuickView::TransferFunction::PQ, 1);
        s.colorMatrix[0] = gamut.m[0][0]; s.colorMatrix[1] = gamut.m[0][1]; s.colorMatrix[2] = gamut.m[0][2];
        s.colorMatrix[4] = gamut.m[1][0]; s.colorMatrix[5] = gamut.m[1][1]; s.colorMatrix[6] = gamut.m[1][2];
        s.colorMatrix[8] = gamut.m[2][0]; s.colorMatrix[9] = gamut.m[2][1]; s.colorMatrix[10] = gamut.m[2][2];

        const float pq400 = NitsToPqSignal(400.0f);
        const uint16_t pqG = static_cast<uint16_t>(std::lround(pq400 * 65535.0f));
        const uint16_t pixel[4] = {0, pqG, 0, 65535};

        ComPtr<ID3D11Texture2D> outTex;
        ASSERT_HRESULT_SUCCEEDED(compute.ToneMapHdrToHdr(
            reinterpret_cast<const uint8_t*>(pixel), 1, 1, 8, s, &outTex,
            QuickView::PixelFormat::R16G16B16A16_UNORM));
        std::vector<uint16_t> fp16;
        ASSERT_TRUE(ReadbackFp16Texture(device.Get(), context.Get(), outTex.Get(), &fp16));
        const float r = HalfToFloat(fp16[0]);
        const float g = HalfToFloat(fp16[1]);
        const float b = HalfToFloat(fp16[2]);
        EXPECT_LT(r, -0.5f); // Rec.2020 green projects to negative red in scRGB
        const float y709 = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        EXPECT_NEAR(y709 * 80.0f, 0.6780f * 400.0f, 5.0f);
    }

    // 2. Alpha premultiplication in linear scRGB space
    {
        const auto s = makeIdentitySettings(QuickView::TransferFunction::Linear, 1);
        const float srcPixels[3 * 4] = {
            2.0f, 4.0f, 1.0f, 0.0f,
            2.0f, 4.0f, 1.0f, 0.5f,
            2.0f, 4.0f, 1.0f, 1.0f,
        };
        ComPtr<ID3D11Texture2D> outTex;
        ASSERT_HRESULT_SUCCEEDED(compute.ToneMapHdrToHdr(
            reinterpret_cast<const uint8_t*>(srcPixels), 3, 1, 3 * 16, s, &outTex,
            QuickView::PixelFormat::R32G32B32A32_FLOAT));
        std::vector<uint16_t> fp16;
        ASSERT_TRUE(ReadbackFp16Texture(device.Get(), context.Get(), outTex.Get(), &fp16));

        // Pixel 0: alpha=0 -> (0,0,0,0)
        EXPECT_NEAR(HalfToFloat(fp16[0]), 0.0f, 1e-4f);
        EXPECT_NEAR(HalfToFloat(fp16[1]), 0.0f, 1e-4f);
        EXPECT_NEAR(HalfToFloat(fp16[3]), 0.0f, 1e-4f);
        // Pixel 1: alpha=0.5 -> (1.0, 2.0, 0.5, 0.5)
        EXPECT_NEAR(HalfToFloat(fp16[4]), 1.0f, 0.02f);
        EXPECT_NEAR(HalfToFloat(fp16[5]), 2.0f, 0.02f);
        EXPECT_NEAR(HalfToFloat(fp16[6]), 0.5f, 0.02f);
        EXPECT_NEAR(HalfToFloat(fp16[7]), 0.5f, 0.02f);
        // Pixel 2: alpha=1.0 -> (2.0, 4.0, 1.0, 1.0)
        EXPECT_NEAR(HalfToFloat(fp16[8]), 2.0f, 0.02f);
        EXPECT_NEAR(HalfToFloat(fp16[9]), 4.0f, 0.02f);
        EXPECT_NEAR(HalfToFloat(fp16[11]), 1.0f, 0.02f);
    }

    // 3. PQ luminance verification (0.6 nits median & 469 nits peak on 1000-nit target)
    {
        auto s = makeIdentitySettings(QuickView::TransferFunction::PQ, 1);
        s.contentPeakScRgb = 469.0f / 80.0f;
        const uint16_t pq06 = static_cast<uint16_t>(std::lround(NitsToPqSignal(0.6f) * 65535.0f));
        const uint16_t pq469 = static_cast<uint16_t>(std::lround(NitsToPqSignal(469.0f) * 65535.0f));
        const uint16_t pixels[8] = {pq06, pq06, pq06, 65535, pq469, pq469, pq469, 65535};
        ComPtr<ID3D11Texture2D> outTex;
        ASSERT_HRESULT_SUCCEEDED(compute.ToneMapHdrToHdr(
            reinterpret_cast<const uint8_t*>(pixels), 2, 1, 16, s, &outTex,
            QuickView::PixelFormat::R16G16B16A16_UNORM));
        std::vector<uint16_t> fp16;
        ASSERT_TRUE(ReadbackFp16Texture(device.Get(), context.Get(), outTex.Get(), &fp16));
        EXPECT_NEAR(HalfToFloat(fp16[0]) * 80.0f, 0.6f, 0.05f);
        EXPECT_NEAR(HalfToFloat(fp16[4]) * 80.0f, 469.0f, 5.0f);
    }

    // 4. GainMap relative linear scaling: 240-nit SDR white -> exposureGain = 3.0
    {
        auto s = makeIdentitySettings(QuickView::TransferFunction::Linear, 1);
        s.exposureGain = 240.0f / 80.0f;
        s.contentPeakScRgb = 12.0f;
        const float pixels[8] = {
            1.0f, 1.0f, 1.0f, 1.0f,
            4.0f, 4.0f, 4.0f, 1.0f,
        };
        ComPtr<ID3D11Texture2D> outTex;
        ASSERT_HRESULT_SUCCEEDED(compute.ToneMapHdrToHdr(
            reinterpret_cast<const uint8_t*>(pixels), 2, 1, 32, s, &outTex,
            QuickView::PixelFormat::R32G32B32A32_FLOAT));
        std::vector<uint16_t> fp16;
        ASSERT_TRUE(ReadbackFp16Texture(device.Get(), context.Get(), outTex.Get(), &fp16));
        EXPECT_NEAR(HalfToFloat(fp16[0]), 3.0f, 0.05f);  // 240 nits
        EXPECT_NEAR(HalfToFloat(fp16[4]), 12.0f, 0.1f);  // 960 nits
    }
}

TEST(HdrPipelineTests, D2D1Fp16PrecisionNoClamp) {
    ComPtr<ID3D11Device> d3dDevice;
    ComPtr<ID3D11DeviceContext> d3dContext;
    ASSERT_HRESULT_SUCCEEDED(CreateD3D11DeviceWithWarpFallback(&d3dDevice, &d3dContext));

    ComPtr<IDXGIDevice> dxgiDevice;
    ASSERT_HRESULT_SUCCEEDED(d3dDevice.As(&dxgiDevice));

    ComPtr<ID2D1Factory1> d2dFactory;
    ASSERT_HRESULT_SUCCEEDED(D2D1CreateFactory(
        D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr, &d2dFactory));

    ComPtr<ID2D1Device> d2dDevice;
    ASSERT_HRESULT_SUCCEEDED(d2dFactory->CreateDevice(dxgiDevice.Get(), &d2dDevice));

    ComPtr<ID2D1DeviceContext> d2dCtx;
    ASSERT_HRESULT_SUCCEEDED(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dCtx));
    ASSERT_TRUE(d2dCtx->IsBufferPrecisionSupported(D2D1_BUFFER_PRECISION_16BPC_FLOAT));

    // Create 16x16 FP16 source bitmap with values > 1.0 and < 0.0
    const UINT srcSize = 16;
    std::vector<uint16_t> srcHalf(srcSize * srcSize * 4u);
    const uint16_t kR = FloatToHalf(4.5f);
    const uint16_t kG = FloatToHalf(-0.35f);
    const uint16_t kB = FloatToHalf(2.25f);
    const uint16_t kA = FloatToHalf(1.0f);
    for (size_t i = 0; i < srcSize * srcSize; ++i) {
        srcHalf[i * 4u + 0u] = kR;
        srcHalf[i * 4u + 1u] = kG;
        srcHalf[i * 4u + 2u] = kB;
        srcHalf[i * 4u + 3u] = kA;
    }

    D2D1_BITMAP_PROPERTIES1 srcProps = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_R16G16B16A16_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    ComPtr<ID2D1Bitmap1> srcBitmap;
    ASSERT_HRESULT_SUCCEEDED(d2dCtx->CreateBitmap(
        D2D1::SizeU(srcSize, srcSize), srcHalf.data(), srcSize * 8u, &srcProps, &srcBitmap));

    d2dCtx->SetDpi(96.0f, 96.0f);
    d2dCtx->SetUnitMode(D2D1_UNIT_MODE_PIXELS);

    D2D1_RENDERING_CONTROLS controls = {};
    d2dCtx->GetRenderingControls(&controls);
    controls.bufferPrecision = D2D1_BUFFER_PRECISION_16BPC_FLOAT;
    d2dCtx->SetRenderingControls(&controls);

    const float kScales[] = {0.6f, 1.0f, 1.5f};
    for (float scale : kScales) {
        const UINT dstSize = static_cast<UINT>(std::lround(static_cast<float>(srcSize) * scale));
        D3D11_TEXTURE2D_DESC rtDesc = {};
        rtDesc.Width = dstSize;
        rtDesc.Height = dstSize;
        rtDesc.MipLevels = 1;
        rtDesc.ArraySize = 1;
        rtDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        rtDesc.SampleDesc.Count = 1;
        rtDesc.Usage = D3D11_USAGE_DEFAULT;
        rtDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        ComPtr<ID3D11Texture2D> rtTex;
        ASSERT_HRESULT_SUCCEEDED(d3dDevice->CreateTexture2D(&rtDesc, nullptr, &rtTex));
        ComPtr<IDXGISurface> rtSurface;
        ASSERT_HRESULT_SUCCEEDED(rtTex.As(&rtSurface));

        D2D1_BITMAP_PROPERTIES1 rtProps = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_R16G16B16A16_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);
        ComPtr<ID2D1Bitmap1> rtBitmap;
        ASSERT_HRESULT_SUCCEEDED(d2dCtx->CreateBitmapFromDxgiSurface(rtSurface.Get(), &rtProps, &rtBitmap));

        // Test 1: CommandList + DrawBitmap (mirrors CompositionEngine pipeline)
        ComPtr<ID2D1CommandList> cmdList;
        ASSERT_HRESULT_SUCCEEDED(d2dCtx->CreateCommandList(&cmdList));
        d2dCtx->SetTarget(cmdList.Get());
        d2dCtx->BeginDraw();
        d2dCtx->Clear(D2D1::ColorF(0, 0, 0, 0));
        const D2D1_RECT_F dstRect = D2D1::RectF(0.0f, 0.0f, static_cast<float>(dstSize), static_cast<float>(dstSize));
        const D2D1_RECT_F srcRect = D2D1::RectF(0.0f, 0.0f, static_cast<float>(srcSize), static_cast<float>(srcSize));
        d2dCtx->DrawBitmap(srcBitmap.Get(), &dstRect, 1.0f, D2D1_INTERPOLATION_MODE_LINEAR, &srcRect);
        ASSERT_HRESULT_SUCCEEDED(d2dCtx->EndDraw());
        ASSERT_HRESULT_SUCCEEDED(cmdList->Close());

        d2dCtx->SetTarget(rtBitmap.Get());
        d2dCtx->BeginDraw();
        d2dCtx->Clear(D2D1::ColorF(0, 0, 0, 0));
        d2dCtx->DrawImage(cmdList.Get());
        ASSERT_HRESULT_SUCCEEDED(d2dCtx->EndDraw());
        d2dCtx->SetTarget(nullptr);

        std::vector<uint16_t> fp16;
        ASSERT_TRUE(ReadbackFp16Texture(d3dDevice.Get(), d3dContext.Get(), rtTex.Get(), &fp16));
        const size_t centerIdx = (static_cast<size_t>(dstSize / 2u) * dstSize + (dstSize / 2u)) * 4u;
        EXPECT_GT(HalfToFloat(fp16[centerIdx + 0u]), 4.0f) << "Scale=" << scale;
        EXPECT_LT(HalfToFloat(fp16[centerIdx + 1u]), -0.2f) << "Scale=" << scale;
        EXPECT_NEAR(HalfToFloat(fp16[centerIdx + 0u]), 4.5f, 0.05f) << "Scale=" << scale;
        EXPECT_NEAR(HalfToFloat(fp16[centerIdx + 1u]), -0.35f, 0.05f) << "Scale=" << scale;

        // Test 2: D2D1Scale effect under 16BPC_FLOAT precision (verifies effect graph no-clamp)
        ComPtr<ID2D1Effect> scaleEffect;
        ASSERT_HRESULT_SUCCEEDED(d2dCtx->CreateEffect(CLSID_D2D1Scale, &scaleEffect));
        scaleEffect->SetInput(0, srcBitmap.Get());
        scaleEffect->SetValue(D2D1_SCALE_PROP_SCALE, D2D1::Vector2F(scale, scale));
        scaleEffect->SetValue(D2D1_SCALE_PROP_INTERPOLATION_MODE, D2D1_SCALE_INTERPOLATION_MODE_LINEAR);

        d2dCtx->SetTarget(rtBitmap.Get());
        d2dCtx->BeginDraw();
        d2dCtx->Clear(D2D1::ColorF(0, 0, 0, 0));
        d2dCtx->DrawImage(scaleEffect.Get());
        ASSERT_HRESULT_SUCCEEDED(d2dCtx->EndDraw());
        d2dCtx->SetTarget(nullptr);

        ASSERT_TRUE(ReadbackFp16Texture(d3dDevice.Get(), d3dContext.Get(), rtTex.Get(), &fp16));
        EXPECT_NEAR(HalfToFloat(fp16[centerIdx + 0u]), 4.5f, 0.05f) << "Effect Scale=" << scale;
        EXPECT_NEAR(HalfToFloat(fp16[centerIdx + 1u]), -0.35f, 0.05f) << "Effect Scale=" << scale;
    }
}

TEST(HdrPipelineTests, LinearPqAndHlgPeakEstimationNoJensenError) {
    // 1. 16-bit UNORM PQ pure Rec.2020 Red at 1000 nits -> Y = 0.2627 * 1000 = 262.7 nits
    {
        const uint16_t pq1000 = static_cast<uint16_t>(std::lround(NitsToPqSignal(1000.0f) * 65535.0f));
        uint16_t px[4] = {pq1000, 0, 0, 65535};
        QuickView::RawImageFrame frame;
        frame.pixels = reinterpret_cast<uint8_t*>(px);
        frame.width = 1;
        frame.height = 1;
        frame.stride = 8;
        frame.format = QuickView::PixelFormat::R16G16B16A16_UNORM;

        EXPECT_NEAR(
            ColorMath::EstimateEncodedFramePeakScRgb(frame, QuickView::TransferFunction::PQ) * 80.0f,
            262.7f, 3.0f);
        EXPECT_NEAR(
            ColorMath::EstimateEncodedFrameAverageScRgb(frame, QuickView::TransferFunction::PQ) * 80.0f,
            262.7f, 3.0f);
    }

    // 2. 16-bit UNORM HLG white (1000 nits) and pure Rec.2020 Red (0.2627^1.2 * 1000 = 201.07 nits)
    {
        uint16_t pxWhite[4] = {65535, 65535, 65535, 65535};
        QuickView::RawImageFrame frameWhite;
        frameWhite.pixels = reinterpret_cast<uint8_t*>(pxWhite);
        frameWhite.width = 1;
        frameWhite.height = 1;
        frameWhite.stride = 8;
        frameWhite.format = QuickView::PixelFormat::R16G16B16A16_UNORM;
        EXPECT_NEAR(
            ColorMath::EstimateEncodedFramePeakScRgb(frameWhite, QuickView::TransferFunction::HLG) * 80.0f,
            1000.0f, 5.0f);

        uint16_t pxRed[4] = {65535, 0, 0, 65535};
        QuickView::RawImageFrame frameRed;
        frameRed.pixels = reinterpret_cast<uint8_t*>(pxRed);
        frameRed.width = 1;
        frameRed.height = 1;
        frameRed.stride = 8;
        frameRed.format = QuickView::PixelFormat::R16G16B16A16_UNORM;
        EXPECT_NEAR(
            ColorMath::EstimateEncodedFramePeakScRgb(frameRed, QuickView::TransferFunction::HLG) * 80.0f,
            201.07f, 3.0f);
    }

    // 3. Percentile outlier rejection on 16-bit PQ frame (1 hot 10000-nit pixel, 999 100-nit pixels)
    {
        const uint16_t pq100 = static_cast<uint16_t>(std::lround(NitsToPqSignal(100.0f) * 65535.0f));
        std::vector<uint16_t> pixels(1000u * 4u);
        for (size_t i = 0; i < 1000u; ++i) {
            const uint16_t v = (i == 0u) ? 65535u : pq100;
            pixels[i * 4u + 0u] = v;
            pixels[i * 4u + 1u] = v;
            pixels[i * 4u + 2u] = v;
            pixels[i * 4u + 3u] = 65535u;
        }
        QuickView::RawImageFrame frame;
        frame.pixels = reinterpret_cast<uint8_t*>(pixels.data());
        frame.width = 1000;
        frame.height = 1;
        frame.stride = 8000;
        frame.format = QuickView::PixelFormat::R16G16B16A16_UNORM;

        EXPECT_NEAR(
            ColorMath::EstimateEncodedFramePeakScRgb(frame, QuickView::TransferFunction::PQ, 99.0f) * 80.0f,
            100.0f, 3.0f);
    }
}

TEST(HdrPipelineTests, ToneMapHdrToSdrOkLabGamutAndAlpha) {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ASSERT_HRESULT_SUCCEEDED(CreateD3D11DeviceWithWarpFallback(&device, &context));

    QuickView::ComputeEngine compute;
    ASSERT_HRESULT_SUCCEEDED(compute.Initialize(device.Get()));

    const ColorMath::ColorMatrix3 rec2020ToXyz = ColorMath::GetRgbToXyzMatrix(QuickView::ColorPrimaries::Rec2020);
    const ColorMath::ColorMatrix3 xyzToSrgb = ColorMath::GetXyzToRgbMatrix(QuickView::ColorPrimaries::SRGB);
    const ColorMath::ColorMatrix3 gamut = ColorMath::MultiplyColorMatrices(xyzToSrgb, rec2020ToXyz);

    auto makeSettings = [](const ColorMath::ColorMatrix3& mat) {
        QuickView::ToneMapSettings s = {};
        s.contentPeakScRgb = 203.0f / 80.0f;
        s.displayPeakScRgb = 203.0f / 80.0f;
        s.paperWhiteScRgb = 1.0f;
        s.exposure = 1.0f;
        s.exposureGain = 1.0f;
        s.mode = 1; // Colorimetric: isolate OKLab gamut mapping from tone curve
        s.isHdrOutput = 0;
        s.realHardwarePeakScRgb = 1.0f;
        s.transferFunction = static_cast<uint32_t>(QuickView::TransferFunction::Linear);
        s.colorMatrix[0] = mat.m[0][0]; s.colorMatrix[1] = mat.m[0][1]; s.colorMatrix[2] = mat.m[0][2];
        s.colorMatrix[4] = mat.m[1][0]; s.colorMatrix[5] = mat.m[1][1]; s.colorMatrix[6] = mat.m[1][2];
        s.colorMatrix[8] = mat.m[2][0]; s.colorMatrix[9] = mat.m[2][1]; s.colorMatrix[10] = mat.m[2][2];
        s.colorMatrix[15] = 1.0f;
        return s;
    };

    // 1. Out-of-gamut Rec.2020 colors in SDR output must preserve OKLab hue and avoid Gold wash-out
    struct ColorSample { const char* name; float rNits, gNits, bNits; };
    const ColorSample kSamples[] = {
        {"Rec2020 Red 100nit",   100.0f,   0.0f,   0.0f},
        {"Rec2020 Red 203nit",   203.0f,   0.0f,   0.0f},
        {"Rec2020 Green 100nit",   0.0f, 100.0f,   0.0f},
        {"Rec2020 Blue 100nit",    0.0f,   0.0f, 100.0f},
        {"Rec2020 Gold 300nit",  300.0f, 180.0f,  20.0f},
    };
    std::vector<float> srcPixels;
    for (const auto& c : kSamples) {
        srcPixels.push_back(c.rNits / 80.0f);
        srcPixels.push_back(c.gNits / 80.0f);
        srcPixels.push_back(c.bNits / 80.0f);
        srcPixels.push_back(1.0f);
    }

    ComPtr<ID3D11Texture2D> outTex;
    ASSERT_HRESULT_SUCCEEDED(compute.ToneMapHdrToSdr(
        reinterpret_cast<const uint8_t*>(srcPixels.data()), 5, 1, 5 * 16,
        makeSettings(gamut), &outTex, QuickView::PixelFormat::R32G32B32A32_FLOAT));
    std::vector<uint8_t> rgba8;
    ASSERT_TRUE(ReadbackRgba8Texture(device.Get(), context.Get(), outTex.Get(), &rgba8));

    for (size_t i = 0; i < 4u; ++i) {
        const auto& c = kSamples[i];
        const float rNorm = c.rNits / 203.0f;
        const float gNorm = c.gNits / 203.0f;
        const float bNorm = c.bNits / 203.0f;
        const float srcLinR = gamut.m[0][0] * rNorm + gamut.m[0][1] * gNorm + gamut.m[0][2] * bNorm;
        const float srcLinG = gamut.m[1][0] * rNorm + gamut.m[1][1] * gNorm + gamut.m[1][2] * bNorm;
        const float srcLinB = gamut.m[2][0] * rNorm + gamut.m[2][1] * gNorm + gamut.m[2][2] * bNorm;
        const float refHue = LinearSrgbToOkLabHueDeg(srcLinR, srcLinG, srcLinB);

        const float outR = ColorMath::SrgbToLinear(static_cast<float>(rgba8[i * 4u + 0u]) / 255.0f);
        const float outG = ColorMath::SrgbToLinear(static_cast<float>(rgba8[i * 4u + 1u]) / 255.0f);
        const float outB = ColorMath::SrgbToLinear(static_cast<float>(rgba8[i * 4u + 2u]) / 255.0f);
        const float outHue = LinearSrgbToOkLabHueDeg(outR, outG, outB);
        const float dHue = std::remainder(outHue - refHue, 360.0f);
        const float maxAllowedHueErrDeg = (i == 1u) ? 1.8f : 1.0f;
        EXPECT_LT(std::abs(dHue), maxAllowedHueErrDeg) << c.name;
    }

    // Over-bright warm highlight (> 203 nits) must preserve warm golden rendering without salmon-pink B inflation
    EXPECT_EQ(rgba8[4u * 4u + 0u], 255u);
    EXPECT_GE(rgba8[4u * 4u + 1u], 210u);
    EXPECT_LE(rgba8[4u * 4u + 2u], 110u);
    EXPECT_GE(static_cast<int>(rgba8[4u * 4u + 1u]) - static_cast<int>(rgba8[4u * 4u + 2u]), 110);

    // 2. SDR alpha premultiplication (D2D1_ALPHA_MODE_PREMULTIPLIED)
    {
        ColorMath::ColorMatrix3 identity = {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
        const float w = 203.0f / 80.0f;
        const float alphaPixels[3 * 4] = {
            w, w, w, 0.0f,
            w, w, w, 0.5f,
            w, w, w, 1.0f,
        };
        ComPtr<ID3D11Texture2D> alphaTex;
        ASSERT_HRESULT_SUCCEEDED(compute.ToneMapHdrToSdr(
            reinterpret_cast<const uint8_t*>(alphaPixels), 3, 1, 3 * 16,
            makeSettings(identity), &alphaTex, QuickView::PixelFormat::R32G32B32A32_FLOAT));
        std::vector<uint8_t> outAlpha;
        ASSERT_TRUE(ReadbackRgba8Texture(device.Get(), context.Get(), alphaTex.Get(), &outAlpha));

        EXPECT_EQ(outAlpha[0], 0u);
        EXPECT_EQ(outAlpha[3], 0u);
        EXPECT_NEAR(outAlpha[4], 128, 2);
        EXPECT_NEAR(outAlpha[7], 128, 1);
        EXPECT_EQ(outAlpha[8], 255u);
        EXPECT_EQ(outAlpha[11], 255u);
    }
}

TEST(HdrPipelineTests, ToneMapHdrToSdrSunHaloGoldenNoPinkShift) {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ASSERT_HRESULT_SUCCEEDED(CreateD3D11DeviceWithWarpFallback(&device, &context));

    QuickView::ComputeEngine compute;
    ASSERT_HRESULT_SUCCEEDED(compute.Initialize(device.Get()));

    const ColorMath::ColorMatrix3 rec2020ToXyz = ColorMath::GetRgbToXyzMatrix(QuickView::ColorPrimaries::Rec2020);
    const ColorMath::ColorMatrix3 xyzToSrgb = ColorMath::GetXyzToRgbMatrix(QuickView::ColorPrimaries::SRGB);
    const ColorMath::ColorMatrix3 rec2020ToSrgb = ColorMath::MultiplyColorMatrices(xyzToSrgb, rec2020ToXyz);

    auto makeSettings = [](uint32_t mode, const ColorMath::ColorMatrix3& mat) {
        QuickView::ToneMapSettings s = {};
        s.contentPeakScRgb = 1525.0f / 80.0f;
        s.displayPeakScRgb = 203.0f / 80.0f;
        s.paperWhiteScRgb = 1.0f;
        s.exposure = 1.0f;
        s.exposureGain = 1.0f;
        s.mode = mode;
        s.isHdrOutput = 0;
        s.realHardwarePeakScRgb = 1.0f;
        s.transferFunction = static_cast<uint32_t>(QuickView::TransferFunction::Linear);
        s.desatThreshold = 0.18f;
        s.desatStrength = 0.75f;
        s.colorMatrix[0] = mat.m[0][0]; s.colorMatrix[1] = mat.m[0][1]; s.colorMatrix[2] = mat.m[0][2];
        s.colorMatrix[4] = mat.m[1][0]; s.colorMatrix[5] = mat.m[1][1]; s.colorMatrix[6] = mat.m[1][2];
        s.colorMatrix[8] = mat.m[2][0]; s.colorMatrix[9] = mat.m[2][1]; s.colorMatrix[10] = mat.m[2][2];
        s.colorMatrix[15] = 1.0f;
        return s;
    };

    // Sub-test A: Real sunrise-PQ Rec.2020 sun-halo clouds via BT.2390 (mode = 2) + decoupled roll-off
    {
        const float haloPixels[3 * 4] = {
            1170.0f / 80.0f,  539.0f / 80.0f, 134.0f / 80.0f, 1.0f, // SunHalo1
            1007.0f / 80.0f,  548.0f / 80.0f, 214.0f / 80.0f, 1.0f, // SunHalo2
            1543.0f / 80.0f, 1409.0f / 80.0f, 455.0f / 80.0f, 1.0f, // SunCore
        };
        ComPtr<ID3D11Texture2D> outTex;
        ASSERT_HRESULT_SUCCEEDED(compute.ToneMapHdrToSdr(
            reinterpret_cast<const uint8_t*>(haloPixels), 3, 1, 3 * 16,
            makeSettings(2u, rec2020ToSrgb), &outTex, QuickView::PixelFormat::R32G32B32A32_FLOAT));
        std::vector<uint8_t> rgba8;
        ASSERT_TRUE(ReadbackRgba8Texture(device.Get(), context.Get(), outTex.Get(), &rgba8));

        // SunHalo1 under BT.2390 (mapped to ~198.9 nits, 2.33x peak): warm golden highlight (not pink washout)
        EXPECT_EQ(rgba8[0], 255u);
        EXPECT_GE(rgba8[1], 195u);
        EXPECT_LE(rgba8[2], 155u);
        EXPECT_GE(static_cast<int>(rgba8[1]) - static_cast<int>(rgba8[2]), 55);

        // SunHalo2 under BT.2390: warm golden highlight
        EXPECT_GE(rgba8[4], 250u);
        EXPECT_GE(rgba8[5], 200u);
        EXPECT_LE(rgba8[6], 165u);
        EXPECT_GE(static_cast<int>(rgba8[5]) - static_cast<int>(rgba8[6]), 50);

        // SunCore: bright warm golden core
        EXPECT_EQ(rgba8[8], 255u);
        EXPECT_GE(rgba8[9], 240u);
        EXPECT_LE(rgba8[10], 185u);
    }

    // Sub-test B: Colorimetric mode = 1, identity matrix -> exact white, path-to-white bloom, and Spline sun-halo regime
    {
        const ColorMath::ColorMatrix3 identity = {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
        const float w = 203.0f / 80.0f;
        const float neonPixels[5 * 4] = {
            w,         w,         w,        1.0f, // ExactWhite
            6.0f * w,  0.30f * w, 1.60f * w, 1.0f, // BlazingNeon
            3.50f * w, 0.80f * w, 0.02f * w, 1.0f, // Fire4000
            1.62f * w, 0.46f * w, 0.07f * w, 1.0f, // SplineSunHalo1 (default Spline normalized sRGB)
            1.36f * w, 0.52f * w, 0.13f * w, 1.0f, // SplineSunHalo2 (default Spline normalized sRGB)
        };
        ComPtr<ID3D11Texture2D> outTex;
        ASSERT_HRESULT_SUCCEEDED(compute.ToneMapHdrToSdr(
            reinterpret_cast<const uint8_t*>(neonPixels), 5, 1, 5 * 16,
            makeSettings(1u, identity), &outTex, QuickView::PixelFormat::R32G32B32A32_FLOAT));
        std::vector<uint8_t> rgba8;
        ASSERT_TRUE(ReadbackRgba8Texture(device.Get(), context.Get(), outTex.Get(), &rgba8));

        // ExactWhite: strictly (255, 255, 255)
        EXPECT_EQ(rgba8[0], 255u);
        EXPECT_EQ(rgba8[1], 255u);
        EXPECT_EQ(rgba8[2], 255u);

        // BlazingNeon: smooth white-hot bloom
        EXPECT_EQ(rgba8[4], 255u);
        EXPECT_GE(rgba8[5], 220u);
        EXPECT_GE(rgba8[6], 240u);

        // Fire4000: warm golden flame (never salmon pink where B approaches G)
        EXPECT_EQ(rgba8[8], 255u);
        EXPECT_GE(rgba8[9], 220u);
        EXPECT_GE(static_cast<int>(rgba8[9]) - static_cast<int>(rgba8[10]), 35);

        // SplineSunHalo1: golden yellow-orange (255, ~177, ~93), strictly rejecting pink B=138 regression
        EXPECT_EQ(rgba8[12], 255u);
        EXPECT_GE(rgba8[13], 170u);
        EXPECT_LE(rgba8[13], 190u);
        EXPECT_LE(rgba8[14], 105u);
        EXPECT_GE(static_cast<int>(rgba8[13]) - static_cast<int>(rgba8[14]), 75);

        // SplineSunHalo2: warm golden cloud (255, ~185, ~103)
        EXPECT_EQ(rgba8[16], 255u);
        EXPECT_GE(rgba8[17], 175u);
        EXPECT_LE(rgba8[18], 115u);
        EXPECT_GE(static_cast<int>(rgba8[17]) - static_cast<int>(rgba8[18]), 70);
    }
}

TEST(HdrPipelineTests, Bt2390NormalizedEetfKneeAccuracy) {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ASSERT_HRESULT_SUCCEEDED(CreateD3D11DeviceWithWarpFallback(&device, &context));

    QuickView::ComputeEngine compute;
    ASSERT_HRESULT_SUCCEEDED(compute.Initialize(device.Get()));

    QuickView::ToneMapSettings s = {};
    s.contentPeakScRgb = 543.7f / 80.0f;
    s.displayPeakScRgb = 203.0f / 80.0f;
    s.paperWhiteScRgb = 1.0f;
    s.exposure = 1.0f;
    s.exposureGain = 1.0f;
    s.mode = 2; // ITU-R BT.2390-10 normalized EETF
    s.isHdrOutput = 1;
    s.realHardwarePeakScRgb = 203.0f / 80.0f;
    s.transferFunction = static_cast<uint32_t>(QuickView::TransferFunction::Linear);
    s.colorMatrix[0] = s.colorMatrix[5] = s.colorMatrix[10] = s.colorMatrix[15] = 1.0f;

    const float pixels[3 * 4] = {
        25.0f / 80.0f,  25.0f / 80.0f,  25.0f / 80.0f,  1.0f,
        100.0f / 80.0f, 100.0f / 80.0f, 100.0f / 80.0f, 1.0f,
        543.7f / 80.0f, 543.7f / 80.0f, 543.7f / 80.0f, 1.0f,
    };

    // 1. HDR shader output verification
    {
        ComPtr<ID3D11Texture2D> outHdr;
        ASSERT_HRESULT_SUCCEEDED(compute.ToneMapHdrToHdr(
            reinterpret_cast<const uint8_t*>(pixels), 3, 1, 3 * 16,
            s, &outHdr, QuickView::PixelFormat::R32G32B32A32_FLOAT));
        std::vector<uint16_t> fp16;
        ASSERT_TRUE(ReadbackFp16Texture(device.Get(), context.Get(), outHdr.Get(), &fp16));
        EXPECT_NEAR(HalfToFloat(fp16[0]), 25.0f / 80.0f, 0.01f);
        // 100 nits is below BT.2390-10 normalized knee (~117 nits), so it must pass through 1:1
        EXPECT_NEAR(HalfToFloat(fp16[4]), 100.0f / 80.0f, 0.02f);
        // Content peak converges smoothly to 203 nits
        EXPECT_NEAR(HalfToFloat(fp16[8]), 203.0f / 80.0f, 0.03f);
    }

    // 2. SDR shader output verification
    {
        s.isHdrOutput = 0;
        ComPtr<ID3D11Texture2D> outSdr;
        ASSERT_HRESULT_SUCCEEDED(compute.ToneMapHdrToSdr(
            reinterpret_cast<const uint8_t*>(pixels), 3, 1, 3 * 16,
            s, &outSdr, QuickView::PixelFormat::R32G32B32A32_FLOAT));
        std::vector<uint8_t> rgba8;
        ASSERT_TRUE(ReadbackRgba8Texture(device.Get(), context.Get(), outSdr.Get(), &rgba8));
        // 100/203 linear -> 0.7305 sRGB -> 186/255
        EXPECT_NEAR(rgba8[4], 186, 2);
        EXPECT_GE(rgba8[8], 254u);
    }
}

TEST(HdrPipelineTests, DisplayColorStateBypassesMonitorProfileOnHdrAndWcg) {
    QuickView::DisplayColorState sdr = {};
    sdr.wideColorActive = false;
    sdr.advancedColorActive = false;
    sdr.highDynamicRangeUserEnabled = false;
    EXPECT_FALSE(sdr.ShouldBypassMonitorProfileForSdr());

    QuickView::DisplayColorState wcg = {};
    wcg.wideColorActive = true;
    wcg.advancedColorActive = false;
    wcg.highDynamicRangeUserEnabled = false;
    EXPECT_TRUE(wcg.ShouldBypassMonitorProfileForSdr());

    QuickView::DisplayColorState hdrOnly = {};
    hdrOnly.wideColorActive = false;
    hdrOnly.advancedColorActive = true;
    hdrOnly.highDynamicRangeUserEnabled = true;
    EXPECT_TRUE(hdrOnly.ShouldBypassMonitorProfileForSdr());

    QuickView::DisplayColorState hdrAndWcg = {};
    hdrAndWcg.wideColorActive = true;
    hdrAndWcg.advancedColorActive = true;
    hdrAndWcg.highDynamicRangeUserEnabled = true;
    EXPECT_TRUE(hdrAndWcg.ShouldBypassMonitorProfileForSdr());
}

TEST(HdrPipelineTests, DisplayColorInfoQueryMonitorCacheAndInvalidation) {
    HMONITOR hMon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    ASSERT_NE(hMon, nullptr);

    QuickView::DisplayColorInfo::InvalidateHardwareCache();
    QuickView::DisplayColorState s1 = {};
    ASSERT_TRUE(QuickView::DisplayColorInfo::QueryMonitorState(hMon, &s1));
    EXPECT_TRUE(s1.isValid);
    EXPECT_GT(s1.sdrWhiteLevelNits, 0.0f);

    QuickView::DisplayColorState s2 = {};
    ASSERT_TRUE(QuickView::DisplayColorInfo::QueryMonitorState(hMon, &s2));
    EXPECT_TRUE(s2.isValid);
    EXPECT_FLOAT_EQ(s1.sdrWhiteLevelNits, s2.sdrWhiteLevelNits);
    EXPECT_FLOAT_EQ(s1.maxLuminanceNits, s2.maxLuminanceNits);

    QuickView::DisplayColorInfo::InvalidateHardwareCache();
    QuickView::DisplayColorState s3 = {};
    ASSERT_TRUE(QuickView::DisplayColorInfo::QueryMonitorState(hMon, &s3));
    EXPECT_TRUE(s3.isValid);
    EXPECT_GT(s3.sdrWhiteLevelNits, 0.0f);
}

TEST(HdrPipelineTests, SdrBitmapToScRgbTargetWhiteLevelAdjustmentWarpReadback) {
    ComPtr<ID3D11Device> d3dDevice;
    ComPtr<ID3D11DeviceContext> d3dContext;
    ASSERT_HRESULT_SUCCEEDED(CreateD3D11DeviceWithWarpFallback(&d3dDevice, &d3dContext));

    ComPtr<IDXGIDevice> dxgiDevice;
    ASSERT_HRESULT_SUCCEEDED(d3dDevice.As(&dxgiDevice));

    ComPtr<ID2D1Factory1> d2dFactory;
    ASSERT_HRESULT_SUCCEEDED(D2D1CreateFactory(
        D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr, &d2dFactory));

    ComPtr<ID2D1Device> d2dDevice;
    ASSERT_HRESULT_SUCCEEDED(d2dFactory->CreateDevice(dxgiDevice.Get(), &d2dDevice));

    ComPtr<ID2D1DeviceContext> d2dCtx;
    ASSERT_HRESULT_SUCCEEDED(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dCtx));
    ASSERT_TRUE(d2dCtx->IsBufferPrecisionSupported(D2D1_BUFFER_PRECISION_16BPC_FLOAT));

    d2dCtx->SetDpi(96.0f, 96.0f);
    d2dCtx->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
    D2D1_RENDERING_CONTROLS controls = {};
    d2dCtx->GetRenderingControls(&controls);
    controls.bufferPrecision = D2D1_BUFFER_PRECISION_16BPC_FLOAT;
    d2dCtx->SetRenderingControls(&controls);

    ComPtr<ID2D1ColorContext> srgbCtx;
    ComPtr<ID2D1ColorContext> scRgbCtx;
    ASSERT_HRESULT_SUCCEEDED(d2dCtx->CreateColorContext(D2D1_COLOR_SPACE_SRGB, nullptr, 0, &srgbCtx));
    ASSERT_HRESULT_SUCCEEDED(d2dCtx->CreateColorContext(D2D1_COLOR_SPACE_SCRGB, nullptr, 0, &scRgbCtx));

    D3D11_TEXTURE2D_DESC rtDesc = {};
    rtDesc.Width = 4;
    rtDesc.Height = 4;
    rtDesc.MipLevels = 1;
    rtDesc.ArraySize = 1;
    rtDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    rtDesc.SampleDesc.Count = 1;
    rtDesc.Usage = D3D11_USAGE_DEFAULT;
    rtDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    ComPtr<ID3D11Texture2D> rtTex;
    ASSERT_HRESULT_SUCCEEDED(d3dDevice->CreateTexture2D(&rtDesc, nullptr, &rtTex));
    ComPtr<IDXGISurface> rtSurface;
    ASSERT_HRESULT_SUCCEEDED(rtTex.As(&rtSurface));

    D2D1_BITMAP_PROPERTIES1 rtProps = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_R16G16B16A16_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f, scRgbCtx.Get());
    ComPtr<ID2D1Bitmap1> rtBitmap;
    ASSERT_HRESULT_SUCCEEDED(d2dCtx->CreateBitmapFromDxgiSurface(rtSurface.Get(), &rtProps, &rtBitmap));

    // 1. 2x2 BGRA8 SDR source: (0,0)=white, (1,0)=mid-gray, (0,1)=50% premul gray, (1,1)=black
    const uint8_t sdrBytes[2 * 2 * 4] = {
        255, 255, 255, 255,  128, 128, 128, 255,
        128, 128, 128, 128,    0,   0,   0, 255,
    };
    D2D1_BITMAP_PROPERTIES1 sdrProps = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f, srgbCtx.Get());
    ComPtr<ID2D1Bitmap1> sdrBitmap;
    ASSERT_HRESULT_SUCCEEDED(d2dCtx->CreateBitmap(D2D1::SizeU(2, 2), sdrBytes, 8, &sdrProps, &sdrBitmap));

    ComPtr<ID2D1Effect> colorMgmt;
    ComPtr<ID2D1Effect> whiteLevel;
    ASSERT_HRESULT_SUCCEEDED(d2dCtx->CreateEffect(CLSID_D2D1ColorManagement, &colorMgmt));
    ASSERT_HRESULT_SUCCEEDED(d2dCtx->CreateEffect(CLSID_D2D1WhiteLevelAdjustment, &whiteLevel));

    colorMgmt->SetInput(0, sdrBitmap.Get());
    ASSERT_HRESULT_SUCCEEDED(colorMgmt->SetValue(D2D1_COLORMANAGEMENT_PROP_SOURCE_COLOR_CONTEXT, srgbCtx.Get()));
    ASSERT_HRESULT_SUCCEEDED(colorMgmt->SetValue(D2D1_COLORMANAGEMENT_PROP_DESTINATION_COLOR_CONTEXT, scRgbCtx.Get()));
    ASSERT_HRESULT_SUCCEEDED(colorMgmt->SetValue(D2D1_COLORMANAGEMENT_PROP_QUALITY, D2D1_COLORMANAGEMENT_QUALITY_BEST));

    whiteLevel->SetInputEffect(0, colorMgmt.Get());
    ASSERT_HRESULT_SUCCEEDED(whiteLevel->SetValue(D2D1_WHITELEVELADJUSTMENT_PROP_INPUT_WHITE_LEVEL, 240.0f));
    ASSERT_HRESULT_SUCCEEDED(whiteLevel->SetValue(D2D1_WHITELEVELADJUSTMENT_PROP_OUTPUT_WHITE_LEVEL, 80.0f));

    ComPtr<ID2D1CommandList> cmdList;
    ASSERT_HRESULT_SUCCEEDED(d2dCtx->CreateCommandList(&cmdList));
    d2dCtx->SetTarget(cmdList.Get());
    d2dCtx->BeginDraw();
    d2dCtx->Clear(D2D1::ColorF(0, 0, 0, 0));
    const D2D1_POINT_2F origin = D2D1::Point2F(0.0f, 0.0f);
    const D2D1_RECT_F sdrSrcRect = D2D1::RectF(0.0f, 0.0f, 2.0f, 2.0f);
    d2dCtx->DrawImage(whiteLevel.Get(), &origin, &sdrSrcRect, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    ASSERT_HRESULT_SUCCEEDED(d2dCtx->EndDraw());
    ASSERT_HRESULT_SUCCEEDED(cmdList->Close());

    d2dCtx->SetTarget(rtBitmap.Get());
    d2dCtx->BeginDraw();
    d2dCtx->Clear(D2D1::ColorF(0, 0, 0, 0));
    d2dCtx->DrawImage(cmdList.Get());
    ASSERT_HRESULT_SUCCEEDED(d2dCtx->EndDraw());
    d2dCtx->SetTarget(nullptr);

    std::vector<uint16_t> fp16;
    ASSERT_TRUE(ReadbackFp16Texture(d3dDevice.Get(), d3dContext.Get(), rtTex.Get(), &fp16));
    // Pixel (0, 0): white -> 240/80 = 3.0
    EXPECT_NEAR(HalfToFloat(fp16[0]), 3.0f, 0.05f);
    EXPECT_NEAR(HalfToFloat(fp16[1]), 3.0f, 0.05f);
    EXPECT_NEAR(HalfToFloat(fp16[2]), 3.0f, 0.05f);
    EXPECT_NEAR(HalfToFloat(fp16[3]), 1.0f, 0.02f);
    // Pixel (1, 0): sRGB(128/255) linear (~0.21586) * 3.0 = 0.6476
    EXPECT_NEAR(HalfToFloat(fp16[4]), 0.6476f, 0.03f);
    EXPECT_NEAR(HalfToFloat(fp16[5]), 0.6476f, 0.03f);
    EXPECT_NEAR(HalfToFloat(fp16[6]), 0.6476f, 0.03f);
    // Pixel (1, 1) at row 1 col 1 (index 4*1 + 1 = 5): black -> < 0.01
    EXPECT_LT(HalfToFloat(fp16[5u * 4u + 0u]), 0.01f);
    EXPECT_LT(HalfToFloat(fp16[5u * 4u + 1u]), 0.01f);
    EXPECT_LT(HalfToFloat(fp16[5u * 4u + 2u]), 0.01f);

    // 2. Verify FP16 HDR bitmap draws directly without white-level scaling
    const uint16_t hdrHalf[2 * 2 * 4] = {
        FloatToHalf(5.0f), FloatToHalf(2.0f), FloatToHalf(1.0f), FloatToHalf(1.0f),
        FloatToHalf(5.0f), FloatToHalf(2.0f), FloatToHalf(1.0f), FloatToHalf(1.0f),
        FloatToHalf(5.0f), FloatToHalf(2.0f), FloatToHalf(1.0f), FloatToHalf(1.0f),
        FloatToHalf(5.0f), FloatToHalf(2.0f), FloatToHalf(1.0f), FloatToHalf(1.0f),
    };
    D2D1_BITMAP_PROPERTIES1 hdrProps = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_R16G16B16A16_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f, scRgbCtx.Get());
    ComPtr<ID2D1Bitmap1> hdrBitmap;
    ASSERT_HRESULT_SUCCEEDED(d2dCtx->CreateBitmap(D2D1::SizeU(2, 2), hdrHalf, 16, &hdrProps, &hdrBitmap));

    d2dCtx->SetTarget(rtBitmap.Get());
    d2dCtx->BeginDraw();
    d2dCtx->Clear(D2D1::ColorF(0, 0, 0, 0));
    const D2D1_RECT_F hdrDstRect = D2D1::RectF(0.0f, 0.0f, 2.0f, 2.0f);
    d2dCtx->DrawBitmap(hdrBitmap.Get(), &hdrDstRect, 1.0f, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, &hdrDstRect);
    ASSERT_HRESULT_SUCCEEDED(d2dCtx->EndDraw());
    d2dCtx->SetTarget(nullptr);

    ASSERT_TRUE(ReadbackFp16Texture(d3dDevice.Get(), d3dContext.Get(), rtTex.Get(), &fp16));
    EXPECT_NEAR(HalfToFloat(fp16[0]), 5.0f, 0.05f);
    EXPECT_NEAR(HalfToFloat(fp16[1]), 2.0f, 0.05f);
    EXPECT_NEAR(HalfToFloat(fp16[2]), 1.0f, 0.05f);
}

namespace {

bool IsFiniteFloatBitsTest(float v) {
    uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    return (bits & 0x7F800000u) != 0x7F800000u;
}

QuickView::ToneMapSettings MakeIdentityToneMapSettings(float peakScRgb, QuickView::TransferFunction tf) {
    QuickView::ToneMapSettings s{};
    s.contentPeakScRgb = peakScRgb;
    s.displayPeakScRgb = peakScRgb;
    s.paperWhiteScRgb = 1.0f;
    s.exposure = 1.0f;
    s.exposureGain = 1.0f;
    s.mode = 1u; // Bypass spline/EETF to isolate EOTF, matrix, and sanitization
    s.isHdrOutput = 1u;
    s.realHardwarePeakScRgb = peakScRgb;
    s.transferFunction = static_cast<uint32_t>(tf);
    s.colorMatrix[0] = 1.0f;
    s.colorMatrix[5] = 1.0f;
    s.colorMatrix[10] = 1.0f;
    s.colorMatrix[15] = 1.0f;
    return s;
}

} // namespace

TEST(HdrPipelineTests, ToneMapHdrToHdrPreservesNegativeScRgbAndSanitizesInfNan) {
    ComPtr<ID3D11Device> d3dDevice;
    ComPtr<ID3D11DeviceContext> d3dContext;
    if (FAILED(CreateD3D11DeviceWithWarpFallback(&d3dDevice, &d3dContext))) {
        GTEST_SKIP() << "D3D11 WARP unavailable";
    }

    QuickView::ComputeEngine compute;
    ASSERT_HRESULT_SUCCEEDED(compute.Initialize(d3dDevice.Get()));

    const float posInf = std::numeric_limits<float>::infinity();
    const float qNaN = std::numeric_limits<float>::quiet_NaN();
    const float pixels[4 * 4] = {
        1.5f, -0.15f, 0.2f, 1.0f,          // Valid wide-gamut scRGB (negative G)
        posInf, 1.0f, 1.0f, 1.0f,          // +Inf in R
        qNaN, 0.5f, 0.5f, 1.0f,            // NaN in R
        100000.0f, 100000.0f, 100000.0f, 1.0f // Exceeds FP16 max
    };

    const QuickView::ToneMapSettings s = MakeIdentityToneMapSettings(10.0f, QuickView::TransferFunction::Linear);
    ComPtr<ID3D11Texture2D> outTex;
    ASSERT_HRESULT_SUCCEEDED(compute.ToneMapHdrToHdr(
        reinterpret_cast<const uint8_t*>(pixels), 4, 1, 4 * 16, s,
        &outTex, QuickView::PixelFormat::R32G32B32A32_FLOAT));

    std::vector<uint16_t> fp16;
    ASSERT_TRUE(ReadbackFp16Texture(d3dDevice.Get(), d3dContext.Get(), outTex.Get(), &fp16));
    ASSERT_EQ(fp16.size(), 16u);

    // Pixel 0: wide-gamut negative green preserved
    EXPECT_NEAR(HalfToFloat(fp16[0]), 1.5f, 0.02f);
    EXPECT_LT(HalfToFloat(fp16[1]), -0.10f);
    EXPECT_NEAR(HalfToFloat(fp16[1]), -0.15f, 0.02f);
    EXPECT_NEAR(HalfToFloat(fp16[2]), 0.2f, 0.02f);
    EXPECT_NEAR(HalfToFloat(fp16[3]), 1.0f, 0.02f);

    // Pixel 1: +Inf sanitized and luminance-bounded
    const float p1r = HalfToFloat(fp16[4]);
    const float p1g = HalfToFloat(fp16[5]);
    const float p1b = HalfToFloat(fp16[6]);
    EXPECT_TRUE(IsFiniteFloatBitsTest(p1r));
    EXPECT_TRUE(IsFiniteFloatBitsTest(p1g));
    EXPECT_TRUE(IsFiniteFloatBitsTest(p1b));
    const float p1Y = 0.2126f * p1r + 0.7152f * p1g + 0.0722f * p1b;
    EXPECT_GT(p1r, 0.0f);
    EXPECT_LE(p1Y, 10.1f);

    // Pixel 2: NaN becomes 0 while valid G/B remain intact
    EXPECT_NEAR(HalfToFloat(fp16[8]), 0.0f, 0.01f);
    EXPECT_NEAR(HalfToFloat(fp16[9]), 0.5f, 0.02f);
    EXPECT_NEAR(HalfToFloat(fp16[10]), 0.5f, 0.02f);

    // Pixel 3: 100000 clipped to FP16 max then scaled cleanly to peak 10.0
    for (size_t c = 0; c < 3; ++c) {
        const float v = HalfToFloat(fp16[12 + c]);
        EXPECT_TRUE(IsFiniteFloatBitsTest(v));
        EXPECT_LE(v, 10.1f);
        EXPECT_NEAR(v, 10.0f, 0.1f);
    }
}

TEST(HdrPipelineTests, ToneMapShadersDecodeGamma22AndRec709Eotf) {
    ComPtr<ID3D11Device> d3dDevice;
    ComPtr<ID3D11DeviceContext> d3dContext;
    if (FAILED(CreateD3D11DeviceWithWarpFallback(&d3dDevice, &d3dContext))) {
        GTEST_SKIP() << "D3D11 WARP unavailable";
    }

    QuickView::ComputeEngine compute;
    ASSERT_HRESULT_SUCCEEDED(compute.Initialize(d3dDevice.Get()));

    const float pixels[2 * 4] = {
        0.5f, 0.5f, 0.5f, 1.0f,
        0.04f, 0.04f, 0.04f, 1.0f
    };

    // Gamma 2.2
    {
        const QuickView::ToneMapSettings s = MakeIdentityToneMapSettings(1.0f, QuickView::TransferFunction::Gamma22);
        ComPtr<ID3D11Texture2D> outTex;
        ASSERT_HRESULT_SUCCEEDED(compute.ToneMapHdrToHdr(
            reinterpret_cast<const uint8_t*>(pixels), 2, 1, 2 * 16, s,
            &outTex, QuickView::PixelFormat::R32G32B32A32_FLOAT));
        std::vector<uint16_t> fp16;
        ASSERT_TRUE(ReadbackFp16Texture(d3dDevice.Get(), d3dContext.Get(), outTex.Get(), &fp16));
        EXPECT_NEAR(HalfToFloat(fp16[0]), std::pow(0.5f, 2.2f), 0.005f);
        EXPECT_NEAR(HalfToFloat(fp16[4]), std::pow(0.04f, 2.2f), 0.001f);
    }

    // Gamma 2.8
    {
        const QuickView::ToneMapSettings s = MakeIdentityToneMapSettings(1.0f, QuickView::TransferFunction::Gamma28);
        ComPtr<ID3D11Texture2D> outTex;
        ASSERT_HRESULT_SUCCEEDED(compute.ToneMapHdrToHdr(
            reinterpret_cast<const uint8_t*>(pixels), 2, 1, 2 * 16, s,
            &outTex, QuickView::PixelFormat::R32G32B32A32_FLOAT));
        std::vector<uint16_t> fp16;
        ASSERT_TRUE(ReadbackFp16Texture(d3dDevice.Get(), d3dContext.Get(), outTex.Get(), &fp16));
        EXPECT_NEAR(HalfToFloat(fp16[0]), std::pow(0.5f, 2.8f), 0.005f);
    }

    // Rec.709 (both toe < 0.081 and power >= 0.081 segments)
    {
        const QuickView::ToneMapSettings s = MakeIdentityToneMapSettings(1.0f, QuickView::TransferFunction::Rec709);
        ComPtr<ID3D11Texture2D> outTex;
        ASSERT_HRESULT_SUCCEEDED(compute.ToneMapHdrToHdr(
            reinterpret_cast<const uint8_t*>(pixels), 2, 1, 2 * 16, s,
            &outTex, QuickView::PixelFormat::R32G32B32A32_FLOAT));
        std::vector<uint16_t> fp16;
        ASSERT_TRUE(ReadbackFp16Texture(d3dDevice.Get(), d3dContext.Get(), outTex.Get(), &fp16));
        const float expectedHigh = std::pow((0.5f + 0.099f) / 1.099f, 1.0f / 0.45f);
        const float expectedLow = 0.04f / 4.5f;
        EXPECT_NEAR(HalfToFloat(fp16[0]), expectedHigh, 0.005f);
        EXPECT_NEAR(HalfToFloat(fp16[4]), expectedLow, 0.001f);
    }

    // ToneMapHdrToSdr must also decode Gamma 2.2 before sRGB encoding (0.5 -> linear 0.2176 -> sRGB ~128, not 188)
    {
        QuickView::ToneMapSettings s = MakeIdentityToneMapSettings(1.0f, QuickView::TransferFunction::Gamma22);
        s.isHdrOutput = 0u;
        ComPtr<ID3D11Texture2D> outTex;
        ASSERT_HRESULT_SUCCEEDED(compute.ToneMapHdrToSdr(
            reinterpret_cast<const uint8_t*>(pixels), 2, 1, 2 * 16, s,
            &outTex, QuickView::PixelFormat::R32G32B32A32_FLOAT));
        std::vector<uint8_t> rgba8;
        ASSERT_TRUE(ReadbackRgba8Texture(d3dDevice.Get(), d3dContext.Get(), outTex.Get(), &rgba8));
        EXPECT_NEAR(static_cast<int>(rgba8[0]), 128, 3);
    }
}

TEST(HdrPipelineTests, ComposeGainMapPreservesAlphaAndHandlesHdrBase) {
    ComPtr<ID3D11Device> d3dDevice;
    ComPtr<ID3D11DeviceContext> d3dContext;
    if (FAILED(CreateD3D11DeviceWithWarpFallback(&d3dDevice, &d3dContext))) {
        GTEST_SKIP() << "D3D11 WARP unavailable";
    }

    QuickView::ComputeEngine compute;
    ASSERT_HRESULT_SUCCEEDED(compute.Initialize(d3dDevice.Get()));

    // Case A: HDR linear float base with non-1.0 alpha and HDR-base direction
    const float hdrBase[4] = { 2.0f, 2.0f, 2.0f, 0.5f };
    const uint8_t gain[1] = { 255u };

    QuickView::GpuShaderPayload payload{};
    payload.gainMapMin[0] = payload.gainMapMin[1] = payload.gainMapMin[2] = 0.0f;
    payload.gainMapMax[0] = payload.gainMapMax[1] = payload.gainMapMax[2] = 2.0f;
    payload.gamma[0] = payload.gamma[1] = payload.gamma[2] = 1.0f;
    payload.offsetSdr[0] = payload.offsetSdr[1] = payload.offsetSdr[2] = 0.0f;
    payload.offsetHdr[0] = payload.offsetHdr[1] = payload.offsetHdr[2] = 0.0f;
    payload.hdrCapacityMin = 0.0f;
    payload.hdrCapacityMax = 2.0f;
    payload.baseIsHdr = 1.0f;
    payload.sdrWidth = 1u;
    payload.sdrHeight = 1u;

    // targetHeadroom = 0.0 -> SDR direction: 2.0 * 2^(-2) = 0.5, alpha = 0.5
    {
        payload.targetHeadroom = 0.0f;
        ComPtr<ID3D11Texture2D> outTex;
        ASSERT_HRESULT_SUCCEEDED(compute.ComposeGainMap(
            reinterpret_cast<const uint8_t*>(hdrBase), 1, 1, 16,
            QuickView::PixelFormat::R32G32B32A32_FLOAT,
            gain, 1, 1, 1, payload, &outTex));
        std::vector<uint16_t> fp16;
        ASSERT_TRUE(ReadbackFp16Texture(d3dDevice.Get(), d3dContext.Get(), outTex.Get(), &fp16));
        EXPECT_NEAR(HalfToFloat(fp16[0]), 0.5f, 0.01f);
        EXPECT_NEAR(HalfToFloat(fp16[3]), 0.5f, 0.01f);
    }

    // targetHeadroom = 2.0 -> HDR identity: 2.0 * 2^0 = 2.0, alpha = 0.5
    {
        payload.targetHeadroom = 2.0f;
        ComPtr<ID3D11Texture2D> outTex;
        ASSERT_HRESULT_SUCCEEDED(compute.ComposeGainMap(
            reinterpret_cast<const uint8_t*>(hdrBase), 1, 1, 16,
            QuickView::PixelFormat::R32G32B32A32_FLOAT,
            gain, 1, 1, 1, payload, &outTex));
        std::vector<uint16_t> fp16;
        ASSERT_TRUE(ReadbackFp16Texture(d3dDevice.Get(), d3dContext.Get(), outTex.Get(), &fp16));
        EXPECT_NEAR(HalfToFloat(fp16[0]), 2.0f, 0.02f);
        EXPECT_NEAR(HalfToFloat(fp16[3]), 0.5f, 0.01f);
    }

    // Non-zero OffsetHdr / OffsetSdr swap at targetHeadroom = 0.0 -> (2.0 + 0.25) * 0.25 - 0.05 = 0.5125
    {
        payload.offsetSdr[0] = payload.offsetSdr[1] = payload.offsetSdr[2] = 0.05f;
        payload.offsetHdr[0] = payload.offsetHdr[1] = payload.offsetHdr[2] = 0.25f;
        payload.targetHeadroom = 0.0f;
        ComPtr<ID3D11Texture2D> outTex;
        ASSERT_HRESULT_SUCCEEDED(compute.ComposeGainMap(
            reinterpret_cast<const uint8_t*>(hdrBase), 1, 1, 16,
            QuickView::PixelFormat::R32G32B32A32_FLOAT,
            gain, 1, 1, 1, payload, &outTex));
        std::vector<uint16_t> fp16;
        ASSERT_TRUE(ReadbackFp16Texture(d3dDevice.Get(), d3dContext.Get(), outTex.Get(), &fp16));
        EXPECT_NEAR(HalfToFloat(fp16[0]), 0.5125f, 0.01f);
    }

    // Case B: 8-bit SDR base preserves partial alpha through ComposeGainMap
    {
        const uint8_t sdrBgra[4] = { 255u, 255u, 255u, 128u };
        QuickView::GpuShaderPayload sdrPayload{};
        sdrPayload.gainMapMax[0] = sdrPayload.gainMapMax[1] = sdrPayload.gainMapMax[2] = 2.0f;
        sdrPayload.gamma[0] = sdrPayload.gamma[1] = sdrPayload.gamma[2] = 1.0f;
        sdrPayload.hdrCapacityMin = 0.0f;
        sdrPayload.hdrCapacityMax = 2.0f;
        sdrPayload.targetHeadroom = 2.0f;
        sdrPayload.baseIsHdr = 0.0f;
        sdrPayload.sdrWidth = 1u;
        sdrPayload.sdrHeight = 1u;

        ComPtr<ID3D11Texture2D> outTex;
        ASSERT_HRESULT_SUCCEEDED(compute.ComposeGainMap(
            sdrBgra, 1, 1, 4, QuickView::PixelFormat::BGRA8888,
            gain, 1, 1, 1, sdrPayload, &outTex));
        std::vector<uint16_t> fp16;
        ASSERT_TRUE(ReadbackFp16Texture(d3dDevice.Get(), d3dContext.Get(), outTex.Get(), &fp16));
        EXPECT_NEAR(HalfToFloat(fp16[0]), 4.0f, 0.05f);
        EXPECT_NEAR(HalfToFloat(fp16[3]), 128.0f / 255.0f, 0.01f);
    }
}

TEST(HdrPipelineTests, ToneMapSupports8BitPqInputAndPeakScan) {
    ComPtr<ID3D11Device> d3dDevice;
    ComPtr<ID3D11DeviceContext> d3dContext;
    if (FAILED(CreateD3D11DeviceWithWarpFallback(&d3dDevice, &d3dContext))) {
        GTEST_SKIP() << "D3D11 WARP unavailable";
    }

    QuickView::ComputeEngine compute;
    ASSERT_HRESULT_SUCCEEDED(compute.Initialize(d3dDevice.Get()));

    const float pq1000 = NitsToPqSignal(1000.0f);
    const uint8_t pqByte = static_cast<uint8_t>(std::lround(pq1000 * 255.0f));
    const float expectedScRgb = ColorMath::PqToLinearScRgb(static_cast<float>(pqByte) / 255.0f);

    std::vector<uint8_t> bgra8(2 * 2 * 4);
    for (size_t i = 0; i < 4; ++i) {
        bgra8[i * 4 + 0] = pqByte;
        bgra8[i * 4 + 1] = pqByte;
        bgra8[i * 4 + 2] = pqByte;
        bgra8[i * 4 + 3] = 255u;
    }

    QuickView::RawImageFrame frame;
    frame.width = 2;
    frame.height = 2;
    frame.stride = 8;
    frame.format = QuickView::PixelFormat::BGRA8888;
    frame.pixels = bgra8.data();

    const float peakScRgb = ColorMath::EstimateEncodedFramePeakScRgb(frame, QuickView::TransferFunction::PQ, 100.0f);
    EXPECT_GT(peakScRgb, 10.0f);
    EXPECT_NEAR(peakScRgb, expectedScRgb, 0.25f);

    const QuickView::ToneMapSettings s = MakeIdentityToneMapSettings(20.0f, QuickView::TransferFunction::PQ);
    ComPtr<ID3D11Texture2D> outTex;
    ASSERT_HRESULT_SUCCEEDED(compute.ToneMapHdrToHdr(
        bgra8.data(), 2, 2, 8, s, &outTex, QuickView::PixelFormat::BGRA8888));

    std::vector<uint16_t> fp16;
    ASSERT_TRUE(ReadbackFp16Texture(d3dDevice.Get(), d3dContext.Get(), outTex.Get(), &fp16));
    EXPECT_GT(HalfToFloat(fp16[0]), 10.0f);
    EXPECT_NEAR(HalfToFloat(fp16[0]), expectedScRgb, 0.15f);

    // Also verify RGBA8888 channel order is preserved cleanly (R > G > B)
    const uint8_t pqR = static_cast<uint8_t>(std::lround(NitsToPqSignal(1000.0f) * 255.0f));
    const uint8_t pqG = static_cast<uint8_t>(std::lround(NitsToPqSignal(400.0f) * 255.0f));
    const uint8_t pqB = static_cast<uint8_t>(std::lround(NitsToPqSignal(80.0f) * 255.0f));
    const uint8_t rgba8[4] = { pqR, pqG, pqB, 255u };
    ComPtr<ID3D11Texture2D> rgbaOutTex;
    ASSERT_HRESULT_SUCCEEDED(compute.ToneMapHdrToHdr(
        rgba8, 1, 1, 4, s, &rgbaOutTex, QuickView::PixelFormat::RGBA8888));
    ASSERT_TRUE(ReadbackFp16Texture(d3dDevice.Get(), d3dContext.Get(), rgbaOutTex.Get(), &fp16));
    EXPECT_GT(HalfToFloat(fp16[0]), HalfToFloat(fp16[1]));
    EXPECT_GT(HalfToFloat(fp16[1]), HalfToFloat(fp16[2]));
    EXPECT_NEAR(HalfToFloat(fp16[0]), ColorMath::PqToLinearScRgb(pqR / 255.0f), 0.15f);
    EXPECT_NEAR(HalfToFloat(fp16[1]), ColorMath::PqToLinearScRgb(pqG / 255.0f), 0.1f);
    EXPECT_NEAR(HalfToFloat(fp16[2]), ColorMath::PqToLinearScRgb(pqB / 255.0f), 0.05f);
}

TEST(HdrPipelineTests, SimdPeakLuminanceFiltersInfAndNan) {
    const float posInf = std::numeric_limits<float>::infinity();
    const float negInf = -std::numeric_limits<float>::infinity();
    const float qNaN = std::numeric_limits<float>::quiet_NaN();

    // 1. 7-pixel arrays exercise pure scalar tail on AVX2 (N_f=8, N_h=16) with Inf/NaN
    const float fp32Scalar[7 * 4] = {
        4.0f, 4.0f, 4.0f, 1.0f,     // Peak finite luminance = 4.0
        1.0f, 1.0f, 1.0f, posInf,   // +Inf in alpha only -> valid RGB
        posInf, 0.5f, 0.5f, 1.0f,   // +Inf in R
        qNaN, 0.5f, 0.5f, 1.0f,     // NaN in R
        negInf, 2.0f, 2.0f, 1.0f,   // -Inf in R
        2.0f, 2.0f, 2.0f, 1.0f,     // Valid
        1.0f, qNaN, 1.0f, 1.0f      // NaN in G
    };
    EXPECT_TRUE(IsFiniteFloatBitsTest(ImageLoaderSimd::FindPeakFloat(fp32Scalar, 7)));
    EXPECT_TRUE(IsFiniteFloatBitsTest(ImageLoaderSimd::FindPeakLuminanceFloat(fp32Scalar, 7)));
    EXPECT_TRUE(IsFiniteFloatBitsTest(ImageLoaderSimd::SumLuminanceFloatRange(fp32Scalar, 0, 7)));
    EXPECT_NEAR(ImageLoaderSimd::FindPeakFloat(fp32Scalar, 7), 4.0f, 1e-4f);
    EXPECT_NEAR(ImageLoaderSimd::FindPeakLuminanceFloat(fp32Scalar, 7), 4.0f, 1e-4f);
    EXPECT_NEAR(ImageLoaderSimd::SumLuminanceFloatRange(fp32Scalar, 0, 7), 7.0f, 1e-3f);

    const uint16_t fp16Scalar[7 * 4] = {
        FloatToHalf(4.0f), FloatToHalf(4.0f), FloatToHalf(4.0f), FloatToHalf(1.0f),
        FloatToHalf(1.0f), FloatToHalf(1.0f), FloatToHalf(1.0f), 0x7C00u,
        0x7C00u, FloatToHalf(0.5f), FloatToHalf(0.5f), FloatToHalf(1.0f),
        0x7E00u, FloatToHalf(0.5f), FloatToHalf(0.5f), FloatToHalf(1.0f),
        0xFC00u, FloatToHalf(2.0f), FloatToHalf(2.0f), FloatToHalf(1.0f),
        FloatToHalf(2.0f), FloatToHalf(2.0f), FloatToHalf(2.0f), FloatToHalf(1.0f),
        FloatToHalf(1.0f), 0x7FFFu, FloatToHalf(1.0f), FloatToHalf(1.0f)
    };
    EXPECT_TRUE(IsFiniteFloatBitsTest(ImageLoaderSimd::FindPeakHalf(fp16Scalar, 7)));
    EXPECT_TRUE(IsFiniteFloatBitsTest(ImageLoaderSimd::FindPeakLuminanceHalf(fp16Scalar, 7)));
    EXPECT_TRUE(IsFiniteFloatBitsTest(ImageLoaderSimd::SumLuminanceHalfRange(fp16Scalar, 0, 7)));
    EXPECT_NEAR(ImageLoaderSimd::FindPeakHalf(fp16Scalar, 7), 4.0f, 1e-3f);
    EXPECT_NEAR(ImageLoaderSimd::FindPeakLuminanceHalf(fp16Scalar, 7), 4.0f, 1e-3f);
    EXPECT_NEAR(ImageLoaderSimd::SumLuminanceHalfRange(fp16Scalar, 0, 7), 7.0f, 1e-2f);

    // 2. 17-pixel arrays exercise AVX2 SIMD main loop + 1-pixel scalar tail
    std::vector<float> fp32Simd(17 * 4, 0.0f);
    std::vector<uint16_t> fp16Simd(17 * 4, 0u);
    std::memcpy(fp32Simd.data(), fp32Scalar, sizeof(fp32Scalar));
    std::memcpy(fp16Simd.data(), fp16Scalar, sizeof(fp16Scalar));
    for (size_t p = 7; p < 16; ++p) {
        fp32Simd[p * 4 + 3] = 1.0f;
        fp16Simd[p * 4 + 3] = FloatToHalf(1.0f);
    }
    // Tail pixel 16: (1.5, 1.5, 1.5, 1.0) -> adds 1.5 to sum -> total 8.5
    fp32Simd[16 * 4 + 0] = fp32Simd[16 * 4 + 1] = fp32Simd[16 * 4 + 2] = 1.5f;
    fp32Simd[16 * 4 + 3] = 1.0f;
    fp16Simd[16 * 4 + 0] = fp16Simd[16 * 4 + 1] = fp16Simd[16 * 4 + 2] = FloatToHalf(1.5f);
    fp16Simd[16 * 4 + 3] = FloatToHalf(1.0f);

    EXPECT_TRUE(IsFiniteFloatBitsTest(ImageLoaderSimd::FindPeakFloat(fp32Simd.data(), 17)));
    EXPECT_TRUE(IsFiniteFloatBitsTest(ImageLoaderSimd::FindPeakLuminanceFloat(fp32Simd.data(), 17)));
    EXPECT_TRUE(IsFiniteFloatBitsTest(ImageLoaderSimd::SumLuminanceFloatRange(fp32Simd.data(), 0, 17)));
    EXPECT_NEAR(ImageLoaderSimd::FindPeakFloat(fp32Simd.data(), 17), 4.0f, 1e-4f);
    EXPECT_NEAR(ImageLoaderSimd::FindPeakLuminanceFloat(fp32Simd.data(), 17), 4.0f, 1e-4f);
    EXPECT_NEAR(ImageLoaderSimd::SumLuminanceFloatRange(fp32Simd.data(), 0, 17), 8.5f, 1e-3f);

    EXPECT_TRUE(IsFiniteFloatBitsTest(ImageLoaderSimd::FindPeakHalf(fp16Simd.data(), 17)));
    EXPECT_TRUE(IsFiniteFloatBitsTest(ImageLoaderSimd::FindPeakLuminanceHalf(fp16Simd.data(), 17)));
    EXPECT_TRUE(IsFiniteFloatBitsTest(ImageLoaderSimd::SumLuminanceHalfRange(fp16Simd.data(), 0, 17)));
    EXPECT_NEAR(ImageLoaderSimd::FindPeakHalf(fp16Simd.data(), 17), 4.0f, 1e-3f);
    EXPECT_NEAR(ImageLoaderSimd::FindPeakLuminanceHalf(fp16Simd.data(), 17), 4.0f, 1e-3f);
    EXPECT_NEAR(ImageLoaderSimd::SumLuminanceHalfRange(fp16Simd.data(), 0, 17), 8.5f, 1e-2f);
}

namespace {

struct LiveWorkerOutputHdr {
    DWORD exitCode = 999;
    QuickView::ToolProcess::DecodeResultHeader header{};
    std::vector<uint8_t> pixelBytes;
    std::vector<uint8_t> iccBytes;
};

std::filesystem::path FindWorkerExecutableForTests() {
    wchar_t exeBuf[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exeBuf, MAX_PATH) == 0) {
        return {};
    }
    std::filesystem::path workerExe = std::filesystem::path(exeBuf).parent_path() / L"QuickView.exe";
    std::error_code ec;
    if (std::filesystem::exists(workerExe, ec)) {
        return workerExe;
    }
    return {};
}

void ExecuteDecodeWorkerSubprocessWithOptions(
    const std::filesystem::path& workerExe,
    const std::filesystem::path& inputPath,
    const wchar_t* mapTag,
    int targetW,
    int targetH,
    uint64_t reserveBpp,
    float hdrHeadroomStops,
    bool preserveFloat,
    LiveWorkerOutputHdr* outResult) {
    using namespace QuickView::ToolProcess;
    ASSERT_NE(outResult, nullptr);
    static std::atomic<uint32_t> s_mapSeq{0};
    const uint32_t seq = s_mapSeq.fetch_add(1u, std::memory_order_relaxed);

    const uint64_t pixelCap = static_cast<uint64_t>(targetW) * static_cast<uint64_t>(targetH) * reserveBpp;
    const DecodeMapLayout layout = ComputeDecodeMapLayout(pixelCap);
    ASSERT_NE(layout.totalMapBytes, 0u);

    wchar_t mapName[160] = {};
    swprintf_s(mapName, L"Local\\QuickView_HdrTestDW_%lu_%u_%ls", GetCurrentProcessId(), seq, mapTag);

    HANDLE hMap = CreateFileMappingW(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_RESERVE,
        static_cast<DWORD>(layout.totalMapBytes >> 32),
        static_cast<DWORD>(layout.totalMapBytes & 0xFFFFFFFFull),
        mapName);
    ASSERT_NE(hMap, nullptr);

    uint8_t* mapView = static_cast<uint8_t*>(MapViewOfFile(
        hMap, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, static_cast<SIZE_T>(layout.totalMapBytes)));
    ASSERT_NE(mapView, nullptr);

    ASSERT_NE(VirtualAlloc(mapView, static_cast<SIZE_T>(layout.headerBytes), MEM_COMMIT, PAGE_READWRITE), nullptr);
    auto* header = reinterpret_cast<DecodeResultHeader*>(mapView);
    *header = {};
    header->magic = kDecodeWorkerMagic;
    header->version = kDecodeWorkerVersion;
    header->hr = static_cast<int32_t>(E_PENDING);
    header->pixelOffset = layout.pixelOffset;
    header->pixelCapacity = layout.pixelCapacity;
    header->iccOffset = layout.iccOffset;
    header->iccCapacity = layout.iccCapacity;

    std::wstring cmd = L"\"" + workerExe.wstring() + L"\" --decode-worker --input \"" +
                       inputPath.wstring() + L"\" --out-map \"" + mapName +
                       L"\" --target-w " + std::to_wstring(targetW) +
                       L" --target-h " + std::to_wstring(targetH) +
                       L" --no-fake-base";
    if (hdrHeadroomStops >= 0.0f) {
        wchar_t headroomBuf[32] = {};
        swprintf_s(headroomBuf, L"%.3f", hdrHeadroomStops);
        cmd += L" --hdr-headroom ";
        cmd += headroomBuf;
    }
    if (preserveFloat) {
        cmd += L" --preserve-float";
    }
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_FORCEOFFFEEDBACK;
    PROCESS_INFORMATION pi{};
    ASSERT_TRUE(CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
                               CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi));
    CloseHandle(pi.hThread);

    DWORD waitRes = WaitForSingleObject(pi.hProcess, 20000);
    GetExitCodeProcess(pi.hProcess, &outResult->exitCode);
    CloseHandle(pi.hProcess);

    ASSERT_EQ(waitRes, WAIT_OBJECT_0);
    outResult->header = *header;
    if (ValidateDecodeResultHeader(*header, layout)) {
        outResult->pixelBytes.assign(
            mapView + layout.pixelOffset,
            mapView + layout.pixelOffset + header->payloadBytes);
        if (header->iccBytes > 0) {
            outResult->iccBytes.assign(
                mapView + layout.iccOffset,
                mapView + layout.iccOffset + header->iccBytes);
        }
    }

    UnmapViewOfFile(mapView);
    CloseHandle(hMap);
}

struct UnpackedHdrFlagsTest {
    bool isValid = false;
    bool isHdr = false;
    bool isSceneLinear = false;
    bool hasNitsMetadata = false;
    bool hasGainMap = false;
    bool gainMapApplied = false;
};

UnpackedHdrFlagsTest UnpackHdrFlagsTestHelper(uint8_t flags) {
    UnpackedHdrFlagsTest u{};
    QuickView::ToolProcess::UnpackHdrFlags(
        flags, u.isValid, u.isHdr, u.isSceneLinear, u.hasNitsMetadata, u.hasGainMap, u.gainMapApplied);
    return u;
}

} // namespace

TEST(HdrPipelineTests, RealHdrJpegPqHlgAndUltraHdrDecode) {
    const auto workerExe = FindWorkerExecutableForTests();
    if (workerExe.empty()) {
        GTEST_SKIP() << "QuickView.exe not present next to test binary";
    }

    // 1. sunrise-PQ.jpg -> CICP PQ + Rec.2020 in ICC
    if (const auto pqPath = FindHdrTestAsset(L"sunrise-PQ.jpg"); !pqPath.empty()) {
        LiveWorkerOutputHdr res;
        ASSERT_NO_FATAL_FAILURE(ExecuteDecodeWorkerSubprocessWithOptions(
            workerExe, pqPath, L"jpg_pq", 1024, 1024, 16ull, -1.0f, false, &res));
        ASSERT_EQ(res.exitCode, 0u);
        ASSERT_HRESULT_SUCCEEDED(static_cast<HRESULT>(res.header.hr));
        EXPECT_EQ(res.header.colorDataSpace, static_cast<uint8_t>(QuickView::PixelDataSpace::EncodedHdr));
        EXPECT_EQ(res.header.colorTransfer, static_cast<uint8_t>(QuickView::TransferFunction::PQ));
        EXPECT_EQ(res.header.colorPrimaries, static_cast<uint8_t>(QuickView::ColorPrimaries::Rec2020));
        EXPECT_TRUE(UnpackHdrFlagsTestHelper(res.header.hdrFlags).isHdr);
    }

    // 2. test_pattern-HLG.jpg -> CICP HLG in ICC
    if (const auto hlgPath = FindHdrTestAsset(L"test_pattern-HLG.jpg"); !hlgPath.empty()) {
        LiveWorkerOutputHdr res;
        ASSERT_NO_FATAL_FAILURE(ExecuteDecodeWorkerSubprocessWithOptions(
            workerExe, hlgPath, L"jpg_hlg", 1024, 1024, 16ull, -1.0f, false, &res));
        ASSERT_EQ(res.exitCode, 0u);
        ASSERT_HRESULT_SUCCEEDED(static_cast<HRESULT>(res.header.hr));
        EXPECT_EQ(res.header.colorDataSpace, static_cast<uint8_t>(QuickView::PixelDataSpace::EncodedHdr));
        EXPECT_EQ(res.header.colorTransfer, static_cast<uint8_t>(QuickView::TransferFunction::HLG));
        EXPECT_TRUE(UnpackHdrFlagsTestHelper(res.header.hdrFlags).isHdr);
    }

    // 3. ultra_hdr.jpg & Ultra_HDR_Samples_Originals_05.jpg -> secondary MPF XMP rdf:Seq & ISO 21496-1 APP2
    const wchar_t* ultraHdrNames[] = { L"ultra_hdr.jpg", L"Ultra_HDR_Samples_Originals_05.jpg" };
    for (size_t i = 0; i < 2; ++i) {
        const auto uhdrPath = FindHdrTestAsset(ultraHdrNames[i]);
        if (uhdrPath.empty()) continue;
        LiveWorkerOutputHdr res;
        ASSERT_NO_FATAL_FAILURE(ExecuteDecodeWorkerSubprocessWithOptions(
            workerExe, uhdrPath, L"jpg_uhdr", 1024, 1024, 16ull, -1.0f, false, &res));
        ASSERT_EQ(res.exitCode, 0u);
        ASSERT_HRESULT_SUCCEEDED(static_cast<HRESULT>(res.header.hr));
        EXPECT_TRUE(UnpackHdrFlagsTestHelper(res.header.hdrFlags).hasGainMap) << ultraHdrNames[i];
        EXPECT_GT(res.header.gainMapAlternateHeadroom, 2.0f) << ultraHdrNames[i];
    }
}

TEST(HdrPipelineTests, RealHdrAvifPqHlgGainMapAndSdr10BitDecode) {
    const auto workerExe = FindWorkerExecutableForTests();
    if (workerExe.empty()) {
        GTEST_SKIP() << "QuickView.exe not present next to test binary";
    }

    // 1. sunrise-PQ.avif (ICC CICP PQ + preserveFloat) -> SceneLinearScRgb FP16
    if (const auto path = FindHdrTestAsset(L"sunrise-PQ.avif"); !path.empty()) {
        LiveWorkerOutputHdr res;
        ASSERT_NO_FATAL_FAILURE(ExecuteDecodeWorkerSubprocessWithOptions(
            workerExe, path, L"avif_pq", 1024, 1024, 16ull, -1.0f, true, &res));
        ASSERT_EQ(res.exitCode, 0u);
        ASSERT_HRESULT_SUCCEEDED(static_cast<HRESULT>(res.header.hr));
        EXPECT_EQ(res.header.pixelFormat, static_cast<uint8_t>(QuickView::PixelFormat::R16G16B16A16_FLOAT));
        EXPECT_EQ(res.header.colorDataSpace, static_cast<uint8_t>(QuickView::PixelDataSpace::SceneLinearScRgb));
        EXPECT_TRUE(UnpackHdrFlagsTestHelper(res.header.hdrFlags).isHdr);
    }

    // 2. test_pattern-HLG-CICP.avif -> SceneLinearScRgb FP16
    if (const auto path = FindHdrTestAsset(L"test_pattern-HLG-CICP.avif"); !path.empty()) {
        LiveWorkerOutputHdr res;
        ASSERT_NO_FATAL_FAILURE(ExecuteDecodeWorkerSubprocessWithOptions(
            workerExe, path, L"avif_hlg", 1024, 1024, 16ull, -1.0f, true, &res));
        ASSERT_EQ(res.exitCode, 0u);
        ASSERT_HRESULT_SUCCEEDED(static_cast<HRESULT>(res.header.hr));
        EXPECT_EQ(res.header.pixelFormat, static_cast<uint8_t>(QuickView::PixelFormat::R16G16B16A16_FLOAT));
        EXPECT_EQ(res.header.colorDataSpace, static_cast<uint8_t>(QuickView::PixelDataSpace::SceneLinearScRgb));
        EXPECT_TRUE(UnpackHdrFlagsTestHelper(res.header.hdrFlags).isHdr);
    }

    // 3. WideGamut-Dolomiti-DisplayP3.avif (10-bit SDR Display P3 with HDR headroom 2.5)
    if (const auto path = FindHdrTestAsset(L"WideGamut-Dolomiti-DisplayP3.avif"); !path.empty()) {
        LiveWorkerOutputHdr res;
        ASSERT_NO_FATAL_FAILURE(ExecuteDecodeWorkerSubprocessWithOptions(
            workerExe, path, L"avif_p3sdr", 1024, 1024, 16ull, 2.5f, true, &res));
        ASSERT_EQ(res.exitCode, 0u);
        ASSERT_HRESULT_SUCCEEDED(static_cast<HRESULT>(res.header.hr));
        EXPECT_FALSE(UnpackHdrFlagsTestHelper(res.header.hdrFlags).isHdr);
        EXPECT_EQ(res.header.colorPrimaries, static_cast<uint8_t>(QuickView::ColorPrimaries::DisplayP3));
        EXPECT_EQ(res.header.colorDataSpace, static_cast<uint8_t>(QuickView::PixelDataSpace::EncodedSdr));
    }

    // 4. GainMap AVIF (Document-Name34_2-1.avif has ISO 21496-1 tmap box): SDR default vs HDR target (headroom 2.5)
    if (const auto path = FindHdrTestAsset(L"Document-Name34_2-1.avif"); !path.empty()) {
        {
            LiveWorkerOutputHdr res;
            ASSERT_NO_FATAL_FAILURE(ExecuteDecodeWorkerSubprocessWithOptions(
                workerExe, path, L"avif_gm_sdr", 1024, 1024, 16ull, -1.0f, false, &res));
            ASSERT_EQ(res.exitCode, 0u);
            ASSERT_HRESULT_SUCCEEDED(static_cast<HRESULT>(res.header.hr));
            const auto flags = UnpackHdrFlagsTestHelper(res.header.hdrFlags);
            EXPECT_TRUE(flags.hasGainMap);
            EXPECT_FALSE(flags.isHdr);
        }
        {
            LiveWorkerOutputHdr res;
            ASSERT_NO_FATAL_FAILURE(ExecuteDecodeWorkerSubprocessWithOptions(
                workerExe, path, L"avif_gm_hdr", 1024, 1024, 16ull, 2.5f, true, &res));
            ASSERT_EQ(res.exitCode, 0u);
            ASSERT_HRESULT_SUCCEEDED(static_cast<HRESULT>(res.header.hr));
            const auto flags = UnpackHdrFlagsTestHelper(res.header.hdrFlags);
            EXPECT_TRUE(flags.gainMapApplied);
            EXPECT_EQ(res.header.iccBytes, 0u);
            EXPECT_EQ(res.header.pixelFormat, static_cast<uint8_t>(QuickView::PixelFormat::R16G16B16A16_FLOAT));
        }
    }

    // 5. Display P3 + PQ AVIF (no tmap box despite filename)
    if (const auto path = FindHdrTestAsset(L"DSC2823_Instagram-post_2160x2160_U_0_P3_gainMap.avif"); !path.empty()) {
        LiveWorkerOutputHdr res;
        ASSERT_NO_FATAL_FAILURE(ExecuteDecodeWorkerSubprocessWithOptions(
            workerExe, path, L"avif_p3pq", 1024, 1024, 16ull, -1.0f, true, &res));
        ASSERT_EQ(res.exitCode, 0u);
        ASSERT_HRESULT_SUCCEEDED(static_cast<HRESULT>(res.header.hr));
        EXPECT_TRUE(UnpackHdrFlagsTestHelper(res.header.hdrFlags).isHdr);
        EXPECT_EQ(res.header.colorPrimaries, static_cast<uint8_t>(QuickView::ColorPrimaries::DisplayP3));
        EXPECT_EQ(res.header.colorDataSpace, static_cast<uint8_t>(QuickView::PixelDataSpace::SceneLinearScRgb));
        EXPECT_EQ(res.header.pixelFormat, static_cast<uint8_t>(QuickView::PixelFormat::R16G16B16A16_FLOAT));
    }
}

TEST(HdrPipelineTests, RealHdrJxlPqHlgAndSrgbDecode) {
    const auto workerExe = FindWorkerExecutableForTests();
    if (workerExe.empty()) {
        GTEST_SKIP() << "QuickView.exe not present next to test binary";
    }

    struct LuminanceStats {
        double meanNits = 0.0;
        double peakNits = 0.0;
        uint64_t pixelCount = 0;
    };

    auto measureUnorm16HdrNits = [](const LiveWorkerOutputHdr& res,
                                    QuickView::TransferFunction tf) -> LuminanceStats {
        LuminanceStats stats{};
        if (res.header.pixelFormat != static_cast<uint8_t>(QuickView::PixelFormat::R16G16B16A16_UNORM) ||
            res.header.width == 0 || res.header.height == 0 || res.pixelBytes.empty()) {
            return stats;
        }
        const bool isHlg = (tf == QuickView::TransferFunction::HLG);
        double totalNits = 0.0;
        for (uint32_t y = 0; y < res.header.height; ++y) {
            const uint16_t* row = reinterpret_cast<const uint16_t*>(
                res.pixelBytes.data() + static_cast<size_t>(y) * res.header.stride);
            for (uint32_t x = 0; x < res.header.width; ++x) {
                const float r = static_cast<float>(row[x * 4u + 0u]) / 65535.0f;
                const float g = static_cast<float>(row[x * 4u + 1u]) / 65535.0f;
                const float b = static_cast<float>(row[x * 4u + 2u]) / 65535.0f;
                double lumNits = 0.0;
                if (isHlg) {
                    const float er = ColorMath::HlgChannelInverseOetf(r);
                    const float eg = ColorMath::HlgChannelInverseOetf(g);
                    const float eb = ColorMath::HlgChannelInverseOetf(b);
                    const float ys = (std::max)(0.0f, 0.2627f * er + 0.6780f * eg + 0.0593f * eb);
                    lumNits = (ys > 0.0f) ? (std::pow(static_cast<double>(ys), 1.2) * 1000.0) : 0.0;
                } else {
                    const double lr = static_cast<double>(ColorMath::PqToLinearScRgb(r)) * 80.0;
                    const double lg = static_cast<double>(ColorMath::PqToLinearScRgb(g)) * 80.0;
                    const double lb = static_cast<double>(ColorMath::PqToLinearScRgb(b)) * 80.0;
                    lumNits = 0.2627 * lr + 0.6780 * lg + 0.0593 * lb;
                }
                totalNits += lumNits;
                if (lumNits > stats.peakNits) {
                    stats.peakNits = lumNits;
                }
                ++stats.pixelCount;
            }
        }
        if (stats.pixelCount > 0) {
            stats.meanNits = totalNits / static_cast<double>(stats.pixelCount);
        }
        return stats;
    };

    // 1. sunrise-PQ.jxl vs sunrise-PQ.png (XYB PQ Rec.2020 physical luminance parity)
    if (const auto jxlPath = FindHdrTestAsset(L"sunrise-PQ.jxl"); !jxlPath.empty()) {
        LiveWorkerOutputHdr jxlRes;
        ASSERT_NO_FATAL_FAILURE(ExecuteDecodeWorkerSubprocessWithOptions(
            workerExe, jxlPath, L"jxl_sunrise", 1024, 1024, 16ull, -1.0f, true, &jxlRes));
        ASSERT_EQ(jxlRes.exitCode, 0u);
        ASSERT_HRESULT_SUCCEEDED(static_cast<HRESULT>(jxlRes.header.hr));
        EXPECT_EQ(jxlRes.header.pixelFormat, static_cast<uint8_t>(QuickView::PixelFormat::R16G16B16A16_UNORM));
        EXPECT_EQ(jxlRes.header.colorDataSpace, static_cast<uint8_t>(QuickView::PixelDataSpace::EncodedHdr));
        EXPECT_EQ(jxlRes.header.colorTransfer, static_cast<uint8_t>(QuickView::TransferFunction::PQ));
        EXPECT_EQ(jxlRes.header.colorPrimaries, static_cast<uint8_t>(QuickView::ColorPrimaries::Rec2020));
        EXPECT_TRUE(UnpackHdrFlagsTestHelper(jxlRes.header.hdrFlags).isHdr);

        const LuminanceStats jxlStats = measureUnorm16HdrNits(jxlRes, QuickView::TransferFunction::PQ);
        EXPECT_GE(jxlStats.peakNits, 1300.0);
        EXPECT_LE(jxlStats.peakNits, 1800.0);

        if (const auto pngPath = FindHdrTestAsset(L"sunrise-PQ.png"); !pngPath.empty()) {
            LiveWorkerOutputHdr pngRes;
            ASSERT_NO_FATAL_FAILURE(ExecuteDecodeWorkerSubprocessWithOptions(
                workerExe, pngPath, L"png_sunrise", 1024, 1024, 16ull, -1.0f, true, &pngRes));
            ASSERT_EQ(pngRes.exitCode, 0u);
            ASSERT_HRESULT_SUCCEEDED(static_cast<HRESULT>(pngRes.header.hr));
            const LuminanceStats pngStats = measureUnorm16HdrNits(pngRes, QuickView::TransferFunction::PQ);
            ASSERT_GT(pngStats.meanNits, 10.0);
            ASSERT_GT(pngStats.peakNits, 100.0);
            const double meanRelErr = std::abs(jxlStats.meanNits - pngStats.meanNits) / pngStats.meanNits;
            const double peakRelErr = std::abs(jxlStats.peakNits - pngStats.peakNits) / pngStats.peakNits;
            EXPECT_LT(meanRelErr, 0.05) << "jxl mean=" << jxlStats.meanNits << " png mean=" << pngStats.meanNits;
            EXPECT_LT(peakRelErr, 0.10) << "jxl peak=" << jxlStats.peakNits << " png peak=" << pngStats.peakNits;
        }
    }

    // 2. test_pattern-PQ.jxl vs test_pattern-PQ.png (Non-XYB PQ Rec.2020)
    if (const auto jxlPqPath = FindHdrTestAsset(L"test_pattern-PQ.jxl"); !jxlPqPath.empty()) {
        LiveWorkerOutputHdr jxlRes;
        ASSERT_NO_FATAL_FAILURE(ExecuteDecodeWorkerSubprocessWithOptions(
            workerExe, jxlPqPath, L"jxl_tp_pq", 1024, 1024, 16ull, -1.0f, true, &jxlRes));
        ASSERT_EQ(jxlRes.exitCode, 0u);
        ASSERT_HRESULT_SUCCEEDED(static_cast<HRESULT>(jxlRes.header.hr));
        EXPECT_EQ(jxlRes.header.pixelFormat, static_cast<uint8_t>(QuickView::PixelFormat::R16G16B16A16_UNORM));
        EXPECT_EQ(jxlRes.header.colorDataSpace, static_cast<uint8_t>(QuickView::PixelDataSpace::EncodedHdr));
        EXPECT_EQ(jxlRes.header.colorTransfer, static_cast<uint8_t>(QuickView::TransferFunction::PQ));
        EXPECT_EQ(jxlRes.header.colorPrimaries, static_cast<uint8_t>(QuickView::ColorPrimaries::Rec2020));
        EXPECT_TRUE(UnpackHdrFlagsTestHelper(jxlRes.header.hdrFlags).isHdr);

        const LuminanceStats jxlStats = measureUnorm16HdrNits(jxlRes, QuickView::TransferFunction::PQ);
        EXPECT_GE(jxlStats.peakNits, 9500.0);

        if (const auto pngPath = FindHdrTestAsset(L"test_pattern-PQ.png"); !pngPath.empty()) {
            LiveWorkerOutputHdr pngRes;
            ASSERT_NO_FATAL_FAILURE(ExecuteDecodeWorkerSubprocessWithOptions(
                workerExe, pngPath, L"png_tp_pq", 1024, 1024, 16ull, -1.0f, true, &pngRes));
            ASSERT_EQ(pngRes.exitCode, 0u);
            ASSERT_HRESULT_SUCCEEDED(static_cast<HRESULT>(pngRes.header.hr));
            const LuminanceStats pngStats = measureUnorm16HdrNits(pngRes, QuickView::TransferFunction::PQ);
            ASSERT_GT(pngStats.meanNits, 10.0);
            const double meanRelErr = std::abs(jxlStats.meanNits - pngStats.meanNits) / pngStats.meanNits;
            EXPECT_LT(meanRelErr, 0.02) << "jxl mean=" << jxlStats.meanNits << " png mean=" << pngStats.meanNits;
        }
    }

    // 3. test_pattern-HLG.jxl vs test_pattern-HLG.png (Non-XYB HLG Rec.2020)
    if (const auto jxlHlgPath = FindHdrTestAsset(L"test_pattern-HLG.jxl"); !jxlHlgPath.empty()) {
        LiveWorkerOutputHdr jxlRes;
        ASSERT_NO_FATAL_FAILURE(ExecuteDecodeWorkerSubprocessWithOptions(
            workerExe, jxlHlgPath, L"jxl_tp_hlg", 1024, 1024, 16ull, -1.0f, true, &jxlRes));
        ASSERT_EQ(jxlRes.exitCode, 0u);
        ASSERT_HRESULT_SUCCEEDED(static_cast<HRESULT>(jxlRes.header.hr));
        EXPECT_EQ(jxlRes.header.pixelFormat, static_cast<uint8_t>(QuickView::PixelFormat::R16G16B16A16_UNORM));
        EXPECT_EQ(jxlRes.header.colorDataSpace, static_cast<uint8_t>(QuickView::PixelDataSpace::EncodedHdr));
        EXPECT_EQ(jxlRes.header.colorTransfer, static_cast<uint8_t>(QuickView::TransferFunction::HLG));
        EXPECT_EQ(jxlRes.header.colorPrimaries, static_cast<uint8_t>(QuickView::ColorPrimaries::Rec2020));
        EXPECT_TRUE(UnpackHdrFlagsTestHelper(jxlRes.header.hdrFlags).isHdr);

        const LuminanceStats jxlStats = measureUnorm16HdrNits(jxlRes, QuickView::TransferFunction::HLG);
        EXPECT_GE(jxlStats.peakNits, 950.0);

        if (const auto pngPath = FindHdrTestAsset(L"test_pattern-HLG.png"); !pngPath.empty()) {
            LiveWorkerOutputHdr pngRes;
            ASSERT_NO_FATAL_FAILURE(ExecuteDecodeWorkerSubprocessWithOptions(
                workerExe, pngPath, L"png_tp_hlg", 1024, 1024, 16ull, -1.0f, true, &pngRes));
            ASSERT_EQ(pngRes.exitCode, 0u);
            ASSERT_HRESULT_SUCCEEDED(static_cast<HRESULT>(pngRes.header.hr));
            const LuminanceStats pngStats = measureUnorm16HdrNits(pngRes, QuickView::TransferFunction::HLG);
            ASSERT_GT(pngStats.meanNits, 10.0);
            const double meanRelErr = std::abs(jxlStats.meanNits - pngStats.meanNits) / pngStats.meanNits;
            EXPECT_LT(meanRelErr, 0.02) << "jxl mean=" << jxlStats.meanNits << " png mean=" << pngStats.meanNits;
        }
    }

    // 4. jxl-rec-2020-16bit.jxl -> PQ Rec.2020 peak > 500 nits
    if (const auto rec2020Jxl = FindHdrTestAsset(L"jxl-rec-2020-16bit.jxl"); !rec2020Jxl.empty()) {
        LiveWorkerOutputHdr jxlRes;
        ASSERT_NO_FATAL_FAILURE(ExecuteDecodeWorkerSubprocessWithOptions(
            workerExe, rec2020Jxl, L"jxl_rec2020_pq", 1024, 1024, 16ull, -1.0f, true, &jxlRes));
        ASSERT_EQ(jxlRes.exitCode, 0u);
        ASSERT_HRESULT_SUCCEEDED(static_cast<HRESULT>(jxlRes.header.hr));
        EXPECT_EQ(jxlRes.header.pixelFormat, static_cast<uint8_t>(QuickView::PixelFormat::R16G16B16A16_UNORM));
        EXPECT_EQ(jxlRes.header.colorDataSpace, static_cast<uint8_t>(QuickView::PixelDataSpace::EncodedHdr));
        EXPECT_EQ(jxlRes.header.colorTransfer, static_cast<uint8_t>(QuickView::TransferFunction::PQ));
        EXPECT_EQ(jxlRes.header.colorPrimaries, static_cast<uint8_t>(QuickView::ColorPrimaries::Rec2020));
        EXPECT_TRUE(UnpackHdrFlagsTestHelper(jxlRes.header.hdrFlags).isHdr);

        const LuminanceStats jxlStats = measureUnorm16HdrNits(jxlRes, QuickView::TransferFunction::PQ);
        EXPECT_GT(jxlStats.peakNits, 500.0);
    }

    // 5. Real sRGB JXL baseline
    if (const auto srgbPath = FindHdrTestAsset(L"test_pattern-sRGB.jxl"); !srgbPath.empty()) {
        LiveWorkerOutputHdr res;
        ASSERT_NO_FATAL_FAILURE(ExecuteDecodeWorkerSubprocessWithOptions(
            workerExe, srgbPath, L"jxl_srgb", 1024, 1024, 16ull, -1.0f, false, &res));
        ASSERT_EQ(res.exitCode, 0u);
        ASSERT_HRESULT_SUCCEEDED(static_cast<HRESULT>(res.header.hr));
        EXPECT_FALSE(UnpackHdrFlagsTestHelper(res.header.hdrFlags).isHdr);
    }

    // 6. Synthetic JXL round-trip tests (XYB HLG Rec.2020 + Non-XYB 16-bit SDR sRGB)
    auto encodeTempJxl = [](uint32_t width, uint32_t height, bool usesOriginalProfile,
                            const JxlColorEncoding& colorEnc, const std::vector<uint16_t>& rgb16,
                            const std::filesystem::path& outPath) -> bool {
        JxlEncoder* enc = JxlEncoderCreate(nullptr);
        if (!enc) return false;
        struct EncGuard {
            JxlEncoder* e;
            ~EncGuard() { JxlEncoderDestroy(e); }
        } guard{enc};

        JxlBasicInfo basicInfo{};
        JxlEncoderInitBasicInfo(&basicInfo);
        basicInfo.xsize = width;
        basicInfo.ysize = height;
        basicInfo.bits_per_sample = 16;
        basicInfo.exponent_bits_per_sample = 0;
        basicInfo.uses_original_profile = usesOriginalProfile ? JXL_TRUE : JXL_FALSE;
        if (JXL_ENC_SUCCESS != JxlEncoderSetBasicInfo(enc, &basicInfo)) return false;
        if (JXL_ENC_SUCCESS != JxlEncoderSetColorEncoding(enc, &colorEnc)) return false;

        JxlEncoderFrameSettings* frameSettings = JxlEncoderFrameSettingsCreate(enc, nullptr);
        if (!frameSettings) return false;
        if (usesOriginalProfile) {
            if (JXL_ENC_SUCCESS != JxlEncoderSetFrameLossless(frameSettings, JXL_TRUE)) return false;
        } else {
            if (JXL_ENC_SUCCESS != JxlEncoderSetFrameLossless(frameSettings, JXL_FALSE)) return false;
            if (JXL_ENC_SUCCESS != JxlEncoderSetFrameDistance(frameSettings, 0.5f)) return false;
        }

        JxlPixelFormat pxFmt = {3, JXL_TYPE_UINT16, JXL_LITTLE_ENDIAN, 0};
        const size_t rawBytes = rgb16.size() * sizeof(uint16_t);
        if (JXL_ENC_SUCCESS != JxlEncoderAddImageFrame(frameSettings, &pxFmt, rgb16.data(), rawBytes)) {
            return false;
        }
        JxlEncoderCloseInput(enc);

        std::vector<uint8_t> compressed(4096);
        uint8_t* nextOut = compressed.data();
        size_t availOut = compressed.size();
        for (;;) {
            const JxlEncoderStatus st = JxlEncoderProcessOutput(enc, &nextOut, &availOut);
            if (st == JXL_ENC_NEED_MORE_OUTPUT) {
                const size_t offset = static_cast<size_t>(nextOut - compressed.data());
                compressed.resize(compressed.size() * 2u);
                nextOut = compressed.data() + offset;
                availOut = compressed.size() - offset;
            } else if (st == JXL_ENC_SUCCESS) {
                compressed.resize(static_cast<size_t>(nextOut - compressed.data()));
                break;
            } else {
                return false;
            }
        }

        std::ofstream ofs(outPath, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) return false;
        ofs.write(reinterpret_cast<const char*>(compressed.data()), static_cast<std::streamsize>(compressed.size()));
        return ofs.good();
    };

    struct TempFileCleanup {
        std::filesystem::path path;
        ~TempFileCleanup() {
            if (!path.empty()) {
                std::error_code ec;
                std::filesystem::remove(path, ec);
            }
        }
    };

    const std::filesystem::path tempDir = std::filesystem::temp_directory_path();
    const DWORD pid = GetCurrentProcessId();

    // 6a. Synthetic XYB HLG Rec.2020 round-trip
    {
        constexpr uint32_t w = 64;
        constexpr uint32_t h = 64;
        const uint16_t darkCode = static_cast<uint16_t>(std::lround(0.25 * 65535.0));
        const uint16_t brightCode = static_cast<uint16_t>(std::lround(0.90 * 65535.0));
        std::vector<uint16_t> pixels(static_cast<size_t>(w) * h * 3u);
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                const uint16_t v = (x < w / 2u) ? darkCode : brightCode;
                const size_t idx = (static_cast<size_t>(y) * w + x) * 3u;
                pixels[idx + 0] = v;
                pixels[idx + 1] = v;
                pixels[idx + 2] = v;
            }
        }

        JxlColorEncoding hlgEnc{};
        hlgEnc.color_space = JXL_COLOR_SPACE_RGB;
        hlgEnc.white_point = JXL_WHITE_POINT_D65;
        hlgEnc.primaries = JXL_PRIMARIES_2100;
        hlgEnc.transfer_function = JXL_TRANSFER_FUNCTION_HLG;
        hlgEnc.rendering_intent = JXL_RENDERING_INTENT_RELATIVE;

        const std::filesystem::path tempPath =
            tempDir / (L"qv_synth_xyb_hlg_" + std::to_wstring(pid) + L".jxl");
        TempFileCleanup cleanup{tempPath};
        ASSERT_TRUE(encodeTempJxl(w, h, false, hlgEnc, pixels, tempPath));

        LiveWorkerOutputHdr res;
        ASSERT_NO_FATAL_FAILURE(ExecuteDecodeWorkerSubprocessWithOptions(
            workerExe, tempPath, L"jxl_synth_hlg", 128, 128, 16ull, -1.0f, true, &res));
        ASSERT_EQ(res.exitCode, 0u);
        ASSERT_HRESULT_SUCCEEDED(static_cast<HRESULT>(res.header.hr));
        EXPECT_EQ(res.header.pixelFormat, static_cast<uint8_t>(QuickView::PixelFormat::R16G16B16A16_UNORM));
        EXPECT_EQ(res.header.colorDataSpace, static_cast<uint8_t>(QuickView::PixelDataSpace::EncodedHdr));
        EXPECT_EQ(res.header.colorTransfer, static_cast<uint8_t>(QuickView::TransferFunction::HLG));
        EXPECT_EQ(res.header.colorPrimaries, static_cast<uint8_t>(QuickView::ColorPrimaries::Rec2020));
        EXPECT_TRUE(UnpackHdrFlagsTestHelper(res.header.hdrFlags).isHdr);

        ASSERT_GE(res.pixelBytes.size(), static_cast<size_t>(res.header.height) * res.header.stride);
        const uint16_t* centerRow = reinterpret_cast<const uint16_t*>(
            res.pixelBytes.data() + static_cast<size_t>(h / 2u) * res.header.stride);
        const uint32_t brightX = (w * 3u) / 4u;
        const float outR = static_cast<float>(centerRow[brightX * 4u + 0u]) / 65535.0f;
        const float outG = static_cast<float>(centerRow[brightX * 4u + 1u]) / 65535.0f;
        const float outB = static_cast<float>(centerRow[brightX * 4u + 2u]) / 65535.0f;
        EXPECT_NEAR(outR, 0.90f, 0.02f);
        EXPECT_NEAR(outG, 0.90f, 0.02f);
        EXPECT_NEAR(outB, 0.90f, 0.02f);
    }

    // 6b. Synthetic Non-XYB 16-bit SDR sRGB must NOT be mislabeled SceneLinear
    {
        constexpr uint32_t w = 32;
        constexpr uint32_t h = 32;
        std::vector<uint16_t> pixels(static_cast<size_t>(w) * h * 3u, 50000u);

        JxlColorEncoding srgbEnc{};
        srgbEnc.color_space = JXL_COLOR_SPACE_RGB;
        srgbEnc.white_point = JXL_WHITE_POINT_D65;
        srgbEnc.primaries = JXL_PRIMARIES_SRGB;
        srgbEnc.transfer_function = JXL_TRANSFER_FUNCTION_SRGB;
        srgbEnc.rendering_intent = JXL_RENDERING_INTENT_PERCEPTUAL;

        const std::filesystem::path tempPath =
            tempDir / (L"qv_synth_sdr16_" + std::to_wstring(pid) + L".jxl");
        TempFileCleanup cleanup{tempPath};
        ASSERT_TRUE(encodeTempJxl(w, h, true, srgbEnc, pixels, tempPath));

        LiveWorkerOutputHdr res;
        ASSERT_NO_FATAL_FAILURE(ExecuteDecodeWorkerSubprocessWithOptions(
            workerExe, tempPath, L"jxl_synth_sdr16", 128, 128, 16ull, 2.0f, true, &res));
        ASSERT_EQ(res.exitCode, 0u);
        ASSERT_HRESULT_SUCCEEDED(static_cast<HRESULT>(res.header.hr));
        EXPECT_EQ(res.header.colorDataSpace, static_cast<uint8_t>(QuickView::PixelDataSpace::EncodedSdr));
        EXPECT_EQ(res.header.colorTransfer, static_cast<uint8_t>(QuickView::TransferFunction::SRGB));
        const auto flags = UnpackHdrFlagsTestHelper(res.header.hdrFlags);
        EXPECT_FALSE(flags.isHdr);
        EXPECT_FALSE(flags.isSceneLinear);
    }
}

TEST(HdrPipelineTests, RealHeicColorMetadataAndGainMapDecode) {
    const auto workerExe = FindWorkerExecutableForTests();
    if (workerExe.empty()) {
        GTEST_SKIP() << "QuickView.exe not present next to test binary";
    }

    bool testedAny = false;
    const wchar_t* heicNames[] = { L"eos_r8.hif", L"reference.HEIC" };
    for (size_t i = 0; i < 2; ++i) {
        const auto path = FindHdrTestAsset(heicNames[i]);
        if (path.empty()) continue;
        LiveWorkerOutputHdr res;
        ASSERT_NO_FATAL_FAILURE(ExecuteDecodeWorkerSubprocessWithOptions(
            workerExe, path, L"heic", 1024, 1024, 16ull, 2.5f, true, &res));
        if (static_cast<HRESULT>(res.header.hr) == WINCODEC_ERR_COMPONENTNOTFOUND) {
            GTEST_SKIP() << "WIC HEVC extension not installed";
        }
        testedAny = true;
        ASSERT_EQ(res.exitCode, 0u);
        ASSERT_HRESULT_SUCCEEDED(static_cast<HRESULT>(res.header.hr));
        const auto flags = UnpackHdrFlagsTestHelper(res.header.hdrFlags);

        if (flags.hasGainMap && !flags.isHdr) {
            EXPECT_EQ(res.header.pixelFormat, static_cast<uint8_t>(QuickView::PixelFormat::BGRA8888));
            EXPECT_EQ(res.header.colorDataSpace, static_cast<uint8_t>(QuickView::PixelDataSpace::EncodedSdr));
        }
        // Must not mislabel non-float GainMap/SDR HEIC as SceneLinearScRgb + SRGB
        if (res.header.hdrTransfer == static_cast<uint8_t>(QuickView::TransferFunction::PQ) ||
            res.header.hdrTransfer == static_cast<uint8_t>(QuickView::TransferFunction::HLG)) {
            EXPECT_TRUE(flags.isHdr);
            EXPECT_TRUE(res.header.colorDataSpace == static_cast<uint8_t>(QuickView::PixelDataSpace::EncodedHdr) ||
                        res.header.colorDataSpace == static_cast<uint8_t>(QuickView::PixelDataSpace::SceneLinearScRgb));
        }
    }
    if (!testedAny) {
        GTEST_SKIP() << "No HEIC/HIF assets available";
    }
}

TEST(HdrPipelineTests, Bt2100HlgOotfLuminanceCouplingAccuracy) {
    const float* hlgInvLut = ColorMath::GetHlg16ToSceneLinearLut();
    ASSERT_NE(hlgInvLut, nullptr);

    auto evalHlgScRgb = [&](uint16_t rCode, uint16_t gCode, uint16_t bCode, float* outRgb) {
        const float er = hlgInvLut[rCode];
        const float eg = hlgInvLut[gCode];
        const float eb = hlgInvLut[bCode];
        const float ys = 0.2627f * er + 0.6780f * eg + 0.0593f * eb;
        const float gain = (ys > 0.0f) ? (std::pow(ys, 0.2f) * 12.5f) : 0.0f;
        outRgb[0] = er * gain;
        outRgb[1] = eg * gain;
        outRgb[2] = eb * gain;
    };

    // Peak white (65535, 65535, 65535) -> Ys = 1.0 -> (12.5, 12.5, 12.5) scRGB (1000 nits)
    float whiteRgb[3] = {};
    evalHlgScRgb(65535u, 65535u, 65535u, whiteRgb);
    EXPECT_NEAR(whiteRgb[0], 12.5f, 0.02f);
    EXPECT_NEAR(whiteRgb[1], 12.5f, 0.02f);
    EXPECT_NEAR(whiteRgb[2], 12.5f, 0.02f);

    // Saturated red (65535, 0, 0) -> Ys = 0.2627 -> gain = 0.2627^0.2 * 12.5 (~9.566), NOT 12.5!
    float redRgb[3] = {};
    evalHlgScRgb(65535u, 0u, 0u, redRgb);
    const float expectedRed = std::pow(0.2627f, 0.2f) * 12.5f;
    EXPECT_NEAR(redRgb[0], expectedRed, 0.02f);
    EXPECT_LT(redRgb[0], 10.0f);
    EXPECT_NEAR(redRgb[1], 0.0f, 1e-6f);
    EXPECT_NEAR(redRgb[2], 0.0f, 1e-6f);

    // Mid-gray (~0.5 signal -> er ~= 1/12) -> Ys = er -> (1/12)^1.2 * 12.5 ~= 0.635
    float midRgb[3] = {};
    evalHlgScRgb(32768u, 32768u, 32768u, midRgb);
    const float midE = (std::pow(32768.0f / 65535.0f, 2.0f)) / 3.0f;
    const float expectedMid = std::pow(midE, 1.2f) * 12.5f;
    EXPECT_NEAR(midRgb[0], expectedMid, 0.01f);
}

namespace {

std::vector<uint8_t> BuildSyntheticRgbTiff(
    uint32_t width, uint32_t height, uint16_t bitsPerSample,
    uint16_t compression, uint16_t predictor, uint16_t sampleFormat,
    const std::vector<uint8_t>& pixelBytes,
    const std::vector<uint8_t>* iccProfile = nullptr) {
    auto appendU16LE = [](std::vector<uint8_t>& b, uint16_t v) {
        b.push_back(static_cast<uint8_t>(v & 0xFFu));
        b.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    };
    auto appendU32LE = [](std::vector<uint8_t>& b, uint32_t v) {
        b.push_back(static_cast<uint8_t>(v & 0xFFu));
        b.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
        b.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
        b.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
    };
    auto appendTag = [&](std::vector<uint8_t>& b, uint16_t tag, uint16_t type,
                         uint32_t count, uint32_t valOrOff) {
        appendU16LE(b, tag);
        appendU16LE(b, type);
        appendU32LE(b, count);
        appendU32LE(b, valOrOff);
    };

    const bool hasIcc = (iccProfile && !iccProfile->empty());
    const uint16_t numTags = hasIcc ? 13u : 12u;
    const uint32_t ifdOffset = 8u;
    const uint32_t afterIfd = ifdOffset + 2u + static_cast<uint32_t>(numTags) * 12u + 4u;
    const uint32_t bpsOffset = afterIfd;
    const uint32_t pixelOffset = bpsOffset + 6u;
    const uint32_t iccOffset = pixelOffset + static_cast<uint32_t>(pixelBytes.size());

    std::vector<uint8_t> tiff;
    tiff.push_back('I'); tiff.push_back('I');
    appendU16LE(tiff, 42u);
    appendU32LE(tiff, ifdOffset);

    appendU16LE(tiff, numTags);
    appendTag(tiff, 256u, 4u, 1u, width);                                   // ImageWidth
    appendTag(tiff, 257u, 4u, 1u, height);                                  // ImageLength
    appendTag(tiff, 258u, 3u, 3u, bpsOffset);                               // BitsPerSample
    appendTag(tiff, 259u, 3u, 1u, compression);                             // Compression
    appendTag(tiff, 262u, 3u, 1u, 2u);                                      // Photometric = RGB
    appendTag(tiff, 273u, 4u, 1u, pixelOffset);                             // StripOffsets
    appendTag(tiff, 277u, 3u, 1u, 3u);                                      // SamplesPerPixel
    appendTag(tiff, 278u, 4u, 1u, height);                                  // RowsPerStrip
    appendTag(tiff, 279u, 4u, 1u, static_cast<uint32_t>(pixelBytes.size())); // StripByteCounts
    appendTag(tiff, 284u, 3u, 1u, 1u);                                      // PlanarConfiguration
    appendTag(tiff, 317u, 3u, 1u, predictor);                               // Predictor
    appendTag(tiff, 339u, 3u, 1u, sampleFormat);                            // SampleFormat
    if (hasIcc) {
        appendTag(tiff, 34675u, 7u, static_cast<uint32_t>(iccProfile->size()), iccOffset);
    }
    appendU32LE(tiff, 0u); // next IFD

    appendU16LE(tiff, bitsPerSample);
    appendU16LE(tiff, bitsPerSample);
    appendU16LE(tiff, bitsPerSample);

    tiff.insert(tiff.end(), pixelBytes.begin(), pixelBytes.end());
    if (hasIcc) {
        tiff.insert(tiff.end(), iccProfile->begin(), iccProfile->end());
    }
    return tiff;
}

void FillToneMapMatrixFromColorMath(
    QuickView::ColorPrimaries srcPrimaries,
    QuickView::ColorPrimaries dstPrimaries,
    QuickView::ToneMapSettings* outSettings) {
    const ColorMath::ColorMatrix3 m = ColorMath::MultiplyColorMatrices(
        ColorMath::GetXyzToRgbMatrix(dstPrimaries),
        ColorMath::GetRgbToXyzMatrix(srcPrimaries));
    std::memset(outSettings->colorMatrix, 0, sizeof(outSettings->colorMatrix));
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            outSettings->colorMatrix[r * 4 + c] = m.m[r][c];
        }
    }
    outSettings->colorMatrix[15] = 1.0f;
}

} // namespace

TEST(HdrPipelineTests, MiniTiffRejectsFloatSampleFormatAndFixes16BitPredictor2) {
    QuickView::Codec::DecodeContext ctx;
    ctx.allocator.pfn = [](void*, size_t s) -> uint8_t* {
        return static_cast<uint8_t*>(_aligned_malloc(s, 64));
    };

    // 1. SampleFormat=3 (IEEE float) must return E_NOTIMPL so WIC float fallback handles it
    {
        const std::vector<uint8_t> raw(1 * 1 * 3 * 2, 0);
        const auto tiff = BuildSyntheticRgbTiff(1, 1, 16, 1, 1, 3, raw);
        QuickView::Codec::DecodeResult res;
        EXPECT_EQ(QuickView::MiniTiff::Load(tiff.data(), tiff.size(), ctx, res), E_NOTIMPL);
        if (res.pixels) _aligned_free(res.pixels);
    }

    // 2. 16-bit RGB Predictor=2: deltas {0x00FF, 0x0001, 0x00FF} accumulate to
    // 16-bit samples {0x00FF, 0x0100, 0x01FF} -> high bytes {0x00, 0x01, 0x01}.
    // Old byte-wise predictor failed to carry from LSB to MSB, giving {0x00, 0x00, 0x00}.
    {
        const uint16_t deltas[9] = {
            0x00FFu, 0x00FFu, 0x00FFu,
            0x0001u, 0x0001u, 0x0001u,
            0x00FFu, 0x00FFu, 0x00FFu
        };
        std::vector<uint8_t> raw(sizeof(deltas));
        std::memcpy(raw.data(), deltas, sizeof(deltas));
        const auto tiff = BuildSyntheticRgbTiff(3, 1, 16, 1, 2, 1, raw);
        QuickView::Codec::DecodeResult res;
        ASSERT_EQ(QuickView::MiniTiff::Load(tiff.data(), tiff.size(), ctx, res), S_OK);
        ASSERT_NE(res.pixels, nullptr);
        EXPECT_EQ(res.pixels[0 * 4 + 2], 0x00u);
        EXPECT_EQ(res.pixels[1 * 4 + 2], 0x01u);
        EXPECT_EQ(res.pixels[2 * 4 + 2], 0x01u);
        _aligned_free(res.pixels);
    }

    // 3. Embedded PQ CICP ICC inside TIFF sets EncodedHdr + PQ + Rec2020 + isHdr
    {
        const auto icc = BuildSyntheticIccCicp(9, 16, 0, 1); // Rec.2020 + PQ
        const std::vector<uint8_t> raw = { 128u, 128u, 128u };
        const auto tiff = BuildSyntheticRgbTiff(1, 1, 8, 1, 1, 1, raw, &icc);
        QuickView::Codec::DecodeResult res;
        ASSERT_EQ(QuickView::MiniTiff::Load(tiff.data(), tiff.size(), ctx, res), S_OK);
        ASSERT_NE(res.pixels, nullptr);
        EXPECT_EQ(res.metadata.colorInfo.dataSpace, QuickView::PixelDataSpace::EncodedHdr);
        EXPECT_EQ(res.metadata.colorInfo.transfer, QuickView::TransferFunction::PQ);
        EXPECT_EQ(res.metadata.colorInfo.primaries, QuickView::ColorPrimaries::Rec2020);
        EXPECT_TRUE(res.metadata.hdrMetadata.isHdr);
        _aligned_free(res.pixels);
    }
}

TEST(HdrPipelineTests, Real32BitPsdAndExrDecodeAndAcesMatrix) {
    // 1. Verify ACES AP0 RGB<->XYZ invertible matrix pair
    {
        const ColorMath::ColorMatrix3 rgbToXyz = ColorMath::GetRgbToXyzMatrix(QuickView::ColorPrimaries::ACES);
        const ColorMath::ColorMatrix3 xyzToRgb = ColorMath::GetXyzToRgbMatrix(QuickView::ColorPrimaries::ACES);
        const ColorMath::ColorMatrix3 srgbToXyz = ColorMath::GetRgbToXyzMatrix(QuickView::ColorPrimaries::SRGB);
        EXPECT_GT(std::abs(rgbToXyz.m[0][0] - srgbToXyz.m[0][0]), 0.1f);
        const ColorMath::ColorMatrix3 prod = ColorMath::MultiplyColorMatrices(rgbToXyz, xyzToRgb);
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                EXPECT_NEAR(prod.m[r][c], (r == c) ? 1.0f : 0.0f, 1e-4f);
            }
        }
    }

    const auto workerExe = FindWorkerExecutableForTests();
    if (workerExe.empty()) {
        GTEST_SKIP() << "QuickView.exe not present next to test binary";
    }

    // 2. Real 32-bit float PSD (white-gradation-32bit.psd with 496-byte 0x040F ICC)
    if (const auto psdPath = FindHdrTestAsset(L"white-gradation-32bit.psd"); !psdPath.empty()) {
        LiveWorkerOutputHdr res;
        ASSERT_NO_FATAL_FAILURE(ExecuteDecodeWorkerSubprocessWithOptions(
            workerExe, psdPath, L"psd32", 512, 512, 16ull, 2.5f, true, &res));
        ASSERT_EQ(res.exitCode, 0u);
        ASSERT_HRESULT_SUCCEEDED(static_cast<HRESULT>(res.header.hr));
        EXPECT_EQ(res.header.pixelFormat, static_cast<uint8_t>(QuickView::PixelFormat::R32G32B32A32_FLOAT));
        EXPECT_EQ(res.header.colorDataSpace, static_cast<uint8_t>(QuickView::PixelDataSpace::SceneLinear));
        EXPECT_EQ(res.header.iccBytes, 496u);
        const auto flags = UnpackHdrFlagsTestHelper(res.header.hdrFlags);
        EXPECT_TRUE(flags.isHdr);
        EXPECT_TRUE(flags.isSceneLinear);

        float maxRgb = -1.0f;
        for (uint32_t y = 0; y < res.header.height; ++y) {
            const float* row = reinterpret_cast<const float*>(
                res.pixelBytes.data() + static_cast<size_t>(y) * res.header.stride);
            for (uint32_t x = 0; x < res.header.width; ++x) {
                for (int c = 0; c < 3; ++c) {
                    const float v = row[x * 4u + c];
                    EXPECT_TRUE(IsFiniteFloatBitsTest(v));
                    if (v > maxRgb) maxRgb = v;
                }
            }
        }
        EXPECT_GT(maxRgb, 0.5f);
    }

    // 3. Real EXR & Radiance HDR live decode
    const wchar_t* hdrAssets[] = { L"desk.exr", L"tree.hdr" };
    for (size_t idx = 0; idx < 2; ++idx) {
        const auto hdrPath = FindHdrTestAsset(hdrAssets[idx]);
        if (hdrPath.empty()) continue;
        LiveWorkerOutputHdr res;
        ASSERT_NO_FATAL_FAILURE(ExecuteDecodeWorkerSubprocessWithOptions(
            workerExe, hdrPath, L"hdr_scene", 512, 512, 16ull, -1.0f, true, &res));
        ASSERT_EQ(res.exitCode, 0u);
        ASSERT_HRESULT_SUCCEEDED(static_cast<HRESULT>(res.header.hr));
        EXPECT_EQ(res.header.colorDataSpace, static_cast<uint8_t>(QuickView::PixelDataSpace::SceneLinear)) << hdrAssets[idx];
        const auto flags = UnpackHdrFlagsTestHelper(res.header.hdrFlags);
        EXPECT_TRUE(flags.isHdr) << hdrAssets[idx];
        EXPECT_TRUE(flags.isSceneLinear) << hdrAssets[idx];

        float peakRgb = 0.0f;
        if (res.header.pixelFormat == static_cast<uint8_t>(QuickView::PixelFormat::R16G16B16A16_FLOAT)) {
            for (uint32_t y = 0; y < res.header.height; ++y) {
                const uint16_t* row = reinterpret_cast<const uint16_t*>(
                    res.pixelBytes.data() + static_cast<size_t>(y) * res.header.stride);
                for (uint32_t x = 0; x < res.header.width; ++x) {
                    for (int c = 0; c < 3; ++c) {
                        const float v = HalfToFloat(row[x * 4u + c]);
                        EXPECT_TRUE(IsFiniteFloatBitsTest(v));
                        if (v > peakRgb) peakRgb = v;
                    }
                }
            }
        } else if (res.header.pixelFormat == static_cast<uint8_t>(QuickView::PixelFormat::R32G32B32A32_FLOAT)) {
            for (uint32_t y = 0; y < res.header.height; ++y) {
                const float* row = reinterpret_cast<const float*>(
                    res.pixelBytes.data() + static_cast<size_t>(y) * res.header.stride);
                for (uint32_t x = 0; x < res.header.width; ++x) {
                    for (int c = 0; c < 3; ++c) {
                        const float v = row[x * 4u + c];
                        EXPECT_TRUE(IsFiniteFloatBitsTest(v));
                        if (v > peakRgb) peakRgb = v;
                    }
                }
            }
        }
        EXPECT_GT(peakRgb, 1.0f) << hdrAssets[idx];
    }
}

TEST(HdrPipelineTests, EndToEndHdrDisplayEmulationAllFormatsWarpReadback) {
    ComPtr<ID3D11Device> d3dDevice;
    ComPtr<ID3D11DeviceContext> d3dContext;
    if (FAILED(CreateD3D11DeviceWithWarpFallback(&d3dDevice, &d3dContext))) {
        GTEST_SKIP() << "D3D11 WARP unavailable";
    }

    QuickView::ComputeEngine compute;
    ASSERT_HRESULT_SUCCEEDED(compute.Initialize(d3dDevice.Get()));

    // Simulated HDR Display: SDR white = 240 nits (3.0 scRGB), Peak = 1400 nits (17.5 scRGB)
    constexpr float kHdrPeakScRgb = 1400.0f / 80.0f;
    constexpr float kSdrWhiteScale = 240.0f / 80.0f;

    // A. Absolute PQ Rec.2020 (1000 nits white + saturated Rec.2020 red, exposureGain = 1.0)
    {
        const uint16_t pq1000 = static_cast<uint16_t>(std::lround(NitsToPqSignal(1000.0f) * 65535.0f));
        const uint16_t srcPixels[2 * 4] = {
            pq1000, pq1000, pq1000, 65535u,
            pq1000, 0u, 0u, 65535u
        };
        QuickView::ToneMapSettings s = MakeIdentityToneMapSettings(kHdrPeakScRgb, QuickView::TransferFunction::PQ);
        s.exposureGain = 1.0f; // Absolute photometric: do NOT scale by 3.0 SDR white
        FillToneMapMatrixFromColorMath(QuickView::ColorPrimaries::Rec2020, QuickView::ColorPrimaries::SRGB, &s);

        ComPtr<ID3D11Texture2D> outTex;
        ASSERT_HRESULT_SUCCEEDED(compute.ToneMapHdrToHdr(
            reinterpret_cast<const uint8_t*>(srcPixels), 2, 1, 16, s,
            &outTex, QuickView::PixelFormat::R16G16B16A16_UNORM));
        std::vector<uint16_t> fp16;
        ASSERT_TRUE(ReadbackFp16Texture(d3dDevice.Get(), d3dContext.Get(), outTex.Get(), &fp16));
        EXPECT_NEAR(HalfToFloat(fp16[0]), 12.5f, 0.25f);
        EXPECT_NEAR(HalfToFloat(fp16[1]), 12.5f, 0.25f);
        EXPECT_NEAR(HalfToFloat(fp16[2]), 12.5f, 0.25f);
        // Saturated Rec.2020 red in scRGB preserves wide-gamut negative G and B
        EXPECT_GT(HalfToFloat(fp16[4]), 17.0f);
        EXPECT_LT(HalfToFloat(fp16[5]), -1.0f);
        EXPECT_LT(HalfToFloat(fp16[6]), -0.1f);
    }

    // B. Absolute HLG Rec.2020 (white = 12.5 scRGB, saturated red obeys Ys^0.2 OOTF, exposureGain = 1.0)
    {
        const uint16_t srcPixels[2 * 4] = {
            65535u, 65535u, 65535u, 65535u,
            65535u, 0u, 0u, 65535u
        };
        QuickView::ToneMapSettings s = MakeIdentityToneMapSettings(kHdrPeakScRgb, QuickView::TransferFunction::HLG);
        s.exposureGain = 1.0f;
        FillToneMapMatrixFromColorMath(QuickView::ColorPrimaries::Rec2020, QuickView::ColorPrimaries::SRGB, &s);

        ComPtr<ID3D11Texture2D> outTex;
        ASSERT_HRESULT_SUCCEEDED(compute.ToneMapHdrToHdr(
            reinterpret_cast<const uint8_t*>(srcPixels), 2, 1, 16, s,
            &outTex, QuickView::PixelFormat::R16G16B16A16_UNORM));
        std::vector<uint16_t> fp16;
        ASSERT_TRUE(ReadbackFp16Texture(d3dDevice.Get(), d3dContext.Get(), outTex.Get(), &fp16));
        EXPECT_NEAR(HalfToFloat(fp16[0]), 12.5f, 0.15f);
        EXPECT_NEAR(HalfToFloat(fp16[1]), 12.5f, 0.15f);
        EXPECT_NEAR(HalfToFloat(fp16[2]), 12.5f, 0.15f);
        EXPECT_GT(HalfToFloat(fp16[4]), 12.0f);
        EXPECT_LT(HalfToFloat(fp16[5]), -0.8f);
    }

    // C. Ultra HDR GainMap (Display P3 8-bit base + gain=255 -> ComposeGainMap -> ToneMapHdrToHdr with exposureGain=3.0)
    {
        const uint8_t sdrBgra[2 * 4] = {
            255u, 255u, 255u, 255u, // White in BGRA
            0u, 0u, 255u, 255u      // Pure P3 red in BGRA
        };
        const uint8_t gain[2] = { 255u, 255u };
        QuickView::GpuShaderPayload gmPayload{};
        gmPayload.gainMapMax[0] = gmPayload.gainMapMax[1] = gmPayload.gainMapMax[2] = 2.0f;
        gmPayload.gamma[0] = gmPayload.gamma[1] = gmPayload.gamma[2] = 1.0f;
        gmPayload.hdrCapacityMin = 0.0f;
        gmPayload.hdrCapacityMax = 2.0f;
        gmPayload.targetHeadroom = 2.0f;
        gmPayload.sdrWidth = 2u;
        gmPayload.sdrHeight = 1u;

        ComPtr<ID3D11Texture2D> composedTex;
        ASSERT_HRESULT_SUCCEEDED(compute.ComposeGainMap(
            sdrBgra, 2, 1, 8, QuickView::PixelFormat::BGRA8888,
            gain, 2, 1, 2, gmPayload, &composedTex));

        QuickView::ToneMapSettings s = MakeIdentityToneMapSettings(kHdrPeakScRgb, QuickView::TransferFunction::Linear);
        s.exposureGain = kSdrWhiteScale; // Relative GainMap base scaled by 240/80 = 3.0
        FillToneMapMatrixFromColorMath(QuickView::ColorPrimaries::DisplayP3, QuickView::ColorPrimaries::SRGB, &s);

        ComPtr<ID3D11Texture2D> finalTex;
        ASSERT_HRESULT_SUCCEEDED(compute.ToneMapHdrTextureToHdr(composedTex.Get(), s, &finalTex));

        std::vector<uint16_t> fp16;
        ASSERT_TRUE(ReadbackFp16Texture(d3dDevice.Get(), d3dContext.Get(), finalTex.Get(), &fp16));
        EXPECT_NEAR(HalfToFloat(fp16[0]), 12.0f, 0.2f);
        EXPECT_NEAR(HalfToFloat(fp16[1]), 12.0f, 0.2f);
        EXPECT_NEAR(HalfToFloat(fp16[2]), 12.0f, 0.2f);
        EXPECT_GT(HalfToFloat(fp16[4]), 12.0f);
        EXPECT_LT(HalfToFloat(fp16[5]), -0.2f);
    }

    // D. Relative Wide-Gamut SDR (Display P3 ICC -> D2D1ColorManagement(scRGB) + WhiteLevelAdjustment(240->80))
    const std::filesystem::path p3IccPath =
        std::filesystem::path(__FILE__).parent_path().parent_path() / "QuickView" / "ICC" / "DisplayP3.icc";
    std::vector<uint8_t> p3IccBytes;
    if (ReadFileBytes(p3IccPath, &p3IccBytes) && !p3IccBytes.empty()) {
        ComPtr<ID2D1Factory1> d2dFactory;
        ASSERT_HRESULT_SUCCEEDED(D2D1CreateFactory(
            D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr,
            reinterpret_cast<void**>(d2dFactory.GetAddressOf())));

        ComPtr<IDXGIDevice> dxgiDevice;
        ASSERT_HRESULT_SUCCEEDED(d3dDevice.As(&dxgiDevice));
        ComPtr<ID2D1Device> d2dDevice;
        ASSERT_HRESULT_SUCCEEDED(d2dFactory->CreateDevice(dxgiDevice.Get(), &d2dDevice));
        ComPtr<ID2D1DeviceContext> d2dCtx;
        ASSERT_HRESULT_SUCCEEDED(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dCtx));

        D3D11_TEXTURE2D_DESC rtDesc = {};
        rtDesc.Width = 2;
        rtDesc.Height = 1;
        rtDesc.MipLevels = 1;
        rtDesc.ArraySize = 1;
        rtDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        rtDesc.SampleDesc.Count = 1;
        rtDesc.Usage = D3D11_USAGE_DEFAULT;
        rtDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> rtTex;
        ASSERT_HRESULT_SUCCEEDED(d3dDevice->CreateTexture2D(&rtDesc, nullptr, &rtTex));

        ComPtr<IDXGISurface> rtSurface;
        ASSERT_HRESULT_SUCCEEDED(rtTex.As(&rtSurface));
        D2D1_BITMAP_PROPERTIES1 rtProps = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_R16G16B16A16_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED));
        ComPtr<ID2D1Bitmap1> rtBitmap;
        ASSERT_HRESULT_SUCCEEDED(d2dCtx->CreateBitmapFromDxgiSurface(rtSurface.Get(), &rtProps, &rtBitmap));

        ComPtr<ID2D1ColorContext> p3Ctx;
        ComPtr<ID2D1ColorContext> scRgbCtx;
        ASSERT_HRESULT_SUCCEEDED(d2dCtx->CreateColorContext(
            D2D1_COLOR_SPACE_CUSTOM, p3IccBytes.data(), static_cast<UINT32>(p3IccBytes.size()), &p3Ctx));
        ASSERT_HRESULT_SUCCEEDED(d2dCtx->CreateColorContext(D2D1_COLOR_SPACE_SCRGB, nullptr, 0, &scRgbCtx));

        const uint8_t p3Bgra[2 * 4] = {
            255u, 255u, 255u, 255u, // P3 White
            0u, 0u, 255u, 255u      // P3 Saturated Red
        };
        D2D1_BITMAP_PROPERTIES1 srcProps = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f, p3Ctx.Get());
        ComPtr<ID2D1Bitmap1> p3Bitmap;
        ASSERT_HRESULT_SUCCEEDED(d2dCtx->CreateBitmap(D2D1::SizeU(2, 1), p3Bgra, 8, &srcProps, &p3Bitmap));

        ComPtr<ID2D1Effect> colorMgmt;
        ComPtr<ID2D1Effect> whiteLevel;
        ASSERT_HRESULT_SUCCEEDED(d2dCtx->CreateEffect(CLSID_D2D1ColorManagement, &colorMgmt));
        ASSERT_HRESULT_SUCCEEDED(d2dCtx->CreateEffect(CLSID_D2D1WhiteLevelAdjustment, &whiteLevel));

        colorMgmt->SetInput(0, p3Bitmap.Get());
        ASSERT_HRESULT_SUCCEEDED(colorMgmt->SetValue(D2D1_COLORMANAGEMENT_PROP_SOURCE_COLOR_CONTEXT, p3Ctx.Get()));
        ASSERT_HRESULT_SUCCEEDED(colorMgmt->SetValue(D2D1_COLORMANAGEMENT_PROP_DESTINATION_COLOR_CONTEXT, scRgbCtx.Get()));
        ASSERT_HRESULT_SUCCEEDED(colorMgmt->SetValue(D2D1_COLORMANAGEMENT_PROP_QUALITY, D2D1_COLORMANAGEMENT_QUALITY_BEST));

        whiteLevel->SetInputEffect(0, colorMgmt.Get());
        ASSERT_HRESULT_SUCCEEDED(whiteLevel->SetValue(D2D1_WHITELEVELADJUSTMENT_PROP_INPUT_WHITE_LEVEL, 240.0f));
        ASSERT_HRESULT_SUCCEEDED(whiteLevel->SetValue(D2D1_WHITELEVELADJUSTMENT_PROP_OUTPUT_WHITE_LEVEL, 80.0f));

        d2dCtx->SetTarget(rtBitmap.Get());
        d2dCtx->BeginDraw();
        d2dCtx->Clear(D2D1::ColorF(0, 0, 0, 0));
        d2dCtx->DrawImage(whiteLevel.Get());
        ASSERT_HRESULT_SUCCEEDED(d2dCtx->EndDraw());
        d2dCtx->SetTarget(nullptr);

        std::vector<uint16_t> fp16;
        ASSERT_TRUE(ReadbackFp16Texture(d3dDevice.Get(), d3dContext.Get(), rtTex.Get(), &fp16));
        EXPECT_NEAR(HalfToFloat(fp16[0]), 3.0f, 0.08f);
        EXPECT_NEAR(HalfToFloat(fp16[1]), 3.0f, 0.08f);
        EXPECT_NEAR(HalfToFloat(fp16[2]), 3.0f, 0.08f);
        // P3 red in scRGB: ~ (1.225 * 3, -0.042 * 3, -0.020 * 3) = (3.67, -0.13, -0.06)
        EXPECT_GT(HalfToFloat(fp16[4]), 3.4f);
        EXPECT_LT(HalfToFloat(fp16[5]), -0.05f);
    }
}

TEST(HdrPipelineTests, SwizzleRGBAToBGRAOnlyDoesNotDoublePremultiply) {
    // 2x1 RGBA pixels with straight/premultiplied semi-transparent values
    uint8_t px[8] = {
        200, 100, 50, 128,
        10, 20, 30, 64
    };
    ImageLoaderSimd::SwizzleRGBAToBGRAOnly(px, 2);
    // R and B swapped, G and A preserved, NO multiplication by alpha
    EXPECT_EQ(px[0], 50);
    EXPECT_EQ(px[1], 100);
    EXPECT_EQ(px[2], 200);
    EXPECT_EQ(px[3], 128);
    EXPECT_EQ(px[4], 30);
    EXPECT_EQ(px[5], 20);
    EXPECT_EQ(px[6], 10);
    EXPECT_EQ(px[7], 64);
}

TEST(HdrPipelineTests, ToneMap8BitPremulAndRgbGainMap) {
    ComPtr<ID3D11Device> d3dDevice;
    ComPtr<ID3D11DeviceContext> d3dContext;
    if (FAILED(CreateD3D11DeviceWithWarpFallback(&d3dDevice, &d3dContext))) {
        GTEST_SKIP() << "D3D11 WARP unavailable";
    }

    QuickView::ComputeEngine compute;
    ASSERT_HRESULT_SUCCEEDED(compute.Initialize(d3dDevice.Get()));

    // 1. 8-bit premultiplied SDR-white pixel with alpha=0.5 (128/255) in sRGB transfer:
    //    Straight color is white (1.0), premultiplied is 128 -> unpremultiplied -> 1.0 -> premul output ~128.
    {
        const uint8_t premulBgra[4] = { 128u, 128u, 128u, 128u };
        QuickView::ToneMapSettings s = MakeIdentityToneMapSettings(1.0f, QuickView::TransferFunction::SRGB);
        s.inputPremultiplied = 1u;
        ComPtr<ID3D11Texture2D> outTex;
        ASSERT_HRESULT_SUCCEEDED(compute.ToneMapHdrToSdr(
            premulBgra, 1, 1, 4, s, &outTex, QuickView::PixelFormat::BGRA8888));

        D3D11_TEXTURE2D_DESC desc{};
        outTex->GetDesc(&desc);
        D3D11_TEXTURE2D_DESC stgDesc = desc;
        stgDesc.Usage = D3D11_USAGE_STAGING;
        stgDesc.BindFlags = 0;
        stgDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> stg;
        ASSERT_HRESULT_SUCCEEDED(d3dDevice->CreateTexture2D(&stgDesc, nullptr, &stg));
        d3dContext->CopyResource(stg.Get(), outTex.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        ASSERT_HRESULT_SUCCEEDED(d3dContext->Map(stg.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        const uint8_t* p = static_cast<const uint8_t*>(mapped.pData);
        EXPECT_NEAR(p[0], 128, 3);
        EXPECT_NEAR(p[1], 128, 3);
        EXPECT_NEAR(p[2], 128, 3);
        EXPECT_NEAR(p[3], 128, 1);
        d3dContext->Unmap(stg.Get(), 0);
    }

    // 2. 4-bpp RGB GainMap applies distinct per-channel gains
    {
        const uint8_t sdrBgra[4] = { 255u, 255u, 255u, 255u };
        const uint8_t rgbGain[4] = { 255u, 0u, 128u, 255u }; // R=max, G=min, B=mid
        QuickView::GpuShaderPayload payload{};
        payload.gainMapMin[0] = payload.gainMapMin[1] = payload.gainMapMin[2] = 0.0f;
        payload.gainMapMax[0] = 2.0f;
        payload.gainMapMax[1] = 0.0f;
        payload.gainMapMax[2] = 1.0f;
        payload.gamma[0] = payload.gamma[1] = payload.gamma[2] = 1.0f;
        payload.hdrCapacityMin = 0.0f;
        payload.hdrCapacityMax = 2.0f;
        payload.targetHeadroom = 2.0f;
        payload.baseIsHdr = 0.0f;
        payload.sdrWidth = 1u;
        payload.sdrHeight = 1u;

        ComPtr<ID3D11Texture2D> outTex;
        ASSERT_HRESULT_SUCCEEDED(compute.ComposeGainMap(
            sdrBgra, 1, 1, 4, QuickView::PixelFormat::BGRA8888,
            rgbGain, 1, 1, 4, payload, &outTex, nullptr, nullptr, 4));
        std::vector<uint16_t> fp16;
        ASSERT_TRUE(ReadbackFp16Texture(d3dDevice.Get(), d3dContext.Get(), outTex.Get(), &fp16));
        EXPECT_NEAR(HalfToFloat(fp16[0]), 4.0f, 0.05f);                       // R: 2^2 = 4.0
        EXPECT_NEAR(HalfToFloat(fp16[1]), 1.0f, 0.02f);                       // G: 2^0 = 1.0
        EXPECT_NEAR(HalfToFloat(fp16[2]), std::exp2(128.0f / 255.0f), 0.02f); // B: 2^(128/255) ~ 1.416
    }
}
