#pragma once

#include <utility>
#include <type_traits>

namespace QuickView {

// ============================================================================
// Lightweight Type-Erased Async Job (No virtual functions, no STL tuple overhead)
// Eradicates std::thread::_Invoke<std::tuple<...>> template bloat.
// ============================================================================
class AsyncJob {
    void* m_target = nullptr;
    void (*m_invoker)(void*) = nullptr;
    void (*m_deleter)(void*) = nullptr;

public:
    AsyncJob() noexcept = default;

    // Direct C-style callback with context (Zero heap allocation, zero template bloat)
    AsyncJob(void (*fn)(void*), void* ctx, void (*cleanup)(void*) = nullptr) noexcept
        : m_target(ctx), m_invoker(fn), m_deleter(cleanup) {}

    // Matches stateless lambdas and function pointers without heap allocation
    template <typename F>
        requires std::is_convertible_v<F, void (*)()>
    AsyncJob(F fn) noexcept
        : m_target(reinterpret_cast<void*>(static_cast<void (*)()>(fn)))
        , m_invoker([](void* ptr) noexcept {
            reinterpret_cast<void (*)()>(ptr)();
        })
        , m_deleter(nullptr) {}

    template <typename F>
        requires (!std::is_same_v<std::decay_t<F>, AsyncJob> &&
                  !std::is_convertible_v<F, void (*)()> &&
                  !std::is_convertible_v<F, void (*)(void*)>)
    AsyncJob(F&& f) {
        using DecayF = std::decay_t<F>;
        auto* p = new DecayF(std::forward<F>(f));
        m_target = p;
        m_invoker = [](void* ptr) noexcept {
            (*static_cast<DecayF*>(ptr))();
        };
        m_deleter = [](void* ptr) noexcept {
            delete static_cast<DecayF*>(ptr);
        };
    }

    AsyncJob(AsyncJob&& o) noexcept
        : m_target(std::exchange(o.m_target, nullptr))
        , m_invoker(std::exchange(o.m_invoker, nullptr))
        , m_deleter(std::exchange(o.m_deleter, nullptr)) {}

    AsyncJob& operator=(AsyncJob&& o) noexcept {
        if (this != &o) {
            Reset();
            m_target = std::exchange(o.m_target, nullptr);
            m_invoker = std::exchange(o.m_invoker, nullptr);
            m_deleter = std::exchange(o.m_deleter, nullptr);
        }
        return *this;
    }

    AsyncJob(const AsyncJob&) = delete;
    AsyncJob& operator=(const AsyncJob&) = delete;

    ~AsyncJob() {
        Reset();
    }

    void RunAndDestroy() noexcept {
        if (m_invoker) {
            m_invoker(m_target);
        }
        Reset();
    }

    void Reset() noexcept {
        if (m_deleter && m_target) {
            m_deleter(m_target);
        }
        m_target = nullptr;
        m_invoker = nullptr;
        m_deleter = nullptr;
    }

    explicit operator bool() const noexcept {
        return m_invoker != nullptr;
    }
};

// Fire-and-forget OS thread dispatch (single native entrypoint across whole binary)
void RunDetached(AsyncJob job);

// Fire-and-forget Windows kernel threadpool dispatch (near-zero latency, reuses threads)
void PostThreadPool(AsyncJob job);

} // namespace QuickView
