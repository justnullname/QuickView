#pragma once
#include <vector>
#include <cstdint>
#include <string>

namespace StbLoader {

    /// <summary>
    /// Load image using stb_image.h
    /// Supports: PSD (Composite), HDR, PIC, PNM, TGA (Fallback)
    /// </summary>
    bool LoadImage(const char* filename, 
                   int* width, int* height, int* channels, 
                   std::vector<uint8_t>& outData, bool useFloat);

    bool LoadImageFromMemory(const uint8_t* inData, size_t size,
                             int* width, int* height, int* channels, 
                             std::vector<uint8_t>& outData, bool useFloat);

    // Expose internal zlib decoder for TinyEXR
    int ZlibDecode(char* obuffer, int olen, const char* ibuffer, int ilen);

    /// <summary>
    /// Check if format is supported by checking file signature
    /// </summary>
    bool IsSupported(const char* filename);

}
