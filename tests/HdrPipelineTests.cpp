#include <gtest/gtest.h>

#include "pch.h"
#include "ColorMath.h"
#include "ComputeEngine.h"
#include "DisplayColorInfo.h"
#include "ImageLoaderSimd.h"
#include "ImageTypes.h"
#include "WuffsLoader.h"

#include <d2d1_1.h>
#include <d2d1effects.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
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

    for (size_t i = 0; i < std::size(kSamples); ++i) {
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
        EXPECT_LT(std::abs(dHue), 1.5f) << c.name;
    }

    // Gold highlight must keep warm saturation (not washed out to white 255,254,254)
    EXPECT_EQ(rgba8[4u * 4u + 0u], 255u);
    EXPECT_LT(rgba8[4u * 4u + 2u], 185u);

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
