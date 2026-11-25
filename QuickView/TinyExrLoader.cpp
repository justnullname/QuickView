#include "pch.h"
#include "TinyExrLoader.h"
#include "QuickViewETW.h"
#include <exr.h>
#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <string_view>

static constexpr const char* CURRENT_MODULE = "TinyExrLoader";

namespace TinyExrLoader {
namespace {

struct ReaderGuard {
    exr_reader* r = nullptr;
    ~ReaderGuard() {
        if (r) exr_reader_close(r);
    }
};

struct PartGuard {
    exr_part part{};
    ~PartGuard() {
        exr_part_free(nullptr, &part);
    }
};

int64_t FloorDiv(int64_t a, int64_t b) {
    int64_t q = a / b;
    int64_t r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) {
        --q;
    }
    return q;
}

int NumSamples(int64_t lo, int64_t hi, int32_t step) {
    if (step <= 0 || hi < lo) return 0;
    int64_t n = (step == 1)
        ? (hi - lo + 1)
        : (FloorDiv(hi, step) - FloorDiv(lo - 1, step));
    return (n > 0 && n <= INT32_MAX) ? static_cast<int>(n) : 0;
}

int SampledIndex(int32_t origin, int32_t pos, int32_t step, int count) {
    if (count <= 1 || step <= 1) {
        return std::clamp(pos, 0, count > 0 ? count - 1 : 0);
    }
    int64_t first = -FloorDiv(-static_cast<int64_t>(origin), step) * step;
    int64_t idx = FloorDiv(static_cast<int64_t>(origin) + pos - first, step);
    if (idx < 0) return 0;
    if (idx >= count) return count - 1;
    return static_cast<int>(idx);
}

int FindFirstFlatPart(const exr_reader* r) {
    int32_t numParts = exr_reader_num_parts(r);
    for (int32_t i = 0; i < numParts; ++i) {
        const exr_header* hdr = exr_reader_part_header(r, i);
        if (hdr && (hdr->part_type == EXR_PART_SCANLINE || hdr->part_type == EXR_PART_TILED)) {
            return i;
        }
    }
    return -1;
}

void FillChromaticities(const exr_header* hdr, ExrChromaticities* outChroma) {
    if (!outChroma) return;
    *outChroma = {};
    if (!hdr || !hdr->has_chromaticities) return;

    outChroma->valid = true;
    outChroma->redX = hdr->chromaticities[0];
    outChroma->redY = hdr->chromaticities[1];
    outChroma->greenX = hdr->chromaticities[2];
    outChroma->greenY = hdr->chromaticities[3];
    outChroma->blueX = hdr->chromaticities[4];
    outChroma->blueY = hdr->chromaticities[5];
    outChroma->whiteX = hdr->chromaticities[6];
    outChroma->whiteY = hdr->chromaticities[7];
}

struct ChannelMap {
    int r = -1;
    int g = -1;
    int b = -1;
    int a = -1;
};

int FindChannelExact(const exr_header& hdr, std::string_view name) {
    for (int32_t i = 0; i < hdr.num_channels; ++i) {
        if (std::string_view(hdr.channels[i].name) == name) {
            return i;
        }
    }
    return -1;
}

ChannelMap ResolveChannels(const exr_header& hdr) {
    ChannelMap map;
    if (hdr.num_channels <= 0 || !hdr.channels) return map;

    map.r = FindChannelExact(hdr, "R");
    map.g = FindChannelExact(hdr, "G");
    map.b = FindChannelExact(hdr, "B");
    map.a = FindChannelExact(hdr, "A");

    if (map.r >= 0 || map.g >= 0 || map.b >= 0) {
        return map;
    }

    // Layered EXR fallback: look for first "<layer>.R" and match siblings "<layer>.{G,B,A}".
    for (int32_t i = 0; i < hdr.num_channels; ++i) {
        std::string_view cname(hdr.channels[i].name);
        if (cname.size() > 2 && cname.ends_with(".R")) {
            std::string_view prefix = cname.substr(0, cname.size() - 1); // includes trailing '.'
            map.r = i;
            for (int32_t j = 0; j < hdr.num_channels; ++j) {
                std::string_view sib(hdr.channels[j].name);
                if (sib.size() == prefix.size() + 1 && sib.starts_with(prefix)) {
                    char suffix = sib.back();
                    if (suffix == 'G') map.g = j;
                    else if (suffix == 'B') map.b = j;
                    else if (suffix == 'A') map.a = j;
                }
            }
            if (map.a < 0) {
                map.a = FindChannelExact(hdr, "A");
            }
            return map;
        }
    }

    // Grayscale fallback ("Y", "L", or channel 0).
    int gray = FindChannelExact(hdr, "Y");
    if (gray < 0) gray = FindChannelExact(hdr, "L");
    if (gray < 0) gray = 0;
    map.r = gray;
    map.g = gray;
    map.b = gray;
    return map;
}

float LoadSample(const void* plane, exr_pixel_type type, size_t idx) {
    if (!plane) return 0.0f;
    switch (type) {
    case EXR_PIXEL_HALF: {
        float f = 0.0f;
        exr_half_to_float(static_cast<const uint16_t*>(plane) + idx, &f, 1);
        return f;
    }
    case EXR_PIXEL_FLOAT:
        return static_cast<const float*>(plane)[idx];
    default:
        return static_cast<float>(static_cast<const uint32_t*>(plane)[idx]);
    }
}

void ReadRowUnpacked(const void* plane, exr_pixel_type type, size_t rowStartIdx,
                     int width, float* dst, float defaultVal) {
    if (!plane) {
        std::fill_n(dst, width, defaultVal);
        return;
    }
    switch (type) {
    case EXR_PIXEL_HALF:
        exr_half_to_float(static_cast<const uint16_t*>(plane) + rowStartIdx,
                          dst, static_cast<size_t>(width));
        break;
    case EXR_PIXEL_FLOAT:
        std::memcpy(dst, static_cast<const float*>(plane) + rowStartIdx,
                    static_cast<size_t>(width) * sizeof(float));
        break;
    default: {
        const uint32_t* src = static_cast<const uint32_t*>(plane) + rowStartIdx;
        for (int x = 0; x < width; ++x) {
            dst[x] = static_cast<float>(src[x]);
        }
        break;
    }
    }
}

bool IsUnitSampledChannel(const exr_part& part, int idx) {
    if (idx < 0) return true;
    const exr_channel& ch = part.header.channels[idx];
    return ch.x_sampling == 1 && ch.y_sampling == 1;
}

bool DecodePartToRgbaFloat(const exr_part& part, std::vector<float>& outData) {
    int32_t w = part.width;
    int32_t h = part.height;
    if (w <= 0 || h <= 0 || !part.images || !part.header.channels) {
        return false;
    }

    size_t pixelCount = static_cast<size_t>(w) * static_cast<size_t>(h);
    if (pixelCount > SIZE_MAX / 4) return false;

    // Luminance-chroma (Y / RY / BY) true-color reconstruction path
    if (exr_part_is_luminance_chroma(&part)) {
        float* ycRgba = nullptr;
        int ycW = 0;
        int ycH = 0;
        exr_result rc = exr_part_yc_to_rgba_float(nullptr, &part, &ycRgba, &ycW, &ycH);
        if (!EXR_OK(rc) || !ycRgba || ycW != w || ycH != h) {
            if (ycRgba) std::free(ycRgba);
            return false;
        }
        outData.resize(pixelCount * 4);
        std::memcpy(outData.data(), ycRgba, pixelCount * 4 * sizeof(float));
        std::free(ycRgba);
        return true;
    }

    ChannelMap map = ResolveChannels(part.header);
    outData.resize(pixelCount * 4);

    if (IsUnitSampledChannel(part, map.r) &&
        IsUnitSampledChannel(part, map.g) &&
        IsUnitSampledChannel(part, map.b) &&
        IsUnitSampledChannel(part, map.a)) {
        // Hot path: SIMD half->float conversion per scanline into reusable scratch
        std::vector<float> rowScratch(static_cast<size_t>(w) * 4);
        float* rowR = rowScratch.data();
        float* rowG = rowR + w;
        float* rowB = rowG + w;
        float* rowA = rowB + w;

        auto getPlane = [&](int idx) -> const void* {
            return (idx >= 0) ? part.images[idx] : nullptr;
        };
        auto getType = [&](int idx) -> exr_pixel_type {
            return (idx >= 0) ? part.header.channels[idx].pixel_type : EXR_PIXEL_FLOAT;
        };

        const void* planeR = getPlane(map.r);
        const void* planeG = getPlane(map.g);
        const void* planeB = getPlane(map.b);
        const void* planeA = getPlane(map.a);
        exr_pixel_type typeR = getType(map.r);
        exr_pixel_type typeG = getType(map.g);
        exr_pixel_type typeB = getType(map.b);
        exr_pixel_type typeA = getType(map.a);

        for (int32_t y = 0; y < h; ++y) {
            size_t rowOff = static_cast<size_t>(y) * static_cast<size_t>(w);
            ReadRowUnpacked(planeR, typeR, rowOff, w, rowR, 0.0f);
            if (map.g == map.r && planeG == planeR) {
                std::memcpy(rowG, rowR, static_cast<size_t>(w) * sizeof(float));
            } else {
                ReadRowUnpacked(planeG, typeG, rowOff, w, rowG, 0.0f);
            }
            if (map.b == map.r && planeB == planeR) {
                std::memcpy(rowB, rowR, static_cast<size_t>(w) * sizeof(float));
            } else {
                ReadRowUnpacked(planeB, typeB, rowOff, w, rowB, 0.0f);
            }
            ReadRowUnpacked(planeA, typeA, rowOff, w, rowA, 1.0f);

            float* dst = outData.data() + rowOff * 4;
            for (int32_t x = 0; x < w; ++x) {
                dst[0] = rowR[x];
                dst[1] = rowG[x];
                dst[2] = rowB[x];
                dst[3] = rowA[x];
                dst += 4;
            }
        }
        return true;
    }

    // General subsampled channel fallback
    struct PlaneInfo {
        const void* data = nullptr;
        exr_pixel_type type = EXR_PIXEL_FLOAT;
        int32_t xs = 1;
        int32_t ys = 1;
        int cw = 0;
        int ch = 0;
        float fallback = 0.0f;
    };

    auto makePlaneInfo = [&](int idx, float fallback) -> PlaneInfo {
        PlaneInfo info;
        info.fallback = fallback;
        if (idx < 0 || !part.images[idx]) return info;
        const exr_channel& c = part.header.channels[idx];
        int64_t x1 = static_cast<int64_t>(part.header.data_window.min_x) + w - 1;
        int64_t y1 = static_cast<int64_t>(part.header.data_window.min_y) + h - 1;
        info.cw = NumSamples(part.header.data_window.min_x, x1, c.x_sampling);
        info.ch = NumSamples(part.header.data_window.min_y, y1, c.y_sampling);
        if (info.cw > 0 && info.ch > 0) {
            info.data = part.images[idx];
            info.type = c.pixel_type;
            info.xs = c.x_sampling;
            info.ys = c.y_sampling;
        }
        return info;
    };

    PlaneInfo pR = makePlaneInfo(map.r, 0.0f);
    PlaneInfo pG = makePlaneInfo(map.g, 0.0f);
    PlaneInfo pB = makePlaneInfo(map.b, 0.0f);
    PlaneInfo pA = makePlaneInfo(map.a, 1.0f);

    int32_t minX = part.header.data_window.min_x;
    int32_t minY = part.header.data_window.min_y;

    for (int32_t y = 0; y < h; ++y) {
        int cyR = pR.data ? SampledIndex(minY, y, pR.ys, pR.ch) : 0;
        int cyG = pG.data ? SampledIndex(minY, y, pG.ys, pG.ch) : 0;
        int cyB = pB.data ? SampledIndex(minY, y, pB.ys, pB.ch) : 0;
        int cyA = pA.data ? SampledIndex(minY, y, pA.ys, pA.ch) : 0;

        float* dst = outData.data() + static_cast<size_t>(y) * static_cast<size_t>(w) * 4;
        for (int32_t x = 0; x < w; ++x) {
            dst[0] = pR.data
                ? LoadSample(pR.data, pR.type, static_cast<size_t>(cyR) * pR.cw + SampledIndex(minX, x, pR.xs, pR.cw))
                : pR.fallback;
            dst[1] = pG.data
                ? LoadSample(pG.data, pG.type, static_cast<size_t>(cyG) * pG.cw + SampledIndex(minX, x, pG.xs, pG.cw))
                : pG.fallback;
            dst[2] = pB.data
                ? LoadSample(pB.data, pB.type, static_cast<size_t>(cyB) * pB.cw + SampledIndex(minX, x, pB.xs, pB.cw))
                : pB.fallback;
            dst[3] = pA.data
                ? LoadSample(pA.data, pA.type, static_cast<size_t>(cyA) * pA.cw + SampledIndex(minX, x, pA.xs, pA.cw))
                : pA.fallback;
            dst += 4;
        }
    }

    return true;
}

bool DecodeReaderToRgbaFloat(exr_reader* r,
                             int* width, int* height,
                             std::vector<float>& outData,
                             ExrChromaticities* outChromaticities) {
    if (outChromaticities) *outChromaticities = {};
    if (!r || !width || !height) return false;

    exr_result rc = exr_reader_parse_header(r);
    if (!EXR_OK(rc)) {
        QV_LOG("Loader_EXR_Error", TraceLoggingString(exr_result_string(rc), "Message"));
        return false;
    }

    int partIdx = FindFirstFlatPart(r);
    if (partIdx < 0) {
        QV_LOG("Loader_EXR_Error", TraceLoggingString("No flat image part found", "Message"));
        return false;
    }

    const exr_header* hdr = exr_reader_part_header(r, partIdx);
    FillChromaticities(hdr, outChromaticities);

    PartGuard partGuard;
    rc = exr_reader_read_part(r, partIdx, &partGuard.part);
    if (!EXR_OK(rc)) {
        QV_LOG("Loader_EXR_Error", TraceLoggingString(exr_result_string(rc), "Message"));
        return false;
    }

    if (!DecodePartToRgbaFloat(partGuard.part, outData)) {
        QV_LOG("Loader_EXR_Error", TraceLoggingString("Failed to convert EXR part to RGBA float", "Message"));
        return false;
    }

    *width = partGuard.part.width;
    *height = partGuard.part.height;
    return true;
}

} // namespace

bool LoadEXR(const char* filename,
             int* width, int* height,
             std::vector<float>& outData) {
    if (!filename || !width || !height) return false;
    ReaderGuard guard;
    exr_result rc = exr_reader_open_file(filename, nullptr, &guard.r);
    if (!EXR_OK(rc)) {
        QV_LOG("Loader_EXR_Error", TraceLoggingString(exr_result_string(rc), "Message"));
        return false;
    }
    return DecodeReaderToRgbaFloat(guard.r, width, height, outData, nullptr);
}

bool LoadEXRFromMemory(const uint8_t* inData, size_t size,
                       int* width, int* height,
                       std::vector<float>& outData,
                       ExrChromaticities* outChromaticities) {
    if (outChromaticities) *outChromaticities = {};
    if (!inData || size < 8 || !width || !height) return false;
    ReaderGuard guard;
    exr_result rc = exr_reader_open_memory(inData, size, nullptr, &guard.r);
    if (!EXR_OK(rc)) {
        QV_LOG("Loader_EXR_Error", TraceLoggingString(exr_result_string(rc), "Message"));
        return false;
    }
    return DecodeReaderToRgbaFloat(guard.r, width, height, outData, outChromaticities);
}

bool GetEXRDimensionsFromMemory(const uint8_t* inData, size_t size, int* width, int* height) {
    if (!inData || size < 8 || !width || !height) return false;

    ReaderGuard guard;
    exr_result rc = exr_reader_open_memory(inData, size, nullptr, &guard.r);
    if (!EXR_OK(rc)) return false;

    rc = exr_reader_parse_header(guard.r);
    if (!EXR_OK(rc)) return false;

    int partIdx = FindFirstFlatPart(guard.r);
    if (partIdx < 0) partIdx = 0;
    const exr_header* hdr = exr_reader_part_header(guard.r, partIdx);
    if (!hdr) return false;

    int64_t w = static_cast<int64_t>(hdr->data_window.max_x) - hdr->data_window.min_x + 1;
    int64_t h = static_cast<int64_t>(hdr->data_window.max_y) - hdr->data_window.min_y + 1;
    if (w <= 0 || h <= 0 || w > INT_MAX || h > INT_MAX) return false;

    *width = static_cast<int>(w);
    *height = static_cast<int>(h);
    return true;
}

} // namespace TinyExrLoader
