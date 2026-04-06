#include "pch.h"
#include "AnimationDecoder.h"
#include "MappedFile.h"
#include <avif/avif.h>

namespace QuickView {

class AvifAnimator : public IAnimationDecoder {
public:
    AvifAnimator() {
        m_decoder = avifDecoderCreate();
        memset(&m_rgb, 0, sizeof(m_rgb));
    }
    ~AvifAnimator() override {
        if (m_rgb.pixels) {
            avifRGBImageFreePixels(&m_rgb);
        }
        if (m_decoder) {
            avifDecoderDestroy(m_decoder);
        }
    }

    bool Initialize(std::shared_ptr<QuickView::MappedFile> file, QuickView::PixelFormat preferredFormat) override {
        m_mappedFile = file;
        
        avifResult res = avifDecoderSetIOMemory(m_decoder, file->data(), file->size());
        if (res != AVIF_RESULT_OK) return false;

        res = avifDecoderParse(m_decoder);
        if (res != AVIF_RESULT_OK) return false;

        m_totalFrames = m_decoder->imageCount;
        if (m_totalFrames <= 1) return false; // Not animated

        avifRGBImageSetDefaults(&m_rgb, m_decoder->image);
        m_rgb.format = AVIF_RGB_FORMAT_BGRA;
        m_rgb.depth = 8;
        
        avifRGBImageAllocatePixels(&m_rgb);
        
        return true;
    }

    std::shared_ptr<RawImageFrame> GetNextFrame() override {
        avifResult res = avifDecoderNextImage(m_decoder);
        if (res == AVIF_RESULT_NO_IMAGES_REMAINING) {
            return nullptr;
        }
        if (res != AVIF_RESULT_OK) return nullptr;
        
        return BuildFrame();
    }

    std::shared_ptr<RawImageFrame> SeekToFrame(uint32_t targetIndex) override {
        if (targetIndex >= m_totalFrames) targetIndex = m_totalFrames - 1;
        
        avifResult res = avifDecoderNthImage(m_decoder, targetIndex);
        if (res != AVIF_RESULT_OK) return nullptr;
        
        return BuildFrame();
    }

    uint32_t GetTotalFrames() const override { return m_totalFrames; }
    bool IsAnimated() const override { return m_totalFrames > 1; }

private:
    std::shared_ptr<RawImageFrame> BuildFrame() {
        avifResult res = avifImageYUVToRGB(m_decoder->image, &m_rgb);
        if (res != AVIF_RESULT_OK) return nullptr;
        
        auto frame = std::make_shared<RawImageFrame>();
        
        frame->width = m_rgb.width;
        frame->height = m_rgb.height;
        frame->stride = m_rgb.rowBytes;
        frame->format = PixelFormat::BGRA8888;
        frame->quality = DecodeQuality::Full;
        
        size_t bufSize = (size_t)frame->stride * frame->height;
        frame->pixels = (uint8_t*)_aligned_malloc(bufSize, 64);
        frame->memoryDeleter = [](uint8_t* p) { _aligned_free(p); };
        memcpy(frame->pixels, m_rgb.pixels, bufSize);
        
        auto& meta = frame->frameMeta;
        meta.index = m_decoder->imageIndex;
        
        avifImageTiming timing;
        avifDecoderNthImageTiming(m_decoder, m_decoder->imageIndex, &timing);
        meta.delayMs = (uint32_t)(timing.duration * 1000.0);
        if (meta.delayMs < 10) meta.delayMs = 100;
        
        meta.totalFrames = m_totalFrames;
        
        // AVIF decoder provides the fully composited image, no partial rects
        meta.disposal = FrameDisposalMode::Keep;
        meta.isDelta = false;
        meta.rcLeft = 0;
        meta.rcTop = 0;
        meta.rcRight = frame->width;
        meta.rcBottom = frame->height;
        
        return frame;
    }

    std::shared_ptr<QuickView::MappedFile> m_mappedFile;
    avifDecoder* m_decoder = nullptr;
    avifRGBImage m_rgb;
    uint32_t m_totalFrames = 0;
};

std::unique_ptr<IAnimationDecoder> CreateAvifAnimator() {
    return std::make_unique<AvifAnimator>();
}

} // namespace QuickView
