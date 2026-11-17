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
#include <atomic>
#include <cstring>
#include <span>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace QuickView::Rating {

namespace {

// A JPEG nests Exif under APP1; a TIFF carries the IFD directly.
struct RatingQueries {
    const wchar_t* exif;
    const wchar_t* xmp;
};

RatingQueries QueriesFor(const std::wstring& path) {
    const std::wstring_view ext = QuickView::ExtensionOf(path);
    const bool isTiff = QuickView::ExtEqualsIgnoreCase(ext, L".tif") ||
                        QuickView::ExtEqualsIgnoreCase(ext, L".tiff");
    if (isTiff) {
        return RatingQueries{ L"/ifd/{ushort=18246}", L"/ifd/xmp/xmp:Rating" };
    }
    return RatingQueries{ L"/app1/ifd/{ushort=18246}", L"/xmp/xmp:Rating" };
}

// Writing SimpleRating (0..5) is enough for both ecosystems: Explorer and
// Photos derive System.Rating (0..99) from it, and Adobe reads xmp:Rating.
HRESULT ApplyRating(IWICMetadataQueryWriter* writer, const RatingQueries& q, int stars) {
    if (!writer) return E_POINTER;
    const int clampedStars = std::clamp(stars, 0, 5);

    if (clampedStars <= 0) {
        // Clearing means removing the property, not storing a zero, so the
        // file goes back to carrying no rating at all. A property that was
        // not there is not an error.
        if (q.exif) writer->RemoveMetadataByName(q.exif);
        if (q.xmp) writer->RemoveMetadataByName(q.xmp);
        return S_OK;
    }

    HRESULT hrExif = E_FAIL;
    if (q.exif) {
        PROPVARIANT var;
        PropVariantInit(&var);
        var.vt = VT_UI2;
        var.uiVal = static_cast<USHORT>(clampedStars);
        hrExif = writer->SetMetadataByName(q.exif, &var);
        PropVariantClear(&var);
    }

    HRESULT hrXmp = E_FAIL;
    const std::wstring text = std::to_wstring(clampedStars);
    if (q.xmp) {
        PROPVARIANT var;
        PropVariantInit(&var);
        var.vt = VT_LPWSTR;
        var.pwszVal = static_cast<WCHAR*>(CoTaskMemAlloc((text.size() + 1) * sizeof(WCHAR)));
        if (var.pwszVal) {
            wcscpy_s(var.pwszVal, text.size() + 1, text.c_str());
            hrXmp = writer->SetMetadataByName(q.xmp, &var);
            PropVariantClear(&var);
        } else {
            hrXmp = E_OUTOFMEMORY;
        }
    }

    // An XMP packet or Exif tag satisfies Explorer / Lightroom; succeed if either landed.
    if (SUCCEEDED(hrExif) || SUCCEEDED(hrXmp)) {
        return S_OK;
    }
    return FAILED(hrExif) ? hrExif : hrXmp;
}

std::wstring CreateUniqueTempPath(const std::wstring& targetPath, const wchar_t* tag) {
    static std::atomic<uint64_t> s_seq{ 0 };
    return targetPath + L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
           std::to_wstring(s_seq.fetch_add(1, std::memory_order_relaxed)) + tag;
}

bool IsFileLockError(DWORD err) {
    return err == ERROR_SHARING_VIOLATION ||
           err == ERROR_LOCK_VIOLATION ||
           err == ERROR_UNABLE_TO_REMOVE_REPLACED ||
           err == ERROR_UNABLE_TO_MOVE_REPLACEMENT;
}

HANDLE OpenFileForReadRetry(const std::wstring& path, DWORD* outLastErr = nullptr) {
    DWORD lastErr = 0;
    for (int attempt = 0; attempt < 8; ++attempt) {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file != INVALID_HANDLE_VALUE) return file;
        lastErr = GetLastError();
        if (!IsFileLockError(lastErr) && lastErr != ERROR_ACCESS_DENIED) break;
        if (attempt + 1 < 8) Sleep(15);
    }
    if (outLastErr) *outLastErr = lastErr;
    return INVALID_HANDLE_VALUE;
}

bool ReadWholeFile(const std::wstring& path, uint64_t minBytes, uint64_t maxBytes,
                   std::vector<uint8_t>& out, DWORD* outErr = nullptr) {
    out.clear();
    DWORD openErr = 0;
    HANDLE file = OpenFileForReadRetry(path, &openErr);
    if (file == INVALID_HANDLE_VALUE) {
        if (outErr) *outErr = openErr;
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < static_cast<LONGLONG>(minBytes) ||
        static_cast<uint64_t>(size.QuadPart) > maxBytes) {
        if (outErr) *outErr = GetLastError();
        CloseHandle(file);
        return false;
    }

    const size_t total = static_cast<size_t>(size.QuadPart);
    out.resize(total);
    size_t off = 0;
    while (off < total) {
        const DWORD toRead = static_cast<DWORD>((std::min)(total - off, static_cast<size_t>(0x40000000u)));
        DWORD read = 0;
        if (!ReadFile(file, out.data() + off, toRead, &read, nullptr) || read == 0) {
            if (outErr) *outErr = GetLastError();
            CloseHandle(file);
            out.clear();
            return false;
        }
        off += read;
    }
    CloseHandle(file);
    return true;
}

bool AtomicReplaceFile(const std::wstring& targetPath, const std::wstring& tempPath,
                       DWORD* outLastErr = nullptr) {
    DWORD lastErr = 0;
    for (int retry = 0; retry < 8; ++retry) {
        if (ReplaceFileW(targetPath.c_str(), tempPath.c_str(), nullptr,
                         REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) {
            return true;
        }
        lastErr = GetLastError();
        if (!IsFileLockError(lastErr) && lastErr != ERROR_ACCESS_DENIED) break;
        if (retry + 1 < 8) Sleep(20);
    }
    DeleteFileW(tempPath.c_str());
    if (outLastErr) *outLastErr = lastErr;
    return false;
}

bool WriteBufferAndReplace(const std::wstring& targetPath, const wchar_t* tag,
                           std::span<const uint8_t> data, WriteStatus& outStatus) {
    const std::wstring tempPath = CreateUniqueTempPath(targetPath, tag);
    DWORD createErr = 0;
    HANDLE hTemp = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hTemp == INVALID_HANDLE_VALUE) {
        createErr = GetLastError();
        outStatus = IsFileLockError(createErr) ? WriteStatus::NeedsTranscode : WriteStatus::Failed;
        return false;
    }

    size_t off = 0;
    bool writeOk = true;
    while (off < data.size()) {
        const DWORD step = static_cast<DWORD>((std::min)(data.size() - off, static_cast<size_t>(0x40000000u)));
        DWORD written = 0;
        if (!WriteFile(hTemp, data.data() + off, step, &written, nullptr) || written != step) {
            writeOk = false;
            break;
        }
        off += written;
    }
    if (writeOk) FlushFileBuffers(hTemp);
    CloseHandle(hTemp);

    if (!writeOk) {
        DeleteFileW(tempPath.c_str());
        outStatus = WriteStatus::Failed;
        return false;
    }

    DWORD replaceErr = 0;
    if (!AtomicReplaceFile(targetPath, tempPath, &replaceErr)) {
        outStatus = (IsFileLockError(replaceErr) || replaceErr == ERROR_ACCESS_DENIED)
            ? WriteStatus::NeedsTranscode
            : WriteStatus::Failed;
        return false;
    }
    outStatus = WriteStatus::WrittenTranscode;
    return true;
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
    const int clamped = std::clamp(starVal, 0, 5);
    std::string s =
        "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
        "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n"
        " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
        "  <rdf:Description rdf:about=\"\"\n"
        "    xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\"\n";
    if (clamped > 0) {
        s += "    xmp:Rating=\"" + std::to_string(clamped) + "\"/>\n";
    } else {
        s += "   />\n";
    }
    s += " </rdf:RDF>\n"
         "</x:xmpmeta>\n";
    s.append(2048, ' ');
    s += "\n<?xpacket end=\"w\"?>";
    return s;
}

// Update an existing XMP packet preserving all non-rating fields and ensuring
// enough whitespace padding for future O(1) in-place edits.
std::optional<std::string> BuildPaddedUpdatedXmp(std::string_view oldXmp, int stars) {
    const int clamped = std::clamp(stars, 0, 5);
    auto updated = UpdateXmpRating(oldXmp, clamped);
    if (!updated) return std::nullopt;

    constexpr std::string_view PACKET_END = "<?xpacket end=";
    const size_t endPos = updated->rfind(PACKET_END);
    if (endPos != std::string_view::npos) {
        size_t wsStart = endPos;
        while (wsStart > 0 && ((*updated)[wsStart - 1] == ' ' || (*updated)[wsStart - 1] == '\t' ||
                               (*updated)[wsStart - 1] == '\r' || (*updated)[wsStart - 1] == '\n')) {
            --wsStart;
        }
        const size_t curWs = endPos - wsStart;
        if (curWs < 512) {
            updated->insert(wsStart, "\n" + std::string(2048 - curWs, ' ') + "\n");
        }
    } else {
        updated->append("\n");
        updated->append(2048, ' ');
        updated->append("\n<?xpacket end=\"w\"?>");
    }
    return updated;
}

// Standard 42-byte ISOBMFF/JXL Exif payload: 4-byte TIFF offset (0) + LE TIFF header + IFD0 (0x4746 + 0x4749).
std::vector<uint8_t> BuildIsobmffExifPayload(int starVal) {
    const int clamped = std::clamp(starVal, 0, 5);
    const uint16_t pct = RatingPercentForStars(clamped);
    std::vector<uint8_t> p;
    p.reserve(42);
    // 4-byte ISOBMFF Exif offset to TIFF header (0, Big Endian)
    p.push_back(0); p.push_back(0); p.push_back(0); p.push_back(0);
    // TIFF header: "II", 42, IFD0 offset 8
    p.push_back('I'); p.push_back('I');
    p.push_back(0x2A); p.push_back(0x00);
    p.push_back(0x08); p.push_back(0x00); p.push_back(0x00); p.push_back(0x00);
    // IFD0 entries count: 2 (LE)
    p.push_back(0x02); p.push_back(0x00);
    // tag 0x4746: SHORT(3), count 1, val = clamped
    p.push_back(0x46); p.push_back(0x47);
    p.push_back(0x03); p.push_back(0x00);
    p.push_back(0x01); p.push_back(0x00); p.push_back(0x00); p.push_back(0x00);
    p.push_back(static_cast<uint8_t>(clamped & 0xFF));
    p.push_back(static_cast<uint8_t>((clamped >> 8) & 0xFF));
    p.push_back(0x00); p.push_back(0x00);
    // tag 0x4749: SHORT(3), count 1, val = pct
    p.push_back(0x49); p.push_back(0x47);
    p.push_back(0x03); p.push_back(0x00);
    p.push_back(0x01); p.push_back(0x00); p.push_back(0x00); p.push_back(0x00);
    p.push_back(static_cast<uint8_t>(pct & 0xFF));
    p.push_back(static_cast<uint8_t>((pct >> 8) & 0xFF));
    p.push_back(0x00); p.push_back(0x00);
    // Next IFD: 0
    p.push_back(0x00); p.push_back(0x00); p.push_back(0x00); p.push_back(0x00);
    return p;
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
WriteStatus Transcode(IWICImagingFactory* factory, const std::wstring& path, int stars) {
    std::vector<BYTE> sourceBytes;
    DWORD readErr = 0;
    if (!ReadWholeFile(path, 1, MAXDWORD, sourceBytes, &readErr)) {
        return IsFileLockError(readErr) ? WriteStatus::NeedsTranscode : WriteStatus::Failed;
    }

    const std::wstring tempPath = CreateUniqueTempPath(path, L".qvrating.tmp");
    bool encodedOk = false;

    // Scope all COM objects before DeleteFileW or AtomicReplaceFile so Windows
    // releases the temp file write lock deterministically on any error path.
    {
        ComPtr<IWICStream> sourceStream;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        GUID containerFormat{};
        UINT frameCount = 0;

        HRESULT hr = factory->CreateStream(&sourceStream);
        if (SUCCEEDED(hr)) {
            hr = sourceStream->InitializeFromMemory(sourceBytes.data(), static_cast<DWORD>(sourceBytes.size()));
        }
        if (SUCCEEDED(hr)) {
            hr = factory->CreateDecoderFromStream(sourceStream.Get(), nullptr,
                                                  WICDecodeMetadataCacheOnDemand, &decoder);
        }
        if (SUCCEEDED(hr)) {
            hr = decoder->GetContainerFormat(&containerFormat);
        }
        if (SUCCEEDED(hr)) {
            hr = decoder->GetFrameCount(&frameCount);
            if (SUCCEEDED(hr) && frameCount == 0) hr = E_FAIL;
        }
        if (SUCCEEDED(hr)) {
            hr = factory->CreateStream(&stream);
        }
        if (SUCCEEDED(hr)) {
            hr = stream->InitializeFromFilename(tempPath.c_str(), GENERIC_WRITE);
        }
        if (SUCCEEDED(hr)) {
            hr = factory->CreateEncoder(containerFormat, nullptr, &encoder);
        }
        if (SUCCEEDED(hr)) {
            hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
        }

        for (UINT i = 0; SUCCEEDED(hr) && i < frameCount; ++i) {
            ComPtr<IWICBitmapFrameDecode> frameDecode;
            hr = decoder->GetFrame(i, &frameDecode);
            if (FAILED(hr)) break;

            ComPtr<IWICBitmapFrameEncode> frameEncode;
            hr = encoder->CreateNewFrame(&frameEncode, nullptr);
            if (FAILED(hr)) break;
            hr = frameEncode->Initialize(nullptr);
            if (FAILED(hr)) break;

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
                hr = frameEncode->GetMetadataQueryWriter(&writer);
                if (SUCCEEDED(hr)) {
                    hr = ApplyRating(writer.Get(), QueriesFor(path), stars);
                }
                if (FAILED(hr)) break;
            }

            hr = frameEncode->WriteSource(frameDecode.Get(), nullptr);
            if (FAILED(hr)) break;
            hr = frameEncode->Commit();
        }

        if (SUCCEEDED(hr)) {
            hr = encoder->Commit();
        }
        encodedOk = SUCCEEDED(hr);
    }

    if (!encodedOk) {
        DeleteFileW(tempPath.c_str());
        return WriteStatus::Failed;
    }

    DWORD replaceErr = 0;
    if (!AtomicReplaceFile(path, tempPath, &replaceErr)) {
        return (IsFileLockError(replaceErr) || replaceErr == ERROR_ACCESS_DENIED)
            ? WriteStatus::NeedsTranscode
            : WriteStatus::Failed;
    }
    return WriteStatus::WrittenTranscode;
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
    const int clampedStars = std::clamp(stars, 0, 5);
    std::vector<uint8_t> src;
    DWORD readErr = 0;
    if (!ReadWholeFile(path, 12, MAXDWORD, src, &readErr)) {
        return IsFileLockError(readErr) ? WriteStatus::NeedsTranscode : WriteStatus::Failed;
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
    int firstXmpIndex = -1;
    int vp8xIndex = -1;
    bool hasExifChunk = false;

    while (pos + 8 <= src.size()) {
        char fcc[4];
        std::memcpy(fcc, src.data() + pos, 4);
        const uint32_t len = static_cast<uint32_t>(src[pos + 4]) |
                             (static_cast<uint32_t>(src[pos + 5]) << 8) |
                             (static_cast<uint32_t>(src[pos + 6]) << 16) |
                             (static_cast<uint32_t>(src[pos + 7]) << 24);
        const size_t dataOffset = pos + 8;
        if (len > src.size() - dataOffset) break; // truncated

        const size_t pad = len & 1;
        const size_t totalSpan = 8 + static_cast<size_t>(len) + pad;

        if (std::memcmp(fcc, "VP8X", 4) == 0) {
            vp8xIndex = static_cast<int>(chunks.size());
        } else if (std::memcmp(fcc, "XMP ", 4) == 0) {
            if (firstXmpIndex < 0) {
                firstXmpIndex = static_cast<int>(chunks.size());
            }
        } else if (std::memcmp(fcc, "EXIF", 4) == 0) {
            hasExifChunk = true;
        }

        chunks.push_back(ChunkSpan{ {fcc[0], fcc[1], fcc[2], fcc[3]}, dataOffset, len, totalSpan });
        pos += totalSpan;
    }

    // Sync any EXIF chunks carrying tag 0x4746/0x4749 so clearing or changing
    // rating is never overshadowed by a stale EXIF chunk.
    bool modifiedExif = false;
    if (hasExifChunk) {
        for (const auto& c : chunks) {
            if (std::memcmp(c.fourcc, "EXIF", 4) != 0 || c.dataSize <= 8) continue;
            size_t tiffOff = 0;
            if (c.dataSize >= 6 && std::memcmp(src.data() + c.dataOffset, "Exif\0\0", 6) == 0) {
                tiffOff = 6;
            }
            if (c.dataSize > tiffOff + 8) {
                if (PatchTiffBufferRating(
                        std::span<uint8_t>(src.data() + c.dataOffset + tiffOff, c.dataSize - tiffOff),
                        clampedStars)) {
                    modifiedExif = true;
                }
            }
        }
    }

    std::string newXmpPayload;
    bool hasNewXmp = false;

    if (firstXmpIndex >= 0) {
        const auto& c = chunks[firstXmpIndex];
        const std::string_view oldXmp(reinterpret_cast<const char*>(src.data() + c.dataOffset), c.dataSize);
        if (clampedStars > 0) {
            if (auto updated = QuickView::Rating::UpdateXmpRating(oldXmp, clampedStars)) {
                newXmpPayload = *updated;
            } else {
                newXmpPayload = QuickView::Rating::BuildMinimalXmp(clampedStars);
            }
            hasNewXmp = true;
        } else {
            if (auto updated = QuickView::Rating::UpdateXmpRating(oldXmp, 0)) {
                // Only keep XMP when it carried metadata other than rating; otherwise strip chunk.
                if (updated->find("crs:") != std::string::npos ||
                    updated->find("dc:") != std::string::npos ||
                    updated->find("photoshop:") != std::string::npos ||
                    updated->find("exif:") != std::string::npos) {
                    newXmpPayload = *updated;
                    hasNewXmp = true;
                }
            }
        }
    } else if (clampedStars > 0) {
        newXmpPayload = QuickView::Rating::BuildMinimalXmp(clampedStars);
        hasNewXmp = true;
    } else if (!modifiedExif) {
        // stars == 0 and neither XMP nor EXIF tag was present: nothing to change
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
        if (hasExifChunk) flags |= 0x08;

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
    if (vp8xIndex >= 0 && vp8xIndex < static_cast<int>(chunks.size())) {
        const auto& v = chunks[vp8xIndex];
        if (v.dataSize >= 10 && v.dataOffset < src.size()) {
            if (hasNewXmp) {
                src[v.dataOffset] |= 0x04;
            } else if (clampedStars <= 0) {
                src[v.dataOffset] &= ~0x04;
            }
        }
    }

    // If synthesized VP8X exists, insert it first right after WEBP
    if (!synthesizedVp8x.empty()) {
        appendChunk("VP8X", synthesizedVp8x.data(), synthesizedVp8x.size());
    }

    bool xmpWritten = false;
    for (size_t i = 0; i < chunks.size(); ++i) {
        const auto& c = chunks[i];
        if (std::memcmp(c.fourcc, "XMP ", 4) == 0) {
            if (hasNewXmp && !xmpWritten) {
                appendChunk("XMP ", newXmpPayload.data(), newXmpPayload.size());
                xmpWritten = true;
            }
            continue; // drop old/duplicate XMP chunks
        }
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

    WriteStatus result = WriteStatus::Failed;
    WriteBufferAndReplace(path, L".qvrating.tmp", out, result);
    return result;
}

static bool PatchIsobmffExifInPlace(const std::wstring& path,
                                    std::span<const uint8_t> src,
                                    size_t exifPayloadOff,
                                    size_t exifPayloadLen,
                                    int stars) {
    if (exifPayloadLen < 16 || exifPayloadOff > src.size() || exifPayloadLen > src.size() - exifPayloadOff) {
        return false;
    }
    const size_t payloadEnd = exifPayloadOff + exifPayloadLen;
    const int clampedStars = std::clamp(stars, 0, 5);

    // ISOBMFF Exif item starts with 4-byte offset to TIFF header
    const uint32_t tiffOffset = ReadBe32(src.data() + exifPayloadOff);

    size_t tiffPos = exifPayloadOff + 4;
    if (tiffOffset <= payloadEnd - tiffPos && (payloadEnd - (tiffPos + tiffOffset)) >= 8) {
        tiffPos += tiffOffset;
    }
    if (tiffPos + 8 > payloadEnd) return false;

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
    if (ifdOffset < 8 || ifdOffset > payloadEnd - tiffPos - 2) return false;

    const size_t ifdPos = tiffPos + ifdOffset;
    const uint16_t numEntries = readU16(src.data() + ifdPos);
    size_t cur = ifdPos + 2;

    size_t tag4746ValOff = 0;
    size_t tag4749ValOff = 0;

    for (uint16_t i = 0; i < numEntries && cur + 12 <= payloadEnd; ++i, cur += 12) {
        const uint16_t tag = readU16(src.data() + cur);
        const uint16_t type = readU16(src.data() + cur + 2);
        const uint32_t count = readU32(src.data() + cur + 4);
        if (type == 3 && count == 1) { // SHORT scalar
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

    auto writeShort = [&](size_t fileOffset, uint16_t val) -> bool {
        LARGE_INTEGER li;
        li.QuadPart = static_cast<LONGLONG>(fileOffset);
        if (!SetFilePointerEx(hFile, li, nullptr, FILE_BEGIN)) return false;
        uint8_t buf[2];
        if (littleEndian) {
            buf[0] = static_cast<uint8_t>(val & 0xFF);
            buf[1] = static_cast<uint8_t>((val >> 8) & 0xFF);
        } else {
            buf[0] = static_cast<uint8_t>((val >> 8) & 0xFF);
            buf[1] = static_cast<uint8_t>(val & 0xFF);
        }
        DWORD written = 0;
        return WriteFile(hFile, buf, 2, &written, nullptr) != FALSE && written == 2;
    };

    bool writeOk = true;
    if (tag4746ValOff > 0) {
        writeOk = writeShort(tag4746ValOff, static_cast<uint16_t>(clampedStars));
    }
    if (writeOk && tag4749ValOff > 0) {
        writeOk = writeShort(tag4749ValOff, RatingPercentForStars(clampedStars));
    }

    if (writeOk) FlushFileBuffers(hFile);
    CloseHandle(hFile);
    return writeOk;
}

WriteStatus WriteRatingToIsobmff(const std::wstring& path, int stars, bool allowTranscode) {
    const int clampedStars = std::clamp(stars, 0, 5);
    std::vector<uint8_t> src;
    DWORD readErr = 0;
    if (!ReadWholeFile(path, 12, 256ull * 1024ull * 1024ull, src, &readErr)) {
        return IsFileLockError(readErr) ? WriteStatus::NeedsTranscode : WriteStatus::Failed;
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
    auto writeU64 = [](std::vector<uint8_t>& buf, uint64_t val) {
        for (int i = 7; i >= 0; --i) {
            buf.push_back(static_cast<uint8_t>((val >> (i * 8)) & 0xFF));
        }
    };
    auto writeVarInt = [](std::vector<uint8_t>& buf, uint64_t val, size_t numBytes) {
        for (size_t i = 0; i < numBytes; ++i) {
            const size_t shift = (numBytes - 1 - i) * 8;
            buf.push_back(static_cast<uint8_t>((val >> shift) & 0xFF));
        }
    };

    // Locate top-level 'meta' box and optional trailing 'mdat' after 'meta'
    size_t pos = 0;
    size_t metaPos = 0, metaSize = 0, metaHeaderSize = 0, metaPayloadPos = 0, metaEnd = 0;
    size_t lastMdatPos = 0, lastMdatSize = 0, lastMdatHeaderSize = 0;

    while (pos + 8 <= src.size()) {
        const uint32_t rawSize = readU32(src.data() + pos);
        const uint8_t* type = src.data() + pos + 4;
        uint64_t boxSize64 = rawSize;
        size_t headerSize = 8;

        if (rawSize == 1) {
            if (pos + 16 > src.size()) break;
            boxSize64 = readU64(src.data() + pos + 8);
            headerSize = 16;
        } else if (rawSize == 0) {
            boxSize64 = src.size() - pos;
        }

        if (boxSize64 < headerSize || boxSize64 > src.size() - pos) break;
        const size_t boxSize = static_cast<size_t>(boxSize64);
        const size_t nextPos = pos + boxSize;

        if (std::memcmp(type, "meta", 4) == 0 && metaPayloadPos == 0) {
            if (pos + headerSize + 4 <= nextPos) {
                metaPos = pos;
                metaSize = boxSize;
                metaHeaderSize = headerSize;
                metaPayloadPos = pos + headerSize + 4;
                metaEnd = nextPos;
            }
        } else if (std::memcmp(type, "mdat", 4) == 0) {
            lastMdatPos = pos;
            lastMdatSize = boxSize;
            lastMdatHeaderSize = headerSize;
        }

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
        if (subSize < 8 || subSize > metaEnd - subPos) break;
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

    if (iinfOffset == 0 || ilocOffset == 0 || iinfSize < 14 || ilocSize < 16) return WriteStatus::Failed;

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
    uint8_t existingIrefVer = 0;
    if (irefOffset > 0 && irefSize >= 12) {
        existingIrefVer = src[irefOffset + 8];
        size_t irefCur = irefOffset + 12;
        while (irefCur + 8 <= irefOffset + irefSize) {
            const uint32_t boxSz = readU32(src.data() + irefCur);
            if (boxSz < 8 || boxSz > (irefOffset + irefSize) - irefCur) break;
            const uint8_t* btype = src.data() + irefCur + 4;
            if (std::memcmp(btype, "cdsc", 4) == 0) {
                if (existingIrefVer == 0 && boxSz >= 14) {
                    const uint16_t fromId = readU16(src.data() + irefCur + 8);
                    const uint16_t refCnt = readU16(src.data() + irefCur + 10);
                    for (uint16_t r = 0; r < refCnt && (size_t)(r + 1) * 2 <= boxSz - 12; ++r) {
                        const uint16_t toId = readU16(src.data() + irefCur + 12 + r * 2);
                        if (toId == primaryItemId) {
                            primaryCdscItems.push_back(fromId);
                            break;
                        }
                    }
                } else if (existingIrefVer == 1 && boxSz >= 18) {
                    const uint32_t fromId = readU32(src.data() + irefCur + 8);
                    const uint16_t refCnt = readU16(src.data() + irefCur + 12);
                    for (uint16_t r = 0; r < refCnt && (size_t)(r + 1) * 4 <= boxSz - 14; ++r) {
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

    // Find exifItemId, xmpItemId, and maxItemId across 'iinf'
    uint32_t exifItemId = 0;
    uint32_t xmpItemId = 0;
    bool exifLocked = false;
    bool xmpLocked = false;
    uint32_t maxItemId = 0;

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
        if (infeSize < 8 || infeSize > (iinfOffset + iinfSize) - iinfCur) break;

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
                if (itemId > maxItemId) maxItemId = itemId;
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
                            (iinfCur + infeSize) - (typeOffset + 4));
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
    size_t exifPayloadOff = 0, exifPayloadLen = 0;
    size_t xmpPayloadOff = 0, xmpPayloadLen = 0;

    const uint8_t ilocVer = src[ilocOffset + 8];
    const uint8_t offSize = (src[ilocOffset + 12] >> 4) & 0x0F;
    const uint8_t lenSize = src[ilocOffset + 12] & 0x0F;
    const uint8_t baseOffSize = (src[ilocOffset + 13] >> 4) & 0x0F;
    const uint8_t indexSize = (ilocVer == 1 || ilocVer == 2) ? (src[ilocOffset + 13] & 0x0F) : 0;
    if (offSize > 8 || lenSize > 8 || baseOffSize > 8 || indexSize > 8) return WriteStatus::Failed;

    const size_t ilocEnd = ilocOffset + ilocSize;
    size_t ilocCur = ilocOffset + 14;
    uint32_t itemCount = 0;
    if (ilocVer < 2) {
        if (ilocCur + 2 > ilocEnd) return WriteStatus::Failed;
        itemCount = readU16(src.data() + ilocCur);
        ilocCur += 2;
    } else {
        if (ilocCur + 4 > ilocEnd) return WriteStatus::Failed;
        itemCount = readU32(src.data() + ilocCur);
        ilocCur += 4;
    }

    const size_t extentStride = static_cast<size_t>(indexSize) + offSize + lenSize;
    existingItems.reserve((std::min)(itemCount, 4096u));

    for (uint32_t i = 0; i < itemCount && ilocCur < ilocEnd; ++i) {
        uint32_t itemId = 0;
        if (ilocVer < 2) {
            if (2 > ilocEnd - ilocCur) return WriteStatus::Failed;
            itemId = readU16(src.data() + ilocCur);
            ilocCur += 2;
        } else {
            if (4 > ilocEnd - ilocCur) return WriteStatus::Failed;
            itemId = readU32(src.data() + ilocCur);
            ilocCur += 4;
        }
        if (itemId > maxItemId) maxItemId = itemId;

        uint16_t cm = 0;
        if (ilocVer == 1 || ilocVer == 2) {
            if (2 > ilocEnd - ilocCur) return WriteStatus::Failed;
            cm = readU16(src.data() + ilocCur) & 0x0F;
            ilocCur += 2;
        }

        if (2 + static_cast<size_t>(baseOffSize) + 2 > ilocEnd - ilocCur) return WriteStatus::Failed;
        const uint16_t dref = readU16(src.data() + ilocCur);
        ilocCur += 2;

        const uint64_t baseOffset = readVarInt(src.data() + ilocCur, baseOffSize);
        ilocCur += baseOffSize;

        const uint16_t extentCount = readU16(src.data() + ilocCur);
        ilocCur += 2;

        if (extentCount > 0 && extentStride > 0 &&
            static_cast<size_t>(extentCount) > (ilocEnd - ilocCur) / extentStride) {
            return WriteStatus::Failed;
        }

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

            if (e == 0 && extentCount == 1 && cm == 0 && dref == 0 &&
                extentOffset <= src.size() && baseOffset <= src.size() - extentOffset) {
                const uint64_t absOff = baseOffset + extentOffset;
                if (extentLength <= src.size() - absOff) {
                    if (itemId == exifItemId && exifItemId > 0) {
                        exifPayloadOff = static_cast<size_t>(absOff);
                        exifPayloadLen = static_cast<size_t>(extentLength);
                    } else if (itemId == xmpItemId && xmpItemId > 0) {
                        xmpPayloadOff = static_cast<size_t>(absOff);
                        xmpPayloadLen = static_cast<size_t>(extentLength);
                    }
                }
            }
        }
        existingItems.push_back(std::move(item));
    }

    bool inPlaceSuccess = false;

    // --- 1. In-Place Exif Rating Patch ---
    if (exifPayloadOff > 0 && exifPayloadLen > 12) {
        if (PatchIsobmffExifInPlace(path, src, exifPayloadOff, exifPayloadLen, clampedStars)) {
            inPlaceSuccess = true;
        }
    }

    // --- 2. In-Place Strict XMP Padding Patch ---
    if (!inPlaceSuccess && xmpPayloadOff > 0 && xmpPayloadLen > 0 && xmpPayloadLen <= src.size() - xmpPayloadOff) {
        const std::string_view oldXmp(reinterpret_cast<const char*>(src.data() + xmpPayloadOff), xmpPayloadLen);
        const auto patched = QuickView::Rating::PatchXmpRatingInPlaceStrict(oldXmp, clampedStars);
        if (patched && patched->size() == xmpPayloadLen) {
            if (*patched == oldXmp) {
                inPlaceSuccess = true;
            } else {
                HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (hFile != INVALID_HANDLE_VALUE) {
                    LARGE_INTEGER seekPos;
                    seekPos.QuadPart = static_cast<LONGLONG>(xmpPayloadOff);
                    DWORD written = 0;
                    const BOOL seekOk = SetFilePointerEx(hFile, seekPos, nullptr, FILE_BEGIN);
                    const BOOL ok = seekOk && WriteFile(hFile, patched->data(), static_cast<DWORD>(xmpPayloadLen), &written, nullptr);
                    if (ok && written == xmpPayloadLen) {
                        FlushFileBuffers(hFile);
                        inPlaceSuccess = true;
                    }
                    CloseHandle(hFile);
                }
            }
        }
    }

    if (inPlaceSuccess) {
        return WriteStatus::WrittenInPlace;
    }

    if (clampedStars <= 0) {
        const bool hasExifRating = (exifPayloadLen > 4) &&
            ParseIsoExifPayload(std::span<const uint8_t>(src.data() + exifPayloadOff, exifPayloadLen)).has_value();
        const bool hasXmpRating = (xmpPayloadLen > 0) &&
            ParseXmpRating(std::string_view(reinterpret_cast<const char*>(src.data() + xmpPayloadOff), xmpPayloadLen)).has_value();
        if (!hasExifRating && !hasXmpRating) {
            return WriteStatus::WrittenInPlace;
        }
    }

    if (!allowTranscode) {
        return WriteStatus::NeedsTranscode;
    }

    // --- 3. Lossless ISOBMFF Reconstruction & Non-Shadowing Injection ---
    if (maxItemId >= 0xFFFFFFFEu || existingItems.size() >= 0xFFFF) {
        return WriteStatus::Failed;
    }

    const std::wstring_view ext = QuickView::ExtensionOf(path);
    const bool isAvif = QuickView::ExtEqualsIgnoreCase(ext, L".avif") ||
                        QuickView::ExtEqualsIgnoreCase(ext, L".avifs");

    // Reuse an existing XMP item (updating its extent in-place in iloc) when an unpadded XMP item
    // exists or an existing Exif item lacks 0x4746 so we never append a shadowed second item.
    uint32_t replaceExistingItemId = 0;
    bool payloadIsExif = false;
    if (xmpItemId != 0) {
        replaceExistingItemId = xmpItemId;
        payloadIsExif = false;
    } else if (exifItemId == 0) {
        payloadIsExif = isAvif;
    } else {
        // Existing Exif lacks 0x4746 and no XMP item exists yet: add padded XMP item so Exif
        // falls through to XMP without replacing camera Exif or creating a shadowed duplicate Exif.
        payloadIsExif = false;
    }

    const bool addingNewItem = (replaceExistingItemId == 0);
    const uint32_t newItemId = addingNewItem ? (maxItemId + 1) : replaceExistingItemId;
    if (addingNewItem && ilocVer < 2 && newItemId > 0xFFFF) {
        return WriteStatus::Failed;
    }

    std::vector<uint8_t> payload;
    std::vector<uint8_t> infeBox;

    if (payloadIsExif) {
        payload = BuildIsobmffExifPayload(clampedStars);
    } else {
        std::string xmpStr;
        if (xmpPayloadOff > 0 && xmpPayloadLen > 0 && xmpPayloadLen <= src.size() - xmpPayloadOff) {
            const std::string_view oldXmp(reinterpret_cast<const char*>(src.data() + xmpPayloadOff), xmpPayloadLen);
            if (auto updated = BuildPaddedUpdatedXmp(oldXmp, clampedStars)) {
                xmpStr = std::move(*updated);
            }
        }
        if (xmpStr.empty()) {
            xmpStr = BuildStandardXmpPayload(clampedStars);
        }
        payload.assign(xmpStr.begin(), xmpStr.end());
    }

    if (addingNewItem) {
        if (payloadIsExif) {
            const bool v3 = (newItemId > 0xFFFF);
            const uint32_t infeSize = 8 + 4 + (v3 ? 4 : 2) + 2 + 4 + 1;
            infeBox.reserve(infeSize);
            writeU32(infeBox, infeSize);
            infeBox.push_back('i'); infeBox.push_back('n'); infeBox.push_back('f'); infeBox.push_back('e');
            writeU32(infeBox, static_cast<uint32_t>(v3 ? 3u : 2u) << 24);
            if (v3) writeU32(infeBox, newItemId);
            else writeU16(infeBox, static_cast<uint16_t>(newItemId));
            writeU16(infeBox, 0);
            infeBox.push_back('E'); infeBox.push_back('x'); infeBox.push_back('i'); infeBox.push_back('f');
            infeBox.push_back('\0');
        } else {
            const std::string_view mimeType = "application/rdf+xml";
            const bool v3 = (newItemId > 0xFFFF);
            const uint32_t infeSize = 8 + 4 + (v3 ? 4 : 2) + 2 + 4 + 1 + static_cast<uint32_t>(mimeType.size() + 1);
            infeBox.reserve(infeSize);
            writeU32(infeBox, infeSize);
            infeBox.push_back('i'); infeBox.push_back('n'); infeBox.push_back('f'); infeBox.push_back('e');
            writeU32(infeBox, static_cast<uint32_t>(v3 ? 3u : 2u) << 24);
            if (v3) writeU32(infeBox, newItemId);
            else writeU16(infeBox, static_cast<uint16_t>(newItemId));
            writeU16(infeBox, 0);
            infeBox.push_back('m'); infeBox.push_back('i'); infeBox.push_back('m'); infeBox.push_back('e');
            infeBox.push_back('\0');
            for (char c : mimeType) infeBox.push_back(static_cast<uint8_t>(c));
            infeBox.push_back('\0');
        }
    }

    // Build new iref box only when adding a brand new item
    std::vector<uint8_t> newIrefBox;
    if (addingNewItem) {
        const bool needV1 = (existingIrefVer == 1 || newItemId > 0xFFFF || primaryItemId > 0xFFFF);
        std::vector<uint8_t> cdscBox;
        if (needV1) {
            cdscBox.reserve(18);
            writeU32(cdscBox, 18);
            cdscBox.push_back('c'); cdscBox.push_back('d'); cdscBox.push_back('s'); cdscBox.push_back('c');
            writeU32(cdscBox, newItemId);
            writeU16(cdscBox, 1);
            writeU32(cdscBox, primaryItemId);
        } else {
            cdscBox.reserve(14);
            writeU32(cdscBox, 14);
            cdscBox.push_back('c'); cdscBox.push_back('d'); cdscBox.push_back('s'); cdscBox.push_back('c');
            writeU16(cdscBox, static_cast<uint16_t>(newItemId));
            writeU16(cdscBox, 1);
            writeU16(cdscBox, static_cast<uint16_t>(primaryItemId));
        }

        if (irefOffset > 0 && irefSize >= 12 && (!needV1 || existingIrefVer == 1)) {
            const uint32_t newSize = static_cast<uint32_t>(irefSize + cdscBox.size());
            newIrefBox.reserve(newSize);
            writeU32(newIrefBox, newSize);
            newIrefBox.insert(newIrefBox.end(), src.data() + irefOffset + 4, src.data() + irefOffset + irefSize);
            newIrefBox.insert(newIrefBox.end(), cdscBox.begin(), cdscBox.end());
        } else if (irefOffset > 0 && irefSize >= 12 && needV1 && existingIrefVer == 0) {
            // Upgrade v0 iref entries to v1 (32-bit IDs) cleanly before appending cdsc
            newIrefBox.insert(newIrefBox.end(), { 0, 0, 0, 0, 'i', 'r', 'e', 'f' });
            writeU32(newIrefBox, 1u << 24);
            size_t rCur = irefOffset + 12;
            while (rCur + 8 <= irefOffset + irefSize) {
                const uint32_t rSz = readU32(src.data() + rCur);
                if (rSz < 12 || rSz > (irefOffset + irefSize) - rCur) break;
                const uint16_t fromId = readU16(src.data() + rCur + 8);
                const uint16_t refCnt = readU16(src.data() + rCur + 10);
                std::vector<uint32_t> targets;
                for (uint16_t r = 0; r < refCnt && (size_t)(r + 1) * 2 <= rSz - 12; ++r) {
                    targets.push_back(readU16(src.data() + rCur + 12 + r * 2));
                }
                const uint32_t upgradedSz = static_cast<uint32_t>(14 + targets.size() * 4);
                writeU32(newIrefBox, upgradedSz);
                newIrefBox.insert(newIrefBox.end(), src.data() + rCur + 4, src.data() + rCur + 8);
                writeU32(newIrefBox, fromId);
                writeU16(newIrefBox, static_cast<uint16_t>(targets.size()));
                for (uint32_t tid : targets) writeU32(newIrefBox, tid);
                rCur += rSz;
            }
            newIrefBox.insert(newIrefBox.end(), cdscBox.begin(), cdscBox.end());
            const uint32_t totalIref = static_cast<uint32_t>(newIrefBox.size());
            newIrefBox[0] = static_cast<uint8_t>((totalIref >> 24) & 0xFF);
            newIrefBox[1] = static_cast<uint8_t>((totalIref >> 16) & 0xFF);
            newIrefBox[2] = static_cast<uint8_t>((totalIref >> 8) & 0xFF);
            newIrefBox[3] = static_cast<uint8_t>(totalIref & 0xFF);
        } else {
            const uint32_t newSize = static_cast<uint32_t>(12 + cdscBox.size());
            newIrefBox.reserve(newSize);
            writeU32(newIrefBox, newSize);
            newIrefBox.push_back('i'); newIrefBox.push_back('r'); newIrefBox.push_back('e'); newIrefBox.push_back('f');
            writeU32(newIrefBox, needV1 ? (1u << 24) : 0u);
            newIrefBox.insert(newIrefBox.end(), cdscBox.begin(), cdscBox.end());
        }
    }

    // Build new iinf box
    std::vector<uint8_t> newIinfBox;
    if (addingNewItem) {
        const size_t newIinfBoxSize = iinfSize + infeBox.size();
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
    } else {
        newIinfBox.assign(src.data() + iinfOffset, src.data() + iinfOffset + iinfSize);
    }

    // Compute upgraded iloc field widths so rebuilt offsets never truncate.
    auto maxForBytes = [](uint8_t b) -> uint64_t {
        if (b == 0) return 0;
        if (b >= 8) return UINT64_MAX;
        return (1ULL << (b * 8)) - 1;
    };
    const uint64_t upperEstOffset = static_cast<uint64_t>(src.size()) + 16384;
    const uint8_t newOffSize = (offSize == 0 || maxForBytes(offSize) >= upperEstOffset)
        ? (offSize == 0 ? 4 : offSize)
        : (upperEstOffset <= 0xFFFFFFFFull ? 4 : 8);
    const uint8_t newLenSize = (maxForBytes(lenSize) >= (std::max)(payload.size(), size_t{ 65536 }))
        ? lenSize
        : 4;
    const uint8_t newBaseOffSize = (baseOffSize > 0 && maxForBytes(baseOffSize) < upperEstOffset)
        ? (upperEstOffset <= 0xFFFFFFFFull ? 4 : 8)
        : baseOffSize;

    size_t newIlocPayloadBytes = 14 + (ilocVer < 2 ? 2 : 4);
    bool replacedHasExistingIloc = false;
    for (const auto& it : existingItems) {
        const size_t extCnt = (replaceExistingItemId != 0 && it.id == replaceExistingItemId) ? 1 : it.extents.size();
        if (replaceExistingItemId != 0 && it.id == replaceExistingItemId) replacedHasExistingIloc = true;
        newIlocPayloadBytes += (ilocVer < 2 ? 2 : 4) +
                               (ilocVer == 1 || ilocVer == 2 ? 2 : 0) +
                               2 + newBaseOffSize + 2 +
                               extCnt * (static_cast<size_t>(indexSize) + newOffSize + newLenSize);
    }
    if (addingNewItem || !replacedHasExistingIloc) {
        newIlocPayloadBytes += (ilocVer < 2 ? 2 : 4) +
                               (ilocVer == 1 || ilocVer == 2 ? 2 : 0) +
                               2 + newBaseOffSize + 2 +
                               (static_cast<size_t>(indexSize) + newOffSize + newLenSize);
    }

    // Compute exact new 'meta' size before finalizing file offsets.
    // Rebuild 'meta' body: preserve all non-(iloc/iinf/iref) subboxes intact.
    std::vector<uint8_t> newMetaSubboxes;
    bool hasIrefInMeta = false;
    size_t scanSub = metaPayloadPos;
    while (scanSub + 8 <= metaEnd) {
        const uint32_t curSize = readU32(src.data() + scanSub);
        if (curSize < 8 || curSize > metaEnd - scanSub) break;
        const uint8_t* curType = src.data() + scanSub + 4;
        if (std::memcmp(curType, "iinf", 4) == 0) {
            newMetaSubboxes.insert(newMetaSubboxes.end(), newIinfBox.begin(), newIinfBox.end());
        } else if (std::memcmp(curType, "iref", 4) == 0) {
            if (addingNewItem) {
                newMetaSubboxes.insert(newMetaSubboxes.end(), newIrefBox.begin(), newIrefBox.end());
            } else {
                newMetaSubboxes.insert(newMetaSubboxes.end(), src.data() + scanSub, src.data() + scanSub + curSize);
            }
            hasIrefInMeta = true;
        } else if (std::memcmp(curType, "iloc", 4) != 0) {
            newMetaSubboxes.insert(newMetaSubboxes.end(), src.data() + scanSub, src.data() + scanSub + curSize);
        }
        scanSub += curSize;
    }
    if (addingNewItem && !hasIrefInMeta) {
        newMetaSubboxes.insert(newMetaSubboxes.end(), newIrefBox.begin(), newIrefBox.end());
    }

    const size_t newMetaBoxSize = 8 + 4 + newIlocPayloadBytes + newMetaSubboxes.size();
    const int64_t metaGrowth = static_cast<int64_t>(newMetaBoxSize) - static_cast<int64_t>(metaSize);

    // Extend trailing mdat at EOF only when mdat follows meta and reaches EOF;
    // otherwise append a new trailing mdat box at EOF (supports mdat before meta cleanly!).
    const bool extendTrailingMdat = (lastMdatPos >= metaEnd &&
                                     lastMdatSize >= lastMdatHeaderSize &&
                                     lastMdatPos + lastMdatSize == src.size());
    const uint64_t payloadAbsOffset = extendTrailingMdat
        ? static_cast<uint64_t>(static_cast<int64_t>(lastMdatPos + lastMdatSize) + metaGrowth)
        : static_cast<uint64_t>(static_cast<int64_t>(src.size()) + metaGrowth + 8);

    // Build finalized iloc box
    std::vector<uint8_t> newIlocBox;
    newIlocBox.reserve(newIlocPayloadBytes);
    writeU32(newIlocBox, static_cast<uint32_t>(newIlocPayloadBytes));
    newIlocBox.insert(newIlocBox.end(), src.data() + ilocOffset + 4, src.data() + ilocOffset + 12);
    newIlocBox.push_back(static_cast<uint8_t>((newOffSize << 4) | (newLenSize & 0x0F)));
    newIlocBox.push_back(static_cast<uint8_t>((newBaseOffSize << 4) | (indexSize & 0x0F)));

    const uint32_t finalIlocItemCount = (addingNewItem || !replacedHasExistingIloc)
        ? static_cast<uint32_t>(existingItems.size() + 1)
        : static_cast<uint32_t>(existingItems.size());
    if (ilocVer < 2) {
        writeU16(newIlocBox, static_cast<uint16_t>(finalIlocItemCount));
    } else {
        writeU32(newIlocBox, finalIlocItemCount);
    }

    auto appendPayloadItemLocation = [&](uint32_t id) {
        if (ilocVer < 2) writeU16(newIlocBox, static_cast<uint16_t>(id));
        else writeU32(newIlocBox, id);
        if (ilocVer == 1 || ilocVer == 2) writeU16(newIlocBox, 0); // cm = 0
        writeU16(newIlocBox, 0); // dref = 0
        if (newBaseOffSize > 0) writeVarInt(newIlocBox, 0, newBaseOffSize);
        writeU16(newIlocBox, 1);
        if (indexSize > 0) writeVarInt(newIlocBox, 0, indexSize);
        writeVarInt(newIlocBox, payloadAbsOffset, newOffSize);
        writeVarInt(newIlocBox, payload.size(), newLenSize);
    };

    for (const auto& it : existingItems) {
        if (replaceExistingItemId != 0 && it.id == replaceExistingItemId) {
            appendPayloadItemLocation(it.id);
            continue;
        }
        if (ilocVer < 2) writeU16(newIlocBox, static_cast<uint16_t>(it.id));
        else writeU32(newIlocBox, it.id);
        if (ilocVer == 1 || ilocVer == 2) writeU16(newIlocBox, it.constructionMethod);
        writeU16(newIlocBox, it.dref);

        uint64_t curBase = it.baseOffset;
        if (offSize == 0 && it.constructionMethod == 0 && curBase >= metaEnd) {
            curBase = static_cast<uint64_t>(static_cast<int64_t>(curBase) + metaGrowth);
        }
        if (newBaseOffSize > 0) {
            writeVarInt(newIlocBox, curBase, newBaseOffSize);
        }
        writeU16(newIlocBox, static_cast<uint16_t>(it.extents.size()));
        for (const auto& ext : it.extents) {
            if (indexSize > 0) {
                writeVarInt(newIlocBox, ext.index, indexSize);
            }
            uint64_t off = ext.offset;
            if (offSize > 0 && it.constructionMethod == 0 && (it.baseOffset + off) >= metaEnd) {
                off = static_cast<uint64_t>(static_cast<int64_t>(off) + metaGrowth);
            }
            writeVarInt(newIlocBox, off, newOffSize);
            writeVarInt(newIlocBox, ext.length, newLenSize);
        }
    }
    if (addingNewItem || !replacedHasExistingIloc) {
        appendPayloadItemLocation(newItemId);
    }

    // Assemble finalized meta box (always 8-byte standard header since meta fits well under 4GB)
    std::vector<uint8_t> newMetaBox;
    newMetaBox.reserve(newMetaBoxSize);
    writeU32(newMetaBox, static_cast<uint32_t>(newMetaBoxSize));
    newMetaBox.push_back('m'); newMetaBox.push_back('e'); newMetaBox.push_back('t'); newMetaBox.push_back('a');
    newMetaBox.insert(newMetaBox.end(), src.data() + metaPos + metaHeaderSize, src.data() + metaPos + metaHeaderSize + 4);
    newMetaBox.insert(newMetaBox.end(), newIlocBox.begin(), newIlocBox.end());
    newMetaBox.insert(newMetaBox.end(), newMetaSubboxes.begin(), newMetaSubboxes.end());

    // Assemble final output file
    std::vector<uint8_t> finalData;
    finalData.reserve(static_cast<size_t>(static_cast<int64_t>(src.size()) + metaGrowth + payload.size() + 16));
    finalData.insert(finalData.end(), src.data(), src.data() + metaPos);
    finalData.insert(finalData.end(), newMetaBox.begin(), newMetaBox.end());

    if (extendTrailingMdat) {
        finalData.insert(finalData.end(), src.data() + metaEnd, src.data() + lastMdatPos);
        const uint64_t newMdatSize64 = static_cast<uint64_t>(lastMdatSize) + payload.size();
        if (lastMdatHeaderSize == 16 || newMdatSize64 > 0xFFFFFFFFull) {
            writeU32(finalData, 1);
            finalData.push_back('m'); finalData.push_back('d'); finalData.push_back('a'); finalData.push_back('t');
            writeU64(finalData, newMdatSize64 + (lastMdatHeaderSize == 8 ? 8 : 0));
            // If upgrading 32->64 bit mdat header mid-flight shouldn't happen since file <=256MB
        } else {
            writeU32(finalData, static_cast<uint32_t>(newMdatSize64));
            finalData.push_back('m'); finalData.push_back('d'); finalData.push_back('a'); finalData.push_back('t');
        }
        finalData.insert(finalData.end(), src.data() + lastMdatPos + lastMdatHeaderSize, src.data() + lastMdatPos + lastMdatSize);
        finalData.insert(finalData.end(), payload.begin(), payload.end());
    } else {
        finalData.insert(finalData.end(), src.data() + metaEnd, src.data() + src.size());
        writeU32(finalData, static_cast<uint32_t>(8 + payload.size()));
        finalData.push_back('m'); finalData.push_back('d'); finalData.push_back('a'); finalData.push_back('t');
        finalData.insert(finalData.end(), payload.begin(), payload.end());
    }

    WriteStatus result = WriteStatus::Failed;
    WriteBufferAndReplace(path, L".qvrating.tmp", finalData, result);
    return result;
}

WriteStatus WriteRatingToJxl(const std::wstring& path, int stars, bool allowTranscode) {
    const int clampedStars = std::clamp(stars, 0, 5);
    std::vector<uint8_t> src;
    DWORD readErr = 0;
    if (!ReadWholeFile(path, 2, 256ull * 1024ull * 1024ull, src, &readErr)) {
        return IsFileLockError(readErr) ? WriteStatus::NeedsTranscode : WriteStatus::Failed;
    }

    auto readU32 = [](const uint8_t* b) -> uint32_t {
        return ReadBe32(b);
    };
    auto readU64 = [&readU32](const uint8_t* b) -> uint64_t {
        return (static_cast<uint64_t>(readU32(b)) << 32) | readU32(b + 4);
    };
    auto writeU32 = [](std::vector<uint8_t>& buf, uint32_t val) {
        AppendBe32(buf, val);
    };
    auto writeU64 = [](std::vector<uint8_t>& buf, uint64_t val) {
        for (int i = 7; i >= 0; --i) {
            buf.push_back(static_cast<uint8_t>((val >> (i * 8)) & 0xFF));
        }
    };
    auto appendJxlBox = [&](std::vector<uint8_t>& dst, const char type[4], std::span<const uint8_t> boxPayload) {
        if (boxPayload.size() + 8 <= 0xFFFFFFFFull) {
            writeU32(dst, static_cast<uint32_t>(boxPayload.size() + 8));
            dst.insert(dst.end(), type, type + 4);
        } else {
            writeU32(dst, 1);
            dst.insert(dst.end(), type, type + 4);
            writeU64(dst, static_cast<uint64_t>(boxPayload.size() + 16));
        }
        dst.insert(dst.end(), boxPayload.begin(), boxPayload.end());
    };
    auto appendStandardMetadataBoxes = [&](std::vector<uint8_t>& dst,
                                           std::span<const uint8_t> exifPayload,
                                           std::string_view xmpText) {
        if (!exifPayload.empty()) {
            appendJxlBox(dst, "Exif", exifPayload);
        }
        if (!xmpText.empty()) {
            appendJxlBox(dst, "xml ", std::span<const uint8_t>(
                reinterpret_cast<const uint8_t*>(xmpText.data()), xmpText.size()));
        }
    };

    // 1. Bare Codestream check (0xFF 0x0A)
    if (src[0] == 0xFF && src[1] == 0x0A) {
        if (clampedStars <= 0) {
            return WriteStatus::WrittenInPlace; // already unrated
        }
        if (!allowTranscode) {
            return WriteStatus::NeedsTranscode;
        }

        std::vector<uint8_t> container;
        container.reserve(src.size() + 4096);

        static constexpr uint8_t JXL_SIG[12] = {
            0x00, 0x00, 0x00, 0x0C, 'J', 'X', 'L', ' ', 0x0D, 0x0A, 0x87, 0x0A
        };
        static constexpr uint8_t JXL_FTYP[20] = {
            0x00, 0x00, 0x00, 0x14, 'f', 't', 'y', 'p',
            'j', 'x', 'l', ' ', 0x00, 0x00, 0x00, 0x00,
            'j', 'x', 'l', ' '
        };
        container.insert(container.end(), JXL_SIG, JXL_SIG + 12);
        container.insert(container.end(), JXL_FTYP, JXL_FTYP + 20);

        const auto exifPayload = BuildIsobmffExifPayload(clampedStars);
        const std::string xmpPayload = BuildStandardXmpPayload(clampedStars);
        appendStandardMetadataBoxes(container, exifPayload, xmpPayload);
        appendJxlBox(container, "jxlc", src);

        WriteStatus result = WriteStatus::Failed;
        WriteBufferAndReplace(path, L".tmp_jxl_rating", container, result);
        return result;
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

        if (bSize < hdrSize || bSize > src.size() - pos) break;

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

    // Inspect any brob metadata boxes so we don't mistakenly return WrittenInPlace when
    // compressed Exif/XMP carries a stale rating that overrides or survives clearing.
    bool hasBrobMetadata = false;
    std::vector<uint8_t> existingExifPayload;
    std::string existingXmpText;
    bool brobExifHasRating = false;
    bool brobXmpHasRating = false;
    bool uncompExifHasRating = false;
    bool uncompXmpNeedsRebuild = false;

    for (const auto& box : boxes) {
        if (box.type == 0x45786966 && existingExifPayload.empty() && box.payloadLen > 4) { // 'Exif'
            const std::span<const uint8_t> pay(src.data() + box.payloadPos, box.payloadLen);
            existingExifPayload.assign(pay.begin(), pay.end());
            if (ParseIsoExifPayload(pay).has_value()) uncompExifHasRating = true;
        } else if (box.type == 0x786D6C20 && existingXmpText.empty() && box.payloadLen > 0) { // 'xml '
            existingXmpText.assign(reinterpret_cast<const char*>(src.data() + box.payloadPos), box.payloadLen);
        } else if (box.type == 0x62726F62 && box.payloadLen > 4) { // 'brob'
            const uint32_t origType = readU32(src.data() + box.payloadPos);
            if (origType == 0x45786966 || origType == 0x786D6C20) {
                hasBrobMetadata = true;
                const auto decomp = DecompressBrotliBounded(
                    std::span<const uint8_t>(src.data() + box.payloadPos + 4, box.payloadLen - 4));
                if (origType == 0x45786966 && !decomp.empty()) {
                    if (existingExifPayload.empty()) existingExifPayload = decomp;
                    if (ParseIsoExifPayload(decomp).has_value()) brobExifHasRating = true;
                } else if (origType == 0x786D6C20 && !decomp.empty()) {
                    const std::string_view xmlView(reinterpret_cast<const char*>(decomp.data()), decomp.size());
                    if (existingXmpText.empty()) existingXmpText.assign(xmlView);
                    if (ParseXmpRating(xmlView).has_value()) brobXmpHasRating = true;
                }
            }
        }
    }

    // In-place path is safe only when no brob Exif/XMP shadows or blocks clearing/updating.
    bool inPlaceExifDone = !uncompExifHasRating && !brobExifHasRating;
    bool inPlaceXmpDone = !brobXmpHasRating;
    bool inPlaceTouched = false;

    for (const auto& box : boxes) {
        if (box.type == 0x45786966 && box.payloadLen > 4 && !brobExifHasRating) {
            if (PatchIsobmffExifInPlace(path, src, box.payloadPos, box.payloadLen, clampedStars)) {
                inPlaceExifDone = true;
                inPlaceTouched = true;
            } else if (ParseIsoExifPayload(std::span<const uint8_t>(src.data() + box.payloadPos, box.payloadLen)).has_value()) {
                inPlaceExifDone = false;
            }
        }
    }

    for (const auto& box : boxes) {
        if (box.type == 0x786D6C20 && box.payloadLen > 0 && !brobXmpHasRating && !brobExifHasRating) {
            const std::string_view xmpStr(reinterpret_cast<const char*>(src.data() + box.payloadPos), box.payloadLen);
            if (auto patched = PatchXmpRatingInPlaceStrict(xmpStr, clampedStars)) {
                if (*patched == xmpStr) {
                    inPlaceXmpDone = true;
                } else {
                    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (hFile != INVALID_HANDLE_VALUE) {
                        LARGE_INTEGER li;
                        li.QuadPart = static_cast<LONGLONG>(box.payloadPos);
                        DWORD written = 0;
                        const BOOL seekOk = SetFilePointerEx(hFile, li, nullptr, FILE_BEGIN);
                        const BOOL wOk = seekOk && WriteFile(hFile, patched->data(), static_cast<DWORD>(patched->size()), &written, nullptr);
                        if (wOk && written == patched->size()) {
                            FlushFileBuffers(hFile);
                            inPlaceXmpDone = true;
                            inPlaceTouched = true;
                        }
                        CloseHandle(hFile);
                    }
                }
                if (inPlaceXmpDone) break;
            } else if (clampedStars > 0 || ParseXmpRating(xmpStr).has_value()) {
                uncompXmpNeedsRebuild = true;
                inPlaceXmpDone = false;
            }
        }
    }

    const bool hasAnyUncompXmp = !uncompXmpNeedsRebuild &&
        std::any_of(boxes.begin(), boxes.end(), [](const JxlBox& b) { return b.type == 0x786D6C20 && b.payloadLen > 0; });

    if (clampedStars <= 0 && !brobExifHasRating && !brobXmpHasRating && !uncompXmpNeedsRebuild && !uncompExifHasRating) {
        return WriteStatus::WrittenInPlace;
    }
    if (!hasBrobMetadata && inPlaceTouched && inPlaceExifDone && (inPlaceXmpDone || hasAnyUncompXmp || !uncompXmpNeedsRebuild)) {
        return WriteStatus::WrittenInPlace;
    }

    if (!allowTranscode) {
        return WriteStatus::NeedsTranscode;
    }

    // Prepare rebuilt metadata boxes preserving existing non-rating Exif and XMP fields
    std::vector<uint8_t> finalExifPayload;
    if (!existingExifPayload.empty()) {
        // Attempt to patch existing decompressed Exif (0x4746/0x4749); if it had no 0x4746 tag,
        // keep camera Exif intact unless stars > 0 in which case padded XMP below carries the rating.
        if (existingExifPayload.size() > 4) {
            const uint32_t tiffOff = ReadBe32(existingExifPayload.data());
            size_t tiffStart = 4;
            if (tiffOff <= existingExifPayload.size() - 4 && existingExifPayload.size() - 4 - tiffOff >= 8) {
                tiffStart = 4 + tiffOff;
            }
            if (tiffStart + 8 <= existingExifPayload.size()) {
                PatchTiffBufferRating(
                    std::span<uint8_t>(existingExifPayload.data() + tiffStart, existingExifPayload.size() - tiffStart),
                    clampedStars);
            }
        }
        finalExifPayload = std::move(existingExifPayload);
    } else if (clampedStars > 0) {
        finalExifPayload = BuildIsobmffExifPayload(clampedStars);
    }

    std::string finalXmpPayload;
    if (!existingXmpText.empty()) {
        if (auto padded = BuildPaddedUpdatedXmp(existingXmpText, clampedStars)) {
            finalXmpPayload = std::move(*padded);
        }
    }
    if (finalXmpPayload.empty() && clampedStars > 0) {
        finalXmpPayload = BuildStandardXmpPayload(clampedStars);
    }

    std::vector<uint8_t> newContainer;
    newContainer.reserve(src.size() + finalExifPayload.size() + finalXmpPayload.size() + 256);
    newContainer.insert(newContainer.end(), JXL_SIG, JXL_SIG + 12);

    bool insertedMetadata = false;
    for (const auto& box : boxes) {
        if (box.type == 0x45786966 || box.type == 0x786D6C20) {
            continue;
        }
        if (box.type == 0x62726F62 && box.payloadLen >= 4) {
            const uint32_t origType = readU32(src.data() + box.payloadPos);
            if (origType == 0x45786966 || origType == 0x786D6C20) {
                continue;
            }
        }

        newContainer.insert(newContainer.end(), src.data() + box.boxPos, src.data() + box.boxPos + static_cast<size_t>(box.boxSize));

        if (!insertedMetadata && box.type == 0x66747970) { // after 'ftyp'
            appendStandardMetadataBoxes(newContainer, finalExifPayload, finalXmpPayload);
            insertedMetadata = true;
        }
    }

    if (!insertedMetadata) {
        appendStandardMetadataBoxes(newContainer, finalExifPayload, finalXmpPayload);
    }

    WriteStatus result = WriteStatus::Failed;
    WriteBufferAndReplace(path, L".tmp_jxl_rebuild", newContainer, result);
    return result;
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

std::vector<uint8_t> BuildPngITXtXmpChunkFromBody(std::string_view xmp) {
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

std::string DecompressZlibBounded(std::span<const uint8_t> comp, size_t maxOut = 4u * 1024u * 1024u) {
    if (comp.empty()) return {};
    z_stream zs{};
    if (inflateInit(&zs) != Z_OK) return {};
    zs.next_in = const_cast<Bytef*>(comp.data());
    zs.avail_in = static_cast<uInt>(comp.size());

    std::string out;
    out.resize((std::min)(maxOut, size_t{ 4096 }));
    size_t totalOut = 0;
    while (true) {
        if (totalOut >= out.size()) {
            if (out.size() >= maxOut) {
                inflateEnd(&zs);
                return {};
            }
            out.resize((std::min)(maxOut, out.size() * 2));
        }
        zs.next_out = reinterpret_cast<Bytef*>(out.data() + totalOut);
        zs.avail_out = static_cast<uInt>(out.size() - totalOut);
        const int rc = inflate(&zs, Z_NO_FLUSH);
        totalOut = zs.total_out;
        if (rc == Z_STREAM_END) {
            inflateEnd(&zs);
            out.resize(totalOut);
            return out;
        }
        if (rc != Z_OK && rc != Z_BUF_ERROR) {
            inflateEnd(&zs);
            return {};
        }
        if (zs.avail_in == 0 && zs.avail_out > 0) {
            inflateEnd(&zs);
            out.resize(totalOut);
            return out;
        }
    }
}

enum class PngITXtKind { NotXmp, UncompressedXmp, CompressedXmp, MalformedXmp };

PngITXtKind InspectPngITXt(std::span<const uint8_t> data, size_t& outXmpOffset, std::string* outDecodedXmp) {
    if (data.size() <= PNG_XMP_KEYWORD.size() ||
        std::memcmp(data.data(), PNG_XMP_KEYWORD.data(), PNG_XMP_KEYWORD.size()) != 0 ||
        data[PNG_XMP_KEYWORD.size()] != 0) {
        return PngITXtKind::NotXmp;
    }
    const size_t flagPos = PNG_XMP_KEYWORD.size() + 1;
    if (flagPos + 4 > data.size()) return PngITXtKind::MalformedXmp;
    const uint8_t compFlag = data[flagPos];
    size_t pos = flagPos + 2;
    for (int i = 0; i < 2; ++i) {
        const auto* nul = static_cast<const uint8_t*>(std::memchr(data.data() + pos, 0, data.size() - pos));
        if (!nul) return PngITXtKind::MalformedXmp;
        pos = static_cast<size_t>(nul - data.data()) + 1;
    }
    outXmpOffset = pos;
    if (compFlag == 0) {
        if (outDecodedXmp) {
            outDecodedXmp->assign(reinterpret_cast<const char*>(data.data() + pos), data.size() - pos);
        }
        return PngITXtKind::UncompressedXmp;
    }
    if (compFlag == 1 && outDecodedXmp) {
        *outDecodedXmp = DecompressZlibBounded(data.subspan(pos));
    }
    return PngITXtKind::CompressedXmp;
}

bool IsPngZTxtXmp(std::span<const uint8_t> data, std::string* outDecodedXmp) {
    if (data.size() <= PNG_XMP_KEYWORD.size() + 2 ||
        std::memcmp(data.data(), PNG_XMP_KEYWORD.data(), PNG_XMP_KEYWORD.size()) != 0 ||
        data[PNG_XMP_KEYWORD.size()] != 0) {
        return false;
    }
    if (outDecodedXmp) {
        *outDecodedXmp = DecompressZlibBounded(data.subspan(PNG_XMP_KEYWORD.size() + 2));
    }
    return true;
}

struct PngXmpChunkRecord {
    uint64_t chunkOffset = 0;
    uint64_t totalSize = 0;
    bool uncompressedITXt = false;
    size_t xmpOffsetInData = 0;
    std::vector<uint8_t> data;
    std::string decodedXmp;
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
                std::string decoded;
                const PngITXtKind kind = InspectPngITXt(data, xmpOff, &decoded);
                if (kind != PngITXtKind::NotXmp) {
                    PngXmpChunkRecord rec;
                    rec.chunkOffset = pos;
                    rec.totalSize = totalSize;
                    rec.uncompressedITXt = (kind == PngITXtKind::UncompressedXmp);
                    rec.xmpOffsetInData = xmpOff;
                    rec.data = std::move(data);
                    rec.decodedXmp = std::move(decoded);
                    out.xmpChunks.push_back(std::move(rec));
                }
            } else if (isZTxt) {
                std::string decoded;
                if (IsPngZTxtXmp(data, &decoded)) {
                    PngXmpChunkRecord rec;
                    rec.chunkOffset = pos;
                    rec.totalSize = totalSize;
                    rec.uncompressedITXt = false;
                    rec.decodedXmp = std::move(decoded);
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
        if (*patched != oldXmp) {
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

    std::string rebuiltXmp;
    for (const auto& xmp : scan.xmpChunks) {
        if (xmp.decodedXmp.empty()) continue;
        if (auto padded = BuildPaddedUpdatedXmp(xmp.decodedXmp, targetStars)) {
            if (targetStars > 0 ||
                padded->find("crs:") != std::string::npos ||
                padded->find("dc:") != std::string::npos ||
                padded->find("photoshop:") != std::string::npos ||
                padded->find("exif:") != std::string::npos) {
                rebuiltXmp = std::move(*padded);
            }
            break;
        }
    }
    if (rebuiltXmp.empty() && targetStars > 0) {
        rebuiltXmp = BuildStandardXmpPayload(targetStars);
    }

    const std::vector<uint8_t> newITXtChunk =
        !rebuiltXmp.empty() ? BuildPngITXtXmpChunkFromBody(rebuiltXmp) : std::vector<uint8_t>{};

    const std::wstring tempPath = CreateUniqueTempPath(path, L".tmp_png_rating");
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
    const int clampedStars = std::clamp(stars, 0, 5);

    const std::wstring_view ext = QuickView::ExtensionOf(path);
    const bool isJxl = QuickView::ExtEqualsIgnoreCase(ext, L".jxl");
    if (isJxl) {
        return WriteRatingToJxl(path, clampedStars, allowTranscode);
    }

    const bool isPng = QuickView::ExtEqualsIgnoreCase(ext, L".png") ||
                       QuickView::ExtEqualsIgnoreCase(ext, L".apng");
    if (isPng) {
        return WriteRatingToPng(path, clampedStars, allowTranscode);
    }

    const bool isWebp = QuickView::ExtEqualsIgnoreCase(ext, L".webp");
    if (isWebp) {
        if (!allowTranscode) return WriteStatus::NeedsTranscode;
        return WriteRatingToWebp(path, clampedStars);
    }

    const bool isIsobmff = QuickView::ExtEqualsIgnoreCase(ext, L".avif") ||
                           QuickView::ExtEqualsIgnoreCase(ext, L".avifs") ||
                           QuickView::ExtEqualsIgnoreCase(ext, L".heic") ||
                           QuickView::ExtEqualsIgnoreCase(ext, L".heif");
    if (isIsobmff) {
        return WriteRatingToIsobmff(path, clampedStars, allowTranscode);
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
        const HRESULT hrInPlace = TryInPlace(factory.Get(), path, clampedStars);
        if (SUCCEEDED(hrInPlace)) {
            return WriteStatus::WrittenInPlace;
        }
    }

    if (!allowTranscode) return WriteStatus::NeedsTranscode;

    return Transcode(factory.Get(), path, clampedStars);
}

} // namespace QuickView::Rating

