// QuickView File Association Center — Windows 10/11 registry engine, CLI path reconstruction, and probe API.
// Copyright (C) 2026-Present QuickView Contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include "pch.h"
#include "FileAssociationManager.h"
#include "SystemInfo.h"

#include <Shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <array>
#include <vector>

#pragma comment(lib, "uuid.lib")

namespace QuickView {

namespace {

constexpr wchar_t kLegacyImageProgId[]      = L"QuickView.Image";
constexpr wchar_t kProgIdPrefix[]           = L"QuickView.";
constexpr wchar_t kApplicationsKey[]        = L"Software\\Classes\\Applications\\QuickView.exe";
constexpr wchar_t kCapabilitiesKey[]        = L"Software\\QuickView\\Capabilities";
constexpr wchar_t kCapabilitiesAssocKey[]   = L"Software\\QuickView\\Capabilities\\FileAssociations";
constexpr wchar_t kRegisteredAppsKey[]      = L"Software\\RegisteredApplications";
constexpr wchar_t kContextMenuSubKey[]      = L"QuickViewOpen";
constexpr wchar_t kImageAssocShellKey[]     = L"Software\\Classes\\SystemFileAssociations\\image\\shell\\QuickViewOpen";
constexpr wchar_t kViewerChildArg[]         = L"--viewer-child";
constexpr wchar_t kFgCallerArg[]            = L"--fg-caller";
constexpr wchar_t kCleanupPidArg[]          = L"--cleanup-pid";

struct ExtMetaEntry {
    std::wstring_view ext;
    const wchar_t* description;
};

// Compile-time contiguous DOD lookup table for friendly type names in Windows Explorer.
constexpr std::array<ExtMetaEntry, 92> kExtMetadataTable = {{
    // Standard (11)
    { L".jpg",   L"JPEG Image" },
    { L".jpeg",  L"JPEG Image" },
    { L".jpe",   L"JPEG Image" },
    { L".jfif",  L"JPEG File Interchange Image" },
    { L".png",   L"PNG Image" },
    { L".bmp",   L"Bitmap Image" },
    { L".dib",   L"Device Independent Bitmap" },
    { L".gif",   L"GIF Animation / Image" },
    { L".tif",   L"TIFF Image" },
    { L".tiff",  L"TIFF Image" },
    { L".ico",   L"Windows Icon" },
    // Web / Modern (9)
    { L".webp",  L"WebP Image" },
    { L".avif",  L"AV1 Image (AVIF)" },
    { L".avifs", L"AVIF Image Sequence" },
    { L".heic",  L"HEIC Image" },
    { L".heif",  L"HEIF Image" },
    { L".svg",   L"Scalable Vector Graphics" },
    { L".svgz",  L"Compressed SVG Graphics" },
    { L".jxl",   L"JPEG XL Image" },
    { L".apng",  L"Animated PNG Image" },
    // Pro / Legacy (25)
    { L".exr",   L"OpenEXR HDR Image" },
    { L".hdr",   L"Radiance HDR Image" },
    { L".pic",   L"Radiance PIC Image" },
    { L".psd",   L"Adobe Photoshop Document" },
    { L".psb",   L"Adobe Photoshop Large Document" },
    { L".tga",   L"Truevision TGA Image" },
    { L".icb",   L"Truevision ICB Image" },
    { L".vda",   L"Truevision VDA Image" },
    { L".vst",   L"Truevision Vista Image" },
    { L".pcx",   L"PCX Paintbrush Image" },
    { L".qoi",   L"Quite OK Image (QOI)" },
    { L".wbmp",  L"Wireless Bitmap Image" },
    { L".pam",   L"Portable Arbitrary Map" },
    { L".pbm",   L"Portable Bitmap Image" },
    { L".pgm",   L"Portable Graymap Image" },
    { L".ppm",   L"Portable Pixmap Image" },
    { L".pnm",   L"Portable Anymap Image" },
    { L".wdp",   L"Windows Media Photo" },
    { L".hdp",   L"HD Photo Image" },
    { L".jxr",   L"JPEG XR Image" },
    { L".hif",   L"Camera HEIF Image" },
    { L".wmf",   L"Windows Metafile" },
    { L".emf",   L"Enhanced Metafile" },
    { L".wmz",   L"Compressed Windows Metafile" },
    { L".emz",   L"Compressed Enhanced Metafile" },
    // Camera RAW (22)
    { L".arw",   L"Sony Alpha RAW Image" },
    { L".cr2",   L"Canon RAW 2 Image" },
    { L".cr3",   L"Canon RAW 3 Image" },
    { L".crw",   L"Canon CIFF RAW Image" },
    { L".dng",   L"Adobe Digital Negative" },
    { L".nef",   L"Nikon Electronic Format RAW" },
    { L".orf",   L"Olympus RAW Image" },
    { L".raf",   L"Fujifilm RAW Image" },
    { L".rw2",   L"Panasonic RAW Image" },
    { L".srw",   L"Samsung RAW Image" },
    { L".x3f",   L"Sigma X3F RAW Image" },
    { L".mrw",   L"Minolta RAW Image" },
    { L".mos",   L"Leaf MOS RAW Image" },
    { L".kdc",   L"Kodak RAW Image" },
    { L".dcr",   L"Kodak Digital Camera RAW" },
    { L".sr2",   L"Sony SR2 RAW Image" },
    { L".pef",   L"Pentax Electronic File" },
    { L".erf",   L"Epson RAW Image" },
    { L".3fr",   L"Hasselblad 3F RAW Image" },
    { L".mef",   L"Mamiya RAW Image" },
    { L".nrw",   L"Nikon Coolpix RAW" },
    { L".raw",   L"Camera RAW Image" },
    // Extended RAW (21)
    { L".ari",   L"ARRI Alexa RAW Image" },
    { L".bay",   L"Casio Bayer RAW Image" },
    { L".braw",  L"Blackmagic RAW Image" },
    { L".cap",   L"Phase One Capture RAW" },
    { L".data",  L"Raw Sensor Data Image" },
    { L".dcs",   L"Kodak DCS RAW Image" },
    { L".drf",   L"Kodak DRF RAW Image" },
    { L".eip",   L"Phase One Enhanced Image" },
    { L".fff",   L"Hasselblad / Imacon RAW" },
    { L".gpr",   L"GoPro RAW Image" },
    { L".iiq",   L"Phase One IIQ RAW Image" },
    { L".k25",   L"Kodak DC25 RAW Image" },
    { L".mdc",   L"Minolta RD175 RAW Image" },
    { L".obm",   L"Olympus OBM RAW Image" },
    { L".ptx",   L"Pentax PTX RAW Image" },
    { L".pxn",   L"Logitech Fotoman RAW" },
    { L".r3d",   L"RED Digital Cinema RAW" },
    { L".rwl",   L"Leica RAW Image" },
    { L".rwz",   L"Rawzor Compressed RAW" },
    { L".srf",   L"Sony SRF RAW Image" },
    { L".sti",   L"Sinar CaptureShop RAW" },
    // Archives (4)
    { L".cbz",   L"Comic Book ZIP Archive" },
    { L".zip",   L"ZIP Image Archive" },
    { L".cbr",   L"Comic Book RAR Archive" },
    { L".rar",   L"RAR Image Archive" }
}};

static_assert(kExtMetadataTable.size() == ALL_ASSOCIABLE_EXTENSIONS.size(),
    "kExtMetadataTable must stay in 1:1 sync with ALL_ASSOCIABLE_EXTENSIONS");

bool DefaultWin32PathExists(std::wstring_view path) {
    if (path.empty()) return false;
    std::wstring buf(path);
    return GetFileAttributesW(buf.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring_view TrimWhitespace(std::wstring_view sv) {
    while (!sv.empty() && (sv.front() == L' ' || sv.front() == L'\t')) sv.remove_prefix(1);
    while (!sv.empty() && (sv.back() == L' ' || sv.back() == L'\t')) sv.remove_suffix(1);
    return sv;
}

bool StartsWithIgnoreCase(std::wstring_view s, std::wstring_view prefix) {
    if (s.size() < prefix.size()) return false;
    return ExtEqualsIgnoreCase(s.substr(0, prefix.size()), prefix);
}

std::wstring_view StripLeadingDot(std::wstring_view s) {
    if (!s.empty() && s.front() == L'.') s.remove_prefix(1);
    return s;
}

bool SameExtNormalized(std::wstring_view a, std::wstring_view b) {
    a = TrimWhitespace(a);
    b = TrimWhitespace(b);
    if (a.empty() || b.empty()) return false;
    return ExtEqualsIgnoreCase(StripLeadingDot(a), StripLeadingDot(b));
}

std::wstring ToLowerExt(std::wstring_view ext) {
    std::wstring res;
    res.reserve(ext.size());
    for (wchar_t c : ext) {
        res.push_back(ToLowerAscii(c));
    }
    return res;
}

std::wstring BuildOpenCommand(std::wstring_view exePath) {
    std::wstring cmd;
    cmd.reserve(exePath.size() + 8);
    cmd.push_back(L'"');
    cmd.append(exePath);
    cmd.append(L"\" \"%1\"");
    return cmd;
}

std::wstring BuildProgId(std::wstring_view ext) {
    ext = StripLeadingDot(ext);
    std::wstring pid;
    pid.reserve(10 + ext.size());
    pid.append(kProgIdPrefix);
    for (wchar_t c : ext) {
        pid.push_back(ToLowerAscii(c));
    }
    return pid;
}

std::wstring GetSystemDirCached() {
    static const std::wstring s_sysDir = []() -> std::wstring {
        wchar_t buf[MAX_PATH]{};
        UINT len = GetSystemDirectoryW(buf, MAX_PATH);
        return (len > 0 && len < MAX_PATH) ? std::wstring(buf, len) : std::wstring(L"C:\\Windows\\System32");
    }();
    return s_sysDir;
}

bool RegReadStringValue(HKEY hRoot, const wchar_t* subKey, const wchar_t* valueName, std::wstring& out) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(hRoot, subKey, 0, KEY_READ, &hKey) != ERROR_SUCCESS) return false;

    wchar_t buf[MAX_PATH * 4]{};
    DWORD bytes = sizeof(buf);
    DWORD type = 0;
    bool ok = false;
    if (RegQueryValueExW(hKey, valueName, nullptr, &type, reinterpret_cast<BYTE*>(buf), &bytes) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ))
    {
        size_t chars = bytes / sizeof(wchar_t);
        if (chars > 0 && buf[chars - 1] == L'\0') --chars;
        out.assign(buf, chars);
        ok = true;
    }
    RegCloseKey(hKey);
    return ok;
}

bool RegHasValue(HKEY hRoot, const wchar_t* subKey, const wchar_t* valueName) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(hRoot, subKey, 0, KEY_READ, &hKey) != ERROR_SUCCESS) return false;
    const LONG rc = RegQueryValueExW(hKey, valueName, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(hKey);
    return rc == ERROR_SUCCESS;
}

bool RegKeyExists(HKEY hRoot, const wchar_t* subKey) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(hRoot, subKey, 0, KEY_READ, &hKey) != ERROR_SUCCESS) return false;
    RegCloseKey(hKey);
    return true;
}

bool SafeRegWriteString(HKEY hRoot, const wchar_t* subKey, const wchar_t* valueName, const std::wstring& value, bool& modified) {
    std::wstring current;
    if (RegReadStringValue(hRoot, subKey, valueName, current) && current == value) {
        return true;
    }
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(hRoot, subKey, 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const LONG rc = RegSetValueExW(
        hKey, valueName, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(value.c_str()),
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(hKey);
    if (rc == ERROR_SUCCESS) {
        modified = true;
        return true;
    }
    return false;
}

bool SafeRegWriteEmptyString(HKEY hRoot, const wchar_t* subKey, const wchar_t* valueName, bool& modified) {
    if (RegHasValue(hRoot, subKey, valueName)) {
        return true;
    }
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(hRoot, subKey, 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const LONG rc = RegSetValueExW(hKey, valueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(L""), sizeof(wchar_t));
    RegCloseKey(hKey);
    if (rc == ERROR_SUCCESS) {
        modified = true;
        return true;
    }
    return false;
}

void SafeRegDeleteTree(HKEY hRoot, const wchar_t* subKey, bool& modified) {
    if (RegDeleteTreeW(hRoot, subKey) == ERROR_SUCCESS) {
        modified = true;
    }
}

void SafeRegDeleteValue(HKEY hRoot, const wchar_t* subKey, const wchar_t* valueName, bool& modified) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(hRoot, subKey, 0, KEY_WRITE, &hKey) != ERROR_SUCCESS) return;
    if (RegDeleteValueW(hKey, valueName) == ERROR_SUCCESS) {
        modified = true;
    }
    RegCloseKey(hKey);
}

// Removes all occurrences of legacy QuickView.Image and cleans per-extension remnants if requested.
void CleanupLegacyImageProgId(bool& modified) {
    SafeRegDeleteTree(HKEY_CURRENT_USER, L"Software\\Classes\\QuickView.Image", modified);
    for (const auto& ext : ALL_ASSOCIABLE_EXTENSIONS) {
        std::wstring extStr(ext);
        std::wstring owpKey = L"Software\\Classes\\" + extStr + L"\\OpenWithProgids";
        SafeRegDeleteValue(HKEY_CURRENT_USER, owpKey.c_str(), kLegacyImageProgId, modified);

        std::wstring extDef;
        std::wstring extKey = L"Software\\Classes\\" + extStr;
        if (RegReadStringValue(HKEY_CURRENT_USER, extKey.c_str(), nullptr, extDef) &&
            ExtEqualsIgnoreCase(extDef, kLegacyImageProgId))
        {
            SafeRegDeleteValue(HKEY_CURRENT_USER, extKey.c_str(), nullptr, modified);
        }
    }
}

void WriteContextMenuForKey(const wchar_t* shellSubKey, std::wstring_view label, const std::wstring& exePathStr, const std::wstring& cmd, bool& modified) {
    std::wstring labelStr(label);
    std::wstring iconVal = exePathStr + L",0";
    std::wstring cmdKey = std::wstring(shellSubKey) + L"\\command";
    SafeRegWriteString(HKEY_CURRENT_USER, shellSubKey, nullptr, labelStr, modified);
    SafeRegWriteString(HKEY_CURRENT_USER, shellSubKey, L"Icon", iconVal, modified);
    SafeRegWriteString(HKEY_CURRENT_USER, cmdKey.c_str(), nullptr, cmd, modified);
}

void TryDeleteEmptySubKey(HKEY hRoot, const wchar_t* subKey) {
    // RegDeleteKeyW only succeeds if the key has zero subkeys; safe for cleaning our own empty shells.
    RegDeleteKeyW(hRoot, subKey);
}

void DeleteContextMenuSubTreeAndCleanEmptyParents(const std::wstring& sfaRootKey, const std::wstring& menuSubKey, bool& modified) {
    const std::wstring fullKey = sfaRootKey + L"\\shell\\" + menuSubKey;
    SafeRegDeleteTree(HKEY_CURRENT_USER, fullKey.c_str(), modified);
    TryDeleteEmptySubKey(HKEY_CURRENT_USER, (sfaRootKey + L"\\shell").c_str());
    TryDeleteEmptySubKey(HKEY_CURRENT_USER, sfaRootKey.c_str());
}

void RemoveContextMenuForAll(bool& modified) {
    SafeRegDeleteTree(HKEY_CURRENT_USER, kImageAssocShellKey, modified);
    TryDeleteEmptySubKey(HKEY_CURRENT_USER, L"Software\\Classes\\SystemFileAssociations\\image\\shell");
    TryDeleteEmptySubKey(HKEY_CURRENT_USER, L"Software\\Classes\\SystemFileAssociations\\image");
    for (const auto& ext : ALL_ASSOCIABLE_EXTENSIONS) {
        const std::wstring sfaRoot = L"Software\\Classes\\SystemFileAssociations\\" + ToLowerExt(ext);
        DeleteContextMenuSubTreeAndCleanEmptyParents(sfaRoot, kContextMenuSubKey, modified);
    }
}

bool IsPerExtContextMenuNeeded(std::wstring_view ext) {
    // Windows built-in SystemFileAssociations\image covers standard web/raster extensions,
    // while RAWs, PSD, EXR, QOI, archives need explicit per-extension entries for context menu coverage.
    AssocExtCategory cat = GetCategoryOfExtension(ext);
    return cat == AssocExtCategory::ProLegacy ||
           cat == AssocExtCategory::CameraRaw ||
           cat == AssocExtCategory::ExtendedRaw ||
           cat == AssocExtCategory::Archive ||
           ExtEqualsIgnoreCase(ext, L".webp") ||
           ExtEqualsIgnoreCase(ext, L".avif") ||
           ExtEqualsIgnoreCase(ext, L".avifs") ||
           ExtEqualsIgnoreCase(ext, L".heic") ||
           ExtEqualsIgnoreCase(ext, L".heif") ||
           ExtEqualsIgnoreCase(ext, L".jxl") ||
           ExtEqualsIgnoreCase(ext, L".svg") ||
           ExtEqualsIgnoreCase(ext, L".svgz") ||
           ExtEqualsIgnoreCase(ext, L".apng");
}

std::wstring UrlEncodeQueryValue(std::wstring_view s) {
    static constexpr wchar_t kHex[] = L"0123456789ABCDEF";
    std::wstring out;
    out.reserve(s.size() + 8);
    for (wchar_t ch : s) {
        const bool isUnreserved =
            (ch >= L'A' && ch <= L'Z') ||
            (ch >= L'a' && ch <= L'z') ||
            (ch >= L'0' && ch <= L'9') ||
            ch == L'-' || ch == L'_' || ch == L'.' || ch == L'~';
        if (isUnreserved) {
            out.push_back(ch);
        } else {
            const uint32_t code = static_cast<uint32_t>(ch) & 0xFFu;
            out.push_back(L'%');
            out.push_back(kHex[(code >> 4) & 0xF]);
            out.push_back(kHex[code & 0xF]);
        }
    }
    return out;
}

struct ProgIdOwnerCacheEntry {
    std::wstring progId;
    bool owned = false;
};

bool ContainsIgnoreCase(std::wstring_view haystack, std::wstring_view needle) {
    if (needle.empty() || haystack.size() < needle.size()) return false;
    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        if (ExtEqualsIgnoreCase(haystack.substr(i, needle.size()), needle)) {
            return true;
        }
    }
    return false;
}

bool IsProgIdOwnedByQuickView(std::wstring_view progId, std::wstring_view currentAumid,
                              std::wstring_view exePath, std::vector<ProgIdOwnerCacheEntry>& cache)
{
    if (progId.empty()) return false;
    if (StartsWithIgnoreCase(progId, kProgIdPrefix)) {
        return true;
    }
    // Check MSIX / Store AppX* generated ProgIDs via HKCR\<ProgId>\Application\AppUserModelID
    if (!StartsWithIgnoreCase(progId, L"AppX")) {
        return false;
    }

    for (const auto& entry : cache) {
        if (ExtEqualsIgnoreCase(entry.progId, progId)) {
            return entry.owned;
        }
    }

    bool owned = false;
    const std::wstring progIdStr(progId);
    if (!currentAumid.empty()) {
        std::wstring regAumid;
        if (RegReadStringValue(HKEY_CLASSES_ROOT, (progIdStr + L"\\Application").c_str(), L"AppUserModelID", regAumid) &&
            ExtEqualsIgnoreCase(regAumid, currentAumid))
        {
            owned = true;
        }
    }
    if (!owned && !exePath.empty()) {
        std::wstring regCmd;
        if (RegReadStringValue(HKEY_CLASSES_ROOT, (progIdStr + L"\\shell\\open\\command").c_str(), nullptr, regCmd)) {
            if (ContainsIgnoreCase(regCmd, exePath) ||
                ContainsIgnoreCase(regCmd, L"VivorLoong.QuickView-FastFileViewer"))
            {
                owned = true;
            }
        }
    }

    cache.push_back({ progIdStr, owned });
    return owned;
}

void IterateCsvTokens(std::wstring_view csv, void (*visitor)(std::wstring_view token, void* ctx), void* ctx) {
    size_t pos = 0;
    while (pos < csv.size()) {
        size_t comma = csv.find(L',', pos);
        if (comma == std::wstring_view::npos) comma = csv.size();
        std::wstring_view token = TrimWhitespace(csv.substr(pos, comma - pos));
        if (!token.empty()) {
            visitor(token, ctx);
        }
        pos = comma + 1;
    }
}

std::wstring BuildCanonicalCsvFromMask(const std::array<bool, ALL_ASSOCIABLE_EXTENSIONS.size()>& mask) {
    std::wstring result;
    result.reserve(256);
    for (size_t i = 0; i < ALL_ASSOCIABLE_EXTENSIONS.size(); ++i) {
        if (!mask[i]) continue;
        if (!result.empty()) result.push_back(L',');
        result.append(ALL_ASSOCIABLE_EXTENSIONS[i]);
    }
    return result;
}

std::array<bool, ALL_ASSOCIABLE_EXTENSIONS.size()> ParseCsvMask(std::wstring_view csv) {
    std::array<bool, ALL_ASSOCIABLE_EXTENSIONS.size()> mask{};
    struct MaskCtx {
        std::array<bool, ALL_ASSOCIABLE_EXTENSIONS.size()>* pMask;
    } ctx{ &mask };

    IterateCsvTokens(csv, [](std::wstring_view token, void* rawCtx) {
        auto* c = static_cast<MaskCtx*>(rawCtx);
        for (size_t i = 0; i < ALL_ASSOCIABLE_EXTENSIONS.size(); ++i) {
            if (SameExtNormalized(token, ALL_ASSOCIABLE_EXTENSIONS[i])) {
                (*c->pMask)[i] = true;
                break;
            }
        }
    }, &ctx);
    return mask;
}

} // namespace

// ============================================================================
// Pure / Testable Helpers
// ============================================================================

std::wstring ReconstructPathFromArgs(int argc, const wchar_t* const* argv, PathExistsFn existsFn) {
    if (!argv || argc <= 1) return {};
    if (!existsFn) existsFn = &DefaultWin32PathExists;

    // Pattern 1: --viewer-child "path"
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] && _wcsicmp(argv[i], kViewerChildArg) == 0) {
            return argv[i + 1] ? std::wstring(argv[i + 1]) : std::wstring{};
        }
    }

    // Pattern 2: Normal launch — skip flags (and known flag value pairs like --fg-caller <hwnd>, --cleanup-pid <pid>)
    int firstPathIdx = -1;
    for (int i = 1; i < argc; ++i) {
        if (!argv[i] || argv[i][0] == L'\0') continue;
        if (_wcsicmp(argv[i], kFgCallerArg) == 0 || _wcsicmp(argv[i], kCleanupPidArg) == 0) {
            if (i + 1 < argc) ++i; // Skip the paired value argument
            continue;
        }
        if (argv[i][0] != L'-') {
            firstPathIdx = i;
            break;
        }
    }

    if (firstPathIdx < 0) return {};

    // Fast path: single argument already exists on disk (quoted path or path without spaces).
    if (existsFn(argv[firstPathIdx])) {
        return std::wstring(argv[firstPathIdx]);
    }

    // Fallback: greedy space-separated reconstruction for unquoted paths (e.g. legacy %1).
    std::wstring candidate = argv[firstPathIdx];
    for (int j = firstPathIdx + 1; j < argc; ++j) {
        if (!argv[j] || argv[j][0] == L'\0') continue;
        if (argv[j][0] == L'-') break;
        candidate.push_back(L' ');
        candidate.append(argv[j]);
        if (existsFn(candidate)) {
            return candidate;
        }
    }

    // Return original first token so caller can present an informative "File not found" OSD.
    return std::wstring(argv[firstPathIdx]);
}

bool IsCommandLineTemplateValid(std::wstring_view regCommand, std::wstring_view exePath) {
    if (exePath.empty()) return false;
    regCommand = TrimWhitespace(regCommand);
    std::wstring expected = BuildOpenCommand(exePath);
    return ExtEqualsIgnoreCase(regCommand, expected);
}

uint64_t ComputeAssocOptionsHash(const FileAssocOptions& opts) {
    // FNV-1a 64-bit hash over canonical mask + iconStyle + contextMenu settings.
    constexpr uint64_t kOffsetBasis = 14695981039346656037ULL;
    constexpr uint64_t kPrime       = 1099511628211ULL;

    auto mixByte = [](uint64_t& h, uint8_t b) {
        h ^= b;
        h *= kPrime;
    };

    uint64_t h = kOffsetBasis;
    const auto mask = ParseCsvMask(opts.selectedExtsCsv);
    for (size_t i = 0; i < mask.size(); ++i) {
        mixByte(h, mask[i] ? 1u : 0u);
    }
    mixByte(h, static_cast<uint8_t>(static_cast<int>(opts.iconStyle) & 0xFF));
    mixByte(h, opts.enableContextMenu ? 1u : 0u);
    for (wchar_t c : opts.contextMenuLabel) {
        mixByte(h, static_cast<uint8_t>(c & 0xFF));
        mixByte(h, static_cast<uint8_t>((c >> 8) & 0xFF));
    }
    return h;
}

std::wstring FormatAssocOptionsHash(uint64_t hash) {
    wchar_t buf[20]{};
    swprintf_s(buf, L"%016llX", static_cast<unsigned long long>(hash));
    return std::wstring(buf);
}

bool IsExtensionInCsv(std::wstring_view csv, std::wstring_view ext) {
    struct FindCtx {
        std::wstring_view target;
        bool found = false;
    } ctx{ ext, false };

    IterateCsvTokens(csv, [](std::wstring_view token, void* rawCtx) {
        auto* c = static_cast<FindCtx*>(rawCtx);
        if (!c->found && SameExtNormalized(token, c->target)) {
            c->found = true;
        }
    }, &ctx);
    return ctx.found;
}

std::wstring ToggleExtensionInCsv(std::wstring_view csv, std::wstring_view ext, bool enable) {
    auto mask = ParseCsvMask(csv);
    for (size_t i = 0; i < ALL_ASSOCIABLE_EXTENSIONS.size(); ++i) {
        if (SameExtNormalized(ext, ALL_ASSOCIABLE_EXTENSIONS[i])) {
            mask[i] = enable;
            break;
        }
    }
    return BuildCanonicalCsvFromMask(mask);
}

std::wstring SetCategoryInCsv(std::wstring_view csv, AssocExtCategory cat, bool enable) {
    auto mask = ParseCsvMask(csv);
    for (size_t i = 0; i < ALL_ASSOCIABLE_EXTENSIONS.size(); ++i) {
        if (GetCategoryOfExtension(ALL_ASSOCIABLE_EXTENSIONS[i]) == cat) {
            mask[i] = enable;
        }
    }
    return BuildCanonicalCsvFromMask(mask);
}

int CountCategoryInCsv(std::wstring_view csv, AssocExtCategory cat) {
    const auto mask = ParseCsvMask(csv);
    int count = 0;
    for (size_t i = 0; i < ALL_ASSOCIABLE_EXTENSIONS.size(); ++i) {
        if (mask[i] && GetCategoryOfExtension(ALL_ASSOCIABLE_EXTENSIONS[i]) == cat) {
            ++count;
        }
    }
    return count;
}

std::wstring GetPresetCsv(AssocPreset preset) {
    switch (preset) {
        case AssocPreset::Recommended: return GetRecommendedAssocExtensionsString();
        case AssocPreset::AllImages:   return GetAllImagesAssocExtensionsString();
        case AssocPreset::PhotoAndRaw: return GetPhotoAndRawAssocExtensionsString();
        case AssocPreset::ClearAll:    return {};
        default:                       return {};
    }
}

const wchar_t* GetExtensionTypeDescription(std::wstring_view ext) {
    for (const auto& entry : kExtMetadataTable) {
        if (ExtEqualsIgnoreCase(entry.ext, ext)) {
            return entry.description;
        }
    }
    return L"QuickView Image";
}

std::wstring GetExtensionNativeIcon(std::wstring_view ext, std::wstring_view sysDir) {
    if (ExtEqualsIgnoreCase(ext, L".ico")) {
        return L"%1";
    }
    std::wstring base(sysDir);
    if (ExtEqualsIgnoreCase(ext, L".png") || ExtEqualsIgnoreCase(ext, L".apng")) {
        return base + L"\\imageres.dll,-83";
    }
    if (ExtEqualsIgnoreCase(ext, L".gif")) {
        return base + L"\\imageres.dll,-71";
    }
    if (ExtEqualsIgnoreCase(ext, L".tif") || ExtEqualsIgnoreCase(ext, L".tiff")) {
        return base + L"\\imageres.dll,-122";
    }
    if (IsArchiveExtension(ext)) {
        return base + L"\\imageres.dll,-174";
    }
    return base + L"\\imageres.dll,-72";
}

bool ShouldIncludeExtInContextMenu(std::wstring_view ext, bool isSelectedInAssoc) {
    // Plain archive containers (.zip/.rar) are only given a context menu entry when the user explicitly opts in,
    // avoiding menu clutter on software/document archives. All 88 image/RAW formats and comic archives (.cbz/.cbr)
    // always receive context menu support when Explorer context menu integration is enabled.
    if (SameExtNormalized(ext, L".zip") || SameExtNormalized(ext, L".rar")) {
        return isSelectedInAssoc;
    }
    return IsAssociableExtension(ext) ||
           SameExtNormalized(ext, L".cbz") ||
           SameExtNormalized(ext, L".cbr");
}

std::wstring BuildDefaultAppsSettingsUri(bool isWin11, bool isPackaged, std::wstring_view aumid) {
    if (isWin11) {
        if (isPackaged) {
            return !aumid.empty()
                ? (L"ms-settings:defaultapps?registeredAUMID=" + UrlEncodeQueryValue(aumid))
                : L"ms-settings:defaultapps";
        }
        return L"ms-settings:defaultapps?registeredAppUser=QuickView";
    }
    // Windows 10 opens the Settings -> Default Apps page (Photo Viewer one-click selector)
    // because legacy LaunchAdvancedAssociationUI silently no-ops on Win10 1803+.
    return L"ms-settings:defaultapps";
}

// ============================================================================
// Windows 10/11 Registry & Shell Engine
// ============================================================================

std::wstring GetCurrentExecutablePath() {
    wchar_t buf[MAX_PATH * 2]{};
    DWORD len = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return (len > 0 && len < std::size(buf)) ? std::wstring(buf, len) : std::wstring{};
}

bool IsContextMenuEntryValid(const wchar_t* shellSubKey, std::wstring_view expectedLabel,
                             std::wstring_view expectedIcon, std::wstring_view exePath)
{
    std::wstring regLabel;
    if (!RegReadStringValue(HKEY_CURRENT_USER, shellSubKey, nullptr, regLabel) ||
        regLabel != expectedLabel)
    {
        return false;
    }
    std::wstring regIcon;
    if (!RegReadStringValue(HKEY_CURRENT_USER, shellSubKey, L"Icon", regIcon) ||
        !ExtEqualsIgnoreCase(regIcon, expectedIcon))
    {
        return false;
    }
    std::wstring cmdKey = std::wstring(shellSubKey) + L"\\command";
    std::wstring regCmd;
    return RegReadStringValue(HKEY_CURRENT_USER, cmdKey.c_str(), nullptr, regCmd) &&
           IsCommandLineTemplateValid(regCmd, exePath);
}

bool IsFileAssociationSyncNeeded(const FileAssocOptions& opts, std::wstring_view exePath) {
    if (exePath.empty() || SystemInfo::IsPackagedApp()) return false;

    // 1. Always migrate away from legacy QuickView.Image pollution.
    if (RegKeyExists(HKEY_CURRENT_USER, L"Software\\Classes\\QuickView.Image")) {
        return true;
    }

    const auto mask = ParseCsvMask(opts.selectedExtsCsv);
    bool anySelected = false;
    for (bool b : mask) {
        if (b) { anySelected = true; break; }
    }

    const std::wstring exePathStr(exePath);
    const std::wstring expectedCtxIcon = exePathStr + L",0";
    const std::wstring_view expectedCtxLabel = opts.contextMenuLabel.empty()
        ? std::wstring_view(L"Open with QuickView")
        : opts.contextMenuLabel;

    // 2. Check Context Menu parity (both generic image handler and per-extension keys).
    const bool hasCtxMenu = RegKeyExists(HKEY_CURRENT_USER, kImageAssocShellKey);
    if (hasCtxMenu != opts.enableContextMenu) {
        return true;
    }
    if (opts.enableContextMenu &&
        !IsContextMenuEntryValid(kImageAssocShellKey, expectedCtxLabel, expectedCtxIcon, exePath))
    {
        return true;
    }

    // 3. Check top-level Applications & Capabilities presence parity.
    if (anySelected) {
        std::wstring appCmd;
        if (!RegReadStringValue(HKEY_CURRENT_USER,
                L"Software\\Classes\\Applications\\QuickView.exe\\shell\\open\\command", nullptr, appCmd) ||
            !IsCommandLineTemplateValid(appCmd, exePath))
        {
            return true;
        }
        if (!RegHasValue(HKEY_CURRENT_USER, kRegisteredAppsKey, L"QuickView")) {
            return true;
        }
    } else {
        if (RegHasValue(HKEY_CURRENT_USER, kRegisteredAppsKey, L"QuickView") ||
            RegKeyExists(HKEY_CURRENT_USER, kCapabilitiesKey) ||
            RegKeyExists(HKEY_CURRENT_USER, kApplicationsKey))
        {
            return true;
        }
    }

    const std::wstring sysDir = GetSystemDirCached();

    // 4. Verify every extension for ProgID parity, icon style, command template, and per-extension context menu cleanup.
    for (size_t i = 0; i < ALL_ASSOCIABLE_EXTENSIONS.size(); ++i) {
        const std::wstring extStr = ToLowerExt(ALL_ASSOCIABLE_EXTENSIONS[i]);
        const std::wstring progId = BuildProgId(extStr);
        const std::wstring progCmdKey = L"Software\\Classes\\" + progId + L"\\shell\\open\\command";

        if (mask[i]) {
            std::wstring regCmd;
            if (!RegReadStringValue(HKEY_CURRENT_USER, progCmdKey.c_str(), nullptr, regCmd) ||
                !IsCommandLineTemplateValid(regCmd, exePath))
            {
                return true;
            }
            const std::wstring expectedIcon = (opts.iconStyle == AssocIconStyle::QuickViewLogo)
                ? expectedCtxIcon
                : GetExtensionNativeIcon(extStr, sysDir);
            std::wstring regIcon;
            if (!RegReadStringValue(HKEY_CURRENT_USER, (L"Software\\Classes\\" + progId + L"\\DefaultIcon").c_str(), nullptr, regIcon) ||
                !ExtEqualsIgnoreCase(regIcon, expectedIcon))
            {
                return true;
            }
            if (!RegHasValue(HKEY_CURRENT_USER, kCapabilitiesAssocKey, extStr.c_str())) {
                return true;
            }
        } else {
            if (RegKeyExists(HKEY_CURRENT_USER, (L"Software\\Classes\\" + progId).c_str()) ||
                RegHasValue(HKEY_CURRENT_USER, (L"Software\\Classes\\" + extStr + L"\\OpenWithProgids").c_str(), progId.c_str()))
            {
                return true;
            }
        }

        const bool wantExtCtx = opts.enableContextMenu &&
            ShouldIncludeExtInContextMenu(extStr, mask[i]) &&
            IsPerExtContextMenuNeeded(extStr);
        const std::wstring sfaKey = L"Software\\Classes\\SystemFileAssociations\\" + extStr + L"\\shell\\" + kContextMenuSubKey;
        const bool hasExtCtx = RegKeyExists(HKEY_CURRENT_USER, sfaKey.c_str());
        if (hasExtCtx != wantExtCtx) {
            return true;
        }
        if (wantExtCtx && !IsContextMenuEntryValid(sfaKey.c_str(), expectedCtxLabel, expectedCtxIcon, exePath)) {
            return true;
        }
    }

    return false;
}

bool SyncFileAssociations(const FileAssocOptions& opts, std::wstring_view exePath) {
    if (SystemInfo::IsPackagedApp()) return true;
    if (exePath.empty()) return false;

    const std::wstring exePathStr(exePath);
    const std::wstring cmd = BuildOpenCommand(exePathStr);
    const std::wstring sysDir = GetSystemDirCached();
    bool modified = false;

    // 1. Clean legacy generic QuickView.Image ProgID & references (eliminates issue #265 source).
    CleanupLegacyImageProgId(modified);

    const auto mask = ParseCsvMask(opts.selectedExtsCsv);
    bool anySelected = false;
    for (bool b : mask) {
        if (b) { anySelected = true; break; }
    }

    // 2. Sync all 92 extensions differentially.
    for (size_t i = 0; i < ALL_ASSOCIABLE_EXTENSIONS.size(); ++i) {
        const std::wstring extStr = ToLowerExt(ALL_ASSOCIABLE_EXTENSIONS[i]);
        const std::wstring progId = BuildProgId(extStr);
        const std::wstring progKey = L"Software\\Classes\\" + progId;
        const std::wstring owpKey  = L"Software\\Classes\\" + extStr + L"\\OpenWithProgids";
        const std::wstring extKey  = L"Software\\Classes\\" + extStr;

        if (mask[i]) {
            // Create/Update per-extension ProgID
            SafeRegWriteString(HKEY_CURRENT_USER, progKey.c_str(), L"FriendlyTypeName", GetExtensionTypeDescription(extStr), modified);
            SafeRegWriteString(HKEY_CURRENT_USER, (progKey + L"\\shell\\open\\command").c_str(), nullptr, cmd, modified);

            const std::wstring iconVal = (opts.iconStyle == AssocIconStyle::QuickViewLogo)
                ? (exePathStr + L",0")
                : GetExtensionNativeIcon(extStr, sysDir);
            SafeRegWriteString(HKEY_CURRENT_USER, (progKey + L"\\DefaultIcon").c_str(), nullptr, iconVal, modified);

            // Add to OpenWithProgids
            SafeRegWriteEmptyString(HKEY_CURRENT_USER, owpKey.c_str(), progId.c_str(), modified);

            // Set HKCU\.ext default only if currently empty or owned by QuickView (safe direct-associate for unclaimed formats)
            std::wstring currentDef;
            if (!RegReadStringValue(HKEY_CURRENT_USER, extKey.c_str(), nullptr, currentDef) ||
                currentDef.empty() ||
                StartsWithIgnoreCase(currentDef, kProgIdPrefix))
            {
                SafeRegWriteString(HKEY_CURRENT_USER, extKey.c_str(), nullptr, progId, modified);
            }

            // Capabilities & SupportedTypes
            SafeRegWriteString(HKEY_CURRENT_USER, kCapabilitiesAssocKey, extStr.c_str(), progId, modified);
            SafeRegWriteEmptyString(HKEY_CURRENT_USER,
                L"Software\\Classes\\Applications\\QuickView.exe\\SupportedTypes", extStr.c_str(), modified);
        } else {
            // Cleanly retract unchecked extension
            SafeRegDeleteTree(HKEY_CURRENT_USER, progKey.c_str(), modified);
            SafeRegDeleteValue(HKEY_CURRENT_USER, owpKey.c_str(), progId.c_str(), modified);

            std::wstring currentDef;
            if (RegReadStringValue(HKEY_CURRENT_USER, extKey.c_str(), nullptr, currentDef) &&
                (ExtEqualsIgnoreCase(currentDef, progId) || ExtEqualsIgnoreCase(currentDef, kLegacyImageProgId)))
            {
                SafeRegDeleteValue(HKEY_CURRENT_USER, extKey.c_str(), nullptr, modified);
            }

            SafeRegDeleteValue(HKEY_CURRENT_USER, kCapabilitiesAssocKey, extStr.c_str(), modified);
            SafeRegDeleteValue(HKEY_CURRENT_USER,
                L"Software\\Classes\\Applications\\QuickView.exe\\SupportedTypes", extStr.c_str(), modified);
        }
    }

    // 3. Applications & Capabilities top-level identity
    if (anySelected) {
        SafeRegWriteString(HKEY_CURRENT_USER, kApplicationsKey, L"FriendlyAppName", L"QuickView", modified);
        SafeRegWriteString(HKEY_CURRENT_USER,
            L"Software\\Classes\\Applications\\QuickView.exe\\shell\\open\\command", nullptr, cmd, modified);

        SafeRegWriteString(HKEY_CURRENT_USER, kCapabilitiesKey, L"ApplicationDescription", L"QuickView Image Viewer", modified);
        SafeRegWriteString(HKEY_CURRENT_USER, kCapabilitiesKey, L"ApplicationName", L"QuickView", modified);
        SafeRegWriteString(HKEY_CURRENT_USER, kRegisteredAppsKey, L"QuickView", kCapabilitiesKey, modified);
    } else {
        SafeRegDeleteTree(HKEY_CURRENT_USER, kApplicationsKey, modified);
        SafeRegDeleteTree(HKEY_CURRENT_USER, kCapabilitiesKey, modified);
        SafeRegDeleteValue(HKEY_CURRENT_USER, kRegisteredAppsKey, L"QuickView", modified);
    }

    // 4. Optional Explorer Context Menu ("Open with QuickView")
    if (opts.enableContextMenu) {
        std::wstring_view label = opts.contextMenuLabel.empty() ? std::wstring_view(L"Open with QuickView") : opts.contextMenuLabel;
        WriteContextMenuForKey(kImageAssocShellKey, label, exePathStr, cmd, modified);

        for (size_t i = 0; i < ALL_ASSOCIABLE_EXTENSIONS.size(); ++i) {
            const std::wstring extStr = ToLowerExt(ALL_ASSOCIABLE_EXTENSIONS[i]);
            const std::wstring sfaRoot = L"Software\\Classes\\SystemFileAssociations\\" + extStr;
            const std::wstring sfaKey = sfaRoot + L"\\shell\\" + kContextMenuSubKey;
            if (ShouldIncludeExtInContextMenu(extStr, mask[i]) && IsPerExtContextMenuNeeded(extStr)) {
                WriteContextMenuForKey(sfaKey.c_str(), label, exePathStr, cmd, modified);
            } else {
                DeleteContextMenuSubTreeAndCleanEmptyParents(sfaRoot, kContextMenuSubKey, modified);
            }
        }
    } else {
        RemoveContextMenuForAll(modified);
    }

    // 5. Notify Shell only when actual edits occurred (zero unnecessary icon flicker).
    if (modified) {
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    }
    return true;
}

void UnregisterAllAssociations() {
    if (SystemInfo::IsPackagedApp()) return;
    bool modified = false;

    CleanupLegacyImageProgId(modified);
    SafeRegDeleteTree(HKEY_CURRENT_USER, kApplicationsKey, modified);
    SafeRegDeleteTree(HKEY_CURRENT_USER, kCapabilitiesKey, modified);
    SafeRegDeleteValue(HKEY_CURRENT_USER, kRegisteredAppsKey, L"QuickView", modified);
    RemoveContextMenuForAll(modified);

    for (const auto& ext : ALL_ASSOCIABLE_EXTENSIONS) {
        const std::wstring extStr = ToLowerExt(ext);
        const std::wstring progId = BuildProgId(extStr);
        SafeRegDeleteTree(HKEY_CURRENT_USER, (L"Software\\Classes\\" + progId).c_str(), modified);
        SafeRegDeleteValue(HKEY_CURRENT_USER, (L"Software\\Classes\\" + extStr + L"\\OpenWithProgids").c_str(), progId.c_str(), modified);

        std::wstring currentDef;
        std::wstring extKey = L"Software\\Classes\\" + extStr;
        if (RegReadStringValue(HKEY_CURRENT_USER, extKey.c_str(), nullptr, currentDef) &&
            ExtEqualsIgnoreCase(currentDef, progId))
        {
            SafeRegDeleteValue(HKEY_CURRENT_USER, extKey.c_str(), nullptr, modified);
        }
    }

    if (modified) {
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    }
}

FileAssocProbeSummary ProbeFileAssociations(std::wstring_view selectedExtsCsv, std::wstring_view exePath) {
    FileAssocProbeSummary summary{};
    const auto mask = ParseCsvMask(selectedExtsCsv);
    const bool isPackaged = SystemInfo::IsPackagedApp();
    const std::wstring& currentAumid = SystemInfo::GetPackagedAumid();
    std::vector<ProgIdOwnerCacheEntry> ownerCache;
    ownerCache.reserve(16);

    if (!isPackaged && !exePath.empty() && RegKeyExists(HKEY_CURRENT_USER, L"Software\\Classes\\QuickView.Image")) {
        std::wstring legacyCmd;
        if (RegReadStringValue(HKEY_CURRENT_USER, L"Software\\Classes\\QuickView.Image\\shell\\open\\command", nullptr, legacyCmd) &&
            !IsCommandLineTemplateValid(legacyCmd, exePath))
        {
            summary.hasBrokenCommand = true;
        }
    }

    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool didInitCo = SUCCEEDED(hrCo);
    IApplicationAssociationRegistration* pAAR = nullptr;
    if (SUCCEEDED(CoCreateInstance(
            CLSID_ApplicationAssociationRegistration,
            nullptr, CLSCTX_INPROC_SERVER,
            IID_IApplicationAssociationRegistration,
            reinterpret_cast<void**>(&pAAR))) == false)
    {
        pAAR = nullptr;
    }

    for (size_t i = 0; i < ALL_ASSOCIABLE_EXTENSIONS.size(); ++i) {
        const std::wstring extStr = ToLowerExt(ALL_ASSOCIABLE_EXTENSIONS[i]);
        const std::wstring expectedProgId = BuildProgId(extStr);
        const std::wstring progCmdKey = L"Software\\Classes\\" + expectedProgId + L"\\shell\\open\\command";

        if (mask[i]) {
            ++summary.selectedCount;
        }

        // Check command health if ProgID exists (desktop unpackaged mode only)
        std::wstring regCmd;
        if (!isPackaged && !exePath.empty() && RegReadStringValue(HKEY_CURRENT_USER, progCmdKey.c_str(), nullptr, regCmd)) {
            if (!IsCommandLineTemplateValid(regCmd, exePath)) {
                summary.hasBrokenCommand = true;
            }
        }

        // Query current Windows effective handler (supports both Win32 QuickView.* and MSIX AppX* ProgIDs)
        bool isDefault = false;
        if (pAAR) {
            LPWSTR currentDefault = nullptr;
            if (SUCCEEDED(pAAR->QueryCurrentDefault(extStr.c_str(), AT_FILEEXTENSION, AL_EFFECTIVE, &currentDefault)) &&
                currentDefault)
            {
                isDefault = IsProgIdOwnedByQuickView(currentDefault, currentAumid, exePath, ownerCache);
                CoTaskMemFree(currentDefault);
            }
        } else {
            // Fallback registry check when COM unavailable
            std::wstring ucProgId;
            std::wstring ucKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\" + extStr + L"\\UserChoice";
            if (RegReadStringValue(HKEY_CURRENT_USER, ucKey.c_str(), L"ProgId", ucProgId)) {
                isDefault = IsProgIdOwnedByQuickView(ucProgId, currentAumid, exePath, ownerCache);
            } else {
                std::wstring extDef;
                if (RegReadStringValue(HKEY_CURRENT_USER, (L"Software\\Classes\\" + extStr).c_str(), nullptr, extDef)) {
                    isDefault = IsProgIdOwnedByQuickView(extDef, currentAumid, exePath, ownerCache);
                }
            }
        }

        const bool isRegisteredOpenWith = isPackaged ||
            RegKeyExists(HKEY_CURRENT_USER, (L"Software\\Classes\\" + expectedProgId).c_str()) ||
            RegHasValue(HKEY_CURRENT_USER, (L"Software\\Classes\\" + extStr + L"\\OpenWithProgids").c_str(), expectedProgId.c_str());

        if (isDefault) {
            summary.states[i] = ExtAssocState::IsSystemDefault;
            if (isPackaged || mask[i]) ++summary.defaultCount;
        } else if (isRegisteredOpenWith) {
            summary.states[i] = ExtAssocState::RegisteredOpenWith;
            if (isPackaged || mask[i]) ++summary.openWithOnlyCount;
        } else {
            summary.states[i] = ExtAssocState::NotRegistered;
        }
    }

    if (pAAR) pAAR->Release();
    if (didInitCo) CoUninitialize();

    return summary;
}

bool OpenSystemDefaultAppsSettings() {
    const bool isPackaged = SystemInfo::IsPackagedApp();
    if (!isPackaged && !RegHasValue(HKEY_CURRENT_USER, kRegisteredAppsKey, L"QuickView")) {
        // Ensure QuickView is registered in Capabilities before opening the Windows Default Apps page.
        const std::wstring recommendedCsv = GetRecommendedAssocExtensionsString();
        FileAssocOptions fallbackOpts;
        fallbackOpts.selectedExtsCsv = recommendedCsv;
        SyncFileAssociations(fallbackOpts, GetCurrentExecutablePath());
    }

    const std::wstring targetUri = BuildDefaultAppsSettingsUri(
        SystemInfo::IsWindows11OrGreater(),
        isPackaged,
        SystemInfo::GetPackagedAumid());

    HINSTANCE res = ShellExecuteW(nullptr, L"open", targetUri.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<intptr_t>(res) > 32) {
        return true;
    }

    if (targetUri != L"ms-settings:defaultapps") {
        HINSTANCE fallbackRes = ShellExecuteW(nullptr, L"open", L"ms-settings:defaultapps", nullptr, nullptr, SW_SHOWNORMAL);
        return reinterpret_cast<intptr_t>(fallbackRes) > 32;
    }
    return false;
}

} // namespace QuickView
