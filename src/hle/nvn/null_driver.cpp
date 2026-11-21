// "Null" NVN driver: accepts every NVN call, tracks the object state the game reads back (sizes,
// mapped pointers, GPU addresses, texture dimensions), and renders nothing. It lets the game get past
// graphics initialization so the rest of the runtime can be exercised, and it is the skeleton the
// Vulkan backend will fill in.
//
// Functions not listed here still get a logging stub (see nvn.cpp); stubs of names containing
// "Initialize" or ending in "SetShaders" report success.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <dirent.h>
#include <string>
#include <unistd.h>
#include <vector>
#include <map>
#include <mutex>
#include <unordered_map>
#include <thread>

#include "common/platform.h"
#include "common/log.h"
#include "hle/nvn.h"
#include "hle/nvn/backend.h"
#include "runtime/runtime.h"
#include "hle/nvn/layout.h"
#include "hle/nvn/state.h"
#include "hle/nvn/shader_dump.h"
#include "hle/nvn/nvn_types.h"
#include "hle/os/os.h"

namespace dsr::hle::nvn {

// ---- formats ------------------------------------------------------------------------------------
FormatInfo GetFormatInfo(int f) {
    if (f >= 1 && f <= 4) return {1, 1, 1};     // R8*
    if (f >= 5 && f <= 9) return {1, 1, 2};     // R16*
    if (f >= 10 && f <= 12) return {1, 1, 4};   // R32*
    if (f >= 13 && f <= 16) return {1, 1, 2};   // RG8*
    if (f >= 17 && f <= 21) return {1, 1, 4};   // RG16*
    if (f >= 22 && f <= 24) return {1, 1, 8};   // RG32*
    if (f >= 25 && f <= 28) return {1, 1, 4};   // RGB8* (padded)
    if (f >= 29 && f <= 33) return {1, 1, 8};   // RGB16*
    if (f >= 34 && f <= 36) return {1, 1, 16};  // RGB32*
    if (f >= 37 && f <= 40) return {1, 1, 4};   // RGBA8*
    if (f >= 41 && f <= 45) return {1, 1, 8};   // RGBA16*
    if (f >= 46 && f <= 48) return {1, 1, 16};  // RGBA32*
    if (f == 49) return {1, 1, 1};              // STENCIL8
    if (f == 50) return {1, 1, 2};              // DEPTH16
    if (f >= 51 && f <= 53) return {1, 1, 4};   // DEPTH24, DEPTH32F, D24S8
    if (f == 54) return {1, 1, 8};              // D32F_S8
    if (f == 55 || f == 56) return {1, 1, 4};   // RGBX8/RGBA8 sRGB
    if (f >= 57 && f <= 60) return {1, 1, 2};   // RGBA4, RGB5, RGB5A1, RGB565
    if (f >= 61 && f <= 64) return {1, 1, 4};   // RGB10A2, R11G11B10F, RGB9E5
    if (f == 65 || f == 66 || f == 69 || f == 70 || f == 73 || f == 74) return {4, 4, 8};  // DXT1, RGTC1
    if (f >= 65 && f <= 80) return {4, 4, 16};  // DXT3/5, RGTC2, BPTC
    if (f >= 109 && f <= 112) return {1, 1, 4};   // RGBX8 (unorm, snorm, uint, sint)
    if (f >= 113 && f <= 117) return {1, 1, 8};   // RGBX16
    if (f >= 118 && f <= 120) return {1, 1, 16};  // RGBX32
    return {1, 1, 8};                           // unknown: generous
}

namespace {
constexpr const char* kTag = "nvn.null";

Table<MemoryPoolState> g_pool_builders, g_pools;
Table<BufferState> g_buffer_builders, g_buffers;
Table<TextureState> g_texture_builders, g_textures;
std::mutex g_tex_addr_mutex;
std::unordered_map<uint64_t, TextureState> g_tex_by_addr;  // textures by storage address
Table<WindowState> g_window_builders, g_windows;
Table<QueueBuilderState> g_queue_builders;

size_t AlignUp(size_t v, size_t a) { return (v + a - 1) / a * a; }

// Exact Tegra block-linear (or pitch) size, so pool allocations and packaged data line up.
size_t StorageSize(const TextureState& t) { return AlignUp(std::max<size_t>(ComputeLayout(t).total, 512), 512); }

uintptr_t PoolBase(const void* pool) { return reinterpret_cast<uintptr_t>(g_pools.Copy(pool).storage); }

// ---- device ----------------------------------------------------------------------------------------
// Returns 0 so getters that fall back to it never hand garbage to the game.
uint64_t Nop() { return 0; }
// CPU reads of texture contents: not emulated (GPU-rendered contents never reach game memory this way).
uint64_t TexReadTexels(const void* tex) {
    static std::atomic<int> n{0};
    if (n++ < 4) LOGI(kTag, "nvnTextureReadTexels(%p): not emulated", tex);
    return 0;
}
bool True() { return true; }

bool DeviceInitialize(void* device, const void* builder) {
    LOGI(kTag, "nvnDeviceInitialize(%p) — null renderer (no GPU output yet)", device);
    return true;
}

void DeviceGetInteger(const void* device, int pname, int* v) {
    switch (pname) {
        case kApiMajorVersion: *v = 53; break;
        case kApiMinorVersion: *v = 105; break;
        case 2: *v = 14; break;       // uniform buffer bindings per stage
        case 3: *v = 65536; break;    // max uniform buffer size
        case kUniformBufferAlignment: *v = 256; break;
        case 5: *v = 8; break;        // color buffer bindings
        case 6: *v = 16; break;       // vertex buffer bindings
        case 7: *v = 4; break;        // transform feedback bindings
        case 8: *v = 16; break;       // storage buffer bindings per stage
        case 9: *v = 32; break;       // texture bindings per stage
        case 10: *v = 16; break;      // counter alignment
        case 11: case 12: case 13: *v = 4; break;
        case 14: *v = 16; break;      // vertex attributes
        case kTextureDescriptorSize: *v = 32; break;
        case kSamplerDescriptorSize: *v = 32; break;
        case 17: case 18: *v = 256; break;  // reserved texture/sampler descriptors
        case kCommandBufferCommandAlignment: *v = 4; break;
        case kCommandBufferControlAlignment: *v = 8; break;
        case 21: *v = 4096; break;    // min command size
        case 22: *v = 4096; break;    // min control size
        case 23: *v = 128; break;     // scratch scale factor minimum
        case 24: *v = 256; break;     // scratch scale factor recommended
        case 25: *v = 4096; break;    // scratch alignment
        case 26: *v = 128 * 1024; break;  // scratch granularity
        case 27: *v = 16; break;      // max anisotropy
        case 36: *v = 1 << 20; break; // max texture pool size
        case 37: *v = 4096; break;    // max sampler pool size
        case 38: *v = 16; break;      // max viewports
        case 39: *v = 4096; break;    // texture object page alignment
        case 43: *v = 256 * 1024; break;  // L2 size
        case 44: *v = 15; break;      // max texture levels
        case 45: *v = 2048; break;    // max texture layers
        case kLinearTextureStrideAlignment: *v = 32; break;
        case 57: *v = 128; break;     // linear render target stride alignment
        case 58: *v = 4096; break;    // memory pool page size
        case kQueueCommandMemoryDefaultSize: *v = 0x10000; break;
        default:
            // Leave the caller's pre-filled default untouched.
            LOGD(kTag, "nvnDeviceGetInteger: unknown pname %d (left at %d)", pname, *v);
            break;
    }
}

uint64_t DeviceGetTextureHandle(const void*, int texture_id, int sampler_id) {
    return (uint64_t(uint32_t(sampler_id)) << 20) | uint32_t(texture_id);
}
uint64_t DeviceGetImageHandle(const void*, int image_id) { return uint32_t(image_id); }
uint64_t DeviceGetSeparateHandle(const void*, int id) { return uint32_t(id); }
uint64_t DeviceGetTimestampNs(const void*) { return uint64_t(os::NowNs()); }

// ---- memory pools ------------------------------------------------------------------------------------
void PoolBuilderSetDefaults(void* b) { g_pool_builders.Reset(b); }
void PoolBuilderSetFlags(void* b, int flags) { g_pool_builders.Get(b).flags = flags; }
void PoolBuilderSetStorage(void* b, void* mem, size_t size) {
    auto& s = g_pool_builders.Get(b);
    s.storage = static_cast<uint8_t*>(mem);
    s.size = size;
}
std::mutex g_pool_range_mutex;
std::map<uintptr_t, size_t> g_pool_ranges;  // storage base -> size
std::atomic<uint64_t> g_pool_ranges_version{1};  // bumped on every change (GuestRangeValid's per-thread cache)

bool PoolInitialize(void* pool, const void* b) {
    MemoryPoolState s = g_pool_builders.Copy(b);
    g_pools.Get(pool) = s;
    if (s.storage) {
        std::lock_guard<std::mutex> lk(g_pool_range_mutex);
        g_pool_ranges[reinterpret_cast<uintptr_t>(s.storage)] = s.size;
        g_pool_ranges_version++;
    }
    return true;
}
void PoolFinalize(void* pool) {
    MemoryPoolState s = g_pools.Copy(pool);
    if (s.storage) {
        std::lock_guard<std::mutex> lk(g_pool_range_mutex);
        g_pool_ranges.erase(reinterpret_cast<uintptr_t>(s.storage));
        g_pool_ranges_version++;
    }
    g_pools.Erase(pool);
}
void* PoolMap(const void* pool) { return g_pools.Copy(pool).storage; }
uint64_t PoolGetBufferAddress(const void* pool) { return PoolBase(pool); }
size_t PoolGetSize(const void* pool) { return g_pools.Copy(pool).size; }
int PoolGetFlags(const void* pool) { return g_pools.Copy(pool).flags; }

// ---- buffers -------------------------------------------------------------------------------------------
void BufferBuilderSetDefaults(void* b) { g_buffer_builders.Reset(b); }
void BufferBuilderSetStorage(void* b, const void* pool, int64_t offset, size_t size) {
    g_buffer_builders.Get(b) = {pool, offset, size};
}
// GPU address -> buffer size, so shader code handed over by address can be sized exactly.
std::mutex g_buffer_addr_mutex;
std::map<uint64_t, size_t> g_buffer_by_addr;

bool BufferInitialize(void* buf, const void* b) {
    BufferState s = g_buffer_builders.Copy(b);
    g_buffers.Get(buf) = s;
    std::lock_guard<std::mutex> lk(g_buffer_addr_mutex);
    g_buffer_by_addr[PoolBase(s.pool) + uint64_t(s.offset)] = s.size;
    return true;
}
// Size of the buffer region containing addr (0 if none).
size_t BufferBytesFrom(uint64_t addr) {
    std::lock_guard<std::mutex> lk(g_buffer_addr_mutex);
    auto it = g_buffer_by_addr.upper_bound(addr);
    if (it == g_buffer_by_addr.begin()) return 0;
    --it;
    if (addr >= it->first + it->second) return 0;
    return size_t(it->first + it->second - addr);
}

// nvnProgramSetShaders(program, count, const NVNshaderData*): NVNshaderData = { u64 code address; const
// void* control }. The code is kept per program for the backend (and dumped for offline work).
struct ShaderData {
    uint64_t address;
    const void* control;
};
bool ProgramSetShaders(void* program, int count, const ShaderData* stages) {
    for (int i = 0; i < count && stages; i++) {
        uint64_t a = stages[i].address;
        size_t n = BufferBytesFrom(a);
        if (!n || n > (4u << 20)) {
            // Not inside a known buffer: scan for the end-of-program self-branch (BRA to itself).
            n = 0;
            const uint64_t* q = reinterpret_cast<const uint64_t*>(a);
            for (size_t k = 0; a && k < 0x4000 / 8; k++)
                if ((q[k] & 0xFFFFFFFFFF0FFFFFull) == 0xE2400FFFFF07000Full) {
                    n = (k + 1) * 8;
                    break;
                }
        }
        // Control section: header word 5 is its size (magic 0x98761234).
        size_t control_size = 0;
        if (stages[i].control) {
            uint32_t hdr[6];
            memcpy(hdr, stages[i].control, sizeof(hdr));
            control_size = (hdr[0] == 0x98761234u && hdr[5] >= 24 && hdr[5] <= 0x10000) ? hdr[5] : 0x800;
        }
        // The whole GPU section (control header word 8) extends past the code; dump it so the location of
        // the compiler's constant pool (read by shaders as c[1]) can be found.
        size_t section = n;
        if (stages[i].control) {
            uint32_t hdr[9];
            memcpy(hdr, stages[i].control, sizeof(hdr));
            if (hdr[0] == 0x98761234u && hdr[8] > n && hdr[8] <= 0x10000 && GuestRangeValid(a, hdr[8])) section = hdr[8];
        }
        // The shader dump (hash + disk write per shader) is a diagnostics tool: only with auto_capture on.
        if (dsr::GetRuntime().config.auto_capture)
            DumpShader(a, reinterpret_cast<const void*>(a), section, stages[i].control, control_size);
        if (a && n >= 0x88) {
            auto sc = std::make_shared<ShaderCode>();
            const uint8_t* p = reinterpret_cast<const uint8_t*>(a);
            sc->code.assign(p, p + n);
            sc->address = a;
            if (stages[i].control) {
                const uint8_t* c = static_cast<const uint8_t*>(stages[i].control);
                sc->control.assign(c, c + control_size);
                if (control_size >= 0x704) {
                    uint32_t csize, coff;
                    memcpy(&csize, c + 0x6fc, 4);
                    memcpy(&coff, c + 0x700, 4);
                    if (csize && csize <= 0x10000 && coff >= n && coff < 0x100000 && GuestRangeValid(a + coff, csize))
                        sc->constants.assign(p + coff, p + coff + csize);
                    static int logged = 0;
                    if (csize && logged < 6) {
                        logged++;
                        float f[2] = {};
                        if (sc->constants.size() >= 8) memcpy(f, sc->constants.data(), 8);
                        LOGI("nvn.null", "shader constants: %u bytes at +0x%x%s (%g, %g, ...)", csize, coff,
                             sc->constants.empty() ? " NOT READABLE" : "", f[0], f[1]);
                    }
                }
            }
            uint64_t h = 1469598103934665603ull;
            for (uint8_t c : sc->code) h = (h ^ c) * 1099511628211ull;
            sc->hash = h;
            uint32_t sph0;
            memcpy(&sph0, p + 0x30, 4);
            const uint32_t type = (sph0 >> 10) & 0xF;
            sc->stage = type == 1 ? 0 : type == 5 ? 1 : -1;
            SetProgramCode(program, std::move(sc));
        }
    }
    return true;
}
void BufferFinalize(void* buf) { g_buffers.Erase(buf); }
void* BufferMap(const void* buf) {
    BufferState s = g_buffers.Copy(buf);
    uintptr_t base = PoolBase(s.pool);
    return base ? reinterpret_cast<void*>(base + s.offset) : nullptr;
}
uint64_t BufferGetAddress(const void* buf) {
    BufferState s = g_buffers.Copy(buf);
    return PoolBase(s.pool) + uint64_t(s.offset);
}
size_t BufferGetSize(const void* buf) { return g_buffers.Copy(buf).size; }

// ---- textures --------------------------------------------------------------------------------------------
void TexBuilderSetDefaults(void* b) { g_texture_builders.Reset(b); }
void TexBuilderSetFlags(void* b, int v) { g_texture_builders.Get(b).flags = v; }
void TexBuilderSetTarget(void* b, int v) { g_texture_builders.Get(b).target = v; }
void TexBuilderSetWidth(void* b, int v) { g_texture_builders.Get(b).width = v; }
void TexBuilderSetHeight(void* b, int v) { g_texture_builders.Get(b).height = v; }
void TexBuilderSetDepth(void* b, int v) { g_texture_builders.Get(b).depth = v; }
void TexBuilderSetSize1D(void* b, int w) {
    auto& t = g_texture_builders.Get(b);
    t.width = w;
    t.height = t.depth = 1;
}
void TexBuilderSetSize2D(void* b, int w, int h) {
    auto& t = g_texture_builders.Get(b);
    t.width = w;
    t.height = h;
    t.depth = 1;
}
void TexBuilderSetSize3D(void* b, int w, int h, int d) {
    auto& t = g_texture_builders.Get(b);
    t.width = w;
    t.height = h;
    t.depth = d;
}
void TexBuilderSetLevels(void* b, int v) { g_texture_builders.Get(b).levels = v; }
void TexBuilderSetFormat(void* b, int v) { g_texture_builders.Get(b).format = v; }
void TexBuilderSetSamples(void* b, int v) { g_texture_builders.Get(b).samples = v; }
void TexBuilderSetStride(void* b, int64_t v) { g_texture_builders.Get(b).stride = int(v); }
void TexBuilderSetStorage(void* b, const void* pool, int64_t offset) {
    auto& t = g_texture_builders.Get(b);
    t.pool = pool;
    t.offset = offset;
}
void TexBuilderSetSwizzle(void* b, int r, int g, int bl, int a) {
    auto& t = g_texture_builders.Get(b);
    t.swizzle[0] = uint8_t(r);
    t.swizzle[1] = uint8_t(g);
    t.swizzle[2] = uint8_t(bl);
    t.swizzle[3] = uint8_t(a);
}
void TexBuilderSetPackagedData(void* b, const void* data) { g_texture_builders.Get(b).packaged_data = data; }
void TexBuilderSetPackagedLayout(void* b, const void* layout) {
    if (layout) memcpy(g_texture_builders.Get(b).packaged_layout, layout, 8);
}
size_t TexBuilderGetStorageSize(const void* b) { return StorageSize(g_texture_builders.Copy(b)); }
size_t TexBuilderGetStorageAlignment(const void* b) { return 512; }
int TexBuilderGetWidth(const void* b) { return g_texture_builders.Copy(b).width; }
int TexBuilderGetHeight(const void* b) { return g_texture_builders.Copy(b).height; }
int TexBuilderGetDepth(const void* b) { return g_texture_builders.Copy(b).depth; }
int TexBuilderGetLevels(const void* b) { return g_texture_builders.Copy(b).levels; }
int TexBuilderGetFormat(const void* b) { return g_texture_builders.Copy(b).format; }
int TexBuilderGetTarget(const void* b) { return g_texture_builders.Copy(b).target; }

bool TexInitialize(void* tex, const void* b) {
    TextureState t = g_texture_builders.Copy(b);
    // Textures built from a texture package: NVN copies the (already tiled) data into the storage.
    if (t.packaged_data && t.pool) {
        uint8_t* dst = reinterpret_cast<uint8_t*>(PoolBase(t.pool) + uint64_t(t.offset));
        const size_t n = ComputeLayout(t).total;
        if (dst && dst != t.packaged_data) memcpy(dst, t.packaged_data, n);
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 6)
            LOGI(kTag, "packaged texture %dx%dx%d format %d levels %d: %zu bytes, layout %02x %02x %02x %02x %02x %02x %02x %02x",
                 t.width, t.height, t.depth, t.format, t.levels, n, t.packaged_layout[0], t.packaged_layout[1],
                 t.packaged_layout[2], t.packaged_layout[3], t.packaged_layout[4], t.packaged_layout[5],
                 t.packaged_layout[6], t.packaged_layout[7]);
    }
    t.packaged_data = nullptr;
    g_textures.Get(tex) = t;
    // A new texture at this storage (the game reuses texture memory as it streams textures in): check the
    // memory at the texture's next use.
    if (t.pool) GetBackend().MarkTextureDirty(PoolBase(t.pool) + uint64_t(t.offset));
    if (t.pool) {
        std::lock_guard<std::mutex> lk(g_tex_addr_mutex);
        g_tex_by_addr[PoolBase(t.pool) + uint64_t(t.offset)] = t;
    }
    return true;
}

// The texture (storage address or NVNtexture object) the video decoder fills: its storage address, if it is
// a single-level-0-writable RGBA8 texture at least w x h. 0 otherwise.
uint64_t VideoTextureStorage(uint64_t addr, int w, int h) {
    TextureState t;
    {
        std::lock_guard<std::mutex> lk(g_tex_addr_mutex);
        auto it = g_tex_by_addr.find(addr);
        if (it != g_tex_by_addr.end()) t = it->second;
    }
    if (!t.pool) {
        // Not a storage address: an NVNtexture object (the movie decoder gets the game's texture object).
        t = g_textures.Copy(reinterpret_cast<const void*>(addr));
        if (!t.pool) return 0;
        addr = PoolBase(t.pool) + uint64_t(t.offset);
    }
    const FormatInfo fi = GetFormatInfo(t.format);
    if (fi.bytes_per_block != 4 || fi.block_w != 1 || fi.block_h != 1) return 0;
    if (t.width != w || t.height != h) return 0;
    return addr;
}
void TexFinalize(void* tex) { g_textures.Erase(tex); }
int TexGetWidth(const void* t) { return g_textures.Copy(t).width; }
int TexGetHeight(const void* t) { return g_textures.Copy(t).height; }
int TexGetDepth(const void* t) { return g_textures.Copy(t).depth; }
int TexGetLevels(const void* t) { return g_textures.Copy(t).levels; }
int TexGetFormat(const void* t) { return g_textures.Copy(t).format; }
int TexGetSamples(const void* t) { return g_textures.Copy(t).samples; }
int TexGetTarget(const void* t) { return g_textures.Copy(t).target; }
size_t TexGetStorageSize(const void* t) { return StorageSize(g_textures.Copy(t)); }
uint64_t TexGetTextureAddress(const void* t) {
    TextureState s = g_textures.Copy(t);
    return PoolBase(s.pool) + uint64_t(s.offset);
}

// nvnTextureWriteTexels: the CPU writes linear texel data into the texture's (tiled) storage, as the
// real driver does. The backend notices the changed storage and re-uploads (video frames use this).
void TexWriteTexelsStrided(const void* tex, const void* /*view*/, const CopyRegion* region, const void* data,
                           int64_t row_stride, int64_t image_stride) {
    if (!tex || !region || !data) return;
    TextureState t = g_textures.Copy(tex);
    uint8_t* base = reinterpret_cast<uint8_t*>(PoolBase(t.pool) + uint64_t(t.offset));
    if (!t.pool || !base) return;
    const TextureLayout layout = ComputeLayout(t);
    if (layout.levels.empty()) return;
    const LevelLayout& l = layout.levels[0];
    const FormatInfo fi = GetFormatInfo(t.format);
    const bool is_3d = t.target == kTarget3D;
    const int w = std::max(0, region->width), h = std::max(1, region->height), d = std::max(1, region->depth);
    const uint32_t x_bytes = uint32_t(region->x / fi.block_w * fi.bytes_per_block);
    const uint32_t y = uint32_t(region->y / fi.block_h);
    const uint32_t w_bytes = uint32_t((w + fi.block_w - 1) / fi.block_w * fi.bytes_per_block);
    const uint32_t rows = uint32_t((h + fi.block_h - 1) / fi.block_h);
    if (x_bytes + w_bytes > uint32_t(l.row_bytes) || y + rows > uint32_t(l.rows)) return;
    const size_t row_pitch = row_stride > 0 ? size_t(row_stride) : w_bytes;
    const size_t slice_pitch = image_stride > 0 ? size_t(image_stride) : row_pitch * rows;
    const uint8_t* src = static_cast<const uint8_t*>(data);
    if (is_3d) {
        WriteLevelRegion(base + l.offset, l, layout.linear, x_bytes, y, uint32_t(region->z), w_bytes, rows,
                         uint32_t(d), src, row_pitch, slice_pitch);
    } else {  // array layers / cube faces are separate layers
        for (int z = 0; z < d; z++) {
            const int layer = region->z + z;
            if (layer < 0 || layer >= layout.layers) break;
            WriteLevelRegion(base + size_t(layer) * layout.layer_stride + l.offset, l, layout.linear, x_bytes, y, 0,
                             w_bytes, rows, 1, src + size_t(z) * slice_pitch, row_pitch, slice_pitch);
        }
    }
    GetBackend().MarkTextureDirty(uint64_t(reinterpret_cast<uintptr_t>(base)));
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 4)
        LOGI(kTag, "nvnTextureWriteTexels %dx%d at %d,%d into %dx%d format %d", w, h, region->x, region->y, t.width,
             t.height, t.format);
}
void TexWriteTexels(const void* tex, const void* view, const CopyRegion* region, const void* data) {
    TexWriteTexelsStrided(tex, view, region, data, 0, 0);
}

// ---- queues ------------------------------------------------------------------------------------------------
void QueueBuilderSetDefaults(void* b) { g_queue_builders.Reset(b); }
void QueueBuilderSetCommandMemorySize(void* b, size_t s) { g_queue_builders.Get(b).command_memory = s; }
void QueueBuilderSetComputeMemorySize(void* b, size_t s) { g_queue_builders.Get(b).compute_memory = s; }
size_t QueueBuilderGetQueueMemorySize(const void* b) {
    QueueBuilderState s = g_queue_builders.Copy(b);
    return AlignUp(s.command_memory + s.compute_memory + 0x10000, 0x1000);
}
bool QueueInitialize(void* q, const void* b) {
    LOGI(kTag, "nvnQueueInitialize(%p)", q);
    return true;
}

// Present: pace to the configured rate (settings: 30 as on Switch, or 60), then let the backend show the
// window texture. Every 10 s, report what the game submitted per frame and how even the frame times were.
// Queue position of each window texture's latest present (window acquire syncs wait for it).
std::mutex g_present_seq_mutex;
std::map<std::pair<const void*, int>, uint64_t> g_present_seqs;
void NoteWindowTexturePresent(const void* window, int index, uint64_t seq) {
    std::lock_guard<std::mutex> lk(g_present_seq_mutex);
    g_present_seqs[{window, index}] = seq;
}
uint64_t WindowTexturePresentSeq(const void* window, int index) {
    std::lock_guard<std::mutex> lk(g_present_seq_mutex);
    auto it = g_present_seqs.find({window, index});
    return it == g_present_seqs.end() ? 0 : it->second;
}

// Every 10 s: the CPU load of each thread of the process (100% = one core), from /proc, to see which thread limits the
// frame (the game's own logic thread, the render thread, the recorder, the decoders...).
static void LogThreadLoad(double wall_s) {
    static std::map<int, unsigned long long> prev;
    struct Row {
        std::string name;
        double pct;
        int tid;
    };
    std::vector<Row> rows;
    DIR* d = opendir("/proc/self/task");
    if (!d) return;
    const double hz = double(sysconf(_SC_CLK_TCK));
    while (dirent* e = readdir(d)) {
        const int tid = atoi(e->d_name);
        if (tid <= 0) continue;
        char path[64];
        snprintf(path, sizeof path, "/proc/self/task/%d/stat", tid);
        FILE* f = fopen(path, "r");
        if (!f) continue;
        char buf[1024];
        const size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[n] = 0;
        const char* open = strchr(buf, '(');
        const char* close = strrchr(buf, ')');
        if (!open || !close || close < open) continue;
        const std::string name(open + 1, size_t(close - open - 1));
        unsigned long long ut = 0, st = 0;
        if (sscanf(close + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) != 2) continue;
        const unsigned long long total = ut + st;
        auto it = prev.find(tid);
        const unsigned long long before = it == prev.end() ? total : it->second;
        prev[tid] = total;
        const double pct = 100.0 * double(total - before) / hz / std::max(wall_s, 0.001);
        if (pct >= 3.0) rows.push_back({name, pct, tid});
    }
    closedir(d);
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.pct > b.pct; });
    std::string out;
    double sum = 0;
    for (const Row& r : rows) sum += r.pct;
    for (size_t i = 0; i < rows.size() && i < 10; i++) {
        char one[96];
        snprintf(one, sizeof one, "%s%s(%d) %.0f%%", i ? ", " : "", rows[i].name.c_str(), rows[i].tid, rows[i].pct);
        out += one;
    }
    LOGI(kTag, "thread load (last %.0f s, 100%% = one core, total %.0f%%): %s", wall_s, sum, out.c_str());
}

void QueuePresentTexture(void* queue, void* window, int index) {
    using clock = std::chrono::steady_clock;
    static thread_local clock::time_point next = clock::now();
    static std::atomic<uint64_t> frames{0};
    static clock::time_point report = clock::now();
    static GpuStats last{};
    // In the background the game's render thread waits here, which holds the whole frame loop.
    const bool was_paused = dsr::platform::WaitWhilePaused() > 0;
    const int fps = dsr::FrameRateNow();  // 30 while a movie plays
    const auto frame = std::chrono::microseconds(1000000 / fps);
    auto now = clock::now();
    // Frame times (present to present): average, worst, and how many took over twice the target (hitches).
    static clock::time_point last_present = now;
    if (was_paused) {  // the pause is not a slow frame
        last_present = now;
        next = now;
    }
    static double ft_sum = 0, ft_max = 0;
    static uint64_t ft_n = 0, ft_hitches = 0;
    static double step_sum = 0;
    // The game's presenting thread: time between two presents (its own work) and how much of it was CPU, then the time
    // spent in here sleeping for the frame cap and waiting for the GPU.
    static double between_sum = 0, cpu_sum = 0, sleep_sum = 0, gpuwait_sum = 0;
    static clock::time_point left_last = now;
    static double cpu_left = 0;
    const auto thread_cpu_ms = [] {
        timespec ts;
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
        return double(ts.tv_sec) * 1e3 + double(ts.tv_nsec) / 1e6;
    };
    if (!was_paused) {
        between_sum += std::chrono::duration<double, std::milli>(now - left_last).count();
        if (cpu_left > 0) cpu_sum += thread_cpu_ms() - cpu_left;
    }
    {
        const double ms = std::chrono::duration<double, std::milli>(now - last_present).count();
        last_present = now;
        ft_sum += ms;
        ft_max = std::max(ft_max, ms);
        ft_n++;
        if (ms > 2.0 * 1000.0 / fps) ft_hitches++;
        step_sum += dsr::GameFrameTime();  // set by the game's frame limiter (60 fps mode)
    }
    static bool caller_logged = false;
    if (!caller_logged) {
        caller_logged = true;
        const loader::Module& m = dsr::GetRuntime().main;
        const uintptr_t ra = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
        LOGI(kTag, "presenting at up to %d fps (settings); nvnQueuePresentTexture called from main+0x%lx", fps,
             (unsigned long)(m.Contains(ra) ? ra - m.base() : 0));
    }
    if (next > now) {
        const auto t0 = clock::now();
        std::this_thread::sleep_until(next);
        sleep_sum += std::chrono::duration<double, std::milli>(clock::now() - t0).count();
    } else if (now - next > frame * 4)
        next = now;  // fell behind (loading): resync instead of bursting
    next += frame;

    TexRef tex = WindowTexture(window, index);
    // Present on the render thread, after the frame's commands. At most two frames queued ahead.
    static uint64_t presents[2] = {0, 0};
    const auto gw0 = clock::now();
    GpuWait(presents[0]);
    gpuwait_sum += std::chrono::duration<double, std::milli>(clock::now() - gw0).count();
    presents[0] = presents[1];
    presents[1] = GpuEnqueue([tex] { GetBackend().Present(tex); });
    NoteWindowTexturePresent(window, index, presents[1]);

    static uint64_t frames_at_report = 0;
    uint64_t n = ++frames;
    if (n == 1)
        LOGI(kTag, "first frame presented (window %p, texture %d: %dx%d format %d)", window, index, tex.tex.width,
             tex.tex.height, tex.tex.format);
    if (clock::now() - report > std::chrono::seconds(10)) {
        report = clock::now();
        double wait_present = 0, wait_sync = 0;
        TakeGameWaits(&wait_present, &wait_sync);
        if (ft_n)
            LOGI(kTag, "frame times: %.1f ms average, %.1f ms worst, %llu over %.0f ms (hitches); game step %.1f ms average; "
                 "game waited for the render thread %.0f ms at presents, %.0f ms in sync waits (last 10 s); cap %d fps%s",
                 ft_sum / ft_n, ft_max, (unsigned long long)ft_hitches, 2.0 * 1000.0 / fps, 1000.0 * step_sum / ft_n,
                 wait_present, wait_sync, dsr::FrameRateNow(), dsr::GetRuntime().config.dynamic_fps ? " (auto)" : "");
        if (ft_n) {
            LOGI(kTag, "game render thread per frame: %.1f ms of its own work between presents (%.1f ms of it on the CPU), "
                 "%.1f ms sleeping for the frame cap, %.1f ms waiting for the GPU at present",
                 between_sum / ft_n, cpu_sum / ft_n, sleep_sum / ft_n, gpuwait_sum / ft_n);
        }
        static clock::time_point load_t = clock::now() - std::chrono::seconds(10);
        LogThreadLoad(std::chrono::duration<double>(clock::now() - load_t).count());
        load_t = clock::now();
        between_sum = cpu_sum = sleep_sum = gpuwait_sum = 0;
        ft_sum = ft_max = step_sum = 0;
        ft_n = ft_hitches = 0;
        GpuStats s = Stats();
        double f = double(std::max<uint64_t>(1, n - frames_at_report));
        LOGI(kTag,
             "%llu frames presented | per frame: %.1f recorded, %.1f submit calls, %.1f lists run, %.1f draws, "
             "%.1f clears, %.1f copies, %.1f target binds%s",
             (unsigned long long)n, (s.recordings - last.recordings) / f, (s.submit_calls - last.submit_calls) / f,
             (s.submits - last.submits) / f, (s.draws - last.draws) / f, (s.clears - last.clears) / f,
             (s.copies - last.copies) / f, (s.rt_binds - last.rt_binds) / f,
             s.unknown_handles != last.unknown_handles ? " (some handles unknown)" : "");
        last = s;
        frames_at_report = n;
    }
    left_last = clock::now();
    cpu_left = thread_cpu_ms();
}

// ---- command buffers -----------------------------------------------------------------------------------------
size_t CmdPlenty(const void*) { return 16u << 20; }

// ---- sync ------------------------------------------------------------------------------------------------------

// ---- window ------------------------------------------------------------------------------------------------------
void WindowBuilderSetDefaults(void* b) { g_window_builders.Reset(b); }
void WindowBuilderSetTextures(void* b, int n, const void* const* textures) {
    auto& w = g_window_builders.Get(b);
    w.num_textures = std::min(n, 4);
    for (int i = 0; i < w.num_textures; i++) w.textures[i] = textures[i];
}
void WindowBuilderSetPresentInterval(void* b, int v) {
    LOGI(kTag, "game asks for present interval %d (window builder); pacing follows settings instead", v);
    g_window_builders.Get(b).present_interval = v;
}
void WindowSetPresentInterval(void* /*window*/, int v) {
    static int last = -1;
    if (v != last) LOGI(kTag, "game asks for present interval %d; pacing follows settings instead", v);
    last = v;
}
bool WindowInitialize(void* w, const void* b) {
    WindowState s = g_window_builders.Copy(b);
    g_windows.Get(w) = s;
    TextureState t = g_textures.Copy(s.textures[0]);
    LOGI(kTag, "nvnWindowInitialize: %d textures, %dx%d format %d", s.num_textures, t.width, t.height, t.format);
    return true;
}
void WindowFinalize(void* w) { g_windows.Erase(w); }
int WindowAcquireTexture(void* w, void* sync, int* index) {
    auto& s = g_windows.Get(w);
    int n = std::max(1, s.num_textures);
    *index = s.next;
    s.next = (s.next + 1) % n;
    // The sync is signalled once the texture's previous present has run (it was left signalled: a game that
    // paces its per-frame buffers on it could overwrite data the render thread had not read yet).
    if (sync) SetSyncTarget(sync, WindowTexturePresentSeq(w, *index));
    return 0;  // success
}
int WindowGetNumTextures(const void* w) { return g_windows.Copy(w).num_textures; }

}  // namespace

bool SubmitTextureFrame(uint64_t addr, std::shared_ptr<const std::vector<uint8_t>> rgba, int w, int h) {
    const uint64_t storage = VideoTextureStorage(addr, w, h);
    if (!storage || !rgba || rgba->size() < size_t(w) * size_t(h) * 4) return false;
    GetBackend().SetTextureFrame(storage, std::move(rgba), w, h);
    return true;
}
bool GuestRangeValid(uint64_t addr, size_t size) {
    // Called many times per draw (~5% of the render thread with a 4-entry cache and a lock on misses): each
    // thread keeps a copy of the sorted pool list, refreshed only when pools change, and searches it lock-free.
    struct Range {
        uintptr_t lo, hi;
    };
    thread_local uint64_t cached_version = 0;
    thread_local std::vector<Range> ranges;
    thread_local size_t last = 0;
    if (addr + size < addr) return false;
    const uint64_t version = g_pool_ranges_version.load(std::memory_order_acquire);
    if (version != cached_version) {
        std::lock_guard<std::mutex> lk(g_pool_range_mutex);
        ranges.clear();
        for (const auto& [base, sz] : g_pool_ranges) ranges.push_back({base, base + sz});
        cached_version = g_pool_ranges_version.load(std::memory_order_relaxed);
        last = 0;
    }
    const uintptr_t lo = uintptr_t(addr), hi = uintptr_t(addr + size);
    if (last < ranges.size() && lo >= ranges[last].lo && hi <= ranges[last].hi) return true;
    // The last pool starting at or below addr.
    size_t l = 0, r = ranges.size();
    while (l < r) {
        const size_t m = (l + r) / 2;
        if (ranges[m].lo <= lo) l = m + 1;
        else r = m;
    }
    if (l == 0) return false;
    const Range& p = ranges[l - 1];
    if (hi > p.hi) return false;
    last = l - 1;
    return true;
}

TexRef MakeTexRef(const void* nvn_texture) {
    TexRef r;
    if (!nvn_texture) return r;
    r.tex = g_textures.Copy(nvn_texture);
    r.addr = PoolBase(r.tex.pool) + uint64_t(r.tex.offset);
    r.valid = r.tex.pool != nullptr;
    return r;
}
TexRef WindowTexture(const void* nvn_window, int index) {
    WindowState w = g_windows.Copy(nvn_window);
    if (index < 0 || index >= w.num_textures) return {};
    return MakeTexRef(w.textures[index]);
}

namespace {
struct Proc {
    const char* name;
    void* fn;
};
#define P(n, f) {n, reinterpret_cast<void*>(&f)}

const Proc kProcs[] = {
    // device
    P("nvnDeviceBuilderSetDefaults", Nop), P("nvnDeviceBuilderSetFlags", Nop),
    P("nvnDeviceInitialize", DeviceInitialize), P("nvnDeviceFinalize", Nop),
    P("nvnDeviceGetInteger", DeviceGetInteger), P("nvnDeviceGetTextureHandle", DeviceGetTextureHandle),
    P("nvnDeviceGetImageHandle", DeviceGetImageHandle),
    P("nvnDeviceGetTexelFetchHandle", DeviceGetSeparateHandle),
    P("nvnDeviceGetSeparateTextureHandle", DeviceGetSeparateHandle),
    P("nvnDeviceGetSeparateSamplerHandle", DeviceGetSeparateHandle),
    P("nvnDeviceGetCurrentTimestampInNanoseconds", DeviceGetTimestampNs),
    
    // memory pools
    P("nvnMemoryPoolBuilderSetDefaults", PoolBuilderSetDefaults), P("nvnMemoryPoolBuilderSetDevice", Nop),
    P("nvnMemoryPoolBuilderSetFlags", PoolBuilderSetFlags), P("nvnMemoryPoolBuilderSetStorage", PoolBuilderSetStorage),
    P("nvnMemoryPoolInitialize", PoolInitialize), P("nvnMemoryPoolFinalize", PoolFinalize),
    P("nvnMemoryPoolMap", PoolMap), P("nvnMemoryPoolGetBufferAddress", PoolGetBufferAddress),
    P("nvnMemoryPoolGetSize", PoolGetSize), P("nvnMemoryPoolGetFlags", PoolGetFlags),
    P("nvnMemoryPoolFlushMappedRange", Nop), P("nvnMemoryPoolInvalidateMappedRange", Nop),
    // buffers
    P("nvnBufferBuilderSetDefaults", BufferBuilderSetDefaults), P("nvnBufferBuilderSetDevice", Nop),
    P("nvnBufferBuilderSetStorage", BufferBuilderSetStorage), P("nvnBufferInitialize", BufferInitialize),
    P("nvnBufferFinalize", BufferFinalize), P("nvnBufferMap", BufferMap), P("nvnBufferGetAddress", BufferGetAddress),
    P("nvnBufferGetSize", BufferGetSize), P("nvnBufferFlushMappedRange", Nop), P("nvnBufferInvalidateMappedRange", Nop),
    // textures
    P("nvnTextureBuilderSetDefaults", TexBuilderSetDefaults), P("nvnTextureBuilderSetDevice", Nop),
    P("nvnTextureBuilderSetFlags", TexBuilderSetFlags), P("nvnTextureBuilderSetTarget", TexBuilderSetTarget),
    P("nvnTextureBuilderSetWidth", TexBuilderSetWidth), P("nvnTextureBuilderSetHeight", TexBuilderSetHeight),
    P("nvnTextureBuilderSetDepth", TexBuilderSetDepth), P("nvnTextureBuilderSetSize1D", TexBuilderSetSize1D),
    P("nvnTextureBuilderSetSize2D", TexBuilderSetSize2D), P("nvnTextureBuilderSetSize3D", TexBuilderSetSize3D),
    P("nvnTextureBuilderSetLevels", TexBuilderSetLevels), P("nvnTextureBuilderSetFormat", TexBuilderSetFormat),
    P("nvnTextureBuilderSetSamples", TexBuilderSetSamples), P("nvnTextureBuilderSetStride", TexBuilderSetStride),
    P("nvnTextureBuilderSetStorage", TexBuilderSetStorage), P("nvnTextureBuilderSetPackagedTextureData", TexBuilderSetPackagedData),
    P("nvnTextureBuilderSetPackagedTextureLayout", TexBuilderSetPackagedLayout), P("nvnTextureBuilderSetSwizzle", TexBuilderSetSwizzle),
    P("nvnTextureBuilderSetDepthStencilMode", Nop), P("nvnTextureBuilderSetGLTextureName", Nop),
    P("nvnTextureBuilderGetStorageSize", TexBuilderGetStorageSize),
    P("nvnTextureBuilderGetStorageAlignment", TexBuilderGetStorageAlignment),
    P("nvnTextureBuilderGetWidth", TexBuilderGetWidth), P("nvnTextureBuilderGetHeight", TexBuilderGetHeight),
    P("nvnTextureBuilderGetDepth", TexBuilderGetDepth), P("nvnTextureBuilderGetLevels", TexBuilderGetLevels),
    P("nvnTextureBuilderGetFormat", TexBuilderGetFormat), P("nvnTextureBuilderGetTarget", TexBuilderGetTarget),
    P("nvnTextureInitialize", TexInitialize), P("nvnTextureFinalize", TexFinalize),
    P("nvnTextureGetWidth", TexGetWidth), P("nvnTextureGetHeight", TexGetHeight),
    P("nvnTextureGetDepth", TexGetDepth), P("nvnTextureGetLevels", TexGetLevels),
    P("nvnTextureGetFormat", TexGetFormat), P("nvnTextureGetSamples", TexGetSamples),
    P("nvnTextureGetTarget", TexGetTarget), P("nvnTextureGetStorageSize", TexGetStorageSize),
    P("nvnTextureGetTextureAddress", TexGetTextureAddress), P("nvnTextureFlushTexels", Nop),
    P("nvnTextureInvalidateTexels", Nop), P("nvnTextureWriteTexels", TexWriteTexels),
    P("nvnTextureWriteTexelsStrided", TexWriteTexelsStrided), P("nvnTextureReadTexels", TexReadTexels),
    P("nvnTextureReadTexelsStrided", TexReadTexels),
    // pools of descriptors, samplers, programs
    P("nvnTexturePoolInitialize", True), P("nvnTexturePoolFinalize", Nop),  P("nvnSamplerPoolInitialize", True), P("nvnSamplerPoolFinalize", Nop),
   
    P("nvnProgramInitialize", True), P("nvnProgramFinalize", Nop), P("nvnProgramSetShaders", ProgramSetShaders),
    P("nvnEventInitialize", True), P("nvnEventFinalize", Nop),
    // queues
    P("nvnQueueBuilderSetDefaults", QueueBuilderSetDefaults), P("nvnQueueBuilderSetDevice", Nop),
    P("nvnQueueBuilderSetFlags", Nop), P("nvnQueueBuilderSetCommandMemorySize", QueueBuilderSetCommandMemorySize),
    P("nvnQueueBuilderSetComputeMemorySize", QueueBuilderSetComputeMemorySize),
    P("nvnQueueBuilderSetCommandFlushThreshold", Nop), P("nvnQueueBuilderSetQueueMemory", Nop),
    P("nvnQueueBuilderGetQueueMemorySize", QueueBuilderGetQueueMemorySize),
    P("nvnQueueInitialize", QueueInitialize), P("nvnQueueFinalize", Nop), P("nvnQueueFlush", Nop), P("nvnQueueWaitSync", Nop),
    P("nvnQueuePresentTexture", QueuePresentTexture),
    // command buffers (recording is a no-op)
    P("nvnCommandBufferInitialize", True), P("nvnCommandBufferFinalize", Nop),
    P("nvnCommandBufferGetCommandMemoryFree", CmdPlenty), P("nvnCommandBufferGetControlMemoryFree", CmdPlenty),
    P("nvnCommandBufferGetCommandMemorySize", CmdPlenty), P("nvnCommandBufferGetControlMemorySize", CmdPlenty),
    // sync
    
    // window
    P("nvnWindowBuilderSetDefaults", WindowBuilderSetDefaults), P("nvnWindowBuilderSetDevice", Nop),
    P("nvnWindowBuilderSetNativeWindow", Nop), P("nvnWindowBuilderSetTextures", WindowBuilderSetTextures),
    P("nvnWindowBuilderSetPresentInterval", WindowBuilderSetPresentInterval),
    P("nvnWindowInitialize", WindowInitialize), P("nvnWindowFinalize", WindowFinalize),
    P("nvnWindowAcquireTexture", WindowAcquireTexture), P("nvnWindowGetNumActiveTextures", WindowGetNumTextures),
    P("nvnWindowGetNumTextures", WindowGetNumTextures), P("nvnWindowSetPresentInterval", WindowSetPresentInterval),
    P("nvnWindowSetCrop", Nop), P("nvnWindowSetNumActiveTextures", Nop),
};
#undef P

// Every other nvnCommandBuffer*/state-object setter is a no-op with no return value worth tracking.
const char* const kNopPrefixes[] = {
    "nvnCommandBuffer", "nvnBlendState", "nvnChannelMaskState", "nvnColorState", "nvnDepthStencilState",
    "nvnMultisampleState", "nvnPolygonState", "nvnVertexAttribState", "nvnVertexStreamState",
    "nvnSamplerBuilder", "nvnTextureView",
};

struct Registrar {
    Registrar() {
        for (const Proc& p : kProcs) RegisterNvnProc(p.name, p.fn);
        for (const char* prefix : kNopPrefixes) RegisterNvnPrefix(prefix, reinterpret_cast<void*>(&Nop));
    }
} g_registrar;

}  // namespace
}  // namespace dsr::hle::nvn
