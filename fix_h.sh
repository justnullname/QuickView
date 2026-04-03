cat << 'HFILE' > test_main.cpp
#include "QuickView/SIMDUtils.h"
int main() { return 0; }
HFILE
g++ test_main.cpp -I ./third_party/vcpkg/packages/highway_x64-linux/include -I QuickView/ -c
