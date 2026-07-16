#include "pch.h"
#include "FileNavigator.h"

extern AppConfig g_config;

// [Directory Watcher] Custom window message posted when background scan completes
// defined in header: constexpr UINT WM_NAVIGATOR_DIR_CHANGED = WM_APP + 50;

void FileNavigator::Initialize(const std::wstring& currentPath, HWND hwnd) {
    // Stop existing watcher and pair verification before mutating state
    StopPairVerification();
    StopDirectoryWatcher();
    if (hwnd) m_hwnd = hwnd;

    namespace fs = std::filesystem;
    
    std::wstring archivePart;
    size_t vfsIndex = (size_t)-1;
    bool isVfsInput = ParseVirtualPath(currentPath, archivePart, vfsIndex);
    
    fs::path p = isVfsInput ? fs::path(archivePart) : fs::path(currentPath);
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

    // [RAW+JPEG Pairing] Verification results belong to one folder
    {
        std::wstring dirStr = dir.wstring();
        if (_wcsicmp(dirStr.c_str(), m_verifyDir.c_str()) != 0) {
            std::lock_guard<std::mutex> lock(m_verifyMutex);
            m_verifyDone.clear();
            m_verifyUnpaired.clear();
            m_verifyDir = std::move(dirStr);
        }
    }

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

    if (QuickView::IsArchiveExtension(pExt)) {
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
    } else {
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (entry.is_regular_file(ec)) {
                std::wstring ext = entry.path().extension().wstring();
                std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c){ return std::towlower(c); });

                // Skip archive container files from the flat folder slideshow playlist
                bool isArchiveExt = QuickView::IsArchiveExtension(ext);
                if (isArchiveExt) continue;

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

    
    std::vector<SortEntry> entries;
    entries.reserve(m_files.size());
    namespace fs2 = std::filesystem;
    for(size_t i=0; i<m_files.size(); ++i) {
        SortEntry e;
        e.p = m_files[i];
        e.s = m_sizes[i];
        std::error_code ec2;
        
        // For virtual paths, use the archive file's timestamp
        if (IsVirtualPath(e.p)) {
            e.m = fs2::last_write_time(p, ec2);
        } else {
            e.m = fs2::last_write_time(e.p, ec2);
        }

        e.t = fs2::path(e.p).extension().wstring();
        std::transform(e.t.begin(), e.t.end(), e.t.begin(), [](wchar_t c){ return std::towlower(c); });

        // Only parse EXIF date if specifically requested and it's a real file
        if (g_runtime.SortOrder == 3 && !IsVirtualPath(e.p)) {
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

    if (m_archive && m_archive->IsValid() && g_config.SortArchivesByNameAscending) {
        sortOrder = 1;      // Force sort by Name
        sortDesc = false;   // Force Ascending
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
                if (m_files[i] == currentFull) {
                    m_currentIndex = (int)i;
                    break;
                }
            }

            // [RAW+JPEG Pairing] The opened file may be a RAW folded behind
            // its rendered sibling -- land on the pair instead.
            if (m_currentIndex < 0 && !m_pairedRaws.empty()) {
                for (const auto& [renderedId, raw] : m_pairedRaws) {
                    if (raw.path == currentFull) {
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

    // [Directory Watcher] Start monitoring for non-VFS real directories
    if (!m_archive && m_hwnd) {
        std::wstring watchDir = dir.wstring();
        if (!watchDir.empty()) {
            StartDirectoryWatcher(watchDir);
        }
    }

    // [RAW+JPEG Pairing] Kick off capture-time verification for fresh pairs
    StartPairVerification();
}

std::wstring FileNavigator::Next(bool /*unused*/) {
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
    if (m_files.empty()) return L"";
    m_hitEnd = false;
    m_currentIndex = 0;
    return m_files[m_currentIndex];
}

std::wstring FileNavigator::Last() {
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
    if (m_files.empty()) return L"";
    size_t nextIdx = (m_currentIndex + 1) % m_files.size();
    return m_files[nextIdx];
}

std::wstring FileNavigator::PeekPrevious() const {
    if (m_files.empty()) return L"";
    size_t prevIdx = (m_currentIndex - 1 + m_files.size()) % m_files.size();
    return m_files[prevIdx];
}

void FileNavigator::Refresh() {
    if (m_currentIndex >= 0 && m_currentIndex < (int)m_files.size()) {
        std::error_code ec;
        m_sizes[m_currentIndex] = std::filesystem::file_size(m_files[m_currentIndex], ec);
    }
}

void FileNavigator::SetIndex(int index) {
    if (index >= 0 && index < (int)m_files.size()) {
        m_currentIndex = index;
        m_hitEnd = false;
    }
}

const std::wstring& FileNavigator::GetFile(int index) const {
    static std::wstring empty;
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
            if (raw.path == requestedPath) {
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

    auto it = std::find(m_files.begin(), m_files.end(), path);
    if (it != m_files.end()) return (int)std::distance(m_files.begin(), it);
    return -1;
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
    std::wstring currentPath;
    if (m_currentIndex >= 0 && m_currentIndex < (int)m_files.size()) {
        currentPath = m_files[m_currentIndex];
    }

    // O(1) swap
    m_files = std::move(result.files);
    m_sizes = std::move(result.sizes);
    m_ids = std::move(result.ids);
    m_pairedRaws = std::move(result.pairedRaws);

    // Relocate current index in new list
    if (!currentPath.empty()) {
        auto it = std::find(m_files.begin(), m_files.end(), currentPath);
        if (it != m_files.end()) {
            m_currentIndex = (int)std::distance(m_files.begin(), it);
        } else {
            // [RAW+JPEG Pairing] The viewed RAW may have just been folded
            // behind its rendered sibling (e.g. its JPG appeared on disk) --
            // relocate to the pair instead of clamping.
            bool redirected = false;
            for (const auto& [renderedId, raw] : m_pairedRaws) {
                if (raw.path == currentPath) {
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

void FileNavigator::StopPairVerification() {
    ++m_verifyGeneration; // cancel the running pass, if any
    if (m_verifyThread.joinable()) m_verifyThread.join();
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

void FileNavigator::SortEntries(std::vector<SortEntry>& entries, int sortOrder, bool sortDesc) {
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

    std::sort(entries.begin(), entries.end(), [sortOrder, sortDesc, &getSortNamePtr](const SortEntry& a, const SortEntry& b){
        int cmp = 0;
        LPCWSTR nameA = getSortNamePtr(a.p);
        LPCWSTR nameB = getSortNamePtr(b.p);
        switch (sortOrder) {
            case 1: // Name
            case 0: // Auto (Use Name Natural Sort)
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
    if (m_files.empty() || m_currentIndex < 0 || m_currentIndex >= (int)m_files.size()) return L"";

    namespace fs = std::filesystem;
    std::wstring currentFile = m_files[m_currentIndex];
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
    namespace fs = std::filesystem;
    std::error_code ec;
    
    for (const auto& entry : fs::directory_iterator(m_watchedDir, ec)) {
        if (entry.is_regular_file(ec)) {
            std::wstring ext = entry.path().extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c){ return std::towlower(c); });

            // Skip archive container files from the flat folder slideshow playlist
            bool isArchiveExt = QuickView::IsArchiveExtension(ext);
            if (isArchiveExt) continue;

            for (const auto& supp : QuickView::SUPPORTED_EXTENSIONS) {
                if (ext == supp) {
                    result.files.push_back(entry.path().wstring());
                    result.sizes.push_back(entry.file_size(ec));
                    break;
                }
            }
        }
    }

    // Sort (same logic as Initialize, but without VFS virtual path handling)
    std::vector<SortEntry> entries;
    entries.reserve(result.files.size());
    for (size_t i = 0; i < result.files.size(); ++i) {
        SortEntry e;
        e.p = result.files[i];
        e.s = result.sizes[i];
        std::error_code ec2;
        e.m = fs::last_write_time(e.p, ec2);
        e.t = fs::path(e.p).extension().wstring();
        std::transform(e.t.begin(), e.t.end(), e.t.begin(), [](wchar_t c){ return std::towlower(c); });

        int sortOrder = g_runtime.SortOrder;
        if (sortOrder == 3) {
            FILE* fp = nullptr;
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
    HANDLE hNotify = FindFirstChangeNotificationW(
        m_watchedDir.c_str(),
        FALSE,                          // Non-recursive (current directory only)
        FILE_NOTIFY_CHANGE_FILE_NAME    // File create, delete, rename only
    );
    if (hNotify == INVALID_HANDLE_VALUE) return;

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

        // === Background Scan (on this thread, zero UI impact) ===
        auto scanResult = PerformDirectoryScan();

        {
            std::lock_guard<std::mutex> lock(m_scanResultMutex);
            m_pendingScanResult = std::move(scanResult);
        }
        PostMessageW(m_hwnd, WM_NAVIGATOR_DIR_CHANGED, 0, 0);

        // Re-arm for next batch of changes
        if (!FindNextChangeNotification(hNotify)) break; // Directory gone
    }

    FindCloseChangeNotification(hNotify);
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
