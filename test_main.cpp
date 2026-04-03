#include "QuickView/SystemInfo.h"
#ifndef HWY_TARGETS
#if defined(_M_X64) || defined(__x86_64__)
    #undef HWY_BASELINE_TARGETS
    #define HWY_BASELINE_TARGETS (HWY_SSE4)
#endif
#endif
#include <hwy/highway.h>

HWY_BEFORE_NAMESPACE();
namespace SIMD_ImageLoader {
namespace HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;
inline void ComputeHistRow(const uint8_t* row, int width, uint32_t* HistR, uint32_t* HistG, uint32_t* HistB, uint32_t* HistL, int& x_out) {
}
}
}
HWY_AFTER_NAMESPACE();

#if HWY_ONCE
namespace SIMD_ImageLoader {
    HWY_EXPORT(ComputeHistRow);
}
#endif

int main() {
    int x_out = 0;
    HWY_DYNAMIC_DISPATCH(SIMD_ImageLoader::ComputeHistRow)(nullptr, 0, nullptr, nullptr, nullptr, nullptr, x_out);
    return 0;
}
