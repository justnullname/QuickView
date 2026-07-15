#include "pch.h"
#include "gtest/gtest.h"
#include "StringUtils.h"

TEST(StringUtilsTest, SplitAndTrimCSV) {
    auto res1 = QuickView::SplitAndTrimCSV(L"A, B, , C, A");
    ASSERT_EQ(res1.size(), 4);
    EXPECT_EQ(res1[0], L"A");
    EXPECT_EQ(res1[1], L"B");
    EXPECT_EQ(res1[2], L"C");
    EXPECT_EQ(res1[3], L"A");
}

TEST(StringUtilsTest, NormalizeCSV) {
    std::vector<std::wstring> allowed = { L"A", L"B", L"C" };
    
    // Normal case: deduplicate, filter allowed, truncate limit
    std::wstring normalized = QuickView::NormalizeCSV(L"A, B, D, C, A, B", allowed, 2);
    EXPECT_EQ(normalized, L"A,B");

    // Under limit
    std::wstring normalized2 = QuickView::NormalizeCSV(L"A, B, D, C, A, B", allowed, 5);
    EXPECT_EQ(normalized2, L"A,B,C");
}
