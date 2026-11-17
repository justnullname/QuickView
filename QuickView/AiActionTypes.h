#pragma once
// ============================================================================
// AiActionTypes.h - Core Data Types for QuickView AI Actions & Model Profiles
// ============================================================================
// Design Principles:
// 1. Separation of Concerns: Model Profiles (Endpoints/Keys) vs Actions (Prompts/Scopes)
// 2. Open Protocol Design: Not hardcoded to specific model versions; supports user-defined model IDs
// 3. No Exceptions: Strict -fno-exceptions compliance; compact memory footprint
// ============================================================================

#include <string>
#include <vector>
#include <cstdint>
#include <string_view>
#include <d2d1.h>

namespace QuickView::AI {

// Supported API Protocol Types
enum class ApiProtocol : uint8_t {
    OpenAiChat = 0,         // OpenAI Chat Completions (Vision Multi-modal, e.g. GPT-4o, Gemini OpenAI-compat, Grok, Qwen-VL)
    OpenAiImagesEdit,       // OpenAI Images Edit (Inpainting multipart/form-data)
    OpenAiImagesGenerate,   // OpenAI Images Generations (DALL-E 3, Flux)
    GeminiNative,           // Google Gemini REST API (v1beta/models/{model}:generateContent)
    StabilityInpaint,       // Stability AI / SD Inpaint API
    ComfyUI,                // Local ComfyUI Webhook
    CustomJsonPost          // Generic HTTP POST JSON
};

// Maximum Resolution Policy for API Payloads
enum class MaxResolution : uint32_t {
    Standard_1K = 1024,     // Standard 1024px (Recommended for SD / Flux / DALL-E)
    FHD_2K      = 2048,     // 2K 2048px (Balanced High-Res)
    Original_4K = 4096,     // Up to 4K 4096px (Native Multi-modal: Gemini 2.0 / GPT-4o)
    NoLimit     = 0         // Send raw resolution (Advanced)
};

// Scope Mode: Full Image vs Selection / Inpainting
enum class ScopeMode : uint8_t {
    FullImage = 0,          // Always process full image
    CropAndBlend = 1        // Crop selection, inpaint/generate, and alpha-feather back onto base
};

// Target Output Aspect Ratio & Size Framing
enum class OutputAspectRatio : uint8_t {
    Auto = 0,               // Adaptive: match input source aspect ratio
    Square_1_1,             // 1:1 Square (1024x1024)
    Landscape_16_9,         // 16:9 Landscape widescreen (1344x768 / 1792x1024)
    Portrait_9_16,          // 9:16 Portrait wallpaper (768x1344 / 1024x1792)
    Standard_4_3,           // 4:3 Standard landscape (1152x864)
    Vertical_3_4            // 3:4 Vertical standard (864x1152)
};

// Target Output Resolution Policy for AI Image Generation
enum class TargetResolution : uint8_t {
    Res_1K = 0,             // 1K Standard (1024px)
    Res_2K = 1,             // 2K High Definition (2048px - Recommended)
    Res_4K = 2              // 4K Ultra HD (4096px - Native Gemini 3.1 / SD Hi-Res)
};

// Model Profile Descriptor (Connection credentials & Endpoint defaults)
struct ModelProfile {
    std::string id;                         // Unique Profile ID (e.g. "gemini_official", "siliconflow")
    std::wstring displayName;               // Display Name in UI (e.g. L"Google Gemini", L"SiliconFlow")
    ApiProtocol protocol = ApiProtocol::OpenAiChat;
    std::string baseUrl;                    // Endpoint Base URL
    std::string encryptedApiKey;            // DPAPI-encrypted Base64 cipher text
    std::string defaultModel;               // User-editable model identifier (e.g. "gemini-2.0-flash", "gpt-4o")
    MaxResolution maxResolution = MaxResolution::Standard_1K;
    int timeoutSeconds = 600;               // Network timeout in seconds (0 = infinite)
    bool isCustom = true;                   // True if user-created, false if built-in template
    std::vector<std::string> fetchedModels; // Dynamically fetched model list from endpoint
};

// AI Action Descriptor (Prompt template, parameter mappings)
struct ActionDesc {
    std::string id;                         // Unique Action ID
    std::wstring name;                      // Action display name (e.g. L"Inpaint & Remove")
    std::string modelProfileId;             // Bound ModelProfile ID (empty = use global default profile)
    std::wstring promptTemplate;            // Prompt template (supports {prompt} macro)
    std::wstring negativePrompt;            // Optional negative prompt
    ScopeMode scopeMode = ScopeMode::FullImage;  // Processing target scope
    OutputAspectRatio aspectRatio = OutputAspectRatio::Auto; // Target output aspect ratio & framing
    TargetResolution targetResolution = TargetResolution::Res_2K; // Target output resolution policy (1K/2K/4K)
    float denoisingStrength = 0.35f;        // Denoising strength for img2img (0.0~1.0, default 0.35 for fidelity)
    int samplingSteps = 25;                 // Diffusion sampling steps (15~50, default 25)
    float cfgScale = 7.0f;                  // Prompt guidance scale (4.0~10.0, default 7.0)
    bool isCollapsed = true;                // Collapsible card state in Settings UI
};

// Execution Context & Result
struct ExecutionResult {
    bool success = false;
    uint64_t taskId = 0;
    uint32_t httpStatusCode = 0;
    std::wstring sourceImagePath;
    std::wstring errorMessage;              // Human-readable error description with hints
    std::wstring mainTitle;                 // Short main instruction for TaskDialog
    std::wstring detailMessage;             // Extracted high-priority semantic error detail
    std::wstring actionAdvice;              // Actionable troubleshooting advice
    std::wstring textContent;               // Text response returned by LLM (if any)
    std::string rawResponseBody;            // For debugging / detailed inspection (expanded in TaskDialog)
    std::vector<uint8_t> resultImageData;   // Decoded PNG/JPEG/WebP raw bytes
    uint32_t imageWidth = 0;
    uint32_t imageHeight = 0;
};

struct VisualFrameSnapshot {
    std::wstring imagePath;
    int netRotation = 0;
    bool netFlipH = false;
    bool flipV = false;
    bool hasCrop = false;
    float cropLeft = 0.0f;
    float cropTop = 0.0f;
    float cropRight = 0.0f;
    float cropBottom = 0.0f;
    bool rawForceFullDecode = false;
    int effectiveExif = 1;
    std::vector<uint8_t> memoryBgra;
    uint32_t memoryWidth = 0;
    uint32_t memoryHeight = 0;
    uint32_t memoryStride = 0;
};

using VisualFrameSnapshotFn = bool (*)(VisualFrameSnapshot& outSnapshot);
using VisualBgraProviderFn = bool (*)(
    const VisualFrameSnapshot& snapshot,
    std::vector<uint8_t>& outBgra,
    uint32_t& outWidth,
    uint32_t& outHeight,
    uint32_t& outStride);

constexpr UINT WM_AI_ACTION_COMPLETED = WM_APP + 62;
constexpr UINT WM_AI_ACTION_PROGRESS  = WM_APP + 63;

struct AsyncAiProgressInfo {
    uint64_t taskId = 0;
    std::wstring message;
    float progress = 0.0f;
};

struct AsyncAiImageResult {
    uint64_t taskId = 0;
    std::wstring actionName;
    ExecutionResult execResult;
};

} // namespace QuickView::AI
