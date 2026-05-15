#include "pch.h"
#include "ArchiveVFS.h"
#include "QuickViewETW.h"
#include "../third_party/unrar_core/rar.hpp"

static constexpr const char* CURRENT_MODULE = "ArchiveVFS";

namespace QuickView {

    static ::std::mutex g_archiveCacheMutex;
    static ::std::unordered_map<::std::wstring, ::std::shared_ptr<IArchive>> g_archiveCache;

    ::std::shared_ptr<IArchive> IArchive::OpenCached(const ::std::wstring& path) {
        ::std::lock_guard<::std::mutex> lock(g_archiveCacheMutex);
        auto it = g_archiveCache.find(path);
        if (it != g_archiveCache.end()) return it->second;

        if (g_archiveCache.size() >= 8) g_archiveCache.clear();

        ::std::wstring ext = ::std::filesystem::path(path).extension().wstring();
        ::std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return ::towlower(c); });

        ::std::shared_ptr<IArchive> res;
        if (ext == L".cbr" || ext == L".rar") res = ::std::make_shared<RarArchive>(path);
        else res = ::std::make_shared<ZipArchive>(path);

        if (res && res->IsValid()) {
            g_archiveCache[path] = res;
        }
        return res;
    }



    ZipArchive::ZipArchive(const ::std::wstring& path) : m_mappedFile(path) {
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
            if (nameLen > 0 && currentOffset + 46 + nameLen <= size) {
                const char* namePtr = (const char*)(data + currentOffset + 46);
                if (namePtr[nameLen - 1] != '/') {
                    isFile = true;
                }
            }

            if (isFile && uncompSize > 0) {
                ArchiveEntry entry;
                entry.headerOffset = headerOffset;
                entry.nameOffset = (uint32_t)(currentOffset + 46); // Absolute offset in mapped file
                entry.compSize = compSize;
                entry.uncompSize = uncompSize;
                entry.nameLen = nameLen;
                entry.method = method;

                m_entries.push_back(entry);
            }

            // Move to next CD record
            currentOffset += 46 + nameLen + extraLen + commentLen;
        }

        return !m_entries.empty();
    }

    ::std::wstring ZipArchive::GetEntryName(size_t index) const {
        if (index >= m_entries.size() || !m_mappedFile.IsValid()) return L"";

        const ArchiveEntry& entry = m_entries[index];
        const uint8_t* data = m_mappedFile.data();
        size_t size = m_mappedFile.size();

        if (entry.nameOffset + entry.nameLen > size) return L"";

        const char* namePtr = (const char*)(data + entry.nameOffset);

        // Lazy UTF-8 to UTF-16 conversion
        int size_needed = MultiByteToWideChar(CP_UTF8, 0, namePtr, (int)entry.nameLen, NULL, 0);
        if (size_needed <= 0) return L"";

        ::std::wstring wstrTo(size_needed, 0);
        MultiByteToWideChar(CP_UTF8, 0, namePtr, (int)entry.nameLen, &wstrTo[0], size_needed);
        return wstrTo;
    }

    ::std::string_view ZipArchive::GetEntryNameView(size_t index) const {
        if (index >= m_entries.size() || !m_mappedFile.IsValid()) return ::std::string_view();

        const ArchiveEntry& entry = m_entries[index];
        if (entry.nameOffset + entry.nameLen > m_mappedFile.size()) return std::string_view();

        return std::string_view((const char*)(m_mappedFile.data() + entry.nameOffset), entry.nameLen);
    }


    size_t ZipArchive::ExtractEntry(size_t index, uint8_t* externalBuffer, size_t bufferSize) const {
        if (index >= m_entries.size() || !m_mappedFile.IsValid() || !externalBuffer) return 0;

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

        return (size_t)entry.uncompSize;
    }

    // --- RarArchive Implementation ---

    RarArchive::SolidState::SolidState() {
        arc = new ::Archive();
        dataIO = new ::ComprDataIO();
    }

    RarArchive::SolidState::~SolidState() {
        delete unpack;
        delete dataIO;
        delete arc;
    }

    void RarArchive::SolidState::Reset(const uint8_t* data, size_t size) {
        delete unpack;
        unpack = nullptr;
        currentIndex = (size_t)-1;
        
        ((::File*)arc)->SetMemoryBuffer((const byte*)data, size);
        if (arc->IsArchive(true)) {
            dataIO->SetMemorySource(const_cast<uint8_t*>(data), size);
            dataIO->SetFiles((::File*)arc, nullptr);
            unpack = new ::Unpack(dataIO);
            // Dictionary is initialized in ExtractEntry
        }
    }

    RarArchive::RarArchive(const std::wstring& path) : m_mappedFile(path) {
        if (m_mappedFile.IsValid()) {
            m_valid = ParseArchive();
        }
    }

    RarArchive::~RarArchive() {
        // m_solidState unique_ptr handles cleanup
    }

    bool RarArchive::ParseArchive() {
        QV_LOG("ParseArchive_Start", TraceLoggingWideString(m_mappedFile.GetPath().c_str(), "Path"));
        ::Archive arc;
        static_cast<::File&>(arc).SetMemoryBuffer(const_cast<uint8_t*>(m_mappedFile.data()), m_mappedFile.size());
        
        if (!arc.IsArchive(true)) {
            QV_LOG("ParseArchive_NotArchive", TraceLoggingWideString(m_mappedFile.GetPath().c_str(), "Path"));
            return false;
        }
        
        QV_LOG("ParseArchive_Format", TraceLoggingInt32((int)arc.Format, "Format"));
        if (arc.Format == RARFMT_NONE) {
            QV_LOG("ParseArchive_None", TraceLoggingWideString(m_mappedFile.GetPath().c_str(), "Path"));
            return false;
        }
        
        while (arc.ReadHeader() > 0) {
            HEADER_TYPE type = arc.GetHeaderType();

            if (type == HEAD_FILE || type == HEAD3_FILE) {
                ArchiveEntry entry;
                std::wstring wideName = arc.FileHead.FileName;
                entry.headerOffset = (uint32_t)arc.CurBlockPos;
                entry.compSize = (uint32_t)arc.FileHead.PackSize;
                entry.uncompSize = (uint32_t)arc.FileHead.UnpSize;
                entry.method = (uint16_t)arc.FileHead.Method;
                
                // Convert FileName to UTF-8 and store in m_namesBuffer
                if (!wideName.empty()) {
                    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wideName.c_str(), (int)wideName.length(), nullptr, 0, nullptr, nullptr);
                    if (utf8Len > 0) {
                        entry.nameOffset = (uint32_t)m_namesBuffer.size();
                        entry.nameLen = (uint16_t)utf8Len;
                        m_namesBuffer.resize(m_namesBuffer.size() + utf8Len);
                        WideCharToMultiByte(CP_UTF8, 0, wideName.c_str(), (int)wideName.length(), &m_namesBuffer[entry.nameOffset], utf8Len, nullptr, nullptr);
                    } else {
                        entry.nameOffset = 0;
                        entry.nameLen = 0;
                    }
                } else {
                    entry.nameOffset = 0;
                    entry.nameLen = 0;
                }
                
                m_entries.push_back(entry);
            }
            arc.SeekToNext();
        }
        m_isSolid = arc.Solid;
        QV_LOG("ParseArchive_End", TraceLoggingUInt64(m_entries.size(), "Entries"), TraceLoggingBoolean(m_isSolid, "IsSolid"));
        return !m_entries.empty();
    }

    std::wstring RarArchive::GetEntryName(size_t index) const {
        if (index >= m_entries.size()) return L"";
        const ArchiveEntry& entry = m_entries[index];
        if (entry.nameLen == 0) return L"";

        std::string_view utf8Name(m_namesBuffer.data() + entry.nameOffset, entry.nameLen);
        int wideLen = MultiByteToWideChar(CP_UTF8, 0, utf8Name.data(), (int)utf8Name.length(), nullptr, 0);
        if (wideLen <= 0) return L"";

        ::std::wstring res(wideLen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8Name.data(), (int)utf8Name.length(), &res[0], wideLen);
        return res;
    }

    ::std::string_view RarArchive::GetEntryNameView(size_t index) const {
        if (index >= m_entries.size()) return ::std::string_view();
        const ArchiveEntry& entry = m_entries[index];
        if (entry.nameLen == 0) return ::std::string_view();
        return ::std::string_view(m_namesBuffer.data() + entry.nameOffset, entry.nameLen);
    }

    size_t RarArchive::ExtractEntry(size_t index, uint8_t* externalBuffer, size_t bufferSize) const {
        if (index >= m_entries.size() || !externalBuffer) return 0;
        
        // [Thread Safety] Ensure serial access for stateful decompression
        ::std::lock_guard<::std::mutex> lock(m_solidMutex);

        QV_LOG("ExtractEntry_Start", TraceLoggingUInt64(index, "Index"), TraceLoggingBoolean(m_isSolid, "IsSolid"));
        auto now = ::std::chrono::steady_clock::now();
        
        // --- Solid State Management ---
        if (!m_isSolid) {
            // Standard non-solid random access (Still use a local state or simplified path)
            ::Archive arc;
            static_cast<::File&>(arc).SetMemoryBuffer(const_cast<uint8_t*>(m_mappedFile.data()), m_mappedFile.size());
            if (!arc.IsArchive(true)) return 0;
            
            const ArchiveEntry& entry = m_entries[index];
            arc.Seek(entry.headerOffset, SEEK_SET);
            if (arc.ReadHeader() <= 0) return 0;

            ::ComprDataIO dataIO;
            dataIO.SetMemorySource(const_cast<uint8_t*>(m_mappedFile.data()), m_mappedFile.size());
            dataIO.SetMemoryDest(externalBuffer, bufferSize);
            dataIO.SetMemoryPos((size_t)arc.Tell());
            dataIO.SetPackedSize(m_mappedFile.size()); // [v6.0.8.2] No hard limit to allow look-ahead
            dataIO.SetFiles(&arc, nullptr);

            if (arc.FileHead.Method == 0) {
                dataIO.UnpWrite(const_cast<uint8_t*>(m_mappedFile.data()) + arc.Tell(), arc.FileHead.UnpSize);
            } else {
                ::Unpack unpack(&dataIO);
                unpack.Init(arc.FileHead.WinSize, arc.FileHead.Solid);
                unpack.SetDestSize(arc.FileHead.UnpSize);
                unpack.DoUnpack(arc.FileHead.UnpVer, arc.FileHead.Solid);
            }
            size_t written = (size_t)dataIO.GetWrittenSize();
            QV_LOG("ExtractEntry_End", TraceLoggingUInt64(index, "Index"), TraceLoggingBoolean(written > 0, "Success"), TraceLoggingUInt64(written, "Written"));
            return written;
        }

        // --- Solid Archive Logic (Stateful & Single-Context) ---
        if (!m_solidState) {
            m_solidState = std::make_unique<SolidState>();
            m_solidState->Reset(m_mappedFile.data(), m_mappedFile.size());
        }

        SolidState& ss = *m_solidState;

        // 2. Check for stale state or backward jump
        if (index < ss.currentIndex + 1) {
            ss.Reset(m_mappedFile.data(), m_mappedFile.size());
        } else if (std::chrono::duration_cast<std::chrono::seconds>(now - ss.lastAccessed).count() > 60) {
            // Timeout cleanup (60s)
            ss.Reset(m_mappedFile.data(), m_mappedFile.size());
        }

        ss.lastAccessed = now;
        ::Archive& arc = *ss.arc;
        ::ComprDataIO& dataIO = *ss.dataIO;
        ::Unpack& unpack = *ss.unpack;

        const size_t MAX_SKIP_ENTRIES = 10000; // [v6.0.7] Support very large archives
        size_t skippedCount = 0;

        // 3. Sequential Skip / Fast Skip
        size_t nextToRead = ss.currentIndex + 1;
        
        while (nextToRead <= index) {
            if (++skippedCount > MAX_SKIP_ENTRIES) {
                ss.Reset(m_mappedFile.data(), m_mappedFile.size());
                return 0;
            }

            if (arc.ReadHeader() <= 0) return 0;
            HEADER_TYPE type = arc.GetHeaderType();
            
            if (type == HEAD_FILE || type == HEAD3_FILE) {
                if (nextToRead == index) {
                    // Extract goal
                    dataIO.SetMemoryDest(externalBuffer, bufferSize);
                    dataIO.SetMemoryPos((size_t)arc.Tell());
                    dataIO.SetPackedSizeToRead(m_mappedFile.size()); // [v6.0.8.2] No hard limit to allow look-ahead

                    if (arc.FileHead.Method == 0) {
                        dataIO.UnpWrite(const_cast<uint8_t*>(m_mappedFile.data()) + arc.Tell(), arc.FileHead.UnpSize);
                    } else {
                        unpack.Init(arc.FileHead.WinSize, arc.FileHead.Solid);
                        unpack.SetDestSize(arc.FileHead.UnpSize);
                        unpack.DoUnpack(arc.FileHead.UnpVer, arc.FileHead.Solid);
                    }
                    ss.currentIndex = index;
                    arc.SeekToNext(); 
                    size_t written = (size_t)dataIO.GetWrittenSize();
                    QV_LOG("ExtractEntry_End", TraceLoggingUInt64(index, "Index"), TraceLoggingBoolean(written > 0, "Success"), TraceLoggingUInt64(written, "Written"));
                    return written;
                } else {
                    // Fast Skip intermediate
                    dataIO.SetMemoryDest(nullptr, 0); 
                    dataIO.SetMemoryPos((size_t)arc.Tell());
                    dataIO.SetPackedSizeToRead(m_mappedFile.size()); // [v6.0.8.2] No hard limit to allow look-ahead

                    if (arc.FileHead.Method == 0) {
                        arc.Seek(arc.FileHead.PackSize, SEEK_CUR);
                    } else {
                        unpack.Init(arc.FileHead.WinSize, arc.FileHead.Solid);
                        unpack.SetDestSize(arc.FileHead.UnpSize);
                        unpack.DoUnpack(arc.FileHead.UnpVer, arc.FileHead.Solid);
                    }
                    nextToRead++;
                    ss.currentIndex++; 
                }
            }
            
            arc.SeekToNext();
        }

        return 0;
    }

    void RarArchive::PurgeState() const {
        ::std::lock_guard<::std::mutex> lock(m_solidMutex);
        m_solidState.reset();
    }


}
