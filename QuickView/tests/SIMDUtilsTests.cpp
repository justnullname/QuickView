#include "../SIMDUtils.h"
#include <iostream>
#include <vector>
#include <cassert>
#include <cstring>
#include <iomanip>

struct Pixel {
    uint8_t r, g, b, a;
};

void PrintPixel(const char* label, const uint8_t* p) {
    std::cout << label << ": ("
              << (int)p[0] << ", "
              << (int)p[1] << ", "
              << (int)p[2] << ", "
              << (int)p[3] << ")" << std::endl;
}

bool PixelsEqual(const uint8_t* p1, const uint8_t* p2) {
    return p1[0] == p2[0] && p1[1] == p2[1] && p1[2] == p2[2] && p1[3] == p2[3];
}

void Test_SwizzleRGBA_to_BGRA_Premul() {
    std::cout << "Testing SwizzleRGBA_to_BGRA_Premul..." << std::endl;

    auto check = [](const std::vector<uint8_t>& input, const std::vector<uint8_t>& expected, size_t count, const char* name) {
        std::vector<uint8_t> data = input;
        SIMDUtils::SwizzleRGBA_to_BGRA_Premul(data.data(), count);
        bool match = true;
        for (size_t i = 0; i < count * 4; ++i) {
            if (data[i] != expected[i]) {
                match = false;
                break;
            }
        }
        if (!match) {
            std::cout << "  FAILED: " << name << std::endl;
            for (size_t i = 0; i < count; ++i) {
                std::cout << "    Pixel " << i << ": Got (";
                std::cout << (int)data[i*4+0] << "," << (int)data[i*4+1] << "," << (int)data[i*4+2] << "," << (int)data[i*4+3] << ") Expected (";
                std::cout << (int)expected[i*4+0] << "," << (int)expected[i*4+1] << "," << (int)expected[i*4+2] << "," << (int)expected[i*4+3] << ")" << std::endl;
            }
        } else {
            std::cout << "  Passed: " << name << std::endl;
        }
    };

    // 1. Opaque white (1 pixel - fallback)
    check({255, 255, 255, 255}, {255, 255, 255, 255}, 1, "Opaque White (1 pixel)");

    // 2. Opaque white (8 pixels - SIMD)
    {
        std::vector<uint8_t> in(32, 255);
        std::vector<uint8_t> exp(32, 255);
        check(in, exp, 8, "Opaque White (8 pixels)");
    }

    // 3. Fully transparent (8 pixels)
    {
        std::vector<uint8_t> in(32, 0);
        for(int i=0; i<8; ++i) { in[i*4+0]=100; in[i*4+1]=150; in[i*4+2]=200; in[i*4+3]=0; }
        std::vector<uint8_t> exp(32, 0);
        check(in, exp, 8, "Fully Transparent (8 pixels)");
    }

    // 4. Semi-transparent (8 pixels) - Expected based on (c*a+127)/255
    {
        // RGBA: (200, 100, 50, 128)
        // BGRA: (round(50*128/255), round(100*128/255), round(200*128/255), 128)
        // B: (50*128+127)/255 = 6527/255 = 25
        // G: (100*128+127)/255 = 12927/255 = 50
        // R: (200*128+127)/255 = 25727/255 = 100
        std::vector<uint8_t> in(32);
        std::vector<uint8_t> exp(32);
        for(int i=0; i<8; ++i) {
            in[i*4+0]=200; in[i*4+1]=100; in[i*4+2]=50; in[i*4+3]=128;
            exp[i*4+0]=25; exp[i*4+1]=50; exp[i*4+2]=100; exp[i*4+3]=128;
        }
        check(in, exp, 8, "Semi-transparent 128 (8 pixels)");
    }

    // 5. Edge case: 9 pixels (SIMD + fallback)
    {
        std::vector<uint8_t> in(36, 255);
        std::vector<uint8_t> exp(36, 255);
        check(in, exp, 9, "Opaque White (9 pixels)");
    }

    // 6. Edge case: 0 pixels
    {
        SIMDUtils::SwizzleRGBA_to_BGRA_Premul(nullptr, 0);
        std::cout << "  Passed: 0 pixels" << std::endl;
    }
}

void Test_PremultiplyAlpha_BGRA() {
    std::cout << "Testing PremultiplyAlpha_BGRA..." << std::endl;

    auto check = [](const std::vector<uint8_t>& input, const std::vector<uint8_t>& expected, int w, int h, const char* name) {
        std::vector<uint8_t> data = input;
        SIMDUtils::PremultiplyAlpha_BGRA(data.data(), w, h);
        bool match = true;
        for (size_t i = 0; i < (size_t)w * h * 4; ++i) {
            if (data[i] != expected[i]) {
                match = false;
                break;
            }
        }
        if (!match) {
            std::cout << "  FAILED: " << name << std::endl;
            for (int i = 0; i < w*h; ++i) {
                if (!PixelsEqual(&data[i*4], &expected[i*4])) {
                   std::cout << "    Pixel " << i << ": Got (";
                   std::cout << (int)data[i*4+0] << "," << (int)data[i*4+1] << "," << (int)data[i*4+2] << "," << (int)data[i*4+3] << ") Expected (";
                   std::cout << (int)expected[i*4+0] << "," << (int)expected[i*4+1] << "," << (int)expected[i*4+2] << "," << (int)expected[i*4+3] << ")" << std::endl;
                }
            }
        } else {
            std::cout << "  Passed: " << name << std::endl;
        }
    };

    // Opaque white (8x1 pixels - SIMD)
    {
        std::vector<uint8_t> in(32, 255);
        std::vector<uint8_t> exp(32, 255);
        check(in, exp, 8, 1, "Opaque White BGRA (8x1)");
    }

    // Semi-transparent (8x1 pixels)
    {
        // BGRA: (50, 100, 200, 128)
        // Expected BGRA: (round(50*128/255), round(100*128/255), round(200*128/255), 128)
        // Same as before: (25, 50, 100, 128)
        std::vector<uint8_t> in(32);
        std::vector<uint8_t> exp(32);
        for(int i=0; i<8; ++i) {
            in[i*4+0]=50; in[i*4+1]=100; in[i*4+2]=200; in[i*4+3]=128;
            exp[i*4+0]=25; exp[i*4+1]=50; exp[i*4+2]=100; exp[i*4+3]=128;
        }
        check(in, exp, 8, 1, "Semi-transparent 128 BGRA (8x1)");
    }
}

int main() {
    Test_SwizzleRGBA_to_BGRA_Premul();
    Test_PremultiplyAlpha_BGRA();
    std::cout << "All tests completed!" << std::endl;
    return 0;
}
