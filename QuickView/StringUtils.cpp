// StringUtils.cpp - Shared string utilities implementation
#include "pch.h"
#include "StringUtils.h"
#include <windows.h>
#include <algorithm>

namespace QuickView {

size_t Utf8ToWide(std::string_view src, std::span<wchar_t> dst) {
    if (dst.empty()) return 0;
    if (src.empty()) {
        dst[0] = L'\0';
        return 0;
    }
    int maxChars = static_cast<int>(dst.size() - 1);
    if (maxChars <= 0) {
        dst[0] = L'\0';
        return 0;
    }
    int written = MultiByteToWideChar(
        CP_UTF8, 0,
        src.data(), static_cast<int>(src.size()),
        dst.data(), maxChars
    );
    if (written > 0 && static_cast<size_t>(written) < dst.size()) {
        dst[static_cast<size_t>(written)] = L'\0';
        return static_cast<size_t>(written);
    }
    dst[0] = L'\0';
    return 0;
}

size_t WideToUtf8(std::wstring_view src, std::span<char> dst) {
    if (dst.empty()) return 0;
    if (src.empty()) {
        dst[0] = '\0';
        return 0;
    }
    int maxChars = static_cast<int>(dst.size() - 1);
    if (maxChars <= 0) {
        dst[0] = '\0';
        return 0;
    }
    int written = WideCharToMultiByte(
        CP_UTF8, 0,
        src.data(), static_cast<int>(src.size()),
        dst.data(), maxChars,
        nullptr, nullptr
    );
    if (written > 0 && static_cast<size_t>(written) < dst.size()) {
        dst[static_cast<size_t>(written)] = '\0';
        return static_cast<size_t>(written);
    }
    dst[0] = '\0';
    return 0;
}

std::wstring Utf8ToWide(std::string_view utf8Str) {
    if (utf8Str.empty()) return L"";
    int req = MultiByteToWideChar(CP_UTF8, 0, utf8Str.data(), static_cast<int>(utf8Str.size()), nullptr, 0);
    if (req <= 0) return L"";
    std::wstring result(static_cast<size_t>(req), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8Str.data(), static_cast<int>(utf8Str.size()), result.data(), req);
    return result;
}

std::string WideToUtf8(std::wstring_view wideStr) {
    if (wideStr.empty()) return "";
    int req = WideCharToMultiByte(CP_UTF8, 0, wideStr.data(), static_cast<int>(wideStr.size()), nullptr, 0, nullptr, nullptr);
    if (req <= 0) return "";
    std::string result(static_cast<size_t>(req), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wideStr.data(), static_cast<int>(wideStr.size()), result.data(), req, nullptr, nullptr);
    return result;
}

std::vector<std::wstring> SplitAndTrimCSV(const std::wstring& str, wchar_t delim) {
    std::vector<std::wstring> tokens;
    size_t start = 0;
    while (true) {
        size_t pos = str.find(delim, start);
        std::wstring token = (pos == std::wstring::npos)
            ? str.substr(start)
            : str.substr(start, pos - start);

        // Trim leading/trailing whitespace
        size_t begin = token.find_first_not_of(L" \t");
        size_t end   = token.find_last_not_of(L" \t");
        if (begin != std::wstring::npos) {
            tokens.push_back(token.substr(begin, end - begin + 1));
        }

        if (pos == std::wstring::npos) break;
        start = pos + 1;
    }
    return tokens;
}

template <typename AllowedRange>
static std::wstring NormalizeCSVImpl(const std::wstring& csv,
                                     const AllowedRange& allowedKeys,
                                     int maxItems) {
    auto tokens = SplitAndTrimCSV(csv);
    std::vector<std::wstring> result;
    for (const auto& t : tokens) {
        if (static_cast<int>(result.size()) >= maxItems) break;
        bool allowed = false;
        for (const auto& k : allowedKeys) {
            if (k == t) {
                allowed = true;
                break;
            }
        }
        if (!allowed) continue;
        bool dup = std::find(result.begin(), result.end(), t) != result.end();
        if (dup) continue;
        result.push_back(t);
    }
    std::wstring out;
    for (size_t i = 0; i < result.size(); ++i) {
        if (i > 0) out += L",";
        out += result[i];
    }
    return out;
}

std::wstring NormalizeCSV(const std::wstring& csv,
                         std::span<const std::wstring_view> allowedKeys,
                         int maxItems) {
    return NormalizeCSVImpl(csv, allowedKeys, maxItems);
}

std::wstring NormalizeCSV(const std::wstring& csv,
                         const std::vector<std::wstring>& allowedKeys,
                         int maxItems) {
    return NormalizeCSVImpl(csv, allowedKeys, maxItems);
}

bool PathExists(const wchar_t* path) noexcept {
    if (!path || !*path) return false;
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

bool IsExistingDirectory(const wchar_t* path) noexcept {
    if (!path || !*path) return false;
    const DWORD attrs = GetFileAttributesW(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool IsExistingRegularFile(const wchar_t* path) noexcept {
    if (!path || !*path) return false;
    const DWORD attrs = GetFileAttributesW(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

uint64_t QueryFileSize(const wchar_t* path, bool* outOk) noexcept {
    if (outOk) *outOk = false;
    if (!path || !*path) return 0;
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) return 0;
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return 0;
    if (outOk) *outOk = true;
    return (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | static_cast<uint64_t>(fad.nFileSizeLow);
}

uint64_t QueryFileLastWriteTicks(const wchar_t* path, bool* outOk) noexcept {
    if (outOk) *outOk = false;
    if (!path || !*path) return 0;
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) return 0;
    if (outOk) *outOk = true;
    return FileTimePartsToTicks(fad.ftLastWriteTime.dwLowDateTime, fad.ftLastWriteTime.dwHighDateTime);
}

} // namespace QuickView
