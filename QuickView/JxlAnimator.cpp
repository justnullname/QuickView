#include "pch.h"
#include "AnimationDecoder.h"
#include "MappedFile.h"
#include <jxl/decode.h>
#include <jxl/decode_cxx.h>
#include <jxl/resizable_parallel_runner.h>
#include <jxl/resizable_parallel_runner_cxx.h>

namespace QuickView {

class JxlAnimator : public IAnimationDecoder {
public:
    JxlAnimator() {
        m_runner = JxlResizableParallelRunnerMake(nullptr);
    }
    ~JxlAnimator() override {
        DestroyDecoder();
    }

    bool Initialize(std::shared_ptr<QuickView::MappedFile> file, QuickView::PixelFormat preferredFormat) override {
        m_mappedFile = file;
        return ResetAndParseBasicInfo();
    }

    std::shared_ptr<RawImageFrame> GetNextFrame() override {
        if (!m_decoder) return nullptr;

        JxlPixelFormat pixFmt = { 4, JXL_TYPE_UINT8, JXL_LITTLE_ENDIAN, 0 };
        
        uint8_t* pixels = nullptr;
        size_t bufferSize = 0;
        int stride = 0;
        
        uint32_t currentDelayMs = 100;
        
        for (;;) {
            JxlDecoderStatus st = JxlDecoderProcessInput(m_decoder.get());
            
            if (st == JXL_DEC_ERROR) {
                if (pixels) _aligned_free(pixels);
                return nullptr;
            }
            if (st == JXL_DEC_NEED_MORE_INPUT) {
                if (pixels) _aligned_free(pixels);
                return nullptr;
            }
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
                size_t reqSize;
                JxlDecoderImageOutBufferSize(m_decoder.get(), &pixFmt, &reqSize);
                
                stride = (m_basicInfo.xsize * 4 + 15) & ~15;
                bufferSize = (size_t)stride * m_basicInfo.ysize;
                pixels = (uint8_t*)_aligned_malloc(bufferSize, 64);
                if (!pixels) return nullptr;
                
                // Set the output buffer for coalesced frame
                JxlDecoderSetImageOutBuffer(m_decoder.get(), &pixFmt, pixels, bufferSize);
            }
            else if (st == JXL_DEC_FULL_IMAGE) {
                // Done decoding one fully composited frame
                break;
            }
            else if (st == JXL_DEC_SUCCESS) {
                // All frames decoded
                if (pixels) _aligned_free(pixels);
                m_isFinished = true;
                return nullptr;
            }
        }
        
        if (!pixels) return nullptr;
        
        auto frame = std::make_shared<RawImageFrame>();
        frame->width = m_basicInfo.xsize;
        frame->height = m_basicInfo.ysize;
        frame->stride = stride;
        frame->format = PixelFormat::BGRA8888;
        frame->quality = DecodeQuality::Full;
        frame->pixels = pixels;
        frame->memoryDeleter = [](uint8_t* p) { _aligned_free(p); };
        
        // Convert RGBA to BGRA
        for (int y = 0; y < frame->height; ++y) {
            uint32_t* row = reinterpret_cast<uint32_t*>(frame->pixels + (size_t)y * frame->stride);
            for (int x = 0; x < frame->width; ++x) {
                uint32_t rgba = row[x];
                // rgba is ABGR in little endian: MSB A B G R LSB
                // wait, if it's JXL_TYPE_UINT8, memory is [R, G, B, A]
                uint8_t* p = reinterpret_cast<uint8_t*>(&row[x]);
                uint8_t r = p[0];
                uint8_t g = p[1];
                uint8_t b = p[2];
                uint8_t a = p[3];
                row[x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
            }
        }
        
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
                if (m_basicInfo.have_animation) {
                    m_animInfo = m_basicInfo.animation;
                    // Pre-scan for total frames (very fast since we don't decode pixels)
                    m_totalFrames = ScanTotalFrames();
                }
                foundBasicInfo = true;
                break; 
            }
        }
        
        return foundBasicInfo && (m_basicInfo.have_animation == JXL_TRUE);
    }

    std::shared_ptr<QuickView::MappedFile> m_mappedFile;
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
