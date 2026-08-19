#pragma once
// ============================================================================
// qvx.h - QuickView Plugin Architecture Core Header (Pure C ABI)
// ============================================================================
// Design Principles:
// 1. Pure C ABI: No C++ classes, no vtables, no STL types crossing DLL boundary.
// 2. High Stability: Packed/POD structs, strict version handshake.
// 3. Extensibility: Interface capability query (QVX_InterfaceID).
// 4. Zero Overhead: Direct C function pointer dispatch (< 1us latency).
// ============================================================================

#include <stdint.h>
#include <stdbool.h>

#define QVX_ABI_VERSION 0x00020000 // 2.0.0
#define QVX_API __declspec(dllexport)

#ifdef __cplusplus
extern "C" {
#endif

// Known interface capability identifiers
typedef enum QVX_InterfaceID {
    QVX_IFACE_NONE             = 0,
    QVX_IFACE_SUPER_RESOLUTION = 1, // Super-Resolution / Neural Upscaling
    QVX_IFACE_CODEC            = 2, // Image/Animation Codec (Reserved)
    QVX_IFACE_FILTER           = 3, // Compute/Color Filter (Reserved)
    QVX_IFACE_VFS              = 4  // Virtual File System / Archive (Reserved)
} QVX_InterfaceID;

// Plugin capabilities and property flags
typedef enum QVX_PluginFlags {
    QVX_FLAG_NONE        = 0,
    QVX_FLAG_THREAD_SAFE = 1 << 0, // Plugin supports concurrent execution across threads
    QVX_FLAG_SANDBOX_PREF= 1 << 1  // Suggest running inside child process isolation
} QVX_PluginFlags;

// Generic cancellation predicate
// Return true if operation should abort immediately.
typedef bool (*QVX_CancelPredicate)(void* cancel_user_data);

// Dynamic Parameter Types supported by GeekWidgets UI
typedef enum QVX_ParamType {
    QVX_PARAM_TYPE_BOOL   = 1, // Boolean toggle switch
    QVX_PARAM_TYPE_INT    = 2, // Integer slider / stepper
    QVX_PARAM_TYPE_FLOAT  = 3, // Floating-point slider
    QVX_PARAM_TYPE_ENUM   = 4  // Single-selection dropdown / segmented list
} QVX_ParamType;

// Enumeration Option Item for QVX_PARAM_TYPE_ENUM
typedef struct QVX_ParamOption {
    int32_t value;             // Numeric option value
    const char* label;         // Display label (UTF-8)
} QVX_ParamOption;

// Dynamic Parameter Descriptor (Pure C-ABI, 64-bit aligned POD)
typedef struct QVX_ParamDesc {
    uint32_t struct_size;      // sizeof(QVX_ParamDesc) for forward compatibility
    const char* id;            // Unique parameter key (ASCII), e.g. "sharpness", "tile_size"
    const char* label;         // UI display name (UTF-8), e.g. "Sharpness"
    const char* tooltip;       // Hover tooltip (UTF-8), optional
    QVX_ParamType type;        // Parameter type

    union {
        struct {
            bool default_val;
        } bool_param;
        struct {
            int32_t min_val;
            int32_t max_val;
            int32_t step;
            int32_t default_val;
            const char* format_str; // Format string, e.g. "%d px", "%d ms"
        } int_param;
        struct {
            float min_val;
            float max_val;
            float step;
            float default_val;
            const char* format_str; // Format string, e.g. "%.2f", "%.1f"
        } float_param;
        struct {
            int32_t default_val;
            uint32_t option_count;
            const QVX_ParamOption* options; // Pointer to static array of options
        } enum_param;
    };
} QVX_ParamDesc;

// Core Plugin Header (Exported by all QVX plugins)
typedef struct QVX_PluginHeader {
    uint32_t struct_size;          // sizeof(QVX_PluginHeader) for forward compatibility
    uint32_t abi_version;          // Must match QVX_ABI_VERSION
    const char* plugin_id;         // Unique ID, e.g. "com.quickview.sr.realesrgan"
    const char* plugin_name;       // Human-readable name, e.g. "Real-ESRGAN DirectML Engine"
    const char* author;            // Plugin author / organization
    const char* version_str;       // Plugin semantic version, e.g. "1.0.0"
    uint32_t flags;                // Bitwise OR of QVX_PluginFlags
    uint32_t supported_interfaces; // Bitmask of (1 << QVX_InterfaceID)

    // Capability Query: Return interface vtable pointer matching (interface_id, version)
    // Returns NULL if interface is not supported or version is incompatible.
    const void* (*get_interface)(uint32_t interface_id, uint32_t interface_version);
} QVX_PluginHeader;

// Standard Entry Points required to be exported by every .qvx (.dll)
QVX_API bool qvx_init(const QVX_PluginHeader** out_header);
QVX_API void qvx_shutdown(void);

#ifdef __cplusplus
}
#endif

