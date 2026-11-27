#include "pch.h"
#include "PluginRegistry.h"
#include "StringUtils.h"
#include "SystemInfo.h"
#include "yyjson.h"
#include <algorithm>
#include <charconv>

namespace QuickView {

namespace {

std::wstring GetExeDir() {
    wchar_t buf[MAX_PATH * 2]{};
    const DWORD len = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (len == 0 || len >= std::size(buf)) return {};
    PathRemoveFileSpecW(buf);
    return std::wstring(buf);
}

bool EqualsIgnoreCaseAscii(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb + ('a' - 'A'));
        if (ca != cb) return false;
    }
    return true;
}

bool IsSafeRelativeEntry(std::string_view entry) noexcept {
    if (entry.empty()) return false;
    if (entry.front() == '/' || entry.front() == '\\') return false;
    if (entry.find(':') != std::string_view::npos) return false;
    if (entry.find("..") != std::string_view::npos) return false;
    return true;
}

uint32_t ParseInterfaceName(std::string_view name) noexcept {
    if (name == "decoder") return 1u << QVX_IFACE_DECODER;
    if (name == "media_stream") return 1u << QVX_IFACE_MEDIA_STREAM;
    if (name == "image_processor") return 1u << QVX_IFACE_IMAGE_PROCESSOR;
    if (name == "analyzer") return 1u << QVX_IFACE_ANALYZER;
    if (name == "indexer") return 1u << QVX_IFACE_INDEXER;
    if (name == "shell_ext") return 1u << QVX_IFACE_SHELL_EXT;
    return 0u;
}

QVX_ProcKind ParseProcessorKind(std::string_view kind) noexcept {
    if (kind == "upscaler") return QVX_PROC_KIND_UPSCALER;
    if (kind == "matting") return QVX_PROC_KIND_MATTING;
    if (kind == "restoration") return QVX_PROC_KIND_RESTORATION;
    if (kind == "filter") return QVX_PROC_KIND_FILTER;
    return QVX_PROC_KIND_UNKNOWN;
}

uint32_t ParseProcessorCap(std::string_view cap) noexcept {
    if (cap == "gpu_d3d11") return QVX_PROC_CAP_GPU_D3D11;
    if (cap == "shared_handle") return QVX_PROC_CAP_SHARED_HANDLE;
    if (cap == "cpu_fallback") return QVX_PROC_CAP_CPU_FALLBACK;
    if (cap == "hdr_fp16") return QVX_PROC_CAP_HDR_FP16;
    if (cap == "auto_trigger") return QVX_PROC_CAP_AUTO_TRIGGER;
    if (cap == "compare_mode") return QVX_PROC_CAP_COMPARE_MODE;
    return 0u;
}

const PluginLocalizedText* FindBestLocale(const std::vector<PluginLocalizedText>& i18n, std::string_view lang) noexcept {
    if (i18n.empty()) return nullptr;
    for (const auto& item : i18n) {
        if (EqualsIgnoreCaseAscii(item.lang, lang)) return &item;
    }
    const size_t dash = lang.find_first_of("-_");
    const std::string_view prefix = (dash == std::string_view::npos) ? lang : lang.substr(0, dash);
    if (!prefix.empty()) {
        for (const auto& item : i18n) {
            const size_t idash = item.lang.find_first_of("-_");
            const std::string_view iprefix = (idash == std::string_view::npos) ? std::string_view(item.lang) : std::string_view(item.lang).substr(0, idash);
            if (EqualsIgnoreCaseAscii(iprefix, prefix)) return &item;
        }
    }
    for (const auto& item : i18n) {
        if (EqualsIgnoreCaseAscii(item.lang, "en-US") || EqualsIgnoreCaseAscii(item.lang, "en")) return &item;
    }
    return &i18n.front();
}

bool TryParseManifestFile(const std::wstring& manifestPath, const std::wstring& pluginDir, PluginSourceKind sourceKind, PluginRecord& outRecord) {
    HANDLE hFile = CreateFileW(manifestPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart <= 0 || fileSize.QuadPart > 1024LL * 1024LL) {
        CloseHandle(hFile);
        return false;
    }

    std::string jsonBuf(static_cast<size_t>(fileSize.QuadPart), '\0');
    DWORD bytesRead = 0;
    const BOOL readOk = ReadFile(hFile, jsonBuf.data(), static_cast<DWORD>(jsonBuf.size()), &bytesRead, nullptr);
    CloseHandle(hFile);
    if (!readOk || bytesRead == 0) return false;

    yyjson_doc* doc = yyjson_read(jsonBuf.data(), bytesRead, 0);
    if (!doc) return false;

    bool valid = false;
    yyjson_val* root = yyjson_doc_get_root(doc);
    if (yyjson_is_obj(root)) {
        yyjson_val* vSchema = yyjson_obj_get(root, "schema_version");
        const int schemaVer = yyjson_is_int(vSchema) ? static_cast<int>(yyjson_get_int(vSchema)) : 0;

        auto readStr = [](yyjson_val* obj, const char* key) -> std::string {
            yyjson_val* v = yyjson_obj_get(obj, key);
            if (v && yyjson_is_str(v)) {
                const char* s = yyjson_get_str(v);
                return s ? std::string(s) : std::string();
            }
            return {};
        };

        std::string abiVer = readStr(root, "abi_version");
        std::string id = readStr(root, "id");
        std::string entry = readStr(root, "entry");
        std::string arch = readStr(root, "arch");

        auto isMatchingHostArch = [](std::string_view rawArch) noexcept -> bool {
            while (!rawArch.empty() && (rawArch.front() == ' ' || rawArch.front() == '\t')) rawArch.remove_prefix(1);
            while (!rawArch.empty() && (rawArch.back() == ' ' || rawArch.back() == '\t')) rawArch.remove_suffix(1);
            if (rawArch.empty()) return true;
            auto iequals = [](std::string_view lhs, std::string_view rhs) noexcept -> bool {
                if (lhs.size() != rhs.size()) return false;
                for (size_t i = 0; i < lhs.size(); ++i) {
                    char c = lhs[i];
                    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
                    if (c != rhs[i]) return false;
                }
                return true;
            };
            if (iequals(rawArch, "any")) return true;
#if defined(_M_ARM64) || defined(__aarch64__)
            return iequals(rawArch, "arm64") || iequals(rawArch, "aarch64");
#elif defined(_M_AMD64) || defined(_M_X64) || defined(__x86_64__)
            return iequals(rawArch, "x64") || iequals(rawArch, "amd64") || iequals(rawArch, "x86_64");
#elif defined(_M_IX86) || defined(__i386__)
            return iequals(rawArch, "x86") || iequals(rawArch, "win32");
#else
            return true;
#endif
        };

        if (schemaVer == 3 && abiVer.starts_with("3.") && !id.empty() &&
            IsSafeRelativeEntry(entry) && isMatchingHostArch(arch)) {
            std::wstring entryRel = Utf8ToWide(entry);
            std::replace(entryRel.begin(), entryRel.end(), L'/', L'\\');
            std::wstring entryFull = JoinPath(pluginDir, entryRel);

            if (IsExistingRegularFile(entryFull.c_str())) {
                outRecord = PluginRecord{};
                outRecord.id = std::move(id);
                outRecord.version = readStr(root, "version");
                outRecord.abiVersion = std::move(abiVer);
                outRecord.author = readStr(root, "author");
                outRecord.entryFileName = std::move(entry);
                outRecord.arch = std::move(arch);
                outRecord.sandboxPref = readStr(root, "sandbox_pref");
                outRecord.pluginDir = pluginDir;
                outRecord.entryFullPath = std::move(entryFull);
                outRecord.manifestPath = manifestPath;
                outRecord.sourceKind = sourceKind;

                // Parse interfaces array
                yyjson_val* ifaces = yyjson_obj_get(root, "interfaces");
                if (yyjson_is_arr(ifaces)) {
                    size_t idx = 0, max = 0;
                    yyjson_val* item = nullptr;
                    yyjson_arr_foreach(ifaces, idx, max, item) {
                        if (yyjson_is_str(item)) {
                            outRecord.interfaceMask |= ParseInterfaceName(yyjson_get_str(item));
                        }
                    }
                }

                // Parse image_processor metadata
                yyjson_val* procObj = yyjson_obj_get(root, "image_processor");
                if (yyjson_is_obj(procObj)) {
                    outRecord.processorKind = ParseProcessorKind(readStr(procObj, "kind"));
                    yyjson_val* capsArr = yyjson_obj_get(procObj, "caps");
                    if (yyjson_is_arr(capsArr)) {
                        size_t idx = 0, max = 0;
                        yyjson_val* item = nullptr;
                        yyjson_arr_foreach(capsArr, idx, max, item) {
                            if (yyjson_is_str(item)) {
                                outRecord.processorCaps |= ParseProcessorCap(yyjson_get_str(item));
                            }
                        }
                    }
                }

                // Parse i18n map
                yyjson_val* i18nObj = yyjson_obj_get(root, "i18n");
                if (yyjson_is_obj(i18nObj)) {
                    size_t idx = 0, max = 0;
                    yyjson_val* key = nullptr;
                    yyjson_val* val = nullptr;
                    yyjson_obj_foreach(i18nObj, idx, max, key, val) {
                        if (yyjson_is_str(key) && yyjson_is_obj(val)) {
                            PluginLocalizedText loc;
                            loc.lang = yyjson_get_str(key);
                            loc.name = readStr(val, "name");
                            loc.desc = readStr(val, "desc");
                            outRecord.i18n.push_back(std::move(loc));
                        }
                    }
                }

                // Parse optional static assets metadata
                yyjson_val* assetsArr = yyjson_obj_get(root, "assets");
                if (yyjson_is_arr(assetsArr)) {
                    size_t idx = 0, max = 0;
                    yyjson_val* item = nullptr;
                    yyjson_arr_foreach(assetsArr, idx, max, item) {
                        if (!yyjson_is_obj(item)) continue;
                        PluginManifestAsset asset;
                        asset.id = readStr(item, "id");
                        if (asset.id.empty()) continue;
                        asset.displayName = readStr(item, "name");
                        asset.extractSubdir = readStr(item, "extract_subdir");
                        asset.downloadUrl = readStr(item, "url");
                        asset.sha256 = readStr(item, "sha256");
                        yyjson_val* vSz = yyjson_obj_get(item, "size_bytes");
                        if (vSz && yyjson_is_uint(vSz)) asset.sizeBytes = yyjson_get_uint(vSz);
                        yyjson_val* vReq = yyjson_obj_get(item, "required");
                        if (vReq && yyjson_is_bool(vReq)) asset.required = yyjson_get_bool(vReq);
                        outRecord.manifestAssets.push_back(std::move(asset));
                    }
                }

                valid = (outRecord.interfaceMask != 0);
            }
        }
    }

    yyjson_doc_free(doc);
    return valid;
}

void ScanDirectoryTree(const std::wstring& dir, PluginSourceKind sourceKind, int depth, std::vector<PluginRecord>& outRecords) {
    if (depth > 3 || dir.empty()) return;

    WIN32_FIND_DATAW ffd{};
    const std::wstring pattern = JoinPath(dir, L"*");
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &ffd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (wcscmp(ffd.cFileName, L".") == 0 || wcscmp(ffd.cFileName, L"..") == 0) continue;
        const std::wstring itemPath = JoinPath(dir, ffd.cFileName);
        if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ScanDirectoryTree(itemPath, sourceKind, depth + 1, outRecords);
        } else {
            const std::wstring_view nameView(ffd.cFileName);
            if (_wcsicmp(ffd.cFileName, L"manifest.json") == 0) {
                PluginRecord rec;
                if (TryParseManifestFile(itemPath, dir, sourceKind, rec)) {
                    outRecords.push_back(std::move(rec));
                }
            } else if (nameView.size() > 15 &&
                       _wcsicmp(ffd.cFileName + (nameView.size() - 15), L".pending_delete") == 0) {
                // Opportunistically clean up deferred deletions from previous updates/uninstalls
                SetFileAttributesW(itemPath.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(itemPath.c_str());
            }
        }
    } while (FindNextFileW(hFind, &ffd));

    FindClose(hFind);
}

int SourcePriority(PluginSourceKind kind, bool isPackaged) noexcept {
    if (isPackaged) {
        return (kind == PluginSourceKind::MsixBundled) ? 3 : 1;
    }
    switch (kind) {
        case PluginSourceKind::LocalAppDir:  return 2;
        case PluginSourceKind::LocalUserDir: return 1;
        case PluginSourceKind::MsixBundled:  return 3;
    }
    return 0;
}

} // namespace

std::string PluginRecord::ResolveName(std::string_view lang) const {
    if (const PluginLocalizedText* loc = FindBestLocale(i18n, lang); loc && !loc->name.empty()) {
        return loc->name;
    }
    return id;
}

std::string PluginRecord::ResolveDesc(std::string_view lang) const {
    if (const PluginLocalizedText* loc = FindBestLocale(i18n, lang); loc && !loc->desc.empty()) {
        return loc->desc;
    }
    return {};
}

int PluginRegistry::CompareVersions(std::string_view a, std::string_view b) noexcept {
    auto normalizeSemVer = [](std::string_view& s, std::string_view& outPre) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == 'v' || s.front() == 'V')) {
            s.remove_prefix(1);
        }
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
            s.remove_suffix(1);
        }
        // Build metadata (+...) does not participate in SemVer precedence.
        const size_t plus = s.find('+');
        if (plus != std::string_view::npos) {
            s = s.substr(0, plus);
        }
        const size_t dash = s.find('-');
        if (dash != std::string_view::npos) {
            outPre = s.substr(dash + 1);
            s = s.substr(0, dash);
        } else {
            outPre = {};
        }
    };

    std::string_view preA;
    std::string_view preB;
    normalizeSemVer(a, preA);
    normalizeSemVer(b, preB);

    auto nextCorePart = [](std::string_view& s) -> uint64_t {
        if (s.empty()) return 0;
        const size_t dot = s.find('.');
        const std::string_view seg = (dot == std::string_view::npos) ? s : s.substr(0, dot);
        uint64_t val = 0;
        if (!seg.empty()) {
            std::from_chars(seg.data(), seg.data() + seg.size(), val);
        }
        if (dot == std::string_view::npos) {
            s = {};
        } else {
            s.remove_prefix(dot + 1);
        }
        return val;
    };

    while (!a.empty() || !b.empty()) {
        const uint64_t ia = nextCorePart(a);
        const uint64_t ib = nextCorePart(b);
        if (ia != ib) return (ia > ib) ? 1 : -1;
    }

    // Core parts match: release > pre-release.
    if (preA.empty() && preB.empty()) return 0;
    if (preA.empty()) return 1;
    if (preB.empty()) return -1;

    auto isAllDigits = [](std::string_view s) noexcept -> bool {
        if (s.empty()) return false;
        for (char c : s) {
            if (c < '0' || c > '9') return false;
        }
        return true;
    };
    auto nextDotSeg = [](std::string_view& s) noexcept -> std::string_view {
        const size_t dot = s.find('.');
        if (dot == std::string_view::npos) {
            std::string_view seg = s;
            s = {};
            return seg;
        }
        std::string_view seg = s.substr(0, dot);
        s.remove_prefix(dot + 1);
        return seg;
    };

    while (!preA.empty() && !preB.empty()) {
        const std::string_view sa = nextDotSeg(preA);
        const std::string_view sb = nextDotSeg(preB);
        const bool numA = isAllDigits(sa);
        const bool numB = isAllDigits(sb);
        if (numA && numB) {
            uint64_t va = 0, vb = 0;
            const auto ra = std::from_chars(sa.data(), sa.data() + sa.size(), va);
            const auto rb = std::from_chars(sb.data(), sb.data() + sb.size(), vb);
            if (ra.ec == std::errc{} && rb.ec == std::errc{}) {
                if (va != vb) return (va > vb) ? 1 : -1;
            } else if (sa != sb) {
                return (sa > sb) ? 1 : -1;
            }
        } else if (numA != numB) {
            // Numeric pre-release identifiers have lower precedence than non-numeric.
            return numA ? -1 : 1;
        } else if (sa != sb) {
            return (sa > sb) ? 1 : -1;
        }
    }
    if (preA.empty() && preB.empty()) return 0;
    return preA.empty() ? -1 : 1;
}

void PluginRegistry::Scan(const std::wstring& customRoot) {
    std::vector<PluginRecord> raw;
    const bool isPackaged = SystemInfo::IsPackagedApp();

    if (!customRoot.empty()) {
        if (IsExistingDirectory(customRoot.c_str())) {
            ScanDirectoryTree(customRoot, PluginSourceKind::LocalAppDir, 0, raw);
        }
    } else {
        const std::wstring exeDir = GetExeDir();
        if (!exeDir.empty()) {
            const std::wstring appPlugins = JoinPath(exeDir, L"plugins");
            if (IsExistingDirectory(appPlugins.c_str())) {
                ScanDirectoryTree(
                    appPlugins,
                    isPackaged ? PluginSourceKind::MsixBundled : PluginSourceKind::LocalAppDir,
                    0,
                    raw);
            }
        }

        if (!isPackaged) {
            const std::wstring localAppData = SystemInfo::GetLocalAppDataDir();
            if (!localAppData.empty()) {
                const std::wstring userPlugins = JoinPath(JoinPath(localAppData, L"QuickView"), L"plugins");
                if (IsExistingDirectory(userPlugins.c_str())) {
                    ScanDirectoryTree(userPlugins, PluginSourceKind::LocalUserDir, 0, raw);
                }
            }
        }
    }

    // Deduplicate records sharing the same plugin ID
    m_records.clear();
    m_records.reserve(raw.size());
    for (auto& cand : raw) {
        auto it = std::find_if(m_records.begin(), m_records.end(), [&](const PluginRecord& r) {
            return r.id == cand.id;
        });
        if (it == m_records.end()) {
            m_records.push_back(std::move(cand));
            continue;
        }
        const int verCmp = CompareVersions(cand.version, it->version);
        const bool preferCand =
            (isPackaged && cand.sourceKind == PluginSourceKind::MsixBundled && it->sourceKind != PluginSourceKind::MsixBundled) ||
            (verCmp > 0 && !(isPackaged && it->sourceKind == PluginSourceKind::MsixBundled)) ||
            (verCmp == 0 && SourcePriority(cand.sourceKind, isPackaged) > SourcePriority(it->sourceKind, isPackaged));
        if (preferCand) {
            *it = std::move(cand);
        }
    }
}

const PluginRecord* PluginRegistry::FindById(std::string_view pluginId) const noexcept {
    for (const auto& rec : m_records) {
        if (rec.id == pluginId) return &rec;
    }
    return nullptr;
}

std::vector<const PluginRecord*> PluginRegistry::GetByInterface(QVX_InterfaceID iface) const {
    std::vector<const PluginRecord*> out;
    for (const auto& rec : m_records) {
        if (rec.HasInterface(iface)) {
            out.push_back(&rec);
        }
    }
    return out;
}

} // namespace QuickView
