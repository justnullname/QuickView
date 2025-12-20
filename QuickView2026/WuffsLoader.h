#pragma once
// ============================================================
// WuffsLoader.h - Wuffs Wrapper API for QuickView
// ============================================================
// This header exposes clean C++ functions for PNG/GIF decoding.
// The actual Wuffs implementation is hidden in WuffsImpl.cpp.

#include <cstdint>
#include <vector>

namespace WuffsLoader {

/// <summary>
/// Decode PNG image to BGRA pixels
/// </summary>
/// <param name="data">PNG file data</param>
/// <param name="size">Data size in bytes</param>
/// <param name="outWidth">Output: image width</param>
/// <param name="outHeight">Output: image height</param>
/// <param name="outPixels">Output: BGRA pixel data (stride = width * 4)</param>
/// <returns>true on success</returns>
bool DecodePNG(const uint8_t* data, size_t size,
               uint32_t* outWidth, uint32_t* outHeight,
               std::vector<uint8_t>& outPixels);

/// <summary>
/// Decode GIF image (first frame) to BGRA pixels
/// </summary>
/// <param name="data">GIF file data</param>
/// <param name="size">Data size in bytes</param>
/// <param name="outWidth">Output: image width</param>
/// <param name="outHeight">Output: image height</param>
/// <param name="outPixels">Output: BGRA pixel data (stride = width * 4)</param>
/// <returns>true on success</returns>
bool DecodeGIF(const uint8_t* data, size_t size,
               uint32_t* outWidth, uint32_t* outHeight,
               std::vector<uint8_t>& outPixels);

} // namespace WuffsLoader
