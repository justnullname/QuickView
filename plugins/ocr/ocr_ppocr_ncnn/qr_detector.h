#pragma once
/*
 * QuickView QR Code Detector (ZXing-C++ v2.3.0 wrapper)
 * Copyright (C) 2026-Present QuickView Contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "Plugin/abi/qvx_analyzer.h"
#include "ReadBarcode.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace QuickView::OcrNcnn {

struct DetectedQrCode {
    QVX_NormQuad quad{};
    std::string utf8Text;
    bool isUrl = false;
};

namespace detail {

inline bool IsQrCancelled(const QVX_CancelToken& cancel) noexcept {
    return cancel.check_cancel && cancel.check_cancel(cancel.user_data);
}

inline char ToLowerAscii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
}

inline bool StartsWithCaseInsensitiveAscii(std::string_view text, std::string_view prefix) noexcept {
    if (text.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (ToLowerAscii(text[i]) != ToLowerAscii(prefix[i])) return false;
    }
    return true;
}

inline std::string_view TrimAsciiWhitespace(std::string_view text) noexcept {
    auto isWs = [](char c) noexcept {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    while (!text.empty() && isWs(text.front())) text.remove_prefix(1);
    while (!text.empty() && isWs(text.back())) text.remove_suffix(1);
    return text;
}

inline float ComputeQuadIou(const QVX_NormQuad& a, const QVX_NormQuad& b) noexcept {
    const float ix0 = (std::max)(a.min_x, b.min_x);
    const float iy0 = (std::max)(a.min_y, b.min_y);
    const float ix1 = (std::min)(a.max_x, b.max_x);
    const float iy1 = (std::min)(a.max_y, b.max_y);
    if (ix1 <= ix0 || iy1 <= iy0) return 0.0f;

    const float inter = (ix1 - ix0) * (iy1 - iy0);
    const float areaA = (std::max)(0.0f, a.max_x - a.min_x) * (std::max)(0.0f, a.max_y - a.min_y);
    const float areaB = (std::max)(0.0f, b.max_x - b.min_x) * (std::max)(0.0f, b.max_y - b.min_y);
    const float uni = areaA + areaB - inter;
    return (uni > 1e-8f) ? (inter / uni) : 0.0f;
}

inline bool IsDuplicateQr(const DetectedQrCode& cand, const std::vector<DetectedQrCode>& existing) noexcept {
    const float cx = (cand.quad.min_x + cand.quad.max_x) * 0.5f;
    const float cy = (cand.quad.min_y + cand.quad.max_y) * 0.5f;
    for (const auto& item : existing) {
        if (item.utf8Text != cand.utf8Text) continue;
        if (ComputeQuadIou(cand.quad, item.quad) > 0.35f) return true;
        const float ex = (item.quad.min_x + item.quad.max_x) * 0.5f;
        const float ey = (item.quad.min_y + item.quad.max_y) * 0.5f;
        const float dx = cx - ex;
        const float dy = cy - ey;
        const float minW = (std::min)(cand.quad.max_x - cand.quad.min_x, item.quad.max_x - item.quad.min_x);
        const float minH = (std::min)(cand.quad.max_y - cand.quad.min_y, item.quad.max_y - item.quad.min_y);
        const float thresh = (std::max)(0.01f, (std::min)(minW, minH) * 0.5f);
        if (dx * dx + dy * dy < thresh * thresh) return true;
    }
    return false;
}

inline void ExtractMaxChannelAndUpsample2x(
    const uint8_t* bgra,
    uint32_t w,
    uint32_t h,
    uint32_t stride,
    bool need2x,
    std::vector<uint8_t>& outMax1x,
    std::vector<uint8_t>& outMax2x)
{
    outMax1x.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* row = bgra + static_cast<size_t>(y) * stride;
        uint8_t* dst = outMax1x.data() + static_cast<size_t>(y) * w;
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t b = row[x * 4u + 0u];
            const uint8_t g = row[x * 4u + 1u];
            const uint8_t r = row[x * 4u + 2u];
            dst[x] = (std::max)({ r, g, b });
        }
    }

    if (!need2x) {
        outMax2x.clear();
        return;
    }

    const uint32_t dw = w * 2u;
    const uint32_t dh = h * 2u;
    outMax2x.resize(static_cast<size_t>(dw) * static_cast<size_t>(dh));
    for (uint32_t y = 0; y < h; ++y) {
        const uint32_t yNext = (y + 1u < h) ? (y + 1u) : y;
        const uint8_t* r0 = outMax1x.data() + static_cast<size_t>(y) * w;
        const uint8_t* r1 = outMax1x.data() + static_cast<size_t>(yNext) * w;
        uint8_t* d0 = outMax2x.data() + static_cast<size_t>(y * 2u) * dw;
        uint8_t* d1 = d0 + dw;

        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t xNext = (x + 1u < w) ? (x + 1u) : x;
            const uint32_t p00 = r0[x];
            const uint32_t p10 = r0[xNext];
            const uint32_t p01 = r1[x];
            const uint32_t p11 = r1[xNext];

            d0[x * 2u]      = static_cast<uint8_t>(p00);
            d0[x * 2u + 1u] = static_cast<uint8_t>((p00 + p10 + 1u) >> 1);
            d1[x * 2u]      = static_cast<uint8_t>((p00 + p01 + 1u) >> 1);
            d1[x * 2u + 1u] = static_cast<uint8_t>((p00 + p10 + p01 + p11 + 2u) >> 2);
        }
    }
}

} // namespace detail

inline bool IsHttpOrHttpsUrlUtf8(std::string_view text) noexcept {
    for (unsigned char ch : text) {
        if (ch < 0x20u || ch == 0x7Fu) return false;
    }
    const std::string_view trimmed = detail::TrimAsciiWhitespace(text);
    if (trimmed.empty()) return false;
    if (detail::StartsWithCaseInsensitiveAscii(trimmed, "https://")) {
        return trimmed.size() > 8u;
    }
    if (detail::StartsWithCaseInsensitiveAscii(trimmed, "http://")) {
        return trimmed.size() > 7u;
    }
    return false;
}

namespace detail {

inline void CollectBarcodesFromView(
    const ZXing::ImageView& view,
    const ZXing::ReaderOptions& opts,
    const QVX_CancelToken& cancel,
    std::vector<DetectedQrCode>& results)
{
    if (IsQrCancelled(cancel)) return;
    const ZXing::Barcodes barcodes = ZXing::ReadBarcodes(view, opts);
    if (IsQrCancelled(cancel) || barcodes.empty()) return;

    const float invW = 1.0f / static_cast<float>(view.width());
    const float invH = 1.0f / static_cast<float>(view.height());

    for (const ZXing::Barcode& b : barcodes) {
        if (IsQrCancelled(cancel)) break;
        if (!b.isValid()) continue;

        std::string utf8 = b.text(ZXing::TextMode::Plain);
        if (TrimAsciiWhitespace(utf8).empty()) continue;

        const ZXing::Position& pos = b.position();
        const ZXing::PointI pts[4] = {
            pos.topLeft(),
            pos.topRight(),
            pos.bottomRight(),
            pos.bottomLeft()
        };

        QVX_NormQuad quad{};
        float minX = 1.0f;
        float minY = 1.0f;
        float maxX = 0.0f;
        float maxY = 0.0f;
        for (int c = 0; c < 4; ++c) {
            const float nx = std::clamp(static_cast<float>(pts[c].x) * invW, 0.0f, 1.0f);
            const float ny = std::clamp(static_cast<float>(pts[c].y) * invH, 0.0f, 1.0f);
            quad.x[c] = nx;
            quad.y[c] = ny;
            minX = (std::min)(minX, nx);
            minY = (std::min)(minY, ny);
            maxX = (std::max)(maxX, nx);
            maxY = (std::max)(maxY, ny);
        }
        quad.min_x = minX;
        quad.min_y = minY;
        quad.max_x = (std::max)(minX, maxX);
        quad.max_y = (std::max)(minY, maxY);

        DetectedQrCode item;
        item.quad = quad;
        item.isUrl = IsHttpOrHttpsUrlUtf8(utf8);
        item.utf8Text = std::move(utf8);
        if (!IsDuplicateQr(item, results)) {
            results.push_back(std::move(item));
        }
    }
}

} // namespace detail

inline std::vector<DetectedQrCode> DetectQrCodesBgra(
    const uint8_t* bgra,
    uint32_t width,
    uint32_t height,
    uint32_t stride,
    const QVX_CancelToken& cancel)
{
    std::vector<DetectedQrCode> results;
    if (!bgra || width < 16u || height < 16u || stride < width * 4u) return results;
    if (detail::IsQrCancelled(cancel)) return results;

    const ZXing::BarcodeFormats kPrimaryFormats =
        ZXing::BarcodeFormat::QRCode |
        ZXing::BarcodeFormat::MicroQRCode |
        ZXing::BarcodeFormat::DataMatrix |
        ZXing::BarcodeFormat::EAN13 |
        ZXing::BarcodeFormat::EAN8 |
        ZXing::BarcodeFormat::UPCA |
        ZXing::BarcodeFormat::UPCE |
        ZXing::BarcodeFormat::Code128 |
        ZXing::BarcodeFormat::Code39;

    ZXing::ReaderOptions primaryOpts;
    primaryOpts.setFormats(kPrimaryFormats);
    primaryOpts.setTryHarder(true);
    primaryOpts.setTryRotate(true);
    primaryOpts.setTryInvert(true);
    primaryOpts.setTryDownscale(true);
    primaryOpts.setBinarizer(ZXing::Binarizer::LocalAverage);
    primaryOpts.setMinLineCount(3);
    primaryOpts.setMaxNumberOfSymbols(0xff);

    ZXing::ImageView view(
        bgra,
        static_cast<int>(width),
        static_cast<int>(height),
        ZXing::ImageFormat::BGRA,
        static_cast<int>(stride));
    detail::CollectBarcodesFromView(view, primaryOpts, cancel, results);
    if (detail::IsQrCancelled(cancel)) return results;

    // Pass 2: QR-only max(R,G,B) luminance suppresses colored annotation boxes touching quiet zones;
    // optionally run 2x upsample on compact/strip images containing tiny QR codes.
    const uint64_t pixelCount = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    if (pixelCount <= 4096ULL * 4096ULL) {
        ZXing::ReaderOptions qrOnlyOpts = primaryOpts;
        qrOnlyOpts.setFormats(ZXing::BarcodeFormat::QRCode | ZXing::BarcodeFormat::MicroQRCode);

        const bool do2x = (std::min)(width, height) <= 640u && pixelCount <= 2560ULL * 1600ULL;
        std::vector<uint8_t> max1x;
        std::vector<uint8_t> max2x;
        detail::ExtractMaxChannelAndUpsample2x(bgra, width, height, stride, do2x, max1x, max2x);

        if (!detail::IsQrCancelled(cancel)) {
            ZXing::ImageView viewMax1x(
                max1x.data(),
                static_cast<int>(width),
                static_cast<int>(height),
                ZXing::ImageFormat::Lum);
            detail::CollectBarcodesFromView(viewMax1x, qrOnlyOpts, cancel, results);
        }
        if (do2x && !max2x.empty() && !detail::IsQrCancelled(cancel)) {
            ZXing::ImageView viewMax2x(
                max2x.data(),
                static_cast<int>(width * 2u),
                static_cast<int>(height * 2u),
                ZXing::ImageFormat::Lum);
            ZXing::ReaderOptions upsampleOpts = primaryOpts;
            upsampleOpts.setTryDownscale(false);
            detail::CollectBarcodesFromView(viewMax2x, upsampleOpts, cancel, results);
        }
    }

    return results;
}

} // namespace QuickView::OcrNcnn
