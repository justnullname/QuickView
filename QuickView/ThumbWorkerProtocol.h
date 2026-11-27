#pragma once
// ============================================================================
// ThumbWorkerProtocol.h - Shared POD Protocol & Helpers for --thumb-worker
// ============================================================================
// Shared between QuickView.exe (--thumb-worker / ThumbnailManager) and the thin
// COM shell extension (plugins/shell/qv_shell_thumb). Zero STL dependencies so
// the shell DLL stays compact with pure Win32 + static CRT.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <thumbcache.h>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace QuickView::ThumbIpc {

inline constexpr uint32_t kThumbPipeMagic          = 0x50545651u; // "QVTP"
inline constexpr uint32_t kThumbPipeVersion        = 1u;
inline constexpr uint32_t kThumbPipeMaxEdge        = 1024u;
inline constexpr uint32_t kThumbPipeMaxPixelsBytes  = 1024u * 1024u * 4u; // 4 MiB (excluding response header)
inline constexpr uint32_t kThumbPipeMaxStreamBytes  = 64u * 1024u * 1024u; // 64 MiB hard cap (isolated stream mode only)
inline constexpr uint32_t kThumbPipeTimeoutMs       = 4500u;
inline constexpr uint32_t kThumbPipeSvgTimeoutMs    = 7000u;
inline constexpr uint32_t kThumbWorkerDecodeCapMs   = 4000u;
inline constexpr uint32_t kThumbWorkerSvgDecodeCapMs = 6500u;
inline constexpr uint32_t kThumbWorkerTitanWaitMs   = 1500u;
inline constexpr uint32_t kThumbWorkerIdleExitMs    = 45000u;

inline constexpr uint32_t kThumbReqFlagStreamMode  = 1u << 0;

// Guard against triggering OneDrive / iCloud Files On-Demand cloud downloads.
inline constexpr DWORD kCloudPlaceholderAttrMask =
    FILE_ATTRIBUTE_OFFLINE |
    0x00400000u /* FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS */ |
    0x00040000u /* FILE_ATTRIBUTE_RECALL_ON_OPEN */;

#pragma pack(push, 1)
struct ThumbPipeRequestHeader {
    uint32_t magic;            // kThumbPipeMagic
    uint32_t version;          // kThumbPipeVersion
    uint32_t flags;            // kThumbReqFlagStreamMode etc.
    uint32_t targetSize;       // Requested max edge in pixels (clamped to [16, kThumbPipeMaxEdge])
    uint64_t maxFileBytes;     // File size threshold enforced by worker
    uint32_t pathChars;        // Length in wchar_t of path or stream extension hint (NOT incl. NUL)
    uint32_t streamPayloadBytes; // Raw image bytes appended right after pathChars (StreamMode only)
};

struct ThumbPipeResponseHeader {
    uint32_t magic;            // kThumbPipeMagic
    uint32_t version;          // kThumbPipeVersion
    int32_t  hr;               // HRESULT (S_OK on success)
    uint32_t width;            // Output width
    uint32_t height;           // Output height
    uint32_t stride;           // Output bytes per row (>= width * 4)
    uint8_t  isPremultiplied;  // 1 if pixel RGB values are premultiplied by alpha
    uint8_t  reserved[3];
    uint32_t pixelBytes;       // Payload byte length following this header
};
#pragma pack(pop)

static_assert(std::is_trivially_copyable_v<ThumbPipeRequestHeader>, "ThumbPipeRequestHeader must be POD");
static_assert(std::is_trivially_copyable_v<ThumbPipeResponseHeader>, "ThumbPipeResponseHeader must be POD");

[[nodiscard]] inline bool IsCloudPlaceholderAttributes(DWORD attrs) noexcept {
    return (attrs == INVALID_FILE_ATTRIBUTES) || ((attrs & kCloudPlaceholderAttrMask) != 0);
}

[[nodiscard]] inline uint64_t HashUserNameFnv1a64() noexcept {
    HANDLE hToken = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        BYTE buf[256]{};
        DWORD cb = 0;
        if (GetTokenInformation(hToken, TokenUser, buf, sizeof(buf), &cb)) {
            const auto* pUser = reinterpret_cast<const TOKEN_USER*>(buf);
            if (pUser && pUser->User.Sid && IsValidSid(pUser->User.Sid)) {
                const DWORD sidLen = GetLengthSid(pUser->User.Sid);
                const auto* bytes = static_cast<const uint8_t*>(pUser->User.Sid);
                uint64_t h = 14695981039346656037ull;
                for (DWORD i = 0; i < sidLen; ++i) {
                    h ^= static_cast<uint64_t>(bytes[i]);
                    h *= 1099511628211ull;
                }
                CloseHandle(hToken);
                return h;
            }
        }
        CloseHandle(hToken);
    }

    wchar_t userBuf[256]{};
    DWORD len = static_cast<DWORD>(_countof(userBuf));
    if (!GetUserNameW(userBuf, &len) || len == 0) {
        const wchar_t def[] = L"default";
        uint64_t h = 14695981039346656037ull;
        for (size_t i = 0; def[i] != L'\0'; ++i) {
            h ^= static_cast<uint64_t>(def[i]);
            h *= 1099511628211ull;
        }
        return h;
    }
    uint64_t h = 14695981039346656037ull;
    for (DWORD i = 0; i < len && userBuf[i] != L'\0'; ++i) {
        wchar_t c = userBuf[i];
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c + (L'a' - L'A'));
        h ^= static_cast<uint64_t>(c);
        h *= 1099511628211ull;
    }
    return h;
}

inline void FormatUint64Hex(uint64_t val, wchar_t* out, size_t cap) noexcept {
    static constexpr wchar_t kHex[] = L"0123456789abcdef";
    if (!out || cap < 17) return;
    for (int i = 15; i >= 0; --i) {
        out[i] = kHex[val & 0xFu];
        val >>= 4u;
    }
    out[16] = L'\0';
}

inline void FormatUint32Dec(uint32_t val, wchar_t* out, size_t cap) noexcept {
    if (!out || cap < 2) return;
    wchar_t rev[16]{};
    size_t n = 0;
    do {
        rev[n++] = static_cast<wchar_t>(L'0' + (val % 10u));
        val /= 10u;
    } while (val > 0 && n < 15);
    size_t outIdx = 0;
    while (n > 0 && outIdx + 1 < cap) {
        out[outIdx++] = rev[--n];
    }
    out[outIdx] = L'\0';
}

inline bool BuildSessionScopedName(
    wchar_t* outBuf,
    size_t cap,
    const wchar_t* prefix) noexcept {
    if (!outBuf || cap < 64 || !prefix) return false;
    DWORD sessionId = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &sessionId)) {
        sessionId = 0;
    }
    wchar_t sidStr[16]{};
    FormatUint32Dec(sessionId, sidStr, _countof(sidStr));
    wchar_t hashStr[20]{};
    FormatUint64Hex(HashUserNameFnv1a64(), hashStr, _countof(hashStr));

    size_t idx = 0;
    auto append = [&](const wchar_t* s) -> bool {
        while (*s) {
            if (idx + 1 >= cap) return false;
            outBuf[idx++] = *s++;
        }
        return true;
    };
    if (!append(prefix) || !append(L"_S") || !append(sidStr) || !append(L"_") || !append(hashStr)) {
        outBuf[0] = L'\0';
        return false;
    }
    outBuf[idx] = L'\0';
    return true;
}

inline bool BuildSessionScopedPipeName(wchar_t* outBuf, size_t cap) noexcept {
    return BuildSessionScopedName(outBuf, cap, L"\\\\.\\pipe\\QuickView_ThumbWorker");
}

inline bool BuildSessionScopedLaunchMutexName(wchar_t* outBuf, size_t cap) noexcept {
    return BuildSessionScopedName(outBuf, cap, L"Local\\QuickView_ThumbWorkerLaunch");
}

namespace detail {
struct AlphaRecipTable {
    uint64_t m[256]{};
    consteval AlphaRecipTable() noexcept {
        m[0] = 0;
        for (uint64_t a = 1; a < 256; ++a) {
            m[a] = ((1ULL << 32) + a - 1ULL) / a;
        }
    }
};
inline constexpr AlphaRecipTable kAlphaRecip{};
} // namespace detail

// Convert BGRA8888 pixels into a top-down 32-bit DIB HBITMAP and report exact WTS_ALPHATYPE.
// Uses WTSAT_RGB when every pixel has A == 0xFF (fastest path for Explorer),
// or accurately un-premultiplies to straight ARGB with WTSAT_ARGB to prevent black halos.
inline HRESULT ConvertBgraToShellDibAndAlpha(
    const uint8_t* srcPixels,
    uint32_t width,
    uint32_t height,
    uint32_t srcStride,
    bool isPremultiplied,
    HBITMAP* outBitmap,
    WTS_ALPHATYPE* outAlphaType) noexcept {
    if (!srcPixels || width == 0 || height == 0 || width > 16384u || height > 16384u ||
        static_cast<uint64_t>(srcStride) < static_cast<uint64_t>(width) * 4ull ||
        !outBitmap || !outAlphaType) {
        return E_INVALIDARG;
    }
    *outBitmap = nullptr;
    *outAlphaType = WTSAT_UNKNOWN;

    bool hasTransparency = false;
    for (uint32_t y = 0; y < height && !hasTransparency; ++y) {
        const uint8_t* row = srcPixels + static_cast<size_t>(y) * srcStride;
        for (uint32_t x = 0; x < width; ++x) {
            if (row[x * 4u + 3u] != 0xFFu) {
                hasTransparency = true;
                break;
            }
        }
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = static_cast<LONG>(width);
    bmi.bmiHeader.biHeight      = -static_cast<LONG>(height); // Top-down DIB
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* dibBits = nullptr;
    HBITMAP hBmp = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &dibBits, nullptr, 0);
    if (!hBmp || !dibBits) {
        if (hBmp) DeleteObject(hBmp);
        return E_OUTOFMEMORY;
    }

    uint8_t* dst = static_cast<uint8_t*>(dibBits);
    const uint32_t dstStride = width * 4u;

    if (!hasTransparency || !isPremultiplied) {
        for (uint32_t y = 0; y < height; ++y) {
            std::memcpy(
                dst + static_cast<size_t>(y) * dstStride,
                srcPixels + static_cast<size_t>(y) * srcStride,
                dstStride);
        }
        *outAlphaType = hasTransparency ? WTSAT_ARGB : WTSAT_RGB;
    } else {
        for (uint32_t y = 0; y < height; ++y) {
            const uint8_t* sRow = srcPixels + static_cast<size_t>(y) * srcStride;
            uint8_t* dRow = dst + static_cast<size_t>(y) * dstStride;
            for (uint32_t x = 0; x < width; ++x) {
                const uint32_t b = sRow[x * 4u + 0u];
                const uint32_t g = sRow[x * 4u + 1u];
                const uint32_t r = sRow[x * 4u + 2u];
                const uint32_t a = sRow[x * 4u + 3u];
                if (a == 0u) {
                    dRow[x * 4u + 0u] = 0;
                    dRow[x * 4u + 1u] = 0;
                    dRow[x * 4u + 2u] = 0;
                    dRow[x * 4u + 3u] = 0;
                } else if (a == 255u) {
                    dRow[x * 4u + 0u] = static_cast<uint8_t>(b);
                    dRow[x * 4u + 1u] = static_cast<uint8_t>(g);
                    dRow[x * 4u + 2u] = static_cast<uint8_t>(r);
                    dRow[x * 4u + 3u] = 255u;
                } else {
                    const uint32_t half = a >> 1u;
                    const uint64_t recip = detail::kAlphaRecip.m[a];
                    const uint32_t ub = static_cast<uint32_t>(((b * 255ull + half) * recip) >> 32u);
                    const uint32_t ug = static_cast<uint32_t>(((g * 255ull + half) * recip) >> 32u);
                    const uint32_t ur = static_cast<uint32_t>(((r * 255ull + half) * recip) >> 32u);
                    dRow[x * 4u + 0u] = static_cast<uint8_t>(ub > 255u ? 255u : ub);
                    dRow[x * 4u + 1u] = static_cast<uint8_t>(ug > 255u ? 255u : ug);
                    dRow[x * 4u + 2u] = static_cast<uint8_t>(ur > 255u ? 255u : ur);
                    dRow[x * 4u + 3u] = static_cast<uint8_t>(a);
                }
            }
        }
        *outAlphaType = WTSAT_ARGB;
    }

    *outBitmap = hBmp;
    return S_OK;
}

} // namespace QuickView::ThumbIpc
