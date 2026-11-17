#include "pch.h"
#include "AsyncJob.h"
#include <process.h>

namespace QuickView {

void RunDetached(AsyncJob job) {
    if (!job) return;
    auto* pJob = new AsyncJob(std::move(job));
    uintptr_t handle = _beginthreadex(
        nullptr,
        0,
        [](void* arg) noexcept -> unsigned {
            auto* j = static_cast<AsyncJob*>(arg);
            j->RunAndDestroy();
            delete j;
            return 0;
        },
        pJob,
        0,
        nullptr
    );
    if (handle != 0) {
        CloseHandle(reinterpret_cast<HANDLE>(handle));
    } else {
        // Failed to spawn thread; run inline as graceful fallback
        pJob->RunAndDestroy();
        delete pJob;
    }
}

void PostThreadPool(AsyncJob job) {
    if (!job) return;
    auto* pJob = new AsyncJob(std::move(job));
    BOOL ok = TrySubmitThreadpoolCallback(
        [](PTP_CALLBACK_INSTANCE, PVOID arg) noexcept {
            auto* j = static_cast<AsyncJob*>(arg);
            j->RunAndDestroy();
            delete j;
        },
        pJob,
        nullptr
    );
    if (!ok) {
        // Fallback to RunDetached if threadpool submission fails
        RunDetached(std::move(*pJob));
        delete pJob;
    }
}

} // namespace QuickView

