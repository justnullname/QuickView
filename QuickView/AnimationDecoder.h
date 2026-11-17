#pragma once
#include <cstring>
#include <memory>
#include <cstdint>
#include <utility>
#include "ImageTypes.h"
#include "MappedFile.h"

namespace QuickView {

// Abstract interface for Animated Image Decoding (GIF, APNG, WebP)
// Enforces Keyframe Checkpoints / Sparse Snapshots pattern.
class IAnimationDecoder {
public:
    virtual ~IAnimationDecoder() = default;

    /// Initialize the decoder and parse metadata. Return false if strictly invalid.
    virtual bool Initialize(std::shared_ptr<QuickView::MappedFile> file, QuickView::PixelFormat preferredFormat) = 0;

    /// Advance to the next frame. Re-uses shared buffer if possible.
    virtual std::shared_ptr<RawImageFrame> GetNextFrame() = 0;

    /// Seek to a specific frame index. Ideally leverages sparse snapshots.
    virtual std::shared_ptr<RawImageFrame> SeekToFrame(uint32_t targetIndex) = 0;

    /// Get total frames. May increment as streaming progresses, or fixed early.
    virtual uint32_t GetTotalFrames() const = 0;

    /// Check if this source is truly animated (multiple frames with delays).
    virtual bool IsAnimated() const = 0;

    /// Return true if the decoder produces valid partial frame updates (dirty rects)
    virtual bool SupportsDirtyRect() const { return false; }

    /// Return true if this decoder holds AI super-resolved frames
    virtual bool IsSrDecoder() const { return false; }
};

// Factory to spawn the appropriate decoder implementation
std::unique_ptr<IAnimationDecoder> CreateWuffsAnimator();
std::unique_ptr<IAnimationDecoder> CreateWebPAnimator();
std::unique_ptr<IAnimationDecoder> CreateAvifAnimator();
std::unique_ptr<IAnimationDecoder> CreateJxlAnimator();

/// Extract frame 0 from an initialized animated decoder, copying into `allocator`
/// when provided or taking ownership of frame 0's aligned pixel buffer otherwise.
/// Always transfers `animator` ownership to `outAnimator` on success.
inline bool ExtractFirstAnimatedFrame(
    std::unique_ptr<IAnimationDecoder> animator,
    const AllocatorCallback* allocator,
    uint8_t*& outPixels,
    int& outWidth,
    int& outHeight,
    int& outStride,
    PixelFormat& outFormat,
    AnimationFrameMeta& outFrameMeta,
    std::shared_ptr<IAnimationDecoder>& outAnimator) {
    if (!animator || !animator->IsAnimated()) {
        return false;
    }

    auto firstFrame = animator->GetNextFrame();
    if (!firstFrame || !firstFrame->pixels) {
        return false;
    }

    if (allocator && allocator->pfn) {
        const size_t totalBytes =
            static_cast<size_t>(firstFrame->stride) * static_cast<size_t>(firstFrame->height);
        uint8_t* allocated = (*allocator)(totalBytes);
        if (!allocated) {
            return false;
        }
        std::memcpy(allocated, firstFrame->pixels, totalBytes);
        outPixels = allocated;
    } else {
        outPixels = firstFrame->pixels;
        firstFrame->pixels = nullptr;
        firstFrame->memoryDeleter.DestroyCtx();
        firstFrame->memoryDeleter.Clear();
    }

    outWidth = firstFrame->width;
    outHeight = firstFrame->height;
    outStride = firstFrame->stride;
    outFormat = firstFrame->format;
    outFrameMeta = firstFrame->frameMeta;
    outAnimator = std::move(animator);
    return true;
}

}
