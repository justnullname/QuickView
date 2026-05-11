#pragma once

// Windows headers
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shlwapi.h>

// Spartan Bedrock (Zero-cost types only)
#include <cstdint>
#include <cstddef>
#include <cwchar>
#include <string_view>
#include <type_traits>
#include <utility>
#include <concepts>
#include <bit>

// [Critical] Resolve Windows macro interference BEFORE Direct2D/DirectWrite headers
#undef DrawText
#undef DrawTextW

// Direct2D and DirectWrite
#include <d2d1_3.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <dwrite.h>
#include <wincodec.h>

// COM smart pointers
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

// Pragmas for linking
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shlwapi.lib") // [SVG] For SHCreateMemStream
#pragma comment(lib, "ole32.lib")   // [SVG] For CreateStreamOnHGlobal

// Helper macro for HRESULT checking

