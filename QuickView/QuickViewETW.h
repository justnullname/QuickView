#pragma once

#include <windows.h>
#include <TraceLoggingProvider.h>
#include "EditState.h" // For AppConfig g_config

// Declare the ETW provider
TRACELOGGING_DECLARE_PROVIDER(g_hQuickViewProvider);

namespace QuickView {
    namespace Logging {
        void Initialize();
        void Shutdown();
    }
}

// RAII scope for managing ETW lifecycle in main
struct EtwScope {
    EtwScope() { QuickView::Logging::Initialize(); }
    ~EtwScope() { QuickView::Logging::Shutdown(); }
};

// Zero-overhead logging macro.
// Short-circuits completely if EnableDebugFeatures is false or the provider isn't listening.
extern AppConfig g_config;

// Every .cpp file that uses QV_LOG must define:
//   static constexpr const char* CURRENT_MODULE = "ModuleName";
// The macro auto-injects it as a structured "Module" field for WPA grouping.
#define QV_LOG(EventName, ...) \
    do { \
        if (g_config.EnableDebugFeatures && TraceLoggingProviderEnabled(g_hQuickViewProvider, 0, 0)) { \
            TraceLoggingWrite(g_hQuickViewProvider, EventName, \
                TraceLoggingString(CURRENT_MODULE, "Module"), __VA_ARGS__); \
        } \
    } while(0)
