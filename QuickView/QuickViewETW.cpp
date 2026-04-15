#include "pch.h"
#include "QuickViewETW.h"

// 使用固定名称，系统会自动生成稳定的 GUID (Microsoft.QuickView)
TRACELOGGING_DEFINE_PROVIDER(
    g_hQuickViewProvider,
    "Microsoft.QuickView",
    // {9E9234A5-0D4A-5A7D-A7F4-D7F607248C5B}
    (0x9e9234a5, 0x0d4a, 0x5a7d, 0xa7, 0xf4, 0xd7, 0xf6, 0x07, 0x24, 0x8c, 0x5b));

namespace QuickView::Logging {
    void Initialize() {
        TraceLoggingRegister(g_hQuickViewProvider);
    }
    void Shutdown() {
        TraceLoggingUnregister(g_hQuickViewProvider);
    }
}
