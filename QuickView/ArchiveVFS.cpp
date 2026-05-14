#include "ArchiveVFS.h"
#include <algorithm>
#include <cstring>
#include <cwchar>

namespace QuickView {

    // Fast FNV-1a hash for UTF-8 bytes (Zero allocation string hashing)
    static uint16_t HashUtf8(const char* str, size_t len) {
        uint32_t hash = 2166136261u;
        for (size_t i = 0; i < len; ++i) {
            hash ^= (uint8_t)(str[i]);
            hash *= 16777619;
        }
        return (uint16_t)(hash & 0xFFFF);
    }

    ZipArchive::ZipArchive(const std::wstring& path) : m_mappedFile(path) {
        if (m_mappedFile.IsValid()) {
            m_valid = ParseCentralDirectory();
        }
    }

    bool ZipArchive::ParseCentralDirectory() {
        const uint8_t* data = m_mappedFile.data();
        size_t size = m_mappedFile.size();
        if (size < 22) return false; // Minimum size for EOCD

        // 1. Find EOCD (End of Central Directory) signature 0x06054b50
        // Search backwards from the end
        size_t eocdOffset = 0;
        bool foundEocd = false;

        // EOCD is at least 22 bytes, max comment length is 65535, so search max 65535+22 bytes
        size_t searchLimit = (size > 65557) ? 65557 : size;
        for (size_t i = 0; i < searchLimit - 21; ++i) {
            size_t p = size - 22 - i;
            if (data[p] == 0x50 && data[p+1] == 0x4b && data[p+2] == 0x05 && data[p+3] == 0x06) {
                eocdOffset = p;
                foundEocd = true;
                break;
            }
        }

        if (!foundEocd) return false;

        // 2. Parse EOCD to get Central Directory offset and size
        uint32_t cdOffset = *(uint32_t*)(data + eocdOffset + 16);
        uint16_t numEntries = *(uint16_t*)(data + eocdOffset + 10);

        if (cdOffset >= size) return false;

        // 3. Parse Central Directory records (Zero Allocation Parsing)
        m_entries.reserve(numEntries);

        size_t currentOffset = cdOffset;
        for (uint16_t i = 0; i < numEntries; ++i) {
            if (currentOffset + 46 > size) break; // CD header is at least 46 bytes

            // Check CD signature 0x02014b50
            if (data[currentOffset] != 0x50 || data[currentOffset+1] != 0x4b ||
                data[currentOffset+2] != 0x01 || data[currentOffset+3] != 0x02) {
                break;
            }

            uint16_t method = *(uint16_t*)(data + currentOffset + 10);
            uint32_t compSize = *(uint32_t*)(data + currentOffset + 20);
            uint32_t uncompSize = *(uint32_t*)(data + currentOffset + 24);
            uint16_t nameLen = *(uint16_t*)(data + currentOffset + 28);
            uint16_t extraLen = *(uint16_t*)(data + currentOffset + 30);
            uint16_t commentLen = *(uint16_t*)(data + currentOffset + 32);
            uint32_t headerOffset = *(uint32_t*)(data + currentOffset + 42);

            // Filter out directories instantly by looking at the last char of the name
            bool isFile = false;
            uint16_t nameHash = 0;
            if (nameLen > 0 && currentOffset + 46 + nameLen <= size) {
                const char* namePtr = (const char*)(data + currentOffset + 46);
                if (namePtr[nameLen - 1] != '/') {
                    isFile = true;
                    nameHash = HashUtf8(namePtr, nameLen);
                }
            }

            if (isFile && uncompSize > 0) {
                ArchiveEntry entry;
                entry.headerOffset = headerOffset;
                entry.compSize = compSize;
                entry.uncompSize = uncompSize;
                entry.nameHash = nameHash;
                entry.nameOffset = (uint16_t)46; // Offset relative to current CD record start
                entry.nameLen = nameLen;
                entry.method = method;

                m_entries.push_back(entry);
            }

            // Move to next CD record
            currentOffset += 46 + nameLen + extraLen + commentLen;
        }

        return !m_entries.empty();
    }

    std::wstring ZipArchive::GetEntryName(size_t index) const {
        if (index >= m_entries.size() || !m_mappedFile.IsValid()) return L"";

        const ArchiveEntry& entry = m_entries[index];
        const uint8_t* data = m_mappedFile.data();
        size_t size = m_mappedFile.size();

        // In the CD parsing phase, we just saved the string length. To actually get the string,
        // we need to re-find the CD record. Since we don't save the CD offset per entry to save memory,
        // we can actually just read the name from the Local File Header instead!
        // The Local File Header starts at entry.headerOffset.

        size_t lfhOffset = entry.headerOffset;
        if (lfhOffset + 30 > size) return L"";

        if (data[lfhOffset] != 0x50 || data[lfhOffset+1] != 0x4b ||
            data[lfhOffset+2] != 0x03 || data[lfhOffset+3] != 0x04) {
            return L"";
        }

        uint16_t nameLen = *(uint16_t*)(data + lfhOffset + 26);
        if (nameLen == 0 || lfhOffset + 30 + nameLen > size) return L"";

        const char* namePtr = (const char*)(data + lfhOffset + 30);

        // Lazy UTF-8 to UTF-16 conversion
        int size_needed = MultiByteToWideChar(CP_UTF8, 0, namePtr, (int)nameLen, NULL, 0);
        if (size_needed <= 0) return L"";

        std::wstring wstrTo(size_needed, 0);
        MultiByteToWideChar(CP_UTF8, 0, namePtr, (int)nameLen, &wstrTo[0], size_needed);
        return wstrTo;
    }

    bool ZipArchive::ExtractEntry(size_t index, uint8_t* externalBuffer, size_t bufferSize) const {
        if (index >= m_entries.size() || !m_mappedFile.IsValid() || !externalBuffer) return false;

        const ArchiveEntry& entry = m_entries[index];
        const uint8_t* data = m_mappedFile.data();
        size_t size = m_mappedFile.size();

        if (bufferSize < entry.uncompSize) return false;
        if (entry.headerOffset + 30 > size) return false;

        // Verify Local File Header signature 0x04034b50
        size_t lfhOffset = entry.headerOffset;
        if (data[lfhOffset] != 0x50 || data[lfhOffset+1] != 0x4b ||
            data[lfhOffset+2] != 0x03 || data[lfhOffset+3] != 0x04) {
            return false;
        }

        uint16_t nameLen = *(uint16_t*)(data + lfhOffset + 26);
        uint16_t extraLen = *(uint16_t*)(data + lfhOffset + 28);

        size_t payloadOffset = lfhOffset + 30 + nameLen + extraLen;

        // Safe integer overflow check
        if (payloadOffset > size || entry.compSize > size - payloadOffset) return false;

        if (entry.method == 0) {
            // Store (No compression)
            if (entry.compSize != entry.uncompSize) return false;
            std::memcpy(externalBuffer, data + payloadOffset, entry.uncompSize);
        } else if (entry.method == 8) {
            // Deflate
            z_stream strm = {};
            strm.next_in = (Bytef*)(data + payloadOffset);
            strm.avail_in = entry.compSize;
            strm.next_out = externalBuffer;
            strm.avail_out = entry.uncompSize;

            // -MAX_WBITS for raw deflate stream (no zlib header inside zip)
            if (inflateInit2(&strm, -MAX_WBITS) != Z_OK) return false;

            int ret = inflate(&strm, Z_FINISH);
            inflateEnd(&strm);

            if (ret != Z_STREAM_END && ret != Z_OK) return false;
        } else {
            // Unsupported compression method
            return false;
        }

        return true;
    }

}
