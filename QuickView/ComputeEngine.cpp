#include "pch.h"
#include "QuickViewETW.h"
static constexpr const char* CURRENT_MODULE = "ComputeEngine";

#include "ComputeEngine.h"
#include "CompiledComputeShaders.h"
#include "Plugin/PluginHost.h"
#include <algorithm>

namespace QuickView {

HRESULT ComputeEngine::Initialize(ID3D11Device* pDevice, std::recursive_mutex* contextMutex) {
    if (!pDevice) return E_INVALIDARG;
    m_d3dDevice = pDevice;
    m_d3dDevice->GetImmediateContext(&m_d3dContext);
    m_externalContextMutex = contextMutex;
    m_valid = true;
    return S_OK;
}

HRESULT ComputeEngine::EnsureComputeShader(ComPtr<ID3D11ComputeShader>& shader, const uint8_t* bytecode, size_t bytecodeSize, const char* debugName) {
    if (shader) return S_OK;
    if (!m_d3dDevice || !bytecode || bytecodeSize == 0) return E_FAIL;

    HRESULT hr = m_d3dDevice->CreateComputeShader(bytecode, bytecodeSize, nullptr, &shader);
    if (FAILED(hr)) {
        OutputDebugStringA("[ComputeEngine] CreateComputeShader Failed: ");
        OutputDebugStringA(debugName ? debugName : "Unknown");
        OutputDebugStringA("\n");
        QV_LOG("Shader_Create_Error", TraceLoggingString(debugName ? debugName : "Unknown", "Shader"), TraceLoggingInt32(hr, "HR"));
    }
    return hr;
}

HRESULT ComputeEngine::EnsureSamplers() {
    if (m_linearSampler && m_pointSampler) return S_OK;
    if (!m_d3dDevice) return E_FAIL;

    if (!m_linearSampler) {
        D3D11_SAMPLER_DESC sampDesc = {};
        sampDesc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        HRESULT hr = m_d3dDevice->CreateSamplerState(&sampDesc, &m_linearSampler);
        if (FAILED(hr)) return hr;
    }

    if (!m_pointSampler) {
        D3D11_SAMPLER_DESC sampDesc = {};
        sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        HRESULT hr = m_d3dDevice->CreateSamplerState(&sampDesc, &m_pointSampler);
        if (FAILED(hr)) return hr;
    }

    return S_OK;
}

HRESULT ComputeEngine::UploadAndConvert(const uint8_t* srcPixels, int width, int height, int stride, PixelFormat srcFormat, ID3D11Texture2D** outTexture) {
    if (!m_valid || !outTexture || width <= 0 || height <= 0 || !srcPixels) return E_INVALIDARG;

    bool isHdrFloat = (srcFormat == PixelFormat::R32G32B32A32_FLOAT || srcFormat == PixelFormat::R16G16B16A16_FLOAT);

    // 1. For standard 32-bit SDR formats (BGRA/BGRX/RGBA), directly create DEFAULT texture without Compute Shader dispatch (100% Thread-Safe)
    if (srcFormat == PixelFormat::BGRA8888 || srcFormat == PixelFormat::BGRX8888 || srcFormat == PixelFormat::RGBA8888) {
        D3D11_TEXTURE2D_DESC directDesc = {};
        directDesc.Width = width;
        directDesc.Height = height;
        directDesc.MipLevels = 1;
        directDesc.ArraySize = 1;
        directDesc.Format = (srcFormat == PixelFormat::RGBA8888) ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
        directDesc.SampleDesc.Count = 1;
        directDesc.Usage = D3D11_USAGE_DEFAULT;
        directDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA initData = {};
        initData.pSysMem = srcPixels;
        initData.SysMemPitch = stride > 0 ? stride : width * 4;

        return m_d3dDevice->CreateTexture2D(&directDesc, &initData, outTexture);
    }

    // 2. Create Staging Texture (Immutable for HDR upload)
    D3D11_TEXTURE2D_DESC srcDesc = {};
    srcDesc.Width = width;
    srcDesc.Height = height;
    srcDesc.MipLevels = 1;
    srcDesc.ArraySize = 1;
    srcDesc.Format = (srcFormat == PixelFormat::R32G32B32A32_FLOAT) ? DXGI_FORMAT_R32G32B32A32_FLOAT : 
                     (srcFormat == PixelFormat::R16G16B16A16_FLOAT) ? DXGI_FORMAT_R16G16B16A16_FLOAT :
                     DXGI_FORMAT_R16G16B16A16_UNORM;
    srcDesc.SampleDesc.Count = 1;
    srcDesc.Usage = D3D11_USAGE_IMMUTABLE;
    srcDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    
    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = srcPixels;
    initData.SysMemPitch = stride > 0 ? stride : width * ((srcFormat == PixelFormat::R32G32B32A32_FLOAT) ? 16 : 8);
    
    ComPtr<ID3D11Texture2D> pSrc;
    HRESULT hr = m_d3dDevice->CreateTexture2D(&srcDesc, &initData, &pSrc);
    if (FAILED(hr)) return hr;

    // 3. Create Destination Texture (UAV + SRV + RTV)
    D3D11_TEXTURE2D_DESC dstDesc = srcDesc;
    dstDesc.Format = isHdrFloat ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
    dstDesc.Usage = D3D11_USAGE_DEFAULT;
    dstDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    
    ComPtr<ID3D11Texture2D> pDst;
    hr = m_d3dDevice->CreateTexture2D(&dstDesc, nullptr, &pDst);
    if (FAILED(hr)) return hr;

    // 3. Dispatch (serialized on immediate context)
    std::lock_guard<std::recursive_mutex> ctxLock(ContextMutex());
    hr = EnsureComputeShader(m_csFormatConvert, Shaders::g_csFormatConvert, sizeof(Shaders::g_csFormatConvert), "FormatConvert");
    if (FAILED(hr)) return hr;

    ComPtr<ID3D11ShaderResourceView> pSRV;
    ComPtr<ID3D11UnorderedAccessView> pUAV;
    m_d3dDevice->CreateShaderResourceView(pSrc.Get(), nullptr, &pSRV);
    m_d3dDevice->CreateUnorderedAccessView(pDst.Get(), nullptr, &pUAV);
    
    m_d3dContext->CSSetShader(m_csFormatConvert.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = { pSRV.Get() };
    m_d3dContext->CSSetShaderResources(0, 1, srvs);
    ID3D11UnorderedAccessView* uavs[] = { pUAV.Get() };
    m_d3dContext->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    m_d3dContext->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    
    // Clear
    ID3D11UnorderedAccessView* nullUAV[] = { nullptr };
    m_d3dContext->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
    ID3D11ShaderResourceView* nullSRV[] = { nullptr };
    m_d3dContext->CSSetShaderResources(0, 1, nullSRV);

    *outTexture = pDst.Detach();
    return S_OK;
}

HRESULT ComputeEngine::GenerateMips(ID3D11Texture2D* pTexture) {
    if (!m_valid || !pTexture) return E_INVALIDARG;

    D3D11_TEXTURE2D_DESC desc;
    pTexture->GetDesc(&desc);
    if (desc.MipLevels <= 1) return S_FALSE;

    std::lock_guard<std::recursive_mutex> ctxLock(ContextMutex());
    HRESULT hr = EnsureComputeShader(m_csGenMips, Shaders::g_csGenerateMips, sizeof(Shaders::g_csGenerateMips), "GenerateMips");
    if (FAILED(hr)) return hr;

    m_d3dContext->CSSetShader(m_csGenMips.Get(), nullptr, 0);

    for (UINT srcMip = 0; srcMip < desc.MipLevels - 1; ++srcMip) {
        UINT dstMip = srcMip + 1;
        UINT dstW = std::max(1u, desc.Width >> dstMip);
        UINT dstH = std::max(1u, desc.Height >> dstMip);

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = desc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = srcMip;
        srvDesc.Texture2D.MipLevels = 1;

        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = desc.Format;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        uavDesc.Texture2D.MipSlice = dstMip;

        ComPtr<ID3D11ShaderResourceView> pSRV;
        ComPtr<ID3D11UnorderedAccessView> pUAV;
        m_d3dDevice->CreateShaderResourceView(pTexture, &srvDesc, &pSRV);
        m_d3dDevice->CreateUnorderedAccessView(pTexture, &uavDesc, &pUAV);

        ID3D11ShaderResourceView* srvs[] = { pSRV.Get() };
        m_d3dContext->CSSetShaderResources(0, 1, srvs);
        ID3D11UnorderedAccessView* uavs[] = { pUAV.Get() };
        m_d3dContext->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

        m_d3dContext->Dispatch((dstW + 7) / 8, (dstH + 7) / 8, 1);

        ID3D11UnorderedAccessView* nullUAV[] = { nullptr };
        m_d3dContext->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
    }

    ID3D11ShaderResourceView* nullSRV[] = { nullptr };
    m_d3dContext->CSSetShaderResources(0, 1, nullSRV);
    return S_OK;
}

HRESULT ComputeEngine::Upload3DLut(const float* rgbValues, int edge, ID3D11Texture3D** outTexture) {
    if (!m_valid || !rgbValues || edge <= 1 || !outTexture) return E_INVALIDARG;

    D3D11_TEXTURE3D_DESC desc = {};
    desc.Width = static_cast<UINT>(edge);
    desc.Height = static_cast<UINT>(edge);
    desc.Depth = static_cast<UINT>(edge);
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    std::vector<uint16_t> packed(static_cast<size_t>(edge) * edge * edge * 4, 0);

    auto floatToHalf = [](float value) -> uint16_t {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        uint32_t sign = (bits >> 16) & 0x8000;
        uint32_t mantissa = bits & 0x007fffff;
        int exp = ((bits >> 23) & 0xff) - 127 + 15;
        if (exp <= 0) return static_cast<uint16_t>(sign);
        if (exp >= 31) return static_cast<uint16_t>(sign | 0x7c00);
        return static_cast<uint16_t>(sign | (exp << 10) | (mantissa >> 13));
    };

    // 3D LUT generation only runs once upon loading, having extremely small size (~35K voxels).
    // Bypassing compiler target AVX instruction constraints to ensure perfect cross-platform compatibility.
    const size_t voxelCount = static_cast<size_t>(edge) * edge * edge;
    for (size_t i = 0; i < voxelCount; ++i) {
        packed[i * 4 + 0] = floatToHalf(rgbValues[i * 3 + 0]);
        packed[i * 4 + 1] = floatToHalf(rgbValues[i * 3 + 1]);
        packed[i * 4 + 2] = floatToHalf(rgbValues[i * 3 + 2]);
        packed[i * 4 + 3] = floatToHalf(1.0f);
    }

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = packed.data();
    initData.SysMemPitch = static_cast<UINT>(edge * sizeof(uint16_t) * 4);
    initData.SysMemSlicePitch = static_cast<UINT>(edge * edge * sizeof(uint16_t) * 4);

    ComPtr<ID3D11Texture3D> texture;
    HRESULT hr = m_d3dDevice->CreateTexture3D(&desc, &initData, &texture);
    if (FAILED(hr)) return hr;
    *outTexture = texture.Detach();
    return S_OK;
}

HRESULT ComputeEngine::UploadOverflowLut(const uint8_t* values, int edge, ID3D11Texture3D** outTexture) {
    if (!m_valid || !values || edge <= 1 || !outTexture) return E_INVALIDARG;

    D3D11_TEXTURE3D_DESC desc = {};
    desc.Width = static_cast<UINT>(edge);
    desc.Height = static_cast<UINT>(edge);
    desc.Depth = static_cast<UINT>(edge);
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8_UNORM;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = values;
    initData.SysMemPitch = static_cast<UINT>(edge);
    initData.SysMemSlicePitch = static_cast<UINT>(edge * edge);

    ComPtr<ID3D11Texture3D> texture;
    HRESULT hr = m_d3dDevice->CreateTexture3D(&desc, &initData, &texture);
    if (FAILED(hr)) return hr;
    *outTexture = texture.Detach();
    return S_OK;
}

HRESULT ComputeEngine::ReadbackMaskTexture(ID3D11Texture2D* maskTexture, GamutMaskReadback* outReadback) {
    if (!maskTexture || !outReadback) return E_INVALIDARG;

    D3D11_TEXTURE2D_DESC desc = {};
    maskTexture->GetDesc(&desc);

    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> staging;
    HRESULT hr = m_d3dDevice->CreateTexture2D(&stagingDesc, nullptr, &staging);
    if (FAILED(hr)) return hr;

    m_d3dContext->CopyResource(staging.Get(), maskTexture);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = m_d3dContext->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return hr;

    outReadback->width = static_cast<int>(desc.Width);
    outReadback->height = static_cast<int>(desc.Height);
    outReadback->mask.assign(static_cast<size_t>(desc.Width) * desc.Height, 0);
    outReadback->hasOverflow = false;

    for (UINT y = 0; y < desc.Height; ++y) {
        const auto* row = reinterpret_cast<const uint32_t*>(
            static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch);
        for (UINT x = 0; x < desc.Width; ++x) {
            const uint8_t value = row[x] ? 255 : 0;
            outReadback->mask[static_cast<size_t>(y) * desc.Width + x] = value;
            outReadback->hasOverflow |= value != 0;
        }
    }

    m_d3dContext->Unmap(staging.Get(), 0);
    return S_OK;
}

HRESULT ComputeEngine::DispatchGamutMaskLut(
    ID3D11Texture2D* srcTexture,
    ID3D11ShaderResourceView* overflowLut,
    int lutEdge,
    float epsilon,
    GamutMaskReadback* outReadback) {
    if (!m_valid || !srcTexture || !overflowLut || lutEdge <= 1 || !outReadback) {
        return E_INVALIDARG;
    }

    std::lock_guard<std::recursive_mutex> ctxLock(ContextMutex());
    D3D11_TEXTURE2D_DESC srcDesc = {};
    srcTexture->GetDesc(&srcDesc);

    ComPtr<ID3D11ShaderResourceView> srcSrv;
    HRESULT hr = m_d3dDevice->CreateShaderResourceView(srcTexture, nullptr, &srcSrv);
    if (FAILED(hr)) return hr;

    D3D11_TEXTURE2D_DESC maskDesc = {};
    maskDesc.Width = srcDesc.Width;
    maskDesc.Height = srcDesc.Height;
    maskDesc.MipLevels = 1;
    maskDesc.ArraySize = 1;
    maskDesc.Format = DXGI_FORMAT_R32_UINT;
    maskDesc.SampleDesc.Count = 1;
    maskDesc.Usage = D3D11_USAGE_DEFAULT;
    maskDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;

    ComPtr<ID3D11Texture2D> maskTexture;
    hr = m_d3dDevice->CreateTexture2D(&maskDesc, nullptr, &maskTexture);
    if (FAILED(hr)) return hr;

    ComPtr<ID3D11UnorderedAccessView> maskUav;
    hr = m_d3dDevice->CreateUnorderedAccessView(maskTexture.Get(), nullptr, &maskUav);
    if (FAILED(hr)) return hr;

    struct LutParams {
        float epsilon;
        uint32_t width;
        uint32_t height;
        uint32_t lutEdge;
    } params = { epsilon, srcDesc.Width, srcDesc.Height, static_cast<uint32_t>(lutEdge) };

    hr = EnsureComputeShader(m_csGamutLut, Shaders::g_csGamutLut, sizeof(Shaders::g_csGamutLut), "GamutLut");
    if (FAILED(hr)) return hr;
    hr = EnsureSamplers();
    if (FAILED(hr)) return hr;

    if (!m_gamutLutConstantBuffer) {
        D3D11_BUFFER_DESC cbDesc = {};
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.ByteWidth = 16;
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = m_d3dDevice->CreateBuffer(&cbDesc, nullptr, &m_gamutLutConstantBuffer);
        if (FAILED(hr)) return hr;
    }

    if (!m_gamutCounterBuffer) {
        D3D11_BUFFER_DESC counterDesc = {};
        counterDesc.ByteWidth = sizeof(uint32_t);
        counterDesc.Usage = D3D11_USAGE_DEFAULT;
        counterDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        counterDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        counterDesc.StructureByteStride = sizeof(uint32_t);
        hr = m_d3dDevice->CreateBuffer(&counterDesc, nullptr, &m_gamutCounterBuffer);
        if (FAILED(hr)) return hr;

        D3D11_BUFFER_DESC stagingDesc = {};
        stagingDesc.ByteWidth = sizeof(uint32_t);
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        hr = m_d3dDevice->CreateBuffer(&stagingDesc, nullptr, &m_gamutCounterStaging);
        if (FAILED(hr)) return hr;
    }

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = m_d3dContext->Map(m_gamutLutConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return hr;
    memcpy(mapped.pData, &params, sizeof(params));
    m_d3dContext->Unmap(m_gamutLutConstantBuffer.Get(), 0);

    m_d3dContext->CSSetShader(m_csGamutLut.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = { srcSrv.Get(), overflowLut };
    m_d3dContext->CSSetShaderResources(0, 2, srvs);

    // Create UAV for atomic overflow counter (u1)
    D3D11_UNORDERED_ACCESS_VIEW_DESC counterUavDesc = {};
    counterUavDesc.Format = DXGI_FORMAT_UNKNOWN;
    counterUavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    counterUavDesc.Buffer.FirstElement = 0;
    counterUavDesc.Buffer.NumElements = 1;
    ComPtr<ID3D11UnorderedAccessView> counterUav;
    hr = m_d3dDevice->CreateUnorderedAccessView(m_gamutCounterBuffer.Get(), &counterUavDesc, &counterUav);
    if (FAILED(hr)) return hr;

    // Clear atomic counter to 0
    const UINT zero = 0;
    m_d3dContext->UpdateSubresource(m_gamutCounterBuffer.Get(), 0, nullptr, &zero, 0, 0);

    // Bind both UAVs: u0=mask, u1=counter
    ID3D11UnorderedAccessView* uavs[] = { maskUav.Get(), counterUav.Get() };
    m_d3dContext->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
    ID3D11Buffer* cbs[] = { m_gamutLutConstantBuffer.Get() };
    m_d3dContext->CSSetConstantBuffers(0, 1, cbs);
    ID3D11SamplerState* samplers[] = { m_linearSampler.Get() };
    m_d3dContext->CSSetSamplers(0, 1, samplers);
    m_d3dContext->Dispatch((srcDesc.Width + 7) / 8, (srcDesc.Height + 7) / 8, 1);

    ID3D11UnorderedAccessView* nullUav[] = { nullptr, nullptr };
    m_d3dContext->CSSetUnorderedAccessViews(0, 2, nullUav, nullptr);
    ID3D11ShaderResourceView* nullSrvs[] = { nullptr, nullptr };
    m_d3dContext->CSSetShaderResources(0, 2, nullSrvs);

    // Fast path: Read back 4-byte counter instead of full texture
    m_d3dContext->CopyResource(m_gamutCounterStaging.Get(), m_gamutCounterBuffer.Get());
    D3D11_MAPPED_SUBRESOURCE counterMapped = {};
    hr = m_d3dContext->Map(m_gamutCounterStaging.Get(), 0, D3D11_MAP_READ, 0, &counterMapped);
    if (FAILED(hr)) return hr;
    const uint32_t overflowCount = *static_cast<const uint32_t*>(counterMapped.pData);
    m_d3dContext->Unmap(m_gamutCounterStaging.Get(), 0);

    outReadback->overflowCount = overflowCount;
    outReadback->hasOverflow = (overflowCount > 0);

    // Only do expensive mask texture readback if visualization is needed
    if (outReadback->hasOverflow) {
        return ReadbackMaskTexture(maskTexture.Get(), outReadback);
    }

    outReadback->width = static_cast<int>(srcDesc.Width);
    outReadback->height = static_cast<int>(srcDesc.Height);
    outReadback->mask.clear();
    return S_OK;
}

namespace {

DXGI_FORMAT ToDxgiToneMapSrcFormat(PixelFormat fmt) noexcept {
    switch (fmt) {
    case PixelFormat::BGRA8888:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case PixelFormat::BGRX8888:
        return DXGI_FORMAT_B8G8R8X8_UNORM;
    case PixelFormat::RGBA8888:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case PixelFormat::R16G16B16A16_UNORM:
        return DXGI_FORMAT_R16G16B16A16_UNORM;
    case PixelFormat::R16G16B16A16_FLOAT:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case PixelFormat::R32G32B32A32_FLOAT:
    default:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    }
}

} // namespace

HRESULT ComputeEngine::ToneMapHdrToSdr(const uint8_t* srcPixels, int width, int height, int stride, const ToneMapSettings& settings, ID3D11Texture2D** outTexture, PixelFormat srcFormat) {
    if (!m_valid || !srcPixels || width <= 0 || height <= 0 || !outTexture) return E_INVALIDARG;
    std::lock_guard<std::recursive_mutex> ctxLock(ContextMutex());

    D3D11_TEXTURE2D_DESC srcDesc = {};
    srcDesc.Width = static_cast<UINT>(width);
    srcDesc.Height = static_cast<UINT>(height);
    srcDesc.MipLevels = 1;
    srcDesc.ArraySize = 1;
    srcDesc.Format = ToDxgiToneMapSrcFormat(srcFormat);
    srcDesc.SampleDesc.Count = 1;
    srcDesc.Usage = D3D11_USAGE_IMMUTABLE;
    srcDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = srcPixels;
    initData.SysMemPitch = static_cast<UINT>(stride);

    ComPtr<ID3D11Texture2D> pSrc;
    HRESULT hr = m_d3dDevice->CreateTexture2D(&srcDesc, &initData, &pSrc);
    if (FAILED(hr)) return hr;

    D3D11_TEXTURE2D_DESC dstDesc = {};
    dstDesc.Width = srcDesc.Width;
    dstDesc.Height = srcDesc.Height;
    dstDesc.MipLevels = 1;
    dstDesc.ArraySize = 1;
    dstDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dstDesc.SampleDesc.Count = 1;
    dstDesc.Usage = D3D11_USAGE_DEFAULT;
    dstDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    ComPtr<ID3D11Texture2D> pDst;
    hr = m_d3dDevice->CreateTexture2D(&dstDesc, nullptr, &pDst);
    if (FAILED(hr)) return hr;

    ComPtr<ID3D11ShaderResourceView> pSRV;
    ComPtr<ID3D11UnorderedAccessView> pUAV;
    hr = m_d3dDevice->CreateShaderResourceView(pSrc.Get(), nullptr, &pSRV);
    if (FAILED(hr)) return hr;
    hr = m_d3dDevice->CreateUnorderedAccessView(pDst.Get(), nullptr, &pUAV);
    if (FAILED(hr)) return hr;

    hr = EnsureComputeShader(m_csToneMapHdrToSdr, Shaders::g_csToneMapHdrToSdr, sizeof(Shaders::g_csToneMapHdrToSdr), "ToneMapHdrToSdr");
    if (FAILED(hr)) return hr;

    if (!m_toneMapConstantBuffer) {
        D3D11_BUFFER_DESC cbDesc = {};
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.ByteWidth = sizeof(ToneMapSettings);
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = m_d3dDevice->CreateBuffer(&cbDesc, nullptr, &m_toneMapConstantBuffer);
        if (FAILED(hr)) return hr;
    }

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = m_d3dContext->Map(m_toneMapConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return hr;

    memcpy(mapped.pData, &settings, sizeof(settings));

    m_d3dContext->Unmap(m_toneMapConstantBuffer.Get(), 0);

    m_d3dContext->CSSetShader(m_csToneMapHdrToSdr.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = { pSRV.Get() };
    m_d3dContext->CSSetShaderResources(0, 1, srvs);
    ID3D11UnorderedAccessView* uavs[] = { pUAV.Get() };
    m_d3dContext->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    ID3D11Buffer* constantBuffers[] = { m_toneMapConstantBuffer.Get() };
    m_d3dContext->CSSetConstantBuffers(0, 1, constantBuffers);
    m_d3dContext->Dispatch((srcDesc.Width + 7) / 8, (srcDesc.Height + 7) / 8, 1);

    ID3D11UnorderedAccessView* nullUAV[] = { nullptr };
    m_d3dContext->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
    ID3D11ShaderResourceView* nullSRV[] = { nullptr };
    m_d3dContext->CSSetShaderResources(0, 1, nullSRV);
    ID3D11Buffer* nullCB[] = { nullptr };
    m_d3dContext->CSSetConstantBuffers(0, 1, nullCB);
    m_d3dContext->CSSetShader(nullptr, nullptr, 0);

    *outTexture = pDst.Detach();
    return S_OK;
}

HRESULT ComputeEngine::ToneMapHdrToHdr(const uint8_t* srcPixels, int width, int height, int stride, const ToneMapSettings& settings, ID3D11Texture2D** outTexture, PixelFormat srcFormat) {
    if (!m_valid || !srcPixels || width <= 0 || height <= 0 || !outTexture) return E_INVALIDARG;
    std::lock_guard<std::recursive_mutex> ctxLock(ContextMutex());

    D3D11_TEXTURE2D_DESC srcDesc = {};
    srcDesc.Width = static_cast<UINT>(width);
    srcDesc.Height = static_cast<UINT>(height);
    srcDesc.MipLevels = 1;
    srcDesc.ArraySize = 1;
    srcDesc.Format = ToDxgiToneMapSrcFormat(srcFormat);
    srcDesc.SampleDesc.Count = 1;
    srcDesc.Usage = D3D11_USAGE_IMMUTABLE;
    srcDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = srcPixels;
    initData.SysMemPitch = static_cast<UINT>(stride);

    ComPtr<ID3D11Texture2D> pSrc;
    HRESULT hr = m_d3dDevice->CreateTexture2D(&srcDesc, &initData, &pSrc);
    if (FAILED(hr)) return hr;

    D3D11_TEXTURE2D_DESC dstDesc = {};
    dstDesc.Width = srcDesc.Width;
    dstDesc.Height = srcDesc.Height;
    dstDesc.MipLevels = 1;
    dstDesc.ArraySize = 1;
    dstDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    dstDesc.SampleDesc.Count = 1;
    dstDesc.Usage = D3D11_USAGE_DEFAULT;
    dstDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    ComPtr<ID3D11Texture2D> pDst;
    hr = m_d3dDevice->CreateTexture2D(&dstDesc, nullptr, &pDst);
    if (FAILED(hr)) return hr;

    ComPtr<ID3D11ShaderResourceView> pSRV;
    ComPtr<ID3D11UnorderedAccessView> pUAV;
    hr = m_d3dDevice->CreateShaderResourceView(pSrc.Get(), nullptr, &pSRV);
    if (FAILED(hr)) return hr;
    hr = m_d3dDevice->CreateUnorderedAccessView(pDst.Get(), nullptr, &pUAV);
    if (FAILED(hr)) return hr;

    hr = EnsureComputeShader(m_csToneMapHdrToHdr, Shaders::g_csToneMapHdrToHdr, sizeof(Shaders::g_csToneMapHdrToHdr), "ToneMapHdrToHdr");
    if (FAILED(hr)) return hr;

    if (!m_toneMapConstantBuffer) {
        D3D11_BUFFER_DESC cbDesc = {};
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.ByteWidth = sizeof(ToneMapSettings);
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = m_d3dDevice->CreateBuffer(&cbDesc, nullptr, &m_toneMapConstantBuffer);
        if (FAILED(hr)) return hr;
    }

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = m_d3dContext->Map(m_toneMapConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return hr;

    memcpy(mapped.pData, &settings, sizeof(settings));

    m_d3dContext->Unmap(m_toneMapConstantBuffer.Get(), 0);

    m_d3dContext->CSSetShader(m_csToneMapHdrToHdr.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = { pSRV.Get() };
    m_d3dContext->CSSetShaderResources(0, 1, srvs);
    ID3D11UnorderedAccessView* uavs[] = { pUAV.Get() };
    m_d3dContext->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    ID3D11Buffer* constantBuffers[] = { m_toneMapConstantBuffer.Get() };
    m_d3dContext->CSSetConstantBuffers(0, 1, constantBuffers);
    m_d3dContext->Dispatch((srcDesc.Width + 7) / 8, (srcDesc.Height + 7) / 8, 1);

    ID3D11UnorderedAccessView* nullUAV[] = { nullptr };
    m_d3dContext->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
    ID3D11ShaderResourceView* nullSRV[] = { nullptr };
    m_d3dContext->CSSetShaderResources(0, 1, nullSRV);
    ID3D11Buffer* nullCB[] = { nullptr };
    m_d3dContext->CSSetConstantBuffers(0, 1, nullCB);
    m_d3dContext->CSSetShader(nullptr, nullptr, 0);

    *outTexture = pDst.Detach();
    return S_OK;
}

namespace {
HRESULT ToneMapTextureCommon(ID3D11Device* device,
                             ID3D11DeviceContext* context,
                             ID3D11ComputeShader* shader,
                             ID3D11Buffer* constantBuffer,
                             ID3D11Texture2D* srcTexture,
                             DXGI_FORMAT dstFormat,
                             const QuickView::ToneMapSettings& settings,
                             ID3D11Texture2D** outTexture) {
    if (!device || !context || !shader || !constantBuffer || !srcTexture || !outTexture) {
        return E_INVALIDARG;
    }

    D3D11_TEXTURE2D_DESC srcDesc = {};
    srcTexture->GetDesc(&srcDesc);
    if (srcDesc.Width == 0 || srcDesc.Height == 0) {
        return E_INVALIDARG;
    }

    D3D11_TEXTURE2D_DESC dstDesc = {};
    dstDesc.Width = srcDesc.Width;
    dstDesc.Height = srcDesc.Height;
    dstDesc.MipLevels = 1;
    dstDesc.ArraySize = 1;
    dstDesc.Format = dstFormat;
    dstDesc.SampleDesc.Count = 1;
    dstDesc.Usage = D3D11_USAGE_DEFAULT;
    dstDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS |
                        D3D11_BIND_SHADER_RESOURCE |
                        D3D11_BIND_RENDER_TARGET;

    ComPtr<ID3D11Texture2D> dst;
    HRESULT hr = device->CreateTexture2D(&dstDesc, nullptr, &dst);
    if (FAILED(hr)) return hr;

    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    hr = device->CreateShaderResourceView(srcTexture, nullptr, &srv);
    if (FAILED(hr)) return hr;
    hr = device->CreateUnorderedAccessView(dst.Get(), nullptr, &uav);
    if (FAILED(hr)) return hr;

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = context->Map(constantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return hr;
    memcpy(mapped.pData, &settings, sizeof(settings));
    context->Unmap(constantBuffer, 0);

    context->CSSetShader(shader, nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = { srv.Get() };
    context->CSSetShaderResources(0, 1, srvs);
    ID3D11UnorderedAccessView* uavs[] = { uav.Get() };
    context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    ID3D11Buffer* cbs[] = { constantBuffer };
    context->CSSetConstantBuffers(0, 1, cbs);
    context->Dispatch((srcDesc.Width + 7) / 8, (srcDesc.Height + 7) / 8, 1);

    ID3D11UnorderedAccessView* nullUav[] = { nullptr };
    ID3D11ShaderResourceView* nullSrv[] = { nullptr };
    ID3D11Buffer* nullCb[] = { nullptr };
    context->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
    context->CSSetShaderResources(0, 1, nullSrv);
    context->CSSetConstantBuffers(0, 1, nullCb);
    context->CSSetShader(nullptr, nullptr, 0);

    *outTexture = dst.Detach();
    return S_OK;
}
} // namespace

HRESULT ComputeEngine::ToneMapHdrTextureToHdr(ID3D11Texture2D* srcTexture,
                                              const ToneMapSettings& settings,
                                              ID3D11Texture2D** outTexture) {
    if (!m_valid) return E_FAIL;
    std::lock_guard<std::recursive_mutex> ctxLock(ContextMutex());
    HRESULT hr = EnsureComputeShader(m_csToneMapHdrToHdr, Shaders::g_csToneMapHdrToHdr, sizeof(Shaders::g_csToneMapHdrToHdr), "ToneMapHdrToHdr");
    if (FAILED(hr)) return hr;
    if (!m_toneMapConstantBuffer) {
        D3D11_BUFFER_DESC cbDesc = {};
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.ByteWidth = sizeof(ToneMapSettings);
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = m_d3dDevice->CreateBuffer(&cbDesc, nullptr, &m_toneMapConstantBuffer);
        if (FAILED(hr)) return hr;
    }
    return ToneMapTextureCommon(m_d3dDevice.Get(), m_d3dContext.Get(),
                                m_csToneMapHdrToHdr.Get(),
                                m_toneMapConstantBuffer.Get(), srcTexture,
                                DXGI_FORMAT_R16G16B16A16_FLOAT, settings,
                                outTexture);
}

HRESULT ComputeEngine::ToneMapHdrTextureToSdr(ID3D11Texture2D* srcTexture,
                                              const ToneMapSettings& settings,
                                              ID3D11Texture2D** outTexture) {
    if (!m_valid) return E_FAIL;
    std::lock_guard<std::recursive_mutex> ctxLock(ContextMutex());
    HRESULT hr = EnsureComputeShader(m_csToneMapHdrToSdr, Shaders::g_csToneMapHdrToSdr, sizeof(Shaders::g_csToneMapHdrToSdr), "ToneMapHdrToSdr");
    if (FAILED(hr)) return hr;
    if (!m_toneMapConstantBuffer) {
        D3D11_BUFFER_DESC cbDesc = {};
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.ByteWidth = sizeof(ToneMapSettings);
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = m_d3dDevice->CreateBuffer(&cbDesc, nullptr, &m_toneMapConstantBuffer);
        if (FAILED(hr)) return hr;
    }
    return ToneMapTextureCommon(m_d3dDevice.Get(), m_d3dContext.Get(),
                                m_csToneMapHdrToSdr.Get(),
                                m_toneMapConstantBuffer.Get(), srcTexture,
                                DXGI_FORMAT_R8G8B8A8_UNORM, settings,
                                outTexture);
}

} // namespace QuickView

// ============================================================================
// [GPU Pipeline] Gain Map Composition (ISO 21496-1) — appended outside namespace
// to avoid line-ending matching issues, then wrapped back in.
// ============================================================================
namespace QuickView {

HRESULT ComputeEngine::ComposeGainMap(
    const uint8_t* sdrPixels, int sdrW, int sdrH, int sdrStride,
    PixelFormat sdrFormat,
    const uint8_t* gainPixels, int gainW, int gainH, int gainStride,
    const GpuShaderPayload& payload,
    ID3D11Texture2D** outTexture,
    ID3D11Texture2D** outSdrTex,
    ID3D11Texture2D** outGainTex,
    int gainBytesPerPixel)
{
    if (!m_valid || !sdrPixels || !gainPixels || !outTexture) {
        QV_LOG("Compute_GainMap", TraceLoggingString("InvalidArgs", "Action"));
        return E_INVALIDARG;
    }
    if (sdrW <= 0 || sdrH <= 0 || gainW <= 0 || gainH <= 0) {
        QV_LOG("Compute_GainMap", TraceLoggingString("InvalidDimensions", "Action"));
        return E_INVALIDARG;
    }

    // [Diagnostic] Log composition start
    QV_LOG("Compute_GainMap",
        TraceLoggingString("Compose Start", "Action"),
        TraceLoggingInt32(sdrW, "SdrW"),
        TraceLoggingInt32(sdrH, "SdrH"),
        TraceLoggingInt32(gainW, "GainW"),
        TraceLoggingInt32(gainH, "GainH"),
        TraceLoggingFloat32(payload.targetHeadroom, "Headroom"),
        TraceLoggingFloat32(payload.gainMapMax[0], "MaxGain"));

    // 1. Upload SDR base layer (Can be BGRA8 or R32G32B32A32_FLOAT)
    D3D11_TEXTURE2D_DESC sdrDesc = {};
    sdrDesc.Width = (UINT)sdrW;
    sdrDesc.Height = (UINT)sdrH;
    sdrDesc.MipLevels = 1;
    sdrDesc.ArraySize = 1;
    sdrDesc.Format = ToDxgiToneMapSrcFormat(sdrFormat);
    sdrDesc.SampleDesc.Count = 1;
    sdrDesc.Usage = D3D11_USAGE_IMMUTABLE;
    sdrDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA sdrData = {};
    sdrData.pSysMem = sdrPixels;
    sdrData.SysMemPitch = (UINT)sdrStride;

    ComPtr<ID3D11Texture2D> pSdr;
    HRESULT hr = m_d3dDevice->CreateTexture2D(&sdrDesc, &sdrData, &pSdr);
    if (FAILED(hr)) return hr;

    // 2. Upload Gain Map (1-channel grayscale R8 or 4-channel RGBA8)
    D3D11_TEXTURE2D_DESC gainDesc = {};
    gainDesc.Width = (UINT)gainW;
    gainDesc.Height = (UINT)gainH;
    gainDesc.MipLevels = 1;
    gainDesc.ArraySize = 1;
    gainDesc.Format = (gainBytesPerPixel == 4) ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R8_UNORM;
    gainDesc.SampleDesc.Count = 1;
    gainDesc.Usage = D3D11_USAGE_IMMUTABLE;
    gainDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA gainData = {};
    gainData.pSysMem = gainPixels;
    gainData.SysMemPitch = (UINT)gainStride;

    ComPtr<ID3D11Texture2D> pGain;
    hr = m_d3dDevice->CreateTexture2D(&gainDesc, &gainData, &pGain);
    if (FAILED(hr)) return hr;

    // Capture uploaded textures if requested
    if (outSdrTex) pSdr.CopyTo(outSdrTex);
    if (outGainTex) pGain.CopyTo(outGainTex);

    return ComposeGainMap(pSdr.Get(), pGain.Get(), payload, outTexture);
}

HRESULT QuickView::ComputeEngine::ComposeGainMap(
    ID3D11Texture2D* sdrTex,
    ID3D11Texture2D* gainTex,
    const GpuShaderPayload& payload,
    ID3D11Texture2D** outTexture)
{
    if (!m_valid || !sdrTex || !gainTex || !outTexture) return E_INVALIDARG;
    std::lock_guard<std::recursive_mutex> ctxLock(ContextMutex());

    D3D11_TEXTURE2D_DESC sdrDesc;
    sdrTex->GetDesc(&sdrDesc);
    D3D11_TEXTURE2D_DESC gainDesc;
    gainTex->GetDesc(&gainDesc);

    // 1. Create FP16 output texture
    D3D11_TEXTURE2D_DESC dstDesc = sdrDesc;
    dstDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    dstDesc.Usage = D3D11_USAGE_DEFAULT;
    dstDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    ComPtr<ID3D11Texture2D> pDst;
    HRESULT hr = m_d3dDevice->CreateTexture2D(&dstDesc, nullptr, &pDst);
    if (FAILED(hr)) return hr;

    // 2. Create views
    ComPtr<ID3D11ShaderResourceView> pSdrSRV, pGainSRV;
    ComPtr<ID3D11UnorderedAccessView> pDstUAV;
    hr = m_d3dDevice->CreateShaderResourceView(sdrTex, nullptr, &pSdrSRV);
    if (FAILED(hr)) return hr;
    hr = m_d3dDevice->CreateShaderResourceView(gainTex, nullptr, &pGainSRV);
    if (FAILED(hr)) return hr;
    hr = m_d3dDevice->CreateUnorderedAccessView(pDst.Get(), nullptr, &pDstUAV);
    if (FAILED(hr)) return hr;

    // 3. Upload constant buffer
    hr = EnsureComputeShader(m_csComposeGainMap, Shaders::g_csComposeGainMap, sizeof(Shaders::g_csComposeGainMap), "ComposeGainMap");
    if (FAILED(hr)) return hr;
    hr = EnsureSamplers();
    if (FAILED(hr)) return hr;

    if (!m_gainMapConstantBuffer) {
        D3D11_BUFFER_DESC cbDesc = {};
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.ByteWidth = sizeof(GpuShaderPayload);
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = m_d3dDevice->CreateBuffer(&cbDesc, nullptr, &m_gainMapConstantBuffer);
        if (FAILED(hr)) return hr;
    }

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(m_d3dContext->Map(m_gainMapConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        GpuShaderPayload safePayload = payload;
        uint32_t baseFlags = 0;
        if (sdrDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
            sdrDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM) {
            baseFlags |= 1u; // BaseIsPremultiplied
        }
        if (gainDesc.Format != DXGI_FORMAT_R8_UNORM) {
            baseFlags |= 2u; // GainMapIsRgb
        }
        safePayload._pad5 = baseFlags;
        // The shader expects _pad6 (BaseIsLinear) to indicate linear float base (1) vs sRGB encoded base (0).
        safePayload._pad6 = (sdrDesc.Format == DXGI_FORMAT_R32G32B32A32_FLOAT ||
                             sdrDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT)
                                ? 1
                                : 0;
        memcpy(mapped.pData, &safePayload, sizeof(GpuShaderPayload));
        m_d3dContext->Unmap(m_gainMapConstantBuffer.Get(), 0);
    }

    // 4. Dispatch
    m_d3dContext->CSSetShader(m_csComposeGainMap.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = { pSdrSRV.Get(), pGainSRV.Get() };
    m_d3dContext->CSSetShaderResources(0, 2, srvs);
    ID3D11UnorderedAccessView* uavs[] = { pDstUAV.Get() };
    m_d3dContext->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    ID3D11Buffer* cbs[] = { m_gainMapConstantBuffer.Get() };
    m_d3dContext->CSSetConstantBuffers(0, 1, cbs);
    ID3D11SamplerState* samplers[] = { m_linearSampler.Get() };
    m_d3dContext->CSSetSamplers(0, 1, samplers);

    m_d3dContext->Dispatch((sdrDesc.Width + 7) / 8, (sdrDesc.Height + 7) / 8, 1);

    // 5. Cleanup
    ID3D11UnorderedAccessView* nullUAV[] = { nullptr };
    m_d3dContext->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
    ID3D11ShaderResourceView* nullSRVs[] = { nullptr, nullptr };
    m_d3dContext->CSSetShaderResources(0, 2, nullSRVs);
    ID3D11Buffer* nullCB[] = { nullptr };
    m_d3dContext->CSSetConstantBuffers(0, 1, nullCB);
    m_d3dContext->CSSetShader(nullptr, nullptr, 0);

    m_d3dContext->Flush();
    *outTexture = pDst.Detach();
    return S_OK;
}

HRESULT QuickView::ComputeEngine::ExecuteFsr1Upscale(
    ID3D11Texture2D* srcTexture,
    UINT srcW, UINT srcH,
    UINT dstW, UINT dstH,
    float sharpness,
    ID3D11Texture2D** outTexture)
{
    if (!m_valid || !srcTexture || !outTexture || srcW == 0 || srcH == 0 || dstW == 0 || dstH == 0) return E_INVALIDARG;
    std::lock_guard<std::recursive_mutex> ctxLock(ContextMutex());

    HRESULT hr = EnsureComputeShader(m_csFsrEasu, Shaders::g_csFsrEasu, sizeof(Shaders::g_csFsrEasu), "FSR_EASU");
    if (FAILED(hr)) return hr;
    hr = EnsureComputeShader(m_csFsrRcas, Shaders::g_csFsrRcas, sizeof(Shaders::g_csFsrRcas), "FSR_RCAS");
    if (FAILED(hr)) return hr;

    if (!m_fsrEasuConstantBuffer) {
        D3D11_BUFFER_DESC cbDesc = {};
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.ByteWidth = 32;
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = m_d3dDevice->CreateBuffer(&cbDesc, nullptr, &m_fsrEasuConstantBuffer);
        if (FAILED(hr)) return hr;
    }
    if (!m_fsrRcasConstantBuffer) {
        D3D11_BUFFER_DESC cbDesc = {};
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.ByteWidth = 16;
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = m_d3dDevice->CreateBuffer(&cbDesc, nullptr, &m_fsrRcasConstantBuffer);
        if (FAILED(hr)) return hr;
    }

    // 1. Create intermediate texture for EASU output (dstW x dstH, RGBA8)
    D3D11_TEXTURE2D_DESC easuDstDesc = {};
    easuDstDesc.Width = dstW;
    easuDstDesc.Height = dstH;
    easuDstDesc.MipLevels = 1;
    easuDstDesc.ArraySize = 1;
    easuDstDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    easuDstDesc.SampleDesc.Count = 1;
    easuDstDesc.Usage = D3D11_USAGE_DEFAULT;
    easuDstDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;

    ComPtr<ID3D11Texture2D> pEasuTex;
    hr = m_d3dDevice->CreateTexture2D(&easuDstDesc, nullptr, &pEasuTex);
    if (FAILED(hr)) return hr;

    // 2. Create final texture for RCAS output (dstW x dstH)
    D3D11_TEXTURE2D_DESC rcasDstDesc = easuDstDesc;
    rcasDstDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    ComPtr<ID3D11Texture2D> pRcasTex;
    hr = m_d3dDevice->CreateTexture2D(&rcasDstDesc, nullptr, &pRcasTex);
    if (FAILED(hr)) return hr;

    // 3. Create views for Pass 1 (EASU)
    ComPtr<ID3D11ShaderResourceView> pSrcSRV;
    ComPtr<ID3D11UnorderedAccessView> pEasuUAV;
    hr = m_d3dDevice->CreateShaderResourceView(srcTexture, nullptr, &pSrcSRV);
    if (FAILED(hr)) return hr;
    hr = m_d3dDevice->CreateUnorderedAccessView(pEasuTex.Get(), nullptr, &pEasuUAV);
    if (FAILED(hr)) return hr;

    // 4. Upload EASU Constant Buffer
    struct FsrEasuConstants {
        float InputSize[2];
        float OutputSize[2];
        float InputSizeInv[2];
        float OutputSizeInv[2];
    } easuCB;
    easuCB.InputSize[0] = (float)srcW; easuCB.InputSize[1] = (float)srcH;
    easuCB.OutputSize[0] = (float)dstW; easuCB.OutputSize[1] = (float)dstH;
    easuCB.InputSizeInv[0] = 1.0f / (float)srcW; easuCB.InputSizeInv[1] = 1.0f / (float)srcH;
    easuCB.OutputSizeInv[0] = 1.0f / (float)dstW; easuCB.OutputSizeInv[1] = 1.0f / (float)dstH;

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(m_d3dContext->Map(m_fsrEasuConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &easuCB, sizeof(easuCB));
        m_d3dContext->Unmap(m_fsrEasuConstantBuffer.Get(), 0);
    }

    // 5. Dispatch Pass 1 (EASU)
    m_d3dContext->CSSetShader(m_csFsrEasu.Get(), nullptr, 0);
    ID3D11ShaderResourceView* easuSRVs[] = { pSrcSRV.Get() };
    m_d3dContext->CSSetShaderResources(0, 1, easuSRVs);
    ID3D11UnorderedAccessView* easuUAVs[] = { pEasuUAV.Get() };
    m_d3dContext->CSSetUnorderedAccessViews(0, 1, easuUAVs, nullptr);
    ID3D11Buffer* easuCBs[] = { m_fsrEasuConstantBuffer.Get() };
    m_d3dContext->CSSetConstantBuffers(0, 1, easuCBs);

    m_d3dContext->Dispatch((dstW + 7) / 8, (dstH + 7) / 8, 1);

    // Unbind UAV & SRV before Pass 2
    ID3D11UnorderedAccessView* nullUAV[] = { nullptr };
    m_d3dContext->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
    ID3D11ShaderResourceView* nullSRV[] = { nullptr };
    m_d3dContext->CSSetShaderResources(0, 1, nullSRV);

    // 6. Create views for Pass 2 (RCAS)
    ComPtr<ID3D11ShaderResourceView> pEasuSRV;
    ComPtr<ID3D11UnorderedAccessView> pRcasUAV;
    hr = m_d3dDevice->CreateShaderResourceView(pEasuTex.Get(), nullptr, &pEasuSRV);
    if (FAILED(hr)) return hr;
    hr = m_d3dDevice->CreateUnorderedAccessView(pRcasTex.Get(), nullptr, &pRcasUAV);
    if (FAILED(hr)) return hr;

    // 7. Upload RCAS Constant Buffer
    struct FsrRcasConstants {
        float Sharpness;
        uint32_t Width;
        uint32_t Height;
        uint32_t _pad;
    } rcasCB;
    rcasCB.Sharpness = std::clamp(sharpness, 0.0f, 1.0f);
    rcasCB.Width = dstW;
    rcasCB.Height = dstH;
    rcasCB._pad = 0;

    if (SUCCEEDED(m_d3dContext->Map(m_fsrRcasConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &rcasCB, sizeof(rcasCB));
        m_d3dContext->Unmap(m_fsrRcasConstantBuffer.Get(), 0);
    }

    // 8. Dispatch Pass 2 (RCAS)
    m_d3dContext->CSSetShader(m_csFsrRcas.Get(), nullptr, 0);
    ID3D11ShaderResourceView* rcasSRVs[] = { pEasuSRV.Get() };
    m_d3dContext->CSSetShaderResources(0, 1, rcasSRVs);
    ID3D11UnorderedAccessView* rcasUAVs[] = { pRcasUAV.Get() };
    m_d3dContext->CSSetUnorderedAccessViews(0, 1, rcasUAVs, nullptr);
    ID3D11Buffer* rcasCBs[] = { m_fsrRcasConstantBuffer.Get() };
    m_d3dContext->CSSetConstantBuffers(0, 1, rcasCBs);

    m_d3dContext->Dispatch((dstW + 7) / 8, (dstH + 7) / 8, 1);

    // 9. Cleanup state
    m_d3dContext->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
    m_d3dContext->CSSetShaderResources(0, 1, nullSRV);
    ID3D11Buffer* nullCB[] = { nullptr };
    m_d3dContext->CSSetConstantBuffers(0, 1, nullCB);
    m_d3dContext->CSSetShader(nullptr, nullptr, 0);

    m_d3dContext->Flush();
    *outTexture = pRcasTex.Detach();
    return S_OK;
}

HRESULT QuickView::ComputeEngine::ExecuteSuperResolution(
    ID3D11Texture2D* srcTexture,
    UINT srcW, UINT srcH,
    UINT dstW, UINT dstH,
    float sharpness,
    SimplePredicate checkCancel,
    ID3D11Texture2D** outTexture,
    QVX_ProgressCallback onProgress,
    void* progressUserData)
{
    if (!m_valid || !srcTexture || !outTexture || srcW == 0 || srcH == 0 || dstW == 0 || dstH == 0) {
        return E_INVALIDARG;
    }

    // 1. Attempt Plugin Execution (if enabled and present)
    auto& host = PluginHost::Instance();
    if (host.IsSrPluginEnabled() && host.EnsureSrContext(m_d3dDevice.Get())) {
        // Create destination texture for plugin output
        D3D11_TEXTURE2D_DESC srcDesc{};
        srcTexture->GetDesc(&srcDesc);

        D3D11_TEXTURE2D_DESC dstDesc = srcDesc;
        dstDesc.Width = dstW;
        dstDesc.Height = dstH;
        dstDesc.MipLevels = 1;
        dstDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

        ComPtr<ID3D11Texture2D> pDstTex;
        HRESULT hr = m_d3dDevice->CreateTexture2D(&dstDesc, nullptr, &pDstTex);
        if (SUCCEEDED(hr)) {
            auto cancelWrapper = [](void* ctx) -> bool {
                if (!ctx) return false;
                auto* pred = static_cast<SimplePredicate*>(ctx);
                return (*pred)();
            };

            int32_t srRes = host.ExecuteSrUpscaleGpu(
                m_d3dDevice.Get(),
                srcTexture, srcW, srcH,
                pDstTex.Get(), dstW, dstH,
                checkCancel ? cancelWrapper : nullptr,
                checkCancel ? &checkCancel : nullptr,
                onProgress,
                progressUserData
            );

            if (srRes == (int32_t)S_OK) {
                *outTexture = pDstTex.Detach();
                return S_OK;
            } else if (srRes == (int32_t)E_ABORT) {
                return E_ABORT;
            }
            // If plugin failed, fall through to built-in FSR 1.0
        }
    }

    // 2. Built-in Fallback: AMD FSR 1.0 (EASU + RCAS)
    return ExecuteFsr1Upscale(srcTexture, srcW, srcH, dstW, dstH, sharpness, outTexture);
}

} // namespace QuickView
