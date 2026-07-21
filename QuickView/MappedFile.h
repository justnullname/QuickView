#pragma once
#include "pch.h"
#include <memory>
#include <string>
#include <cstdint>
#include <vector>

namespace QuickView {

    // ============================================================================
    // MappedFile: Zero-Copy IO Tunnel
    // ============================================================================
    // Encapsulates Windows Memory Mapped Files for safe, zero-copy read access.
    // Throws std::runtime_error on failure (construction).
    class MappedFile {
    public:
        MappedFile(const std::wstring& path) : m_path(path) {
            // [BugFix] Support FILE_SHARE_DELETE so that background/temporary files deleted by 
            // the sending processes don't trigger access denied during open.
            m_hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (m_hFile == INVALID_HANDLE_VALUE) return; // Invalid

            LARGE_INTEGER size;
            if (!GetFileSizeEx(m_hFile, &size)) return;
            m_size = (size_t)size.QuadPart;

            if (m_size > 0) {
                // [Stability Optimization] For files under 128MB (99.9% of normal PNGs/JPEGs),
                // we load them entirely into memory vector to prevent STATUS_IN_PAGE_ERROR (0xc0000006) 
                // caused by asynchronous file locking/deletion by antivirus, DLP or compressor software.
                if (m_size < 128 * 1024 * 1024) {
                    m_buffer.resize(m_size);
                    DWORD bytesRead = 0;
                    if (ReadFile(m_hFile, m_buffer.data(), (DWORD)m_size, &bytesRead, nullptr) && bytesRead == m_size) {
                        m_ptr = m_buffer.data();
                    } else {
                        m_buffer.clear();
                        m_size = 0;
                        m_ptr = nullptr;
                    }
                } else {
                    // For massive files (>128MB), map them to preserve address space & RAM
                    m_hMap = CreateFileMappingW(m_hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
                    if (m_hMap) {
                        m_ptr = static_cast<const uint8_t*>(MapViewOfFile(m_hMap, FILE_MAP_READ, 0, 0, 0));
                    }
                }
            }
        }

        ~MappedFile() {
            if (m_ptr && m_buffer.empty()) UnmapViewOfFile(m_ptr);
            if (m_hMap) CloseHandle(m_hMap);
            if (m_hFile != INVALID_HANDLE_VALUE) CloseHandle(m_hFile);
        }

        // Disable Copy
        MappedFile(const MappedFile&) = delete;
        MappedFile& operator=(const MappedFile&) = delete;

        bool IsValid() const { return m_ptr != nullptr; }
        const uint8_t* data() const { return m_ptr; }
        size_t size() const { return m_size; }
        const std::wstring& GetPath() const { return m_path; }

    public:
        void Prefetch(size_t offset, size_t length) {
            // If already fully in memory, prefetch is a no-op
            if (!m_buffer.empty()) return;
            
            if (!m_ptr || m_size == 0) return;
            if (offset >= m_size) return;
            
            size_t validLen = length;
            if (offset + validLen > m_size) validLen = m_size - offset;

            // Define function pointer type for PrefetchVirtualMemory
            typedef BOOL (WINAPI *PfnPrefetchVirtualMemory)(HANDLE, ULONG_PTR, PWIN32_MEMORY_RANGE_ENTRY, ULONG);
            
            static PfnPrefetchVirtualMemory pPrefetch = []() -> PfnPrefetchVirtualMemory {
                HMODULE hKernel = GetModuleHandleW(L"kernel32.dll");
                if (hKernel) {
                    return (PfnPrefetchVirtualMemory)GetProcAddress(hKernel, "PrefetchVirtualMemory");
                }
                return nullptr;
            }();

            if (pPrefetch) {
                WIN32_MEMORY_RANGE_ENTRY entry;
                entry.VirtualAddress = (void*)(m_ptr + offset);
                entry.NumberOfBytes = validLen;
                pPrefetch(GetCurrentProcess(), 1, &entry, 0);
            }
        }
        
    private:
        HANDLE m_hFile = INVALID_HANDLE_VALUE;
        HANDLE m_hMap = nullptr;
        const uint8_t* m_ptr = nullptr;
        size_t m_size = 0;
        std::wstring m_path;
        std::vector<uint8_t> m_buffer; // Buffered content for small files
    };

} // namespace QuickView
