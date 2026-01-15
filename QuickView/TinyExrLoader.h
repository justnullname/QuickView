#pragma once
#include <vector>
#include <cstdint>

namespace TinyExrLoader {

    /// <summary>
    /// Load EXR image
    /// Returns RGBA float data
    /// </summary>
    bool LoadEXR(const char* filename, 
                 int* width, int* height, 
                 std::vector<float>& outData);

    bool LoadEXRFromMemory(const uint8_t* inData, size_t size,
                           int* width, int* height, 
                           std::vector<float>& outData);

    // [v9.9] Fast dimension extraction without full decode
    bool GetEXRDimensionsFromMemory(const uint8_t* inData, size_t size, int* width, int* height);
}
