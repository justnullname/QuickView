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
