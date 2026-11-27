#pragma once
// ============================================================================
// qvx_processor.h - QuickView Universal Image Processor Interface (Pure C ABI)
// ============================================================================
// Generalized pixel/GPU image processing slot (QVX_IFACE_IMAGE_PROCESSOR).
// Unifies Super-Resolution (Upscaler), Background Matting, Restoration/Denoise,
// and GPU/CPU Color Filters under a single zero-bloat host contract.
// ============================================================================

#include "qvx_core.h"
#include <windows.h>
#include <d3d11.h>

#define QVX_PROCESSOR_INTERFACE_VERSION 0x00030000u // 3.0.0

#ifdef __cplusplus
extern "C" {
#endif

// Opaque processor context handle (owns model weights, GPU states, scratch buffers)
typedef void* QVX_ProcContext;

// High-level processing category used by host UI & dispatch policies
typedef enum QVX_ProcKind {
    QVX_PROC_KIND_UNKNOWN     = 0,
    QVX_PROC_KIND_UPSCALER    = 1, // Resolution upscaling (e.g. Real-ESRGAN)
    QVX_PROC_KIND_MATTING     = 2, // Background removal / alpha matting
    QVX_PROC_KIND_RESTORATION = 3, // Denoise / deblur / restoration
    QVX_PROC_KIND_FILTER      = 4  // Stylize / 3D LUT / color grading
} QVX_ProcKind;

// Processor capability flags
typedef enum QVX_ProcCaps {
    QVX_PROC_CAP_NONE          = 0,
    QVX_PROC_CAP_GPU_D3D11     = 1u << 0, // Supports process_gpu (ID3D11Texture2D)
    QVX_PROC_CAP_SHARED_HANDLE = 1u << 1, // Supports process_shared (DXGI HANDLE)
    QVX_PROC_CAP_CPU_FALLBACK  = 1u << 2, // Supports process_cpu (BGRA buffer)
    QVX_PROC_CAP_HDR_FP16      = 1u << 3, // Supports RGBA16F HDR textures
    QVX_PROC_CAP_AUTO_TRIGGER  = 1u << 4, // Eligible for viewer auto-trigger on small images
    QVX_PROC_CAP_COMPARE_MODE  = 1u << 5  // Supports before/after Compare Mode preview
} QVX_ProcCaps;

// Preset / Model Descriptor (replaces SR-specific model metadata)
typedef struct QVX_ProcPresetInfo {
    uint32_t    struct_size;          // sizeof(QVX_ProcPresetInfo)
    const char* preset_id;            // Unique preset/model key
    const char* display_name;         // Localized name (UTF-8)
    const char* description;          // Localized description (UTF-8)
    const char* required_asset_id;    // Links to QVX_AssetDesc::asset_id (NULL if built-in)
    float       scale_factor;         // 1.0f for filters/matting; 2.0f/3.0f/4.0f for upscalers
    bool        is_hdr_capable;       // Supports FP16 / HDR input & output
    bool        output_has_alpha;     // True if output produces meaningful alpha (e.g. matting)
    bool        default_compare_mode; // Suggest Compare Mode by default
    bool        is_dynamic_auto;      // True if preset auto-routes across scales based on zoom
    uint32_t    preferred_tile_size;  // Optional hint (0 = auto)
    uint32_t    default_debounce_ms;  // Recommended debounce delay in ms
    uint32_t    max_input_dim;        // Safety guard limit per axis (0 = host default)
    uint64_t    max_input_pixels;     // Safety guard total pixel limit (0 = host default)
} QVX_ProcPresetInfo;

// Per-invocation execution parameters
typedef struct QVX_ProcExecuteParams {
    uint32_t             struct_size; // sizeof(QVX_ProcExecuteParams)
    uint32_t             in_width;
    uint32_t             in_height;
    uint32_t             out_width;
    uint32_t             out_height;
    QVX_CancelToken      cancel;
    QVX_ProgressCallback on_progress;
    void*                progress_user_data;
} QVX_ProcExecuteParams;

// Image Processor Virtual Function Table
typedef struct QVX_Processor_VTable {
    uint32_t     struct_size; // sizeof(QVX_Processor_VTable)
    QVX_ProcKind kind;
    uint32_t     caps;        // Bitwise OR of QVX_ProcCaps

    // 1. Preset / Model Enumeration
    uint32_t                  (*get_preset_count)(void);
    const QVX_ProcPresetInfo* (*get_preset_info)(uint32_t index);

    // Optional: Compute output dimensions for a given input and stretch ratio
    void (*query_output_size)(
        const char* preset_id,
        uint32_t    in_width,
        uint32_t    in_height,
        float       viewport_stretch_ratio,
        uint32_t*   out_width,
        uint32_t*   out_height
    );

    // 2. Context Lifecycle
    QVX_ProcContext (*create_context)(ID3D11Device* pDevice, const char* preset_id);
    void            (*destroy_context)(QVX_ProcContext ctx);

    // 3. GPU VRAM Direct Processing (Primary Hot-Path)
    int32_t (*process_gpu)(
        QVX_ProcContext              ctx,
        ID3D11Texture2D*             in_tex,
        ID3D11Texture2D*             out_tex,
        const QVX_ProcExecuteParams* params
    );

    // 4. DXGI Shared Handle Processing (Optional cross-API / cross-process)
    int32_t (*process_shared)(
        QVX_ProcContext              ctx,
        HANDLE                       in_shared_handle,
        HANDLE                       out_shared_handle,
        const QVX_ProcExecuteParams* params
    );

    // 5. CPU Fallback Processing (Optional)
    int32_t (*process_cpu)(
        QVX_ProcContext              ctx,
        const uint8_t*               in_bgra,
        uint32_t                     in_stride,
        uint8_t*                     out_bgra,
        uint32_t                     out_stride,
        const QVX_ProcExecuteParams* params
    );
} QVX_Processor_VTable;

#ifdef __cplusplus
}
#endif
