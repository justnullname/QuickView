#include "pch.h"
#include "TinyExrLoader.h"
#include <algorithm>
#include "StbLoader.h" // For ZlibDecode

// Define implementation
// Use system zlib (zlib-ng) for performance
#define TINYEXR_USE_MINIZ 0
#undef TINYEXR_USE_STB_ZLIB
#define TINYEXR_USE_STB_ZLIB 1
#include <zlib.h>

// [v9.9] Enable multi-threaded decompression for large EXR files
#define TINYEXR_USE_THREAD 1

// Wrappers moved to StbLoader.cpp to solve linkage
// Forward decl if needed, but TinyEXR header declares it.

#define TINYEXR_IMPLEMENTATION
#include "../third_party/tinyexr/tinyexr.h"

namespace TinyExrLoader {

    bool LoadEXR(const char* filename, 
                 int* width, int* height, 
                 std::vector<float>& outData) {
        
        float* out = nullptr; // width * height * RGBA
        int w = 0, h = 0;
        const char* err = nullptr;

        int ret = ::LoadEXR(&out, &w, &h, filename, &err);
        
        if (ret != TINYEXR_SUCCESS) {
            if (err) {
                // Log?
                ::FreeEXRErrorMessage(err); // Must free err
            }
            return false;
        }

        *width = w;
        *height = h;
        
        // Copy to vector
        size_t size = (size_t)w * h * 4;
        outData.resize(size);
        // tinyexr outputs float per component.
        memcpy(outData.data(), out, size * sizeof(float));

        free(out); // tinyexr uses free
        return true;
    }

    bool LoadEXRFromMemory(const uint8_t* inData, size_t size,
                           int* width, int* height, 
                           std::vector<float>& outData) {
        float* out = nullptr; 
        int w = 0, h = 0;
        const char* err = nullptr;

        // Use global LoadEXRFromMemory
        int ret = ::LoadEXRFromMemory(&out, &w, &h, inData, size, &err);

         if (ret != TINYEXR_SUCCESS) {
            if (err) {
                OutputDebugStringA("TinyEXR Error: ");
                OutputDebugStringA(err);
                OutputDebugStringA("\n");
                MessageBoxA(nullptr, err, "TinyEXR Error", MB_OK | MB_ICONERROR);
                ::FreeEXRErrorMessage(err); 
            } else {
                 OutputDebugStringA("TinyEXR Error: Unknown error (ret code check needed)\n");
            }
            return false;
        }

        *width = w;
        *height = h;
        
        size_t pixelSize = (size_t)w * h * 4;
        outData.resize(pixelSize);
        memcpy(outData.data(), out, pixelSize * sizeof(float));

        free(out);
        return true;
    }
    
    // [v9.9] Fast dimension extraction without full decode
    bool GetEXRDimensionsFromMemory(const uint8_t* inData, size_t size, int* width, int* height) {
        if (!inData || size < 8 || !width || !height) return false;
        
        EXRVersion exr_version;
        if (ParseEXRVersionFromMemory(&exr_version, inData, size) != TINYEXR_SUCCESS) {
            return false;
        }
        
        EXRHeader exr_header;
        InitEXRHeader(&exr_header);
        
        const char* err = nullptr;
        if (ParseEXRHeaderFromMemory(&exr_header, &exr_version, inData, size, &err) != TINYEXR_SUCCESS) {
            if (err) FreeEXRErrorMessage(err);
            return false;
        }
        
        // Extract data window dimensions
        *width = exr_header.data_window.max_x - exr_header.data_window.min_x + 1;
        *height = exr_header.data_window.max_y - exr_header.data_window.min_y + 1;
        
        FreeEXRHeader(&exr_header);
        return (*width > 0 && *height > 0);
    }
}
