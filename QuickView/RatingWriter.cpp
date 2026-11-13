/*
 * QuickView Star Ratings - writing a rating into an image file
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

#include "RatingWriter.h"

#include "pch.h"

#include <wincodecsdk.h>
#include "RatingMetadata.h"
#include "SupportedExtensions.h"

#include <wrl/client.h>
#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <span>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace QuickView::Rating {

namespace {

// A JPEG nests Exif under APP1; a TIFF carries the IFD directly. Both take
// xmp:Rating, which is the property Lightroom and Bridge read. PNG supports
// iTXt XMP or eXIf chunk.
struct RatingQueries {
    const wchar_t* exif;
    const wchar_t* xmp;
    const wchar_t* xmpFallback;
};

RatingQueries QueriesFor(const std::wstring& path) {
    const std::wstring_view ext = QuickView::ExtensionOf(path);
    const bool isTiff = QuickView::ExtEqualsIgnoreCase(ext, L".tif") ||
                        QuickView::ExtEqualsIgnoreCase(ext, L".tiff");
    const bool isPng = QuickView::ExtEqualsIgnoreCase(ext, L".png") ||
                       QuickView::ExtEqualsIgnoreCase(ext, L".apng");
    if (isTiff) {
        return RatingQueries{ L"/ifd/{ushort=18246}", L"/ifd/xmp/xmp:Rating", nullptr };
    }
    if (isPng) {
        // WIC PNG encoder surfaces XMP at the root /xmp namespace.
        // /xmp/xmp:Rating embeds a standard iTXt chunk carrying <xmp:Rating>,
        // natively recognized by Windows Explorer (System.Rating) and Lightroom.
        return RatingQueries{ nullptr, L"/xmp/xmp:Rating", nullptr };
    }
    return RatingQueries{ L"/app1/ifd/{ushort=18246}", L"/xmp/xmp:Rating", nullptr };
}

// Writing SimpleRating (0..5) is enough for both ecosystems: Explorer and
// Photos derive System.Rating (0..99) from it, and Adobe reads xmp:Rating.
HRESULT ApplyRating(IWICMetadataQueryWriter* writer, const RatingQueries& q, int stars) {
    if (stars <= 0) {
        // Clearing means removing the property, not storing a zero, so the
        // file goes back to carrying no rating at all. A property that was
        // not there is not an error.
        if (q.exif) writer->RemoveMetadataByName(q.exif);
        if (q.xmp) writer->RemoveMetadataByName(q.xmp);
        if (q.xmpFallback) writer->RemoveMetadataByName(q.xmpFallback);
        return S_OK;
    }

    HRESULT hrExif = E_FAIL;
    if (q.exif) {
        PROPVARIANT var;
        PropVariantInit(&var);
        var.vt = VT_UI2;
        var.uiVal = (USHORT)stars;
        hrExif = writer->SetMetadataByName(q.exif, &var);
        PropVariantClear(&var);
    }

    HRESULT hrXmp = E_FAIL;
    const std::wstring text = std::to_wstring(stars);
    if (q.xmp) {
        PROPVARIANT var;
        PropVariantInit(&var);
        var.vt = VT_LPWSTR;
        var.pwszVal = (WCHAR*)CoTaskMemAlloc((text.size() + 1) * sizeof(WCHAR));
        if (var.pwszVal) {
            wcscpy_s(var.pwszVal, text.size() + 1, text.c_str());
            hrXmp = writer->SetMetadataByName(q.xmp, &var);
            PropVariantClear(&var);
        }
    }

    if (FAILED(hrXmp) && q.xmpFallback) {
        PROPVARIANT var;
        PropVariantInit(&var);
        var.vt = VT_LPWSTR;
        var.pwszVal = (WCHAR*)CoTaskMemAlloc((text.size() + 1) * sizeof(WCHAR));
        if (var.pwszVal) {
            wcscpy_s(var.pwszVal, text.size() + 1, text.c_str());
            hrXmp = writer->SetMetadataByName(q.xmpFallback, &var);
            PropVariantClear(&var);
        }
    }

    // An XMP packet or Exif tag satisfies Explorer / Lightroom; succeed if either landed.
    if (SUCCEEDED(hrExif) || SUCCEEDED(hrXmp)) {
        return S_OK;
    }
    return FAILED(hrExif) ? hrExif : hrXmp;
}

// Patch the value into the padding the file already has: no re-encode, no
// rewrite, every pixel byte untouched.
HRESULT TryInPlace(IWICImagingFactory* factory, const std::wstring& path, int stars) {
    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr,
                                                    GENERIC_READ | GENERIC_WRITE,
                                                    WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr)) return hr;

    ComPtr<IWICFastMetadataEncoder> encoder;
    hr = factory->CreateFastMetadataEncoderFromDecoder(decoder.Get(), &encoder);
    if (FAILED(hr)) return hr;

    ComPtr<IWICMetadataQueryWriter> writer;
    hr = encoder->GetMetadataQueryWriter(&writer);
    if (FAILED(hr)) return hr;

    hr = ApplyRating(writer.Get(), QueriesFor(path), stars);
    if (FAILED(hr)) return hr;

    return encoder->Commit();
}

// Rebuild the file when there was no room to patch. WriteSource with a null
// rectangle copies the compressed frame as it is, so the pixels come through
// bit for bit; only the metadata block is new.
HRESULT Transcode(IWICImagingFactory* factory, const std::wstring& path, int stars) {
    // Decode from a copy held in memory rather than from the file. A decoder
    // opened on the path keeps it open even after every interface has been
    // released, and the swap at the end then fails with a sharing violation.
    std::vector<BYTE> sourceBytes;
    {
        HANDLE file = INVALID_HANDLE_VALUE;
        for (int retry = 0; retry < 5; ++retry) {
            file = CreateFileW(path.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (file != INVALID_HANDLE_VALUE) break;
            Sleep(15);
        }
        if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > MAXDWORD) {
            CloseHandle(file);
            return E_FAIL;
        }
        sourceBytes.resize((size_t)size.QuadPart);
        DWORD read = 0;
        const BOOL ok = ReadFile(file, sourceBytes.data(), (DWORD)sourceBytes.size(), &read, nullptr);
        CloseHandle(file);
        if (!ok || read != sourceBytes.size()) return E_FAIL;
    }

    ComPtr<IWICStream> sourceStream;
    HRESULT hr = factory->CreateStream(&sourceStream);
    if (FAILED(hr)) return hr;
    hr = sourceStream->InitializeFromMemory(sourceBytes.data(), (DWORD)sourceBytes.size());
    if (FAILED(hr)) return hr;

    ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromStream(sourceStream.Get(), nullptr,
                                          WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr)) return hr;

    GUID containerFormat{};
    hr = decoder->GetContainerFormat(&containerFormat);
    if (FAILED(hr)) return hr;

    UINT frameCount = 0;
    hr = decoder->GetFrameCount(&frameCount);
    if (FAILED(hr) || frameCount == 0) return FAILED(hr) ? hr : E_FAIL;

    const std::wstring tempPath = path + L".qvrating.tmp";
    ComPtr<IWICStream> stream;
    hr = factory->CreateStream(&stream);
    if (FAILED(hr)) return hr;
    hr = stream->InitializeFromFilename(tempPath.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) return hr;

    auto dropTemp = [&tempPath] { DeleteFileW(tempPath.c_str()); };

    ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(containerFormat, nullptr, &encoder);
    if (FAILED(hr)) { dropTemp(); return hr; }
    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) { dropTemp(); return hr; }

    for (UINT i = 0; i < frameCount; ++i) {
        ComPtr<IWICBitmapFrameDecode> frameDecode;
        hr = decoder->GetFrame(i, &frameDecode);
        if (FAILED(hr)) { dropTemp(); return hr; }

        ComPtr<IWICBitmapFrameEncode> frameEncode;
        hr = encoder->CreateNewFrame(&frameEncode, nullptr);
        if (FAILED(hr)) { dropTemp(); return hr; }
        hr = frameEncode->Initialize(nullptr);
        if (FAILED(hr)) { dropTemp(); return hr; }

        // Carry the existing metadata over before adding the rating to it.
        ComPtr<IWICMetadataBlockReader> blockReader;
        if (SUCCEEDED(frameDecode.As(&blockReader))) {
            ComPtr<IWICMetadataBlockWriter> blockWriter;
            if (SUCCEEDED(frameEncode.As(&blockWriter))) {
                blockWriter->InitializeFromBlockReader(blockReader.Get());
            }
        }

        if (i == 0) {
            ComPtr<IWICMetadataQueryWriter> writer;
            if (SUCCEEDED(frameEncode->GetMetadataQueryWriter(&writer))) {
                ApplyRating(writer.Get(), QueriesFor(path), stars);
            }
        }

        hr = frameEncode->WriteSource(frameDecode.Get(), nullptr);
        if (FAILED(hr)) { dropTemp(); return hr; }
        hr = frameEncode->Commit();
        if (FAILED(hr)) { dropTemp(); return hr; }
    }

    hr = encoder->Commit();
    if (FAILED(hr)) { dropTemp(); return hr; }

    // Release everything that keeps the temp file's stream alive before the
    // swap -- the query writer counts, since it holds the frame encoder, which
    // holds the stream, and ReplaceFileW needs the replacement to itself.
    // (The source file needs no such care: it was decoded from memory, so the
    // original was only ever open for the moment it took to read it.)
    stream.Reset();
    encoder.Reset();
    decoder.Reset();
    sourceStream.Reset();

    BOOL replaceOk = FALSE;
    DWORD lastErr = 0;
    for (int retry = 0; retry < 5; ++retry) {
        if (ReplaceFileW(path.c_str(), tempPath.c_str(), nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS,
                         nullptr, nullptr)) {
            replaceOk = TRUE;
            break;
        }
        lastErr = GetLastError();
        Sleep(20);
    }

    if (!replaceOk) {
        dropTemp();
        return HRESULT_FROM_WIN32(lastErr);
    }
    return S_OK;
}

uint16_t RatingPercentForStars(int starVal) {
    switch (starVal) {
        case 1: return 1;
        case 2: return 25;
        case 3: return 50;
        case 4: return 75;
        case 5: return 99;
        default: return 0;
    }
}

std::string BuildStandardXmpPayload(int starVal) {
    std::string s =
        "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
        "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n"
        " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
        "  <rdf:Description rdf:about=\"\"\n"
        "    xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\"\n";
    if (starVal > 0) {
        s += "    xmp:Rating=\"" + std::to_string(starVal) + "\"/>\n";
    } else {
        s += "   />\n";
    }
    s += " </rdf:RDF>\n"
         "</x:xmpmeta>\n";
    s.append(2048, ' ');
    s += "\n<?xpacket end=\"w\"?>";
    return s;
}

bool IsFileLockError(DWORD err) {
    return err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION;
}

HANDLE OpenFileForReadRetry(const std::wstring& path, DWORD* outLastErr = nullptr) {
    DWORD lastErr = 0;
    for (int attempt = 0; attempt < 5; ++attempt) {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) return file;
        lastErr = GetLastError();
        if (attempt + 1 < 5) Sleep(15);
    }
    if (outLastErr) *outLastErr = lastErr;
    return INVALID_HANDLE_VALUE;
}

bool AtomicReplaceFile(const std::wstring& targetPath, const std::wstring& tempPath,
                       DWORD* outLastErr = nullptr) {
    DWORD lastErr = 0;
    for (int retry = 0; retry < 5; ++retry) {
        if (ReplaceFileW(targetPath.c_str(), tempPath.c_str(), nullptr,
                         REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) {
            return true;
        }
        lastErr = GetLastError();
        if (retry + 1 < 5) Sleep(20);
    }
    DeleteFileW(tempPath.c_str());
    if (outLastErr) *outLastErr = lastErr;
    return false;
}

uint32_t ReadBe32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

void AppendBe32(std::vector<uint8_t>& out, uint32_t val) {
    out.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(val & 0xFF));
}

// Updates inline IFD0 SimpleRating (0x4746) and RatingPercent (0x4749) in place
// inside an uncompressed TIFF buffer. Returns true if at least one tag was patched.
bool PatchTiffBufferRating(std::span<uint8_t> tiff, int stars) {
    if (tiff.size() < 8) return false;
    const bool littleEndian = (tiff[0] == 'I' && tiff[1] == 'I');
    const bool bigEndian = (tiff[0] == 'M' && tiff[1] == 'M');
    if (!littleEndian && !bigEndian) return false;

    auto read16 = [littleEndian](const uint8_t* p) -> uint16_t {
        return littleEndian ? (static_cast<uint16_t>(p[1]) << 8 | p[0])
                            : (static_cast<uint16_t>(p[0]) << 8 | p[1]);
    };
    auto read32 = [littleEndian](const uint8_t* p) -> uint32_t {
        return littleEndian ? ((static_cast<uint32_t>(p[3]) << 24) | (static_cast<uint32_t>(p[2]) << 16) |
                               (static_cast<uint32_t>(p[1]) << 8) | p[0])
                            : ((static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
                               (static_cast<uint32_t>(p[2]) << 8) | p[3]);
    };
    auto write16 = [littleEndian](uint8_t* p, uint16_t v) {
        if (littleEndian) {
            p[0] = static_cast<uint8_t>(v & 0xFF);
            p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
        } else {
            p[0] = static_cast<uint8_t>((v >> 8) & 0xFF);
            p[1] = static_cast<uint8_t>(v & 0xFF);
        }
    };
    auto write32 = [littleEndian](uint8_t* p, uint32_t v) {
        if (littleEndian) {
            p[0] = static_cast<uint8_t>(v & 0xFF);
            p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
            p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
            p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
        } else {
            p[0] = static_cast<uint8_t>((v >> 24) & 0xFF);
            p[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
            p[2] = static_cast<uint8_t>((v >> 8) & 0xFF);
            p[3] = static_cast<uint8_t>(v & 0xFF);
        }
    };

    if (read16(tiff.data() + 2) != 42) return false;
    const uint32_t ifdOffset = read32(tiff.data() + 4);
    if (ifdOffset < 8 || static_cast<uint64_t>(ifdOffset) + 2 > tiff.size()) return false;

    const uint16_t entryCount = read16(tiff.data() + ifdOffset);
    const size_t tableEnd = static_cast<size_t>(ifdOffset) + 2 + static_cast<size_t>(entryCount) * 12;
    if (entryCount == 0 || tableEnd > tiff.size()) return false;

    const int clampedStars = std::clamp(stars, 0, 5);
    const uint16_t pct = RatingPercentForStars(clampedStars);
    bool updatedAny = false;

    for (uint16_t i = 0; i < entryCount; ++i) {
        uint8_t* entry = tiff.data() + ifdOffset + 2 + static_cast<size_t>(i) * 12;
        const uint16_t tag = read16(entry);
        if (tag != 0x4746 && tag != 0x4749) continue;

        const uint16_t type = read16(entry + 2);
        const uint32_t count = read32(entry + 4);
        if (count != 1) continue;

        const uint32_t val = (tag == 0x4746) ? static_cast<uint32_t>(clampedStars) : static_cast<uint32_t>(pct);
        switch (type) {
            case 1: // BYTE
                entry[8] = static_cast<uint8_t>(val & 0xFF);
                updatedAny = true;
                break;
            case 3: // SHORT
                write16(entry + 8, static_cast<uint16_t>(val));
                updatedAny = true;
                break;
            case 4: // LONG
                write32(entry + 8, val);
                updatedAny = true;
                break;
            default:
                break;
        }
    }
    return updatedAny;
}

// Lossless native RIFF container surgery for WebP: preserves all compressed
// frames bit-for-bit without requiring a WIC WebP encoder component.
WriteStatus WriteRatingToWebp(const std::wstring& path, int stars) {
    std::vector<uint8_t> src;
    {
        HANDLE file = INVALID_HANDLE_VALUE;
        for (int retry = 0; retry < 5; ++retry) {
            file = CreateFileW(path.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (file != INVALID_HANDLE_VALUE) break;
            Sleep(15);
        }
        if (file == INVALID_HANDLE_VALUE) return WriteStatus::Failed;

        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file, &size) || size.QuadPart < 12 || size.QuadPart > MAXDWORD) {
            CloseHandle(file);
            return WriteStatus::Failed;
        }
        src.resize((size_t)size.QuadPart);
        DWORD read = 0;
        const BOOL ok = ReadFile(file, src.data(), (DWORD)src.size(), &read, nullptr);
        CloseHandle(file);
        if (!ok || read != src.size()) return WriteStatus::Failed;
    }

    if (src.size() < 12 ||
        std::memcmp(src.data(), "RIFF", 4) != 0 ||
        std::memcmp(src.data() + 8, "WEBP", 4) != 0) {
        return WriteStatus::Failed;
    }

    struct ChunkSpan {
        char fourcc[4];
        size_t dataOffset;
        uint32_t dataSize;
        size_t totalChunkSpan;
    };

    std::vector<ChunkSpan> chunks;
    size_t pos = 12;
    int xmpIndex = -1;
    int vp8xIndex = -1;

    while (pos + 8 <= src.size()) {
        char fcc[4];
        std::memcpy(fcc, src.data() + pos, 4);
        const uint32_t len = static_cast<uint32_t>(src[pos + 4]) |
                             (static_cast<uint32_t>(src[pos + 5]) << 8) |
                             (static_cast<uint32_t>(src[pos + 6]) << 16) |
                             (static_cast<uint32_t>(src[pos + 7]) << 24);
        const size_t dataOffset = pos + 8;
        if (dataOffset + len > src.size()) break; // truncated

        const size_t pad = len & 1;
        const size_t totalSpan = 8 + len + pad;

        if (std::memcmp(fcc, "VP8X", 4) == 0) {
            vp8xIndex = static_cast<int>(chunks.size());
        } else if (std::memcmp(fcc, "XMP ", 4) == 0) {
            xmpIndex = static_cast<int>(chunks.size());
        }

        chunks.push_back(ChunkSpan{ {fcc[0], fcc[1], fcc[2], fcc[3]}, dataOffset, len, totalSpan });
        pos += totalSpan;
    }

    std::string newXmpPayload;
    bool hasNewXmp = false;

    if (xmpIndex >= 0) {
        const auto& c = chunks[xmpIndex];
        const std::string_view oldXmp(reinterpret_cast<const char*>(src.data() + c.dataOffset), c.dataSize);
        if (stars > 0) {
            if (auto updated = QuickView::Rating::UpdateXmpRating(oldXmp, stars)) {
                newXmpPayload = *updated;
                hasNewXmp = true;
            } else {
                newXmpPayload = QuickView::Rating::BuildMinimalXmp(stars);
                hasNewXmp = true;
            }
        } else {
            // stars == 0: remove rating
            if (auto updated = QuickView::Rating::UpdateXmpRating(oldXmp, 0)) {
                newXmpPayload = *updated;
                hasNewXmp = true;
            }
        }
    } else if (stars > 0) {
        newXmpPayload = QuickView::Rating::BuildMinimalXmp(stars);
        hasNewXmp = true;
    } else {
        // stars == 0 and no prior XMP: nothing to remove or write
        return WriteStatus::WrittenInPlace;
    }

    // Rebuild the WebP file with zero pixel re-encode
    std::vector<uint8_t> out;
    out.reserve(src.size() + newXmpPayload.size() + 64);

    // Initial 12-byte RIFF header (placeholder size)
    out.insert(out.end(), { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P' });

    auto appendChunk = [&out](const char fourcc[4], const void* data, size_t size) {
        out.push_back(fourcc[0]); out.push_back(fourcc[1]);
        out.push_back(fourcc[2]); out.push_back(fourcc[3]);
        const uint32_t sz = static_cast<uint32_t>(size);
        out.push_back(static_cast<uint8_t>(sz & 0xFF));
        out.push_back(static_cast<uint8_t>((sz >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>((sz >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((sz >> 24) & 0xFF));
        const uint8_t* p = reinterpret_cast<const uint8_t*>(data);
        out.insert(out.end(), p, p + size);
        if (size & 1) {
            out.push_back(0); // 1-byte RIFF padding
        }
    };

    // Synthesize VP8X if absent and we are adding XMP
    std::vector<uint8_t> synthesizedVp8x;
    if (vp8xIndex < 0 && hasNewXmp) {
        uint32_t canvasWidth = 0;
        uint32_t canvasHeight = 0;
        uint8_t flags = 0x04; // XMP bit

        for (const auto& c : chunks) {
            const uint8_t* p = src.data() + c.dataOffset;
            if (std::memcmp(c.fourcc, "VP8 ", 4) == 0 && c.dataSize >= 10) {
                if ((p[0] & 1) == 0 && p[3] == 0x9D && p[4] == 0x01 && p[5] == 0x2A) {
                    canvasWidth = (static_cast<uint32_t>(p[6]) | (static_cast<uint32_t>(p[7]) << 8)) & 0x3FFF;
                    canvasHeight = (static_cast<uint32_t>(p[8]) | (static_cast<uint32_t>(p[9]) << 8)) & 0x3FFF;
                    break;
                }
            } else if (std::memcmp(c.fourcc, "VP8L", 4) == 0 && c.dataSize >= 5 && p[0] == 0x2F) {
                const uint32_t b1 = p[1], b2 = p[2], b3 = p[3], b4 = p[4];
                canvasWidth = 1 + ((b1 | (b2 << 8)) & 0x3FFF);
                canvasHeight = 1 + (((b2 >> 6) | (b3 << 2) | ((b4 & 0x0F) << 10)) & 0x3FFF);
                if (b4 & 0x10) flags |= 0x10; // Alpha flag
                break;
            }
        }

        if (canvasWidth > 0 && canvasHeight > 0) {
            synthesizedVp8x.resize(10, 0);
            synthesizedVp8x[0] = flags;
            const uint32_t wMinus1 = canvasWidth - 1;
            const uint32_t hMinus1 = canvasHeight - 1;
            synthesizedVp8x[4] = static_cast<uint8_t>(wMinus1 & 0xFF);
            synthesizedVp8x[5] = static_cast<uint8_t>((wMinus1 >> 8) & 0xFF);
            synthesizedVp8x[6] = static_cast<uint8_t>((wMinus1 >> 16) & 0xFF);
            synthesizedVp8x[7] = static_cast<uint8_t>(hMinus1 & 0xFF);
            synthesizedVp8x[8] = static_cast<uint8_t>((hMinus1 >> 8) & 0xFF);
            synthesizedVp8x[9] = static_cast<uint8_t>((hMinus1 >> 16) & 0xFF);
        }
    }

    // Update existing VP8X XMP flag if present
    if (vp8xIndex >= 0 && vp8xIndex < (int)chunks.size()) {
        auto& v = chunks[vp8xIndex];
        if (v.dataSize >= 10 && v.dataOffset < src.size()) {
            if (hasNewXmp) {
                src[v.dataOffset] |= 0x04; // Set XMP flag bit
            } else if (stars <= 0 && xmpIndex >= 0) {
                src[v.dataOffset] &= ~0x04; // Clear XMP flag bit
            }
        }
    }

    // If synthesized VP8X exists, insert it first right after WEBP
    if (!synthesizedVp8x.empty()) {
        appendChunk("VP8X", synthesizedVp8x.data(), synthesizedVp8x.size());
    }

    bool xmpWritten = false;
    for (size_t i = 0; i < chunks.size(); ++i) {
        if ((int)i == xmpIndex) {
            if (hasNewXmp) {
                appendChunk("XMP ", newXmpPayload.data(), newXmpPayload.size());
                xmpWritten = true;
            }
            continue; // replaced old XMP
        }
        const auto& c = chunks[i];
        appendChunk(c.fourcc, src.data() + c.dataOffset, c.dataSize);
    }

    if (hasNewXmp && !xmpWritten) {
        appendChunk("XMP ", newXmpPayload.data(), newXmpPayload.size());
    }

    // Patch RIFF total size: out.size() - 8
    const uint32_t totalRiffSize = static_cast<uint32_t>(out.size() - 8);
    out[4] = static_cast<uint8_t>(totalRiffSize & 0xFF);
    out[5] = static_cast<uint8_t>((totalRiffSize >> 8) & 0xFF);
    out[6] = static_cast<uint8_t>((totalRiffSize >> 16) & 0xFF);
    out[7] = static_cast<uint8_t>((totalRiffSize >> 24) & 0xFF);

    // Atomic write via temporary file
    const std::wstring tempPath = path + L".qvrating.tmp";
    HANDLE hTemp = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hTemp == INVALID_HANDLE_VALUE) return WriteStatus::Failed;

    DWORD written = 0;
    const BOOL writeOk = WriteFile(hTemp, out.data(), (DWORD)out.size(), &written, nullptr);
    CloseHandle(hTemp);

    if (!writeOk || written != out.size()) {
        DeleteFileW(tempPath.c_str());
        return WriteStatus::Failed;
    }

    BOOL replaceOk = FALSE;
    DWORD lastError = 0;
    for (int retry = 0; retry < 5; ++retry) {
        if (ReplaceFileW(path.c_str(), tempPath.c_str(), nullptr,
                         REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) {
            replaceOk = TRUE;
            break;
        }
        lastError = GetLastError();
        Sleep(20);
    }

    if (!replaceOk) {
        DeleteFileW(tempPath.c_str());
        if (lastError == ERROR_SHARING_VIOLATION || lastError == ERROR_LOCK_VIOLATION ||
            lastError == ERROR_ACCESS_DENIED) {
            return WriteStatus::NeedsTranscode;
        }
        return WriteStatus::Failed;
    }

    return WriteStatus::WrittenTranscode;
}

static bool PatchIsobmffExifInPlace(const std::wstring& path,
                                    std::span<const uint8_t> src,
                                    size_t exifPayloadOff,
                                    size_t exifPayloadLen,
                                    int stars) {
    if (exifPayloadLen < 16 || exifPayloadOff + exifPayloadLen > src.size()) return false;

    // ISOBMFF Exif item starts with 4-byte offset to TIFF header
    const uint32_t tiffOffset = (static_cast<uint32_t>(src[exifPayloadOff]) << 24) |
                                (static_cast<uint32_t>(src[exifPayloadOff + 1]) << 16) |
                                (static_cast<uint32_t>(src[exifPayloadOff + 2]) << 8) |
                                static_cast<uint32_t>(src[exifPayloadOff + 3]);

    size_t tiffPos = exifPayloadOff + 4 + tiffOffset;
    if (tiffPos + 8 > exifPayloadOff + exifPayloadLen || tiffPos >= src.size()) {
        tiffPos = exifPayloadOff + 4;
    }
    if (tiffPos + 8 > src.size()) return false;

    const bool littleEndian = (src[tiffPos] == 'I' && src[tiffPos + 1] == 'I');
    const bool bigEndian = (src[tiffPos] == 'M' && src[tiffPos + 1] == 'M');
    if (!littleEndian && !bigEndian) return false;

    auto readU16 = [littleEndian](const uint8_t* p) -> uint16_t {
        return littleEndian ? (static_cast<uint16_t>(p[1]) << 8 | p[0])
                            : (static_cast<uint16_t>(p[0]) << 8 | p[1]);
    };
    auto readU32 = [littleEndian](const uint8_t* p) -> uint32_t {
        return littleEndian ? ((static_cast<uint32_t>(p[3]) << 24) | (static_cast<uint32_t>(p[2]) << 16) |
                               (static_cast<uint32_t>(p[1]) << 8) | p[0])
                            : ((static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
                               (static_cast<uint32_t>(p[2]) << 8) | p[3]);
    };

    if (readU16(src.data() + tiffPos + 2) != 42) return false;
    const uint32_t ifdOffset = readU32(src.data() + tiffPos + 4);
    if (ifdOffset == 0 || tiffPos + ifdOffset + 2 > src.size()) return false;

    const size_t ifdPos = tiffPos + ifdOffset;
    const uint16_t numEntries = readU16(src.data() + ifdPos);
    size_t cur = ifdPos + 2;

    size_t tag4746ValOff = 0;
    size_t tag4749ValOff = 0;

    for (uint16_t i = 0; i < numEntries && cur + 12 <= src.size(); ++i, cur += 12) {
        const uint16_t tag = readU16(src.data() + cur);
        const uint16_t type = readU16(src.data() + cur + 2);
        if (type == 3) { // SHORT
            if (tag == 0x4746) {
                tag4746ValOff = cur + 8;
            } else if (tag == 0x4749) {
                tag4749ValOff = cur + 8;
            }
        }
    }

    if (tag4746ValOff == 0 && tag4749ValOff == 0) return false;

    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    auto writeShort = [&](size_t fileOffset, uint16_t val) {
        LARGE_INTEGER li;
        li.QuadPart = static_cast<LONGLONG>(fileOffset);
        SetFilePointerEx(hFile, li, nullptr, FILE_BEGIN);
        uint8_t buf[2];
        if (littleEndian) {
            buf[0] = static_cast<uint8_t>(val & 0xFF);
            buf[1] = static_cast<uint8_t>((val >> 8) & 0xFF);
        } else {
            buf[0] = static_cast<uint8_t>((val >> 8) & 0xFF);
            buf[1] = static_cast<uint8_t>(val & 0xFF);
        }
        DWORD written = 0;
        WriteFile(hFile, buf, 2, &written, nullptr);
    };

    if (tag4746ValOff > 0) {
        writeShort(tag4746ValOff, static_cast<uint16_t>(stars > 0 ? stars : 0));
    }
    if (tag4749ValOff > 0) {
        uint16_t pct = 0;
        switch (stars) {
            case 1: pct = 1; break;
            case 2: pct = 25; break;
            case 3: pct = 50; break;
            case 4: pct = 75; break;
            case 5: pct = 99; break;
            default: pct = 0; break;
        }
        writeShort(tag4749ValOff, pct);
    }

    FlushFileBuffers(hFile);
    CloseHandle(hFile);
    return true;
}

WriteStatus WriteRatingToIsobmff(const std::wstring& path, int stars) {
    std::vector<uint8_t> src;
    {
        HANDLE file = INVALID_HANDLE_VALUE;
        for (int attempt = 0; attempt < 5; ++attempt) {
            file = CreateFileW(path.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) break;
            Sleep(15);
        }
        if (file == INVALID_HANDLE_VALUE) return WriteStatus::Failed;

        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file, &size) || size.QuadPart < 12 || size.QuadPart > 256 * 1024 * 1024) {
            CloseHandle(file);
            return WriteStatus::Failed;
        }
        src.resize((size_t)size.QuadPart);
        DWORD read = 0;
        const BOOL ok = ReadFile(file, src.data(), (DWORD)src.size(), &read, nullptr);
        CloseHandle(file);
        if (!ok || read != src.size()) return WriteStatus::Failed;
    }

    if (src.size() < 12 || std::memcmp(src.data() + 4, "ftyp", 4) != 0) {
        return WriteStatus::Failed;
    }

    auto readU16 = [](const uint8_t* b) -> uint16_t {
        return (static_cast<uint16_t>(b[0]) << 8) | b[1];
    };
    auto readU32 = [](const uint8_t* b) -> uint32_t {
        return (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) |
               (static_cast<uint32_t>(b[2]) << 8) | b[3];
    };
    auto readU64 = [&readU32](const uint8_t* b) -> uint64_t {
        return (static_cast<uint64_t>(readU32(b)) << 32) | readU32(b + 4);
    };
    auto readVarInt = [](const uint8_t* b, size_t numBytes) -> uint64_t {
        uint64_t val = 0;
        for (size_t i = 0; i < numBytes; ++i) val = (val << 8) | b[i];
        return val;
    };
    auto writeU16 = [](std::vector<uint8_t>& buf, uint16_t val) {
        buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
        buf.push_back(static_cast<uint8_t>(val & 0xFF));
    };
    auto writeU32 = [](std::vector<uint8_t>& buf, uint32_t val) {
        buf.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
        buf.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
        buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
        buf.push_back(static_cast<uint8_t>(val & 0xFF));
    };
    auto writeVarInt = [](std::vector<uint8_t>& buf, uint64_t val, size_t numBytes) {
        for (size_t i = 0; i < numBytes; ++i) {
            const size_t shift = (numBytes - 1 - i) * 8;
            buf.push_back(static_cast<uint8_t>((val >> shift) & 0xFF));
        }
    };

    // Locate top-level boxes: 'meta', 'mdat'
    size_t pos = 0;
    size_t metaPos = 0, metaSize = 0, metaHeaderSize = 0, metaPayloadPos = 0, metaEnd = 0;
    size_t mdatPos = 0, mdatSize = 0;

    while (pos + 8 <= src.size()) {
        const uint32_t rawSize = readU32(src.data() + pos);
        const uint8_t* type = src.data() + pos + 4;
        size_t boxSize = rawSize;
        size_t headerSize = 8;

        if (rawSize == 1) {
            if (pos + 16 > src.size()) break;
            const uint64_t extSize = readU64(src.data() + pos + 8);
            if (extSize < 16) break;
            boxSize = static_cast<size_t>(extSize);
            headerSize = 16;
        } else if (rawSize == 0) {
            boxSize = src.size() - pos;
        }

        if (boxSize < headerSize) break;
        const size_t nextPos = pos + boxSize;

        if (std::memcmp(type, "meta", 4) == 0) {
            if (pos + headerSize + 4 <= src.size()) {
                metaPos = pos;
                metaSize = boxSize;
                metaHeaderSize = headerSize;
                metaPayloadPos = pos + headerSize + 4;
                metaEnd = (std::min)(nextPos, src.size());
            }
        } else if (std::memcmp(type, "mdat", 4) == 0) {
            mdatPos = pos;
            mdatSize = boxSize;
        }

        if (nextPos <= pos || nextPos > src.size()) break;
        pos = nextPos;
    }

    if (metaPayloadPos == 0 || metaPayloadPos >= metaEnd) return WriteStatus::Failed;

    // Locate subboxes inside 'meta'
    size_t iinfOffset = 0, iinfSize = 0;
    size_t ilocOffset = 0, ilocSize = 0;
    size_t pitmOffset = 0, pitmSize = 0;
    size_t irefOffset = 0, irefSize = 0;

    size_t subPos = metaPayloadPos;
    while (subPos + 8 <= metaEnd) {
        const uint32_t subSize = readU32(src.data() + subPos);
        if (subSize < 8 || subPos + subSize > metaEnd) break;
        const uint8_t* subType = src.data() + subPos + 4;

        if (std::memcmp(subType, "iinf", 4) == 0) {
            iinfOffset = subPos;
            iinfSize = subSize;
        } else if (std::memcmp(subType, "iloc", 4) == 0) {
            ilocOffset = subPos;
            ilocSize = subSize;
        } else if (std::memcmp(subType, "pitm", 4) == 0) {
            pitmOffset = subPos;
            pitmSize = subSize;
        } else if (std::memcmp(subType, "iref", 4) == 0) {
            irefOffset = subPos;
            irefSize = subSize;
        }
        subPos += subSize;
    }

    if (iinfOffset == 0 || ilocOffset == 0) return WriteStatus::Failed;

    uint32_t primaryItemId = 1;
    if (pitmOffset > 0 && pitmSize >= 14) {
        const uint8_t pitmVer = src[pitmOffset + 8];
        if (pitmVer == 0 && pitmSize >= 14) {
            primaryItemId = readU16(src.data() + pitmOffset + 12);
        } else if (pitmVer == 1 && pitmSize >= 16) {
            primaryItemId = readU32(src.data() + pitmOffset + 12);
        }
    }

    std::vector<uint32_t> primaryCdscItems;
    if (irefOffset > 0 && irefSize >= 12) {
        const uint8_t irefVer = src[irefOffset + 8];
        size_t irefCur = irefOffset + 12;
        while (irefCur + 8 <= irefOffset + irefSize) {
            const uint32_t boxSz = readU32(src.data() + irefCur);
            if (boxSz < 8 || irefCur + boxSz > irefOffset + irefSize) break;
            const uint8_t* btype = src.data() + irefCur + 4;
            if (std::memcmp(btype, "cdsc", 4) == 0) {
                if (irefVer == 0 && boxSz >= 14) {
                    const uint16_t fromId = readU16(src.data() + irefCur + 8);
                    const uint16_t refCnt = readU16(src.data() + irefCur + 10);
                    for (uint16_t r = 0; r < refCnt && irefCur + 12 + (r + 1) * 2 <= irefCur + boxSz; ++r) {
                        const uint16_t toId = readU16(src.data() + irefCur + 12 + r * 2);
                        if (toId == primaryItemId) {
                            primaryCdscItems.push_back(fromId);
                            break;
                        }
                    }
                } else if (irefVer == 1 && boxSz >= 18) {
                    const uint32_t fromId = readU32(src.data() + irefCur + 8);
                    const uint16_t refCnt = readU16(src.data() + irefCur + 12);
                    for (uint16_t r = 0; r < refCnt && irefCur + 14 + (r + 1) * 4 <= irefCur + boxSz; ++r) {
                        const uint32_t toId = readU32(src.data() + irefCur + 14 + r * 4);
                        if (toId == primaryItemId) {
                            primaryCdscItems.push_back(fromId);
                            break;
                        }
                    }
                }
            }
            irefCur += boxSz;
        }
    }

    auto isCdscForPrimary = [&](uint32_t id) {
        return std::find(primaryCdscItems.begin(), primaryCdscItems.end(), id) != primaryCdscItems.end();
    };

    // Find exifItemId and xmpItemId from 'iinf'
    uint32_t exifItemId = 0;
    uint32_t xmpItemId = 0;
    bool exifLocked = false;
    bool xmpLocked = false;

    const uint8_t iinfVer = src[iinfOffset + 8];
    size_t iinfCur = iinfOffset + 12;
    uint32_t entryCount = 0;
    if (iinfVer == 0) {
        if (iinfCur + 2 <= iinfOffset + iinfSize) {
            entryCount = readU16(src.data() + iinfCur);
            iinfCur += 2;
        }
    } else {
        if (iinfCur + 4 <= iinfOffset + iinfSize) {
            entryCount = readU32(src.data() + iinfCur);
            iinfCur += 4;
        }
    }

    for (uint32_t i = 0; i < entryCount && iinfCur + 8 <= iinfOffset + iinfSize; ++i) {
        const uint32_t infeSize = readU32(src.data() + iinfCur);
        if (infeSize < 8 || iinfCur + infeSize > iinfOffset + iinfSize) break;

        if (std::memcmp(src.data() + iinfCur + 4, "infe", 4) == 0 && infeSize >= 12) {
            const uint8_t infeVer = src[iinfCur + 8];
            if (infeVer >= 2) {
                uint32_t itemId = 0;
                size_t typeOffset = 0;
                if (infeVer == 2 && infeSize >= 16) {
                    itemId = readU16(src.data() + iinfCur + 12);
                    typeOffset = iinfCur + 16;
                } else if (infeVer == 3 && infeSize >= 18) {
                    itemId = readU32(src.data() + iinfCur + 12);
                    typeOffset = iinfCur + 18;
                }
                if (typeOffset > 0 && typeOffset + 4 <= iinfCur + infeSize) {
                    const uint8_t* itype = src.data() + typeOffset;
                    if (std::memcmp(itype, "Exif", 4) == 0) {
                        if (isCdscForPrimary(itemId)) {
                            if (!exifLocked) {
                                exifItemId = itemId;
                                exifLocked = true;
                            }
                        } else if (!exifLocked && exifItemId == 0) {
                            exifItemId = itemId;
                        }
                    } else if (std::memcmp(itype, "mime", 4) == 0) {
                        std::string_view rest(
                            reinterpret_cast<const char*>(src.data() + typeOffset + 4),
                            infeSize - (typeOffset + 4 - iinfCur));
                        if (rest.find("application/rdf+xml") != std::string_view::npos ||
                            rest.find("application/x-xmp") != std::string_view::npos ||
                            rest.find("xmp") != std::string_view::npos) {
                            if (isCdscForPrimary(itemId)) {
                                if (!xmpLocked) {
                                    xmpItemId = itemId;
                                    xmpLocked = true;
                                }
                            } else if (!xmpLocked && xmpItemId == 0) {
                                xmpItemId = itemId;
                            }
                        }
                    }
                }
            }
        }
        iinfCur += infeSize;
    }

    struct Extent {
        uint64_t offset = 0;
        uint64_t length = 0;
        uint64_t index = 0;
    };
    struct ExistingItem {
        uint32_t id = 0;
        uint16_t constructionMethod = 0;
        uint16_t dref = 0;
        uint64_t baseOffset = 0;
        std::vector<Extent> extents;
    };

    std::vector<ExistingItem> existingItems;
    uint32_t maxItemId = 0;
    size_t exifPayloadOff = 0, exifPayloadLen = 0;
    size_t xmpPayloadOff = 0, xmpPayloadLen = 0;

    const uint8_t ilocVer = src[ilocOffset + 8];
    const uint8_t offSize = (src[ilocOffset + 12] >> 4) & 0x0F;
    const uint8_t lenSize = src[ilocOffset + 12] & 0x0F;
    const uint8_t baseOffSize = (src[ilocOffset + 13] >> 4) & 0x0F;
    const uint8_t indexSize = (ilocVer == 1 || ilocVer == 2) ? (src[ilocOffset + 13] & 0x0F) : 0;

    size_t ilocCur = ilocOffset + 14;
    uint32_t itemCount = 0;
    if (ilocVer < 2) {
        if (ilocCur + 2 <= ilocOffset + ilocSize) {
            itemCount = readU16(src.data() + ilocCur);
            ilocCur += 2;
        }
    } else {
        if (ilocCur + 4 <= ilocOffset + ilocSize) {
            itemCount = readU32(src.data() + ilocCur);
            ilocCur += 4;
        }
    }

    existingItems.reserve(itemCount);

    for (uint32_t i = 0; i < itemCount && ilocCur < ilocOffset + ilocSize; ++i) {
        uint32_t itemId = 0;
        if (ilocVer < 2) {
            if (ilocCur + 2 > ilocOffset + ilocSize) break;
            itemId = readU16(src.data() + ilocCur);
            ilocCur += 2;
        } else {
            if (ilocCur + 4 > ilocOffset + ilocSize) break;
            itemId = readU32(src.data() + ilocCur);
            ilocCur += 4;
        }
        if (itemId > maxItemId) maxItemId = itemId;

        uint16_t cm = 0;
        if (ilocVer == 1 || ilocVer == 2) {
            if (ilocCur + 2 > ilocOffset + ilocSize) break;
            cm = readU16(src.data() + ilocCur) & 0x0F;
            ilocCur += 2;
        }

        uint16_t dref = 0;
        if (ilocCur + 2 <= ilocOffset + ilocSize) {
            dref = readU16(src.data() + ilocCur);
        }
        ilocCur += 2;

        const uint64_t baseOffset = readVarInt(src.data() + ilocCur, baseOffSize);
        ilocCur += baseOffSize;

        if (ilocCur + 2 > ilocOffset + ilocSize) break;
        const uint16_t extentCount = readU16(src.data() + ilocCur);
        ilocCur += 2;

        ExistingItem item;
        item.id = itemId;
        item.constructionMethod = cm;
        item.dref = dref;
        item.baseOffset = baseOffset;
        item.extents.reserve(extentCount);

        for (uint16_t e = 0; e < extentCount; ++e) {
            uint64_t idx = 0;
            if (indexSize > 0) {
                idx = readVarInt(src.data() + ilocCur, indexSize);
                ilocCur += indexSize;
            }
            const uint64_t extentOffset = readVarInt(src.data() + ilocCur, offSize);
            ilocCur += offSize;
            const uint64_t extentLength = readVarInt(src.data() + ilocCur, lenSize);
            ilocCur += lenSize;

            item.extents.push_back(Extent{ extentOffset, extentLength, idx });

            if (e == 0) {
                if (itemId == exifItemId && exifItemId > 0) {
                    exifPayloadOff = static_cast<size_t>(baseOffset + extentOffset);
                    exifPayloadLen = static_cast<size_t>(extentLength);
                } else if (itemId == xmpItemId && xmpItemId > 0) {
                    xmpPayloadOff = static_cast<size_t>(baseOffset + extentOffset);
                    xmpPayloadLen = static_cast<size_t>(extentLength);
                }
            }
        }
        existingItems.push_back(std::move(item));
    }

    bool inPlaceSuccess = false;

    // --- 1. In-Place Exif Rating Patch ---
    if (exifPayloadOff > 0 && exifPayloadLen > 12) {
        if (PatchIsobmffExifInPlace(path, src, exifPayloadOff, exifPayloadLen, stars)) {
            inPlaceSuccess = true;
        }
    }

    // --- 2. In-Place Strict XMP Padding Patch ---
    if (!inPlaceSuccess && xmpPayloadOff > 0 && xmpPayloadLen > 0 && xmpPayloadOff + xmpPayloadLen <= src.size()) {
        const std::string_view oldXmp(reinterpret_cast<const char*>(src.data() + xmpPayloadOff), xmpPayloadLen);
        const auto patched = QuickView::Rating::PatchXmpRatingInPlaceStrict(oldXmp, stars);
        if (patched && patched->size() == xmpPayloadLen) {
            HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hFile != INVALID_HANDLE_VALUE) {
                LARGE_INTEGER seekPos;
                seekPos.QuadPart = static_cast<LONGLONG>(xmpPayloadOff);
                SetFilePointerEx(hFile, seekPos, nullptr, FILE_BEGIN);
                DWORD written = 0;
                const BOOL ok = WriteFile(hFile, patched->data(), static_cast<DWORD>(xmpPayloadLen), &written, nullptr);
                FlushFileBuffers(hFile);
                CloseHandle(hFile);
                if (ok && written == xmpPayloadLen) {
                    inPlaceSuccess = true;
                }
            }
        }
    }

    if (inPlaceSuccess) {
        return WriteStatus::WrittenInPlace;
    }

    if (stars <= 0) {
        return WriteStatus::WrittenInPlace;
    }

    // --- 3. Lossless ISOBMFF Reconstruction & Safe Injection ---
    if (maxItemId >= 0xFFFF || existingItems.size() >= 0xFFFF) {
        return WriteStatus::Failed;
    }

    const std::wstring_view ext = QuickView::ExtensionOf(path);
    const bool isAvif = QuickView::ExtEqualsIgnoreCase(ext, L".avif") ||
                        QuickView::ExtEqualsIgnoreCase(ext, L".avifs");

    const uint32_t newItemId = (maxItemId > 0 ? maxItemId : static_cast<uint32_t>(existingItems.size())) + 1;

    // Determine payload: AVIF needs Exif (with cdsc) for Windows Explorer PhotoMetadataHandler;
    // HEIC/HEIF without XMP injects XMP with cdsc (or Exif if no Exif).
    const bool injectExif = isAvif || (exifItemId == 0 && xmpItemId != 0);

    std::vector<uint8_t> payload;
    std::vector<uint8_t> infeBox;

    if (injectExif) {
        uint16_t pct = 0;
        switch (stars) {
            case 1: pct = 1; break;
            case 2: pct = 25; break;
            case 3: pct = 50; break;
            case 4: pct = 75; break;
            case 5: pct = 99; break;
            default: pct = 0; break;
        }

        // 4 bytes ISOBMFF Exif offset to TIFF header (0) (Big Endian per ISOBMFF spec)
        writeU32(payload, 0);
        // TIFF header: "II" (little endian), 42 (0x002A), IFD0 offset 8
        payload.push_back('I'); payload.push_back('I');
        payload.push_back(0x2A); payload.push_back(0x00);
        payload.push_back(0x08); payload.push_back(0x00); payload.push_back(0x00); payload.push_back(0x00);
        // IFD0 entries count: 2 (LE)
        payload.push_back(0x02); payload.push_back(0x00);
        // tag 0x4746: type 3 (SHORT), count 1, val = stars (All LE)
        payload.push_back(0x46); payload.push_back(0x47);
        payload.push_back(0x03); payload.push_back(0x00);
        payload.push_back(0x01); payload.push_back(0x00); payload.push_back(0x00); payload.push_back(0x00);
        payload.push_back(static_cast<uint8_t>(stars & 0xFF));
        payload.push_back(static_cast<uint8_t>((stars >> 8) & 0xFF));
        payload.push_back(0x00); payload.push_back(0x00);
        // tag 0x4749: type 3 (SHORT), count 1, val = pct (All LE)
        payload.push_back(0x49); payload.push_back(0x47);
        payload.push_back(0x03); payload.push_back(0x00);
        payload.push_back(0x01); payload.push_back(0x00); payload.push_back(0x00); payload.push_back(0x00);
        payload.push_back(static_cast<uint8_t>(pct & 0xFF));
        payload.push_back(static_cast<uint8_t>((pct >> 8) & 0xFF));
        payload.push_back(0x00); payload.push_back(0x00);
        // Next IFD: 0 (LE)
        payload.push_back(0x00); payload.push_back(0x00); payload.push_back(0x00); payload.push_back(0x00);

        // infe for Exif: ver=2, flags=0, itemId(2), protection=0(2), 'Exif'(4), name='\0'(1)
        const uint32_t infeSize = 8 + 4 + 2 + 2 + 4 + 1;
        infeBox.reserve(infeSize);
        writeU32(infeBox, infeSize);
        infeBox.push_back('i'); infeBox.push_back('n'); infeBox.push_back('f'); infeBox.push_back('e');
        writeU32(infeBox, 2u << 24);
        writeU16(infeBox, static_cast<uint16_t>(newItemId));
        writeU16(infeBox, 0);
        infeBox.push_back('E'); infeBox.push_back('x'); infeBox.push_back('i'); infeBox.push_back('f');
        infeBox.push_back('\0');
    } else {
        const std::string xmpStr =
            "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
            "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n"
            " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
            "  <rdf:Description rdf:about=\"\"\n"
            "    xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\"\n"
            "    xmp:Rating=\"" + std::to_string(stars) + "\"/>\n"
            " </rdf:RDF>\n"
            "</x:xmpmeta>\n"
            "<?xpacket end=\"w\"?>\n";
        payload.assign(xmpStr.begin(), xmpStr.end());

        // infe for XMP: ver=2, flags=0, itemId(2), protection=0(2), 'mime'(4), name='\0'(1), mimeType(20)
        const std::string_view mimeType = "application/rdf+xml";
        const uint32_t infeSize = 8 + 4 + 2 + 2 + 4 + 1 + static_cast<uint32_t>(mimeType.size() + 1);
        infeBox.reserve(infeSize);
        writeU32(infeBox, infeSize);
        infeBox.push_back('i'); infeBox.push_back('n'); infeBox.push_back('f'); infeBox.push_back('e');
        writeU32(infeBox, 2u << 24);
        writeU16(infeBox, static_cast<uint16_t>(newItemId));
        writeU16(infeBox, 0);
        infeBox.push_back('m'); infeBox.push_back('i'); infeBox.push_back('m'); infeBox.push_back('e');
        infeBox.push_back('\0');
        for (char c : mimeType) infeBox.push_back(static_cast<uint8_t>(c));
        infeBox.push_back('\0');
    }

    const size_t infeDelta = infeBox.size();

    // Prepare cdsc reference box: from newItemId to primaryItemId (14 bytes)
    // Box size: 14, type 'cdsc', from_id(2), ref_count(2)=1, to_id(2)
    std::vector<uint8_t> cdscBox;
    cdscBox.reserve(14);
    writeU32(cdscBox, 14);
    cdscBox.push_back('c'); cdscBox.push_back('d'); cdscBox.push_back('s'); cdscBox.push_back('c');
    writeU16(cdscBox, static_cast<uint16_t>(newItemId));
    writeU16(cdscBox, 1);
    writeU16(cdscBox, static_cast<uint16_t>(primaryItemId));

    // Build new iref box
    std::vector<uint8_t> newIrefBox;
    size_t irefDelta = 0;
    if (irefOffset > 0 && irefSize >= 12) {
        irefDelta = cdscBox.size();
        newIrefBox.reserve(irefSize + irefDelta);
        writeU32(newIrefBox, static_cast<uint32_t>(irefSize + irefDelta));
        newIrefBox.insert(newIrefBox.end(), src.data() + irefOffset + 4, src.data() + irefOffset + irefSize);
        newIrefBox.insert(newIrefBox.end(), cdscBox.begin(), cdscBox.end());
    } else {
        const uint32_t newIrefSize = static_cast<uint32_t>(12 + cdscBox.size());
        irefDelta = newIrefSize;
        newIrefBox.reserve(newIrefSize);
        writeU32(newIrefBox, newIrefSize);
        newIrefBox.push_back('i'); newIrefBox.push_back('r'); newIrefBox.push_back('e'); newIrefBox.push_back('f');
        writeU32(newIrefBox, 0); // FullBox ver=0, flags=0
        newIrefBox.insert(newIrefBox.end(), cdscBox.begin(), cdscBox.end());
    }

    // iloc growth calculation (retaining EXACT ilocVer, offSize, lenSize, baseOffSize, indexSize):
    const size_t newItemIlocSize = (ilocVer < 2 ? 2 : 4) +
                                   (ilocVer == 1 || ilocVer == 2 ? 2 : 0) +
                                   2 + baseOffSize + 2 +
                                   indexSize + offSize + lenSize;
    const size_t newIlocBoxSize = ilocSize + newItemIlocSize;
    const size_t ilocDelta = newItemIlocSize;

    const size_t metaGrowth = infeDelta + irefDelta + ilocDelta;

    // Determine payload offset in file
    uint64_t payloadAbsOffset = 0;
    const bool appendToMdat = (mdatPos > 0 && mdatSize >= 8);
    if (appendToMdat) {
        const size_t newMdatPos = mdatPos + metaGrowth;
        payloadAbsOffset = static_cast<uint64_t>(newMdatPos + mdatSize);
    } else {
        payloadAbsOffset = static_cast<uint64_t>(src.size() + metaGrowth + 8);
    }

    // Rebuild iloc box strictly preserving version and construction_method
    std::vector<uint8_t> newIlocBox;
    newIlocBox.reserve(newIlocBoxSize);
    writeU32(newIlocBox, static_cast<uint32_t>(newIlocBoxSize));
    newIlocBox.insert(newIlocBox.end(), src.data() + ilocOffset + 4, src.data() + ilocOffset + 14);

    if (ilocVer < 2) {
        writeU16(newIlocBox, static_cast<uint16_t>(existingItems.size() + 1));
    } else {
        writeU32(newIlocBox, static_cast<uint32_t>(existingItems.size() + 1));
    }

    for (const auto& it : existingItems) {
        if (ilocVer < 2) {
            writeU16(newIlocBox, static_cast<uint16_t>(it.id));
        } else {
            writeU32(newIlocBox, it.id);
        }
        if (ilocVer == 1 || ilocVer == 2) {
            writeU16(newIlocBox, it.constructionMethod);
        }
        writeU16(newIlocBox, it.dref);
        uint64_t curBase = it.baseOffset;
        if (offSize == 0 && it.constructionMethod == 0 && curBase >= metaEnd) {
            curBase += metaGrowth;
        }
        if (baseOffSize > 0) {
            writeVarInt(newIlocBox, curBase, baseOffSize);
        }
        writeU16(newIlocBox, static_cast<uint16_t>(it.extents.size()));
        for (const auto& ext : it.extents) {
            if (indexSize > 0) {
                writeVarInt(newIlocBox, ext.index, indexSize);
            }
            uint64_t off = ext.offset;
            // CRITICAL: Only extents with constructionMethod == 0 (file absolute offset)
            // located after metaEnd should be shifted! Extents with cm != 0 (such as grid
            // items with cm=1 relative to idat) MUST NEVER be shifted!
            if (offSize > 0 && it.constructionMethod == 0 && (it.baseOffset + off) >= metaEnd) {
                off += metaGrowth;
            }
            writeVarInt(newIlocBox, off, offSize);
            writeVarInt(newIlocBox, ext.length, lenSize);
        }
    }

    // Add new item (payload)
    if (ilocVer < 2) {
        writeU16(newIlocBox, static_cast<uint16_t>(newItemId));
    } else {
        writeU32(newIlocBox, newItemId);
    }
    if (ilocVer == 1 || ilocVer == 2) {
        writeU16(newIlocBox, 0); // cm = 0 (file offset)
    }
    writeU16(newIlocBox, 0); // dref = 0
    uint64_t newBaseOff = 0;
    uint64_t newExtentOff = payloadAbsOffset;
    if (offSize == 0) {
        newBaseOff = payloadAbsOffset;
        newExtentOff = 0;
    }
    if (baseOffSize > 0) {
        writeVarInt(newIlocBox, newBaseOff, baseOffSize);
    }
    writeU16(newIlocBox, 1); // 1 extent
    if (indexSize > 0) {
        writeVarInt(newIlocBox, 0, indexSize);
    }
    writeVarInt(newIlocBox, newExtentOff, offSize);
    writeVarInt(newIlocBox, payload.size(), lenSize);

    // Build new iinf box
    std::vector<uint8_t> newIinfBox;
    const size_t newIinfBoxSize = iinfSize + infeDelta;
    newIinfBox.reserve(newIinfBoxSize);
    writeU32(newIinfBox, static_cast<uint32_t>(newIinfBoxSize));
    newIinfBox.push_back('i'); newIinfBox.push_back('i'); newIinfBox.push_back('n'); newIinfBox.push_back('f');
    newIinfBox.insert(newIinfBox.end(), src.data() + iinfOffset + 8, src.data() + iinfOffset + 12);
    if (iinfVer == 0) {
        writeU16(newIinfBox, static_cast<uint16_t>(entryCount + 1));
        newIinfBox.insert(newIinfBox.end(), src.data() + iinfOffset + 14, src.data() + iinfOffset + iinfSize);
    } else {
        writeU32(newIinfBox, entryCount + 1);
        newIinfBox.insert(newIinfBox.end(), src.data() + iinfOffset + 16, src.data() + iinfOffset + iinfSize);
    }
    newIinfBox.insert(newIinfBox.end(), infeBox.begin(), infeBox.end());

    // Build new meta box
    std::vector<uint8_t> newMetaBox;
    const size_t newMetaBoxSize = metaSize + metaGrowth;
    newMetaBox.reserve(newMetaBoxSize);
    writeU32(newMetaBox, static_cast<uint32_t>(newMetaBoxSize));
    newMetaBox.push_back('m'); newMetaBox.push_back('e'); newMetaBox.push_back('t'); newMetaBox.push_back('a');
    newMetaBox.insert(newMetaBox.end(), src.data() + metaPos + metaHeaderSize, src.data() + metaPos + metaHeaderSize + 4);

    // Iterate through meta subboxes in order
    size_t scanSub = metaPayloadPos;
    bool hasIref = false;
    while (scanSub + 8 <= metaEnd) {
        const uint32_t curSize = readU32(src.data() + scanSub);
        if (curSize < 8 || scanSub + curSize > metaEnd) break;
        const uint8_t* curType = src.data() + scanSub + 4;

        if (std::memcmp(curType, "iloc", 4) == 0) {
            newMetaBox.insert(newMetaBox.end(), newIlocBox.begin(), newIlocBox.end());
        } else if (std::memcmp(curType, "iinf", 4) == 0) {
            newMetaBox.insert(newMetaBox.end(), newIinfBox.begin(), newIinfBox.end());
        } else if (std::memcmp(curType, "iref", 4) == 0) {
            newMetaBox.insert(newMetaBox.end(), newIrefBox.begin(), newIrefBox.end());
            hasIref = true;
        } else {
            newMetaBox.insert(newMetaBox.end(), src.data() + scanSub, src.data() + scanSub + curSize);
        }
        scanSub += curSize;
    }

    if (!hasIref) {
        newMetaBox.insert(newMetaBox.end(), newIrefBox.begin(), newIrefBox.end());
    }

    std::vector<uint8_t> finalData;
    if (appendToMdat) {
        finalData.reserve(src.size() + metaGrowth + payload.size());
        finalData.insert(finalData.end(), src.data(), src.data() + metaPos);
        finalData.insert(finalData.end(), newMetaBox.begin(), newMetaBox.end());
        finalData.insert(finalData.end(), src.data() + metaEnd, src.data() + mdatPos);

        // New mdat box: header with updated size + content + payload
        const uint32_t newMdatSize = static_cast<uint32_t>(mdatSize + payload.size());
        std::vector<uint8_t> mdatHdr;
        writeU32(mdatHdr, newMdatSize);
        mdatHdr.push_back('m'); mdatHdr.push_back('d'); mdatHdr.push_back('a'); mdatHdr.push_back('t');
        finalData.insert(finalData.end(), mdatHdr.begin(), mdatHdr.end());
        finalData.insert(finalData.end(), src.data() + mdatPos + 8, src.data() + mdatPos + mdatSize);
        finalData.insert(finalData.end(), payload.begin(), payload.end());
        finalData.insert(finalData.end(), src.data() + mdatPos + mdatSize, src.data() + src.size());
    } else {
        std::vector<uint8_t> freeBox;
        const uint32_t freeBoxSize = static_cast<uint32_t>(8 + payload.size());
        freeBox.reserve(freeBoxSize);
        writeU32(freeBox, freeBoxSize);
        freeBox.push_back('f'); freeBox.push_back('r'); freeBox.push_back('e'); freeBox.push_back('e');
        freeBox.insert(freeBox.end(), payload.begin(), payload.end());

        finalData.reserve(src.size() + metaGrowth + freeBox.size());
        finalData.insert(finalData.end(), src.data(), src.data() + metaPos);
        finalData.insert(finalData.end(), newMetaBox.begin(), newMetaBox.end());
        finalData.insert(finalData.end(), src.data() + metaEnd, src.data() + src.size());
        finalData.insert(finalData.end(), freeBox.begin(), freeBox.end());
    }

    const std::wstring tempPath = path + L".qvrating.tmp";
    HANDLE hTmp = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hTmp == INVALID_HANDLE_VALUE) return WriteStatus::Failed;

    DWORD written = 0;
    const BOOL ok = WriteFile(hTmp, finalData.data(), static_cast<DWORD>(finalData.size()), &written, nullptr);
    FlushFileBuffers(hTmp);
    CloseHandle(hTmp);

    if (!ok || written != finalData.size()) {
        DeleteFileW(tempPath.c_str());
        return WriteStatus::Failed;
    }

    BOOL replaceOk = FALSE;
    DWORD lastError = 0;
    for (int retry = 0; retry < 5; ++retry) {
        if (ReplaceFileW(path.c_str(), tempPath.c_str(), nullptr,
                         REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) {
            replaceOk = TRUE;
            break;
        }
        lastError = GetLastError();
        Sleep(20);
    }

    if (!replaceOk) {
        DeleteFileW(tempPath.c_str());
        if (lastError == ERROR_SHARING_VIOLATION || lastError == ERROR_LOCK_VIOLATION ||
            lastError == ERROR_ACCESS_DENIED) {
            return WriteStatus::NeedsTranscode;
        }
        return WriteStatus::Failed;
    }

    return WriteStatus::WrittenInPlace;
}

WriteStatus WriteRatingToJxl(const std::wstring& path, int stars, bool allowTranscode) {
    std::vector<uint8_t> src;
    {
        HANDLE file = INVALID_HANDLE_VALUE;
        for (int attempt = 0; attempt < 5; ++attempt) {
            file = CreateFileW(path.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) break;
            Sleep(15);
        }
        if (file == INVALID_HANDLE_VALUE) return WriteStatus::Failed;

        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file, &size) || size.QuadPart < 2 || size.QuadPart > 256 * 1024 * 1024) {
            CloseHandle(file);
            return WriteStatus::Failed;
        }
        src.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        const BOOL ok = ReadFile(file, src.data(), static_cast<DWORD>(src.size()), &read, nullptr);
        CloseHandle(file);
        if (!ok || read != src.size()) return WriteStatus::Failed;
    }

    auto readU32 = [](const uint8_t* b) -> uint32_t {
        return (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) |
               (static_cast<uint32_t>(b[2]) << 8) | b[3];
    };
    auto readU64 = [&readU32](const uint8_t* b) -> uint64_t {
        return (static_cast<uint64_t>(readU32(b)) << 32) | readU32(b + 4);
    };
    auto writeU32 = [](std::vector<uint8_t>& buf, uint32_t val) {
        buf.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
        buf.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
        buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
        buf.push_back(static_cast<uint8_t>(val & 0xFF));
    };
    auto writeU64 = [](std::vector<uint8_t>& buf, uint64_t val) {
        for (int i = 7; i >= 0; --i) {
            buf.push_back(static_cast<uint8_t>((val >> (i * 8)) & 0xFF));
        }
    };

    auto buildStandardExifPayload = [](int starVal) -> std::vector<uint8_t> {
        uint16_t pct = 0;
        switch (starVal) {
            case 1: pct = 1; break;
            case 2: pct = 25; break;
            case 3: pct = 50; break;
            case 4: pct = 75; break;
            case 5: pct = 99; break;
            default: pct = 0; break;
        }
        std::vector<uint8_t> p;
        p.reserve(42);
        // 4 bytes tiff offset = 0
        p.push_back(0); p.push_back(0); p.push_back(0); p.push_back(0);
        // TIFF header: "II", 42, IFD0 offset 8
        p.push_back('I'); p.push_back('I');
        p.push_back(0x2A); p.push_back(0x00);
        p.push_back(0x08); p.push_back(0x00); p.push_back(0x00); p.push_back(0x00);
        // IFD0 entries count: 2
        p.push_back(0x02); p.push_back(0x00);
        // tag 0x4746: SHORT, count 1, val = starVal
        p.push_back(0x46); p.push_back(0x47);
        p.push_back(0x03); p.push_back(0x00);
        p.push_back(0x01); p.push_back(0x00); p.push_back(0x00); p.push_back(0x00);
        p.push_back(static_cast<uint8_t>(starVal & 0xFF));
        p.push_back(static_cast<uint8_t>((starVal >> 8) & 0xFF));
        p.push_back(0x00); p.push_back(0x00);
        // tag 0x4749: SHORT, count 1, val = pct
        p.push_back(0x49); p.push_back(0x47);
        p.push_back(0x03); p.push_back(0x00);
        p.push_back(0x01); p.push_back(0x00); p.push_back(0x00); p.push_back(0x00);
        p.push_back(static_cast<uint8_t>(pct & 0xFF));
        p.push_back(static_cast<uint8_t>((pct >> 8) & 0xFF));
        p.push_back(0x00); p.push_back(0x00);
        // Next IFD: 0
        p.push_back(0x00); p.push_back(0x00); p.push_back(0x00); p.push_back(0x00);
        return p;
    };

    // 1. Bare Codestream check (0xFF 0x0A)
    if (src[0] == 0xFF && src[1] == 0x0A) {
        if (stars <= 0) {
            return WriteStatus::WrittenInPlace; // already unrated
        }
        if (!allowTranscode) {
            return WriteStatus::NeedsTranscode;
        }

        std::vector<uint8_t> container;
        container.reserve(src.size() + 4096);

        // Signature box (12 bytes)
        static constexpr uint8_t JXL_SIG[12] = {
            0x00, 0x00, 0x00, 0x0C, 'J', 'X', 'L', ' ', 0x0D, 0x0A, 0x87, 0x0A
        };
        container.insert(container.end(), JXL_SIG, JXL_SIG + 12);

        // ftyp box (20 bytes)
        static constexpr uint8_t JXL_FTYP[20] = {
            0x00, 0x00, 0x00, 0x14, 'f', 't', 'y', 'p',
            'j', 'x', 'l', ' ', 0x00, 0x00, 0x00, 0x00,
            'j', 'x', 'l', ' '
        };
        container.insert(container.end(), JXL_FTYP, JXL_FTYP + 20);

        // Exif box (50 bytes)
        const auto exifPayload = buildStandardExifPayload(stars);
        writeU32(container, static_cast<uint32_t>(8 + exifPayload.size()));
        container.push_back('E'); container.push_back('x'); container.push_back('i'); container.push_back('f');
        container.insert(container.end(), exifPayload.begin(), exifPayload.end());

        // xml box (XMP)
        const auto xmpPayload = BuildStandardXmpPayload(stars);
        writeU32(container, static_cast<uint32_t>(8 + xmpPayload.size()));
        container.push_back('x'); container.push_back('m'); container.push_back('l'); container.push_back(' ');
        container.insert(container.end(), xmpPayload.begin(), xmpPayload.end());

        // jxlc box
        if (src.size() + 8 <= 0xFFFFFFFF) {
            writeU32(container, static_cast<uint32_t>(src.size() + 8));
            container.push_back('j'); container.push_back('x'); container.push_back('l'); container.push_back('c');
        } else {
            writeU32(container, 1);
            container.push_back('j'); container.push_back('x'); container.push_back('l'); container.push_back('c');
            writeU64(container, static_cast<uint64_t>(src.size() + 16));
        }
        container.insert(container.end(), src.begin(), src.end());

        const std::wstring tempPath = path + L".tmp_jxl_rating";
        HANDLE hTemp = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hTemp == INVALID_HANDLE_VALUE) return WriteStatus::Failed;
        DWORD written = 0;
        BOOL writeOk = WriteFile(hTemp, container.data(), static_cast<DWORD>(container.size()), &written, nullptr);
        CloseHandle(hTemp);
        if (!writeOk || written != container.size()) {
            DeleteFileW(tempPath.c_str());
            return WriteStatus::Failed;
        }

        if (!AtomicReplaceFile(path, tempPath)) {
            return WriteStatus::Failed;
        }
        return WriteStatus::WrittenTranscode;
    }

    // 2. Container check
    static constexpr uint8_t JXL_SIG[12] = {
        0x00, 0x00, 0x00, 0x0C, 'J', 'X', 'L', ' ', 0x0D, 0x0A, 0x87, 0x0A
    };
    if (src.size() < 12 || std::memcmp(src.data(), JXL_SIG, 12) != 0) {
        return WriteStatus::Failed;
    }

    struct JxlBox {
        uint32_t type = 0;
        size_t boxPos = 0;
        size_t hdrSize = 8;
        uint64_t boxSize = 0;
        size_t payloadPos = 0;
        size_t payloadLen = 0;
    };
    std::vector<JxlBox> boxes;
    size_t pos = 12;

    while (pos + 8 <= src.size()) {
        uint64_t bSize = readU32(src.data() + pos);
        const uint32_t bType = readU32(src.data() + pos + 4);
        size_t hdrSize = 8;

        if (bSize == 1) {
            if (pos + 16 > src.size()) break;
            bSize = readU64(src.data() + pos + 8);
            hdrSize = 16;
        } else if (bSize == 0) {
            bSize = src.size() - pos;
        }

        if (bSize < hdrSize || pos + bSize > src.size()) break;

        JxlBox box{};
        box.type = bType;
        box.boxPos = pos;
        box.hdrSize = hdrSize;
        box.boxSize = bSize;
        box.payloadPos = pos + hdrSize;
        box.payloadLen = static_cast<size_t>(bSize - hdrSize);
        boxes.push_back(box);

        pos += static_cast<size_t>(bSize);
    }

    // Try in-place modification
    bool inPlaceSuccess = false;

    // Check for uncompressed Exif box ('Exif' = 0x45786966)
    for (const auto& box : boxes) {
        if (box.type == 0x45786966 && box.payloadLen > 4) {
            if (PatchIsobmffExifInPlace(path, src, box.payloadPos, box.payloadLen, stars)) {
                inPlaceSuccess = true;
                break;
            }
        }
    }

    // Check for uncompressed xml box ('xml ' = 0x786D6C20)
    for (const auto& box : boxes) {
        if (box.type == 0x786D6C20 && box.payloadLen > 0) {
            std::string_view xmpStr(reinterpret_cast<const char*>(src.data() + box.payloadPos), box.payloadLen);
            if (auto patched = PatchXmpRatingInPlaceStrict(xmpStr, stars)) {
                HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (hFile != INVALID_HANDLE_VALUE) {
                    LARGE_INTEGER li;
                    li.QuadPart = static_cast<LONGLONG>(box.payloadPos);
                    SetFilePointerEx(hFile, li, nullptr, FILE_BEGIN);
                    DWORD written = 0;
                    WriteFile(hFile, patched->data(), static_cast<DWORD>(patched->size()), &written, nullptr);
                    FlushFileBuffers(hFile);
                    CloseHandle(hFile);
                    inPlaceSuccess = true;
                    break;
                }
            }
        }
    }

    if (inPlaceSuccess) {
        return WriteStatus::WrittenInPlace;
    }

    // If clearing (0 stars) and no in-place succeeded, nothing left to do
    if (stars <= 0) {
        return WriteStatus::WrittenInPlace;
    }

    // In-place failed: rebuild container by injecting new Exif and XMP boxes
    if (!allowTranscode) {
        return WriteStatus::NeedsTranscode;
    }

    std::vector<uint8_t> newContainer;
    newContainer.reserve(src.size() + 4096);

    // Write signature box
    newContainer.insert(newContainer.end(), JXL_SIG, JXL_SIG + 12);

    bool insertedMetadata = false;

    for (const auto& box : boxes) {
        // Skip old Exif, xml, or brob boxes carrying metadata
        if (box.type == 0x45786966 || box.type == 0x786D6C20) {
            continue;
        }
        if (box.type == 0x62726F62 && box.payloadLen >= 4) { // 'brob'
            const uint32_t origType = readU32(src.data() + box.payloadPos);
            if (origType == 0x45786966 || origType == 0x786D6C20) {
                continue; // Skip compressed Exif/xml brob
            }
        }

        // Copy this box
        newContainer.insert(newContainer.end(), src.data() + box.boxPos, src.data() + box.boxPos + static_cast<size_t>(box.boxSize));

        // Inject new Exif & xml right after ftyp ('ftyp' = 0x66747970)
        if (!insertedMetadata && box.type == 0x66747970) {
            // 1. Exif box
            const auto exifPayload = buildStandardExifPayload(stars);
            writeU32(newContainer, static_cast<uint32_t>(8 + exifPayload.size()));
            newContainer.push_back('E'); newContainer.push_back('x'); newContainer.push_back('i'); newContainer.push_back('f');
            newContainer.insert(newContainer.end(), exifPayload.begin(), exifPayload.end());

            // 2. xml box
            const auto xmpPayload = BuildStandardXmpPayload(stars);
            writeU32(newContainer, static_cast<uint32_t>(8 + xmpPayload.size()));
            newContainer.push_back('x'); newContainer.push_back('m'); newContainer.push_back('l'); newContainer.push_back(' ');
            newContainer.insert(newContainer.end(), xmpPayload.begin(), xmpPayload.end());

            insertedMetadata = true;
        }
    }

    // Fallback: if no ftyp was present (rare), inject metadata now
    if (!insertedMetadata) {
        const auto exifPayload = buildStandardExifPayload(stars);
        writeU32(newContainer, static_cast<uint32_t>(8 + exifPayload.size()));
        newContainer.push_back('E'); newContainer.push_back('x'); newContainer.push_back('i'); newContainer.push_back('f');
        newContainer.insert(newContainer.end(), exifPayload.begin(), exifPayload.end());

        const auto xmpPayload = BuildStandardXmpPayload(stars);
        writeU32(newContainer, static_cast<uint32_t>(8 + xmpPayload.size()));
        newContainer.push_back('x'); newContainer.push_back('m'); newContainer.push_back('l'); newContainer.push_back(' ');
        newContainer.insert(newContainer.end(), xmpPayload.begin(), xmpPayload.end());
    }

    const std::wstring tempPath = path + L".tmp_jxl_rebuild";
    HANDLE hTemp = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hTemp == INVALID_HANDLE_VALUE) return WriteStatus::Failed;
    DWORD written = 0;
    BOOL writeOk = WriteFile(hTemp, newContainer.data(), static_cast<DWORD>(newContainer.size()), &written, nullptr);
    CloseHandle(hTemp);
    if (!writeOk || written != newContainer.size()) {
        DeleteFileW(tempPath.c_str());
        return WriteStatus::Failed;
    }

    if (!AtomicReplaceFile(path, tempPath)) {
        return WriteStatus::Failed;
    }
    return WriteStatus::WrittenTranscode;
}

// ============================================================================
// Lossless native PNG / APNG Chunk Writer (O(1) in-place + 64KB streaming)
// ============================================================================

constexpr uint8_t PNG_SIGNATURE[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
constexpr std::string_view PNG_XMP_KEYWORD = "XML:com.adobe.xmp";
constexpr uint32_t PNG_MAX_METADATA_CHUNK_BYTES = 16u * 1024u * 1024u;

bool SeekFileTo(HANDLE hFile, uint64_t offset) {
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(offset);
    return SetFilePointerEx(hFile, li, nullptr, FILE_BEGIN) != FALSE;
}

bool ReadExactBytes(HANDLE hFile, uint8_t* dst, size_t bytes) {
    size_t offset = 0;
    while (offset < bytes) {
        const DWORD toRead = static_cast<DWORD>(std::min<size_t>(bytes - offset, 0x40000000u));
        DWORD read = 0;
        if (!ReadFile(hFile, dst + offset, toRead, &read, nullptr) || read == 0) {
            return false;
        }
        offset += read;
    }
    return true;
}

bool WriteExactBytes(HANDLE hFile, const uint8_t* src, size_t bytes) {
    size_t offset = 0;
    while (offset < bytes) {
        const DWORD toWrite = static_cast<DWORD>(std::min<size_t>(bytes - offset, 0x40000000u));
        DWORD written = 0;
        if (!WriteFile(hFile, src + offset, toWrite, &written, nullptr) || written != toWrite) {
            return false;
        }
        offset += written;
    }
    return true;
}

bool CopyFileRangeChunked(HANDLE hSrc, HANDLE hDst, uint64_t startOffset, uint64_t endOffset,
                          std::vector<uint8_t>& ioBuf) {
    if (endOffset < startOffset) return false;
    if (endOffset == startOffset) return true;
    if (!SeekFileTo(hSrc, startOffset)) return false;

    uint64_t remaining = endOffset - startOffset;
    while (remaining > 0) {
        const DWORD chunk = static_cast<DWORD>(std::min<uint64_t>(remaining, ioBuf.size()));
        if (!ReadExactBytes(hSrc, ioBuf.data(), chunk)) return false;
        if (!WriteExactBytes(hDst, ioBuf.data(), chunk)) return false;
        remaining -= chunk;
    }
    return true;
}

uint32_t ComputePngChunkCrc(const uint8_t type[4], std::span<const uint8_t> data) {
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, type, 4);
    size_t offset = 0;
    while (offset < data.size()) {
        const uInt step = static_cast<uInt>(std::min<size_t>(data.size() - offset, 0x40000000u));
        crc = crc32(crc, data.data() + offset, step);
        offset += step;
    }
    return static_cast<uint32_t>(crc);
}

std::vector<uint8_t> BuildPngChunkBytes(const uint8_t type[4], std::span<const uint8_t> data) {
    std::vector<uint8_t> out;
    out.reserve(12 + data.size());
    AppendBe32(out, static_cast<uint32_t>(data.size()));
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    AppendBe32(out, ComputePngChunkCrc(type, data));
    return out;
}

std::vector<uint8_t> BuildPngITXtXmpChunk(int stars) {
    const std::string xmp = BuildStandardXmpPayload(stars);
    std::vector<uint8_t> payload;
    payload.reserve(PNG_XMP_KEYWORD.size() + 5 + xmp.size());
    payload.insert(payload.end(), PNG_XMP_KEYWORD.begin(), PNG_XMP_KEYWORD.end());
    payload.push_back(0); // keyword null-terminator
    payload.push_back(0); // compression flag = uncompressed
    payload.push_back(0); // compression method = 0
    payload.push_back(0); // language tag null-terminator
    payload.push_back(0); // translated keyword null-terminator
    payload.insert(payload.end(), xmp.begin(), xmp.end());
    static constexpr uint8_t ITXT_TYPE[4] = { 'i', 'T', 'X', 't' };
    return BuildPngChunkBytes(ITXT_TYPE, payload);
}

enum class PngITXtKind { NotXmp, UncompressedXmp, CompressedOrMalformedXmp };

PngITXtKind InspectPngITXt(std::span<const uint8_t> data, size_t& outXmpOffset) {
    if (data.size() <= PNG_XMP_KEYWORD.size() ||
        std::memcmp(data.data(), PNG_XMP_KEYWORD.data(), PNG_XMP_KEYWORD.size()) != 0 ||
        data[PNG_XMP_KEYWORD.size()] != 0) {
        return PngITXtKind::NotXmp;
    }
    const size_t flagPos = PNG_XMP_KEYWORD.size() + 1;
    if (flagPos + 4 > data.size() || data[flagPos] != 0) {
        return PngITXtKind::CompressedOrMalformedXmp;
    }
    size_t pos = flagPos + 2;
    for (int i = 0; i < 2; ++i) {
        const auto* nul = static_cast<const uint8_t*>(std::memchr(data.data() + pos, 0, data.size() - pos));
        if (!nul) return PngITXtKind::CompressedOrMalformedXmp;
        pos = static_cast<size_t>(nul - data.data()) + 1;
    }
    outXmpOffset = pos;
    return PngITXtKind::UncompressedXmp;
}

bool IsPngZTxtXmp(std::span<const uint8_t> data) {
    return data.size() > PNG_XMP_KEYWORD.size() &&
           std::memcmp(data.data(), PNG_XMP_KEYWORD.data(), PNG_XMP_KEYWORD.size()) == 0 &&
           data[PNG_XMP_KEYWORD.size()] == 0;
}

struct PngXmpChunkRecord {
    uint64_t chunkOffset = 0;
    uint64_t totalSize = 0;
    bool uncompressedITXt = false;
    size_t xmpOffsetInData = 0;
    std::vector<uint8_t> data;
};

struct PngExifChunkRecord {
    uint64_t chunkOffset = 0;
    uint64_t totalSize = 0;
    std::vector<uint8_t> data;
    bool hasPatchedRating = false;
};

struct PngPreIdatScan {
    uint64_t fileSize = 0;
    uint64_t insertOffset = 0;
    bool foundImageOrEndBoundary = false;
    std::vector<PngXmpChunkRecord> xmpChunks;
    std::vector<PngExifChunkRecord> exifChunks;
};

bool ScanPngPreIdat(HANDLE hFile, int targetStars, PngPreIdatScan& out) {
    LARGE_INTEGER sizeLi{};
    if (!GetFileSizeEx(hFile, &sizeLi) || sizeLi.QuadPart < 20) return false;
    out.fileSize = static_cast<uint64_t>(sizeLi.QuadPart);

    if (!SeekFileTo(hFile, 0)) return false;
    uint8_t sig[8]{};
    if (!ReadExactBytes(hFile, sig, 8) || std::memcmp(sig, PNG_SIGNATURE, 8) != 0) {
        return false;
    }

    uint64_t pos = 8;
    bool insertAnchorLocked = false;

    while (pos + 12 <= out.fileSize) {
        if (!SeekFileTo(hFile, pos)) return false;
        uint8_t header[8]{};
        if (!ReadExactBytes(hFile, header, 8)) return false;

        const uint32_t chunkLen = ReadBe32(header);
        if (chunkLen > 0x7FFFFFFFu) return false; // PNG spec forbids bit 31 set
        const uint64_t totalSize = 12ull + chunkLen;
        if (totalSize > out.fileSize - pos) return false;

        if (pos == 8 && (std::memcmp(header + 4, "IHDR", 4) != 0 || chunkLen < 13)) {
            return false;
        }

        const bool isActl = (std::memcmp(header + 4, "acTL", 4) == 0);
        const bool isFctl = (std::memcmp(header + 4, "fcTL", 4) == 0);
        const bool isIdat = (std::memcmp(header + 4, "IDAT", 4) == 0);
        const bool isIend = (std::memcmp(header + 4, "IEND", 4) == 0);

        if (!insertAnchorLocked && (isActl || isFctl || isIdat || isIend)) {
            out.insertOffset = pos;
            insertAnchorLocked = true;
        }

        if (isIdat || isIend) {
            out.foundImageOrEndBoundary = true;
            break;
        }

        const bool isITXt = (std::memcmp(header + 4, "iTXt", 4) == 0);
        const bool isZTxt = (std::memcmp(header + 4, "zTXt", 4) == 0);
        const bool isExif = (std::memcmp(header + 4, "eXIf", 4) == 0);

        if ((isITXt || isZTxt || isExif) && chunkLen > 0) {
            if (chunkLen > PNG_MAX_METADATA_CHUNK_BYTES) return false;
            std::vector<uint8_t> data(chunkLen);
            if (!ReadExactBytes(hFile, data.data(), chunkLen)) return false;

            if (isITXt) {
                size_t xmpOff = 0;
                const PngITXtKind kind = InspectPngITXt(data, xmpOff);
                if (kind != PngITXtKind::NotXmp) {
                    PngXmpChunkRecord rec;
                    rec.chunkOffset = pos;
                    rec.totalSize = totalSize;
                    rec.uncompressedITXt = (kind == PngITXtKind::UncompressedXmp);
                    rec.xmpOffsetInData = xmpOff;
                    rec.data = std::move(data);
                    out.xmpChunks.push_back(std::move(rec));
                }
            } else if (isZTxt) {
                if (IsPngZTxtXmp(data)) {
                    PngXmpChunkRecord rec;
                    rec.chunkOffset = pos;
                    rec.totalSize = totalSize;
                    rec.uncompressedITXt = false;
                    out.xmpChunks.push_back(std::move(rec));
                }
            } else if (isExif) {
                PngExifChunkRecord rec;
                rec.chunkOffset = pos;
                rec.totalSize = totalSize;
                rec.data = std::move(data);
                rec.hasPatchedRating = PatchTiffBufferRating(rec.data, targetStars);
                out.exifChunks.push_back(std::move(rec));
            }
        }

        pos += totalSize;
    }

    return out.foundImageOrEndBoundary && insertAnchorLocked;
}

struct PngInPlaceWriteOp {
    uint64_t dataFileOffset = 0;
    const uint8_t* dataBytes = nullptr;
    size_t dataSize = 0;
    uint64_t crcFileOffset = 0;
    uint32_t newCrc = 0;
};

bool TryPngInPlaceUpdate(const std::wstring& path, PngPreIdatScan& scan, int targetStars,
                         bool& outFileLocked) {
    outFileLocked = false;

    bool anyExifHasRating = false;
    for (const auto& exif : scan.exifChunks) {
        if (exif.hasPatchedRating) anyExifHasRating = true;
    }

    // Cannot safely patch compressed or duplicate XMP chunks in place.
    if (scan.xmpChunks.size() > 1) return false;
    if (scan.xmpChunks.size() == 1 && !scan.xmpChunks[0].uncompressedITXt) return false;

    // Nothing in file carries a rating yet: clearing is a no-op, setting needs injection.
    if (scan.xmpChunks.empty() && !anyExifHasRating) {
        return targetStars <= 0;
    }

    std::vector<PngInPlaceWriteOp> ops;

    if (scan.xmpChunks.size() == 1) {
        auto& xmpChunk = scan.xmpChunks[0];
        const std::string_view oldXmp(
            reinterpret_cast<const char*>(xmpChunk.data.data() + xmpChunk.xmpOffsetInData),
            xmpChunk.data.size() - xmpChunk.xmpOffsetInData);
        auto patched = PatchXmpRatingInPlaceStrict(oldXmp, targetStars);
        if (!patched || patched->size() != oldXmp.size()) {
            return false;
        }
        std::memcpy(xmpChunk.data.data() + xmpChunk.xmpOffsetInData, patched->data(), patched->size());
        static constexpr uint8_t ITXT_TYPE[4] = { 'i', 'T', 'X', 't' };
        PngInPlaceWriteOp op;
        op.dataFileOffset = xmpChunk.chunkOffset + 8 + xmpChunk.xmpOffsetInData;
        op.dataBytes = xmpChunk.data.data() + xmpChunk.xmpOffsetInData;
        op.dataSize = patched->size();
        op.crcFileOffset = xmpChunk.chunkOffset + 8 + xmpChunk.data.size();
        op.newCrc = ComputePngChunkCrc(ITXT_TYPE, xmpChunk.data);
        ops.push_back(op);
    }

    for (const auto& exif : scan.exifChunks) {
        if (!exif.hasPatchedRating) continue;
        static constexpr uint8_t EXIF_TYPE[4] = { 'e', 'X', 'I', 'f' };
        PngInPlaceWriteOp op;
        op.dataFileOffset = exif.chunkOffset + 8;
        op.dataBytes = exif.data.data();
        op.dataSize = exif.data.size();
        op.crcFileOffset = exif.chunkOffset + 8 + exif.data.size();
        op.newCrc = ComputePngChunkCrc(EXIF_TYPE, exif.data);
        ops.push_back(op);
    }

    if (ops.empty()) return true;

    HANDLE hWrite = INVALID_HANDLE_VALUE;
    DWORD lastErr = 0;
    for (int attempt = 0; attempt < 5; ++attempt) {
        hWrite = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hWrite != INVALID_HANDLE_VALUE) break;
        lastErr = GetLastError();
        if (attempt + 1 < 5) Sleep(15);
    }
    if (hWrite == INVALID_HANDLE_VALUE) {
        outFileLocked = IsFileLockError(lastErr);
        return false;
    }

    bool ok = true;
    for (const auto& op : ops) {
        if (!SeekFileTo(hWrite, op.dataFileOffset) ||
            !WriteExactBytes(hWrite, op.dataBytes, op.dataSize)) {
            ok = false;
            break;
        }
        uint8_t crcBe[4]{};
        crcBe[0] = static_cast<uint8_t>((op.newCrc >> 24) & 0xFF);
        crcBe[1] = static_cast<uint8_t>((op.newCrc >> 16) & 0xFF);
        crcBe[2] = static_cast<uint8_t>((op.newCrc >> 8) & 0xFF);
        crcBe[3] = static_cast<uint8_t>(op.newCrc & 0xFF);
        if (!SeekFileTo(hWrite, op.crcFileOffset) || !WriteExactBytes(hWrite, crcBe, 4)) {
            ok = false;
            break;
        }
    }

    if (ok) FlushFileBuffers(hWrite);
    CloseHandle(hWrite);
    return ok;
}

WriteStatus WriteRatingToPng(const std::wstring& path, int stars, bool allowTranscode) {
    const int targetStars = std::clamp(stars, 0, 5);

    DWORD openErr = 0;
    HANDLE hSrc = OpenFileForReadRetry(path, &openErr);
    if (hSrc == INVALID_HANDLE_VALUE) {
        return IsFileLockError(openErr) ? WriteStatus::NeedsTranscode : WriteStatus::Failed;
    }

    PngPreIdatScan scan;
    const bool validPng = ScanPngPreIdat(hSrc, targetStars, scan);
    if (!validPng) {
        CloseHandle(hSrc);
        return WriteStatus::Failed;
    }

    bool inPlaceLocked = false;
    if (TryPngInPlaceUpdate(path, scan, targetStars, inPlaceLocked)) {
        CloseHandle(hSrc);
        return WriteStatus::WrittenInPlace;
    }
    if (inPlaceLocked || !allowTranscode) {
        CloseHandle(hSrc);
        return WriteStatus::NeedsTranscode;
    }

    struct ChunkReplacement {
        uint64_t offset = 0;
        uint64_t totalSize = 0;
        std::vector<uint8_t> bytes;
    };

    std::vector<ChunkReplacement> replacements;
    replacements.reserve(scan.xmpChunks.size() + scan.exifChunks.size());
    for (const auto& xmp : scan.xmpChunks) {
        replacements.push_back(ChunkReplacement{ xmp.chunkOffset, xmp.totalSize, {} });
    }
    for (const auto& exif : scan.exifChunks) {
        if (!exif.hasPatchedRating) continue;
        static constexpr uint8_t EXIF_TYPE[4] = { 'e', 'X', 'I', 'f' };
        replacements.push_back(ChunkReplacement{
            exif.chunkOffset, exif.totalSize, BuildPngChunkBytes(EXIF_TYPE, exif.data)
        });
    }
    std::sort(replacements.begin(), replacements.end(),
              [](const ChunkReplacement& a, const ChunkReplacement& b) {
                  return a.offset < b.offset;
              });

    const std::vector<uint8_t> newITXtChunk =
        (targetStars > 0) ? BuildPngITXtXmpChunk(targetStars) : std::vector<uint8_t>{};

    const std::wstring tempPath = path + L".tmp_png_rating";
    HANDLE hDst = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hDst == INVALID_HANDLE_VALUE) {
        const DWORD createErr = GetLastError();
        CloseHandle(hSrc);
        return IsFileLockError(createErr) ? WriteStatus::NeedsTranscode : WriteStatus::Failed;
    }

    std::vector<uint8_t> ioBuf(64u * 1024u);
    bool streamOk = true;
    uint64_t cursor = 0;
    bool insertedNewXmp = newITXtChunk.empty();

    for (const auto& rep : replacements) {
        if (!insertedNewXmp && scan.insertOffset <= rep.offset) {
            if (!CopyFileRangeChunked(hSrc, hDst, cursor, scan.insertOffset, ioBuf) ||
                !WriteExactBytes(hDst, newITXtChunk.data(), newITXtChunk.size())) {
                streamOk = false;
                break;
            }
            cursor = scan.insertOffset;
            insertedNewXmp = true;
        }
        if (!CopyFileRangeChunked(hSrc, hDst, cursor, rep.offset, ioBuf) ||
            (!rep.bytes.empty() && !WriteExactBytes(hDst, rep.bytes.data(), rep.bytes.size()))) {
            streamOk = false;
            break;
        }
        cursor = rep.offset + rep.totalSize;
    }

    if (streamOk && !insertedNewXmp) {
        if (!CopyFileRangeChunked(hSrc, hDst, cursor, scan.insertOffset, ioBuf) ||
            !WriteExactBytes(hDst, newITXtChunk.data(), newITXtChunk.size())) {
            streamOk = false;
        } else {
            cursor = scan.insertOffset;
        }
    }

    if (streamOk && !CopyFileRangeChunked(hSrc, hDst, cursor, scan.fileSize, ioBuf)) {
        streamOk = false;
    }

    if (streamOk) FlushFileBuffers(hDst);
    CloseHandle(hDst);
    CloseHandle(hSrc);

    if (!streamOk) {
        DeleteFileW(tempPath.c_str());
        return WriteStatus::Failed;
    }

    DWORD replaceErr = 0;
    if (!AtomicReplaceFile(path, tempPath, &replaceErr)) {
        return IsFileLockError(replaceErr) ? WriteStatus::NeedsTranscode : WriteStatus::Failed;
    }
    return WriteStatus::WrittenTranscode;
}

} // namespace


WriteStatus WriteRatingToImage(const std::wstring& path, int stars, bool allowTranscode) {
    if (path.empty()) return WriteStatus::Failed;

    const std::wstring_view ext = QuickView::ExtensionOf(path);
    const bool isJxl = QuickView::ExtEqualsIgnoreCase(ext, L".jxl");
    if (isJxl) {
        return WriteRatingToJxl(path, stars, allowTranscode);
    }

    const bool isPng = QuickView::ExtEqualsIgnoreCase(ext, L".png") ||
                       QuickView::ExtEqualsIgnoreCase(ext, L".apng");
    if (isPng) {
        return WriteRatingToPng(path, stars, allowTranscode);
    }

    const bool isWebp = QuickView::ExtEqualsIgnoreCase(ext, L".webp");
    if (isWebp) {
        if (!allowTranscode) return WriteStatus::NeedsTranscode;
        return WriteRatingToWebp(path, stars);
    }

    const bool isIsobmff = QuickView::ExtEqualsIgnoreCase(ext, L".avif") ||
                           QuickView::ExtEqualsIgnoreCase(ext, L".avifs") ||
                           QuickView::ExtEqualsIgnoreCase(ext, L".heic") ||
                           QuickView::ExtEqualsIgnoreCase(ext, L".heif");
    if (isIsobmff) {
        if (!allowTranscode) return WriteStatus::NeedsTranscode;
        return WriteRatingToIsobmff(path, stars);
    }

    const bool isTiff = QuickView::ExtEqualsIgnoreCase(ext, L".tif") ||
                        QuickView::ExtEqualsIgnoreCase(ext, L".tiff");

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory)))) {
        return WriteStatus::Failed;
    }

    // WIC TIFF decoder does NOT support FastMetadataEncoder (returns WINCODEC_ERR_UNSUPPORTEDOPERATION).
    // Bypassing TryInPlace avoids open handle collisions during subsequent Transcode.
    if (!isTiff) {
        const HRESULT hrInPlace = TryInPlace(factory.Get(), path, stars);
        if (SUCCEEDED(hrInPlace)) {
            return WriteStatus::WrittenInPlace;
        }
    }

    if (!allowTranscode) return WriteStatus::NeedsTranscode;

    const HRESULT hrTrans = Transcode(factory.Get(), path, stars);
    return SUCCEEDED(hrTrans) ? WriteStatus::WrittenTranscode : WriteStatus::Failed;
}

} // namespace QuickView::Rating

