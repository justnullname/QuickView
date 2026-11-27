#include "pch.h"
#include "PreviewExtractor.h"
#include <algorithm>
#include <cstring>

// Helper Macros
#define U16LE(p) (*(const uint16_t*)(p))
#define U32LE(p) (*(const uint32_t*)(p))
#define U16BE(p) (uint16_t)((p)[0]<<8 | (p)[1])
#define U32BE(p) (uint32_t)((p)[0]<<24 | (p)[1]<<16 | (p)[2]<<8 | (p)[3])

// --- RAW (TIFF-based) ---

namespace {

[[nodiscard]] bool IsJpegSoiAt(const uint8_t* data, size_t size, uint64_t off, uint64_t len) noexcept {
    if (len < 128 || off >= size || len > size - off) return false;
    return data[off] == 0xFF && data[off + 1] == 0xD8 && data[off + 2] == 0xFF;
}

[[nodiscard]] bool ReadJpegSofDims(const uint8_t* data, size_t size, int& outW, int& outH) noexcept {
    outW = 0;
    outH = 0;
    if (!data || size < 4 || data[0] != 0xFF || data[1] != 0xD8) return false;
    size_t pos = 2;
    while (pos + 4 <= size) {
        while (pos < size && data[pos] != 0xFF) ++pos;
        while (pos < size && data[pos] == 0xFF) ++pos;
        if (pos >= size) break;
        const uint8_t marker = data[pos++];
        if (marker == 0x00 || (marker >= 0xD0 && marker <= 0xD8)) continue;
        if (marker == 0xD9 || marker == 0xDA) break;
        if (pos + 2 > size) break;
        const uint16_t segLen = static_cast<uint16_t>((data[pos] << 8) | data[pos + 1]);
        if (segLen < 2 || pos + segLen > size) break;
        const bool isSof =
            (marker >= 0xC0 && marker <= 0xC3) ||
            (marker >= 0xC5 && marker <= 0xC7) ||
            (marker >= 0xC9 && marker <= 0xCB) ||
            (marker >= 0xCD && marker <= 0xCF);
        if (isSof && segLen >= 7 && pos + 7 <= size) {
            outH = (data[pos + 3] << 8) | data[pos + 4];
            outW = (data[pos + 5] << 8) | data[pos + 6];
            return (outW > 0 && outH > 0);
        }
        pos += segLen;
    }
    return false;
}

} // namespace

bool PreviewExtractor::ExtractFromRAW(const uint8_t* data, size_t size, ExtractedData& out) {
    if (size < 1024) return false;

    bool isLE = false;
    if (data[0] == 0x49 && data[1] == 0x49) isLE = true;
    else if (data[0] == 0x4D && data[1] == 0x4D) isLE = false;
    else return false;

    uint32_t firstIFD = isLE ? U32LE(data + 4) : U32BE(data + 4);
    if (firstIFD == 0 || firstIFD >= size) return false;

    uint64_t jpgOff = 0, jpgSz = 0;
    if (ParseTiffIFD(data, size, firstIFD, isLE, jpgOff, jpgSz)) {
        if (IsJpegSoiAt(data, size, jpgOff, jpgSz)) {
            out.pData = data + jpgOff;
            out.size = static_cast<size_t>(jpgSz);
            return true;
        }
    }
    return false;
}

bool PreviewExtractor::ExtractFromTIFF(const uint8_t* data, size_t size, ExtractedData& out) {
    if (size < 16) return false;

    bool isLE = false;
    if (data[0] == 0x49 && data[1] == 0x49) isLE = true;
    else if (data[0] == 0x4D && data[1] == 0x4D) isLE = false;
    else return false;

    // Only Classic TIFF (magic == 42) uses 32-bit IFD offsets and 12-byte entries.
    uint16_t magic = isLE ? U16LE(data + 2) : U16BE(data + 2);
    if (magic != 42) return false;

    uint32_t ifd0 = isLE ? U32LE(data + 4) : U32BE(data + 4);
    if (ifd0 == 0 || ifd0 >= size) return false;

    uint64_t jpgOff = 0;
    uint64_t jpgSz = 0;
    if (!ParseTiffIFD(data, size, ifd0, isLE, jpgOff, jpgSz)) {
        return false;
    }

    if (IsJpegSoiAt(data, size, jpgOff, jpgSz) && jpgSz > 512) {
        out.pData = data + jpgOff;
        out.size = static_cast<size_t>(jpgSz);
        return true;
    }

    return false;
}

namespace {

struct TiffIfdVisitBudget {
    size_t visited[64]{};
    int count = 0;
    int remainingNodes = 64;

    bool TryVisit(size_t off) noexcept {
        if (remainingNodes <= 0) return false;
        for (int i = 0; i < count; ++i) {
            if (visited[i] == off) return false;
        }
        --remainingNodes;
        if (count < 64) {
            visited[count++] = off;
        }
        return true;
    }
};

bool ParseTiffIFDImpl(const uint8_t* data, size_t size, size_t offset, bool isLE,
                      uint64_t& jpegOffset, uint64_t& jpegSize, int depth,
                      TiffIfdVisitBudget& budget, uint16_t* outOrientation) {
    jpegOffset = 0;
    jpegSize = 0;
    if (depth > 4) return false;

    uint64_t bestOff = 0;
    uint64_t bestSz = 0;
    auto considerCandidate = [&](uint64_t candOff, uint64_t candSz) {
        if (!IsJpegSoiAt(data, size, candOff, candSz)) return;
        if (candSz > bestSz) {
            bestOff = candOff;
            bestSz = candSz;
        }
    };

    size_t curIfd = offset;
    for (int ifdStep = 0; ifdStep < 8 && curIfd > 0 && curIfd + 2 <= size; ++ifdStep) {
        if (!budget.TryVisit(curIfd)) break;
        uint16_t count = isLE ? U16LE(data + curIfd) : U16BE(data + curIfd);
        size_t entrySize = 12;
        size_t current = curIfd + 2;
        if (count > 512 || current + static_cast<size_t>(count) * entrySize > size) break;

        uint64_t jifOff = 0, jifSz = 0;
        uint64_t stripOff = 0, stripSz = 0;
        uint32_t stripOffCnt = 0, stripSzCnt = 0;
        uint64_t subIfdVal = 0;
        uint32_t subIfdCnt = 0;
        uint16_t subIfdType = 0;

        for (int i = 0; i < count; ++i) {
            const uint8_t* ev = data + current + static_cast<size_t>(i) * entrySize;
            uint16_t tag = isLE ? U16LE(ev) : U16BE(ev);
            uint16_t type = isLE ? U16LE(ev + 2) : U16BE(ev + 2);
            uint32_t cnt = isLE ? U32LE(ev + 4) : U32BE(ev + 4);
            uint32_t valOrOff = (type == 3 && cnt == 1)
                ? static_cast<uint32_t>(isLE ? U16LE(ev + 8) : U16BE(ev + 8))
                : (isLE ? U32LE(ev + 8) : U32BE(ev + 8));

            if (tag == 0x0112 && outOrientation && *outOrientation == 0 && cnt == 1 &&
                valOrOff >= 1 && valOrOff <= 8) {
                *outOrientation = static_cast<uint16_t>(valOrOff);
            } else if (tag == 0x0201) { // JPEGInterchangeFormat
                jifOff = valOrOff;
            } else if (tag == 0x0202) { // JPEGInterchangeFormatLength
                jifSz = valOrOff;
            } else if (tag == 0x0111) { // StripOffsets (CR2/embedded JPEG preview)
                stripOff = valOrOff;
                stripOffCnt = cnt;
            } else if (tag == 0x0117) { // StripByteCounts
                stripSz = valOrOff;
                stripSzCnt = cnt;
            } else if (tag == 0x014A && (type == 4 || type == 3 || type == 13)) { // SubIFDs
                subIfdVal = valOrOff;
                subIfdCnt = cnt;
                subIfdType = type;
            }
        }

        considerCandidate(jifOff, jifSz);
        if (stripOffCnt == 1 && stripSzCnt == 1) {
            considerCandidate(stripOff, stripSz);
        }

        if (subIfdCnt > 0 && subIfdVal > 0 && subIfdVal < size) {
            if (subIfdCnt == 1) {
                uint64_t sOff = 0, sSz = 0;
                if (ParseTiffIFDImpl(data, size, static_cast<size_t>(subIfdVal), isLE, sOff, sSz, depth + 1, budget, nullptr)) {
                    considerCandidate(sOff, sSz);
                }
            } else {
                const size_t elemBytes = (subIfdType == 3) ? 2u : 4u;
                const uint32_t capped = (std::min)(subIfdCnt, 16u);
                if (subIfdVal <= size && static_cast<size_t>(capped) * elemBytes <= size - subIfdVal) {
                    for (uint32_t s = 0; s < capped; ++s) {
                        const uint8_t* ptr = data + subIfdVal + static_cast<size_t>(s) * elemBytes;
                        const uint32_t subOff = (elemBytes == 2u)
                            ? static_cast<uint32_t>(isLE ? U16LE(ptr) : U16BE(ptr))
                            : (isLE ? U32LE(ptr) : U32BE(ptr));
                        if (subOff > 0 && subOff < size) {
                            uint64_t sOff = 0, sSz = 0;
                            if (ParseTiffIFDImpl(data, size, subOff, isLE, sOff, sSz, depth + 1, budget, nullptr)) {
                                considerCandidate(sOff, sSz);
                            }
                        }
                    }
                }
            }
        }

        const size_t nextPtrPos = current + static_cast<size_t>(count) * entrySize;
        if (nextPtrPos + 4 > size) break;
        const uint32_t nextIfd = isLE ? U32LE(data + nextPtrPos) : U32BE(data + nextPtrPos);
        if (nextIfd == 0 || nextIfd == curIfd) break;
        curIfd = nextIfd;
    }

    if (bestOff > 0 && bestSz > 0) {
        jpegOffset = bestOff;
        jpegSize = bestSz;
        return true;
    }
    return false;
}

} // namespace

bool PreviewExtractor::ParseTiffIFD(const uint8_t* data, size_t size, size_t offset, bool isLE, uint64_t& jpegOffset, uint64_t& jpegSize, int depth) {
    TiffIfdVisitBudget budget;
    return ParseTiffIFDImpl(data, size, offset, isLE, jpegOffset, jpegSize, depth, budget, nullptr);
}


// --- HEIC (ISOBMFF) ---
uint32_t PreviewExtractor::ReadU32BE(const uint8_t* p) { return U32BE(p); }

bool PreviewExtractor::ExtractFromHEIC(const uint8_t* data, size_t size, ExtractedData& out) {
    // Simplified ISOBMFF Parser
    // Look for 'meta' box -> 'iloc' box -> item info
    // HEIC usually stores thumb as a separate item, or main image (grid)
    // Finding extracted JPEG (Exif thumb) inside 'meta' -> 'Exif' item?
    // Often HEIC contains a small AVC/HEVC thumbnail track.
    // BUT, we want JPEG.
    // Many camera HEIC embed Exif. Exif has JPEG thumb.
    
    // Quick Scan for Exif Marker?
    // Exif in ISOBMFF is wrapped in 'Exif' item.
    // Scanning for "Exif\0\0" (Standard Exif header) might work.
    
    if (size < 1024) return false;
    
    const uint8_t exifSig[] = { 0x45, 0x78, 0x69, 0x66, 0x00, 0x00 };
    // Brute force scan for Exif header (fast enough for memory mapped file)
    // Limit scan to first 1MB?
    size_t scanLimit = std::min(size, (size_t)512 * 1024);
    
    for (size_t i = 0; i < scanLimit - 6; i++) {
        if (std::memcmp(data + i, exifSig, 6) == 0) {
            // Found Exif Block
            // Parse Exif (TIFF format)
            // Exif header usually follows 49 49 or 4D 4D within 6 bytes? 
            // Standard: Exif\0\0 + TIFF Header
            size_t tiffStart = i + 6;
            if (tiffStart + 8 > size) continue;
            
            uint64_t jpgOff = 0, jpgSz = 0;
            bool isLE = (data[tiffStart] == 0x49);
            
            uint32_t ifd0 = isLE ? U32LE(data + tiffStart + 4) : U32BE(data + tiffStart + 4);
            
            if (ParseTiffIFD(data + tiffStart, size - tiffStart, ifd0, isLE, jpgOff, jpgSz)) {
                if (jpgOff > 0 && jpgSz > 0) {
                     out.pData = data + tiffStart + jpgOff;
                     out.size = (size_t)jpgSz;
                     return true;
                }
            }
        }
    }
    return false;
}

// --- PSD ---

bool PreviewExtractor::ExtractFromPSD(const uint8_t* data, size_t size, ExtractedData& out) {
    if (size < 26) return false;
    if (std::memcmp(data, "8BPS", 4) != 0) return false;
    
    // Header (26 bytes)
    // Channels (2 bytes), Height (4), Width (4), Depth (2), Mode (2)
    
    // Color Mode Data Section
    size_t offset = 26;
    uint32_t colorModeLen = U32BE(data + offset);
    offset += 4 + colorModeLen;
    if (offset >= size) return false;
    
    // Image Resources Section
    if (offset + 4 > size) return false;
    uint32_t imgResLen = U32BE(data + offset);
    size_t resEnd = offset + 4 + imgResLen;
    offset += 4;
    
    if (resEnd > size) return false;
    
    // Parse Resources
    while (offset < resEnd) {
        if (offset + 6 > resEnd) break;
        // Signature 8BIM
        if (std::memcmp(data + offset, "8BIM", 4) != 0) break;
        offset += 4;
        
        uint16_t id = U16BE(data + offset);
        offset += 2;
        
        // Name (Pascal String, padded to even)
        uint8_t nameLen = data[offset];
        offset += 1 + nameLen;
        if (offset % 2 != 0) offset++; // Pad
        
        if (offset + 4 > resEnd) break;
        uint32_t sizeData = U32BE(data + offset);
        offset += 4;
        
        if (offset + sizeData > resEnd) break;
        
        // ID 1033 (Thumbnail resource 1) or 1036 (Thumbnail resource 2 - preferred)
        if (id == 1036 || id == 1033) {
            // Found thumbnail resource!
            // Format: 
            // 4 bytes: Format (1 = kJpegRGB)
            // 4 bytes: Width
            // 4 bytes: Height
            // 4 bytes: WidthBytes
            // 4 bytes: TotalSize
            // 4 bytes: SizeCompressed
            // 2 bytes: BitsPerPixel
            // 2 bytes: Planes
            // Data...
            
            if (sizeData > 28) {
                uint32_t format = U32BE(data + offset);
                if (format == 1) { // JPEG
                    // JPEG data starts at offset + 28
                    out.pData = data + offset + 28;
                    out.size = sizeData - 28;
                    return true;
                }
            }
        }
        
        offset += sizeData;
        if (offset % 2 != 0) offset++; // Pad
    }
    
    return false;
}

// --- JPEG (EXIF Thumbnail) ---

bool PreviewExtractor::ExtractFromJPEG(const uint8_t* data, size_t size, ExtractedData& out) {
    // JPEG files have APP1 marker (0xFF 0xE1) containing EXIF
    // EXIF contains a TIFF structure with embedded JPEG thumbnail
    
    if (size < 20) return false;
    
    // Check JPEG signature
    if (data[0] != 0xFF || data[1] != 0xD8) return false;
    
    // Scan for APP1 marker
    size_t pos = 2;
    while (pos < size - 10) {
        if (data[pos] != 0xFF) {
            pos++;
            continue;
        }
        
        uint8_t marker = data[pos + 1];
        
        // Skip to next if padding
        if (marker == 0xFF) {
            pos++;
            continue;
        }
        
        // End markers
        if (marker == 0xD9 || marker == 0xDA) break;
        if (pos + 4 > size) break;

        // Get segment length (includes the 2 length bytes themselves)
        const uint16_t segLen = static_cast<uint16_t>((data[pos + 2] << 8) | data[pos + 3]);
        if (segLen < 2 || pos + 2 + static_cast<size_t>(segLen) > size) break;

        // APP1 marker (0xE1) - EXIF data
        if (marker == 0xE1) {
            // Require >= 16 bytes: 2B length + 6B "Exif\0\0" + 8B TIFF header
            if (segLen >= 16 &&
                data[pos + 4] == 'E' && data[pos + 5] == 'x' &&
                data[pos + 6] == 'i' && data[pos + 7] == 'f' &&
                data[pos + 8] == 0 && data[pos + 9] == 0) {

                // TIFF header starts at pos + 10
                const uint8_t* tiffStart = data + pos + 10;
                const size_t tiffSize = static_cast<size_t>(segLen) - 8u;

                // Check TIFF byte order & classic magic (42)
                const bool isLE = (tiffStart[0] == 'I' && tiffStart[1] == 'I');
                const bool isBE = (tiffStart[0] == 'M' && tiffStart[1] == 'M');
                if (!isLE && !isBE) {
                    pos += 2 + static_cast<size_t>(segLen);
                    continue;
                }
                const uint16_t magic = isLE ? U16LE(tiffStart + 2) : U16BE(tiffStart + 2);
                if (magic != 42) {
                    pos += 2 + static_cast<size_t>(segLen);
                    continue;
                }

                // Get IFD0 offset
                const uint32_t ifd0Offset = isLE ? U32LE(tiffStart + 4) : U32BE(tiffStart + 4);

                // Parse TIFF IFD chain (IFD0 -> IFD1 -> ...) for thumbnail
                uint64_t jpgOff = 0, jpgSz = 0;
                if (ParseTiffIFD(tiffStart, tiffSize, ifd0Offset, isLE, jpgOff, jpgSz)) {
                    if (jpgOff > 0 && jpgSz > 100 && jpgOff <= tiffSize && jpgSz <= tiffSize - jpgOff) {
                        out.pData = tiffStart + jpgOff;
                        out.size = static_cast<size_t>(jpgSz);
                        return true;
                    }
                }
            }
        }

        pos += 2 + static_cast<size_t>(segLen);
    }

    return false;
}

bool PreviewExtractor::ExtractFromPSDFile(const wchar_t* filePath, std::vector<uint8_t>& outJpeg, bool* outIsBgr) {
    outJpeg.clear();
    if (outIsBgr) *outIsBgr = false;
    if (!filePath || !filePath[0]) return false;

    HANDLE hFile = CreateFileW(filePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    auto readExact = [&](void* dst, DWORD bytes) -> bool {
        uint8_t* p = static_cast<uint8_t*>(dst);
        DWORD total = 0;
        while (total < bytes) {
            DWORD chunk = 0;
            if (!ReadFile(hFile, p + total, bytes - total, &chunk, nullptr) || chunk == 0) {
                return false;
            }
            total += chunk;
        }
        return true;
    };

    uint8_t hdr[30]{};
    if (!readExact(hdr, 30) || std::memcmp(hdr, "8BPS", 4) != 0) {
        CloseHandle(hFile);
        return false;
    }

    const uint32_t colorModeLen = U32BE(hdr + 26);
    if (colorModeLen > 64u * 1024u * 1024u) {
        CloseHandle(hFile);
        return false;
    }

    LARGE_INTEGER seekPos{};
    seekPos.QuadPart = 30ll + static_cast<LONGLONG>(colorModeLen);
    if (!SetFilePointerEx(hFile, seekPos, nullptr, FILE_BEGIN)) {
        CloseHandle(hFile);
        return false;
    }

    uint8_t resLenBuf[4]{};
    if (!readExact(resLenBuf, 4)) {
        CloseHandle(hFile);
        return false;
    }
    const uint32_t imgResLen = U32BE(resLenBuf);
    constexpr uint32_t kMaxImgResRead = 8u * 1024u * 1024u;
    if (imgResLen == 0 || imgResLen > kMaxImgResRead) {
        CloseHandle(hFile);
        return false;
    }

    std::vector<uint8_t> resBlock(imgResLen);
    if (!readExact(resBlock.data(), imgResLen)) {
        CloseHandle(hFile);
        return false;
    }
    CloseHandle(hFile);

    std::vector<uint8_t> fallback1033;
    size_t off = 0;
    while (off + 8 <= resBlock.size()) {
        const bool isValidSig =
            std::memcmp(resBlock.data() + off, "8BIM", 4) == 0 ||
            std::memcmp(resBlock.data() + off, "MeSa", 4) == 0 ||
            std::memcmp(resBlock.data() + off, "8B64", 4) == 0 ||
            std::memcmp(resBlock.data() + off, "PHUT", 4) == 0;
        if (!isValidSig) break;
        const bool is8Bim = (resBlock[off] == '8' && resBlock[off + 1] == 'B' &&
                             resBlock[off + 2] == 'I' && resBlock[off + 3] == 'M');
        off += 4;
        const uint16_t id = U16BE(resBlock.data() + off);
        off += 2;
        if (off >= resBlock.size()) break;
        const uint8_t nameLen = resBlock[off];
        off += 1 + nameLen;
        if ((off & 1u) != 0) ++off;
        if (off + 4 > resBlock.size()) break;
        const uint32_t dataSz = U32BE(resBlock.data() + off);
        off += 4;
        if (dataSz > resBlock.size() - off) break;

        if (is8Bim && (id == 1036 || id == 1033) && dataSz > 28) {
            const uint32_t fmt = U32BE(resBlock.data() + off);
            if (fmt == 1 && IsJpegSoiAt(resBlock.data(), resBlock.size(), off + 28, dataSz - 28)) {
                if (id == 1036) {
                    outJpeg.assign(resBlock.data() + off + 28, resBlock.data() + off + dataSz);
                    if (outIsBgr) *outIsBgr = false;
                    return true;
                }
                if (fallback1033.empty()) {
                    fallback1033.assign(resBlock.data() + off + 28, resBlock.data() + off + dataSz);
                }
            }
        }
        off += dataSz;
        if ((off & 1u) != 0) ++off;
    }
    if (!fallback1033.empty()) {
        outJpeg = std::move(fallback1033);
        if (outIsBgr) *outIsBgr = true;
        return true;
    }
    return false;
}

bool PreviewExtractor::ExtractFromJPEGHeaderValidated(const wchar_t* filePath, int targetSize,
                                                      std::vector<uint8_t>& outJpeg,
                                                      int& outMainW, int& outMainH,
                                                      int* outMainOrientation) {
    outJpeg.clear();
    outMainW = 0;
    outMainH = 0;
    if (outMainOrientation) *outMainOrientation = 1;
    if (!filePath || !filePath[0]) return false;

    HANDLE hFile = CreateFileW(filePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    constexpr DWORD kProbeBytes = 256u * 1024u;
    std::vector<uint8_t> buf(kProbeBytes);
    DWORD readBytes = 0;
    const BOOL ok = ReadFile(hFile, buf.data(), kProbeBytes, &readBytes, nullptr);
    CloseHandle(hFile);
    if (!ok || readBytes < 64) return false;
    buf.resize(readBytes);

    if (!ReadJpegSofDims(buf.data(), buf.size(), outMainW, outMainH)) return false;

    // Extract main EXIF Orientation (Tag 0x0112) from APP1 IFD0 if requested
    if (outMainOrientation && buf.size() >= 20 && buf[0] == 0xFF && buf[1] == 0xD8) {
        size_t pos = 2;
        while (pos + 4 <= buf.size()) {
            if (buf[pos] != 0xFF) { ++pos; continue; }
            const uint8_t marker = buf[pos + 1];
            if (marker == 0xFF) { ++pos; continue; }
            if (marker == 0xD9 || marker == 0xDA) break;
            const uint16_t segLen = static_cast<uint16_t>((buf[pos + 2] << 8) | buf[pos + 3]);
            if (segLen < 2 || pos + 2 + static_cast<size_t>(segLen) > buf.size()) break;
            if (marker == 0xE1 && segLen >= 16 &&
                buf[pos + 4] == 'E' && buf[pos + 5] == 'x' && buf[pos + 6] == 'i' &&
                buf[pos + 7] == 'f' && buf[pos + 8] == 0 && buf[pos + 9] == 0) {
                const uint8_t* tiffStart = buf.data() + pos + 10;
                const size_t tiffSize = static_cast<size_t>(segLen) - 8u;
                const bool isLE = (tiffStart[0] == 'I' && tiffStart[1] == 'I');
                const bool isBE = (tiffStart[0] == 'M' && tiffStart[1] == 'M');
                if (isLE || isBE) {
                    const uint16_t magic = isLE ? U16LE(tiffStart + 2) : U16BE(tiffStart + 2);
                    const uint32_t ifd0 = isLE ? U32LE(tiffStart + 4) : U32BE(tiffStart + 4);
                    if (magic == 42 && ifd0 < tiffSize) {
                        uint64_t dummyOff = 0, dummySz = 0;
                        uint16_t ori = 0;
                        TiffIfdVisitBudget budget;
                        ParseTiffIFDImpl(tiffStart, tiffSize, ifd0, isLE, dummyOff, dummySz, 0, budget, &ori);
                        if (ori >= 1 && ori <= 8) {
                            *outMainOrientation = static_cast<int>(ori);
                            break;
                        }
                    }
                }
            }
            pos += 2 + static_cast<size_t>(segLen);
        }
    }

    ExtractedData ex;
    if (!ExtractFromJPEG(buf.data(), buf.size(), ex) || !ex.IsValid()) return false;

    int thumbW = 0, thumbH = 0;
    if (!ReadJpegSofDims(ex.pData, ex.size, thumbW, thumbH)) return false;

    const int reqMax = (std::max)(targetSize, 16);
    if ((std::max)(thumbW, thumbH) < reqMax) return false;

    // Anti-black-bar / anti-stale-preview aspect check (< 5% relative error)
    const double lhs = static_cast<double>(thumbW) * static_cast<double>(outMainH);
    const double rhs = static_cast<double>(thumbH) * static_cast<double>(outMainW);
    const double diff = (lhs >= rhs) ? (lhs - rhs) : (rhs - lhs);
    if (rhs <= 0.0 || diff > 0.05 * rhs) return false;

    outJpeg.assign(ex.pData, ex.pData + ex.size);
    return true;
}
