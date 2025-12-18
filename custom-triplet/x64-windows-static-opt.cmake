set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)

# Common flags
# /Gw = Optimize global data
set(COMMON_FLAGS "/DWIN32 /D_WINDOWS /W3 /utf-8 /MP /Gw")

set(VCPKG_C_FLAGS "${COMMON_FLAGS}")
set(VCPKG_CXX_FLAGS "${COMMON_FLAGS}")

# Release: Maximize Speed (/O2), Intrinsic (/Oi), Inline (/Ob2), LTO (/GL)
set(VCPKG_C_FLAGS_RELEASE "/O2 /Oi /Ob2 /GL")
set(VCPKG_CXX_FLAGS_RELEASE "/O2 /Oi /Ob2 /GL")
set(VCPKG_LINKER_FLAGS_RELEASE "/LTCG")

# Debug: Default (no forced optimizations, allows /RTC1)
