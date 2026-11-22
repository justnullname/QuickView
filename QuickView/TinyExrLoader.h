#pragma once
#include <vector>
#include <cstdint>

namespace TinyExrLoader {

    struct ExrChromaticities {
        bool valid = false;
        float redX = 0.0f;
        float redY = 0.0f;
        float greenX = 0.0f;
        float greenY = 0.0f;
        float blueX = 0.0f;
        float blueY = 0.0f;
        float whiteX = 0.0f;
        float whiteY = 0.0f;
    };

    /// <summary>
    /// Load EXR image
    /// Returns RGBA float data
    /// </summary>
    bool LoadEXR(const char* filename, 
                 int* width, int* height, 
                 std::vector<float>& outData);

    bool LoadEXRFromMemory(const uint8_t* inData, size_t size,
                           int* width, int* height, 
                           std::vector<float>& outData,
                           ExrChromaticities* outChromaticities = nullptr);

    // [v9.9] Fast dimension extraction without full decode
    bool GetEXRDimensionsFromMemory(const uint8_t* inData, size_t size, int* width, int* height);
}
