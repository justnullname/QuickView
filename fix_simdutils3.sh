cat << 'HFILE' >> QuickView/SIMDUtils.h

#if HWY_ONCE
namespace SIMDUtils {
    inline void PremultiplyAlpha_BGRA(uint8_t* pData, int width, int height, int stride = 0) {
        HWY_STATIC_DISPATCH(PremultiplyAlpha_BGRA_Impl)(pData, width, height, stride);
    }

    inline void SwizzleRGBA_to_BGRA_Premul(uint8_t* pData, size_t pixelCount) {
        HWY_STATIC_DISPATCH(SwizzleRGBA_to_BGRA_Premul_Impl)(pData, pixelCount);
    }

    inline float FindPeak_R32G32B32A32_FLOAT(const float* pData, size_t pixelCount) {
        return HWY_STATIC_DISPATCH(FindPeak_R32G32B32A32_FLOAT_Impl)(pData, pixelCount);
    }

    inline void ResizeBilinear(const uint8_t* src, int w, int h, int srcStride, uint8_t* dst, int newW, int newH, int dstStride = 0) {
        HWY_STATIC_DISPATCH(ResizeBilinear_Impl)(src, w, h, srcStride, dst, newW, newH, dstStride);
    }
}
#endif
HFILE
