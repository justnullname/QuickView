#pragma once
// ============================================================================
// qvx_shell_ext.h - QuickView Plugin Architecture v3 Shell Extension ABI
// ============================================================================
// Pure C11 / C++23 POD interface for OS Shell integration (e.g. IThumbnailProvider).
// Uses bidirectional host callbacks so in-process activations can look up
// hot-decoded gallery thumbnails directly without extra IPC round-trips.
// ============================================================================

#include "qvx_core.h"

#define QVX_SHELL_EXT_INTERFACE_VERSION 0x00030000u

#ifdef __cplusplus
extern "C" {
#endif

typedef enum QVX_ShellRegState {
    QVX_SHELL_REG_UNREGISTERED    = 0,
    QVX_SHELL_REG_REGISTERED      = 1,
    QVX_SHELL_REG_NEEDS_REPAIR    = 2,
    QVX_SHELL_REG_MANAGED_BY_MSIX = 3
} QVX_ShellRegState;

// Zero-IPC in-process hot-cache lookup invoked when the COM extension runs
// inside QuickView.exe (e.g. during background thumbcache warm-up).
// Implementor MUST enforce exact (path, size, mtime) match and maxEdge >= requested_cx.
typedef bool (*QVX_InProcThumbLookupFn)(
    void*       user_data,
    const char* canonical_path_utf8,
    uint64_t    file_size,
    uint64_t    file_mtime_100ns,
    uint32_t    requested_cx,
    uint8_t*    out_bgra_buf,
    uint32_t    max_out_bytes,
    uint32_t*   out_width,
    uint32_t*   out_height,
    uint32_t*   out_stride,
    uint8_t*    out_is_premultiplied
);

typedef struct QVX_ShellExt_VTable {
    uint32_t struct_size;       // sizeof(QVX_ShellExt_VTable)
    uint32_t interface_version; // QVX_SHELL_EXT_INTERFACE_VERSION

    // Query HKCU / MSIX registration status for the given host executable path.
    int32_t (*get_registration_state)(const char* host_exe_utf8, int32_t* out_state);

    // Register / sync enabled formats under HKCU (zero UAC), politely backing up
    // any pre-existing third-party CLSIDs before writing QuickView's handler.
    int32_t (*register_shell)(const char* host_exe_utf8);

    // Remove QuickView's HKCU registrations and restore any backed-up third-party CLSIDs.
    int32_t (*unregister_shell)(void);

    // Broadcast shell association / thumbnail cache refresh notification.
    int32_t (*refresh_shell_cache)(void);

    // Query whether gallery warm-up should persist thumbnails for this extension (e.g. ".jxl").
    bool (*should_warmup_extension)(const char* ext_utf8);

    // Register / clear in-process hot-cache lookup callback (pass NULL to clear on shutdown).
    void (*set_inproc_thumb_provider)(QVX_InProcThumbLookupFn fn, void* user_data);
} QVX_ShellExt_VTable;

#ifdef __cplusplus
}
#endif
