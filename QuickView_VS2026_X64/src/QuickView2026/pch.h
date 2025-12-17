#pragma once

// Windows headers
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>

// Direct2D and DirectWrite
#include <d2d1_3.h>
#include <d3d11_1.h>
#include <dxgi1_3.h>
#include <dwrite.h>
#include <wincodec.h>

// COM smart pointers
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

// C++ Standard Library
#include <memory>
#include <string>
#include <vector>
#include <stdexcept>

// Pragmas for linking
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")

// Helper macro for HRESULT checking
#ifndef THROW_IF_FAILED
#define THROW_IF_FAILED(hr) \
    if (FAILED(hr)) { throw std::runtime_error("HRESULT failed: " + std::to_string(hr)); }
#endif
