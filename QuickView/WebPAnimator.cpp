#include "AnimationDecoder.h"
#include "MappedFile.h"
#include <algorithm>
#include <memory>
#include <webp/demux.h>
#include <webp/decode.h>
#include <vector>
#include <thread>
#include <shared_mutex>
#include <stop_token>

namespace QuickView {

class WebPAnimator : public IAnimationDecoder {
public:
    WebPAnimator() {
        memset(&m_iter, 0, sizeof(m_iter));
    }
    
    ~WebPAnimator() override {
        ReleaseIterator();
        if (m_demux) {
            WebPDemuxDelete(m_demux);
        }
    }

    bool Initialize(std::shared_ptr<QuickView::MappedFile> file, QuickView::PixelFormat /*preferredFormat*/) override {
        if (!file || !file->IsValid()) return false;
        m_mappedFile = file;
        ReleaseIterator();
        if (m_demux) {
            WebPDemuxDelete(m_demux);
            m_demux = nullptr;
        }

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
        m_iterInitialized = true;

        m_canvas.resize((size_t)m_width * m_height * 4, 0); // BGRA Canvas, clear to 0
        // Defer background indexer until non-zero seek to avoid cold-boot CPU/memory contention.
        return true;
    }

    std::shared_ptr<RawImageFrame> GetNextFrame() override {
        if (!m_iterInitialized || m_currentIndex >= m_totalFrames) return nullptr;
        
        // 1. Process previous frame disposal if needed
        if (m_currentIndex > 0 && m_lastDisposal == WEBP_MUX_DISPOSE_BACKGROUND) {
            ClearRectTransparent(m_canvas, m_width, m_height, m_lastRect);
        }
        
        // 2. Decode current frame fragment
        int w = 0, h = 0;
        uint8_t* rgba = WebPDecodeBGRA(m_iter.fragment.bytes, m_iter.fragment.size, &w, &h);
        if (!rgba) return nullptr;
        
        // 3. Blend onto canvas with bounds clamping
        int x_offset = m_iter.x_offset;
        int y_offset = m_iter.y_offset;
        CompositeFragmentBGRA(m_canvas, m_width, m_height, rgba, w, h, x_offset, y_offset, m_iter.blend_method);
        
        WebPFree(rgba);
        
        // 4. Update Snapshot Checkpoints is now handled by the background indexer
        // 5. Output
        auto frame = std::make_shared<RawImageFrame>();
        size_t bufSize = m_canvas.size();
        frame->pixels = (uint8_t*)_aligned_malloc(bufSize, 64);
        if (!frame->pixels) return nullptr;
        frame->memoryDeleter = QuickView::MemoryDeleter::FromAlignedFree();
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
        if (m_totalFrames == 0) return nullptr;
        if (targetIndex >= m_totalFrames) targetIndex = m_totalFrames - 1;
        if (targetIndex > 0) {
            EnsureIndexerStarted();
        }
        if (targetIndex == m_currentIndex) return GetNextFrame();
        
        uint32_t bestIndex = 0;
        if (m_currentIndex <= targetIndex) {
            bestIndex = m_currentIndex;
        }

        int bestSnap = -1;
        std::shared_lock<std::shared_mutex> lock(m_snapshotMutex);
        for (int i = (int)m_snapshots.size() - 1; i >= 0; i--) {
            if (m_snapshots[i].index <= targetIndex) {
                bestSnap = i;
                break;
            }
        }
        
        if (bestSnap >= 0 && m_snapshots[bestSnap].index > bestIndex) {
            auto& snap = m_snapshots[bestSnap];
            m_currentIndex = snap.index;
            m_canvas = snap.canvas;
            ReleaseIterator();
            m_iterInitialized = WebPDemuxGetFrame(m_demux, snap.demuxIndex, &m_iter) != 0;
            m_lastDisposal = snap.lastDisposal;
            m_lastRect = snap.lastRect;
        } else if (m_currentIndex > targetIndex) {
            m_currentIndex = 0;
            memset(m_canvas.data(), 0, m_canvas.size());
            ReleaseIterator();
            m_iterInitialized = WebPDemuxGetFrame(m_demux, 1, &m_iter) != 0;
            m_lastDisposal = WEBP_MUX_DISPOSE_NONE;
            m_lastRect = {0,0,0,0};
        }
        lock.unlock();
        
        std::shared_ptr<RawImageFrame> lastFrame = nullptr;
        while (m_currentIndex < targetIndex) {
            lastFrame = GetNextFrame();
            if (!lastFrame) break;
        }
        
        return GetNextFrame();
    }

    uint32_t GetTotalFrames() const override { return m_totalFrames; }
    bool IsAnimated() const override { return m_totalFrames > 1; }
    bool SupportsDirtyRect() const override { return true; }

private:
    struct Rect { int x, y, width, height; };

    void ReleaseIterator() {
        if (m_iterInitialized) {
            WebPDemuxReleaseIterator(&m_iter);
            memset(&m_iter, 0, sizeof(m_iter));
            m_iterInitialized = false;
        }
    }

    static void ClearRectTransparent(std::vector<uint8_t>& canvas, uint32_t canvasW, uint32_t canvasH, const Rect& rect) {
        int x0 = std::clamp(rect.x, 0, (int)canvasW);
        int y0 = std::clamp(rect.y, 0, (int)canvasH);
        int x1 = std::clamp(rect.x + rect.width, 0, (int)canvasW);
        int y1 = std::clamp(rect.y + rect.height, 0, (int)canvasH);
        if (x1 <= x0 || y1 <= y0) return;

        size_t clearBytes = (size_t)(x1 - x0) * 4;
        for (int y = y0; y < y1; y++) {
            uint8_t* row = canvas.data() + ((size_t)y * canvasW + (size_t)x0) * 4;
            memset(row, 0, clearBytes);
        }
    }

    static void CompositeFragmentBGRA(
        std::vector<uint8_t>& canvas, uint32_t canvasW, uint32_t canvasH,
        const uint8_t* rgba, int fragW, int fragH, int xOffset, int yOffset, WebPMuxAnimBlend blendMethod)
    {
        if (!rgba || fragW <= 0 || fragH <= 0) return;
        int x0 = std::clamp(xOffset, 0, (int)canvasW);
        int y0 = std::clamp(yOffset, 0, (int)canvasH);
        int x1 = std::clamp(xOffset + fragW, 0, (int)canvasW);
        int y1 = std::clamp(yOffset + fragH, 0, (int)canvasH);
        if (x1 <= x0 || y1 <= y0) return;

        int copyW = x1 - x0;
        int srcStartX = x0 - xOffset;
        if (blendMethod == WEBP_MUX_NO_BLEND) {
            for (int y = y0; y < y1; y++) {
                uint8_t* dstRow = canvas.data() + ((size_t)y * canvasW + (size_t)x0) * 4;
                const uint8_t* srcRow = rgba + ((size_t)(y - yOffset) * fragW + (size_t)srcStartX) * 4;
                for (int x = 0; x < copyW; x++) {
                    uint8_t a = srcRow[x * 4 + 3];
                    if (a == 255) {
                        dstRow[x * 4 + 0] = srcRow[x * 4 + 0];
                        dstRow[x * 4 + 1] = srcRow[x * 4 + 1];
                        dstRow[x * 4 + 2] = srcRow[x * 4 + 2];
                        dstRow[x * 4 + 3] = 255;
                    } else if (a == 0) {
                        dstRow[x * 4 + 0] = 0;
                        dstRow[x * 4 + 1] = 0;
                        dstRow[x * 4 + 2] = 0;
                        dstRow[x * 4 + 3] = 0;
                    } else {
                        dstRow[x * 4 + 0] = static_cast<uint8_t>((srcRow[x * 4 + 0] * a + 127) / 255);
                        dstRow[x * 4 + 1] = static_cast<uint8_t>((srcRow[x * 4 + 1] * a + 127) / 255);
                        dstRow[x * 4 + 2] = static_cast<uint8_t>((srcRow[x * 4 + 2] * a + 127) / 255);
                        dstRow[x * 4 + 3] = a;
                    }
                }
            }
        } else {
            for (int y = y0; y < y1; y++) {
                uint8_t* dstRow = canvas.data() + ((size_t)y * canvasW + (size_t)x0) * 4;
                const uint8_t* srcRow = rgba + ((size_t)(y - yOffset) * fragW + (size_t)srcStartX) * 4;
                for (int x = 0; x < copyW; x++) {
                    uint8_t a = srcRow[x * 4 + 3];
                    if (a == 255) {
                        dstRow[x * 4 + 0] = srcRow[x * 4 + 0];
                        dstRow[x * 4 + 1] = srcRow[x * 4 + 1];
                        dstRow[x * 4 + 2] = srcRow[x * 4 + 2];
                        dstRow[x * 4 + 3] = 255;
                    } else if (a > 0) {
                        uint8_t inv_a = 255 - a;
                        uint32_t src_b = (srcRow[x * 4 + 0] * a + 127) / 255;
                        uint32_t src_g = (srcRow[x * 4 + 1] * a + 127) / 255;
                        uint32_t src_r = (srcRow[x * 4 + 2] * a + 127) / 255;

                        dstRow[x * 4 + 0] = static_cast<uint8_t>(std::min<int>(255, src_b + (dstRow[x * 4 + 0] * inv_a + 127) / 255));
                        dstRow[x * 4 + 1] = static_cast<uint8_t>(std::min<int>(255, src_g + (dstRow[x * 4 + 1] * inv_a + 127) / 255));
                        dstRow[x * 4 + 2] = static_cast<uint8_t>(std::min<int>(255, src_r + (dstRow[x * 4 + 2] * inv_a + 127) / 255));
                        dstRow[x * 4 + 3] = static_cast<uint8_t>(std::min<int>(255, a + (dstRow[x * 4 + 3] * inv_a + 127) / 255));
                    }
                }
            }
        }
    }

    void EnsureIndexerStarted() {
        std::call_once(m_indexerOnce, [this]() {
            m_indexerThread = std::jthread([this](std::stop_token st) {
                BackgroundIndexer(st);
            });
        });
    }

    std::shared_ptr<QuickView::MappedFile> m_mappedFile;
    WebPDemuxer* m_demux = nullptr;
    WebPIterator m_iter;
    bool m_iterInitialized = false;
    uint32_t m_totalFrames = 0;
    uint32_t m_currentIndex = 0;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    
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
    std::shared_mutex m_snapshotMutex;
    std::once_flag m_indexerOnce;
    std::jthread m_indexerThread;

    void BackgroundIndexer(std::stop_token st) {
        if (!m_mappedFile || !m_mappedFile->IsValid()) return;
        WebPData webp_data;
        webp_data.bytes = m_mappedFile->data();
        webp_data.size = m_mappedFile->size();

        WebPDemuxer* bgDemux = WebPDemux(&webp_data);
        if (!bgDemux) return;

        WebPIterator bgIter;
        memset(&bgIter, 0, sizeof(bgIter));
        if (!WebPDemuxGetFrame(bgDemux, 1, &bgIter)) {
            WebPDemuxDelete(bgDemux);
            return;
        }

        std::vector<uint8_t> bgCanvas((size_t)m_width * m_height * 4, 0);
        uint32_t bgCurrentIndex = 0;
        WebPMuxAnimDispose bgLastDisposal = WEBP_MUX_DISPOSE_NONE;
        Rect bgLastRect = {0,0,0,0};

        uint32_t snapshotInterval = 20;
        size_t memoryLimit = 256 * 1024 * 1024; // 256MB
        size_t currentMemory = 0;

        while (!st.stop_requested() && bgCurrentIndex < m_totalFrames) {
            if (bgCurrentIndex > 0 && bgLastDisposal == WEBP_MUX_DISPOSE_BACKGROUND) {
                ClearRectTransparent(bgCanvas, m_width, m_height, bgLastRect);
            }

            // Capture snapshot AFTER applying previous frame's disposal and BEFORE decoding bgCurrentIndex.
            if (bgCurrentIndex % snapshotInterval == 0) {
                Snapshot snap;
                snap.index = bgCurrentIndex;
                snap.canvas = bgCanvas;
                snap.demuxIndex = bgIter.frame_num;
                snap.lastDisposal = WEBP_MUX_DISPOSE_NONE;
                snap.lastRect = {0, 0, 0, 0};

                size_t snapSize = snap.canvas.size();

                std::unique_lock<std::shared_mutex> lock(m_snapshotMutex);
                m_snapshots.push_back(std::move(snap));
                currentMemory += snapSize;

                if (currentMemory > memoryLimit) {
                    std::vector<Snapshot> newSnapshots;
                    currentMemory = 0;
                    for (size_t i = 0; i < m_snapshots.size(); i += 2) {
                        newSnapshots.push_back(std::move(m_snapshots[i]));
                        currentMemory += newSnapshots.back().canvas.size();
                    }
                    m_snapshots = std::move(newSnapshots);
                    snapshotInterval *= 2;
                }
                lock.unlock();
            }

            int w = 0, h = 0;
            uint8_t* rgba = WebPDecodeBGRA(bgIter.fragment.bytes, bgIter.fragment.size, &w, &h);
            if (rgba) {
                CompositeFragmentBGRA(bgCanvas, m_width, m_height, rgba, w, h, bgIter.x_offset, bgIter.y_offset, bgIter.blend_method);
                WebPFree(rgba);
            }

            bgLastDisposal = bgIter.dispose_method;
            bgLastRect = { bgIter.x_offset, bgIter.y_offset, bgIter.width, bgIter.height };
            
            bgCurrentIndex++;
            if (!WebPDemuxNextFrame(&bgIter)) break;
        }

        WebPDemuxReleaseIterator(&bgIter);
        WebPDemuxDelete(bgDemux);
    }
};

std::unique_ptr<IAnimationDecoder> CreateWebPAnimator() {
    return std::make_unique<WebPAnimator>();
}

} // namespace QuickView
