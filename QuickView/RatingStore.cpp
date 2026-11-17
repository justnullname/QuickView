/*
 * QuickView Star Ratings - isolated rating cache and background reader
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

#include "RatingStore.h"

#include "RatingWriter.h"
#include "SupportedExtensions.h"

#include <algorithm>
#include <vector>

namespace {

// A rating sits in the file header, so a small prefix is all that is ever
// read -- the whole point is to stay off the decode pipeline.
constexpr DWORD HEADER_READ_BYTES = 128 * 1024;
constexpr DWORD SIDECAR_READ_BYTES = 16 * 1024;

// Long enough that holding a digit or running 1-3-5 lands one write,
// short enough that the file is up to date by the time the user looks.
constexpr auto WRITE_DEBOUNCE = std::chrono::milliseconds(400);

// Delay before safely writing a file that requires transcode/rebuild while
// still being displayed on screen. Gives the user enough time to settle
// without infinite postponement.
constexpr auto RESIDENT_TRANSCODE_DELAY = std::chrono::milliseconds(1500);

// An update has to preserve the whole sidecar, so writing reads all of it,
// unlike the prefix that display needs. Anything larger than this is not a
// rating sidecar and is left alone.
constexpr DWORD MAX_SIDECAR_BYTES = 4 * 1024 * 1024;

// How long after writing a file a directory-change notification is assumed to
// be the echo of that write. Kept reasonably small to avoid missing external file ops.
constexpr auto SELF_WRITE_ECHO = std::chrono::milliseconds(600);
std::atomic<int64_t> g_lastSelfWriteTick{ 0 };

// Wide-char file API throughout: a path round-tripped through a narrow code
// page fails to open on non-ASCII names.
std::vector<uint8_t> ReadFilePrefix(const std::wstring& path, DWORD maxBytes) {
    std::vector<uint8_t> buffer;
    if (path.empty()) return buffer;

    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return buffer;

    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0) {
        const DWORD toRead = (size.QuadPart < (LONGLONG)maxBytes)
                                 ? (DWORD)size.QuadPart
                                 : maxBytes;
        buffer.resize(toRead);
        DWORD read = 0;
        if (!ReadFile(file, buffer.data(), toRead, &read, nullptr)) {
            buffer.clear();
        } else {
            buffer.resize(read);
        }
    }
    CloseHandle(file);
    return buffer;
}

} // namespace

RatingStore::~RatingStore() {
    Shutdown();
}

void RatingStore::Initialize(HWND hwnd) {
    if (m_running.load()) return;
    m_hwnd = hwnd;
    m_running = true;
    m_worker = std::thread(&RatingStore::WorkerLoop, this);
    m_writeWorker = std::thread(&RatingStore::WriteLoop, this);
}

void RatingStore::Shutdown() {
    if (!m_running.exchange(false)) return;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_queue.clear();
        m_pending.clear();
    }
    m_cv.notify_all();
    if (m_worker.joinable()) m_worker.join();

    {
        std::lock_guard<std::mutex> writeLock(m_writeMutex);
    }
    m_writeCv.notify_all();
    if (m_writeWorker.joinable()) m_writeWorker.join();
}

std::wstring RatingStore::SidecarPathFor(const std::wstring& path) {
    const std::wstring_view ext = QuickView::ExtensionOf(path);
    if (ext.empty()) return std::wstring();
    return path.substr(0, path.size() - ext.size()) + L".xmp";
}

std::optional<int> RatingStore::ReadRatingFromSidecar(const std::wstring& sidecarPath) {
    const std::vector<uint8_t> bytes = ReadFilePrefix(sidecarPath, SIDECAR_READ_BYTES);
    if (bytes.empty()) return std::nullopt;
    return QuickView::Rating::ParseXmpRating(
        std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

namespace {

bool IsInFileRatableExtension(std::wstring_view ext) {
    return QuickView::ExtEqualsIgnoreCase(ext, L".jpg") ||
           QuickView::ExtEqualsIgnoreCase(ext, L".jpeg") ||
           QuickView::ExtEqualsIgnoreCase(ext, L".jpe") ||
           QuickView::ExtEqualsIgnoreCase(ext, L".jfif") ||
           QuickView::ExtEqualsIgnoreCase(ext, L".tif") ||
           QuickView::ExtEqualsIgnoreCase(ext, L".tiff") ||
           QuickView::ExtEqualsIgnoreCase(ext, L".png") ||
           QuickView::ExtEqualsIgnoreCase(ext, L".apng") ||
           QuickView::ExtEqualsIgnoreCase(ext, L".webp") ||
           QuickView::ExtEqualsIgnoreCase(ext, L".avif") ||
           QuickView::ExtEqualsIgnoreCase(ext, L".avifs") ||
           QuickView::ExtEqualsIgnoreCase(ext, L".heic") ||
           QuickView::ExtEqualsIgnoreCase(ext, L".heif") ||
           QuickView::ExtEqualsIgnoreCase(ext, L".jxl");
}

bool SeekHandleAbsolute(HANDLE file, uint64_t offset) {
    if (offset > static_cast<uint64_t>(INT64_MAX)) return false;
    LARGE_INTEGER li{};
    li.QuadPart = static_cast<LONGLONG>(offset);
    return SetFilePointerEx(file, li, nullptr, FILE_BEGIN) != FALSE;
}

bool ReadExactHandleBytes(HANDLE file, void* dst, size_t bytes) {
    auto* out = static_cast<uint8_t*>(dst);
    size_t offset = 0;
    while (offset < bytes) {
        const DWORD chunk = static_cast<DWORD>((std::min)(bytes - offset, static_cast<size_t>(0x40000000u)));
        DWORD read = 0;
        if (!ReadFile(file, out + offset, chunk, &read, nullptr) || read == 0) {
            return false;
        }
        offset += read;
    }
    return true;
}

bool ReadHandleSpan(HANDLE file, uint64_t offset, uint64_t length, uint64_t fileSize,
                    size_t maxBytes, std::vector<uint8_t>& out) {
    if (length == 0 || length > maxBytes) return false;
    if (offset > fileSize || length > fileSize - offset) return false;
    if (!SeekHandleAbsolute(file, offset)) return false;
    out.resize(static_cast<size_t>(length));
    if (!ReadExactHandleBytes(file, out.data(), out.size())) {
        out.clear();
        return false;
    }
    return true;
}

std::optional<int> ReadTiffRatingDirect(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::nullopt;

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(file, &fileSize) || fileSize.QuadPart < 8) {
        CloseHandle(file);
        return std::nullopt;
    }

    uint8_t header[8];
    if (!ReadExactHandleBytes(file, header, 8)) {
        CloseHandle(file);
        return std::nullopt;
    }

    bool isLE = false;
    if (header[0] == 'I' && header[1] == 'I' && header[2] == 42 && header[3] == 0) {
        isLE = true;
    } else if (header[0] == 'M' && header[1] == 'M' && header[2] == 0 && header[3] == 42) {
        isLE = false;
    } else {
        CloseHandle(file);
        return std::nullopt;
    }

    auto read16 = [isLE](const uint8_t* b) -> uint16_t {
        return isLE ? (uint16_t)(b[0] | (b[1] << 8)) : (uint16_t)((b[0] << 8) | b[1]);
    };
    auto read32 = [isLE](const uint8_t* b) -> uint32_t {
        return isLE ? (uint32_t)(b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24))
                    : (uint32_t)((b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]);
    };

    const uint32_t ifdOffset = read32(header + 4);
    if (ifdOffset < 8 || (LONGLONG)ifdOffset + 2 > fileSize.QuadPart ||
        !SeekHandleAbsolute(file, ifdOffset)) {
        CloseHandle(file);
        return std::nullopt;
    }

    uint8_t countBytes[2];
    if (!ReadExactHandleBytes(file, countBytes, 2)) {
        CloseHandle(file);
        return std::nullopt;
    }

    const uint16_t entryCount = read16(countBytes);
    if (entryCount == 0 || entryCount > 4096 ||
        (LONGLONG)ifdOffset + 2 + (LONGLONG)entryCount * 12 > fileSize.QuadPart) {
        CloseHandle(file);
        return std::nullopt;
    }

    std::vector<uint8_t> entries(static_cast<size_t>(entryCount) * 12);
    const bool readOk = ReadExactHandleBytes(file, entries.data(), entries.size());
    CloseHandle(file);
    if (!readOk) return std::nullopt;

    constexpr uint16_t TAG_SIMPLE_RATING = 0x4746;
    for (uint16_t i = 0; i < entryCount; ++i) {
        const uint8_t* e = entries.data() + static_cast<size_t>(i) * 12;
        if (read16(e) != TAG_SIMPLE_RATING) continue;
        return QuickView::Rating::DecodeTiffRatingEntry(
            read16(e + 2), read32(e + 4), std::span<const uint8_t>(e + 8, 4), !isLE);
    }

    return std::nullopt;
}

std::optional<int> ReadWebpRatingDirect(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::nullopt;

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(file, &fileSize) || fileSize.QuadPart < 12) {
        CloseHandle(file);
        return std::nullopt;
    }
    const uint64_t totalBytes = static_cast<uint64_t>(fileSize.QuadPart);

    // Read initial 30 bytes: 12 bytes RIFF header + up to 18 bytes for first chunk (VP8X)
    uint8_t initHeader[30];
    DWORD read = 0;
    const DWORD toRead = static_cast<DWORD>((std::min)(fileSize.QuadPart, (LONGLONG)30));
    if (!ReadFile(file, initHeader, toRead, &read, nullptr) || read < 12) {
        CloseHandle(file);
        return std::nullopt;
    }

    if (std::memcmp(initHeader, "RIFF", 4) != 0 || std::memcmp(initHeader + 8, "WEBP", 4) != 0) {
        CloseHandle(file);
        return std::nullopt;
    }

    // Check if the first chunk is VP8X (Extended WebP)
    bool allowExif = true;
    bool allowXmp = true;
    if (read >= 21 && std::memcmp(initHeader + 12, "VP8X", 4) == 0) {
        const uint8_t flags = initHeader[20];
        allowXmp = (flags & 0x04) != 0;
        allowExif = (flags & 0x08) != 0;
        if (!allowXmp && !allowExif) {
            CloseHandle(file);
            return std::nullopt;
        }
    } else if (read >= 16 && (std::memcmp(initHeader + 12, "VP8 ", 4) == 0 ||
                              std::memcmp(initHeader + 12, "VP8L", 4) == 0)) {
        // Simple WebP without VP8X header contains no metadata chunks
        CloseHandle(file);
        return std::nullopt;
    }

    // Stream through RIFF chunks: EXIF takes precedence over XMP, matching ParseWebpRating.
    std::optional<int> fromXmp;
    uint64_t pos = 12;

    while (pos + 8 <= totalBytes) {
        if (!SeekHandleAbsolute(file, pos)) break;
        uint8_t chunkHdr[8];
        if (!ReadExactHandleBytes(file, chunkHdr, 8)) break;

        const uint32_t chunkLen = static_cast<uint32_t>(chunkHdr[4]) |
                                  (static_cast<uint32_t>(chunkHdr[5]) << 8) |
                                  (static_cast<uint32_t>(chunkHdr[6]) << 16) |
                                  (static_cast<uint32_t>(chunkHdr[7]) << 24);
        const uint64_t dataPos = pos + 8;
        if (chunkLen > totalBytes - dataPos) break;

        if (allowExif && std::memcmp(chunkHdr, "EXIF", 4) == 0 && chunkLen > 8) {
            const size_t exifReadLen = (std::min)(static_cast<size_t>(chunkLen), size_t{ 2u * 1024u * 1024u });
            std::vector<uint8_t> exifBuf(exifReadLen);
            if (ReadExactHandleBytes(file, exifBuf.data(), exifReadLen)) {
                size_t tiffOff = 0;
                if (exifReadLen >= 6 && std::memcmp(exifBuf.data(), "Exif\0\0", 6) == 0) {
                    tiffOff = 6;
                }
                if (exifReadLen > tiffOff + 8) {
                    if (auto r = QuickView::Rating::ParseTiffRating(
                            std::span<const uint8_t>(exifBuf.data() + tiffOff, exifReadLen - tiffOff))) {
                        CloseHandle(file);
                        return r;
                    }
                }
            }
        } else if (allowXmp && !fromXmp && std::memcmp(chunkHdr, "XMP ", 4) == 0 && chunkLen > 0) {
            const size_t xmpReadLen = (std::min)(static_cast<size_t>(chunkLen), size_t{ 1024u * 1024u });
            std::string xmpBuf(xmpReadLen, '\0');
            if (ReadExactHandleBytes(file, xmpBuf.data(), xmpReadLen)) {
                fromXmp = QuickView::Rating::ParseXmpRating(xmpBuf);
            }
        }

        const uint64_t advance = static_cast<uint64_t>(chunkLen) + (chunkLen & 1u);
        if (8 + advance > totalBytes - pos) break;
        pos += 8 + advance;
    }

    CloseHandle(file);
    return fromXmp;
}

uint32_t ReadBe32Bytes(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

uint64_t ReadBe64Bytes(const uint8_t* p) {
    return (static_cast<uint64_t>(ReadBe32Bytes(p)) << 32) | ReadBe32Bytes(p + 4);
}

std::optional<int> ReadIsobmffRatingDirect(const std::wstring& path) {
    const std::vector<uint8_t> prefix = ReadFilePrefix(path, HEADER_READ_BYTES);
    if (prefix.empty()) return std::nullopt;

    if (auto rating = QuickView::Rating::ParseIsobmffRating(prefix)) {
        return rating;
    }

    // If metadata resides after mdat or past the initial 128KB prefix, stream only
    // box headers to locate 'meta' and read just the metadata box + item extents.
    if (prefix.size() >= 12 && std::memcmp(prefix.data() + 4, "ftyp", 4) == 0) {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE) return std::nullopt;

        LARGE_INTEGER fileSizeLi{};
        if (!GetFileSizeEx(file, &fileSizeLi) || fileSizeLi.QuadPart <= (LONGLONG)HEADER_READ_BYTES) {
            CloseHandle(file);
            return std::nullopt;
        }
        const uint64_t fileSize = static_cast<uint64_t>(fileSizeLi.QuadPart);

        uint64_t pos = 0;
        uint64_t metaPos = 0, metaLen = 0;
        while (pos + 8 <= fileSize) {
            if (!SeekHandleAbsolute(file, pos)) break;
            uint8_t hdr[16];
            if (!ReadExactHandleBytes(file, hdr, 8)) break;

            uint64_t boxSize = ReadBe32Bytes(hdr);
            size_t hdrSize = 8;
            if (boxSize == 1) {
                if (pos + 16 > fileSize || !ReadExactHandleBytes(file, hdr + 8, 8)) break;
                boxSize = ReadBe64Bytes(hdr + 8);
                hdrSize = 16;
            } else if (boxSize == 0) {
                boxSize = fileSize - pos;
            }
            if (boxSize < hdrSize || boxSize > fileSize - pos) break;

            if (std::memcmp(hdr + 4, "meta", 4) == 0) {
                metaPos = pos;
                metaLen = boxSize;
                break;
            }
            pos += boxSize;
        }

        std::vector<uint8_t> metaBuf;
        QuickView::Rating::IsobmffMetaLocations loc{};
        if (metaLen >= 12 && ReadHandleSpan(file, metaPos, metaLen, fileSize, 8u * 1024u * 1024u, metaBuf) &&
            QuickView::Rating::ParseIsobmffMetaLocations(metaBuf, loc)) {
            auto loadItem = [&](uint64_t offset, uint64_t length, bool inIdat,
                                std::vector<uint8_t>& storage) -> std::span<const uint8_t> {
                if (length == 0 || length > 4u * 1024u * 1024u) return {};
                if (inIdat) {
                    if (offset > loc.idatLength || length > loc.idatLength - offset) return {};
                    return std::span<const uint8_t>(
                        metaBuf.data() + static_cast<size_t>(loc.idatOffsetInMeta + offset),
                        static_cast<size_t>(length));
                }
                if (offset < prefix.size() && length <= prefix.size() - offset) {
                    return std::span<const uint8_t>(prefix.data() + static_cast<size_t>(offset),
                                                    static_cast<size_t>(length));
                }
                if (ReadHandleSpan(file, offset, length, fileSize, 4u * 1024u * 1024u, storage)) {
                    return storage;
                }
                return {};
            };

            std::vector<uint8_t> itemBuf;
            if (loc.hasExif) {
                const auto exifSpan = loadItem(loc.exifOffset, loc.exifLength, loc.exifInIdat, itemBuf);
                if (auto r = QuickView::Rating::ParseIsoExifPayload(exifSpan)) {
                    CloseHandle(file);
                    return r;
                }
            }
            if (loc.hasXmp) {
                const auto xmpSpan = loadItem(loc.xmpOffset, loc.xmpLength, loc.xmpInIdat, itemBuf);
                if (!xmpSpan.empty()) {
                    if (auto r = QuickView::Rating::ParseXmpRating(
                            std::string_view(reinterpret_cast<const char*>(xmpSpan.data()), xmpSpan.size()))) {
                        CloseHandle(file);
                        return r;
                    }
                }
            }
        }
        CloseHandle(file);
    }

    return std::nullopt;
}

std::optional<int> ReadJxlRatingDirect(const std::wstring& path) {
    const std::vector<uint8_t> prefix = ReadFilePrefix(path, HEADER_READ_BYTES);
    if (prefix.empty()) return std::nullopt;

    if (prefix.size() >= 2 && prefix[0] == 0xFF && prefix[1] == 0x0A) {
        return std::nullopt; // Bare codestreams carry no metadata
    }

    if (auto rating = QuickView::Rating::ParseJxlRating(prefix)) {
        return rating;
    }

    static constexpr uint8_t JXL_SIG[12] = {
        0x00, 0x00, 0x00, 0x0C, 'J', 'X', 'L', ' ', 0x0D, 0x0A, 0x87, 0x0A
    };
    if (prefix.size() >= 12 && std::memcmp(prefix.data(), JXL_SIG, 12) == 0) {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE) return std::nullopt;

        LARGE_INTEGER fileSizeLi{};
        if (!GetFileSizeEx(file, &fileSizeLi) || fileSizeLi.QuadPart <= (LONGLONG)HEADER_READ_BYTES) {
            CloseHandle(file);
            return std::nullopt;
        }
        const uint64_t fileSize = static_cast<uint64_t>(fileSizeLi.QuadPart);

        std::optional<int> fromXmp;
        uint64_t pos = 12;
        std::vector<uint8_t> boxBuf;

        while (pos + 8 <= fileSize) {
            if (!SeekHandleAbsolute(file, pos)) break;
            uint8_t hdr[16];
            if (!ReadExactHandleBytes(file, hdr, 8)) break;

            uint64_t boxSize = ReadBe32Bytes(hdr);
            size_t hdrSize = 8;
            if (boxSize == 1) {
                if (pos + 16 > fileSize || !ReadExactHandleBytes(file, hdr + 8, 8)) break;
                boxSize = ReadBe64Bytes(hdr + 8);
                hdrSize = 16;
            } else if (boxSize == 0) {
                boxSize = fileSize - pos;
            }
            if (boxSize < hdrSize || boxSize > fileSize - pos) break;

            const uint64_t payloadOff = pos + hdrSize;
            const uint64_t payloadLen = boxSize - hdrSize;
            const bool isExif = (std::memcmp(hdr + 4, "Exif", 4) == 0);
            const bool isXml = (std::memcmp(hdr + 4, "xml ", 4) == 0);
            const bool isBrob = (std::memcmp(hdr + 4, "brob", 4) == 0);

            if ((isExif || (isXml && !fromXmp) || isBrob) && payloadLen > 4 &&
                ReadHandleSpan(file, payloadOff, payloadLen, fileSize, 4u * 1024u * 1024u, boxBuf)) {
                if (isExif) {
                    if (auto r = QuickView::Rating::ParseIsoExifPayload(boxBuf)) {
                        CloseHandle(file);
                        return r;
                    }
                } else if (isXml) {
                    fromXmp = QuickView::Rating::ParseXmpRating(
                        std::string_view(reinterpret_cast<const char*>(boxBuf.data()), boxBuf.size()));
                } else if (isBrob && boxBuf.size() > 4) {
                    const uint8_t* origType = boxBuf.data();
                    const std::span<const uint8_t> comp(boxBuf.data() + 4, boxBuf.size() - 4);
                    if (std::memcmp(origType, "Exif", 4) == 0) {
                        const auto decomp = QuickView::Rating::DecompressBrotliBounded(comp);
                        if (auto r = QuickView::Rating::ParseIsoExifPayload(decomp)) {
                            CloseHandle(file);
                            return r;
                        }
                    } else if (std::memcmp(origType, "xml ", 4) == 0 && !fromXmp) {
                        const auto decomp = QuickView::Rating::DecompressBrotliBounded(comp);
                        if (!decomp.empty()) {
                            fromXmp = QuickView::Rating::ParseXmpRating(
                                std::string_view(reinterpret_cast<const char*>(decomp.data()), decomp.size()));
                        }
                    }
                }
            }

            pos += boxSize;
        }
        CloseHandle(file);
        return fromXmp;
    }

    return std::nullopt;
}

} // namespace

std::optional<int> RatingStore::ReadRatingFromFile(const std::wstring& path) {
    const std::wstring_view ext = QuickView::ExtensionOf(path);
    if (!IsInFileRatableExtension(ext)) return std::nullopt;

    const bool isTiff = QuickView::ExtEqualsIgnoreCase(ext, L".tif") ||
                        QuickView::ExtEqualsIgnoreCase(ext, L".tiff");
    const bool isPng = QuickView::ExtEqualsIgnoreCase(ext, L".png") ||
                       QuickView::ExtEqualsIgnoreCase(ext, L".apng");
    const bool isWebp = QuickView::ExtEqualsIgnoreCase(ext, L".webp");
    const bool isIsobmff = QuickView::ExtEqualsIgnoreCase(ext, L".avif") ||
                           QuickView::ExtEqualsIgnoreCase(ext, L".avifs") ||
                           QuickView::ExtEqualsIgnoreCase(ext, L".heic") ||
                           QuickView::ExtEqualsIgnoreCase(ext, L".heif");
    const bool isJxl = QuickView::ExtEqualsIgnoreCase(ext, L".jxl");

    if (isTiff) {
        // An IFD in a real-world TIFF can reside anywhere, very frequently placed at the
        // end of the file (after all megabytes of pixel strips). Reading only the first
        // 128KB fails for virtually all multi-megabyte TIFFs.
        return ReadTiffRatingDirect(path);
    }

    if (isWebp) {
        // WebP metadata chunks (especially XMP) often follow large pixel chunks (VP8/VP8L).
        // Stream directly through chunk headers rather than blindly reading a 128KB prefix.
        return ReadWebpRatingDirect(path);
    }

    if (isIsobmff) {
        return ReadIsobmffRatingDirect(path);
    }

    if (isJxl) {
        return ReadJxlRatingDirect(path);
    }

    const std::vector<uint8_t> bytes = ReadFilePrefix(path, HEADER_READ_BYTES);
    if (bytes.empty()) return std::nullopt;
    if (isPng) {
        return QuickView::Rating::ParsePngRating(bytes);
    }
    return QuickView::Rating::ParseJpegRating(bytes);
}


RatingStore::Writability RatingStore::GetWritability(const std::wstring& renderedPath,
                                                    const std::wstring& rawPath) {
    if (renderedPath.find(L'|') != std::wstring::npos) {
        return Writability::UnsupportedFormat; // entry inside an archive
    }

    const std::wstring_view ext = QuickView::ExtensionOf(renderedPath);
    const bool inFileRatable = IsInFileRatableExtension(ext);
    const bool usesSidecarOnly = !rawPath.empty() || QuickView::IsRawPath(renderedPath);
    if (!inFileRatable && !usesSidecarOnly) {
        return Writability::UnsupportedFormat;
    }

    // Paired RAW+rendered views and standalone RAWs write their sidecar next to the RAW/file.
    // A read-only camera RAW itself must remain rateable as long as its .xmp sidecar is writable.
    if (usesSidecarOnly) {
        const std::wstring& sidecarOwner = rawPath.empty() ? renderedPath : rawPath;
        const std::wstring sidecarPath = SidecarPathFor(sidecarOwner);
        if (!sidecarPath.empty()) {
            const DWORD scAttrs = GetFileAttributesW(sidecarPath.c_str());
            if (scAttrs != INVALID_FILE_ATTRIBUTES && (scAttrs & FILE_ATTRIBUTE_READONLY)) {
                return Writability::ReadOnlyFile;
            }
        }
        return Writability::Writable;
    }

    const DWORD attrs = GetFileAttributesW(renderedPath.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY)) {
        return Writability::ReadOnlyFile;
    }
    return Writability::Writable;
}

QuickView::Rating::Resolved RatingStore::ApplyRatingOptimistic(
    ImageID id, int stars, const std::wstring& renderedPath, const std::wstring& rawPath,
    bool isResident) {
    QuickView::Rating::Resolved resolved;
    resolved.stars = stars;
    // A rating set here is the user's, so it is authoritative on both carriers
    // and there is no disagreement left to report.
    resolved.source = rawPath.empty() ? QuickView::Rating::Source::InFile
                                      : QuickView::Rating::Source::Sidecar;
    if (stars == 0) resolved.source = QuickView::Rating::Source::None;

    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        m_cache[id] = resolved;
    }

    {
        // Replacing the entry is what makes a burst of keypresses collapse
        // into one write: only the last value survives to reach the disk.
        std::lock_guard<std::mutex> lock(m_writeMutex);
        PendingWrite& pending = m_pendingWrites[id];
        pending.id = id;
        pending.stars = stars;
        pending.renderedPath = renderedPath;
        pending.rawPath = rawPath;
        pending.resident = isResident;
        pending.due = std::chrono::steady_clock::now() + WRITE_DEBOUNCE;
    }
    m_writeCv.notify_one();
    return resolved;
}

void RatingStore::ReleaseResident(const std::wstring& nowResidentPath) {
    bool woke = false;
    {
        std::lock_guard<std::mutex> lock(m_writeMutex);
        for (auto& [id, pending] : m_pendingWrites) {
            if (pending.renderedPath != nowResidentPath) {
                pending.resident = false; // free to rebuild the file now
                pending.due = std::chrono::steady_clock::now();
                woke = true;
            }
        }
    }
    if (woke) m_writeCv.notify_one();
}

void RatingStore::FlushPendingWrites() {
    std::vector<PendingWrite> due;
    {
        std::lock_guard<std::mutex> lock(m_writeMutex);
        for (auto& [id, pending] : m_pendingWrites) {
            pending.resident = false; // nothing is on screen any more
            due.push_back(pending);
        }
        m_pendingWrites.clear();
    }
    for (const auto& write : due) PerformWrite(write);
}

void RatingStore::NotifySelfWrite() {
    g_lastSelfWriteTick.store(
        std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_relaxed);
}

bool RatingStore::WasSelfWriteJustNow() {
    const int64_t ticks = g_lastSelfWriteTick.load(std::memory_order_relaxed);
    if (ticks == 0) return false;
    const auto last = std::chrono::steady_clock::time_point(
        std::chrono::steady_clock::duration(ticks));
    return (std::chrono::steady_clock::now() - last) < SELF_WRITE_ECHO;
}

bool RatingStore::WriteSidecar(const std::wstring& ownerPath, int stars, bool allowCreate) {
    const std::wstring sidecarPath = SidecarPathFor(ownerPath);
    if (sidecarPath.empty()) return false;

    // Distinguish missing sidecar (safe to create if allowCreate) from an existing
    // sidecar that cannot be read cleanly (e.g. sharing lock from external editor).
    bool fileExists = false;
    std::string existing;
    {
        HANDLE hRead = INVALID_HANDLE_VALUE;
        DWORD lastErr = 0;
        for (int attempt = 0; attempt < 5; ++attempt) {
            hRead = CreateFileW(sidecarPath.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (hRead != INVALID_HANDLE_VALUE) break;
            lastErr = GetLastError();
            if (lastErr == ERROR_FILE_NOT_FOUND || lastErr == ERROR_PATH_NOT_FOUND) break;
            if (attempt + 1 < 5) Sleep(15);
        }

        if (hRead == INVALID_HANDLE_VALUE) {
            if (lastErr != ERROR_FILE_NOT_FOUND && lastErr != ERROR_PATH_NOT_FOUND) {
                return false; // Locked or inaccessible existing sidecar: never overwrite.
            }
        } else {
            fileExists = true;
            LARGE_INTEGER size{};
            if (!GetFileSizeEx(hRead, &size) || size.QuadPart < 0 || size.QuadPart > (LONGLONG)MAX_SIDECAR_BYTES) {
                CloseHandle(hRead);
                return false;
            }
            if (size.QuadPart > 0) {
                existing.resize(static_cast<size_t>(size.QuadPart));
                const bool readOk = ReadExactHandleBytes(hRead, existing.data(), existing.size());
                CloseHandle(hRead);
                if (!readOk) return false;
            } else {
                CloseHandle(hRead);
            }
        }
    }

    std::string updated;
    if (!fileExists) {
        if (!allowCreate) return true;                          // update-only owner
        if (stars <= QuickView::Rating::MIN_STARS) return true; // nothing to write
        updated = QuickView::Rating::BuildMinimalXmp(stars);
    } else if (existing.empty()) {
        if (!allowCreate) return false;
        if (stars <= QuickView::Rating::MIN_STARS) return true;
        updated = QuickView::Rating::BuildMinimalXmp(stars);
    } else {
        const auto edited = QuickView::Rating::UpdateXmpRating(existing, stars);
        if (!edited) {
            // The document is not shaped as expected. Someone else's develop
            // settings are worth more than this rating, so leave it alone.
            return false;
        }
        if (*edited == existing) return true; // already says what we want
        updated = *edited;
    }

    // Write through a unique temp file so a failure cannot leave a half-written
    // sidecar or collide with delete-pending handles.
    static std::atomic<uint64_t> s_tmpSeq{ 0 };
    const std::wstring tempPath =
        sidecarPath + L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
        std::to_wstring(s_tmpSeq.fetch_add(1, std::memory_order_relaxed)) + L".qvtmp";
    {
        HANDLE file = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        const BOOL ok = WriteFile(file, updated.data(), (DWORD)updated.size(), &written, nullptr);
        FlushFileBuffers(file);
        CloseHandle(file);
        if (!ok || written != updated.size()) {
            DeleteFileW(tempPath.c_str());
            return false;
        }
    }

    NotifySelfWrite(); // our own change; the watcher should not rescan for it

    if (!fileExists) {
        if (!MoveFileExW(tempPath.c_str(), sidecarPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileW(tempPath.c_str());
            return false;
        }
        return true;
    }
    for (int retry = 0; retry < 5; ++retry) {
        if (ReplaceFileW(sidecarPath.c_str(), tempPath.c_str(), nullptr,
                         REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) {
            return true;
        }
        if (retry + 1 < 5) Sleep(20);
    }
    DeleteFileW(tempPath.c_str());
    return false;
}

void RatingStore::PerformWrite(const PendingWrite& write) {
    if (write.renderedPath.empty()) return;

    const ImageID targetId = (write.id != 0)
        ? write.id
        : FileNavigator::PathToImageID(write.rawPath.empty() ? write.renderedPath : write.rawPath);

    auto rollbackCacheFromDisk = [&]() {
        const std::optional<int> inFile = ReadRatingFromFile(write.renderedPath);
        const std::wstring owner = write.rawPath.empty() ? write.renderedPath : write.rawPath;
        const std::optional<int> sidecar = ReadRatingFromSidecar(SidecarPathFor(owner));
        const auto diskResolved = QuickView::Rating::ResolvePairRating(inFile, sidecar);

        bool updated = false;
        {
            std::scoped_lock lock(m_writeMutex, m_cacheMutex);
            if (m_pendingWrites.find(targetId) == m_pendingWrites.end()) {
                m_cache[targetId] = diskResolved;
                updated = true;
            }
        }
        if (updated && m_hwnd) {
            PostMessageW(m_hwnd, WM_RATING_READY, (WPARAM)targetId, 0);
        }
    };

    // The sidecar goes first, because it is the side that wins when the two
    // disagree: if the in-file write then fails, what the user sees is still
    // the rating they set, rather than the old one coming back.
    const std::wstring sidecarOwner =
        write.rawPath.empty() ? write.renderedPath : write.rawPath;
    const bool ownerIsRaw = QuickView::IsRawPath(sidecarOwner);
    const bool sidecarExists =
        GetFileAttributesW(SidecarPathFor(sidecarOwner).c_str()) != INVALID_FILE_ATTRIBUTES;
    if (ownerIsRaw || sidecarExists) {
        if (!WriteSidecar(sidecarOwner, write.stars, /*allowCreate*/ ownerIsRaw)) {
            rollbackCacheFromDisk();
            return; // refused: leave the in-file half alone and sync UI with disk
        }
    }

    // A standalone RAW has no in-file half to write.
    if (QuickView::IsRawPath(write.renderedPath)) return;

    const auto status = QuickView::Rating::WriteRatingToImage(
        write.renderedPath, write.stars, /*allowTranscode*/ !write.resident);

    if (status == QuickView::Rating::WriteStatus::WrittenInPlace ||
        status == QuickView::Rating::WriteStatus::WrittenTranscode) {
        NotifySelfWrite();
    } else if (status == QuickView::Rating::WriteStatus::NeedsTranscode) {
        std::lock_guard<std::mutex> lock(m_writeMutex);
        if (m_pendingWrites.find(targetId) == m_pendingWrites.end()) {
            auto& pending = m_pendingWrites[targetId];
            pending = write;
            pending.id = targetId;
            pending.due = write.resident
                ? std::chrono::steady_clock::time_point::max()
                : (std::chrono::steady_clock::now() + RESIDENT_TRANSCODE_DELAY);
            m_writeCv.notify_one();
        }
    } else if (status == QuickView::Rating::WriteStatus::Failed) {
        if (!ownerIsRaw && !sidecarExists) {
            rollbackCacheFromDisk();
        }
    }
}

void RatingStore::WriteLoop() {
    // WIC is COM, and this thread owns its own apartment: without this every
    // write would fail at CoCreateInstance.
    const HRESULT comInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool ownsCom = SUCCEEDED(comInit);

    struct ComScope {
        bool owns;
        ~ComScope() { if (owns) CoUninitialize(); }
    } comScope{ ownsCom };

    while (true) {
        std::vector<PendingWrite> due;
        {
            std::unique_lock<std::mutex> lock(m_writeMutex);
            if (m_pendingWrites.empty()) {
                m_writeCv.wait(lock, [this] { return !m_running.load() || !m_pendingWrites.empty(); });
            } else {
                // Sleep exactly until the next entry is due rather than on a
                // fixed tick; an entry waiting for its photo to leave the
                // screen has no deadline and must not cause a wakeup at all.
                auto earliest = std::chrono::steady_clock::time_point::max();
                for (const auto& [id, pending] : m_pendingWrites) {
                    earliest = (std::min)(earliest, pending.due);
                }
                if (earliest == std::chrono::steady_clock::time_point::max()) {
                    m_writeCv.wait(lock);
                } else {
                    m_writeCv.wait_until(lock, earliest);
                }
            }
            if (!m_running.load()) break;

            const auto now = std::chrono::steady_clock::now();
            for (auto it = m_pendingWrites.begin(); it != m_pendingWrites.end();) {
                if (it->second.due <= now) {
                    due.push_back(it->second);
                    it = m_pendingWrites.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (const auto& write : due) PerformWrite(write);
    }

    FlushPendingWrites();
}

std::optional<QuickView::Rating::Resolved> RatingStore::TryGet(ImageID id) const {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    auto it = m_cache.find(id);
    if (it == m_cache.end()) return std::nullopt;
    return it->second;
}

std::optional<QuickView::Rating::Resolved> RatingStore::TryGetOrQueueRead(
    ImageID id, const std::wstring& renderedPath, std::wstring_view rawPath) {
    {
        std::lock_guard<std::mutex> cacheLock(m_cacheMutex);
        auto it = m_cache.find(id);
        if (it != m_cache.end()) return it->second;
    }
    if (renderedPath.empty()) return std::nullopt;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (!m_pending.insert(id).second) return std::nullopt;
        m_queue.push_back(Task{ id, renderedPath, std::wstring(rawPath), m_generation.load() });
    }
    m_cv.notify_one();
    return std::nullopt;
}

void RatingStore::QueueRead(ImageID id, const std::wstring& renderedPath,
                            const std::wstring& rawPath) {
    // Deliberately not gated on m_running: the first image can be navigated to
    // before Initialize runs, and such a request must still be honoured once
    // the worker starts rather than being dropped.
    if (renderedPath.empty()) return;
    {
        std::lock_guard<std::mutex> cacheLock(m_cacheMutex);
        if (m_cache.find(id) != m_cache.end()) return; // already known
    }
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (!m_pending.insert(id).second) return;      // already queued
        m_queue.push_back(Task{ id, renderedPath, rawPath, m_generation.load() });
    }
    m_cv.notify_one();
}

void RatingStore::Clear() {
    ++m_generation; // invalidates results still in flight
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_queue.clear();
        m_pending.clear();
    }
    std::lock_guard<std::mutex> cacheLock(m_cacheMutex);
    m_cache.clear();
}

void RatingStore::WorkerLoop() {
    while (true) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_cv.wait(lock, [this] { return !m_running.load() || !m_queue.empty(); });
            if (!m_running.load()) return;
            task = std::move(m_queue.front());
            m_queue.pop_front();
        }

        // A rating can live in the file itself (JPEG/TIFF) and in the sidecar
        // of the paired RAW. A standalone RAW has no in-file rating, so only
        // its own sidecar is probed.
        const std::optional<int> inFile = ReadRatingFromFile(task.renderedPath);
        const std::wstring sidecarOwner = task.rawPath.empty() ? task.renderedPath : task.rawPath;
        const std::optional<int> sidecar = ReadRatingFromSidecar(SidecarPathFor(sidecarOwner));

        const auto resolved = QuickView::Rating::ResolvePairRating(inFile, sidecar);

        bool committed = false;
        {
            std::scoped_lock lock(m_cacheMutex, m_queueMutex);
            if (task.generation != m_generation.load()) continue;
            // Erase from pending even if cache already holds an optimistic rating.
            m_pending.erase(task.id);
            if (m_cache.find(task.id) == m_cache.end()) {
                m_cache[task.id] = resolved;
                committed = true;
            }
        }
        if (committed && m_hwnd) {
            PostMessageW(m_hwnd, WM_RATING_READY, (WPARAM)task.id, 0);
        }
    }
}

