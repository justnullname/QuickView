#pragma once
#include <string>
#include <vector>
#include <filesystem>
#include <algorithm>
#include <cwctype>
#include <functional>  // for std::hash
#include <Shlwapi.h>   // for StrCmpLogicalW
#include "EditState.h" // for g_runtime
#include "exif.h"      // for easyexif

#pragma comment(lib, "Shlwapi.lib")

// [ImageID Architecture] Stable content-based unique identifier
using ImageID = size_t;  // 64-bit path hash

// Helper: Compute normalized path hash (case-insensitive for Windows)
inline ImageID ComputePathHash(const std::wstring& path) {
    std::wstring normalized = path;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), ::towlower);
    return std::hash<std::wstring>{}(normalized);
}

class FileNavigator {
public:
    void Initialize(const std::wstring& currentPath) {
        namespace fs = std::filesystem;
        fs::path p(currentPath);
        if (!fs::exists(p)) return;

        m_files.clear();
        m_currentIndex = -1;

        const bool isDirectory = fs::is_directory(p);

        // If a directory is passed in, scan it directly. Otherwise scan the parent directory.
        fs::path dir = isDirectory ? p : p.parent_path();
        if (dir.empty()) return;

        // Supported extensions (comprehensive list including RAW formats)
        // Supported extensions (comprehensive list including RAW formats)
        const std::vector<std::wstring> extensions = {
            // Standard
            L".jpg", L".jpeg", L".jpe", L".jfif", L".png", L".bmp", L".dib", L".gif", 
            L".tif", L".tiff", L".ico", 
            // Web / Modern
            L".webp", L".avif", L".heic", L".heif", L".svg", L".svgz", L".jxl",
            // Professional / HDR / Legacy
            L".exr", L".hdr", L".pic", L".psd", L".psb", L".tga", L".pcx", L".qoi", 
            L".wbmp", L".pam", L".pbm", L".pgm", L".ppm", L".wdp", L".hdp", L".jxr", L".hif",
            // RAW Formats (LibRaw supported)
            L".arw", L".cr2", L".cr3", L".dng", L".nef", L".orf", L".raf", L".rw2", L".srw", L".x3f",
            L".mrw", L".mos", L".kdc", L".dcr", L".sr2", L".pef", L".erf", L".3fr", L".mef", L".nrw"
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

        
        struct Entry {
            std::wstring p;
            uintmax_t s;
            std::filesystem::file_time_type m;
            std::wstring t; // type (extension)
            std::string exifDate; // EXIF DateTaken
        };

        std::vector<Entry> entries;
        entries.reserve(m_files.size());
        namespace fs = std::filesystem;
        for(size_t i=0; i<m_files.size(); ++i) {
            Entry e;
            e.p = m_files[i];
            e.s = m_sizes[i];
            try {
                e.m = fs::last_write_time(e.p);
            } catch (...) {}

            e.t = fs::path(e.p).extension().wstring();
            std::transform(e.t.begin(), e.t.end(), e.t.begin(), [](wchar_t c){ return std::towlower(c); });

            // Only parse EXIF date if specifically requested (to save load time)
            if (g_runtime.SortOrder == 3) {
                 FILE *fp = _wfopen(e.p.c_str(), L"rb");
                 if (fp) {
                     unsigned char buf[65536];
                     size_t bytes = fread(buf, 1, sizeof(buf), fp);
                     fclose(fp);
                     if (bytes > 0) {
                         easyexif::EXIFInfo info;
                         if (info.parseFrom(buf, (unsigned)bytes) == PARSE_EXIF_SUCCESS) {
                             e.exifDate = info.DateTimeOriginal;
                         }
                     }
                 }
            }

            entries.push_back(e);
        }
        
        int sortOrder = g_runtime.SortOrder;
        bool sortDesc = g_runtime.SortDescending;

        std::sort(entries.begin(), entries.end(), [sortOrder, sortDesc](const Entry& a, const Entry& b){
            int cmp = 0;
            switch (sortOrder) {
                case 1: // Name
                case 0: // Auto (Use Name Natural Sort)
                    cmp = StrCmpLogicalW(a.p.c_str(), b.p.c_str());
                    break;
                case 2: // Modified
                    if (a.m < b.m) cmp = -1;
                    else if (a.m > b.m) cmp = 1;
                    else cmp = StrCmpLogicalW(a.p.c_str(), b.p.c_str()); // Fallback
                    break;
                case 3: // Date Taken
                    if (a.exifDate.empty() && !b.exifDate.empty()) cmp = 1; // Empty goes last
                    else if (!a.exifDate.empty() && b.exifDate.empty()) cmp = -1;
                    else {
                        cmp = a.exifDate.compare(b.exifDate);
                        if (cmp == 0) cmp = StrCmpLogicalW(a.p.c_str(), b.p.c_str());
                    }
                    break;
                case 4: // Size
                    if (a.s < b.s) cmp = -1;
                    else if (a.s > b.s) cmp = 1;
                    else cmp = StrCmpLogicalW(a.p.c_str(), b.p.c_str());
                    break;
                case 5: // Type
                    cmp = StrCmpLogicalW(a.t.c_str(), b.t.c_str());
                    if (cmp == 0) cmp = StrCmpLogicalW(a.p.c_str(), b.p.c_str());
                    break;
            }

            if (sortDesc) return cmp > 0;
            return cmp < 0;
        });
        
        // Write back
        m_files.clear();
        m_sizes.clear();
        for(const auto& e : entries) {
            m_files.push_back(e.p);
            m_sizes.push_back(e.s);
        }
        
        // [ImageID] Compute stable hash IDs for all files
        m_ids.clear();
        m_ids.reserve(m_files.size());
        for (const auto& f : m_files) {
            m_ids.push_back(ComputePathHash(f));
        }

        // Find current index
        if (!isDirectory) {
            std::wstring currentFull = p.wstring();
            for (size_t i = 0; i < m_files.size(); ++i) {
                if (m_files[i] == currentFull) {
                    m_currentIndex = (int)i;
                    break;
                }
            }
        }
    }

    // Legacy support: We ignore `loop` and strictly use NavLoopMode instead
    std::wstring Next(bool unused = true) {
        if (m_files.empty()) return L"";

        if (m_currentIndex >= (int)m_files.size() - 1) {
            if (g_runtime.NavTraverse) {
                // Through folders
                std::wstring nextFolderImg = FindAdjacentFolderImage(true);
                if (!nextFolderImg.empty()) {
                    m_crossFolderMessage = L">>> Entering [" + std::filesystem::path(nextFolderImg).parent_path().filename().wstring() + L"] >>>";
                    return nextFolderImg;
                }
            }

            if (g_runtime.NavLoop) {
                // Loop
                m_hitEnd = true; // Signal OSD
                m_currentIndex = 0;
                return m_files[m_currentIndex];
            } else {
                // Stop at end
                m_hitEnd = true;
                return L"";
            }
        }

        m_hitEnd = false;
        m_currentIndex++;
        return m_files[m_currentIndex];
    }

    std::wstring Previous(bool unused = true) {
        if (m_files.empty()) return L"";

        if (m_currentIndex <= 0) {
            if (g_runtime.NavTraverse) {
                // Through folders
                std::wstring prevFolderImg = FindAdjacentFolderImage(false);
                if (!prevFolderImg.empty()) {
                    m_crossFolderMessage = L"<<< Entering [" + std::filesystem::path(prevFolderImg).parent_path().filename().wstring() + L"] <<<";
                    return prevFolderImg;
                }
            }

            if (g_runtime.NavLoop) {
                // Loop
                m_hitEnd = true; // Signal OSD
                m_currentIndex = (int)m_files.size() - 1;
                return m_files[m_currentIndex];
            } else {
                // Stop at start
                m_hitEnd = true;
                return L"";
            }
        }

        m_hitEnd = false;
        m_currentIndex--;
        return m_files[m_currentIndex];
    }

    std::wstring First() {
        if (m_files.empty()) return L"";
        m_hitEnd = false;
        m_currentIndex = 0;
        return m_files[m_currentIndex];
    }

    std::wstring Last() {
        if (m_files.empty()) return L"";
        m_hitEnd = false;
        m_currentIndex = (int)m_files.size() - 1;
        return m_files[m_currentIndex];
    }
    
    bool HitEnd() const { return m_hitEnd; }

    std::wstring GetCrossFolderMessage() {
        std::wstring msg = m_crossFolderMessage;
        m_crossFolderMessage.clear(); // Consume
        return msg;
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
    
    // [Fix] Refresh metadata for current file (e.g. after Save)
    void Refresh() {
        if (m_currentIndex >= 0 && m_currentIndex < (int)m_files.size()) {
            try {
                namespace fs = std::filesystem;
                m_sizes[m_currentIndex] = fs::file_size(m_files[m_currentIndex]);
            } catch (...) {
                m_sizes[m_currentIndex] = 0;
            }
        }
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
    
    // [ImageID] Get stable hash ID for image at index
    ImageID GetImageID(int index) const {
        if (index < 0 || index >= (int)m_ids.size()) return 0;
        return m_ids[index];
    }
    
    // [ImageID] Get hash ID for current image
    ImageID GetCurrentImageID() const {
        return GetImageID(m_currentIndex);
    }
    
    // [ImageID] Compute hash from path (for external use)
    static ImageID PathToImageID(const std::wstring& path) {
        return ComputePathHash(path);
    }

private:
    std::wstring FindAdjacentFolderImage(bool next) {
        if (m_files.empty()) return L"";

        namespace fs = std::filesystem;
        fs::path currentDir = fs::path(m_files[0]).parent_path();
        
        // [Logic Upgrade] Through Subfolders: 
        // 1. If moving forward, check if currentDir has subfolders first.
        if (next) {
            try {
                std::vector<std::wstring> subfolders;
                for (const auto& entry : fs::directory_iterator(currentDir)) {
                    if (entry.is_directory()) subfolders.push_back(entry.path().wstring());
                }
                if (!subfolders.empty()) {
                    std::sort(subfolders.begin(), subfolders.end(), [](const std::wstring& a, const std::wstring& b) {
                        return StrCmpLogicalW(a.c_str(), b.c_str()) < 0;
                    });
                    for (const auto& sub : subfolders) {
                        FileNavigator tempNav;
                        tempNav.Initialize(sub);
                        if (tempNav.Count() > 0) return tempNav.First();
                    }
                }
            } catch (...) {}
        }

        // 2. Siblings navigation
        fs::path parentDir = currentDir.parent_path();
        if (parentDir.empty() || parentDir == currentDir) return L"";

        std::vector<std::wstring> folders;
        try {
            for (const auto& entry : fs::directory_iterator(parentDir)) {
                if (entry.is_directory()) folders.push_back(entry.path().wstring());
            }
        } catch(...) { return L""; }

        if (folders.empty()) return L"";

        std::sort(folders.begin(), folders.end(), [](const std::wstring& a, const std::wstring& b){
             return StrCmpLogicalW(a.c_str(), b.c_str()) < 0;
        });

        std::wstring currentStr = currentDir.wstring();
        auto it = std::find(folders.begin(), folders.end(), currentStr);
        int idx = (it == folders.end()) ? -1 : (int)std::distance(folders.begin(), it);

        int startIdx = idx;
        while (true) {
            if (next) idx++; else idx--;

            // Boundary logic
            if (idx < 0 || idx >= (int)folders.size()) {
                if (g_runtime.NavLoop) {
                    // Loop globally: wrap around
                    idx = (idx < 0) ? (int)folders.size() - 1 : 0;
                } else {
                    return L""; // Stop at global boundary
                }
            }
            
            if (idx == startIdx) break; // Wrapped full circle and found nothing

            FileNavigator tempNav;
            tempNav.Initialize(folders[idx]);
            if (tempNav.Count() > 0) {
                 return next ? tempNav.First() : tempNav.Last();
            }
        }

        return L"";
    }

    std::vector<std::wstring> m_files;
    std::vector<uintmax_t> m_sizes;
    std::vector<ImageID> m_ids;  // [ImageID] Precomputed path hashes
    int m_currentIndex = -1;
    bool m_hitEnd = false;
    std::wstring m_crossFolderMessage;
};
