// QuickView File Association Center — Unit Tests for CLI Path Reconstruction, Extension Metadata, Presets, and Command Validation.
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

#include <gtest/gtest.h>
#include "FileAssociationManager.h"
#include "SupportedExtensions.h"

#include <set>
#include <string>

using namespace QuickView;

namespace {

bool MockPathExistsSpacedPhoto(std::wstring_view path) {
    return path == L"D:\\My Vacation Photos\\Summer 2026\\IMG 0042.JPG" ||
           path == L"C:\\Simple\\test.png";
}

bool MockPathNeverExists(std::wstring_view) {
    return false;
}

} // namespace

TEST(FileAssociationTest, ReconstructPathHandlesQuotedSpacedPath) {
    const wchar_t* argv[] = {
        L"C:\\Program Files\\QuickView\\QuickView.exe",
        L"D:\\My Vacation Photos\\Summer 2026\\IMG 0042.JPG"
    };
    EXPECT_EQ(
        ReconstructPathFromArgs(2, argv, &MockPathExistsSpacedPhoto),
        L"D:\\My Vacation Photos\\Summer 2026\\IMG 0042.JPG");
}

TEST(FileAssociationTest, ReconstructPathFixesIssue265UnquotedSplitArgs) {
    // Regression test for issue #265: external/legacy shell commands with unquoted %1
    // cause Windows CreateProcess to split a path with spaces across multiple argv tokens.
    const wchar_t* argv[] = {
        L"C:\\Program Files\\QuickView\\QuickView.exe",
        L"D:\\My",
        L"Vacation",
        L"Photos\\Summer",
        L"2026\\IMG",
        L"0042.JPG"
    };
    EXPECT_EQ(
        ReconstructPathFromArgs(6, argv, &MockPathExistsSpacedPhoto),
        L"D:\\My Vacation Photos\\Summer 2026\\IMG 0042.JPG");
}

TEST(FileAssociationTest, ReconstructPathSkipsKnownFlagsBeforeSpacedPath) {
    const wchar_t* argv[] = {
        L"QuickView.exe",
        L"--fg-caller",
        L"123456",
        L"D:\\My",
        L"Vacation",
        L"Photos\\Summer",
        L"2026\\IMG",
        L"0042.JPG"
    };
    EXPECT_EQ(
        ReconstructPathFromArgs(8, argv, &MockPathExistsSpacedPhoto),
        L"D:\\My Vacation Photos\\Summer 2026\\IMG 0042.JPG");
}

TEST(FileAssociationTest, ReconstructPathSkipsCleanupPidOnSelfUpdateRestart) {
    const wchar_t* argvOnlyPid[] = {
        L"QuickView.exe",
        L"--cleanup-pid",
        L"42912"
    };
    EXPECT_TRUE(ReconstructPathFromArgs(3, argvOnlyPid, &MockPathNeverExists).empty());

    const wchar_t* argvWithImage[] = {
        L"QuickView.exe",
        L"--cleanup-pid",
        L"42912",
        L"D:\\My Vacation Photos\\Summer 2026\\IMG 0042.JPG"
    };
    EXPECT_EQ(
        ReconstructPathFromArgs(4, argvWithImage, &MockPathExistsSpacedPhoto),
        L"D:\\My Vacation Photos\\Summer 2026\\IMG 0042.JPG");
}

TEST(FileAssociationTest, ReconstructPathViewerChildModeReturnsExactNextArg) {
    const wchar_t* argv[] = {
        L"QuickView.exe",
        L"--viewer-child",
        L"Z:\\NonExistent\\Target File.png"
    };
    EXPECT_EQ(
        ReconstructPathFromArgs(3, argv, &MockPathNeverExists),
        L"Z:\\NonExistent\\Target File.png");
}

TEST(FileAssociationTest, ReconstructPathFallbackWhenMissingFile) {
    const wchar_t* argv[] = {
        L"QuickView.exe",
        L"C:\\Missing",
        L"Folder\\photo.jpg"
    };
    // Falls back cleanly to the first non-flag token when no joined path exists on disk
    EXPECT_EQ(
        ReconstructPathFromArgs(3, argv, &MockPathNeverExists),
        L"C:\\Missing");
}

TEST(FileAssociationTest, ReconstructPathEmptyWhenNoFileArgs) {
    const wchar_t* argv1[] = { L"QuickView.exe" };
    EXPECT_TRUE(ReconstructPathFromArgs(1, argv1, &MockPathNeverExists).empty());

    const wchar_t* argv2[] = { L"QuickView.exe", L"--fg-caller", L"999" };
    EXPECT_TRUE(ReconstructPathFromArgs(3, argv2, &MockPathNeverExists).empty());
}

TEST(FileAssociationTest, CommandLineValidationDetectsIssue265UnquotedPercentOne) {
    const std::wstring exe = L"C:\\Program Files\\QuickView\\QuickView.exe";
    EXPECT_TRUE(IsCommandLineTemplateValid(L"\"C:\\Program Files\\QuickView\\QuickView.exe\" \"%1\"", exe));
    EXPECT_TRUE(IsCommandLineTemplateValid(L"  \"c:\\program files\\quickview\\quickview.exe\" \"%1\"  ", exe));

    // Must reject unquoted %1 (issue #265)
    EXPECT_FALSE(IsCommandLineTemplateValid(L"\"C:\\Program Files\\QuickView\\QuickView.exe\" %1", exe));
    // Must reject unquoted executable path or mismatched path
    EXPECT_FALSE(IsCommandLineTemplateValid(L"C:\\Program Files\\QuickView\\QuickView.exe \"%1\"", exe));
    EXPECT_FALSE(IsCommandLineTemplateValid(L"\"C:\\Old\\QuickView.exe\" \"%1\"", exe));
    EXPECT_FALSE(IsCommandLineTemplateValid(L"", exe));
}

TEST(FileAssociationTest, All95SupportedExtensionsHaveValidCategoryMetadataAndIcon) {
    constexpr size_t kExpectedTotal = 95;
    ASSERT_EQ(ALL_ASSOCIABLE_EXTENSIONS.size(), kExpectedTotal);

    size_t categorySum = 0;
    std::set<std::wstring> covered;
    for (size_t ci = 0; ci < static_cast<size_t>(AssocExtCategory::Count); ++ci) {
        const auto cat = static_cast<AssocExtCategory>(ci);
        const auto exts = GetExtensionsByCategory(cat);
        EXPECT_FALSE(exts.empty());
        categorySum += exts.size();
        for (auto ext : exts) {
            EXPECT_TRUE(covered.insert(std::wstring(ext)).second) << "Duplicate category assignment";
            EXPECT_TRUE(IsAssociableExtension(ext));
            EXPECT_EQ(GetCategoryOfExtension(ext), cat);

            const wchar_t* desc = GetExtensionTypeDescription(ext);
            ASSERT_NE(desc, nullptr);
            EXPECT_GT(wcslen(desc), 2u);

            const std::wstring icon = GetExtensionNativeIcon(ext, L"C:\\Windows\\System32");
            EXPECT_FALSE(icon.empty());
        }
    }
    EXPECT_EQ(categorySum, kExpectedTotal);
    EXPECT_EQ(covered.size(), kExpectedTotal);
}

TEST(FileAssociationTest, RecommendedPresetNeverIncludesArchives) {
    const std::wstring rec = GetPresetCsv(AssocPreset::Recommended);
    EXPECT_FALSE(rec.empty());
    EXPECT_TRUE(IsExtensionInCsv(rec, L".jpg"));
    EXPECT_TRUE(IsExtensionInCsv(rec, L".PNG")); // case-insensitive
    EXPECT_TRUE(IsExtensionInCsv(rec, L".webp"));
    EXPECT_TRUE(IsExtensionInCsv(rec, L".avif"));
    EXPECT_TRUE(IsExtensionInCsv(rec, L".jxl"));
    EXPECT_TRUE(IsExtensionInCsv(rec, L".heic"));
    // Never hijack compression archives or comic archives by default
    EXPECT_FALSE(IsExtensionInCsv(rec, L".zip"));
    EXPECT_FALSE(IsExtensionInCsv(rec, L".rar"));
    EXPECT_FALSE(IsExtensionInCsv(rec, L".cbz"));
    EXPECT_FALSE(IsExtensionInCsv(rec, L".cbr"));

    const std::wstring allImg = GetPresetCsv(AssocPreset::AllImages);
    EXPECT_TRUE(IsExtensionInCsv(allImg, L".cr3"));
    EXPECT_TRUE(IsExtensionInCsv(allImg, L".braw"));
    EXPECT_FALSE(IsExtensionInCsv(allImg, L".zip"));
    EXPECT_FALSE(IsExtensionInCsv(allImg, L".rar"));

    const std::wstring photoRaw = GetPresetCsv(AssocPreset::PhotoAndRaw);
    EXPECT_TRUE(IsExtensionInCsv(photoRaw, L".jpg"));
    EXPECT_TRUE(IsExtensionInCsv(photoRaw, L".nef"));
    EXPECT_FALSE(IsExtensionInCsv(photoRaw, L".zip"));

    EXPECT_TRUE(GetPresetCsv(AssocPreset::ClearAll).empty());
}

TEST(FileAssociationTest, CsvToggleCategoryCountAndCanonicalOrder) {
    std::wstring csv = L"PNG,.jpg,unknown_ext";
    EXPECT_TRUE(IsExtensionInCsv(csv, L".jpg"));
    EXPECT_TRUE(IsExtensionInCsv(csv, L"png"));
    EXPECT_FALSE(IsExtensionInCsv(csv, L".webp"));

    csv = ToggleExtensionInCsv(csv, L".WEBP", true);
    EXPECT_TRUE(IsExtensionInCsv(csv, L".webp"));

    csv = ToggleExtensionInCsv(csv, L".jpg", false);
    EXPECT_FALSE(IsExtensionInCsv(csv, L".jpg"));
    EXPECT_TRUE(IsExtensionInCsv(csv, L".png"));

    const auto archiveExts = GetExtensionsByCategory(AssocExtCategory::Archive);
    csv = SetCategoryInCsv(csv, AssocExtCategory::Archive, true);
    EXPECT_EQ(CountCategoryInCsv(csv, AssocExtCategory::Archive), static_cast<int>(archiveExts.size()));

    csv = SetCategoryInCsv(csv, AssocExtCategory::Archive, false);
    EXPECT_EQ(CountCategoryInCsv(csv, AssocExtCategory::Archive), 0);
}

TEST(FileAssociationTest, HashIsOrderIndependentAndSensitiveToOptions) {
    FileAssocOptions a;
    a.selectedExtsCsv = L".jpg,.png,.webp";
    a.iconStyle = AssocIconStyle::SystemNative;
    a.enableContextMenu = false;

    FileAssocOptions b;
    b.selectedExtsCsv = L"WEBP,.JPG,png"; // Different order & casing, same effective mask
    b.iconStyle = AssocIconStyle::SystemNative;
    b.enableContextMenu = false;

    EXPECT_EQ(ComputeAssocOptionsHash(a), ComputeAssocOptionsHash(b));

    FileAssocOptions c = a;
    c.iconStyle = AssocIconStyle::QuickViewLogo;
    EXPECT_NE(ComputeAssocOptionsHash(a), ComputeAssocOptionsHash(c));

    FileAssocOptions d = a;
    d.enableContextMenu = true;
    EXPECT_NE(ComputeAssocOptionsHash(a), ComputeAssocOptionsHash(d));

    const std::wstring hex = FormatAssocOptionsHash(ComputeAssocOptionsHash(a));
    EXPECT_EQ(hex.size(), 16u);
}

TEST(FileAssociationTest, AllImagesPresetIncludesComicsAndExcludesZipRarAndDocuments) {
    const std::wstring allImg = GetPresetCsv(AssocPreset::AllImages);
    const std::wstring direct = GetAllImagesAssocExtensionsString();
    EXPECT_EQ(allImg, direct);

    EXPECT_TRUE(IsExtensionInCsv(allImg, L".cbz"));
    EXPECT_TRUE(IsExtensionInCsv(allImg, L".cbr"));
    EXPECT_TRUE(IsExtensionInCsv(allImg, L".cur"));
    EXPECT_FALSE(IsExtensionInCsv(allImg, L".zip"));
    EXPECT_FALSE(IsExtensionInCsv(allImg, L".rar"));
    EXPECT_FALSE(IsExtensionInCsv(allImg, L".pdf"));
    EXPECT_FALSE(IsExtensionInCsv(allImg, L".ai"));

    int count = 0;
    for (auto ext : ALL_ASSOCIABLE_EXTENSIONS) {
        if (IsExtensionInCsv(allImg, ext)) ++count;
    }
    EXPECT_EQ(count, static_cast<int>(ALL_ASSOCIABLE_EXTENSIONS.size()) - 4);
}

TEST(FileAssociationTest, ContextMenuPolicyCoversImagesAndComicsUnconditionally) {
    // Images, RAW and comics are always included in Explorer Context Menu when enabled
    EXPECT_TRUE(ShouldIncludeExtInContextMenu(L".jpg", false));
    EXPECT_TRUE(ShouldIncludeExtInContextMenu(L".PNG", false));
    EXPECT_TRUE(ShouldIncludeExtInContextMenu(L".cr3", false));
    EXPECT_TRUE(ShouldIncludeExtInContextMenu(L".cbz", false));
    EXPECT_TRUE(ShouldIncludeExtInContextMenu(L".CBR", false));

    // General compressed archives only appear in Context Menu when explicitly opted in
    EXPECT_FALSE(ShouldIncludeExtInContextMenu(L".zip", false));
    EXPECT_FALSE(ShouldIncludeExtInContextMenu(L".rar", false));
    EXPECT_TRUE(ShouldIncludeExtInContextMenu(L".zip", true));
    EXPECT_TRUE(ShouldIncludeExtInContextMenu(L".RAR", true));

    // Unknown extension requires explicit opt-in or is rejected if not checked
    EXPECT_FALSE(ShouldIncludeExtInContextMenu(L".txt", false));
}

TEST(FileAssociationTest, DefaultAppsSettingsUriRouting) {
    EXPECT_EQ(
        BuildDefaultAppsSettingsUri(true, false, L""),
        L"ms-settings:defaultapps?registeredAppUser=QuickView");

    EXPECT_EQ(
        BuildDefaultAppsSettingsUri(true, true, L"VivorLoong.QuickView-FastFileViewer_jbgfn8pzmmykp!App"),
        L"ms-settings:defaultapps?registeredAUMID=VivorLoong.QuickView-FastFileViewer_jbgfn8pzmmykp%21App");

    EXPECT_EQ(
        BuildDefaultAppsSettingsUri(true, true, L""),
        L"ms-settings:defaultapps");

    EXPECT_EQ(
        BuildDefaultAppsSettingsUri(false, false, L""),
        L"ms-settings:defaultapps");
    EXPECT_EQ(
        BuildDefaultAppsSettingsUri(false, true, L"VivorLoong.QuickView-FastFileViewer_jbgfn8pzmmykp!App"),
        L"ms-settings:defaultapps");
}
