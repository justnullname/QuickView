/*
 * Mock QVX v3 Analyzer (OCR) Plugin for Unit & Integration Testing
 * Copyright (C) 2026-Present QuickView Contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <new>

#include "Plugin/qvx_sdk.hpp"

namespace {

constexpr const char* kPluginId = "com.quickview.test.mock_ocr";

std::atomic<int32_t> g_createCount{0};
std::atomic<int32_t> g_destroyCount{0};
std::atomic<int32_t> g_delayMs{0};

struct MockContext {
    bool valid = true;
};

struct MockResultBuffer {
    QVX_AnalyzerLine lines[2]{};
    QVX_AnalyzerToken tokens[4]{};
    char utf8Pool[64]{};
};

QVX_NormQuad MakeNormQuad(float x0, float y0, float x1, float y1) noexcept {
    QVX_NormQuad q{};
    q.x[0] = x0; q.y[0] = y0;
    q.x[1] = x1; q.y[1] = y0;
    q.x[2] = x1; q.y[2] = y1;
    q.x[3] = x0; q.y[3] = y1;
    q.min_x = x0; q.min_y = y0;
    q.max_x = x1; q.max_y = y1;
    return q;
}

uint32_t Analyzer_GetCaps() {
    return QVX_ANALYZER_CAP_OCR | QVX_ANALYZER_CAP_MULTI_LANG | QVX_ANALYZER_CAP_ROI;
}

uint32_t Analyzer_GetPresetCount() {
    return 2;
}

const QVX_AnalyzerPresetInfo* Analyzer_GetPresetInfo(uint32_t index) {
    static const QVX_AnalyzerPresetInfo kPresets[2] = {
        {
            sizeof(QVX_AnalyzerPresetInfo),
            "mock_fast",
            "Mock Fast OCR",
            "Deterministic in-memory mock OCR preset",
            nullptr,
            true,
            0,
            nullptr,
            nullptr
        },
        {
            sizeof(QVX_AnalyzerPresetInfo),
            "mock_missing_asset",
            "Mock Missing Weights",
            "Simulates missing weights to trigger Windows OCR fallback",
            "missing_weights",
            false,
            1024,
            nullptr,
            nullptr
        }
    };
    return (index < 2) ? &kPresets[index] : nullptr;
}

QVX_AnalyzerContext Analyzer_CreateContext(const char* preset_id) {
    if (preset_id && std::strcmp(preset_id, "mock_missing_asset") == 0) {
        return nullptr;
    }
    auto* ctx = new (std::nothrow) MockContext{};
    if (ctx) {
        g_createCount.fetch_add(1, std::memory_order_acq_rel);
    }
    return ctx;
}

void Analyzer_DestroyContext(QVX_AnalyzerContext ctx) {
    auto* c = static_cast<MockContext*>(ctx);
    if (c) {
        g_destroyCount.fetch_add(1, std::memory_order_acq_rel);
        delete c;
    }
}

int32_t Analyzer_Analyze(
    QVX_AnalyzerContext ctx,
    const QVX_AnalyzerInput* input,
    QVX_AnalyzerResult* out_result)
{
    if (!ctx || !input || !out_result || !input->pixels || input->width == 0 || input->height == 0) {
        return QVX_E_INVALIDARG;
    }
    if (qvx::IsCancelled(input)) {
        return QVX_E_ABORT;
    }

    const int32_t delay = g_delayMs.load(std::memory_order_acquire);
    for (int32_t elapsed = 0; elapsed < delay; elapsed += 5) {
        Sleep(5);
        if (qvx::IsCancelled(input)) {
            return QVX_E_ABORT;
        }
    }

    auto* buf = new (std::nothrow) MockResultBuffer{};
    if (!buf) {
        return QVX_E_OUTOFMEMORY;
    }

    // UTF-8 pool layout:
    // Line 0: "Mock OCR Line1" (14 bytes at 0..14)
    //   tok 0: "Mock"  [0..4)
    //   tok 1: "OCR"   [5..8)
    //   tok 2: "Line1" [9..14)
    // Line 1: "\xE6\xB5\x8B\xE8\xAF\x95" ("测试", 6 bytes at 15..21)
    //   tok 3: "测试"  [15..21)
    static constexpr const char kLine0[] = "Mock OCR Line1";
    static constexpr const char kLine1[] = "\xE6\xB5\x8B\xE8\xAF\x95";
    std::memcpy(buf->utf8Pool, kLine0, sizeof(kLine0) - 1);
    buf->utf8Pool[14] = '\0';
    std::memcpy(buf->utf8Pool + 15, kLine1, sizeof(kLine1) - 1);
    buf->utf8Pool[21] = '\0';

    buf->tokens[0] = { MakeNormQuad(0.10f, 0.15f, 0.25f, 0.28f), 0, 4, 0.99f, 1, {} };
    buf->tokens[1] = { MakeNormQuad(0.28f, 0.15f, 0.40f, 0.28f), 5, 3, 0.98f, 1, {} };
    buf->tokens[2] = { MakeNormQuad(0.43f, 0.15f, 0.62f, 0.28f), 9, 5, 0.97f, 0, {} };
    buf->tokens[3] = { MakeNormQuad(0.10f, 0.45f, 0.35f, 0.60f), 15, 6, 0.96f, 0, {} };

    buf->lines[0] = { MakeNormQuad(0.10f, 0.15f, 0.62f, 0.28f), 0, 14, 0, 3, 0.98f, QVX_ANALYZER_FLAG_NONE };
    buf->lines[1] = { MakeNormQuad(0.10f, 0.45f, 0.35f, 0.60f), 15, 6, 3, 1, 0.96f, QVX_ANALYZER_FLAG_NONE };

    out_result->struct_size = sizeof(QVX_AnalyzerResult);
    out_result->text_angle_deg = 0.0f;
    out_result->line_count = 2;
    out_result->lines = buf->lines;
    out_result->token_count = 4;
    out_result->tokens = buf->tokens;
    out_result->utf8_pool_bytes = 22;
    out_result->utf8_pool = buf->utf8Pool;
    out_result->plugin_private = buf;
    return QVX_OK;
}

void Analyzer_FreeResult(QVX_AnalyzerContext ctx, QVX_AnalyzerResult* result) {
    (void)ctx;
    if (!result) return;
    delete static_cast<MockResultBuffer*>(result->plugin_private);
    result->lines = nullptr;
    result->tokens = nullptr;
    result->utf8_pool = nullptr;
    result->plugin_private = nullptr;
}

uint32_t Common_GetParamCount() {
    return 1;
}

const QVX_ParamDesc* Common_GetParamDesc(uint32_t index) {
    if (index != 0) return nullptr;
    static QVX_ParamDesc desc = []() {
        QVX_ParamDesc d{};
        d.struct_size = sizeof(QVX_ParamDesc);
        d.id = "delay_ms";
        d.label = "Delay (ms)";
        d.tooltip = "Artificial delay for cancel testing";
        d.type = QVX_PARAM_TYPE_INT;
        d.int_param.min_val = 0;
        d.int_param.max_val = 2000;
        d.int_param.step = 10;
        d.int_param.default_val = 0;
        d.int_param.format_str = "%d ms";
        return d;
    }();
    return &desc;
}

int32_t Common_SetParamNumber(const char* param_id, float value) {
    if (!param_id) return QVX_E_INVALIDARG;
    if (std::strcmp(param_id, "delay_ms") == 0) {
        g_delayMs.store(static_cast<int32_t>(value), std::memory_order_release);
        return QVX_OK;
    }
    return QVX_E_INVALIDARG;
}

int32_t Analyzer_GetParamValue(QVX_AnalyzerContext ctx, const char* param_id, float* out_val) {
    (void)ctx;
    if (!param_id || !out_val) return QVX_E_INVALIDARG;
    if (std::strcmp(param_id, "delay_ms") == 0) {
        *out_val = static_cast<float>(g_delayMs.load(std::memory_order_acquire));
        return QVX_OK;
    }
    return QVX_E_INVALIDARG;
}

int32_t Analyzer_SetParamValue(QVX_AnalyzerContext ctx, const char* param_id, float value) {
    (void)ctx;
    return Common_SetParamNumber(param_id, value);
}

uint32_t Common_GetAssetCount() {
    return 1;
}

const QVX_AssetDesc* Common_GetAssetDesc(uint32_t index) {
    if (index != 0) return nullptr;
    static const QVX_AssetDesc kAsset = {
        sizeof(QVX_AssetDesc),
        "missing_weights",
        "Missing Mock Weights",
        "Test missing required asset descriptor",
        "models",
        "",
        "",
        1024,
        false,
        true
    };
    return &kAsset;
}

constexpr QVX_Analyzer_VTable g_analyzerVTable = {
    sizeof(QVX_Analyzer_VTable),
    &Analyzer_GetCaps,
    &Analyzer_GetPresetCount,
    &Analyzer_GetPresetInfo,
    &Analyzer_CreateContext,
    &Analyzer_DestroyContext,
    &Analyzer_Analyze,
    &Analyzer_FreeResult,
    &Common_GetParamCount,
    &Common_GetParamDesc,
    &Analyzer_GetParamValue,
    &Analyzer_SetParamValue,
    nullptr,
    nullptr,
    &Common_GetAssetCount,
    &Common_GetAssetDesc,
    nullptr
};

const void* QvxGetInterface(uint32_t interfaceId, uint32_t interfaceVersion) noexcept {
    if (interfaceId == QVX_IFACE_ANALYZER && interfaceVersion == QVX_ANALYZER_INTERFACE_VERSION) {
        return &g_analyzerVTable;
    }
    return nullptr;
}

constexpr QVX_PluginHeader g_pluginHeader = {
    sizeof(QVX_PluginHeader),
    QVX_ABI_VERSION,
    kPluginId,
    "Mock OCR Analyzer",
    "QuickView Tests",
    "1.0.0",
    QVX_FLAG_THREAD_SAFE,
    (1u << QVX_IFACE_ANALYZER),
    &QvxGetInterface,
    nullptr,
    &Common_GetParamCount,
    &Common_GetParamDesc,
    &Common_SetParamNumber,
    nullptr,
    &Common_GetAssetCount,
    &Common_GetAssetDesc,
    nullptr
};

} // namespace

extern "C" QVX_API bool qvx_init(const QVX_HostServices* host, const QVX_PluginHeader** outHeader) {
    (void)host;
    if (!outHeader) return false;
    *outHeader = &g_pluginHeader;
    return true;
}

extern "C" QVX_API void qvx_shutdown(void) {
}

extern "C" QVX_API int32_t MockOcrGetCreateCount() {
    return g_createCount.load(std::memory_order_acquire);
}

extern "C" QVX_API int32_t MockOcrGetDestroyCount() {
    return g_destroyCount.load(std::memory_order_acquire);
}
