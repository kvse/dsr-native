// Vulkan backend (Android).
//
// Executes recorded NVN command lists on the device GPU:
//   - render targets, clears and texture copies on real GPU images (keyed by guest storage address);
//   - draws: Maxwell shaders translated to SPIR-V (shader/translator.cpp), pipelines built from the
//     mirrored NVN state, vertex/index/uniform data copied from game memory into a streaming buffer,
//     textures deswizzled from block-linear game memory and uploaded on first use;
//   - presentation of the window texture to a swapchain on the device screen.
//
// Execution is deliberately simple: each submitted command list is recorded into one Vulkan command
// buffer, submitted and waited on. NVN syncs are therefore always already signaled, and the streaming
// buffer and descriptor pool can be reset at the start of every list. Dynamic rendering (Vulkan 1.3)
// avoids render-pass objects entirely.
#if defined(__ANDROID__)

#define VK_USE_PLATFORM_ANDROID_KHR
#define VK_NO_PROTOTYPES
#include <android/native_window.h>
#include <vulkan/vulkan.h>
#include <adrenotools/bcenabler.h>
#include <adrenotools/driver.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <type_traits>
#include <memory>
#include <thread>
#include <deque>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <sys/resource.h>
#include <dlfcn.h>
#include <unistd.h>

#include "common/log.h"
#include "common/profiler.h"
#include "common/platform.h"
#include "hle/nvn/backend.h"
#include "hle/nvn/bc_decode.h"
#include "hle/nvn/etc_encode.h"
#include "hle/nvn/parallel.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include "runtime/runtime.h"
#include "hle/nvn/layout.h"
#include "hle/os/os.h"
#include "hle/nvn/shader/translator.h"
#include "hle/nvn/state.h"
#include "hle/video.h"

namespace dsr::hle::nvn {
#include "hle/nvn/vk_dispatch.h"
namespace {
constexpr const char* kTag = "vulkan";

// Opens the Vulkan driver: a custom one (settings.txt `gpu_driver`, prepared by the Android front end into the
// app's private folder) through libadrenotools, or the system's libvulkan. Loads the global entry points.
bool LoadVulkanLibrary() {
    static int state = 0;  // 0 = not tried, 1 = loaded, 2 = failed
    if (state) return state == 1;
    state = 2;
    const RuntimeConfig& cfg = GetRuntime().config;
    void* lib = nullptr;
    if (!cfg.gpu_driver_lib.empty()) {
        lib = adrenotools_open_libvulkan(RTLD_NOW | RTLD_LOCAL, ADRENOTOOLS_DRIVER_CUSTOM, nullptr,
                                         (cfg.native_lib_dir + "/").c_str(), cfg.gpu_driver_dir.c_str(),
                                         cfg.gpu_driver_lib.c_str(), nullptr, nullptr);
        if (lib)
            LOGI(kTag, "custom GPU driver: %s (%s) loaded through libadrenotools", cfg.gpu_driver.c_str(),
                 cfg.gpu_driver_lib.c_str());
        else
            LOGW(kTag, "custom GPU driver %s (%s) could not be loaded: using the system driver", cfg.gpu_driver.c_str(),
                 cfg.gpu_driver_lib.c_str());
    }
    if (!lib) lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        LOGE(kTag, "libvulkan.so could not be opened: %s", dlerror());
        return false;
    }
    vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
    if (!vkGetInstanceProcAddr) {
        LOGE(kTag, "the Vulkan driver has no vkGetInstanceProcAddr");
        return false;
    }
#define DSR_VK_LOAD(name) name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(VK_NULL_HANDLE, #name));
    DSR_VK_GLOBAL_FUNCS(DSR_VK_LOAD)
#undef DSR_VK_LOAD
    state = vkCreateInstance ? 1 : 2;
    return state == 1;
}
void LoadInstanceFunctions(VkInstance instance) {
#define DSR_VK_LOAD(name) name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(instance, #name));
    DSR_VK_INSTANCE_FUNCS(DSR_VK_LOAD)
    DSR_VK_DEVICE_FUNCS(DSR_VK_LOAD)
#undef DSR_VK_LOAD
}
// Device functions straight from the driver (no loader trampoline); ones it does not return keep the
// instance-level pointer.
void LoadDeviceFunctions(VkDevice device) {
#define DSR_VK_LOAD(name) \
    if (auto f = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name))) name = f;
    DSR_VK_DEVICE_FUNCS(DSR_VK_LOAD)
#undef DSR_VK_LOAD
}
constexpr VkDeviceSize kStreamSize = 48ull << 20;  // per slot (four slots)
constexpr VkDeviceSize kMaxUniformRange = 65536;
constexpr uint32_t kCbufBase = 3;      // NVN uniform-buffer binding b is read by shaders as c[b + 3]
constexpr uint32_t kCompilerConstants = 1;  // c[1]: literal constants from the shader's control section

bool IsDepthFormat(int f) { return f >= 49 && f <= 54; }
bool HasDepth(int f) { return f >= 50 && f <= 54; }
bool HasStencil(int f) { return f == 49 || f == 53 || f == 54; }
bool IsCompressed(int f) { return f >= 65 && f <= 80; }
bool IgnoresAlpha(int f) { return f == 55 || (f >= 109 && f <= 120); }  // RGBX formats
VkImageAspectFlags AspectOf(int f) {
    if (!IsDepthFormat(f)) return VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageAspectFlags a = 0;
    if (HasDepth(f)) a |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (HasStencil(f)) a |= VK_IMAGE_ASPECT_STENCIL_BIT;
    return a;
}

// ---- format tables ------------------------------------------------------------------------------------------
// The sRGB / linear counterpart of a format (NVN texture views can switch between them), or UNDEFINED.
VkFormat SrgbTwin(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SRGB;
        case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_SRGB;
        case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_BC1_RGB_UNORM_BLOCK: return VK_FORMAT_BC1_RGB_SRGB_BLOCK;
        case VK_FORMAT_BC1_RGB_SRGB_BLOCK: return VK_FORMAT_BC1_RGB_UNORM_BLOCK;
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case VK_FORMAT_BC2_UNORM_BLOCK: return VK_FORMAT_BC2_SRGB_BLOCK;
        case VK_FORMAT_BC2_SRGB_BLOCK: return VK_FORMAT_BC2_UNORM_BLOCK;
        case VK_FORMAT_BC3_UNORM_BLOCK: return VK_FORMAT_BC3_SRGB_BLOCK;
        case VK_FORMAT_BC3_SRGB_BLOCK: return VK_FORMAT_BC3_UNORM_BLOCK;
        case VK_FORMAT_BC7_UNORM_BLOCK: return VK_FORMAT_BC7_SRGB_BLOCK;
        case VK_FORMAT_BC7_SRGB_BLOCK: return VK_FORMAT_BC7_UNORM_BLOCK;
        case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK: return VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK;
        case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK: return VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
        case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK: return VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK;
        case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK: return VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK;
        case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK: return VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK;
        case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK: return VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK;
        default: return VK_FORMAT_UNDEFINED;
    }
}

// NVN format -> candidate Vulkan formats for images, best first.
std::vector<VkFormat> ImageCandidates(int f) {
    switch (f) {
        case 1: return {VK_FORMAT_R8_UNORM};
        case 2: return {VK_FORMAT_R8_SNORM};
        case 3: return {VK_FORMAT_R8_UINT};
        case 4: return {VK_FORMAT_R8_SINT};
        case 5: return {VK_FORMAT_R16_SFLOAT};
        case 6: return {VK_FORMAT_R16_UNORM};
        case 7: return {VK_FORMAT_R16_SNORM};
        case 8: return {VK_FORMAT_R16_UINT};
        case 9: return {VK_FORMAT_R16_SINT};
        case 10: return {VK_FORMAT_R32_SFLOAT};
        case 11: return {VK_FORMAT_R32_UINT};
        case 12: return {VK_FORMAT_R32_SINT};
        case 13: return {VK_FORMAT_R8G8_UNORM};
        case 14: return {VK_FORMAT_R8G8_SNORM};
        case 15: return {VK_FORMAT_R8G8_UINT};
        case 16: return {VK_FORMAT_R8G8_SINT};
        case 17: return {VK_FORMAT_R16G16_SFLOAT};
        case 18: return {VK_FORMAT_R16G16_UNORM};
        case 19: return {VK_FORMAT_R16G16_SNORM};
        case 20: return {VK_FORMAT_R16G16_UINT};
        case 21: return {VK_FORMAT_R16G16_SINT};
        case 22: return {VK_FORMAT_R32G32_SFLOAT};
        case 23: return {VK_FORMAT_R32G32_UINT};
        case 24: return {VK_FORMAT_R32G32_SINT};
        case 37: return {VK_FORMAT_R8G8B8A8_UNORM};
        case 38: return {VK_FORMAT_R8G8B8A8_SNORM};
        case 39: return {VK_FORMAT_R8G8B8A8_UINT};
        case 40: return {VK_FORMAT_R8G8B8A8_SINT};
        case 41: return {VK_FORMAT_R16G16B16A16_SFLOAT};
        case 42: return {VK_FORMAT_R16G16B16A16_UNORM};
        case 43: return {VK_FORMAT_R16G16B16A16_SNORM};
        case 44: return {VK_FORMAT_R16G16B16A16_UINT};
        case 45: return {VK_FORMAT_R16G16B16A16_SINT};
        case 46: return {VK_FORMAT_R32G32B32A32_SFLOAT};
        case 47: return {VK_FORMAT_R32G32B32A32_UINT};
        case 48: return {VK_FORMAT_R32G32B32A32_SINT};
        case 49: return {VK_FORMAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT};
        case 50: return {VK_FORMAT_D16_UNORM, VK_FORMAT_D32_SFLOAT};
        case 51: return {VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT};
        case 52: return {VK_FORMAT_D32_SFLOAT};
        case 53: return {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT};
        case 54:  // `hole_test` 64: D24S8 first (square-hole diagnostic)
            if (dsr::GetRuntime().config.hole_test & 64) return {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT};
            return {VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT};
        case 55:
        case 56: return {VK_FORMAT_R8G8B8A8_SRGB};
        case 57: return {VK_FORMAT_R4G4B4A4_UNORM_PACK16};
        case 58:
        case 59: return {VK_FORMAT_A1R5G5B5_UNORM_PACK16};
        case 60: return {VK_FORMAT_R5G6B5_UNORM_PACK16};
        case 61: return {VK_FORMAT_A2B10G10R10_UNORM_PACK32};
        case 62: return {VK_FORMAT_A2B10G10R10_UINT_PACK32};
        case 63: return {VK_FORMAT_B10G11R11_UFLOAT_PACK32};
        case 64: return {VK_FORMAT_E5B9G9R9_UFLOAT_PACK32};
        case 65: return {VK_FORMAT_BC1_RGB_UNORM_BLOCK};
        case 66: return {VK_FORMAT_BC1_RGBA_UNORM_BLOCK};
        case 67: return {VK_FORMAT_BC2_UNORM_BLOCK};
        case 68: return {VK_FORMAT_BC3_UNORM_BLOCK};
        case 69: return {VK_FORMAT_BC1_RGB_SRGB_BLOCK};
        case 70: return {VK_FORMAT_BC1_RGBA_SRGB_BLOCK};
        case 71: return {VK_FORMAT_BC2_SRGB_BLOCK};
        case 72: return {VK_FORMAT_BC3_SRGB_BLOCK};
        case 73: return {VK_FORMAT_BC4_UNORM_BLOCK};
        case 74: return {VK_FORMAT_BC4_SNORM_BLOCK};
        case 75: return {VK_FORMAT_BC5_UNORM_BLOCK};
        case 76: return {VK_FORMAT_BC5_SNORM_BLOCK};
        case 77: return {VK_FORMAT_BC7_UNORM_BLOCK};
        case 78: return {VK_FORMAT_BC7_SRGB_BLOCK};
        case 79: return {VK_FORMAT_BC6H_SFLOAT_BLOCK};
        case 80: return {VK_FORMAT_BC6H_UFLOAT_BLOCK};
        // RGBX: RGBA storage with alpha ignored (sampled as 1, see IgnoresAlpha).
        case 109: return {VK_FORMAT_R8G8B8A8_UNORM};
        case 110: return {VK_FORMAT_R8G8B8A8_SNORM};
        case 111: return {VK_FORMAT_R8G8B8A8_UINT};
        case 112: return {VK_FORMAT_R8G8B8A8_SINT};
        case 113: return {VK_FORMAT_R16G16B16A16_SFLOAT};
        case 114: return {VK_FORMAT_R16G16B16A16_UNORM};
        case 115: return {VK_FORMAT_R16G16B16A16_SNORM};
        case 116: return {VK_FORMAT_R16G16B16A16_UINT};
        case 117: return {VK_FORMAT_R16G16B16A16_SINT};
        case 118: return {VK_FORMAT_R32G32B32A32_SFLOAT};
        case 119: return {VK_FORMAT_R32G32B32A32_UINT};
        case 120: return {VK_FORMAT_R32G32B32A32_SINT};
        default: return {};
    }
}

struct VertexFormat {
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t bytes = 0;
    bool is_int = false, is_signed = false;
};
VertexFormat VertexFormatOf(int f) {
    // components x component bytes; integer formats are fetched as integers.
    struct E { int nvn; VkFormat vk; uint32_t bytes; bool i, s; };
    static const E kTable[] = {
        {1, VK_FORMAT_R8_UNORM, 1, false, false}, {2, VK_FORMAT_R8_SNORM, 1, false, false},
        {3, VK_FORMAT_R8_UINT, 1, true, false}, {4, VK_FORMAT_R8_SINT, 1, true, true},
        {5, VK_FORMAT_R16_SFLOAT, 2, false, false}, {6, VK_FORMAT_R16_UNORM, 2, false, false},
        {7, VK_FORMAT_R16_SNORM, 2, false, false}, {8, VK_FORMAT_R16_UINT, 2, true, false},
        {9, VK_FORMAT_R16_SINT, 2, true, true}, {10, VK_FORMAT_R32_SFLOAT, 4, false, false},
        {11, VK_FORMAT_R32_UINT, 4, true, false}, {12, VK_FORMAT_R32_SINT, 4, true, true},
        {13, VK_FORMAT_R8G8_UNORM, 2, false, false}, {14, VK_FORMAT_R8G8_SNORM, 2, false, false},
        {15, VK_FORMAT_R8G8_UINT, 2, true, false}, {16, VK_FORMAT_R8G8_SINT, 2, true, true},
        {17, VK_FORMAT_R16G16_SFLOAT, 4, false, false}, {18, VK_FORMAT_R16G16_UNORM, 4, false, false},
        {19, VK_FORMAT_R16G16_SNORM, 4, false, false}, {20, VK_FORMAT_R16G16_UINT, 4, true, false},
        {21, VK_FORMAT_R16G16_SINT, 4, true, true}, {22, VK_FORMAT_R32G32_SFLOAT, 8, false, false},
        {23, VK_FORMAT_R32G32_UINT, 8, true, false}, {24, VK_FORMAT_R32G32_SINT, 8, true, true},
        {25, VK_FORMAT_R8G8B8_UNORM, 3, false, false}, {26, VK_FORMAT_R8G8B8_SNORM, 3, false, false},
        {27, VK_FORMAT_R8G8B8_UINT, 3, true, false}, {28, VK_FORMAT_R8G8B8_SINT, 3, true, true},
        {29, VK_FORMAT_R16G16B16_SFLOAT, 6, false, false}, {30, VK_FORMAT_R16G16B16_UNORM, 6, false, false},
        {31, VK_FORMAT_R16G16B16_SNORM, 6, false, false}, {32, VK_FORMAT_R16G16B16_UINT, 6, true, false},
        {33, VK_FORMAT_R16G16B16_SINT, 6, true, true}, {34, VK_FORMAT_R32G32B32_SFLOAT, 12, false, false},
        {35, VK_FORMAT_R32G32B32_UINT, 12, true, false}, {36, VK_FORMAT_R32G32B32_SINT, 12, true, true},
        {37, VK_FORMAT_R8G8B8A8_UNORM, 4, false, false}, {38, VK_FORMAT_R8G8B8A8_SNORM, 4, false, false},
        {39, VK_FORMAT_R8G8B8A8_UINT, 4, true, false}, {40, VK_FORMAT_R8G8B8A8_SINT, 4, true, true},
        {41, VK_FORMAT_R16G16B16A16_SFLOAT, 8, false, false}, {42, VK_FORMAT_R16G16B16A16_UNORM, 8, false, false},
        {43, VK_FORMAT_R16G16B16A16_SNORM, 8, false, false}, {44, VK_FORMAT_R16G16B16A16_UINT, 8, true, false},
        {45, VK_FORMAT_R16G16B16A16_SINT, 8, true, true}, {46, VK_FORMAT_R32G32B32A32_SFLOAT, 16, false, false},
        {47, VK_FORMAT_R32G32B32A32_UINT, 16, true, false}, {48, VK_FORMAT_R32G32B32A32_SINT, 16, true, true},
        {61, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4, false, false},
        {62, VK_FORMAT_A2B10G10R10_UINT_PACK32, 4, true, false},
        {63, VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4, false, false},
    };
    for (const E& e : kTable)
        if (e.nvn == f) return {e.vk, e.bytes, e.i, e.s};
    return {};
}

// ---- NVN enum -> Vulkan (NVN uses GL-style orderings; confirmed from the state logs) --------------------------
// BC (block-compressed) NVN formats and what they are decoded to on GPUs without BC support.
BcKind BcKindOf(int f) {
    switch (f) {
        case 65: case 66: case 69: case 70: return BcKind::BC1;
        case 67: case 71: return BcKind::BC2;
        case 68: case 72: return BcKind::BC3;
        case 73: return BcKind::BC4U;
        case 74: return BcKind::BC4S;
        case 75: return BcKind::BC5U;
        case 76: return BcKind::BC5S;
        case 77: case 78: return BcKind::BC7;
        case 79: return BcKind::BC6H_S;
        case 80: return BcKind::BC6H_U;
        default: return BcKind::None;
    }
}
VkFormat BcDecodedFormat(int f) {
    switch (f) {
        case 65: case 66: case 67: case 68: case 77: return VK_FORMAT_R8G8B8A8_UNORM;
        case 69: case 70: case 71: case 72: case 78: return VK_FORMAT_R8G8B8A8_SRGB;
        case 73: return VK_FORMAT_R8_UNORM;
        case 74: return VK_FORMAT_R8_SNORM;
        case 75: return VK_FORMAT_R8G8_UNORM;
        case 76: return VK_FORMAT_R8G8_SNORM;
        case 79: case 80: return VK_FORMAT_R16G16B16A16_SFLOAT;
        default: return VK_FORMAT_UNDEFINED;
    }
}

// What a BC texture becomes on a GPU without BC support: decoded (RGBA8, R8, RG8, RGBA16F) or re-encoded as ETC2.
enum class BcTo : uint8_t { Decoded, Etc2, Etc2Rgba8 };
VkFormat ConvertedFormat(int f, BcTo to) {
    const bool srgb = f == 69 || f == 70 || f == 71 || f == 72 || f == 78;
    if (to == BcTo::Etc2 && IsEtcEncodable(f)) {
        if (EtcHasPunchThrough(f)) return srgb ? VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK : VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK;
        return srgb ? VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK : VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
    }
    if (to == BcTo::Etc2Rgba8 && IsEtcRgba8Encodable(f))
        return srgb ? VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK : VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK;
    return BcDecodedFormat(f);
}

VkCompareOp CompareOp(int f) {  // NEVER=1 .. ALWAYS=8
    static const VkCompareOp k[] = {VK_COMPARE_OP_NEVER, VK_COMPARE_OP_NEVER, VK_COMPARE_OP_LESS,
                                    VK_COMPARE_OP_EQUAL, VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_GREATER,
                                    VK_COMPARE_OP_NOT_EQUAL, VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_ALWAYS};
    return (f >= 1 && f <= 8) ? k[f] : VK_COMPARE_OP_ALWAYS;
}
// The same comparison with its operands exchanged (a < b == b > a).
VkCompareOp SwapCompareOperands(VkCompareOp op) {
    switch (op) {
        case VK_COMPARE_OP_LESS: return VK_COMPARE_OP_GREATER;
        case VK_COMPARE_OP_GREATER: return VK_COMPARE_OP_LESS;
        case VK_COMPARE_OP_LESS_OR_EQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
        case VK_COMPARE_OP_GREATER_OR_EQUAL: return VK_COMPARE_OP_LESS_OR_EQUAL;
        default: return op;  // never, equal, not equal, always
    }
}
VkBlendFactor BlendFactor(int f) {
    switch (f) {
        case 1: return VK_BLEND_FACTOR_ZERO;
        case 2: return VK_BLEND_FACTOR_ONE;
        case 3: return VK_BLEND_FACTOR_SRC_COLOR;
        case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
        case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 7: return VK_BLEND_FACTOR_DST_ALPHA;
        case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 9: return VK_BLEND_FACTOR_DST_COLOR;
        case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case 16: return VK_BLEND_FACTOR_SRC1_COLOR;
        case 17: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
        case 18: return VK_BLEND_FACTOR_SRC1_ALPHA;
        case 19: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
        case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        default: return VK_BLEND_FACTOR_ONE;
    }
}
VkBlendOp BlendOp(int e) {
    switch (e) {
        case 2: return VK_BLEND_OP_SUBTRACT;
        case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case 4: return VK_BLEND_OP_MIN;
        case 5: return VK_BLEND_OP_MAX;
        default: return VK_BLEND_OP_ADD;
    }
}
VkStencilOp StencilOp(int o) {
    switch (o) {
        case 2: return VK_STENCIL_OP_ZERO;
        case 3: return VK_STENCIL_OP_REPLACE;
        case 4: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        case 5: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        case 6: return VK_STENCIL_OP_INVERT;
        case 7: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
        case 8: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
        default: return VK_STENCIL_OP_KEEP;
    }
}
VkSamplerAddressMode AddressMode(int w) {
    switch (w) {
        case 1: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case 2: case 3: case 4: case 6: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}
VkComponentSwizzle Swizzle(uint8_t s) {
    switch (s) {
        case 0: return VK_COMPONENT_SWIZZLE_ZERO;
        case 1: return VK_COMPONENT_SWIZZLE_ONE;
        case 2: return VK_COMPONENT_SWIZZLE_R;
        case 3: return VK_COMPONENT_SWIZZLE_G;
        case 4: return VK_COMPONENT_SWIZZLE_B;
        case 5: return VK_COMPONENT_SWIZZLE_A;
        default: return VK_COMPONENT_SWIZZLE_IDENTITY;
    }
}

// Hash of a byte range, 8 bytes per step (an FNV-style multiply plus a shift that carries the high bits down):
// the byte-at-a-time FNV-1a it replaces was a few percent of the render thread on slower cores (texture change
// checks, cache keys). In-memory only: nothing hashed here is stored across runs.
inline uint64_t HashStep(uint64_t h, uint64_t w) {
    h = (h ^ w) * 1099511628211ull;
    return h ^ (h >> 32);
}
uint64_t Fnv(const void* p, size_t n, uint64_t h = 1469598103934665603ull) {
    const auto* b = static_cast<const uint8_t*>(p);
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, b + i, 8);
        h = HashStep(h, w);
    }
    if (i < n) {
        uint64_t w = 0;
        memcpy(&w, b + i, n - i);
        h = HashStep(h, w ^ (uint64_t(n - i) << 56));
    }
    return h;
}
// Incremental 64-bit hash for cache keys (no allocation).
struct Key64 {
    uint64_t h = 1469598103934665603ull;
    void Add(const void* p, size_t n) { h = Fnv(p, n, h); }
    template <typename T>
    void Add(const T& v) { Add(&v, sizeof(v)); }
};
struct VecHash {
    size_t operator()(const std::vector<uint32_t>& v) const { return size_t(Fnv(v.data(), v.size() * 4)); }
};

// ---- objects ------------------------------------------------------------------------------------------------
struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkFormat vk_format = VK_FORMAT_UNDEFINED;
    VkImageType type = VK_IMAGE_TYPE_2D;
    int nvn_format = 0, target = kTarget2D;
    int width = 0, height = 0, depth = 1, levels = 1, layers = 1;
    VkImageAspectFlags aspect = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkFormatFeatureFlags features = 0;
    bool written = false;  // has the GPU (or an upload) put anything in it
    bool uploaded = false;
    bool pending = false;  // converted on the background thread; its copy is recorded when the result is ready
    uint64_t serial = 0;   // identifies one upload (a result for a replaced image is dropped)
    // Contents came from game memory (not rendered by the GPU): re-uploaded when the game rewrites the
    // memory. Checked via a sparse hash: after an unchanged check the next one comes twice as many command
    // lists later (up to kMaxCheckInterval); a change, or an explicit MarkTextureDirty, resets that.
    bool cpu_source = false;
    uint64_t sample_hash = 0;
    uint64_t checked_in_list = 0;
    uint64_t next_check_list = 0;
    uint32_t check_interval = 1;
    std::map<std::vector<uint32_t>, VkImageView> views;
    std::unordered_map<uint64_t, VkImageView> fast_views;  // same views by hashed key
    uint64_t addr = 0;     // storage in game memory
    TextureState tex{};    // as the game described it (layout of that storage)
    VkFormat twin = VK_FORMAT_UNDEFINED;  // sRGB/linear counterpart views may use (image is mutable-format)
    // A clear not done yet: done as the load of the next render pass that starts on this subresource (no
    // separate full-image write and read on a tiling GPU), or for real before any other use.
    bool clear_pending = false;
    // Was an attachment of a render pass with no barrier on it since: a later pass that uses it again in the same
    // layout needs a barrier first (see BeginRendering).
    bool attach_unsynced = false;
    // PassEndBarrier made this render target readable by fragment shaders only (mali_barriers); a vertex shader
    // that samples it needs one more barrier first (VertexReadBarrier).
    bool frag_only_sync = false;
    // texture_mip_skip: the game's level 0 was left out (this image holds its levels 1..n as 0..n-1, at half size).
    int mip_skip = 0;
    // Depth written by a fragment shader (gl_FragDepth) since the image was last attached: see LrzTouch.
    bool depth_exported = false;
    // Ever had depth written by a fragment shader (gl_FragDepth): depth tests against it are done in the shader.
    bool fragdepth_written = false;
    BcKind bc = BcKind::None;  // block-compressed texture converted on upload (GPU without BC support)
    BcTo bc_to = BcTo::Decoded;  // ... into what
    bool storage = false;      // created with VK_IMAGE_USAGE_STORAGE_BIT
    bool mutable_fmt = false;  // created with VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT (views in `twin` allowed)
    uint64_t attach_gen = 0;  // times attached to a render pass (`hole_test` 16 copies)
    VkClearValue clear_value{};
    VkImageAspectFlags clear_aspect = 0;
    int clear_layer = 0, clear_level = 0;
};

struct Shader {
    bool ok = false;
    VkShaderModule module = VK_NULL_HANDLE;
    shader::ShaderInfo info;
    std::string error;
};

// Per-stage resources for one draw.
struct DrawState {
    std::shared_ptr<const ShaderCode> program[2];  // vertex, fragment
    std::vector<VertexAttribObj> attribs;
    std::vector<VertexStreamObj> streams;
    BufferRange vertex_buffers[16];
    BufferRange uniforms[2][18];
    BoundTexture textures[2][32];
    TexRef images[2][8];  // storage images (nvnCommandBufferBindImage)
    BlendStateObj blend[8];
    ColorStateObj color;
    ChannelMaskStateObj channel;
    DepthStencilStateObj depth;
    PolygonStateObj polygon;
    MultisampleStateObj multisample;
    float viewport[4] = {0, 0, 0, 0};
    bool viewport_set = false;
    float scissor[4] = {0, 0, 0, 0};
    bool scissor_set = false;
    float depth_range[2] = {0.0f, 1.0f};
    float poly_offset[3] = {0.0f, 0.0f, 0.0f};  // factor, units, clamp
    uint64_t sampler_pool = 0;                   // NVNsamplerPool bound (SetSamplerPool)
    float alpha_ref = 0.0f;
    float blend_color[4] = {0, 0, 0, 0};
    uint32_t stencil_ref[2] = {0, 0}, stencil_write[2] = {0xFF, 0xFF}, stencil_compare[2] = {0xFF, 0xFF};
    std::vector<TexRef> rts;  // colors then depth at [num_rt]
    int num_rt = 0;
};

class VulkanBackend final : public Backend {
public:
    ~VulkanBackend() { StopBcThread(); }
private:
public:
    const char* Name() const override { return "vulkan"; }

    struct MsTimer {
        double& total;
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        explicit MsTimer(double& t) : total(t) {}
        ~MsTimer() { total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); }
    };

    // The commands of a list, in order; called lists (CallList) are executed in place.
    void ExecuteCommands(const CmdList& cmds, int depth) {
        for (const Cmd& c : cmds) {
            switch (c.type) {
                case CmdType::CallList:
                    if (c.obj && depth < 8) ExecuteCommands(*static_cast<const CmdList*>(c.obj.get()), depth + 1);
                    break;
                case CmdType::SetRenderTargets:
                    if (capture_active_) CaptureEndOfPass();
                    st_.num_rt = c.num_rt;
                    st_.rts = c.rts ? *c.rts : std::vector<TexRef>{};
                    LogTargets(st_.rts, st_.num_rt);
                    break;
                case CmdType::ClearColor:
                    if (c.mask == 0 || c.index < 0 || c.index >= st_.num_rt) break;
                    if (Image* img = GetImage(st_.rts[c.index], false)) {
                        Begin();
                        const int layer = st_.rts[c.index].layer, level = st_.rts[c.index].level;
                        if (!IsDepthFormat(img->nvn_format) && !capture_active_) {
                            VkClearValue v{};
                            memcpy(v.color.uint32, c.color, sizeof(v.color.uint32));
                            if (rendering_ && size_t(c.index) < pass_color_count_ && pass_colors_[c.index] == img &&
                                pass_color_sub_[c.index] == std::make_pair(layer, level)) {
                                // Target of the open pass: cleared inside it.
                                VkClearAttachment ca{VK_IMAGE_ASPECT_COLOR_BIT, uint32_t(c.index), v};
                                const VkClearRect rect{{{0, 0}, pass_extent_}, 0, 1};
                                Rec([ca, rect](VkCommandBuffer cb) { vkCmdClearAttachments(cb, 1, &ca, 1, &rect); });
                                clears_in_pass_++;
                            } else {
                                if (InOpenPass(img)) EndRendering();  // draws after the clear must see it
                                if (img->clear_pending) FlushClear(*img);
                                img->clear_pending = true;
                                img->clear_value = v;
                                img->clear_aspect = VK_IMAGE_ASPECT_COLOR_BIT;
                                img->clear_layer = layer;
                                img->clear_level = level;
                                clears_deferred_++;
                            }
                            img->written = true;
                            img->cpu_source = false;
                        } else {
                            EndRendering();
                            ClearColorImage(*img, c.color, c.kind, layer, level);
                        }
                    }
                    break;
                case CmdType::ClearTexture:
                    if (c.mask == 0) break;
                    if (Image* img = c.copy ? GetImage(c.copy->dst, false) : nullptr) {
                        Begin();
                        EndRendering();
                        ClearColorImage(*img, c.color, c.kind);
                    }
                    break;
                case CmdType::ClearDepthStencil: ClearDepth(c); break;
                case CmdType::CopyTexture: {
                    if (!c.copy) break;
                    Image* src = GetImage(c.copy->src, true);  // a CPU-written source (video frames) is refreshed
                    Image* dst = GetImage(c.copy->dst, false);
                    if (!src || !dst || src == dst) break;
                    Begin();
                    EndRendering();
                    CopyImage(*src, c.copy->src_region, *dst, c.copy->dst_region, c.flags);
                    break;
                }
                case CmdType::CopyBufferToTexture: CopyBufferToTexture(c); break;
                case CmdType::CopyTextureToBuffer: CopyTextureToBuffer(c); break;
                case CmdType::Report: {
                    Begin();
                    Readback rb{0, c.dst_addr, 0, 0, 1, 0, 0};
                    rb.report = c.index;
                    rb.value = uint32_t(c.mask);
                    slots_[slot_].readbacks.push_back(rb);
                    break;
                }
                case CmdType::CopyBufferToBuffer:
                    if (c.size64 && GuestRangeValid(c.src_addr, c.size64) && GuestRangeValid(c.dst_addr, c.size64)) {
                        memmove(reinterpret_cast<void*>(c.dst_addr), reinterpret_cast<const void*>(c.src_addr), c.size64);
                        MemoryWrittenInList(c.dst_addr, c.size64);
                        list_b2b_copies_++;
                    } else
                        LogOnce("b2b-range", "buffer-to-buffer copy outside known memory");
                    break;
                case CmdType::UpdateUniform: {
                    const std::span<const uint8_t> data = Items<uint8_t>(c);
                    if (!data.empty() && GuestRangeValid(c.dst_addr, data.size())) {
                        memcpy(reinterpret_cast<void*>(c.dst_addr), data.data(), data.size());
                        MemoryWrittenInList(c.dst_addr, data.size());  // cached copies may include the old bytes
                        list_updates_++;
                    }
                    break;
                }
                case CmdType::BindProgram: {
                    const auto* code = static_cast<const ShaderCode*>(c.obj.get());
                    if (program_binds_logged_.size() < 12 &&
                        program_binds_logged_.insert(c.index * 16 + (code ? code->stage + 1 : 0)).second)
                        LOGI(kTag, "program bind: stage bits 0x%x, program stage %d", c.index, code ? code->stage : -1);
                    for (int s = 0; s < 2; s++) {
                        if (!(c.index & (1 << s))) continue;
                        const ShaderCode* want = (code && code->stage == s) ? code : nullptr;
                        if (st_.program[s].get() == want) continue;  // no shared_ptr copy (atomic refcounts)
                        if (want) st_.program[s] = std::static_pointer_cast<const ShaderCode>(c.obj);
                        else st_.program[s] = nullptr;
                    }
                    break;
                }
                case CmdType::BindVertexAttribs:
                    {
                        const auto v = Items<VertexAttribObj>(c);
                        st_.attribs.assign(v.begin(), v.end());
                    }
                    break;
                case CmdType::BindVertexStreams:
                    {
                        const auto v = Items<VertexStreamObj>(c);
                        st_.streams.assign(v.begin(), v.end());
                    }
                    break;
                case CmdType::BindVertexBuffers: {
                    const auto v = Items<BufferRange>(c);
                    for (size_t i = 0; i < v.size(); i++)
                        if (c.index + i < 16) st_.vertex_buffers[c.index + i] = v[i];
                    break;
                }
                case CmdType::BindUniformBuffers: {
                    if (c.stage < 0 || c.stage > 1) break;  // only vertex / fragment
                    const auto v = Items<BufferRange>(c);
                    for (size_t i = 0; i < v.size(); i++) {
                        const size_t idx = size_t(c.index) + i;
                        if (idx < 18) st_.uniforms[c.stage][idx] = v[i];
                        LogUniformBind(c.stage, int(idx), v[i]);
                    }
                    break;
                }
                case CmdType::BindTextures: {
                    if (c.stage < 0 || c.stage > 1) break;
                    const auto v = Items<BoundTexture>(c);
                    for (size_t i = 0; i < v.size(); i++)
                        if (c.index + i < 32) st_.textures[c.stage][c.index + i] = v[i];
                    break;
                }
                case CmdType::BindImages: {
                    if (c.stage < 0 || c.stage > 1) break;
                    const auto v = Items<TexRef>(c);
                    for (size_t i = 0; i < v.size(); i++) {
                        if (c.index + i >= 8) break;
                        st_.images[c.stage][c.index + i] = v[i];
                        if (image_binds_logged_ < 6 && v[i].valid) {
                            image_binds_logged_++;
                            LOGI(kTag, "image bound: stage %d slot %zu, %dx%dx%d format %d", c.stage, c.index + i,
                                 v[i].tex.width, v[i].tex.height, v[i].tex.depth, v[i].tex.format);
                        }
                    }
                    break;
                }
                case CmdType::BindBlend: {
                    const BlendStateObj b = One<BlendStateObj>(c);
                    if (b.target >= 0 && b.target < 8) st_.blend[b.target] = b;
                    break;
                }
                case CmdType::BindColor: st_.color = One<ColorStateObj>(c); break;
                case CmdType::BindChannelMask:
                    st_.channel = One<ChannelMaskStateObj>(c);
                    break;
                case CmdType::BindDepthStencil:
                    st_.depth = One<DepthStencilStateObj>(c);
                    break;
                case CmdType::BindPolygon: st_.polygon = One<PolygonStateObj>(c); break;
                case CmdType::BindMultisample:
                    st_.multisample = One<MultisampleStateObj>(c);
                    break;
                case CmdType::SetViewport:
                    memcpy(st_.viewport, c.f, sizeof(st_.viewport));
                    st_.viewport_set = true;
                    break;
                case CmdType::SetScissor:
                    memcpy(st_.scissor, c.f, sizeof(st_.scissor));
                    st_.scissor_set = true;
                    break;
                case CmdType::SetDepthRange:
                    st_.depth_range[0] = c.f[0];
                    st_.depth_range[1] = c.f[1];
                    break;
                case CmdType::SetSamplerPool:
                    if (st_.sampler_pool != c.src_addr) handle_memo_list_ = ~0ull;  // memo: other pool's samplers
                    st_.sampler_pool = c.src_addr;
                    break;
                case CmdType::SetPolygonOffset:
                    st_.poly_offset[0] = c.f[0];
                    st_.poly_offset[1] = c.f[1];
                    st_.poly_offset[2] = c.f[2];
                    break;
                case CmdType::SetAlphaRef: st_.alpha_ref = c.f[0]; break;
                case CmdType::SetBlendColor: memcpy(st_.blend_color, c.f, sizeof(st_.blend_color)); break;
                case CmdType::SetStencil:
                    for (int face = 0; face < 2; face++) {
                        if (!(c.stage & (1 << face))) continue;
                        if (c.index == 0) st_.stencil_ref[face] = uint32_t(c.mask);
                        if (c.index == 1) st_.stencil_write[face] = uint32_t(c.mask);
                        if (c.index == 2) st_.stencil_compare[face] = uint32_t(c.mask);
                    }
                    break;
                case CmdType::Draw: Draw(c.draw); break;
                case CmdType::FenceSync: break;  // signalled by the queue once the list has run
            }
        }
    }

    // What a slow list spent its time on (stutters): counters at the start of the list.
    struct ListMarks {
        std::chrono::steady_clock::time_point t;
        double drain, wait, compile;
        uint64_t draws, geo_bytes, upload_bytes, pipelines, reuploads, uploads, flushes, waits, barriers, passes;
    };
    ListMarks Marks() const {
        return {std::chrono::steady_clock::now(), drain_ms_, wait_ms_, compile_ms_, drawn_, geo_bytes_, upload_bytes_,
                uint64_t(new_pipelines_), reuploads_, uint64_t(uploads_), uint64_t(flushes_), pipeline_waits_,
                barriers_, render_passes_};
    }
    int slow_lists_logged_ = 0;
    std::chrono::steady_clock::time_point slow_window_;
    void CheckSlowList(const ListMarks& a) {
        const ListMarks b = Marks();
        const double ms = std::chrono::duration<double, std::milli>(b.t - a.t).count();
        if (ms < 25) return;
        if (b.t - slow_window_ > std::chrono::seconds(10)) {
            slow_window_ = b.t;
            slow_lists_logged_ = 0;
        }
        if (slow_lists_logged_++ >= 8) return;
        LOGI(kTag, "slow list: %.1f ms, %llu draws, %llu passes, %llu barriers | waited %.1f ms for the recorder, %.1f ms "
             "for the GPU | %llu pipelines new (%.1f ms compiling, %llu draws waited) | %llu texture uploads (%llu "
             "re-uploads), %.2f MB geometry copied, %.2f MB streamed, %llu flushes",
             ms, (unsigned long long)(b.draws - a.draws), (unsigned long long)(b.passes - a.passes),
             (unsigned long long)(b.barriers - a.barriers), b.drain - a.drain, b.wait - a.wait,
             (unsigned long long)(b.pipelines - a.pipelines), b.compile - a.compile, (unsigned long long)(b.waits - a.waits),
             (unsigned long long)(b.uploads - a.uploads), (unsigned long long)(b.reuploads - a.reuploads),
             (b.geo_bytes - a.geo_bytes) / 1048576.0, (b.upload_bytes - a.upload_bytes) / 1048576.0,
             (unsigned long long)(b.flushes - a.flushes));
    }

    void Execute(const CmdList& cmds) override {
        std::lock_guard<std::mutex> lk(m_);
        MsTimer timer(exec_ms_);
        const ListMarks marks = Marks();
        if (!EnsureDevice()) return;
        StartRecorder();
        ResetTransient();
        list_counter_++;
        // Each submitted list starts with no sampler pool until it binds one (its recording re-binds the pool its
        // command buffer had bound): sampler ids then resolve as before pools were tracked. Lists it calls
        // inherit its pool.
        st_.sampler_pool = 0;
        MaybeArmCapture();
        ApplyDirtyMarks();
        if (!watched_copies_.empty()) CheckWatchedCopies();
        PollReadbacks();
        geo_token_list_ = ++geo_token_;
        geo_lo_ = UINTPTR_MAX;
        geo_hi_ = 0;
        StartPrehash();
        ExecuteCommands(cmds, 0);
        if (capture_active_) CaptureEndOfPass();
        if (recording_) WriteBackSmallTargets();
        if (recording_) {
            EndRendering();
            SubmitAndWait(VK_NULL_HANDLE, VK_NULL_HANDLE);
            recording_ = false;
        }
        if (persist_full_) {  // the recorder found the kept descriptor sets full: empty them (GPU idle)
            WaitGpuIdle();
            ResetPersistentSets();
            persist_full_ = false;
        }
        if (capture_active_) FinishCapture();
        CheckSlowList(marks);
        last_list_draws_ = drawn_ - list_start_draws_;
        list_start_draws_ = drawn_;
    }

    // ======================================================================================================
    // frame capture (diagnostics): every image a frame renders, downscaled to RGBA8, saved to one file
    // ======================================================================================================
    struct CaptureItem {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory image_memory = VK_NULL_HANDLE;
        uint32_t w = 0, h = 0, src_w = 0, src_h = 0, nvn_format = 0, draws = 0, pass = 0, flags = 0, raw_bpp = 4;
        uint64_t fs_hash = 0;
    };
    // Video frames: the newest picture per texture storage address, uploaded at that texture's next use.
    struct PendingFrame {
        std::shared_ptr<const std::vector<uint8_t>> rgba;
        int w = 0, h = 0;
    };
    std::mutex frames_mutex_;
    std::unordered_map<uint64_t, PendingFrame> pending_frames_;
    std::atomic<int> pending_count_{0};
    uint64_t frames_set_ = 0, frames_uploaded_ = 0;  // statistics (frames_set_ under frames_mutex_)

    // Textures the CPU wrote through NVN (nvnTextureWriteTexels): checked at their next use. Thread-safe.
    // Addresses queued by any thread; the render thread applies them at the start of each list (the images
    // there are checked at their next use; addresses with no image yet need nothing: they upload when used).
    std::mutex dirty_mutex_;
    std::vector<uint64_t> dirty_queue_;
    std::atomic<bool> dirty_pending_{false};
    static constexpr uint32_t kMaxCheckInterval = 16;
    void MarkTextureDirty(uint64_t addr) override {
        std::lock_guard<std::mutex> lk(dirty_mutex_);
        dirty_queue_.push_back(addr);
        dirty_pending_.store(true, std::memory_order_release);
    }
    void ApplyDirtyMarks() {
        if (!dirty_pending_.load(std::memory_order_acquire)) return;
        std::vector<uint64_t> q;
        {
            std::lock_guard<std::mutex> lk(dirty_mutex_);
            q.swap(dirty_queue_);
            dirty_pending_.store(false, std::memory_order_relaxed);
        }
        for (uint64_t a : q) {
            auto it = images_.find(a);
            if (it == images_.end()) continue;
            it->second.next_check_list = 0;
            it->second.check_interval = 1;
            it->second.checked_in_list = 0;
        }
    }

    void SetTextureFrame(uint64_t addr, std::shared_ptr<const std::vector<uint8_t>> rgba, int w, int h) override {
        std::lock_guard<std::mutex> lk(frames_mutex_);
        PendingFrame& f = pending_frames_[addr];
        if (!f.rgba) pending_count_++;
        f = {std::move(rgba), w, h};
        frames_set_++;
    }
    // Uploads the pending frame for `img` (render thread). True if there was one.
    bool TakeFrame(Image& img, const TexRef& t) {
        if (pending_count_.load(std::memory_order_relaxed) == 0) return false;
        PendingFrame f;
        {
            std::lock_guard<std::mutex> lk(frames_mutex_);
            auto it = pending_frames_.find(t.addr);
            if (it == pending_frames_.end() || !it->second.rgba) return false;
            f = std::move(it->second);
            pending_frames_.erase(it);
            pending_count_--;
        }
        const int w = std::min(f.w, img.width), h = std::min(f.h, img.height);
        const size_t row = size_t(f.w) * 4, n = row * size_t(h);
        Begin();
        EndRendering();
        if (StreamRemaining() < n + 1024) Flush();
        const VkDeviceSize off = StreamAlloc(n, 16);
        if (off == VK_WHOLE_SIZE) return false;
        memcpy(stream_ptr_ + off, f.rgba->data(), n);
        VkBufferImageCopy r{};
        r.bufferOffset = off;
        r.bufferRowLength = uint32_t(f.w);
        r.bufferImageHeight = uint32_t(h);
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.imageExtent = {uint32_t(w), uint32_t(h), 1};
        Transition(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        Rec([r, buf = stream_, image = img.image](VkCommandBuffer cb) {
            vkCmdCopyBufferToImage(cb, buf, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        });
        // The texture's game memory is not updated: keep the change detector from re-uploading it.
        img.uploaded = true;
        img.written = true;
        img.cpu_source = true;
        img.sample_hash = SampleHash(t);
        img.checked_in_list = list_counter_;
        frames_uploaded_++;
        {
            const uint64_t shown = presented_ - last_video_present_;  // presents since the previous video frame
            last_video_present_ = presented_;
            video_cadence_[std::min<uint64_t>(shown, 4)]++;
        }
        return true;
    }

    void RequestCapture(int delay_s) override {
        capture_request_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch()).count() + int64_t(delay_s) * 1000;
    }
    void MaybeArmCapture() {
        if (capture_active_) return;
        const auto now = std::chrono::steady_clock::now();
        const int64_t req = capture_request_ms_.load();
        if (req && std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() >= req) {
            capture_request_ms_ = 0;
            capture_active_ = true;
            world_draws_logged_ = 0;
            capture_pass_ = 0;
            pass_written_.clear();
            pass_draws_ = 0;
            query_pass_.clear();
            capture_draws_logged_ = 0;
            geo_dumps_ = 0;
            map_geo_dumps_ = 0;
            if (!query_pool_) {  // requested captures had no pool: no per-draw sample counts
                VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
                qi.queryType = VK_QUERY_TYPE_OCCLUSION;
                qi.queryCount = kMaxQueries;
                vkCreateQueryPool(device_, &qi, nullptr, &query_pool_);
            }
            if (query_pool_) {
                Begin();
                EndRendering();
                vkCmdResetQueryPool(SyncCmd(), query_pool_, 0, kMaxQueries);
            }
            StartPassTimestamps();
            LOGI(kTag, "frame capture (requested): recording this frame's passes");
            return;
        }
        if (captures_done_ >= 6 || !dsr::GetRuntime().config.auto_capture) return;
        if (!capture_clock_started_) {
            if (last_list_draws_ < 100) return;  // wait for real 3D frames
            capture_clock_started_ = true;
            next_capture_ = now + std::chrono::seconds(15);
            return;
        }
        if (now < next_capture_) return;
        next_capture_ = now + std::chrono::seconds(45);
        capture_active_ = true;
        world_draws_logged_ = 0;
        capture_pass_ = 0;
        if (!query_pool_) {
            VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qi.queryType = VK_QUERY_TYPE_OCCLUSION;
            qi.queryCount = kMaxQueries;
            vkCreateQueryPool(device_, &qi, nullptr, &query_pool_);
        }
        if (query_pool_) {
            Begin();
            EndRendering();
            vkCmdResetQueryPool(SyncCmd(), query_pool_, 0, kMaxQueries);
        }
        query_pass_.clear();
        StartPassTimestamps();
        pass_written_.clear();
        pass_draws_ = 0;
        LOGI(kTag, "frame capture %d: recording this frame's passes", captures_done_ + 1);
    }
    void NoteWritten(Image* img) {
        if (!capture_active_ || !img) return;
        if (std::find(pass_written_.begin(), pass_written_.end(), img) == pass_written_.end()) pass_written_.push_back(img);
    }
    // GPU timestamps at pass boundaries during a capture: where the GPU's time goes.
    void StartPassTimestamps() {
        ts_count_ = 0;
        if (!ts_pool_) {
            VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = kMaxTimestamps;
            if (vkCreateQueryPool(device_, &qi, nullptr, &ts_pool_) != VK_SUCCESS) ts_pool_ = VK_NULL_HANDLE;
        }
        if (!ts_pool_) return;
        Begin();
        EndRendering();
        vkCmdResetQueryPool(SyncCmd(), ts_pool_, 0, kMaxTimestamps);
        WriteTimestamp();
    }
    void WriteTimestamp() {
        if (!ts_pool_ || ts_count_ >= kMaxTimestamps) return;
        Begin();
        vkCmdWriteTimestamp(SyncCmd(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ts_pool_, ts_count_);
        ts_pass_[ts_count_] = capture_pass_;
        ts_count_++;
    }
    void CaptureEndOfPass() {
        WriteTimestamp();
        if (!pass_written_.empty())
            LOGI(kTag, "capture pass %u: %u draws, last fs %016llx (code @%llx, vs @%llx), last textures:%s", capture_pass_,
                 pass_draws_, (unsigned long long)pass_fs_hash_, (unsigned long long)pass_fs_addr_,
                 (unsigned long long)pass_vs_addr_, pass_tex_desc_.empty() ? " none" : pass_tex_desc_.c_str());
        if (!pass_written_.empty() && !pass_state_desc_.empty())
            LOGI(kTag, "capture pass %u state: %s", capture_pass_, pass_state_desc_.c_str());
        pass_state_desc_.clear();
        if (!pass_written_.empty() && !pass_ubo_desc_.empty())
            for (size_t at = 0; at < pass_ubo_desc_.size(); at += 2800)
                LOGI(kTag, "capture pass %u constants%s:%s", capture_pass_, at ? " (cont.)" : "",
                     pass_ubo_desc_.substr(at, 2800).c_str());
        pass_ubo_desc_.clear();
        for (Image* img : pass_written_) CaptureImage(*img);
        pass_written_.clear();
        pass_draws_ = 0;
        capture_pass_++;
    }
    void CaptureImage(Image& img) {
        if (capture_items_.size() >= 120) return;
        CaptureItem it;
        it.src_w = uint32_t(img.width);
        it.src_h = uint32_t(img.height);
        it.nvn_format = uint32_t(img.nvn_format);
        it.draws = pass_draws_;
        it.pass = capture_pass_;
        it.fs_hash = pass_fs_hash_;
        const bool depth = IsDepthFormat(img.nvn_format);
        // Raw copies (full size, layer 0, mip 0): depth (as the driver stores it, float on Adreno) and float
        // colour formats, whose values an 8-bit blit would clamp (negatives, HDR).
        uint32_t raw_bpp = 0;
        switch (img.vk_format) {
            case VK_FORMAT_R16_SFLOAT: raw_bpp = 2; break;
            case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_B10G11R11_UFLOAT_PACK32: raw_bpp = 4; break;
            case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R32G32_SFLOAT: raw_bpp = 8; break;
            case VK_FORMAT_R32G32B32A32_SFLOAT: raw_bpp = 16; break;
            case VK_FORMAT_R8G8B8A8_UNORM:  // small ones (light probes, cube maps): raw, all layers
                if (it.src_w * it.src_h <= 64u * 64u) raw_bpp = 4;
                break;
            default: break;
        }
        if (depth) raw_bpp = HasDepth(img.nvn_format) ? 4 : 0;
        // Depth up to 2048x2048 (shadow maps, 16 MB raw), the rest up to the screen size.
        const uint32_t max_px = depth ? 2048u * 2048u : 1280u * 720u;
        if (raw_bpp && img.type == VK_IMAGE_TYPE_2D && it.src_w * it.src_h <= max_px) {
            // Array images: up to 4 layers, stacked vertically (flag 16).
            const uint32_t layers = uint32_t(std::min(6, std::max(1, img.layers)));
            it.w = it.src_w;
            it.h = it.src_h * layers;
            it.flags = (depth ? 4u : 8u) | (layers > 1 ? 16u : 0u) | (uint32_t(img.vk_format) << 8);
            it.raw_bpp = raw_bpp;
            VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bci.size = VkDeviceSize(it.w) * it.h * raw_bpp;
            bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            if (vkCreateBuffer(device_, &bci, nullptr, &it.buffer) != VK_SUCCESS) return;
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(device_, it.buffer, &req);
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            ai.allocationSize = req.size;
            ai.memoryTypeIndex = FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            vkAllocateMemory(device_, &ai, nullptr, &it.memory);
            vkBindBufferMemory(device_, it.buffer, it.memory, 0);
            Begin();
            EndRendering();
            const VkImageLayout restore = img.layout;
            Transition(img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkBufferImageCopy c{};
            c.imageSubresource = {depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT), 0, 0, layers};
            c.imageExtent = {it.w, it.src_h, 1};
            vkCmdCopyImageToBuffer(SyncCmd(), img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, it.buffer, 1, &c);
            if (restore != VK_IMAGE_LAYOUT_UNDEFINED) Transition(img, restore);
            capture_items_.push_back(it);
            return;
        }
        if (depth || !(img.features & VK_FORMAT_FEATURE_BLIT_SRC_BIT) || img.type == VK_IMAGE_TYPE_3D) {
            it.flags = 1;  // not captured (depth or unblittable): metadata only
            capture_items_.push_back(it);
            return;
        }
        uint32_t w = it.src_w, h = it.src_h;
        while (w > 480) {
            w = (w + 1) / 2;
            h = (h + 1) / 2;
        }
        w = std::max(1u, w);
        h = std::max(1u, h);
        it.w = w;
        it.h = h;
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = VK_FORMAT_R8G8B8A8_UNORM;
        ci.extent = {w, h, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (vkCreateImage(device_, &ci, nullptr, &it.image) != VK_SUCCESS) return;
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device_, it.image, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(device_, &ai, nullptr, &it.image_memory) != VK_SUCCESS) {
            vkDestroyImage(device_, it.image, nullptr);
            return;
        }
        vkBindImageMemory(device_, it.image, it.image_memory, 0);
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = VkDeviceSize(w) * h * 4;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        vkCreateBuffer(device_, &bci, nullptr, &it.buffer);
        vkGetBufferMemoryRequirements(device_, it.buffer, &req);
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(device_, &ai, nullptr, &it.memory);
        vkBindBufferMemory(device_, it.buffer, it.memory, 0);

        Begin();
        EndRendering();
        const VkImageLayout restore = img.layout;
        Transition(img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        RawBarrier(it.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageBlit b{};
        b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        b.srcOffsets[1] = {int32_t(it.src_w), int32_t(img.type == VK_IMAGE_TYPE_1D ? 1 : it.src_h), 1};
        b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        b.dstOffsets[1] = {int32_t(w), int32_t(h), 1};
        const bool linear = img.features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        vkCmdBlitImage(SyncCmd(), img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, it.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &b, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
        RawBarrier(it.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy c{};
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {w, h, 1};
        vkCmdCopyImageToBuffer(SyncCmd(), it.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, it.buffer, 1, &c);
        if (restore != VK_IMAGE_LAYOUT_UNDEFINED) Transition(img, restore);
        capture_items_.push_back(it);
    }
    void FinishCapture() {
        capture_draws_logged_ = 0;
        geo_dumps_ = 0;
        map_geo_dumps_ = 0;
        WaitGpuIdle();  // the readback buffers are written by the submission that just went out
        capture_active_ = false;
        if (ts_pool_ && ts_count_ > 1) {
            std::vector<uint64_t> ts(ts_count_);
            if (vkGetQueryPoolResults(device_, ts_pool_, 0, ts_count_, ts.size() * 8, ts.data(), 8,
                                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
                const double ns = props_.limits.timestampPeriod;
                std::string line;
                double total = 0;
                for (uint32_t i = 1; i < ts_count_; i++) {
                    const double ms = double(ts[i] - ts[i - 1]) * ns / 1e6;
                    total += ms;
                    if (ms < 0.05) continue;
                    char d[48];
                    snprintf(d, sizeof(d), " p%u:%.2f", ts_pass_[i], ms);
                    line += d;
                }
                LOGI(kTag, "capture %d GPU ms per pass (total %.1f ms):%s", captures_done_ + 1, total, line.c_str());
            }
        }
        captures_done_++;
        if (query_pool_ && !query_pass_.empty()) {
            std::vector<uint64_t> res(query_pass_.size());
            if (vkGetQueryPoolResults(device_, query_pool_, 0, uint32_t(res.size()), res.size() * 8, res.data(), 8,
                                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
                std::map<uint32_t, std::pair<uint64_t, uint32_t>> per_pass;  // samples, draws
                for (size_t q = 0; q < res.size(); q++) {
                    per_pass[query_pass_[q]].first += res[q];
                    per_pass[query_pass_[q]].second++;
                }
                std::string line;
                for (auto& [pass, v] : per_pass) {
                    char d[64];
                    snprintf(d, sizeof(d), " p%u:%llu/%u", pass, (unsigned long long)v.first, v.second);
                    line += d;
                }
                LOGI(kTag, "capture %d samples passed per pass (samples/draws):%s", captures_done_, line.c_str());
                // Per draw, for passes of few draws (single objects): did it cover any pixels?
                std::string per_draw;
                for (size_t q = 0; q < res.size(); q++) {
                    if (per_pass[query_pass_[q]].second > 4) continue;
                    char d[48];
                    snprintf(d, sizeof(d), " q%zu(p%u):%llu", q, query_pass_[q], (unsigned long long)res[q]);
                    per_draw += d;
                }
                for (size_t at = 0; at < per_draw.size(); at += 2800)
                    LOGI(kTag, "capture %d samples per draw (passes of up to 4 draws)%s:%s", captures_done_,
                         at ? " (cont.)" : "", per_draw.substr(at, 2800).c_str());
            }
        }
        char name[64];
        time_t now = time(nullptr);
        tm t;
        localtime_r(&now, &t);
        strftime(name, sizeof(name), "dsr_frame_%Y%m%d_%H%M%S.bin", &t);
        const int fd = dsr::platform::CreateUserFile(name, "application/octet-stream");
        size_t bytes = 0;
        for (CaptureItem& it : capture_items_) {
            struct Header {
                char magic[4];
                uint32_t w, h, src_w, src_h, nvn_format, draws, pass, flags;
                uint64_t fs_hash;
            } hd{{'D', 'F', 'R', 'M'}, it.w, it.h, it.src_w, it.src_h, it.nvn_format, it.draws, it.pass, it.flags, it.fs_hash};
            void* px = nullptr;
            const size_t n = size_t(it.w) * it.h * it.raw_bpp;
            if (it.memory && n) vkMapMemory(device_, it.memory, 0, n, 0, &px);
            if (!px) {
                hd.w = hd.h = 0;
                hd.flags |= 2;
            }
            if (fd >= 0) {
                bytes += size_t(write(fd, &hd, sizeof(hd)));
                if (px) bytes += size_t(write(fd, px, n));
            }
            if (px) vkUnmapMemory(device_, it.memory);
            if (it.buffer) vkDestroyBuffer(device_, it.buffer, nullptr);
            if (it.memory) vkFreeMemory(device_, it.memory, nullptr);
            if (it.image) vkDestroyImage(device_, it.image, nullptr);
            if (it.image_memory) vkFreeMemory(device_, it.image_memory, nullptr);
        }
        if (fd >= 0) close(fd);
        LOGI(kTag, "frame capture %d: %zu images, %zu bytes -> Download/DSRNative/logs/%s", captures_done_,
             capture_items_.size(), bytes, fd >= 0 ? name : "(could not create file)");
        capture_items_.clear();
    }

    void Present(const TexRef& tex) override {
        std::lock_guard<std::mutex> lk(m_);
        if (!EnsureDevice()) return;
        ReportDraws();
        // The frame rate cap changed while the game runs (settings menu): tell the display.
        if (window_ && swapchain_ && window_fps_ && window_fps_ != std::clamp(dsr::GetRuntime().config.target_fps, 30, 60))
            SetWindowFrameRate();
        Image* src = tex.valid ? FindImage(tex) : nullptr;
        if (src && !(src->written && !IsDepthFormat(src->nvn_format))) src = nullptr;
        // The swapchain is sized for the game's frame (see EnsureSwapchain): a frame of another size remakes it.
        if (src && dsr::GetRuntime().config.compositor_scaling && (src->width != frame_w_ || src->height != frame_h_)) {
            frame_w_ = src->width;
            frame_h_ = src->height;
            if (swapchain_) swapchain_dirty_ = true;
        }
        if (!EnsureSwapchain()) return;
        // Acquire, copy and present run on the recorder after this frame's commands; the render thread goes on.
        BeginCommands();
        PumpBcUploads();
        PresentJob pj;
        if (src) {
            Transition(*src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            pj.src = src->image;
            pj.sw = src->width;
            pj.sh = src->height;
            if (!logged_first_image_) {
                logged_first_image_ = true;
                LOGI(kTag, "showing the game's frame: %dx%d (format %d)", pj.sw, pj.sh, src->nvn_format);
            }
        }
        barriers_ += 2;
        Slot& sl = slots_[slot_];
        sl.ts_written = sl.ts != VK_NULL_HANDLE;
        sl.pending = true;
        sl.submits_queued++;
        sl.seq = ++submit_seq_;
        Rec([this, i = slot_, pj](VkCommandBuffer) { PresentSlot(i, pj); });
        Publish();
        NextSlot();
        if (++presented_ == 1) LOGI(kTag, "first frame on screen");
        dsr::platform::CountFrameShown();
    }

private:
    struct PresentJob {
        VkImage src = VK_NULL_HANDLE;  // the game's frame (in TRANSFER_SRC layout), or none: black
        int sw = 0, sh = 0;
    };
    static void CmdImageBarrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &b);
    }
    // Recorder: acquires a swapchain image, copies the frame into it, submits slot i and presents.
    void PresentSlot(int i, const PresentJob& pj) {
        uint32_t idx = 0;
        VkResult r = vkAcquireNextImageKHR(device_, swapchain_, 1000000000ull, acquire_sem_, VK_NULL_HANDLE, &idx);
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
            if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_SURFACE_LOST_KHR) {
                swapchain_dirty_ = true;
            } else {
                static std::atomic<bool> logged{false};
                if (!logged.exchange(true)) LOGW(kTag, "vkAcquireNextImageKHR failed: %d", r);
            }
            SubmitSlot(i, VK_NULL_HANDLE, VK_NULL_HANDLE);  // nothing to show; the slot's fence still signals
            return;
        }
        const VkImage sc = sc_images_[idx];
        CmdImageBarrier(cmd_, sc, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const VkClearColorValue black{};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        // Fit the game's frame into the screen, keeping its aspect ratio.
        const int dw = int(sc_extent_.width), dh = int(sc_extent_.height);
        int w = dw, h = pj.src ? int(int64_t(dw) * pj.sh / std::max(1, pj.sw)) : 0;
        if (h > dh) {
            h = dh;
            w = int(int64_t(dh) * pj.sw / std::max(1, pj.sh));
        }
        // Within a pixel of the whole image: all of it (and then no clear: the frame covers everything).
        if (pj.src && std::abs(w - dw) <= 1 && std::abs(h - dh) <= 1) {
            w = dw;
            h = dh;
        } else {
            vkCmdClearColorImage(cmd_, sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
        }
        if (pj.src) {
            const int x = (dw - w) / 2, y = (dh - h) / 2;
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.srcOffsets[1] = {pj.sw, pj.sh, 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.dstOffsets[0] = {x, y, 0};
            blit.dstOffsets[1] = {x + w, y + h, 1};
            vkCmdBlitImage(cmd_, pj.src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &blit, w == pj.sw && h == pj.sh ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
        }
        CmdImageBarrier(cmd_, sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        SubmitSlot(i, acquire_sem_, render_sem_);
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &render_sem_;
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain_;
        pi.pImageIndices = &idx;
        r = vkQueuePresentKHR(queue_, &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_SURFACE_LOST_KHR) {
            swapchain_dirty_ = true;
        } else if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true)) LOGW(kTag, "vkQueuePresentKHR failed: %d", r);
        }
    }

    // ======================================================================================================
    // device
    // ======================================================================================================
    bool EnsureDevice() {
        if (device_) return true;
        if (init_failed_) return false;
        init_failed_ = true;  // cleared on success
        if (!LoadVulkanLibrary()) {
            LOGE(kTag, "no Vulkan driver — continuing without graphics");
            return false;
        }
        uint32_t inst_version = VK_API_VERSION_1_1;
        auto enum_version = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
            vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
        if (enum_version) enum_version(&inst_version);
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "DSR Native";
        app.apiVersion = std::min<uint32_t>(inst_version, VK_API_VERSION_1_3);
        const char* inst_ext[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        ici.enabledExtensionCount = 2;
        ici.ppEnabledExtensionNames = inst_ext;
        VkResult r = vkCreateInstance(&ici, nullptr, &instance_);
        if (r != VK_SUCCESS) {
            LOGE(kTag, "vkCreateInstance failed: %d — continuing without graphics", r);
            return false;
        }
        LoadInstanceFunctions(instance_);
        uint32_t n = 0;
        vkEnumeratePhysicalDevices(instance_, &n, nullptr);
        if (!n) {
            LOGE(kTag, "no Vulkan GPU found — continuing without graphics");
            return false;
        }
        std::vector<VkPhysicalDevice> pds(n);
        vkEnumeratePhysicalDevices(instance_, &n, pds.data());
        phys_ = pds[0];
        vkGetPhysicalDeviceProperties(phys_, &props_);
        LOGI(kTag, "GPU: %s, Vulkan %u.%u.%u, driver 0x%x (%u.%u)", props_.deviceName, VK_VERSION_MAJOR(props_.apiVersion),
             VK_VERSION_MINOR(props_.apiVersion), VK_VERSION_PATCH(props_.apiVersion), props_.driverVersion,
             VK_VERSION_MAJOR(props_.driverVersion), VK_VERSION_MINOR(props_.driverVersion));
        // Qualcomm's Adreno 6xx drivers (512.x before 512.514) do not expose the BC texture formats the GPU
        // supports; BCeNabler (libadrenotools) patches the driver in memory to expose them, which saves the
        // software decoding (and BC6H/BC7 then look right).
        if (GetRuntime().config.bc_enabler) {
            const adrenotools_bcn_type bcn = adrenotools_get_bcn_type(
                VK_VERSION_MAJOR(props_.driverVersion), VK_VERSION_MINOR(props_.driverVersion), props_.vendorID);
            if (bcn == ADRENOTOOLS_BCN_PATCH) {
                const bool ok = adrenotools_patch_bcn(reinterpret_cast<void*>(vkGetPhysicalDeviceFormatProperties));
                LOGI(kTag, "BC textures: driver patched by BCeNabler: %s", ok ? "ok" : "FAILED");
                bc_patched_ = ok;
            }
        }
        vkGetPhysicalDeviceMemoryProperties(phys_, &mem_props_);

        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(phys_, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qf(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(phys_, &qn, qf.data());
        family_ = UINT32_MAX;
        for (uint32_t i = 0; i < qn; i++)
            if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                family_ = i;
                break;
            }
        if (family_ == UINT32_MAX) {
            LOGE(kTag, "no graphics queue — continuing without graphics");
            return false;
        }

        // Features: dynamic rendering is required for draws; the rest are used when present.
        VkPhysicalDeviceVulkan13Features have13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceFeatures2 have{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        const bool v13 = VK_API_VERSION_MINOR(props_.apiVersion) >= 3 && VK_API_VERSION_MINOR(app.apiVersion) >= 3;
        if (v13) have.pNext = &have13;
        vkGetPhysicalDeviceFeatures2(phys_, &have);
        VkPhysicalDeviceFeatures en{};
        en.textureCompressionBC = have.features.textureCompressionBC;
        en.textureCompressionETC2 = have.features.textureCompressionETC2;
        en.samplerAnisotropy = have.features.samplerAnisotropy;
        en.shaderImageGatherExtended = have.features.shaderImageGatherExtended;
        en.fillModeNonSolid = have.features.fillModeNonSolid;
        en.independentBlend = have.features.independentBlend;
        en.dualSrcBlend = have.features.dualSrcBlend;
        en.imageCubeArray = have.features.imageCubeArray;
        en.depthClamp = have.features.depthClamp;
        en.depthBiasClamp = have.features.depthBiasClamp;
        en.fragmentStoresAndAtomics = have.features.fragmentStoresAndAtomics;
        en.geometryShader = have.features.geometryShader;  // gl_PrimitiveID in fragment shaders
        en.shaderStorageImageWriteWithoutFormat = have.features.shaderStorageImageWriteWithoutFormat;
        features_ = en;
        VkPhysicalDeviceVulkan13Features en13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        en13.dynamicRendering = have13.dynamicRendering;
        VkPhysicalDeviceFeatures2 en2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        en2.features = en;
        if (v13) en2.pNext = &en13;
        LOGI(kTag, "features: dynamic rendering %d, BC textures %d, ETC2 textures %d, anisotropy %d, gather-extended %d",
             int(en13.dynamicRendering), int(en.textureCompressionBC), int(en.textureCompressionETC2),
             int(en.samplerAnisotropy), int(en.shaderImageGatherExtended));

        float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = family_;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        const char* dev_ext[2] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, nullptr};
        uint32_t ndev_ext = 1;
        // Image format lists (core in 1.2): with them, sRGB/linear-viewable images keep their compression.
        format_list_ = VK_API_VERSION_MINOR(props_.apiVersion) >= 2;
        if (false) {  // VK_KHR_image_format_list on Vulkan 1.1 (Adreno 630): glitches (purple streaks); not used
            uint32_t n = 0;
            vkEnumerateDeviceExtensionProperties(phys_, nullptr, &n, nullptr);
            std::vector<VkExtensionProperties> exts(n);
            vkEnumerateDeviceExtensionProperties(phys_, nullptr, &n, exts.data());
            for (const VkExtensionProperties& e : exts)
                if (!strcmp(e.extensionName, VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME)) {
                    dev_ext[ndev_ext++] = VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME;
                    format_list_ = true;
                }
            LOGI(kTag, "image format lists: %s", format_list_ ? "VK_KHR_image_format_list" : "not available");
        }
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.pNext = &en2;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = ndev_ext;
        dci.ppEnabledExtensionNames = dev_ext;
        r = vkCreateDevice(phys_, &dci, nullptr, &device_);
        if (r != VK_SUCCESS) {
            LOGE(kTag, "vkCreateDevice failed: %d — continuing without graphics", r);
            device_ = VK_NULL_HANDLE;
            return false;
        }
        LoadDeviceFunctions(device_);
        ProbePatchedBc();
        dsr::SetGpuHasBc(features_.textureCompressionBC || bc_forced_);
        mip_skip_ = dsr::GetRuntime().config.texture_mip_skip > 0;
        if (mip_skip_) LOGI(kTag, "texture_mip_skip: big BC textures are loaded without their most detailed level");
        if (const int rw = dsr::GetRuntime().config.render_width, rh = dsr::GetRuntime().config.render_height;
            (rw != 1280 || rh != 720) && rw > 0 && rh > 0 && dsr::GetRuntime().config.hud_box) {
            // The largest 16:9 box in the frame (the whole frame at 16:9, e.g. with a render scale).
            hud_box_w_ = std::min(rw, rh * 16 / 9);
            hud_box_h_ = std::min(rh, hud_box_w_ * 9 / 16);
            LOGI(kTag, "aspect ratio: HUD and menus drawn in a centred %dx%d box of the %dx720 frame", hud_box_w_,
                 hud_box_h_, rw);
        }
        if (en13.dynamicRendering && dsr::GetRuntime().config.render_pass_objects)
            LOGI(kTag, "settings render_pass_objects = 1: render pass objects instead of the driver's dynamic rendering");
        else if (en13.dynamicRendering) {
            begin_rendering_ = reinterpret_cast<PFN_vkCmdBeginRendering>(vkGetDeviceProcAddr(device_, "vkCmdBeginRendering"));
            end_rendering_ = reinterpret_cast<PFN_vkCmdEndRendering>(vkGetDeviceProcAddr(device_, "vkCmdEndRendering"));
        }
        mali_barriers_ = dsr::GetRuntime().config.mali_barriers;
        if (mali_barriers_)
            LOGI(kTag, "settings mali_barriers = 1: narrow layout barriers, render targets readable when their pass ends");
        if (!begin_rendering_ || !end_rendering_) {
            // Older drivers (Vulkan 1.1, e.g. Adreno 6xx on Android 10/11): render pass and framebuffer objects.
            begin_rendering_ = nullptr;
            end_rendering_ = nullptr;
            rp_fallback_ = true;
            lean_images_ = dsr::GetRuntime().config.gpu_compression;
            LOGI(kTag, "no dynamic rendering: using render pass objects instead%s",
                 lean_images_ ? "; storage usage only on images written as storage images" : "");
        }

        vkGetDeviceQueue(device_, family_, 0, &queue_);
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = family_;
        vkCreateCommandPool(device_, &pci, nullptr, &pool_);
        // Two sets of per-submission resources: the GPU executes one while the next is recorded.
        for (Slot& sl : slots_) {
            VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            cai.commandPool = pool_;
            cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cai.commandBufferCount = 1;
            vkAllocateCommandBuffers(device_, &cai, &sl.cmd);
            VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            vkCreateFence(device_, &fci, nullptr, &sl.fence);
            if (props_.limits.timestampComputeAndGraphics) {
                VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
                qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
                qi.queryCount = 2;
                if (vkCreateQueryPool(device_, &qi, nullptr, &sl.ts) != VK_SUCCESS) sl.ts = VK_NULL_HANDLE;
            }
            desc_pool_ = MakeDescriptorPool();
            if (!CreateStream() || !desc_pool_) return false;
            if (!persist_pool_) CreatePersistentPool();
            sl.stream = stream_;
            sl.mem = stream_memory_;
            sl.ptr = stream_ptr_;
            sl.pool = desc_pool_;
        }
        UseSlot(0);
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(device_, &sci, nullptr, &acquire_sem_);
        vkCreateSemaphore(device_, &sci, nullptr, &render_sem_);
        LoadPipelineCache();
        LoadShaderCache();
        WarmupShaders();
        init_failed_ = false;
        LOGI(kTag, "device ready (queue family %u)", family_);
        return true;
    }

    uint32_t FindMemory(uint32_t bits, VkMemoryPropertyFlags want) {
        for (uint32_t i = 0; i < mem_props_.memoryTypeCount; i++)
            if ((bits & (1u << i)) && (mem_props_.memoryTypes[i].propertyFlags & want) == want) return i;
        for (uint32_t i = 0; i < mem_props_.memoryTypeCount; i++)
            if (bits & (1u << i)) return i;
        return 0;
    }

    // ---- streaming buffer: per-list copies of vertex/index/uniform data and upload staging -------------------
    bool CreateStream() {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = kStreamSize + kMaxUniformRange;  // slack: a uniform range may start anywhere in the stream
        bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                   VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                   VK_BUFFER_USAGE_TRANSFER_DST_BIT;  // destination of texture -> buffer readbacks
        if (vkCreateBuffer(device_, &bi, nullptr, &stream_) != VK_SUCCESS) return false;
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device_, stream_, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex =
            FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(device_, &ai, nullptr, &stream_memory_) != VK_SUCCESS) return false;
        vkBindBufferMemory(device_, stream_, stream_memory_, 0);
        void* p = nullptr;
        if (vkMapMemory(device_, stream_memory_, 0, kStreamSize + kMaxUniformRange, 0, &p) != VK_SUCCESS) return false;
        stream_ptr_ = static_cast<uint8_t*>(p);
        // A zero-filled uniform range for constant buffers the game did not bind.
        zero_offset_ = 0;
        memset(stream_ptr_, 0, kMaxUniformRange);
        return true;
    }
    // Returns the offset of `size` bytes in the stream (aligned), flushing the current list if full.
    // Callers must not hold offsets across a call that may flush (see PrepareDraw).
    VkDeviceSize StreamAlloc(VkDeviceSize size, VkDeviceSize align, bool* flushed = nullptr) {
        if (size > kStreamSize - kMaxUniformRange) return VK_WHOLE_SIZE;
        VkDeviceSize off = (stream_off_ + align - 1) / align * align;
        if (off + size > kStreamSize) {
            Flush();
            if (flushed) *flushed = true;
            off = (stream_off_ + align - 1) / align * align;
        }
        stream_off_ = off + size;
        return off;
    }
    VkDeviceSize StreamRemaining() const { return kStreamSize - stream_off_; }

    // Compiled pipelines are kept on disk so a second run does not stall on them.
    // One file per driver (its pipeline cache UUID): a cache written by another driver (e.g. Turnip, then the
    // phone's own) is never handed to this one. Drivers should reject foreign data themselves, but a stock Adreno
    // driver given Turnip's cache drew garbled geometry.
    std::string PipelineCachePath() const {
        const std::string& dir = dsr::GetRuntime().config.cache_dir;
        if (dir.empty()) return std::string();
        char id[40];
        snprintf(id, sizeof(id), "%08x_", props_.driverVersion);
        for (int i = 0; i < 8; i++) snprintf(id + 9 + i * 2, 3, "%02x", props_.pipelineCacheUUID[i]);
        return dir + "/vk_pipelines_" + id + ".bin";
    }
    // The header Vulkan defines for pipeline cache data: this device and driver, or nothing.
    bool PipelineCacheMatches(const std::vector<uint8_t>& d) const {
        if (d.size() < 32) return false;
        uint32_t h[4];
        memcpy(h, d.data(), sizeof(h));
        return h[0] >= 32 && h[1] == VK_PIPELINE_CACHE_HEADER_VERSION_ONE && h[2] == props_.vendorID &&
               h[3] == props_.deviceID && memcmp(d.data() + 16, props_.pipelineCacheUUID, VK_UUID_SIZE) == 0;
    }
    void LoadPipelineCache() {
        std::vector<uint8_t> data;
        const std::string path = PipelineCachePath();
        if (FILE* f = path.empty() ? nullptr : fopen(path.c_str(), "rb")) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (n > 0 && n < (256l << 20)) {
                data.resize(size_t(n));
                if (fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
            }
            fclose(f);
        }
        if (!data.empty() && !PipelineCacheMatches(data)) {
            LOGW(kTag, "pipeline cache %s was written by another GPU driver: not used", path.c_str());
            data.clear();
        }
        // The single file of earlier versions (any driver): no longer read.
        const std::string& dir = dsr::GetRuntime().config.cache_dir;
        if (!dir.empty()) remove((dir + "/vk_pipelines.bin").c_str());
        VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        ci.initialDataSize = data.size();
        ci.pInitialData = data.empty() ? nullptr : data.data();
        if (vkCreatePipelineCache(device_, &ci, nullptr, &pipeline_cache_) != VK_SUCCESS) {
            ci.initialDataSize = 0;
            ci.pInitialData = nullptr;
            vkCreatePipelineCache(device_, &ci, nullptr, &pipeline_cache_);
        }
        LOGI(kTag, "pipeline cache: %zu bytes loaded", data.size());
    }
    // Written on a background thread: fetching the cache data (megabytes) and writing the file took the render
    // thread tens of milliseconds, 10 s after new scenery came into view (a stutter).
    std::atomic<bool> cache_saving_{false};
    void SavePipelineCache() {
        if (!pipeline_cache_ || cache_saving_.exchange(true)) return;
        std::thread([this] {
            WritePipelineCache();
            cache_saving_ = false;
        }).detach();
    }
    void WritePipelineCache() {
        const std::string path = PipelineCachePath();
        if (!pipeline_cache_ || path.empty()) return;
        size_t n = 0;
        if (vkGetPipelineCacheData(device_, pipeline_cache_, &n, nullptr) != VK_SUCCESS || !n) return;
        std::vector<uint8_t> data(n);
        if (vkGetPipelineCacheData(device_, pipeline_cache_, &n, data.data()) != VK_SUCCESS) return;
        const std::string tmp = path + ".tmp";
        if (FILE* f = fopen(tmp.c_str(), "wb")) {
            const bool ok = fwrite(data.data(), 1, n, f) == n;
            fclose(f);
            if (ok) rename(tmp.c_str(), path.c_str());
        }
    }

    bool CreatePersistentPool() {
        VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 32768},
                                        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 65536},
                                        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2048},
                                        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 65536}};
        VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        ci.maxSets = 16384;
        ci.poolSizeCount = 4;
        ci.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(device_, &ci, nullptr, &persist_pool_) != VK_SUCCESS) persist_pool_ = VK_NULL_HANDLE;
        return true;
    }
    VkDescriptorPool MakeDescriptorPool() {
        LOGI(kTag, "uniform buffers: up to %u with dynamic offsets per pipeline layout, alignment %llu, range %u",
             props_.limits.maxDescriptorSetUniformBuffersDynamic,
             (unsigned long long)props_.limits.minUniformBufferOffsetAlignment, props_.limits.maxUniformBufferRange);
        VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 16384},
                                        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16384},
                                        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1024},
                                        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 16384}};
        VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        ci.maxSets = 4096;
        ci.poolSizeCount = 4;
        ci.pPoolSizes = sizes;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        if (vkCreateDescriptorPool(device_, &ci, nullptr, &pool) != VK_SUCCESS) return VK_NULL_HANDLE;
        return pool;
    }

    // The current slot starts over: its stream (render thread) and its descriptor pools (recorder, in order with
    // the commands; the slot's earlier submission has finished on the GPU).
    void ResetTransient() {
        stream_off_ = kMaxUniformRange;  // [0, 64K) is the zero buffer
        stream_cache_.clear();
        Rec([this, i = slot_](VkCommandBuffer) {
            Slot& sl = slots_[i];
            rec_slot_ = i;
            desc_pool_ = sl.pool;
            vkResetDescriptorPool(device_, sl.pool, 0);
            for (size_t k = 0; k < sl.extra_used; k++) vkResetDescriptorPool(device_, sl.extra_pools[k], 0);
            sl.extra_used = 0;
            for (VkFramebuffer fb : sl.framebuffers) vkDestroyFramebuffer(device_, fb, nullptr);
            sl.framebuffers.clear();
            desc_cache_.clear();
        });
    }
    void Begin() {
        if (recording_) return;
        BeginCommands();
        recording_ = true;
    }
    // Submit what is recorded so far and start over (streaming buffer / descriptors exhausted).
    void Flush() {
        if (!recording_) return;
        EndRendering();
        SubmitAndWait(VK_NULL_HANDLE, VK_NULL_HANDLE);
        recording_ = false;
        Begin();
        flushes_++;
    }

    // ======================================================================================================
    // swapchain
    // ======================================================================================================
    void DestroySwapchain(bool and_surface) {
        if (device_) vkDeviceWaitIdle(device_);
        if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
        sc_images_.clear();
        if (and_surface && surface_) {
            vkDestroySurfaceKHR(instance_, surface_, nullptr);
            surface_ = VK_NULL_HANDLE;
        }
    }

    // Tells Android the frame rate we present at, so the display runs at a rate that is a multiple of it (on a
    // 120 Hz or 90 Hz panel, 60 fps frames otherwise land on uneven numbers of refreshes: smooth video and
    // camera pans judder). ANativeWindow_setFrameRate is API 30+: looked up at run time.
    void SetWindowFrameRate() {
        using Fn = int32_t (*)(ANativeWindow*, float, int8_t);
        static Fn fn = reinterpret_cast<Fn>(dlsym(RTLD_DEFAULT, "ANativeWindow_setFrameRate"));
        const float fps = float(std::clamp(dsr::GetRuntime().config.target_fps, 30, 60));
        if (!fn) {
            LOGI(kTag, "ANativeWindow_setFrameRate not available: display rate left to the system");
            return;
        }
        window_fps_ = int(fps);
        const int32_t r = fn(static_cast<ANativeWindow*>(window_), fps, 1 /* FIXED_SOURCE */);
        LOGI(kTag, "window frame rate set to %.0f fps (fixed source): %s", fps, r == 0 ? "ok" : "refused");
    }
    // `present_mode` setting. FIFO is the only mode every device must offer, so it is the fallback.
    VkPresentModeKHR ChoosePresentMode() {
        const std::string& want = dsr::GetRuntime().config.present_mode;
        VkPresentModeKHR m = VK_PRESENT_MODE_FIFO_KHR;
        if (want == "mailbox") m = VK_PRESENT_MODE_MAILBOX_KHR;
        else if (want == "immediate") m = VK_PRESENT_MODE_IMMEDIATE_KHR;
        else if (want == "fifo_relaxed") m = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
        else if (want != "fifo") LOGW(kTag, "present_mode '%s' not recognised: using fifo", want.c_str());
        uint32_t n = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &n, nullptr);
        std::vector<VkPresentModeKHR> modes(n);
        if (n) vkGetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &n, modes.data());
        std::string offered;
        for (VkPresentModeKHR x : modes) {
            static const char* kNames[] = {"immediate", "mailbox", "fifo", "fifo_relaxed"};
            offered += x <= 3                                                ? kNames[x]
                       : x == VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR     ? "shared_demand_refresh"
                       : x == VK_PRESENT_MODE_SHARED_CONTINUOUS_REFRESH_KHR ? "shared_continuous_refresh"
                                                                            : std::to_string(int(x)).c_str();
            offered += ' ';
        }
        const bool ok = std::find(modes.begin(), modes.end(), m) != modes.end();
        if (!ok) m = VK_PRESENT_MODE_FIFO_KHR;
        if (!present_mode_logged_) {
            present_mode_logged_ = true;
            LOGI(kTag, "present mode: %s requested, %s used (device offers: %s)", want.c_str(),
                 m == VK_PRESENT_MODE_MAILBOX_KHR     ? "mailbox"
                 : m == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate"
                 : m == VK_PRESENT_MODE_FIFO_RELAXED_KHR ? "fifo_relaxed"
                                                         : "fifo",
                 offered.c_str());
        }
        return m;
    }
    bool present_mode_logged_ = false;
    bool EnsureSwapchain() {
        void* w = dsr::video::GetNativeWindow();
        if (w != window_) {
            if (window_) LOGI(kTag, "window changed (%p -> %p)", window_, w);
            DestroySwapchain(true);
            window_ = w;
        }
        if (!window_) return false;
        if (!surface_) {
            VkAndroidSurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
            sci.window = static_cast<ANativeWindow*>(window_);
            VkResult r = vkCreateAndroidSurfaceKHR(instance_, &sci, nullptr, &surface_);
            if (r != VK_SUCCESS) {
                LogOnce("surface", "vkCreateAndroidSurfaceKHR failed: %d", r);
                surface_ = VK_NULL_HANDLE;
                return false;
            }
            SetWindowFrameRate();
        }
        if (swapchain_ && !swapchain_dirty_) return true;
        Drain();  // the recorder acquires and presents with the swapchain
        if (swapchain_) vkQueueWaitIdle(queue_);

        VkSurfaceCapabilitiesKHR caps;
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, surface_, &caps) != VK_SUCCESS) return false;
        uint32_t nf = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &nf, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(nf);
        vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &nf, formats.data());
        if (formats.empty()) return false;
        VkSurfaceFormatKHR fmt = formats[0];
        for (const auto& f : formats)
            if (f.format == VK_FORMAT_R8G8B8A8_UNORM) {
                fmt = f;
                break;
            }
        VkExtent2D extent = caps.currentExtent;
        if (extent.width == UINT32_MAX) {
            auto* aw = static_cast<ANativeWindow*>(window_);
            extent = {uint32_t(ANativeWindow_getWidth(aw)), uint32_t(ANativeWindow_getHeight(aw))};
        }
        // The swapchain is made at the size of the game's frame instead of the screen's (same aspect ratio as the
        // window, so that the frame fits it 1:1): the system compositor then scales it to the screen (in the
        // display hardware, as a rule) instead of the GPU clearing and filling a screen-sized image every frame
        // (2340x1080 from 1560x720 on the OnePlus 6T: twice the pixels of the game's own frame).
        const VkExtent2D window_extent = extent;
        if (dsr::GetRuntime().config.compositor_scaling && frame_w_ > 0 && frame_h_ > 0 && extent.width && extent.height) {
            const double k = std::max(double(frame_w_) / extent.width, double(frame_h_) / extent.height);
            if (k < 0.999) {
                extent.width = std::clamp(uint32_t(std::lround(extent.width * k)), std::max(1u, caps.minImageExtent.width),
                                          caps.maxImageExtent.width);
                extent.height = std::clamp(uint32_t(std::lround(extent.height * k)), std::max(1u, caps.minImageExtent.height),
                                           caps.maxImageExtent.height);
            }
        }
        // Identity transform: the compositor rotates for the panel (the AYN Thor reports 90 degrees);
        // such swapchains report SUBOPTIMAL every frame, which is expected.
        VkSurfaceTransformFlagBitsKHR transform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                                                      ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                                                      : caps.currentTransform;
        VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        for (VkCompositeAlphaFlagBitsKHR a : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR})
            if (caps.supportedCompositeAlpha & a) {
                alpha = a;
                break;
            }
        uint32_t count = std::max(caps.minImageCount, 3u);
        if (caps.maxImageCount) count = std::min(count, caps.maxImageCount);

        VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        ci.surface = surface_;
        ci.minImageCount = count;
        ci.imageFormat = fmt.format;
        ci.imageColorSpace = fmt.colorSpace;
        ci.imageExtent = extent;
        ci.imageArrayLayers = 1;
        ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform = transform;
        ci.compositeAlpha = alpha;
        ci.presentMode = ChoosePresentMode();
        ci.clipped = VK_TRUE;
        ci.oldSwapchain = swapchain_;
        VkSwapchainKHR sc = VK_NULL_HANDLE;
        VkResult r = vkCreateSwapchainKHR(device_, &ci, nullptr, &sc);
        if (r != VK_SUCCESS && ci.presentMode != VK_PRESENT_MODE_FIFO_KHR) {
            LOGW(kTag, "swapchain with present mode %d failed (%d): retrying with fifo", int(ci.presentMode), r);
            ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
            r = vkCreateSwapchainKHR(device_, &ci, nullptr, &sc);
        }
        DestroySwapchain(false);
        if (r != VK_SUCCESS) {
            LogOnce("swapchain", "vkCreateSwapchainKHR failed: %d", r);
            return false;
        }
        swapchain_ = sc;
        swapchain_dirty_ = false;
        sc_extent_ = extent;
        uint32_t ni = 0;
        vkGetSwapchainImagesKHR(device_, swapchain_, &ni, nullptr);
        sc_images_.resize(ni);
        vkGetSwapchainImagesKHR(device_, swapchain_, &ni, sc_images_.data());
        if (++swapchains_created_ <= 8)
            LOGI(kTag, "swapchain %ux%u (window %ux%u, game frame %dx%d), format %d, %u images", extent.width, extent.height,
                 window_extent.width, window_extent.height, frame_w_, frame_h_, fmt.format, ni);
        return true;
    }

    // ======================================================================================================
    // images
    // ======================================================================================================
    Image* FindImage(const TexRef& t) {
        auto it = images_.find(t.addr);
        if (it == images_.end()) return nullptr;
        Image& img = it->second;
        const int ow = img.mip_skip ? img.tex.width : img.width, oh = img.mip_skip ? img.tex.height : img.height;
        if (img.nvn_format != t.tex.format || ow != t.tex.width || oh != t.tex.height) {
            const uint64_t key = t.addr ^ (uint64_t(t.tex.format) << 48) ^ (uint64_t(img.nvn_format) << 56);
            if (mismatch_logged_.size() < 24 && mismatch_logged_.insert(key).second)
                LOGW(kTag, "memory @%llx used as fmt %d %dx%d but holds an image of fmt %d %dx%d%s (replaced)",
                     (unsigned long long)t.addr, t.tex.format, t.tex.width, t.tex.height, img.nvn_format, img.width,
                     img.height, img.written ? " that was drawn into" : "");
            return nullptr;
        }
        return &img;
    }

    // Adreno 6xx drivers patched by BCeNabler create and sample BC images, but their format query keeps reporting
    // the formats as unsupported (so every BC texture was still decoded to RGBA8 on the CPU: 4 to 8 times the
    // memory and the sampling bandwidth). Images of each BC format are created here as a test; if all work, BC
    // textures are used as they are.
    void ProbePatchedBc() {
        if (!bc_patched_) return;
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(phys_, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, &fp);
        static const VkFormat kFormats[] = {
            VK_FORMAT_BC1_RGB_UNORM_BLOCK, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC1_RGBA_SRGB_BLOCK,
            VK_FORMAT_BC2_UNORM_BLOCK,     VK_FORMAT_BC3_UNORM_BLOCK,      VK_FORMAT_BC3_SRGB_BLOCK,
            VK_FORMAT_BC4_UNORM_BLOCK,     VK_FORMAT_BC5_UNORM_BLOCK,      VK_FORMAT_BC7_UNORM_BLOCK,
            VK_FORMAT_BC6H_SFLOAT_BLOCK};
        int failed = 0, first_failed = 0;
        for (int mut = 0; mut < 2; mut++) {
            for (VkFormat f : kFormats) {
                if (mut && f != VK_FORMAT_BC1_RGBA_UNORM_BLOCK && f != VK_FORMAT_BC3_UNORM_BLOCK) continue;
                VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
                ci.imageType = VK_IMAGE_TYPE_2D;
                ci.format = f;
                ci.extent = {256, 256, 1};
                ci.mipLevels = 9;
                ci.arrayLayers = 1;
                ci.samples = VK_SAMPLE_COUNT_1_BIT;
                ci.tiling = VK_IMAGE_TILING_OPTIMAL;
                ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
                ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                const VkFormat pair[2] = {f, f == VK_FORMAT_BC1_RGBA_UNORM_BLOCK ? VK_FORMAT_BC1_RGBA_SRGB_BLOCK
                                                                                 : VK_FORMAT_BC3_SRGB_BLOCK};
                VkImageFormatListCreateInfo fl{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
                if (mut) {  // viewed as sRGB or linear, as game textures are
                    ci.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
                    fl.viewFormatCount = 2;
                    fl.pViewFormats = pair;
                    if (format_list_) ci.pNext = &fl;
                }
                VkImage image = VK_NULL_HANDLE;
                const VkResult r = vkCreateImage(device_, &ci, nullptr, &image);
                VkMemoryRequirements req{};
                if (r == VK_SUCCESS) vkGetImageMemoryRequirements(device_, image, &req);
                // BC1 256x256 with all its levels is 43.7 KB; an image stored uncompressed would need 350 KB.
                const bool ok = r == VK_SUCCESS && req.size > 0 && req.size < 300 * 1024;
                if (!ok && !failed++) first_failed = int(f);
                if (image) vkDestroyImage(device_, image, nullptr);
            }
        }
        bc_forced_ = failed == 0;
        LOGI(kTag, "BC textures: patched driver reports features 0x%x for BC1; test images: %s (%d failed, first format %d)%s",
             fp.optimalTilingFeatures, failed ? "FAILED" : "ok", failed, first_failed,
             bc_forced_ ? " - BC textures used as they are (not decoded)" : " - BC textures stay decoded on upload");
    }

    // What a BC texture is converted into when the GPU cannot sample BC: ETC2 when the GPU has it (BC1 always;
    // BC3 with settings bc3_etc2), else decoded. ETC2 keeps BC1 textures at their compressed size (8 bytes per
    // 4x4 block; decoded RGBA8 is 4x larger), which saves memory and texture bandwidth on Mali.
    BcTo BcTarget(int nvn_format) {
        const dsr::RuntimeConfig& c = dsr::GetRuntime().config;
        if (!features_.textureCompressionETC2 || !c.bc_etc2) return BcTo::Decoded;
        BcTo to = BcTo::Decoded;
        if (IsEtcEncodable(nvn_format)) to = BcTo::Etc2;
        else if (c.bc3_etc2 && IsEtcRgba8Encodable(nvn_format)) to = BcTo::Etc2Rgba8;
        if (to == BcTo::Decoded) return to;
        VkFormatProperties p;
        vkGetPhysicalDeviceFormatProperties(phys_, ConvertedFormat(nvn_format, to), &p);
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        return (p.optimalTilingFeatures & need) == need ? to : BcTo::Decoded;
    }

    VkFormat PickFormat(int nvn_format, VkFormatFeatureFlags* features) {
        auto cached = format_cache_.find(nvn_format);
        if (cached != format_cache_.end()) {
            *features = cached->second.second;
            return cached->second.first;
        }
        VkFormatFeatureFlags need = IsDepthFormat(nvn_format) ? VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT
                                                              : (VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
                                                                 VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
        VkFormat chosen = VK_FORMAT_UNDEFINED;
        VkFormatFeatureFlags feats = 0;
        for (VkFormat f : ImageCandidates(nvn_format)) {
            VkFormatProperties p;
            vkGetPhysicalDeviceFormatProperties(phys_, f, &p);
            // Driver patched by BCeNabler: BC images work, though the format query still reports nothing.
            if (bc_forced_ && BcKindOf(nvn_format) != BcKind::None)
                p.optimalTilingFeatures |= need | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
            if ((p.optimalTilingFeatures & need) == need) {
                chosen = f;
                feats = p.optimalTilingFeatures;
                break;
            }
        }
        if (chosen == VK_FORMAT_UNDEFINED && BcKindOf(nvn_format) != BcKind::None) {
            // No BC support (Adreno 6xx and older, Mali): converted on upload, into ETC2 or decoded.
            const BcTo to = BcTarget(nvn_format);
            const VkFormat f = ConvertedFormat(nvn_format, to);
            VkFormatProperties p;
            vkGetPhysicalDeviceFormatProperties(phys_, f, &p);
            if ((p.optimalTilingFeatures & need) == need) {
                chosen = f;
                feats = p.optimalTilingFeatures;
                LOGI(kTag, "NVN format %d (block-compressed) not supported by the GPU: %s on upload (Vulkan format %d)",
                     nvn_format, to == BcTo::Decoded ? "decoded" : "re-encoded as ETC2", int(f));
                if (BcKindOf(nvn_format) == BcKind::BC7 || BcKindOf(nvn_format) == BcKind::BC6H_S ||
                    BcKindOf(nvn_format) == BcKind::BC6H_U)
                    LOGW(kTag, "NVN format %d: decoding not implemented yet, shown as grey", nvn_format);
            }
        }
        if (chosen == VK_FORMAT_UNDEFINED) LOGW(kTag, "NVN format %d has no usable Vulkan format yet", nvn_format);
        format_cache_[nvn_format] = {chosen, feats};
        *features = feats;
        return chosen;
    }

    // Image for a texture, created on first use. For sampling (`upload`), a texture the GPU never wrote is
    // filled from game memory (deswizzled) the first time.
    Image* GetImage(const TexRef& t, bool upload) {
        if (!t.valid || t.tex.width <= 0 || t.tex.height <= 0) return nullptr;
        Image* img = FindImage(t);
        if (!img) img = CreateImage(t);
        if (!img) return nullptr;
        if (img->pending && !img->written) return nullptr;  // converting in the background: not drawn yet
        if (upload && TakeFrame(*img, t)) return img;
        if (upload && !img->written && !img->uploaded) {
            Upload(*img, t);
        } else if (upload && img->cpu_source && img->checked_in_list != list_counter_ &&
                   (list_counter_ >= img->next_check_list ||
                    size_t(img->width) * img->height <= 4096)) {  // small (lookup tables): every list
            // Most game textures never change after loading; hashing every one every frame cost ~15 ms a
            // frame (a 1 MB texture is thousands of cache misses). So checks back off while nothing changes.
            img->checked_in_list = list_counter_;
            const bool big = size_t(img->width) * img->height >= 512 * 512;
            if (big) big_checks_++;
            if (SampleHash(t) == img->sample_hash) {
                img->check_interval = std::min<uint32_t>(img->check_interval * 2, kMaxCheckInterval);
                img->next_check_list = list_counter_ + img->check_interval;
            } else {
                if (big) big_changes_++;
                Upload(*img, t);
                if (++reuploads_ <= 8 || reuploads_ % 300 == 0)
                    LOGI(kTag, "texture %dx%d format %d changed in game memory: re-uploaded (%llu so far)", img->width,
                         img->height, img->nvn_format, (unsigned long long)reuploads_);
            }
        }
        return img;
    }

    // Creates the VkImage (and its memory) for an Image's description. `img.twin` = the sRGB/linear counterpart
    // views may use (UNDEFINED: none); it is cleared when the image cannot be viewed that way.
    bool AllocImage(Image& img, bool storage, bool mutable_fmt, VkImage* out_image, VkDeviceMemory* out_memory) {
        const int target = img.target;
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = img.type;
        ci.format = img.vk_format;
        ci.extent = {uint32_t(img.width), uint32_t(img.type == VK_IMAGE_TYPE_1D ? 1 : img.height), uint32_t(img.depth)};
        ci.mipLevels = uint32_t(img.levels);
        ci.arrayLayers = uint32_t(img.layers);
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        if ((target == kTargetCubemap || target == kTargetCubemapArray) && img.layers % 6 == 0 &&
            img.width == img.height)
            ci.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        if (storage) ci.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        img.storage = storage;
        if (IsDepthFormat(img.nvn_format))
            ci.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        else if (img.features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)
            ci.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        // Formats with an sRGB/linear counterpart may be viewed as either (NVN texture views): mutable, with the
        // pair listed so the driver can keep its compression.
        VkFormat pair[2] = {img.vk_format, img.twin};
        VkImageFormatListCreateInfo fl{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
        // `hole_test` 2/3 (diagnostic): depth images mutable, which makes Adreno store them uncompressed.
        if ((dsr::GetRuntime().config.hole_test & 2) && IsDepthFormat(img.nvn_format))
            ci.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        // `hole_test` 256: colour render targets mutable with no format list (stored uncompressed on Adreno).
        const bool plain_color = (dsr::GetRuntime().config.hole_test & 256) && !IsDepthFormat(img.nvn_format) &&
                                 (ci.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
        img.mutable_fmt = false;
        if (plain_color) {
            ci.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
            img.mutable_fmt = true;
        } else if (pair[1] != VK_FORMAT_UNDEFINED && mutable_fmt) {
            img.mutable_fmt = true;
            ci.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
            fl.viewFormatCount = 2;
            fl.pViewFormats = pair;
            // The format list is Vulkan 1.2 or VK_KHR_image_format_list (without it Adreno stores mutable images
            // uncompressed).
            if (format_list_) ci.pNext = &fl;
        }
        VkImage image = VK_NULL_HANDLE;
        if (vkCreateImage(device_, &ci, nullptr, &image) != VK_SUCCESS) {
            LogOnce("image", "vkCreateImage failed for %dx%d format %d", img.width, img.height, img.nvn_format);
            return false;
        }
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device_, image, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkDeviceMemory memory = VK_NULL_HANDLE;
        if (vkAllocateMemory(device_, &ai, nullptr, &memory) != VK_SUCCESS) {
            vkDestroyImage(device_, image, nullptr);
            LogOnce("memory", "out of GPU memory for %dx%d format %d", img.width, img.height, img.nvn_format);
            return false;
        }
        vkBindImageMemory(device_, image, memory, 0);
        *out_image = image;
        *out_memory = memory;
        return true;
    }

    // Older Adreno GPUs (lean_images_): images are created without the storage usage and the mutable-format
    // flag, which would turn off their bandwidth compression; an image the game writes as a storage image, or
    // views in its sRGB/linear twin format, is re-created with them (contents copied over, the old one retired
    // like a destroyed image, its layout restored).
    bool AddStorage(Image& img) {
        if (!lean_images_ || img.storage || !(img.features & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) return img.storage;
        storage_addrs_.insert(img.addr);
        return Recreate(img, true, img.mutable_fmt, "written as a storage image");
    }
    bool MakeMutable(Image& img) {
        if (img.mutable_fmt || img.twin == VK_FORMAT_UNDEFINED) return img.mutable_fmt;
        mutable_addrs_.insert(img.addr);
        return Recreate(img, img.storage, true, "viewed in its sRGB/linear twin format");
    }
    bool Recreate(Image& img, bool storage, bool mutable_fmt, const char* why) {
        EndRendering();
        const VkImageLayout old_layout = img.layout;
        VkImage ni = VK_NULL_HANDLE;
        VkDeviceMemory nm = VK_NULL_HANDLE;
        const bool was_storage = img.storage, was_mutable = img.mutable_fmt;
        if (!AllocImage(img, storage, mutable_fmt, &ni, &nm)) {
            img.storage = was_storage;
            img.mutable_fmt = was_mutable;
            return false;
        }
        const bool keep = img.written || img.uploaded || img.clear_pending;
        if (keep) Transition(img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        const VkImage old_image = img.image;
        Retired r{submit_seq_ + 1, img.image, img.memory, {}};
        for (auto& [k, v] : img.views) r.views.push_back(v);
        retired_.push_back(std::move(r));
        img.views.clear();
        img.fast_views.clear();
        img.image = ni;
        img.memory = nm;
        img.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        img.attach_unsynced = false;
        if (keep) {
            Transition(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            std::vector<VkImageCopy> regions;
            for (int l = 0; l < img.levels; l++) {
                VkImageCopy c{};
                c.srcSubresource = {img.aspect, uint32_t(l), 0, uint32_t(img.layers)};
                c.dstSubresource = c.srcSubresource;
                c.extent = {uint32_t(std::max(1, img.width >> l)),
                            uint32_t(img.type == VK_IMAGE_TYPE_1D ? 1 : std::max(1, img.height >> l)),
                            uint32_t(std::max(1, img.depth >> l))};
                regions.push_back(c);
            }
            Rec([regions = std::move(regions), old_image, ni](VkCommandBuffer cb) {
                vkCmdCopyImage(cb, old_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ni,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(regions.size()), regions.data());
            });
        }
        Rec([this](VkCommandBuffer) {
            persist_cache_.clear();
            desc_cache_.clear();
        });
        if (keep || old_layout != VK_IMAGE_LAYOUT_UNDEFINED) {
            const VkImageLayout to = old_layout != VK_IMAGE_LAYOUT_UNDEFINED ? old_layout : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            Transition(img, to);
        }
        if (++recreated_ <= 16)
            LOGI(kTag, "image %dx%d format %d is %s: re-created for it (%d so far)", img.width, img.height,
                 img.nvn_format, why, recreated_);
        return true;
    }

    Image* CreateImage(const TexRef& t) {
        VkFormatFeatureFlags feats = 0;
        VkFormat vf = PickFormat(t.tex.format, &feats);
        if (vf == VK_FORMAT_UNDEFINED) {
            if (unknown_formats_.insert(t.tex.format).second)
                LOGW(kTag, "first texture with unsupported NVN format %d: %dx%dx%d, target %d, %d levels", t.tex.format,
                     t.tex.width, t.tex.height, t.tex.depth, t.tex.target, t.tex.levels);
            return nullptr;
        }
        auto old = images_.find(t.addr);
        if (old != images_.end()) {
            DestroyImage(old->second);  // retired: destroyed once the GPU is done with it
            images_.erase(old);
        }
        const TextureLayout layout = ComputeLayout(t.tex);
        Image img;
        img.nvn_format = t.tex.format;
        img.target = t.tex.target;
        img.addr = t.addr;
        img.tex = t.tex;
        img.vk_format = vf;
        if (BcKindOf(t.tex.format) != BcKind::None)  // a BC texture in a format the GPU has instead: converted
            for (BcTo to : {BcTo::Etc2, BcTo::Etc2Rgba8, BcTo::Decoded})
                if (vf == ConvertedFormat(t.tex.format, to) && (to == BcTo::Decoded || vf != BcDecodedFormat(t.tex.format))) {
                    img.bc = BcKindOf(t.tex.format);
                    img.bc_to = to;
                    break;
                }
        img.width = t.tex.width;
        img.height = t.tex.height;
        img.levels = std::max(1, std::min(t.tex.levels, int(layout.levels.size())));
        img.layers = std::max(1, layout.layers);
        // texture_mip_skip: big BC textures (the game's art; never drawn into) without their most detailed level.
        if (mip_skip_ && BcKindOf(t.tex.format) != BcKind::None && img.levels >= 2 && t.tex.width >= 512 &&
            t.tex.height >= 512 && t.tex.target != kTarget3D && t.tex.target != kTarget1D &&
            t.tex.target != kTarget1DArray) {
            img.mip_skip = 1;
            img.width = std::max(1, t.tex.width >> 1);
            img.height = std::max(1, t.tex.height >> 1);
            img.levels -= 1;
            mip_skipped_++;
        }
        img.depth = t.tex.target == kTarget3D ? std::max(1, t.tex.depth) : 1;
        img.aspect = AspectOf(t.tex.format);
        img.features = feats;
        img.type = t.tex.target == kTarget3D ? VK_IMAGE_TYPE_3D
                   : (t.tex.target == kTarget1D || t.tex.target == kTarget1DArray) ? VK_IMAGE_TYPE_1D
                                                                                    : VK_IMAGE_TYPE_2D;
        img.twin = SrgbTwin(vf);
        // Older Adreno GPUs keep no bandwidth compression (UBWC) for images that may be storage images, so there
        // the usage is only added to images the game has written as storage images (see AddStorage).
        const bool storage = (feats & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) && (!lean_images_ || storage_addrs_.count(t.addr));
        const bool mutable_fmt = img.twin != VK_FORMAT_UNDEFINED && (!lean_images_ || mutable_addrs_.count(t.addr));
        if (!AllocImage(img, storage, mutable_fmt, &img.image, &img.memory)) return nullptr;
        if (images_created_++ < 48)
            LOGI(kTag, "image %dx%d%s NVN format %d (target %d, %d levels, %d layers) -> Vulkan format %d", img.width,
                 img.height, img.depth > 1 ? "x3D" : "", img.nvn_format, img.target, img.levels, img.layers, vf);
        return &(images_[t.addr] = img);
    }

    void ResetPersistentSets() {
        if (!persist_pool_) return;
        Drain();
        vkResetDescriptorPool(device_, persist_pool_, 0);
        persist_cache_.clear();
        desc_cache_.clear();  // may hold sets from the persistent pool
        InvalidateBoundState();
        persist_resets_++;
    }
    // An image the game no longer uses (its memory now holds another texture). Frames still on the GPU, or
    // still queued for the recorder, may read it: its Vulkan objects are destroyed once the submission being
    // recorded now has finished. (Destroying at once, and emptying the descriptor-set pool the GPU was still
    // reading, showed grey textures for a frame when the game streamed in new textures.)
    struct Retired {
        uint64_t seq;
        VkImage image;
        VkDeviceMemory memory;
        std::vector<VkImageView> views;
    };
    std::deque<Retired> retired_;
    uint64_t submit_seq_ = 0;     // submissions queued (render thread)
    uint64_t completed_seq_ = 0;  // submissions known finished on the GPU
    uint64_t images_retired_ = 0;
    void DestroyImage(Image& img) {
        if (InOpenPass(&img)) EndRendering();  // the pass bookkeeping points at it
        small_rts_.erase(&img);
        Retired r{submit_seq_ + 1, img.image, img.memory, {}};
        for (auto& [k, v] : img.views) r.views.push_back(v);
        retired_.push_back(std::move(r));
        images_retired_++;
        // Cached descriptor sets may name its views: forgotten (their pool is emptied when full, GPU idle).
        Rec([this](VkCommandBuffer) {
            persist_cache_.clear();
            desc_cache_.clear();
        });
        img = Image{};
    }
    void ReapRetired() {
        while (!retired_.empty() && retired_.front().seq <= completed_seq_) {
            Retired& r = retired_.front();
            if (rp_fallback_) ForgetFramebuffers(r.views);
            for (VkImageView v : r.views) vkDestroyImageView(device_, v, nullptr);
            if (r.image) vkDestroyImage(device_, r.image, nullptr);
            if (r.memory) vkFreeMemory(device_, r.memory, nullptr);
            retired_.pop_front();
        }
    }

    // Cheap change detector for game-memory textures: 64 bytes at 64 spots plus both ends.
    uint64_t SampleHash(const TexRef& t) {
        const size_t total = ComputeLayout(t.tex).total;
        if (!total || !GuestRangeValid(t.addr, total)) return 0;
        const uint8_t* p = reinterpret_cast<const uint8_t*>(t.addr);
        uint64_t h = Fnv(p, std::min<size_t>(256, total));
        h = Fnv(p + total - std::min<size_t>(256, total), std::min<size_t>(256, total), h);
        // Large textures (video frames: mostly dark, letterboxed) need many spots or changes go unnoticed.
        const size_t spots = total >= (1u << 20) ? 1024 : 64, len = total >= (1u << 20) ? 32 : 64;
        for (size_t i = 1; i < spots; i++) {
            size_t off = total / spots * i;
            h = Fnv(p + off, std::min<size_t>(len, total - off), h);
        }
        return h;
    }

    // Bytes one level of a converted BC texture takes in the upload stream.
    static size_t ConvertedLevelBytes(const Image& img, int w, int h, int d) {
        switch (img.bc_to) {
            case BcTo::Etc2: return EtcLevelBytes(w, h, d);
            case BcTo::Etc2Rgba8: return EtcRgba8LevelBytes(w, h, d);
            default: return size_t(w) * size_t(h) * size_t(d) * size_t(BcDecodedBytes(img.bc));
        }
    }

    // ---- BC texture conversion (GPUs without BC), spread over the CPU cores ------------------------------------
    // From pull request #13 (fernanfez00): a texture's levels used to be converted one after another on the render
    // thread (scenes with many new textures took seconds to load). The blocks of every level are deswizzled first,
    // then converted in bands of block rows by a pool of workers (parallel.h), the render thread helping. ETC2
    // results are also kept in a cache keyed by the source blocks: the game uploads the same textures again.
    struct BcJob {
        BcKind kind;
        BcTo to;
        bool punch;
        const uint8_t* blocks;  // deswizzled, tightly packed rows of `bx` blocks, `brows` rows per slice
        int bx, brows, w, h, depth;
        uint8_t* out;
        size_t out_bytes;
        uint64_t key;  // ETC2 cache key (0: not cached)
    };
    struct BcTask {
        uint32_t job;
        int z, row0, rows;
    };
    std::vector<uint8_t> bc_scratch_;
    std::vector<BcJob> bc_jobs_;
    std::vector<BcTask> bc_tasks_;
    uint64_t bc_decoded_bytes_ = 0;
    std::unordered_map<uint64_t, std::vector<uint8_t>> etc_cache_;
    size_t etc_cache_bytes_ = 0;
    static constexpr size_t kEtcCacheLimit = 96u << 20;
    double bc_convert_ms_ = 0;     // conversion time, for the stats line
    uint64_t bc_cache_hits_ = 0, bc_converted_levels_ = 0;

    // ---- BC textures: converted on a background thread --------------------------------------------------
    // Converting a large texture takes seconds on a phone; done on the render thread it stalled the game. Now one
    // background thread converts it (WorkPool helps), and the render thread records the copy a few frames later
    // (PumpBcUploads), at most kBcFrameBudget bytes per frame. Until the copy is recorded the texture is not drawn.
    static constexpr size_t kBcFrameBudget = size_t(24) << 20;
    static constexpr size_t kBcSyncBytes = size_t(2) << 20;  // converted textures up to this size are not queued

    struct PendingLevel {
        size_t src_off = 0, n = 0, out_off = 0, out_bytes = 0;
        LevelLayout lay;
        bool linear = false;
        int bx = 0, brows = 0, w = 1, h = 1, depth = 1;
    };
    struct PendingBc {
        uint64_t addr = 0, serial = 0;
        int nvn_format = 0;
        BcKind kind = BcKind::None;
        BcTo to = BcTo::Decoded;
        bool punch = false;
        std::vector<uint8_t> raw;                // a copy of the texture's guest memory (the game may rewrite it)
        std::vector<PendingLevel> levels;
        std::vector<VkBufferImageCopy> regions;  // bufferOffset relative to `result`
        size_t result_bytes = 0;
        std::vector<uint8_t> result;             // converted levels, back to back (filled by the background thread)
        uint64_t cache_hits = 0;
        std::atomic<bool> done{false};
    };
    std::deque<std::shared_ptr<PendingBc>> bc_queue_;     // to the background thread (under bc_mutex_)
    std::deque<std::shared_ptr<PendingBc>> bc_inflight_;  // render thread: queued order, recorded when done
    std::mutex bc_mutex_, etc_mutex_;
    std::condition_variable bc_cv_;
    bool bc_stop_ = false;
    std::thread bc_thread_;
    uint64_t image_serial_ = 0, bc_queued_ = 0;

    void EnsureBcThread() {
        if (!bc_thread_.joinable()) bc_thread_ = std::thread([this] { BcThreadLoop(); });
    }
    void StopBcThread() {
        {
            std::lock_guard<std::mutex> lk(bc_mutex_);
            bc_stop_ = true;
        }
        bc_cv_.notify_all();
        if (bc_thread_.joinable()) bc_thread_.join();
    }
    void BcThreadLoop() {
        for (;;) {
            std::shared_ptr<PendingBc> p;
            {
                std::unique_lock<std::mutex> lk(bc_mutex_);
                bc_cv_.wait(lk, [this] { return bc_stop_ || !bc_queue_.empty(); });
                if (bc_queue_.empty()) return;  // stop requested and nothing left to convert
                p = bc_queue_.front();
                bc_queue_.pop_front();
            }
            ConvertPending(*p);
            p->done.store(true, std::memory_order_release);
        }
    }

    // Converts the BC jobs (BC -> RGBA8 or ETC2) with WorkPool; the tasks of each job write its own output.
    void ConvertJobs(const std::vector<BcJob>& jobs) {
        if (jobs.empty()) return;
        std::vector<BcTask> tasks;
        for (uint32_t j = 0; j < jobs.size(); j++) {
            const BcJob& job = jobs[j];
            const int band = std::max(2, 4096 / std::max(1, job.bx));  // ~4096 blocks per task
            for (int z = 0; z < job.depth; z++)
                for (int r = 0; r < job.brows && r * 4 < job.h; r += band)
                    tasks.push_back({j, z, r, std::min(band, job.brows - r)});
        }
        WorkPool::Get().Run(tasks.size(), [&](size_t i) {
            thread_local std::vector<uint8_t> rgba;
            const BcTask& k = tasks[i];
            const BcJob& j = jobs[k.job];
            const int bpb = (j.kind == BcKind::BC1 || j.kind == BcKind::BC4U || j.kind == BcKind::BC4S) ? 8 : 16;
            const uint8_t* blocks = j.blocks + (size_t(k.z) * size_t(j.brows) + size_t(k.row0)) * size_t(j.bx) * bpb;
            const int y0 = k.row0 * 4, hh = std::min(j.h - y0, k.rows * 4);
            const size_t slice = j.out_bytes / size_t(std::max(1, j.depth));
            uint8_t* out = j.out + size_t(k.z) * slice;
            const int bxn = (j.w + 3) / 4;
            switch (j.to) {
                case BcTo::Decoded:
                    BcDecode(j.kind, blocks, j.bx, k.rows, out + size_t(y0) * size_t(j.w) * size_t(BcDecodedBytes(j.kind)),
                             j.w, hh);
                    break;
                case BcTo::Etc2:
                case BcTo::Etc2Rgba8:
                    rgba.resize(size_t(j.w) * size_t(hh) * 4);
                    BcDecode(j.kind, blocks, j.bx, k.rows, rgba.data(), j.w, hh);
                    if (j.to == BcTo::Etc2)
                        EncodeEtc2Rgb(out + size_t(k.row0) * size_t(bxn) * 8, rgba.data(), j.w, hh, 1, j.punch);
                    else
                        EncodeEtc2Rgba8(out + size_t(k.row0) * size_t(bxn) * 16, rgba.data(), j.w, hh, 1);
                    break;
            }
        });
    }

    // Background thread: deswizzles every level, takes what the ETC2 cache has, converts the rest.
    void ConvertPending(PendingBc& p) {
        p.result.assign(p.result_bytes, 0);
        size_t blocks_total = 0;
        for (const PendingLevel& L : p.levels) blocks_total += L.n;
        std::vector<uint8_t> blocks(blocks_total);
        std::vector<BcJob> jobs;
        std::vector<uint64_t> keys;  // parallel to jobs: the ETC2 cache key (0: not cached)
        size_t off = 0;
        for (const PendingLevel& L : p.levels) {
            uint8_t* dst = blocks.data() + off;
            off += L.n;
            if (L.linear)
                memcpy(dst, p.raw.data() + L.src_off, L.n);
            else
                DeswizzleLevel(dst, p.raw.data() + L.src_off, L.lay);
            uint8_t* out = p.result.data() + L.out_off;
            uint64_t key = 0;
            if (p.to != BcTo::Decoded) {
                key = HashStep(Fnv(dst, L.n), uint64_t(p.nvn_format) << 48 ^ uint64_t(L.w) << 24 ^ uint64_t(L.h) ^
                                                  uint64_t(L.depth) << 40) | 1;
                std::lock_guard<std::mutex> lk(etc_mutex_);
                auto hit = etc_cache_.find(key);
                if (hit != etc_cache_.end() && hit->second.size() == L.out_bytes) {
                    memcpy(out, hit->second.data(), L.out_bytes);
                    p.cache_hits++;
                    continue;
                }
            }
            jobs.push_back(BcJob{p.kind, p.to, p.punch, dst, L.bx, L.brows, L.w, L.h, L.depth, out, L.out_bytes, key});
            keys.push_back(key);
        }
        p.raw.clear();
        p.raw.shrink_to_fit();  // the copy of the game memory is no longer needed
        ConvertJobs(jobs);
        if (p.to != BcTo::Decoded) {
            std::lock_guard<std::mutex> lk(etc_mutex_);
            for (size_t i = 0; i < jobs.size(); i++) {
                if (!keys[i]) continue;
                const BcJob& j = jobs[i];
                if (etc_cache_bytes_ + j.out_bytes > kEtcCacheLimit) {
                    etc_cache_.clear();
                    etc_cache_bytes_ = 0;
                }
                auto& e = etc_cache_[j.key];
                etc_cache_bytes_ += j.out_bytes - e.size();
                e.assign(j.out, j.out + j.out_bytes);
            }
        }
    }

    // Render thread: queues a BC texture for the background thread. The image is not drawn until PumpBcUploads
    // has recorded its copy (a re-upload keeps the old contents meanwhile).
    void EnqueueBcUpload(Image& img, const TexRef& t, const TextureLayout& layout, const FormatInfo& fi) {
        auto p = std::make_shared<PendingBc>();
        p->addr = t.addr;
        p->serial = img.serial = ++image_serial_;
        p->nvn_format = img.nvn_format;
        p->kind = img.bc;
        p->to = img.bc_to;
        p->punch = EtcHasPunchThrough(img.nvn_format);
        const uint8_t* base = reinterpret_cast<const uint8_t*>(t.addr);
        p->raw.assign(base, base + layout.total);
        size_t out = 0;
        for (int layer = 0; layer < img.layers; layer++)
            for (int lv = 0; lv < img.levels; lv++) {
                const LevelLayout& l = layout.levels[size_t(lv + img.mip_skip)];
                PendingLevel L;
                L.src_off = size_t(layer) * layout.layer_stride + l.offset;
                L.n = size_t(l.row_bytes) * l.rows * l.depth;
                L.lay = l;
                L.linear = layout.linear;
                L.w = std::max(1, l.width);
                L.h = std::max(1, l.height);
                L.depth = l.depth;
                L.bx = l.row_bytes / std::max(1, fi.bytes_per_block);
                L.brows = l.rows;
                L.out_bytes = ConvertedLevelBytes(img, L.w, L.h, L.depth);
                out = (out + 15) & ~size_t(15);
                L.out_off = out;
                out += L.out_bytes;
                VkBufferImageCopy r{};
                r.bufferOffset = L.out_off;
                r.imageSubresource = {img.aspect & ~VK_IMAGE_ASPECT_STENCIL_BIT ? img.aspect & ~VK_IMAGE_ASPECT_STENCIL_BIT
                                                                                 : img.aspect,
                                      uint32_t(lv), uint32_t(layer), 1};
                r.imageExtent = {uint32_t(L.w), uint32_t(img.type == VK_IMAGE_TYPE_1D ? 1 : L.h), uint32_t(L.depth)};
                p->regions.push_back(r);
                p->levels.push_back(L);
            }
        p->result_bytes = out;
        // Small conversions are done right here and recorded now: a texture that is queued is drawn with a white
        // stand-in until its copy lands, and those few frames read as flicker. Only big ones go to the thread.
        if (out <= kBcSyncBytes && StreamRemaining() >= out + 1024) {
            ConvertPending(*p);
            p->done.store(true, std::memory_order_release);
            Begin();
            EndRendering();
            const VkDeviceSize off = StreamAlloc(out, 16);
            memcpy(stream_ptr_ + off, p->result.data(), out);
            std::vector<VkBufferImageCopy> regions = p->regions;
            for (VkBufferImageCopy& r : regions) r.bufferOffset += off;
            Transition(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            Rec([regions = std::move(regions), buf = stream_, image = img.image](VkCommandBuffer cb) {
                vkCmdCopyBufferToImage(cb, buf, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(regions.size()),
                                       regions.data());
            });
            img.written = true;
            img.pending = false;
            uploads_++;
            bc_converted_levels_ += p->levels.size();
            bc_cache_hits_ += p->cache_hits;
            return;
        }
        img.pending = true;
        EnsureBcThread();
        {
            std::lock_guard<std::mutex> lk(bc_mutex_);
            bc_queue_.push_back(p);
        }
        bc_cv_.notify_one();
        bc_inflight_.push_back(p);
        if (++bc_queued_ <= 24)
            LOGI(kTag, "texture %dx%d format %d: converting in the background (%zu bytes)", img.width, img.height,
                 img.nvn_format, out);
    }

    // Render thread, once per frame: records the copies of converted textures, in the order they were queued,
    // at most kBcFrameBudget bytes per frame. A result whose texture was replaced or released is dropped.
    void PumpBcUploads() {
        if (bc_inflight_.empty()) return;
        size_t budget = kBcFrameBudget;
        bool ended = false;
        while (!bc_inflight_.empty() && bc_inflight_.front()->done.load(std::memory_order_acquire)) {
            std::shared_ptr<PendingBc> p = bc_inflight_.front();
            auto it = images_.find(p->addr);
            if (it == images_.end() || !it->second.pending || it->second.serial != p->serial) {
                bc_inflight_.pop_front();
                continue;
            }
            const size_t bytes = p->result_bytes;
            if (bytes > budget && budget < kBcFrameBudget) break;  // the rest waits for the next frame
            if (StreamRemaining() < bytes + 1024) break;           // the stream is reset next frame
            if (!ended) {
                EndRendering();
                ended = true;
            }
            Image& img = it->second;
            const VkDeviceSize off = StreamAlloc(bytes, 16);
            memcpy(stream_ptr_ + off, p->result.data(), bytes);
            std::vector<VkBufferImageCopy> regions = p->regions;
            for (VkBufferImageCopy& r : regions) r.bufferOffset += off;
            Transition(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            Rec([regions = std::move(regions), buf = stream_, image = img.image](VkCommandBuffer cb) {
                vkCmdCopyBufferToImage(cb, buf, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(regions.size()),
                                       regions.data());
            });
            img.written = true;
            img.pending = false;
            uploads_++;
            bc_converted_levels_ += p->levels.size();
            bc_cache_hits_ += p->cache_hits;
            budget = budget > bytes ? budget - bytes : 0;
            std::vector<uint8_t>().swap(p->result);  // free the CPU copy of the converted texels
            bc_inflight_.pop_front();
        }
    }

    // Fills a sampled texture from game memory: deswizzle each level/layer into the stream, then copy.
    void Upload(Image& img, const TexRef& t) {
        if (img.pending) return;  // already on its way (a re-upload while the first conversion runs)
        img.uploaded = true;
        img.cpu_source = true;
        img.sample_hash = SampleHash(t);
        img.checked_in_list = list_counter_;
        img.check_interval = 1;
        img.next_check_list = list_counter_ + 1;
        const TextureLayout layout = ComputeLayout(t.tex);
        if (!GuestRangeValid(t.addr, layout.total)) {
            LogOnce("upload-range", "texture %dx%d format %d: storage outside known memory, not uploaded",
                    t.tex.width, t.tex.height, t.tex.format);
            return;
        }
        const FormatInfo fi = GetFormatInfo(t.tex.format);
        size_t needed = 0;
        for (int layer = 0; layer < img.layers; layer++)
            for (int lv = 0; lv < img.levels; lv++) {
                const LevelLayout& l = layout.levels[size_t(lv + img.mip_skip)];
                needed += img.bc != BcKind::None ? ConvertedLevelBytes(img, std::max(1, l.width), std::max(1, l.height),
                                                                       l.depth) + 16
                                                 : size_t(l.row_bytes) * l.rows * l.depth + 16;
            }
        if (needed > kStreamSize - kMaxUniformRange) {
            LogOnce("upload-size", "texture too large to upload in one go (%zu bytes)", needed);
            return;
        }
        if (img.bc != BcKind::None) {
            EnqueueBcUpload(img, t, layout, fi);
            return;
        }
        Begin();
        EndRendering();
        if (StreamRemaining() < needed + 1024) Flush();
        std::vector<VkBufferImageCopy> regions;
        const uint8_t* base = reinterpret_cast<const uint8_t*>(t.addr);
        size_t scratch_used = 0;
        (void)scratch_used;
        for (int layer = 0; layer < img.layers; layer++) {
            for (int lv = 0; lv < img.levels; lv++) {
                const LevelLayout& l = layout.levels[size_t(lv + img.mip_skip)];
                const size_t n = size_t(l.row_bytes) * l.rows * l.depth;
                const uint8_t* src = base + size_t(layer) * layout.layer_stride + l.offset;
                VkDeviceSize off = StreamAlloc(n, 16);
                if (layout.linear)
                    memcpy(stream_ptr_ + off, src, n);
                else
                    DeswizzleLevel(stream_ptr_ + off, src, l);
                VkBufferImageCopy r{};
                r.bufferOffset = off;
                r.bufferRowLength = uint32_t(l.row_bytes / fi.bytes_per_block * fi.block_w);
                r.bufferImageHeight = uint32_t(l.rows * fi.block_h);
                r.imageSubresource = {img.aspect & ~VK_IMAGE_ASPECT_STENCIL_BIT ? img.aspect & ~VK_IMAGE_ASPECT_STENCIL_BIT
                                                                                 : img.aspect,
                                      uint32_t(lv), uint32_t(layer), 1};
                r.imageExtent = {uint32_t(l.width), uint32_t(img.type == VK_IMAGE_TYPE_1D ? 1 : l.height),
                                 uint32_t(l.depth)};
                regions.push_back(r);
            }
        }
        Transition(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        Rec([regions = std::move(regions), buf = stream_, image = img.image](VkCommandBuffer cb) {
            vkCmdCopyBufferToImage(cb, buf, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(regions.size()),
                                   regions.data());
        });
        img.written = true;
        uploads_++;
        if (img.width == 256 && img.height == 1 && fi.bytes_per_block == 16 && GuestRangeValid(t.addr, 4096)) {
            // Diagnostics (brightness): the final pass's colour lookup table.
            const float* lut = reinterpret_cast<const float*>(t.addr);
            static int logged = 0;
            if (logged++ < 40)
                LOGI(kTag, "lookup table 256x1 @%llx uploaded: [0] %.4f %.4f %.4f | [64] %.4f | [128] %.4f | [255] %.4f %.4f %.4f",
                     (unsigned long long)t.addr, lut[0], lut[1], lut[2], lut[64 * 4], lut[128 * 4], lut[255 * 4],
                     lut[255 * 4 + 1], lut[255 * 4 + 2]);
        }
        if (uploads_ <= 24)
            LOGI(kTag, "uploaded texture %dx%d format %d (%d levels, %d layers, %zu bytes)", img.width, img.height,
                 img.nvn_format, img.levels, img.layers, needed);
    }

    // The Vulkan format a view with NVN format `view_nvn` uses on `img` (UNDEFINED: the image's own).
    VkFormat ViewFormat(const Image& img, int view_nvn) {
        if (!view_nvn || view_nvn == img.nvn_format) return VK_FORMAT_UNDEFINED;
        if (img.bc != BcKind::None && BcKindOf(view_nvn) != BcKind::None) {  // converted: compare converted formats
            const VkFormat f = ConvertedFormat(view_nvn, img.bc_to);
            if (f == img.vk_format) return VK_FORMAT_UNDEFINED;
            if (f == img.twin && f != VK_FORMAT_UNDEFINED) return f;
        }
        for (VkFormat f : ImageCandidates(view_nvn)) {
            if (f == img.vk_format) return VK_FORMAT_UNDEFINED;
            if (f == img.twin && f != VK_FORMAT_UNDEFINED) {
                if (view_formats_logged_.insert(uint64_t(img.nvn_format) << 32 | uint32_t(view_nvn)).second)
                    LOGI(kTag, "texture of NVN format %d viewed as NVN format %d (Vulkan %d)", img.nvn_format, view_nvn,
                         int(f));
                return f;
            }
        }
        if (view_formats_logged_.insert(uint64_t(img.nvn_format) << 32 | uint32_t(view_nvn) | 1ull << 63).second)
            LOGW(kTag, "texture of NVN format %d viewed as NVN format %d: not supported, its own format is used",
                 img.nvn_format, view_nvn);
        return VK_FORMAT_UNDEFINED;
    }
    std::set<uint64_t> view_formats_logged_;
    VkImageView View(Image& img, VkImageViewType type, uint32_t base_layer, uint32_t layers, uint32_t levels,
                     VkImageAspectFlags aspect, const uint8_t swz[4], uint32_t base_level = 0,
                     VkFormat format = VK_FORMAT_UNDEFINED) {
        Key64 k;
        const uint32_t parts[11] = {uint32_t(type), base_layer, layers, levels, aspect, swz[0], swz[1], swz[2], swz[3],
                                    base_level, uint32_t(format)};
        k.Add(parts, sizeof(parts));
        auto fast = img.fast_views.find(k.h);
        if (fast != img.fast_views.end()) return fast->second;
        std::vector<uint32_t> key(parts, parts + 11);
        auto it = img.views.find(key);
        if (it != img.views.end()) return it->second;
        if (format != VK_FORMAT_UNDEFINED && format != img.vk_format && !img.mutable_fmt && !MakeMutable(img))
            format = VK_FORMAT_UNDEFINED;  // could not re-create: its own format
        VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ci.image = img.image;
        ci.viewType = type;
        ci.format = format != VK_FORMAT_UNDEFINED ? format : img.vk_format;
        ci.components = {Swizzle(swz[0]), Swizzle(swz[1]), Swizzle(swz[2]), Swizzle(swz[3])};
        ci.subresourceRange = {aspect, base_level, levels, base_layer, layers};
        VkImageView v = VK_NULL_HANDLE;
        if (vkCreateImageView(device_, &ci, nullptr, &v) != VK_SUCCESS) {
            LogOnce("view", "vkCreateImageView failed (format %d, type %d)", img.nvn_format, int(type));
            v = VK_NULL_HANDLE;
        }
        img.views[key] = v;
        img.fast_views[k.h] = v;
        return v;
    }
    // The level/layer a render target view selects (clamped to the image).
    static uint32_t SubLevel(const Image& img, int level) { return uint32_t(std::clamp(level, 0, img.levels - 1)); }
    static uint32_t SubLayer(const Image& img, int layer) {
        return uint32_t(std::clamp(layer, 0, (img.type == VK_IMAGE_TYPE_3D ? img.depth : img.layers) - 1));
    }
    VkImageView AttachmentView(Image& img, int layer = 0, int level = 0, int view_nvn = 0) {
        static const uint8_t kIdentity[4] = {2, 3, 4, 5};
        return View(img, img.type == VK_IMAGE_TYPE_1D ? VK_IMAGE_VIEW_TYPE_1D : VK_IMAGE_VIEW_TYPE_2D,
                    img.type == VK_IMAGE_TYPE_3D ? 0 : SubLayer(img, layer), 1, 1, img.aspect, kIdentity, SubLevel(img, level),
                    ViewFormat(img, view_nvn));
    }
    // View matching what the shader declared for the slot.
    VkImageView SampledView(Image& img, shader::TexDim dim, bool array, const uint8_t swz[4], const TexRef* ref = nullptr) {
        VkImageViewType type;
        uint32_t layers = 1;
        switch (dim) {
            case shader::TexDim::D1: type = array ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D; break;
            case shader::TexDim::D3: type = VK_IMAGE_VIEW_TYPE_3D; break;
            case shader::TexDim::Cube:
                type = array ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
                layers = array ? uint32_t(img.layers / 6 * 6) : 6;
                break;
            default: type = array ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D; break;
        }
        // A texture view registered with the texture picks the first level/layer (and counts).
        const uint32_t base_level = ref ? SubLevel(img, ref->level - img.mip_skip) : 0;  // mip_skip: one level less
        uint32_t levels = uint32_t(img.levels) - base_level;
        if (ref && ref->view_levels > 0) levels = std::min<uint32_t>(levels, uint32_t(ref->view_levels));
        uint32_t base_layer = 0;
        if (ref && img.type != VK_IMAGE_TYPE_3D && dim != shader::TexDim::Cube) base_layer = SubLayer(img, ref->layer);
        if (array && dim != shader::TexDim::Cube) {
            layers = uint32_t(img.layers) - base_layer;
            if (ref && ref->view_layers > 0) layers = std::min<uint32_t>(layers, uint32_t(ref->view_layers));
        }
        const bool type_ok = (dim == shader::TexDim::D3) == (img.type == VK_IMAGE_TYPE_3D) &&
                             (dim == shader::TexDim::D1) == (img.type == VK_IMAGE_TYPE_1D) &&
                             (dim != shader::TexDim::Cube || img.layers >= 6) && base_layer + layers <= uint32_t(img.layers);
        if (!type_ok) return VK_NULL_HANDLE;
        VkImageAspectFlags aspect = img.aspect;
        if (aspect & VK_IMAGE_ASPECT_DEPTH_BIT) aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
        return View(img, type, base_layer, layers, levels, aspect, swz, base_level,
                    ref ? ViewFormat(img, ref->view_format) : VK_FORMAT_UNDEFINED);
    }

    // Formats the GPU cannot filter linearly (32-bit float on Adreno, unlike the Switch): sampling them with a
    // linear sampler is undefined in Vulkan and reads zeros here. Such textures get a nearest sampler.
    bool Filterable(const Image& img) {
        const bool ok = img.features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if (!ok && unfilterable_logged_.insert(img.nvn_format).second)
            LOGI(kTag, "NVN format %d (Vulkan %d) cannot be filtered linearly on this GPU: sampled with nearest filtering",
                 img.nvn_format, img.vk_format);
        return ok;
    }
    VkSampler Sampler(const SamplerObj& s, bool shadow, bool filterable = true) {
        Key64 fk;
        fk.Add(&s, sizeof(SamplerObj));
        fk.Add(shadow);
        fk.Add(filterable);
        auto fast = samplers_fast_.find(fk.h);
        if (fast != samplers_fast_.end()) return fast->second;
        std::vector<uint32_t> key(sizeof(SamplerObj) / 4 + 2);
        memcpy(key.data(), &s, sizeof(SamplerObj));
        key[key.size() - 2] = shadow;
        key.back() = filterable;
        auto it = samplers_.find(key);
        if (it != samplers_.end()) return it->second;
        VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        ci.magFilter = s.mag_filter == 0 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        ci.minFilter = (s.min_filter == 0 || s.min_filter == 2 || s.min_filter == 4) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        ci.mipmapMode = (s.min_filter == 4 || s.min_filter == 5) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        ci.addressModeU = AddressMode(s.wrap[0]);
        ci.addressModeV = AddressMode(s.wrap[1]);
        ci.addressModeW = AddressMode(s.wrap[2]);
        if (!filterable) {
            ci.magFilter = VK_FILTER_NEAREST;
            ci.minFilter = VK_FILTER_NEAREST;
            ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        }
        ci.mipLodBias = s.lod_bias;
        ci.minLod = std::max(0.0f, s.lod_min);
        ci.maxLod = (s.min_filter <= 1) ? 0.25f : std::max(ci.minLod, std::min(s.lod_max, 16.0f));
        if (filterable && features_.samplerAnisotropy && s.max_anisotropy > 1.0f) {
            ci.anisotropyEnable = VK_TRUE;
            ci.maxAnisotropy = std::min(s.max_anisotropy, props_.limits.maxSamplerAnisotropy);
            // `max_anisotropy` (settings): a lower limit than the game's, for slower GPUs (0 = the game's).
            const int cap = dsr::GetRuntime().config.max_anisotropy;
            if (cap > 0) ci.maxAnisotropy = std::min(ci.maxAnisotropy, float(std::max(cap, 1)));
            LogOnce(("aniso" + std::to_string(int(s.max_anisotropy))).c_str(),
                    "sampler with anisotropic filtering: the game asks for %.0fx, used %.0fx", s.max_anisotropy,
                    ci.maxAnisotropy);
            if (ci.maxAnisotropy <= 1.0f) ci.anisotropyEnable = VK_FALSE;
        }
        if (shadow) {
            ci.compareEnable = VK_TRUE;
            // The game's shadow lookups (GREATER) give the shadow amount: the shaders compute light as 1 - result *
            // strength, so 1 means shadowed (surface farther than the shadow map's texel), as in Vulkan.
            ci.compareOp = s.compare_mode ? CompareOp(s.compare_func) : VK_COMPARE_OP_LESS_OR_EQUAL;
            switch (dsr::GetRuntime().config.shadow_compare) {  // diagnostics (settings shadow_compare)
                case 1: ci.compareOp = s.compare_mode ? SwapCompareOperands(CompareOp(s.compare_func)) : VK_COMPARE_OP_GREATER_OR_EQUAL; break;
                case 2: ci.compareOp = VK_COMPARE_OP_ALWAYS; break;
                case 3: ci.compareOp = VK_COMPARE_OP_NEVER; break;
                default: break;
            }
        }
        ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        VkSampler smp = VK_NULL_HANDLE;
        vkCreateSampler(device_, &ci, nullptr, &smp);
        samplers_[key] = smp;
        samplers_fast_[fk.h] = smp;
        return smp;
    }

    // Viewport, scissor, blend constants and stencil values of a draw.
    struct DynState {
        VkViewport vp;
        VkRect2D sc;
        float blend[4];
        uint32_t stencil[6];
        float bias[3];  // depth bias: constant (units), clamp, slope (factor)
    };
    // One draw for the recorder: everything it needs, worked out on the render thread.
    struct DrawPacket {
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkDescriptorSetLayout set_layouts[2] = {};
        DynState dyn{};
        float push[4] = {};
        VkBuffer index_buf = VK_NULL_HANDLE;
        VkDeviceSize index_off = 0;
        VkIndexType index_type = VK_INDEX_TYPE_UINT32;
        bool indexed = false;
        uint32_t count = 0, instances = 0, first_vertex = 0, base_instance = 0;
        int32_t vertex_offset = 0;
        uint32_t query = UINT32_MAX;  // occlusion query index (captures)
        uint16_t n_vb = 0, n_tex = 0, n_ubo = 0;
    };
    // ======================================================================================================
    // recorder thread
    // ======================================================================================================
    // The render thread works out each draw (shaders, textures, uniform and vertex data, pipeline, render pass)
    // and queues the result; the recorder thread builds the descriptor sets and records the Vulkan commands, so
    // the two halves of a draw run on two cores. Commands keep their order: every command goes through Rec(), or
    // through SyncCmd() which first waits for the recorder. The recorder alone uses the command buffer, the
    // descriptor pools and caches and the bound-state trackers while it runs; the render thread touches them only
    // after Drain() (submits, resets, statistics). Diagnostics captures record everything on the render thread.
    using OpRun = void (*)(VulkanBackend*, uint8_t*);
    using OpDestroy = void (*)(uint8_t*);
    struct OpHead {
        OpRun run;
        OpDestroy destroy;
        size_t bytes;  // header and payload
    };
    struct OpBatch {
        std::unique_ptr<uint8_t[]> buf;
        size_t cap = 0, used = 0;
        uint32_t ops = 0;
    };
    static constexpr size_t kHeadBytes = (sizeof(OpHead) + 15) & ~size_t(15);
    static constexpr size_t kBatchBytes = 64 << 10;
    OpBatch* rec_batch_ = nullptr;     // being filled (render thread)
    std::deque<OpBatch*> rec_queue_;   // under rec_mutex_
    std::vector<OpBatch*> rec_free_;   // under rec_mutex_
    bool rec_sleeping_ = false;        // under rec_mutex_: the recorder waits on rec_cv_
    std::atomic<int> rec_queued_{0};   // batches in rec_queue_
    std::atomic<bool> rec_busy_{false};
    std::mutex rec_mutex_;
    std::condition_variable rec_cv_;
    bool rec_started_ = false;
    std::atomic<uint64_t> rec_ns_{0};  // recorder busy time
    double drain_ms_ = 0;              // render thread waiting for the recorder
    uint64_t rec_batches_ = 0;

    bool Async() const { return rec_started_ && !capture_active_; }
    void StartRecorder() {
        if (rec_started_ || !dsr::GetRuntime().config.record_thread) return;
        rec_started_ = true;
        std::thread([this] {
            pthread_setname_np(pthread_self(), "dsr-record");
            profiler::RegisterCurrentThread("dsr-record");
            if (dsr::GetRuntime().config.fast_cores)
                dsr::platform::PreferFastCores("recorder thread");  // also restored if the system resets it
            else
                KeepOffSlowCores();
            for (;;) {
                // Batches arrive every few hundred microseconds while the render thread produces: wait for the
                // next one awake for a moment, then sleep (it stays on the performance cores either way; spinning
                // longer kept a core busy for nothing: ~6 s of CPU per 10 s).
                const auto spin_until = std::chrono::steady_clock::now() + std::chrono::microseconds(250);
                for (int i = 0; rec_queued_.load(std::memory_order_acquire) == 0; i++) {
                    if ((i & 255) == 255 && std::chrono::steady_clock::now() > spin_until) break;
                    CpuRelax();
                }
                OpBatch* b;
                {
                    std::unique_lock<std::mutex> lk(rec_mutex_);
                    if (rec_queue_.empty()) {
                        rec_sleeping_ = true;
                        rec_cv_.wait(lk, [&] { return !rec_queue_.empty(); });
                        rec_sleeping_ = false;
                    }
                    b = rec_queue_.front();
                    rec_queue_.pop_front();
                    rec_busy_.store(true, std::memory_order_relaxed);
                    rec_queued_.fetch_sub(1, std::memory_order_release);  // publishes rec_busy_ = true
                }
                const auto t0 = std::chrono::steady_clock::now();
                RunBatch(*b);
                rec_ns_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now() - t0).count());
                {
                    std::lock_guard<std::mutex> lk(rec_mutex_);
                    rec_free_.push_back(b);
                }
                rec_busy_.store(false, std::memory_order_release);  // Drain() sees everything recorded so far
            }
        }).detach();
        LOGI(kTag, "recorder thread on: draws are prepared on the render thread and recorded on a second thread");
    }
    void RunBatch(OpBatch& b) {
        for (size_t at = 0; at < b.used;) {
            auto* h = reinterpret_cast<OpHead*>(b.buf.get() + at);
            uint8_t* p = b.buf.get() + at + kHeadBytes;
            h->run(this, p);
            if (h->destroy) h->destroy(p);
            at += h->bytes;
        }
        b.used = 0;
        b.ops = 0;
    }
    // Space for one queued command (render thread).
    uint8_t* RecAlloc(size_t payload, OpRun run, OpDestroy destroy) {
        const size_t total = kHeadBytes + ((payload + 15) & ~size_t(15));
        if (rec_batch_ && rec_batch_->used + total > rec_batch_->cap) Publish();
        if (!rec_batch_) {
            {
                std::lock_guard<std::mutex> lk(rec_mutex_);
                if (!rec_free_.empty()) {
                    rec_batch_ = rec_free_.back();
                    rec_free_.pop_back();
                }
            }
            if (!rec_batch_) rec_batch_ = new OpBatch;
            const size_t cap = std::max(total, kBatchBytes);
            if (rec_batch_->cap < cap) {
                rec_batch_->buf.reset(new uint8_t[cap]);  // 16-byte aligned (operator new[] on arm64)
                rec_batch_->cap = cap;
            }
        }
        uint8_t* p = rec_batch_->buf.get() + rec_batch_->used;
        *reinterpret_cast<OpHead*>(p) = {run, destroy, total};
        rec_batch_->used += total;
        rec_batch_->ops++;
        return p + kHeadBytes;
    }
    // Hands the batch being filled to the recorder.
    void Publish() {
        if (!rec_batch_) return;
        if (!rec_batch_->ops) return;
        bool wake;
        {
            std::lock_guard<std::mutex> lk(rec_mutex_);
            rec_queue_.push_back(rec_batch_);
            rec_queued_.fetch_add(1, std::memory_order_release);
            wake = rec_sleeping_;
        }
        rec_batch_ = nullptr;
        rec_batches_++;
        if (wake) rec_cv_.notify_one();
    }
    // Publishes small batches so the recorder starts early (about 32 draws' worth).
    void MaybePublish() {
        if (rec_batch_ && (rec_batch_->used >= (24u << 10) || rec_batch_->ops >= 96)) Publish();
    }
    // Waits until everything queued is recorded (render thread).
    void Drain() {
        if (!rec_started_) return;
        Publish();
        auto idle = [&] {
            return rec_queued_.load(std::memory_order_acquire) == 0 && !rec_busy_.load(std::memory_order_acquire);
        };
        if (idle()) return;
        // The wait is short (the end of the recorder's last batch): spin, so neither thread is put to sleep.
        MsTimer t(drain_ms_);
        for (int i = 0; !idle(); i++) {
            if ((i & 1023) == 1023) std::this_thread::yield();
            else CpuRelax();
        }
    }
    static void CpuRelax() {
#if defined(__aarch64__)
        asm volatile("yield" ::: "memory");
#endif
    }
    // The recorder runs on the performance cores: on the efficiency cores the driver's recording is several
    // times slower, and the render thread would wait for it at every submit.
    static void KeepOffSlowCores() {
#if defined(__linux__)
        long khz[32] = {};
        long lowest = 0, highest = 0;
        for (int cpu = 0; cpu < 32; cpu++) {
            char path[96];
            snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
            if (FILE* f = fopen(path, "r")) {
                if (fscanf(f, "%ld", &khz[cpu]) != 1) khz[cpu] = 0;
                fclose(f);
            }
            if (khz[cpu] > 0 && (!lowest || khz[cpu] < lowest)) lowest = khz[cpu];
            highest = std::max(highest, khz[cpu]);
        }
        if (!lowest || lowest == highest) return;  // one kind of core
        cpu_set_t set;
        CPU_ZERO(&set);
        std::string list;
        for (int cpu = 0; cpu < 32; cpu++)
            if (khz[cpu] > lowest) {
                CPU_SET(cpu, &set);
                list += " " + std::to_string(cpu);
            }
        const int r = sched_setaffinity(0, sizeof(set), &set);
        LOGI(kTag, "recorder thread on CPUs%s: %s", list.c_str(), r == 0 ? "ok" : "refused");
#endif
    }
    // Records `f(command buffer)`: on the recorder, or here when it is off.
    template <class F>
    void Rec(F&& f) {
        using T = std::decay_t<F>;
        static_assert(alignof(T) <= 16, "queued command over-aligned");
        if (!Async()) {
            Drain();
            f(cmd_);
            return;
        }
        OpDestroy destroy = nullptr;
        if constexpr (!std::is_trivially_destructible_v<T>) destroy = [](uint8_t* q) { reinterpret_cast<T*>(q)->~T(); };
        uint8_t* p = RecAlloc(sizeof(T), [](VulkanBackend* self, uint8_t* q) { (*reinterpret_cast<T*>(q))(self->cmd_); },
                              destroy);
        new (p) T(std::forward<F>(f));
        MaybePublish();
    }
    // The command buffer, for recording here directly (diagnostics): the recorder is idle.
    VkCommandBuffer SyncCmd() {
        Drain();
        return cmd_;
    }

    // ======================================================================================================
    // command buffer helpers
    // ======================================================================================================
    // Everything recorded state-skipping relies on starts over with a new command buffer.
    void InvalidateBoundState() {
        bound_pipeline_ = VK_NULL_HANDLE;
        bound_layout_ = VK_NULL_HANDLE;
        bound_sets_[0] = bound_sets_[1] = VK_NULL_HANDLE;
        dyn_valid_ = false;
        for (auto& b : bound_vb_) b = {VK_NULL_HANDLE, 0};
        bound_ib_ = {VK_NULL_HANDLE, 0};
        bound_ib_type_ = VK_INDEX_TYPE_MAX_ENUM;
        push_valid_ = false;
    }
    // Starts the current slot's command buffer (on the recorder: cmd_ is the recorder's).
    void BeginCommands() {
        Rec([this, i = slot_](VkCommandBuffer) {
            cmd_ = slots_[i].cmd;
            InvalidateBoundState();
            vkResetCommandBuffer(cmd_, 0);
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(cmd_, &bi);
            if (VkQueryPool ts = slots_[i].ts) {
                vkCmdResetQueryPool(cmd_, ts, 0, 2);
                vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, ts, 0);
            }
        });
    }
    struct Slot;
    // GPU time of a finished submission (its fence has signalled), added to the 10 s statistics.
    double gpu_busy_ms_ = 0;
    void CollectGpuTime(Slot& sl) {
        if (!sl.ts || !sl.ts_written) return;
        sl.ts_written = false;
        uint64_t t[2];
        if (vkGetQueryPoolResults(device_, sl.ts, 0, 2, sizeof(t), t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t[1] > t[0]) {
            const double ms = double(t[1] - t[0]) * props_.limits.timestampPeriod / 1e6;
            gpu_busy_ms_ += ms;
            dsr::ReportGpuFrameTime(ms);  // fps = auto weighs it against the frame period
        }
    }

    // Submits the current slot's commands and moves to the other slot. The CPU waits only if that slot's
    // previous submission is still running (so CPU recording and GPU execution overlap by one submission).
    // The recorder ends the buffer and submits it once it has recorded everything queued before; the render
    // thread moves on to the next slot without waiting (it waited ~1.5 ms a frame for the recorder's tail).
    void SubmitAndWait(VkSemaphore wait, VkSemaphore signal) {
        Slot& sl = slots_[slot_];
        sl.ts_written = sl.ts != VK_NULL_HANDLE;
        sl.pending = true;
        sl.submits_queued++;
        sl.seq = ++submit_seq_;
        Rec([this, i = slot_, wait, signal](VkCommandBuffer) { SubmitSlot(i, wait, signal); });
        Publish();  // the GPU gets it as soon as the recorder is done
        NextSlot();
    }
    // Recorder: ends slot i's command buffer and submits it (its fence always gets signalled).
    void SubmitSlot(int i, VkSemaphore wait, VkSemaphore signal) {
        Slot& sl = slots_[i];
        if (sl.ts) vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, sl.ts, 1);
        vkEndCommandBuffer(cmd_);
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd_;
        if (wait) {
            si.waitSemaphoreCount = 1;
            si.pWaitSemaphores = &wait;
            si.pWaitDstStageMask = &stage;
        }
        if (signal) {
            si.signalSemaphoreCount = 1;
            si.pSignalSemaphores = &signal;
        }
        VkResult r = vkQueueSubmit(queue_, 1, &si, sl.fence);
        if (r != VK_SUCCESS) {
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true)) LOGW(kTag, "vkQueueSubmit failed: %d", r);
            vkQueueSubmit(queue_, 0, nullptr, sl.fence);  // the render thread waits on this fence
        }
        sl.submits_done.fetch_add(1, std::memory_order_release);
    }
    // A slot's fence may be waited on once the recorder has submitted it (a fence must not be used by two
    // threads at once).
    void WaitSubmitted(Slot& sl) {
        if (sl.submits_done.load(std::memory_order_acquire) == sl.submits_queued) return;
        Publish();
        MsTimer t(drain_ms_);
        for (int i = 0; sl.submits_done.load(std::memory_order_acquire) != sl.submits_queued; i++) {
            if ((i & 1023) == 1023) std::this_thread::yield();
            else CpuRelax();
        }
    }
    void UseSlot(int i) {
        slot_ = i;
        stream_ = slots_[i].stream;
        stream_memory_ = slots_[i].mem;
        stream_ptr_ = slots_[i].ptr;
    }
    // Writes the finished readbacks of a slot to game memory (its fence has signalled).
    void DeliverReadbacks(Slot& sl) {
        for (const Readback& rb : sl.readbacks) {
            if (rb.report >= 0) {
                if (!GuestRangeValid(rb.dst, rb.report ? 16 : 4)) continue;
                if (rb.report == 0) {
                    memcpy(reinterpret_cast<void*>(rb.dst), &rb.value, 4);
                } else if (rb.value == 0) {
                    // Timestamp counter. NVNcounterData: the counter, then a timestamp (GPU ticks, 19.2 MHz like
                    // the CPU's). Other counters (samples passed, ...) are left alone, as before.
                    const int64_t ticks = hle::os::NowTicks();
                    const uint64_t data[2] = {uint64_t(ticks), uint64_t(ticks)};
                    memcpy(reinterpret_cast<void*>(rb.dst), data, 16);
                }
                reports_done_++;
                continue;
            }
            if (rb.texture) {
                const TextureLayout layout = ComputeLayout(rb.tex);
                if (layout.levels.empty() || !GuestRangeValid(rb.dst, layout.levels[0].offset + layout.levels[0].size))
                    continue;
                WriteLevelRegion(reinterpret_cast<uint8_t*>(rb.dst) + layout.levels[0].offset, layout.levels[0],
                                 layout.linear, 0, 0, 0, uint32_t(rb.row), uint32_t(rb.rows), 1, sl.ptr + rb.off, rb.row,
                                 rb.row * rb.rows);
                rt_writebacks_++;
                if (rb.tex.format == 10 && rb.row * rb.rows <= 4096) {  // R32F luminance / exposure (diagnostics)
                    static std::chrono::steady_clock::time_point last;
                    static std::set<uint64_t> seen;
                    const auto now = std::chrono::steady_clock::now();
                    if (seen.insert(rb.dst).second || now - last > std::chrono::seconds(10)) {
                        last = now;
                        const float* f = reinterpret_cast<const float*>(sl.ptr + rb.off);
                        LOGI(kTag, "written back %zux%zu R32F @%llx: texel 0 = %.4f, texel 1 = %.4f",
                             rb.row / 4, rb.rows, (unsigned long long)rb.dst, f[0], f[1]);
                    }
                }
                continue;
            }
            const size_t span = rb.dst_image_stride * (rb.depth - 1) + rb.dst_stride * (rb.rows - 1) + rb.row;
            if (!GuestRangeValid(rb.dst, span)) continue;
            uint8_t* dst = reinterpret_cast<uint8_t*>(rb.dst);
            const uint8_t* src = sl.ptr + rb.off;
            for (size_t z = 0; z < rb.depth; z++)
                for (size_t y = 0; y < rb.rows; y++)
                    memcpy(dst + z * rb.dst_image_stride + y * rb.dst_stride, src + (z * rb.rows + y) * rb.row, rb.row);
            readbacks_done_++;
        }
        sl.readbacks.clear();
    }
    // Slots whose work has finished deliver their readbacks without waiting (called once per list).
    void PollReadbacks() {
        for (Slot& sl : slots_)
            if (sl.pending && !sl.readbacks.empty() &&
                sl.submits_done.load(std::memory_order_acquire) == sl.submits_queued &&
                vkGetFenceStatus(device_, sl.fence) == VK_SUCCESS)
                DeliverReadbacks(sl);
    }
    void NextSlot() {
        const int n = (slot_ + 1) % kSlots;
        Slot& sl = slots_[n];
        if (sl.pending) {
            WaitSubmitted(sl);
            MsTimer wait_timer(wait_ms_);
            VkResult r = vkWaitForFences(device_, 1, &sl.fence, VK_TRUE, 5000000000ull);
            if (r != VK_SUCCESS) LogOnce("fence", "GPU did not finish within 5 s (%d)", r);
            DeliverReadbacks(sl);
            CollectGpuTime(sl);
            vkResetFences(device_, 1, &sl.fence);
            sl.pending = false;
            completed_seq_ = std::max(completed_seq_, sl.seq);  // one queue: earlier submissions are done too
            ReapRetired();
        }
        // `hole_test` 128 (square holes): the CPU waits for the GPU to finish everything before it writes into the
        // next slot, so nothing the GPU may still read can change under it.
        if (dsr::GetRuntime().config.hole_test & 128) WaitGpuIdle();
        UseSlot(n);
        ResetTransient();
    }
    // Waits for all submitted work (CPU readback of GPU results).
    void WaitGpuIdle() {
        Drain();
        MsTimer wait_timer(wait_ms_);
        vkQueueWaitIdle(queue_);
        for (Slot& sl : slots_)
            if (sl.pending) {
                DeliverReadbacks(sl);
                CollectGpuTime(sl);
                vkResetFences(device_, 1, &sl.fence);
                sl.pending = false;
            }
        completed_seq_ = submit_seq_;
        ReapRetired();
    }

    // Stages that use an image in `layout` (settings mali_barriers, from PR #13). A layout transition then holds back
    // only these stages of the commands after it, not ALL_COMMANDS: on a tile-based GPU (Mali) a barrier whose
    // destination includes the vertex stage makes the next pass's vertex/tiler work wait for the previous pass's
    // fragment work, which otherwise overlap. The source stays ALL_COMMANDS: every earlier writer is waited for.
    static VkPipelineStageFlags StagesUsing(VkImageLayout layout) {
        switch (layout) {
            case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
                return VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return VK_PIPELINE_STAGE_TRANSFER_BIT;
            case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
                return VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            default: return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        }
    }
    bool mali_barriers_ = false;  // settings mali_barriers
    uint64_t vertex_read_barriers_ = 0;
    void RawBarrier(VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                    uint32_t levels = 1, uint32_t layers = 1) {
        barriers_++;
        const VkPipelineStageFlags dst_stages = mali_barriers_ ? StagesUsing(to) : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {aspect, 0, levels, 0, layers};
        Rec([b, dst_stages](VkCommandBuffer cb) {
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, dst_stages, 0, 0, nullptr, 0, nullptr, 1, &b);
        });
    }
    void Transition(Image& img, VkImageLayout to) {
        if (img.clear_pending) FlushClear(img);
        if (img.layout == to) return;
        RawBarrier(img.image, img.aspect, img.layout, to, uint32_t(img.levels), uint32_t(img.layers));
        img.layout = to;
        img.attach_unsynced = false;
        img.frag_only_sync = false;  // the barrier above waited for every earlier write
    }
    // mali_barriers: a colour target the pass wrote becomes readable by fragment shaders as soon as the pass ends (a
    // barrier outside the pass), so a later draw that samples it needs no pass break to make it readable. Only
    // fragment shaders: fragment -> vertex dependencies are what stop a tile-based GPU from running the next pass's
    // vertex work while this pass shades; a vertex shader sampling a render target is rare (VertexReadBarrier).
    void PassEndBarrier(Image& img) {
        barriers_++;
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img.image;
        b.subresourceRange = {img.aspect, 0, uint32_t(img.levels), 0, uint32_t(img.layers)};
        Rec([b](VkCommandBuffer cb) {
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                                 0, nullptr, 0, nullptr, 1, &b);
        });
        img.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        img.attach_unsynced = false;
        img.frag_only_sync = true;
    }
    // Outside a render pass: lets vertex shaders read a render target PassEndBarrier released to fragment shaders only.
    void VertexReadBarrier(Image& img) {
        barriers_++;
        vertex_read_barriers_++;
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img.image;
        b.subresourceRange = {img.aspect, 0, uint32_t(img.levels), 0, uint32_t(img.layers)};
        Rec([b](VkCommandBuffer cb) {
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &b);
        });
        img.frag_only_sync = false;
    }

    // ======================================================================================================
    // clears / copies
    // ======================================================================================================
    // Performs a deferred clear (outside a render pass).
    void FlushClear(Image& img) {
        img.clear_pending = false;
        EndRendering();
        const VkImageLayout dst = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        if (img.layout != dst) {
            RawBarrier(img.image, img.aspect, img.layout, dst, uint32_t(img.levels), uint32_t(img.layers));
            img.layout = dst;
            img.attach_unsynced = false;
        }
        VkImageSubresourceRange range{img.clear_aspect, SubLevel(img, img.clear_level), 1,
                                      img.type == VK_IMAGE_TYPE_3D ? 0 : SubLayer(img, img.clear_layer), 1};
        Rec([range, image = img.image, v = img.clear_value,
             color = (img.clear_aspect & VK_IMAGE_ASPECT_COLOR_BIT) != 0](VkCommandBuffer cb) {
            if (color)
                vkCmdClearColorImage(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v.color, 1, &range);
            else
                vkCmdClearDepthStencilImage(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v.depthStencil, 1, &range);
        });
        clears_flushed_++;
    }
    uint64_t clears_deferred_ = 0, clears_flushed_ = 0, clears_in_pass_ = 0, pass_barriers_ = 0, lrz_touches_ = 0,
             shader_depth_draws_ = 0;
    // The render pass currently open: its colour images (and subresources) and depth image.
    Image* pass_colors_[8] = {};
    std::pair<int, int> pass_color_sub_[8] = {};
    size_t pass_color_count_ = 0;
    Image* pass_depth_ = nullptr;
    VkExtent2D pass_extent_{};
    bool InOpenPass(const Image* img) const {
        if (!rendering_) return false;
        if (pass_depth_ == img) return true;
        for (size_t i = 0; i < pass_color_count_; i++)
            if (pass_colors_[i] == img) return true;
        return false;
    }
    void ClearColorImage(Image& img, const uint32_t color[4], uint8_t kind, int layer = 0, int level = 0) {
        if (IsDepthFormat(img.nvn_format)) return;
        Transition(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue v;
        memcpy(v.uint32, color, sizeof(v.uint32));  // float/int/uint share the bits
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, SubLevel(img, level), 1,
                                      img.type == VK_IMAGE_TYPE_3D ? 0 : SubLayer(img, layer), 1};
        Rec([v, range, image = img.image](VkCommandBuffer cb) {
            vkCmdClearColorImage(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &range);
        });
        img.written = true;
        img.cpu_source = false;
        if (kind == kClearFloat && clear_colors_logged_ < 16) {
            float f[4];
            memcpy(f, color, sizeof(f));
            uint64_t key = (uint64_t(img.nvn_format) << 48) ^ ((uint64_t(color[0]) * 31 + color[1]) * 31 + color[2]);
            if (logged_clears_.insert(key).second) {
                clear_colors_logged_++;
                LOGI(kTag, "clear %dx%d (format %d) to (%.3f, %.3f, %.3f, %.3f)", img.width, img.height,
                     img.nvn_format, f[0], f[1], f[2], f[3]);
            }
        }
    }

    void ClearDepth(const Cmd& c) {
        if (st_.rts.size() <= size_t(st_.num_rt)) return;
        Image* img = GetImage(st_.rts[size_t(st_.num_rt)], false);
        if (!img) return;
        depth_gen_++;
        VkImageAspectFlags a = 0;
        if (c.depth_mask && HasDepth(img->nvn_format)) a |= VK_IMAGE_ASPECT_DEPTH_BIT;
        if (c.stencil_mask && HasStencil(img->nvn_format)) a |= VK_IMAGE_ASPECT_STENCIL_BIT;
        a &= img->aspect;
        if (!a) return;
        Begin();
        const TexRef& rr = st_.rts[size_t(st_.num_rt)];
        if (a == img->aspect && !capture_active_) {  // every aspect cleared: deferred, or inside the open pass
            VkClearValue v{};
            v.depthStencil = {std::clamp(c.depth, 0.0f, 1.0f), uint32_t(c.stencil)};
            if (rendering_ && pass_depth_ == img) {
                VkClearAttachment ca{a, 0, v};
                const VkClearRect rect{{{0, 0}, pass_extent_}, 0, 1};
                Rec([ca, rect](VkCommandBuffer cb) { vkCmdClearAttachments(cb, 1, &ca, 1, &rect); });
                clears_in_pass_++;
            } else {
                if (InOpenPass(img)) EndRendering();
                if (img->clear_pending) FlushClear(*img);
                img->clear_pending = true;
                img->clear_value = v;
                img->clear_aspect = a;
                img->clear_layer = rr.layer;
                img->clear_level = rr.level;
                clears_deferred_++;
            }
            img->written = true;
            img->cpu_source = false;
            return;
        }
        EndRendering();
        Transition(*img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearDepthStencilValue v{std::clamp(c.depth, 0.0f, 1.0f), uint32_t(c.stencil)};
        const TexRef& ref = st_.rts[size_t(st_.num_rt)];
        VkImageSubresourceRange range{a, SubLevel(*img, ref.level), 1, SubLayer(*img, ref.layer), 1};
        Rec([v, range, image = img->image](VkCommandBuffer cb) {
            vkCmdClearDepthStencilImage(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &range);
        });
        img->written = true;
        img->cpu_source = false;
    }

    void CopyImage(Image& src, const CopyRegion& sr, Image& dst, const CopyRegion& dr, int flags) {
        if (IsDepthFormat(src.nvn_format) != IsDepthFormat(dst.nvn_format)) return;
        auto clampw = [](int v, int def) { return v > 0 ? v : def; };
        int sw = clampw(sr.width, src.width), sh = clampw(sr.height, src.height);
        int dw = clampw(dr.width, dst.width), dh = clampw(dr.height, dst.height);
        Transition(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        Transition(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const bool can_blit = (src.features & VK_FORMAT_FEATURE_BLIT_SRC_BIT) &&
                              (dst.features & VK_FORMAT_FEATURE_BLIT_DST_BIT) &&
                              (!IsDepthFormat(src.nvn_format) || src.vk_format == dst.vk_format);
        if (can_blit) {
            VkImageBlit b{};
            b.srcSubresource = {src.aspect, 0, 0, 1};
            b.srcOffsets[0] = {sr.x, sr.y, 0};
            b.srcOffsets[1] = {sr.x + sw, sr.y + sh, 1};
            b.dstSubresource = {dst.aspect, 0, 0, 1};
            b.dstOffsets[0] = {dr.x, dr.y, 0};
            b.dstOffsets[1] = {dr.x + dw, dr.y + dh, 1};
            if (flags & 0x8) std::swap(b.dstOffsets[0].y, b.dstOffsets[1].y);  // MIRROR_Y
            if (flags & 0x4) std::swap(b.dstOffsets[0].x, b.dstOffsets[1].x);  // MIRROR_X
            VkFilter filter = ((flags & 0x1) && !IsDepthFormat(src.nvn_format) &&
                               (src.features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
                                  ? VK_FILTER_LINEAR
                                  : VK_FILTER_NEAREST;
            Rec([b, filter, si = src.image, di = dst.image](VkCommandBuffer cb) {
                vkCmdBlitImage(cb, si, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, di, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b,
                               filter);
            });
        } else if (src.vk_format == dst.vk_format && sw == dw && sh == dh) {
            VkImageCopy c{};
            c.srcSubresource = {src.aspect, 0, 0, 1};
            c.srcOffset = {sr.x, sr.y, 0};
            c.dstSubresource = {dst.aspect, 0, 0, 1};
            c.dstOffset = {dr.x, dr.y, 0};
            c.extent = {uint32_t(sw), uint32_t(sh), 1};
            Rec([c, si = src.image, di = dst.image](VkCommandBuffer cb) {
                vkCmdCopyImage(cb, si, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, di, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            });
        } else {
            LogOnce("copy", "cannot copy format %d -> %d yet", src.nvn_format, dst.nvn_format);
            return;
        }
        dst.written = dst.written || src.written;
        dst.cpu_source = false;
        if (copies_logged_ < 16) {
            copies_logged_++;
            LOGI(kTag, "copy %dx%d (format %d) -> %dx%d (format %d), flags 0x%x", sw, sh, src.nvn_format, dw, dh,
                 dst.nvn_format, flags);
        }
    }

    // Linear buffer data (row stride from SetCopyRowStride, 0 = tight) into a texture region.
    // Small tables filled by buffer->texture copies (the gamma ramp: 256x1, filled twice at start-up and never
    // copied again): the copy source is watched, and when the game rewrites it the copy is repeated.
    struct WatchedCopy {
        Cmd cmd;
        uint64_t hash;
        size_t size;
    };
    std::vector<WatchedCopy> watched_copies_;
    void CheckWatchedCopies() {
        for (WatchedCopy& w : watched_copies_) {
            if (!w.cmd.copy || !GuestRangeValid(w.cmd.src_addr, w.size)) continue;
            const uint8_t* p = reinterpret_cast<const uint8_t*>(w.cmd.src_addr);
            const uint64_t h = Fnv(p, w.size);
            if (h == w.hash) continue;
            w.hash = h;
            // Only something that still looks like a ramp is copied (the memory may be a reused upload buffer):
            // one entry per texel (8-bit or 32-bit float channels), each colour channel non-decreasing.
            const int width = w.cmd.copy->dst.tex.width;
            const size_t bpp = width > 0 ? w.size / size_t(width) : 0;
            auto channel = [&](int i, int ch) -> float {
                if (bpp == 4) return p[size_t(i) * 4 + size_t(ch)] / 255.f;
                if (bpp == 16) return reinterpret_cast<const float*>(p)[size_t(i) * 4 + size_t(ch)];
                return -1.f;
            };
            bool ramp = (bpp == 4 || bpp == 16) && width >= 16;
            for (int i = 0; ramp && i < width; i++)
                for (int ch = 0; ch < 3 && ramp; ch++) {
                    const float v = channel(i, ch);
                    ramp = v == v && v >= 0.f && v <= 4.f && (i == 0 || v + 1e-6f >= channel(i - 1, ch));
                }
            static int logged = 0;
            if (logged++ < 30)
                LOGI(kTag, "copy source @%llx of a %dx%d table changed (%zu B/texel): [0] %.3f %.3f %.3f | [%d] %.3f | [%d] %.3f: %s",
                     (unsigned long long)w.cmd.src_addr, width, w.cmd.copy->dst.tex.height, bpp, channel(0, 0),
                     channel(0, 1), channel(0, 2), width / 2, channel(width / 2, 0), width - 1, channel(width - 1, 0),
                     ramp ? "copied again" : "not a ramp, ignored");
            if (!ramp) continue;
            const Cmd c = w.cmd;
            CopyBufferToTexture(c, true);
        }
    }
    // The game's tone-mapping pixel shader (its code hash): its exposure constant is scaled by the `exposure`
    // setting.
    static constexpr uint64_t kToneMapShaderHash = 0x9ea3c1ced6b87f4cull;
    uint64_t readbacks_done_ = 0, readbacks_recorded_ = 0, rt_writebacks_ = 0, reports_done_ = 0;
    // Small render targets (luminance / exposure textures) are copied back to game memory after each list, in
    // the texture's own layout: on Switch the CPU can read what the GPU rendered straight from memory, and the
    // game computes its auto exposure that way (it stayed at its default without it: too dark).
    static constexpr size_t kWritebackTexels = 64 * 64;
    std::unordered_set<Image*> small_rts_;
    void WriteBackSmallTargets() {
        if (small_rts_.empty()) return;
        for (Image* img : small_rts_) {
            const FormatInfo fi = GetFormatInfo(img->nvn_format);
            if (IsDepthFormat(img->nvn_format) || fi.block_w != 1 || fi.block_h != 1 ||
                VkTexelBytes(img->vk_format) != uint32_t(fi.bytes_per_block) || !img->addr)
                continue;
            const size_t row = size_t(img->width) * size_t(fi.bytes_per_block), rows = size_t(img->height);
            Begin();
            EndRendering();
            if (StreamRemaining() < row * rows + 1024) Flush();
            const VkDeviceSize off = StreamAlloc(row * rows, 16);
            if (off == VK_WHOLE_SIZE) continue;
            VkBufferImageCopy region{};
            region.bufferOffset = off;
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {uint32_t(img->width), uint32_t(img->height), 1};
            Transition(*img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            Rec([region, buf = stream_, image = img->image](VkCommandBuffer cb) {
                vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);
            });
            Readback rb{off, img->addr, row, rows, 1, row, row * rows};
            rb.texture = true;
            rb.tex = img->tex;
            slots_[slot_].readbacks.push_back(rb);
        }
        small_rts_.clear();
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        Rec([mb](VkCommandBuffer cb) {
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0,
                                 nullptr);
        });
    }
    static uint32_t VkTexelBytes(VkFormat f) {
        switch (f) {
            case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_UINT: return 1;
            case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16_UNORM: case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R16_UINT: return 2;
            case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB:
            case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R16G16_UNORM:
            case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_B10G11R11_UFLOAT_PACK32: case VK_FORMAT_R8G8B8A8_UINT:
                return 4;
            case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R16G16B16A16_UNORM: return 8;
            case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_R32G32B32A32_UINT: return 16;
            default: return 0;
        }
    }
    void CopyTextureToBuffer(const Cmd& c) {
        if (!c.copy) return;
        const CopyInfo& ci = *c.copy;
        Image* img = GetImage(ci.src, false);
        if (!img) {
            LogOnce("t2b-noimg", "texture-to-buffer copy from a texture never rendered or uploaded: skipped");
            return;
        }
        const FormatInfo fi = GetFormatInfo(img->nvn_format);
        if (IsDepthFormat(img->nvn_format) || fi.block_w != 1 || fi.block_h != 1 ||
            VkTexelBytes(img->vk_format) != uint32_t(fi.bytes_per_block)) {
            LogOnce("t2b-format", "texture-to-buffer copy of NVN format %d (Vulkan %d): layouts differ, skipped",
                    img->nvn_format, int(img->vk_format));
            return;
        }
        const CopyRegion& r = ci.src_region;
        const int w = r.width > 0 ? r.width : img->width, h = r.height > 0 ? r.height : img->height;
        const int d = std::max(1, r.depth);
        const size_t row = size_t(w) * size_t(fi.bytes_per_block), rows = size_t(h);
        const size_t stride = c.f[0] > 0 ? size_t(c.f[0]) : row;
        const size_t image_stride = c.f[1] > 0 ? size_t(c.f[1]) : stride * rows;
        const size_t bytes = row * rows * size_t(d);
        Begin();
        EndRendering();
        if (StreamRemaining() < bytes + 1024) Flush();
        const VkDeviceSize off = StreamAlloc(bytes, 16);
        if (off == VK_WHOLE_SIZE) return;
        VkBufferImageCopy region{};
        region.bufferOffset = off;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, uint32_t(std::max(0, ci.src.level)), 0, 1};
        region.imageOffset = {r.x, r.y, img->type == VK_IMAGE_TYPE_3D ? r.z : 0};
        region.imageExtent = {uint32_t(w), uint32_t(h), uint32_t(img->type == VK_IMAGE_TYPE_3D ? d : 1)};
        if (img->type != VK_IMAGE_TYPE_3D) {
            region.imageSubresource.baseArrayLayer = uint32_t(std::max(0, r.z) + std::max(0, ci.src.layer));
            region.imageSubresource.layerCount = uint32_t(d);
        }
        Transition(*img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        Rec([region, buf = stream_, image = img->image](VkCommandBuffer cb) {
            vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);
        });
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        Rec([mb](VkCommandBuffer cb) {
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0,
                                 nullptr);
        });
        slots_[slot_].readbacks.push_back({off, c.dst_addr, row, rows, size_t(d), stride, image_stride});
        readbacks_recorded_++;
        static int logged = 0;
        if (logged++ < 6)
            LOGI(kTag, "texture -> buffer %dx%d at %d,%d (NVN format %d) -> @%llx (row stride %zu)", w, h, r.x, r.y,
                 img->nvn_format, (unsigned long long)c.dst_addr, stride);
    }

    void CopyBufferToTexture(const Cmd& c, bool rerun = false) {
        if (!c.copy) return;
        const CopyInfo& ci = *c.copy;
        Image* img = GetImage(ci.dst, false);
        if (!img || IsDepthFormat(img->nvn_format)) return;
        const FormatInfo fi = GetFormatInfo(img->nvn_format);
        const CopyRegion& r = ci.dst_region;
        const int w = r.width > 0 ? r.width : img->width, h = r.height > 0 ? r.height : img->height;
        const int d = std::max(1, r.depth);
        const size_t row = size_t((w + fi.block_w - 1) / fi.block_w) * size_t(fi.bytes_per_block);
        const size_t rows = size_t((h + fi.block_h - 1) / fi.block_h);
        const size_t stride = c.f[0] > 0 ? size_t(c.f[0]) : row;
        const size_t image_stride = c.f[1] > 0 ? size_t(c.f[1]) : stride * rows;
        const size_t src_size = image_stride * size_t(d - 1) + stride * (rows - 1) + row;
        if (!GuestRangeValid(c.src_addr, src_size)) {
            LogOnce("b2t-range", "buffer-to-texture copy source outside known memory");
            return;
        }
        if (!rerun && h == 1 && w <= 1024 && src_size <= 65536) {
            bool known = false;
            for (WatchedCopy& wc : watched_copies_)
                if (wc.cmd.copy && wc.cmd.copy->dst.addr == ci.dst.addr) {
                    wc = {c, Fnv(reinterpret_cast<const uint8_t*>(c.src_addr), src_size), src_size};
                    known = true;
                }
            if (!known && watched_copies_.size() < 16)
                watched_copies_.push_back({c, Fnv(reinterpret_cast<const uint8_t*>(c.src_addr), src_size), src_size});
            LOGI(kTag, "watching the source @%llx (%zu bytes) of a %dx%d table copy", (unsigned long long)c.src_addr,
                 src_size, w, h);
        }
        if (img->bc != BcKind::None) {
            LogOnce("b2t-bc", "buffer-to-texture copy into a block-compressed texture on a GPU without BC support: skipped");
            return;
        }
        Begin();
        EndRendering();
        if (StreamRemaining() < row * rows * size_t(d) + 1024) Flush();
        VkDeviceSize off = StreamAlloc(row * rows * size_t(d), 16);
        const uint8_t* src = reinterpret_cast<const uint8_t*>(c.src_addr);
        for (int z = 0; z < d; z++)
            for (size_t y = 0; y < rows; y++)
                memcpy(stream_ptr_ + off + (size_t(z) * rows + y) * row, src + size_t(z) * image_stride + y * stride, row);
        // The brightness ramp (256x1, 8-bit RGBA) the final pass reads: the `gamma` setting is applied on top.
        const float gamma = dsr::GetRuntime().config.gamma;
        if (gamma != 1.0f && w == 256 && h == 1 && d == 1 && row == 1024) {
            uint8_t* p = stream_ptr_ + off;
            for (int i = 0; i < 256; i++)
                for (int ch = 0; ch < 3; ch++) {
                    const float v = p[i * 4 + ch] / 255.0f;
                    p[i * 4 + ch] = uint8_t(std::lround(255.0f * std::pow(v, 1.0f / gamma)));
                }
        }
        VkBufferImageCopy region{};
        region.bufferOffset = off;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageOffset = {r.x, r.y, img->type == VK_IMAGE_TYPE_3D ? r.z : 0};
        region.imageExtent = {uint32_t(w), uint32_t(h), uint32_t(img->type == VK_IMAGE_TYPE_3D ? d : 1)};
        if (img->type != VK_IMAGE_TYPE_3D) region.imageSubresource.baseArrayLayer = uint32_t(std::max(0, r.z));
        Transition(*img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        Rec([region, buf = stream_, image = img->image](VkCommandBuffer cb) {
            vkCmdCopyBufferToImage(cb, buf, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        });
        img->written = true;
        img->uploaded = true;
        // The game may later rewrite this texture's memory with the CPU (the brightness lookup table is filled
        // by a copy at start-up, then rewritten in place): watch that memory from here on, like an upload.
        img->cpu_source = true;
        img->sample_hash = SampleHash(ci.dst);
        img->checked_in_list = list_counter_;
        img->check_interval = 1;
        img->next_check_list = list_counter_ + 1;
        if (b2t_logged_++ < 8)
            LOGI(kTag, "buffer -> texture %dx%d at %d,%d (format %d)", w, h, r.x, r.y, img->nvn_format);
    }

    // ======================================================================================================
    // rendering
    // ======================================================================================================
    // `line`: the caller (diagnostics: which code path breaks render passes, counted per 10 s).
    void EndRendering(int line = __builtin_LINE()) {
        if (rendering_) {
            Rec([this](VkCommandBuffer cb) {
                if (rp_fallback_)
                    vkCmdEndRenderPass(cb);
                else
                    end_rendering_(cb);
            });
            rendering_ = false;
            pass_breaks_[line]++;
            if (mali_barriers_)
                for (size_t i = 0; i < pass_color_count_; i++) {
                    Image* c = pass_colors_[i];
                    if (c && !c->clear_pending && c->layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) PassEndBarrier(*c);
                }
        }
    }

    // Starts rendering into the current render targets (or keeps the current pass if they match).
    // Layer and level of render target i of the current set (8: depth).
    std::pair<int, int> RtSub(size_t i) const {
        const size_t idx = i == 8 ? size_t(st_.num_rt) : i;
        if (idx >= st_.rts.size() || (i != 8 && int(i) >= st_.num_rt)) return {0, 0};
        return {st_.rts[idx].layer, st_.rts[idx].level};
    }
    struct BeginPass {  // a vkCmdBeginRendering, as queued for the recorder
        VkRenderingAttachmentInfo colors[8];
        VkRenderingAttachmentInfo depth;
        VkFormat color_formats[8] = {};  // for render pass objects (GPUs without dynamic rendering)
        VkFormat depth_format = VK_FORMAT_UNDEFINED;
        uint32_t count = 0;
        bool has_depth = false, has_stencil = false;
        VkRect2D area;
    };
    bool EnsureRendering(const std::vector<Image*>& colors, Image* depth, VkExtent2D* extent) {
        std::vector<uint64_t>& key = scratch_rendering_key_;
        key.clear();
        for (size_t i = 0; i < colors.size(); i++) {
            key.push_back(reinterpret_cast<uint64_t>(colors[i] ? colors[i]->image : VK_NULL_HANDLE));
            key.push_back(uint64_t(RtSub(i).first) << 32 | uint32_t(RtSub(i).second));
            key.push_back(i < st_.rts.size() ? uint64_t(uint32_t(st_.rts[i].view_format)) : 0);
        }
        key.push_back(reinterpret_cast<uint64_t>(depth ? depth->image : VK_NULL_HANDLE));
        key.push_back(uint64_t(RtSub(8).first) << 32 | uint32_t(RtSub(8).second));
        uint32_t w = UINT32_MAX, h = UINT32_MAX;
        for (size_t i = 0; i < colors.size(); i++)
            if (Image* c = colors[i]) {
                const uint32_t lv = SubLevel(*c, RtSub(i).second);
                w = std::min(w, std::max(1u, uint32_t(c->width) >> lv));
                h = std::min(h, std::max(1u, uint32_t(c->height) >> lv));
            }
        if (depth) {
            const uint32_t lv = SubLevel(*depth, RtSub(8).second);
            w = std::min(w, std::max(1u, uint32_t(depth->width) >> lv));
            h = std::min(h, std::max(1u, uint32_t(depth->height) >> lv));
        }
        if (w == UINT32_MAX) {
            if (!no_attachment_extent_.width) return false;
            w = no_attachment_extent_.width;
            h = no_attachment_extent_.height;
            key.push_back(uint64_t(w) << 32 | h);
        }
        *extent = {w, h};
        if (rendering_ && key == rendering_key_) return true;
        EndRendering();
        if (depth && depth->depth_exported && dsr::GetRuntime().config.lrz_workaround) LrzTouch(*depth);
        // An image the previous pass rendered to, used again here in the same layout, gets no layout transition
        // and so had no barrier: without one the GPU may run the passes overlapped. On tiled GPUs (Adreno) a
        // bin of this pass then loads the image before the earlier pass stored that bin, and whatever it drew
        // there is lost: rectangular, bin-sized holes (and LRZ-block squares) in smoke, dust and glow that
        // flicker with timing. One memory barrier covers every attachment of the pass.
        {
            bool hazard = false;
            for (Image* c : colors)
                if (c && c->attach_unsynced && c->layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) hazard = true;
            if (depth && depth->attach_unsynced && depth->layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
                hazard = true;
            if (hazard) {
                pass_barriers_++;
                Rec([](VkCommandBuffer cb) {
                    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                    mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                    mb.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                    const VkPipelineStageFlags st = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                                    VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                                    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
                    vkCmdPipelineBarrier(cb, st, st, 0, 1, &mb, 0, nullptr, 0, nullptr);
                });
            }
        }
        BeginPass bp;
        bp.area = {{0, 0}, {w, h}};
        for (size_t ci = 0; ci < colors.size() && ci < 8; ci++) {
            Image* c = colors[ci];
            VkRenderingAttachmentInfo a{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            if (c) {
                // A pending clear of exactly this subresource, covering the whole render area: the load clears.
                const uint32_t lv = SubLevel(*c, RtSub(ci).second);
                const bool clear_on_load = c->clear_pending && c->clear_layer == RtSub(ci).first &&
                                           c->clear_level == RtSub(ci).second && w == std::max(1u, uint32_t(c->width) >> lv) &&
                                           h == std::max(1u, uint32_t(c->height) >> lv);
                if (clear_on_load) c->clear_pending = false;
                Transition(*c, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                a.imageView = AttachmentView(*c, RtSub(ci).first, RtSub(ci).second,
                                             ci < st_.rts.size() ? st_.rts[ci].view_format : 0);
                const VkFormat vf = ci < st_.rts.size() && st_.rts[ci].view_format ? ViewFormat(*c, st_.rts[ci].view_format)
                                                                                   : VK_FORMAT_UNDEFINED;
                bp.color_formats[bp.count] = vf != VK_FORMAT_UNDEFINED ? vf : c->vk_format;
                a.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                a.loadOp = c->written ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
                if (clear_on_load) {
                    a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                    a.clearValue = c->clear_value;
                }
                a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                c->attach_unsynced = true;
                c->attach_gen++;
                NoteWritten(c);
                c->written = true;
                c->cpu_source = false;
                if (size_t(c->width) * size_t(c->height) <= kWritebackTexels && c->type == VK_IMAGE_TYPE_2D)
                    small_rts_.insert(c);
            }
            bp.colors[bp.count++] = a;
        }
        VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        if (depth) {
            const uint32_t lv = SubLevel(*depth, RtSub(8).second);
            const bool clear_on_load = depth->clear_pending && depth->clear_layer == RtSub(8).first &&
                                       depth->clear_level == RtSub(8).second &&
                                       w == std::max(1u, uint32_t(depth->width) >> lv) &&
                                       h == std::max(1u, uint32_t(depth->height) >> lv);
            if (clear_on_load) depth->clear_pending = false;
            Transition(*depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
            da.imageView = AttachmentView(*depth, RtSub(8).first, RtSub(8).second);
            bp.depth_format = depth->vk_format;
            da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            da.loadOp = depth->written ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
            da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            depth->attach_unsynced = true;
            depth->attach_gen++;
            da.clearValue.depthStencil = {1.0f, 0};
            if (clear_on_load) {
                da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                da.clearValue = depth->clear_value;
            }
            NoteWritten(depth);
            depth->written = true;
            depth->cpu_source = false;
        }
        bp.depth = da;
        bp.has_depth = depth && HasDepth(depth->nvn_format);
        bp.has_stencil = depth && HasStencil(depth->nvn_format);
        Rec([this, bp](VkCommandBuffer cb) {
            VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
            ri.renderArea = bp.area;
            ri.layerCount = 1;
            ri.colorAttachmentCount = bp.count;
            ri.pColorAttachments = bp.colors;
            if (bp.has_depth) ri.pDepthAttachment = &bp.depth;
            if (bp.has_stencil) ri.pStencilAttachment = &bp.depth;
            if (rp_fallback_)
                BeginRenderPassObject(cb, bp);
            else
                begin_rendering_(cb, &ri);
        });
        pass_color_count_ = std::min<size_t>(colors.size(), 8);
        for (size_t i = 0; i < pass_color_count_; i++) {
            pass_colors_[i] = colors[i];
            pass_color_sub_[i] = RtSub(i);
        }
        pass_depth_ = depth;
        pass_extent_ = {w, h};
        render_passes_++;
        rendering_ = true;
        rendering_key_ = key;
        return true;
    }

    // ======================================================================================================
    // shaders, layouts, pipelines
    // ======================================================================================================
    // ---- persistent shader cache and warm-up ------------------------------------------------------------------
    // Guest shaders are translated once. The translation (SPIR-V and shader info) is kept in
    // cache/shader_cache.bin, keyed by the guest code's hash and the translation options, so it is the same on
    // every run. Each translated shader's code and options are also kept in cache/shader_warm.bin; on the next
    // start, WarmupShaders() translates the ones the disk cache lacks, before the game starts, so the game does not
    // stop to translate them. Bump the versions when the translator's output or these layouts change.
    static constexpr uint32_t kShaderCacheVersion = 1, kWarmVersion = 1;
    struct CachedShader {
        std::vector<uint32_t> spirv;
        shader::ShaderInfo info;
    };
    std::unordered_map<std::vector<uint32_t>, CachedShader, VecHash> shader_disk_;
    std::unordered_map<std::vector<uint32_t>, int, VecHash> warm_seen_;
    bool shader_cache_writable_ = false;
    uint64_t shader_cache_hits_ = 0, shader_cache_misses_ = 0;
    struct WarmRec {
        std::vector<uint32_t> key;
        shader::TranslateOptions opt;
        std::vector<uint8_t> code;
    };

    std::string CacheFile(const char* name) const {
        const std::string& dir = dsr::GetRuntime().config.cache_dir;
        return dir.empty() ? std::string() : dir + "/" + name;
    }
    static void WriteShaderRecord(FILE* f, const std::vector<uint32_t>& key, const std::vector<uint32_t>& spirv,
                                  const shader::ShaderInfo& info) {
        const uint32_t nwords = uint32_t(spirv.size()), info_size = uint32_t(sizeof(info));
        fwrite(key.data(), 4, key.size(), f);
        fwrite(&nwords, 4, 1, f);
        fwrite(&info_size, 4, 1, f);
        fwrite(&info, sizeof(info), 1, f);
        fwrite(spirv.data(), 4, spirv.size(), f);
    }
    void RewriteShaderCache(const std::string& path) {
        FILE* f = fopen(path.c_str(), "wb");
        if (!f) {
            shader_cache_writable_ = false;
            return;
        }
        const char magic[8] = {'D', 'S', 'R', 'S', 'C', '0', '1', 0};
        fwrite(magic, 1, 8, f);
        fwrite(&kShaderCacheVersion, 4, 1, f);
        for (const auto& e : shader_disk_) WriteShaderRecord(f, e.first, e.second.spirv, e.second.info);
        fclose(f);
        shader_cache_writable_ = true;
    }
    void LoadShaderCache() {
        static_assert(std::is_trivially_copyable<shader::ShaderInfo>::value, "ShaderInfo must be plain data");
        const std::string path = CacheFile("shader_cache.bin");
        if (path.empty()) return;
        const char magic[8] = {'D', 'S', 'R', 'S', 'C', '0', '1', 0};
        bool damaged = true;
        if (FILE* f = fopen(path.c_str(), "rb")) {
            char m[8];
            uint32_t ver = 0;
            if (fread(m, 1, 8, f) == 8 && memcmp(m, magic, 8) == 0 && fread(&ver, 4, 1, f) == 1 &&
                ver == kShaderCacheVersion) {
                damaged = false;
                for (;;) {
                    uint32_t key[6], nwords = 0, info_size = 0;
                    if (fread(key, 4, 6, f) != 6) break;  // clean end of file
                    if (fread(&nwords, 4, 1, f) != 1 || fread(&info_size, 4, 1, f) != 1) {
                        damaged = true;
                        break;
                    }
                    CachedShader c;
                    if (info_size != sizeof(c.info) || nwords == 0 || nwords > (1u << 22)) {
                        damaged = true;
                        break;
                    }
                    c.spirv.resize(nwords);
                    if (fread(&c.info, sizeof(c.info), 1, f) != 1 || fread(c.spirv.data(), 4, nwords, f) != nwords) {
                        damaged = true;
                        break;
                    }
                    shader_disk_.emplace(std::vector<uint32_t>(key, key + 6), std::move(c));
                }
            }
            fclose(f);
        }
        if (damaged) {
            shader_disk_.clear();  // missing, another version or damaged: start a fresh file
            RewriteShaderCache(path);
        } else {
            shader_cache_writable_ = true;
        }
        LOGI(kTag, "shader cache: %zu translated shaders loaded", shader_disk_.size());
    }
    void AppendShaderCache(const std::vector<uint32_t>& key, const shader::TranslateResult& r) {
        if (!shader_cache_writable_) return;
        if (FILE* f = fopen(CacheFile("shader_cache.bin").c_str(), "ab")) {
            WriteShaderRecord(f, key, r.spirv, r.info);
            fclose(f);
        }
    }
    // Shaders translated in this run: their code and options, for the warm-up of the next start.
    void RememberForWarmup(const ShaderCode& code, const shader::TranslateOptions& opt, const std::vector<uint32_t>& key) {
        static_assert(std::is_trivially_copyable<shader::TranslateOptions>::value, "options are stored as bytes");
        if (!warm_seen_.emplace(key, 1).second) return;
        const std::string path = CacheFile("shader_warm.bin");
        FILE* f = path.empty() ? nullptr : fopen(path.c_str(), "ab");
        if (!f) return;
        if (ftell(f) == 0) {
            const char magic[8] = {'D', 'S', 'R', 'W', 'U', '0', '1', 0};
            fwrite(magic, 1, 8, f);
            fwrite(&kWarmVersion, 4, 1, f);
        }
        const uint32_t opt_size = uint32_t(sizeof(opt)), code_size = uint32_t(code.code.size());
        fwrite(key.data(), 4, key.size(), f);
        fwrite(&opt_size, 4, 1, f);
        fwrite(&code_size, 4, 1, f);
        fwrite(&opt, sizeof(opt), 1, f);
        fwrite(code.code.data(), 1, code.code.size(), f);
        fclose(f);
    }
    std::vector<WarmRec> ReadWarm() {
        std::vector<WarmRec> out;
        const std::string path = CacheFile("shader_warm.bin");
        FILE* f = path.empty() ? nullptr : fopen(path.c_str(), "rb");
        if (!f) return out;
        const char magic[8] = {'D', 'S', 'R', 'W', 'U', '0', '1', 0};
        char m[8];
        uint32_t ver = 0;
        if (fread(m, 1, 8, f) == 8 && memcmp(m, magic, 8) == 0 && fread(&ver, 4, 1, f) == 1 && ver == kWarmVersion) {
            for (;;) {
                uint32_t k[6], opt_size = 0, code_size = 0;
                if (fread(k, 4, 6, f) != 6) break;
                if (fread(&opt_size, 4, 1, f) != 1 || fread(&code_size, 4, 1, f) != 1) break;
                WarmRec w;
                w.key.assign(k, k + 6);
                if (opt_size != sizeof(w.opt) || code_size == 0 || code_size > (1u << 24)) break;  // another version
                if (fread(&w.opt, sizeof(w.opt), 1, f) != 1) break;
                w.code.resize(code_size);
                if (fread(w.code.data(), 1, code_size, f) != code_size) break;
                warm_seen_.emplace(w.key, 1);
                out.push_back(std::move(w));
            }
        }
        fclose(f);
        return out;
    }
    // Before the game starts: translates the recorded shaders that the disk cache does not have (at most 30 s).
    void WarmupShaders() {
        const std::vector<WarmRec> recs = ReadWarm();
        if (recs.empty()) return;
        const auto t0 = std::chrono::steady_clock::now();
        size_t missing = 0, translated = 0;
        for (const WarmRec& w : recs) {
            if (shader_disk_.count(w.key)) continue;
            missing++;
            if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(30)) break;
            shader::TranslateResult r = shader::Translate(w.code.data(), w.code.size(), w.opt);
            if (!r.ok) continue;
            shader_disk_[w.key] = CachedShader{r.spirv, r.info};
            AppendShaderCache(w.key, r);
            translated++;
        }
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        LOGI(kTag, "warm-up: %zu of %zu recorded shaders were missing; %zu translated before the game starts (%.1f s)",
             missing, recs.size(), translated, secs);
    }

    Shader* GetShader(const ShaderCode& code, const shader::TranslateOptions& opt) {
        const uint32_t parts[6] = {uint32_t(code.hash), uint32_t(code.hash >> 32), opt.int_attrib_mask,
                                   opt.signed_attrib_mask, uint32_t(opt.gl_depth_range),
                                   uint32_t(opt.alpha_func) | (opt.debug_outputs ? 0x100u : 0u) |
                                       (uint32_t(opt.depth_test_func) << 12)};  // the depth test variant is its own shader
        Key64 fk;
        fk.Add(parts, sizeof(parts));
        auto fast = shaders_fast_.find(fk.h);
        if (fast != shaders_fast_.end()) return fast->second;
        std::vector<uint32_t> key(parts, parts + 6);
        auto it = shaders_.find(key);
        if (it != shaders_.end()) {
            shaders_fast_[fk.h] = &it->second;
            return &it->second;
        }
        Shader& s = shaders_[key];
        shader::TranslateResult r;
        const auto disk = shader_disk_.find(key);
        if (disk != shader_disk_.end()) {
            r.ok = true;
            r.spirv = disk->second.spirv;
            r.info = disk->second.info;
            shader_cache_hits_++;
        } else {
            r = shader::Translate(code.code.data(), code.code.size(), opt);
            shader_cache_misses_++;
            if (r.ok) {
                shader_disk_[key] = CachedShader{r.spirv, r.info};
                AppendShaderCache(key, r);
                RememberForWarmup(code, opt, key);
            }
        }
        if (!r.ok) {
            s.error = r.error;
            if (shader_failures_logged_++ < 24)
                LOGW(kTag, "shader %016llx not translated: %s", (unsigned long long)code.hash, r.error.c_str());
            return &s;
        }
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = r.spirv.size() * 4;
        ci.pCode = r.spirv.data();
        if (vkCreateShaderModule(device_, &ci, nullptr, &s.module) != VK_SUCCESS) {
            s.error = "vkCreateShaderModule failed";
            LogOnce("module", "vkCreateShaderModule failed for shader %016llx", (unsigned long long)code.hash);
            return &s;
        }
        s.info = r.info;
        s.ok = true;
        if (++shaders_translated_ <= 8 || shaders_translated_ % 100 == 0)
            LOGI(kTag, "translated %s shader %016llx (%u instructions); %d so far",
                 r.info.stage == shader::Stage::Vertex ? "vertex" : "pixel", (unsigned long long)code.hash,
                 r.info.instruction_count, shaders_translated_);
        return &s;
    }

    // Uniform buffers of a stage are bound with dynamic offsets (so descriptor sets do not depend on where this
    // draw's uniform data landed in the stream, and are shared between draws) when the device allows that many.
    bool DynamicUniforms(const shader::ShaderInfo& info) const {
        return uint32_t(__builtin_popcount(info.cbuf_mask)) <= props_.limits.maxDescriptorSetUniformBuffersDynamic / 2;
    }
    VkDescriptorSetLayout SetLayout(VkShaderStageFlags stage, const shader::ShaderInfo& info) {
        const uint32_t stage_key = uint32_t(stage) | (DynamicUniforms(info) ? 0x80000000u : 0);
        const uint64_t fast_key = (uint64_t(info.cbuf_mask) << 40) ^ (uint64_t(info.texture_mask) << 20) ^
                                  (uint64_t(info.shadow_mask) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(info.image_mask) << 58) ^
                                  uint64_t(stage_key);
        auto fast = set_layouts_fast_.find(fast_key);
        if (fast != set_layouts_fast_.end() && fast->second.first.size() == 5 && fast->second.first[0] == stage_key &&
            fast->second.first[1] == info.cbuf_mask && fast->second.first[2] == info.texture_mask &&
            fast->second.first[3] == info.shadow_mask && fast->second.first[4] == info.image_mask)
            return fast->second.second;
        std::vector<uint32_t> key = {stage_key, info.cbuf_mask, info.texture_mask, info.shadow_mask, info.image_mask};
        auto it = set_layouts_.find(key);
        if (it != set_layouts_.end()) set_layouts_fast_[fast_key] = {key, it->second};
        if (it != set_layouts_.end()) return it->second;
        std::vector<VkDescriptorSetLayoutBinding> b;
        for (uint32_t i = 0; i < 18; i++)
            if (info.cbuf_mask >> i & 1)
                b.push_back({i, DynamicUniforms(info) ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                             1, stage, nullptr});
        for (uint32_t i = 0; i < 32; i++) {
            if (info.texture_mask >> i & 1) b.push_back({32 + i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, stage, nullptr});
            if (info.shadow_mask >> i & 1) b.push_back({64 + i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, stage, nullptr});
        }
        for (uint32_t i = 0; i < 8; i++)
            if (info.image_mask >> i & 1) b.push_back({96 + i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, stage, nullptr});
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = uint32_t(b.size());
        ci.pBindings = b.data();
        VkDescriptorSetLayout l = VK_NULL_HANDLE;
        vkCreateDescriptorSetLayout(device_, &ci, nullptr, &l);
        set_layouts_[key] = l;
        return l;
    }

    VkPipelineLayout PipelineLayout(VkDescriptorSetLayout vs, VkDescriptorSetLayout fs) {
        if (vs == last_pl_vs_ && fs == last_pl_fs_ && last_pl_) return last_pl_;
        last_pl_vs_ = vs;
        last_pl_fs_ = fs;
        last_pl_ = PipelineLayoutLookup(vs, fs);
        return last_pl_;
    }
    VkPipelineLayout PipelineLayoutLookup(VkDescriptorSetLayout vs, VkDescriptorSetLayout fs) {
        std::vector<uint32_t> key = {uint32_t(reinterpret_cast<uintptr_t>(vs)), uint32_t(uint64_t(reinterpret_cast<uintptr_t>(vs)) >> 32),
                                     uint32_t(reinterpret_cast<uintptr_t>(fs)), uint32_t(uint64_t(reinterpret_cast<uintptr_t>(fs)) >> 32)};
        auto it = pipeline_layouts_.find(key);
        if (it != pipeline_layouts_.end()) return it->second;
        VkDescriptorSetLayout sets[2] = {vs, fs};
        VkPushConstantRange pc{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
        VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        ci.setLayoutCount = 2;
        ci.pSetLayouts = sets;
        ci.pushConstantRangeCount = 1;
        ci.pPushConstantRanges = &pc;
        VkPipelineLayout l = VK_NULL_HANDLE;
        vkCreatePipelineLayout(device_, &ci, nullptr, &l);
        pipeline_layouts_[key] = l;
        return l;
    }

    struct VertexInput {
        std::vector<VkVertexInputBindingDescription> bindings;
        std::vector<VkVertexInputAttributeDescription> attributes;
        uint32_t extent[16] = {};  // bytes past the element start each stream's attributes read
    };

    // ======================================================================================================
    // draws
    // ======================================================================================================
    // 1x1 opaque white image for the dimension a shader expects (6 layers for cubes).
    // A copy of a depth buffer, refreshed when depth has been written since the last copy.
    // Square holes in smoke, fog and glow: the game makes its half- and quarter-size depth buffers with a
    // full-screen draw writing gl_FragDepth, then depth-tests its particles against them. Frame captures show the
    // same rectangular screen regions missing from every particle pass of a frame while the depth itself is
    // right: Adreno's low-resolution depth buffer (LRZ, which rejects whole blocks of pixels early) is left stale
    // by the depth written from the fragment shader. A copy into a depth image makes the driver drop its LRZ data,
    // so before such an image is attached again one texel is copied out and straight back (value unchanged).
    void LrzTouch(Image& depth) {
        depth.depth_exported = false;
        TexRef t;
        t.valid = true;
        t.addr = 0x400 + uint64_t(uint32_t(depth.nvn_format));  // never a real guest address
        t.tex.format = depth.nvn_format;
        t.tex.width = t.tex.height = 1;
        t.tex.levels = 1;
        t.tex.target = kTarget2D;
        t.tex.depth = 1;
        auto it = images_.find(t.addr);
        Image* one = it != images_.end() ? &it->second : CreateImage(t);
        if (!one || one->vk_format != depth.vk_format) return;
        CopyRegion px;
        px.width = px.height = px.depth = 1;
        const VkImageLayout back = depth.layout;
        CopyImage(depth, px, *one, px, 0);
        CopyImage(*one, px, depth, px, 0);
        Transition(depth, back == VK_IMAGE_LAYOUT_UNDEFINED ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : back);
        lrz_touches_++;
    }

    Image* SampledCopy(Image& src, const TexRef& ref) {
        TexRef copy_ref = ref;
        copy_ref.addr = ref.addr ^ (3ull << 61);
        auto it = images_.find(copy_ref.addr);
        Image* copy = nullptr;
        if (it != images_.end() && it->second.width == src.width && it->second.height == src.height &&
            it->second.vk_format == src.vk_format)
            copy = &it->second;
        else
            copy = CreateImage(copy_ref);
        if (!copy) return nullptr;
        uint64_t& gen = sampled_copy_gen_[copy_ref.addr];
        if (gen != src.attach_gen + 1) {
            EndRendering();
            CopyImage(src, CopyRegion{}, *copy, CopyRegion{}, 0);
            copy->written = true;
            gen = src.attach_gen + 1;
        }
        if (copy->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            EndRendering();
            Transition(*copy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        return copy;
    }
    std::unordered_map<uint64_t, uint64_t> sampled_copy_gen_;

    Image* DepthSnapshot(Image& depth, const TexRef& ref) {
        TexRef snap_ref = ref;
        snap_ref.addr = ref.addr ^ (1ull << 62);
        auto it = images_.find(snap_ref.addr);
        Image* snap = nullptr;
        if (it != images_.end() && it->second.width == depth.width && it->second.height == depth.height &&
            it->second.vk_format == depth.vk_format)
            snap = &it->second;
        else
            snap = CreateImage(snap_ref);
        if (!snap) return nullptr;
        auto& gen = snapshot_gen_[snap_ref.addr];
        if (gen != depth_gen_ + 1) {
            EndRendering();
            CopyImage(depth, CopyRegion{}, *snap, CopyRegion{}, 0);
            snap->written = true;
            gen = depth_gen_ + 1;
            Transition(depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        }
        if (snap->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            EndRendering();
            Transition(*snap, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        return snap;
    }

    // 1x1 stand-in of a dimension: white (missing textures) or opaque black (`black`: placeholders of the wrong
    // dimension, like the game's 16x16 black texture bound where a cube map is sampled).
    Image* Fallback(shader::TexDim dim, bool array, bool black = false) {
        const int key = int(dim) * 2 + (array ? 1 : 0) + (black ? 64 : 0);
        auto it = fallbacks_.find(key);
        if (it != fallbacks_.end()) return it->second;
        TexRef t;
        t.valid = true;
        t.addr = 0x100 + uint64_t(key);  // never a real guest address
        t.tex.format = 37;
        t.tex.width = t.tex.height = 1;
        t.tex.levels = 1;
        t.tex.target = dim == shader::TexDim::D1 ? (array ? kTarget1DArray : kTarget1D)
                       : dim == shader::TexDim::D3 ? kTarget3D
                       : dim == shader::TexDim::Cube ? kTargetCubemap
                       : (array ? kTarget2DArray : kTarget2D);
        t.tex.depth = 1;
        Image* img = CreateImage(t);
        if (img) {
            Begin();
            EndRendering();
            Transition(*img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkClearColorValue white;
            white.float32[0] = white.float32[1] = white.float32[2] = black ? 0.0f : 1.0f;
            white.float32[3] = 1.0f;
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, uint32_t(img->layers)};
            Rec([white, range, image = img->image](VkCommandBuffer cb) {
                vkCmdClearColorImage(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &range);
            });
            img->written = img->uploaded = true;
        }
        fallbacks_[key] = img;
        return img;
    }

    // This game does not bind textures through NVN: it writes texture handles (from
    // nvnDeviceGetTextureHandle) into its own uniform buffer at binding 10, which shaders read as
    // c[13][0x20 + 8 * slot]. Read the handle from there and resolve it like a bind would.
    std::unordered_map<uint32_t, BoundTexture> handle_memo_;
    uint64_t handle_memo_list_ = ~0ull;
    BoundTexture HandleFromUniforms(int stage, uint32_t slot) {
        BoundTexture bt;
        const BufferRange& r = st_.uniforms[stage][13 - kCbufBase];
        const uint64_t off = 0x20 + 8ull * slot;
        if (!r.address || off + 4 > r.size || !GuestRangeValid(r.address + off, 4)) return bt;
        uint32_t handle;
        memcpy(&handle, reinterpret_cast<const void*>(r.address + off), 4);
        // Resolved once per list (two locked lookups in the shared pools otherwise, for every texture of every
        // draw); registrations are made before the list is submitted.
        if (handle_memo_list_ != list_counter_) {
            handle_memo_.clear();
            handle_memo_list_ = list_counter_;
        }
        auto memo = handle_memo_.find(handle);
        if (memo != handle_memo_.end()) return memo->second;
        bt.texture = TextureById(int(handle & 0xFFFFF));
        bt.sampler = SamplerById(int(handle >> 20), reinterpret_cast<const void*>(st_.sampler_pool));
        bt.valid = bt.texture.valid;
        handle_memo_.emplace(handle, bt);
        if (!bt.valid && handles_logged_++ < 8)
            LOGW(kTag, "texture handle 0x%08x (stage %d slot %u) does not name a registered texture", handle, stage, slot);
        return bt;
    }

    void Skip(const char* reason) {
        auto& n = skipped_[reason];
        if (n++ == 0) LOGW(kTag, "draw skipped: %s", reason);
    }

    // Draw-path profile: time spent per phase (reported with the 10 s stats).
    static constexpr const char* kPhaseNames[7] = {"setup", "targets", "textures", "uniforms", "vertices",
                                                   "pipeline", "pass+queue"};
    double phase_ms_[7] = {};
    std::chrono::steady_clock::time_point phase_t_;
    bool phase_timing_ = false;  // one draw in 8 is timed (7 clock reads per draw showed up in profiles)
    void Phase(int finished) {
        if (!phase_timing_) return;
        auto now = std::chrono::steady_clock::now();
        if (finished >= 0) phase_ms_[finished] += 8 * std::chrono::duration<double, std::milli>(now - phase_t_).count();
        phase_t_ = now;
    }
    struct PhaseEnd {
        VulkanBackend* self;
        ~PhaseEnd() { self->Phase(6); }
    };

    // Per-draw scratch storage (reused, so a draw allocates nothing). Draw only re-enters itself as
    // `return Draw(p)`, after which the outer call uses none of these.
    struct TexWrite {
        uint32_t set, binding;
        VkDescriptorImageInfo info;
    };
    struct UboWrite {
        uint32_t set, binding;
        VkDescriptorBufferInfo info;
        bool dynamic;
        uint32_t dyn_offset;
    };
    std::vector<TexWrite> scratch_tex_writes_;
    std::vector<UboWrite> scratch_ubo_writes_;
    std::vector<Image*> scratch_colors_;
    std::vector<VkFormat> scratch_color_formats_;
    std::vector<uint64_t> scratch_rendering_key_;
    VertexInput scratch_vin_;

    void Draw(const DrawParams& p) {
        phase_timing_ = (drawn_ & 7) == 0;
        Phase(-1);
        PhaseEnd phase_end{this};
        no_attachment_extent_ = {0, 0};
        if (p.count <= 0 || p.instances <= 0) return;
        if (!begin_rendering_ && !rp_fallback_) return Skip("no dynamic rendering");
        if (!st_.program[0]) return Skip("no vertex program bound");
        const DeviceModes modes = GetDeviceModes();

        // ---- vertex layout and shader options --------------------------------------------------------------
        shader::TranslateOptions vopt, fopt;
        vopt.gl_depth_range = modes.depth_mode == 0;
        for (size_t i = 0; i < st_.attribs.size() && i < 32; i++) {
            VertexFormat vf = VertexFormatOf(st_.attribs[i].format);
            if (vf.is_int) vopt.int_attrib_mask |= 1u << i;
            if (vf.is_signed) vopt.signed_attrib_mask |= 1u << i;
        }
        fopt.alpha_func = st_.color.alpha_func;
        // `hole_test` 1024 (square holes): the smoke / fog / glow fragment shaders show what they computed (see
        // TranslateOptions::debug_outputs), drawn without blending, depth test or the 3 empty targets.
        const bool debug_fx = (dsr::GetRuntime().config.hole_test & 1024) && st_.program[1] &&
                              (st_.program[1]->hash == 0x6f634579337b2b69ull || st_.program[1]->hash == 0x3fc1b1baa8114318ull);
        fopt.debug_outputs = debug_fx;
        // Square holes in smoke, fog and glow: the game makes its half- and quarter-size depth buffers with a
        // full-screen draw that writes gl_FragDepth, and then draws particles testing (not writing) against them.
        // Adreno's hardware depth test then drops whole screen tiles of those particles (frame captures: the same
        // tiles in every effect pass of a frame, different ones each frame; the depth values themselves are right,
        // and with the hardware test off the holes are gone). Such draws test in the fragment shader instead,
        // against a copy of the depth buffer, and the hardware test is off for them.
        const Image* depth_peek = st_.rts.size() > size_t(st_.num_rt) ? GetImage(st_.rts[size_t(st_.num_rt)], false) : nullptr;
        const bool shader_depth_test = dsr::GetRuntime().config.shader_depth_test && depth_peek &&
                                       depth_peek->fragdepth_written && IsDepthFormat(depth_peek->nvn_format) &&
                                       HasDepth(depth_peek->nvn_format) && st_.depth.depth_test && !st_.depth.depth_write &&
                                       st_.program[1] && st_.depth.depth_func != 8 && !debug_fx;
        if (shader_depth_test) fopt.depth_test_func = st_.depth.depth_func;
        Shader* vs = GetShader(*st_.program[0], vopt);
        // No pixel program: a depth-only pass (shadow maps, depth prepass). Draw with the vertex stage only.
        Shader* fs = st_.program[1] ? GetShader(*st_.program[1], fopt) : &no_fragment_;
        if (!vs->ok || (st_.program[1] && !fs->ok)) return Skip("shader not translated");

        VertexInput& vin = scratch_vin_;
        vin.bindings.clear();
        vin.attributes.clear();
        memset(vin.extent, 0, sizeof(vin.extent));
        uint32_t used_streams = 0;
        for (uint32_t loc = 0; loc < 32; loc++) {
            if (!(vs->info.input_mask >> loc & 1)) continue;
            if (loc >= st_.attribs.size()) return Skip("shader reads an attribute that is not set up");
            const VertexAttribObj& a = st_.attribs[loc];
            VertexFormat vf = VertexFormatOf(a.format);
            if (vf.format == VK_FORMAT_UNDEFINED) {
                LogOnce(("vfmt" + std::to_string(a.format)).c_str(), "unknown vertex format %d", a.format);
                return Skip("unknown vertex format");
            }
            if (a.stream < 0 || a.stream >= 16) return Skip("bad vertex stream");
            if (!VertexFormatSupported(vf.format)) return Skip("vertex format not supported by the GPU");
            vin.attributes.push_back({loc, uint32_t(a.stream), vf.format, uint32_t(a.offset)});
            vin.extent[a.stream] = std::max(vin.extent[a.stream], uint32_t(a.offset) + vf.bytes);
            used_streams |= 1u << a.stream;
        }
        for (uint32_t s = 0; s < 16; s++) {
            if (!(used_streams >> s & 1)) continue;
            const VertexStreamObj vs_ = s < st_.streams.size() ? st_.streams[s] : VertexStreamObj{};
            vin.bindings.push_back({s, uint32_t(vs_.stride),
                                    vs_.divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX});
        }

        Phase(0);
        // ---- render targets ----------------------------------------------------------------------------------
        std::vector<Image*>& colors = scratch_colors_;
        colors.assign(size_t(std::max(0, st_.num_rt)), nullptr);
        for (int i = 0; i < st_.num_rt; i++) {
            Image* img = GetImage(st_.rts[size_t(i)], false);
            if (img && (img->features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) && !IsDepthFormat(img->nvn_format))
                colors[size_t(i)] = img;
        }
        Image* depth = st_.rts.size() > size_t(st_.num_rt) ? GetImage(st_.rts[size_t(st_.num_rt)], false) : nullptr;
        // `hole_test` 4 (square holes): colour targets this draw cannot write (channel mask 0) are left out of the
        // render pass instead of being loaded and stored back untouched.
        if (dsr::GetRuntime().config.hole_test & 4)
            for (size_t i = 0; i < colors.size(); i++)
                if (((st_.channel.mask >> (4 * i)) & 0xF) == 0) colors[i] = nullptr;
        // `hole_test` 512 (square holes): trailing colour targets with no image (the game binds 4, only the first is
        // usable) are dropped from the render pass and the pipeline, instead of being null attachments.
        if (dsr::GetRuntime().config.hole_test & 512)
            while (!colors.empty() && !colors.back()) colors.pop_back();
        if (depth && !IsDepthFormat(depth->nvn_format)) depth = nullptr;

        Begin();
        // Keep enough streaming space for this draw so nothing allocated below is lost to a flush.
        if (StreamRemaining() < (16ull << 20)) Flush();

        Phase(1);
        // ---- textures (uploads and layout changes happen outside rendering) ---------------------------------
        std::vector<TexWrite>& tex_writes = scratch_tex_writes_;  // members: no allocation per draw
        tex_writes.clear();
        for (int stage = 0; stage < 2; stage++) {
            const shader::ShaderInfo& info = stage == 0 ? vs->info : fs->info;
            for (uint32_t slot = 0; slot < 32; slot++) {
                const bool plain = info.texture_mask >> slot & 1, shadow = info.shadow_mask >> slot & 1;
                if (!plain && !shadow) continue;
                BoundTexture bt = st_.textures[stage][slot];
                Image* img = nullptr;
                if (shader_depth_test && stage == 1 && slot == 31 && depth) {
                    // The depth copy the shader tests against (TranslateOptions::depth_test_func).
                    bt = BoundTexture{};
                    bt.valid = true;
                    bt.texture = st_.rts[size_t(st_.num_rt)];
                    for (int k = 0; k < 4; k++) bt.texture.tex.swizzle[k] = uint8_t(2 + k);
                    img = DepthSnapshot(*depth, st_.rts[size_t(st_.num_rt)]);
                    if (!img) return Skip("no depth copy for the shader depth test");
                    shader_depth_draws_++;
                } else {
                    if (!bt.valid) bt = HandleFromUniforms(stage, slot);
                    img = bt.valid ? GetImage(bt.texture, true) : nullptr;
                }
                if (!img) {
                    // Missing or unsupported texture: draw with a white stand-in rather than dropping the
                    // draw (one skipped pass can blank the whole frame).
                    Skip(bt.valid ? "texture format not supported (white stand-in used)"
                                  : "texture handle not found (white stand-in used)");
                    img = Fallback(info.texture_dim[slot], info.texture_array[slot]);
                    bt.texture.tex.swizzle[0] = 2;
                    bt.texture.tex.swizzle[1] = 3;
                    bt.texture.tex.swizzle[2] = 4;
                    bt.texture.tex.swizzle[3] = 5;
                    if (!img) return Skip("no stand-in texture");
                }
                for (Image* c : colors)
                    if (c == img) return Skip("texture is also a render target");
                if (img != depth && IsDepthFormat(img->nvn_format) && (dsr::GetRuntime().config.hole_test & 16)) {
                    // `hole_test` 16 (square holes): depth textures are read from a copy made after their last
                    // render pass, instead of directly.
                    Image* copy = SampledCopy(*img, bt.texture);
                    if (copy) img = copy;
                }
                if (depth == img) {
                    // Sampling the bound depth buffer (soft particles, fog): read a snapshot copy instead.
                    img = DepthSnapshot(*depth, st_.rts[size_t(st_.num_rt)]);
                    if (!img) return Skip("texture is also the depth target");
                }
                if (stage == 0 && img->frag_only_sync && img->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
                    !img->clear_pending) {  // mali_barriers: released to fragment shaders only
                    EndRendering();
                    VertexReadBarrier(*img);
                }
                if (img->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL || img->clear_pending) {
                    if (rendering_) {
                        char k[96];
                        snprintf(k, sizeof(k), "fmt %d %dx%d from layout %d%s", img->nvn_format, img->width,
                                 img->height, int(img->layout), img == depth ? " (depth target)" : "");
                        sampled_breaks_[k]++;
                    }
                    EndRendering();
                    Transition(*img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                }
                uint8_t swz[4];
                memcpy(swz, bt.texture.tex.swizzle, 4);
                if (IgnoresAlpha(img->nvn_format))
                    for (uint8_t& c : swz)
                        if (c == 5) c = 1;  // A -> ONE
                if (capture_active_) {
                    if (stage == 0 && slot == 0 && tex_desc_draw_ != drawn_) {
                        pass_tex_desc_.clear();
                        tex_desc_draw_ = drawn_;
                    }
                    char d[200];
                    const SamplerObj& so = bt.sampler;  // wrap s/t/r, min/mag filter: stretched or black surfaces
                    snprintf(d, sizeof(d), " [%s%u: fmt %d %dx%d @%llx%s%s%s wrap %d%d%d filter %d/%d]", stage ? "fs" : "vs",
                             slot, img->nvn_format, img->width, img->height, (unsigned long long)bt.texture.addr,
                             img->written ? " drawn" : "", img->uploaded ? " uploaded" : "", shadow ? " compare" : "",
                             so.wrap[0], so.wrap[1], so.wrap[2], so.min_filter, so.mag_filter);
                    if (tex_desc_draw_ != drawn_) {
                        pass_tex_desc_.clear();
                        tex_desc_draw_ = drawn_;
                    }
                    pass_tex_desc_ += d;
                    if (draw_images_draw_ != drawn_) {
                        draw_images_.clear();
                        draw_images_draw_ = drawn_;
                    }
                    draw_images_.push_back({int(stage), int(slot), img});
                }
                VkImageView view = SampledView(*img, info.texture_dim[slot], info.texture_array[slot], swz,
                                               bt.valid ? &bt.texture : nullptr);
                if (!view) {
                    char k[160];
                    snprintf(k, sizeof(k), "view: shader %s%d dim %d array %d | image type %d %dx%dx%d layers %d fmt %d",
                             stage ? "fs" : "vs", int(slot), int(info.texture_dim[slot]), int(info.texture_array[slot]),
                             int(img->type), img->width, img->height, img->depth, img->layers, img->nvn_format);
                    if (view_mismatch_logged_.size() < 16 && view_mismatch_logged_.insert(k).second)
                        LOGW(kTag, "texture view does not match the shader: %s (target %d, view layers %d, layer %d)", k,
                             bt.texture.tex.target, bt.texture.view_layers, bt.texture.layer);
                    // The game binds a placeholder of another dimension (a 16x16 black 2D texture where the shader
                    // samples a cube map); the hardware samples it anyway. A black stand-in of the right dimension
                    // keeps the draw (it was skipped: objects went missing).
                    img = Fallback(info.texture_dim[slot], info.texture_array[slot], true);
                    if (img && img->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
                        EndRendering();
                        Transition(*img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    }
                    view = img ? SampledView(*img, info.texture_dim[slot], info.texture_array[slot], swz, nullptr) : VK_NULL_HANDLE;
                    if (!view) return Skip("texture view does not match the shader");
                    view_standins_++;
                }
                if (plain)
                    tex_writes.push_back({uint32_t(stage), 32 + slot,
                                          {Sampler(bt.sampler, false, Filterable(*img)), view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}});
                if (shadow)
                    tex_writes.push_back({uint32_t(stage), 64 + slot,  // point-sampled: the shader filters (2x2 PCF)
                                          {Sampler(bt.sampler, true, info.texture_dim[slot] != shader::TexDim::D2 &&
                                                                         Filterable(*img)),
                                           view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}});
            }
        }

        // Storage images the fragment shader writes (SUST). Kept in GENERAL layout while bound.
        for (int stage = 0; stage < 2; stage++) {
            const shader::ShaderInfo& info = stage == 0 ? vs->info : fs->info;
            for (uint32_t slot = 0; slot < 8; slot++) {
                if (!(info.image_mask >> slot & 1)) continue;
                if (!features_.fragmentStoresAndAtomics || !features_.shaderStorageImageWriteWithoutFormat)
                    return Skip("storage image writes not supported by the GPU");
                const TexRef& ref = st_.images[stage][slot];
                Image* img = ref.valid ? GetImage(ref, false) : nullptr;
                if (!img) return Skip("storage image not bound");
                if (!(img->features & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) return Skip("storage image format not supported");
                if (!img->storage && !AddStorage(*img)) return Skip("storage image could not be re-created");
                bool conflict = false;
                for (Image* c : colors) conflict |= c == img;
                if (conflict || depth == img) {
                    // The game leaves the image bound as a colour target while the shader writes it directly
                    // (the shader has no colour outputs). Draw without attachments, sized like the image.
                    if (stage != 1 || fs->info.output_mask != 0) return Skip("storage image is also a render target");
                    colors.clear();
                    if (depth == img) depth = nullptr;
                    no_attachment_extent_ = {uint32_t(img->width), uint32_t(img->height)};
                }
                VkImageViewType type = info.image_dim[slot] == shader::TexDim::D3 ? VK_IMAGE_VIEW_TYPE_3D
                                       : info.image_dim[slot] == shader::TexDim::D1
                                           ? (info.image_array[slot] ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D)
                                           : (info.image_array[slot] ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D);
                const bool type_ok = (type == VK_IMAGE_VIEW_TYPE_3D) == (img->type == VK_IMAGE_TYPE_3D) &&
                                     (info.image_dim[slot] == shader::TexDim::D1) == (img->type == VK_IMAGE_TYPE_1D);
                if (!type_ok) return Skip("storage image dimension does not match the shader");
                if (img->layout != VK_IMAGE_LAYOUT_GENERAL || img->clear_pending) {
                    EndRendering();
                    Transition(*img, VK_IMAGE_LAYOUT_GENERAL);
                }
                img->written = true;
                NoteWritten(img);
                static const uint8_t kIdentity[4] = {2, 3, 4, 5};
                const uint32_t layers = info.image_array[slot] ? uint32_t(img->layers) : 1;
                VkImageView view = View(*img, type, 0, layers, 1, img->aspect, kIdentity);
                if (!view) return Skip("storage image view failed");
                tex_writes.push_back({uint32_t(stage), 96 + slot, {VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL}});
            }
        }
        Phase(2);
        // ---- uniform data ------------------------------------------------------------------------------------
        const bool dyn_stage[2] = {DynamicUniforms(vs->info), DynamicUniforms(fs->info)};
        std::vector<UboWrite>& ubo_writes = scratch_ubo_writes_;
        ubo_writes.clear();
        const VkDeviceSize ubo_align = std::max<VkDeviceSize>(16, props_.limits.minUniformBufferOffsetAlignment);
        for (int stage = 0; stage < 2; stage++) {
            const shader::ShaderInfo& info = stage == 0 ? vs->info : fs->info;
            if (!st_.program[stage]) continue;
            const ShaderCode& code = *st_.program[stage];
            for (uint32_t n = 0; n < 18; n++) {
                if (!(info.cbuf_mask >> n & 1)) continue;
                const uint8_t* src = nullptr;
                size_t size = 0;
                if (n == kCompilerConstants) {
                    ShaderConstants(code, &src, &size);
                } else if (n >= kCbufBase) {
                    const BufferRange& r = st_.uniforms[stage][n - kCbufBase];
                    size = size_t(std::min<uint64_t>(r.size, kMaxUniformRange));
                    // Only what the shader can read.
                    if (info.cbuf_bytes[n]) size = std::min<size_t>(size, info.cbuf_bytes[n]);
                    if (r.address && size && GuestRangeValid(r.address, size))
                        src = reinterpret_cast<const uint8_t*>(r.address);
                    else
                        size = 0;
                }
                VkDescriptorBufferInfo bi{stream_, zero_offset_, kMaxUniformRange};
                if (src && size) {
                    // Pad to 16 bytes from guest memory when possible (the shader reads whole vec4s).
                    size_t padded = (size + 15) & ~size_t(15);
                    if (padded != size && n != kCompilerConstants && GuestRangeValid(uint64_t(uintptr_t(src)), padded)) size = padded;
                    VkDeviceSize off;
                    // `exposure` setting: the tone-mapping pass's exposure constant (fs c3[56].z) is scaled. The
                    // data gets its own copy (cached uploads are shared between draws).
                    const float exposure = dsr::GetRuntime().config.exposure;
                    const bool scale_exposure = exposure != 1.0f && stage == 1 && n == 3 &&
                                                code.hash == kToneMapShaderHash && size >= 56 * 16 + 12;
                    if (scale_exposure) {
                        off = StreamAlloc(padded, ubo_align);
                        if (off != VK_WHOLE_SIZE) {
                            memset(stream_ptr_ + off, 0, padded);
                            memcpy(stream_ptr_ + off, src, size);
                            float e;
                            memcpy(&e, stream_ptr_ + off + 56 * 16 + 8, 4);
                            e *= exposure;
                            memcpy(stream_ptr_ + off + 56 * 16 + 8, &e, 4);
                        }
                    } else if (size == padded) {
                        off = UploadCached(src, size, ubo_align);
                    } else {
                        off = StreamAlloc(padded, ubo_align);
                        if (off != VK_WHOLE_SIZE) {
                            memset(stream_ptr_ + off, 0, padded);
                            memcpy(stream_ptr_ + off, src, size);
                        }
                    }
                    if (off != VK_WHOLE_SIZE) bi = {stream_, off, padded};
                } else {
                    MissingUniform(stage, n);
                }
                if (dyn_stage[stage]) {
                    // Fixed descriptor (whole range from 0); the offset goes in at bind time.
                    ubo_writes.push_back({uint32_t(stage), n, {stream_, 0, kMaxUniformRange}, true, uint32_t(bi.offset)});
                } else {
                    ubo_writes.push_back({uint32_t(stage), n, bi, false, 0});
                }
                if (capture_active_ && src && size && n >= kCbufBase) {
                    if (ubo_desc_draw_ != drawn_) {
                        pass_ubo_desc_.clear();
                        ubo_desc_draw_ = drawn_;
                    }
                    char d[64];
                    snprintf(d, sizeof(d), " %s c%u(%zu B):", stage ? "fs" : "vs", n, size);
                    pass_ubo_desc_ += d;
                    const size_t nf = std::min<size_t>(size / 4, 1024);  // all of it (brightness hunt)
                    for (size_t k = 0; k < nf; k++) {
                        float f;
                        memcpy(&f, src + k * 4, 4);
                        snprintf(d, sizeof(d), " %g", f);
                        pass_ubo_desc_ += d;
                    }
                }
            }
        }

        Phase(3);
        // ---- vertex + index data -------------------------------------------------------------------------------
        // Fast path: the game's own index and vertex buffers are uploaded as they are (each once per list) and
        // the GPU applies the base vertex. Needs u16/u32 indices (or none), a topology Vulkan has, and bound
        // vertex buffers of a reasonable size. Otherwise indices are scanned and rewritten (slow path).
        std::vector<uint32_t> expanded;  // converted index list (slow path)
        const uint64_t geo_resets_before = geo_resets_;
        VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        bool use_indices = false, fast = false;
        VkIndexType index_type = VK_INDEX_TYPE_UINT32;
        uint32_t index_count = 0, first_vertex = 0;
        int32_t vertex_offset = 0;
        VkDeviceSize vb_offsets[16] = {};
        VkBuffer vb_bufs[16] = {};
        VkDeviceSize index_off = 0;
        VkBuffer index_buf = stream_;
        {
            static const int8_t kTopology[14] = {
                VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, -1, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP,
                VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN,
                -1, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN,
                VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY,
                VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY};
            fast = p.mode >= 0 && p.mode < 14 && kTopology[p.mode] >= 0 && (p.index_type < 0 || p.index_type == 1 || p.index_type == 2);
            const uint32_t isz = p.index_type == 1 ? 2 : 4;
            if (fast && p.index_type >= 0 && !GuestRangeValid(p.index_address, size_t(p.count) * isz)) fast = false;
            // Vertex range the draw reads: from the (cached) index scan, or first..first+count.
            int64_t vmin = p.first, vmax = int64_t(p.first) + p.count - 1;
            if (fast && p.index_type >= 0) {
                uint32_t lo, hi;
                IndexRange(p.index_address, uint32_t(p.count), isz, &lo, &hi);
                vmin = int64_t(lo) + p.base_vertex;
                vmax = int64_t(hi) + p.base_vertex;
                if (vmin < 0 || vmax - vmin > (8 << 20)) fast = false;
            }
            for (const VkVertexInputBindingDescription& b : vin.bindings) {
                if (!fast) break;
                const BufferRange& r = st_.vertex_buffers[b.binding];
                const VertexStreamObj so = b.binding < st_.streams.size() ? st_.streams[b.binding] : VertexStreamObj{};
                const int64_t n = so.divisor ? (int64_t(p.instances) + so.divisor - 1) / so.divisor : vmax - vmin + 1;
                const uint64_t bytes = uint64_t(std::max<int64_t>(0, n - 1)) * b.stride + vin.extent[b.binding];
                if (!r.address || bytes > (64u << 20)) {
                    fast = false;
                    break;
                }
            }
            if (fast) {
                topology = VkPrimitiveTopology(kTopology[p.mode]);
                for (const VkVertexInputBindingDescription& b : vin.bindings) {
                    const BufferRange& r = st_.vertex_buffers[b.binding];
                    const VertexStreamObj so = b.binding < st_.streams.size() ? st_.streams[b.binding] : VertexStreamObj{};
                    const int64_t first = so.divisor ? p.base_instance : vmin;
                    const int64_t n = so.divisor ? (int64_t(p.instances) + so.divisor - 1) / so.divisor : vmax - vmin + 1;
                    const uint64_t start = uint64_t(first) * b.stride;
                    const uint64_t bytes = uint64_t(std::max<int64_t>(0, n - 1)) * b.stride + vin.extent[b.binding];
                    VkDeviceSize off;
                    if (start + bytes <= r.size && GuestRangeValid(r.address + start, size_t(bytes))) {
                        const uint8_t* src = reinterpret_cast<const uint8_t*>(r.address + start);
                        off = GeoCached(src, size_t(bytes));
                        vb_bufs[b.binding] = geo_buf_;
                        if (off == VK_WHOLE_SIZE) {
                            off = UploadCached(src, size_t(bytes), 16);
                            vb_bufs[b.binding] = stream_;
                        }
                    } else {
                        off = UploadPadded(r, start, bytes);
                        vb_bufs[b.binding] = stream_;
                    }
                    if (off == VK_WHOLE_SIZE) return Skip("vertex data too large");
                    // Instanced streams start at base_instance: the draw then uses firstInstance 0.
                    vb_offsets[b.binding] = off;
                }
                vertex_offset = int32_t(int64_t(p.base_vertex) - vmin);  // indexed: index + base - vmin
                first_vertex = uint32_t(int64_t(p.first) - vmin);        // non-indexed: 0
                if (p.index_type >= 0) {
                    use_indices = true;
                    index_type = p.index_type == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
                    index_count = uint32_t(p.count);
                    const uint8_t* isrc = reinterpret_cast<const uint8_t*>(p.index_address);
                    index_off = GeoCached(isrc, size_t(p.count) * isz);
                    index_buf = geo_buf_;
                    if (index_off == VK_WHOLE_SIZE) {
                        index_off = UploadCached(isrc, size_t(p.count) * isz, 4);
                        index_buf = stream_;
                    }
                    if (index_off == VK_WHOLE_SIZE) return Skip("index data too large");
                }
                fast_draws_++;
            }
        }
        if (!fast) {
            int topology_mode = p.mode;
            int64_t min_vertex = 0, max_vertex = 0;
            bool indexed = p.index_type >= 0;
            if (indexed) {
                const uint32_t isz = p.index_type == 0 ? 1 : p.index_type == 1 ? 2 : 4;
                if (!GuestRangeValid(p.index_address, size_t(p.count) * isz)) return Skip("index data outside known memory");
                const uint8_t* ib = reinterpret_cast<const uint8_t*>(p.index_address);
                expanded.resize(size_t(p.count));
                min_vertex = INT64_MAX;
                max_vertex = INT64_MIN;
                for (int i = 0; i < p.count; i++) {
                    uint32_t v = isz == 1 ? ib[i] : isz == 2 ? reinterpret_cast<const uint16_t*>(ib)[i]
                                                             : reinterpret_cast<const uint32_t*>(ib)[i];
                    expanded[size_t(i)] = v;
                    int64_t vv = int64_t(v) + p.base_vertex;
                    min_vertex = std::min(min_vertex, vv);
                    max_vertex = std::max(max_vertex, vv);
                }
                if (min_vertex < 0) return Skip("negative vertex index");
                for (uint32_t& v : expanded) v = uint32_t(int64_t(v) + p.base_vertex - min_vertex);
            } else {
                min_vertex = p.first;
                max_vertex = int64_t(p.first) + p.count - 1;
                expanded.resize(size_t(p.count));
                for (int i = 0; i < p.count; i++) expanded[size_t(i)] = uint32_t(i);
            }
            if (max_vertex - min_vertex > (8 << 20)) return Skip("vertex range too large");
            // Primitive conversions Vulkan lacks.
            use_indices = indexed;
            switch (topology_mode) {
                case 0: topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
                case 1: topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
                case 2: {  // line loop -> strip with the first vertex repeated
                    topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
                    expanded.push_back(expanded.front());
                    use_indices = true;
                    break;
                }
                case 3: topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
                case 4: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
                case 5: case 8: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
                case 6: case 9: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; break;
                case 7: {  // quads -> triangles
                    topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
                    std::vector<uint32_t> tris;
                    for (size_t q = 0; q + 3 < expanded.size(); q += 4) {
                        const uint32_t* v = &expanded[q];
                        tris.insert(tris.end(), {v[0], v[1], v[2], v[0], v[2], v[3]});
                    }
                    expanded.swap(tris);
                    use_indices = true;
                    break;
                }
                case 10: topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY; break;
                case 11: topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY; break;
                case 12: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY; break;
                case 13: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY; break;
                default: return Skip("unsupported primitive type");
            }
            if (expanded.empty()) return;
            index_count = uint32_t(expanded.size());

            for (const VkVertexInputBindingDescription& b : vin.bindings) {
                const BufferRange& r = st_.vertex_buffers[b.binding];
                const VertexStreamObj so = b.binding < st_.streams.size() ? st_.streams[b.binding] : VertexStreamObj{};
                int64_t first = so.divisor ? p.base_instance : min_vertex;
                int64_t n = so.divisor ? (int64_t(p.instances) + so.divisor - 1) / so.divisor : max_vertex - min_vertex + 1;
                const uint64_t start = uint64_t(first) * b.stride;
                const uint64_t bytes = uint64_t(std::max<int64_t>(0, n - 1)) * b.stride + vin.extent[b.binding];
                if (!r.address) return Skip("vertex buffer not bound");
                VkDeviceSize off = start + bytes <= r.size && GuestRangeValid(r.address + start, size_t(bytes))
                                       ? UploadCached(reinterpret_cast<const uint8_t*>(r.address + start), size_t(bytes), 16)
                                       : UploadPadded(r, start, bytes);
                if (off == VK_WHOLE_SIZE) return Skip("vertex data too large");
                vb_offsets[b.binding] = off;
                vb_bufs[b.binding] = stream_;
            }
            if (use_indices) {
                index_off = StreamAlloc(expanded.size() * 4, 4);
                memcpy(stream_ptr_ + index_off, expanded.data(), expanded.size() * 4);
            }
        }
        // The geometry cache was emptied during this draw (a new command buffer, other offsets now stale): redo it.
        if (geo_resets_ != geo_resets_before) return Draw(p);
        Phase(4);
        // ---- pipeline ----------------------------------------------------------------------------------------
        VkExtent2D extent;
        // Formats the pipeline renders to (dynamic rendering).
        std::vector<VkFormat>& color_formats = scratch_color_formats_;
        color_formats.clear();
        for (size_t i = 0; i < colors.size(); i++) {
            Image* c = colors[i];
            VkFormat f = c ? c->vk_format : VK_FORMAT_UNDEFINED;
            if (c && i < st_.rts.size() && st_.rts[i].view_format) {  // rendered through a view of another format
                const VkFormat vf = ViewFormat(*c, st_.rts[i].view_format);
                if (vf != VK_FORMAT_UNDEFINED) f = vf;
            }
            color_formats.push_back(f);
        }
        VkFormat depth_format = depth ? depth->vk_format : VK_FORMAT_UNDEFINED;
        VkDescriptorSetLayout vsl = SetLayout(VK_SHADER_STAGE_VERTEX_BIT, vs->info);
        VkDescriptorSetLayout fsl = SetLayout(VK_SHADER_STAGE_FRAGMENT_BIT, fs->info);
        VkPipelineLayout layout = PipelineLayout(vsl, fsl);
        // `hole_test` 1/3 (diagnostic for the square holes): the smoke / fog / glow shaders draw without the depth
        // test, to tell a bad depth buffer from a bad depth texture.
        const bool saved_depth_test = st_.depth.depth_test;
        const auto saved_blend_enable = st_.color.blend_enable;
        const auto saved_mask = st_.channel.mask;
        if (shader_depth_test) st_.depth.depth_test = false;  // done in the fragment shader
        if (debug_fx) {
            st_.depth.depth_test = false;
            st_.color.blend_enable = 0;
            st_.channel.mask = 0xffffffffu;
        }
        // `hole_test` 2048: no depth test for any draw into a reduced-size target that tests without writing.
        if ((dsr::GetRuntime().config.hole_test & 2048) && depth && depth->width <= 640 && !st_.depth.depth_write)
            st_.depth.depth_test = false;
        if ((dsr::GetRuntime().config.hole_test & 1) && st_.program[1] &&
            (st_.program[1]->hash == 0x6f634579337b2b69ull || st_.program[1]->hash == 0x3fc1b1baa8114318ull))
            st_.depth.depth_test = false;
        VkPipeline pipeline = GetPipeline(vs, fs, vin, topology, color_formats, depth ? depth->nvn_format : 0,
                                          depth_format, layout);
        st_.depth.depth_test = saved_depth_test;
        st_.color.blend_enable = saved_blend_enable;
        st_.channel.mask = saved_mask;
        if (!pipeline) {
            if (pipeline_pending_) return;  // compiling in the background: drawn once it is ready
            return Skip("pipeline creation failed");
        }

        Phase(5);
        // ---- queue the draw: the recorder thread builds its descriptor sets and records it --------------------
        if (!EnsureRendering(colors, depth, &extent)) return Skip("no render target");
        if (depth && fs->info.writes_depth && st_.depth.depth_write) depth->depth_exported = depth->fragdepth_written = true;
        DrawPacket d;
        d.pipeline = pipeline;
        d.layout = layout;
        d.set_layouts[0] = vsl;
        d.set_layouts[1] = fsl;
        DynamicState(extent, modes, &d.dyn);
        // Other aspect ratios (aspect_ratio): the game lays its HUD and menus out for 1280x720 and stretches that
        // over the whole (wider or narrower) frame. Every draw made with that 1280x720 2D projection (several
        // shaders: quads, text, bars) goes into a centred 16:9 box instead: 1280x720 in the middle of wider frames,
        // the full width and 9/16 of it high on narrower ones, so the HUD keeps its shape and layout. Full-screen
        // quads (fades, dimming behind menus) still cover the whole frame.
        if (hud_box_w_ && int(extent.height) == dsr::GetRuntime().config.render_height &&
            int(extent.width) == dsr::GetRuntime().config.render_width && HudBoxed(p, vin)) {
            const int fh = int(extent.height);
            const float sx = float(hud_box_w_) / float(extent.width), sy = float(hud_box_h_) / float(fh);
            const float x0 = float((int(extent.width) - hud_box_w_) / 2), y0 = float((fh - hud_box_h_) / 2);
            d.dyn.vp.x = x0 + d.dyn.vp.x * sx;
            d.dyn.vp.width *= sx;
            d.dyn.vp.y = y0 + d.dyn.vp.y * sy;
            d.dyn.vp.height *= sy;
            // Clip rectangles (menu text windows) are in the game's 1280x720 layout units, like the vertices; one
            // covering the whole frame (0,0 and the frame size) is the default and becomes the whole box.
            VkRect2D& sc = d.dyn.sc;
            float l = x0, t = y0, r = x0 + float(hud_box_w_), b = y0 + float(hud_box_h_);
            if (st_.scissor_set && st_.scissor[2] > 0 && st_.scissor[3] > 0 &&
                !(st_.scissor[0] <= 0 && st_.scissor[1] <= 0 && st_.scissor[2] >= int(extent.width) &&
                  st_.scissor[3] >= int(extent.height))) {
                const float kx = float(hud_box_w_) / 1280.0f, ky = float(hud_box_h_) / 720.0f;
                l = std::max(l, x0 + float(st_.scissor[0]) * kx);
                t = std::max(t, y0 + float(st_.scissor[1]) * ky);
                // The game cuts its rectangles at the frame's edges (960 on a 4:3 frame, 540 high at a 0.75 render
                // scale), which in layout units are not the layout's edges: one ending there runs to them.
                const float right = st_.scissor[0] + st_.scissor[2] >= float(extent.width) - 0.5f
                                        ? 1280.0f : st_.scissor[0] + st_.scissor[2];
                const float bottom = st_.scissor[1] + st_.scissor[3] >= float(fh) - 0.5f
                                         ? 720.0f : st_.scissor[1] + st_.scissor[3];
                r = std::min(r, x0 + right * kx);
                b = std::min(b, y0 + bottom * ky);
            }
            const int il = int(std::floor(l)), it = int(std::floor(t)), ir = int(std::ceil(r)), ib = int(std::ceil(b));
            sc.offset = {il, it};
            sc.extent = {uint32_t(std::max(0, ir - il)), uint32_t(std::max(0, ib - it))};
            hud_draws_boxed_++;
        }
        d.push[0] = st_.alpha_ref;
        d.indexed = use_indices;
        d.index_buf = fast ? index_buf : stream_;
        d.index_off = index_off;
        d.index_type = fast ? index_type : VK_INDEX_TYPE_UINT32;
        d.count = use_indices ? index_count : uint32_t(p.count);
        d.first_vertex = fast ? first_vertex : 0;
        d.vertex_offset = fast ? vertex_offset : 0;
        d.instances = uint32_t(p.instances);
        // Instanced streams were uploaded from base_instance on, so the GPU must start at instance 0 (it read
        // them at twice the offset, past the uploaded data).
        bool instanced = false;
        for (const VkVertexInputBindingDescription& b : vin.bindings)
            instanced |= b.inputRate == VK_VERTEX_INPUT_RATE_INSTANCE;
        d.base_instance = instanced ? 0 : uint32_t(p.base_instance);
        if (capture_active_ && query_pool_ && query_pass_.size() < kMaxQueries) {
            d.query = uint32_t(query_pass_.size());
            query_pass_.push_back(capture_pass_);
        }
        if (capture_active_ && capture_draws_logged_ < 1500) {
            capture_draws_logged_++;
            std::string desc;
            char b[320];
            snprintf(b, sizeof(b), "capture draw q%d (pass %u): vs %016llx, %s count %d, %d instances (base %d), base vertex %d, "
                     "first %d, viewport %g,%g %gx%g scissor %d,%d %ux%u on %ux%u | streams:", d.query == UINT32_MAX ? -1 : int(d.query),
                     capture_pass_, (unsigned long long)st_.program[0]->hash, p.index_type >= 0 ? "indexed" : "arrays",
                     p.count, p.instances, p.base_instance, p.base_vertex, p.first, d.dyn.vp.x, d.dyn.vp.y, d.dyn.vp.width,
                     d.dyn.vp.height, d.dyn.sc.offset.x, d.dyn.sc.offset.y, d.dyn.sc.extent.width, d.dyn.sc.extent.height,
                     extent.width, extent.height);
            desc = b;
            snprintf(b, sizeof(b), " | depth test %d write %d func %d%s", int(st_.depth.depth_test || shader_depth_test),
                     int(st_.depth.depth_write), int(st_.depth.depth_func), shader_depth_test ? " (in the shader)" : "");
            desc += b;
            for (const VkVertexInputBindingDescription& vb_ : vin.bindings) {
                const BufferRange& r = st_.vertex_buffers[vb_.binding];
                snprintf(b, sizeof(b), " s%u stride %u%s @%llx size %llu", vb_.binding, vb_.stride,
                         vb_.inputRate == VK_VERTEX_INPUT_RATE_INSTANCE ? " (instanced)" : "", (unsigned long long)r.address,
                         (unsigned long long)r.size);
                desc += b;
            }
            desc += " | attribs:";
            for (const VkVertexInputAttributeDescription& a : vin.attributes) {
                snprintf(b, sizeof(b), " %u:s%u fmt %d +%u", a.location, a.binding, st_.attribs[a.location].format, a.offset);
                desc += b;
            }
            // Geometry dump (square holes in fog / smoke / glow): for the effect fragment shaders, the draw's index
            // data, the vertices it reads and the vertex shader's c3, so the vertex shader can be run offline on them.
            {
                const uint64_t fs_hash = st_.program[1] ? st_.program[1]->hash : 0;
                const bool effect = fs_hash == 0x6f634579337b2b69ull || fs_hash == 0x3fc1b1baa8114318ull;
                // Map geometry (stretched ground in 1.0.3): its vertex shaders, a few draws.
                const uint64_t vs_hash = st_.program[0] ? st_.program[0]->hash : 0;
                const bool map = (vs_hash == 0xbcef43496f5e76f0ull || vs_hash == 0xe45c8b7260213764ull ||
                                  vs_hash == 0x418df1fd521dac42ull) && map_geo_dumps_ < 8;
                const int isz = p.index_type == 0 ? 1 : p.index_type == 1 ? 2 : p.index_type == 2 ? 4 : 0;
                if (map && isz && p.count > 0 && p.count <= 65536) map_geo_dumps_++;
                if (((effect && extent.width <= 640) || map) && geo_dumps_ < 40 && isz && p.count > 0 &&
                    p.count <= (map ? 65536 : 16384) && p.index_address &&
                    GuestRangeValid(p.index_address, size_t(p.count) * isz) && !vin.bindings.empty()) {
                    geo_dumps_++;
                    const VkVertexInputBindingDescription& vb0 = vin.bindings[0];
                    const BufferRange& r = st_.vertex_buffers[vb0.binding];
                    const uint8_t* ip = reinterpret_cast<const uint8_t*>(p.index_address);
                    uint32_t lo = UINT32_MAX, hi = 0;
                    for (int k = 0; k < p.count; k++) {
                        uint32_t v = isz == 1 ? ip[k] : isz == 2 ? reinterpret_cast<const uint16_t*>(ip)[k]
                                                                  : reinterpret_cast<const uint32_t*>(ip)[k];
                        lo = std::min(lo, v);
                        hi = std::max(hi, v);
                    }
                    const uint64_t vstart = uint64_t(int64_t(lo) + p.base_vertex) * vb0.stride;
                    const uint64_t vbytes = uint64_t(hi - lo + 1) * vb0.stride;
                    const BufferRange& c3 = st_.uniforms[0][0];
                    LOGI(kTag, "capture geo q%d: mode %d, index size %d, count %d, base vertex %d, vertices %u..%u stride %u, "
                         "vs c3 %llu bytes", d.query == UINT32_MAX ? -1 : int(d.query), p.mode, isz, p.count, p.base_vertex,
                         lo, hi, vb0.stride, (unsigned long long)c3.size);
                    const auto dump = [&](const char* what, uint64_t addr, uint64_t bytes) {
                        if (!addr || bytes > (1u << 20) || !GuestRangeValid(addr, size_t(bytes))) {
                            LOGI(kTag, "capture geo q%d %s: unavailable", int(d.query), what);
                            return;
                        }
                        static const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                        const uint8_t* src = reinterpret_cast<const uint8_t*>(addr);
                        std::string out;
                        out.reserve(size_t(bytes) * 4 / 3 + 4);
                        for (uint64_t k = 0; k < bytes; k += 3) {
                            uint32_t v = uint32_t(src[k]) << 16;
                            if (k + 1 < bytes) v |= uint32_t(src[k + 1]) << 8;
                            if (k + 2 < bytes) v |= src[k + 2];
                            out += kB64[v >> 18 & 63];
                            out += kB64[v >> 12 & 63];
                            out += k + 1 < bytes ? kB64[v >> 6 & 63] : '=';
                            out += k + 2 < bytes ? kB64[v & 63] : '=';
                        }
                        for (size_t at = 0, part = 0; at < out.size(); at += 3000, part++)
                            LOGI(kTag, "capture geo q%d %s %zu: %s", int(d.query), what, part, out.substr(at, 3000).c_str());
                    };
                    dump("indices", p.index_address, uint64_t(p.count) * isz);
                    dump("vertices", r.address + vstart, vbytes);
                    dump("c3", c3.address, c3.size);
                }
            }
            desc += " | uniforms:";
            for (int stage = 0; stage < 2; stage++)
                for (int n = 0; n < 15; n++)
                    if (st_.uniforms[stage][n].address) {
                        snprintf(b, sizeof(b), " %s%d %llu", stage ? "fs" : "vs", n + int(kCbufBase),
                                 (unsigned long long)st_.uniforms[stage][n].size);
                        desc += b;
                    }
            LOGI(kTag, "%s", desc.c_str());
        }
        VbBind vb[16];
        for (const VkVertexInputBindingDescription& b : vin.bindings)
            vb[d.n_vb++] = {b.binding, vb_bufs[b.binding], vb_offsets[b.binding]};
        QueueDraw(d, vb, tex_writes, ubo_writes);
        // `hole_test` 8 (square holes): every draw into a reduced-size target gets a render pass of its own.
        if ((dsr::GetRuntime().config.hole_test & 8) && extent.width <= 640) EndRendering();
        if (capture_active_) {
            char d[320];
            snprintf(d, sizeof(d),
                     "vs %016llx | %zu colour targets | alpha test func %d ref %g | blend enable 0x%x | channel mask 0x%08x"
                     " | cull %d front %d | depth test %d write %d | stencil %d",
                     (unsigned long long)(st_.program[0] ? st_.program[0]->hash : 0), colors.size(), st_.color.alpha_func,
                     st_.alpha_ref, st_.color.blend_enable, st_.channel.mask, st_.polygon.cull, st_.polygon.front_face,
                     int(st_.depth.depth_test), int(st_.depth.depth_write), int(st_.depth.stencil_test));
            pass_state_desc_ = d;
            pass_state_desc_ += " | targets:";
            for (size_t i = 0; i < colors.size(); i++) {
                if (!colors[i]) {
                    pass_state_desc_ += " -";
                    continue;
                }
                char t[96];
                snprintf(t, sizeof(t), " %zu:%dx%d f%d @%llx", i, colors[i]->width, colors[i]->height,
                         colors[i]->nvn_format, (unsigned long long)colors[i]->addr);
                pass_state_desc_ += t;
            }
            if (depth) {
                char t[96];
                snprintf(t, sizeof(t), " depth:%dx%d f%d @%llx", depth->width, depth->height, depth->nvn_format,
                         (unsigned long long)depth->addr);
                pass_state_desc_ += t;
            }
        }
        // Diagnostics (darkness): the first few lit world draws of a capture (shadow map + cube map sampled)
        // log their textures and constants, and their sampled textures are captured.
        if (capture_active_ && world_draws_logged_ < 3 && fs->info.shadow_mask) {
            bool cube = false;
            for (int k = 0; k < 32; k++) cube |= fs->info.texture_dim[k] == shader::TexDim::Cube;
            if (cube) {
                world_draws_logged_++;
                LOGI(kTag, "capture world draw %d (pass %u): fs code @%llx vs code @%llx, textures:%s", world_draws_logged_,
                     capture_pass_, (unsigned long long)(st_.program[1] ? st_.program[1]->address : 0),
                     (unsigned long long)(st_.program[0] ? st_.program[0]->address : 0), pass_tex_desc_.c_str());
                for (size_t at = 0; at < pass_ubo_desc_.size(); at += 2800)
                    LOGI(kTag, "capture world draw %d constants%s:%s", world_draws_logged_, at ? " (cont.)" : "",
                         pass_ubo_desc_.substr(at, 2800).c_str());
                for (const DrawImage& di : draw_images_) {
                    if (draw_images_draw_ != drawn_ || di.stage != 1) continue;
                    Image& im = *di.img;
                    // Per layer, the mean of each channel of the game's own copy (deswizzled with our layout code):
                    // shows whether the data is there and whether our layer layout finds it.
                    std::string means;
                    const FormatInfo fi = GetFormatInfo(im.nvn_format);
                    const TextureLayout lay = ComputeLayout(im.tex);
                    if (fi.bytes_per_block == 4 && fi.block_w == 1 && !lay.levels.empty() && !IsDepthFormat(im.nvn_format) &&
                        GuestRangeValid(im.addr, lay.total)) {
                        const LevelLayout& l = lay.levels[0];
                        std::vector<uint8_t> lin(size_t(l.row_bytes) * size_t(l.rows) * size_t(std::max(1, l.depth)));
                        for (int layer = 0; layer < std::min(lay.layers, 6); layer++) {
                            const uint8_t* base = reinterpret_cast<const uint8_t*>(im.addr) + size_t(layer) * lay.layer_stride + l.offset;
                            if (lay.linear) memcpy(lin.data(), base, lin.size());
                            else DeswizzleLevel(lin.data(), base, l);
                            double m[4] = {0, 0, 0, 0};
                            for (size_t q = 0; q + 3 < lin.size(); q += 4)
                                for (int c = 0; c < 4; c++) m[c] += lin[q + c];
                            const double n = double(lin.size() / 4);
                            char b[96];
                            snprintf(b, sizeof(b), " [layer %d: %.1f %.1f %.1f %.1f]", layer, m[0] / n, m[1] / n, m[2] / n, m[3] / n);
                            means += b;
                        }
                    }
                    LOGI(kTag, "capture world draw %d texture fs%d: NVN format %d %dx%d target %d, %d layers (stride %zu), "
                               "%d levels -> item %zu; game memory means:%s",
                         world_draws_logged_, di.slot, im.nvn_format, im.width, im.height, im.target, im.layers,
                         lay.layer_stride, im.levels, capture_items_.size(), means.empty() ? " n/a" : means.c_str());
                    CaptureImage(im);
                }
            }
        }
        drawn_++;
        pass_draws_++;
        if (st_.program[1]) {
            pass_fs_hash_ = st_.program[1]->hash;
            pass_fs_addr_ = st_.program[1]->address;
        }
        if (st_.program[0]) pass_vs_addr_ = st_.program[0]->address;
        if (depth && st_.depth.depth_test && st_.depth.depth_write) depth_gen_++;
        if (drawn_ == 1) LOGI(kTag, "first draw recorded");
    }

    // ---- the recorder's half of a draw ---------------------------------------------------------------------
    struct VbBind {
        uint32_t binding;
        VkBuffer buf;
        VkDeviceSize off;
    };
    // A draw as queued: the packet, then its vertex bindings, texture writes and uniform writes.
    void QueueDraw(DrawPacket& d, const VbBind* vb, const std::vector<TexWrite>& tw, const std::vector<UboWrite>& uw) {
        d.n_tex = uint16_t(tw.size());
        d.n_ubo = uint16_t(uw.size());
        if (!Async()) {
            Drain();
            RecordDraw(d, vb, tw.data(), uw.data());
            return;
        }
        const size_t head = (sizeof(DrawPacket) + 15) & ~size_t(15);
        const size_t vb_bytes = sizeof(VbBind) * d.n_vb, tw_bytes = sizeof(TexWrite) * d.n_tex;
        uint8_t* p = RecAlloc(head + vb_bytes + tw_bytes + sizeof(UboWrite) * d.n_ubo, [](VulkanBackend* self, uint8_t* q) {
            const DrawPacket& d = *reinterpret_cast<const DrawPacket*>(q);
            const uint8_t* at = q + ((sizeof(DrawPacket) + 15) & ~size_t(15));
            const auto* vb = reinterpret_cast<const VbBind*>(at);
            const auto* tw = reinterpret_cast<const TexWrite*>(at + sizeof(VbBind) * d.n_vb);
            const auto* uw = reinterpret_cast<const UboWrite*>(at + sizeof(VbBind) * d.n_vb + sizeof(TexWrite) * d.n_tex);
            self->RecordDraw(d, vb, tw, uw);
        }, nullptr);
        memcpy(p, &d, sizeof(d));
        memcpy(p + head, vb, vb_bytes);
        memcpy(p + head + vb_bytes, tw.data(), tw_bytes);
        memcpy(p + head + vb_bytes + tw_bytes, uw.data(), sizeof(UboWrite) * d.n_ubo);
        MaybePublish();
    }
    // Transient descriptor sets; a full pool continues in another one (reset with the slot).
    bool AllocTransientSets(const VkDescriptorSetLayout layouts[2], VkDescriptorSet sets[2]) {
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorSetCount = 2;
        dai.pSetLayouts = layouts;
        for (int tries = 0; tries < 3; tries++) {
            dai.descriptorPool = desc_pool_;
            if (vkAllocateDescriptorSets(device_, &dai, sets) == VK_SUCCESS) return true;
            Slot& sl = slots_[rec_slot_];
            if (sl.extra_used >= sl.extra_pools.size()) {
                const VkDescriptorPool np = MakeDescriptorPool();
                if (!np) return false;
                sl.extra_pools.push_back(np);
                desc_pools_added_++;
            }
            desc_pool_ = sl.extra_pools[sl.extra_used++];
        }
        return false;
    }
    // Descriptor sets, bindings, dynamic state and the draw command (recorder thread, or inline).
    void RecordDraw(const DrawPacket& d, const VbBind* vb, const TexWrite* tex_writes, const UboWrite* ubo_writes) {
        const VkCommandBuffer cb = cmd_;
        VkDescriptorSet sets[2] = {};
        const VkDescriptorSetLayout* layouts = d.set_layouts;
        // Identical bindings (same layouts, buffers, offsets, images and samplers) reuse one pair of sets within
        // the list; the pool is reset with the list.
        Key64 dk;
        dk.Add(layouts[0]);
        dk.Add(layouts[1]);
        uint32_t dyn_offsets[36];
        uint32_t dyn_count = 0;
        bool persistable = true;
        for (uint32_t i = 0; i < d.n_ubo; i++) {
            const UboWrite& u = ubo_writes[i];
            dk.Add(u.set);
            dk.Add(u.binding);
            dk.Add(u.info);  // for dynamic ones: buffer, 0, range (the same for every draw of the list)
            if (u.dynamic && dyn_count < 36) dyn_offsets[dyn_count++] = u.dyn_offset;
            // Only sets whose uniform buffers all take dynamic offsets can outlive the list: a fixed offset
            // points at this list's data in the stream, which a later list overwrites.
            persistable &= u.dynamic;
        }
        for (uint32_t i = 0; i < d.n_tex; i++) {
            const TexWrite& t = tex_writes[i];
            dk.Add(t.set);
            dk.Add(t.binding);
            dk.Add(t.info.sampler);
            dk.Add(t.info.imageView);
            dk.Add(t.info.imageLayout);
        }
        auto cached_sets = desc_cache_.find(dk.h);
        bool reuse = cached_sets != desc_cache_.end();
        if (!reuse) {
            auto ps = persist_cache_.find(dk.h);
            if (ps != persist_cache_.end()) {
                cached_sets = desc_cache_.emplace(dk.h, ps->second).first;
                reuse = true;
                persist_hits_++;
            }
        }
        if (reuse) {
            sets[0] = cached_sets->second.first;
            sets[1] = cached_sets->second.second;
            desc_reused_++;
        } else {
            bool persistent = false;
            if (persist_pool_ && persistable && !persist_full_) {
                VkDescriptorSetAllocateInfo pai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                pai.descriptorPool = persist_pool_;
                pai.descriptorSetCount = 2;
                pai.pSetLayouts = layouts;
                if (vkAllocateDescriptorSets(device_, &pai, sets) == VK_SUCCESS)
                    persistent = true;
                else if (persist_cache_.size() > 64)
                    persist_full_ = true;  // emptied by the render thread after this list (the GPU must be idle)
            }
            if (!persistent && !AllocTransientSets(layouts, sets)) {
                skipped_desc_++;
                return;
            }
            VkWriteDescriptorSet writes[192];  // at most 36 uniform + 144 texture/image bindings
            uint32_t n = 0;
            for (uint32_t i = 0; i < d.n_ubo && n < 192; i++) {
                const UboWrite& u = ubo_writes[i];
                VkWriteDescriptorSet& w = writes[n++];
                w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                w.dstSet = sets[u.set];
                w.dstBinding = u.binding;
                w.descriptorCount = 1;
                w.descriptorType = u.dynamic ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                w.pBufferInfo = &u.info;
            }
            for (uint32_t i = 0; i < d.n_tex && n < 192; i++) {
                const TexWrite& t = tex_writes[i];
                VkWriteDescriptorSet& w = writes[n++];
                w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                w.dstSet = sets[t.set];
                w.dstBinding = t.binding;
                w.descriptorCount = 1;
                w.descriptorType = t.binding >= 96 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w.pImageInfo = &t.info;
            }
            if (n) vkUpdateDescriptorSets(device_, n, writes, 0, nullptr);
            desc_cache_[dk.h] = {sets[0], sets[1]};
            if (persistent) persist_cache_[dk.h] = {sets[0], sets[1]};
        }

        if (d.pipeline != bound_pipeline_) {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d.pipeline);
            bound_pipeline_ = d.pipeline;
        }
        if (d.layout != bound_layout_ || sets[0] != bound_sets_[0] || sets[1] != bound_sets_[1] ||
            dyn_count != bound_dyn_count_ || memcmp(dyn_offsets, bound_dyn_, dyn_count * 4) != 0) {
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d.layout, 0, 2, sets, dyn_count, dyn_offsets);
            if (d.layout != bound_layout_) push_valid_ = false;
            bound_layout_ = d.layout;
            bound_sets_[0] = sets[0];
            bound_sets_[1] = sets[1];
            bound_dyn_count_ = dyn_count;
            memcpy(bound_dyn_, dyn_offsets, dyn_count * 4);
        }
        for (uint32_t i = 0; i < d.n_vb; i++) {
            auto& bound = bound_vb_[vb[i].binding];
            if (bound.first == vb[i].buf && bound.second == vb[i].off) continue;
            vkCmdBindVertexBuffers(cb, vb[i].binding, 1, &vb[i].buf, &vb[i].off);
            bound = {vb[i].buf, vb[i].off};
        }
        if (!push_valid_ || memcmp(d.push, bound_push_, sizeof(d.push)) != 0) {
            vkCmdPushConstants(cb, d.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(d.push), d.push);
            memcpy(bound_push_, d.push, sizeof(d.push));
            push_valid_ = true;
        }
        ApplyDynamicState(d.dyn);
        if (d.query != UINT32_MAX) vkCmdBeginQuery(cb, query_pool_, d.query, 0);
        if (d.indexed) {
            if (bound_ib_.first != d.index_buf || bound_ib_.second != d.index_off || bound_ib_type_ != d.index_type) {
                vkCmdBindIndexBuffer(cb, d.index_buf, d.index_off, d.index_type);
                bound_ib_ = {d.index_buf, d.index_off};
                bound_ib_type_ = d.index_type;
            }
            vkCmdDrawIndexed(cb, d.count, d.instances, 0, d.vertex_offset, d.base_instance);
        } else {
            vkCmdDraw(cb, d.count, d.instances, d.first_vertex, d.base_instance);
        }
        if (d.query != UINT32_MAX) vkCmdEndQuery(cb, query_pool_, d.query);
    }

    // Viewport, scissor, blend constants and stencil values of the draw (render thread).
    void DynamicState(VkExtent2D extent, const DeviceModes& modes, DynState* out) {
        // Y direction. In NVN's upper-left origin mode (Direct3D style), clip-space +Y is the top of the target
        // (row 0); in Vulkan it is the bottom. A negative-height viewport makes the two agree, so images land in
        // memory the way the game expects, and front faces keep their winding as seen on screen. Without it,
        // every rasterised image is mirrored and every full-screen pass mirrors it again (the parity decides
        // what ends up upside down), and back-face culling is inverted.
        // In the OpenGL-style lower-left mode, row 0 is the bottom and clip +Y the highest row: as in Vulkan.
        const bool flip = modes.origin_mode == 1;
        float vx = 0, vy = 0, vw = float(extent.width), vh = float(extent.height);
        // A negative height (the game flipping Y itself) is kept: Vulkan accepts it the same way.
        if (st_.viewport_set && st_.viewport[2] > 0 && st_.viewport[3] != 0) {
            vx = st_.viewport[0];
            vy = st_.viewport[1];
            vw = st_.viewport[2];
            vh = st_.viewport[3];
        }
        // Logged while few have been seen (the game uses ~10, so a cap alone kept this running on every draw:
        // ~6% of the render thread formatting floats).
        if (st_.viewport_set && viewports_logged_ < 12 && list_counter_ < 2000) {
            uint64_t key;
            memcpy(&key, st_.viewport, 8);
            uint64_t k2;
            memcpy(&k2, st_.viewport + 2, 8);
            key ^= k2 * 0x9E3779B97F4A7C15ull;
            if (viewport_keys_.insert(key).second) {
                viewports_logged_++;
                LOGI(kTag, "viewport (%g, %g, %g, %g) on a %ux%u target", st_.viewport[0], st_.viewport[1], st_.viewport[2],
                     st_.viewport[3], extent.width, extent.height);
            }
        }
        VkViewport vp;
        vp.x = vx;
        vp.width = vw;
        if (flip) {
            vp.y = vy + vh;
            vp.height = -vh;
        } else {
            vp.y = vy;
            vp.height = vh;
        }
        vp.minDepth = std::clamp(st_.depth_range[0], 0.0f, 1.0f);
        vp.maxDepth = std::clamp(st_.depth_range[1], 0.0f, 1.0f);
        VkRect2D sc{{0, 0}, extent};
        if (st_.scissor_set && st_.scissor[2] > 0 && st_.scissor[3] > 0) {
            int x = int(st_.scissor[0]), y = int(st_.scissor[1]), w = int(st_.scissor[2]), h = int(st_.scissor[3]);
            x = std::max(0, x);
            y = std::max(0, y);
            w = std::max(0, std::min(w, int(extent.width) - x));
            h = std::max(0, std::min(h, int(extent.height) - y));
            sc = {{x, y}, {uint32_t(w), uint32_t(h)}};
        }
        out->vp = vp;
        out->sc = sc;
        memcpy(out->blend, st_.blend_color, sizeof(out->blend));
        const uint32_t stencil[6] = {st_.stencil_ref[0], st_.stencil_ref[1], st_.stencil_write[0], st_.stencil_write[1],
                                     st_.stencil_compare[0], st_.stencil_compare[1]};
        memcpy(out->stencil, stencil, sizeof(stencil));
        // Polygon offset: NVN (like OpenGL) offsets by factor * slope + units * r, as Vulkan's depth bias does.
        if (PolygonOffsetOn()) {
            out->bias[0] = st_.poly_offset[1];
            out->bias[1] = features_.depthBiasClamp ? st_.poly_offset[2] : 0.0f;
            out->bias[2] = st_.poly_offset[0];
        } else {
            out->bias[0] = out->bias[1] = out->bias[2] = 0.0f;
        }
    }
    // The polygon state enables the offset per polygon mode (NVN: point 1, line 2, fill 4).
    bool PolygonOffsetOn() const {
        const int mode = st_.polygon.mode;
        const int bit = mode == 0 ? 1 : mode == 1 ? 2 : 4;
        return (st_.polygon.offset_enables & bit) != 0;
    }
    // Dynamic state lasts for the whole command buffer: record only what changed since the last draw (recorder).
    void ApplyDynamicState(const DynState& s) {
        const VkCommandBuffer cb = cmd_;
        const bool all = !dyn_valid_;
        if (all || memcmp(&s.vp, &dyn_.vp, sizeof(s.vp)) != 0) vkCmdSetViewport(cb, 0, 1, &s.vp);
        if (all || memcmp(&s.sc, &dyn_.sc, sizeof(s.sc)) != 0) vkCmdSetScissor(cb, 0, 1, &s.sc);
        if (all || memcmp(s.blend, dyn_.blend, sizeof(dyn_.blend)) != 0) vkCmdSetBlendConstants(cb, s.blend);
        if (all || s.stencil[0] != dyn_.stencil[0]) vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_BIT, s.stencil[0]);
        if (all || s.stencil[1] != dyn_.stencil[1]) vkCmdSetStencilReference(cb, VK_STENCIL_FACE_BACK_BIT, s.stencil[1]);
        if (all || s.stencil[2] != dyn_.stencil[2]) vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_BIT, s.stencil[2]);
        if (all || s.stencil[3] != dyn_.stencil[3]) vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_BACK_BIT, s.stencil[3]);
        if (all || s.stencil[4] != dyn_.stencil[4]) vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_BIT, s.stencil[4]);
        if (all || s.stencil[5] != dyn_.stencil[5]) vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_BACK_BIT, s.stencil[5]);
        if (all || memcmp(s.bias, dyn_.bias, sizeof(s.bias)) != 0) vkCmdSetDepthBias(cb, s.bias[0], s.bias[1], s.bias[2]);
        dyn_ = s;
        dyn_valid_ = true;
    }

    bool VertexFormatSupported(VkFormat f) {
        auto it = vertex_format_ok_.find(f);
        if (it != vertex_format_ok_.end()) return it->second;
        VkFormatProperties p;
        vkGetPhysicalDeviceFormatProperties(phys_, f, &p);
        bool ok = p.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
        if (!ok) LOGW(kTag, "vertex format %d not supported by this GPU", int(f));
        return vertex_format_ok_[f] = ok;
    }

    // Everything a pipeline is made from, copied so a worker thread can build it.
    struct PipelineJob {
        VkDevice device = VK_NULL_HANDLE;
        VkPipelineCache cache = VK_NULL_HANDLE;
        VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
        std::vector<VkVertexInputBindingDescription> bindings;
        std::vector<VkVertexInputAttributeDescription> attributes;
        VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        std::vector<VkPipelineColorBlendAttachmentState> blend;
        std::vector<VkFormat> color_formats;
        VkFormat depth_format = VK_FORMAT_UNDEFINED, stencil_format = VK_FORMAT_UNDEFINED;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkRenderPass render_pass = VK_NULL_HANDLE;  // set: no dynamic rendering (a compatible render pass)
        // Result.
        std::atomic<bool> done{false};
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkResult result = VK_SUCCESS;
        double ms = 0;
    };
    static void BuildPipeline(PipelineJob& j) {
        const auto t0 = std::chrono::steady_clock::now();
        VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                     {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = j.vs;
        stages[0].pName = "main";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = j.fs;
        stages[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vi.vertexBindingDescriptionCount = uint32_t(j.bindings.size());
        vi.pVertexBindingDescriptions = j.bindings.data();
        vi.vertexAttributeDescriptionCount = uint32_t(j.attributes.size());
        vi.pVertexAttributeDescriptions = j.attributes.data();
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = j.topology;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = uint32_t(j.blend.size());
        cb.pAttachments = j.blend.data();
        VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                                VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                                VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_DEPTH_BIAS};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount = uint32_t(sizeof(dyn) / sizeof(dyn[0]));
        dy.pDynamicStates = dyn;
        VkPipelineRenderingCreateInfo rinfo{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rinfo.colorAttachmentCount = uint32_t(j.color_formats.size());
        rinfo.pColorAttachmentFormats = j.color_formats.data();
        rinfo.depthAttachmentFormat = j.depth_format;
        rinfo.stencilAttachmentFormat = j.stencil_format;
        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        ci.pNext = &rinfo;
        if (j.render_pass) {  // a render pass of the same formats (load / store operations do not matter)
            ci.pNext = nullptr;
            ci.renderPass = j.render_pass;
            ci.subpass = 0;
        }
        ci.stageCount = j.fs != VK_NULL_HANDLE ? 2 : 1;
        ci.pStages = stages;
        ci.pVertexInputState = &vi;
        ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vp;
        ci.pRasterizationState = &j.rs;
        ci.pMultisampleState = &ms;
        ci.pDepthStencilState = &j.ds;
        ci.pColorBlendState = &cb;
        ci.pDynamicState = &dy;
        ci.layout = j.layout;
        j.result = vkCreateGraphicsPipelines(j.device, j.cache, 1, &ci, nullptr, &j.pipeline);
        if (j.result != VK_SUCCESS) j.pipeline = VK_NULL_HANDLE;
        j.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        j.done.store(true, std::memory_order_release);
    }

    // Background compilers: a few threads taking jobs in order.
    std::mutex compile_mutex_;
    std::condition_variable compile_cv_;
    std::deque<std::shared_ptr<PipelineJob>> compile_queue_;
    std::vector<std::thread> compilers_;
    std::unordered_map<uint64_t, std::pair<std::vector<uint32_t>, std::shared_ptr<PipelineJob>>> compiling_;
    uint64_t pipeline_waits_ = 0;  // draws skipped while their pipeline compiled (reported)
    bool pipeline_pending_ = false;  // the last GetPipeline returned null because it is still compiling
    void StartCompilers() {
        if (!compilers_.empty()) return;
        // 2 threads up to 7 cores, 3 on 8-9, 4 on 10+: phones with many slow cores build new scenery's
        // pipelines sooner (the draws waiting for them are skipped meanwhile).
        const unsigned hw = std::thread::hardware_concurrency();
        const int count = std::clamp(int(hw / 2) - 1, 2, 4);
        LOGI(kTag, "%d pipeline compiler threads (%u CPUs)", count, hw);
        for (int i = 0; i < count; i++)
            compilers_.emplace_back([this] {
                // Every CPU: created by a thread kept on the fast cores, they inherited its pin and, at low
                // priority, got almost no time there while the slow cores idled (Helio G80: pipelines took
                // seconds).
                dsr::platform::UseAllCores();
                // Below the render threads: new pipelines are built in the background and must not take CPU
                // time from the frame being drawn (camera turns into new scenery stuttered).
                setpriority(PRIO_PROCESS, 0, 8);
                for (;;) {
                    std::shared_ptr<PipelineJob> j;
                    {
                        std::unique_lock<std::mutex> lk(compile_mutex_);
                        compile_cv_.wait(lk, [&] { return !compile_queue_.empty(); });
                        j = std::move(compile_queue_.front());
                        compile_queue_.pop_front();
                    }
                    BuildPipeline(*j);
                }
            });
        for (auto& t : compilers_) t.detach();
    }
    // Takes a finished background compile into the caches. VK_NULL_HANDLE while it is still running.
    VkPipeline FinishCompile(uint64_t h, std::pair<std::vector<uint32_t>, std::shared_ptr<PipelineJob>>& e) {
        PipelineJob& j = *e.second;
        if (!j.done.load(std::memory_order_acquire)) return VK_NULL_HANDLE;
        compile_ms_ += j.ms;
        if (j.result != VK_SUCCESS) LogOnce("pipeline", "vkCreateGraphicsPipelines failed: %d", j.result);
        const VkPipeline p = j.pipeline;
        pipelines_[e.first] = p;
        if (p) pipelines_fast_[h] = p;
        compiling_.erase(h);
        return p;
    }

    VkPipeline GetPipeline(Shader* vs, Shader* fs, const VertexInput& vin, VkPrimitiveTopology topology,
                           const std::vector<VkFormat>& color_formats, int depth_nvn_format, VkFormat depth_format,
                           VkPipelineLayout layout) {
        // Key: everything that shapes the pipeline. Hashed first (no allocation); the full key only on a miss.
        Key64 fk;
        fk.Add(vs->module);
        fk.Add(fs->module);
        fk.Add(vin.bindings.data(), vin.bindings.size() * sizeof(VkVertexInputBindingDescription));
        fk.Add(uint32_t(0xFFFF));
        fk.Add(vin.attributes.data(), vin.attributes.size() * sizeof(VkVertexInputAttributeDescription));
        fk.Add(topology);
        fk.Add(color_formats.data(), color_formats.size() * sizeof(VkFormat));
        fk.Add(depth_format);
        fk.Add(&st_.polygon, sizeof(st_.polygon));
        fk.Add(&st_.depth, sizeof(st_.depth));
        fk.Add(&st_.color, sizeof(st_.color));
        fk.Add(&st_.channel, sizeof(st_.channel));
        for (size_t i = 0; i < color_formats.size(); i++) fk.Add(&st_.blend[i], sizeof(BlendStateObj));
        fk.Add(layout);
        pipeline_pending_ = false;
        auto fast = pipelines_fast_.find(fk.h);
        if (fast != pipelines_fast_.end()) return fast->second;
        auto running = compiling_.find(fk.h);
        if (running != compiling_.end()) {
            VkPipeline p = FinishCompile(fk.h, running->second);
            pipeline_pending_ = !p;
            if (!p) pipeline_waits_++;
            return p;
        }
        std::vector<uint32_t> key;
        auto add = [&](const void* p, size_t n) {
            const size_t at = key.size();
            key.resize(at + (n + 3) / 4, 0);
            memcpy(&key[at], p, n);
        };
        uint64_t mods[2] = {reinterpret_cast<uint64_t>(vs->module), reinterpret_cast<uint64_t>(fs->module)};
        add(mods, sizeof(mods));
        add(vin.bindings.data(), vin.bindings.size() * sizeof(VkVertexInputBindingDescription));
        key.push_back(0xFFFF);
        add(vin.attributes.data(), vin.attributes.size() * sizeof(VkVertexInputAttributeDescription));
        key.push_back(uint32_t(topology));
        add(color_formats.data(), color_formats.size() * sizeof(VkFormat));
        key.push_back(uint32_t(depth_format));
        add(&st_.polygon, sizeof(st_.polygon));
        add(&st_.depth, sizeof(st_.depth));
        add(&st_.color, sizeof(st_.color));
        add(&st_.channel, sizeof(st_.channel));
        for (size_t i = 0; i < color_formats.size(); i++) add(&st_.blend[i], sizeof(BlendStateObj));
        auto it = pipelines_.find(key);
        if (it != pipelines_.end()) {
            pipelines_fast_[fk.h] = it->second;
            return it->second;
        }

        auto job = std::make_shared<PipelineJob>();
        PipelineJob& j = *job;
        j.device = device_;
        j.cache = pipeline_cache_;
        j.vs = vs->module;
        j.fs = fs->module;
        const bool has_fragment = fs->module != VK_NULL_HANDLE;
        j.bindings = vin.bindings;
        j.attributes = vin.attributes;
        j.topology = topology;
        VkPipelineRasterizationStateCreateInfo& rs = j.rs;
        rs.polygonMode = (st_.polygon.mode == 0 && features_.fillModeNonSolid) ? VK_POLYGON_MODE_POINT
                         : (st_.polygon.mode == 1 && features_.fillModeNonSolid) ? VK_POLYGON_MODE_LINE
                                                                                 : VK_POLYGON_MODE_FILL;
        rs.cullMode = st_.polygon.cull == 1 ? VK_CULL_MODE_FRONT_BIT
                      : st_.polygon.cull == 2 ? VK_CULL_MODE_BACK_BIT
                      : st_.polygon.cull == 3 ? VK_CULL_MODE_FRONT_AND_BACK
                                              : VK_CULL_MODE_NONE;
        rs.frontFace = st_.polygon.front_face == 0 ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;
        rs.depthBiasEnable = PolygonOffsetOn() ? VK_TRUE : VK_FALSE;  // values: dynamic state
        VkPipelineDepthStencilStateCreateInfo& ds = j.ds;
        const bool has_depth = depth_format != VK_FORMAT_UNDEFINED && HasDepth(depth_nvn_format);
        const bool has_stencil = depth_format != VK_FORMAT_UNDEFINED && HasStencil(depth_nvn_format);
        ds.depthTestEnable = has_depth && st_.depth.depth_test;
        ds.depthWriteEnable = has_depth && st_.depth.depth_test && st_.depth.depth_write;
        ds.depthCompareOp = CompareOp(st_.depth.depth_func);
        ds.stencilTestEnable = has_stencil && st_.depth.stencil_test;
        for (int face = 0; face < 2; face++) {
            VkStencilOpState& so = face ? ds.back : ds.front;
            so.failOp = StencilOp(st_.depth.stencil_fail[face]);
            so.passOp = StencilOp(st_.depth.depth_pass[face]);
            so.depthFailOp = StencilOp(st_.depth.depth_fail[face]);
            so.compareOp = CompareOp(st_.depth.stencil_func[face]);
            so.compareMask = 0xFF;
            so.writeMask = 0xFF;
        }
        j.blend.resize(color_formats.size());
        for (size_t i = 0; i < j.blend.size(); i++) {
            VkPipelineColorBlendAttachmentState& b = j.blend[i];
            const BlendStateObj& bs = st_.blend[features_.independentBlend ? i : 0];
            b.blendEnable = (st_.color.blend_enable >> (features_.independentBlend ? i : 0)) & 1;
            b.srcColorBlendFactor = BlendFactor(bs.src_rgb);
            b.dstColorBlendFactor = BlendFactor(bs.dst_rgb);
            b.colorBlendOp = BlendOp(bs.eq_rgb);
            b.srcAlphaBlendFactor = BlendFactor(bs.src_a);
            b.dstAlphaBlendFactor = BlendFactor(bs.dst_a);
            b.alphaBlendOp = BlendOp(bs.eq_a);
            b.colorWriteMask = has_fragment ? ((st_.channel.mask >> (4 * i)) & 0xF) : 0;
        }
        j.color_formats = color_formats;
        j.depth_format = has_depth ? depth_format : VK_FORMAT_UNDEFINED;
        j.stencil_format = has_stencil ? depth_format : VK_FORMAT_UNDEFINED;
        if (rp_fallback_) j.render_pass = CompatRenderPass(j.color_formats, j.depth_format, j.stencil_format);
        j.layout = layout;
        new_pipelines_++;

        VkPipeline pipeline = VK_NULL_HANDLE;
        if (dsr::GetRuntime().config.async_pipelines) {
            // Compile in the background. A pipeline the driver has cached comes back within a few ms: wait that
            // long, so it is used this frame. A genuinely new one skips its draws until ready (no frame hitch).
            StartCompilers();
            {
                std::lock_guard<std::mutex> lk(compile_mutex_);
                compile_queue_.push_back(job);
            }
            compile_cv_.notify_one();
            auto& entry = compiling_[fk.h];
            entry = {std::move(key), job};
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(2500);
            while (!job->done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            pipeline = FinishCompile(fk.h, entry);
            if (!pipeline) {
                pipeline_waits_++;
                pipeline_pending_ = true;
                return VK_NULL_HANDLE;
            }
        } else {
            BuildPipeline(j);
            compile_ms_ += j.ms;
            pipeline = j.pipeline;
            if (j.result != VK_SUCCESS) LogOnce("pipeline", "vkCreateGraphicsPipelines failed: %d", j.result);
            pipelines_[key] = pipeline;
            if (pipeline) pipelines_fast_[fk.h] = pipeline;
        }
        if (pipeline && pipelines_.size() <= 8)
            LOGI(kTag, "pipeline %zu: %zu attributes, %zu targets, depth format %d, topology %d, blend %s",
                 pipelines_.size(), vin.attributes.size(), color_formats.size(), int(depth_format), int(topology),
                 (st_.color.blend_enable & 1) ? "on" : "off");
        return pipeline;
    }

    // c[1]: the literal constants NVN's compiler stored with the shader. The control section header gives
    // its size (word 5) and the constants' size (word 11); the constants sit at the end of the section.
    void ShaderConstants(const ShaderCode& code, const uint8_t** data, size_t* size) {
        *data = code.constants.empty() ? nullptr : code.constants.data();
        *size = code.constants.size();
    }

    // Smallest and largest index of an index buffer. Scanned once and remembered across frames: index
    // buffers are almost always static. The key includes a sparse checksum so a rewritten buffer is rescanned.
    void IndexRange(uint64_t addr, uint32_t count, uint32_t isz, uint32_t* lo, uint32_t* hi) {
        const uint8_t* ib = reinterpret_cast<const uint8_t*>(addr);
        auto at = [&](uint32_t i) -> uint32_t {
            return isz == 2 ? reinterpret_cast<const uint16_t*>(ib)[i] : reinterpret_cast<const uint32_t*>(ib)[i];
        };
        uint64_t key = (addr * 0x9E3779B97F4A7C15ull) ^ (uint64_t(count) << 32) ^ isz;
        const uint32_t step = std::max<uint32_t>(1, count / 16);
        for (uint32_t i = 0; i < count; i += step) key = (key ^ at(i)) * 0x100000001B3ull;
        key = (key ^ at(count - 1)) * 0x100000001B3ull;
        auto it = index_ranges_.find(key);
        if (it != index_ranges_.end()) {
            *lo = it->second.first;
            *hi = it->second.second;
            return;
        }
        uint32_t mn = UINT32_MAX, mx = 0;
        for (uint32_t i = 0; i < count; i++) {
            const uint32_t v = at(i);
            mn = std::min(mn, v);
            mx = std::max(mx, v);
        }
        if (index_ranges_.size() > 200000) index_ranges_.clear();
        index_ranges_[key] = {mn, mx};
        *lo = mn;
        *hi = mx;
    }

    // Copies guest data into the stream once per list: the same range bound by several draws (UBOs reused
    // across passes, a mesh drawn into depth, shadow and color) is uploaded once. Guest memory only changes
    // during a list through UpdateUniform, which clears the cache.
    // ---- geometry cache: vertex and index data kept on the GPU across frames -------------------------------
    // Most geometry (the level, characters) never changes after loading, yet was copied into the stream every
    // frame (~30 MB a frame in Undead Burg). Ranges now live in one persistent buffer and are copied again only
    // when their content changed: small ranges (where per-frame data like particles and UI lives) are hashed in
    // full, large ones at 256 spots plus both ends. When the buffer fills up it is emptied (after the GPU is
    // done with it) and refilled.
    static constexpr VkDeviceSize kGeoSize = 192ull << 20;
    VkBuffer geo_buf_ = VK_NULL_HANDLE;
    VkDeviceMemory geo_mem_ = VK_NULL_HANDLE;
    uint8_t* geo_ptr_ = nullptr;
    VkDeviceSize geo_off_ = 0;
    bool geo_failed_ = false;
    struct GeoEntry {
        uint64_t hash;
        VkDeviceSize off;
        uint32_t changes;  // content changes seen; from 2 on the range is streamed instead (volatile)
        uint64_t next_check_list;  // unused (checks every list)
        uint32_t interval;
        uint64_t checked_list = 0;  // content verified in this list already: memory a list draws from cannot change
                                    // while the list runs, so later draws of the same range skip the hash
        const uint8_t* src = nullptr;  // the range (for hashing ahead)
        size_t size = 0;
    };
    // Hashing ahead: at the start of a list, the ranges the previous list used (almost all of them are used
    // again) are hashed on worker threads while the render thread works; GeoCached takes a finished result
    // instead of hashing on the render thread (content hashing was ~15% of the render thread).
    struct PrehashJob {
        uint64_t key;
        const uint8_t* src;
        size_t size;
        uint64_t hash;
        std::atomic<bool> done{false};
    };
    std::unique_ptr<PrehashJob[]> prehash_jobs_;
    size_t prehash_capacity_ = 0;
    std::atomic<size_t> prehash_count_{0};
    std::unordered_map<uint64_t, size_t> prehash_index_;
    std::atomic<size_t> prehash_next_{0};
    std::atomic<int> prehash_active_{0};  // workers inside the job loop
    uint64_t prehash_generation_ = 0;     // under prehash_mutex_
    std::mutex prehash_mutex_;
    std::condition_variable prehash_cv_;
    bool prehash_started_ = false;
    std::vector<uint64_t> geo_used_prev_, geo_used_now_;
    uint64_t prehash_used_ = 0, prehash_inline_ = 0, stat_lists_ = 0;
    static constexpr size_t kNoMoreJobs = SIZE_MAX / 2;
    void PrehashWorker() {
        uint64_t seen = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(prehash_mutex_);
                prehash_cv_.wait(lk, [&] { return prehash_generation_ != seen; });
                seen = prehash_generation_;
            }
            prehash_active_.fetch_add(1);
            for (;;) {
                const size_t i = prehash_next_.fetch_add(1);
                if (i >= prehash_count_.load()) break;
                PrehashJob& j = prehash_jobs_[i];
                j.hash = GuestRangeValid(uint64_t(uintptr_t(j.src)), j.size) ? GeoHash(j.src, j.size) : 0;
                j.done.store(true, std::memory_order_release);
            }
            prehash_active_.fetch_sub(1);
        }
    }
    // At the start of a list (render thread): queue the ranges the previous list used.
    void StartPrehash() {
        if (!prehash_started_) {
            prehash_started_ = true;
            const unsigned hw = std::thread::hardware_concurrency();
            const int n = hw >= 6 ? 3 : hw >= 4 ? 2 : 1;
            for (int i = 0; i < n; i++) std::thread([this] { PrehashWorker(); }).detach();
        }
        // Stop the previous generation: no new jobs are taken, and the ones being hashed finish.
        prehash_next_.store(kNoMoreJobs);
        while (prehash_active_.load() != 0) std::this_thread::yield();
        geo_used_prev_.swap(geo_used_now_);
        geo_used_now_.clear();
        prehash_index_.clear();
        const size_t n = geo_used_prev_.size();
        if (n > prehash_capacity_) {
            prehash_jobs_.reset(new PrehashJob[n * 2]);
            prehash_capacity_ = n * 2;
        }
        size_t count = 0;
        for (uint64_t key : geo_used_prev_) {
            auto it = geo_.find(key);
            if (it == geo_.end() || it->second.changes >= 2 || !it->second.src) continue;
            PrehashJob& j = prehash_jobs_[count];
            j.key = key;
            j.src = it->second.src;
            j.size = it->second.size;
            j.done.store(false, std::memory_order_relaxed);
            prehash_index_[key] = count;
            count++;
        }
        prehash_count_.store(count);
        prehash_next_.store(0);
        {
            std::lock_guard<std::mutex> lk(prehash_mutex_);
            prehash_generation_++;
        }
        prehash_cv_.notify_all();
    }
    std::unordered_map<uint64_t, GeoEntry> geo_;
    // Geometry content is checked once per token: a new token with each list, and whenever the list itself
    // writes game memory (buffer-to-buffer copies, inline buffer updates). Draws after such a write in the same
    // list used the copy checked before it: stale particle quads (holes in smoke and dust). Hashes made ahead
    // by the workers (list start) are not used after such a write either.
    uint64_t geo_token_ = 1, geo_token_list_ = 1;
    uint64_t list_b2b_copies_ = 0, list_updates_ = 0;
    uintptr_t geo_lo_ = UINTPTR_MAX, geo_hi_ = 0;  // span of the geometry checked under the current token
    void MemoryWrittenInList(uint64_t addr, uint64_t size) {
        stream_cache_.clear();
        if (addr < geo_hi_ && addr + size > geo_lo_) {
            geo_token_++;
            geo_lo_ = UINTPTR_MAX;
            geo_hi_ = 0;
            geo_rechecks_++;
        }
    }
    uint64_t geo_rechecks_ = 0;
    uint64_t geo_hits_ = 0, geo_misses_ = 0, geo_bytes_ = 0, geo_resets_ = 0, geo_volatile_ = 0;

    bool EnsureGeo() {
        if (geo_buf_) return true;
        if (geo_failed_) return false;
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = kGeoSize;
        bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        VkMemoryRequirements req;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        void* p = nullptr;
        if (vkCreateBuffer(device_, &bi, nullptr, &geo_buf_) != VK_SUCCESS) goto fail;
        vkGetBufferMemoryRequirements(device_, geo_buf_, &req);
        ai.allocationSize = req.size;
        ai.memoryTypeIndex =
            FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(device_, &ai, nullptr, &geo_mem_) != VK_SUCCESS) goto fail;
        vkBindBufferMemory(device_, geo_buf_, geo_mem_, 0);
        if (vkMapMemory(device_, geo_mem_, 0, kGeoSize, 0, &p) != VK_SUCCESS) goto fail;
        geo_ptr_ = static_cast<uint8_t*>(p);
        LOGI(kTag, "geometry cache: %llu MB", (unsigned long long)(kGeoSize >> 20));
        return true;
    fail:
        LOGW(kTag, "geometry cache unavailable: vertex data is streamed every frame");
        if (geo_buf_) vkDestroyBuffer(device_, geo_buf_, nullptr);
        if (geo_mem_) vkFreeMemory(device_, geo_mem_, nullptr);
        geo_buf_ = VK_NULL_HANDLE;
        geo_mem_ = VK_NULL_HANDLE;
        geo_failed_ = true;
        return false;
    }
    static uint64_t GeoHash(const uint8_t* p, size_t n) {
        constexpr uint64_t K = 0x9E3779B97F4A7C15ull;
        auto mix = [](uint64_t h, uint64_t v) { return (h ^ v) * K; };
        uint64_t h = 0xcbf29ce484222325ull ^ n;
        uint64_t v;
        // Up to 512 KB: the whole range, four independent lanes (a single chain is latency bound). Per-frame
        // geometry (particles, effects) lives in ranges like these; sampling them missed partial rewrites and
        // left stale particle quads on screen (flickering squares). Most of this runs on the hash workers.
        if (n <= (512u << 10)) {
            uint64_t a = h, b = h + 1, c = h + 2, d = h + 3;
            size_t i = 0;
            for (; i + 32 <= n; i += 32) {
                uint64_t w[4];
                memcpy(w, p + i, 32);
                a = (a ^ w[0]) * K;
                b = (b ^ w[1]) * K;
                c = (c ^ w[2]) * K;
                d = (d ^ w[3]) * K;
            }
            for (; i + 8 <= n; i += 8) {
                memcpy(&v, p + i, 8);
                a = mix(a, v);
            }
            for (; i < n; i++) b = mix(b, p[i]);
            return mix(mix(mix(a, b >> 1 | b << 63), c >> 2 | c << 62), d >> 3 | d << 61);
        }
        for (size_t i = 0; i < 64; i += 8) {  // both ends
            memcpy(&v, p + i, 8);
            h = mix(h, v);
            memcpy(&v, p + n - 64 + i, 8);
            h = mix(h, v);
        }
        const size_t step = (n - 8) / 256;
        for (size_t k = 0; k < 256; k++) {
            memcpy(&v, p + k * step, 8);
            h = mix(h, v);
        }
        return h;
    }
    // Offset of the range in geo_buf_ (content checked), or VK_WHOLE_SIZE if it cannot be cached.
    VkDeviceSize GeoCached(const uint8_t* src, size_t size) {
        if (size == 0 || size > (16u << 20) || !EnsureGeo()) return VK_WHOLE_SIZE;
        const uint64_t key = (uint64_t(reinterpret_cast<uintptr_t>(src)) * 0x9E3779B97F4A7C15ull) ^ uint64_t(size);
        auto it = geo_.find(key);
        if (it != geo_.end() && it->second.changes >= 2) {
            geo_volatile_++;
            return VK_WHOLE_SIZE;  // rewritten every frame or so: streamed with the list
        }
        if (it != geo_.end() && it->second.checked_list == geo_token_) {
            geo_hits_++;
            return it->second.off;
        }
        geo_used_now_.push_back(key);
        geo_lo_ = std::min(geo_lo_, reinterpret_cast<uintptr_t>(src));
        geo_hi_ = std::max(geo_hi_, reinterpret_cast<uintptr_t>(src) + size);
        // Checked every list: skipping checks for ranges that had been stable showed other geometry (memory the
        // game reused for another mesh of the same size) for a few frames: flickering textures. The hash usually
        // comes from a worker thread (hashed at the start of the list).
        uint64_t h;
        auto pre = prehash_index_.find(key);
        if (pre != prehash_index_.end() && geo_token_ == geo_token_list_ &&
            prehash_jobs_[pre->second].done.load(std::memory_order_acquire) &&
            prehash_jobs_[pre->second].src == src) {
            h = prehash_jobs_[pre->second].hash;
            prehash_used_++;
        } else {
            h = GeoHash(src, size);
            prehash_inline_++;
        }
        if (it != geo_.end() && it->second.hash == h) {
            geo_hits_++;
            it->second.checked_list = geo_token_;
            it->second.interval = std::min<uint32_t>(it->second.interval * 2, 8);
            it->second.next_check_list = list_counter_ + it->second.interval;
            return it->second.off;
        }
        const uint32_t changes = it != geo_.end() ? it->second.changes + 1 : 0;
        if (changes >= 2) {
            it->second.changes = changes;
            geo_volatile_++;
            return VK_WHOLE_SIZE;
        }
        const VkDeviceSize need = (size + 63) & ~VkDeviceSize(63);
        if (geo_off_ + need > kGeoSize) {
            // Full: start over once the GPU no longer reads any of it.
            Flush();
            WaitGpuIdle();
            geo_.clear();
            geo_off_ = 0;
            geo_resets_++;
            InvalidateBoundState();
            it = geo_.end();
        }
        const VkDeviceSize off = geo_off_;
        geo_off_ += need;
        memcpy(geo_ptr_ + off, src, size);
        geo_misses_++;
        geo_bytes_ += size;
        // A changed range gets new space (the old copy may still be read by frames in flight).
        geo_[key] = {h, off, changes, list_counter_ + 1, 1, geo_token_, src, size};
        return off;
    }

    // Vertex data read past the end of the bound buffer (or of known memory): the part inside is copied and the
    // rest is zeros, as the hardware's out-of-range vertex fetches return. (Such draws were skipped: whole
    // batches of particles went missing, leaving see-through squares in smoke and dust.)
    int map_geo_dumps_ = 0;  // per capture
    uint64_t padded_fetches_ = 0;
    int padded_logged_ = 0;
    VkDeviceSize UploadPadded(const BufferRange& r, uint64_t start, uint64_t bytes) {
        padded_fetches_++;
        if (bytes > kStreamSize / 4) return VK_WHOLE_SIZE;
        const bool in_memory = GuestRangeValid(r.address + start, size_t(bytes));
        if (padded_logged_ < 12) {
            padded_logged_++;
            LOGI(kTag, "vertex fetch past the bound buffer: buffer @%llx size %llu, reading %llu bytes from %llu (%s)",
                 (unsigned long long)r.address, (unsigned long long)r.size, (unsigned long long)bytes,
                 (unsigned long long)start, in_memory ? "inside game memory: read as is" : "outside known memory: zeros");
        }
        // The data is there (the fog wall's tiles drew grey with zeros): the bound size understates the buffer.
        if (in_memory) return UploadCached(reinterpret_cast<const uint8_t*>(r.address + start), size_t(bytes), 16);
        if (StreamRemaining() < bytes + 1024) Flush();
        const VkDeviceSize off = StreamAlloc(bytes, 16);
        if (off == VK_WHOLE_SIZE) return off;
        uint8_t* dst = stream_ptr_ + off;
        uint64_t avail = start < r.size ? std::min<uint64_t>(bytes, r.size - start) : 0;
        // Only memory inside a known pool is read: shrink to what is valid (page granularity is enough).
        while (avail && !GuestRangeValid(r.address + start, size_t(avail))) avail = avail > 4096 ? avail - 4096 : 0;
        if (avail) memcpy(dst, reinterpret_cast<const void*>(r.address + start), size_t(avail));
        memset(dst + avail, 0, size_t(bytes - avail));
        return off;
    }
    VkDeviceSize UploadCached(const uint8_t* src, size_t size, VkDeviceSize align) {
        const uint64_t key = (uint64_t(reinterpret_cast<uintptr_t>(src)) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(size) << 1) ^ align;
        auto it = stream_cache_.find(key);
        if (it != stream_cache_.end()) {
            upload_hits_++;
            return it->second;
        }
        bool flushed = false;
        VkDeviceSize off = StreamAlloc(size, align, &flushed);
        if (off == VK_WHOLE_SIZE) return off;
        memcpy(stream_ptr_ + off, src, size);
        upload_bytes_ += size;
        stream_cache_[key] = off;
        return off;
    }

    void MissingUniform(int stage, uint32_t n) {
        auto key = std::to_string(stage) + ":" + std::to_string(n);
        if (missing_ubo_logged_.insert(key).second && missing_ubo_logged_.size() <= 16)
            LOGW(kTag, "%s shader reads c[%u] but nothing is bound there (zeros used)", stage ? "pixel" : "vertex", n);
    }

    void LogUniformBind(int stage, int index, const BufferRange& r) {
        const uint64_t bit = 1ull << ((stage & 1) * 32 + (index & 31));  // called for every bind: no strings
        if (ubo_binds_seen_ & bit) return;
        ubo_binds_seen_ |= bit;
        LOGI(kTag, "uniform buffer bound: stage %d index %d (%llu bytes)", stage, index, (unsigned long long)r.size);
    }
    uint64_t ubo_binds_seen_ = 0;

    void ReportDraws() {
        auto now = std::chrono::steady_clock::now();
        if (now - last_report_ < std::chrono::seconds(10)) return;
        last_report_ = now;
        Drain();  // the recorder's counters below
        if (rec_started_) {
            LOGI(kTag, "recorder thread: busy %.0f ms in the last 10 s; render thread waited %.0f ms for it; %llu batches; "
                 "%llu extra descriptor pools; %llu draws without descriptor sets",
                 rec_ns_.exchange(0) / 1e6, drain_ms_, (unsigned long long)rec_batches_,
                 (unsigned long long)desc_pools_added_, (unsigned long long)skipped_desc_);
            drain_ms_ = 0;
            rec_batches_ = 0;
        }
        std::string skipped;
        for (auto& [reason, n] : skipped_) skipped += " | " + std::to_string(n) + " " + reason;
        LOGI(kTag, "draws: %llu drawn, %zu pipelines, %d shaders, %d uploads, %d flushes%s", (unsigned long long)drawn_,
             pipelines_.size(), shaders_translated_, uploads_, flushes_, skipped.c_str());
        if (bc_converted_levels_ || bc_cache_hits_) {
            LOGI(kTag, "BC textures converted on the CPU: %llu levels in %.0f ms, %llu from the ETC2 cache (%.0f MB)",
                 (unsigned long long)bc_converted_levels_, bc_convert_ms_, (unsigned long long)bc_cache_hits_,
                 etc_cache_bytes_ / 1048576.0);
            bc_converted_levels_ = bc_cache_hits_ = 0;
            bc_convert_ms_ = 0;
        }
        {
            long rss_kb = 0;
            if (FILE* f = fopen("/proc/self/statm", "r")) {
                long pages = 0, resident = 0;
                if (fscanf(f, "%ld %ld", &pages, &resident) == 2) rss_kb = resident * (sysconf(_SC_PAGESIZE) / 1024);
                fclose(f);
            }
            uint64_t gpu = 0;
            for (auto& [a, img] : images_) {
                VkMemoryRequirements req{};
                if (img.image) vkGetImageMemoryRequirements(device_, img.image, &req);
                gpu += req.size;
            }
            LOGI(kTag, "memory: process %ld MB resident, %zu GPU images using %llu MB, %zu index ranges cached%s", rss_kb / 1024,
                 images_.size(), (unsigned long long)(gpu >> 20), index_ranges_.size(),
                 mip_skip_ ? (", " + std::to_string(mip_skipped_) + " big textures without their top level").c_str() : "");
        }
        if (big_checks_) {
            LOGI(kTag, "game-written textures (>= 512x512): %llu checks, %llu changed and re-uploaded",
                 (unsigned long long)big_checks_, (unsigned long long)big_changes_);
            big_checks_ = big_changes_ = 0;
        }
        {
            std::lock_guard<std::mutex> lk(frames_mutex_);
            if (frames_set_) {
                LOGI(kTag, "video frames: %llu decoded into textures, %llu uploaded (newer ones replace unused ones); "
                     "shown for 0/1/2/3/4+ frames: %llu/%llu/%llu/%llu/%llu",
                     (unsigned long long)frames_set_, (unsigned long long)frames_uploaded_,
                     (unsigned long long)video_cadence_[0], (unsigned long long)video_cadence_[1],
                     (unsigned long long)video_cadence_[2], (unsigned long long)video_cadence_[3],
                     (unsigned long long)video_cadence_[4]);
                frames_set_ = frames_uploaded_ = 0;
                for (uint64_t& c : video_cadence_) c = 0;
            }
        }
        LOGI(kTag, "last 10 s: %llu render passes started, %llu image barriers (%llu for vertex-shader reads of render targets)",
             (unsigned long long)render_passes_, (unsigned long long)barriers_, (unsigned long long)vertex_read_barriers_);
        vertex_read_barriers_ = 0;
        render_passes_ = barriers_ = 0;
        if (!pass_breaks_.empty()) {
            std::vector<std::pair<uint64_t, int>> v;
            for (auto& [line, n] : pass_breaks_) v.push_back({n, line});
            std::sort(v.rbegin(), v.rend());
            std::string top;
            for (size_t i = 0; i < v.size() && i < 8; i++)
                top += " line " + std::to_string(v[i].second) + ": " + std::to_string(v[i].first) + ";";
            LOGI(kTag, "render passes ended by (source line: count):%s", top.c_str());
            pass_breaks_.clear();
        }
        if (!sampled_breaks_.empty()) {
            std::vector<std::pair<uint64_t, std::string>> v;
            for (auto& [k, n] : sampled_breaks_) v.push_back({n, k});
            std::sort(v.rbegin(), v.rend());
            std::string top;
            for (size_t i = 0; i < v.size() && i < 8; i++) top += " [" + v[i].second + "]: " + std::to_string(v[i].first) + ";";
            LOGI(kTag, "textures that broke a render pass to become readable:%s", top.c_str());
            sampled_breaks_.clear();
        }
        LOGI(kTag, "last 10 s: %.0f ms executing lists (%.0f ms of it waiting for the GPU), %.1f MB streamed, %llu cached uploads",
             exec_ms_, wait_ms_, upload_bytes_ / 1048576.0, (unsigned long long)upload_hits_);
        LOGI(kTag, "GPU busy %.0f ms in the last 10 s (%.1f ms per list); geometry hashed ahead: %llu of %llu checks",
             gpu_busy_ms_, list_counter_ > stat_lists_ ? gpu_busy_ms_ / double(list_counter_ - stat_lists_) : 0.0,
             (unsigned long long)prehash_used_, (unsigned long long)(prehash_used_ + prehash_inline_));
        gpu_busy_ms_ = 0;
        stat_lists_ = list_counter_;
        prehash_used_ = prehash_inline_ = 0;
        if (geo_buf_) {
            LOGI(kTag, "geometry cache: %llu reused, %llu copied (%.1f MB), %llu volatile (streamed), %.0f of %llu MB used, "
                 "%llu resets", (unsigned long long)geo_hits_, (unsigned long long)geo_misses_, geo_bytes_ / 1048576.0,
                 (unsigned long long)geo_volatile_, geo_off_ / 1048576.0, (unsigned long long)(kGeoSize >> 20),
                 (unsigned long long)geo_resets_);
            geo_hits_ = geo_misses_ = geo_bytes_ = geo_volatile_ = 0;
        }
        std::string phases;
        for (int i = 0; i < 7; i++) {
            phases += " " + std::string(kPhaseNames[i]) + "=" + std::to_string(int(phase_ms_[i]));
            phase_ms_[i] = 0;
        }
        if (readbacks_recorded_ || readbacks_done_) {
            LOGI(kTag, "GPU -> memory copies: %llu recorded, %llu delivered", (unsigned long long)readbacks_recorded_,
                 (unsigned long long)readbacks_done_);
            readbacks_recorded_ = readbacks_done_ = 0;
        }
        LOGI(kTag, "clears: %llu done as render pass loads or deferred, %llu inside passes, %llu performed separately; "
             "%llu barriers between passes reusing an attachment, %llu depth buffers refreshed for LRZ; %llu draws depth-tested "
             "in the shader",
             (unsigned long long)clears_deferred_, (unsigned long long)clears_in_pass_, (unsigned long long)clears_flushed_,
             (unsigned long long)pass_barriers_, (unsigned long long)lrz_touches_, (unsigned long long)shader_depth_draws_);
        clears_deferred_ = clears_in_pass_ = clears_flushed_ = pass_barriers_ = lrz_touches_ = shader_depth_draws_ = 0;
        if (rt_writebacks_ || reports_done_) {
            LOGI(kTag, "small render targets written back to game memory: %llu; reports written: %llu",
                 (unsigned long long)rt_writebacks_, (unsigned long long)reports_done_);
            rt_writebacks_ = reports_done_ = 0;
        }
        LOGI(kTag, "descriptor sets: %llu reused from earlier frames, %zu kept, %llu resets", (unsigned long long)persist_hits_,
             persist_cache_.size(), (unsigned long long)persist_resets_);
        if (list_b2b_copies_ || list_updates_) {
            LOGI(kTag, "game memory written by command lists: %llu buffer copies, %llu buffer updates; geometry re-checked "
                 "after %llu of them", (unsigned long long)list_b2b_copies_, (unsigned long long)list_updates_,
                 (unsigned long long)geo_rechecks_);
            list_b2b_copies_ = list_updates_ = geo_rechecks_ = 0;
        }
        if (padded_fetches_ || view_standins_) {
            LOGI(kTag, "draws reading vertex data past their bound buffer: %llu; with a stand-in for a "
                 "texture of the wrong dimension: %llu", (unsigned long long)padded_fetches_,
                 (unsigned long long)view_standins_);
            padded_fetches_ = view_standins_ = 0;
        }
        if (images_retired_) {
            LOGI(kTag, "images replaced by new textures in the same memory: %llu (destroyed after the GPU is done)",
                 (unsigned long long)images_retired_);
            images_retired_ = 0;
        }
        persist_hits_ = 0;
        LOGI(kTag, "draw time by phase (ms):%s | %d pipelines compiled in %.0f ms (%llu draws waited for one) | %llu fast-path draws | %llu descriptor sets reused",
             phases.c_str(), new_pipelines_, compile_ms_, (unsigned long long)pipeline_waits_,
             (unsigned long long)fast_draws_, (unsigned long long)desc_reused_);
        pipeline_waits_ = 0;
        desc_reused_ = 0;
        if (new_pipelines_) SavePipelineCache();
        new_pipelines_ = 0;
        compile_ms_ = 0;
        fast_draws_ = 0;
        exec_ms_ = wait_ms_ = 0;
        upload_bytes_ = upload_hits_ = 0;
    }

    void LogTargets(const std::vector<TexRef>& rts, int num_rt) {
        if (target_sets_logged_ >= 24 || list_counter_ > 2000) return;
        uint64_t key = 1469598103934665603ull;
        bool any = false;
        for (size_t i = 0; i < rts.size(); i++) {
            const TexRef& t = rts[i];
            if (!t.valid) continue;
            any = true;
            key = (key ^ (t.addr + uint64_t(t.tex.format) * 7919 + i)) * 1099511628211ull;
        }
        if (!any || !logged_targets_.insert(key).second) return;
        std::string desc;
        for (size_t i = 0; i < rts.size(); i++) {
            const TexRef& t = rts[i];
            if (!t.valid) continue;
            char buf[64];
            snprintf(buf, sizeof(buf), "%s%s%dx%d fmt %d", desc.empty() ? "" : ", ",
                     int(i) == num_rt ? "depth " : "", t.tex.width, t.tex.height, t.tex.format);
            desc += buf;
        }
        target_sets_logged_++;
        LOGI(kTag, "render targets: %s", desc.c_str());
    }

    template <typename... A>
    void LogOnce(const char* key, const char* fmt, A... args) {
        if (!logged_once_.insert(key).second) return;
        if constexpr (sizeof...(A) == 0)
            LOGW(kTag, "%s", fmt);
        else
            LOGW(kTag, fmt, args...);
    }

    std::mutex m_;
    bool init_failed_ = false;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props_{};
    VkPhysicalDeviceMemoryProperties mem_props_{};
    VkPhysicalDeviceFeatures features_{};
    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t family_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;  // recorder: the command buffer being recorded
    VkSemaphore acquire_sem_ = VK_NULL_HANDLE, render_sem_ = VK_NULL_HANDLE;
    PFN_vkCmdBeginRendering begin_rendering_ = nullptr;
    PFN_vkCmdEndRendering end_rendering_ = nullptr;
    bool rp_fallback_ = false;  // no dynamic rendering: VkRenderPass + VkFramebuffer per pass
    bool format_list_ = false;  // VkImageFormatListCreateInfo usable (Vulkan 1.2 or VK_KHR_image_format_list)
    // Older GPUs (Adreno 6xx): images get VK_IMAGE_USAGE_STORAGE_BIT only once written as storage images, since
    // the usage turns off their bandwidth compression. storage_addrs_: the game addresses of those.
    bool lean_images_ = false;
    std::unordered_set<uint64_t> storage_addrs_, mutable_addrs_;
    int recreated_ = 0;

    // ---- render pass objects (GPUs without dynamic rendering) ----------------------------------------------
    // Render passes are cached by formats + load operations; framebuffers are made per pass and destroyed once
    // the slot that used them has finished on the GPU. Layouts stay what the explicit barriers set (initial =
    // final = attachment layout), so the passes add no transitions of their own.
    std::mutex rp_mutex_;
    std::map<std::vector<uint32_t>, VkRenderPass> render_passes_cache_;
    std::mutex fb_mutex_;
    std::map<std::vector<uint64_t>, VkFramebuffer> fb_cache_;  // {render pass, w<<32|h, views...}
    // Destroys cached framebuffers that use any of `views` (about to be destroyed; the GPU is done with them).
    void ForgetFramebuffers(const std::vector<VkImageView>& views) {
        if (views.empty()) return;
        std::lock_guard<std::mutex> lk(fb_mutex_);
        for (auto it = fb_cache_.begin(); it != fb_cache_.end();) {
            bool uses = false;
            for (size_t i = 2; i < it->first.size() && !uses; i++)
                for (VkImageView v : views)
                    if (it->first[i] == reinterpret_cast<uint64_t>(v)) {
                        uses = true;
                        break;
                    }
            if (uses) {
                vkDestroyFramebuffer(device_, it->second, nullptr);
                it = fb_cache_.erase(it);
            } else {
                ++it;
            }
        }
    }
    VkRenderPass MakeRenderPass(const VkFormat* colors, uint32_t ncolors, VkFormat depth, VkFormat stencil_hint,
                                const VkAttachmentLoadOp* color_load, VkAttachmentLoadOp depth_load) {
        std::vector<uint32_t> key;
        for (uint32_t i = 0; i < ncolors; i++) {
            key.push_back(uint32_t(colors[i]));
            key.push_back(color_load ? uint32_t(color_load[i]) : 0u);
        }
        key.push_back(0xffffffffu);
        key.push_back(uint32_t(depth));
        key.push_back(uint32_t(depth_load));
        std::lock_guard<std::mutex> lk(rp_mutex_);
        auto it = render_passes_cache_.find(key);
        if (it != render_passes_cache_.end()) return it->second;
        std::vector<VkAttachmentDescription> att;
        std::vector<VkAttachmentReference> refs;
        for (uint32_t i = 0; i < ncolors; i++) {
            if (colors[i] == VK_FORMAT_UNDEFINED) {
                refs.push_back({VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED});
                continue;
            }
            VkAttachmentDescription a{};
            a.format = colors[i];
            a.samples = VK_SAMPLE_COUNT_1_BIT;
            a.loadOp = color_load ? color_load[i] : VK_ATTACHMENT_LOAD_OP_LOAD;
            a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            a.initialLayout = a.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            refs.push_back({uint32_t(att.size()), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
            att.push_back(a);
        }
        VkAttachmentReference dref{VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED};
        if (depth != VK_FORMAT_UNDEFINED) {
            VkAttachmentDescription a{};
            a.format = depth;
            a.samples = VK_SAMPLE_COUNT_1_BIT;
            a.loadOp = depth_load;
            a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            const bool stencil = stencil_hint != VK_FORMAT_UNDEFINED;
            a.stencilLoadOp = stencil ? depth_load : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            a.stencilStoreOp = stencil ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
            a.initialLayout = a.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            dref = {uint32_t(att.size()), VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
            att.push_back(a);
        }
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = uint32_t(refs.size());
        sub.pColorAttachments = refs.data();
        if (dref.attachment != VK_ATTACHMENT_UNUSED) sub.pDepthStencilAttachment = &dref;
        VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        ci.attachmentCount = uint32_t(att.size());
        ci.pAttachments = att.data();
        ci.subpassCount = 1;
        ci.pSubpasses = &sub;
        VkRenderPass rp = VK_NULL_HANDLE;
        if (vkCreateRenderPass(device_, &ci, nullptr, &rp) != VK_SUCCESS) {
            LogOnce("renderpass", "vkCreateRenderPass failed");
            rp = VK_NULL_HANDLE;
        }
        render_passes_cache_[key] = rp;
        return rp;
    }
    VkRenderPass CompatRenderPass(const std::vector<VkFormat>& colors, VkFormat depth, VkFormat stencil) {
        const VkFormat ds = depth != VK_FORMAT_UNDEFINED ? depth : stencil;
        return MakeRenderPass(colors.data(), uint32_t(colors.size()), ds, stencil, nullptr, VK_ATTACHMENT_LOAD_OP_LOAD);
    }
    // Recorder thread.
    void BeginRenderPassObject(VkCommandBuffer cb, const BeginPass& bp) {
        VkFormat colors[8];
        VkAttachmentLoadOp loads[8];
        VkImageView views[9];
        VkClearValue clears[9];
        uint32_t nviews = 0;
        for (uint32_t i = 0; i < bp.count; i++) {
            const bool used = bp.colors[i].imageView != VK_NULL_HANDLE;
            colors[i] = used ? bp.color_formats[i] : VK_FORMAT_UNDEFINED;
            loads[i] = bp.colors[i].loadOp;
            if (used) {
                clears[nviews] = bp.colors[i].clearValue;
                views[nviews++] = bp.colors[i].imageView;
            }
        }
        const bool ds = (bp.has_depth || bp.has_stencil) && bp.depth.imageView != VK_NULL_HANDLE;
        const VkFormat dfmt = ds ? bp.depth_format : VK_FORMAT_UNDEFINED;
        if (ds) {
            clears[nviews] = bp.depth.clearValue;
            views[nviews++] = bp.depth.imageView;
        }
        VkRenderPass rp = MakeRenderPass(colors, bp.count, dfmt, bp.has_stencil ? dfmt : VK_FORMAT_UNDEFINED, loads,
                                         ds ? bp.depth.loadOp : VK_ATTACHMENT_LOAD_OP_LOAD);
        if (!rp) {
            LogOnce("framebuffer", "no render pass object");
            return;
        }
        const uint32_t fw = std::max(1u, bp.area.extent.width), fh = std::max(1u, bp.area.extent.height);
        // Framebuffers are cached by render pass, size and views (creating one per pass cost the driver an
        // mmap/munmap each, a large part of the recorder thread on Adreno 6xx); they go when a view is destroyed.
        std::vector<uint64_t> key;
        key.reserve(nviews + 2);
        key.push_back(reinterpret_cast<uint64_t>(rp));
        key.push_back(uint64_t(fw) << 32 | fh);
        for (uint32_t i = 0; i < nviews; i++) key.push_back(reinterpret_cast<uint64_t>(views[i]));
        VkFramebuffer fb = VK_NULL_HANDLE;
        {
            std::lock_guard<std::mutex> lk(fb_mutex_);
            auto it = fb_cache_.find(key);
            if (it != fb_cache_.end()) fb = it->second;
        }
        if (!fb) {
            VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fi.renderPass = rp;
            fi.attachmentCount = nviews;
            fi.pAttachments = views;
            fi.width = fw;
            fi.height = fh;
            fi.layers = 1;
            if (vkCreateFramebuffer(device_, &fi, nullptr, &fb) != VK_SUCCESS) {
                LogOnce("framebuffer", "vkCreateFramebuffer failed");
                return;
            }
            std::lock_guard<std::mutex> lk(fb_mutex_);
            fb_cache_.emplace(std::move(key), fb);
        }
        VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        bi.renderPass = rp;
        bi.framebuffer = fb;
        bi.renderArea = bp.area;
        bi.clearValueCount = nviews;
        bi.pClearValues = clears;
        vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    }
    bool recording_ = false, rendering_ = false;
    std::vector<uint64_t> rendering_key_;

    VkBuffer stream_ = VK_NULL_HANDLE;
    VkDeviceMemory stream_memory_ = VK_NULL_HANDLE;
    uint8_t* stream_ptr_ = nullptr;
    VkDeviceSize stream_off_ = 0, zero_offset_ = 0;
    std::unordered_map<uint64_t, VkDeviceSize> stream_cache_;
    uint64_t upload_hits_ = 0, upload_bytes_ = 0;
    uint64_t depth_gen_ = 0;
    // HUD box for other aspect ratios (see Draw): size, 0 = off.
    // Whether a draw on the final frame is one of the game's 2D HUD / menu draws to keep in a 16:9 box: its vertex
    // shader's c3 is the 2D projection for a 1280x720 layout (x * 2/1280 - 1, y * -2/720 + 1), and its vertices do
    // not cover the whole 1280x720 screen (full-screen fades and dimming are left to fill the frame).
    // The game's 2D projection in vertex shader c3 (row-major: x scale, y scale, translation -1 / +1), if it is one.
    bool Projection2D(float* sx, float* sy) {
        const BufferRange& c3 = st_.uniforms[0][0];
        if (!c3.address || c3.size < 64 || !GuestRangeValid(c3.address, 64)) return false;
        float m[16];
        memcpy(m, reinterpret_cast<const void*>(c3.address), sizeof(m));
        if (m[1] != 0.0f || m[4] != 0.0f || std::fabs(m[3] + 1.0f) > 1e-4f || std::fabs(m[7] - 1.0f) > 1e-4f) return false;
        *sx = m[0];
        *sy = m[5];
        return true;
    }
    // The 2D extent of a draw's vertices (position attribute at location 0), read from guest memory; false if it
    // cannot be read.
    bool DrawBounds(const DrawParams& p, const VertexInput& vin, float* x0, float* x1, float* y0, float* y1) {
        const VkVertexInputAttributeDescription* pos = nullptr;
        for (const VkVertexInputAttributeDescription& a : vin.attributes)
            if (a.location == 0) pos = &a;
        if (!pos || p.count <= 0 || p.count > 4096 || p.instances != 1) return false;
        uint32_t stride = 0;
        for (const VkVertexInputBindingDescription& b : vin.bindings)
            if (b.binding == pos->binding) stride = b.stride;
        const int fmt = st_.attribs[0].format;
        if (fmt != 21 && fmt != 22 && fmt != 34 && fmt != 46) return false;
        const BufferRange& vb = st_.vertex_buffers[pos->binding];
        const int isz = p.index_type == 0 ? 1 : p.index_type == 1 ? 2 : p.index_type == 2 ? 4 : 0;
        if (isz && (!p.index_address || !GuestRangeValid(p.index_address, size_t(p.count) * isz))) return false;
        *x0 = *y0 = 1e30f;
        *x1 = *y1 = -1e30f;
        for (int k = 0; k < p.count; k++) {
            int64_t v;
            if (isz) {
                const uint8_t* ip = reinterpret_cast<const uint8_t*>(p.index_address);
                v = isz == 1 ? ip[k] : isz == 2 ? reinterpret_cast<const uint16_t*>(ip)[k]
                                                : int64_t(reinterpret_cast<const uint32_t*>(ip)[k]);
                v += p.base_vertex;
            } else {
                v = int64_t(p.first) + k;
            }
            if (v < 0) return false;
            const uint64_t at = uint64_t(v) * stride + pos->offset;
            if (!vb.address || at + 8 > vb.size || !GuestRangeValid(vb.address + at, 8)) return false;
            const uint8_t* src = reinterpret_cast<const uint8_t*>(vb.address + at);
            float x, y;
            if (fmt == 21) {
                int16_t xy[2];  // 1/8-pixel units (0..10240 x 0..5760 for the whole screen)
                memcpy(xy, src, 4);
                x = xy[0] / 8.0f;
                y = xy[1] / 8.0f;
            } else {
                float xy[2];
                memcpy(xy, src, 8);
                x = xy[0];
                y = xy[1];
            }
            *x0 = std::min(*x0, x);
            *x1 = std::max(*x1, x);
            *y0 = std::min(*y0, y);
            *y1 = std::max(*y1, y);
        }
        return true;
    }
    // Whether a draw on the final frame is one of the game's 2D HUD / menu draws to keep in a 16:9 box: its vertex
    // shader's c3 is the 2D projection for a 1280x720 layout (x * 2/1280 - 1, y * -2/720 + 1), it blends, and its
    // vertices do not cover the whole 1280x720 screen (full-screen fades and dimming are left to fill the frame).
    bool HudBoxed(const DrawParams& p, const VertexInput& vin) {
        float sx, sy;
        if (!Projection2D(&sx, &sy) || std::fabs(sx - 2.0f / 1280.0f) > 1e-6f || std::fabs(sy + 2.0f / 720.0f) > 1e-6f)
            return false;
        // HUD and menu draws blend; the game's final full-screen copy of the frame (same projection) does not.
        if (!(st_.color.blend_enable & 1)) return false;
        float x0, x1, y0, y1;
        if (!DrawBounds(p, vin, &x0, &x1, &y0, &y1)) return true;
        const bool full = x0 <= 2.0f && x1 >= 1278.0f && y0 <= 2.0f && y1 >= 718.0f;
        const uint64_t hash = st_.program[0]->hash;
        if (hud_box_logged_.insert(hash * 2 + (full ? 1 : 0)).second)
            LOGI(kTag, "aspect ratio: HUD vertex shader %016llx (%s): %.0f..%.0f x %.0f..%.0f", (unsigned long long)hash,
                 full ? "full screen, not boxed" : "in the 16:9 box", x0, x1, y0, y1);
        if (full) hud_full_screen_++;
        return !full;
    }
    bool bc_patched_ = false, bc_forced_ = false;
    bool mip_skip_ = false;      // settings texture_mip_skip
    uint64_t mip_skipped_ = 0;   // images created without their top level
    int hud_box_w_ = 0, hud_box_h_ = 0;
    std::unordered_set<uint64_t> hud_box_logged_;
    uint64_t hud_full_screen_ = 0;
    uint64_t hud_draws_boxed_ = 0;
    int viewports_logged_ = 0;
    int image_binds_logged_ = 0;
    std::set<uint64_t> mismatch_logged_;
    std::set<int> unfilterable_logged_;
    // frame capture
    bool capture_active_ = false, capture_clock_started_ = false;
    int captures_done_ = 0;
    std::atomic<int64_t> capture_request_ms_{0};
    uint32_t capture_pass_ = 0, pass_draws_ = 0;
    uint64_t pass_fs_hash_ = 0, last_list_draws_ = 0, list_start_draws_ = 0;
    uint64_t pass_fs_addr_ = 0, pass_vs_addr_ = 0;
    std::chrono::steady_clock::time_point next_capture_;
    std::vector<Image*> pass_written_;
    std::string pass_tex_desc_, pass_ubo_desc_, pass_state_desc_;
    static constexpr uint32_t kMaxQueries = 2048;
    VkQueryPool query_pool_ = VK_NULL_HANDLE;
    static constexpr uint32_t kMaxTimestamps = 256;
    VkQueryPool ts_pool_ = VK_NULL_HANDLE;
    uint32_t ts_count_ = 0, ts_pass_[kMaxTimestamps] = {};
    uint64_t render_passes_ = 0, barriers_ = 0;
    std::map<int, uint64_t> pass_breaks_;
    std::map<std::string, uint64_t> sampled_breaks_;
    std::vector<uint32_t> query_pass_;
    uint64_t ubo_desc_draw_ = ~0ull;
    int world_draws_logged_ = 0;
    struct DrawImage {
        int stage, slot;
        Image* img;
    };
    std::vector<DrawImage> draw_images_;  // textures resolved for the current draw (captures only)
    uint64_t draw_images_draw_ = ~0ull;
    uint64_t tex_desc_draw_ = ~0ull;
    std::vector<CaptureItem> capture_items_;
    VkExtent2D no_attachment_extent_{0, 0};  // render area for a draw with no attachments (storage-only)
    uint64_t fast_draws_ = 0, big_checks_ = 0, big_changes_ = 0;
    std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> index_ranges_;
    VkPipelineCache pipeline_cache_ = VK_NULL_HANDLE;
    int new_pipelines_ = 0;
    double compile_ms_ = 0;
    std::unordered_set<uint64_t> viewport_keys_;
    std::set<std::string> view_mismatch_logged_;
    uint64_t view_standins_ = 0;
    int capture_draws_logged_ = 0;
    int geo_dumps_ = 0;
    std::unordered_map<uint64_t, uint64_t> snapshot_gen_;
    double exec_ms_ = 0, wait_ms_ = 0;
    VkDescriptorPool desc_pool_ = VK_NULL_HANDLE;

    void* window_ = nullptr;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    std::atomic<bool> swapchain_dirty_{false};  // set by the recorder (acquire/present), read by the render thread
    VkExtent2D sc_extent_{};
    int window_fps_ = 0;  // the rate last given to ANativeWindow_setFrameRate
    int frame_w_ = 0, frame_h_ = 0;  // the game's presented frame the swapchain is sized for (compositor_scaling)
    std::vector<VkImage> sc_images_;

    DrawState st_;
    Shader no_fragment_;  // stands in for an absent pixel program (ok = false, no module)
    std::unordered_map<uint64_t, Image> images_;
    std::unordered_map<int, std::pair<VkFormat, VkFormatFeatureFlags>> format_cache_;
    std::unordered_map<VkFormat, bool> vertex_format_ok_;
    std::unordered_map<std::vector<uint32_t>, Shader, VecHash> shaders_;
    std::unordered_map<std::vector<uint32_t>, VkDescriptorSetLayout, VecHash> set_layouts_;
    std::unordered_map<std::vector<uint32_t>, VkPipelineLayout, VecHash> pipeline_layouts_;
    std::unordered_map<std::vector<uint32_t>, VkPipeline, VecHash> pipelines_;
    std::map<std::vector<uint32_t>, VkSampler> samplers_;
    // A texture -> buffer copy: the GPU copies into the slot's stream; once the slot's work has finished the
    // rows are written to game memory.
    struct Readback {
        VkDeviceSize off;
        uint64_t dst;
        size_t row, rows, depth, dst_stride, dst_image_stride;
        bool texture = false;  // dst is a texture's storage: written in its own layout (level 0, layer 0)
        TextureState tex{};
        int report = -1;       // 0: write the 32-bit `value`; 1: write a counter report {counter, timestamp}
        uint32_t value = 0;
    };
    struct Slot {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        bool pending = false;
        std::vector<Readback> readbacks;
        VkQueryPool ts = VK_NULL_HANDLE;  // GPU time of the submission (start, end)
        bool ts_written = false;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        std::vector<VkDescriptorPool> extra_pools;  // when `pool` fills up (recorder thread)
        std::vector<VkFramebuffer> framebuffers;    // render pass fallback: destroyed when the slot is reused
        uint64_t submits_queued = 0;                // render thread
        uint64_t seq = 0;                           // submit_seq_ of its last submission
        std::atomic<uint64_t> submits_done{0};      // recorder, after vkQueueSubmit
        size_t extra_used = 0;
        VkBuffer stream = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        uint8_t* ptr = nullptr;
    };
    // Each frame makes two submissions (its command list, then the present): four slots let the next frame
    // record while the previous frame is still on the GPU (with two, it waited for the frame just submitted).
    static constexpr int kSlots = 4;
    Slot slots_[kSlots];
    int slot_ = 0;      // render thread: the slot being filled
    int rec_slot_ = 0;  // recorder: the slot whose descriptor pools it allocates from
    // Hashed fronts of the caches above (no allocation per lookup).
    std::unordered_map<uint64_t, VkSampler> samplers_fast_;
    std::unordered_map<uint64_t, Shader*> shaders_fast_;
    std::unordered_map<uint64_t, VkPipeline> pipelines_fast_;
    std::unordered_map<uint64_t, std::pair<std::vector<uint32_t>, VkDescriptorSetLayout>> set_layouts_fast_;
    VkDescriptorSetLayout last_pl_vs_ = VK_NULL_HANDLE, last_pl_fs_ = VK_NULL_HANDLE;
    VkPipelineLayout last_pl_ = VK_NULL_HANDLE;
    std::unordered_map<uint64_t, std::pair<VkDescriptorSet, VkDescriptorSet>> desc_cache_;
    // Descriptor sets kept across lists (most draws bind the same textures frame after frame): their own pool,
    // emptied when an image is destroyed (the GPU is idle then) or when it is full.
    VkDescriptorPool persist_pool_ = VK_NULL_HANDLE;
    std::unordered_map<uint64_t, std::pair<VkDescriptorSet, VkDescriptorSet>> persist_cache_;
    uint64_t persist_hits_ = 0, persist_resets_ = 0;
    uint64_t desc_reused_ = 0;
    VkPipeline bound_pipeline_ = VK_NULL_HANDLE;
    // Last recorded dynamic state, vertex/index bindings and push constants (skipped when unchanged).
    DynState dyn_{};
    bool dyn_valid_ = false;
    std::atomic<bool> persist_full_{false};  // set by the recorder; the render thread empties the pool
    uint64_t skipped_desc_ = 0, desc_pools_added_ = 0;
    std::pair<VkBuffer, VkDeviceSize> bound_vb_[16] = {};
    std::pair<VkBuffer, VkDeviceSize> bound_ib_ = {};
    VkIndexType bound_ib_type_ = VK_INDEX_TYPE_MAX_ENUM;
    float bound_push_[4] = {};
    bool push_valid_ = false;
    VkPipelineLayout bound_layout_ = VK_NULL_HANDLE;
    VkDescriptorSet bound_sets_[2] = {};
    uint32_t bound_dyn_[36] = {};
    uint32_t bound_dyn_count_ = 0;

    // diagnostics
    uint64_t last_video_present_ = 0, video_cadence_[5] = {};
    uint64_t presented_ = 0, drawn_ = 0, list_counter_ = 0, reuploads_ = 0;
    int swapchains_created_ = 0, images_created_ = 0, clear_colors_logged_ = 0, copies_logged_ = 0,
        target_sets_logged_ = 0, shaders_translated_ = 0, shader_failures_logged_ = 0, uploads_ = 0, flushes_ = 0,
        b2t_logged_ = 0, constants_logged_ = 0, handles_logged_ = 0;
    bool logged_first_image_ = false;
    std::map<std::string, uint64_t> skipped_;
    std::set<int> unknown_formats_, program_binds_logged_;
    std::map<int, Image*> fallbacks_;
    std::set<uint64_t> logged_clears_, logged_targets_;
    std::set<std::string> logged_once_, missing_ubo_logged_, ubo_binds_logged_;
    std::chrono::steady_clock::time_point last_report_ = std::chrono::steady_clock::now();
};

}  // namespace

Backend* CreatePlatformBackend() { return new VulkanBackend; }

}  // namespace dsr::hle::nvn

#endif  // __ANDROID__
