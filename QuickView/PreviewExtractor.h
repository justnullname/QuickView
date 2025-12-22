#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <map>

// PreviewExtractor: Static helper to extract embedded thumbnails
class PreviewExtractor {
public:
    struct ExtractedData {
        const uint8_t* pData = nullptr; // Pointer to data within source buffer
        size_t size = 0;
        bool isCopy = false; // If true, caller owns pData (rare)
        std::vector<uint8_t> buffer; // Storage if copy needed
        
        bool IsValid() const { return pData != nullptr && size > 512; } // Min valid size
    };

    // RAW (ARW, CR2, NEF, DNG, RAF, ORF)
    // All usually Tiff-based.
    static bool ExtractFromRAW(const uint8_t* fileData, size_t fileSize, ExtractedData& out);

    // HEIC / HEIF / AVIF
    // ISOBMFF based.
    static bool ExtractFromHEIC(const uint8_t* fileData, size_t fileSize, ExtractedData& out);

    // PSD / PSB
    // Resource block based.
    static bool ExtractFromPSD(const uint8_t* fileData, size_t fileSize, ExtractedData& out);

private:
    // TIFF Parsing Helpers
    static bool ParseTiffIFD(const uint8_t* start, size_t size, size_t offset, bool isLittleEndian, uint64_t& jpegOffset, uint64_t& jpegSize);
    
    // ISOBMFF Parsing Helpers
    static uint32_t ReadU32BE(const uint8_t* p);
    static uint64_t ReadU64BE(const uint8_t* p);
};
