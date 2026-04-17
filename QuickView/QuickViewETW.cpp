#include "pch.h"
#include "QuickViewETW.h"

// Define the "QuickView" ETW provider
// {YOUR-GUID-HERE-OR-AUTO-GENERATED-BY-MACRO}
// The macro automatically hashes the name "QuickView" to generate the GUID.
TRACELOGGING_DEFINE_PROVIDER(
    g_hQuickViewProvider,
    "QuickView",
    (0xa3a9c9e8, 0x1d3a, 0x4d5b, 0xa1, 0x5d, 0x24, 0x98, 0xb7, 0x3d, 0x6e, 0x5a) // Explicit GUID generated via uuidgen or powershell [guid]::NewGuid()
);

namespace QuickView {
    namespace Logging {
        void Initialize() {
            TraceLoggingRegister(g_hQuickViewProvider);
        }

        void Shutdown() {
            TraceLoggingUnregister(g_hQuickViewProvider);
        }
    }
}
