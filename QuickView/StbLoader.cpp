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

#pragma warning(push)
#pragma warning(disable: 4100)
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#endif

#include "../third_party/stb/stb_image.h"

#ifdef __clang__
#pragma clang diagnostic pop
#endif
#pragma warning(pop)

namespace StbLoader {

    bool LoadImage(const char* filename, 
                   int* width, int* height, int* channels, 
                   std::pmr::vector<uint8_t>& outData, bool useFloat) {
        
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
        size_t totalSize = (size_t)w * h * 4 * pixelSize;

        outData.resize(totalSize);
        memcpy(outData.data(), data, totalSize);

        stbi_image_free(data);
        return true;
    }

    bool LoadImageFromMemory(const uint8_t* inData, size_t size,
                             int* width, int* height, int* channels, 
                             std::pmr::vector<uint8_t>& outData, bool useFloat) {
        
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
        size_t totalSize = (size_t)w * h * 4 * pixelSize;

        outData.resize(totalSize);
        memcpy(outData.data(), data, totalSize);

        stbi_image_free(data);
        return true;
    }

    bool IsSupported(const char* filename) {
        // stbi_info checks magic
        int w, h, c;
        return stbi_info(filename, &w, &h, &c) != 0;
    }
}
