#pragma once
/*
 * QuickView Analyzer Plugin Interface ABI (QVX_IFACE_ANALYZER, Pure C11 / C++23)
 * Copyright (C) 2026-Present QuickView Contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

// ============================================================================
// qvx_analyzer.h - Structured Visual Analysis Interface (OCR, Barcode, etc.)
// ============================================================================
// Design Principles:
// 1. Pure C ABI: POD structs only, no STL/exceptions/RTTI across DLL boundary.
// 2. DOD Flat Arrays: Result lines, tokens, and UTF-8 string pool are flat
//    contiguous buffers owned by the plugin until free_result().
// 3. Normalized Coordinates: All quads and ROIs are normalized [0.0, 1.0] in
//    visual image space, invariant to downsampling or DPI scaling.
// ============================================================================

#include "qvx_core.h"

#define QVX_ANALYZER_INTERFACE_VERSION 0x00030000u // 3.0.0

#ifdef __cplusplus
extern "C" {
#endif

// Opaque analyzer context handle (owns inference runtime, weights, scratch memory)
typedef void* QVX_AnalyzerContext;

// High-level analyzer category
typedef enum QVX_AnalyzerKind {
    QVX_ANALYZER_KIND_UNKNOWN = 0,
    QVX_ANALYZER_KIND_OCR     = 1,
    QVX_ANALYZER_KIND_BARCODE = 2
} QVX_AnalyzerKind;

// Analyzer capability flags
typedef enum QVX_AnalyzerCaps {
    QVX_ANALYZER_CAP_NONE          = 0,
    QVX_ANALYZER_CAP_OCR           = 1u << 0,
    QVX_ANALYZER_CAP_BARCODE       = 1u << 1,
    QVX_ANALYZER_CAP_MULTI_LANG    = 1u << 2,
    QVX_ANALYZER_CAP_GPU           = 1u << 3,
    QVX_ANALYZER_CAP_VERTICAL_TEXT = 1u << 4,
    QVX_ANALYZER_CAP_ROI           = 1u << 5
} QVX_AnalyzerCaps;

// Per-line semantic and layout flags
typedef enum QVX_AnalyzerSpanFlags {
    QVX_ANALYZER_FLAG_NONE     = 0,
    QVX_ANALYZER_FLAG_VERTICAL = 1u << 0,
    QVX_ANALYZER_FLAG_RTL      = 1u << 1,
    QVX_ANALYZER_FLAG_URL      = 1u << 2,
    QVX_ANALYZER_FLAG_BARCODE  = 1u << 3
} QVX_AnalyzerSpanFlags;

// Normalized visual quadrilateral [0.0, 1.0] with precomputed AABB
typedef struct QVX_NormQuad {
    float x[4]; // TL, TR, BR, BL
    float y[4]; // TL, TR, BR, BL
    float min_x;
    float min_y;
    float max_x;
    float max_y;
} QVX_NormQuad;

// Preset / Model descriptor exposed by analyzer plugins
typedef struct QVX_AnalyzerPresetInfo {
    uint32_t    struct_size;        // sizeof(QVX_AnalyzerPresetInfo)
    const char* preset_id;          // Unique preset/model ID
    const char* display_name;       // Localized UTF-8 display name
    const char* description;        // Localized UTF-8 description
    const char* required_asset_id;  // Links to QVX_AssetDesc::asset_id (NULL if built-in)
    bool        is_installed;       // True if required weights/assets are locally present
    uint64_t    asset_size_bytes;   // Asset download size hint (0 if embedded)
    const char* asset_download_url; // Optional download URL override
    const char* asset_sha256;       // Optional SHA-256 hex digest
} QVX_AnalyzerPresetInfo;

// Input frame and execution hints for analyze()
typedef struct QVX_AnalyzerInput {
    uint32_t             struct_size;       // sizeof(QVX_AnalyzerInput)
    const uint8_t*       pixels;            // Tight or strided pixel buffer
    uint32_t             width;             // Full visual width in pixels
    uint32_t             height;            // Full visual height in pixels
    uint32_t             stride;            // Bytes per row (>= width * 4 for BGRA8)
    QVX_PixelFormat      format;            // Expected: QVX_PIXFMT_BGRA8888
    bool                 has_roi;           // True to analyze only normalized roi_norm
    float                roi_norm[4];       // { left, top, right, bottom } in [0.0, 1.0]
    const char*          language_hint;     // BCP-47 tag (e.g. "zh-Hans", "en-US") or NULL for auto
    QVX_CancelToken      cancel;            // Cancellation token polled during inference
    QVX_ProgressCallback on_progress;       // Optional progress callback
    void*                progress_user_data;
} QVX_AnalyzerInput;

// Optional word/token span inside a line
typedef struct QVX_AnalyzerToken {
    QVX_NormQuad quad;
    uint32_t     utf8_offset;
    uint32_t     utf8_len;
    float        confidence;
    uint8_t      space_after; // 1 = insert space before next token, 0 = tight (e.g. CJK)
    uint8_t      reserved[3];
} QVX_AnalyzerToken;

// Line / region span
typedef struct QVX_AnalyzerLine {
    QVX_NormQuad quad;
    uint32_t     utf8_offset;
    uint32_t     utf8_len;
    uint32_t     token_start;
    uint32_t     token_count; // 0 = host derives simple character splits from line quad
    float        confidence;
    uint32_t     flags;       // Bitwise OR of QVX_AnalyzerSpanFlags
} QVX_AnalyzerLine;

// Flat contiguous analysis result owned by plugin until free_result()
typedef struct QVX_AnalyzerResult {
    uint32_t                 struct_size;     // sizeof(QVX_AnalyzerResult)
    float                    text_angle_deg;  // Estimated page skew angle in degrees
    uint32_t                 line_count;
    const QVX_AnalyzerLine*  lines;
    uint32_t                 token_count;
    const QVX_AnalyzerToken* tokens;
    uint32_t                 utf8_pool_bytes;
    const char*              utf8_pool;
    void*                    plugin_private;  // Opaque handle released in free_result()
} QVX_AnalyzerResult;

// Analyzer Virtual Function Table
typedef struct QVX_Analyzer_VTable {
    uint32_t struct_size; // sizeof(QVX_Analyzer_VTable)

    uint32_t (*get_caps)(void);

    // Preset / Model Enumeration
    uint32_t                      (*get_preset_count)(void);
    const QVX_AnalyzerPresetInfo* (*get_preset_info)(uint32_t index);

    // Context Lifecycle
    QVX_AnalyzerContext (*create_context)(const char* preset_id);
    void                (*destroy_context)(QVX_AnalyzerContext ctx);

    // Synchronous Analysis (called from host worker thread)
    int32_t (*analyze)(
        QVX_AnalyzerContext       ctx,
        const QVX_AnalyzerInput*  input,
        QVX_AnalyzerResult*       out_result
    );
    void (*free_result)(QVX_AnalyzerContext ctx, QVX_AnalyzerResult* result);

    // Optional Dynamic Parameters (may also be mirrored on QVX_PluginHeader)
    uint32_t             (*get_param_count)(void);
    const QVX_ParamDesc* (*get_param_desc)(uint32_t index);
    int32_t              (*get_param_value)(QVX_AnalyzerContext ctx, const char* param_id, float* out_val);
    int32_t              (*set_param_value)(QVX_AnalyzerContext ctx, const char* param_id, float val);
    int32_t              (*get_param_string)(QVX_AnalyzerContext ctx, const char* param_id, char* out_buf, uint32_t buf_bytes);
    int32_t              (*set_param_string)(QVX_AnalyzerContext ctx, const char* param_id, const char* utf8_val);

    // Optional Asset Descriptors
    uint32_t             (*get_asset_count)(void);
    const QVX_AssetDesc* (*get_asset_desc)(uint32_t index);

    // Optional Localization
    int32_t (*set_language)(const char* lang_code);
} QVX_Analyzer_VTable;

#ifdef __cplusplus
}
#endif
