#include "pch.h"
#include "StbLoader.h"

// Define implementation for stb_image
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_JPEG // Use WIC or LibJPEG-Turbo
#define STBI_NO_PNG  // Use Wuffs
#define STBI_NO_GIF  // Use Wuffs
#define STBI_NO_BMP  // Use Wuffs
#define STBI_NO_TGA  // Use Wuffs (saves ~3KB)
// Keep PSD, HDR, PIC, PNM (stb handles these, Wuffs doesn't fully)

#include "../third_party/stb/stb_image.h"
#include <zlib.h>

// ----------------------------------------------------------------------------
// Zlib Shim for TinyEXR (using System Zlib / zlib-ng)
// ----------------------------------------------------------------------------
extern "C" int stbi_zlib_decode_buffer(char *obuffer, int olen, const char *ibuffer, int ilen) {
    uLongf destLen = (uLongf)olen;
    int ret = uncompress((Bytef*)obuffer, &destLen, (const Bytef*)ibuffer, (uLong)ilen);
    if (ret == Z_OK) return (int)destLen;
    return -1;
}

extern "C" unsigned char *stbi_zlib_compress(unsigned char *data, int data_len, int *out_len, int quality) {
    uLongf destLen = compressBound(data_len);
    unsigned char* dest = (unsigned char*)malloc(destLen);
    if (!dest) return nullptr;
    if (compress(dest, &destLen, data, data_len) != Z_OK) {
        free(dest);
        return nullptr;
    }
    if (out_len) *out_len = (int)destLen;
    return dest;
}

namespace StbLoader {

    bool LoadImage(const char* filename, 
                   int* width, int* height, int* channels, 
                   std::vector<uint8_t>& outData, bool useFloat) {
        
        // stbi_load returns data that needs to be freed
        // We want 4 channels (RGBA) forced for simplified handling
        int w, h, c;
        void* data = nullptr;

        if (useFloat) {
            data = stbi_loadf(filename, &w, &h, &c, 4); 
        } else {
            data = stbi_load(filename, &w, &h, &c, 4); 
        }

        if (!data) return false;

        *width = w;
        *height = h;
        *channels = 4;

        size_t pixelSize = useFloat ? sizeof(float) : sizeof(uint8_t);
        size_t totalSize = w * h * 4 * pixelSize;

        outData.resize(totalSize);
        memcpy(outData.data(), data, totalSize);

        stbi_image_free(data);
        return true;
    }

    bool LoadImageFromMemory(const uint8_t* inData, size_t size,
                             int* width, int* height, int* channels, 
                             std::vector<uint8_t>& outData, bool useFloat) {
        
        int w, h, c;
        void* data = nullptr;

        if (useFloat) {
            data = stbi_loadf_from_memory(inData, (int)size, &w, &h, &c, 4); // Force 4 channels
        } else {
            data = stbi_load_from_memory(inData, (int)size, &w, &h, &c, 4); // Force 4 channels
        }

        if (!data) return false;

        *width = w;
        *height = h;
        *channels = 4;

        size_t pixelSize = useFloat ? sizeof(float) : sizeof(uint8_t);
        size_t totalSize = w * h * 4 * pixelSize;

        outData.resize(totalSize);
        memcpy(outData.data(), data, totalSize);

        stbi_image_free(data);
        return true;
    }

    int ZlibDecode(char* obuffer, int olen, const char* ibuffer, int ilen) {
        // Use system zlib
        uLongf destLen = (uLongf)olen;
        int ret = uncompress((Bytef*)obuffer, &destLen, (const Bytef*)ibuffer, (uLong)ilen);
        if (ret == Z_OK) return (int)destLen;
        return -1;
    }

    bool IsSupported(const char* filename) {
        // stbi_info checks magic
        int w, h, c;
        return stbi_info(filename, &w, &h, &c) != 0;
    }
}
