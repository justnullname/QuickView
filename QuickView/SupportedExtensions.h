#pragma once
#include <string_view>
#include <array>
#include <string>

namespace QuickView {

// ============================================================================
// Extension lists — single source of truth
// ============================================================================
// Every extension appears in EXACTLY ONE segment below. The public lists
// (SUPPORTED_EXTENSIONS, RAW_EXTENSIONS) are assembled from the segments at
// compile time via consteval concatenation, so the segments can never drift
// apart. All helpers are constexpr and allocation-free (zero-cost: they
// compile down to the same comparisons the old hand-written checks did).
// Extensions are ASCII by definition, so case folding is a constexpr A-Z map.

namespace detail {

template <std::size_t... Ns>
consteval auto Concat(const std::array<std::wstring_view, Ns>&... lists) {
    std::array<std::wstring_view, (Ns + ...)> result{};
    std::size_t i = 0;
    (([&] { for (const auto& e : lists) result[i++] = e; }()), ...);
    return result;
}

// Standard raster formats
inline constexpr std::array<std::wstring_view, 11> STANDARD_EXTENSIONS = {
    L".jpg", L".jpeg", L".jpe", L".jfif", L".png", L".bmp", L".dib", L".gif",
    L".tif", L".tiff", L".ico"
};

// Web / Modern formats
inline constexpr std::array<std::wstring_view, 9> WEB_MODERN_EXTENSIONS = {
    L".webp", L".avif", L".avifs", L".heic", L".heif", L".svg", L".svgz", L".jxl", L".apng"
};

// Professional / HDR / Legacy formats
inline constexpr std::array<std::wstring_view, 21> PRO_LEGACY_EXTENSIONS = {
    L".exr", L".hdr", L".pic", L".psd", L".psb", L".tga", L".icb", L".vda", L".vst", L".pcx", L".qoi",
    L".wbmp", L".pam", L".pbm", L".pgm", L".ppm", L".pnm", L".wdp", L".hdp", L".jxr", L".hif"
};

// Camera RAW formats that QuickView browses (LibRaw-decoded, part of
// SUPPORTED_EXTENSIONS)
inline constexpr std::array<std::wstring_view, 22> CAMERA_RAW_EXTENSIONS = {
    L".arw", L".cr2", L".cr3", L".crw", L".dng", L".nef", L".orf", L".raf", L".rw2", L".srw", L".x3f",
    L".mrw", L".mos", L".kdc", L".dcr", L".sr2", L".pef", L".erf", L".3fr", L".mef", L".nrw", L".raw"
};

// Additional RAW extensions recognized for classification/decode-routing but
// not browsed in folder playlists (rare stills, digital-back and cine formats;
// reachable via Open dialog "All Files", drag-drop or the command line)
inline constexpr std::array<std::wstring_view, 21> EXTENDED_RAW_EXTENSIONS = {
    L".ari", L".bay", L".braw", L".cap", L".data", L".dcs", L".drf", L".eip", L".fff", L".gpr",
    L".iiq", L".k25", L".mdc", L".obm", L".ptx", L".pxn", L".r3d", L".rwl", L".rwz", L".srf", L".sti"
};

// Archive container formats browsed via the VFS
inline constexpr std::array<std::wstring_view, 4> ARCHIVE_EXTENSIONS_SEG = {
    L".cbz", L".zip", L".cbr", L".rar"
};

} // namespace detail

// Everything QuickView can open from a folder scan
inline constexpr auto SUPPORTED_EXTENSIONS = detail::Concat(
    detail::STANDARD_EXTENSIONS, detail::WEB_MODERN_EXTENSIONS, detail::PRO_LEGACY_EXTENSIONS,
    detail::CAMERA_RAW_EXTENSIONS, detail::ARCHIVE_EXTENSIONS_SEG);

// Every extension classified as camera RAW (decode-routing, RAW toggles, etc.)
inline constexpr auto RAW_EXTENSIONS = detail::Concat(
    detail::CAMERA_RAW_EXTENSIONS, detail::EXTENDED_RAW_EXTENSIONS);

// Archive container formats (VFS)
inline constexpr auto ARCHIVE_EXTENSIONS = detail::ARCHIVE_EXTENSIONS_SEG;

// ============================================================================
// Classification helpers (constexpr, allocation-free, case-insensitive)
// ============================================================================

constexpr wchar_t ToLowerAscii(wchar_t c) {
    return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + (L'a' - L'A')) : c;
}

// Case-insensitive compare; both sides are expected to include the dot.
constexpr bool ExtEqualsIgnoreCase(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (ToLowerAscii(a[i]) != ToLowerAscii(b[i])) return false;
    }
    return true;
}

// Extension of a plain or VFS path ("dir\a.CR3" -> ".CR3", "a.zip|0|b.png" ->
// ".png"), empty if none. A dot inside a directory name does not count.
constexpr std::wstring_view ExtensionOf(std::wstring_view path) {
    const std::size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring_view::npos) return {};
    const std::size_t sep = path.find_last_of(L"\\/|");
    if (sep != std::wstring_view::npos && sep > dot) return {};
    return path.substr(dot);
}

constexpr bool IsRawExtension(std::wstring_view ext) {
    for (const auto& raw : RAW_EXTENSIONS) {
        if (ExtEqualsIgnoreCase(ext, raw)) return true;
    }
    return false;
}

constexpr bool IsRawPath(std::wstring_view path) {
    return IsRawExtension(ExtensionOf(path));
}

// HEIF family (camera .hif plus .heic/.heif); the extensions live in their
// support segments above — this checks the family without a duplicate list.
constexpr bool IsHeifExtension(std::wstring_view ext) {
    return ExtEqualsIgnoreCase(ext, L".heic") || ExtEqualsIgnoreCase(ext, L".heif")
        || ExtEqualsIgnoreCase(ext, L".hif");
}

constexpr bool IsHeifPath(std::wstring_view path) {
    return IsHeifExtension(ExtensionOf(path));
}

constexpr bool IsArchiveExtension(std::wstring_view ext) {
    for (const auto& a : ARCHIVE_EXTENSIONS) {
        if (ExtEqualsIgnoreCase(ext, a)) return true;
    }
    return false;
}

constexpr bool IsArchivePath(std::wstring_view path) {
    return IsArchiveExtension(ExtensionOf(path));
}

// ============================================================================
// RAW+JPEG pairing policy
// ============================================================================
// The out-of-camera *rendered* stills that legitimately sit next to a RAW (the
// visible half of a RAW+JPEG pair). Deliberately a curated WHITELIST of what
// cameras actually write, NOT "everything that isn't RAW": no camera writes a
// .png/.psd/.svg beside a RAW, so a same-name file of such a type comes from
// some other software (an export, an edit, a generated file) and a RAW must
// never be hidden behind it.
// JPEG family: every camera writes .JPG; .JPEG appears via software.
// HEIF family: Canon/Nikon/Sony/Fujifilm write .HIF to the card; Apple and
// newer Panasonic use .HEIC; .HEIF covers generic/Hasselblad HEIF. Excluded on
// purpose: TIFF (no modern camera captures it -- it only appears via in-camera
// RAW development, so a same-name .tif is a RAW-derived export) and Panasonic
// HLG .HSP (not in SUPPORTED_EXTENSIONS, so QuickView cannot display it).
inline constexpr std::array<std::wstring_view, 5> RENDERED_PAIR_EXTENSIONS = {
    L".jpg", L".jpeg", L".heic", L".heif", L".hif"
};

// Can this extension be the visible (rendered) half of a RAW+JPEG pair?
constexpr bool IsRenderedPairExtension(std::wstring_view ext) {
    for (const auto& r : RENDERED_PAIR_EXTENSIONS) {
        if (ExtEqualsIgnoreCase(ext, r)) return true;
    }
    return false;
}

// Compile-time regression tests (zero runtime cost)
static_assert(SUPPORTED_EXTENSIONS.size() == 67);
static_assert(RAW_EXTENSIONS.size() == 43);
static_assert(IsRawExtension(L".crw"));   // was missing from main.cpp's IsRawFile
static_assert(IsRawExtension(L".CR3"));   // case-insensitive
static_assert(!IsRawExtension(L".jpg"));
static_assert(!IsRawExtension(L""));
static_assert(IsRawPath(L"C:\\Photos\\IMG_0001.NEF"));
static_assert(!IsRawPath(L"C:\\dir.raw\\readme"));
static_assert(ExtensionOf(L"C:\\a\\manga.cbz|12|page01.png") == L".png");
static_assert(IsHeifExtension(L".HIF"));
static_assert(IsArchivePath(L"C:\\comics\\manga.CBZ"));
static_assert(IsRenderedPairExtension(L".JPG"));
static_assert(!IsRenderedPairExtension(L".tif"));  // TIFF is a RAW-derived export
static_assert(!IsRenderedPairExtension(L".png")); // not camera-written beside a RAW

// Generate the COMDLG filter string, e.g., "All Images\0*.jpg;*.png...\0All Files\0*.*\0\0"
inline std::wstring GetSupportedExtensionsFilter() {
    std::wstring filter;
    filter.append(L"All Images");
    filter.push_back(L'\0');

    for (size_t i = 0; i < std::size(SUPPORTED_EXTENSIONS); ++i) {
        filter += L"*";
        filter += SUPPORTED_EXTENSIONS[i];
        if (i < std::size(SUPPORTED_EXTENSIONS) - 1) {
            filter += L";";
        }
    }

    filter.push_back(L'\0');
    filter.append(L"All Files");
    filter.push_back(L'\0');
    filter.append(L"*.*");
    filter.push_back(L'\0');
    filter.push_back(L'\0');

    return filter;
}

}
