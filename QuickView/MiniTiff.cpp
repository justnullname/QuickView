/*
 * QuickView Mini TIFF Decoder - Core implementation
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
#include "MiniTiff.h"
#include "ImageLoaderSimd.h"
#include <cstring>
#include <vector>
#include <span>
#include <atomic>
#include <limits>
#include <omp.h>
#include <zlib.h>
#include <algorithm>

namespace QuickView::MiniTiff {

static constexpr uint32_t kMaxImageDimension = 262144;
static constexpr uint16_t kMaxSamplesPerPixel = 16;
static constexpr size_t kMaxUnitBufferBytes = 256ULL * 1024 * 1024;

static inline bool CheckedMul(size_t a, size_t b, size_t& out) noexcept {
    if (a != 0 && b > (std::numeric_limits<size_t>::max)() / a) {
        return false;
    }
    out = a * b;
    return true;
}

static inline bool CheckRange(size_t offset, size_t count, size_t elemSize, size_t totalSize) noexcept {
    if (offset > totalSize) {
        return false;
    }
    size_t byteLen = 0;
    if (!CheckedMul(count, elemSize, byteLen)) {
        return false;
    }
    return byteLen <= totalSize - offset;
}

struct TiffImageDesc {
    uint32_t width = 0;
    uint32_t height = 0;
    uint16_t compression = 1;
    uint16_t photometric = 2;
    uint16_t samples = 1;
    uint16_t bitsPerSample = 8;
    uint16_t planarConfig = 1;
    uint16_t predictor = 1;
    uint16_t sampleFormat = 1;
    uint16_t orientation = 1;
    uint16_t extraSamples = 0;
    uint32_t rowsPerStrip = 0;
    bool isTiled = false;
    uint32_t tileWidth = 0;
    uint32_t tileHeight = 0;
    bool isLE = true;

    std::vector<uint64_t> offsets;
    std::vector<uint64_t> byteCounts;
    std::span<const uint8_t> iccProfile;
    std::vector<uint16_t> colorMap;
};

// PackBits decoder implementation
static bool DecompressPackBits(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstLen) {
    size_t srcIdx = 0;
    size_t dstIdx = 0;
    while (srcIdx < srcLen && dstIdx < dstLen) {
        int8_t n = static_cast<int8_t>(src[srcIdx++]);
        if (n >= 0) {
            size_t count = static_cast<size_t>(n) + 1;
            if (count > srcLen - srcIdx || count > dstLen - dstIdx) {
                return false;
            }
            std::memcpy(dst + dstIdx, src + srcIdx, count);
            srcIdx += count;
            dstIdx += count;
        } else if (n != -128) {
            size_t count = static_cast<size_t>(-n) + 1;
            if (srcIdx >= srcLen || count > dstLen - dstIdx) {
                return false;
            }
            uint8_t val = src[srcIdx++];
            std::memset(dst + dstIdx, val, count);
            dstIdx += count;
        }
    }
    return dstIdx == dstLen;
}

static bool DecompressDeflate(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstLen) {
    if (srcLen > (std::numeric_limits<uInt>::max)() || dstLen > (std::numeric_limits<uInt>::max)()) {
        return false;
    }

    z_stream strm;
    std::memset(&strm, 0, sizeof(strm));
    strm.next_in = const_cast<Bytef*>(src);
    strm.avail_in = static_cast<uInt>(srcLen);
    strm.next_out = dst;
    strm.avail_out = static_cast<uInt>(dstLen);

    // Try standard zlib wrapper (15)
    int ret = inflateInit2(&strm, 15);
    if (ret == Z_OK) {
        ret = inflate(&strm, Z_FINISH);
        inflateEnd(&strm);
        if (ret == Z_STREAM_END && strm.total_out == dstLen) {
            return true;
        }
    }

    // Try raw deflate retry (-15)
    std::memset(&strm, 0, sizeof(strm));
    strm.next_in = const_cast<Bytef*>(src);
    strm.avail_in = static_cast<uInt>(srcLen);
    strm.next_out = dst;
    strm.avail_out = static_cast<uInt>(dstLen);

    ret = inflateInit2(&strm, -15);
    if (ret == Z_OK) {
        ret = inflate(&strm, Z_FINISH);
        inflateEnd(&strm);
        if (ret == Z_STREAM_END && strm.total_out == dstLen) {
            return true;
        }
    }

    return false;
}

static Status ParseTiffIFD(const uint8_t* data, size_t size, TiffImageDesc& desc) {
    if (size < 8) return Status::NotTiff;

    bool isLE = false;
    if (data[0] == 0x49 && data[1] == 0x49 && data[2] == 0x2A && data[3] == 0x00) {
        isLE = true;
    } else if (data[0] == 0x4D && data[1] == 0x4D && data[2] == 0x00 && data[3] == 0x2A) {
        isLE = false;
    } else {
        return Status::NotTiff;
    }
    desc.isLE = isLE;

    auto read16 = [&](size_t off) -> uint16_t {
        if (off > size || size - off < 2) return 0;
        if (isLE) {
            return data[off] | (static_cast<uint16_t>(data[off + 1]) << 8);
        } else {
            return (static_cast<uint16_t>(data[off]) << 8) | data[off + 1];
        }
    };

    auto read32 = [&](size_t off) -> uint32_t {
        if (off > size || size - off < 4) return 0;
        if (isLE) {
            return data[off] | (static_cast<uint32_t>(data[off + 1]) << 8) |
                   (static_cast<uint32_t>(data[off + 2]) << 16) | (static_cast<uint32_t>(data[off + 3]) << 24);
        } else {
            return (static_cast<uint32_t>(data[off]) << 24) | (static_cast<uint32_t>(data[off + 1]) << 16) |
                   (static_cast<uint32_t>(data[off + 2]) << 8) | data[off + 3];
        }
    };

    auto getTagValue = [&](size_t entryOff, uint16_t type, uint32_t count) -> uint32_t {
        if (count != 1) return 0;
        if (type == 3) {
            return read16(entryOff + 8);
        } else if (type == 4) {
            return read32(entryOff + 8);
        } else if (type == 1) {
            return data[entryOff + 8];
        }
        return 0;
    };

    uint32_t ifdOffset = read32(4);
    if (ifdOffset == 0 || !CheckRange(ifdOffset, 1, 2, size)) {
        return Status::Corrupt;
    }

    uint16_t numEntries = read16(ifdOffset);
    size_t current = static_cast<size_t>(ifdOffset) + 2;
    if (!CheckRange(current, numEntries, 12, size)) {
        return Status::Corrupt;
    }

    uint32_t stripOffsetsOffset = 0;
    uint32_t stripOffsetsCount = 0;
    uint16_t stripOffsetsType = 0;

    uint32_t stripByteCountsOffset = 0;
    uint32_t stripByteCountsCount = 0;
    uint16_t stripByteCountsType = 0;

    uint32_t tileOffsetsOffset = 0;
    uint32_t tileOffsetsCount = 0;
    uint16_t tileOffsetsType = 0;

    uint32_t tileByteCountsOffset = 0;
    uint32_t tileByteCountsCount = 0;
    uint16_t tileByteCountsType = 0;

    for (uint16_t i = 0; i < numEntries; ++i) {
        size_t entryOff = current + i * 12;
        uint16_t tag = read16(entryOff);
        uint16_t type = read16(entryOff + 2);
        uint32_t count = read32(entryOff + 4);

        switch (tag) {
            case 256: // ImageWidth
                desc.width = getTagValue(entryOff, type, count);
                break;
            case 257: // ImageHeight
                desc.height = getTagValue(entryOff, type, count);
                break;
            case 258: // BitsPerSample
                if (count == 1) {
                    desc.bitsPerSample = static_cast<uint16_t>(getTagValue(entryOff, type, count));
                } else if (count > 1) {
                    uint32_t valOrOff = read32(entryOff + 8);
                    if (type == 3) {
                        if (!CheckRange(valOrOff, count, 2, size)) return Status::Corrupt;
                        desc.bitsPerSample = read16(valOrOff);
                        for (uint32_t j = 1; j < count; ++j) {
                            if (read16(static_cast<size_t>(valOrOff) + static_cast<size_t>(j) * 2) != desc.bitsPerSample) {
                                return Status::Unsupported; // Inconsistent bit depths
                            }
                        }
                    } else {
                        return Status::Unsupported;
                    }
                }
                break;
            case 259: // Compression
                desc.compression = static_cast<uint16_t>(getTagValue(entryOff, type, count));
                break;
            case 262: // PhotometricInterpretation
                desc.photometric = static_cast<uint16_t>(getTagValue(entryOff, type, count));
                break;
            case 273: // StripOffsets
                stripOffsetsOffset = read32(entryOff + 8);
                stripOffsetsCount = count;
                stripOffsetsType = type;
                break;
            case 274: // Orientation
                desc.orientation = static_cast<uint16_t>(getTagValue(entryOff, type, count));
                break;
            case 277: // SamplesPerPixel
                desc.samples = static_cast<uint16_t>(getTagValue(entryOff, type, count));
                break;
            case 278: // RowsPerStrip
                desc.rowsPerStrip = getTagValue(entryOff, type, count);
                break;
            case 279: // StripByteCounts
                stripByteCountsOffset = read32(entryOff + 8);
                stripByteCountsCount = count;
                stripByteCountsType = type;
                break;
            case 284: // PlanarConfiguration
                desc.planarConfig = static_cast<uint16_t>(getTagValue(entryOff, type, count));
                break;
            case 317: // Predictor
                desc.predictor = static_cast<uint16_t>(getTagValue(entryOff, type, count));
                break;
            case 339: // SampleFormat
                desc.sampleFormat = static_cast<uint16_t>(getTagValue(entryOff, type, count));
                break;
            case 320: // ColorMap
                if (type == 3) {
                    uint32_t valOrOff = read32(entryOff + 8);
                    if (!CheckRange(valOrOff, count, 2, size)) return Status::Corrupt;
                    desc.colorMap.resize(count);
                    for (uint32_t j = 0; j < count; ++j) {
                        desc.colorMap[j] = read16(static_cast<size_t>(valOrOff) + static_cast<size_t>(j) * 2);
                    }
                }
                break;
            case 322: // TileWidth
                desc.isTiled = true;
                desc.tileWidth = getTagValue(entryOff, type, count);
                break;
            case 323: // TileLength
                desc.tileHeight = getTagValue(entryOff, type, count);
                break;
            case 324: // TileOffsets
                tileOffsetsOffset = read32(entryOff + 8);
                tileOffsetsCount = count;
                tileOffsetsType = type;
                break;
            case 325: // TileByteCounts
                tileByteCountsOffset = read32(entryOff + 8);
                tileByteCountsCount = count;
                tileByteCountsType = type;
                break;
            case 338: // ExtraSamples
                desc.extraSamples = static_cast<uint16_t>(getTagValue(entryOff, type, count));
                break;
            case 34675: // ICCProfile
                if (type == 7 || type == 1) {
                    uint32_t valOrOff = read32(entryOff + 8);
                    if (CheckRange(valOrOff, count, 1, size)) {
                        desc.iccProfile = std::span<const uint8_t>(data + valOrOff, count);
                    }
                }
                break;
        }
    }

    // Populate offsets and byteCounts
    auto readUintArray = [&](uint32_t arrayOffset, uint32_t count, uint16_t type, std::vector<uint64_t>& dst) -> Status {
        if (count == 1) {
            dst.resize(1);
            dst[0] = arrayOffset;
            return Status::Ok;
        }
        size_t elemSize = 0;
        if (type == 3) {
            elemSize = 2;
        } else if (type == 4) {
            elemSize = 4;
        } else {
            return Status::Unsupported;
        }
        if (!CheckRange(arrayOffset, count, elemSize, size)) {
            return Status::Corrupt;
        }
        dst.resize(count);
        for (uint32_t j = 0; j < count; ++j) {
            size_t pos = static_cast<size_t>(arrayOffset) + static_cast<size_t>(j) * elemSize;
            dst[j] = (elemSize == 2) ? read16(pos) : read32(pos);
        }
        return Status::Ok;
    };

    if (desc.isTiled) {
        if (tileOffsetsCount == 0 || tileOffsetsCount != tileByteCountsCount) {
            return Status::Corrupt;
        }
        Status st = readUintArray(tileOffsetsOffset, tileOffsetsCount, tileOffsetsType, desc.offsets);
        if (st != Status::Ok) return st;
        st = readUintArray(tileByteCountsOffset, tileByteCountsCount, tileByteCountsType, desc.byteCounts);
        if (st != Status::Ok) return st;
    } else {
        if (stripOffsetsCount == 0 || stripOffsetsCount != stripByteCountsCount) {
            return Status::Corrupt;
        }
        Status st = readUintArray(stripOffsetsOffset, stripOffsetsCount, stripOffsetsType, desc.offsets);
        if (st != Status::Ok) return st;
        st = readUintArray(stripByteCountsOffset, stripByteCountsCount, stripByteCountsType, desc.byteCounts);
        if (st != Status::Ok) return st;
    }

    return Status::Ok;
}

static Status CapabilityGate(const TiffImageDesc& desc) {
    if (desc.width == 0 || desc.height == 0) return Status::Corrupt;
    if (desc.width > kMaxImageDimension || desc.height > kMaxImageDimension) {
        return Status::Unsupported;
    }

    if (desc.isTiled) {
        if (desc.tileWidth == 0 || desc.tileHeight == 0) {
            return Status::Corrupt;
        }
        if (desc.tileWidth > kMaxImageDimension || desc.tileHeight > kMaxImageDimension) {
            return Status::Unsupported;
        }
    }

    // Phase 1 supported compressions: 1 (Uncompressed), 32773 (PackBits), 5 (LZW), 8 (Deflate), 32946 (Deflate)
    if (desc.compression != 1 && desc.compression != 32773 &&
        desc.compression != 5 && desc.compression != 8 && desc.compression != 32946) {
        return Status::Unsupported;
    }

    // 8-bit and 16-bit unsigned integers supported (16-bit will be downsampled to 8-bit)
    if (desc.bitsPerSample != 8 && desc.bitsPerSample != 16) {
        return Status::Unsupported;
    }
    if (desc.sampleFormat != 1) {
        return Status::Unsupported;
    }

    // Photometrics supported: 0 (WhiteIsZero), 1 (BlackIsZero), 2 (RGB), 3 (Palette), 5 (CMYK)
    if (desc.photometric > 5 || desc.photometric == 4) {
        return Status::Unsupported;
    }

    // PlanarConfiguration: only contiguous (1) supported
    if (desc.planarConfig != 1) {
        return Status::Unsupported;
    }

    // SamplesPerPixel checks
    if (desc.photometric == 2 && desc.samples < 3) return Status::Corrupt;
    if (desc.photometric == 5 && desc.samples < 4) return Status::Corrupt;
    if (desc.photometric != 2 && desc.photometric != 5 && desc.samples < 1) return Status::Corrupt;
    if (desc.samples > kMaxSamplesPerPixel) return Status::Unsupported;

    // Predictor: supports None (1) or Horizontal (2)
    if (desc.predictor != 1 && desc.predictor != 2) {
        return Status::Unsupported;
    }

    return Status::Ok;
}

static void UndoHorizontalPredictor(uint8_t* rowData, uint32_t width, uint32_t samples,
                                    uint32_t bytesPerSample, bool isLE) {
    if (!rowData || width <= 1 || samples == 0) return;
    const size_t pixelStride = static_cast<size_t>(samples) * bytesPerSample;
    if (bytesPerSample == 2) {
        for (uint32_t x = 1; x < width; ++x) {
            const size_t currBase = static_cast<size_t>(x) * pixelStride;
            const size_t prevBase = currBase - pixelStride;
            for (uint32_t c = 0; c < samples; ++c) {
                const size_t curOff = currBase + static_cast<size_t>(c) * 2u;
                const size_t prvOff = prevBase + static_cast<size_t>(c) * 2u;
                const uint16_t prevVal = isLE
                    ? static_cast<uint16_t>(rowData[prvOff] | (static_cast<uint16_t>(rowData[prvOff + 1]) << 8))
                    : static_cast<uint16_t>((static_cast<uint16_t>(rowData[prvOff]) << 8) | rowData[prvOff + 1]);
                const uint16_t currVal = isLE
                    ? static_cast<uint16_t>(rowData[curOff] | (static_cast<uint16_t>(rowData[curOff + 1]) << 8))
                    : static_cast<uint16_t>((static_cast<uint16_t>(rowData[curOff]) << 8) | rowData[curOff + 1]);
                const uint16_t sum = static_cast<uint16_t>(prevVal + currVal);
                if (isLE) {
                    rowData[curOff] = static_cast<uint8_t>(sum & 0xFFu);
                    rowData[curOff + 1] = static_cast<uint8_t>(sum >> 8);
                } else {
                    rowData[curOff] = static_cast<uint8_t>(sum >> 8);
                    rowData[curOff + 1] = static_cast<uint8_t>(sum & 0xFFu);
                }
            }
        }
    } else {
        for (uint32_t x = 1; x < width; ++x) {
            const size_t currBase = static_cast<size_t>(x) * pixelStride;
            const size_t prevBase = currBase - pixelStride;
            for (size_t c = 0; c < pixelStride; ++c) {
                rowData[currBase + c] += rowData[prevBase + c];
            }
        }
    }
}

HRESULT LoadRegion(const uint8_t* data, size_t size,
                   const QuickView::Codec::DecodeContext& ctx,
                   QuickView::Codec::DecodeResult& result,
                   int cropX, int cropY, int cropW, int cropH) {
    TiffImageDesc desc;
    Status status = ParseTiffIFD(data, size, desc);
    if (status != Status::Ok) {
        return (status == Status::NotTiff) ? E_NOTIMPL : E_FAIL;
    }

    status = CapabilityGate(desc);
    if (status != Status::Ok) {
        return (status == Status::Unsupported) ? E_NOTIMPL : E_FAIL;
    }

    // Clamp crop parameters to valid boundaries
    cropX = (std::max)(0, cropX);
    cropY = (std::max)(0, cropY);
    cropW = (std::min)(cropW, static_cast<int>(desc.width) - cropX);
    cropH = (std::min)(cropH, static_cast<int>(desc.height) - cropY);
    if (cropW <= 0 || cropH <= 0) return E_INVALIDARG;

    uint32_t samples = desc.samples;
    uint32_t bytesPerSample = desc.bitsPerSample / 8;
    if (bytesPerSample == 0) bytesPerSample = 1;
    uint32_t highByteOffset = (bytesPerSample == 2) ? (desc.isLE ? 1 : 0) : 0;

    size_t pixelStride = 0;
    size_t rowBytes = 0;
    if (!CheckedMul(static_cast<size_t>(samples), static_cast<size_t>(bytesPerSample), pixelStride) ||
        pixelStride == 0 ||
        !CheckedMul(static_cast<size_t>(desc.width), pixelStride, rowBytes)) {
        return E_FAIL;
    }

    uint32_t tilesPerRow = 0;
    uint32_t tilesCount = 0;
    size_t tileRowBytes = 0;
    size_t tileBytes = 0;
    uint32_t rowsPerStrip = 0;
    uint32_t stripsCount = 0;

    if (desc.isTiled) {
        uint64_t tilesPerRow64 = 1ULL + (static_cast<uint64_t>(desc.width) - 1ULL) / desc.tileWidth;
        uint64_t tilesPerCol64 = 1ULL + (static_cast<uint64_t>(desc.height) - 1ULL) / desc.tileHeight;
        size_t tilesCountSz = 0;
        if (!CheckedMul(static_cast<size_t>(tilesPerRow64), static_cast<size_t>(tilesPerCol64), tilesCountSz) ||
            tilesCountSz > static_cast<size_t>((std::numeric_limits<int>::max)()) ||
            desc.offsets.size() < tilesCountSz || desc.byteCounts.size() < tilesCountSz) {
            return E_FAIL;
        }
        tilesPerRow = static_cast<uint32_t>(tilesPerRow64);
        tilesCount = static_cast<uint32_t>(tilesCountSz);

        if (!CheckedMul(static_cast<size_t>(desc.tileWidth), pixelStride, tileRowBytes) ||
            !CheckedMul(tileRowBytes, static_cast<size_t>(desc.tileHeight), tileBytes) ||
            tileBytes == 0 || tileBytes > kMaxUnitBufferBytes) {
            return E_FAIL;
        }
    } else {
        rowsPerStrip = desc.rowsPerStrip;
        if (rowsPerStrip == 0 || rowsPerStrip > desc.height) {
            rowsPerStrip = desc.height;
        }
        uint64_t stripsCount64 = 1ULL + (static_cast<uint64_t>(desc.height) - 1ULL) / rowsPerStrip;
        if (stripsCount64 > static_cast<uint64_t>((std::numeric_limits<int>::max)()) ||
            desc.offsets.size() < stripsCount64 || desc.byteCounts.size() < stripsCount64) {
            return E_FAIL;
        }
        stripsCount = static_cast<uint32_t>(stripsCount64);
    }

    // Prepare Output
    int outStride = ((cropW * 4) + 63) & ~63; // 64-byte aligned (cropW <= kMaxImageDimension guarantees no int overflow)
    size_t totalBytes = 0;
    if (!CheckedMul(static_cast<size_t>(outStride), static_cast<size_t>(cropH), totalBytes)) {
        return E_OUTOFMEMORY;
    }
    uint8_t* pixels = ctx.allocator(totalBytes);
    if (!pixels) {
        return E_OUTOFMEMORY;
    }
    std::memset(pixels, 0, totalBytes);

    uint16_t predictor = desc.predictor;
    uint16_t photometric = desc.photometric;
    const bool needRgb8SimdBuf = (photometric == 2 && bytesPerSample == 2 && samples == 3 && highByteOffset == 1);

    std::atomic<bool> decodeFailed{false};

    if (desc.isTiled) {
        // Tiled parallel decoder
        [[maybe_unused]] bool useParallel = (tilesCount >= 4 && cropH >= 512);

        #pragma omp parallel if(useParallel) shared(decodeFailed)
        {
            std::vector<uint8_t> localBuf;
            std::vector<uint8_t> predictorBuf;
            std::vector<uint8_t> rgb8Buf;
            if (needRgb8SimdBuf) {
                rgb8Buf.resize(static_cast<size_t>((std::min)(static_cast<uint32_t>(cropW), desc.tileWidth)) * 3);
            }

            #pragma omp for
            for (int i = 0; i < static_cast<int>(tilesCount); ++i) {
                if (decodeFailed.load(std::memory_order_relaxed)) continue;

                if (ctx.checkCancel && ctx.checkCancel()) {
                    decodeFailed.store(true, std::memory_order_relaxed);
                    continue;
                }

                // Locate Tile boundary
                int tileX = static_cast<int>((static_cast<uint32_t>(i) % tilesPerRow) * desc.tileWidth);
                int tileY = static_cast<int>((static_cast<uint32_t>(i) / tilesPerRow) * desc.tileHeight);
                int unitW = (std::min)(static_cast<int>(desc.tileWidth), static_cast<int>(desc.width) - tileX);
                int unitH = (std::min)(static_cast<int>(desc.tileHeight), static_cast<int>(desc.height) - tileY);

                // Compute Intersection with requested ROI
                int intersectX = (std::max)(cropX, tileX);
                int intersectEndX = (std::min)(cropX + cropW, tileX + unitW);
                int intersectY = (std::max)(cropY, tileY);
                int intersectEndY = (std::min)(cropY + cropH, tileY + unitH);

                if (intersectX >= intersectEndX || intersectY >= intersectEndY) {
                    continue; // No overlap with this tile, skip decoding!
                }

                uint64_t offset = desc.offsets[i];
                uint64_t byteCount = desc.byteCounts[i];
                if (offset > size || byteCount > size - offset) {
                    decodeFailed.store(true, std::memory_order_relaxed);
                    continue;
                }

                const uint8_t* tileData = nullptr;

                if (desc.compression == 1) {
                    if (tileBytes > size - offset) {
                        decodeFailed.store(true, std::memory_order_relaxed);
                        continue;
                    }
                    tileData = data + offset;
                } else if (desc.compression == 32773) {
                    localBuf.resize(tileBytes);
                    if (!DecompressPackBits(data + offset, static_cast<size_t>(byteCount), localBuf.data(), tileBytes)) {
                        decodeFailed.store(true, std::memory_order_relaxed);
                        continue;
                    }
                    tileData = localBuf.data();
                } else if (desc.compression == 5) {
                    localBuf.resize(tileBytes);
                    if (!DecompressLzw(data + offset, static_cast<size_t>(byteCount), localBuf.data(), tileBytes)) {
                        decodeFailed.store(true, std::memory_order_relaxed);
                        continue;
                    }
                    tileData = localBuf.data();
                } else if (desc.compression == 8 || desc.compression == 32946) {
                    localBuf.resize(tileBytes);
                    if (!DecompressDeflate(data + offset, static_cast<size_t>(byteCount), localBuf.data(), tileBytes)) {
                        decodeFailed.store(true, std::memory_order_relaxed);
                        continue;
                    }
                    tileData = localBuf.data();
                }

                if (!tileData) {
                    decodeFailed.store(true, std::memory_order_relaxed);
                    continue;
                }

                // Undo predictor
                if (predictor == 2) {
                    if (desc.compression == 1) {
                        predictorBuf.assign(tileData, tileData + tileBytes);
                        tileData = predictorBuf.data();
                    }
                    uint8_t* mutableData = const_cast<uint8_t*>(tileData);
                    for (uint32_t y = 0; y < desc.tileHeight; ++y) {
                        uint8_t* rowData = mutableData + static_cast<size_t>(y) * tileRowBytes;
                        UndoHorizontalPredictor(rowData, desc.tileWidth, samples, bytesPerSample, desc.isLE);
                    }
                }

                // Pack pixels falling into ROI
                for (int y = intersectY; y < intersectEndY; ++y) {
                    size_t localY = static_cast<size_t>(y - tileY);
                    size_t outY = static_cast<size_t>(y - cropY);
                    const uint8_t* srcRow = tileData + localY * tileRowBytes;
                    uint8_t* dstRow = pixels + outY * static_cast<size_t>(outStride);

                    if (photometric == 0 || photometric == 1) {
                        for (int x = intersectX; x < intersectEndX; ++x) {
                            size_t localX = static_cast<size_t>(x - tileX);
                            size_t outX = static_cast<size_t>(x - cropX);
                            uint8_t val = srcRow[localX * pixelStride + highByteOffset];
                            if (photometric == 0) val = 255 - val;
                            dstRow[outX * 4 + 0] = val;
                            dstRow[outX * 4 + 1] = val;
                            dstRow[outX * 4 + 2] = val;
                            dstRow[outX * 4 + 3] = (samples > 1) ? srcRow[localX * pixelStride + bytesPerSample + highByteOffset] : 255;
                        }
                    } else if (photometric == 2) {
                        int runWidth = intersectEndX - intersectX;
                        // [Titan Perf] SIMD Accelerated 16-to-8 bit downsampling + BGRA Swizzle
                        // Only applicable when image is RGB (samples == 3) and Little-Endian (highByteOffset == 1)
                        if (needRgb8SimdBuf) {
                            size_t localXStart = static_cast<size_t>(intersectX - tileX);
                            size_t outXStart = static_cast<size_t>(intersectX - cropX);
                            const uint16_t* src16 = reinterpret_cast<const uint16_t*>(srcRow + localXStart * pixelStride);
                            uint8_t* dst32 = dstRow + outXStart * 4;
                            
                            ImageLoaderSimd::Pack16to8(src16, rgb8Buf.data(), runWidth * 3);
                            ImageLoaderSimd::ConvertRGBToBGRA(rgb8Buf.data(), dst32, runWidth, 1, runWidth * 4);
                        } else {
                            for (int x = intersectX; x < intersectEndX; ++x) {
                                size_t localX = static_cast<size_t>(x - tileX);
                                size_t outX = static_cast<size_t>(x - cropX);
                                uint8_t r = srcRow[localX * pixelStride + 0 * bytesPerSample + highByteOffset];
                                uint8_t g = srcRow[localX * pixelStride + 1 * bytesPerSample + highByteOffset];
                                uint8_t b = srcRow[localX * pixelStride + 2 * bytesPerSample + highByteOffset];
                                uint8_t a = (samples >= 4) ? srcRow[localX * pixelStride + 3 * bytesPerSample + highByteOffset] : 255;
                                if (samples >= 4 && desc.extraSamples != 1) {
                                    r = static_cast<uint8_t>((r * a + 127) / 255);
                                    g = static_cast<uint8_t>((g * a + 127) / 255);
                                    b = static_cast<uint8_t>((b * a + 127) / 255);
                                }
                                dstRow[outX * 4 + 0] = b;
                                dstRow[outX * 4 + 1] = g;
                                dstRow[outX * 4 + 2] = r;
                                dstRow[outX * 4 + 3] = a;
                            }
                        }
                    } else if (photometric == 3) {
                        if (desc.colorMap.size() >= 768) {
                            for (int x = intersectX; x < intersectEndX; ++x) {
                                size_t localX = static_cast<size_t>(x - tileX);
                                size_t outX = static_cast<size_t>(x - cropX);
                                uint8_t idx = srcRow[localX * pixelStride + highByteOffset];
                                uint8_t r = static_cast<uint8_t>(desc.colorMap[idx] / 256);
                                uint8_t g = static_cast<uint8_t>(desc.colorMap[idx + 256] / 256);
                                uint8_t b = static_cast<uint8_t>(desc.colorMap[idx + 512] / 256);
                                dstRow[outX * 4 + 0] = b;
                                dstRow[outX * 4 + 1] = g;
                                dstRow[outX * 4 + 2] = r;
                                dstRow[outX * 4 + 3] = 255;
                            }
                        } else {
                            decodeFailed.store(true, std::memory_order_relaxed);
                        }
                    } else if (photometric == 5) {
                        size_t localXStart = static_cast<size_t>(intersectX - tileX);
                        size_t outXStart = static_cast<size_t>(intersectX - cropX);
                        int runWidth = intersectEndX - intersectX;
                        if (bytesPerSample == 1) {
                            ConvertCmykToBgra(srcRow + localXStart * pixelStride, dstRow + outXStart * 4, runWidth, samples);
                        } else {
                            for (int dx = 0; dx < runWidth; ++dx) {
                                size_t srcBase = (localXStart + static_cast<size_t>(dx)) * pixelStride;
                                uint8_t c = srcRow[srcBase + 0 * bytesPerSample + highByteOffset];
                                uint8_t m = srcRow[srcBase + 1 * bytesPerSample + highByteOffset];
                                uint8_t ye = srcRow[srcBase + 2 * bytesPerSample + highByteOffset];
                                uint8_t k = (samples >= 4) ? srcRow[srcBase + 3 * bytesPerSample + highByteOffset] : 0;
                                uint8_t r = (255 - c) * (255 - k) / 255;
                                uint8_t g = (255 - m) * (255 - k) / 255;
                                uint8_t b = (255 - ye) * (255 - k) / 255;
                                dstRow[(outXStart + static_cast<size_t>(dx)) * 4 + 0] = b;
                                dstRow[(outXStart + static_cast<size_t>(dx)) * 4 + 1] = g;
                                dstRow[(outXStart + static_cast<size_t>(dx)) * 4 + 2] = r;
                                dstRow[(outXStart + static_cast<size_t>(dx)) * 4 + 3] = 255;
                            }
                        }
                    }
                }
            }
        }
    } else {
        // Strip layout
        [[maybe_unused]] bool useParallel = (stripsCount >= 4 && cropH >= 512);
        int startStrip = cropY / static_cast<int>(rowsPerStrip);
        int endStrip = (std::min)((cropY + cropH - 1) / static_cast<int>(rowsPerStrip), static_cast<int>(stripsCount) - 1);

        #pragma omp parallel if(useParallel) shared(decodeFailed)
        {
            std::vector<uint8_t> localBuf;
            std::vector<uint8_t> predictorBuf;
            std::vector<uint8_t> rgb8Buf;
            if (needRgb8SimdBuf) {
                rgb8Buf.resize(static_cast<size_t>(cropW) * 3);
            }

            #pragma omp for
            for (int i = startStrip; i <= endStrip; ++i) {
                if (decodeFailed.load(std::memory_order_relaxed)) continue;

                if (ctx.checkCancel && ctx.checkCancel()) {
                    decodeFailed.store(true, std::memory_order_relaxed);
                    continue;
                }

                int startY = i * static_cast<int>(rowsPerStrip);
                int unitH = (std::min)(static_cast<int>(rowsPerStrip), static_cast<int>(desc.height) - startY);

                // Y overlap check
                int intersectY = (std::max)(cropY, startY);
                int intersectEndY = (std::min)(cropY + cropH, startY + unitH);

                if (intersectY >= intersectEndY) {
                    continue; // Skip strip with no vertical intersection
                }

                uint64_t offset = desc.offsets[i];
                uint64_t byteCount = desc.byteCounts[i];
                if (offset > size || byteCount > size - offset) {
                    decodeFailed.store(true, std::memory_order_relaxed);
                    continue;
                }

                size_t stripBytes = 0;
                if (!CheckedMul(static_cast<size_t>(unitH), rowBytes, stripBytes) ||
                    stripBytes == 0 || stripBytes > kMaxUnitBufferBytes) {
                    decodeFailed.store(true, std::memory_order_relaxed);
                    continue;
                }

                const uint8_t* stripData = nullptr;

                if (desc.compression == 1) {
                    if (stripBytes > size - offset) {
                        decodeFailed.store(true, std::memory_order_relaxed);
                        continue;
                    }
                    stripData = data + offset;
                } else if (desc.compression == 32773) {
                    localBuf.resize(stripBytes);
                    if (!DecompressPackBits(data + offset, static_cast<size_t>(byteCount), localBuf.data(), stripBytes)) {
                        decodeFailed.store(true, std::memory_order_relaxed);
                        continue;
                    }
                    stripData = localBuf.data();
                } else if (desc.compression == 5) {
                    localBuf.resize(stripBytes);
                    if (!DecompressLzw(data + offset, static_cast<size_t>(byteCount), localBuf.data(), stripBytes)) {
                        decodeFailed.store(true, std::memory_order_relaxed);
                        continue;
                    }
                    stripData = localBuf.data();
                } else if (desc.compression == 8 || desc.compression == 32946) {
                    localBuf.resize(stripBytes);
                    if (!DecompressDeflate(data + offset, static_cast<size_t>(byteCount), localBuf.data(), stripBytes)) {
                        decodeFailed.store(true, std::memory_order_relaxed);
                        continue;
                    }
                    stripData = localBuf.data();
                }

                if (!stripData) {
                    decodeFailed.store(true, std::memory_order_relaxed);
                    continue;
                }

                // Undo predictor
                if (predictor == 2) {
                    if (desc.compression == 1) {
                        predictorBuf.assign(stripData, stripData + stripBytes);
                        stripData = predictorBuf.data();
                    }
                    uint8_t* mutableData = const_cast<uint8_t*>(stripData);
                    for (int y = 0; y < unitH; ++y) {
                        uint8_t* rowData = mutableData + static_cast<size_t>(y) * rowBytes;
                        UndoHorizontalPredictor(rowData, desc.width, samples, bytesPerSample, desc.isLE);
                    }
                }

                // Pack pixels falling into ROI horizontally
                for (int y = intersectY; y < intersectEndY; ++y) {
                    size_t localY = static_cast<size_t>(y - startY);
                    size_t outY = static_cast<size_t>(y - cropY);
                    const uint8_t* srcRow = stripData + localY * rowBytes;
                    uint8_t* dstRow = pixels + outY * static_cast<size_t>(outStride);

                    if (photometric == 0 || photometric == 1) {
                        for (int x = cropX; x < cropX + cropW; ++x) {
                            size_t ux = static_cast<size_t>(x);
                            size_t outX = static_cast<size_t>(x - cropX);
                            uint8_t val = srcRow[ux * pixelStride + highByteOffset];
                            if (photometric == 0) val = 255 - val;
                            dstRow[outX * 4 + 0] = val;
                            dstRow[outX * 4 + 1] = val;
                            dstRow[outX * 4 + 2] = val;
                            dstRow[outX * 4 + 3] = (samples > 1) ? srcRow[ux * pixelStride + bytesPerSample + highByteOffset] : 255;
                        }
                    } else if (photometric == 2) {
                        // [Titan Perf] SIMD Accelerated 16-to-8 bit downsampling + BGRA Swizzle for Stripped Layout
                        if (needRgb8SimdBuf) {
                            const uint16_t* src16 = reinterpret_cast<const uint16_t*>(srcRow + static_cast<size_t>(cropX) * pixelStride);
                            ImageLoaderSimd::Pack16to8(src16, rgb8Buf.data(), cropW * 3);
                            ImageLoaderSimd::ConvertRGBToBGRA(rgb8Buf.data(), dstRow, cropW, 1, cropW * 4);
                        } else {
                            for (int x = cropX; x < cropX + cropW; ++x) {
                                size_t ux = static_cast<size_t>(x);
                                size_t outX = static_cast<size_t>(x - cropX);
                                uint8_t r = srcRow[ux * pixelStride + 0 * bytesPerSample + highByteOffset];
                                uint8_t g = srcRow[ux * pixelStride + 1 * bytesPerSample + highByteOffset];
                                uint8_t b = srcRow[ux * pixelStride + 2 * bytesPerSample + highByteOffset];
                                uint8_t a = (samples >= 4) ? srcRow[ux * pixelStride + 3 * bytesPerSample + highByteOffset] : 255;
                                if (samples >= 4 && desc.extraSamples != 1) {
                                    r = static_cast<uint8_t>((r * a + 127) / 255);
                                    g = static_cast<uint8_t>((g * a + 127) / 255);
                                    b = static_cast<uint8_t>((b * a + 127) / 255);
                                }
                                dstRow[outX * 4 + 0] = b;
                                dstRow[outX * 4 + 1] = g;
                                dstRow[outX * 4 + 2] = r;
                                dstRow[outX * 4 + 3] = a;
                            }
                        }
                    } else if (photometric == 3) {
                        if (desc.colorMap.size() >= 768) {
                            for (int x = cropX; x < cropX + cropW; ++x) {
                                size_t ux = static_cast<size_t>(x);
                                size_t outX = static_cast<size_t>(x - cropX);
                                uint8_t idx = srcRow[ux * pixelStride + highByteOffset];
                                uint8_t r = static_cast<uint8_t>(desc.colorMap[idx] / 256);
                                uint8_t g = static_cast<uint8_t>(desc.colorMap[idx + 256] / 256);
                                uint8_t b = static_cast<uint8_t>(desc.colorMap[idx + 512] / 256);
                                dstRow[outX * 4 + 0] = b;
                                dstRow[outX * 4 + 1] = g;
                                dstRow[outX * 4 + 2] = r;
                                dstRow[outX * 4 + 3] = 255;
                            }
                        } else {
                            decodeFailed.store(true, std::memory_order_relaxed);
                        }
                    } else if (photometric == 5) {
                        if (bytesPerSample == 1) {
                            ConvertCmykToBgra(srcRow + static_cast<size_t>(cropX) * pixelStride, dstRow, cropW, samples);
                        } else {
                            for (int dx = 0; dx < cropW; ++dx) {
                                size_t srcBase = static_cast<size_t>(cropX + dx) * pixelStride;
                                uint8_t c = srcRow[srcBase + 0 * bytesPerSample + highByteOffset];
                                uint8_t m = srcRow[srcBase + 1 * bytesPerSample + highByteOffset];
                                uint8_t ye = srcRow[srcBase + 2 * bytesPerSample + highByteOffset];
                                uint8_t k = (samples >= 4) ? srcRow[srcBase + 3 * bytesPerSample + highByteOffset] : 0;
                                uint8_t r = (255 - c) * (255 - k) / 255;
                                uint8_t g = (255 - m) * (255 - k) / 255;
                                uint8_t b = (255 - ye) * (255 - k) / 255;
                                dstRow[static_cast<size_t>(dx) * 4 + 0] = b;
                                dstRow[static_cast<size_t>(dx) * 4 + 1] = g;
                                dstRow[static_cast<size_t>(dx) * 4 + 2] = r;
                                dstRow[static_cast<size_t>(dx) * 4 + 3] = 255;
                            }
                        }
                    }
                }
            }
        }
    }

    if (decodeFailed.load(std::memory_order_relaxed)) {
        return E_FAIL;
    }

    // Setup DecodeResult
    result.pixels = pixels;
    result.width = cropW;
    result.height = cropH;
    result.stride = outStride;
    result.format = PixelFormat::BGRA8888;
    result.success = true;

    // Setup Metadata
    result.metadata.Width = desc.width;
    result.metadata.Height = desc.height;
    result.metadata.Format = L"TIFF";
    
    wchar_t details[128];
    const wchar_t* compStr = L"";
    if (desc.compression == 1) compStr = L"Uncompressed";
    else if (desc.compression == 32773) compStr = L"PackBits";
    else if (desc.compression == 5) compStr = L"LZW";
    else if (desc.compression == 8 || desc.compression == 32946) compStr = L"Deflate";

    const wchar_t* photoStr = L"";
    if (photometric == 0 || photometric == 1) photoStr = L"Grayscale";
    else if (photometric == 2) photoStr = L"RGB";
    else if (photometric == 3) photoStr = L"Palette";
    else if (photometric == 5) photoStr = L"CMYK";

    swprintf_s(details, L"8-bit %s %s Lossless", photoStr, compStr);
    result.metadata.FormatDetails = details;
    result.metadata.ExifOrientation = desc.orientation;
    result.metadata.LoaderName = L"MiniTIFF";

    if (!desc.iccProfile.empty() && desc.photometric != 5) {
        result.metadata.iccProfileData.assign(desc.iccProfile.begin(), desc.iccProfile.end());
        result.metadata.HasEmbeddedColorProfile = true;
        result.metadata.colorInfo.hasEmbeddedIcc = true;
        result.metadata.colorInfo.nominalBitDepth = 8;

        QuickView::ColorPrimaries cicpPrimaries = QuickView::ColorPrimaries::Unknown;
        QuickView::TransferFunction cicpTransfer = QuickView::TransferFunction::Unknown;
        if (QuickView::TryParseIccCicp(desc.iccProfile.data(), desc.iccProfile.size(),
                                        &cicpPrimaries, &cicpTransfer)) {
            if (cicpPrimaries != QuickView::ColorPrimaries::Unknown) {
                result.metadata.colorInfo.primaries = cicpPrimaries;
                result.metadata.hdrMetadata.primaries = cicpPrimaries;
            }
            if (cicpTransfer == QuickView::TransferFunction::PQ ||
                cicpTransfer == QuickView::TransferFunction::HLG) {
                result.metadata.colorInfo.dataSpace = QuickView::PixelDataSpace::EncodedHdr;
                result.metadata.colorInfo.transfer = cicpTransfer;
                result.metadata.hdrMetadata.isValid = true;
                result.metadata.hdrMetadata.isHdr = true;
                result.metadata.hdrMetadata.transfer = cicpTransfer;
                result.metadata.hdrMetadata.isSceneLinear = false;
            } else {
                result.metadata.colorInfo.dataSpace = QuickView::PixelDataSpace::EncodedSdr;
                result.metadata.colorInfo.transfer =
                    (cicpTransfer != QuickView::TransferFunction::Unknown)
                        ? cicpTransfer
                        : QuickView::TransferFunction::SRGB;
            }
        } else {
            result.metadata.colorInfo.dataSpace = QuickView::PixelDataSpace::EncodedSdr;
            result.metadata.colorInfo.transfer = QuickView::TransferFunction::SRGB;
        }
    } else {
        result.metadata.HasEmbeddedColorProfile = false;
        result.metadata.colorInfo.dataSpace = QuickView::PixelDataSpace::EncodedSdr;
        result.metadata.colorInfo.transfer = QuickView::TransferFunction::SRGB;
        result.metadata.colorInfo.nominalBitDepth = 8;
    }

    return S_OK;
}

HRESULT Load(const uint8_t* data, size_t size,
             const QuickView::Codec::DecodeContext& ctx,
             QuickView::Codec::DecodeResult& result) {
    TiffImageDesc desc;
    Status status = ParseTiffIFD(data, size, desc);
    if (status != Status::Ok) {
        return (status == Status::NotTiff) ? E_NOTIMPL : E_FAIL;
    }

    status = CapabilityGate(desc);
    if (status != Status::Ok) {
        return (status == Status::Unsupported) ? E_NOTIMPL : E_FAIL;
    }

    return LoadRegion(data, size, ctx, result, 0, 0, desc.width, desc.height);
}

} // namespace QuickView::MiniTiff
