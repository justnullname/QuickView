#pragma once
// ============================================================================
// qvx_sdk.hpp - Modern C++23 Header-Only SDK for QuickView Plugin Developers
// ============================================================================
// Designed for -fno-exceptions, zero-cost RAII, and C++23/26 standards.
// ============================================================================

#include "qvx.h"
#include "qvx_sr.h"
#include <string_view>
#include <span>
#include <optional>
#include <expected>
#include <wrl/client.h>

namespace qvx {

using Microsoft::WRL::ComPtr;

// Inline helper to test cancellation predicate
inline bool IsCancelled(const QVX_SR_ExecuteParams* params) noexcept {
    return params && params->check_cancel && params->check_cancel(params->cancel_user_data);
}

// Scoped Context Wrapper
template <typename TContext, void (*DestroyFn)(TContext)>
class UniqueContext {
public:
    constexpr UniqueContext() noexcept : m_ctx(nullptr) {}
    explicit UniqueContext(TContext ctx) noexcept : m_ctx(ctx) {}
    ~UniqueContext() noexcept { Reset(); }

    UniqueContext(const UniqueContext&) = delete;
    UniqueContext& operator=(const UniqueContext&) = delete;

    UniqueContext(UniqueContext&& o) noexcept : m_ctx(o.m_ctx) {
        o.m_ctx = nullptr;
    }

    UniqueContext& operator=(UniqueContext&& o) noexcept {
        if (this != &o) {
            Reset();
            m_ctx = o.m_ctx;
            o.m_ctx = nullptr;
        }
        return *this;
    }

    void Reset(TContext newCtx = nullptr) noexcept {
        if (m_ctx && DestroyFn) {
            DestroyFn(m_ctx);
        }
        m_ctx = newCtx;
    }

    [[nodiscard]] TContext Get() const noexcept { return m_ctx; }
    [[nodiscard]] explicit operator bool() const noexcept { return m_ctx != nullptr; }

private:
    TContext m_ctx;
};

// Constexpr Parameter Manifest Helpers
constexpr inline QVX_ParamDesc MakeSliderParam(
    const char* id, const char* label, const char* tooltip,
    float min_val, float max_val, float step, float default_val, const char* format_str = "%.2f"
) noexcept {
    QVX_ParamDesc d{};
    d.struct_size = sizeof(QVX_ParamDesc);
    d.id = id;
    d.label = label;
    d.tooltip = tooltip;
    d.type = QVX_PARAM_TYPE_FLOAT;
    d.float_param.min_val = min_val;
    d.float_param.max_val = max_val;
    d.float_param.step = step;
    d.float_param.default_val = default_val;
    d.float_param.format_str = format_str;
    return d;
}

constexpr inline QVX_ParamDesc MakeToggleParam(
    const char* id, const char* label, const char* tooltip, bool default_val
) noexcept {
    QVX_ParamDesc d{};
    d.struct_size = sizeof(QVX_ParamDesc);
    d.id = id;
    d.label = label;
    d.tooltip = tooltip;
    d.type = QVX_PARAM_TYPE_BOOL;
    d.bool_param.default_val = default_val;
    return d;
}

constexpr inline QVX_ParamDesc MakeIntSliderParam(
    const char* id, const char* label, const char* tooltip,
    int32_t min_val, int32_t max_val, int32_t step, int32_t default_val, const char* format_str = "%d"
) noexcept {
    QVX_ParamDesc d{};
    d.struct_size = sizeof(QVX_ParamDesc);
    d.id = id;
    d.label = label;
    d.tooltip = tooltip;
    d.type = QVX_PARAM_TYPE_INT;
    d.int_param.min_val = min_val;
    d.int_param.max_val = max_val;
    d.int_param.step = step;
    d.int_param.default_val = default_val;
    d.int_param.format_str = format_str;
    return d;
}

constexpr inline QVX_ParamDesc MakeEnumParam(
    const char* id, const char* label, const char* tooltip,
    int32_t default_val, uint32_t option_count, const QVX_ParamOption* options
) noexcept {
    QVX_ParamDesc d{};
    d.struct_size = sizeof(QVX_ParamDesc);
    d.id = id;
    d.label = label;
    d.tooltip = tooltip;
    d.type = QVX_PARAM_TYPE_ENUM;
    d.enum_param.default_val = default_val;
    d.enum_param.option_count = option_count;
    d.enum_param.options = options;
    return d;
}

// Plugin Export Registration Helper
// Simplifies defining qvx_init / qvx_shutdown in C++ plugins
#define QVX_EXPORT_PLUGIN(HeaderInstance)                                       \
    extern "C" QVX_API bool qvx_init(const QVX_PluginHeader** out_header) {     \
        if (!out_header) return false;                                          \
        *out_header = &(HeaderInstance);                                        \
        return true;                                                            \
    }                                                                           \
    extern "C" QVX_API void qvx_shutdown(void) {}

} // namespace qvx
