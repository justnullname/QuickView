#pragma once
// ============================================================================
// qvx_sdk.hpp - Modern C++23 Header-Only SDK for QuickView v3 Plugin Authors
// ============================================================================
// Designed for -fno-exceptions, zero-cost RAII, and C++23/26 standards.
// ============================================================================

#include "abi/qvx_core.h"
#include "abi/qvx_processor.h"
#include "abi/qvx_analyzer.h"
#include "abi/qvx_shell_ext.h"
#include <string_view>
#include <span>
#include <optional>
#include <expected>
#include <wrl/client.h>

namespace qvx {

using Microsoft::WRL::ComPtr;

// Inline helpers to test cancellation tokens
inline bool IsCancelled(const QVX_CancelToken& token) noexcept {
    return token.check_cancel && token.check_cancel(token.user_data);
}

inline bool IsCancelled(const QVX_ProcExecuteParams* params) noexcept {
    return params && IsCancelled(params->cancel);
}

inline bool IsCancelled(const QVX_AnalyzerInput* input) noexcept {
    return input && IsCancelled(input->cancel);
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
        if (m_ctx == newCtx) {
            return;
        }
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

// Constexpr Parameter Descriptor Helpers
constexpr inline QVX_ParamDesc MakeFloatParam(
    const char* id,
    const char* label,
    const char* tooltip,
    float min_val,
    float max_val,
    float default_val,
    float step = 0.01f,
    const char* format_str = "%.2f",
    const char* visible_for_preset = nullptr,
    const char* group = nullptr
) noexcept {
    QVX_ParamDesc d{};
    d.struct_size = sizeof(QVX_ParamDesc);
    d.id = id;
    d.label = label;
    d.tooltip = tooltip;
    d.group = group;
    d.visible_for_preset = visible_for_preset;
    d.type = QVX_PARAM_TYPE_FLOAT;
    d.float_param.min_val = min_val;
    d.float_param.max_val = max_val;
    d.float_param.step = step;
    d.float_param.default_val = default_val;
    d.float_param.format_str = format_str;
    return d;
}

constexpr inline QVX_ParamDesc MakeSliderParam(
    const char* id,
    const char* label,
    const char* tooltip,
    float min_val,
    float max_val,
    float default_val,
    float step = 0.01f,
    const char* format_str = "%.2f",
    const char* visible_for_preset = nullptr,
    const char* group = nullptr
) noexcept {
    return MakeFloatParam(id, label, tooltip, min_val, max_val, default_val, step, format_str, visible_for_preset, group);
}

constexpr inline QVX_ParamDesc MakeToggleParam(
    const char* id,
    const char* label,
    const char* tooltip,
    bool default_val,
    const char* visible_for_preset = nullptr,
    const char* group = nullptr
) noexcept {
    QVX_ParamDesc d{};
    d.struct_size = sizeof(QVX_ParamDesc);
    d.id = id;
    d.label = label;
    d.tooltip = tooltip;
    d.group = group;
    d.visible_for_preset = visible_for_preset;
    d.type = QVX_PARAM_TYPE_BOOL;
    d.bool_param.default_val = default_val;
    return d;
}

constexpr inline QVX_ParamDesc MakeIntSliderParam(
    const char* id,
    const char* label,
    const char* tooltip,
    int32_t min_val,
    int32_t max_val,
    int32_t default_val,
    int32_t step = 1,
    const char* format_str = "%d",
    const char* visible_for_preset = nullptr,
    const char* group = nullptr
) noexcept {
    QVX_ParamDesc d{};
    d.struct_size = sizeof(QVX_ParamDesc);
    d.id = id;
    d.label = label;
    d.tooltip = tooltip;
    d.group = group;
    d.visible_for_preset = visible_for_preset;
    d.type = QVX_PARAM_TYPE_INT;
    d.int_param.min_val = min_val;
    d.int_param.max_val = max_val;
    d.int_param.step = step;
    d.int_param.default_val = default_val;
    d.int_param.format_str = format_str;
    return d;
}

constexpr inline QVX_ParamDesc MakeEnumParam(
    const char* id,
    const char* label,
    const char* tooltip,
    int32_t default_val,
    uint32_t option_count,
    const QVX_ParamOption* options,
    const char* visible_for_preset = nullptr,
    const char* group = nullptr
) noexcept {
    QVX_ParamDesc d{};
    d.struct_size = sizeof(QVX_ParamDesc);
    d.id = id;
    d.label = label;
    d.tooltip = tooltip;
    d.group = group;
    d.visible_for_preset = visible_for_preset;
    d.type = QVX_PARAM_TYPE_ENUM;
    d.enum_param.default_val = default_val;
    d.enum_param.option_count = option_count;
    d.enum_param.options = options;
    return d;
}

constexpr inline QVX_ParamDesc MakeStringParam(
    const char* id,
    const char* label,
    const char* tooltip,
    const char* default_val,
    uint32_t max_len = 256,
    const char* visible_for_preset = nullptr,
    const char* group = nullptr
) noexcept {
    QVX_ParamDesc d{};
    d.struct_size = sizeof(QVX_ParamDesc);
    d.id = id;
    d.label = label;
    d.tooltip = tooltip;
    d.group = group;
    d.visible_for_preset = visible_for_preset;
    d.type = QVX_PARAM_TYPE_STRING;
    d.string_param.default_val = default_val;
    d.string_param.max_len = max_len;
    return d;
}

} // namespace qvx
