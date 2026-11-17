// StringUtils.h - Shared string utilities for QuickView
// Lightweight helpers that avoid <iostream>, <sstream>, <regex>, <locale>.
#pragma once
#include <string>
#include <vector>
#include <string_view>
#include <span>

namespace QuickView {

// High performance zero-heap UTF conversion into user-supplied buffer.
// Returns the number of characters written to dst (excluding null terminator, or 0 on error/truncation).
size_t Utf8ToWide(std::string_view src, std::span<wchar_t> dst);
size_t WideToUtf8(std::wstring_view src, std::span<char> dst);

// Heap allocating convenience wrappers (when std::(w)string return is needed).
std::wstring Utf8ToWide(std::string_view utf8Str);
std::string WideToUtf8(std::wstring_view wideStr);

// Split a wide string by delimiter, skipping empty tokens.
// Trims leading/trailing whitespace from each token.
std::vector<std::wstring> SplitAndTrimCSV(const std::wstring& str, wchar_t delim = L',');

// Normalize a CSV string: split, trim, deduplicate, filter against allowed keys, truncate to maxItems.
// Returns the cleaned CSV string.
std::wstring NormalizeCSV(const std::wstring& csv,
                         std::span<const std::wstring_view> allowedKeys,
                         int maxItems);
std::wstring NormalizeCSV(const std::wstring& csv,
                         const std::vector<std::wstring>& allowedKeys,
                         int maxItems);

// Lightweight zero-allocation path view helpers (Windows backslash/slash aware)
inline constexpr std::wstring_view ParentPathView(std::wstring_view path) noexcept {
    while (path.size() > 1 && (path.back() == L'\\' || path.back() == L'/')) {
        if (path.size() == 3 && path[1] == L':') break;
        path.remove_suffix(1);
    }
    const size_t sep = path.find_last_of(L"\\/");
    if (sep == std::wstring_view::npos) return {};
    if (sep == 0) return path.substr(0, 1);
    if (sep == 2 && path[1] == L':') return path.substr(0, 3);
    return path.substr(0, sep);
}

inline constexpr std::wstring_view FileNameView(std::wstring_view path) noexcept {
    const size_t sep = path.find_last_of(L"\\/");
    return (sep == std::wstring_view::npos) ? path : path.substr(sep + 1);
}

inline constexpr std::wstring_view StemView(std::wstring_view path) noexcept {
    const std::wstring_view name = FileNameView(path);
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring_view::npos || dot == 0) return name;
    return name.substr(0, dot);
}

// Returns path without its file extension (preserving directory prefix), e.g. "C:\a\IMG_01.CR3" -> "C:\a\IMG_01".
inline constexpr std::wstring_view PathWithoutExtensionView(std::wstring_view path) noexcept {
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring_view::npos) return path;
    const size_t sep = path.find_last_of(L"\\/|");
    if (sep != std::wstring_view::npos && (dot <= sep + 1)) return path;
    return path.substr(0, dot);
}

inline std::wstring JoinPath(std::wstring_view dir, std::wstring_view name) {
    if (dir.empty()) return std::wstring(name);
    std::wstring out;
    const bool hasSep = (dir.back() == L'\\' || dir.back() == L'/');
    out.reserve(dir.size() + (hasSep ? 0u : 1u) + name.size());
    out.append(dir);
    if (!hasSep) out.push_back(L'\\');
    out.append(name);
    return out;
}

inline constexpr uint64_t FileTimePartsToTicks(uint32_t lowDateTime, uint32_t highDateTime) noexcept {
    return (static_cast<uint64_t>(highDateTime) << 32) | static_cast<uint64_t>(lowDateTime);
}

// Win32 native filesystem metadata probes (replaces <filesystem> without CRT/exception bloat)
bool PathExists(const wchar_t* path) noexcept;
bool IsExistingDirectory(const wchar_t* path) noexcept;
bool IsExistingRegularFile(const wchar_t* path) noexcept;
uint64_t QueryFileSize(const wchar_t* path, bool* outOk = nullptr) noexcept;
uint64_t QueryFileLastWriteTicks(const wchar_t* path, bool* outOk = nullptr) noexcept;

} // namespace QuickView
