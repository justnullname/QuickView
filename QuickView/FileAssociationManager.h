// QuickView File Association Center — Windows 10/11 registry engine, CLI path reconstruction, and probe API.
// Copyright (C) 2026-Present QuickView Contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include "SupportedExtensions.h"

namespace QuickView {

enum class AssocIconStyle : int {
    SystemNative = 0,
    QuickViewLogo = 1
};

enum class ExtAssocState : uint8_t {
    NotRegistered = 0,
    RegisteredOpenWith = 1,
    IsSystemDefault = 2
};

enum class AssocPreset : uint8_t {
    Recommended = 0,
    AllImages,
    PhotoAndRaw,
    ClearAll
};

struct FileAssocOptions {
    std::wstring_view selectedExtsCsv;
    AssocIconStyle iconStyle = AssocIconStyle::SystemNative;
    bool enableContextMenu = false;
    std::wstring_view contextMenuLabel = L"Open with QuickView";
};

struct FileAssocProbeSummary {
    std::array<ExtAssocState, ALL_ASSOCIABLE_EXTENSIONS.size()> states{};
    int selectedCount = 0;
    int defaultCount = 0;
    int openWithOnlyCount = 0;
    bool hasBrokenCommand = false;
};

using PathExistsFn = bool (*)(std::wstring_view path);

// ============================================================================
// Pure / Testable Helpers (Zero OS registry writes, zero exceptions)
// ============================================================================

// Reconstructs an image path from argc/argv, handling unquoted paths split on spaces
// (e.g., when a broken external caller or legacy "%1" passes a spaced path without quotes).
std::wstring ReconstructPathFromArgs(int argc, const wchar_t* const* argv, PathExistsFn existsFn = nullptr);

// Strictly validates a shell\open\command string against `"<exePath>" "%1"`.
// Rejects unquoted %1 (issue #265) or mismatched executable paths.
bool IsCommandLineTemplateValid(std::wstring_view regCommand, std::wstring_view exePath);

// Computes a deterministic FNV-1a 64-bit fingerprint over selected extensions + options.
uint64_t ComputeAssocOptionsHash(const FileAssocOptions& opts);
std::wstring FormatAssocOptionsHash(uint64_t hash);

// Extension CSV manipulation helpers (case-insensitive, deduplicated, canonical lowercase).
bool IsExtensionInCsv(std::wstring_view csv, std::wstring_view ext);
std::wstring ToggleExtensionInCsv(std::wstring_view csv, std::wstring_view ext, bool enable);
std::wstring SetCategoryInCsv(std::wstring_view csv, AssocExtCategory cat, bool enable);
int CountCategoryInCsv(std::wstring_view csv, AssocExtCategory cat);
std::wstring GetPresetCsv(AssocPreset preset);

// Static zero-allocation metadata queries.
const wchar_t* GetExtensionTypeDescription(std::wstring_view ext);
std::wstring GetExtensionNativeIcon(std::wstring_view ext, std::wstring_view sysDir);

// Determines whether Explorer context menu should cover an extension when context menu is enabled:
// always true for all 88 image/RAW formats and comic archives (.cbz/.cbr); only when selected for .zip/.rar.
bool ShouldIncludeExtInContextMenu(std::wstring_view ext, bool isSelectedInAssoc);

// Builds the official Windows Settings URI for opening Default Apps (handles Win10/11 and MSIX Store AUMID).
std::wstring BuildDefaultAppsSettingsUri(bool isWin11, bool isPackaged, std::wstring_view aumid);

// ============================================================================
// Windows 10/11 Registry & Shell Engine
// ============================================================================

// Checks whether registry state differs from the desired options or contains broken/legacy keys.
bool IsFileAssociationSyncNeeded(const FileAssocOptions& opts, std::wstring_view exePath);

// Applies delta changes to HKCU registry for all 92 supported formats and optionally the context menu.
// Cleans up legacy QuickView.Image pollution and unchecked extensions; notifies Shell only on actual edits.
bool SyncFileAssociations(const FileAssocOptions& opts, std::wstring_view exePath);

// Removes all QuickView file associations, Capabilities, and context menu entries from HKCU.
void UnregisterAllAssociations();

// On-demand read-only probe for the Settings UI (queries Windows effective default handler per extension).
FileAssocProbeSummary ProbeFileAssociations(std::wstring_view selectedExtsCsv, std::wstring_view exePath);

// Opens the official Windows 10/11 Default Apps UI targeted at QuickView.
bool OpenSystemDefaultAppsSettings();

// Reads the current executable path in Windows (up to MAX_PATH).
std::wstring GetCurrentExecutablePath();

} // namespace QuickView
