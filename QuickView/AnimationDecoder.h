#pragma once
#include <memory>
#include <cstdint>
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
};

// Factory to spawn the appropriate decoder implementation
std::unique_ptr<IAnimationDecoder> CreateWuffsAnimator();
std::unique_ptr<IAnimationDecoder> CreateWebPAnimator();
std::unique_ptr<IAnimationDecoder> CreateAvifAnimator();
std::unique_ptr<IAnimationDecoder> CreateJxlAnimator();

}
