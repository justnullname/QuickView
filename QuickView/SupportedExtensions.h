#pragma once
#include <string_view>
#include <array>
#include <string>

namespace QuickView {

inline constexpr std::wstring_view SUPPORTED_EXTENSIONS[] = {
    // Standard
    L".jpg", L".jpeg", L".jpe", L".jfif", L".png", L".bmp", L".dib", L".gif", 
    L".tif", L".tiff", L".ico", 
    // Web / Modern
    L".webp", L".avif", L".avifs", L".heic", L".heif", L".svg", L".svgz", L".jxl", L".apng",
    // Professional / HDR / Legacy
    L".exr", L".hdr", L".pic", L".psd", L".psb", L".tga", L".icb", L".vda", L".vst", L".pcx", L".qoi", 
    L".wbmp", L".pam", L".pbm", L".pgm", L".ppm", L".pnm", L".wdp", L".hdp", L".jxr", L".hif",
    // RAW Formats (LibRaw supported)
    L".arw", L".cr2", L".cr3", L".crw", L".dng", L".nef", L".orf", L".raf", L".rw2", L".srw", L".x3f",
    L".mrw", L".mos", L".kdc", L".dcr", L".sr2", L".pef", L".erf", L".3fr", L".mef", L".nrw", L".raw",
    // Archives (VFS supported)
    L".cbz", L".zip"
};

// Generate the COMDLG filter string, e.g., "All Images\0*.jpg;*.png...\0All Files\0*.*\0\0"
inline std::wstring GetSupportedExtensionsFilter() {
    std::wstring filter;
    filter.append(L"All Images");
    filter.push_back(L'\0');
    
    for (size_t i = 0; i < std::size(SUPPORTED_EXTENSIONS); ++i) {
        filter += L"*";
        filter += SUPPORTED_EXTENSIONS[i];
        if (i < std::size(SUPPORTED_EXTENSIONS) - 1) {
            filter += L";";
        }
    }
    
    filter.push_back(L'\0');
    filter.append(L"All Files");
    filter.push_back(L'\0');
    filter.append(L"*.*");
    filter.push_back(L'\0');
    filter.push_back(L'\0');
    
    return filter;
}

}
