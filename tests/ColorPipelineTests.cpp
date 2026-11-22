#include "pch.h"
#include <gtest/gtest.h>

#include "ImageTypes.h"
#include "MiniTiff.h"
#include "TileManager.h"
#include "ToolProcessProtocol.h"
#include "WuffsLoader.h"

#include <zlib.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

void WriteU32BE(uint8_t* dst, uint32_t v) {
    dst[0] = static_cast<uint8_t>((v >> 24) & 0xFFu);
    dst[1] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    dst[2] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    dst[3] = static_cast<uint8_t>(v & 0xFFu);
}

void AppendU16LE(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFFu));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
}

void AppendU32LE(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFFu));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
}

std::vector<uint8_t> BuildSyntheticIccProfile(size_t totalSize) {
    if (totalSize < 132) totalSize = 132;
    std::vector<uint8_t> icc(totalSize, 0);
    WriteU32BE(icc.data(), static_cast<uint32_t>(totalSize));
    std::memcpy(icc.data() + 36, "acsp", 4);
    icc[12] = 'm';
    icc[13] = 'n';
    icc[14] = 't';
    icc[15] = 'r';
    icc[16] = 'R';
    icc[17] = 'G';
    icc[18] = 'B';
    icc[19] = ' ';
    for (size_t i = 132; i < totalSize; ++i) {
        icc[i] = static_cast<uint8_t>((i * 131u + 17u) & 0xFFu);
    }
    return icc;
}

void AppendPngChunk(std::vector<uint8_t>& png, const char type[4],
                    const uint8_t* payload, size_t len) {
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

std::vector<uint8_t> BuildSyntheticRgbaPng(uint32_t width, uint32_t height,
                                           uint8_t bitDepth,
                                           const std::vector<uint8_t>& iccProfile) {
    static constexpr uint8_t kPngSig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> png(kPngSig, kPngSig + 8);

    uint8_t ihdr[13] = {};
    WriteU32BE(ihdr, width);
    WriteU32BE(ihdr + 4, height);
    ihdr[8] = bitDepth; // 8 or 16
    ihdr[9] = 6;        // RGBA
    AppendPngChunk(png, "IHDR", ihdr, sizeof(ihdr));

    if (!iccProfile.empty()) {
        std::vector<uint8_t> compressed(compressBound(static_cast<uLong>(iccProfile.size())));
        uLong compLen = static_cast<uLong>(compressed.size());
        if (compress2(compressed.data(), &compLen, iccProfile.data(),
                      static_cast<uLong>(iccProfile.size()), Z_BEST_SPEED) == Z_OK) {
            std::vector<uint8_t> iccp;
            const char name[] = "AdobeRGB";
            iccp.insert(iccp.end(), name, name + sizeof(name));
            iccp.push_back(0); // zlib deflate
            iccp.insert(iccp.end(), compressed.data(), compressed.data() + compLen);
            AppendPngChunk(png, "iCCP", iccp.data(), iccp.size());
        }
    }

    const size_t sampleBytes = (bitDepth == 16) ? 8u : 4u;
    const size_t rowBytes = 1u + static_cast<size_t>(width) * sampleBytes;
    std::vector<uint8_t> raw(rowBytes * height, 0);
    for (uint32_t y = 0; y < height; ++y) {
        uint8_t* row = raw.data() + static_cast<size_t>(y) * rowBytes;
        row[0] = 0; // Filter None
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t* px = row + 1 + static_cast<size_t>(x) * sampleBytes;
            if (bitDepth == 16) {
                px[0] = 0x80; px[1] = 0x00; // R
                px[2] = 0x40; px[3] = 0x00; // G
                px[4] = 0xFF; px[5] = 0xFF; // B
                px[6] = 0xFF; px[7] = 0xFF; // A
            } else {
                px[0] = 128; // R
                px[1] = 64;  // G
                px[2] = 255; // B
                px[3] = 255; // A
            }
        }
    }

    std::vector<uint8_t> idat(compressBound(static_cast<uLong>(raw.size())));
    uLong idatLen = static_cast<uLong>(idat.size());
    compress2(idat.data(), &idatLen, raw.data(), static_cast<uLong>(raw.size()), Z_BEST_SPEED);
    AppendPngChunk(png, "IDAT", idat.data(), idatLen);
    AppendPngChunk(png, "IEND", nullptr, 0);
    return png;
}

std::vector<uint8_t> BuildSynthetic2x2RgbTiffWithIcc(const std::vector<uint8_t>& iccProfile) {
    // Layout:
    //   0..7   : TIFF LE header (IFD offset = 8)
    //   8..    : IFD (10 entries * 12 + 2 + 4 = 126 bytes -> ends at 134)
    //   134..139 : BitsPerSample [8, 8, 8] (6 bytes)
    //   140..151 : Pixels (2x2 * 3 bytes = 12 bytes)
    //   152..    : ICC Profile
    std::vector<uint8_t> tiff;
    tiff.reserve(256 + iccProfile.size());

    // Header
    tiff.push_back('I'); tiff.push_back('I');
    AppendU16LE(tiff, 42);
    AppendU32LE(tiff, 8);

    const uint16_t entryCount = 10;
    AppendU16LE(tiff, entryCount);

    const uint32_t bpsOffset = 134;
    const uint32_t pixelOffset = 140;
    const uint32_t pixelBytes = 12;
    const uint32_t iccOffset = pixelOffset + pixelBytes;

    auto writeIfdEntry = [&](uint16_t tag, uint16_t type, uint32_t count, uint32_t valueOrOffset) {
        AppendU16LE(tiff, tag);
        AppendU16LE(tiff, type);
        AppendU32LE(tiff, count);
        AppendU32LE(tiff, valueOrOffset);
    };

    // Tags must be sorted in ascending tag ID order
    writeIfdEntry(256, 4, 1, 2);                                    // ImageWidth
    writeIfdEntry(257, 4, 1, 2);                                    // ImageLength
    writeIfdEntry(258, 3, 3, bpsOffset);                            // BitsPerSample
    writeIfdEntry(259, 3, 1, 1);                                    // Compression = Uncompressed
    writeIfdEntry(262, 3, 1, 2);                                    // Photometric = RGB
    writeIfdEntry(273, 4, 1, pixelOffset);                          // StripOffsets
    writeIfdEntry(277, 3, 1, 3);                                    // SamplesPerPixel
    writeIfdEntry(278, 4, 1, 2);                                    // RowsPerStrip
    writeIfdEntry(279, 4, 1, pixelBytes);                           // StripByteCounts
    writeIfdEntry(34675, 1, static_cast<uint32_t>(iccProfile.size()), iccOffset); // ICC Profile
    AppendU32LE(tiff, 0); // Next IFD = 0

    EXPECT_EQ(tiff.size(), bpsOffset);
    AppendU16LE(tiff, 8);
    AppendU16LE(tiff, 8);
    AppendU16LE(tiff, 8);

    EXPECT_EQ(tiff.size(), pixelOffset);
    for (uint32_t i = 0; i < pixelBytes; ++i) {
        tiff.push_back(static_cast<uint8_t>((i * 29u + 10u) & 0xFFu));
    }

    EXPECT_EQ(tiff.size(), iccOffset);
    tiff.insert(tiff.end(), iccProfile.begin(), iccProfile.end());
    return tiff;
}

} // namespace

TEST(ColorPipelineTests, PixelColorMetadataRoundTripAndReleaseReset) {
    QuickView::RawImageFrame src;
    src.width = 4;
    src.height = 4;
    src.stride = 16;
    src.format = QuickView::PixelFormat::BGRA8888;
    src.exifOrientation = 6;
    src.iccProfile = BuildSyntheticIccProfile(560);
    src.colorInfo.dataSpace = QuickView::PixelDataSpace::EncodedSdr;
    src.colorInfo.transfer = QuickView::TransferFunction::SRGB;
    src.colorInfo.primaries = QuickView::ColorPrimaries::AdobeRGB;
    src.colorInfo.nominalBitDepth = 8;
    src.colorInfo.hasEmbeddedIcc = true;
    src.hdrMetadata.isValid = true;
    src.hdrMetadata.primaries = QuickView::ColorPrimaries::AdobeRGB;

    const QuickView::PixelColorMetadata bundle = QuickView::ExtractColorMetadata(src);
    EXPECT_TRUE(bundle.HasColorDescriptor());
    EXPECT_EQ(bundle.iccProfile.size(), 560u);
    EXPECT_EQ(bundle.exifOrientation, 6);

    QuickView::RawImageFrame dst;
    QuickView::ApplyColorMetadata(dst, bundle, false);
    EXPECT_EQ(dst.iccProfile, src.iccProfile);
    EXPECT_EQ(dst.colorInfo.primaries, QuickView::ColorPrimaries::AdobeRGB);
    EXPECT_TRUE(dst.colorInfo.hasEmbeddedIcc);
    EXPECT_TRUE(dst.hdrMetadata.isValid);
    EXPECT_EQ(dst.exifOrientation, 1);

    QuickView::ApplyColorMetadata(dst, bundle, true);
    EXPECT_EQ(dst.exifOrientation, 6);

    src.Release();
    EXPECT_TRUE(src.iccProfile.empty());
    EXPECT_EQ(src.colorInfo.dataSpace, QuickView::PixelDataSpace::Unknown);
    EXPECT_EQ(src.colorInfo.primaries, QuickView::ColorPrimaries::Unknown);
    EXPECT_EQ(src.colorInfo.transfer, QuickView::TransferFunction::Unknown);
    EXPECT_FALSE(src.colorInfo.hasEmbeddedIcc);
    EXPECT_FALSE(src.hdrMetadata.isValid);
    EXPECT_EQ(src.exifOrientation, 1);

    QuickView::PixelColorInfo merged;
    merged.dataSpace = QuickView::PixelDataSpace::EncodedSdr;
    QuickView::PixelColorInfo secondary;
    secondary.dataSpace = QuickView::PixelDataSpace::SceneLinear;
    secondary.transfer = QuickView::TransferFunction::SRGB;
    secondary.primaries = QuickView::ColorPrimaries::DisplayP3;
    secondary.nominalBitDepth = 10;
    secondary.hasEmbeddedIcc = true;
    QuickView::MergeColorInfo(merged, secondary);
    EXPECT_EQ(merged.dataSpace, QuickView::PixelDataSpace::EncodedSdr);
    EXPECT_EQ(merged.transfer, QuickView::TransferFunction::SRGB);
    EXPECT_EQ(merged.primaries, QuickView::ColorPrimaries::DisplayP3);
    EXPECT_EQ(merged.nominalBitDepth, 10u);
    EXPECT_TRUE(merged.hasEmbeddedIcc);
}

TEST(ColorPipelineTests, DecodeWorkerProtocolV2SecReserveRoundTrip) {
    using namespace QuickView::ToolProcess;

    const std::vector<size_t> iccSizes = {0u, 560u, 2u * 1024u * 1024u};
    const uint64_t pixelCap = 64ull * 64ull * 4ull;
    const DecodeMapLayout layout = ComputeDecodeMapLayout(pixelCap);
    ASSERT_NE(layout.totalMapBytes, 0u);
    EXPECT_EQ(layout.headerBytes, kDecodeMapPageSize);
    EXPECT_EQ(layout.pixelOffset, kDecodeMapPageSize);
    EXPECT_EQ(layout.iccOffset % kDecodeMapPageSize, 0u);

    for (size_t iccSize : iccSizes) {
        HANDLE hMap = CreateFileMappingW(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_RESERVE,
            static_cast<DWORD>(layout.totalMapBytes >> 32),
            static_cast<DWORD>(layout.totalMapBytes & 0xFFFFFFFFull),
            nullptr);
        ASSERT_NE(hMap, nullptr);

        uint8_t* view = static_cast<uint8_t*>(MapViewOfFile(
            hMap, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, static_cast<SIZE_T>(layout.totalMapBytes)));
        ASSERT_NE(view, nullptr);

        ASSERT_NE(VirtualAlloc(view, static_cast<SIZE_T>(layout.headerBytes), MEM_COMMIT, PAGE_READWRITE), nullptr);
        auto* hdr = reinterpret_cast<DecodeResultHeader*>(view);
        *hdr = {};
        hdr->magic = kDecodeWorkerMagic;
        hdr->version = kDecodeWorkerVersion;
        hdr->hr = static_cast<int32_t>(E_PENDING);
        hdr->pixelOffset = layout.pixelOffset;
        hdr->pixelCapacity = layout.pixelCapacity;
        hdr->iccOffset = layout.iccOffset;
        hdr->iccCapacity = layout.iccCapacity;

        // Child demand-commits pixels
        std::vector<uint8_t> pixels(static_cast<size_t>(pixelCap), 0x5Au);
        void* committedPixels = VirtualAlloc(view + hdr->pixelOffset, static_cast<SIZE_T>(pixelCap), MEM_COMMIT, PAGE_READWRITE);
        ASSERT_NE(committedPixels, nullptr);
        std::memcpy(committedPixels, pixels.data(), pixels.size());

        std::vector<uint8_t> icc;
        if (iccSize > 0) {
            icc = BuildSyntheticIccProfile(iccSize);
            void* committedIcc = VirtualAlloc(view + hdr->iccOffset, icc.size(), MEM_COMMIT, PAGE_READWRITE);
            ASSERT_NE(committedIcc, nullptr);
            std::memcpy(committedIcc, icc.data(), icc.size());
        }

        hdr->hr = S_OK;
        hdr->width = 64;
        hdr->height = 64;
        hdr->stride = 256;
        hdr->payloadBytes = pixelCap;
        hdr->iccBytes = static_cast<uint32_t>(iccSize);
        hdr->hasEmbeddedIcc = (iccSize > 0) ? 1 : 0;
        hdr->colorPrimaries = static_cast<uint8_t>(QuickView::ColorPrimaries::AdobeRGB);

        EXPECT_TRUE(ValidateDecodeResultHeader(*hdr, layout));
        if (iccSize > 0) {
            EXPECT_EQ(std::memcmp(view + layout.iccOffset, icc.data(), icc.size()), 0);
        }

        // Validation guards
        DecodeResultHeader corrupted = *hdr;
        corrupted.iccBytes = kDecodeWorkerMaxIccBytes + 1u;
        EXPECT_FALSE(ValidateDecodeResultHeader(corrupted, layout));

        corrupted = *hdr;
        corrupted.iccOffset += kDecodeMapPageSize;
        EXPECT_FALSE(ValidateDecodeResultHeader(corrupted, layout));

        corrupted = *hdr;
        corrupted.payloadBytes = pixelCap + 4ull;
        EXPECT_FALSE(ValidateDecodeResultHeader(corrupted, layout));

        UnmapViewOfFile(view);
        CloseHandle(hMap);
    }
}

TEST(ColorPipelineTests, WuffsDecodePngForce8BitBgraPreservesIccFor8And16Bit) {
    const std::vector<uint8_t> icc = BuildSyntheticIccProfile(560);

    for (uint8_t bitDepth : {uint8_t{8}, uint8_t{16}}) {
        const std::vector<uint8_t> png = BuildSyntheticRgbaPng(2, 2, bitDepth, icc);
        std::vector<uint8_t> outBuf(2u * 2u * 4u, 0);
        struct AllocCtx {
            uint8_t* ptr;
            size_t cap;
            bool exceeded;
        } actx{outBuf.data(), outBuf.size(), false};

        WuffsLoader::AllocatorFunc alloc;
        alloc.ctx = &actx;
        alloc.pfn = [](void* c, size_t sz) -> uint8_t* {
            auto* p = static_cast<AllocCtx*>(c);
            if (sz > p->cap) {
                p->exceeded = true;
                return nullptr;
            }
            return p->ptr;
        };

        uint32_t w = 0, h = 0;
        WuffsLoader::WuffsImageInfo info;
        ASSERT_TRUE(WuffsLoader::DecodePNG(png.data(), png.size(), &w, &h, alloc, {}, &info, true));
        EXPECT_FALSE(actx.exceeded);
        EXPECT_EQ(w, 2u);
        EXPECT_EQ(h, 2u);
        EXPECT_EQ(info.bitDepth, static_cast<int>(bitDepth));
        ASSERT_EQ(info.iccProfile.size(), icc.size());
        EXPECT_EQ(std::memcmp(info.iccProfile.data(), icc.data(), icc.size()), 0);
    }
}

TEST(ColorPipelineTests, MiniTiffLoadRegionPropagatesEmbeddedIccFlags) {
    const std::vector<uint8_t> icc = BuildSyntheticIccProfile(560);
    const std::vector<uint8_t> tiff = BuildSynthetic2x2RgbTiffWithIcc(icc);

    QuickView::Codec::DecodeContext ctx;
    ctx.allocator.pfn = [](void*, size_t s) -> uint8_t* {
        return static_cast<uint8_t*>(_aligned_malloc(s, 64));
    };

    QuickView::Codec::DecodeResult result;
    HRESULT hr = QuickView::MiniTiff::LoadRegion(tiff.data(), tiff.size(), ctx, result, 0, 0, 2, 2);
    ASSERT_EQ(hr, S_OK);
    ASSERT_NE(result.pixels, nullptr);
    EXPECT_TRUE(result.metadata.HasEmbeddedColorProfile.value_or(false));
    EXPECT_TRUE(result.metadata.colorInfo.hasEmbeddedIcc);
    EXPECT_EQ(result.metadata.colorInfo.dataSpace, QuickView::PixelDataSpace::EncodedSdr);
    ASSERT_EQ(result.metadata.iccProfileData.size(), icc.size());
    EXPECT_EQ(std::memcmp(result.metadata.iccProfileData.data(), icc.data(), icc.size()), 0);

    _aligned_free(result.pixels);
}

TEST(ColorPipelineTests, TileManagerInvalidateGpuTilesDemotesCpuFreedTilesOnly) {
    QuickView::TileManager tm;
    tm.Initialize(1536, 512);

    const QuickView::RegionRect vp{0, 0, 1536, 512};
    const auto initialMissing = tm.Update(vp, 1.0f, 0.0f, 0.0f, 1536, 512, 0.25f);
    ASSERT_GE(initialMissing.size(), 3u);

    const auto keyKeep = QuickView::TileKey::From(0, 0, 0);
    const auto keyFreed = QuickView::TileKey::From(1, 0, 0);
    const auto keyQueued = QuickView::TileKey::From(2, 0, 0);

    auto makeDummyFrame = []() {
        auto f = std::make_shared<QuickView::RawImageFrame>();
        f->width = 512;
        f->height = 512;
        f->stride = 512 * 4;
        f->pixels = static_cast<uint8_t*>(_aligned_malloc(static_cast<size_t>(f->stride) * f->height, 64));
        f->memoryDeleter = QuickView::MemoryDeleter::FromAlignedFree();
        return f;
    };

    tm.OnTileReady(keyKeep, makeDummyFrame());
    tm.OnTileReady(keyFreed, makeDummyFrame());
    ASSERT_EQ(tm.GetReadyCount(), 2);

    auto tileKeep = tm.GetTile(keyKeep);
    auto tileFreed = tm.GetTile(keyFreed);
    auto tileQueued = tm.GetTile(keyQueued);
    ASSERT_NE(tileKeep, nullptr);
    ASSERT_NE(tileFreed, nullptr);
    ASSERT_NE(tileQueued, nullptr);
    tileKeep->uploaded = true;
    tileFreed->uploaded = true;
    tileFreed->frame.reset(); // Simulate CPU release after DComp VirtualSurface upload

    tm.InvalidateGpuTiles();

    EXPECT_EQ(tm.GetReadyCount(), 1);
    EXPECT_TRUE(tm.IsReady(keyKeep));
    EXPECT_FALSE(tileKeep->uploaded);
    EXPECT_FALSE(tm.IsReady(keyFreed));
    EXPECT_FALSE(tm.IsReady(keyQueued));
    EXPECT_EQ(tileQueued->state, QuickView::TileStateCode::Queued);

    const auto reseeded = tm.Update(vp, 1.0f, 0.0f, 0.0f, 1536, 512, 0.25f);
    EXPECT_NE(std::find(reseeded.begin(), reseeded.end(), keyFreed), reseeded.end());
    EXPECT_EQ(std::find(reseeded.begin(), reseeded.end(), keyKeep), reseeded.end());
    EXPECT_EQ(std::find(reseeded.begin(), reseeded.end(), keyQueued), reseeded.end());

    // In-flight tile completion must still be accepted after InvalidateGpuTiles
    tm.OnTileReady(keyQueued, makeDummyFrame());
    EXPECT_TRUE(tm.IsReady(keyQueued));
    EXPECT_EQ(tm.GetReadyCount(), 2);
}

TEST(ColorPipelineTests, LiveDecodeWorkerSubprocessPreservesIccOverSecReserve) {
    wchar_t exeBuf[MAX_PATH] = {};
    DWORD exeLen = GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
    ASSERT_GT(exeLen, 0u);
    std::filesystem::path exeDir = std::filesystem::path(exeBuf).parent_path();
    std::filesystem::path workerExe = exeDir / L"QuickView.exe";
    if (!std::filesystem::exists(workerExe)) {
        GTEST_SKIP() << "QuickView.exe not present next to test binary";
    }

    const std::vector<uint8_t> icc = BuildSyntheticIccProfile(560);
    const std::vector<uint8_t> png = BuildSyntheticRgbaPng(9, 7, 8, icc);

    wchar_t tempDir[MAX_PATH] = {};
    ASSERT_GT(GetTempPathW(MAX_PATH, tempDir), 0u);
    std::filesystem::path tempPng =
        std::filesystem::path(tempDir) / L"qv_color_pipeline_test_icc.png";
    struct TempFileGuard {
        std::filesystem::path p;
        ~TempFileGuard() {
            std::error_code ec;
            std::filesystem::remove(p, ec);
        }
    } fileGuard{tempPng};

    {
        std::ofstream ofs(tempPng, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(ofs.is_open());
        ofs.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
        ASSERT_TRUE(ofs.good());
    }

    using namespace QuickView::ToolProcess;
    const uint64_t pixelCap = 9ull * 7ull * 4ull;
    const DecodeMapLayout layout = ComputeDecodeMapLayout(pixelCap);
    ASSERT_NE(layout.totalMapBytes, 0u);

    wchar_t mapName[128] = {};
    swprintf_s(mapName, L"Local\\QuickView_TestDW_%lu", GetCurrentProcessId());

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
                       tempPng.wstring() + L"\" --out-map \"" + mapName +
                       L"\" --target-w 9 --target-h 7 --no-fake-base";
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_FORCEOFFFEEDBACK;
    PROCESS_INFORMATION pi{};
    ASSERT_TRUE(CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
                               CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi));
    CloseHandle(pi.hThread);

    DWORD waitRes = WaitForSingleObject(pi.hProcess, 15000);
    DWORD exitCode = 999;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);

    ASSERT_EQ(waitRes, WAIT_OBJECT_0);
    EXPECT_EQ(exitCode, 0u);
    EXPECT_EQ(static_cast<HRESULT>(header->hr), S_OK);
    EXPECT_TRUE(ValidateDecodeResultHeader(*header, layout));
    EXPECT_EQ(header->width, 9u);
    EXPECT_EQ(header->height, 7u);
    EXPECT_LE(header->payloadBytes, pixelCap);
    EXPECT_EQ(header->hasEmbeddedIcc, 1u);
    ASSERT_EQ(header->iccBytes, static_cast<uint32_t>(icc.size()));
    EXPECT_EQ(std::memcmp(mapView + layout.iccOffset, icc.data(), icc.size()), 0);

    UnmapViewOfFile(mapView);
    CloseHandle(hMap);
}

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

std::vector<uint8_t> BuildSynthetic2x2Ico() {
    // Minimal 2x2 32bpp uncompressed Windows ICO without ICC/EXIF tags (routes through WIC fallback).
    // ICONDIR (6B) + ICONDIRENTRY (16B) + BITMAPINFOHEADER (40B) + XOR (16B) + AND (8B) = 86B.
    std::vector<uint8_t> ico(86, 0);
    // ICONDIR
    ico[2] = 1; // Type = ICO
    ico[4] = 1; // Count = 1
    // ICONDIRENTRY (offset 6)
    ico[6] = 2; // Width
    ico[7] = 2; // Height
    ico[10] = 1; // Planes
    ico[12] = 32; // BitCount
    const uint32_t bytesInRes = 64; // 40 + 16 + 8
    const uint32_t imageOffset = 22;
    std::memcpy(ico.data() + 14, &bytesInRes, sizeof(bytesInRes));
    std::memcpy(ico.data() + 18, &imageOffset, sizeof(imageOffset));
    // BITMAPINFOHEADER (offset 22)
    const uint32_t biSize = 40;
    const int32_t biWidth = 2;
    const int32_t biHeight = 4; // 2 * 2 (XOR + AND)
    const uint16_t biPlanes = 1;
    const uint16_t biBitCount = 32;
    std::memcpy(ico.data() + 22, &biSize, sizeof(biSize));
    std::memcpy(ico.data() + 26, &biWidth, sizeof(biWidth));
    std::memcpy(ico.data() + 30, &biHeight, sizeof(biHeight));
    std::memcpy(ico.data() + 34, &biPlanes, sizeof(biPlanes));
    std::memcpy(ico.data() + 36, &biBitCount, sizeof(biBitCount));
    // XOR 2x2 BGRA pixels (offset 62..77)
    for (size_t i = 0; i < 4; ++i) {
        ico[62 + i * 4 + 0] = 0x80u;
        ico[62 + i * 4 + 1] = 0x80u;
        ico[62 + i * 4 + 2] = 0x80u;
        ico[62 + i * 4 + 3] = 0xFFu;
    }
    return ico;
}

struct LiveWorkerOutput {
    DWORD exitCode = 999;
    QuickView::ToolProcess::DecodeResultHeader header{};
    std::vector<uint8_t> pixelBytes;
    std::vector<uint8_t> iccBytes;
};

void ExecuteDecodeWorkerSubprocess(
    const std::filesystem::path& workerExe,
    const std::filesystem::path& inputPath,
    const wchar_t* mapTag,
    int targetW,
    int targetH,
    uint64_t reserveBpp,
    LiveWorkerOutput* outResult) {
    using namespace QuickView::ToolProcess;
    ASSERT_NE(outResult, nullptr);

    const uint64_t pixelCap = static_cast<uint64_t>(targetW) * static_cast<uint64_t>(targetH) * reserveBpp;
    const DecodeMapLayout layout = ComputeDecodeMapLayout(pixelCap);
    ASSERT_NE(layout.totalMapBytes, 0u);

    wchar_t mapName[128] = {};
    swprintf_s(mapName, L"Local\\QuickView_TestDW_%lu_%ls", GetCurrentProcessId(), mapTag);

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
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_FORCEOFFFEEDBACK;
    PROCESS_INFORMATION pi{};
    ASSERT_TRUE(CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
                               CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi));
    CloseHandle(pi.hThread);

    DWORD waitRes = WaitForSingleObject(pi.hProcess, 15000);
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

} // namespace

TEST(ColorPipelineTests, LiveDecodeWorkerPreservesJxrAndWdpHdrScRgbMetadata) {
    wchar_t exeBuf[MAX_PATH] = {};
    DWORD exeLen = GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
    ASSERT_GT(exeLen, 0u);
    std::filesystem::path exeDir = std::filesystem::path(exeBuf).parent_path();
    std::filesystem::path workerExe = exeDir / L"QuickView.exe";
    if (!std::filesystem::exists(workerExe)) {
        GTEST_SKIP() << "QuickView.exe not present next to test binary";
    }

    const auto jxrPath = FindHdrTestAsset(L"cyberpunk2077.jxr");
    const auto wdpPath = FindHdrTestAsset(L"anthony.wdp");
    if (jxrPath.empty() || wdpPath.empty()) {
        GTEST_SKIP() << "HDR JXR/WDP test assets not found";
    }

    auto scanFp16MinMax = [](const uint8_t* data, uint32_t width, uint32_t height, uint32_t stride,
                             float* outMin, float* outMax) {
        float minV = 1e9f;
        float maxV = -1e9f;
        for (uint32_t y = 0; y < height; ++y) {
            const auto* row = reinterpret_cast<const uint16_t*>(data + static_cast<size_t>(y) * stride);
            for (uint32_t x = 0; x < width; ++x) {
                for (uint32_t c = 0; c < 3; ++c) {
                    const float v = HalfToFloat(row[x * 4u + c]);
                    if (v < minV) minV = v;
                    if (v > maxV) maxV = v;
                }
            }
        }
        *outMin = minV;
        *outMax = maxV;
    };

    // 1. Floating-point HDR JXR without ICC (cyberpunk2077.jxr, 64bppRGBAHalf)
    {
        LiveWorkerOutput res;
        ASSERT_NO_FATAL_FAILURE(ExecuteDecodeWorkerSubprocess(
            workerExe, jxrPath, L"jxr", 1024, 768, 16ull, &res));
        EXPECT_EQ(res.exitCode, 0u);
        EXPECT_EQ(static_cast<HRESULT>(res.header.hr), S_OK);
        EXPECT_EQ(res.header.pixelFormat, static_cast<uint8_t>(QuickView::PixelFormat::R16G16B16A16_FLOAT));
        EXPECT_EQ(res.header.colorDataSpace, static_cast<uint8_t>(QuickView::PixelDataSpace::SceneLinearScRgb));
        EXPECT_EQ(res.header.colorTransfer, static_cast<uint8_t>(QuickView::TransferFunction::Linear));
        EXPECT_EQ(res.header.colorPrimaries, static_cast<uint8_t>(QuickView::ColorPrimaries::SRGB));

        bool isValid = false, isHdr = false, isSceneLinear = false;
        bool hasNits = false, hasGainMap = false, gainApplied = false;
        QuickView::ToolProcess::UnpackHdrFlags(
            res.header.hdrFlags, isValid, isHdr, isSceneLinear, hasNits, hasGainMap, gainApplied);
        EXPECT_TRUE(isValid);
        EXPECT_TRUE(isHdr);
        EXPECT_TRUE(isSceneLinear);

        ASSERT_FALSE(res.pixelBytes.empty());
        float minRgb = 0.0f, maxRgb = 0.0f;
        scanFp16MinMax(res.pixelBytes.data(), res.header.width, res.header.height, res.header.stride,
                       &minRgb, &maxRgb);
        EXPECT_GT(maxRgb, 10.0f);
    }

    // 2. Fixed-point HDR WDP/JXR (anthony.wdp, 48bppRGBFixedPoint)
    {
        LiveWorkerOutput res;
        ASSERT_NO_FATAL_FAILURE(ExecuteDecodeWorkerSubprocess(
            workerExe, wdpPath, L"wdp", 800, 600, 16ull, &res));
        EXPECT_EQ(res.exitCode, 0u);
        EXPECT_EQ(static_cast<HRESULT>(res.header.hr), S_OK);
        EXPECT_EQ(res.header.pixelFormat, static_cast<uint8_t>(QuickView::PixelFormat::R16G16B16A16_FLOAT));
        EXPECT_EQ(res.header.colorDataSpace, static_cast<uint8_t>(QuickView::PixelDataSpace::SceneLinearScRgb));
        EXPECT_EQ(res.header.colorTransfer, static_cast<uint8_t>(QuickView::TransferFunction::Linear));
        EXPECT_NE(res.header.colorPrimaries, static_cast<uint8_t>(QuickView::ColorPrimaries::Unknown));
        EXPECT_NE(res.header.colorPrimaries, static_cast<uint8_t>(QuickView::ColorPrimaries::Rec2020));
        EXPECT_GT(res.header.iccBytes, 0u);

        bool isValid = false, isHdr = false, isSceneLinear = false;
        bool hasNits = false, hasGainMap = false, gainApplied = false;
        QuickView::ToolProcess::UnpackHdrFlags(
            res.header.hdrFlags, isValid, isHdr, isSceneLinear, hasNits, hasGainMap, gainApplied);
        EXPECT_TRUE(isValid);
        EXPECT_TRUE(isHdr);
        EXPECT_TRUE(isSceneLinear);

        ASSERT_FALSE(res.pixelBytes.empty());
        float minRgb = 0.0f, maxRgb = 0.0f;
        scanFp16MinMax(res.pixelBytes.data(), res.header.width, res.header.height, res.header.stride,
                       &minRgb, &maxRgb);
        EXPECT_GT(maxRgb, 2.0f);
        EXPECT_LT(minRgb, 0.0f);
    }
}

TEST(ColorPipelineTests, LiveDecodeWorkerUntaggedSdrPreservesUnknownPrimaries) {
    wchar_t exeBuf[MAX_PATH] = {};
    DWORD exeLen = GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
    ASSERT_GT(exeLen, 0u);
    std::filesystem::path exeDir = std::filesystem::path(exeBuf).parent_path();
    std::filesystem::path workerExe = exeDir / L"QuickView.exe";
    if (!std::filesystem::exists(workerExe)) {
        GTEST_SKIP() << "QuickView.exe not present next to test binary";
    }

    const std::vector<uint8_t> ico = BuildSynthetic2x2Ico();
    wchar_t tempDir[MAX_PATH] = {};
    ASSERT_GT(GetTempPathW(MAX_PATH, tempDir), 0u);
    std::filesystem::path tempIco =
        std::filesystem::path(tempDir) / L"qv_color_pipeline_test_untagged.ico";
    struct TempFileGuard {
        std::filesystem::path p;
        ~TempFileGuard() {
            std::error_code ec;
            std::filesystem::remove(p, ec);
        }
    } fileGuard{tempIco};

    {
        std::ofstream ofs(tempIco, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(ofs.is_open());
        ofs.write(reinterpret_cast<const char*>(ico.data()), static_cast<std::streamsize>(ico.size()));
        ASSERT_TRUE(ofs.good());
    }

    LiveWorkerOutput res;
    ASSERT_NO_FATAL_FAILURE(ExecuteDecodeWorkerSubprocess(
        workerExe, tempIco, L"ico", 2, 2, 16ull, &res));
    EXPECT_EQ(res.exitCode, 0u);
    EXPECT_EQ(static_cast<HRESULT>(res.header.hr), S_OK);
    EXPECT_EQ(res.header.pixelFormat, static_cast<uint8_t>(QuickView::PixelFormat::BGRA8888));
    EXPECT_EQ(res.header.colorDataSpace, static_cast<uint8_t>(QuickView::PixelDataSpace::EncodedSdr));
    EXPECT_EQ(res.header.colorPrimaries, static_cast<uint8_t>(QuickView::ColorPrimaries::Unknown));
    EXPECT_EQ(res.header.iccBytes, 0u);
}
