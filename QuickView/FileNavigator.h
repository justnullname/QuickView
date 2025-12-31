#pragma once
#include <string>
#include <vector>
#include <filesystem>
#include <algorithm>
#include <cwctype>

class FileNavigator {
public:
    void Initialize(const std::wstring& currentPath) {
        namespace fs = std::filesystem;
        fs::path p(currentPath);
        if (!fs::exists(p)) return;

        m_files.clear();
        m_currentIndex = -1;

        // Parent directory
        fs::path dir = p.parent_path();
        if (dir.empty()) return;

        // Supported extensions (comprehensive list including RAW formats)
        const std::vector<std::wstring> extensions = {
            // Standard formats
            L".jpg", L".jpeg", L".png", L".webp", L".bmp", L".gif", L".tif", L".tiff",
            // Modern formats  
            L".heic", L".heif", L".avif", L".jxl", L".exr", L".hdr",
            // RAW formats
            L".cr2", L".cr3", L".nef", L".arw", L".orf", L".rw2", L".dng", L".raf", L".pef", L".srw", L".raw"
        };

        try {
            m_sizes.clear();
            
            for (const auto& entry : fs::directory_iterator(dir)) {
                if (entry.is_regular_file()) {
                    std::wstring ext = entry.path().extension().wstring();
                    std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c){ return std::towlower(c); });
                    
                    for (const auto& supp : extensions) {
                        if (ext == supp) {
                            m_files.push_back(entry.path().wstring());
                            // Cache file size for Scout Lane decision
                            try {
                                m_sizes.push_back(entry.file_size());
                            } catch (...) {
                                m_sizes.push_back(0); // Error case
                            }
                            break;
                        }
                    }
                }
            }
        } catch (...) {}

        
        // Natural Sort would be better, but lexicon is fine for start
        // Fix: Sort indices or use struct to keep size synced with path
        struct Entry { std::wstring p; uintmax_t s; };
        std::vector<Entry> entries;
        entries.reserve(m_files.size());
        for(size_t i=0; i<m_files.size(); ++i) {
            entries.push_back({m_files[i], m_sizes[i]});
        }
        
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b){
            return a.p < b.p;
        });
        
        // Write back
        m_files.clear();
        m_sizes.clear();
        for(const auto& e : entries) {
            m_files.push_back(e.p);
            m_sizes.push_back(e.s);
        }

        // Find current index
        std::wstring currentFull = p.wstring();
        for (size_t i = 0; i < m_files.size(); ++i) {
            if (m_files[i] == currentFull) {
                m_currentIndex = (int)i;
                break;
            }
        }
    }

    std::wstring Next(bool loop = true) {
        if (m_files.empty()) return L"";
        if (!loop && m_currentIndex >= (int)m_files.size() - 1) {
            m_hitEnd = true;
            return L""; // At end, don't loop
        }
        m_hitEnd = false;
        m_currentIndex = (m_currentIndex + 1) % m_files.size();
        return m_files[m_currentIndex];
    }

    std::wstring Previous(bool loop = true) {
        if (m_files.empty()) return L"";
        if (!loop && m_currentIndex <= 0) {
            m_hitEnd = true;
            return L""; // At start, don't loop
        }
        m_hitEnd = false;
        m_currentIndex = (m_currentIndex - 1 + m_files.size()) % m_files.size();
        return m_files[m_currentIndex];
    }
    
    bool HitEnd() const { return m_hitEnd; }

    std::wstring PeekNext() const {
        if (m_files.empty()) return L"";
        size_t nextIdx = (m_currentIndex + 1) % m_files.size();
        return m_files[nextIdx];
    }

    std::wstring PeekPrevious() const {
        if (m_files.empty()) return L"";
        size_t prevIdx = (m_currentIndex - 1 + m_files.size()) % m_files.size();
        return m_files[prevIdx];
    }
    
    // Status info
    // Status info
    size_t Count() const { return m_files.size(); }
    int Index() const { return m_currentIndex; }

    // Random Access (For Gallery Virtualization)
    const std::wstring& GetFile(int index) const {
        static std::wstring empty;
        if (index < 0 || index >= (int)m_files.size()) return empty;
        return m_files[index];
    }

    int FindIndex(const std::wstring& path) const {
        auto it = std::find(m_files.begin(), m_files.end(), path);
        if (it != m_files.end()) return (int)std::distance(m_files.begin(), it);
        return -1;
    }

    const std::vector<std::wstring>& GetAllFiles() const { return m_files; }

    uintmax_t GetFileSize(int index) const {
        if (index < 0 || index >= (int)m_sizes.size()) return 0;
        return m_sizes[index];
    }

private:
    std::vector<std::wstring> m_files;
    std::vector<uintmax_t> m_sizes;
    int m_currentIndex = -1;
    bool m_hitEnd = false;
};
