#include "pch.h"
#include <gtest/gtest.h>
#include "AnimationDecoder.h"
#include "MappedFile.h"
#include <zlib.h>
#include <webp/encode.h>
#include <webp/mux.h>
#include <jxl/encode.h>
#include <jxl/encode_cxx.h>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>

namespace {

std::shared_ptr<QuickView::MappedFile> MakeMappedBytes(const std::vector<uint8_t>& bytes) {
    return QuickView::MappedFile::FromMemory(bytes.data(), bytes.size());
}

void VerifyPixelBGRA(const QuickView::RawImageFrame& frame, int x, int y,
                     uint8_t b, uint8_t g, uint8_t r, uint8_t a, int tol = 0) {
    ASSERT_LT(x, frame.width);
    ASSERT_LT(y, frame.height);
    ASSERT_NE(frame.pixels, nullptr);
    const uint8_t* px = frame.pixels + static_cast<size_t>(y) * frame.stride + static_cast<size_t>(x) * 4;
    EXPECT_NEAR(px[0], b, tol) << "B mismatch at (" << x << "," << y << ")";
    EXPECT_NEAR(px[1], g, tol) << "G mismatch at (" << x << "," << y << ")";
    EXPECT_NEAR(px[2], r, tol) << "R mismatch at (" << x << "," << y << ")";
    EXPECT_NEAR(px[3], a, tol) << "A mismatch at (" << x << "," << y << ")";
}

void PushBE32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
}

void PushBE16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
}

void PushPngChunk(std::vector<uint8_t>& out, const char tag[4], const std::vector<uint8_t>& payload) {
    PushBE32(out, static_cast<uint32_t>(payload.size()));
    const size_t tagPos = out.size();
    out.insert(out.end(), tag, tag + 4);
    out.insert(out.end(), payload.begin(), payload.end());
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, out.data() + tagPos, static_cast<uInt>(4 + payload.size()));
    PushBE32(out, static_cast<uint32_t>(crc));
}

std::vector<uint8_t> CompressPngRgba(uint32_t w, uint32_t h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(h) * (1 + static_cast<size_t>(w) * 4));
    for (uint32_t y = 0; y < h; ++y) {
        raw.push_back(0); // filter: None
        for (uint32_t x = 0; x < w; ++x) {
            raw.push_back(r);
            raw.push_back(g);
            raw.push_back(b);
            raw.push_back(a);
        }
    }
    uLongf dstLen = compressBound(static_cast<uLong>(raw.size()));
    std::vector<uint8_t> compressed(dstLen);
    compress2(compressed.data(), &dstLen, raw.data(), static_cast<uLong>(raw.size()), Z_BEST_SPEED);
    compressed.resize(dstLen);
    return compressed;
}

std::vector<uint8_t> MakePngOrApng(uint32_t w, uint32_t h, bool animated, size_t extraTextBytes) {
    std::vector<uint8_t> out = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };

    std::vector<uint8_t> ihdr;
    PushBE32(ihdr, w);
    PushBE32(ihdr, h);
    ihdr.push_back(8); // bit depth
    ihdr.push_back(6); // RGBA
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    PushPngChunk(out, "IHDR", ihdr);

    if (extraTextBytes > 0) {
        std::vector<uint8_t> txt(extraTextBytes, 'A');
        txt[0] = 'K';
        txt[1] = 0; // tEXt keyword NUL separator
        PushPngChunk(out, "tEXt", txt);
    }

    const uint8_t colors[3][4] = {
        { 255,   0,   0, 255 }, // Red
        {   0, 255,   0, 255 }, // Green
        {   0,   0, 255, 255 }, // Blue
    };

    if (animated) {
        std::vector<uint8_t> actl;
        PushBE32(actl, 3); // num_frames
        PushBE32(actl, 0); // num_plays (infinite)
        PushPngChunk(out, "acTL", actl);

        uint32_t seq = 0;
        for (int f = 0; f < 3; ++f) {
            std::vector<uint8_t> fctl;
            PushBE32(fctl, seq++);
            PushBE32(fctl, w);
            PushBE32(fctl, h);
            PushBE32(fctl, 0); // x_offset
            PushBE32(fctl, 0); // y_offset
            PushBE16(fctl, 50); // delay_num
            PushBE16(fctl, 1000); // delay_den
            fctl.push_back(0); // dispose: NONE
            fctl.push_back(0); // blend: SOURCE
            PushPngChunk(out, "fcTL", fctl);

            auto zdata = CompressPngRgba(w, h, colors[f][0], colors[f][1], colors[f][2], colors[f][3]);
            if (f == 0) {
                PushPngChunk(out, "IDAT", zdata);
            } else {
                std::vector<uint8_t> fdat;
                PushBE32(fdat, seq++);
                fdat.insert(fdat.end(), zdata.begin(), zdata.end());
                PushPngChunk(out, "fdAT", fdat);
            }
        }
    } else {
        auto zdata = CompressPngRgba(w, h, colors[0][0], colors[0][1], colors[0][2], colors[0][3]);
        PushPngChunk(out, "IDAT", zdata);
    }

    PushPngChunk(out, "IEND", {});
    return out;
}

std::vector<uint8_t> MakeMinimalGif(bool animated) {
    // 2x2 GIF89a with 2-color GCT: index 0 = Red (255,0,0), index 1 = Blue (0,0,255)
    std::vector<uint8_t> gif = {
        'G', 'I', 'F', '8', '9', 'a',
        2, 0, 2, 0,       // 2x2
        0x80, 0x00, 0x00, // GCT flag=1, res=0, sort=0, size=2 entries
        0xFF, 0x00, 0x00, // color 0: Red
        0x00, 0x00, 0xFF, // color 1: Blue
    };
    if (animated) {
        // NETSCAPE2.0 loop application extension
        const uint8_t loopExt[] = {
            0x21, 0xFF, 0x0B, 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0',
            0x03, 0x01, 0x00, 0x00, 0x00
        };
        gif.insert(gif.end(), std::begin(loopExt), std::end(loopExt));
    }

    const int frameCount = animated ? 2 : 1;
    for (int f = 0; f < frameCount; ++f) {
        // Graphic Control Extension: 50ms delay (5 * 10ms)
        const uint8_t gce[] = { 0x21, 0xF9, 0x04, 0x00, 0x05, 0x00, 0x00, 0x00 };
        gif.insert(gif.end(), std::begin(gce), std::end(gce));

        // Image Descriptor (2x2 at 0,0, local color table = 0)
        const uint8_t desc[] = { 0x2C, 0x00, 0x00, 0x00, 0x00, 2, 0, 2, 0, 0x00 };
        gif.insert(gif.end(), std::begin(desc), std::end(desc));

        // LZW min code size = 2 (Clear=4, EOI=5). Re-emit Clear after every 2 pixels
        // so code width never steps from 3 bits to 4 bits before EOI.
        uint32_t stream = 0;
        int bitPos = 0;
        auto emit = [&](uint32_t code) {
            stream |= (code << bitPos);
            bitPos += 3;
        };
        const uint32_t px = static_cast<uint32_t>(f);
        emit(4); // Clear
        emit(px);
        emit(px);
        emit(4); // Clear before code table fills to 8
        emit(px);
        emit(px);
        emit(5); // EOI
        const uint8_t nbytes = static_cast<uint8_t>((bitPos + 7) / 8);
        gif.push_back(2); // LZW minimum code size
        gif.push_back(nbytes);
        for (int b = 0; b < nbytes; ++b) {
            gif.push_back(static_cast<uint8_t>((stream >> (b * 8)) & 0xFF));
        }
        gif.push_back(0x00); // block terminator
    }
    gif.push_back(0x3B); // trailer
    return gif;
}

std::vector<uint8_t> EncodeLosslessWebPFrame(const std::vector<uint8_t>& bgra, int w, int h) {
    uint8_t* outPtr = nullptr;
    size_t outSize = WebPEncodeLosslessBGRA(bgra.data(), w, h, w * 4, &outPtr);
    std::vector<uint8_t> res;
    if (outPtr && outSize > 0) {
        res.assign(outPtr, outPtr + outSize);
        WebPFree(outPtr);
    }
    return res;
}

std::vector<uint8_t> MakeAnimatedWebPPartialDisposal(uint32_t frameCount) {
    WebPMux* mux = WebPMuxNew();
    if (!mux) return {};

    WebPMuxAnimParams animParams{};
    animParams.bgcolor = 0;
    animParams.loop_count = 0;
    WebPMuxSetAnimationParams(mux, &animParams);

    constexpr int kCanvasW = 24;
    constexpr int kCanvasH = 24;

    // Frame 0: full 24x24 dark blue canvas (B=200, G=10, R=10, A=255)
    std::vector<uint8_t> fullFrame(static_cast<size_t>(kCanvasW) * kCanvasH * 4);
    for (size_t i = 0; i < static_cast<size_t>(kCanvasW) * kCanvasH; ++i) {
        fullFrame[i * 4 + 0] = 200;
        fullFrame[i * 4 + 1] = 10;
        fullFrame[i * 4 + 2] = 10;
        fullFrame[i * 4 + 3] = 255;
    }
    auto f0Bytes = EncodeLosslessWebPFrame(fullFrame, kCanvasW, kCanvasH);

    WebPMuxFrameInfo info{};
    info.id = WEBP_CHUNK_ANMF;
    info.duration = 40;
    info.x_offset = 0;
    info.y_offset = 0;
    info.dispose_method = WEBP_MUX_DISPOSE_NONE;
    info.blend_method = WEBP_MUX_NO_BLEND;
    info.bitstream.bytes = f0Bytes.data();
    info.bitstream.size = f0Bytes.size();
    WebPMuxPushFrame(mux, &info, 1);

    // Partial 8x8 fragments at (8, 8) alternating Red/Green with DISPOSE_BACKGROUND + BLEND
    constexpr int kFragW = 8;
    constexpr int kFragH = 8;
    std::vector<uint8_t> redFrag(static_cast<size_t>(kFragW) * kFragH * 4);
    std::vector<uint8_t> greenFrag(static_cast<size_t>(kFragW) * kFragH * 4);
    for (size_t i = 0; i < static_cast<size_t>(kFragW) * kFragH; ++i) {
        redFrag[i * 4 + 0] = 0;
        redFrag[i * 4 + 1] = 0;
        redFrag[i * 4 + 2] = 255;
        redFrag[i * 4 + 3] = 255;

        greenFrag[i * 4 + 0] = 0;
        greenFrag[i * 4 + 1] = 255;
        greenFrag[i * 4 + 2] = 0;
        greenFrag[i * 4 + 3] = 255;
    }
    auto redBytes = EncodeLosslessWebPFrame(redFrag, kFragW, kFragH);
    auto greenBytes = EncodeLosslessWebPFrame(greenFrag, kFragW, kFragH);

    for (uint32_t idx = 1; idx < frameCount; ++idx) {
        WebPMuxFrameInfo fragInfo{};
        fragInfo.id = WEBP_CHUNK_ANMF;
        fragInfo.duration = 40;
        fragInfo.x_offset = 8;
        fragInfo.y_offset = 8;
        fragInfo.dispose_method = WEBP_MUX_DISPOSE_BACKGROUND;
        fragInfo.blend_method = WEBP_MUX_BLEND;
        const auto& bytes = (idx % 2 == 1) ? redBytes : greenBytes;
        fragInfo.bitstream.bytes = bytes.data();
        fragInfo.bitstream.size = bytes.size();
        WebPMuxPushFrame(mux, &fragInfo, 1);
    }

    WebPData assembled{};
    std::vector<uint8_t> result;
    if (WebPMuxAssemble(mux, &assembled) == WEBP_MUX_OK && assembled.bytes && assembled.size > 0) {
        result.assign(assembled.bytes, assembled.bytes + assembled.size);
        WebPDataClear(&assembled);
    }
    WebPMuxDelete(mux);
    return result;
}

std::vector<uint8_t> MakeAnimatedJxl(uint32_t width, uint32_t height, bool animated) {
    JxlEncoderPtr enc = JxlEncoderMake(nullptr);
    if (!enc) return {};

    JxlBasicInfo basicInfo;
    JxlEncoderInitBasicInfo(&basicInfo);
    basicInfo.xsize = width;
    basicInfo.ysize = height;
    basicInfo.bits_per_sample = 8;
    basicInfo.num_color_channels = 3;
    basicInfo.num_extra_channels = 1;
    basicInfo.alpha_bits = 8;
    basicInfo.alpha_premultiplied = JXL_FALSE;
    basicInfo.uses_original_profile = JXL_TRUE;
    if (animated) {
        basicInfo.have_animation = JXL_TRUE;
        basicInfo.animation.tps_numerator = 10;
        basicInfo.animation.tps_denominator = 1;
        basicInfo.animation.num_loops = 0;
    }
    if (JxlEncoderSetBasicInfo(enc.get(), &basicInfo) != JXL_ENC_SUCCESS) return {};

    JxlColorEncoding ce{};
    JxlColorEncodingSetToSRGB(&ce, JXL_FALSE);
    if (JxlEncoderSetColorEncoding(enc.get(), &ce) != JXL_ENC_SUCCESS) return {};

    JxlEncoderFrameSettings* frameSettings = JxlEncoderFrameSettingsCreate(enc.get(), nullptr);
    if (!frameSettings) return {};
    JxlEncoderSetFrameLossless(frameSettings, JXL_TRUE);

    const JxlPixelFormat pixFmt = { 4, JXL_TYPE_UINT8, JXL_LITTLE_ENDIAN, 0 };
    const uint8_t colors[3][4] = {
        { 255,   0,   0, 255 }, // Red (RGBA)
        {   0, 255,   0, 255 }, // Green
        {   0,   0, 255, 255 }, // Blue
    };
    const int frameCount = animated ? 3 : 1;
    for (int f = 0; f < frameCount; ++f) {
        JxlFrameHeader fh;
        JxlEncoderInitFrameHeader(&fh);
        fh.duration = 1;
        fh.is_last = (f == frameCount - 1) ? JXL_TRUE : JXL_FALSE;
        if (JxlEncoderSetFrameHeader(frameSettings, &fh) != JXL_ENC_SUCCESS) return {};

        std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
        for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i) {
            memcpy(&pixels[i * 4], colors[f], 4);
        }
        if (JxlEncoderAddImageFrame(frameSettings, &pixFmt, pixels.data(), pixels.size()) != JXL_ENC_SUCCESS) {
            return {};
        }
    }
    JxlEncoderCloseInput(enc.get());

    std::vector<uint8_t> compressed(4096);
    uint8_t* nextOut = compressed.data();
    size_t availOut = compressed.size();
    for (;;) {
        const JxlEncoderStatus st = JxlEncoderProcessOutput(enc.get(), &nextOut, &availOut);
        if (st == JXL_ENC_NEED_MORE_OUTPUT) {
            const size_t offset = static_cast<size_t>(nextOut - compressed.data());
            compressed.resize(compressed.size() * 2);
            nextOut = compressed.data() + offset;
            availOut = compressed.size() - offset;
        } else if (st == JXL_ENC_SUCCESS) {
            compressed.resize(static_cast<size_t>(nextOut - compressed.data()));
            break;
        } else {
            return {};
        }
    }
    return compressed;
}

// 8x8 3-frame animated AVIS (ftypavis) encoding Red, Green, Blue frames.
constexpr uint8_t kAnimatedAvisFixture[] = {
    0x00, 0x00, 0x00, 0x2C, 0x66, 0x74, 0x79, 0x70, 0x61, 0x76, 0x69, 0x73, 0x00, 0x00, 0x00, 0x00,
    0x61, 0x76, 0x69, 0x73, 0x61, 0x76, 0x69, 0x66, 0x6D, 0x73, 0x66, 0x31, 0x69, 0x73, 0x6F, 0x38,
    0x6D, 0x69, 0x66, 0x31, 0x6D, 0x69, 0x61, 0x66, 0x4D, 0x41, 0x31, 0x41, 0x00, 0x00, 0x00, 0xF9,
    0x6D, 0x65, 0x74, 0x61, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2F, 0x68, 0x64, 0x6C, 0x72,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x70, 0x69, 0x63, 0x74, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x50, 0x69, 0x63, 0x74, 0x75, 0x72, 0x65, 0x48,
    0x61, 0x6E, 0x64, 0x6C, 0x65, 0x72, 0x00, 0x00, 0x00, 0x00, 0x0E, 0x70, 0x69, 0x74, 0x6D, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x1E, 0x69, 0x6C, 0x6F, 0x63, 0x00, 0x00, 0x00,
    0x00, 0x44, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x03, 0xDC, 0x00,
    0x00, 0x00, 0x25, 0x00, 0x00, 0x00, 0x28, 0x69, 0x69, 0x6E, 0x66, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x1A, 0x69, 0x6E, 0x66, 0x65, 0x02, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
    0x00, 0x61, 0x76, 0x30, 0x31, 0x43, 0x6F, 0x6C, 0x6F, 0x72, 0x00, 0x00, 0x00, 0x00, 0x6A, 0x69,
    0x70, 0x72, 0x70, 0x00, 0x00, 0x00, 0x4B, 0x69, 0x70, 0x63, 0x6F, 0x00, 0x00, 0x00, 0x14, 0x69,
    0x73, 0x70, 0x65, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x08, 0x00,
    0x00, 0x00, 0x10, 0x70, 0x69, 0x78, 0x69, 0x00, 0x00, 0x00, 0x00, 0x03, 0x08, 0x08, 0x08, 0x00,
    0x00, 0x00, 0x0C, 0x61, 0x76, 0x31, 0x43, 0x81, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x13, 0x63,
    0x6F, 0x6C, 0x72, 0x6E, 0x63, 0x6C, 0x78, 0x00, 0x02, 0x00, 0x02, 0x00, 0x00, 0x80, 0x00, 0x00,
    0x00, 0x17, 0x69, 0x70, 0x6D, 0x61, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01,
    0x04, 0x01, 0x02, 0x83, 0x04, 0x00, 0x00, 0x02, 0xAF, 0x6D, 0x6F, 0x6F, 0x76, 0x00, 0x00, 0x00,
    0x6C, 0x6D, 0x76, 0x68, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x03, 0xE8, 0x00, 0x00, 0x01, 0x2C, 0x00, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x02, 0x3B, 0x74, 0x72, 0x61,
    0x6B, 0x00, 0x00, 0x00, 0x68, 0x74, 0x6B, 0x68, 0x64, 0x01, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00,
    0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x24, 0x65, 0x64, 0x74,
    0x73, 0x00, 0x00, 0x00, 0x1C, 0x65, 0x6C, 0x73, 0x74, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x01, 0x2C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01,
    0xA7, 0x6D, 0x64, 0x69, 0x61, 0x00, 0x00, 0x00, 0x20, 0x6D, 0x64, 0x68, 0x64, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x0C,
    0x00, 0x55, 0xC4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2F, 0x68, 0x64, 0x6C, 0x72, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x70, 0x69, 0x63, 0x74, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x50, 0x69, 0x63, 0x74, 0x75, 0x72, 0x65, 0x48, 0x61, 0x6E, 0x64,
    0x6C, 0x65, 0x72, 0x00, 0x00, 0x00, 0x01, 0x50, 0x6D, 0x69, 0x6E, 0x66, 0x00, 0x00, 0x00, 0x14,
    0x76, 0x6D, 0x68, 0x64, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x24, 0x64, 0x69, 0x6E, 0x66, 0x00, 0x00, 0x00, 0x1C, 0x64, 0x72, 0x65, 0x66,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0C, 0x75, 0x72, 0x6C, 0x20,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01, 0x10, 0x73, 0x74, 0x62, 0x6C, 0x00, 0x00, 0x00, 0x8C,
    0x73, 0x74, 0x73, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x7C,
    0x61, 0x76, 0x30, 0x31, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x08,
    0x00, 0x48, 0x00, 0x00, 0x00, 0x48, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x17, 0x4C,
    0x61, 0x76, 0x63, 0x36, 0x33, 0x2E, 0x31, 0x2E, 0x31, 0x30, 0x31, 0x20, 0x6C, 0x69, 0x62, 0x61,
    0x6F, 0x6D, 0x2D, 0x61, 0x76, 0x31, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18,
    0xFF, 0xFF, 0x00, 0x00, 0x00, 0x0C, 0x61, 0x76, 0x31, 0x43, 0x81, 0x20, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x0A, 0x66, 0x69, 0x65, 0x6C, 0x01, 0x00, 0x00, 0x00, 0x00, 0x10, 0x63, 0x63, 0x73, 0x74,
    0x00, 0x00, 0x00, 0x00, 0x7C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x73, 0x74, 0x74, 0x73,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x04, 0x00,
    0x00, 0x00, 0x00, 0x14, 0x73, 0x74, 0x73, 0x73, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x1C, 0x73, 0x74, 0x73, 0x63, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x20, 0x73, 0x74, 0x73, 0x7A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x25, 0x00, 0x00, 0x00, 0x1C, 0x00, 0x00, 0x00, 0x1C,
    0x00, 0x00, 0x00, 0x14, 0x73, 0x74, 0x63, 0x6F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x03, 0xDC, 0x00, 0x00, 0x00, 0x65, 0x6D, 0x64, 0x61, 0x74, 0x0A, 0x0C, 0x20, 0x00,
    0x00, 0x01, 0x17, 0xE7, 0xD7, 0xCD, 0x01, 0x0D, 0x00, 0x20, 0x32, 0x15, 0x10, 0x00, 0xC0, 0x00,
    0x00, 0x02, 0x80, 0x00, 0x00, 0x05, 0xF5, 0xEF, 0x64, 0xDD, 0x73, 0x01, 0xE3, 0x9A, 0x1B, 0x61,
    0xD8, 0x32, 0x1A, 0x30, 0x03, 0xC0, 0x80, 0x00, 0x00, 0x16, 0xC0, 0x00, 0x00, 0x02, 0x80, 0x00,
    0x00, 0x80, 0x00, 0x1E, 0x9C, 0x0E, 0x74, 0xA8, 0x0F, 0x0A, 0x0D, 0x8A, 0x80, 0x32, 0x1A, 0x30,
    0x05, 0xC1, 0x04, 0x00, 0x00, 0x46, 0xC0, 0x00, 0x00, 0x02, 0x80, 0x00, 0x00, 0x80, 0x00, 0x8D,
    0x04, 0x88, 0x55, 0x25, 0xDF, 0x2C, 0x0F, 0xF5, 0x80
};

void VerifyRepoWuffsAnimation(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return;
    auto file = std::make_shared<QuickView::MappedFile>(path.wstring());
    ASSERT_TRUE(file && file->IsValid()) << path.string();

    auto dec = QuickView::CreateWuffsAnimator();
    ASSERT_TRUE(dec->Initialize(file, QuickView::PixelFormat::BGRA8888)) << path.string();
    EXPECT_TRUE(dec->IsAnimated()) << path.string();

    uint32_t decoded = 0;
    while (auto frame = dec->GetNextFrame()) {
        ASSERT_NE(frame->pixels, nullptr);
        EXPECT_GT(frame->width, 0);
        EXPECT_GT(frame->height, 0);
        ++decoded;
        if (decoded > 200) break;
    }
    EXPECT_GE(decoded, 2u) << path.string();
    EXPECT_EQ(dec->GetNextFrame(), nullptr);

    auto f0 = dec->SeekToFrame(0);
    ASSERT_NE(f0, nullptr) << path.string();
    EXPECT_EQ(f0->frameMeta.index, 0u);
}

} // namespace

TEST(AnimationDecoderTests, ApngLargePreIdatMetadataAndLoop) {
    // 1.35 MB pre-IDAT tEXt metadata forces decode_image_config short_read loop expansion
    auto apngBytes = MakePngOrApng(4, 4, true, 1'350'000);
    auto file = MakeMappedBytes(apngBytes);
    ASSERT_TRUE(file && file->IsValid());

    auto dec = QuickView::CreateWuffsAnimator();
    ASSERT_TRUE(dec->Initialize(file, QuickView::PixelFormat::BGRA8888));
    EXPECT_TRUE(dec->IsAnimated());
    EXPECT_EQ(dec->GetTotalFrames(), 3u);

    auto f0 = dec->GetNextFrame();
    ASSERT_NE(f0, nullptr);
    VerifyPixelBGRA(*f0, 2, 2, 0, 0, 255, 255);

    auto f1 = dec->GetNextFrame();
    ASSERT_NE(f1, nullptr);
    VerifyPixelBGRA(*f1, 2, 2, 0, 255, 0, 255);

    auto f2 = dec->GetNextFrame();
    ASSERT_NE(f2, nullptr);
    VerifyPixelBGRA(*f2, 2, 2, 255, 0, 0, 255);

    EXPECT_EQ(dec->GetNextFrame(), nullptr);

    // Loop back to frame 0
    auto loop0 = dec->SeekToFrame(0);
    ASSERT_NE(loop0, nullptr);
    EXPECT_EQ(loop0->frameMeta.index, 0u);
    VerifyPixelBGRA(*loop0, 2, 2, 0, 0, 255, 255);

    auto s2 = dec->SeekToFrame(2);
    ASSERT_NE(s2, nullptr);
    EXPECT_EQ(s2->frameMeta.index, 2u);
    VerifyPixelBGRA(*s2, 2, 2, 255, 0, 0, 255);
}

TEST(AnimationDecoderTests, StaticPngNotAnimated) {
    auto pngBytes = MakePngOrApng(4, 4, false, 0);
    auto file = MakeMappedBytes(pngBytes);
    auto dec = QuickView::CreateWuffsAnimator();
    if (dec->Initialize(file, QuickView::PixelFormat::BGRA8888)) {
        EXPECT_FALSE(dec->IsAnimated());
    }
}

TEST(AnimationDecoderTests, GifDecodeAndLoop) {
    auto gifBytes = MakeMinimalGif(true);
    auto file = MakeMappedBytes(gifBytes);
    ASSERT_TRUE(file && file->IsValid());

    auto dec = QuickView::CreateWuffsAnimator();
    ASSERT_TRUE(dec->Initialize(file, QuickView::PixelFormat::BGRA8888));
    EXPECT_TRUE(dec->IsAnimated());

    auto f0 = dec->GetNextFrame();
    ASSERT_NE(f0, nullptr);
    VerifyPixelBGRA(*f0, 0, 0, 0, 0, 255, 255);

    auto f1 = dec->GetNextFrame();
    ASSERT_NE(f1, nullptr);
    VerifyPixelBGRA(*f1, 1, 1, 255, 0, 0, 255);

    EXPECT_EQ(dec->GetNextFrame(), nullptr);

    auto loop0 = dec->SeekToFrame(0);
    ASSERT_NE(loop0, nullptr);
    EXPECT_EQ(loop0->frameMeta.index, 0u);
    VerifyPixelBGRA(*loop0, 0, 0, 0, 0, 255, 255);

    auto staticGif = MakeMinimalGif(false);
    auto staticDec = QuickView::CreateWuffsAnimator();
    if (staticDec->Initialize(MakeMappedBytes(staticGif), QuickView::PixelFormat::BGRA8888)) {
        EXPECT_FALSE(staticDec->IsAnimated());
    }
}

TEST(AnimationDecoderTests, RepoWuffsFixturesIfPresent) {
    const std::filesystem::path dataDir = std::filesystem::path(__FILE__).parent_path().parent_path() / "third_party/wuffs/test/data";
    VerifyRepoWuffsAnimation(dataDir / "animated-red-blue.apng");
    VerifyRepoWuffsAnimation(dataDir / "artificial-png/apng-skip-idat.png");
    VerifyRepoWuffsAnimation(dataDir / "muybridge.apng");
    VerifyRepoWuffsAnimation(dataDir / "animated-red-blue.gif");
    VerifyRepoWuffsAnimation(dataDir / "muybridge.gif");
}

TEST(AnimationDecoderTests, WebPPartialRectDisposalSnapshotSeekAndLoop) {
    constexpr uint32_t kFrames = 24;
    auto webpBytes = MakeAnimatedWebPPartialDisposal(kFrames);
    ASSERT_FALSE(webpBytes.empty());
    auto file = MakeMappedBytes(webpBytes);

    auto dec = QuickView::CreateWebPAnimator();
    ASSERT_TRUE(dec->Initialize(file, QuickView::PixelFormat::BGRA8888));
    EXPECT_TRUE(dec->IsAnimated());
    EXPECT_EQ(dec->GetTotalFrames(), kFrames);

    for (uint32_t i = 0; i < kFrames; ++i) {
        auto frame = dec->GetNextFrame();
        ASSERT_NE(frame, nullptr) << "Failed at frame " << i;
        EXPECT_EQ(frame->frameMeta.index, i);
        VerifyPixelBGRA(*frame, 0, 0, 200, 10, 10, 255);
        if (i == 0) {
            VerifyPixelBGRA(*frame, 10, 10, 200, 10, 10, 255);
        } else if (i % 2 == 1) {
            VerifyPixelBGRA(*frame, 10, 10, 0, 0, 255, 255);
        } else {
            VerifyPixelBGRA(*frame, 10, 10, 0, 255, 0, 255);
        }
    }
    EXPECT_EQ(dec->GetNextFrame(), nullptr);

    // Trigger background indexer via SeekToFrame(21), allow indexer thread to build checkpoint 20, then seek backward/forward.
    auto f21 = dec->SeekToFrame(21);
    ASSERT_NE(f21, nullptr);
    VerifyPixelBGRA(*f21, 10, 10, 0, 0, 255, 255);

    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    auto f20 = dec->SeekToFrame(20);
    ASSERT_NE(f20, nullptr);
    VerifyPixelBGRA(*f20, 10, 10, 0, 255, 0, 255);

    auto f0 = dec->SeekToFrame(0);
    ASSERT_NE(f0, nullptr);
    VerifyPixelBGRA(*f0, 10, 10, 200, 10, 10, 255);
}

TEST(AnimationDecoderTests, AvifAvisDecodeSeekAndLoop) {
    std::vector<uint8_t> avisBytes(std::begin(kAnimatedAvisFixture), std::end(kAnimatedAvisFixture));
    auto file = MakeMappedBytes(avisBytes);
    ASSERT_TRUE(file && file->IsValid());

    auto dec = QuickView::CreateAvifAnimator();
    ASSERT_TRUE(dec->Initialize(file, QuickView::PixelFormat::BGRA8888));
    EXPECT_TRUE(dec->IsAnimated());
    EXPECT_EQ(dec->GetTotalFrames(), 3u);

    auto f0 = dec->GetNextFrame();
    ASSERT_NE(f0, nullptr);
    VerifyPixelBGRA(*f0, 4, 4, 0, 0, 255, 255, 35); // Red (YUV BT.601 conversion tolerance)

    auto f1 = dec->GetNextFrame();
    ASSERT_NE(f1, nullptr);
    VerifyPixelBGRA(*f1, 4, 4, 0, 255, 0, 255, 35); // Green

    auto f2 = dec->GetNextFrame();
    ASSERT_NE(f2, nullptr);
    VerifyPixelBGRA(*f2, 4, 4, 255, 0, 0, 255, 35); // Blue

    EXPECT_EQ(dec->GetNextFrame(), nullptr);

    auto loop0 = dec->SeekToFrame(0);
    ASSERT_NE(loop0, nullptr);
    VerifyPixelBGRA(*loop0, 4, 4, 0, 0, 255, 255, 35);

    auto clamped = dec->SeekToFrame(99);
    ASSERT_NE(clamped, nullptr);
    VerifyPixelBGRA(*clamped, 4, 4, 255, 0, 0, 255, 35);
}

TEST(AnimationDecoderTests, JxlUnalignedStrideSwizzleSeekAndLoop) {
    // width=5 has (width % 4) != 0, so stride=32 > width*4=20; verify every row swizzles cleanly
    auto jxlBytes = MakeAnimatedJxl(5, 5, true);
    ASSERT_FALSE(jxlBytes.empty());
    auto file = MakeMappedBytes(jxlBytes);

    auto dec = QuickView::CreateJxlAnimator();
    ASSERT_TRUE(dec->Initialize(file, QuickView::PixelFormat::BGRA8888));
    EXPECT_TRUE(dec->IsAnimated());
    EXPECT_EQ(dec->GetTotalFrames(), 3u);

    const uint8_t expectedBGRA[3][4] = {
        { 0, 0, 255, 255 }, // Red in BGRA
        { 0, 255, 0, 255 }, // Green in BGRA
        { 255, 0, 0, 255 }, // Blue in BGRA
    };

    for (int f = 0; f < 3; ++f) {
        auto frame = dec->GetNextFrame();
        ASSERT_NE(frame, nullptr) << "JXL frame " << f;
        EXPECT_EQ(frame->width, 5);
        EXPECT_EQ(frame->height, 5);
        for (int y = 0; y < 5; ++y) {
            for (int x = 0; x < 5; ++x) {
                VerifyPixelBGRA(*frame, x, y,
                                expectedBGRA[f][0], expectedBGRA[f][1], expectedBGRA[f][2], expectedBGRA[f][3]);
            }
        }
    }

    EXPECT_EQ(dec->GetNextFrame(), nullptr);

    auto loop0 = dec->SeekToFrame(0);
    ASSERT_NE(loop0, nullptr);
    EXPECT_EQ(loop0->frameMeta.index, 0u);
    VerifyPixelBGRA(*loop0, 4, 4, 0, 0, 255, 255);

    auto s2 = dec->SeekToFrame(2);
    ASSERT_NE(s2, nullptr);
    EXPECT_EQ(s2->frameMeta.index, 2u);
    VerifyPixelBGRA(*s2, 4, 4, 255, 0, 0, 255);

    auto staticJxl = MakeAnimatedJxl(5, 5, false);
    ASSERT_FALSE(staticJxl.empty());
    auto staticDec = QuickView::CreateJxlAnimator();
    EXPECT_FALSE(staticDec->Initialize(MakeMappedBytes(staticJxl), QuickView::PixelFormat::BGRA8888));
}
