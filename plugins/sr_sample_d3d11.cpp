// ============================================================================
// sr_sample_d3d11.cpp - Official Reference D3D11 Super-Resolution Plugin
// ============================================================================
// Demonstrates:
// 1. Pure C ABI export via qvx_init / qvx_shutdown
// 2. GPU VRAM Direct 0-Copy via D3D11 Texture2D & Compute Shader
// 3. Multi-threaded worker context isolation (Deferred Context)
// 4. Millisecond-level Cancellation Token response
// ============================================================================

#include "../QuickView/Plugin/qvx_sdk.hpp"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

// Embedded Fast 2x Edge-Adaptive Compute Shader
static const char* HLSL_SampleSR = R"(
Texture2D<float4> SrcTex : register(t0);
RWTexture2D<float4> DstTex : register(u0);

cbuffer SRParams : register(b0)
{
    uint2 InDim;
    uint2 OutDim;
    float Sharpness;
    float Denoise;
    float2 _pad;
};

// Catmull-Rom weights
float4 CubicWeights(float x)
{
    float x2 = x * x;
    float x3 = x2 * x;
    return float4(
        -0.5 * x3 + x2 - 0.5 * x,
         1.5 * x3 - 2.5 * x2 + 1.0,
        -1.5 * x3 + 2.0 * x2 + 0.5 * x,
         0.5 * x3 - 0.5 * x2
    );
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= OutDim.x || dtid.y >= OutDim.y) return;

    // Sub-pixel aligned coordinate
    float2 uv = (float2(dtid.xy) + 0.5f) / float2(OutDim);
    float2 srcCoord = uv * float2(InDim) - 0.5f;
    int2 base = int2(floor(srcCoord));
    float2 f = frac(srcCoord);

    int2 maxPos = int2(InDim) - 1;

    float4 wx = CubicWeights(f.x);
    float4 wy = CubicWeights(f.y);

    float4 color = float4(0, 0, 0, 0);
    float4 minCol = float4(1e9, 1e9, 1e9, 1e9);
    float4 maxCol = float4(-1e9, -1e9, -1e9, -1e9);

    for (int y = -1; y <= 2; ++y)
    {
        for (int x = -1; x <= 2; ++x)
        {
            int2 sPos = clamp(base + int2(x, y), int2(0, 0), maxPos);
            float4 s = SrcTex[sPos];
            color += s * (wx[x + 1] * wy[y + 1]);

            if (x >= 0 && x <= 1 && y >= 0 && y <= 1)
            {
                minCol = min(minCol, s);
                maxCol = max(maxCol, s);
            }
        }
    }

    // Adaptive soft clamp
    float4 pad = (maxCol - minCol) * (0.05 + 0.15 * saturate(Sharpness));
    color = clamp(color, minCol - pad, maxCol + pad);

    if (color.a < 0.001f) color.a = 1.0f;

    DstTex[dtid.xy] = color;
}
)";

struct SRConstantBuffer {
    uint32_t InDim[2];
    uint32_t OutDim[2];
    float Sharpness;
    float Denoise;
    float _pad[2];
};

struct PluginContextImpl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> deferredContext;
    ComPtr<ID3D11ComputeShader> computeShader;
    ComPtr<ID3D11Buffer> constantBuffer;
    bool isHdr = false;
};

static const QVX_SR_ModelInfo s_sampleModels[] = {
    {
        sizeof(QVX_SR_ModelInfo),
        "sample_fast_2x",
        "Fast 2x Edge-Adaptive Upscaler (Sample)",
        "Fast edge-preserving cubic upscaler",
        2.0f,
        true,
        true,
        0,
        "",
        512
    },
    {
        sizeof(QVX_SR_ModelInfo),
        "sample_fast_4x",
        "Fast 4x Edge-Adaptive Upscaler (Sample)",
        "Fast 4x edge-preserving cubic upscaler",
        4.0f,
        true,
        true,
        0,
        "",
        512
    }
};

static uint32_t Sample_GetModelCount(void) {
    return static_cast<uint32_t>(sizeof(s_sampleModels) / sizeof(s_sampleModels[0]));
}

static const QVX_SR_ModelInfo* Sample_GetModelInfo(uint32_t index) {
    if (index >= Sample_GetModelCount()) return nullptr;
    return &s_sampleModels[index];
}

static QVX_SR_Context Sample_CreateContext(ID3D11Device* pDevice, [[maybe_unused]] const char* model_id) {
    if (!pDevice) return nullptr;

    auto* impl = new (std::nothrow) PluginContextImpl();
    if (!impl) return nullptr;

    impl->device = pDevice;

    // Create worker Deferred Context for thread isolation
    HRESULT hr = pDevice->CreateDeferredContext(0, &impl->deferredContext);
    if (FAILED(hr)) {
        // Fallback to device's immediate context if deferred unsupported
        pDevice->GetImmediateContext(&impl->deferredContext);
    }

    // Compile Embedded Shader
    ComPtr<ID3DBlob> csBlob;
    ComPtr<ID3DBlob> errorBlob;
    hr = D3DCompile(HLSL_SampleSR, strlen(HLSL_SampleSR), "SampleSR_CS", nullptr, nullptr, "CSMain", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &csBlob, &errorBlob);
    if (FAILED(hr) || !csBlob) {
        delete impl;
        return nullptr;
    }

    hr = pDevice->CreateComputeShader(csBlob->GetBufferPointer(), csBlob->GetBufferSize(), nullptr, &impl->computeShader);
    if (FAILED(hr)) {
        delete impl;
        return nullptr;
    }

    // Create Constant Buffer
    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = sizeof(SRConstantBuffer);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = pDevice->CreateBuffer(&cbDesc, nullptr, &impl->constantBuffer);
    if (FAILED(hr)) {
        delete impl;
        return nullptr;
    }

    return static_cast<QVX_SR_Context>(impl);
}

static void Sample_DestroyContext(QVX_SR_Context ctx) {
    if (ctx) {
        auto* impl = static_cast<PluginContextImpl*>(ctx);
        delete impl;
    }
}

static int32_t Sample_UpscaleGpu(
    QVX_SR_Context ctx,
    ID3D11Texture2D* in_tex,
    ID3D11Texture2D* out_tex,
    const QVX_SR_ExecuteParams* params
) {
    if (!ctx || !in_tex || !out_tex || !params) return QVX_E_INVALIDARG;
    if (params->in_width == 0 || params->in_height == 0 || params->out_width == 0 || params->out_height == 0) return QVX_E_INVALIDARG;

    // Check cancellation before work
    if (qvx::IsCancelled(params)) {
        return QVX_E_ABORT;
    }

    auto* impl = static_cast<PluginContextImpl*>(ctx);
    auto pCtx = impl->deferredContext;
    if (!pCtx) return QVX_E_FAIL;

    // Create Views
    ComPtr<ID3D11ShaderResourceView> pSrcSRV;
    HRESULT hr = impl->device->CreateShaderResourceView(in_tex, nullptr, &pSrcSRV);
    if (FAILED(hr)) return hr;

    ComPtr<ID3D11UnorderedAccessView> pDstUAV;
    hr = impl->device->CreateUnorderedAccessView(out_tex, nullptr, &pDstUAV);
    if (FAILED(hr)) return hr;

    // Upload Constants
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = pCtx->Map(impl->constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        auto* cb = static_cast<SRConstantBuffer*>(mapped.pData);
        cb->InDim[0] = params->in_width;
        cb->InDim[1] = params->in_height;
        cb->OutDim[0] = params->out_width;
        cb->OutDim[1] = params->out_height;
        cb->Sharpness = params->sharpness;
        cb->Denoise = params->denoise;
        pCtx->Unmap(impl->constantBuffer.Get(), 0);
    } else {
        return hr;
    }

    // Check cancellation before dispatch
    if (qvx::IsCancelled(params)) {
        return QVX_E_ABORT;
    }

    // Bind & Dispatch Compute Shader
    pCtx->CSSetShader(impl->computeShader.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = { pSrcSRV.Get() };
    pCtx->CSSetShaderResources(0, 1, srvs);
    ID3D11UnorderedAccessView* uavs[] = { pDstUAV.Get() };
    pCtx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    ID3D11Buffer* cbs[] = { impl->constantBuffer.Get() };
    pCtx->CSSetConstantBuffers(0, 1, cbs);

    pCtx->Dispatch((params->out_width + 7) / 8, (params->out_height + 7) / 8, 1);

    // Unbind
    ID3D11ShaderResourceView* nullSRV[] = { nullptr };
    ID3D11UnorderedAccessView* nullUAV[] = { nullptr };
    ID3D11Buffer* nullCB[] = { nullptr };
    pCtx->CSSetShaderResources(0, 1, nullSRV);
    pCtx->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
    pCtx->CSSetConstantBuffers(0, 1, nullCB);
    pCtx->CSSetShader(nullptr, nullptr, 0);

    // If deferred context was used, execute command list on immediate context
    ComPtr<ID3D11CommandList> cmdList;
    if (SUCCEEDED(pCtx->FinishCommandList(FALSE, &cmdList)) && cmdList) {
        ComPtr<ID3D11DeviceContext> immContext;
        impl->device->GetImmediateContext(&immContext);
        if (immContext) {
            immContext->ExecuteCommandList(cmdList.Get(), FALSE);
            immContext->Flush();
        }
    } else {
        pCtx->Flush();
    }

    return QVX_OK;
}

static uint32_t Sample_GetParamCount(void) {
    return 0;
}

static const QVX_ParamDesc* Sample_GetParamDesc([[maybe_unused]] uint32_t index) {
    return nullptr;
}

static int32_t Sample_GetParamValue([[maybe_unused]] QVX_SR_Context ctx, [[maybe_unused]] const char* param_id, [[maybe_unused]] float* out_val) {
    return QVX_E_NOTIMPL;
}

static int32_t Sample_SetParamValue([[maybe_unused]] QVX_SR_Context ctx, [[maybe_unused]] const char* param_id, [[maybe_unused]] float val) {
    return QVX_E_NOTIMPL;
}

static const QVX_SR_VTable s_sampleSRVTable = {
    sizeof(QVX_SR_VTable),
    Sample_GetModelCount,
    Sample_GetModelInfo,
    Sample_CreateContext,
    Sample_DestroyContext,
    Sample_UpscaleGpu,
    nullptr, // upscale_shared
    nullptr, // upscale_cpu
    Sample_GetParamCount,
    Sample_GetParamDesc,
    Sample_GetParamValue,
    Sample_SetParamValue
};

static const void* Sample_GetInterface(uint32_t interface_id, uint32_t version) {
    if (interface_id == QVX_IFACE_SUPER_RESOLUTION && version == QVX_SR_INTERFACE_VERSION) {
        return &s_sampleSRVTable;
    }
    return nullptr;
}

static const QVX_PluginHeader s_sampleHeader = {
    sizeof(QVX_PluginHeader),
    QVX_ABI_VERSION,
    "com.quickview.sr.sample",
    "QuickView Official Reference D3D11 SR Plugin",
    "QuickView Team",
    "1.0.0",
    QVX_FLAG_THREAD_SAFE,
    (1 << QVX_IFACE_SUPER_RESOLUTION),
    Sample_GetInterface
};

QVX_EXPORT_PLUGIN(s_sampleHeader)
