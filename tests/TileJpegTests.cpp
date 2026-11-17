#include "pch.h"
#include <gtest/gtest.h>
#include <turbojpeg.h>
#include <vector>
#include <fstream>
#include <iostream>

TEST(TileJpegTests, TestPortfolioWebLOD1Decoding) {
    const std::wstring testPath = L"D:\\Works\\Personal\\Coding\\QuickView\\Local-Files\\test_img\\tile\\portfolio-web (1).jpg";
    std::ifstream file(testPath, std::ios::binary | std::ios::ate);
    ASSERT_TRUE(file.is_open()) << "Failed to open test image";
    
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> buffer(size);
    ASSERT_TRUE(file.read((char*)buffer.data(), size));
    
    tjhandle tj = tj3Init(TJINIT_DECOMPRESS);
    ASSERT_NE(tj, nullptr);
    struct TjGuard {
        tjhandle h;
        ~TjGuard() { if (h) tj3Destroy(h); }
    } guard{tj};
    
    int r = tj3DecompressHeader(tj, buffer.data(), buffer.size());
    ASSERT_EQ(r, 0) << tj3GetErrorStr(tj);
    
    int width = tj3Get(tj, TJPARAM_JPEGWIDTH);
    int height = tj3Get(tj, TJPARAM_JPEGHEIGHT);
    int subsamp = tj3Get(tj, TJPARAM_SUBSAMP);
    std::cout << "[INFO] Image: " << width << "x" << height << ", subsamp=" << subsamp << std::endl;
    
    int numFactors = 0;
    tjscalingfactor* factors = tj3GetScalingFactors(&numFactors);
    
    // Test LOD 0, 1, 2
    for (int lod = 0; lod <= 2; ++lod) {
        float scale = 1.0f / (float)(1 << lod);
        tjscalingfactor chosen = {1, 1};
        float bestDiff = 100.0f;
        for (int i = 0; i < numFactors; i++) {
            float f = (float)factors[i].num / factors[i].denom;
            float diff = std::abs(f - scale);
            if (diff < bestDiff) {
                bestDiff = diff;
                chosen = factors[i];
            }
        }
        
        r = tj3SetScalingFactor(tj, chosen);
        ASSERT_EQ(r, 0) << tj3GetErrorStr(tj);
        
        int tileSize = 512 << lod;
        int cols = (width + tileSize - 1) / tileSize;
        int rows = (height + tileSize - 1) / tileSize;
        std::cout << "\n=== Testing LOD " << lod << " (scale=" << scale << ", factor=" << chosen.num << "/" << chosen.denom 
                  << ", tileSize=" << tileSize << ", cols=" << cols << ", rows=" << rows << ") ===" << std::endl;
        
        int fullScaledW = TJSCALED(width, chosen);
        int fullScaledH = TJSCALED(height, chosen);
        
        for (int y = 0; y < rows; ++y) {
            for (int x = 0; x < cols; ++x) {
                int rectX = x * tileSize;
                int rectY = y * tileSize;
                int rectW = tileSize;
                int rectH = tileSize;
                
                int tjScdX = TJSCALED(rectX, chosen);
                int tjScdY = TJSCALED(rectY, chosen);
                int tjScdW = TJSCALED(rectW, chosen);
                int tjScdH = TJSCALED(rectH, chosen);
                
                if (tjScdX + tjScdW > fullScaledW) tjScdW = fullScaledW - tjScdX;
                if (tjScdY + tjScdH > fullScaledH) tjScdH = fullScaledH - tjScdY;
                
                tjregion cropRegion = {tjScdX, tjScdY, tjScdW, tjScdH};
                r = tj3SetCroppingRegion(tj, cropRegion);
                if (r != 0) {
                    std::cout << "[ERROR] tj3SetCroppingRegion FAILED! LOD=" << lod << " tile(" << x << "," << y << ") "
                              << "rect={" << rectX << "," << rectY << "," << rectW << "," << rectH << "} "
                              << "cropRegion={" << tjScdX << "," << tjScdY << "," << tjScdW << "," << tjScdH << "} "
                              << "Error: " << tj3GetErrorStr(tj) << std::endl;
                } else {
                    // Try decompress exactly like LoadJpegRegion_V3
                    int frameW = 512;
                    int frameH = 512;
                    int stride = frameW * 4;
                    std::vector<uint8_t> dst(stride * frameH, 0);
                    r = tj3Decompress8(tj, buffer.data(), buffer.size(), dst.data(), stride, TJPF_BGRX);
                    if (r != 0) {
                        std::cout << "[ERROR] tj3Decompress8 FAILED! LOD=" << lod << " tile(" << x << "," << y << ") "
                                  << "cropRegion={" << tjScdX << "," << tjScdY << "," << tjScdW << "," << tjScdH << "} "
                                  << "stride=" << stride << " "
                                  << "Error: " << tj3GetErrorStr(tj) << std::endl;
                    }
                }
            }
        }
    }
}
