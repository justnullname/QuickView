sed -i '/\/\/ --- Peak Detection (HDR \/ Linear) ---/,$d' QuickView/SIMDUtils.h
echo '} // namespace SIMDUtils' >> QuickView/SIMDUtils.h
