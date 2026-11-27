#include "pch.h"
#include "ThumbWorker.h"
#include "StringUtils.h"
#include "SupportedExtensions.h"
#include <sddl.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <semaphore>
#include <thread>
#include <unordered_map>

namespace QuickView {

namespace {

[[nodiscard]] uint64_t Fnv1a64(std::string_view sv) noexcept {
    uint64_t h = 14695981039346656037ull;
    for (unsigned char c : sv) {
        h ^= static_cast<uint64_t>(c);
        h *= 1099511628211ull;
    }
    return h;
}

struct ScopedSecurityDescriptor {
    PSECURITY_DESCRIPTOR sd = nullptr;
    SECURITY_ATTRIBUTES sa{};
    bool valid = false;

    ScopedSecurityDescriptor() noexcept {
        HANDLE hToken = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) return;

        BYTE tokBuf[256]{};
        DWORD cb = 0;
        LPWSTR userSidStr = nullptr;
        if (GetTokenInformation(hToken, TokenUser, tokBuf, sizeof(tokBuf), &cb)) {
            const auto* pUser = reinterpret_cast<const TOKEN_USER*>(tokBuf);
            if (pUser && pUser->User.Sid && IsValidSid(pUser->User.Sid)) {
                ConvertSidToStringSidW(pUser->User.Sid, &userSidStr);
            }
        }
        CloseHandle(hToken);

        std::wstring sddl = L"D:(A;;GA;;;SY)";
        if (userSidStr) {
            sddl += L"(A;;GA;;;";
            sddl += userSidStr;
            sddl += L")";
            LocalFree(userSidStr);
        } else {
            sddl += L"(A;;GA;;;OW)";
        }
        // Allow Low-IL thumbnail host processes owned by the same user to connect,
        // while keeping DACL restricted to SYSTEM + current user SID.
        sddl += L"S:(ML;;NW;;;LW)";

        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) {
            sa.nLength = sizeof(sa);
            sa.lpSecurityDescriptor = sd;
            sa.bInheritHandle = FALSE;
            valid = true;
        }
    }

    ~ScopedSecurityDescriptor() {
        if (sd) {
            LocalFree(sd);
            sd = nullptr;
        }
    }
};

bool ReadExactFromPipe(HANDLE hPipe, HANDLE hIoEvent, void* buf, DWORD totalBytes, DWORD deadlineTick) noexcept {
    uint8_t* dst = static_cast<uint8_t*>(buf);
    DWORD remaining = totalBytes;
    while (remaining > 0) {
        const DWORD now = GetTickCount();
        if (static_cast<int32_t>(deadlineTick - now) <= 0) return false;
        const DWORD waitMs = deadlineTick - now;

        ResetEvent(hIoEvent);
        OVERLAPPED ov{};
        ov.hEvent = hIoEvent;

        DWORD readBytes = 0;
        const BOOL ok = ReadFile(hPipe, dst, remaining, &readBytes, &ov);
        if (!ok) {
            const DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING) return false;
            const DWORD wr = WaitForSingleObject(hIoEvent, waitMs);
            if (wr != WAIT_OBJECT_0) {
                CancelIoEx(hPipe, &ov);
                GetOverlappedResult(hPipe, &ov, &readBytes, TRUE);
                return false;
            }
            if (!GetOverlappedResult(hPipe, &ov, &readBytes, FALSE)) return false;
        }
        if (readBytes == 0) return false;
        dst += readBytes;
        remaining -= readBytes;
    }
    return true;
}

bool WriteExactToPipe(HANDLE hPipe, HANDLE hIoEvent, const void* buf, DWORD totalBytes, DWORD deadlineTick) noexcept {
    const uint8_t* src = static_cast<const uint8_t*>(buf);
    DWORD remaining = totalBytes;
    while (remaining > 0) {
        const DWORD now = GetTickCount();
        if (static_cast<int32_t>(deadlineTick - now) <= 0) return false;
        const DWORD waitMs = deadlineTick - now;

        ResetEvent(hIoEvent);
        OVERLAPPED ov{};
        ov.hEvent = hIoEvent;

        DWORD written = 0;
        const BOOL ok = WriteFile(hPipe, src, remaining, &written, &ov);
        if (!ok) {
            const DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING) return false;
            const DWORD wr = WaitForSingleObject(hIoEvent, waitMs);
            if (wr != WAIT_OBJECT_0) {
                CancelIoEx(hPipe, &ov);
                GetOverlappedResult(hPipe, &ov, &written, TRUE);
                return false;
            }
            if (!GetOverlappedResult(hPipe, &ov, &written, FALSE)) return false;
        }
        if (written == 0) return false;
        src += written;
        remaining -= written;
    }
    return true;
}

// Wait briefly for the client to finish draining the response and close its pipe handle
// (or timeout without blocking the worker pool indefinitely like FlushFileBuffers).
void WaitBrieflyForClientDrain(HANDLE hPipe, HANDLE hIoEvent, DWORD timeoutMs) noexcept {
    uint8_t dummyByte = 0;
    ResetEvent(hIoEvent);
    OVERLAPPED ov{};
    ov.hEvent = hIoEvent;
    DWORD got = 0;
    if (!ReadFile(hPipe, &dummyByte, 1, &got, &ov)) {
        if (GetLastError() == ERROR_IO_PENDING) {
            const DWORD wr = WaitForSingleObject(hIoEvent, timeoutMs);
            if (wr != WAIT_OBJECT_0) {
                CancelIoEx(hPipe, &ov);
            }
            GetOverlappedResult(hPipe, &ov, &got, TRUE);
        }
    }
}

bool TryReadWorkerArgValue(int argc, LPWSTR* argv, LPCWSTR flag, std::wstring* out) noexcept {
    if (!argv || !flag || !out) return false;
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] && _wcsicmp(argv[i], flag) == 0 && argv[i + 1] && argv[i + 1][0] != L'\0') {
            *out = argv[i + 1];
            return true;
        }
    }
    return false;
}

} // namespace

bool IsHeavyThumbnailExtension(std::wstring_view pathOrExt) noexcept {
    std::wstring_view ext = (pathOrExt.starts_with(L'.') && pathOrExt.find_first_of(L"\\/|") == std::wstring_view::npos)
        ? pathOrExt
        : ExtensionOf(pathOrExt);
    // Multi-page virtual paths (e.g. "book.pdf|3|Page 4") store the format extension on the host path.
    if (ext.empty()) {
        const size_t pipe = pathOrExt.find(L'|');
        if (pipe != std::wstring_view::npos && pipe > 0) {
            ext = ExtensionOf(pathOrExt.substr(0, pipe));
        }
    }
    if (ext.empty()) return false;
    if (IsRawExtension(ext) || IsArchiveExtension(ext) || IsDocumentExtension(ext)) return true;
    return ExtEqualsIgnoreCase(ext, L".psd") ||
           ExtEqualsIgnoreCase(ext, L".psb") ||
           ExtEqualsIgnoreCase(ext, L".exr") ||
           ExtEqualsIgnoreCase(ext, L".hdr") ||
           ExtEqualsIgnoreCase(ext, L".tif") ||
           ExtEqualsIgnoreCase(ext, L".tiff");
}

bool CanonicalizeThumbCacheKey(
    std::wstring_view filePath,
    std::string& outCanonicalPathUtf8,
    uint64_t& outFileSize,
    uint64_t& outFileMtime100ns) {
    outCanonicalPathUtf8.clear();
    outFileSize = 0;
    outFileMtime100ns = 0;

    if (filePath.empty() || filePath.find(L'|') != std::wstring_view::npos) return false;
    if (filePath.starts_with(L"\\\\")) return false; // Skip UNC paths

    std::wstring pathStr(filePath);
    wchar_t fullBuf[MAX_PATH * 4]{};
    const DWORD fullLen = GetFullPathNameW(pathStr.c_str(), static_cast<DWORD>(_countof(fullBuf)), fullBuf, nullptr);
    if (fullLen == 0 || fullLen >= _countof(fullBuf)) return false;

    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(fullBuf, GetFileExInfoStandard, &fad)) return false;
    if ((fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return false;
    if (ThumbIpc::IsCloudPlaceholderAttributes(fad.dwFileAttributes)) return false;

    std::wstring norm(fullBuf, fullLen);
    for (wchar_t& ch : norm) {
        if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch + (L'a' - L'A'));
    }

    outCanonicalPathUtf8 = WideToUtf8(norm);
    outFileSize = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    outFileMtime100ns = (static_cast<uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
                        fad.ftLastWriteTime.dwLowDateTime;
    return !outCanonicalPathUtf8.empty();
}

void ThumbHotCache::EvictIfNeededLocked() {
    while (m_totalBytes > kMaxBytes) {
        size_t victimIdx = kMaxEntries;
        uint64_t oldestSeq = UINT64_MAX;
        for (size_t i = 0; i < kMaxEntries; ++i) {
            if (m_slots[i].occupied && m_slots[i].lruSeq < oldestSeq) {
                oldestSeq = m_slots[i].lruSeq;
                victimIdx = i;
            }
        }
        if (victimIdx == kMaxEntries) break;
        const size_t sz = m_slots[victimIdx].pixels.size();
        m_totalBytes = (m_totalBytes >= sz) ? (m_totalBytes - sz) : 0;
        m_slots[victimIdx] = Slot{};
    }
}

void ThumbHotCache::Insert(
    std::string canonicalPathUtf8,
    uint64_t fileSize,
    uint64_t fileMtime100ns,
    uint32_t width,
    uint32_t height,
    uint32_t stride,
    bool isPremultiplied,
    std::vector<uint8_t> pixels,
    uint32_t origMaxEdge) {
    if (canonicalPathUtf8.empty() || width == 0 || height == 0 || pixels.empty()) return;
    if (width > ThumbIpc::kThumbPipeMaxEdge || height > ThumbIpc::kThumbPipeMaxEdge) return;
    if (static_cast<uint64_t>(stride) < static_cast<uint64_t>(width) * 4ull) return;
    const uint64_t needed = static_cast<uint64_t>(stride) * static_cast<uint64_t>(height);
    if (pixels.size() < needed || pixels.size() > kMaxBytes / 2) return;

    const uint64_t h = Fnv1a64(canonicalPathUtf8);
    std::lock_guard<std::mutex> lock(m_mutex);
    size_t targetIdx = kMaxEntries;
    size_t emptyIdx = kMaxEntries;
    size_t oldestIdx = kMaxEntries;
    uint64_t oldestSeq = UINT64_MAX;

    for (size_t i = 0; i < kMaxEntries; ++i) {
        const Slot& s = m_slots[i];
        if (!s.occupied) {
            if (emptyIdx == kMaxEntries) emptyIdx = i;
            continue;
        }
        if (s.pathHash == h && s.canonicalPathUtf8 == canonicalPathUtf8) {
            targetIdx = i;
            break;
        }
        if (s.lruSeq < oldestSeq) {
            oldestSeq = s.lruSeq;
            oldestIdx = i;
        }
    }

    if (targetIdx == kMaxEntries) {
        targetIdx = (emptyIdx != kMaxEntries) ? emptyIdx : oldestIdx;
    }
    if (targetIdx == kMaxEntries) return;

    Slot& slot = m_slots[targetIdx];
    if (slot.occupied) {
        const size_t oldSz = slot.pixels.size();
        m_totalBytes = (m_totalBytes >= oldSz) ? (m_totalBytes - oldSz) : 0;
    }
    slot.occupied = true;
    slot.pathHash = h;
    slot.lruSeq = ++m_lruCounter;
    slot.canonicalPathUtf8 = std::move(canonicalPathUtf8);
    slot.fileSize = fileSize;
    slot.fileMtime100ns = fileMtime100ns;
    slot.width = width;
    slot.height = height;
    slot.stride = stride;
    slot.origMaxEdge = origMaxEdge;
    slot.isPremultiplied = isPremultiplied;
    slot.pixels = std::move(pixels);
    m_totalBytes += slot.pixels.size();

    EvictIfNeededLocked();
}

bool ThumbHotCache::Lookup(
    const char* canonicalPathUtf8,
    uint64_t fileSize,
    uint64_t fileMtime100ns,
    uint32_t requestedCx,
    uint8_t* outBgraBuf,
    uint32_t maxOutBytes,
    uint32_t* outWidth,
    uint32_t* outHeight,
    uint32_t* outStride,
    uint8_t* outIsPremultiplied) {
    if (!canonicalPathUtf8 || !canonicalPathUtf8[0] || !outBgraBuf ||
        !outWidth || !outHeight || !outStride || !outIsPremultiplied) {
        return false;
    }

    const std::string_view keySv(canonicalPathUtf8);
    const uint64_t h = Fnv1a64(keySv);

    std::lock_guard<std::mutex> lock(m_mutex);
    for (size_t i = 0; i < kMaxEntries; ++i) {
        Slot& s = m_slots[i];
        if (!s.occupied || s.pathHash != h || s.canonicalPathUtf8 != keySv) continue;
        if (s.fileSize != fileSize || s.fileMtime100ns != fileMtime100ns) return false;

        // Anti-resolution-inversion guard: never satisfy a larger Shell request with a smaller thumbnail,
        // unless the original image itself is smaller than requestedCx.
        const uint32_t cachedMaxEdge = (std::max)(s.width, s.height);
        const uint32_t effectiveReq =
            (s.origMaxEdge > 0 && s.origMaxEdge < requestedCx) ? s.origMaxEdge : requestedCx;
        if (cachedMaxEdge < effectiveReq) return false;
        if (s.pixels.size() > maxOutBytes) return false;

        std::memcpy(outBgraBuf, s.pixels.data(), s.pixels.size());
        *outWidth = s.width;
        *outHeight = s.height;
        *outStride = s.stride;
        *outIsPremultiplied = s.isPremultiplied ? 1u : 0u;
        s.lruSeq = ++m_lruCounter;
        return true;
    }
    return false;
}

void ThumbHotCache::Clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& s : m_slots) {
        s = Slot{};
    }
    m_lruCounter = 0;
    m_totalBytes = 0;
}

int RunThumbWorker(int argc, LPWSTR* argv, const ThumbWorkerDecodeOps& ops) {
    if (!ops.decodeFile || !ops.decodeMemory) return 2;

    std::wstring pipeName;
    if (!TryReadWorkerArgValue(argc, argv, L"--pipe", &pipeName)) {
        wchar_t buf[128]{};
        if (!ThumbIpc::BuildSessionScopedPipeName(buf, _countof(buf))) return 2;
        pipeName = buf;
    }

    const ScopedSecurityDescriptor pipeSec;
    SECURITY_ATTRIBUTES* pPipeSa = pipeSec.valid ? const_cast<SECURITY_ATTRIBUTES*>(&pipeSec.sa) : nullptr;

    const unsigned hw = std::thread::hardware_concurrency();
    const uint32_t poolThreads = std::clamp(hw > 0 ? hw / 2u : 2u, 2u, 4u);

    constexpr size_t kMaxQueuedConns = 32u;
    std::mutex queueMtx;
    std::condition_variable queueCv;
    std::deque<HANDLE> connQueue;
    std::atomic<bool> stopping{false};
    std::atomic<uint32_t> activeConns{0};
    std::atomic<uint64_t> lastActivityTick{GetTickCount64()};
    std::counting_semaphore<2> heavySem{2};
    std::counting_semaphore<1> titanSem{1};

    struct ActiveJobSlot {
        std::atomic<bool> cancelFlag{false};
        ULONGLONG deadlineTick = 0;
    };
    std::mutex watchdogMtx;
    std::condition_variable watchdogCv;
    std::vector<ActiveJobSlot*> activeSlots;
    std::thread watchdogThread([&]() {
        std::unique_lock<std::mutex> lk(watchdogMtx);
        while (!stopping.load(std::memory_order_acquire)) {
            if (activeSlots.empty()) {
                watchdogCv.wait(lk, [&] {
                    return stopping.load(std::memory_order_acquire) || !activeSlots.empty();
                });
            } else {
                watchdogCv.wait_for(lk, std::chrono::milliseconds(80), [&] {
                    return stopping.load(std::memory_order_acquire);
                });
            }
            if (stopping.load(std::memory_order_acquire)) break;
            const ULONGLONG now = GetTickCount64();
            for (ActiveJobSlot* slot : activeSlots) {
                if (now >= slot->deadlineTick) {
                    slot->cancelFlag.store(true, std::memory_order_release);
                }
            }
        }
    });

    struct NegCacheEntry {
        uint64_t size = 0;
        uint64_t mtime = 0;
        ULONGLONG expireTick = 0;
    };
    constexpr ULONGLONG kNegCacheTtlMs = 60000ULL;
    constexpr size_t kNegCacheMaxEntries = 256u;
    std::mutex negMtx;
    std::unordered_map<std::wstring, NegCacheEntry> negCache;

    auto makeLowerKey = [](const std::wstring& p) -> std::wstring {
        std::wstring k = p;
        for (wchar_t& c : k) {
            if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c + 32);
        }
        return k;
    };
    auto isNegCached = [&](const std::wstring& lowerKey, uint64_t fsize, uint64_t mtime) -> bool {
        const ULONGLONG now = GetTickCount64();
        std::lock_guard<std::mutex> lk(negMtx);
        auto it = negCache.find(lowerKey);
        if (it == negCache.end()) return false;
        if (it->second.size == fsize && it->second.mtime == mtime && now < it->second.expireTick) {
            return true;
        }
        negCache.erase(it);
        return false;
    };
    auto putNegCache = [&](const std::wstring& lowerKey, uint64_t fsize, uint64_t mtime) {
        const ULONGLONG now = GetTickCount64();
        std::lock_guard<std::mutex> lk(negMtx);
        if (negCache.size() >= kNegCacheMaxEntries) {
            for (auto it = negCache.begin(); it != negCache.end(); ) {
                if (now >= it->second.expireTick) it = negCache.erase(it);
                else ++it;
            }
            if (negCache.size() >= kNegCacheMaxEntries) {
                negCache.erase(negCache.begin());
            }
        }
        negCache[lowerKey] = NegCacheEntry{fsize, mtime, now + kNegCacheTtlMs};
    };

    auto handlePipeClient = [&](HANDLE hPipe, HANDLE hIoEvent, void* threadLoader) {
        struct PipeGuard {
            HANDLE h;
            HANDLE ioEv;
            bool sentResponse = false;
            std::atomic<uint32_t>& active;
            std::atomic<uint64_t>& lastTick;
            ~PipeGuard() {
                if (sentResponse && ioEv) {
                    WaitBrieflyForClientDrain(h, ioEv, 150u);
                }
                DisconnectNamedPipe(h);
                CloseHandle(h);
                lastTick.store(GetTickCount64(), std::memory_order_release);
                active.fetch_sub(1, std::memory_order_acq_rel);
            }
        } guard{hPipe, hIoEvent, false, activeConns, lastActivityTick};

        const DWORD ioDeadline = GetTickCount() + ThumbIpc::kThumbPipeTimeoutMs;
        ThumbIpc::ThumbPipeRequestHeader req{};
        if (!ReadExactFromPipe(hPipe, hIoEvent, &req, sizeof(req), ioDeadline)) return;

        auto sendError = [&](HRESULT hrCode) {
            ThumbIpc::ThumbPipeResponseHeader resp{};
            resp.magic = ThumbIpc::kThumbPipeMagic;
            resp.version = ThumbIpc::kThumbPipeVersion;
            resp.hr = static_cast<int32_t>(hrCode);
            if (WriteExactToPipe(hPipe, hIoEvent, &resp, sizeof(resp), GetTickCount() + 1000u)) {
                guard.sentResponse = true;
            }
        };

        if (req.magic != ThumbIpc::kThumbPipeMagic || req.version != ThumbIpc::kThumbPipeVersion) {
            sendError(E_INVALIDARG);
            return;
        }
        if (req.pathChars > 4096u || req.streamPayloadBytes > ThumbIpc::kThumbPipeMaxStreamBytes) {
            sendError(E_INVALIDARG);
            return;
        }

        const bool isStreamMode = (req.flags & ThumbIpc::kThumbReqFlagStreamMode) != 0;
        if (!isStreamMode && (req.pathChars == 0 || req.streamPayloadBytes != 0)) {
            sendError(E_INVALIDARG);
            return;
        }
        if (isStreamMode && req.streamPayloadBytes == 0) {
            sendError(E_INVALIDARG);
            return;
        }

        const uint64_t maxFileLimit = req.maxFileBytes; // 0 = unlimited
        if (isStreamMode && maxFileLimit > 0 && req.streamPayloadBytes > maxFileLimit) {
            sendError(E_FAIL);
            return;
        }

        std::wstring pathOrHint;
        if (req.pathChars > 0) {
            pathOrHint.resize(req.pathChars);
            if (!ReadExactFromPipe(hPipe, hIoEvent, pathOrHint.data(), req.pathChars * sizeof(wchar_t), ioDeadline)) {
                return;
            }
            if (pathOrHint.find(L'\0') != std::wstring::npos) {
                sendError(E_INVALIDARG);
                return;
            }
        }

        struct HeapBufferDeleter {
            void operator()(uint8_t* p) const noexcept {
                if (p) HeapFree(GetProcessHeap(), 0, p);
            }
        };
        std::unique_ptr<uint8_t[], HeapBufferDeleter> streamPayload;
        if (req.streamPayloadBytes > 0) {
            streamPayload.reset(static_cast<uint8_t*>(HeapAlloc(GetProcessHeap(), 0, req.streamPayloadBytes)));
            if (!streamPayload) {
                sendError(E_OUTOFMEMORY);
                return;
            }
            if (!ReadExactFromPipe(hPipe, hIoEvent, streamPayload.get(), req.streamPayloadBytes, ioDeadline)) {
                return;
            }
        }

        bool isTitanSize = false;
        uint64_t fsize = 0;
        uint64_t mtime = 0;
        std::wstring lowerKey;
        if (!isStreamMode) {
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (!GetFileAttributesExW(pathOrHint.c_str(), GetFileExInfoStandard, &fad)) {
                sendError(E_FAIL);
                return;
            }
            if ((fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
                ThumbIpc::IsCloudPlaceholderAttributes(fad.dwFileAttributes)) {
                sendError(E_FAIL);
                return;
            }
            fsize = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
            mtime = (static_cast<uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
                    fad.ftLastWriteTime.dwLowDateTime;
            if (fsize == 0 || (maxFileLimit > 0 && fsize > maxFileLimit)) {
                sendError(E_FAIL);
                return;
            }
            lowerKey = makeLowerKey(pathOrHint);
            if (isNegCached(lowerKey, fsize, mtime)) {
                sendError(E_FAIL);
                return;
            }
            isTitanSize = (fsize > 32ull * 1024ull * 1024ull);
        }

        const int clampedTarget = static_cast<int>(std::clamp(req.targetSize, 16u, ThumbIpc::kThumbPipeMaxEdge));
        const std::wstring_view ext = ExtensionOf(pathOrHint);
        const bool isSvg = ExtEqualsIgnoreCase(ext, L".svg") || ExtEqualsIgnoreCase(ext, L".svgz");
        const bool isHeavy = !isTitanSize && IsHeavyThumbnailExtension(pathOrHint);

        if (isTitanSize) {
            if (!titanSem.try_acquire_for(std::chrono::milliseconds(ThumbIpc::kThumbWorkerTitanWaitMs))) {
                sendError(E_PENDING);
                return;
            }
            SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        } else if (isHeavy) {
            if (!heavySem.try_acquire_for(std::chrono::milliseconds(ThumbIpc::kThumbWorkerTitanWaitMs))) {
                sendError(E_PENDING);
                return;
            }
        }

        const DWORD decodeCapMs = isSvg ? ThumbIpc::kThumbWorkerSvgDecodeCapMs : ThumbIpc::kThumbWorkerDecodeCapMs;
        ActiveJobSlot slot{};
        slot.deadlineTick = GetTickCount64() + decodeCapMs;
        {
            std::lock_guard<std::mutex> lk(watchdogMtx);
            activeSlots.push_back(&slot);
        }
        watchdogCv.notify_one();

        ThumbDecodeResult result;
        HRESULT hrDec = E_FAIL;
        if (isStreamMode) {
            hrDec = ops.decodeMemory(
                threadLoader, streamPayload.get(), req.streamPayloadBytes,
                pathOrHint.empty() ? L"stream.bin" : pathOrHint.c_str(),
                clampedTarget, result, &slot.cancelFlag);
        } else {
            hrDec = ops.decodeFile(threadLoader, pathOrHint.c_str(), clampedTarget, result, &slot.cancelFlag);
        }

        {
            std::lock_guard<std::mutex> lk(watchdogMtx);
            auto it = std::find(activeSlots.begin(), activeSlots.end(), &slot);
            if (it != activeSlots.end()) activeSlots.erase(it);
        }

        if (isTitanSize) {
            SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
            SetProcessWorkingSetSizeEx(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1), 0);
            titanSem.release();
        } else if (isHeavy) {
            heavySem.release();
        }

        const bool timedOut = slot.cancelFlag.load(std::memory_order_acquire);

        if (FAILED(hrDec) || result.width == 0 || result.height == 0 ||
            result.width > ThumbIpc::kThumbPipeMaxEdge || result.height > ThumbIpc::kThumbPipeMaxEdge ||
            static_cast<uint64_t>(result.stride) < static_cast<uint64_t>(result.width) * 4ull ||
            result.pixels.empty()) {
            // Negatively cache watchdog timeouts too so Explorer does not storm retries on giant slow files.
            if (!isStreamMode && !isSvg && hrDec != E_PENDING &&
                hrDec != E_OUTOFMEMORY && (hrDec != E_ABORT || timedOut) && !lowerKey.empty()) {
                putNegCache(lowerKey, fsize, mtime);
            }
            sendError(FAILED(hrDec) ? hrDec : E_FAIL);
            return;
        }

        const uint32_t packedStride = result.width * 4u;
        uint64_t bytesNeeded64 = static_cast<uint64_t>(result.stride) * result.height;
        if (bytesNeeded64 > ThumbIpc::kThumbPipeMaxPixelsBytes && result.stride > packedStride &&
            static_cast<uint64_t>(packedStride) * result.height <= ThumbIpc::kThumbPipeMaxPixelsBytes) {
            for (uint32_t y = 1; y < result.height; ++y) {
                std::memmove(
                    result.pixels.data() + static_cast<size_t>(y) * packedStride,
                    result.pixels.data() + static_cast<size_t>(y) * result.stride,
                    packedStride);
            }
            result.stride = packedStride;
            bytesNeeded64 = static_cast<uint64_t>(packedStride) * result.height;
        }

        if (bytesNeeded64 > ThumbIpc::kThumbPipeMaxPixelsBytes || bytesNeeded64 > result.pixels.size()) {
            sendError(E_FAIL);
            return;
        }

        ThumbIpc::ThumbPipeResponseHeader resp{};
        resp.magic = ThumbIpc::kThumbPipeMagic;
        resp.version = ThumbIpc::kThumbPipeVersion;
        resp.hr = S_OK;
        resp.width = result.width;
        resp.height = result.height;
        resp.stride = result.stride;
        resp.isPremultiplied = result.isPremultiplied ? 1u : 0u;
        resp.pixelBytes = static_cast<uint32_t>(bytesNeeded64);

        const DWORD writeDeadline = GetTickCount() + ThumbIpc::kThumbPipeTimeoutMs;
        if (WriteExactToPipe(hPipe, hIoEvent, &resp, sizeof(resp), writeDeadline)) {
            if (WriteExactToPipe(hPipe, hIoEvent, result.pixels.data(), resp.pixelBytes, writeDeadline)) {
                guard.sentResponse = true;
            }
        }
    };

    std::vector<std::thread> workers;
    workers.reserve(poolThreads);
    for (uint32_t i = 0; i < poolThreads; ++i) {
        workers.emplace_back([&]() {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            HANDLE hIoEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            void* threadLoader = ops.createLoader ? ops.createLoader() : nullptr;

            for (;;) {
                HANDLE hConn = INVALID_HANDLE_VALUE;
                {
                    std::unique_lock<std::mutex> lk(queueMtx);
                    queueCv.wait(lk, [&] { return stopping.load(std::memory_order_acquire) || !connQueue.empty(); });
                    if (stopping.load(std::memory_order_acquire) && connQueue.empty()) break;
                    hConn = connQueue.front();
                    connQueue.pop_front();
                }
                if (hConn != INVALID_HANDLE_VALUE && hIoEvent) {
                    handlePipeClient(hConn, hIoEvent, threadLoader);
                } else if (hConn != INVALID_HANDLE_VALUE) {
                    CloseHandle(hConn);
                    activeConns.fetch_sub(1, std::memory_order_acq_rel);
                }
            }

            if (ops.destroyLoader && threadLoader) {
                ops.destroyLoader(threadLoader);
            }
            if (hIoEvent) CloseHandle(hIoEvent);
            if (SUCCEEDED(coHr)) CoUninitialize();
        });
    }

    HANDLE connectEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!connectEvent) {
        stopping.store(true, std::memory_order_release);
        queueCv.notify_all();
        watchdogCv.notify_all();
        for (auto& w : workers) if (w.joinable()) w.join();
        if (watchdogThread.joinable()) watchdogThread.join();
        return 2;
    }

    auto createPipeInstance = [&](bool first) -> HANDLE {
        DWORD openMode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;
        if (first) openMode |= FILE_FLAG_FIRST_PIPE_INSTANCE;
        return CreateNamedPipeW(
            pipeName.c_str(),
            openMode,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            256u * 1024u,
            256u * 1024u,
            0,
            pPipeSa);
    };

    bool firstInstance = true;
    HANDLE hPrefetchedPipe = INVALID_HANDLE_VALUE;
    while (!stopping.load(std::memory_order_acquire)) {
        HANDLE hPipe = hPrefetchedPipe;
        hPrefetchedPipe = INVALID_HANDLE_VALUE;
        if (hPipe == INVALID_HANDLE_VALUE) {
            hPipe = createPipeInstance(firstInstance);
            if (hPipe == INVALID_HANDLE_VALUE) {
                if (firstInstance && GetLastError() == ERROR_ACCESS_DENIED) {
                    // Another --thumb-worker for this user+session is already running.
                    break;
                }
                Sleep(50);
                continue;
            }
            firstInstance = false;
        }

        ResetEvent(connectEvent);
        OVERLAPPED ov{};
        ov.hEvent = connectEvent;
        BOOL connected = ConnectNamedPipe(hPipe, &ov);
        if (!connected) {
            const DWORD err = GetLastError();
            if (err == ERROR_PIPE_CONNECTED) {
                connected = TRUE;
            } else if (err == ERROR_IO_PENDING) {
                while (!stopping.load(std::memory_order_acquire)) {
                    const DWORD wr = WaitForSingleObject(connectEvent, 500);
                    if (wr == WAIT_OBJECT_0) {
                        DWORD dummy = 0;
                        connected = GetOverlappedResult(hPipe, &ov, &dummy, FALSE);
                        break;
                    }
                    if (activeConns.load(std::memory_order_acquire) == 0 &&
                        (GetTickCount64() - lastActivityTick.load(std::memory_order_acquire)) >= ThumbIpc::kThumbWorkerIdleExitMs) {
                        stopping.store(true, std::memory_order_release);
                        CancelIoEx(hPipe, &ov);
                        DWORD dummy = 0;
                        GetOverlappedResult(hPipe, &ov, &dummy, TRUE);
                        break;
                    }
                }
            }
        }

        if (connected && !stopping.load(std::memory_order_acquire)) {
            // Pre-create next listening instance before dispatching so Explorer burst connections
            // never hit a zero-listener window.
            hPrefetchedPipe = createPipeInstance(false);

            bool accepted = false;
            {
                std::lock_guard<std::mutex> lk(queueMtx);
                if (connQueue.size() < kMaxQueuedConns) {
                    activeConns.fetch_add(1, std::memory_order_acq_rel);
                    lastActivityTick.store(GetTickCount64(), std::memory_order_release);
                    connQueue.push_back(hPipe);
                    accepted = true;
                }
            }
            if (accepted) {
                queueCv.notify_one();
            } else {
                DisconnectNamedPipe(hPipe);
                CloseHandle(hPipe);
            }
        } else {
            CloseHandle(hPipe);
        }
    }

    if (hPrefetchedPipe != INVALID_HANDLE_VALUE) {
        CloseHandle(hPrefetchedPipe);
    }
    CloseHandle(connectEvent);
    stopping.store(true, std::memory_order_release);
    queueCv.notify_all();
    watchdogCv.notify_all();
    for (auto& w : workers) {
        if (w.joinable()) w.join();
    }
    if (watchdogThread.joinable()) watchdogThread.join();
    return 0;
}

} // namespace QuickView
