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
#include <d2d1_1.h>
#include <documenttarget.h>
#include <wrl/client.h>

namespace QuickView {

class PrintManager {
public:
    static PrintManager& GetInstance() {
        static PrintManager instance;
        return instance;
    }

    /// <summary>
    /// Prompts the modern system print dialog and executes D2D-based printing
    /// with color proofing and memory defenses.
    /// </summary>
    /// <param name="hwndOwner">Owner window handle</param>
    /// <param name="imagePath">Absolute path to the image file to print</param>
    /// <param name="customPrinterIcc">Optional path to a custom printer ICC profile</param>
    /// <returns>S_OK on success, or HRESULT error code</returns>
    HRESULT PrintImage(HWND hwndOwner, const std::wstring& imagePath, const std::wstring& customPrinterIcc = L"");

private:
    PrintManager() = default;
    ~PrintManager() = default;

    PrintManager(const PrintManager&) = delete;
    PrintManager& operator=(const PrintManager&) = delete;
    
    HRESULT CreatePrinterColorContext(
        ID2D1DeviceContext* d2dContext,
        HDC printerDC,
        const std::wstring& customPrinterIcc,
        ID2D1ColorContext** outColorContext
    );
};

} // namespace QuickView
