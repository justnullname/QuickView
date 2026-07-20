/*
 * QuickView - Modern D2D Print Engine
 * Copyright (C) 2026-Present QuickView Contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "pch.h"
#include <string>
#include <vector>
#include <expected>
#include <set>
#include <atomic>
#include <d2d1_1.h>
#include <windows.h>
#include <winspool.h>

namespace QuickView {

enum class PrintLayoutMode {
    Fit,      // 适应纸张 (等比缩放至最大可见)
    Fill,     // 裁剪填充 (等比缩放至填满纸张)
    Original, // 原始比例 (居中，可能超出纸张)
    Custom    // 自定义缩放比例
};

enum class PrintAlignment {
    TopLeft, TopCenter, TopRight,
    CenterLeft, Center, CenterRight,
    BottomLeft, BottomCenter, BottomRight
};

struct PrintJobSettings {
    std::wstring printerName;
    std::wstring imagePath;
    
    int16_t copies = 1;
    bool isLandscape = false;
    bool grayscale = false;
    int rotationAngle = 0; // 0, 90, 180, 270 (user rotation on top of EXIF)
    float customScale = 1.0f; // 1.0 = 100%
    
    // 纸张相关, 取决于打印机的支持情况 (可通过 DEVMODE 间接传递)
    short paperSize = 0; // 0 means use DEVMODE default
    
    // 排版控制
    PrintLayoutMode layoutMode = PrintLayoutMode::Fit;
    PrintAlignment alignment = PrintAlignment::Center;
    
    // 边距调整 (单位: 毫米 mm)
    float marginLeft = 10.0f; 
    float marginTop = 10.0f;
    float marginRight = 10.0f;
    float marginBottom = 10.0f;
    float paperWidthMm = 210.0f;  // A4 default width
    float paperHeightMm = 297.0f; // A4 default height

    // 多页选择
    bool printMultiPage = false; 
    std::set<int> disabledPages;

    // 驱动持久化数据 (保存用户在高级属性里选择的非标纸张、无边距等状态)
    std::vector<uint8_t> devModeData;

    // Layout-locked source geometry (sensor/decode pixel space — NOT display surface).
    // Preview and ExecutePrintJob must share these so page counts match.
    float sourceWidthPx = 0.0f;
    float sourceHeightPx = 0.0f;
    double imageDpiX = 96.0;
    double imageDpiY = 96.0;
    // EXIF orientation 1-8 applied when drawing; 1 = identity / AutoRotate off.
    int exifOrientation = 1;
};

struct PrinterInfo {
    std::wstring name;
    bool isDefault = false;
};

class PrintManager {
public:
    static PrintManager& GetInstance() {
        static PrintManager instance;
        return instance;
    }

    /// <summary>
    /// 获取本地所有可用的打印机列表。
    /// </summary>
    std::vector<PrinterInfo> EnumLocalPrinters() const;

    /// <summary>
    /// 获取指定打印机支持的纸张尺寸等信息 (预留扩展接口)
    /// </summary>
    bool GetPrinterCapabilities(const std::wstring& printerName);

    /// <summary>
    /// 唤起指定打印机特有的高级属性对话框，并持久化 DEVMODE 数据
    /// </summary>
    bool ShowPrinterProperties(HWND hwndOwner, const std::wstring& printerName, std::vector<uint8_t>& inOutDevModeData);

    /// <summary>
    /// 后台执行打印任务 (非阻塞或内部开线程)
    /// </summary>
    std::expected<void, HRESULT> ExecutePrintJob(const PrintJobSettings& settings);

    struct PrintDeviceMetrics {
        float dpiX;
        float dpiY;
        float physicalWidthMm;    // Physical paper width (e.g. 210 for A4)
        float physicalHeightMm;   // Physical paper height (e.g. 297 for A4)
        float printableWidthPx;   // HORZRES
        float printableHeightPx;  // VERTRES
        float physicalOffsetX_Px; // PHYSICALOFFSETX
        float physicalOffsetY_Px; // PHYSICALOFFSETY
    };

    /// <summary>
    /// Shared DEVMODE-aware device metrics query for preview (CreateIC) and print (CreateDC).
    /// When outDc is non-null, creates a real printer DC (caller owns via DeleteDC).
    /// When outDc is null, uses an information context only.
    /// </summary>
    static bool QueryPrintDeviceMetrics(
        const PrintJobSettings& settings,
        PrintDeviceMetrics& outMetrics,
        HDC* outDc = nullptr
    );

    /// <summary>
    /// EXIF orientation matrix (sensor pixels centered at origin → oriented space).
    /// Matches the GPU pre-rotation used by the main viewer.
    /// </summary>
    static D2D1::Matrix3x2F BuildExifOrientationMatrix(int orientation);

    /// <summary>
    /// Oriented layout size after EXIF (before user rotationAngle).
    /// </summary>
    static D2D1_SIZE_F GetOrientedSourceSize(float sourceWidthPx, float sourceHeightPx, int exifOrientation);

    /// <summary>
    /// 用于在屏幕和打印机共用的排版矩阵计算方法 (基于绝对毫米物理尺寸域计算)
    /// imageSize is sensor/decode pixel size (unoriented). EXIF + user rotation are applied inside.
    /// DPI is taken from settings.imageDpiX/Y when positive.
    /// </summary>
    static D2D1::Matrix3x2F CalculatePrintTransform(
        const PrintDeviceMetrics& metrics, 
        const D2D1_SIZE_F& imageSize, 
        const PrintJobSettings& settings, 
        int* outCols = nullptr, 
        int* outRows = nullptr,
        D2D1_RECT_F* outMarginRectPx = nullptr,
        float* outPageWPx = nullptr,
        float* outPageHPx = nullptr
    );

    bool HasActiveJobs() const { return m_activeJobs.load() > 0; }
    void IncrementActiveJobs() { m_activeJobs++; }
    void DecrementActiveJobs() { m_activeJobs--; }

private:
    PrintManager() = default;
    ~PrintManager() = default;

    PrintManager(const PrintManager&) = delete;
    PrintManager& operator=(const PrintManager&) = delete;

    std::atomic<int> m_activeJobs{0};

    static bool BuildConfiguredDevMode(
        const PrintJobSettings& settings,
        std::vector<uint8_t>& outBuf,
        DEVMODEW** outDevMode
    );

    static PrintDeviceMetrics MetricsFromHdc(HDC hdc);

    HRESULT CreatePrinterColorContext(
        ID2D1DeviceContext* d2dContext,
        HDC printerDC,
        const std::wstring& customPrinterIcc,
        ID2D1ColorContext** outColorContext
    );
};

} // namespace QuickView
