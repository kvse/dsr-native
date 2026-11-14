// Self-contained tests for the loader and HLE runtime (no game files needed).
// Portable tests run everywhere; ABI tests (x18 handling, thunks, setjmp) run only on arm64.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>
#include <tuple>
#include <vector>

#include "arch/arch.h"
#include "hle/nvn.h"
#include "hle/nvn/backend.h"
#include "hle/nvn/state.h"
#include "hle/nvn/layout.h"
#include "hle/nvn/shader/translator.h"
#include "hle/registry.h"
#include "loader/dynamic_thunks.h"
#include "loader/nso.h"
#include "loader/sha256.h"

static int g_failures = 0;
#include "hle/nvn/etc_encode.h"
#include "hle/nvn/parallel.h"
#include "runtime/cheat_vm.h"

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                                        \
        }                                                                        \
    } while (0)

template <typename F>
F Hle(const char* mangled) {
    void* p = dsr::hle::FindFunction(mangled);
    if (!p) {
        fprintf(stderr, "  missing HLE function %s\n", mangled);
        exit(1);
    }
    return reinterpret_cast<F>(p);
}

// ---- loader ------------------------------------------------------------------------------------
static void TestLz4() {
    const uint8_t lit[] = {0x50, 'h', 'e', 'l', 'l', 'o'};
    uint8_t out[16] = {};
    CHECK(dsr::loader::Lz4DecompressBlock(lit, sizeof(lit), out, 5));
    CHECK(memcmp(out, "hello", 5) == 0);
    // "abc" + match(offset 3, length 6) -> "abcabcabc" (overlapping copy)
    const uint8_t match[] = {0x32, 'a', 'b', 'c', 0x03, 0x00};
    CHECK(dsr::loader::Lz4DecompressBlock(match, sizeof(match), out, 9));
    CHECK(memcmp(out, "abcabcabc", 9) == 0);
    // wrong output size must be rejected
    CHECK(!dsr::loader::Lz4DecompressBlock(match, sizeof(match), out, 8));
}

static void TestSha256() {
    uint8_t d[32];
    dsr::loader::Sha256(reinterpret_cast<const uint8_t*>("abc"), 3, d);
    const uint8_t expect[4] = {0xba, 0x78, 0x16, 0xbf};
    CHECK(memcmp(d, expect, 4) == 0);
    std::vector<uint8_t> big(1000, 'a');
    dsr::loader::Sha256(big.data(), big.size(), d);  // exercises the two-block tail path
    const uint8_t expect2[4] = {0x41, 0xed, 0xec, 0xe4};
    CHECK(memcmp(d, expect2, 4) == 0);
}

// ---- nn::mem::StandardAllocator ----------------------------------------------------------------
static void TestStandardAllocator() {
    using Ctor = void (*)(void*);
    using Init = void (*)(void*, void*, size_t);
    using Alloc = void* (*)(void*, size_t, size_t);
    using Free = void (*)(void*, void*);
    using FreeSize = size_t (*)(void*);
    auto ctor = Hle<Ctor>("_ZN2nn3mem17StandardAllocatorC1Ev");
    auto init = Hle<Init>("_ZN2nn3mem17StandardAllocator10InitializeEPvm");
    auto alloc = Hle<Alloc>("_ZN2nn3mem17StandardAllocator8AllocateEmm");
    auto free_ = Hle<Free>("_ZN2nn3mem17StandardAllocator4FreeEPv");
    auto free_size = Hle<FreeSize>("_ZNK2nn3mem17StandardAllocator16GetTotalFreeSizeEv");

    alignas(16) uint8_t obj[64];
    const size_t region_size = 8 << 20;
    auto* region = static_cast<uint8_t*>(malloc(region_size));
    ctor(obj);
    init(obj, region, region_size);
    const size_t initial_free = free_size(obj);

    std::mt19937 rng(1234);
    struct A { uint8_t* p; size_t n; uint8_t tag; };
    std::vector<A> live;
    for (int i = 0; i < 20000; i++) {
        if (live.empty() || rng() % 3) {
            size_t n = 1 + rng() % 4096;
            size_t align = size_t(1) << (4 + rng() % 9);  // 16..4096
            auto* p = static_cast<uint8_t*>(alloc(obj, n, align));
            if (!p) continue;
            CHECK((reinterpret_cast<uintptr_t>(p) & (align - 1)) == 0);
            CHECK(p >= region && p + n <= region + region_size);
            uint8_t tag = uint8_t(rng());
            memset(p, tag, n);
            live.push_back({p, n, tag});
        } else {
            size_t k = rng() % live.size();
            A a = live[k];
            for (size_t j = 0; j < a.n; j++)
                if (a.p[j] != a.tag) {
                    CHECK(!"allocation was overwritten");
                    break;
                }
            free_(obj, a.p);
            live[k] = live.back();
            live.pop_back();
        }
    }
    for (const A& a : live) free_(obj, a.p);
    CHECK(free_size(obj) == initial_free);  // everything coalesced back
    free(region);
}

// ---- nn::os sync primitives (operate on SDK struct layouts) ---------------------------------------
static void TestOsSync() {
    using MutexInit = void (*)(void*, bool, int);
    using MutexOp = void (*)(void*);
    auto minit = Hle<MutexInit>("_ZN2nn2os15InitializeMutexEPNS0_9MutexTypeEbi");
    auto lock = Hle<MutexOp>("_ZN2nn2os9LockMutexEPNS0_9MutexTypeE");
    auto unlock = Hle<MutexOp>("_ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE");

    alignas(8) uint8_t mutex[0x20];
    minit(mutex, true, 0);
    lock(mutex);
    lock(mutex);  // recursive
    unlock(mutex);
    unlock(mutex);

    long counter = 0;
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; t++)
        ts.emplace_back([&] {
            for (int i = 0; i < 20000; i++) {
                lock(mutex);
                counter++;
                unlock(mutex);
            }
        });
    for (auto& t : ts) t.join();
    CHECK(counter == 80000);

    // Event: auto-clear wakes exactly one waiter per signal.
    using EventInit = void (*)(void*, bool, int);
    using EventOp = void (*)(void*);
    using EventTimed = bool (*)(void*, int64_t);
    auto einit = Hle<EventInit>("_ZN2nn2os15InitializeEventEPNS0_9EventTypeEbNS0_14EventClearModeE");
    auto signal = Hle<EventOp>("_ZN2nn2os11SignalEventEPNS0_9EventTypeE");
    auto timed = Hle<EventTimed>("_ZN2nn2os14TimedWaitEventEPNS0_9EventTypeENS_8TimeSpanE");
    alignas(8) uint8_t ev[0x28];
    einit(ev, false, 1);
    CHECK(!timed(ev, 1'000'000));
    std::thread waker([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        signal(ev);
    });
    CHECK(timed(ev, 2'000'000'000));
    CHECK(!timed(ev, 1'000'000));  // auto-cleared
    waker.join();

    // Semaphore producer/consumer.
    using SemInit = void (*)(void*, int, int);
    using SemOp = void (*)(void*);
    auto sinit = Hle<SemInit>("_ZN2nn2os19InitializeSemaphoreEPNS0_13SemaphoreTypeEii");
    auto acquire = Hle<SemOp>("_ZN2nn2os16AcquireSemaphoreEPNS0_13SemaphoreTypeE");
    auto release = Hle<SemOp>("_ZN2nn2os16ReleaseSemaphoreEPNS0_13SemaphoreTypeE");
    alignas(8) uint8_t sem[0x28];
    sinit(sem, 0, 1000);
    std::atomic<int> got{0};
    std::thread consumer([&] {
        for (int i = 0; i < 500; i++) {
            acquire(sem);
            got++;
        }
    });
    for (int i = 0; i < 500; i++) release(sem);
    consumer.join();
    CHECK(got == 500);
}

// ---- null NVN driver ---------------------------------------------------------------------------
template <typename F>
F Nvn(const char* name) {
    void* p = dsr::hle::nvn::FindNvnImpl(name);
    if (!p) {
        fprintf(stderr, "  missing NVN proc %s\n", name);
        exit(1);
    }
    return reinterpret_cast<F>(p);
}

static void TestNullNvn() {
    // Version check the game performs before anything else.
    auto get_int = Nvn<void (*)(const void*, int, int*)>("nvnDeviceGetInteger");
    int major = 0, minor = 0, ubo = 0, unknown = 1234;
    get_int(nullptr, 0, &major);
    get_int(nullptr, 1, &minor);
    get_int(nullptr, 4, &ubo);
    get_int(nullptr, 9999, &unknown);
    CHECK(major == 53 && minor > 104 && ubo == 256);
    CHECK(unknown == 1234);  // unknown queries keep the caller's default

    // Memory pool -> buffer: mapped pointer and GPU address must follow pool storage + offset.
    alignas(16) static uint8_t storage[0x4000];
    uint8_t pool_builder[64], pool[256], buf_builder[64], buf[64];
    Nvn<void (*)(void*)>("nvnMemoryPoolBuilderSetDefaults")(pool_builder);
    Nvn<void (*)(void*, void*, size_t)>("nvnMemoryPoolBuilderSetStorage")(pool_builder, storage, sizeof(storage));
    CHECK(Nvn<bool (*)(void*, const void*)>("nvnMemoryPoolInitialize")(pool, pool_builder));
    CHECK(Nvn<void* (*)(const void*)>("nvnMemoryPoolMap")(pool) == storage);
    Nvn<void (*)(void*)>("nvnBufferBuilderSetDefaults")(buf_builder);
    Nvn<void (*)(void*, const void*, int64_t, size_t)>("nvnBufferBuilderSetStorage")(buf_builder, pool, 0x100, 0x200);
    CHECK(Nvn<bool (*)(void*, const void*)>("nvnBufferInitialize")(buf, buf_builder));
    CHECK(Nvn<void* (*)(const void*)>("nvnBufferMap")(buf) == storage + 0x100);
    CHECK(Nvn<uint64_t (*)(const void*)>("nvnBufferGetAddress")(buf) == reinterpret_cast<uint64_t>(storage) + 0x100);

    // Texture builder -> texture: dimensions read back, storage estimate covers the data.
    uint8_t tb[128], tex[192];
    Nvn<void (*)(void*)>("nvnTextureBuilderSetDefaults")(tb);
    Nvn<void (*)(void*, int, int)>("nvnTextureBuilderSetSize2D")(tb, 1280, 720);
    Nvn<void (*)(void*, int)>("nvnTextureBuilderSetFormat")(tb, 37);  // RGBA8
    Nvn<void (*)(void*, int)>("nvnTextureBuilderSetLevels")(tb, 1);
    size_t sz = Nvn<size_t (*)(const void*)>("nvnTextureBuilderGetStorageSize")(tb);
    CHECK(sz >= size_t(1280) * 720 * 4);
    CHECK(Nvn<bool (*)(void*, const void*)>("nvnTextureInitialize")(tex, tb));
    CHECK(Nvn<int (*)(const void*)>("nvnTextureGetWidth")(tex) == 1280);
    CHECK(Nvn<int (*)(const void*)>("nvnTextureGetHeight")(tex) == 720);

    // Prefix fallback covers command-buffer setters; creation calls report success.
    CHECK(dsr::hle::nvn::FindNvnImpl("nvnCommandBufferSetViewport") != nullptr);
    CHECK(Nvn<bool (*)(void*, const void*)>("nvnSamplerPoolInitialize")(pool, nullptr));
}

// Command recording: what the game records must reach the backend in order, with textures resolved.
namespace {
struct RecordingBackend final : dsr::hle::nvn::Backend {
    std::vector<dsr::hle::nvn::CmdList> executed;
    int presents = 0;
    dsr::hle::nvn::TexRef last_present;
    void Execute(const dsr::hle::nvn::CmdList& c) override { executed.push_back(c); }
    void Present(const dsr::hle::nvn::TexRef& t) override {
        presents++;
        last_present = t;
    }
    const char* Name() const override { return "test"; }
};
}  // namespace

static void TestNvnCommands() {
    using namespace dsr::hle::nvn;
    RecordingBackend be;
    SetBackendForTesting(&be);

    alignas(16) static uint8_t storage[0x400000];
    uint8_t pool_builder[64], pool[256], tb[128], color[192], depth[192], cb[256], wb[64], window[128];
    Nvn<void (*)(void*)>("nvnMemoryPoolBuilderSetDefaults")(pool_builder);
    Nvn<void (*)(void*, void*, size_t)>("nvnMemoryPoolBuilderSetStorage")(pool_builder, storage, sizeof(storage));
    CHECK(Nvn<bool (*)(void*, const void*)>("nvnMemoryPoolInitialize")(pool, pool_builder));
    auto make_tex = [&](void* t, int w, int h, int fmt, int64_t off) {
        Nvn<void (*)(void*)>("nvnTextureBuilderSetDefaults")(tb);
        Nvn<void (*)(void*, int, int)>("nvnTextureBuilderSetSize2D")(tb, w, h);
        Nvn<void (*)(void*, int)>("nvnTextureBuilderSetFormat")(tb, fmt);
        Nvn<void (*)(void*, const void*, int64_t)>("nvnTextureBuilderSetStorage")(tb, pool, off);
        CHECK(Nvn<bool (*)(void*, const void*)>("nvnTextureInitialize")(t, tb));
    };
    make_tex(color, 1280, 720, 37, 0);
    make_tex(depth, 1280, 720, 53, 0x200000);

    const void* colors[1] = {color};
    const float red[4] = {1, 0, 0, 1};
    Nvn<void (*)(void*)>("nvnCommandBufferBeginRecording")(cb);
    CHECK(Nvn<bool (*)(const void*)>("nvnCommandBufferIsRecording")(cb));
    Nvn<void (*)(void*, int, const void* const*, const void* const*, const void*, const void*)>(
        "nvnCommandBufferSetRenderTargets")(cb, 1, colors, nullptr, depth, nullptr);
    Nvn<void (*)(void*, int, const float*, int)>("nvnCommandBufferClearColor")(cb, 0, red, 0xF);
    Nvn<void (*)(void*, float, bool, int, int)>("nvnCommandBufferClearDepthStencil")(cb, 1.0f, true, 0, 0xFF);
    Nvn<void (*)(void*)>("nvnCommandBufferDrawArrays")(cb);
    // The game only submits buffers that report command memory use.
    CHECK(Nvn<size_t (*)(const void*)>("nvnCommandBufferGetCommandMemoryUsed")(cb) > 0);
    uint64_t h = Nvn<uint64_t (*)(void*)>("nvnCommandBufferEndRecording")(cb);
    CHECK(h != 0);
    CHECK(!Nvn<bool (*)(const void*)>("nvnCommandBufferIsRecording")(cb));

    // A second buffer that inlines the first one.
    uint8_t cb2[256];
    Nvn<void (*)(void*)>("nvnCommandBufferBeginRecording")(cb2);
    Nvn<void (*)(void*, int, const uint64_t*)>("nvnCommandBufferCallCommands")(cb2, 1, &h);
    uint64_t h2 = Nvn<uint64_t (*)(void*)>("nvnCommandBufferEndRecording")(cb2);

    uint64_t handles[2] = {h, h2};
    Nvn<void (*)(void*, int, const uint64_t*)>("nvnQueueSubmitCommands")(nullptr, 2, handles);
    CHECK(be.executed.size() == 2);
    if (be.executed.size() == 2) {
        const CmdList& l = be.executed[0];
        CHECK(l.size() == 4);
        CHECK(l.size() == 4 && l[0].type == CmdType::SetRenderTargets && l[0].num_rt == 1);
        if (l.size() == 4 && l[0].rts && l[0].rts->size() == 2) {
            CHECK((*l[0].rts)[0].valid && (*l[0].rts)[0].tex.width == 1280 && (*l[0].rts)[0].tex.format == 37);
            CHECK((*l[0].rts)[0].addr == reinterpret_cast<uint64_t>(storage));
            CHECK((*l[0].rts)[1].addr == reinterpret_cast<uint64_t>(storage) + 0x200000 && (*l[0].rts)[1].tex.format == 53);
        } else {
            CHECK(!"render targets not captured");
        }
        float got[4];
        if (l.size() == 4) memcpy(got, l[1].color, sizeof(got));
        CHECK(l.size() == 4 && l[1].type == CmdType::ClearColor && got[0] == 1.0f && got[1] == 0.0f && l[1].mask == 0xF);
        CHECK(l.size() == 4 && l[2].type == CmdType::ClearDepthStencil && l[2].depth == 1.0f && l[2].depth_mask);
        CHECK(l.size() == 4 && l[3].type == CmdType::Draw);
        // The second list calls the first (referenced, not copied): the same four commands.
        size_t called = 0;
        dsr::hle::nvn::ForEachCmd(be.executed[1], [&](const dsr::hle::nvn::Cmd&) { called++; });
        CHECK(be.executed[1].size() == 1 && be.executed[1][0].type == CmdType::CallList && called == 4);
    }

    // Presenting window texture 0 resolves to the color texture.
    const void* wtex[2] = {color, depth};
    Nvn<void (*)(void*)>("nvnWindowBuilderSetDefaults")(wb);
    Nvn<void (*)(void*, int, const void* const*)>("nvnWindowBuilderSetTextures")(wb, 2, wtex);
    CHECK(Nvn<bool (*)(void*, const void*)>("nvnWindowInitialize")(window, wb));
    Nvn<void (*)(void*, void*, int)>("nvnQueuePresentTexture")(nullptr, window, 0);
    CHECK(be.presents == 1 && be.last_present.valid && be.last_present.addr == reinterpret_cast<uint64_t>(storage));

    // Sampler ids are per sampler pool: the same id in two pools names two samplers, and a texture handle
    // resolves its sampler in the pool the command buffer has bound (in later recordings too).
    {
        alignas(16) uint8_t sb[96], spool_a[64], spool_b[64], tpool[64], cb3[256];
        auto reg = [&](void* sp, int wrap) {
            Nvn<void (*)(void*)>("nvnSamplerBuilderSetDefaults")(sb);
            Nvn<void (*)(void*, int, int, int)>("nvnSamplerBuilderSetWrapMode")(sb, wrap, wrap, wrap);
            Nvn<void (*)(void*, int, const void*)>("nvnSamplerPoolRegisterSamplerBuilder")(sp, 5, sb);
        };
        reg(spool_a, 5);
        reg(spool_b, 7);
        Nvn<void (*)(void*, int, const void*, const void*)>("nvnTexturePoolRegisterTexture")(tpool, 300, color, nullptr);
        const uint64_t handle = (uint64_t(5) << 20) | 300;
        auto record = [&](void* sp) {
            Nvn<void (*)(void*)>("nvnCommandBufferBeginRecording")(cb3);
            if (sp) Nvn<void (*)(void*, const void*)>("nvnCommandBufferSetSamplerPool")(cb3, sp);
            Nvn<void (*)(void*, int, int, uint64_t)>("nvnCommandBufferBindTexture")(cb3, 1, 0, handle);
            return Nvn<uint64_t (*)(void*)>("nvnCommandBufferEndRecording")(cb3);
        };
        uint64_t hs[3] = {record(spool_a), record(spool_b), record(nullptr)};
        be.executed.clear();
        Nvn<void (*)(void*, int, const uint64_t*)>("nvnQueueSubmitCommands")(nullptr, 3, hs);
        auto wrap_of = [&](size_t i) {
            int w = -1, pools = 0;
            if (i < be.executed.size())
                ForEachCmd(be.executed[i], [&](const Cmd& c) {
                    if (c.type == CmdType::SetSamplerPool) pools++;
                    if (c.type == CmdType::BindTextures)
                        w = One<BoundTexture>(c).sampler.wrap[0];
                });
            return std::make_pair(w, pools);
        };
        CHECK(wrap_of(0).first == 5);
        CHECK(wrap_of(1).first == 7);
        CHECK(wrap_of(2).first == 7 && wrap_of(2).second == 1);  // pool b still bound, re-bound at the start
        CHECK(SamplerById(5, spool_a).wrap[0] == 5 && SamplerById(5, spool_b).wrap[0] == 7);
    }

    SetBackendForTesting(nullptr);
}

// Shader translator: a hand-assembled pass-through vertex shader and a texture * color pixel shader
// (the shape of the game's menu shaders) must translate and report their interface.
static std::vector<uint8_t> MakeShader(uint32_t sph_type, uint32_t omap, const std::vector<uint64_t>& insts) {
    std::vector<uint8_t> code(0x80, 0);
    uint32_t w0 = (sph_type << 10) | (3u << 5) | 1u;
    memcpy(&code[0x30], &w0, 4);
    memcpy(&code[0x30 + 18 * 4], &omap, 4);
    size_t n = 0;
    for (uint64_t inst : insts) {
        if (n % 4 == 0) {  // scheduling word
            uint64_t sched = 0x001f8000fc0007e0ull;
            code.insert(code.end(), reinterpret_cast<uint8_t*>(&sched), reinterpret_cast<uint8_t*>(&sched) + 8);
            n++;
        }
        code.insert(code.end(), reinterpret_cast<const uint8_t*>(&inst), reinterpret_cast<const uint8_t*>(&inst) + 8);
        n++;
    }
    return code;
}

// Tiny Maxwell encoder for tests: opcode bits + the operand fields the translator reads.
namespace enc {
constexpr uint64_t kPT = 7ull << 16, kRZ = 255;
uint64_t Ald(uint32_t d, uint32_t addr) { return (0xefd8ull << 48) | (uint64_t(addr) << 20) | (kRZ << 8) | kPT | d; }
uint64_t Ast(uint32_t s, uint32_t addr) { return (0xeff0ull << 48) | (uint64_t(addr) << 20) | (kRZ << 8) | kPT | s; }
uint64_t MovCbuf(uint32_t d, uint32_t idx, uint32_t off) {
    return (0x4c98ull << 48) | (uint64_t(idx) << 34) | (uint64_t(off / 4) << 20) | kPT | d;
}
uint64_t Ipa(uint32_t d, uint32_t attr, uint32_t mode, uint32_t mul) {
    return (0xe0ull << 56) | (uint64_t(mode) << 54) | (uint64_t(attr) << 28) | (uint64_t(mul) << 20) | (kRZ << 8) | kPT | d;
}
uint64_t Mufu(uint32_t d, uint32_t a, uint32_t op) { return (0x5080ull << 48) | (uint64_t(op) << 20) | (a << 8) | kPT | d; }
uint64_t Fmul(uint32_t d, uint32_t a, uint32_t b) { return (0x5c68ull << 48) | (uint64_t(b) << 20) | (a << 8) | kPT | d; }
uint64_t TexB(uint32_t d, uint32_t coord, uint32_t meta, uint32_t type, uint32_t mask) {
    return (0xdeb8ull << 48) | (uint64_t(mask) << 31) | (uint64_t(type) << 28) | (uint64_t(meta) << 20) |
           (coord << 8) | kPT | d;
}
constexpr uint64_t kExit = (0xe300ull << 48) | kPT | 0xf;
constexpr uint64_t kBra = (0xe240ull << 48) | kPT | 0xf;
}  // namespace enc

static void TestShaderTranslator() {
    using namespace dsr::hle::nvn::shader;
    using namespace enc;
    // Vertex: position from attribute 0, varying 0 from attribute 1.
    auto vs = MakeShader(1, 0, {Ald(0, 0x80), Ald(1, 0x84), Ald(2, 0x88), Ald(3, 0x8c), Ast(0, 0x70), Ast(1, 0x74),
                                Ast(2, 0x78), Ast(3, 0x7c), Ald(4, 0x90), Ast(4, 0x80), kExit});
    TranslateResult v = Translate(vs.data(), vs.size(), TranslateOptions{});
    if (!v.ok) fprintf(stderr, "  vertex: %s\n", v.error.c_str());
    CHECK(v.ok);
    CHECK(v.ok && v.info.stage == Stage::Vertex && v.info.input_mask == 0x3 && v.info.output_mask == 0x1);
    CHECK(v.ok && v.spirv.size() > 5 && v.spirv[0] == 0x07230203);

    // Fragment, shaped like the game's menu shader: handle from the texture table, perspective divide,
    // 2D rgba sample, multiply by the interpolated color, write render target 0.
    auto fs = MakeShader(5, 0xF, {MovCbuf(11, 13, 0x20), Ipa(10, 0x7c, 0, 255), Ipa(4, 0x80, 0, 255),
                                  Ipa(5, 0x84, 0, 255), Mufu(10, 10, 4), Fmul(4, 10, 4), Fmul(5, 10, 5),
                                  TexB(0, 4, 11, 2, 0xF), Ipa(6, 0x90, 1, 10), Fmul(0, 0, 6), kExit});
    TranslateResult f = Translate(fs.data(), fs.size(), TranslateOptions{});
    if (!f.ok) fprintf(stderr, "  fragment: %s\n", f.error.c_str());
    CHECK(f.ok);
    CHECK(f.ok && f.info.stage == Stage::Fragment && f.info.texture_mask == 0x1 && f.info.output_mask == 0x1);
    CHECK(f.ok && f.info.texture_dim[0] == TexDim::D2 && f.info.cbuf_mask == 0);  // the handle needs no buffer
    CHECK(f.ok && f.info.input_mask == 0x3);

    // A texture handle that does not come from the binding table is reported, not guessed.
    auto bad = MakeShader(5, 0xF, {Ipa(4, 0x80, 0, 255), TexB(0, 4, 4, 2, 0xF), kExit});
    CHECK(!Translate(bad.data(), bad.size(), TranslateOptions{}).ok);
    // Loops (backward branches) are reported, not mistranslated.
    auto br = MakeShader(5, 0xF, {Ipa(4, 0x80, 0, 255), kBra | (0xFFFFE8ull << 20), kExit});
    CHECK(!Translate(br.data(), br.size(), TranslateOptions{}).ok);
}

// Block-linear layout: exact sizes and a swizzle/deswizzle round trip over awkward sizes and mips.
static void TestBlockLinear() {
    using namespace dsr::hle::nvn;
    TextureState t;
    t.width = 1280;
    t.height = 720;
    t.format = 37;  // RGBA8: 5120-byte rows = 80 GOBs; 720 rows -> 16-GOB blocks (128 rows) x 6
    TextureLayout l = ComputeLayout(t);
    CHECK(l.levels.size() == 1 && l.levels[0].bh_log2 == 4);
    CHECK(l.total == size_t(80) * 6 * 512 * 16);

    for (auto [w, h, d, fmt, levels, target] : std::vector<std::tuple<int, int, int, int, int, int>>{
             {37, 13, 1, 37, 3, kTarget2D}, {256, 256, 1, 68, 9, kTarget2D}, {64, 64, 6, 37, 1, kTargetCubemap},
             {32, 16, 8, 41, 2, kTarget3D}, {100, 3, 4, 1, 1, kTarget2DArray}}) {
        TextureState s;
        s.width = w;
        s.height = h;
        s.depth = d;
        s.format = fmt;
        s.levels = levels;
        s.target = target;
        TextureLayout tl = ComputeLayout(s);
        std::vector<uint8_t> swz(tl.total, 0);
        for (size_t layer = 0; layer < size_t(tl.layers); layer++)
            for (const LevelLayout& lv : tl.levels) {
                const size_t n = size_t(lv.row_bytes) * lv.rows * lv.depth;
                CHECK(lv.offset + lv.size <= tl.layer_stride);
                std::vector<uint8_t> lin(n), back(n);
                for (size_t k = 0; k < n; k++) lin[k] = uint8_t(k * 131 + layer * 7 + lv.offset);
                uint8_t* base = swz.data() + layer * tl.layer_stride + lv.offset;
                SwizzleLevel(base, lin.data(), lv);
                DeswizzleLevel(back.data(), base, lv);
                CHECK(lin == back);
                // Region writes (nvnTextureWriteTexels) in two halves must give the same bytes.
                std::vector<uint8_t> by_region(lv.size + 64, 0), whole(lv.size + 64, 0);
                SwizzleLevel(whole.data(), lin.data(), lv);
                const uint32_t half = uint32_t(lv.rows / 2);
                WriteLevelRegion(by_region.data(), lv, false, 0, 0, 0, uint32_t(lv.row_bytes), half, uint32_t(lv.depth),
                                 lin.data(), size_t(lv.row_bytes), size_t(lv.row_bytes) * lv.rows);
                WriteLevelRegion(by_region.data(), lv, false, 0, half, 0, uint32_t(lv.row_bytes), uint32_t(lv.rows) - half,
                                 uint32_t(lv.depth), lin.data() + size_t(half) * lv.row_bytes, size_t(lv.row_bytes),
                                 size_t(lv.row_bytes) * lv.rows);
                CHECK(by_region == whole);
            }
    }
}

// ---- arm64 ABI glue ----------------------------------------------------------------------------
#if defined(__aarch64__)
extern "C" {
uint64_t test_guest_call_import(void* thunk, uint64_t arg);
uint64_t test_guest_sum7(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
double test_guest_fp(double, double);
int dsr_setjmp(void*) __attribute__((returns_twice));
}

static uint64_t g_seen_x18 = 0;
extern "C" uint64_t test_host_import(uint64_t a) {
    uint64_t v;
    asm volatile("mov %0, x18" : "=r"(v));
    g_seen_x18 = v;
    return a * 2;
}

static uint64_t ReadX18() {
    uint64_t v;
    asm volatile("mov %0, x18" : "=r"(v));
    return v;
}
// Loads a sentinel into x18 immediately before the call. This file is built with -ffixed-x18, so the
// compiler does not touch x18 between this and dsr_guest_call (which records it as the host value).
static void SetX18(uint64_t v) { asm volatile("mov x18, %0" ::"r"(v)); }

static void TestAbi() {
    using dsr::arch::GuestCall;
    constexpr uint64_t kSentinel = 0x5C5C5C5C12345678ull;

    // guest -> import thunk -> host: host must see its own x18 again, and get x18 back afterwards.
    void* thunk = dsr::loader::MakeDynamicThunk(reinterpret_cast<void*>(&test_host_import), 0);
    SetX18(kSentinel);
    uint64_t r = GuestCall<uint64_t>(reinterpret_cast<void*>(&test_guest_call_import), thunk, uint64_t(20));
    uint64_t after = ReadX18();
    CHECK(r == 41);
    CHECK(g_seen_x18 == kSentinel);
    CHECK(after == kSentinel);

    // Unimplemented stub returns 0 (so the guest computes 0 + 1).
    void* stub = dsr::loader::MakeDynamicThunk(dsr::arch::UnimplementedNamedEntry(),
                                               reinterpret_cast<uint64_t>("test::Unimplemented"));
    SetX18(kSentinel);
    r = GuestCall<uint64_t>(reinterpret_cast<void*>(&test_guest_call_import), stub, uint64_t(5));
    after = ReadX18();
    CHECK(r == 1);
    CHECK(after == kSentinel);

    // Argument shifting for 7 integer args (guest clobbers x18), and FP passthrough.
    SetX18(kSentinel);
    r = GuestCall<uint64_t>(reinterpret_cast<void*>(&test_guest_sum7), uint64_t(1), uint64_t(2), uint64_t(3),
                            uint64_t(4), uint64_t(5), uint64_t(6), uint64_t(7));
    after = ReadX18();
    CHECK(r == 28);
    CHECK(after == kSentinel);
    double d = GuestCall<double>(reinterpret_cast<void*>(&test_guest_fp), 1.5, 4.0);
    CHECK(d == 6.0);

    // musl-layout setjmp/longjmp.
    alignas(16) static uint8_t buf[312];
    volatile int stage = 0;
    int v = dsr_setjmp(buf);
    if (v == 0) {
        stage = 1;
        dsr_longjmp(buf, 7);
    }
    CHECK(stage == 1 && v == 7);
}
#endif

// BC -> ETC2 re-encoding (GPUs without BC): blocks decode back close to the source (spec decoder), cut-out alpha is
// kept exactly in the punch-through format (no individual mode there), and the work pool runs every task once.
static void TestEtcEncode() {
    using namespace dsr::hle::nvn;
    const int W = 37, H = 29;  // edges not on block boundaries
    std::vector<uint8_t> img(size_t(W) * H * 4);
    uint32_t seed = 7;
    auto rnd = [&] { return (seed = seed * 1103515245u + 12345u) >> 16; };
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint8_t* p = &img[(size_t(y) * W + x) * 4];
            p[0] = uint8_t(x * 6 + rnd() % 9);
            p[1] = uint8_t(y * 8 + rnd() % 9);
            p[2] = uint8_t(((x ^ y) & 8) ? 220 : 30);
            p[3] = ((x / 3 + y / 5) % 3 == 0) ? 0 : 255;
        }
    for (int mode = 0; mode < 3; mode++) {
        const bool punch = mode == 1, rgba = mode == 2;
        const int bpb = rgba ? 16 : 8, bxn = (W + 3) / 4, byn = (H + 3) / 4;
        std::vector<uint8_t> out(size_t(bxn) * byn * bpb);
        CHECK(out.size() == (rgba ? EtcRgba8LevelBytes(W, H, 1) : EtcLevelBytes(W, H, 1)));
        if (rgba)
            EncodeEtc2Rgba8(out.data(), img.data(), W, H, 1);
        else
            EncodeEtc2Rgb(out.data(), img.data(), W, H, 1, punch);
        double se = 0;
        long n = 0;
        int alpha_wrong = 0;
        for (int by = 0; by < byn; by++)
            for (int bx = 0; bx < bxn; bx++) {
                const uint8_t* b = &out[(size_t(by) * bxn + bx) * bpb];
                uint8_t d[16][4], a[16];
                DecodeEtc2RgbBlock(rgba ? b + 8 : b, punch, d);
                if (rgba) DecodeEacAlphaBlock(b, a);
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++) {
                        const int sx = bx * 4 + x, sy = by * 4 + y;
                        if (sx >= W || sy >= H) continue;
                        const uint8_t* p = &img[(size_t(sy) * W + sx) * 4];
                        const uint8_t* q = d[y * 4 + x];
                        const int src_a = (punch || rgba) ? p[3] : 255, dec_a = rgba ? a[y * 4 + x] : q[3];
                        if ((src_a >= 128) != (dec_a >= 128)) alpha_wrong++;
                        if (src_a >= 128)
                            for (int c = 0; c < 3; c++) {
                                const double e = double(q[c]) - p[c];
                                se += e * e;
                                n++;
                            }
                    }
            }
        CHECK(alpha_wrong == 0);
        CHECK(n > 0 && se / n < 40.0 * 40.0 / 4);  // PSNR above ~28 dB on this noisy, hard-edged picture
    }
    std::vector<int> hits(1000);
    WorkPool::Get().Run(hits.size(), [&](size_t i) { hits[i]++; });
    bool once = true;
    for (int h : hits) once = once && h == 1;
    CHECK(once);
}

// Cheat codes (Atmosphère format): parsing and the machine, on a fake memory with main at 0x1000 and the heap at
// 0x100000.
namespace {
class FakeMemory final : public dsr::cheats::Memory {
public:
    std::vector<uint8_t> main = std::vector<uint8_t>(0x1000), heap = std::vector<uint8_t>(0x1000);
    uint64_t RegionBase(int r) override { return r == 0 ? 0x1000 : r == 1 ? 0x100000 : 0; }
    uint8_t* At(uint64_t a, size_t n) {
        if (a >= 0x1000 && a + n <= 0x2000) return &main[a - 0x1000];
        if (a >= 0x100000 && a + n <= 0x101000) return &heap[a - 0x100000];
        return nullptr;
    }
    bool Read(uint64_t a, void* out, size_t n) override {
        uint8_t* p = At(a, n);
        if (p) memcpy(out, p, n);
        return p != nullptr;
    }
    bool Write(uint64_t a, const void* in, size_t n) override {
        uint8_t* p = At(a, n);
        if (p) memcpy(p, in, n);
        return p != nullptr;
    }
    void Log(const std::string&) override {}
    uint32_t U32(uint64_t a) {
        uint32_t v = 0;
        Read(a, &v, 4);
        return v;
    }
};
}  // namespace

static void TestCheats() {
    using namespace dsr::cheats;
    const std::string file =
        "Title line\r\n"
        "{Master}\r\n"
        "04000000 00000010 0000002A\r\n"  // main+0x10 = 42 (4 bytes)
        "\n"
        "[Pointer]\n"
        "58000000 00000100\n"            // r0 = [main+0x100] (8 bytes)
        "78000000 00000020\n"            // r0 += 0x20
        "64000000 00000000 12345678\n"   // [r0] = 0x12345678 (4 bytes)
        "[Keys]\n"
        "80000001\n"                     // if A held
        "04100000 00000008 00000001\n"   //   heap+8 = 1
        "21000000\n"                     // else
        "04100000 00000008 00000002\n"   //   heap+8 = 2
        "20000000\n"
        "[Loop] 30000000 00000003 04100000 0000000C 00000000 3100000\n"
        "[Bad]\n"
        "0400000 00000010\n";
    std::vector<Cheat> c = Parse(file);
    CHECK(c.size() == 5);
    CHECK(c[0].master && c[0].name == "Master" && c[0].error.empty() && c[0].code.size() == 3);
    CHECK(c[1].name == "Pointer" && c[1].error.empty() && c[1].code.size() == 7);
    CHECK(c[2].error.empty() && OpenBlocks(c[2].code) == 0);
    CHECK(!c[3].error.empty());  // "3100000" is 7 digits
    CHECK(!c[4].error.empty());
    CHECK(file.substr(c[1].begin, c[1].end - c[1].begin) == c[1].text && c[1].text.rfind("[Pointer]", 0) == 0);

    FakeMemory mem;
    const uint64_t ptr = 0x100100;
    memcpy(&mem.main[0x100], &ptr, 8);
    // Cheats run as one program (as on the Switch): registers carry over from one cheat to the next.
    std::vector<uint32_t> prog = c[0].code;
    prog.insert(prog.end(), c[1].code.begin(), c[1].code.end());
    Vm vm;
    CHECK(vm.Execute(prog, mem, 0, 0));
    CHECK(mem.U32(0x1010) == 42);
    CHECK(mem.U32(0x100120) == 0x12345678);
    CHECK(vm.Register(0) == 0x100120);
    CHECK(vm.Execute(c[2].code, mem, 0, 0));
    CHECK(mem.U32(0x100008) == 2);  // A not held: else branch
    CHECK(vm.Execute(c[2].code, mem, 1, 0));
    CHECK(mem.U32(0x100008) == 1);

    // Loop: heap+0x10.. 4 words = 7 via register 1 as a moving pointer (type A with increment).
    std::vector<uint32_t> loop = {0x40010000, 0x00000000, 0x00100010,  // r1 = 0x100010
                                  0x30020000, 0x00000004,              // loop r2 = 4
                                  0x40030000, 0x00000000, 0x00000007,  //   r3 = 7
                                  0xA4311000,                          //   [r1] = r3 (4 bytes), r1 += 4
                                  0x31020000};                         // end loop
    CHECK(Validate(loop).empty());
    CHECK(vm.Execute(loop, mem, 0, 0));
    for (int i = 0; i < 4; i++) CHECK(mem.U32(0x100010 + i * 4) == 7);
    CHECK(mem.U32(0x100020) == 0);

    // Register conditional (C0) against a static value, 9 arithmetic with an immediate, nested blocks.
    std::vector<uint32_t> cond = {0x40050000, 0x00000000, 0x00000005,      // r5 = 5
                                  0x94055100, 0x00000003,                  // r5 = r5 + 3 (4 bytes)
                                  0xC0455400, 0x00000008,                  // if r5 == 8
                                  0x80000002,                              //   if B held
                                  0x04100000, 0x00000030, 0x00000063,      //     heap+0x30 = 99
                                  0x20000000,
                                  0x04100000, 0x00000034, 0x00000058,      //   heap+0x34 = 88
                                  0x20000000};
    CHECK(Validate(cond).empty());
    CHECK(vm.Execute(cond, mem, 0, 0));
    CHECK(mem.U32(0x100030) == 0 && mem.U32(0x100034) == 88);
    CHECK(vm.Execute(cond, mem, 2, 0));
    CHECK(mem.U32(0x100030) == 99);

    // Outside the memory: counted, nothing breaks; an endless loop stops at the step limit.
    const size_t before = vm.failed_writes();
    CHECK(vm.Execute({0x04000000, 0x00009000, 0x00000001}, mem, 0, 0));
    CHECK(vm.failed_writes() == before + 1);
    CHECK(!vm.Execute({0x30000000, 0xFFFFFFFF, 0x31000000}, mem, 0, 0, 1000));
    CHECK(!Validate({0x04000000, 0x00000010}).empty());  // cut off
    CHECK(!Validate({0xE0000000}).empty());              // unknown type
}

int main() {
    struct T { const char* name; void (*fn)(); } tests[] = {
        {"lz4", TestLz4},
        {"sha256", TestSha256},
        {"standard_allocator", TestStandardAllocator},
        {"os_sync", TestOsSync},
        {"null_nvn", TestNullNvn},
        {"nvn_commands", TestNvnCommands},
        {"shader_translator", TestShaderTranslator},
        {"block_linear", TestBlockLinear},
        {"etc_encode", TestEtcEncode},
        {"cheats", TestCheats},
#if defined(__aarch64__)
        {"abi_arm64", TestAbi},
#endif
    };
    for (const T& t : tests) {
        int before = g_failures;
        t.fn();
        printf("%-20s %s\n", t.name, g_failures == before ? "ok" : "FAILED");
    }
#if !defined(__aarch64__)
    printf("%-20s skipped (not arm64)\n", "abi_arm64");
#endif
    printf("%d failure(s)\n", g_failures);
    return g_failures ? 1 : 0;
}
