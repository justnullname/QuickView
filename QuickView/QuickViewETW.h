#pragma once
#include <windows.h>
#include <TraceLoggingProvider.h>
#include <concepts>
#include <string_view>

// {A7F4B3C5-1234-4A3D-B7E9-D7F607248C5B} - 示例 GUID，实际将使用基于名称的哈希或静态生成
TRACELOGGING_DECLARE_PROVIDER(g_hQuickViewProvider);

namespace QuickView::Logging {
    void Initialize();
    void Shutdown();
}

// 定义日志级别映射
#define QV_LOG_LEVEL_CRITICAL    1
#define QV_LOG_LEVEL_ERROR       2
#define QV_LOG_LEVEL_WARNING     3
#define QV_LOG_LEVEL_INFO        4
#define QV_LOG_LEVEL_VERBOSE     5

// 核心宏：极致性能路径，利用模板和 C++23 静态特性
#define QV_LOG(level, name, ...) \
    TraceLoggingWrite(g_hQuickViewProvider, name, \
        TraceLoggingLevel(level), \
        __VA_ARGS__)

// 快慢路径宏封装
#define QV_LOG_INFO(msg)    QV_LOG(QV_LOG_LEVEL_INFO, "Info", TraceLoggingWideString(msg, "Message"))
#define QV_LOG_WARN(msg)    QV_LOG(QV_LOG_LEVEL_WARNING, "Warning", TraceLoggingWideString(msg, "Message"))
#define QV_LOG_ERR(msg)     QV_LOG(QV_LOG_LEVEL_ERROR, "Error", TraceLoggingWideString(msg, "Message"))
#define QV_LOG_VERBOSE(msg) QV_LOG(QV_LOG_LEVEL_VERBOSE, "Verbose", TraceLoggingWideString(msg, "Message"))
