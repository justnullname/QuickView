// Helper: Read Big-Endian uint32
static uint32_t ReadBE32(const uint8_t* p) {
    return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

// Helper: Parse ISOBMFF to find HEIC dimensions (ispe box)
static bool GetHEICDimensions(LPCWSTR filePath, uint32_t* width, uint32_t* height) {
    if (!width || !height) return false;
    
    HANDLE hFile = CreateFileW(filePath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    
    // Read first 4KB. Typically enough to reach 'ispe' in 'ipco'.
    // If 'meta' is far (e.g. after huge 'mdat'), we might miss it in 4KB.
    // Let's read 64KB to be robust. 
    DWORD bytesToRead = 65536; 
    std::vector<uint8_t> buffer(bytesToRead);
    DWORD bytesRead = 0;
    
    if (!ReadFile(hFile, buffer.data(), bytesToRead, &bytesRead, nullptr)) {
        CloseHandle(hFile);
        return false;
    }
    CloseHandle(hFile);
    
    if (bytesRead < 12) return false;
    
    const uint8_t* p = buffer.data();
    const uint8_t* end = p + bytesRead;
    
    // Simple robust scan: Look for 'ipco' then 'ispe' inside it.
    // Traversing strict hierarchy is better but more complex. 
    // Given we just need a hint for Fast/Heavy dispatch, finding the first 'ispe' is usually correct (primary item).
    
    // Scan for 'meta' -> 'iprp' -> 'ipco' -> 'ispe'
    // But 'meta' content can be complex.
    // Shortcut: Search for 'ispe' directly? No, might match random data.
    // Middle ground: Iterate top level, enter 'meta', enter 'iprp', enter 'ipco'.
    
    auto parseBox = [&](const uint8_t* start, const uint8_t* limit, uint32_t targetType, const uint8_t** outBody, size_t* outSize) -> bool {
        const uint8_t* curr = start;
        while (curr + 8 <= limit) {
            uint32_t size = ReadBE32(curr);
            uint32_t type = ReadBE32(curr + 4);
            
            if (size < 8) {
                if (size == 1) { // Large box (64-bit size)
                    if (curr + 16 > limit) return false;
                    // We don't support skipping 64-bit boxes in this mini-parser, assuming headers are small 32-bit boxes.
                    // Just skip 16 bytes header? No, size is crucial.
                    // For headers, 32-bit size is standard.
                    size = 16; // Treat as header-only skip if we can't parse size using 32-bit var? 
                    // Actually, if we hit a 64-bit box in header, we likely failed.
                    // But let's assume standard small boxes.
                } else if (size == 0) {
                    // unexpected in header
                    return false; 
                }
            }
            
            if (type == targetType) {
                *outBody = curr + 8;
                *outSize = size - 8;
                return true;
            }
            
            curr += size;
        }
        return false;
    };
    
    // 1. Find 'meta'
    // 'meta' is often at top level.
    const uint8_t* metaBody = nullptr;
    size_t metaSize = 0;
    
    // Top level iteration
    const uint8_t* curr = p;
    while (curr + 8 <= end) {
        uint32_t size = ReadBE32(curr);
        uint32_t type = ReadBE32(curr + 4);
        
        if (type == 0x6D657461) { // 'meta'
            // 'meta' is FullBox: version(1) + flags(3) = 4 bytes extra
            metaBody = curr + 12; 
            metaSize = size - 12;
            break;
        }
        
        if (size < 8) break; // Invalid
        curr += size;
    }
    
    if (!metaBody) return false;
    
    // 2. Find 'iprp' inside 'meta'
    const uint8_t* iprpBody = nullptr;
    size_t iprpSize = 0;
    if (!parseBox(metaBody, metaBody + metaSize, 0x69707270, &iprpBody, &iprpSize)) { // 'iprp'
        return false;
    }
    
    // 3. Find 'ipco' inside 'iprp'
    const uint8_t* ipcoBody = nullptr;
    size_t ipcoSize = 0;
    if (!parseBox(iprpBody, iprpBody + iprpSize, 0x6970636F, &ipcoBody, &ipcoSize)) { // 'ipco'
        return false;
    }
    
    // 4. Find first 'ispe' inside 'ipco'
    const uint8_t* ispeBody = nullptr;
    size_t ispeSize = 0;
    // Note: 'ipco' contains properties. Iterate them.
    if (parseBox(ipcoBody, ipcoBody + ipcoSize, 0x69737065, &ispeBody, &ispeSize)) { // 'ispe'
        // 'ispe' is FullBox (4 bytes) + width (4) + height (4)
        if (ispeSize >= 12) {
            *width = ReadBE32(ispeBody + 4);
            *height = ReadBE32(ispeBody + 8);
            return true;
        }
    }
    
    return false;
}
