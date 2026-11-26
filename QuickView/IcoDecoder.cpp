/*
 * QuickView ICO Multi-Size Decoder - Implementation
 * Copyright (C) 2026-Present QuickView Contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "pch.h"
#include "IcoDecoder.h"
#include <algorithm>
#include <cstring>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace QuickView::IcoDecoder {

namespace {

constexpr size_t kMaxIcoEntries = 32;

inline uint16_t ReadLE16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

inline uint32_t ReadLE32(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

inline uint32_t ReadBE32(const uint8_t* p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

bool IsPngPayload(const uint8_t* p, size_t len) noexcept {
    static constexpr uint8_t kPngSig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    return len >= 8 && std::memcmp(p, kPngSig, 8) == 0;
}

void ProbePngMeta(const uint8_t* p, size_t len, uint16_t& w, uint16_t& h, uint16_t& bpp) noexcept {
    if (len < 33) return;
    // IHDR chunk starts at byte 8: [len:4][IHDR:4][w:4][h:4][bitDepth:1][colorType:1]
    if (std::memcmp(p + 12, "IHDR", 4) != 0) return;
    const uint32_t pw = ReadBE32(p + 16);
    const uint32_t ph = ReadBE32(p + 20);
    if (pw > 0 && pw <= 4096 && ph > 0 && ph <= 4096) {
        w = static_cast<uint16_t>(pw);
        h = static_cast<uint16_t>(ph);
    }
    const uint8_t bitDepth = p[24];
    const uint8_t colorType = p[25];
    uint16_t channels = 4;
    switch (colorType) {
        case 0: channels = 1; break; // Gray
        case 2: channels = 3; break; // RGB
        case 3: channels = 1; break; // Indexed
        case 4: channels = 2; break; // Gray+Alpha
        case 6: channels = 4; break; // RGBA
        default: channels = 4; break;
    }
    if (bitDepth > 0) {
        bpp = (colorType == 3) ? bitDepth : static_cast<uint16_t>(bitDepth * channels);
    }
}

void ProbeDibMeta(const uint8_t* p, size_t len, uint16_t& w, uint16_t& h, uint16_t& bpp) noexcept {
    if (len < 16) return;
    const uint32_t headerSize = ReadLE32(p);
    if (headerSize < 40 || headerSize > len) return;
    const uint16_t planes = ReadLE16(p + 12);
    const uint16_t dibBpp = ReadLE16(p + 14);
    (void)planes;
    if (dibBpp > 0 && bpp == 0) {
        bpp = dibBpp;
    }
    if (w == 0) {
        const uint32_t dw = ReadLE32(p + 4);
        if (dw > 0 && dw <= 4096) w = static_cast<uint16_t>(dw);
    }
    if (h == 0) {
        // ICO DIB height is stored as 2x (XOR + AND mask)
        const uint32_t dh = ReadLE32(p + 8) / 2;
        if (dh > 0 && dh <= 4096) h = static_cast<uint16_t>(dh);
    }
}

HRESULT DecodeWicStreamToBgra(IWICImagingFactory* factory, const uint8_t* buf, size_t bufLen,
                              QuickView::RawImageFrame& outFrame) {
    if (!factory || !buf || bufLen == 0 || bufLen > 0xFFFFFFFFu) {
        return E_INVALIDARG;
    }
    ComPtr<IWICStream> stream;
    HRESULT hr = factory->CreateStream(&stream);
    if (FAILED(hr)) return hr;

    hr = stream->InitializeFromMemory(const_cast<BYTE*>(buf), static_cast<DWORD>(bufLen));
    if (FAILED(hr)) return hr;

    ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr)) return hr;

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) return hr;

    UINT w = 0, h = 0;
    hr = frame->GetSize(&w, &h);
    if (FAILED(hr) || w == 0 || h == 0) return E_FAIL;

    ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(&converter);
    if (FAILED(hr)) return hr;

    hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                               WICBitmapDitherTypeNone, nullptr, 0.0,
                               WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return hr;

    const int stride = static_cast<int>(w) * 4;
    const size_t totalBytes = static_cast<size_t>(stride) * static_cast<size_t>(h);
    uint8_t* pixels = static_cast<uint8_t*>(_aligned_malloc(totalBytes, 64));
    if (!pixels) return E_OUTOFMEMORY;

    hr = converter->CopyPixels(nullptr, static_cast<UINT>(stride), static_cast<UINT>(totalBytes), pixels);
    if (FAILED(hr)) {
        _aligned_free(pixels);
        return hr;
    }

    outFrame = QuickView::RawImageFrame();
    outFrame.pixels = pixels;
    outFrame.width = static_cast<int>(w);
    outFrame.height = static_cast<int>(h);
    outFrame.stride = stride;
    outFrame.format = QuickView::PixelFormat::BGRA8888;
    outFrame.memoryDeleter = QuickView::MemoryDeleter::FromAlignedFree();
    outFrame.colorInfo.dataSpace = QuickView::PixelDataSpace::EncodedSdr;
    outFrame.colorInfo.primaries = QuickView::ColorPrimaries::Unknown;
    outFrame.colorInfo.transfer = QuickView::TransferFunction::SRGB;
    outFrame.colorInfo.nominalBitDepth = 8;
    return S_OK;
}

} // namespace

bool ParseDirectory(const uint8_t* data, size_t size, std::vector<IcoEntryInfo>& outEntries) noexcept {
    outEntries.clear();
    if (!data || size < 6) return false;

    const uint16_t reserved = ReadLE16(data);
    const uint16_t type = ReadLE16(data + 2);
    const uint16_t count = ReadLE16(data + 4);
    if (reserved != 0 || (type != 1 && type != 2) || count == 0) {
        return false;
    }

    const size_t entryCount = (std::min<size_t>)(count, kMaxIcoEntries);
    if (size < 6 + entryCount * 16) return false;

    outEntries.reserve(entryCount);
    for (size_t i = 0; i < entryCount; ++i) {
        const uint8_t* e = data + 6 + i * 16;
        uint16_t w = e[0] == 0 ? 256 : static_cast<uint16_t>(e[0]);
        uint16_t h = e[1] == 0 ? 256 : static_cast<uint16_t>(e[1]);
        uint16_t bpp = ReadLE16(e + 6);
        const uint32_t bytesInRes = ReadLE32(e + 8);
        const uint32_t imgOffset = ReadLE32(e + 12);

        if (bytesInRes == 0 || imgOffset > size || bytesInRes > size - imgOffset) {
            continue;
        }

        const uint8_t* payload = data + imgOffset;
        const bool isPng = IsPngPayload(payload, bytesInRes);
        if (isPng) {
            ProbePngMeta(payload, bytesInRes, w, h, bpp);
        } else {
            ProbeDibMeta(payload, bytesInRes, w, h, bpp);
        }

        IcoEntryInfo info;
        info.rawIndex = static_cast<uint16_t>(i);
        info.width = w;
        info.height = h;
        info.bpp = bpp;
        info.isPng = isPng;
        info.offset = imgOffset;
        info.size = bytesInRes;
        outEntries.push_back(info);
    }

    if (outEntries.empty()) return false;

    std::sort(outEntries.begin(), outEntries.end(), [](const IcoEntryInfo& a, const IcoEntryInfo& b) {
        const uint32_t areaA = static_cast<uint32_t>(a.width) * static_cast<uint32_t>(a.height);
        const uint32_t areaB = static_cast<uint32_t>(b.width) * static_cast<uint32_t>(b.height);
        if (areaA != areaB) return areaA > areaB;
        if (a.bpp != b.bpp) return a.bpp > b.bpp;
        return a.rawIndex < b.rawIndex;
    });
    return true;
}

std::wstring BuildSummary(const std::vector<IcoEntryInfo>& entries) {
    if (entries.empty()) return {};
    if (entries.size() == 1) {
        wchar_t buf[64]{};
        swprintf_s(buf, L"%ux%u %ubpp%s", entries[0].width, entries[0].height,
                   entries[0].bpp, entries[0].isPng ? L" PNG" : L"");
        return buf;
    }
    uint16_t maxW = entries.front().width;
    uint16_t minW = entries.back().width;
    wchar_t buf[64]{};
    swprintf_s(buf, L"%zu sizes (%u..%u)", entries.size(), maxW, minW);
    return buf;
}

HRESULT DecodeEntry(const uint8_t* data, size_t size,
                    const IcoEntryInfo& entry,
                    IWICImagingFactory* wicFactory,
                    QuickView::RawImageFrame& outFrame) {
    if (!data || !wicFactory || entry.offset > size || entry.size > size - entry.offset || entry.size == 0) {
        return E_INVALIDARG;
    }

    const uint8_t* payload = data + entry.offset;
    if (entry.isPng) {
        return DecodeWicStreamToBgra(wicFactory, payload, entry.size, outFrame);
    }

    // Wrap single DIB entry in a minimal 22-byte single-icon container so WIC handles all bit depths & AND masks.
    if (size < 6 + static_cast<size_t>(entry.rawIndex) * 16 + 16) {
        return E_FAIL;
    }
    std::vector<uint8_t> singleIco(22 + entry.size);
    singleIco[0] = 0; singleIco[1] = 0; // reserved
    singleIco[2] = data[2]; singleIco[3] = data[3]; // type (1=ICO, 2=CUR)
    singleIco[4] = 1; singleIco[5] = 0; // count = 1
    std::memcpy(singleIco.data() + 6, data + 6 + static_cast<size_t>(entry.rawIndex) * 16, 12);
    const uint32_t newOffset = 22;
    singleIco[18] = static_cast<uint8_t>(newOffset & 0xFF);
    singleIco[19] = static_cast<uint8_t>((newOffset >> 8) & 0xFF);
    singleIco[20] = static_cast<uint8_t>((newOffset >> 16) & 0xFF);
    singleIco[21] = static_cast<uint8_t>((newOffset >> 24) & 0xFF);
    std::memcpy(singleIco.data() + 22, payload, entry.size);

    return DecodeWicStreamToBgra(wicFactory, singleIco.data(), singleIco.size(), outFrame);
}

HRESULT RenderContactSheet(const uint8_t* data, size_t size,
                           const std::vector<IcoEntryInfo>& entries,
                           IWICImagingFactory* wicFactory,
                           float dpiScale,
                           QuickView::RawImageFrame& outFrame) {
    if (!data || entries.empty() || !wicFactory) return E_INVALIDARG;

    struct DecodedTile {
        IcoEntryInfo info;
        QuickView::RawImageFrame frame;
        float slotW = 0.0f;
        float slotH = 0.0f;
        float x = 0.0f;
        float y = 0.0f;
    };

    std::vector<DecodedTile> tiles;
    tiles.reserve(entries.size());
    for (const auto& entry : entries) {
        QuickView::RawImageFrame frame;
        if (SUCCEEDED(DecodeEntry(data, size, entry, wicFactory, frame)) && frame.pixels && frame.width > 0 && frame.height > 0) {
            DecodedTile tile;
            tile.info = entry;
            tile.frame = std::move(frame);
            tiles.push_back(std::move(tile));
        }
    }
    if (tiles.empty()) return E_FAIL;

    const float scale = (std::clamp)(dpiScale, 1.0f, 3.0f);
    const float outerPad = std::round(24.0f * scale);
    const float cellGap = std::round(18.0f * scale);
    const float innerPad = std::round(10.0f * scale);
    const float labelH = std::round(32.0f * scale);
    const float minBoxSize = std::round(60.0f * scale);
    const float minSlotW = std::round(82.0f * scale);
    const float maxRowWidth = std::round(920.0f * scale);

    float curX = outerPad;
    float curY = outerPad;
    float rowH = 0.0f;
    float canvasW = 0.0f;

    for (auto& tile : tiles) {
        const float imgW = static_cast<float>(tile.frame.width);
        const float imgH = static_cast<float>(tile.frame.height);
        tile.slotW = (std::max)(minSlotW, (std::max)(minBoxSize, imgW) + innerPad * 2.0f);
        tile.slotH = (std::max)(minBoxSize, imgH) + innerPad * 2.0f + labelH;

        if (curX > outerPad && curX + tile.slotW + outerPad > maxRowWidth) {
            curX = outerPad;
            curY += rowH + cellGap;
            rowH = 0.0f;
        }
        tile.x = curX;
        tile.y = curY;
        curX += tile.slotW + cellGap;
        rowH = (std::max)(rowH, tile.slotH);
        canvasW = (std::max)(canvasW, tile.x + tile.slotW + outerPad);
    }
    const float canvasH = curY + rowH + outerPad;

    const UINT outW = (std::max<UINT>)(1, static_cast<UINT>(std::ceil(canvasW)));
    const UINT outH = (std::max<UINT>)(1, static_cast<UINT>(std::ceil(canvasH)));

    ComPtr<IWICBitmap> wicCanvas;
    HRESULT hr = wicFactory->CreateBitmap(outW, outH, GUID_WICPixelFormat32bppPBGRA,
                                          WICBitmapCacheOnDemand, &wicCanvas);
    if (FAILED(hr)) return hr;

    ComPtr<ID2D1Factory> d2dFactory;
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory.GetAddressOf());
    if (FAILED(hr)) return hr;

    D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);

    ComPtr<ID2D1RenderTarget> rt;
    hr = d2dFactory->CreateWicBitmapRenderTarget(wicCanvas.Get(), rtProps, &rt);
    if (FAILED(hr)) return hr;

    ComPtr<ID2D1SolidColorBrush> borderBrush;
    ComPtr<ID2D1SolidColorBrush> textBrush;
    ComPtr<ID2D1SolidColorBrush> subBrush;
    rt->CreateSolidColorBrush(D2D1::ColorF(0.55f, 0.55f, 0.60f, 0.42f), &borderBrush);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.52f, 0.54f, 0.58f, 0.96f), &textBrush);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.45f, 0.48f, 0.54f, 0.80f), &subBrush);

    ComPtr<IDWriteFactory> dwFactory;
    ComPtr<IDWriteTextFormat> fmtMain;
    ComPtr<IDWriteTextFormat> fmtSub;
    if (SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                      reinterpret_cast<IUnknown**>(dwFactory.GetAddressOf())))) {
        dwFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                    11.0f * scale, L"", &fmtMain);
        dwFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                    9.5f * scale, L"", &fmtSub);
        if (fmtMain) {
            fmtMain->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            fmtMain->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        if (fmtSub) {
            fmtSub->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            fmtSub->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }

    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    for (const auto& tile : tiles) {
        const float boxW = static_cast<float>(tile.frame.width);
        const float boxH = static_cast<float>(tile.frame.height);
        const float imgAreaH = tile.slotH - labelH;
        const float imgX = std::floor(tile.x + (tile.slotW - boxW) * 0.5f);
        const float imgY = std::floor(tile.y + (imgAreaH - boxH) * 0.5f);

        D2D1_RECT_F slotRect = D2D1::RectF(tile.x + 0.5f, tile.y + 0.5f,
                                           tile.x + tile.slotW - 0.5f, tile.y + tile.slotH - 0.5f);
        if (borderBrush) {
            rt->DrawRoundedRectangle(D2D1::RoundedRect(slotRect, 6.0f * scale, 6.0f * scale),
                                     borderBrush.Get(), 1.0f);
        }

        ComPtr<ID2D1Bitmap> bmp;
        D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (SUCCEEDED(rt->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(tile.frame.width),
                                                   static_cast<UINT32>(tile.frame.height)),
                                       tile.frame.pixels,
                                       static_cast<UINT32>(tile.frame.stride),
                                       bp, &bmp))) {
            D2D1_RECT_F dst = D2D1::RectF(imgX, imgY, imgX + boxW, imgY + boxH);
            rt->DrawBitmap(bmp.Get(), dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        }

        if (fmtMain && textBrush) {
            wchar_t dimBuf[32]{};
            swprintf_s(dimBuf, L"%u\x00D7%u", tile.info.width, tile.info.height);
            D2D1_RECT_F textRect = D2D1::RectF(tile.x + 2.0f, tile.y + imgAreaH - 2.0f,
                                               tile.x + tile.slotW - 2.0f, tile.y + imgAreaH + 14.0f * scale);
            rt->DrawText(dimBuf, static_cast<UINT32>(wcslen(dimBuf)), fmtMain.Get(), textRect, textBrush.Get());
        }
        if (fmtSub && subBrush) {
            wchar_t subBuf[32]{};
            if (tile.info.bpp > 0) {
                swprintf_s(subBuf, L"%ub %s", tile.info.bpp, tile.info.isPng ? L"PNG" : L"BMP");
            } else {
                swprintf_s(subBuf, L"%s", tile.info.isPng ? L"PNG" : L"BMP");
            }
            D2D1_RECT_F subRect = D2D1::RectF(tile.x + 2.0f, tile.y + imgAreaH + 12.0f * scale,
                                              tile.x + tile.slotW - 2.0f, tile.y + tile.slotH - 2.0f);
            rt->DrawText(subBuf, static_cast<UINT32>(wcslen(subBuf)), fmtSub.Get(), subRect, subBrush.Get());
        }
    }

    hr = rt->EndDraw();
    if (FAILED(hr)) return hr;

    const int outStride = static_cast<int>(outW) * 4;
    const size_t totalBytes = static_cast<size_t>(outStride) * static_cast<size_t>(outH);
    uint8_t* outPixels = static_cast<uint8_t*>(_aligned_malloc(totalBytes, 64));
    if (!outPixels) return E_OUTOFMEMORY;

    WICRect copyRect = {0, 0, static_cast<INT>(outW), static_cast<INT>(outH)};
    hr = wicCanvas->CopyPixels(&copyRect, static_cast<UINT>(outStride), static_cast<UINT>(totalBytes), outPixels);
    if (FAILED(hr)) {
        _aligned_free(outPixels);
        return hr;
    }

    outFrame = QuickView::RawImageFrame();
    outFrame.pixels = outPixels;
    outFrame.width = static_cast<int>(outW);
    outFrame.height = static_cast<int>(outH);
    outFrame.stride = outStride;
    outFrame.format = QuickView::PixelFormat::BGRA8888;
    outFrame.memoryDeleter = QuickView::MemoryDeleter::FromAlignedFree();
    outFrame.colorInfo.dataSpace = QuickView::PixelDataSpace::EncodedSdr;
    outFrame.colorInfo.primaries = QuickView::ColorPrimaries::Unknown;
    outFrame.colorInfo.transfer = QuickView::TransferFunction::SRGB;
    outFrame.colorInfo.nominalBitDepth = 8;
    return S_OK;
}

} // namespace QuickView::IcoDecoder
