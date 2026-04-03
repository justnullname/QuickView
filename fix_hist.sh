sed -i '/\/\/ Process 8 pixels per iteration./,/#endif/d' QuickView/ImageLoader.cpp
cat << 'HFILE' > QuickView/ImageLoader.cpp.patch
--- QuickView/ImageLoader.cpp
+++ QuickView/ImageLoader.cpp
@@ -9510,9 +9510,11 @@
         const uint8_t* row = ptr + (UINT64)y * stride;
         UINT x = 0;

+        int x_out = 0;
+        HWY_DYNAMIC_DISPATCH(ComputeHistRow)(row, frame.width, pMetadata->HistR.data(), pMetadata->HistG.data(), pMetadata->HistB.data(), pMetadata->HistL.data(), x_out);
+        x = (UINT)x_out;

     for (; x < frame.width; x++) {
-        // Unrolling or SIMD could be added here, but scalar is fast enough with skip sampling.
-        // Layout: B, G, R, A
         uint8_t b = row[x * 4 + 0];
         uint8_t g = row[x * 4 + 1];
         uint8_t r = row[x * 4 + 2];
HFILE
patch QuickView/ImageLoader.cpp < QuickView/ImageLoader.cpp.patch
