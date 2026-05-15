cmake_minimum_required(VERSION 3.20)
project(unrar_core)

# Core decoding files - stripped for QuickView
set(UNRAR_SOURCES
    archive.cpp
    arcread.cpp
    blake2s.cpp
    crc.cpp
    encname.cpp
    errhnd.cpp
    extinfo.cpp
    file.cpp
    filefn.cpp
    filestr.cpp
    getbits.cpp
    hash.cpp
    headers.cpp
    isnt.cpp
    largepage.cpp
    match.cpp
    motw.cpp
    options.cpp
    pathfn.cpp
    rarvm.cpp
    rawread.cpp
    rdwrfn.cpp
    sha1.cpp
    sha256.cpp
    smallfn.cpp
    strfn.cpp
    strlist.cpp
    system.cpp
    threadpool.cpp
    timefn.cpp
    unicode.cpp
    unpack.cpp
)

add_library(unrar_core STATIC ${UNRAR_SOURCES})

target_compile_definitions(unrar_core PRIVATE
    RAR_NOCRYPT
    _WIN_ALL
    SILENT
    NOVOLUME
    UNRAR_CORE_BUILD
)

target_compile_options(unrar_core PRIVATE
    -fno-exceptions
    -fno-rtti
    -msse -msse2 -mssse3 -msse4.1
    # Suppress warnings from third-party code
    $<$<CXX_COMPILER_ID:Clang,AppleClang>:
        -Wno-unused-parameter
        -Wno-macro-redefined
        -Wno-dangling-else
        -Wno-switch
        -Wno-nontrivial-memcall
        -Wno-deprecated-declarations
        -Wno-logical-not-parentheses
    >
    $<$<CXX_COMPILER_ID:MSVC>:/wd4100 /wd4005 /wd4706 /wd4065 /wd4996>
)
set_property(TARGET unrar_core PROPERTY CXX_STANDARD 20)
