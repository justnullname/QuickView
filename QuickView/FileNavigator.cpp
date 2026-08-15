#include "pch.h"
#include "FileNavigator.h"
#include <shlobj.h>
#include <exdisp.h>
#include <shobjidl.h>
#include <propkey.h>
#include <wrl/client.h>

extern AppConfig g_config;

// [Directory Watcher] Custom window message posted when background scan completes
// defined in header: constexpr UINT WM_NAVIGATOR_DIR_CHANGED = WM_APP + 50;
constexpr WPARAM NAVIGATOR_EXPLORER_REFRESH = 1;

static bool PathsEqualCi(const std::wstring& a, const std::wstring& b) {
    return !a.empty() && !b.empty() && _wcsicmp(a.c_str(), b.c_str()) == 0;
}

static std::wstring CanonicalFolderPath(const std::wstring& dir) {
    namespace fs = std::filesystem;
    std::wstring s = fs::path(dir).lexically_normal().wstring();
    while (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}

static std::wstring ShellItemFilePath(IShellItem* item) {
    if (!item) return {};
    PWSTR psz = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &psz)) || !psz) return {};
    std::wstring path = psz;
    CoTaskMemFree(psz);
    return path;
}

static std::wstring PidlsToPath(PCIDLIST_ABSOLUTE folderPidl, PCUITEMID_CHILD itemPidl) {
    if (!folderPidl || !itemPidl) return {};
    PIDLIST_ABSOLUTE full = ILCombine(folderPidl, itemPidl);
    if (!full) return {};
    Microsoft::WRL::ComPtr<IShellItem> item;
    std::wstring path;
    if (SUCCEEDED(SHCreateItemFromIDList(full, IID_PPV_ARGS(&item))) && item) {
        path = ShellItemFilePath(item.Get());
    }
    ILFree(full);
    return path;
}

// Live IFolderView for one Explorer window showing `dir`. UI-thread / STA only.
// One Item() call is cheap; never walk the whole view.
struct ExplorerFolderView {
    Microsoft::WRL::ComPtr<IFolderView> view;
    Microsoft::WRL::ComPtr<IShellFolder> shellFolder;
    PIDLIST_ABSOLUTE folderPidl = nullptr;
    int itemCount = 0;

    ExplorerFolderView() = default;
    ~ExplorerFolderView() {
        if (folderPidl) CoTaskMemFree(folderPidl);
    }
    ExplorerFolderView(const ExplorerFolderView&) = delete;
    ExplorerFolderView& operator=(const ExplorerFolderView&) = delete;

    bool Open(const std::wstring& dir) {
        if (dir.empty()) return false;
        const std::wstring target = CanonicalFolderPath(dir);

        Microsoft::WRL::ComPtr<IShellWindows> pShellWindows;
        if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
                                    IID_IShellWindows, (void**)&pShellWindows)) || !pShellWindows) {
            return false;
        }

        long windows = 0;
        pShellWindows->get_Count(&windows);
        for (long i = 0; i < windows; ++i) {
            VARIANT vIndex;
            vIndex.vt = VT_I4;
            vIndex.lVal = i;

            Microsoft::WRL::ComPtr<IDispatch> pDisp;
            if (FAILED(pShellWindows->Item(vIndex, &pDisp)) || !pDisp) continue;

            Microsoft::WRL::ComPtr<IWebBrowser2> pWebBrowser;
            if (FAILED(pDisp.As(&pWebBrowser)) || !pWebBrowser) continue;

            Microsoft::WRL::ComPtr<IServiceProvider> pServiceProvider;
            if (FAILED(pWebBrowser.As(&pServiceProvider)) || !pServiceProvider) continue;

            Microsoft::WRL::ComPtr<IShellBrowser> pShellBrowser;
            if (FAILED(pServiceProvider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&pShellBrowser))) || !pShellBrowser) continue;

            Microsoft::WRL::ComPtr<IShellView> pShellView;
            if (FAILED(pShellBrowser->QueryActiveShellView(&pShellView)) || !pShellView) continue;

            Microsoft::WRL::ComPtr<IFolderView> pFolderView;
            if (FAILED(pShellView.As(&pFolderView)) || !pFolderView) continue;

            Microsoft::WRL::ComPtr<IPersistFolder2> pPersistFolder;
            if (FAILED(pFolderView->GetFolder(IID_PPV_ARGS(&pPersistFolder))) || !pPersistFolder) continue;

            PIDLIST_ABSOLUTE pidlFolder = nullptr;
            if (FAILED(pPersistFolder->GetCurFolder(&pidlFolder)) || !pidlFolder) continue;

            Microsoft::WRL::ComPtr<IShellItem> folderItem;
            std::wstring folderPath;
            if (SUCCEEDED(SHCreateItemFromIDList(pidlFolder, IID_PPV_ARGS(&folderItem))) && folderItem) {
                folderPath = ShellItemFilePath(folderItem.Get());
            }
            if (folderPath.empty()) {
                wchar_t buf[MAX_PATH] = {};
                if (SHGetPathFromIDListW(pidlFolder, buf)) folderPath = buf;
            }
            if (CanonicalFolderPath(folderPath) != target) {
                CoTaskMemFree(pidlFolder);
                continue;
            }

            view = pFolderView;
            folderPidl = pidlFolder;
            pFolderView->GetFolder(IID_PPV_ARGS(&shellFolder));
            pFolderView->ItemCount(SVGIO_ALLVIEW, &itemCount);
            return true;
        }
        return false;
    }

    int FocusedIndex() const {
        if (!view) return -1;
        int index = -1;
        if (FAILED(view->GetFocusedItem(&index))) return -1;
        return index;
    }

    int SelectionMarkedIndex() const {
        if (!view) return -1;
        int index = -1;
        if (FAILED(view->GetSelectionMarkedItem(&index))) return -1;
        return index;
    }

    bool GetSortColumnRaw(SORTCOLUMN& sc) const {
        if (!view) return false;
        Microsoft::WRL::ComPtr<IFolderView2> view2;
        if (FAILED(view.As(&view2)) || !view2) return false;
        return SUCCEEDED(view2->GetSortColumns(&sc, 1));
    }

    std::wstring PathAt(int index) const {
        if (!view || index < 0) return {};
        PITEMID_CHILD pidlItem = nullptr;
        if (FAILED(view->Item(index, &pidlItem)) || !pidlItem) return {};
        std::wstring path = PidlsToPath(folderPidl, pidlItem);
        CoTaskMemFree(pidlItem);
        return path;
    }

    bool GetSortColumn(int& sortOrder, bool& sortDesc) const {
        if (!view) return false;
        Microsoft::WRL::ComPtr<IFolderView2> view2;
        if (FAILED(view.As(&view2)) || !view2) return false;
        SORTCOLUMN sc{};
        if (FAILED(view2->GetSortColumns(&sc, 1))) return false;
        sortDesc = (sc.direction == SORT_DESCENDING);
        const PROPERTYKEY& k = sc.propkey;
        if (k == PKEY_DateModified || k == PKEY_DateCreated || k == PKEY_DateAccessed) {
            sortOrder = 2;
        } else if (k == PKEY_Size) {
            sortOrder = 4;
        } else if (k == PKEY_FileExtension || k == PKEY_ItemType) {
            sortOrder = 5;
        } else if (k == PKEY_Photo_DateTaken || k == PKEY_ItemDate) {
            sortOrder = 3;
        } else {
            sortOrder = 1;
        }
        return true;
    }
};

static std::wstring UniqueRenderedSibling(const std::wstring& rawPath) {
    if (!QuickView::IsRawPath(rawPath)) return {};
    const std::filesystem::path p(rawPath);
    const std::wstring stem = (p.parent_path() / p.stem()).wstring();
    std::wstring found;
    int count = 0;
    for (const auto ext : QuickView::RENDERED_PAIR_EXTENSIONS) {
        std::wstring cand = stem;
        cand.append(ext.data(), ext.size());
        if (GetFileAttributesW(cand.c_str()) != INVALID_FILE_ATTRIBUTES) {
            ++count;
            found = std::move(cand);
        }
    }
    return (count == 1) ? found : std::wstring{};
}

static bool PathIsSameShot(const std::wstring& a, const std::wstring& b) {
    if (PathsEqualCi(a, b)) return true;
    if (a.empty() || b.empty()) return false;
    return PathsEqualCi(UniqueRenderedSibling(a), b)
        || PathsEqualCi(a, UniqueRenderedSibling(b));
}

static int LocatePathInView(const ExplorerFolderView& view, const std::wstring& path, bool allowWalk) {
    if (path.empty() || !view.view) return -1;
    auto matches = [&](int idx) {
        return idx >= 0 && PathIsSameShot(view.PathAt(idx), path);
    };
    const int mark = view.SelectionMarkedIndex();
    if (matches(mark)) return mark;
    const int focus = view.FocusedIndex();
    if (matches(focus)) return focus;
    if (!allowWalk) return -1;
    for (int i = 0; i < view.itemCount; ++i) {
        if (matches(i)) return i;
    }
    return -1;
}

void FileNavigator::Initialize(const std::wstring& currentPath, HWND hwnd, bool deferFolderScan) {
    if (hwnd) m_hwnd = hwnd;

    namespace fs = std::filesystem;
    
    std::wstring archivePart;
    size_t vfsIndex = (size_t)-1;
    bool isVfsInput = ParseVirtualPath(currentPath, archivePart, vfsIndex);
    
    fs::path p = isVfsInput ? fs::path(archivePart) : fs::path(currentPath);
    if (!fs::exists(p)) return;

    const bool isDirectory = fs::is_directory(p);

    // If a directory is passed in, scan it directly. Otherwise scan the parent directory.
    fs::path dir = isDirectory ? p : p.parent_path();
    if (dir.empty()) return;

    std::wstring pExt = p.extension().wstring();
    std::transform(pExt.begin(), pExt.end(), pExt.begin(), [](wchar_t c){ return std::towlower(c); });
    const bool isArchive = !isDirectory && QuickView::IsArchiveExtension(pExt);
    const std::wstring dirStr = dir.wstring();

    // Same folder, scan already running or finished: retarget without
    // tearing down the watcher (opening another file in a 40k folder).
    if (deferFolderScan && hwnd && !isDirectory && !isArchive && !isVfsInput
        && m_watcherThread.joinable()
        && !m_watchedDir.empty() && _wcsicmp(dirStr.c_str(), m_watchedDir.c_str()) == 0) {
        if (TrySelectExisting(p.wstring())) return;
        if (!m_playlistReady) {
            SeedOpenedFile(p.wstring());
            if (g_runtime.SortOrder == 0 && TryBindExplorer(dirStr, CurrentVisiblePath())) {
                m_needInitialScan = false;
            } else {
                ClearExplorerCursor();
                EnsureMaterialized();
            }
            return;
        }
    }

    // Stop existing watcher and pair verification before mutating state
    StopPairVerification();
    StopDirectoryWatcher();

    // VFS State Teardown
    m_archive.reset();
    m_archivePath.clear();

    m_files.clear();
    m_sizes.clear();
    m_ids.clear();
    m_pairedRaws.clear();
    m_currentIndex = -1;
    m_playlistReady = false;
    ClearExplorerCursor();

    // [RAW+JPEG Pairing] Verification results belong to one folder
    {
        if (_wcsicmp(dirStr.c_str(), m_verifyDir.c_str()) != 0) {
            std::lock_guard<std::mutex> lock(m_verifyMutex);
            m_verifyDone.clear();
            m_verifyUnpaired.clear();
            m_verifyDir = dirStr;
        }
    }

    if (isArchive) {
        // Load from Archive VFS
        m_archivePath = p.wstring();
        if (pExt == L".cbr" || pExt == L".rar") {
            m_archive = std::make_unique<QuickView::RarArchive>(m_archivePath);
        } else {
            m_archive = std::make_unique<QuickView::ZipArchive>(m_archivePath);
        }

        if (m_archive && m_archive->IsValid()) {
            size_t numEntries = m_archive->GetEntryCount();
            for (size_t i = 0; i < numEntries; ++i) {
                const QuickView::ArchiveEntry& entry = m_archive->GetEntry(i);
                // Zero-allocation extension check using string_view
                std::string_view nameView = m_archive->GetEntryNameView(i);
                
                size_t lastDot = nameView.find_last_of('.');
                if (lastDot != std::string_view::npos) {
                    std::string_view extUtf8 = nameView.substr(lastDot);
                    
                    // Fast ASCII to wide lower-case conversion for extension
                    std::wstring ext;
                    ext.reserve(extUtf8.length());
                    for (char c : extUtf8) {
                        ext.push_back(std::towlower((wchar_t)(uint8_t)c));
                    }

                    for (const auto& supp : QuickView::SUPPORTED_EXTENSIONS) {
                        if (ext == supp) {
                            // Full conversion only for supported image types
                            std::wstring name = m_archive->GetEntryName(i);
                            std::wstring virtualPath = m_archivePath + L"|" + std::to_wstring(i) + L"|" + name;
                            m_files.push_back(virtualPath);
                            m_sizes.push_back(entry.uncompSize);
                            break;
                        }
                    }
                }
            }
        }
    } else if (deferFolderScan && hwnd && !isDirectory) {
        SeedOpenedFile(isVfsInput ? currentPath : p.wstring());
        const bool bound = (g_runtime.SortOrder == 0)
            && TryBindExplorer(dirStr, CurrentVisiblePath());
        m_needInitialScan = !bound;
        StartDirectoryWatcher(dirStr);
        StartPairVerification();
        return;
    }

    std::vector<SortEntry> entries;
    if (isArchive) {
        entries.reserve(m_files.size());
        std::error_code archiveTimeEc;
        const auto archiveTime = fs::last_write_time(p, archiveTimeEc);
        for (size_t i = 0; i < m_files.size(); ++i) {
            SortEntry e;
            e.p = m_files[i];
            e.s = m_sizes[i];
            e.m = archiveTime;
            e.t = fs::path(e.p).extension().wstring();
            std::transform(e.t.begin(), e.t.end(), e.t.begin(), [](wchar_t c){ return std::towlower(c); });
            entries.push_back(std::move(e));
        }
    } else if (!CollectFolderEntries(dirStr, entries)) {
        return;
    }

    int sortOrder = g_runtime.SortOrder;
    bool sortDesc = g_runtime.SortDescending;

    if (m_archive && m_archive->IsValid() && g_config.SortArchivesByNameAscending) {
        sortOrder = 1;      // Force sort by Name
        sortDesc = false;   // Force Ascending
    } else if (sortOrder == 0 && !isArchive) {
        int mapped = 1;
        bool mappedDesc = sortDesc;
        if (ResolveExplorerSortColumn(dirStr, mapped, mappedDesc)) {
            sortOrder = mapped;
            sortDesc = mappedDesc;
        } else {
            sortOrder = 1;
        }
    }

    if (sortOrder == 3 && !isArchive) {
        for (auto& e : entries) {
            FILE* fp = nullptr;
            _wfopen_s(&fp, e.p.c_str(), L"rb");
            if (!fp) continue;
            unsigned char buf[65536];
            size_t bytes = fread(buf, 1, sizeof(buf), fp);
            fclose(fp);
            if (bytes == 0) continue;
            easyexif::EXIFInfo info;
            if (info.parseFrom(buf, (unsigned)bytes) == PARSE_EXIF_SUCCESS) {
                e.exifDate = info.DateTimeOriginal;
            }
        }
    }

    SortEntries(entries, sortOrder, sortDesc);

    // [RAW+JPEG Pairing] Fold same-name RAW + rendered pairs (real folders
    // only; archives are never paired)
    m_pairedRaws.clear();
    if (g_config.PairRawJpeg && !m_archive) {
        std::unordered_set<ImageID> skip;
        {
            std::lock_guard<std::mutex> lock(m_verifyMutex);
            skip = m_verifyUnpaired;
        }
        ApplyRawJpegPairing(entries, m_pairedRaws, skip.empty() ? nullptr : &skip);
    }

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
        std::wstring currentFull = isVfsInput ? currentPath : p.wstring();

        // Fix initial page load for Archive VFS
        if (m_archive && m_archive->IsValid() && !isVfsInput && currentFull == m_archivePath) {
            if (!m_files.empty()) {
                m_currentIndex = 0; // Default to first page in archive
            }
        } else {
            for (size_t i = 0; i < m_files.size(); ++i) {
                if (_wcsicmp(m_files[i].c_str(), currentFull.c_str()) == 0) {
                    m_currentIndex = (int)i;
                    break;
                }
            }

            // [RAW+JPEG Pairing] The opened file may be a RAW folded behind
            // its rendered sibling -- land on the pair instead.
            if (m_currentIndex < 0 && !m_pairedRaws.empty()) {
                for (const auto& [renderedId, raw] : m_pairedRaws) {
                    if (_wcsicmp(raw.path.c_str(), currentFull.c_str()) == 0) {
                        for (size_t i = 0; i < m_ids.size(); ++i) {
                            if (m_ids[i] == renderedId) {
                                m_currentIndex = (int)i;
                                break;
                            }
                        }
                        break;
                    }
                }
            }
        }
    }

    m_playlistReady = true;
    m_needInitialScan = false;

    // [Directory Watcher] Start monitoring for non-VFS real directories
    if (!m_archive && m_hwnd) {
        if (!dirStr.empty()) {
            StartDirectoryWatcher(dirStr);
        }
    }

    // [RAW+JPEG Pairing] Kick off capture-time verification for fresh pairs
    StartPairVerification();
}

std::wstring FileNavigator::Next(bool /*unused*/) {
    if (g_runtime.SortOrder == 0) SyncExplorerCursor(true);
    if (UsingExplorerCursor()) {
        std::wstring path = ExplorerStep(+1);
        if (path.empty() && g_runtime.NavTraverse) {
            std::wstring nextFolderImg = FindAdjacentFolderImage(true);
            if (!nextFolderImg.empty()) {
                m_crossFolderMessage = L">>> Entering [" + std::filesystem::path(nextFolderImg).parent_path().filename().wstring() + L"] >>>";
                return nextFolderImg;
            }
        }
        return path;
    }

    if (m_files.empty()) return L"";

    if (g_runtime.NavTraverse) {
        bool shouldTraverse = false;
        
        // Case 1: We are at the end of the current playlist
        if (m_currentIndex >= (int)m_files.size() - 1) {
            shouldTraverse = true;
        } else {
            // Case 2: The next sibling in the parent directory is a container (folder/archive)
            std::wstring currentFile = m_files[m_currentIndex];
            std::wstring physicalCurrent = std::wstring(GetPhysicalHostPath(currentFile));
            
            namespace fs = std::filesystem;
            fs::path currentPath(physicalCurrent);
            fs::path parentDir = currentPath.parent_path();
            
            std::vector<std::wstring> siblings = GetSortedSiblings(parentDir);
            
            auto it = std::find(siblings.begin(), siblings.end(), physicalCurrent);
            int idx = (it == siblings.end()) ? -1 : (int)std::distance(siblings.begin(), it);
            
            if (idx != -1 && idx < (int)siblings.size() - 1) {
                std::wstring nextSibling = siblings[idx + 1];
                bool isContainer = false;
                if (fs::is_directory(nextSibling)) {
                    isContainer = true;
                } else {
                    std::wstring nextExt = fs::path(nextSibling).extension().wstring();
                    std::transform(nextExt.begin(), nextExt.end(), nextExt.begin(), [](wchar_t c){ return std::towlower(c); });
                    if (nextExt == L".cbz" || nextExt == L".zip" || nextExt == L".cbr" || nextExt == L".rar") {
                        isContainer = true;
                    }
                }
                
                if (isContainer) {
                    shouldTraverse = true;
                }
            }
        }
        
        if (shouldTraverse) {
            std::wstring nextFolderImg = FindAdjacentFolderImage(true);
            if (!nextFolderImg.empty()) {
                m_crossFolderMessage = L">>> Entering [" + std::filesystem::path(nextFolderImg).parent_path().filename().wstring() + L"] >>>";
                return nextFolderImg;
            }
        }
    }

    if (m_currentIndex >= (int)m_files.size() - 1) {
        if (g_runtime.NavLoop) {
            m_hitEnd = true; // Signal OSD
            m_currentIndex = 0;
            return m_files[m_currentIndex];
        } else {
            m_hitEnd = true;
            return L"";
        }
    }

    m_hitEnd = false;
    m_currentIndex++;
    return m_files[m_currentIndex];
}

std::wstring FileNavigator::Previous(bool /*unused*/) {
    if (g_runtime.SortOrder == 0) SyncExplorerCursor(true);
    if (UsingExplorerCursor()) {
        std::wstring path = ExplorerStep(-1);
        if (path.empty() && g_runtime.NavTraverse) {
            std::wstring prevFolderImg = FindAdjacentFolderImage(false);
            if (!prevFolderImg.empty()) {
                m_crossFolderMessage = L"<<< Entering [" + std::filesystem::path(prevFolderImg).parent_path().filename().wstring() + L"] <<<";
                return prevFolderImg;
            }
        }
        return path;
    }

    if (m_files.empty()) return L"";

    if (g_runtime.NavTraverse) {
        bool shouldTraverse = false;
        
        // Case 1: We are at the beginning of the current playlist
        if (m_currentIndex <= 0) {
            shouldTraverse = true;
        } else {
            // Case 2: The previous sibling in the parent directory is a container (folder/archive)
            std::wstring currentFile = m_files[m_currentIndex];
            std::wstring physicalCurrent = std::wstring(GetPhysicalHostPath(currentFile));
            
            namespace fs = std::filesystem;
            fs::path currentPath(physicalCurrent);
            fs::path parentDir = currentPath.parent_path();
            
            std::vector<std::wstring> siblings = GetSortedSiblings(parentDir);
            
            auto it = std::find(siblings.begin(), siblings.end(), physicalCurrent);
            int idx = (it == siblings.end()) ? -1 : (int)std::distance(siblings.begin(), it);
            
            if (idx > 0) {
                std::wstring prevSibling = siblings[idx - 1];
                bool isContainer = false;
                if (fs::is_directory(prevSibling)) {
                    isContainer = true;
                } else {
                    std::wstring prevExt = fs::path(prevSibling).extension().wstring();
                    std::transform(prevExt.begin(), prevExt.end(), prevExt.begin(), [](wchar_t c){ return std::towlower(c); });
                    if (prevExt == L".cbz" || prevExt == L".zip" || prevExt == L".cbr" || prevExt == L".rar") {
                        isContainer = true;
                    }
                }
                
                if (isContainer) {
                    shouldTraverse = true;
                }
            }
        }
        
        if (shouldTraverse) {
            std::wstring prevFolderImg = FindAdjacentFolderImage(false);
            if (!prevFolderImg.empty()) {
                m_crossFolderMessage = L"<<< Entering [" + std::filesystem::path(prevFolderImg).parent_path().filename().wstring() + L"] <<<";
                return prevFolderImg;
            }
        }
    }

    if (m_currentIndex <= 0) {
        if (g_runtime.NavLoop) {
            m_hitEnd = true; // Signal OSD
            m_currentIndex = (int)m_files.size() - 1;
            return m_files[m_currentIndex];
        } else {
            m_hitEnd = true;
            return L"";
        }
    }

    m_hitEnd = false;
    m_currentIndex--;
    return m_files[m_currentIndex];
}

std::wstring FileNavigator::First() {
    if (g_runtime.SortOrder == 0) SyncExplorerCursor(true);
    if (UsingExplorerCursor()) {
        const int saved = m_explorerIndex;
        m_explorerIndex = -1;
        std::wstring path = ExplorerStep(+1);
        if (path.empty()) {
            m_explorerIndex = saved;
            m_currentIndex = saved;
        }
        return path;
    }
    if (m_files.empty()) return L"";
    m_hitEnd = false;
    m_currentIndex = 0;
    return m_files[m_currentIndex];
}

std::wstring FileNavigator::Last() {
    if (g_runtime.SortOrder == 0) SyncExplorerCursor(true);
    if (UsingExplorerCursor()) {
        const int saved = m_explorerIndex;
        m_explorerIndex = m_explorerCount;
        std::wstring path = ExplorerStep(-1);
        if (path.empty()) {
            m_explorerIndex = saved;
            m_currentIndex = saved;
        }
        return path;
    }
    if (m_files.empty()) return L"";
    m_hitEnd = false;
    m_currentIndex = (int)m_files.size() - 1;
    return m_files[m_currentIndex];
}

std::wstring FileNavigator::GetCrossFolderMessage() {
    std::wstring msg = m_crossFolderMessage;
    m_crossFolderMessage.clear(); // Consume
    return msg;
}

std::wstring FileNavigator::PeekNext() const {
    if (g_runtime.SortOrder == 0) {
        const_cast<FileNavigator*>(this)->SyncExplorerCursor(false);
    }
    if (UsingExplorerCursor()) {
        const int next = FindExplorerNeighbor(+1);
        if (next < 0) return L"";
        auto it = m_explorerPathCache.find(next);
        if (it == m_explorerPathCache.end()) return L"";
        const std::wstring rendered = UniqueRenderedSibling(it->second);
        return rendered.empty() ? it->second : rendered;
    }
    if (m_files.empty()) return L"";
    size_t nextIdx = (m_currentIndex + 1) % m_files.size();
    return m_files[nextIdx];
}

std::wstring FileNavigator::PeekPrevious() const {
    if (g_runtime.SortOrder == 0) {
        const_cast<FileNavigator*>(this)->SyncExplorerCursor(false);
    }
    if (UsingExplorerCursor()) {
        const int prev = FindExplorerNeighbor(-1);
        if (prev < 0) return L"";
        auto it = m_explorerPathCache.find(prev);
        if (it == m_explorerPathCache.end()) return L"";
        const std::wstring rendered = UniqueRenderedSibling(it->second);
        return rendered.empty() ? it->second : rendered;
    }
    if (m_files.empty()) return L"";
    size_t prevIdx = (m_currentIndex - 1 + m_files.size()) % m_files.size();
    return m_files[prevIdx];
}

void FileNavigator::Refresh() {
    const std::wstring path = CurrentVisiblePath();
    if (path.empty()) return;
    std::error_code ec;
    const uintmax_t sz = std::filesystem::file_size(path, ec);
    if (UsingExplorerCursor()) {
        if (!m_sizes.empty()) m_sizes[0] = sz;
        return;
    }
    if (m_currentIndex >= 0 && m_currentIndex < (int)m_sizes.size()) {
        m_sizes[m_currentIndex] = sz;
    }
}

void FileNavigator::SetIndex(int index) {
    if (UsingExplorerCursor()) {
        if (index < 0 || index >= m_explorerCount) return;
        m_explorerIndex = index;
        m_currentIndex = index;
        m_hitEnd = false;
        std::wstring path = GetFile(index);
        if (!path.empty()) AdoptCurrentFile(path);
        return;
    }
    if (index >= 0 && index < (int)m_files.size()) {
        m_currentIndex = index;
        m_hitEnd = false;
    }
}

const std::wstring& FileNavigator::GetFile(int index) const {
    static std::wstring empty;
    if (UsingExplorerCursor()) {
        if (index < 0 || (m_explorerCount > 0 && index >= m_explorerCount)) return empty;
        auto it = m_explorerPathCache.find(index);
        if (it != m_explorerPathCache.end()) return it->second;
        if (index == m_explorerIndex && !m_files.empty()) {
            return CacheExplorerPath(index, m_files.front());
        }
        ExplorerFolderView view;
        if (!view.Open(m_watchedDir)) return empty;
        m_explorerCount = view.itemCount;
        std::wstring path = view.PathAt(index);
        if (path.empty()) return empty;
        return CacheExplorerPath(index, std::move(path));
    }
    if (index < 0 || index >= (int)m_files.size()) return empty;
    return m_files[index];
}

std::wstring FileNavigator::GetResolvedPath(const std::wstring& requestedPath) const {
    if (m_archive && m_archive->IsValid() && requestedPath == m_archivePath) {
        if (!m_files.empty() && m_currentIndex >= 0 && m_currentIndex < (int)m_files.size()) {
            return m_files[m_currentIndex];
        }
    }

    // [RAW+JPEG Pairing] A RAW folded behind its rendered sibling resolves to
    // that sibling: with pairing enabled the pair is one logical photo and the
    // rendered file is its visible face, regardless of which file was opened.
    if (!m_pairedRaws.empty()) {
        for (const auto& [renderedId, raw] : m_pairedRaws) {
            if (_wcsicmp(raw.path.c_str(), requestedPath.c_str()) == 0) {
                for (size_t i = 0; i < m_ids.size(); ++i) {
                    if (m_ids[i] == renderedId) return m_files[i];
                }
                break;
            }
        }
    }

    return requestedPath;
}

int FileNavigator::FindIndex(const std::wstring& path) const {
    // Handle virtual path matching where we might be passed just the archive path from OS
    if (m_archive && m_archive->IsValid()) {
        if (path == m_archivePath) {
            return 0; // Return first entry if they try to open the archive itself
        }
    }

    if (UsingExplorerCursor()) {
        if (PathsEqualCi(CurrentVisiblePath(), path)) return m_explorerIndex;
        for (const auto& [idx, cached] : m_explorerPathCache) {
            if (PathsEqualCi(cached, path)) return idx;
        }
        return -1;
    }

    for (size_t i = 0; i < m_files.size(); ++i) {
        if (_wcsicmp(m_files[i].c_str(), path.c_str()) == 0) return (int)i;
    }
    return -1;
}

bool FileNavigator::TrySelectExisting(const std::wstring& path) {
    if (path.empty()) return false;

    if (UsingExplorerCursor()) {
        if (PathsEqualCi(CurrentVisiblePath(), path)) return true;
        for (const auto& [id, raw] : m_pairedRaws) {
            if (PathsEqualCi(raw.path, path)) return true;
        }
        SeedOpenedFile(path);
        if (TryBindExplorer(m_watchedDir, CurrentVisiblePath())) return true;
        ClearExplorerCursor();
        m_currentIndex = 0;
        return false;
    }

    if (m_files.empty()) return false;

    for (size_t i = 0; i < m_files.size(); ++i) {
        if (_wcsicmp(m_files[i].c_str(), path.c_str()) == 0) {
            SetIndex((int)i);
            return true;
        }
    }

    if (!m_pairedRaws.empty()) {
        for (const auto& [renderedId, raw] : m_pairedRaws) {
            if (_wcsicmp(raw.path.c_str(), path.c_str()) == 0) {
                for (size_t i = 0; i < m_ids.size(); ++i) {
                    if (m_ids[i] == renderedId) {
                        SetIndex((int)i);
                        return true;
                    }
                }
                break;
            }
        }
    }

    if (m_archive && m_archive->IsValid() && _wcsicmp(path.c_str(), m_archivePath.c_str()) == 0) {
        SetIndex(0);
        return true;
    }
    return false;
}

void FileNavigator::SeedOpenedFile(const std::wstring& path) {
    namespace fs = std::filesystem;
    m_archive.reset();
    m_archivePath.clear();
    m_files.clear();
    m_sizes.clear();
    m_ids.clear();
    m_pairedRaws.clear();
    m_currentIndex = -1;
    m_hitEnd = false;
    m_playlistReady = false;

    std::error_code ec;
    uintmax_t sz = fs::file_size(path, ec);
    std::wstring visible = path;

    if (g_config.PairRawJpeg && QuickView::IsRawPath(path)) {
        const fs::path p(path);
        const std::wstring stem = (p.parent_path() / p.stem()).wstring();
        std::wstring found;
        int renderedCount = 0;
        for (const auto ext : QuickView::RENDERED_PAIR_EXTENSIONS) {
            std::wstring cand = stem;
            cand.append(ext.data(), ext.size());
            if (GetFileAttributesW(cand.c_str()) != INVALID_FILE_ATTRIBUTES) {
                ++renderedCount;
                found = std::move(cand);
            }
        }
        if (renderedCount == 1) {
            visible = std::move(found);
            const ImageID rid = ComputePathHash(visible);
            m_pairedRaws.emplace(rid, PairedRaw{ path, sz, ComputePathHash(path) });
            sz = fs::file_size(visible, ec);
        }
    }

    m_files.push_back(visible);
    m_sizes.push_back(sz);
    m_ids.push_back(ComputePathHash(visible));
    m_currentIndex = 0;
}

bool FileNavigator::ScanCancelled() const {
    return m_hCancelEvent && WaitForSingleObject(m_hCancelEvent, 0) == WAIT_OBJECT_0;
}

bool FileNavigator::CollectFolderEntries(const std::wstring& dir, std::vector<SortEntry>& entries) const {
    namespace fs = std::filesystem;
    std::error_code ec;
    size_t n = 0;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if ((++n & 0xFF) == 0 && ScanCancelled()) return false;
        if (!entry.is_regular_file(ec)) continue;

        std::wstring ext = entry.path().extension().wstring();
        if (!QuickView::IsPlaylistImageExtension(ext)) continue;

        SortEntry e;
        e.p = entry.path().wstring();
        e.s = entry.file_size(ec);
        e.m = entry.last_write_time(ec); // cached from FindFirstFile
        std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c){ return std::towlower(c); });
        e.t = std::move(ext);
        entries.push_back(std::move(e));
    }
    return true;
}

void FileNavigator::ClearExplorerCursor() {
    m_explorerBound = false;
    m_explorerIndex = -1;
    m_explorerCount = 0;
    m_explorerPathCache.clear();
    m_hasExplorerSort = false;
    m_explorerSortPid = 0;
    m_explorerSortDir = 0;
    m_explorerSortFmtid = {};
}

std::wstring FileNavigator::CurrentVisiblePath() const {
    if (UsingExplorerCursor()) {
        if (!m_files.empty()) return m_files.front();
        if (m_explorerIndex >= 0) {
            auto it = m_explorerPathCache.find(m_explorerIndex);
            if (it != m_explorerPathCache.end()) return it->second;
        }
        return {};
    }
    if (m_currentIndex >= 0 && m_currentIndex < (int)m_files.size()) {
        return m_files[m_currentIndex];
    }
    return {};
}

void FileNavigator::AdoptCurrentFile(const std::wstring& path) {
    std::error_code ec;
    uintmax_t sz = std::filesystem::file_size(path, ec);
    std::wstring visible = path;
    m_pairedRaws.clear();
    if (g_config.PairRawJpeg && QuickView::IsRawPath(path)) {
        std::wstring rendered = UniqueRenderedSibling(path);
        if (!rendered.empty()) {
            const ImageID rid = ComputePathHash(rendered);
            bool skip = false;
            {
                std::lock_guard<std::mutex> lock(m_verifyMutex);
                skip = m_verifyUnpaired.find(rid) != m_verifyUnpaired.end();
            }
            if (!skip) {
                m_pairedRaws.emplace(rid, PairedRaw{ path, sz, ComputePathHash(path) });
                visible = std::move(rendered);
                sz = std::filesystem::file_size(visible, ec);
            }
        }
    }
    m_files.clear();
    m_sizes.clear();
    m_ids.clear();
    m_files.push_back(visible);
    m_sizes.push_back(sz);
    m_ids.push_back(ComputePathHash(visible));
}

const std::wstring& FileNavigator::CacheExplorerPath(int index, std::wstring path) const {
    static std::wstring empty;
    if (path.empty()) return empty;
    auto [it, inserted] = m_explorerPathCache.emplace(index, std::move(path));
    if (!inserted) it->second = std::move(path);
    return it->second;
}

bool FileNavigator::IsExplorerVisibleImage(const std::wstring& path) const {
    if (path.empty()) return false;
    const DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) return false;
    if (!QuickView::IsPlaylistImageExtension(QuickView::ExtensionOf(path))) return false;
    if (g_config.PairRawJpeg && QuickView::IsRawPath(path)) {
        const std::wstring rendered = UniqueRenderedSibling(path);
        if (!rendered.empty()) {
            const ImageID rid = ComputePathHash(rendered);
            std::lock_guard<std::mutex> lock(m_verifyMutex);
            if (m_verifyUnpaired.find(rid) == m_verifyUnpaired.end()) return false;
        }
    }
    return true;
}

bool FileNavigator::TryBindExplorer(const std::wstring& dir, const std::wstring& openedPath) {
    if (dir.empty() || openedPath.empty() || g_runtime.SortOrder != 0) {
        ClearExplorerCursor();
        return false;
    }

    ExplorerFolderView view;
    if (!view.Open(dir) || view.itemCount <= 0) {
        ClearExplorerCursor();
        return false;
    }

    // Prefer the selection mark: after a double-click Explorer keeps the file
    // selected even when the view no longer has keyboard focus (GetFocusedItem
    // then returns -1 and we used to fall back to a frozen name list).
    int idx = LocatePathInView(view, openedPath, false);
    if (idx < 0) idx = LocatePathInView(view, CurrentVisiblePath(), false);
    if (idx < 0) {
        ClearExplorerCursor();
        return false;
    }

    m_explorerBound = true;
    m_explorerIndex = idx;
    m_explorerCount = view.itemCount;
    m_currentIndex = idx;
    m_explorerPathCache.clear();
    CacheExplorerPath(idx, openedPath);
    SORTCOLUMN sc{};
    if (view.GetSortColumnRaw(sc)) {
        m_explorerSortFmtid = sc.propkey.fmtid;
        m_explorerSortPid = sc.propkey.pid;
        m_explorerSortDir = (int)sc.direction;
        m_hasExplorerSort = true;
    }
    return true;
}

bool FileNavigator::SyncExplorerCursor(bool allowDematerialize) {
    if (g_runtime.SortOrder != 0 || m_archive || m_watchedDir.empty()) return UsingExplorerCursor();

    ExplorerFolderView view;
    if (!view.Open(m_watchedDir) || view.itemCount <= 0) return UsingExplorerCursor();

    SORTCOLUMN sc{};
    const bool haveSort = view.GetSortColumnRaw(sc);
    PROPERTYKEY oldKey{};
    oldKey.fmtid = m_explorerSortFmtid;
    oldKey.pid = m_explorerSortPid;
    const bool sortChanged = haveSort && m_hasExplorerSort
        && (!IsEqualPropertyKey(sc.propkey, oldKey) || (int)sc.direction != m_explorerSortDir);

    if (haveSort) {
        m_explorerSortFmtid = sc.propkey.fmtid;
        m_explorerSortPid = sc.propkey.pid;
        m_explorerSortDir = (int)sc.direction;
        m_hasExplorerSort = true;
    }

    const std::wstring current = CurrentVisiblePath();
    const bool indexStale = UsingExplorerCursor()
        && m_explorerIndex >= 0
        && !PathIsSameShot(view.PathAt(m_explorerIndex), current);

    if (!sortChanged && !indexStale && UsingExplorerCursor()) {
        m_explorerCount = view.itemCount;
        return true;
    }

    // Relocate the current file in the (possibly re-sorted) view. Walk the
    // whole view only when sort actually changed and selection no longer
    // points at the file we are showing.
    int idx = LocatePathInView(view, current, false);
    if (idx < 0 && (sortChanged || UsingExplorerCursor())) {
        idx = LocatePathInView(view, current, true);
    }
    if (idx < 0) return UsingExplorerCursor();

    if (sortChanged || indexStale) m_explorerPathCache.clear();

    m_explorerBound = true;
    m_explorerIndex = idx;
    m_explorerCount = view.itemCount;
    m_currentIndex = idx;
    if (allowDematerialize && m_playlistReady) {
        m_playlistReady = false;
        if (!current.empty()) AdoptCurrentFile(current);
    }
    if (!current.empty()) CacheExplorerPath(idx, current);
    return true;
}

void FileNavigator::SyncWithExplorer() {
    SyncExplorerCursor(true);
}

int FileNavigator::FindExplorerNeighbor(int dir) const {
    if (dir == 0 || m_watchedDir.empty()) return -1;
    ExplorerFolderView view;
    if (!view.Open(m_watchedDir) || view.itemCount <= 0) return -1;
    m_explorerCount = view.itemCount;

    int i = m_explorerIndex;
    for (int n = 0; n < view.itemCount; ++n) {
        i += dir;
        if (i < 0 || i >= view.itemCount) {
            if (!g_runtime.NavLoop) return -2;
            i = (dir > 0) ? 0 : view.itemCount - 1;
        }
        if (i == m_explorerIndex) return -2;
        std::wstring path = view.PathAt(i);
        if (!IsExplorerVisibleImage(path)) continue;
        CacheExplorerPath(i, std::move(path));
        return i;
    }
    return -2;
}

std::wstring FileNavigator::ExplorerStep(int dir) {
    const int next = FindExplorerNeighbor(dir);
    if (next == -1) {
        // Explorer window gone — fall back to a names-only playlist.
        ClearExplorerCursor();
        EnsureMaterialized();
        return (dir > 0) ? Next() : Previous();
    }
    if (next < 0) {
        m_hitEnd = true;
        return L"";
    }
    m_hitEnd = (g_runtime.NavLoop && ((dir > 0 && next < m_explorerIndex) || (dir < 0 && next > m_explorerIndex)));
    m_explorerIndex = next;
    m_currentIndex = next;
    auto it = m_explorerPathCache.find(next);
    const std::wstring path = (it != m_explorerPathCache.end()) ? it->second : CurrentVisiblePath();
    if (!path.empty()) AdoptCurrentFile(path);
    return CurrentVisiblePath();
}

void FileNavigator::RefreshExplorerCount() {
    if (!UsingExplorerCursor() || m_watchedDir.empty()) return;
    ExplorerFolderView view;
    if (!view.Open(m_watchedDir)) {
        ClearExplorerCursor();
        return;
    }
    m_explorerCount = view.itemCount;
    m_explorerPathCache.clear();
    if (m_explorerIndex >= 0 && m_explorerIndex < m_explorerCount) {
        CacheExplorerPath(m_explorerIndex, CurrentVisiblePath());
    }
}

void FileNavigator::EnsureMaterialized() {
    if (m_playlistReady || m_archive) return;
    // Auto + live Explorer view: keep the cursor. Materializing would freeze
    // the playlist to one snapshot and ignore later Explorer sort changes.
    if (g_runtime.SortOrder == 0 && !m_watchedDir.empty()) {
        if (UsingExplorerCursor() || TryBindExplorer(m_watchedDir, CurrentVisiblePath())) {
            return;
        }
    }
    if (m_watchedDir.empty() && m_files.empty()) return;

    const std::wstring current = CurrentVisiblePath();
    if (m_watchedDir.empty()) {
        m_playlistReady = true;
        ClearExplorerCursor();
        return;
    }

    DirectoryScanResult result = PerformDirectoryScan();
    {
        std::lock_guard<std::mutex> lock(m_scanResultMutex);
        m_pendingScanResult = std::move(result);
    }
    // ApplyPending reads the current file from m_files[m_currentIndex] —
    // park the visible path at index 0 so relocation works.
    if (!current.empty()) {
        if (m_files.empty()) m_files.push_back(current);
        else m_files[0] = current;
        m_currentIndex = 0;
    }
    ClearExplorerCursor();
    ApplyPendingScanResult();
}

size_t FileNavigator::Count() const {
    if (UsingExplorerCursor()) {
        return (m_explorerCount > 0) ? (size_t)m_explorerCount : (size_t)1;
    }
    return m_files.size();
}

int FileNavigator::Index() const {
    if (UsingExplorerCursor()) return m_explorerIndex;
    return m_currentIndex;
}

uintmax_t FileNavigator::GetFileSize(int index) const {
    if (UsingExplorerCursor()) {
        if (index == m_explorerIndex && !m_sizes.empty()) return m_sizes[0];
        const std::wstring& path = GetFile(index);
        if (path.empty()) return 0;
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return 0;
        return (static_cast<uintmax_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    }
    if (index < 0 || index >= (int)m_sizes.size()) return 0;
    return m_sizes[index];
}

ImageID FileNavigator::GetImageID(int index) const {
    if (UsingExplorerCursor()) {
        if (index == m_explorerIndex && !m_ids.empty()) return m_ids[0];
        const std::wstring& path = GetFile(index);
        return path.empty() ? 0 : ComputePathHash(path);
    }
    if (index < 0 || index >= (int)m_ids.size()) return 0;
    return m_ids[index];
}

bool FileNavigator::ParseVirtualPath(const std::wstring& path, std::wstring& outArchivePath, size_t& outIndex) {
    size_t firstPipe = path.find(L"|");
    if (firstPipe == std::wstring::npos) return false;

    size_t secondPipe = path.find(L"|", firstPipe + 1);
    if (secondPipe == std::wstring::npos) return false;

    outArchivePath = path.substr(0, firstPipe);
    size_t index = 0;
    size_t len = secondPipe - firstPipe - 1;
    if (len == 0) return false;
    
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

void FileNavigator::ApplyPendingScanResult() {
    DirectoryScanResult result;
    {
        std::lock_guard<std::mutex> lock(m_scanResultMutex);
        if (!m_pendingScanResult) return;
        result = std::move(*m_pendingScanResult);
        m_pendingScanResult.reset();
    }

    // Cache current path BEFORE swap for index reconciliation
    std::wstring currentPath = CurrentVisiblePath();

    // O(1) swap
    m_files = std::move(result.files);
    m_sizes = std::move(result.sizes);
    m_ids = std::move(result.ids);
    m_pairedRaws = std::move(result.pairedRaws);

    // Relocate current index in new list
    if (!currentPath.empty()) {
        auto it = std::find_if(m_files.begin(), m_files.end(), [&](const std::wstring& f) {
            return _wcsicmp(f.c_str(), currentPath.c_str()) == 0;
        });
        if (it != m_files.end()) {
            m_currentIndex = (int)std::distance(m_files.begin(), it);
        } else {
            // [RAW+JPEG Pairing] The viewed RAW may have just been folded
            // behind its rendered sibling (e.g. its JPG appeared on disk) --
            // relocate to the pair instead of clamping.
            bool redirected = false;
            for (const auto& [renderedId, raw] : m_pairedRaws) {
                if (_wcsicmp(raw.path.c_str(), currentPath.c_str()) == 0) {
                    for (size_t i = 0; i < m_ids.size(); ++i) {
                        if (m_ids[i] == renderedId) {
                            m_currentIndex = (int)i;
                            redirected = true;
                            break;
                        }
                    }
                    break;
                }
            }

            // Fallback: file was deleted externally — clamp to nearest valid index
            if (!redirected) {
                if (m_currentIndex >= (int)m_files.size()) {
                    m_currentIndex = (int)m_files.size() - 1;
                }
                if (m_files.empty()) m_currentIndex = -1;
            }
        }
    }

    m_playlistReady = true;
    ClearExplorerCursor();

    // [RAW+JPEG Pairing] A rescan may have folded new pairs -- verify them.
    // Already-verified pairs are skipped, so this cannot loop.
    StartPairVerification();
}

void FileNavigator::RescanDirectory() {
    if (m_watchedDir.empty()) return; // archive or no folder open
    // Join any in-flight verification pass first: it could otherwise post a
    // result computed before this one right after it.
    StopPairVerification();
    DirectoryScanResult result = PerformDirectoryScan();
    {
        std::lock_guard<std::mutex> lock(m_scanResultMutex);
        m_pendingScanResult = std::move(result);
    }
    ApplyPendingScanResult();
}

int64_t FileNavigator::ParseExifDateTime(const std::string& exifDateTime) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    if (sscanf_s(exifDateTime.c_str(), "%d:%d:%d %d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
    if (y < 1970 || y > 3000 || mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
    std::tm t{};
    t.tm_year = y - 1900;
    t.tm_mon = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min = mi;
    t.tm_sec = se;
    t.tm_isdst = -1;
    const time_t tt = std::mktime(&t); // local time, matching LibRaw's own conversion
    return tt <= 0 ? 0 : (int64_t)tt;
}

// [RAW+JPEG Pairing] Capture time (DateTimeOriginal) of a JPEG file via
// easyexif (a JPEG-only parser: exif.cpp rejects anything not starting with
// FFD8); 0 when unreadable.
static int64_t ReadJpegCaptureTime(const std::wstring& path) {
    FILE* fp = nullptr;
    _wfopen_s(&fp, path.c_str(), L"rb");
    if (!fp) return 0;
    unsigned char buf[65536];
    size_t bytes = fread(buf, 1, sizeof(buf), fp);
    fclose(fp);
    if (bytes == 0) return 0;
    easyexif::EXIFInfo info;
    if (info.parseFrom(buf, (unsigned)bytes) != PARSE_EXIF_SUCCESS) return 0;
    return FileNavigator::ParseExifDateTime(info.DateTimeOriginal);
}

void FileNavigator::StartPairVerification() {
    if (!m_hwnd || m_pairedRaws.empty() || m_watchedDir.empty()) return;

    // Snapshot the pairs that still need a capture-time check
    struct VerifyItem {
        std::wstring renderedPath;
        std::wstring rawPath;
        ImageID renderedId = 0;
    };
    std::vector<VerifyItem> todo;
    {
        std::lock_guard<std::mutex> lock(m_verifyMutex);
        for (const auto& [renderedId, raw] : m_pairedRaws) {
            if (m_verifyDone.find(renderedId) != m_verifyDone.end()) continue;
            for (size_t i = 0; i < m_ids.size(); ++i) {
                if (m_ids[i] == renderedId) {
                    todo.push_back({ m_files[i], raw.path, renderedId });
                    break;
                }
            }
        }
    }
    if (todo.empty()) return;

    StopPairVerification();
    const uint32_t gen = ++m_verifyGeneration;
    m_verifyThread = std::thread([this, gen, todo = std::move(todo)]() {
        bool anyUnpaired = false;
        for (const auto& item : todo) {
            if (m_verifyGeneration.load() != gen) return; // superseded

            // Rendered side: dispatch by extension -- JPEG through easyexif
            // (fastest), everything else (HEIF) straight to the fallback
            // reader (WIC). A JPEG whose date lives only in XMP gets one WIC
            // retry too.
            const std::wstring_view rext = QuickView::ExtensionOf(item.renderedPath);
            const bool isJpeg = QuickView::ExtEqualsIgnoreCase(rext, L".jpg")
                             || QuickView::ExtEqualsIgnoreCase(rext, L".jpeg");
            int64_t tRendered = isJpeg ? ReadJpegCaptureTime(item.renderedPath) : 0;
            if (tRendered == 0 && s_captureTimeFallback) {
                tRendered = s_captureTimeFallback(item.renderedPath.c_str());
            }

            // RAW side: always the fallback reader (LibRaw branch)
            const int64_t tRaw = s_captureTimeFallback ? s_captureTimeFallback(item.rawPath.c_str()) : 0;

            std::lock_guard<std::mutex> lock(m_verifyMutex);
            m_verifyDone.insert(item.renderedId);
            if (PairVerificationFails(tRendered, tRaw)) {
                m_verifyUnpaired.insert(item.renderedId);
                anyUnpaired = true;
            }
        }
        if (!anyUnpaired || m_verifyGeneration.load() != gen) return;

        // Split the mismatched pairs back up: rescan with the blacklist in
        // effect and hand the result to the main thread through the exact
        // channel the directory watcher already uses (one atomic list swap).
        DirectoryScanResult result = PerformDirectoryScan();
        if (m_verifyGeneration.load() != gen) return;
        {
            std::lock_guard<std::mutex> lock(m_scanResultMutex);
            m_pendingScanResult = std::move(result);
        }
        PostMessageW(m_hwnd, WM_NAVIGATOR_DIR_CHANGED, 0, 0);
    });
}

void FileNavigator::StopPairVerification(bool forceSync) {
    ++m_verifyGeneration; // cancel the running pass, if any
    if (m_verifyThread.joinable()) {
        if (forceSync) {
            m_verifyThread.join();
        } else {
            m_verifyThread.detach();
        }
    }
}

void FileNavigator::ApplyRawJpegPairing(std::vector<SortEntry>& entries,
                                        std::unordered_map<ImageID, PairedRaw>& outPairedRaws,
                                        const std::unordered_set<ImageID>* skipRendered) {
    outPairedRaws.clear();

    // Early exit: pairing is only possible when the folder mixes camera RAWs
    // with whitelisted rendered stills. One cheap in-memory pass, no I/O.
    bool anyRaw = false, anyRendered = false;
    for (const auto& e : entries) {
        anyRaw = anyRaw || QuickView::IsRawExtension(e.t);
        anyRendered = anyRendered || QuickView::IsRenderedPairExtension(e.t);
        if (anyRaw && anyRendered) break;
    }
    if (!anyRaw || !anyRendered) return;

    // Group pairing candidates by lowercase stem (file name minus extension;
    // the scan covers a single directory, so the stem identifies the shot).
    // Non-candidates (e.g. a same-name .png screenshot) neither pair nor
    // block a pair.
    struct Group {
        int rawIdx = -1;
        int renderedIdx = -1;
        int rawCount = 0;
        int renderedCount = 0;
    };
    std::unordered_map<std::wstring, Group> groups;
    groups.reserve(entries.size());
    for (int i = 0; i < (int)entries.size(); ++i) {
        const auto& e = entries[i];
        const bool isRaw = QuickView::IsRawExtension(e.t);
        const bool isRendered = !isRaw && QuickView::IsRenderedPairExtension(e.t);
        if (!isRaw && !isRendered) continue;

        const size_t sep = e.p.find_last_of(L"\\/");
        const size_t start = (sep == std::wstring::npos) ? 0 : sep + 1;
        std::wstring stem = e.p.substr(start, e.p.size() - start - e.t.size());
        std::transform(stem.begin(), stem.end(), stem.begin(), [](wchar_t c){ return std::towlower(c); });

        Group& g = groups[stem];
        if (isRaw) { g.rawCount++; g.rawIdx = i; }
        else       { g.renderedCount++; g.renderedIdx = i; }
    }

    // Strict 1:1: fold only when a stem has exactly one RAW and exactly one
    // rendered still. Ambiguous groups (rename collisions, bracketing
    // leftovers) stay fully visible so the user can see and resolve them.
    std::vector<char> hide(entries.size(), 0);
    for (const auto& [stem, g] : groups) {
        if (g.rawCount != 1 || g.renderedCount != 1) continue;
        const SortEntry& raw = entries[g.rawIdx];
        const SortEntry& rendered = entries[g.renderedIdx];
        const ImageID renderedId = ComputePathHash(rendered.p);
        // Capture-time verification confirmed these are different shots
        if (skipRendered && skipRendered->find(renderedId) != skipRendered->end()) continue;
        outPairedRaws.emplace(renderedId,
                              PairedRaw{ raw.p, raw.s, ComputePathHash(raw.p) });
        hide[g.rawIdx] = 1;
    }
    if (outPairedRaws.empty()) return;

    // Drop the hidden RAW entries, preserving sort order.
    std::vector<SortEntry> kept;
    kept.reserve(entries.size() - outPairedRaws.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        if (!hide[i]) kept.push_back(std::move(entries[i]));
    }
    entries = std::move(kept);
}

bool FileNavigator::ResolveExplorerSortColumn(const std::wstring& targetDir, int& sortOrder, bool& sortDesc) {
    ExplorerFolderView view;
    if (!view.Open(targetDir)) return false;
    return view.GetSortColumn(sortOrder, sortDesc);
}

void FileNavigator::SortEntries(std::vector<SortEntry>& entries, int sortOrder, bool sortDesc, const std::wstring& dirPath) {
    // Helper to get pointer to null-terminated file/entry name substring to avoid dynamic allocations
    auto getSortNamePtr = [](const std::wstring& path) -> LPCWSTR {
        size_t lastPipe = path.find_last_of(L'|');
        if (lastPipe != std::wstring::npos) {
            return path.c_str() + lastPipe + 1;
        }
        size_t lastSlash = path.find_last_of(L"\\/");
        if (lastSlash != std::wstring::npos) {
            return path.c_str() + lastSlash + 1;
        }
        return path.c_str();
    };

    // Auto: adopt Explorer's sort *column* (one COM query). Never walk the
    // folder's item list — that is O(files) shell binds and freezes 40k+ dirs.
    if (sortOrder == 0) {
        int mapped = 1;
        bool mappedDesc = sortDesc;
        if (!dirPath.empty() && ResolveExplorerSortColumn(dirPath, mapped, mappedDesc)) {
            sortOrder = mapped;
            sortDesc = mappedDesc;
        } else {
            sortOrder = 1;
        }
    }

    std::sort(entries.begin(), entries.end(), [sortOrder, sortDesc, &getSortNamePtr](const SortEntry& a, const SortEntry& b){
        int cmp = 0;
        LPCWSTR nameA = getSortNamePtr(a.p);
        LPCWSTR nameB = getSortNamePtr(b.p);
        switch (sortOrder) {
            case 0: // leftover Auto — treat as name
            case 1: // Name
                cmp = StrCmpLogicalW(nameA, nameB);
                break;
            case 2: // Modified
                if (a.m < b.m) cmp = -1;
                else if (a.m > b.m) cmp = 1;
                else cmp = StrCmpLogicalW(nameA, nameB); // Fallback
                break;
            case 3: // Date Taken
                if (a.exifDate.empty() && !b.exifDate.empty()) cmp = 1; // Empty goes last
                else if (!a.exifDate.empty() && b.exifDate.empty()) cmp = -1;
                else {
                    cmp = a.exifDate.compare(b.exifDate);
                    if (cmp == 0) cmp = StrCmpLogicalW(nameA, nameB);
                }
                break;
            case 4: // Size
                if (a.s < b.s) cmp = -1;
                else if (a.s > b.s) cmp = 1;
                else cmp = StrCmpLogicalW(nameA, nameB);
                break;
            case 5: // Type
                cmp = StrCmpLogicalW(a.t.c_str(), b.t.c_str());
                if (cmp == 0) cmp = StrCmpLogicalW(nameA, nameB);
                break;
        }

        if (sortDesc) return cmp > 0;
        return cmp < 0;
    });
}

std::wstring_view FileNavigator::GetPhysicalHostPath(std::wstring_view vfsPath) {
    auto pos = vfsPath.find(L'|');
    if (pos != std::wstring_view::npos) {
        return vfsPath.substr(0, pos);
    }
    return vfsPath;
}

__declspec(noinline) std::vector<std::wstring> FileNavigator::GetSortedSiblings(const std::filesystem::path& parentDir) {
    std::vector<std::wstring> siblings;
    std::error_code ec;
    namespace fs = std::filesystem;
    for (const auto& entry : fs::directory_iterator(parentDir, ec)) {
        if (entry.is_directory(ec)) {
            siblings.push_back(entry.path().wstring());
        } else if (entry.is_regular_file(ec)) {
            std::wstring ext = entry.path().extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c){ return std::towlower(c); });
            bool isArchive = QuickView::IsArchiveExtension(ext);
            if (isArchive) {
                siblings.push_back(entry.path().wstring());
                continue;
            }
            for (const auto& supp : QuickView::SUPPORTED_EXTENSIONS) {
                if (ext == supp) {
                    siblings.push_back(entry.path().wstring());
                    break;
                }
            }
        }
    }
    std::sort(siblings.begin(), siblings.end(), [](const std::wstring& a, const std::wstring& b) {
        return StrCmpLogicalW(a.c_str(), b.c_str()) < 0;
    });
    return siblings;
}

std::wstring FileNavigator::FindAdjacentFolderImage(bool next) {
    std::wstring currentFile = CurrentVisiblePath();
    if (currentFile.empty()) return L"";

    namespace fs = std::filesystem;
    std::wstring currentPhysical = std::wstring(GetPhysicalHostPath(currentFile));

    fs::path currentPath(currentPhysical);
    fs::path parentDir = currentPath.parent_path();
    if (parentDir.empty() || parentDir == currentPath) return L"";

    std::vector<std::wstring> siblings = GetSortedSiblings(parentDir);
    if (siblings.empty()) return L"";

    std::wstring physicalStr = currentPath.wstring();
    auto it = std::find(siblings.begin(), siblings.end(), physicalStr);
    int idx = (it == siblings.end()) ? -1 : (int)std::distance(siblings.begin(), it);

    if (idx == -1) return L"";

    int nextIdx = next ? idx + 1 : idx - 1;
    
    // Boundary logic
    if (nextIdx < 0 || nextIdx >= (int)siblings.size()) {
        if (g_runtime.NavLoop) {
            nextIdx = (nextIdx < 0) ? (int)siblings.size() - 1 : 0;
        } else {
            return L"";
        }
    }

    int startIdx = nextIdx;
    while (true) {
        std::wstring sib = siblings[nextIdx];
        bool isContainer = false;
        if (fs::is_directory(sib)) {
            isContainer = true;
        } else {
            std::wstring sibExt = fs::path(sib).extension().wstring();
            std::transform(sibExt.begin(), sibExt.end(), sibExt.begin(), [](wchar_t c){ return std::towlower(c); });
            if (QuickView::IsArchiveExtension(sibExt)) {
                isContainer = true;
            }
        }

        if (isContainer) {
            FileNavigator tempNav;
            tempNav.Initialize(sib);
            if (tempNav.Count() > 0) {
                return next ? tempNav.First() : tempNav.Last();
            }
        } else {
            return sib;
        }

        if (next) nextIdx++; else nextIdx--;
        if (nextIdx < 0 || nextIdx >= (int)siblings.size()) {
            if (g_runtime.NavLoop) {
                nextIdx = (nextIdx < 0) ? (int)siblings.size() - 1 : 0;
            } else {
                return L"";
            }
        }
        if (nextIdx == startIdx) break;
    }

    return L"";
}

FileNavigator::DirectoryScanResult FileNavigator::PerformDirectoryScan() {
    DirectoryScanResult result;
    std::vector<SortEntry> entries;
    if (!CollectFolderEntries(m_watchedDir, entries)) {
        return result; // cancelled
    }

    int sortOrder = g_runtime.SortOrder;
    bool sortDesc = g_runtime.SortDescending;
    if (sortOrder == 0) {
        int mapped = 1;
        bool mappedDesc = sortDesc;
        if (ResolveExplorerSortColumn(m_watchedDir, mapped, mappedDesc)) {
            sortOrder = mapped;
            sortDesc = mappedDesc;
        } else {
            sortOrder = 1;
        }
    }

    if (sortOrder == 3) {
        for (auto& e : entries) {
            if (ScanCancelled()) return {};
            FILE* fp = nullptr;
            _wfopen_s(&fp, e.p.c_str(), L"rb");
            if (!fp) continue;
            unsigned char buf[65536];
            size_t bytes = fread(buf, 1, sizeof(buf), fp);
            fclose(fp);
            if (bytes == 0) continue;
            easyexif::EXIFInfo info;
            if (info.parseFrom(buf, (unsigned)bytes) == PARSE_EXIF_SUCCESS) {
                e.exifDate = info.DateTimeOriginal;
            }
        }
    }

    if (ScanCancelled()) return {};
    SortEntries(entries, sortOrder, sortDesc);

    // [RAW+JPEG Pairing] Same fold as Initialize (watcher rescan path)
    if (g_config.PairRawJpeg) {
        std::unordered_set<ImageID> skip;
        {
            std::lock_guard<std::mutex> lock(m_verifyMutex);
            skip = m_verifyUnpaired;
        }
        ApplyRawJpegPairing(entries, result.pairedRaws, skip.empty() ? nullptr : &skip);
    }

    result.files.clear();
    result.sizes.clear();
    for (const auto& e : entries) {
        result.files.push_back(e.p);
        result.sizes.push_back(e.s);
    }

    result.ids.reserve(result.files.size());
    for (const auto& f : result.files) {
        result.ids.push_back(ComputePathHash(f));
    }

    return result;
}

void FileNavigator::WatcherThreadProc() {
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    auto publishScan = [this]() {
        if (ScanCancelled()) return;
        auto scanResult = PerformDirectoryScan();
        if (ScanCancelled()) return;
        if (scanResult.files.empty() && !std::filesystem::exists(m_watchedDir)) return;
        // An empty folder is a valid result (publish it). A cancelled scan
        // returns a default-constructed result; ScanCancelled already bailed.
        {
            std::lock_guard<std::mutex> lock(m_scanResultMutex);
            m_pendingScanResult = std::move(scanResult);
        }
        if (m_hwnd) PostMessageW(m_hwnd, WM_NAVIGATOR_DIR_CHANGED, 0, 0);
    };

    // Names-only list only when we are not using the live Explorer cursor.
    if (m_needInitialScan.load()) publishScan();

    HANDLE hNotify = FindFirstChangeNotificationW(
        m_watchedDir.c_str(),
        FALSE,                          // Non-recursive (current directory only)
        FILE_NOTIFY_CHANGE_FILE_NAME    // File create, delete, rename only
    );
    if (hNotify == INVALID_HANDLE_VALUE) {
        if (SUCCEEDED(comHr)) CoUninitialize();
        return;
    }

    HANDLE handles[2] = { hNotify, m_hCancelEvent };

    while (true) {
        DWORD wait = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (wait == WAIT_OBJECT_0 + 1) break;  // Cancel event signaled
        if (wait != WAIT_OBJECT_0) break;       // Error or abandoned

        // === Coalescing / Debounce Loop (300ms) ===
        // Drain all rapid-fire events until 300ms of silence
        bool cancelled = false;
        while (true) {
            if (!FindNextChangeNotification(hNotify)) {
                cancelled = true; // Directory removed or device ejected
                break;
            }
            DWORD r = WaitForMultipleObjects(2, handles, FALSE, 300);
            if (r == WAIT_OBJECT_0 + 1) { cancelled = true; break; } // Cancel
            if (r == WAIT_TIMEOUT) break; // 300ms silence — proceed to scan
            if (r != WAIT_OBJECT_0) { cancelled = true; break; } // Error
            // r == WAIT_OBJECT_0: more changes arrived, loop again (reset timer)
        }
        if (cancelled) break;

        // Materialized playlists rescan; Explorer cursor just refreshes ItemCount.
        if (m_playlistReady.load() || m_needInitialScan.load()) {
            publishScan();
        } else if (m_hwnd) {
            PostMessageW(m_hwnd, WM_NAVIGATOR_DIR_CHANGED, NAVIGATOR_EXPLORER_REFRESH, 0);
        }

        // Re-arm for next batch of changes
        if (!FindNextChangeNotification(hNotify)) break; // Directory gone
    }

    FindCloseChangeNotification(hNotify);
    if (SUCCEEDED(comHr)) CoUninitialize();
}

void FileNavigator::StartDirectoryWatcher(const std::wstring& dirPath) {
    m_watchedDir = dirPath;

    // Create manual-reset event (initially non-signaled) for graceful shutdown
    m_hCancelEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_hCancelEvent) return;

    m_watcherThread = std::thread(&FileNavigator::WatcherThreadProc, this);
}

void FileNavigator::StopDirectoryWatcher() {
    if (m_hCancelEvent) {
        SetEvent(m_hCancelEvent); // Signal cancellation
    }
    if (m_watcherThread.joinable()) {
        m_watcherThread.join();   // Wait for clean exit
    }
    if (m_hCancelEvent) {
        CloseHandle(m_hCancelEvent);
        m_hCancelEvent = nullptr;
    }
    // Discard any unprocessed result
    std::lock_guard<std::mutex> lock(m_scanResultMutex);
    m_pendingScanResult.reset();
}
