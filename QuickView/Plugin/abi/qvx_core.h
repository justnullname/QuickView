#pragma once
// ============================================================================
// qvx_core.h - QuickView Plugin Architecture v3 Core ABI (Pure C11 / C++23)
// ============================================================================
// Design Principles:
// 1. Pure C ABI: POD structs only, no STL/exceptions/RTTI across DLL boundary.
// 2. Forward Compatibility: Every public struct begins with uint32_t struct_size.
// 3. Bidirectional Services: Host injects paths, 64B-aligned allocators, and logging.
// 4. Demand-Driven Growth: Core enumerates orthogonal slots; each slot ABI is
//    finalized only alongside a real reference plugin.
// ============================================================================

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define QVX_ABI_VERSION 0x00030000u // 3.0.0
#ifdef QVX_BUILDING_PLUGIN
#define QVX_API __declspec(dllexport)
#else
#define QVX_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Standard QVX Return Codes (HRESULT-compatible)
#define QVX_OK                 ((int32_t)0x00000000L) // S_OK
#define QVX_S_FALSE            ((int32_t)0x00000001L) // S_FALSE
#define QVX_E_FAIL             ((int32_t)0x80004005L) // E_FAIL
#define QVX_E_ABORT            ((int32_t)0x80004004L) // E_ABORT (Cancelled)
#define QVX_E_INVALIDARG       ((int32_t)0x80070057L) // E_INVALIDARG
#define QVX_E_NOTIMPL          ((int32_t)0x80004001L) // E_NOTIMPL
#define QVX_E_OUTOFMEMORY      ((int32_t)0x8007000EL) // E_OUTOFMEMORY
#define QVX_E_EOF              ((int32_t)0x80070026L) // ERROR_HANDLE_EOF
#define QVX_E_PASSWORD         ((int32_t)0x80070056L) // ERROR_INVALID_PASSWORD
#define QVX_E_MISSING_ASSET    ((int32_t)0x80070002L) // ERROR_FILE_NOT_FOUND
#define QVX_E_DEVICE_REMOVED   ((int32_t)0x887A0005L) // DXGI_ERROR_DEVICE_REMOVED

// Orthogonal plugin interface capability identifiers
typedef enum QVX_InterfaceID {
    QVX_IFACE_NONE            = 0,
    QVX_IFACE_DECODER         = 1, // Static / Multi-page / Vector-ROI Decoder
    QVX_IFACE_MEDIA_STREAM    = 2, // Reserved: Timed Media Stream
    QVX_IFACE_IMAGE_PROCESSOR = 3, // Pixel/GPU Image Processor (SR, Matting, Filter)
    QVX_IFACE_ANALYZER        = 4, // Image -> Structured Boxes/Text (OCR, Barcode)
    QVX_IFACE_INDEXER         = 5, // Persistent Library Indexer (Duplicate/Search)
    QVX_IFACE_SHELL_EXT       = 6  // OS Shell Integration (Thumbnail Provider, etc.)
} QVX_InterfaceID;

// Plugin capability and execution flags
typedef enum QVX_PluginFlags {
    QVX_FLAG_NONE         = 0,
    QVX_FLAG_THREAD_SAFE  = 1u << 0, // Supports concurrent calls across threads
    QVX_FLAG_SANDBOX_PREF = 1u << 1, // Prefer out-of-process worker isolation
    QVX_FLAG_SANDBOX_ONLY = 1u << 2  // Requires out-of-process worker isolation
} QVX_PluginFlags;

// Log severity levels passed to QVX_HostServices::log
typedef enum QVX_LogLevel {
    QVX_LOG_DEBUG = 0,
    QVX_LOG_INFO  = 1,
    QVX_LOG_WARN  = 2,
    QVX_LOG_ERROR = 3
} QVX_LogLevel;

// Generic pixel format enum shared across plugin slots
typedef enum QVX_PixelFormat {
    QVX_PIXFMT_UNKNOWN  = 0,
    QVX_PIXFMT_BGRA8888 = 1, // DXGI_FORMAT_B8G8R8A8_UNORM (Preferred)
    QVX_PIXFMT_BGRX8888 = 2, // Opaque BGRA8
    QVX_PIXFMT_RGBA8888 = 3,
    QVX_PIXFMT_RGBA16F  = 4, // DXGI_FORMAT_R16G16B16A16_FLOAT
    QVX_PIXFMT_RGBA32F  = 5, // DXGI_FORMAT_R32G32B32A32_FLOAT
    QVX_PIXFMT_NV12     = 6,
    QVX_PIXFMT_P010     = 7
} QVX_PixelFormat;

// Generic cancellation predicate: return true to abort immediately.
typedef bool (*QVX_CancelPredicate)(void* cancel_user_data);

// Reusable cancellation token passed into long-running plugin calls
typedef struct QVX_CancelToken {
    QVX_CancelPredicate check_cancel;
    void*               user_data;
} QVX_CancelToken;

// Generic progress callback: progress in [0.0f, 1.0f]
typedef void (*QVX_ProgressCallback)(float progress, void* progress_user_data);

// Dynamic Parameter Types rendered automatically by host Settings UI
typedef enum QVX_ParamType {
    QVX_PARAM_TYPE_BOOL   = 1, // Toggle switch
    QVX_PARAM_TYPE_INT    = 2, // Integer slider / stepper
    QVX_PARAM_TYPE_FLOAT  = 3, // Floating-point slider
    QVX_PARAM_TYPE_ENUM   = 4, // Single-selection dropdown
    QVX_PARAM_TYPE_STRING = 5  // UTF-8 text input
} QVX_ParamType;

// Option item for QVX_PARAM_TYPE_ENUM
typedef struct QVX_ParamOption {
    int32_t     value;
    const char* label; // UTF-8
} QVX_ParamOption;

// Universal Dynamic Parameter Descriptor
typedef struct QVX_ParamDesc {
    uint32_t      struct_size;        // sizeof(QVX_ParamDesc)
    const char*   id;                 // Unique ASCII parameter key
    const char*   label;              // Localized UI label (UTF-8)
    const char*   tooltip;            // Optional hover tooltip (UTF-8)
    const char*   group;              // Optional UI sub-group header (UTF-8)
    const char*   visible_for_preset; // Optional comma-separated preset IDs (NULL = always visible)
    QVX_ParamType type;

    union {
        struct {
            bool default_val;
        } bool_param;
        struct {
            int32_t     min_val;
            int32_t     max_val;
            int32_t     step;
            int32_t     default_val;
            const char* format_str; // e.g. "%d px"
        } int_param;
        struct {
            float       min_val;
            float       max_val;
            float       step;
            float       default_val;
            const char* format_str; // e.g. "%.2f"
        } float_param;
        struct {
            int32_t                default_val;
            uint32_t               option_count;
            const QVX_ParamOption* options;
        } enum_param;
        struct {
            const char* default_val;
            uint32_t    max_len;
        } string_param;
    };
} QVX_ParamDesc;

// Universal Downloadable Asset Descriptor (Model weights, OCR dicts, font packs)
typedef struct QVX_AssetDesc {
    uint32_t    struct_size;    // sizeof(QVX_AssetDesc)
    const char* asset_id;       // Unique asset key within this plugin
    const char* display_name;   // Localized display name (UTF-8)
    const char* description;    // Localized description (UTF-8)
    const char* extract_subdir; // Relative subdir under host data_dir, e.g. "models"
    const char* download_url;   // HTTPS ZIP URL (or '\n'-separated multi-ZIP URLs)
    const char* sha256;         // Optional hex SHA-256 (empty/NULL if omitted)
    uint64_t    file_size_bytes;
    bool        is_installed;   // Checked by plugin against host->get_data_dir() / plugin_dir
    bool        is_required;    // True if asset is mandatory for the plugin/preset
} QVX_AssetDesc;

// Host-to-Plugin Service Table (injected during qvx_init)
typedef struct QVX_HostServices {
    uint32_t struct_size; // sizeof(QVX_HostServices)
    uint32_t abi_version; // QVX_ABI_VERSION

    // Normalized UTF-8 directory paths (MSIX & portable safe)
    const char* (*get_plugin_dir)(void* host_ctx); // Directory containing this .qvx
    const char* (*get_data_dir)(void* host_ctx);   // Writable directory for assets/db/cache

    // Zero-copy aligned buffer allocation (directly adoptable by host RawImageFrame)
    uint8_t* (*alloc_buffer)(void* host_ctx, size_t size_bytes, size_t alignment);
    void     (*free_buffer)(void* host_ctx, uint8_t* ptr);

    // Diagnostics logger
    void (*log)(void* host_ctx, int32_t level, const char* utf8_msg);

    // Reserved for Indexer plugins: thread-safe, low-priority host thumbnail decode (may be NULL)
    int32_t (*decode_thumbnail)(
        void*       host_ctx,
        const char* file_path_utf8,
        uint32_t    max_edge,
        uint8_t**   out_bgra_pixels,
        uint32_t*   out_width,
        uint32_t*   out_height,
        uint32_t*   out_stride
    );

    void* host_ctx; // Opaque context passed back to every callback above
} QVX_HostServices;

// Core Plugin Header exported by every QVX v3 plugin
typedef struct QVX_PluginHeader {
    uint32_t    struct_size;          // sizeof(QVX_PluginHeader)
    uint32_t    abi_version;          // Must match QVX_ABI_VERSION
    const char* plugin_id;            // Reverse-DNS ID, e.g. "com.quickview.sr.ncnn_vulkan"
    const char* plugin_name;          // Fallback human-readable name (UTF-8)
    const char* author;               // Author / organization (UTF-8)
    const char* version_str;          // SemVer string, e.g. "0.3.0"
    uint32_t    flags;                // Bitwise OR of QVX_PluginFlags
    uint32_t    supported_interfaces; // Bitmask of (1u << QVX_InterfaceID)

    // Interface vtable lookup by (interface_id, interface_version)
    const void* (*get_interface)(uint32_t interface_id, uint32_t interface_version);

    // Optional common services shared across all plugin types (may be NULL)
    int32_t (*set_language)(const char* bcp47_lang);

    uint32_t             (*get_param_count)(void);
    const QVX_ParamDesc* (*get_param_desc)(uint32_t index);
    int32_t              (*set_param_number)(const char* param_id, float value);
    int32_t              (*set_param_string)(const char* param_id, const char* utf8_value);

    uint32_t             (*get_asset_count)(void);
    const QVX_AssetDesc* (*get_asset_desc)(uint32_t index);
    void                 (*notify_assets_changed)(void);
} QVX_PluginHeader;

// Standard entry points exported by every .qvx binary
QVX_API bool qvx_init(const QVX_HostServices* host, const QVX_PluginHeader** out_header);
QVX_API void qvx_shutdown(void);

#ifdef __cplusplus
}
#endif
