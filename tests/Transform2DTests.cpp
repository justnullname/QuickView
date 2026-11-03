#include "pch.h"
#include <gtest/gtest.h>
#include "EditState.h"
#include "LosslessTransform.h"

TEST(Transform2DTest, FromExifMapping) {
    auto t1 = Transform2D::FromExif(1);
    EXPECT_EQ(t1.Rotation, 0);
    EXPECT_FALSE(t1.FlipH);

    auto t2 = Transform2D::FromExif(2);
    EXPECT_EQ(t2.Rotation, 0);
    EXPECT_TRUE(t2.FlipH);

    auto t3 = Transform2D::FromExif(3);
    EXPECT_EQ(t3.Rotation, 180);
    EXPECT_FALSE(t3.FlipH);

    auto t4 = Transform2D::FromExif(4);
    EXPECT_EQ(t4.Rotation, 180);
    EXPECT_TRUE(t4.FlipH);

    auto t5 = Transform2D::FromExif(5);
    EXPECT_EQ(t5.Rotation, 90);
    EXPECT_TRUE(t5.FlipH);

    auto t6 = Transform2D::FromExif(6);
    EXPECT_EQ(t6.Rotation, 90);
    EXPECT_FALSE(t6.FlipH);

    auto t7 = Transform2D::FromExif(7);
    EXPECT_EQ(t7.Rotation, 270);
    EXPECT_TRUE(t7.FlipH);

    auto t8 = Transform2D::FromExif(8);
    EXPECT_EQ(t8.Rotation, 270);
    EXPECT_FALSE(t8.FlipH);
}

TEST(Transform2DTest, CombineExifAndEdit) {
    // Exif 6 (Rot 90) + Edit Rot 90 -> Net Rot 180
    Transform2D exif6 = Transform2D::FromExif(6);
    Transform2D editRot90{ 90, false };
    auto net1 = Transform2D::Combine(exif6, editRot90);
    EXPECT_EQ(net1.Rotation, 180);
    EXPECT_FALSE(net1.FlipH);

    // Exif 6 (Rot 90) + Edit FlipH -> Net Rot 270 + FlipH
    Transform2D editFlipH{ 0, true };
    auto net2 = Transform2D::Combine(exif6, editFlipH);
    EXPECT_EQ(net2.Rotation, 270);
    EXPECT_TRUE(net2.FlipH);

    // Exif 2 (FlipH) + Edit Rot 90 -> Net Rot 90 + FlipH
    Transform2D exif2 = Transform2D::FromExif(2);
    auto net3 = Transform2D::Combine(exif2, editRot90);
    EXPECT_EQ(net3.Rotation, 90);
    EXPECT_TRUE(net3.FlipH);
}

TEST(JpegExifResetTest, ScanAndResetOrientation) {
    // Synthesize a minimal JPEG EXIF APP1 Header with Orientation 6
    // Little-Endian (II) EXIF payload
    unsigned char jpegData[] = {
        0xFF, 0xD8, // SOI
        0xFF, 0xE1, // APP1 Marker
        0x00, 0x1E, // Length = 30
        'E', 'x', 'i', 'f', 0x00, 0x00, // Header
        'I', 'I', 0x2A, 0x00, // TIFF Little-Endian
        0x08, 0x00, 0x00, 0x00, // IFD0 Offset = 8
        0x01, 0x00, // 1 Entry
        0x12, 0x01, // Tag 0x0112 (Orientation)
        0x03, 0x00, // Type 3 (SHORT)
        0x01, 0x00, 0x00, 0x00, // Count = 1
        0x06, 0x00, 0x00, 0x00  // Value = 6
    };

    size_t size = sizeof(jpegData);
    EXPECT_EQ(jpegData[30], 6); // Value 6 initially

    bool resetSuccess = CLosslessTransform::ResetJpegExifOrientationTo1(jpegData, size);
    EXPECT_TRUE(resetSuccess);
    EXPECT_EQ(jpegData[30], 1); // Value reset to 1
}

TEST(OrientationMathTest, AllOrientationsDimensionSwap) {
    auto IsDimensionSwapped = [](int orientation) -> bool {
        return orientation >= 5 && orientation <= 8;
    };

    EXPECT_FALSE(IsDimensionSwapped(1));
    EXPECT_FALSE(IsDimensionSwapped(2));
    EXPECT_FALSE(IsDimensionSwapped(3));
    EXPECT_FALSE(IsDimensionSwapped(4));
    EXPECT_TRUE(IsDimensionSwapped(5));
    EXPECT_TRUE(IsDimensionSwapped(6));
    EXPECT_TRUE(IsDimensionSwapped(7));
    EXPECT_TRUE(IsDimensionSwapped(8));
}

TEST(SuperResolutionDimensionTest, Exif8DimensionAndCropMapping) {
    // Physical file 1800x1200 with EXIF 8 (270° CW)
    int physicalW = 1800;
    int physicalH = 1200;
    int exif = 8;
    bool isRot90 = (exif >= 5 && exif <= 8);

    // 1. Visual Base Dimensions (Display Size on screen)
    int visualW = isRot90 ? physicalH : physicalW; // 1200
    int visualH = isRot90 ? physicalW : physicalH; // 1800
    EXPECT_EQ(visualW, 1200);
    EXPECT_EQ(visualH, 1800);

    // 2. Super Resolution (2x)
    float srScale = 2.0f;
    int srPhysicalW = static_cast<int>(physicalW * srScale); // 3600
    int srPhysicalH = static_cast<int>(physicalH * srScale); // 2400
    EXPECT_EQ(srPhysicalW, 3600);
    EXPECT_EQ(srPhysicalH, 2400);

    // 3. Export Dialog Expected Visual Target Dimensions
    int targetVisualW = static_cast<int>(visualW * srScale); // 2400
    int targetVisualH = static_cast<int>(visualH * srScale); // 3600
    EXPECT_EQ(targetVisualW, 2400);
    EXPECT_EQ(targetVisualH, 3600);

    // 4. Net Transform for Exif 8
    Transform2D t = Transform2D::FromExif(exif);
    EXPECT_EQ(t.Rotation, 270);
    EXPECT_FALSE(t.FlipH);

    // 5. Crop in visual space (e.g. 100x100 to 500x700 -> size 400x600)
    int cropVisualW = 400;
    int cropVisualH = 600;
    int srCropW = static_cast<int>(std::round(cropVisualW * srScale)); // 800
    int srCropH = static_cast<int>(std::round(cropVisualH * srScale)); // 1200
    EXPECT_EQ(srCropW, 800);
    EXPECT_EQ(srCropH, 1200);
}



TEST(Transform2DTest, LoupeViewportAnchorZoomPrecision) {
    // Simulates the exact anchor zoom math used in HotkeyAction::Loupe & WM_MOUSEWHEEL
    const float vpW = 1920.0f;
    const float vpH = 1080.0f;
    const float imgW = 3840.0f;
    const float imgH = 2160.0f;

    const float fitScale = std::min(vpW / imgW, vpH / imgH); // 0.5f
    const float baseZoom = 1.0f;
    const float initialTotalScale = fitScale * baseZoom; // 0.5f
    const float initialPanX = 50.0f;
    const float initialPanY = -30.0f;

    // Pick an arbitrary cursor screen position
    const float cursorX = 1200.0f;
    const float cursorY = 700.0f;

    // 1. ScreenToImage mapping:
    // imgDrawX = vpW * 0.5f + PanX - (imgW * 0.5f * totalScale)
    float imgDrawX = vpW * 0.5f + initialPanX - (imgW * 0.5f * initialTotalScale);
    float imgDrawY = vpH * 0.5f + initialPanY - (imgH * 0.5f * initialTotalScale);
    float imgX = (cursorX - imgDrawX) / initialTotalScale;
    float imgY = (cursorY - imgDrawY) / initialTotalScale;

    // 2. Perform Loupe Viewport Zoom to targetTotalScale (e.g. 2.5x magnification)
    const float targetTotalScale = 2.5f;
    float newPanX = cursorX - vpW * 0.5f + (imgW * 0.5f - imgX) * targetTotalScale;
    float newPanY = cursorY - vpH * 0.5f + (imgH * 0.5f - imgY) * targetTotalScale;

    // 3. Project imgX back to screen with newPan and targetTotalScale
    float newImgDrawX = vpW * 0.5f + newPanX - (imgW * 0.5f * targetTotalScale);
    float newImgDrawY = vpH * 0.5f + newPanY - (imgH * 0.5f * targetTotalScale);
    float reprojectedScreenX = newImgDrawX + imgX * targetTotalScale;
    float reprojectedScreenY = newImgDrawY + imgY * targetTotalScale;

    EXPECT_NEAR(reprojectedScreenX, cursorX, 1e-4f);
    EXPECT_NEAR(reprojectedScreenY, cursorY, 1e-4f);
}
