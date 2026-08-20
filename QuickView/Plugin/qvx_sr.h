#pragma once
// ============================================================================
// qvx_sr.h - QuickView Super-Resolution & Neural Upscaling Interface (Pure C ABI)
// ============================================================================
// Design Principles:
// 1. GPU VRAM Direct: D3D11 Texture2D & DXGI Shared Handle 0-copy interop.
// 2. Cancellation: Millisecond-level abort polling via QVX_CancelPredicate.
// 3. Thread-Safe: Worker context isolation via ID3D11Device creation.
// 4. Zero Alloc: Reuse pre-warmed sessions and GPU constant buffers.
// ============================================================================

#include "qvx.h"
#include <windows.h>
#include <d3d11.h>

#define QVX_SR_INTERFACE_VERSION 0x00020100 // 2.1.0

// Official Built-in / Target Super-Resolution Plugin Version Handshake
#define QVX_OFFICIAL_SR_PLUGIN_VERSION "2.1.0"
#define QVX_OFFICIAL_SR_PLUGIN_VERSION_INT 0x00020100

#ifdef __cplusplus
extern "C" {
#endif

// Standard QVX Return Codes (HRESULT-compatible)
#define QVX_OK                 ((int32_t)0x00000000L) // S_OK
#define QVX_E_FAIL             ((int32_t)0x80004005L) // E_FAIL
#define QVX_E_ABORT            ((int32_t)0x80004004L) // E_ABORT (Cancelled by user)
#define QVX_E_INVALIDARG       ((int32_t)0x80070057L) // E_INVALIDARG
#define QVX_E_NOTIMPL          ((int32_t)0x80004001L) // E_NOTIMPL
#define QVX_E_OUTOFMEMORY      ((int32_t)0x8007000EL) // E_OUTOFMEMORY
#define QVX_E_DEVICE_REMOVED   ((int32_t)0x887A0005L) // DXGI_ERROR_DEVICE_REMOVED

// Super-Resolution Model Metadata Description
typedef struct QVX_SR_ModelInfo {
    uint32_t struct_size;         // sizeof(QVX_SR_ModelInfo)
    const char* model_id;         // e.g. "realesr-animevideov3-x2"
    const char* display_name;     // e.g. "Anime Fast 2x" (Localizable UTF-8)
    const char* description;      // e.g. "Ultra-fast anime & illustration upscaler, lightweight VRAM" (Localizable UTF-8)
    float scale;                  // e.g. 2.0f, 4.0f
    bool is_hdr_capable;          // Supports FP16 / HDR input & output
    bool is_installed;            // True if model file/weights ready locally
    uint64_t file_size_bytes;     // Size in bytes, 0 if embedded
    const char* download_url;     // Direct download URL if not installed
    uint32_t preferred_tile_size; // Recommended tile dimension (e.g. 512, 0 = full frame)
    uint32_t default_debounce_ms; // Recommended debounce delay in ms
    bool default_compare_mode;    // Recommend compare mode by default
} QVX_SR_ModelInfo;

// Execution Parameters for Upscale Call
typedef struct QVX_SR_ExecuteParams {
    uint32_t in_width;
    uint32_t in_height;
    uint32_t out_width;
    uint32_t out_height;

    // Cancellation Predicate: Polled between tiles or convolution passes.
    // If returns true, plugin must abort immediately and return QVX_E_ABORT.
    QVX_CancelPredicate check_cancel;
    void* cancel_user_data;

    // Optional visual tuning parameters (0.0f = engine default)
    float sharpness;              // Contrast-adaptive sharpening strength [0.0 - 1.0]
    float denoise;                // Pre-denoise strength [0.0 - 1.0]
} QVX_SR_ExecuteParams;

// Opaque context handle holding model weights, GPU pipeline state, and scratch buffers
typedef void* QVX_SR_Context;

// Super-Resolution Virtual Function Table
typedef struct QVX_SR_VTable {
    uint32_t struct_size; // sizeof(QVX_SR_VTable)

    // 1. Model Enumeration
    uint32_t (*get_model_count)(void);
    const QVX_SR_ModelInfo* (*get_model_info)(uint32_t index);

    // 2. Context Lifecycle
    // @param pDevice: Host D3D11 device. Plugin may use it to create worker context or allocate VRAM.
    // @param model_id: Target model ID string from get_model_info. NULL for default model.
    QVX_SR_Context (*create_context)(ID3D11Device* pDevice, const char* model_id);
    void (*destroy_context)(QVX_SR_Context ctx);

    // 3. GPU VRAM Direct Upscaling (Primary Hot-Path, 0-Copy)
    // @param in_tex: Input D3D11 Texture2D (BGRA8 or FP16)
    // @param out_tex: Output D3D11 Texture2D allocated by host (matched to out_width x out_height)
    int32_t (*upscale_gpu)(
        QVX_SR_Context ctx,
        ID3D11Texture2D* in_tex,
        ID3D11Texture2D* out_tex,
        const QVX_SR_ExecuteParams* params
    );

    // 4. DXGI Shared Handle Direct Upscaling (Cross-API / Cross-Process 0-Copy)
    // @param in_shared_handle: DXGI shared handle for input resource
    // @param out_shared_handle: DXGI shared handle for output resource
    int32_t (*upscale_shared)(
        QVX_SR_Context ctx,
        HANDLE in_shared_handle,
        HANDLE out_shared_handle,
        const QVX_SR_ExecuteParams* params
    );

    // 5. CPU Fallback Upscaling (Optional fallback when GPU unavailable)
    int32_t (*upscale_cpu)(
        QVX_SR_Context ctx,
        const uint8_t* in_bgra,
        uint32_t in_stride,
        uint8_t* out_bgra,
        uint32_t out_stride,
        const QVX_SR_ExecuteParams* params
    );

    // 6. Dynamic Parameter Manifest & Live Control (Pure C ABI)
    uint32_t (*get_param_count)(void);
    const QVX_ParamDesc* (*get_param_desc)(uint32_t index);
    int32_t (*get_param_value)(QVX_SR_Context ctx, const char* param_id, float* out_val);
    int32_t (*set_param_value)(QVX_SR_Context ctx, const char* param_id, float val);

    // 7. Multi-language Localization (Pure C ABI, zero host bloat)
    // @param lang_code: ISO 639-1 / BCP-47 tag, e.g. "zh-CN", "zh-TW", "en-US", "ja-JP", "de-DE", "es-ES", "fr-FR"
    int32_t (*set_language)(const char* lang_code);
} QVX_SR_VTable;

#ifdef __cplusplus
}
#endif
