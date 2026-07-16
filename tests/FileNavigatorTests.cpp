#include <gtest/gtest.h>
#include "FileNavigator.h"
#include <fstream>
#include <filesystem>

// Stubs for global configs needed by FileNavigator
RuntimeConfig g_runtime;
AppConfig g_config;

// Test suite for FileNavigator sorting algorithm
TEST(FileNavigatorTest, SortEntries_VirtualPaths_ByName) {
    std::vector<FileNavigator::SortEntry> entries;
    
    // Construct entries with virtual paths in comic archives
    // Format: {archivePath}|{archiveIndex}|{entryName}
    // We add them in scrambled order and check if they are sorted by entryName
    FileNavigator::SortEntry e1;
    e1.p = L"C:\\comics\\manga.cbz|90|image001.jpg";
    e1.s = 1000;
    
    FileNavigator::SortEntry e2;
    e2.p = L"C:\\comics\\manga.cbz|0|image010.jpg";
    e2.s = 2000;
    
    FileNavigator::SortEntry e3;
    e3.p = L"C:\\comics\\manga.cbz|50|image002.jpg";
    e3.s = 1500;
    
    // Scrambled insertion
    entries.push_back(e2); // image010.jpg, index 0
    entries.push_back(e1); // image001.jpg, index 90
    entries.push_back(e3); // image002.jpg, index 50
    
    // Sort by Name (sortOrder = 1, sortDesc = false)
    FileNavigator::SortEntries(entries, 1, false);
    
    // Verify sorted order (should be e1, e3, e2 based on entry name natural sort)
    ASSERT_EQ(entries.size(), 3u);
    EXPECT_EQ(entries[0].p, e1.p); // image001.jpg
    EXPECT_EQ(entries[1].p, e3.p); // image002.jpg
    EXPECT_EQ(entries[2].p, e2.p); // image010.jpg
}

TEST(FileNavigatorTest, SortEntries_VirtualPaths_ByName_Descending) {
    std::vector<FileNavigator::SortEntry> entries;
    
    FileNavigator::SortEntry e1;
    e1.p = L"C:\\comics\\manga.cbz|90|image001.jpg";
    FileNavigator::SortEntry e2;
    e2.p = L"C:\\comics\\manga.cbz|0|image010.jpg";
    FileNavigator::SortEntry e3;
    e3.p = L"C:\\comics\\manga.cbz|50|image002.jpg";
    
    entries.push_back(e2);
    entries.push_back(e1);
    entries.push_back(e3);
    
    // Sort by Name Descending (sortOrder = 1, sortDesc = true)
    FileNavigator::SortEntries(entries, 1, true);
    
    ASSERT_EQ(entries.size(), 3u);
    EXPECT_EQ(entries[0].p, e2.p); // image010.jpg
    EXPECT_EQ(entries[1].p, e3.p); // image002.jpg
    EXPECT_EQ(entries[2].p, e1.p); // image001.jpg
}

TEST(FileNavigatorTest, SortEntries_MixedVirtualAndStandardPaths) {
    std::vector<FileNavigator::SortEntry> entries;
    
    FileNavigator::SortEntry e1;
    e1.p = L"C:\\comics\\manga.cbz|12|img02.png"; // Sort name: img02.png
    FileNavigator::SortEntry e2;
    e2.p = L"C:\\comics\\img01.png";             // Sort name: img01.png
    FileNavigator::SortEntry e3;
    e3.p = L"C:\\comics\\img03.png";             // Sort name: img03.png
    
    entries.push_back(e1);
    entries.push_back(e3);
    entries.push_back(e2);
    
    // Sort by Name (sortOrder = 1, sortDesc = false)
    FileNavigator::SortEntries(entries, 1, false);
    
    ASSERT_EQ(entries.size(), 3u);
    EXPECT_EQ(entries[0].p, e2.p); // img01.png
    EXPECT_EQ(entries[1].p, e1.p); // img02.png (inside archive)
    EXPECT_EQ(entries[2].p, e3.p); // img03.png
}

TEST(FileNavigatorTest, SortEntries_VirtualPaths_BySize) {
    std::vector<FileNavigator::SortEntry> entries;
    
    FileNavigator::SortEntry e1;
    e1.p = L"C:\\comics\\manga.cbz|90|small.jpg";
    e1.s = 100; // Small
    
    FileNavigator::SortEntry e2;
    e2.p = L"C:\\comics\\manga.cbz|0|large.jpg";
    e2.s = 5000; // Large
    
    FileNavigator::SortEntry e3;
    e3.p = L"C:\\comics\\manga.cbz|50|medium.jpg";
    e3.s = 1500; // Medium
    
    entries.push_back(e2);
    entries.push_back(e1);
    entries.push_back(e3);
    
    // Sort by Size (sortOrder = 4, sortDesc = false)
    FileNavigator::SortEntries(entries, 4, false);
    
    ASSERT_EQ(entries.size(), 3u);
    EXPECT_EQ(entries[0].p, e1.p); // small.jpg (100)
    EXPECT_EQ(entries[1].p, e3.p); // medium.jpg (1500)
    EXPECT_EQ(entries[2].p, e2.p); // large.jpg (5000)
}

TEST(FileNavigatorTest, SortEntries_VirtualPaths_ByType) {
    std::vector<FileNavigator::SortEntry> entries;
    
    FileNavigator::SortEntry e1;
    e1.p = L"C:\\comics\\manga.cbz|90|image.png";
    e1.t = L".png";
    
    FileNavigator::SortEntry e2;
    e2.p = L"C:\\comics\\manga.cbz|0|image.bmp";
    e2.t = L".bmp";
    
    FileNavigator::SortEntry e3;
    e3.p = L"C:\\comics\\manga.cbz|50|image.jpg";
    e3.t = L".jpg";
    
    entries.push_back(e1);
    entries.push_back(e2);
    entries.push_back(e3);
    
    // Sort by Type (sortOrder = 5, sortDesc = false)
    FileNavigator::SortEntries(entries, 5, false);
    
    ASSERT_EQ(entries.size(), 3u);
    EXPECT_EQ(entries[0].p, e2.p); // .bmp
    EXPECT_EQ(entries[1].p, e3.p); // .jpg
    EXPECT_EQ(entries[2].p, e1.p); // .png
}


// ============================================================================
// [RAW+JPEG Pairing] Unit tests for the pairing pass (pure, no disk I/O)
// ============================================================================

static FileNavigator::SortEntry MakeEntry(const std::wstring& path, uintmax_t size = 100) {
    FileNavigator::SortEntry e;
    e.p = path;
    e.s = size;
    std::filesystem::path fsPath(path);
    e.t = fsPath.extension().wstring();
    std::transform(e.t.begin(), e.t.end(), e.t.begin(), [](wchar_t c){ return std::towlower(c); });
    return e;
}

TEST(RawJpegPairingTest, StrictOneToOnePairs) {
    std::vector<FileNavigator::SortEntry> entries = {
        MakeEntry(L"C:\\p\\IMG_001.JPG"),
        MakeEntry(L"C:\\p\\IMG_001.CR3"),
        MakeEntry(L"C:\\p\\IMG_002.JPG"),
    };
    std::unordered_map<ImageID, FileNavigator::PairedRaw> paired;
    FileNavigator::ApplyRawJpegPairing(entries, paired);

    ASSERT_EQ(entries.size(), 2u); // CR3 folded away
    EXPECT_EQ(entries[0].p, L"C:\\p\\IMG_001.JPG");
    EXPECT_EQ(entries[1].p, L"C:\\p\\IMG_002.JPG");

    ASSERT_EQ(paired.size(), 1u);
    const auto* raw = &paired.at(ComputePathHash(L"C:\\p\\IMG_001.JPG"));
    EXPECT_EQ(raw->path, L"C:\\p\\IMG_001.CR3");
    EXPECT_EQ(raw->id, ComputePathHash(L"C:\\p\\IMG_001.CR3"));
}

TEST(RawJpegPairingTest, CaseInsensitiveStemAndExtension) {
    std::vector<FileNavigator::SortEntry> entries = {
        MakeEntry(L"C:\\p\\dsc_0007.nef"),
        MakeEntry(L"C:\\p\\DSC_0007.JPG"),
    };
    std::unordered_map<ImageID, FileNavigator::PairedRaw> paired;
    FileNavigator::ApplyRawJpegPairing(entries, paired);
    EXPECT_EQ(entries.size(), 1u);
    EXPECT_EQ(paired.size(), 1u);
}

TEST(RawJpegPairingTest, AmbiguousGroupsStayVisible) {
    // Two rendered stills against one RAW -> no pair, everything visible
    std::vector<FileNavigator::SortEntry> entries = {
        MakeEntry(L"C:\\p\\IMG_001.JPG"),
        MakeEntry(L"C:\\p\\IMG_001.HEIC"),
        MakeEntry(L"C:\\p\\IMG_001.CR3"),
    };
    std::unordered_map<ImageID, FileNavigator::PairedRaw> paired;
    FileNavigator::ApplyRawJpegPairing(entries, paired);
    EXPECT_EQ(entries.size(), 3u);
    EXPECT_TRUE(paired.empty());

    // Two RAWs against one rendered -> same
    entries = {
        MakeEntry(L"C:\\p\\IMG_002.JPG"),
        MakeEntry(L"C:\\p\\IMG_002.CR3"),
        MakeEntry(L"C:\\p\\IMG_002.DNG"),
    };
    FileNavigator::ApplyRawJpegPairing(entries, paired);
    EXPECT_EQ(entries.size(), 3u);
    EXPECT_TRUE(paired.empty());
}

TEST(RawJpegPairingTest, NonWhitelistedNeitherPairsNorBlocks) {
    // A same-stem .png (not camera-written, origin unknown) or .tif
    // (RAW-derived export) must not become the visible half, and must not
    // break the JPG+RAW pair.
    std::vector<FileNavigator::SortEntry> entries = {
        MakeEntry(L"C:\\p\\IMG_001.JPG"),
        MakeEntry(L"C:\\p\\IMG_001.CR3"),
        MakeEntry(L"C:\\p\\IMG_001.png"),
        MakeEntry(L"C:\\p\\IMG_001.tif"),
    };
    std::unordered_map<ImageID, FileNavigator::PairedRaw> paired;
    FileNavigator::ApplyRawJpegPairing(entries, paired);
    ASSERT_EQ(entries.size(), 3u); // only the CR3 folded
    EXPECT_EQ(paired.size(), 1u);

    // RAW + only a .tif sibling -> nothing pairs, RAW stays visible
    entries = {
        MakeEntry(L"C:\\p\\IMG_003.tif"),
        MakeEntry(L"C:\\p\\IMG_003.ARW"),
    };
    FileNavigator::ApplyRawJpegPairing(entries, paired);
    EXPECT_EQ(entries.size(), 2u);
    EXPECT_TRUE(paired.empty());
}

TEST(RawJpegPairingTest, EarlyExitWhenNoMix) {
    // Only rendered files -> untouched
    std::vector<FileNavigator::SortEntry> entries = {
        MakeEntry(L"C:\\p\\a.jpg"), MakeEntry(L"C:\\p\\b.jpg"),
    };
    std::unordered_map<ImageID, FileNavigator::PairedRaw> paired;
    FileNavigator::ApplyRawJpegPairing(entries, paired);
    EXPECT_EQ(entries.size(), 2u);
    EXPECT_TRUE(paired.empty());

    // Only RAWs -> untouched
    entries = { MakeEntry(L"C:\\p\\a.cr3"), MakeEntry(L"C:\\p\\b.nef") };
    FileNavigator::ApplyRawJpegPairing(entries, paired);
    EXPECT_EQ(entries.size(), 2u);
    EXPECT_TRUE(paired.empty());
}

TEST(RawJpegPairingTest, MultiDotStemsMatchExactly) {
    // "photo.bak.jpg" pairs only with "photo.bak.cr3", not "photo.cr3"
    std::vector<FileNavigator::SortEntry> entries = {
        MakeEntry(L"C:\\p\\photo.bak.jpg"),
        MakeEntry(L"C:\\p\\photo.cr3"),
    };
    std::unordered_map<ImageID, FileNavigator::PairedRaw> paired;
    FileNavigator::ApplyRawJpegPairing(entries, paired);
    EXPECT_EQ(entries.size(), 2u);
    EXPECT_TRUE(paired.empty());
}

TEST(RawJpegPairingTest, ParseExifDateTime) {
    const int64_t a = FileNavigator::ParseExifDateTime("2026:07:09 14:30:45");
    const int64_t b = FileNavigator::ParseExifDateTime("2026:07:09 14:30:45");
    const int64_t c = FileNavigator::ParseExifDateTime("2026:07:09 14:30:48");
    ASSERT_NE(a, 0);
    EXPECT_EQ(a, b);            // deterministic
    EXPECT_EQ(c - a, 3);        // second-level arithmetic
    // Unparsable inputs
    EXPECT_EQ(FileNavigator::ParseExifDateTime(""), 0);
    EXPECT_EQ(FileNavigator::ParseExifDateTime("not a date"), 0);
    EXPECT_EQ(FileNavigator::ParseExifDateTime("2026-07-09 14:30:45"), 0); // wrong separator
    EXPECT_EQ(FileNavigator::ParseExifDateTime("1899:01:01 00:00:00"), 0); // pre-epoch
}

TEST(RawJpegPairingTest, PairVerificationIsStrict) {
    // One shutter actuation writes the identical DateTimeOriginal into both
    // files: only an exact match passes.
    EXPECT_FALSE(FileNavigator::PairVerificationFails(1000, 1000));
    // Any difference splits the pair, even one second
    EXPECT_TRUE(FileNavigator::PairVerificationFails(1000, 1001));
    EXPECT_TRUE(FileNavigator::PairVerificationFails(1001, 1000));
    EXPECT_TRUE(FileNavigator::PairVerificationFails(1000, 5000));
    // An unreadable side fails verification: a same-name file without a
    // readable capture time is likely not the camera's output of this shot
    EXPECT_TRUE(FileNavigator::PairVerificationFails(0, 0));
    EXPECT_TRUE(FileNavigator::PairVerificationFails(0, 1000));
    EXPECT_TRUE(FileNavigator::PairVerificationFails(1000, 0));
}

TEST(RawJpegPairingTest, VerifiedMismatchBlacklistPreventsFold) {
    std::vector<FileNavigator::SortEntry> entries = {
        MakeEntry(L"C:\\p\\IMG_001.JPG"),
        MakeEntry(L"C:\\p\\IMG_001.CR3"),
        MakeEntry(L"C:\\p\\IMG_002.JPG"),
        MakeEntry(L"C:\\p\\IMG_002.NEF"),
    };
    // IMG_001's capture times were verified as mismatching
    std::unordered_set<ImageID> skip = { ComputePathHash(L"C:\\p\\IMG_001.JPG") };
    std::unordered_map<ImageID, FileNavigator::PairedRaw> paired;
    FileNavigator::ApplyRawJpegPairing(entries, paired, &skip);

    // IMG_001 stays split; IMG_002 still folds
    ASSERT_EQ(entries.size(), 3u);
    EXPECT_EQ(paired.size(), 1u);
    EXPECT_TRUE(paired.count(ComputePathHash(L"C:\\p\\IMG_002.JPG")) == 1);
    EXPECT_TRUE(std::any_of(entries.begin(), entries.end(),
        [](const auto& e) { return e.p == L"C:\\p\\IMG_001.CR3"; }));
}

// Integration: pairing through FileNavigator::Initialize on a real directory
TEST(RawJpegPairingTest, InitializePairsAndRedirectsRawOpen) {
    namespace fs = std::filesystem;
    const bool oldPair = g_config.PairRawJpeg;
    const int oldSortOrder = g_runtime.SortOrder;
    const bool oldSortDesc = g_runtime.SortDescending;
    g_runtime.SortOrder = 1;
    g_runtime.SortDescending = false;

    fs::path tempDir = fs::current_path() / "test_pairing_dir";
    std::error_code ec;
    fs::create_directory(tempDir, ec);
    ASSERT_FALSE(ec);

    fs::path jpg1 = tempDir / "IMG_001.JPG";
    fs::path raw1 = tempDir / "IMG_001.CR3";
    fs::path jpg2 = tempDir / "IMG_002.JPG";
    std::ofstream(jpg1).close();
    std::ofstream(raw1).close();
    std::ofstream(jpg2).close();

    // Flag off: stock behavior, all three visible
    g_config.PairRawJpeg = false;
    {
        FileNavigator nav;
        nav.Initialize(jpg1.wstring(), nullptr);
        EXPECT_EQ(nav.Count(), 3u);
        EXPECT_EQ(nav.PairedRawCount(), 0u);
    }

    // Flag on: pair folds, metadata queryable via the rendered file's ImageID
    g_config.PairRawJpeg = true;
    {
        FileNavigator nav;
        nav.Initialize(jpg1.wstring(), nullptr);
        ASSERT_EQ(nav.Count(), 2u);
        EXPECT_EQ(nav.GetFile(0), jpg1.wstring());
        EXPECT_EQ(nav.GetFile(1), jpg2.wstring());

        const ImageID jpgId = nav.GetImageID(0);
        ASSERT_TRUE(nav.HasPairedRaw(jpgId));
        EXPECT_EQ(nav.GetPairedRaw(jpgId)->path, raw1.wstring());
        EXPECT_FALSE(nav.HasPairedRaw(nav.GetImageID(1)));

        // Opening the hidden RAW itself lands on its rendered sibling: both
        // the navigator index and the path the viewer should actually load
        // (GetResolvedPath is what main.cpp feeds to LoadImageAsync).
        FileNavigator nav2;
        nav2.Initialize(raw1.wstring(), nullptr);
        ASSERT_EQ(nav2.Count(), 2u);
        EXPECT_EQ(nav2.Index(), 0);
        EXPECT_EQ(nav2.GetFile(nav2.Index()), jpg1.wstring());
        EXPECT_EQ(nav2.GetResolvedPath(raw1.wstring()), jpg1.wstring());
        // Non-hidden paths resolve to themselves
        EXPECT_EQ(nav2.GetResolvedPath(jpg2.wstring()), jpg2.wstring());
    }

    fs::remove_all(tempDir, ec);
    g_config.PairRawJpeg = oldPair;
    g_runtime.SortOrder = oldSortOrder;
    g_runtime.SortDescending = oldSortDesc;
}

// Helper to create a valid uncompressed Zip archive programmatically
static bool CreateMockZip(const std::wstring& zipPath, const std::string& entryName) {
    std::ofstream fs(zipPath, std::ios::binary);
    if (!fs) return false;

    // Local file header
    uint32_t lfhSig = 0x04034b50;
    uint16_t versionNeeded = 10;
    uint16_t flags = 0;
    uint16_t method = 0; // Stored (no compression)
    uint16_t modTime = 0;
    uint16_t modDate = 0;
    uint32_t crc32 = 0;
    uint32_t compSize = 1;
    uint32_t uncompSize = 1;
    uint16_t nameLen = (uint16_t)entryName.size();
    uint16_t extraLen = 0;

    fs.write(reinterpret_cast<char*>(&lfhSig), 4);
    fs.write(reinterpret_cast<char*>(&versionNeeded), 2);
    fs.write(reinterpret_cast<char*>(&flags), 2);
    fs.write(reinterpret_cast<char*>(&method), 2);
    fs.write(reinterpret_cast<char*>(&modTime), 2);
    fs.write(reinterpret_cast<char*>(&modDate), 2);
    fs.write(reinterpret_cast<char*>(&crc32), 4);
    fs.write(reinterpret_cast<char*>(&compSize), 4);
    fs.write(reinterpret_cast<char*>(&uncompSize), 4);
    fs.write(reinterpret_cast<char*>(&nameLen), 2);
    fs.write(reinterpret_cast<char*>(&extraLen), 2);
    fs.write(entryName.data(), nameLen);
    
    // Write 1 byte of payload (since size is 1)
    fs.write("a", 1);

    uint32_t cdOffset = (uint32_t)fs.tellp();

    // Central directory file header
    uint32_t cdfhSig = 0x02014b50;
    uint16_t versionMadeBy = 10;
    uint16_t fileCommentLen = 0;
    uint16_t diskStart = 0;
    uint16_t internalAttr = 0;
    uint32_t externalAttr = 0;
    uint32_t localHeaderOffset = 0;

    fs.write(reinterpret_cast<char*>(&cdfhSig), 4);
    fs.write(reinterpret_cast<char*>(&versionMadeBy), 2);
    fs.write(reinterpret_cast<char*>(&versionNeeded), 2);
    fs.write(reinterpret_cast<char*>(&flags), 2);
    fs.write(reinterpret_cast<char*>(&method), 2);
    fs.write(reinterpret_cast<char*>(&modTime), 2);
    fs.write(reinterpret_cast<char*>(&modDate), 2);
    fs.write(reinterpret_cast<char*>(&crc32), 4);
    fs.write(reinterpret_cast<char*>(&compSize), 4);
    fs.write(reinterpret_cast<char*>(&uncompSize), 4);
    fs.write(reinterpret_cast<char*>(&nameLen), 2);
    fs.write(reinterpret_cast<char*>(&extraLen), 2);
    fs.write(reinterpret_cast<char*>(&fileCommentLen), 2);
    fs.write(reinterpret_cast<char*>(&diskStart), 2);
    fs.write(reinterpret_cast<char*>(&internalAttr), 2);
    fs.write(reinterpret_cast<char*>(&externalAttr), 4);
    fs.write(reinterpret_cast<char*>(&localHeaderOffset), 4);
    fs.write(entryName.data(), nameLen);

    uint32_t eocdOffset = (uint32_t)fs.tellp();
    uint32_t cdSize = eocdOffset - cdOffset;

    // End of central directory record (EOCD)
    uint32_t eocdSig = 0x06054b50;
    uint16_t diskNum = 0;
    uint16_t cdDisk = 0;
    uint16_t numEntriesThisDisk = 1;
    uint16_t numEntriesTotal = 1;
    uint16_t commentLen = 0;

    fs.write(reinterpret_cast<char*>(&eocdSig), 4);
    fs.write(reinterpret_cast<char*>(&diskNum), 2);
    fs.write(reinterpret_cast<char*>(&cdDisk), 2);
    fs.write(reinterpret_cast<char*>(&numEntriesThisDisk), 2);
    fs.write(reinterpret_cast<char*>(&numEntriesTotal), 2);
    fs.write(reinterpret_cast<char*>(&cdSize), 4);
    fs.write(reinterpret_cast<char*>(&cdOffset), 4);
    fs.write(reinterpret_cast<char*>(&commentLen), 2);

    return true;
}

// Integration Test to verify navigation and archive traversal matching the user's scenario
TEST(FileNavigatorTest, TraverseFolderAndArchives) {
    namespace fs = std::filesystem;
    
    // 1. Setup temporary directory structure on disk
    fs::path tempDir = fs::current_path() / "test_traverse_dir";
    std::error_code ec;
    fs::create_directory(tempDir, ec);
    ASSERT_FALSE(ec);
    
    fs::path p1 = tempDir / "1.png";
    fs::path p2 = tempDir / "2.png";
    fs::path p3 = tempDir / "3.png";
    fs::path zip3 = tempDir / "3.zip";
    fs::path p4 = tempDir / "4.png";
    fs::path zipMpv = tempDir / "mpv_window_screen.zip";
    
    // Create empty images
    std::ofstream(p1).close();
    std::ofstream(p2).close();
    std::ofstream(p3).close();
    std::ofstream(p4).close();
    
    // Create valid mock zip files with supported images inside
    ASSERT_TRUE(CreateMockZip(zip3.wstring(), "inner.png"));
    ASSERT_TRUE(CreateMockZip(zipMpv.wstring(), "screen.png"));
    
    // 2. Initialize FileNavigator starting at 1.png
    g_runtime.SortOrder = 1; // Sort by Name
    g_runtime.SortDescending = false;
    g_runtime.NavTraverse = true; // Enable through subfolders/archives
    g_runtime.NavLoop = false;
    
    FileNavigator nav;
    nav.Initialize(p1.wstring(), nullptr);
    
    // 3. Verify normal playlist (zip archives must be skipped)
    // Playlist should be: 1.png, 2.png, 3.png, 4.png
    ASSERT_EQ(nav.Count(), 4u);
    EXPECT_EQ(nav.GetFile(0), p1.wstring());
    EXPECT_EQ(nav.GetFile(1), p2.wstring());
    EXPECT_EQ(nav.GetFile(2), p3.wstring());
    EXPECT_EQ(nav.GetFile(3), p4.wstring());
    
    // 4. Verify Forward Traverse Navigation
    // 1.png -> 2.png
    EXPECT_EQ(nav.Next(), p2.wstring());
    // 2.png -> 3.png
    EXPECT_EQ(nav.Next(), p3.wstring());
    
    // 3.png -> 3.zip (since 3.zip is a container sibling that comes alphabetically before 4.png)
    std::wstring nextPath = nav.Next();
    std::wstring expectedVirtual = zip3.wstring() + L"|0|inner.png";
    EXPECT_EQ(nextPath, expectedVirtual);
    
    // Initialize navigator on entering 3.zip
    nav.Initialize(nextPath, nullptr);
    ASSERT_EQ(nav.Count(), 1u);
    EXPECT_EQ(nav.GetFile(0), expectedVirtual);
    
    // At the end of 3.zip, press Next -> should traverse to 4.png (its next sibling alphabetically)
    nextPath = nav.Next();
    EXPECT_EQ(nextPath, p4.wstring());
    
    // Initialize navigator on 4.png
    nav.Initialize(nextPath, nullptr);
    ASSERT_EQ(nav.Count(), 4u);
    
    // At 4.png, press Next -> should traverse into mpv_window_screen.zip (next sibling after 4.png)
    nextPath = nav.Next();
    std::wstring expectedMpvVirtual = zipMpv.wstring() + L"|0|screen.png";
    EXPECT_EQ(nextPath, expectedMpvVirtual);
    
    // Initialize navigator on mpv_window_screen.zip
    nav.Initialize(nextPath, nullptr);
    // At end of mpv_window_screen.zip, press Next -> no more siblings, should return L"" (NavLoop = false)
    EXPECT_EQ(nav.Next(), L"");
    
    // 5. Verify Backward Traverse Navigation
    // Start inside mpv_window_screen.zip, press Previous -> should traverse backward to 4.png
    std::wstring prevPath = nav.Previous();
    EXPECT_EQ(prevPath, p4.wstring());
    
    // Initialize navigator on 4.png
    nav.Initialize(prevPath, nullptr);
    // At 4.png, press Previous -> should traverse backward into 3.zip (last page)
    prevPath = nav.Previous();
    EXPECT_EQ(prevPath, expectedVirtual);
    
    // Initialize navigator on 3.zip
    nav.Initialize(prevPath, nullptr);
    // At first page of 3.zip, press Previous -> should traverse backward to 3.png
    prevPath = nav.Previous();
    EXPECT_EQ(prevPath, p3.wstring());
    
    // Initialize navigator on 3.png
    nav.Initialize(prevPath, nullptr);
    // At 3.png, press Previous -> 3.png -> 2.png
    EXPECT_EQ(nav.Previous(), p2.wstring());
    // 2.png -> 1.png
    EXPECT_EQ(nav.Previous(), p1.wstring());
    
    // At 1.png (first file), press Previous -> no more siblings, should return L"" (NavLoop = false)
    EXPECT_EQ(nav.Previous(), L"");
    
    // 6. Cleanup
    fs::remove_all(tempDir, ec);
}

static bool CreateMockZipMultiple(const std::wstring& zipPath, const std::vector<std::string>& entryNames) {
    std::ofstream fs(zipPath, std::ios::binary);
    if (!fs) return false;

    struct EntryInfo {
        std::string name;
        uint32_t localHeaderOffset;
    };
    std::vector<EntryInfo> infos;

    for (const auto& name : entryNames) {
        EntryInfo info;
        info.name = name;
        info.localHeaderOffset = (uint32_t)fs.tellp();
        infos.push_back(info);

        uint32_t lfhSig = 0x04034b50;
        uint16_t versionNeeded = 10;
        uint16_t flags = 0;
        uint16_t method = 0;
        uint16_t modTime = 0;
        uint16_t modDate = 0;
        uint32_t crc32 = 0;
        uint32_t compSize = 1;
        uint32_t uncompSize = 1;
        uint16_t nameLen = (uint16_t)name.size();
        uint16_t extraLen = 0;

        fs.write(reinterpret_cast<char*>(&lfhSig), 4);
        fs.write(reinterpret_cast<char*>(&versionNeeded), 2);
        fs.write(reinterpret_cast<char*>(&flags), 2);
        fs.write(reinterpret_cast<char*>(&method), 2);
        fs.write(reinterpret_cast<char*>(&modTime), 2);
        fs.write(reinterpret_cast<char*>(&modDate), 2);
        fs.write(reinterpret_cast<char*>(&crc32), 4);
        fs.write(reinterpret_cast<char*>(&compSize), 4);
        fs.write(reinterpret_cast<char*>(&uncompSize), 4);
        fs.write(reinterpret_cast<char*>(&nameLen), 2);
        fs.write(reinterpret_cast<char*>(&extraLen), 2);
        fs.write(name.data(), nameLen);
        fs.write("a", 1);
    }

    uint32_t cdOffset = (uint32_t)fs.tellp();

    for (const auto& info : infos) {
        uint32_t cdfhSig = 0x02014b50;
        uint16_t versionMadeBy = 10;
        uint16_t versionNeeded = 10;
        uint16_t flags = 0;
        uint16_t method = 0;
        uint16_t modTime = 0;
        uint16_t modDate = 0;
        uint32_t crc32 = 0;
        uint32_t compSize = 1;
        uint32_t uncompSize = 1;
        uint16_t nameLen = (uint16_t)info.name.size();
        uint16_t extraLen = 0;
        uint16_t fileCommentLen = 0;
        uint16_t diskStart = 0;
        uint16_t internalAttr = 0;
        uint32_t externalAttr = 0;
        uint32_t localHeaderOffset = info.localHeaderOffset;

        fs.write(reinterpret_cast<char*>(&cdfhSig), 4);
        fs.write(reinterpret_cast<char*>(&versionMadeBy), 2);
        fs.write(reinterpret_cast<char*>(&versionNeeded), 2);
        fs.write(reinterpret_cast<char*>(&flags), 2);
        fs.write(reinterpret_cast<char*>(&method), 2);
        fs.write(reinterpret_cast<char*>(&modTime), 2);
        fs.write(reinterpret_cast<char*>(&modDate), 2);
        fs.write(reinterpret_cast<char*>(&crc32), 4);
        fs.write(reinterpret_cast<char*>(&compSize), 4);
        fs.write(reinterpret_cast<char*>(&uncompSize), 4);
        fs.write(reinterpret_cast<char*>(&nameLen), 2);
        fs.write(reinterpret_cast<char*>(&extraLen), 2);
        fs.write(reinterpret_cast<char*>(&fileCommentLen), 2);
        fs.write(reinterpret_cast<char*>(&diskStart), 2);
        fs.write(reinterpret_cast<char*>(&internalAttr), 2);
        fs.write(reinterpret_cast<char*>(&externalAttr), 4);
        fs.write(reinterpret_cast<char*>(&localHeaderOffset), 4);
        fs.write(info.name.data(), nameLen);
    }

    uint32_t eocdOffset = (uint32_t)fs.tellp();
    uint32_t cdSize = eocdOffset - cdOffset;

    uint32_t eocdSig = 0x06054b50;
    uint16_t diskNum = 0;
    uint16_t cdDisk = 0;
    uint16_t numEntriesThisDisk = (uint16_t)entryNames.size();
    uint16_t numEntriesTotal = (uint16_t)entryNames.size();
    uint16_t commentLen = 0;

    fs.write(reinterpret_cast<char*>(&eocdSig), 4);
    fs.write(reinterpret_cast<char*>(&diskNum), 2);
    fs.write(reinterpret_cast<char*>(&cdDisk), 2);
    fs.write(reinterpret_cast<char*>(&numEntriesThisDisk), 2);
    fs.write(reinterpret_cast<char*>(&numEntriesTotal), 2);
    fs.write(reinterpret_cast<char*>(&cdSize), 4);
    fs.write(reinterpret_cast<char*>(&cdOffset), 4);
    fs.write(reinterpret_cast<char*>(&commentLen), 2);

    return true;
}

TEST(FileNavigatorTest, SortArchivesByNameAscendingOverride) {
    // Save current config
    bool oldOverride = g_config.SortArchivesByNameAscending;
    int oldSortOrder = g_runtime.SortOrder;
    bool oldSortDescending = g_runtime.SortDescending;

    // Enable override
    g_config.SortArchivesByNameAscending = true;
    
    // Set global sorting to Sort by Size (4) and Descending (true)
    g_runtime.SortOrder = 4;
    g_runtime.SortDescending = true;

    namespace fs = std::filesystem;
    fs::path tempDir = fs::current_path() / "test_sort_override_dir";
    std::error_code ec;
    fs::create_directory(tempDir, ec);
    ASSERT_FALSE(ec);

    fs::path zipFile = tempDir / "archive.zip";
    
    // Write out-of-order images with different sizes
    // We want to verify that they are sorted alphabetically by filename, NOT by size or descending.
    // e.g. "image002.png" (size 1), "image003.png" (size 1), "image001.png" (size 1)
    std::vector<std::string> fileNames = { "image002.png", "image003.png", "image001.png" };
    ASSERT_TRUE(CreateMockZipMultiple(zipFile.wstring(), fileNames));

    FileNavigator nav;
    // Initialize starting with the first entry in virtual path format
    std::wstring startPath = zipFile.wstring() + L"|0|image002.png";
    nav.Initialize(startPath, nullptr);

    // Verify file count
    ASSERT_EQ(nav.Count(), 3u);

    // Even though global sort is descending or size, it must sort by filename ascending because of the override:
    // Expected order:
    // 0: image001.png
    // 1: image002.png
    // 2: image003.png
    EXPECT_TRUE(nav.GetFile(0).find(L"image001.png") != std::wstring::npos);
    EXPECT_TRUE(nav.GetFile(1).find(L"image002.png") != std::wstring::npos);
    EXPECT_TRUE(nav.GetFile(2).find(L"image003.png") != std::wstring::npos);

    // Now test with SortArchivesByNameAscending = false
    g_config.SortArchivesByNameAscending = false;
    // Re-initialize to re-sort
    nav.Initialize(startPath, nullptr);
    
    // Since SortArchivesByNameAscending is false, it uses the global sort order (4: Size, Descending).
    // Because all files have same size (1) in our mock zip, they fallback to natural name sort descending.
    // Expected order:
    // 0: image003.png
    // 1: image002.png
    // 2: image001.png
    EXPECT_TRUE(nav.GetFile(0).find(L"image003.png") != std::wstring::npos);
    EXPECT_TRUE(nav.GetFile(1).find(L"image002.png") != std::wstring::npos);
    EXPECT_TRUE(nav.GetFile(2).find(L"image001.png") != std::wstring::npos);

    // Cleanup
    fs::remove_all(tempDir, ec);

    // Restore config
    g_config.SortArchivesByNameAscending = oldOverride;
    g_runtime.SortOrder = oldSortOrder;
    g_runtime.SortDescending = oldSortDescending;
}

