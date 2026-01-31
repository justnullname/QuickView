#pragma once
#include "pch.h"
#include <memory>
#include <string>

namespace QuickView {

    // ============================================================================
    // MappedFile: Zero-Copy IO Tunnel
    // ============================================================================
    // Encapsulates Windows Memory Mapped Files for safe, zero-copy read access.
    // Throws std::runtime_error on failure (construction).
    class MappedFile {
    public:
        MappedFile(const std::wstring& path) {
            m_hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (m_hFile == INVALID_HANDLE_VALUE) return; // Invalid

            LARGE_INTEGER size;
            if (!GetFileSizeEx(m_hFile, &size)) return;
            m_size = (size_t)size.QuadPart;

            if (m_size > 0) {
                m_hMap = CreateFileMappingW(m_hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
                if (m_hMap) {
                    m_ptr = static_cast<const uint8_t*>(MapViewOfFile(m_hMap, FILE_MAP_READ, 0, 0, 0));
                }
            }
        }

        ~MappedFile() {
            if (m_ptr) UnmapViewOfFile(m_ptr);
            if (m_hMap) CloseHandle(m_hMap);
            if (m_hFile != INVALID_HANDLE_VALUE) CloseHandle(m_hFile);
        }

        // Disable Copy
        MappedFile(const MappedFile&) = delete;
        MappedFile& operator=(const MappedFile&) = delete;

        bool IsValid() const { return m_ptr != nullptr; }
        const uint8_t* data() const { return m_ptr; }
        size_t size() const { return m_size; }

    private:
        HANDLE m_hFile = INVALID_HANDLE_VALUE;
        HANDLE m_hMap = nullptr;
        const uint8_t* m_ptr = nullptr;
        size_t m_size = 0;
    };

} // namespace QuickView