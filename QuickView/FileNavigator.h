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
#include "SupportedExtensions.h" // Unified supported extensions

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

        // VFS State Teardown
        m_archive.reset();
        m_archivePath.clear();

        m_files.clear();
        m_currentIndex = -1;

        const bool isDirectory = fs::is_directory(p);

        // If a directory is passed in, scan it directly. Otherwise scan the parent directory.
        fs::path dir = isDirectory ? p : p.parent_path();
        if (dir.empty()) return;

        // Supported extensions (comprehensive list including RAW formats)
        // using QuickView::SUPPORTED_EXTENSIONS from SupportedExtensions.h

        std::error_code ec;
        if (fs::exists(p, ec) && fs::is_directory(p, ec)) {
            // Already handled isDirectory
        }

        m_sizes.clear();

        // VFS Support for Archives
        std::wstring pExt = p.extension().wstring();
        std::transform(pExt.begin(), pExt.end(), pExt.begin(), [](wchar_t c){ return std::towlower(c); });

        if (pExt == L".cbz" || pExt == L".zip") {
            // Load from Archive VFS
            m_archivePath = p.wstring();
            m_archive = std::make_unique<QuickView::ZipArchive>(m_archivePath);

            if (m_archive && m_archive->IsValid()) {
                size_t numEntries = m_archive->GetEntryCount();
                for (size_t i = 0; i < numEntries; ++i) {
                    const QuickView::ArchiveEntry& entry = m_archive->GetEntry(i);
                    // Use lazy evaluation for strings
                    std::wstring name = m_archive->GetEntryName(i);

                    std::wstring ext = std::filesystem::path(name).extension().wstring();
                    std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c){ return std::towlower(c); });

                    for (const auto& supp : QuickView::SUPPORTED_EXTENSIONS) {
                        if (ext == supp) {
                            // Construct virtual path: ArchivePath|Index|InternalName
                            std::wstring virtualPath = m_archivePath + L"|" + std::to_wstring(i) + L"|" + name;
                            m_files.push_back(virtualPath);
                            m_sizes.push_back(entry.uncompSize); // Use uncompressed size
                            break;
                        }
                    }
                }
            }
        } else {
            for (const auto& entry : fs::directory_iterator(dir, ec)) {
                if (entry.is_regular_file(ec)) {
                    std::wstring ext = entry.path().extension().wstring();
                    std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c){ return std::towlower(c); });

                    for (const auto& supp : QuickView::SUPPORTED_EXTENSIONS) {
                        if (ext == supp) {
                            m_files.push_back(entry.path().wstring());
                            // Cache file size for Scout Lane decision
                            m_sizes.push_back(entry.file_size(ec));
                            break;
                        }
                    }
                }
            }
        }

        
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
            std::error_code ec;
            e.m = fs::last_write_time(e.p, ec);

            e.t = fs::path(e.p).extension().wstring();
            std::transform(e.t.begin(), e.t.end(), e.t.begin(), [](wchar_t c){ return std::towlower(c); });

            // Only parse EXIF date if specifically requested (to save load time)
            if (g_runtime.SortOrder == 3) {
                 FILE *fp = nullptr;
                 _wfopen_s(&fp, e.p.c_str(), L"rb");
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

            // Fix initial page load for Archive VFS
            if (m_archive && m_archive->IsValid() && currentFull == m_archivePath) {
                if (!m_files.empty()) {
                    m_currentIndex = 0; // Default to first page in archive
                }
            } else {
                for (size_t i = 0; i < m_files.size(); ++i) {
                    if (m_files[i] == currentFull) {
                        m_currentIndex = (int)i;
                        break;
                    }
                }
            }
        }
    }

    // Legacy support: We ignore `loop` and strictly use NavLoopMode instead
    std::wstring Next(bool /*unused*/ = true) {
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

    std::wstring Previous(bool /*unused*/ = true) {
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
            std::error_code ec;
            m_sizes[m_currentIndex] = std::filesystem::file_size(m_files[m_currentIndex], ec);
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
        // Handle virtual path matching where we might be passed just the archive path from OS
        if (m_archive && m_archive->IsValid()) {
            if (path == m_archivePath) {
                return 0; // Return first entry if they try to open the archive itself
            }
        }

        auto it = std::find(m_files.begin(), m_files.end(), path);
        if (it != m_files.end()) return (int)std::distance(m_files.begin(), it);
        return -1;
    }

    // Virtual file system accessors
    bool IsVirtualPath(const std::wstring& path) const {
        return path.find(L"|") != std::wstring::npos;
    }

    // Parse virtual path avoiding exceptions
    static bool ParseVirtualPath(const std::wstring& path, std::wstring& outArchivePath, size_t& outIndex) {
        size_t firstPipe = path.find(L"|");
        if (firstPipe == std::wstring::npos) return false;

        size_t secondPipe = path.find(L"|", firstPipe + 1);
        if (secondPipe == std::wstring::npos) return false;

        outArchivePath = path.substr(0, firstPipe);

        // Fast string to integer avoiding exceptions
        size_t index = 0;
        for (size_t i = firstPipe + 1; i < secondPipe; ++i) {
            wchar_t c = path[i];
            if (c >= L'0' && c <= L'9') {
                index = index * 10 + (c - L'0');
            } else {
                return false; // Invalid character
            }
        }
        outIndex = index;
        return true;
    }

    QuickView::ZipArchive* GetArchive() const { return m_archive.get(); }

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
    // Helper to get physical host path from a potentially virtual path, zero allocations
    static std::wstring_view GetPhysicalHostPath(std::wstring_view vfsPath) {
        auto pos = vfsPath.find(L'|');
        if (pos != std::wstring_view::npos) {
            return vfsPath.substr(0, pos);
        }
        return vfsPath;
    }

    std::wstring FindAdjacentFolderImage(bool next) {
        if (m_files.empty()) return L"";

        namespace fs = std::filesystem;

        // Extract physical path to ensure std::filesystem works correctly
        std::wstring_view physicalView = GetPhysicalHostPath(m_files[0]);
        fs::path physicalPath(physicalView);
        fs::path currentDir = physicalPath.parent_path();
        
        // [Logic Upgrade] Through Subfolders: 
        // 1. If moving forward, check if currentDir has subfolders first.
        if (next) {
            std::error_code ec;
            std::vector<std::wstring> subfolders;
            for (const auto& entry : fs::directory_iterator(currentDir, ec)) {
                if (entry.is_directory(ec)) {
                    subfolders.push_back(entry.path().wstring());
                } else if (entry.is_regular_file(ec)) {
                    // Treat archives as traversable directories
                    std::wstring ext = entry.path().extension().wstring();
                    std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c){ return std::towlower(c); });
                    if (ext == L".cbz" || ext == L".zip") {
                        subfolders.push_back(entry.path().wstring());
                    }
                }
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
        }

        // 2. Siblings navigation
        fs::path parentDir = currentDir.parent_path();
        if (parentDir.empty() || parentDir == currentDir) return L"";

        std::vector<std::wstring> folders;
        std::error_code ec_fold;
        for (const auto& entry : fs::directory_iterator(parentDir, ec_fold)) {
            if (entry.is_directory(ec_fold)) {
                folders.push_back(entry.path().wstring());
            } else if (entry.is_regular_file(ec_fold)) {
                // Treat archives as sibling directories
                std::wstring ext = entry.path().extension().wstring();
                std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c){ return std::towlower(c); });
                if (ext == L".cbz" || ext == L".zip") {
                    folders.push_back(entry.path().wstring());
                }
            }
        }

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

    // VFS Support
    std::unique_ptr<QuickView::ZipArchive> m_archive;

public:
    std::wstring m_archivePath;
};
