#include "pch.h"
#include "AnimationDecoder.h"
#include "MappedFile.h"
#include <jxl/decode.h>
#include <jxl/decode_cxx.h>
#include <jxl/resizable_parallel_runner.h>
#include <jxl/resizable_parallel_runner_cxx.h>
#include "ImageLoaderSimd.h"
#include <vector>

namespace QuickView {

class JxlAnimator : public IAnimationDecoder {
public:
    JxlAnimator() {
        m_runner = JxlResizableParallelRunnerMake(nullptr);
    }
    ~JxlAnimator() override {
        DestroyDecoder();
        if (m_accumulationBuffer) _aligned_free(m_accumulationBuffer);
    }

    bool Initialize(std::shared_ptr<QuickView::MappedFile> file, QuickView::PixelFormat preferredFormat) override {
        m_mappedFile = file;
        return ResetAndParseBasicInfo();
    }

    std::shared_ptr<RawImageFrame> GetNextFrame() override {
        if (!m_decoder) return nullptr;

        JxlPixelFormat pixFmt = { 4, JXL_TYPE_UINT8, JXL_LITTLE_ENDIAN, 0 };
        
        uint32_t currentDelayMs = 100;
        
        for (;;) {
            JxlDecoderStatus st = JxlDecoderProcessInput(m_decoder.get());
            
            if (st == JXL_DEC_ERROR) return nullptr;
            if (st == JXL_DEC_NEED_MORE_INPUT) return nullptr;
            
            if (st == JXL_DEC_COLOR_ENCODING) {
                JxlColorEncoding ce = {};
                ce.color_space = JXL_COLOR_SPACE_RGB;
                ce.white_point = JXL_WHITE_POINT_D65;
                ce.primaries = JXL_PRIMARIES_SRGB;
                ce.transfer_function = JXL_TRANSFER_FUNCTION_SRGB;
                ce.rendering_intent = JXL_RENDERING_INTENT_PERCEPTUAL;
                JxlDecoderSetOutputColorProfile(m_decoder.get(), &ce, nullptr, 0);
            }
            else if (st == JXL_DEC_FRAME) {
                JxlFrameHeader frameHeader;
                if (JXL_DEC_SUCCESS == JxlDecoderGetFrameHeader(m_decoder.get(), &frameHeader)) {
                    if (m_animInfo.tps_numerator > 0) {
                        double seconds = (double)frameHeader.duration * (double)m_animInfo.tps_denominator / (double)m_animInfo.tps_numerator;
                        currentDelayMs = (uint32_t)(seconds * 1000.0);
                        if (currentDelayMs < 10) currentDelayMs = 100;
                    }
                }
            }
            else if (st == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
                // Point libjxl to our persistent accumulation buffer.
                // Since coalescing is ON, libjxl blends the current frame onto this buffer.
                if (JXL_DEC_SUCCESS != JxlDecoderSetImageOutBuffer(m_decoder.get(), &pixFmt, m_accumulationBuffer, m_accumulationBufferSize)) {
                    return nullptr;
                }
            }
            else if (st == JXL_DEC_FULL_IMAGE) {
                // Done decoding one fully composited frame into m_accumulationBuffer
                break;
            }
            else if (st == JXL_DEC_SUCCESS) {
                m_isFinished = true;
                return nullptr;
            }
        }
        
        // At this point, m_accumulationBuffer contains the fully composited RGBA frame.
        auto frame = std::make_shared<RawImageFrame>();
        frame->width = m_basicInfo.xsize;
        frame->height = m_basicInfo.ysize;
        frame->stride = (frame->width * 4 + 15) & ~15;
        frame->format = PixelFormat::BGRA8888;
        frame->quality = DecodeQuality::Full;
        
        size_t outSize = (size_t)frame->stride * frame->height;
        frame->pixels = (uint8_t*)_aligned_malloc(outSize, 64);
        if (!frame->pixels) return nullptr;
        frame->memoryDeleter = [](uint8_t* p) { _aligned_free(p); };
        
        // Copy from accumulation buffer
        // Note: libjxl output was RGBA (from pixFmt), we want BGRA.
        // Copy row by row to handle potential stride differences between accumulation buffer and frame.
        int accStride = (frame->width * 4 + 15) & ~15; 
        for (int y = 0; y < frame->height; ++y) {
            memcpy(frame->pixels + (size_t)y * frame->stride, m_accumulationBuffer + (size_t)y * accStride, (size_t)frame->width * 4);
        }
        
        // SIMD Optimized RGBA -> BGRA swizzle
        ImageLoaderSimd::SwizzleRGBAToBGRA(frame->pixels, (size_t)frame->width * frame->height);
        
        auto& meta = frame->frameMeta;
        meta.index = m_currentIndex++;
        meta.delayMs = currentDelayMs;
        meta.totalFrames = m_totalFrames; 
        meta.disposal = FrameDisposalMode::Keep; // Coalesced frames don't need disposal
        meta.isDelta = false;
        meta.rcLeft = 0;
        meta.rcTop = 0;
        meta.rcRight = frame->width;
        meta.rcBottom = frame->height;
        
        return frame;
    }

    std::shared_ptr<RawImageFrame> SeekToFrame(uint32_t targetIndex) override {
        if (targetIndex == m_currentIndex) {
            // Already at the right index before GetNextFrame, actually we need to decode targetIndex
            // Wait, GetNextFrame() returns m_currentIndex and increments.
            // If targetIndex == m_currentIndex, we just call GetNextFrame.
            // But if targetIndex < m_currentIndex, we must restart.
        }
        
        if (targetIndex < m_currentIndex) {
            ResetAndParseBasicInfo(); // Restart
        }
        
        std::shared_ptr<RawImageFrame> frame;
        while (m_currentIndex <= targetIndex) {
            frame = GetNextFrame();
            if (!frame || m_isFinished) break;
        }
        return frame;
    }

    uint32_t GetTotalFrames() const override { 
        return m_totalFrames;
    }
    
    bool IsAnimated() const override { return m_basicInfo.have_animation == JXL_TRUE; }

private:
    void DestroyDecoder() {
        m_decoder.reset();
    }

    bool ResetAndParseBasicInfo() {
        m_decoder = JxlDecoderMake(nullptr);
        m_currentIndex = 0;
        m_isFinished = false;
        
        if (m_runner) {
            JxlResizableParallelRunnerSetThreads(m_runner.get(), 1); // FastLane is single-threaded
            JxlDecoderSetParallelRunner(m_decoder.get(), JxlResizableParallelRunner, m_runner.get());
        }

        if (JXL_DEC_SUCCESS != JxlDecoderSubscribeEvents(m_decoder.get(), 
            JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE)) {
            return false;
        }
        
        // [BugFix] Explicitly enable coalescing for transparent animations to avoid artifacts
        JxlDecoderSetCoalescing(m_decoder.get(), JXL_TRUE);

        auto rv = JxlDecoderSetInput(m_decoder.get(), m_mappedFile->data(), m_mappedFile->size());
        if (rv != JXL_DEC_SUCCESS) return false;
        
        JxlDecoderCloseInput(m_decoder.get());
        
        // Coalescing is ON by default in libjxl. We explicitly want the fully composited frames.
        
        bool foundBasicInfo = false;
        for (;;) {
            JxlDecoderStatus st = JxlDecoderProcessInput(m_decoder.get());
            if (st == JXL_DEC_ERROR) return false;
            if (st == JXL_DEC_BASIC_INFO) {
                if (JXL_DEC_SUCCESS != JxlDecoderGetBasicInfo(m_decoder.get(), &m_basicInfo)) return false;
                
                // Allocate or re-allocate accumulation buffer
                size_t stride = (m_basicInfo.xsize * 4 + 15) & ~15;
                size_t neededSize = stride * m_basicInfo.ysize;
                if (!m_accumulationBuffer || m_accumulationBufferSize < neededSize) {
                    if (m_accumulationBuffer) _aligned_free(m_accumulationBuffer);
                    m_accumulationBuffer = (uint8_t*)_aligned_malloc(neededSize, 64);
                    m_accumulationBufferSize = neededSize;
                }
                if (m_accumulationBuffer) {
                    memset(m_accumulationBuffer, 0, m_accumulationBufferSize);
                } else {
                    return false;
                }

                if (m_basicInfo.have_animation) {
                    m_animInfo = m_basicInfo.animation;
                    // Pre-scan for total frames (very fast since we don't decode pixels)
                    m_totalFrames = ScanTotalFrames();
                }
                foundBasicInfo = true;
                break; 
            }
        }
        
        // [BUGFIX] Many single-frame JXLs have the animation flag set but only contain 1 frame.
        // We enforce totalFrames > 1 to truly consider it an animation and avoid animating a static image.
        if (foundBasicInfo && m_basicInfo.have_animation && m_totalFrames > 1) {
            return true;
        }
        
        return false;
    }

    std::shared_ptr<QuickView::MappedFile> m_mappedFile;
    uint8_t* m_accumulationBuffer = nullptr;
    size_t m_accumulationBufferSize = 0;
    JxlDecoderPtr m_decoder;
    JxlResizableParallelRunnerPtr m_runner;
    JxlBasicInfo m_basicInfo = {};
    JxlAnimationHeader m_animInfo = {};
    uint32_t m_currentIndex = 0;
    uint32_t m_totalFrames = 0;
    bool m_isFinished = false;
    
    uint32_t ScanTotalFrames() {
        auto scanDec = JxlDecoderMake(nullptr);
        if (JXL_DEC_SUCCESS != JxlDecoderSubscribeEvents(scanDec.get(), JXL_DEC_FRAME)) return 0;
        if (JXL_DEC_SUCCESS != JxlDecoderSetInput(scanDec.get(), m_mappedFile->data(), m_mappedFile->size())) return 0;
        JxlDecoderCloseInput(scanDec.get());

        uint32_t count = 0;
        for (;;) {
            JxlDecoderStatus st = JxlDecoderProcessInput(scanDec.get());
            if (st == JXL_DEC_FRAME) count++;
            else if (st == JXL_DEC_SUCCESS) break;
            else if (st == JXL_DEC_ERROR) break;
            else if (st == JXL_DEC_NEED_MORE_INPUT) break;
        }
        return count;
    }
};

std::unique_ptr<IAnimationDecoder> CreateJxlAnimator() {
    return std::make_unique<JxlAnimator>();
}

} // namespace QuickView
