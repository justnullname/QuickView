#include "pch.h"
#include "AnimationDecoder.h"
#include "MappedFile.h"
#include <webp/demux.h>
#include <webp/decode.h>
#include <vector>

namespace QuickView {

class WebPAnimator : public IAnimationDecoder {
public:
    WebPAnimator() {
        memset(&m_iter, 0, sizeof(m_iter));
    }
    
    ~WebPAnimator() override {
        if (m_iter.has_alpha || m_iter.width > 0) { // basic init check
            WebPDemuxReleaseIterator(&m_iter);
        }
        if (m_demux) {
            WebPDemuxDelete(m_demux);
        }
    }

    bool Initialize(std::shared_ptr<QuickView::MappedFile> file, QuickView::PixelFormat preferredFormat) override {
        m_mappedFile = file;
        WebPData webp_data;
        webp_data.bytes = file->data();
        webp_data.size = file->size();

        m_demux = WebPDemux(&webp_data);
        if (!m_demux) return false;

        m_totalFrames = WebPDemuxGetI(m_demux, WEBP_FF_FRAME_COUNT);
        if (m_totalFrames <= 1) return false; // Not animated
        
        uint32_t flags = WebPDemuxGetI(m_demux, WEBP_FF_FORMAT_FLAGS);
        if (!(flags & ANIMATION_FLAG)) return false;

        m_width = WebPDemuxGetI(m_demux, WEBP_FF_CANVAS_WIDTH);
        m_height = WebPDemuxGetI(m_demux, WEBP_FF_CANVAS_HEIGHT);
        if (m_width == 0 || m_height == 0) return false;

        if (!WebPDemuxGetFrame(m_demux, 1, &m_iter)) return false; // 1-based index

        m_canvas.resize((size_t)m_width * m_height * 4, 0); // BGRA Canvas, clear to 0
        return true;
    }

    std::shared_ptr<RawImageFrame> GetNextFrame() override {
        if (m_currentIndex >= m_totalFrames) return nullptr;
        
        // 1. Process previous frame disposal if needed
        if (m_currentIndex > 0) {
            if (m_lastDisposal == WEBP_MUX_DISPOSE_BACKGROUND) {
                // Clear the previous frame's rect to transparent
                for (int y = 0; y < m_lastRect.height; y++) {
                    uint8_t* row = m_canvas.data() + (m_lastRect.y + y) * (m_width * 4) + m_lastRect.x * 4;
                    memset(row, 0, m_lastRect.width * 4);
                }
            } else if (m_lastDisposal == WEBP_MUX_DISPOSE_NONE) {
                // Keep
            }
        }
        
        // 2. Decode current frame fragment
        int w, h;
        uint8_t* rgba = WebPDecodeBGRA(m_iter.fragment.bytes, m_iter.fragment.size, &w, &h);
        if (!rgba) return nullptr;
        
        // 3. Blend onto canvas
        int x_offset = m_iter.x_offset;
        int y_offset = m_iter.y_offset;
        
        if (m_iter.blend_method == WEBP_MUX_NO_BLEND) {
            // Overwrite
            for (int y = 0; y < h; y++) {
                memcpy(m_canvas.data() + (y_offset + y) * (m_width * 4) + x_offset * 4,
                       rgba + y * (w * 4), w * 4);
            }
        } else {
            // Alpha Blend (SRC_OVER)
            for (int y = 0; y < h; y++) {
                uint8_t* dstRow = m_canvas.data() + (y_offset + y) * (m_width * 4) + x_offset * 4;
                const uint8_t* srcRow = rgba + y * (w * 4);
                for (int x = 0; x < w; x++) {
                    uint8_t a = srcRow[x*4 + 3];
                    if (a == 255) {
                        dstRow[x*4+0] = srcRow[x*4+0];
                        dstRow[x*4+1] = srcRow[x*4+1];
                        dstRow[x*4+2] = srcRow[x*4+2];
                        dstRow[x*4+3] = 255;
                    } else if (a > 0) {
                        uint8_t inv_a = 255 - a;
                        dstRow[x*4+0] = (srcRow[x*4+0] * 255 + dstRow[x*4+0] * inv_a) / 255;
                        dstRow[x*4+1] = (srcRow[x*4+1] * 255 + dstRow[x*4+1] * inv_a) / 255;
                        dstRow[x*4+2] = (srcRow[x*4+2] * 255 + dstRow[x*4+2] * inv_a) / 255;
                        uint32_t outA = a + dstRow[x*4+3] * inv_a / 255;
                        dstRow[x*4+3] = std::min(255u, outA);
                    }
                }
            }
        }
        
        WebPFree(rgba);
        
        // 4. Update Snapshot Checkpoints (sparse)
        if (m_currentIndex % 20 == 0) {
            Snapshot snap;
            snap.index = m_currentIndex;
            snap.canvas = m_canvas;
            snap.demuxIndex = m_iter.frame_num; // 1-based internal index
            snap.lastDisposal = m_iter.dispose_method;
            snap.lastRect = { m_iter.x_offset, m_iter.y_offset, m_iter.width, m_iter.height };
            m_snapshots.push_back(std::move(snap));
        }

        // 5. Output
        auto frame = std::make_shared<RawImageFrame>();
        size_t bufSize = m_canvas.size();
        frame->pixels = (uint8_t*)_aligned_malloc(bufSize, 64);
        frame->memoryDeleter = [](uint8_t* p) { _aligned_free(p); };
        memcpy(frame->pixels, m_canvas.data(), bufSize);
        
        frame->width = m_width;
        frame->height = m_height;
        frame->stride = m_width * 4; // Canvas stride is always width * 4
        frame->format = PixelFormat::BGRA8888;
        frame->quality = DecodeQuality::Full;
        
        auto& meta = frame->frameMeta;
        meta.index = m_currentIndex;
        meta.delayMs = m_iter.duration;
        if (meta.delayMs == 0) meta.delayMs = 100;
        meta.totalFrames = m_totalFrames;
        if (m_iter.dispose_method == WEBP_MUX_DISPOSE_BACKGROUND) meta.disposal = FrameDisposalMode::RestoreBackground;
        else meta.disposal = FrameDisposalMode::Keep;
        meta.isDelta = true;
        meta.rcLeft = x_offset;
        meta.rcTop = y_offset;
        meta.rcRight = x_offset + w;
        meta.rcBottom = y_offset + h;
        
        m_lastDisposal = m_iter.dispose_method;
        m_lastRect = { x_offset, y_offset, w, h };
        
        m_currentIndex++;
        WebPDemuxNextFrame(&m_iter);
        
        return frame;
    }

    std::shared_ptr<RawImageFrame> SeekToFrame(uint32_t targetIndex) override {
        if (targetIndex >= m_totalFrames) targetIndex = m_totalFrames - 1;
        if (targetIndex == m_currentIndex) return GetNextFrame();
        
        int bestSnap = -1;
        for (int i = (int)m_snapshots.size() - 1; i >= 0; i--) {
            if (m_snapshots[i].index <= targetIndex) {
                bestSnap = i;
                break;
            }
        }
        
        if (bestSnap >= 0) {
            auto& snap = m_snapshots[bestSnap];
            m_currentIndex = snap.index;
            m_canvas = snap.canvas;
            WebPDemuxGetFrame(m_demux, snap.demuxIndex, &m_iter);
            m_lastDisposal = snap.lastDisposal;
            m_lastRect = snap.lastRect;
        } else {
            m_currentIndex = 0;
            memset(m_canvas.data(), 0, m_canvas.size());
            WebPDemuxGetFrame(m_demux, 1, &m_iter);
            m_lastDisposal = WEBP_MUX_DISPOSE_NONE;
            m_lastRect = {0,0,0,0};
        }
        
        std::shared_ptr<RawImageFrame> lastFrame = nullptr;
        while (m_currentIndex < targetIndex) {
            lastFrame = GetNextFrame();
            if (!lastFrame) break;
        }
        
        return GetNextFrame();
    }

    uint32_t GetTotalFrames() const override { return m_totalFrames; }
    bool IsAnimated() const override { return true; }

private:
    std::shared_ptr<QuickView::MappedFile> m_mappedFile;
    WebPDemuxer* m_demux = nullptr;
    WebPIterator m_iter;
    uint32_t m_totalFrames = 0;
    uint32_t m_currentIndex = 0;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    
    struct Rect { int x, y, width, height; };
    WebPMuxAnimDispose m_lastDisposal = WEBP_MUX_DISPOSE_NONE;
    Rect m_lastRect = {0,0,0,0};
    
    std::vector<uint8_t> m_canvas;
    
    struct Snapshot {
        uint32_t index;
        std::vector<uint8_t> canvas;
        int demuxIndex;
        WebPMuxAnimDispose lastDisposal;
        Rect lastRect;
    };
    std::vector<Snapshot> m_snapshots;
};

std::unique_ptr<IAnimationDecoder> CreateWebPAnimator() {
    return std::make_unique<WebPAnimator>();
}

} // namespace QuickView
