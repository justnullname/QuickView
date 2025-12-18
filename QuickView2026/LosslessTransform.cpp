#include "pch.h"
#include "LosslessTransform.h"

// Include libjpeg-turbo header
// Include libjpeg-turbo header
#include <turbojpeg.h>


// JPEG magic bytes: FF D8 FF
static const unsigned char JPEG_MAGIC[] = { 0xFF, 0xD8, 0xFF };
// PNG magic bytes: 89 50 4E 47
static const unsigned char PNG_MAGIC[] = { 0x89, 0x50, 0x4E, 0x47 };

// Helper: Read file into memory
static unsigned char* ReadFileToMemory(LPCWSTR filePath, unsigned int& lengthBytes) {
    const unsigned int MAX_FILE_SIZE = 1024 * 1024 * 100; // 100 MB
    
    lengthBytes = 0;
    HANDLE hFile = ::CreateFileW(filePath, GENERIC_READ, FILE_SHARE_READ, 
                                  NULL, OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        return nullptr;
    }
    
    LARGE_INTEGER fileSize;
    if (!::GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart > MAX_FILE_SIZE) {
        ::CloseHandle(hFile);
        return nullptr;
    }
    
    unsigned char* buffer = new(std::nothrow) unsigned char[static_cast<size_t>(fileSize.QuadPart)];
    if (!buffer) {
        ::CloseHandle(hFile);
        return nullptr;
    }
    
    DWORD bytesRead;
    if (!::ReadFile(hFile, buffer, static_cast<DWORD>(fileSize.QuadPart), &bytesRead, NULL) ||
        bytesRead != fileSize.QuadPart) {
        delete[] buffer;
        ::CloseHandle(hFile);
        return nullptr;
    }
    
    ::CloseHandle(hFile);
    lengthBytes = static_cast<unsigned int>(fileSize.QuadPart);
    return buffer;
}

// Helper: Read first N bytes of file to check magic
static bool ReadFileMagic(LPCWSTR filePath, unsigned char* buffer, size_t count) {
    HANDLE hFile = ::CreateFileW(filePath, GENERIC_READ, FILE_SHARE_READ,
                                  NULL, OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }
    
    DWORD bytesRead;
    bool ok = ::ReadFile(hFile, buffer, static_cast<DWORD>(count), &bytesRead, NULL) &&
              bytesRead == count;
    
    ::CloseHandle(hFile);
    return ok;
}

// Helper: Write memory to file (with safe temp file approach)
static bool WriteMemoryToFile(LPCWSTR filePath, unsigned char* buffer, unsigned int lengthBytes,
                               std::wstring& errorDetail) {
    errorDetail.clear();
    
    // Get ALL original file times (create, access, modify)
    FILETIME ftCreate, ftAccess, ftModify;
    bool restoreTime = false;
    HANDLE hExisting = ::CreateFileW(filePath, GENERIC_READ, FILE_SHARE_READ,
                                      NULL, OPEN_EXISTING, 0, NULL);
    if (hExisting != INVALID_HANDLE_VALUE) {
        restoreTime = ::GetFileTime(hExisting, &ftCreate, &ftAccess, &ftModify) != 0;
        ::CloseHandle(hExisting);
    }
    
    // Write to temp file first for safety
    std::wstring tempPath = std::wstring(filePath) + L".tmp";
    HANDLE hFile = ::CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0,
                                  NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        errorDetail = L"Cannot create temp file (error " + std::to_wstring(err) + L")";
        return false;
    }
    
    DWORD bytesWritten;
    bool ok = ::WriteFile(hFile, buffer, lengthBytes, &bytesWritten, NULL) &&
              bytesWritten == lengthBytes;
    
    // Restore ALL original timestamps (create, access, modify)
    if (ok && restoreTime) {
        ::SetFileTime(hFile, &ftCreate, &ftAccess, &ftModify);
    }
    
    ::CloseHandle(hFile);
    
    if (!ok) {
        ::DeleteFileW(tempPath.c_str());
        errorDetail = L"Failed to write temp file";
        return false;
    }
    
    // Replace original with temp using MoveFileEx with REPLACE_EXISTING
    // This is atomic on NTFS and handles file locking better
    if (!::MoveFileExW(tempPath.c_str(), filePath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD err = GetLastError();
        
        // Try alternative: delete original first then move
        if (::DeleteFileW(filePath)) {
            if (::MoveFileW(tempPath.c_str(), filePath)) {
                return true;  // Success via fallback path
            }
        }
        
        // Failed - clean up temp file
        ::DeleteFileW(tempPath.c_str());
        
        switch (err) {
            case ERROR_ACCESS_DENIED:
                errorDetail = L"Access denied - file may be in use or read-only";
                break;
            case ERROR_SHARING_VIOLATION:
                errorDetail = L"File is locked by another process";
                break;
            default:
                errorDetail = L"Cannot replace file (error " + std::to_wstring(err) + L")";
                break;
        }
        return false;
    }
    
    return true;
}

// Helper: Convert TransformType to TurboJPEG operation code
static int TransformTypeToTJOp(TransformType type) {
    switch (type) {
        case TransformType::Rotate90CW:     return TJXOP_ROT90;
        case TransformType::Rotate90CCW:    return TJXOP_ROT270;
        case TransformType::Rotate180:      return TJXOP_ROT180;
        case TransformType::FlipHorizontal: return TJXOP_HFLIP;
        case TransformType::FlipVertical:   return TJXOP_VFLIP;
        default:                            return TJXOP_NONE;
    }
}

bool CLosslessTransform::IsJPEG(LPCWSTR filePath) {
    if (!filePath) return false;
    
    // Check file content magic bytes, not just extension
    unsigned char magic[4] = { 0 };
    if (!ReadFileMagic(filePath, magic, 4)) {
        return false;
    }
    
    // JPEG starts with FF D8 FF
    return (magic[0] == JPEG_MAGIC[0] && 
            magic[1] == JPEG_MAGIC[1] && 
            magic[2] == JPEG_MAGIC[2]);
}

bool CLosslessTransform::IsJPEGByExtension(LPCWSTR filePath) {
    if (!filePath) return false;
    
    std::wstring path(filePath);
    size_t dotPos = path.rfind(L'.');
    if (dotPos == std::wstring::npos) return false;
    
    std::wstring ext = path.substr(dotPos);
    // Convert to lowercase
    for (auto& c : ext) c = towlower(c);
    
    return (ext == L".jpg" || ext == L".jpeg" || ext == L".jpe");
}

TransformResult CLosslessTransform::TransformJPEG(
    LPCWSTR inputPath, 
    LPCWSTR outputPath, 
    TransformType type) {
    
    if (!inputPath || !outputPath) {
        return TransformResult::Error(L"Invalid file path");
    }
    
    // Read input file
    unsigned int inputSize;
    unsigned char* inputData = ReadFileToMemory(inputPath, inputSize);
    if (!inputData) {
        return TransformResult::Error(L"Failed to read file");
    }
    
    // Verify it's actually a JPEG by checking magic bytes
    if (inputSize < 3 || 
        inputData[0] != JPEG_MAGIC[0] || 
        inputData[1] != JPEG_MAGIC[1] || 
        inputData[2] != JPEG_MAGIC[2]) {
        delete[] inputData;
        
        // Check if it's a PNG masquerading as JPEG
        if (inputSize >= 4 && 
            inputData[0] == PNG_MAGIC[0] && 
            inputData[1] == PNG_MAGIC[1]) {
            return TransformResult::Error(L"File is PNG (not JPEG) - using generic transform");
        }
        
        return TransformResult::Error(L"Not a valid JPEG file");
    }
    
    // Initialize TurboJPEG transformer
    tjhandle tjHandle = tj3Init(TJINIT_TRANSFORM);
    if (!tjHandle) {
        delete[] inputData;
        return TransformResult::Error(L"Failed to initialize JPEG transformer");
    }
    
    // Set up transformation
    tjtransform transform = { 0 };
    transform.op = TransformTypeToTJOp(type);
    
    // First try PERFECT transformation (lossless)
    transform.options = TJXOPT_PERFECT; 
    
    // Perform transformation
    unsigned char* outputData = nullptr;
    size_t outputSize = 0;
    
    EditQuality quality = EditQuality::Lossless;
    
    int result = tj3Transform(tjHandle, inputData, inputSize, 1, 
                               &outputData, &outputSize, &transform);
    
    // If perfect transform failed due to MCU alignment, try TRIM
    if (result != 0) {
        const char* errStr = tj3GetErrorStr(tjHandle);
        // Check for "perfect" or "MCU" in error message
        // libjpeg-turbo usually says "Transform is not perfect"
        if (errStr && (strstr(errStr, "perfect") != nullptr || strstr(errStr, "MCU") != nullptr)) {
            // Retry with TRIM
            transform.options = TJXOPT_TRIM;
            result = tj3Transform(tjHandle, inputData, inputSize, 1,
                                   &outputData, &outputSize, &transform);
            
            if (result == 0) {
                quality = EditQuality::EdgeAdapted;
            }
        }
    }
    
    delete[] inputData;
    
    TransformResult transformResult;
    
    if (result == 0 && outputData) {
        // Write output file
        std::wstring writeError;
        if (WriteMemoryToFile(outputPath, outputData, static_cast<unsigned int>(outputSize), writeError)) {
            transformResult = TransformResult::OK(quality);
        } else {
            transformResult = TransformResult::Error(writeError);
        }
        tj3Free(outputData);
    } else {
        std::wstring errMsg = L"JPEG transformation failed";
        const char* errStr = tj3GetErrorStr(tjHandle);
        if (errStr) {
            // Convert error string to wide
            int len = MultiByteToWideChar(CP_UTF8, 0, errStr, -1, NULL, 0);
            if (len > 0) {
                std::wstring wideErr(len, 0);
                MultiByteToWideChar(CP_UTF8, 0, errStr, -1, &wideErr[0], len);
                errMsg += L": " + wideErr;
            }
        }
        transformResult = TransformResult::Error(errMsg);
    }
    
    tj3Destroy(tjHandle);
    return transformResult;
}

TransformResult CLosslessTransform::TransformGeneric(
    LPCWSTR inputPath, 
    LPCWSTR outputPath, 
    TransformType type) {
    
    // For non-JPEG files, use WIC to decode, transform, and re-encode
    // PNG/BMP/GIF are lossless formats - re-encoding preserves quality
    // JPEG/WebP lossy would lose quality
    
    HRESULT hr = S_OK;
    
    // Read original file times first
    FILETIME ftCreate, ftAccess, ftModify;
    bool restoreTime = false;
    HANDLE hTimeFile = ::CreateFileW(inputPath, GENERIC_READ, FILE_SHARE_READ,
                                      NULL, OPEN_EXISTING, 0, NULL);
    if (hTimeFile != INVALID_HANDLE_VALUE) {
        restoreTime = ::GetFileTime(hTimeFile, &ftCreate, &ftAccess, &ftModify) != 0;
        ::CloseHandle(hTimeFile);
    }
    
    ComPtr<IWICImagingFactory> wicFactory;
    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&wicFactory));
    if (FAILED(hr)) {
        return TransformResult::Error(L"Failed to create WIC factory");
    }
    
    // Load image with cache on load to release file handle early
    ComPtr<IWICBitmapDecoder> decoder;
    hr = wicFactory->CreateDecoderFromFilename(inputPath, nullptr, GENERIC_READ,
                                                WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr)) {
        return TransformResult::Error(L"Failed to load image");
    }
    
    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) {
        return TransformResult::Error(L"Failed to get image frame");
    }
    
    // Get original encoder CLSID before releasing decoder
    GUID containerFormat;
    hr = decoder->GetContainerFormat(&containerFormat);
    if (FAILED(hr)) {
        return TransformResult::Error(L"Failed to get image format");
    }
    
    // Determine format name for message
    bool isLosslessFormat = (containerFormat == GUID_ContainerFormatPng ||
                             containerFormat == GUID_ContainerFormatBmp ||
                             containerFormat == GUID_ContainerFormatGif ||
                             containerFormat == GUID_ContainerFormatTiff);
    
    // Cache the image in memory to release the file handle
    ComPtr<IWICBitmap> cachedBitmap;
    hr = wicFactory->CreateBitmapFromSource(frame.Get(), WICBitmapCacheOnLoad, &cachedBitmap);
    if (FAILED(hr)) {
        return TransformResult::Error(L"Failed to cache image in memory");
    }
    
    // Release decoder and frame to free file handle
    frame.Reset();
    decoder.Reset();
    
    // Determine WIC rotation
    WICBitmapTransformOptions wicTransform;
    switch (type) {
        case TransformType::Rotate90CW:     wicTransform = WICBitmapTransformRotate90; break;
        case TransformType::Rotate90CCW:    wicTransform = WICBitmapTransformRotate270; break;
        case TransformType::Rotate180:      wicTransform = WICBitmapTransformRotate180; break;
        case TransformType::FlipHorizontal: wicTransform = WICBitmapTransformFlipHorizontal; break;
        case TransformType::FlipVertical:   wicTransform = WICBitmapTransformFlipVertical; break;
        default:                            wicTransform = WICBitmapTransformRotate0; break;
    }
    
    // Apply rotation using WIC FlipRotator on the cached bitmap
    ComPtr<IWICBitmapFlipRotator> rotator;
    hr = wicFactory->CreateBitmapFlipRotator(&rotator);
    if (FAILED(hr)) {
        return TransformResult::Error(L"Failed to create rotator");
    }
    
    hr = rotator->Initialize(cachedBitmap.Get(), wicTransform);
    if (FAILED(hr)) {
        return TransformResult::Error(L"Failed to apply rotation");
    }
    
    // For in-place transform, we need to use a temp file
    std::wstring tempPath = std::wstring(outputPath) + L".wic.tmp";
    
    // Create encoder
    ComPtr<IWICBitmapEncoder> encoder;
    hr = wicFactory->CreateEncoder(containerFormat, nullptr, &encoder);
    if (FAILED(hr)) {
        // Fallback to PNG
        hr = wicFactory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
        isLosslessFormat = true;  // PNG is lossless
        if (FAILED(hr)) {
            return TransformResult::Error(L"Failed to create encoder");
        }
    }
    
    // Create output stream
    ComPtr<IWICStream> stream;
    hr = wicFactory->CreateStream(&stream);
    if (FAILED(hr)) {
        return TransformResult::Error(L"Failed to create output stream");
    }
    
    hr = stream->InitializeFromFilename(tempPath.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) {
        return TransformResult::Error(L"Failed to open output file");
    }
    
    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) {
        return TransformResult::Error(L"Failed to initialize encoder");
    }
    
    // Create frame
    ComPtr<IWICBitmapFrameEncode> frameEncode;
    hr = encoder->CreateNewFrame(&frameEncode, nullptr);
    if (FAILED(hr)) {
        return TransformResult::Error(L"Failed to create output frame");
    }
    
    hr = frameEncode->Initialize(nullptr);
    if (FAILED(hr)) {
        return TransformResult::Error(L"Failed to initialize output frame");
    }
    
    // Write rotated image
    UINT width, height;
    rotator->GetSize(&width, &height);
    hr = frameEncode->SetSize(width, height);
    if (FAILED(hr)) {
        return TransformResult::Error(L"Failed to set output size");
    }
    
    // Get pixel format
    WICPixelFormatGUID pixelFormat;
    rotator->GetPixelFormat(&pixelFormat);
    hr = frameEncode->SetPixelFormat(&pixelFormat);
    
    hr = frameEncode->WriteSource(rotator.Get(), nullptr);
    if (FAILED(hr)) {
        ::DeleteFileW(tempPath.c_str());
        return TransformResult::Error(L"Failed to write output image");
    }
    
    hr = frameEncode->Commit();
    if (FAILED(hr)) {
        ::DeleteFileW(tempPath.c_str());
        return TransformResult::Error(L"Failed to commit output frame");
    }
    
    hr = encoder->Commit();
    if (FAILED(hr)) {
        ::DeleteFileW(tempPath.c_str());
        return TransformResult::Error(L"Failed to commit output file");
    }
    
    // Close the stream before moving
    stream.Reset();
    encoder.Reset();
    frameEncode.Reset();
    
    // Restore original timestamps on temp file before move
    if (restoreTime) {
        HANDLE hOutFile = ::CreateFileW(tempPath.c_str(), FILE_WRITE_ATTRIBUTES, 0,
                                         NULL, OPEN_EXISTING, 0, NULL);
        if (hOutFile != INVALID_HANDLE_VALUE) {
            ::SetFileTime(hOutFile, &ftCreate, &ftAccess, &ftModify);
            ::CloseHandle(hOutFile);
        }
    }
    
    // Move temp file to output
    if (!::MoveFileExW(tempPath.c_str(), outputPath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD err = GetLastError();
        ::DeleteFileW(tempPath.c_str());
        return TransformResult::Error(L"Failed to save file (error " + std::to_wstring(err) + L")");
    }
    
    // Return appropriate quality based on format
    if (isLosslessFormat) {
        return TransformResult::OK(EditQuality::LosslessReenc);
    } else {
        return TransformResult::OK(EditQuality::Lossy);
    }
}

const wchar_t* CLosslessTransform::GetTransformName(TransformType type) {
    switch (type) {
        case TransformType::Rotate90CW:     return L"Rotate 90\x00B0 CW";
        case TransformType::Rotate90CCW:    return L"Rotate 90\x00B0 CCW";
        case TransformType::Rotate180:      return L"Rotate 180\x00B0";
        case TransformType::FlipHorizontal: return L"Flip Horizontal";
        case TransformType::FlipVertical:   return L"Flip Vertical";
        default:                            return L"Unknown";
    }
}
