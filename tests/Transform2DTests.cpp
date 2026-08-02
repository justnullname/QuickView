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
