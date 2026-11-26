/*
 * QuickView Lightweight PDF Decoder & Extension Routing Unit Tests
 * Copyright (C) 2026-Present QuickView Contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "pch.h"
#include "gtest/gtest.h"
#include "PdfDecoder.h"
#include "IcoDecoder.h"
#include "SupportedExtensions.h"
#include "FileNavigator.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string BuildSyntheticPdf(bool twoPages) {
    std::vector<std::string> objects;
    objects.emplace_back("1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
    if (twoPages) {
        objects.emplace_back("2 0 obj\n<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>\nendobj\n");
    } else {
        objects.emplace_back("2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n");
    }
    objects.emplace_back("3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Contents 5 0 R >>\nendobj\n");
    if (twoPages) {
        objects.emplace_back("4 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Contents 6 0 R >>\nendobj\n");
    }

    const std::string stream0 = "1 0 0 rg 40 40 120 120 re f\n";
    objects.push_back(
        "5 0 obj\n<< /Length " + std::to_string(stream0.size()) + " >>\nstream\n" +
        stream0 + "endstream\nendobj\n");
    if (twoPages) {
        const std::string stream1 = "0 0 1 rg 40 40 120 120 re f\n";
        objects.push_back(
            "6 0 obj\n<< /Length " + std::to_string(stream1.size()) + " >>\nstream\n" +
            stream1 + "endstream\nendobj\n");
    }

    // Object IDs: 1..3,5 for 1-page; 1..6 for 2-page. Keep dense 1..maxId for xref.
    std::string pdf = "%PDF-1.4\n";
    if (!twoPages) {
        // Re-map stream to object 4 for dense 1-page xref
        objects[2] = "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Contents 4 0 R >>\nendobj\n";
        objects[3] = "4 0 obj\n<< /Length " + std::to_string(stream0.size()) + " >>\nstream\n" +
                     stream0 + "endstream\nendobj\n";
    }

    std::vector<size_t> offsets;
    offsets.reserve(objects.size());
    for (const auto& obj : objects) {
        offsets.push_back(pdf.size());
        pdf += obj;
    }

    const size_t xrefPos = pdf.size();
    char hdr[64]{};
    snprintf(hdr, sizeof(hdr), "xref\n0 %zu\n0000000000 65535 f \n", offsets.size() + 1);
    pdf += hdr;
    for (size_t off : offsets) {
        char entry[32]{};
        snprintf(entry, sizeof(entry), "%010zu 00000 n \n", off);
        pdf += entry;
    }
    char trl[128]{};
    snprintf(trl, sizeof(trl), "trailer\n<< /Size %zu /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF\n",
             offsets.size() + 1, xrefPos);
    pdf += trl;
    return pdf;
}

bool WriteAllBytes(const std::wstring& path, const std::string& bytes) {
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"wb") != 0 || !fp) return false;
    const size_t written = fwrite(bytes.data(), 1, bytes.size(), fp);
    fclose(fp);
    return written == bytes.size();
}

} // namespace

TEST(PdfDecoderTest, DocumentRoutingExcludesNormalFolderPlaylist) {
    EXPECT_TRUE(QuickView::IsDocumentExtension(L".pdf"));
    EXPECT_TRUE(QuickView::IsDocumentExtension(L".PDF"));
    EXPECT_TRUE(QuickView::IsDocumentExtension(L".ai"));
    EXPECT_TRUE(QuickView::IsDocumentExtension(L".AI"));
    EXPECT_TRUE(QuickView::IsDocumentPath(L"C:\\Books\\spec.pdf"));
    EXPECT_TRUE(QuickView::IsDocumentPath(L"C:\\Designs\\poster.ai"));
    EXPECT_FALSE(QuickView::IsSupportedExtension(L".pdf"));
    EXPECT_FALSE(QuickView::IsSupportedExtension(L".ai"));
    EXPECT_FALSE(QuickView::IsPlaylistImageExtension(L".pdf"));
    EXPECT_FALSE(QuickView::IsPlaylistImageExtension(L".ai"));
    EXPECT_TRUE(QuickView::IsSupportedExtension(L".cur"));
    EXPECT_TRUE(QuickView::IsPlaylistImageExtension(L".cur"));
    EXPECT_TRUE(QuickView::IsNonImageDocumentExtension(L".pdf"));
    EXPECT_TRUE(QuickView::IsNonImageDocumentExtension(L".ai"));
    EXPECT_TRUE(QuickView::IsNonImageDocumentExtension(L".cbz"));
    EXPECT_FALSE(QuickView::IsNonImageDocumentExtension(L".jpg"));

    const std::wstring vp = FileNavigator::MakeMultiPagePath(L"C:\\Books\\spec.pdf", 2, 10);
    EXPECT_TRUE(FileNavigator::IsMultiPageVirtualPath(vp));
    EXPECT_EQ(FileNavigator::GetPhysicalPath(vp), L"C:\\Books\\spec.pdf");
}

TEST(PdfDecoderTest, SchemeADocumentSiblingNavigationExcludesImagesAndSupportsCur) {
    // 1. Verify CUR (idType == 2) parses via IcoDecoder::ParseDirectory
    std::vector<uint8_t> curBytes;
    // ICONDIR header: reserved=0, type=2 (CUR), count=1
    curBytes.insert(curBytes.end(), {0, 0, 2, 0, 1, 0});
    // Minimal 16-byte ICONDIRENTRY pointing to dummy 16-byte DIB
    curBytes.insert(curBytes.end(), {32, 32, 0, 0, 0, 0, 0, 0, 16, 0, 0, 0, 22, 0, 0, 0});
    curBytes.resize(22 + 16, 0);
    std::vector<QuickView::IcoDecoder::IcoEntryInfo> curEntries;
    EXPECT_TRUE(QuickView::IcoDecoder::ParseDirectory(curBytes.data(), curBytes.size(), curEntries));
    ASSERT_EQ(curEntries.size(), 1u);
    EXPECT_EQ(curEntries[0].width, 32);

    // 2. Verify Scheme A: exiting BookMode on a .pdf scans only non-image siblings (.pdf/.ai/.cbz)
    const auto tempDir = std::filesystem::temp_directory_path() / L"qv_scheme_a_doc_nav_test";
    std::error_code ec;
    std::filesystem::remove_all(tempDir, ec);
    std::filesystem::create_directories(tempDir, ec);

    const std::wstring imgPath = (tempDir / L"photo.jpg").wstring();
    const std::wstring pdfPath = (tempDir / L"doc1.pdf").wstring();
    const std::wstring aiPath  = (tempDir / L"art.ai").wstring();
    const std::wstring cbzPath = (tempDir / L"comic.cbz").wstring();
    ASSERT_TRUE(WriteAllBytes(imgPath, "fakejpg"));
    ASSERT_TRUE(WriteAllBytes(pdfPath, BuildSyntheticPdf(true)));
    ASSERT_TRUE(WriteAllBytes(aiPath,  BuildSyntheticPdf(false)));
    ASSERT_TRUE(WriteAllBytes(cbzPath, "PK\x05\x06"));

    struct CleanupDir {
        std::filesystem::path dir;
        ~CleanupDir() {
            std::error_code e;
            std::filesystem::remove_all(dir, e);
        }
    } cleanupDir{ tempDir };

    FileNavigator nav;
    nav.Initialize(pdfPath);
    EXPECT_EQ(nav.Count(), 1u); // Explicitly opened PDF never mixes with folder images
    ASSERT_TRUE(nav.EnterMultiPageBookMode(pdfPath, 2, 0, true));
    EXPECT_TRUE(nav.IsBookModeActive());
    EXPECT_EQ(nav.Count(), 2u);

    std::wstring target;
    ASSERT_TRUE(nav.ExitBookMode(target));
    EXPECT_FALSE(nav.IsBookModeActive());
    EXPECT_EQ(target, pdfPath);
    EXPECT_EQ(nav.Count(), 3u); // doc1.pdf, art.ai, comic.cbz — photo.jpg excluded!
    for (size_t i = 0; i < nav.Count(); ++i) {
        EXPECT_TRUE(QuickView::IsNonImageDocumentPath(nav.GetFile(static_cast<int>(i))));
    }
}

TEST(PdfDecoderTest, RendersMultiPagePdfRoiAndInvalidatesCacheOnChange) {
    const auto tempDir = std::filesystem::temp_directory_path();
    const std::wstring pdfPath = (tempDir / L"qv_pdf_decoder_unit_test.pdf").wstring();

    struct Cleanup {
        std::wstring path;
        ~Cleanup() {
            QuickView::PdfDecoder::ReleaseResources();
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    } cleanup{ pdfPath };

    ASSERT_TRUE(WriteAllBytes(pdfPath, BuildSyntheticPdf(true)));

    HRESULT hrInfo = E_FAIL;
    HRESULT hrP0 = E_FAIL;
    HRESULT hrP1 = E_FAIL;
    HRESULT hrRoi = E_FAIL;
    HRESULT hrInvalidated = E_FAIL;

    QuickView::PdfDecoder::PdfPageInfo info{};
    QuickView::PdfDecoder::PdfPageInfo infoAfterModify{};
    QuickView::RawImageFrame frame0;
    QuickView::RawImageFrame frame1;
    QuickView::RawImageFrame roiFrame;

    // Exercise on an explicit MTA worker thread (never wait WinRT async on Main STA)
    std::thread worker([&]() {
        const HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        hrInfo = QuickView::PdfDecoder::GetDocumentInfo(pdfPath.c_str(), 0, info);
        hrP0 = QuickView::PdfDecoder::RenderPage(pdfPath.c_str(), 0, 512, frame0, nullptr);
        hrP1 = QuickView::PdfDecoder::RenderPage(pdfPath.c_str(), 1, 512, frame1, nullptr);
        hrRoi = QuickView::PdfDecoder::RenderPageRoi(
            pdfPath.c_str(), 0, D2D1::RectF(0.25f, 0.25f, 0.75f, 0.75f), 320, 320, roiFrame);

        if (WriteAllBytes(pdfPath, BuildSyntheticPdf(false))) {
            hrInvalidated = QuickView::PdfDecoder::GetDocumentInfo(pdfPath.c_str(), 0, infoAfterModify);
        }

        if (SUCCEEDED(comHr)) CoUninitialize();
    });
    worker.join();

    if (hrInfo == HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND) ||
        hrInfo == HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND)) {
        GTEST_SKIP() << "windows.data.pdf.dll unavailable in test environment";
    }

    ASSERT_EQ(hrInfo, S_OK);
    EXPECT_EQ(info.pageCount, 2u);
    EXPECT_GT(info.pageWidthDip, 100.0f);
    EXPECT_GT(info.pageHeightDip, 100.0f);
    EXPECT_FALSE(info.isPasswordProtected);

    ASSERT_EQ(hrP0, S_OK);
    ASSERT_TRUE(frame0.IsValid());
    EXPECT_EQ(frame0.width, 512u);
    EXPECT_EQ(frame0.height, 512u);
    EXPECT_EQ(frame0.pageCount, 2u);
    EXPECT_EQ(frame0.currentPage, 0u);

    ASSERT_EQ(hrP1, S_OK);
    ASSERT_TRUE(frame1.IsValid());
    EXPECT_EQ(frame1.pageCount, 2u);
    EXPECT_EQ(frame1.currentPage, 1u);

    // Center pixel: Page 0 drew red (BGRA: B=0, G=0, R=255), Page 1 drew blue (B=255, G=0, R=0)
    const size_t centerIdx = (static_cast<size_t>(frame0.height) / 2) * frame0.stride + (frame0.width / 2) * 4;
    EXPECT_GT(frame0.pixels[centerIdx + 2], 200);
    EXPECT_LT(frame0.pixels[centerIdx + 0], 40);
    EXPECT_GT(frame1.pixels[centerIdx + 0], 200);
    EXPECT_LT(frame1.pixels[centerIdx + 2], 40);

    ASSERT_EQ(hrRoi, S_OK);
    ASSERT_TRUE(roiFrame.IsValid());
    EXPECT_EQ(roiFrame.width, 320u);
    EXPECT_EQ(roiFrame.height, 320u);
    const size_t roiCenter = (static_cast<size_t>(roiFrame.height) / 2) * roiFrame.stride + (roiFrame.width / 2) * 4;
    EXPECT_GT(roiFrame.pixels[roiCenter + 2], 200);

    // Overwriting the PDF with 1 page must invalidate the IPdfDocument cache cleanly
    ASSERT_EQ(hrInvalidated, S_OK);
    EXPECT_EQ(infoAfterModify.pageCount, 1u);
}
