// StringUtils.h - Shared string utilities for QuickView
// Lightweight helpers that avoid <iostream>, <sstream>, <regex>, <locale>.
#pragma once
#include <string>
#include <vector>
#include <algorithm>

namespace QuickView {

// Split a wide string by delimiter, skipping empty tokens.
// Trims leading/trailing whitespace from each token.
inline std::vector<std::wstring> SplitAndTrimCSV(const std::wstring& str, wchar_t delim = L',') {
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

// Normalize a CSV string: split, trim, deduplicate, filter against allowed keys, truncate to maxItems.
// Returns the cleaned CSV string.
inline std::wstring NormalizeCSV(const std::wstring& csv,
                                 const std::vector<std::wstring>& allowedKeys,
                                 int maxItems) {
    auto tokens = SplitAndTrimCSV(csv);
    std::vector<std::wstring> result;
    for (const auto& t : tokens) {
        if ((int)result.size() >= maxItems) break;
        // Check allowed
        bool allowed = std::find(allowedKeys.begin(), allowedKeys.end(), t) != allowedKeys.end();
        if (!allowed) continue;
        // Check duplicate
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

} // namespace QuickView
