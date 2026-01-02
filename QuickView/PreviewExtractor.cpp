#include "pch.h"
#include "PreviewExtractor.h"
#include <algorithm>
#include <cstring>

// Helper Macros
#define U16LE(p) (*(const uint16_t*)(p))
#define U32LE(p) (*(const uint32_t*)(p))
#define U16BE(p) (uint16_t)((p)[0]<<8 | (p)[1])
#define U32BE(p) (uint32_t)((p)[0]<<24 | (p)[1]<<16 | (p)[2]<<8 | (p)[3])

// --- RAW (TIFF-based) ---

bool PreviewExtractor::ExtractFromRAW(const uint8_t* data, size_t size, ExtractedData& out) {
    if (size < 1024) return false;

    // TIFF Header check
    // II (49 49) = Little Endian
    // MM (4D 4D) = Big Endian
    bool isLE = false;
    if (data[0] == 0x49 && data[1] == 0x49) isLE = true;
    else if (data[0] == 0x4D && data[1] == 0x4D) isLE = false;
    else return false; // Not TIFF

    uint16_t fortyTwo = isLE ? U16LE(data + 2) : U16BE(data + 2);
    if (fortyTwo != 42 && fortyTwo != 0x55 && fortyTwo != 0x4F52) { // 42, Panasonic(85), Olympus(0x4F52)
        // Check for CR3? CR3 is ISOBMFF, not TIFF. 
        // If CR3, this check fails, handled by HEIC parser (similar structure) or specific CR3 logic.
        // But Canon CR2 is TIFF.
        // Let's assume TIFF standard for now.
    }
    
    uint32_t firstIFD = isLE ? U32LE(data + 4) : U32BE(data + 4);
    if (firstIFD >= size) return false;

    uint64_t jpgOff = 0, jpgSz = 0;
    
    // Parse IFD0
    // IFD0 usually contains the tiny thumbnail (160x120).
    // SubIFDs usually contain the full preview.
    // Sony ARW: Preview in SubIFD.
    // Canon CR2: Preview image tag in IFD0 or StripOffsets?
    
    // Strategy: Look for "JpgFromRawStart" (Tag 0x0201) or "PreviewImageStart"
    // Also check SubIFDs (Tag 0x014A)
    
    if (ParseTiffIFD(data, size, firstIFD, isLE, jpgOff, jpgSz)) {
        if (jpgOff > 0 && jpgSz > 1024 && jpgOff + jpgSz <= size) {
             out.pData = data + jpgOff;
             out.size = (size_t)jpgSz;
             return true;
        }
    }
    
    return false;
}

bool PreviewExtractor::ParseTiffIFD(const uint8_t* data, size_t size, size_t offset, bool isLE, uint64_t& jpegOffset, uint64_t& jpegSize) {
    if (offset + 2 > size) return false;
    
    uint16_t count = isLE ? U16LE(data + offset) : U16BE(data + offset);
    size_t entrySize = 12;
    size_t current = offset + 2;
    
    if (current + count * entrySize > size) return false;

    uint64_t subIfdOffset = 0;
    
    for (int i = 0; i < count; i++) {
        const uint8_t* ev = data + current + i * entrySize;
        uint16_t tag = isLE ? U16LE(ev) : U16BE(ev);
        uint16_t type = isLE ? U16LE(ev + 2) : U16BE(ev + 2);
        uint32_t cnt = isLE ? U32LE(ev + 4) : U32BE(ev + 4);
        uint32_t valOrOff = isLE ? U32LE(ev + 8) : U32BE(ev + 8); // Value if fits 4 bytes, else offset
        
        // JPEGInterchangeFormat (0x0201) - Common in EXIF/TIFF, points to SOI
        if (tag == 0x0201) { 
            jpegOffset = valOrOff; 
        }
        // JPEGInterchangeFormatLength (0x0202)
        else if (tag == 0x0202) { 
            jpegSize = valOrOff; 
        }
        // StripOffsets (0x0111) - Sometimes used for thumb
        // StripByteCounts (0x0117)
        // SubIFDs (0x014A) - VERY IMPORTANT for RAW
        else if (tag == 0x014A) {
             // SubIFDs usually points to a list of offsets.
             // If type=4 (LONG), cnt=1, then valOrOff is offset.
             // If cnt > 1, valOrOff is offset to array of offsets.
             if (type == 4 || type == 3) { // LONG or SHORT
                 if (cnt == 1) subIfdOffset = valOrOff;
                 else subIfdOffset = valOrOff; // Offset to array
             }
        }
    }
    
    // If found valid JPEG in this IFD, check if it's "large enough" (> 50KB?)
    // Thumbnails are small. Previews are large.
    if (jpegOffset > 0 && jpegSize > 50000) return true;
    
    // Recurse into SubIFDs if current wasn't good
    if (jpegOffset == 0 && subIfdOffset > 0 && subIfdOffset < size) {
        // Assume subIfdOffset points to first SubIFD or array
        // For simplicity, just try to parse the offset itself as an IFD
        // Correct logic is complex (array reading).
        // Hack: Try to parse whatever is at subIfdOffset.
        // Usually it's an array of LONGs. Read first one.
        // If SubIFDS tag pointed to array, we need to read memory.
        // Let's assume it points to valid IFD.
        
        // Actually for ARW/DNG, SubIFD is main image or preview.
        // Let's implement full parsing later. 
        // For now, let's enable DNG/ARW basic detection.
    }
    
    return (jpegOffset > 0 && jpegSize > 0);
}


// --- HEIC (ISOBMFF) ---
uint32_t PreviewExtractor::ReadU32BE(const uint8_t* p) { return U32BE(p); }

bool PreviewExtractor::ExtractFromHEIC(const uint8_t* data, size_t size, ExtractedData& out) {
    // Simplified ISOBMFF Parser
    // Look for 'meta' box -> 'iloc' box -> item info
    // HEIC usually stores thumb as a separate item, or main image (grid)
    // Finding extracted JPEG (Exif thumb) inside 'meta' -> 'Exif' item?
    // Often HEIC contains a small AVC/HEVC thumbnail track.
    // BUT, we want JPEG.
    // Many camera HEIC embed Exif. Exif has JPEG thumb.
    
    // Quick Scan for Exif Marker?
    // Exif in ISOBMFF is wrapped in 'Exif' item.
    // Scanning for "Exif\0\0" (Standard Exif header) might work.
    
    if (size < 1024) return false;
    
    const uint8_t exifSig[] = { 0x45, 0x78, 0x69, 0x66, 0x00, 0x00 };
    // Brute force scan for Exif header (fast enough for memory mapped file)
    // Limit scan to first 1MB?
    size_t scanLimit = std::min(size, (size_t)512 * 1024);
    
    for (size_t i = 0; i < scanLimit - 6; i++) {
        if (std::memcmp(data + i, exifSig, 6) == 0) {
            // Found Exif Block
            // Parse Exif (TIFF format)
            // Exif header usually follows 49 49 or 4D 4D within 6 bytes? 
            // Standard: Exif\0\0 + TIFF Header
            size_t tiffStart = i + 6;
            if (tiffStart + 8 > size) continue;
            
            uint64_t jpgOff = 0, jpgSz = 0;
            bool isLE = (data[tiffStart] == 0x49);
            
            uint32_t ifd0 = isLE ? U32LE(data + tiffStart + 4) : U32BE(data + tiffStart + 4);
            
            if (ParseTiffIFD(data + tiffStart, size - tiffStart, ifd0, isLE, jpgOff, jpgSz)) {
                if (jpgOff > 0 && jpgSz > 0) {
                     out.pData = data + tiffStart + jpgOff;
                     out.size = (size_t)jpgSz;
                     return true;
                }
            }
        }
    }
    return false;
}

// --- PSD ---

bool PreviewExtractor::ExtractFromPSD(const uint8_t* data, size_t size, ExtractedData& out) {
    if (size < 26) return false;
    if (std::memcmp(data, "8BPS", 4) != 0) return false;
    
    // Header (26 bytes)
    // Channels (2 bytes), Height (4), Width (4), Depth (2), Mode (2)
    
    // Color Mode Data Section
    size_t offset = 26;
    uint32_t colorModeLen = U32BE(data + offset);
    offset += 4 + colorModeLen;
    if (offset >= size) return false;
    
    // Image Resources Section
    if (offset + 4 > size) return false;
    uint32_t imgResLen = U32BE(data + offset);
    size_t resEnd = offset + 4 + imgResLen;
    offset += 4;
    
    if (resEnd > size) return false;
    
    // Parse Resources
    while (offset < resEnd) {
        if (offset + 6 > resEnd) break;
        // Signature 8BIM
        if (std::memcmp(data + offset, "8BIM", 4) != 0) break;
        offset += 4;
        
        uint16_t id = U16BE(data + offset);
        offset += 2;
        
        // Name (Pascal String, padded to even)
        uint8_t nameLen = data[offset];
        offset += 1 + nameLen;
        if (offset % 2 != 0) offset++; // Pad
        
        if (offset + 4 > resEnd) break;
        uint32_t sizeData = U32BE(data + offset);
        offset += 4;
        
        if (offset + sizeData > resEnd) break;
        
        // ID 1033 (Thumbnail resource 1) or 1036 (Thumbnail resource 2 - preferred)
        if (id == 1036 || id == 1033) {
            // Found thumbnail resource!
            // Format: 
            // 4 bytes: Format (1 = kJpegRGB)
            // 4 bytes: Width
            // 4 bytes: Height
            // 4 bytes: WidthBytes
            // 4 bytes: TotalSize
            // 4 bytes: SizeCompressed
            // 2 bytes: BitsPerPixel
            // 2 bytes: Planes
            // Data...
            
            if (sizeData > 28) {
                uint32_t format = U32BE(data + offset);
                if (format == 1) { // JPEG
                    // JPEG data starts at offset + 28
                    out.pData = data + offset + 28;
                    out.size = sizeData - 28;
                    return true;
                }
            }
        }
        
        offset += sizeData;
        if (offset % 2 != 0) offset++; // Pad
    }
    
    return false;
}

// --- JPEG (EXIF Thumbnail) ---

bool PreviewExtractor::ExtractFromJPEG(const uint8_t* data, size_t size, ExtractedData& out) {
    // JPEG files have APP1 marker (0xFF 0xE1) containing EXIF
    // EXIF contains a TIFF structure with embedded JPEG thumbnail
    
    if (size < 20) return false;
    
    // Check JPEG signature
    if (data[0] != 0xFF || data[1] != 0xD8) return false;
    
    // Scan for APP1 marker
    size_t pos = 2;
    while (pos < size - 10) {
        if (data[pos] != 0xFF) {
            pos++;
            continue;
        }
        
        uint8_t marker = data[pos + 1];
        
        // Skip to next if padding
        if (marker == 0xFF) {
            pos++;
            continue;
        }
        
        // End markers
        if (marker == 0xD9 || marker == 0xDA) break;
        
        // Get segment length
        uint16_t segLen = (data[pos + 2] << 8) | data[pos + 3];
        
        // APP1 marker (0xE1) - EXIF data
        if (marker == 0xE1) {
            // Check for "Exif\0\0"
            if (pos + 10 < size && 
                data[pos + 4] == 'E' && data[pos + 5] == 'x' && 
                data[pos + 6] == 'i' && data[pos + 7] == 'f' &&
                data[pos + 8] == 0 && data[pos + 9] == 0) {
                
                // TIFF header starts at pos + 10
                const uint8_t* tiffStart = data + pos + 10;
                size_t tiffSize = segLen - 8; // Subtract "Exif\0\0" and length bytes
                
                if (tiffSize < 8 || pos + 10 + tiffSize > size) {
                    pos += 2 + segLen;
                    continue;
                }
                
                // Check TIFF byte order
                bool isLE = (tiffStart[0] == 'I' && tiffStart[1] == 'I');
                
                if ((tiffStart[0] != 'I' && tiffStart[0] != 'M') || 
                    (tiffStart[0] == 'I' && tiffStart[1] != 'I') ||
                    (tiffStart[0] == 'M' && tiffStart[1] != 'M')) {
                    pos += 2 + segLen;
                    continue;
                }
                
                // Get IFD0 offset
                uint32_t ifd0Offset = isLE ? 
                    (tiffStart[4] | (tiffStart[5] << 8) | (tiffStart[6] << 16) | (tiffStart[7] << 24)) :
                    ((tiffStart[4] << 24) | (tiffStart[5] << 16) | (tiffStart[6] << 8) | tiffStart[7]);
                
                // Parse TIFF IFD for thumbnail
                uint64_t jpgOff = 0, jpgSz = 0;
                if (ParseTiffIFD(tiffStart, tiffSize, ifd0Offset, isLE, jpgOff, jpgSz)) {
                    if (jpgOff > 0 && jpgSz > 100 && jpgOff + jpgSz <= tiffSize) {
                        out.pData = tiffStart + jpgOff;
                        out.size = (size_t)jpgSz;
                        return true;
                    }
                }
                
                // Try IFD1 (often contains the thumbnail)
                // After IFD0 entries, there's a pointer to IFD1
                if (ifd0Offset < tiffSize) {
                    uint16_t numEntries = isLE ? 
                        (tiffStart[ifd0Offset] | (tiffStart[ifd0Offset + 1] << 8)) :
                        ((tiffStart[ifd0Offset] << 8) | tiffStart[ifd0Offset + 1]);
                    
                    size_t ifd1PtrPos = ifd0Offset + 2 + numEntries * 12;
                    if (ifd1PtrPos + 4 <= tiffSize) {
                        uint32_t ifd1Offset = isLE ?
                            (tiffStart[ifd1PtrPos] | (tiffStart[ifd1PtrPos + 1] << 8) | 
                             (tiffStart[ifd1PtrPos + 2] << 16) | (tiffStart[ifd1PtrPos + 3] << 24)) :
                            ((tiffStart[ifd1PtrPos] << 24) | (tiffStart[ifd1PtrPos + 1] << 16) | 
                             (tiffStart[ifd1PtrPos + 2] << 8) | tiffStart[ifd1PtrPos + 3]);
                        
                        if (ifd1Offset > 0 && ifd1Offset < tiffSize) {
                            if (ParseTiffIFD(tiffStart, tiffSize, ifd1Offset, isLE, jpgOff, jpgSz)) {
                                if (jpgOff > 0 && jpgSz > 100 && jpgOff + jpgSz <= tiffSize) {
                                    out.pData = tiffStart + jpgOff;
                                    out.size = (size_t)jpgSz;
                                    return true;
                                }
                            }
                        }
                    }
                }
            }
        }
        
        pos += 2 + segLen;
    }
    
    return false;
}
