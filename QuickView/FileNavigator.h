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

        // Supported extensions (Basic list for now)
        const std::vector<std::wstring> extensions = { L".jpg", L".jpeg", L".png", L".webp", L".bmp", L".gif", L".heic", L".avif" };

        try {
            for (const auto& entry : fs::directory_iterator(dir)) {
                if (entry.is_regular_file()) {
                    std::wstring ext = entry.path().extension().wstring();
                    std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c){ return std::towlower(c); });
                    
                    for (const auto& supp : extensions) {
                        if (ext == supp) {
                            m_files.push_back(entry.path().wstring());
                            break;
                        }
                    }
                }
            }
        } catch (...) {}

        // Natural Sort would be better, but lexicon is fine for start
        std::sort(m_files.begin(), m_files.end());

        // Find current index
        std::wstring currentFull = p.wstring();
        for (size_t i = 0; i < m_files.size(); ++i) {
            if (m_files[i] == currentFull) {
                m_currentIndex = (int)i;
                break;
            }
        }
    }

    std::wstring Next() {
        if (m_files.empty()) return L"";
        m_currentIndex = (m_currentIndex + 1) % m_files.size();
        return m_files[m_currentIndex];
    }

    std::wstring Previous() {
        if (m_files.empty()) return L"";
        m_currentIndex = (m_currentIndex - 1 + m_files.size()) % m_files.size();
        return m_files[m_currentIndex];
    }

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


private:
    std::vector<std::wstring> m_files;
    int m_currentIndex = -1;
};
