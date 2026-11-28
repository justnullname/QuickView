#include "pch.h"
#include "gtest/gtest.h"
#include "OffscreenWebView2.h"

using QuickView::OffscreenWebView2;

TEST(SvgNeedsFallback, StaticCircleStaysOnD2D) {
    constexpr std::string_view svg =
        "<svg xmlns='http://www.w3.org/2000/svg'><circle cx='10' cy='10' r='5'/></svg>";
    EXPECT_FALSE(OffscreenWebView2::NeedsFallback(svg));
}

TEST(SvgNeedsFallback, AnimateTransformRoutesToWebView) {
    EXPECT_TRUE(OffscreenWebView2::NeedsFallback(
        "<svg><animateTransform attributeName='transform' type='rotate'/></svg>"));
}

TEST(SvgNeedsFallback, AnimateAndSetAndCssAndScript) {
    EXPECT_TRUE(OffscreenWebView2::NeedsFallback("<svg><animate attributeName='r'/></svg>"));
    EXPECT_TRUE(OffscreenWebView2::NeedsFallback("<svg><set attributeName='opacity' to='0'/></svg>"));
    EXPECT_TRUE(OffscreenWebView2::NeedsFallback("<svg><style>@keyframes spin { to { transform: rotate(360deg); } }</style></svg>"));
    EXPECT_TRUE(OffscreenWebView2::NeedsFallback("<svg><style>.x { animation: spin 1s; }</style></svg>"));
    EXPECT_TRUE(OffscreenWebView2::NeedsFallback("<svg><script>document.querySelector('circle')</script></svg>"));
}

TEST(SvgNeedsFallback, ExistingComplexTagsStillRoute) {
    EXPECT_TRUE(OffscreenWebView2::NeedsFallback("<svg><foreignObject></foreignObject></svg>"));
    EXPECT_TRUE(OffscreenWebView2::NeedsFallback("<svg><FILTER></FILTER></svg>"));
    EXPECT_TRUE(OffscreenWebView2::NeedsFallback("<svg><Mask></Mask></svg>"));
}

TEST(SvgNeedsFallback, SettingsWordIsNotSetTag) {
    EXPECT_FALSE(OffscreenWebView2::NeedsFallback(
        "<svg><text>settings</text></svg>"));
}

TEST(SvgNeedsFallback, PatternTagRoutesToWebView) {
    EXPECT_TRUE(OffscreenWebView2::NeedsFallback(
        "<svg><defs><pattern id='p' width='10' height='10'><rect width='10' height='10'/></pattern></defs></svg>"));
    EXPECT_TRUE(OffscreenWebView2::NeedsFallback(
        "<svg><defs><PATTERN id='p'/></defs></svg>"));
    // MasterGo / Figma pattern+image wrapper (like 222.svg)
    EXPECT_TRUE(OffscreenWebView2::NeedsFallback(
        "<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink' "
        "width='1080' height='1300' viewBox='0 0 1080 1300'>"
        "<defs><pattern x='0' y='0' width='1080' height='1300' patternUnits='userSpaceOnUse' id='master_svg0__0'>"
        "<image x='0' y='0' width='1080' height='1300' xlink:href='data:image/png;base64,AAAA'/>"
        "</pattern></defs>"
        "<g><rect x='0' y='0' width='1080' height='1300' fill='url(#master_svg0__0)'/></g></svg>"));
}

TEST(SvgNeedsFallback, PatternWordNotTagDoesNotRoute) {
    EXPECT_FALSE(OffscreenWebView2::NeedsFallback(
        "<svg><rect id='pattern_1'/><text>patternUnits</text></svg>"));
}

TEST(SvgNeedsFallback, HasD2dBreakingFeaturesMatchesMaskFilterPattern) {
    EXPECT_TRUE(OffscreenWebView2::HasD2dBreakingFeatures("<svg><mask id='m'/></svg>"));
    EXPECT_TRUE(OffscreenWebView2::HasD2dBreakingFeatures("<svg><FILTER id='f'></FILTER></svg>"));
    EXPECT_TRUE(OffscreenWebView2::HasD2dBreakingFeatures("<svg><defs><Pattern id='p'/></defs></svg>"));

    EXPECT_FALSE(OffscreenWebView2::HasD2dBreakingFeatures("<svg><circle cx='1' cy='1' r='1'/></svg>"));
    EXPECT_FALSE(OffscreenWebView2::HasD2dBreakingFeatures("<svg><animate attributeName='r'/></svg>"));
    EXPECT_FALSE(OffscreenWebView2::HasD2dBreakingFeatures("<svg><foreignObject/></svg>"));
    EXPECT_FALSE(OffscreenWebView2::HasD2dBreakingFeatures("<svg><text>pattern filter mask</text></svg>"));
}

TEST(SvgNeedsFallback, UnwrapSimplePatternImageSucceedsFor222Skeleton) {
    constexpr std::string_view kSvg =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" "
        "fill=\"none\" version=\"1.1\" width=\"1080\" height=\"1300\" viewBox=\"0 0 1080 1300\">"
        "<defs><pattern x=\"0\" y=\"0\" width=\"1080\" height=\"1300\" patternUnits=\"userSpaceOnUse\" id=\"master_svg0__0\">"
        "<image x=\"0\" y=\"0\" width=\"1080\" height=\"1300\" xlink:href=\"data:image/png;base64,iVBORw0KGgo=\"/>"
        "</pattern></defs>"
        "<g><rect x=\"0\" y=\"0\" width=\"1080\" height=\"1300\" rx=\"0\" fill=\"url(#master_svg0__0)\" fill-opacity=\"1\"/></g>"
        "</svg>";
    auto unwrapped = OffscreenWebView2::TryUnwrapSimplePatternImageSvg(kSvg);
    ASSERT_TRUE(unwrapped.has_value());
    EXPECT_FALSE(OffscreenWebView2::NeedsFallback(*unwrapped));
    EXPECT_EQ(unwrapped->find("<pattern"), std::string::npos);
    EXPECT_NE(unwrapped->find("<image"), std::string::npos);
    EXPECT_NE(unwrapped->find("data:image/png;base64,iVBORw0KGgo="), std::string::npos);
}

TEST(SvgNeedsFallback, UnwrapSimplePatternImageRejectsTilingAndRoundedRect) {
    // Tiled pattern (pattern 100x100 vs rect 1080x1300)
    EXPECT_FALSE(OffscreenWebView2::TryUnwrapSimplePatternImageSvg(
        "<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink'>"
        "<defs><pattern id='p' width='100' height='100' patternUnits='userSpaceOnUse'>"
        "<image width='100' height='100' xlink:href='data:image/png;base64,AA=='/></pattern></defs>"
        "<rect width='1080' height='1300' fill='url(#p)'/></svg>").has_value());

    // Rounded rect (rx=16)
    EXPECT_FALSE(OffscreenWebView2::TryUnwrapSimplePatternImageSvg(
        "<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink'>"
        "<defs><pattern id='p' width='100' height='100' patternUnits='userSpaceOnUse'>"
        "<image width='100' height='100' xlink:href='data:image/png;base64,AA=='/></pattern></defs>"
        "<rect width='100' height='100' rx='16' fill='url(#p)'/></svg>").has_value());

    // patternTransform / objectBoundingBox / multiple patterns / filter
    EXPECT_FALSE(OffscreenWebView2::TryUnwrapSimplePatternImageSvg(
        "<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink'>"
        "<defs><pattern id='p' width='100' height='100' patternTransform='rotate(45)'>"
        "<image width='100' height='100' xlink:href='data:image/png;base64,AA=='/></pattern></defs>"
        "<rect width='100' height='100' fill='url(#p)'/></svg>").has_value());

    EXPECT_FALSE(OffscreenWebView2::TryUnwrapSimplePatternImageSvg(
        "<svg xmlns='http://www.w3.org/2000/svg'>"
        "<defs><pattern id='p' width='100' height='100'><rect width='100' height='100' fill='red'/></pattern></defs>"
        "<rect width='100' height='100' fill='url(#p)'/></svg>").has_value());
}
