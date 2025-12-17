#pragma once
#include "pch.h"

/// <summary>
/// Lossless transformation types
/// </summary>
enum class TransformType {
    Rotate90CW,      // Clockwise 90 degrees
    Rotate90CCW,     // Counter-clockwise 90 degrees
    Rotate180,       // 180 degrees
    FlipHorizontal,  // Horizontal flip (mirror)
    FlipVertical     // Vertical flip
};

/// <summary>
/// Edit quality after rotation/flip
/// </summary>
enum class EditQuality {
    Lossless,       // Green - JPEG lossless
    LosslessReenc,  // Green - PNG/BMP re-encoded but lossless
    EdgeAdapted,    // Yellow - JPEG edge adapted (trimmed for MCU alignment)
    Lossy           // Red - Lossy re-encode (WebP, etc.)
};

/// <summary>
/// Result of a transformation operation
/// </summary>
struct TransformResult {
    bool Success = false;
    std::wstring ErrorMessage;
    EditQuality Quality = EditQuality::Lossless;
    
    static TransformResult OK(EditQuality quality) {
        return { true, L"", quality };
    }
    
    static TransformResult Error(const std::wstring& msg) {
        return { false, msg, EditQuality::Lossless };
    }
};

/// <summary>
/// Lossless image transformation using libjpeg-turbo
/// Supports JPEG lossless rotation and flip operations
/// </summary>
class CLosslessTransform {
public:
    /// <summary>
    /// Check if file is JPEG format by reading magic bytes
    /// </summary>
    static bool IsJPEG(LPCWSTR filePath);
    
    /// <summary>
    /// Check if file has JPEG extension (may not be actual JPEG)
    /// </summary>
    static bool IsJPEGByExtension(LPCWSTR filePath);
    
    /// <summary>
    /// Perform lossless JPEG transformation
    /// Input and output can be the same file (in-place transform)
    /// </summary>
    static TransformResult TransformJPEG(
        LPCWSTR inputPath, 
        LPCWSTR outputPath, 
        TransformType type);
    
    /// <summary>
    /// Perform transformation on any image format using WIC (lossy for non-JPEG)
    /// </summary>
    static TransformResult TransformGeneric(
        LPCWSTR inputPath, 
        LPCWSTR outputPath, 
        TransformType type);
    
    /// <summary>
    /// Get human-readable name for transform type
    /// </summary>
    static const wchar_t* GetTransformName(TransformType type);
};
