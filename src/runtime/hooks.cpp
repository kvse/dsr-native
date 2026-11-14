// Hooks on specific functions inside the game's main module, for the game builds listed in kBuilds (matched by
// build id): 1.0.0 and the 1.0.3 update (two editions of it: title 010088F00820E000's and 01004AB00A260000's, the same
// version with different programs). They replace engine functions whose output is useful to us but goes
// nowhere in a retail build, and patch the frame rate and resolution. The places were found in 1.0.0 and matched
// in 1.0.3 by their code (the same instructions around them, with addresses masked).
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <thread>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <pthread.h>
#include <unistd.h>
#include <vector>
#include <string>
#include <mutex>
#include <map>

#include "common/log.h"
#include "hle/os/heap.h"
#include "hle/os/os.h"
#include <atomic>

#include "runtime/runtime.h"

namespace dsr {
namespace {
float* g_game_step = nullptr;  // the game's per-frame time step (scratch page), when patched

// Where things are in one game build (offsets in main).
struct GameBuild {
    const char* build_id;  // first 16 bytes, hex
    const char* version;
    const char* data_version;  // DataVersion.txt of the RomFS that goes with it
    // Per-frame update: adrp x8 / ldr s8, [x8, #...] loading the constant 1/30 s time step.
    uint64_t step_adrp;
    uint32_t step_adrp_insn, step_ldr_insn;
    // Render thread frame limiter (this, fps) and its state in `this`.
    uint64_t limiter;
    uint64_t limiter_global;  // 1.0.0: skipped while this int (main+) is non-zero; 0 = see limiter_enabled
    uint32_t limiter_enabled;  // 1.0.3: skipped while this byte of `this` is zero
    uint32_t last_tick, skip_active, skip_toggle, skip_time;
    // Resolution defaults (mov w2/w1, #1280 / #720).
    uint64_t width_cfg, width_get, display_width, height_cfg, height_get, display_height;
    // Engine log, log with va_list, DL_PANIC.
    uint64_t log, vlog, panic;
    // Game frame rate while a video plays. The movie player is frame-based: 1.0.0 shows one video frame per game
    // frame (30 fps videos: 30), 1.0.3 one per two game frames (60; at 30 its videos ran at half speed).
    int movie_fps;
    // Performance mode (settings performance_mode): the engine's own quality variables, from its debug menu. (The
    // LOD level bias, 1 on the Switch, is left alone: at 2 nearby walls switched to low-detail models with gaps.)
    //   lowvfx_read:   ldrb w10, [x20, #0x599] reading "Enable Low Quality Vfx" (0) into the effect setup: becomes
    //                  mov w10, #1 (effects through the engine's low-quality resolve).
    //   shadow_size:   the shadow map size global (debug menu "ShadwoMap Size", 512..2048, 2048): set to 1024
    //                  before the game starts; the engine sizes the shadow target and its texel offsets from it.
    uint64_t lowvfx_read, shadow_size;
};
const GameBuild kBuilds[] = {
    {"5a797a3a5da6c4f3468cb7c84e4dde63", "1.0.0", "20180818_462",
     0x66c9c8, 0xB00080A8, 0xBD497108,
     0x70bd90, 0x251fb4c, 0, 0x310, 0x31d, 0x31e, 0x320,
     0x6a170c, 0x6a18f8, 0x716170, 0x6a1758, 0x6a1968, 0x716178,
     0x7fea18, 0x7feb30, 0x7892d8, 30,
     0x3620f4, 0x1aeb890},
    {"6569410c91c398e78ec76b418ee4cca8", "1.0.3", "20181026_101",
     0x667930, 0xB0008068, 0xBD455908,
     0x706d28, 0, 0x2f7, 0x2e8, 0x2f5, 0x2f6, 0x2f8,
     0x69c638, 0x69c824, 0x711ff0, 0x69c684, 0x69c894, 0x711ff8,
     0x7f71d4, 0x7f72ec, 0x782dac, 60,
     0x356790, 0x1adf890},
    // 1.0.3 of title 01004AB00A260000 (update v196608, buildinfo region DV): the same game and data as the other
    // 1.0.3 with its code moved; matched from it, and the limiter's fields are the same.
    {"df3766a2bb651a3e822828abf10b6f7b", "1.0.3", "20181026_101",
     0x667900, 0xF0008088, 0xBD46ED08,
     0x707090, 0, 0x2f7, 0x2e8, 0x2f5, 0x2f6, 0x2f8,
     0x69c608, 0x69c7f4, 0x711ec8, 0x69c654, 0x69c864, 0x711ed0,
     0x7f9ec8, 0x7f9fe0, 0x784660, 60,
     0x356790, 0x1ae3890},
};
const GameBuild* g_build = nullptr;

// Skip intro logos (settings skip_intro). The title screen's step machine (TitleStep / TitleStep_forNS, step
// number at this+0xc) opens the company logo dialog (menu dialog 60, FrpgMenuDlgLogo) by setting its request word
// to 1 (`bl` to set(60, 1)) and then waits until the dialog has cleared it again after its last logo. Both classes'
// open calls become `nop`, so the wait passes at once, as if the logos had finished. TitleStep_forNS also counts
// down a timer (this+0xe0) before closing the dialog: that step branches straight to its close path. Everything
// else (loading, the title movie, press start) runs as before.
struct LogoSkip {
    const char* build_id;  // first hex digits of the build (GameBuild::build_id)
    uint64_t open[2];
    uint32_t open_insn[2];  // bl set_dialog_request (60, 1)
    uint64_t timer;         // ldr s1, [x19, #0xe0]
    uint64_t timer_done;    // the timer-expired path
};
const LogoSkip kLogoSkips[] = {
    {"5a797a3a", {0x6f1c30, 0x6f73e8}, {0x97fa3d6c, 0x97fa277e}, 0x6f7450, 0x6f746c},
    {"6569410c", {0x6ed0e4, 0x6f27fc}, {0x97fa3510, 0x97fa1f4a}, 0x6f2864, 0x6f2880},
    {"df3766a2", {0x6ecf7c, 0x6f268c}, {0x97fa355e, 0x97fa1f9a}, 0x6f26f4, 0x6f2710},
};
constexpr uint32_t kNop = 0xD503201F, kLdrS1Timer = 0xBD40E261;

// Performance mode: low-quality effects, 1024x1024 shadow maps (see GameBuild); the menu also sets render scale 0.75.
// Each part is checked before it is changed and is skipped (logged) if the game's code or data is not as expected.
void ApplyPerformanceMode(Runtime& rt, const GameBuild& b) {
    constexpr uint32_t kLdrbLowVfx = 0x3956668A;    // ldrb w10, [x20, #0x599]
    constexpr uint32_t kMovLowVfxOn = 0x5280002A;   // mov w10, #1
    std::string err;
    std::string done;
    if (rt.main.PatchInstruction(b.lowvfx_read, kLdrbLowVfx, kMovLowVfxOn, &err))
        done += done.empty() ? "low-quality effects" : ", low-quality effects";
    else
        LOGW("hooks", "performance mode: effects quality not changed (%s)", err.c_str());
    int32_t* shadow = reinterpret_cast<int32_t*>(rt.main.base() + b.shadow_size);
    if (rt.main.Contains(reinterpret_cast<uintptr_t>(shadow)) && *shadow == 2048) {
        *shadow = 1024;
        done += done.empty() ? "shadow maps 1024" : ", shadow maps 1024";
    } else {
        LOGW("hooks", "performance mode: shadow map size not changed (found %d)",
             rt.main.Contains(reinterpret_cast<uintptr_t>(shadow)) ? *shadow : -1);
    }
    LOGI("hooks", "performance mode: %s", done.empty() ? "nothing applied" : done.c_str());
}

void SkipIntroLogos(Runtime& rt, const GameBuild& b) {
    const LogoSkip* p = nullptr;
    for (const LogoSkip& l : kLogoSkips)
        if (!strncmp(l.build_id, b.build_id, strlen(l.build_id))) p = &l;
    if (!p) return;
    struct Edit { uint64_t at; uint32_t from, to; };
    const Edit edits[] = {
        {p->open[0], p->open_insn[0], kNop},
        {p->open[1], p->open_insn[1], kNop},
        {p->timer, kLdrS1Timer, 0x14000000u | uint32_t(((p->timer_done - p->timer) / 4) & 0x3FFFFFF)},  // b
    };
    std::string err;
    size_t done = 0;
    while (done < std::size(edits) && rt.main.PatchInstruction(edits[done].at, edits[done].from, edits[done].to, &err))
        done++;
    if (done == std::size(edits)) {
        LOGI("hooks", "skip_intro: the intro logos are skipped");
        return;
    }
    LOGW("hooks", "skip_intro: patch failed (%s): the logos are shown", err.c_str());
    std::string undo_err;
    while (done-- > 0) rt.main.PatchInstruction(edits[done].at, edits[done].to, edits[done].from, &undo_err);
}

// ---- dynamic frame rate (fps = auto) ---------------------------------------------------------------------------
// After PR #13 ("Dynamic FPS" mods for console ports): the game's time step already follows the real frame time
// (FrameLimiter), so the game keeps its speed at any rate; this half picks the rate. A phone screen shows 60, 30, 20
// and 15 fps with even frame spacing (they divide 60 and 90 Hz), 22-25 fps would alternate short and long frames, so
// the cap moves between those rates with hysteresis: down quickly when frames do not fit their period, up slowly and
// only with clear headroom. Not while a video plays: videos are paced at the game version's movie rate.
std::atomic<int> g_movies{0};
std::atomic<int> g_cap_fps{60};  // the rate the limiter paces to with fps = auto
std::atomic<float> g_gpu_frame_ms{0.0f};
bool MoviePlaying() { return g_movies.load(std::memory_order_relaxed) > 0; }
int MovieFps() { return g_build ? g_build->movie_fps : 30; }
int MinFps() { return std::clamp(GetRuntime().config.min_fps, 15, 30); }

struct DynamicFps {
    double fast = 0, slow = 0;  // smoothed cost of a frame in ms (fast: ~7 frames, slow: ~90 frames)
    int over = 0, under = 0, hold = 0;
};
DynamicFps g_dyn;

int DynamicRates(int out[6]) {  // highest first: 60, 30, 20, 15 down to min_fps
    static constexpr int kRates[] = {60, 30, 20, 15};
    const int lo = MinFps();
    int n = 0;
    for (int r : kRates)
        if (r >= lo) out[n++] = r;
    if (out[n - 1] > lo) out[n++] = lo;
    return n;
}

void DynamicStep(double work_ms) {
    DynamicFps& s = g_dyn;
    int rates[6];
    const int n = DynamicRates(rates);
    int cap = g_cap_fps.load(std::memory_order_relaxed);
    int idx = n - 1;
    for (int i = 0; i < n; i++)
        if (rates[i] <= cap) {
            idx = i;
            break;
        }
    cap = rates[idx];
    const double period = 1000.0 / cap;
    // A frame's cost: what the game thread took, or the GPU's time if that is longer (with the cap leaving the GPU
    // idle, the game thread alone looks cheap). A loading stall is not the cost of a frame.
    const double gpu = g_gpu_frame_ms.load(std::memory_order_relaxed);
    const double cost = std::min(std::max(work_ms, gpu * 1.03), period * 3.0);
    if (s.fast == 0) s.fast = s.slow = cost;
    s.fast += 0.15 * (cost - s.fast);
    s.slow += (1.0 / 90.0) * (cost - s.slow);
    if (s.hold > 0) s.hold--;
    s.over = s.fast > period * 0.98 ? s.over + 1 : 0;
    if (s.over >= 10 && idx + 1 < n) {
        int next = idx + 1;
        while (next + 1 < n && 1000.0 / rates[next] < s.fast * 1.06) next++;
        LOGI("hooks", "fps auto: %d -> %d fps (frame cost %.1f ms, GPU %.1f ms)", cap, rates[next], s.fast, gpu);
        g_cap_fps.store(rates[next], std::memory_order_relaxed);
        s.over = s.under = 0;
        s.slow = s.fast;
        s.hold = 900;  // ~30-45 s before trying a higher rate again
        return;
    }
    if (idx > 0 && s.hold == 0) {
        s.under = s.slow * 1.15 < 1000.0 / rates[idx - 1] ? s.under + 1 : 0;
        if (s.under >= 240) {
            LOGI("hooks", "fps auto: %d -> %d fps (frame cost %.1f ms, GPU %.1f ms)", cap, rates[idx - 1], s.slow, gpu);
            g_cap_fps.store(rates[idx - 1], std::memory_order_relaxed);
            s.under = 0;
            s.hold = 300;
        }
    } else {
        s.under = 0;
    }
}

// Replacement for the game's frame limiter (1.0.0: main+0x70bd90, 1.0.3: main+0x706d28; this, fps): called once
// per game update on the render thread. Same contract as the original: skip when the engine says so (1.0.0: a
// global flag; 1.0.3: a flag in `this`); sleep until a frame period has passed since the last call; remember the
// tick (1.0.0: this+0x310). Then the game's time step is set to the period that actually passed, and the
// frame-skip state (1.0.0: this+0x31d/0x31e/0x320) is kept clear.
void FrameLimiter(uint8_t* self, int /*fps*/) {
    const Runtime& rt = GetRuntime();
    const GameBuild& b = *g_build;
    if (b.limiter_global ? *reinterpret_cast<const int32_t*>(rt.main.base() + b.limiter_global) != 0
                         : self[b.limiter_enabled] == 0)
        return;
    using clock = std::chrono::steady_clock;
    static clock::time_point last;
    auto now = clock::now();
    // fps = auto: what this frame cost before any sleeping (videos are paced at their own rate).
    if (rt.config.dynamic_fps && last.time_since_epoch().count() != 0 && !MoviePlaying())
        DynamicStep(std::chrono::duration<double, std::milli>(now - last).count());
    const int fps = FrameRateNow();
    const auto period = std::chrono::nanoseconds(1'000'000'000 / fps);
    if (last.time_since_epoch().count() != 0 && now < last + period) {
        std::this_thread::sleep_until(last + period);
        now = clock::now();
    }
    const double seconds = last.time_since_epoch().count() ? std::chrono::duration<double>(now - last).count() : 1.0 / fps;
    last = now;
    // Up to 1/20 s per step (1/min_fps with fps = auto and min_fps 15): slower devices keep the game at normal speed
    // instead of slow motion.
    const double slowest = rt.config.dynamic_fps ? std::min(MinFps(), 20) : 20;
    if (g_game_step) *g_game_step = float(std::clamp(seconds, 1.0 / 60.0, 1.0 / slowest));
    *reinterpret_cast<int64_t*>(self + b.last_tick) = hle::os::NowTicks();
    self[b.skip_active] = 0;
    self[b.skip_toggle] = 0;
    *reinterpret_cast<double*>(self + b.skip_time) = 0;
}

void Trim(char* s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = 0;
}

uintptr_t GuestOffset(const void* p) {
    uintptr_t a = reinterpret_cast<uintptr_t>(p);
    const loader::Module& m = GetRuntime().main;
    return m.Contains(a) ? a - m.base() : 0;
}

// Engine debug print (1.0.0: main+0x7fea18), (int level, const char* fmt, ...)
void EngineLog(int level, const char* fmt, ...) {
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt ? fmt : "", ap);
    va_end(ap);
    Trim(msg);
    if (msg[0]) LOGI("engine", "[%d] %s", level, msg);
}

// The same with a va_list (1.0.0: main+0x7feb30)
void EngineLogV(int level, const char* fmt, va_list ap) {
    char msg[2048];
    vsnprintf(msg, sizeof(msg), fmt ? fmt : "", ap);
    Trim(msg);
    if (msg[0]) LOGI("engine", "[%d] %s", level, msg);
}

// DL_PANIC(const char* file, int line, const char* fmt, ...) (1.0.0: main+0x7892d8)
void EnginePanic(const char* file, int line, const char* fmt, ...) {
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt ? fmt : "", ap);
    va_end(ap);
    Trim(msg);
    const char* base = file ? strrchr(file, '/') : nullptr;
    FATAL("engine", "DL_PANIC: %s  (%s:%d, called from main+0x%lx)", msg, base ? base + 1 : (file ? file : "?"), line,
          (unsigned long)GuestOffset(__builtin_return_address(0)));
}

// ---- aspect ratio: camera aspect (experimental) --------------------------------------------------------------
// With a wider or narrower render resolution the 3D view still came out stretched: the camera that renders the
// scene keeps a 16:9 aspect ratio. Cameras keep {fov, aspect, near, far} at +0x50..+0x5c (main+0xfa7f4 reads
// fov and aspect for the view frustum). This scans the guest heap and the game's data for such blocks with a
// 16:9 aspect, logs them (with the owner's vtable, to find the code that sets it) and writes the wanted aspect
// into them, again every second (rescanning every 2 s until cameras are found, then every 10 s).
float g_aspect = 0;
void CameraAspectThread() {
    pthread_setname_np(pthread_self(), "dsr-aspect");
    const loader::Module& m = GetRuntime().main;
    std::vector<float*> found;
    int scans = 0;
    auto plausible = [](const float* a) {  // a = &aspect: fov before it, near and far after
        const float fov = a[-1], nearz = a[1], farz = a[2];
        return fov > 0.2f && fov < 2.5f && nearz > 0.001f && nearz < 50.0f && farz > nearz && farz < 1e6f;
    };
    for (;;) {
        const auto scan_start = std::chrono::steady_clock::now();
        // Scan: 16:9 exactly (0x3FE38E39) at a 4-byte boundary.
        uintptr_t hb = 0;
        size_t hs = 0;
        hle::os::GuestHeapRange(&hb, &hs);
        std::vector<std::pair<uintptr_t, size_t>> ranges;
        if (hb && hs) ranges.push_back({hb, hs});
        const loader::NsoImage& nso = GetRuntime().main_nso;  // .data + .bss
        const size_t data = nso.data.mem_offset, data_size = size_t(nso.data.size) + nso.bss_size;
        if (data && m.size() > data) ranges.push_back({m.base() + data, std::min<size_t>(m.size() - data, data_size)});
        int new_found = 0;
        for (const auto& [base, size] : ranges) {
            const uint32_t* p = reinterpret_cast<const uint32_t*>(base + 4);
            const uint32_t* end = reinterpret_cast<const uint32_t*>(base + size - 12);
            for (; p < end; p++) {
                if (*p != 0x3FE38E39u) continue;
                float* a = reinterpret_cast<float*>(const_cast<uint32_t*>(p));
                if (!plausible(a)) continue;
                if (std::find(found.begin(), found.end(), a) != found.end()) continue;
                found.push_back(a);
                new_found++;
                const uintptr_t obj = reinterpret_cast<uintptr_t>(a) - 0x54;
                uintptr_t vt = 0;
                if ((obj & 7) == 0) memcpy(&vt, reinterpret_cast<const void*>(obj), 8);
                if (found.size() <= 40)
                    LOGI("hooks", "camera aspect: 16:9 at %p (fov %.3f near %.3f far %.1f), object vtable %s0x%lx", a,
                         a[-1], a[1], a[2], m.Contains(vt) ? "main+" : "", (unsigned long)(m.Contains(vt) ? vt - m.base() : vt));
            }
        }
        scans++;
        const double scan_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - scan_start).count();
        if (new_found || scans <= 3 || scans % 60 == 0)
            LOGI("hooks", "camera aspect: scan %d (%.0f ms): %d new 16:9 cameras, %zu in all; setting them to %.3f", scans,
                 scan_ms, new_found, found.size(), g_aspect);
        const int rewrites = found.empty() ? 2 : 10;  // 2 s, then 10 s between scans
        for (int k = 0; k < rewrites; k++) {
            for (float* a : found)
                if (*a > 1.7777f && *a < 1.7779f && plausible(a)) *a = g_aspect;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}

struct Hook {
    uint64_t offset;
    void* fn;
    const char* name;
};

}  // namespace

float GameFrameTime() { return g_game_step ? *g_game_step : 1.0f / 30.0f; }
void ReportGpuFrameTime(double ms) {
    if (!(ms > 0.0)) return;
    const float prev = g_gpu_frame_ms.load(std::memory_order_relaxed);
    g_gpu_frame_ms.store(prev > 0.0f ? prev + 0.1f * (float(ms) - prev) : float(ms), std::memory_order_relaxed);
}
void SetMoviePlaying(bool playing) {
    playing ? ++g_movies : --g_movies;
    LOGI("hooks", "movie %s: frame rate %d fps", playing ? "started" : "ended", FrameRateNow());
}
bool FrameRateAdjustable() { return g_game_step != nullptr; }
namespace {
std::atomic<int> g_gpu_has_bc{-1};
}
void SetGpuHasBc(bool has) { g_gpu_has_bc = has ? 1 : 0; }
int GpuHasBc() { return g_gpu_has_bc.load(); }
int FrameRateNow() {
    if (MoviePlaying()) return MovieFps();
    const RuntimeConfig& c = GetRuntime().config;
    return c.dynamic_fps ? g_cap_fps.load(std::memory_order_relaxed) : std::clamp(c.target_fps, 30, 60);
}

const char* GameVersion() { return g_build ? g_build->version : nullptr; }

void InstallGameHooks() {
    Runtime& rt = GetRuntime();
    for (const GameBuild& b : kBuilds)
        if (rt.main_nso.build_id_hex.rfind(b.build_id, 0) == 0) g_build = &b;
    if (g_build && !rt.config.game_hooks) {
        LOGW("hooks", "game version %s: engine patches off (settings game_hooks = 0): 30 fps, 16:9, no engine log",
             g_build->version);
        rt.config.target_fps = 30;
        rt.config.dynamic_fps = false;
        return;
    }
    if (!g_build) {
        LOGW("hooks", "unknown game build %.32s: engine hooks disabled (supported: 1.0.0, 1.0.3)",
             rt.main_nso.build_id_hex.c_str());
        rt.config.dynamic_fps = false;  // no frame limiter of ours: the game paces itself
        return;
    }
    const GameBuild& b = *g_build;
    LOGI("hooks", "game version %s (build %.16s...)", b.version, rt.main_nso.build_id_hex.c_str());
    // The program and the data files must be the same version (e.g. the 1.0.3 exefs over 1.0.0 data is not).
    if (FILE* f = fopen((rt.config.game_dir + "/DataVersion.txt").c_str(), "r")) {
        char data[64] = {};
        if (fgets(data, sizeof(data), f)) {
            Trim(data);
            for (char* e = data + strlen(data); e > data && e[-1] == ' '; *--e = 0) {}
            for (const GameBuild& o : kBuilds)
                if (strcmp(o.data_version, b.data_version) != 0 && strcmp(data, o.data_version) == 0)
                    LOGW("hooks", "the game's data files are version %s (DataVersion %s) but the program is %s: "
                         "install the matching data (the update's RomFS) or the game may misbehave", o.version, data,
                         b.version);
            if (strcmp(data, b.data_version) == 0) LOGI("hooks", "data files: version %s (%s)", b.version, data);
        }
        fclose(f);
    }
    // Frame rate (fps = 60; offsets below are 1.0.0's). The same two places a well-known Switch 60 fps mod changes,
    // done differently:
    // 1. The game's per-frame update (main+0x66c99c) steps every system by a constant 1/30 s loaded from
    //    read-only data (main+0x66c9c8 adrp x8, page; +0x66c9cc ldr s8, [x8, #0x970]). It now loads a float in
    //    the scratch page instead.
    // 2. The render thread's frame limiter (main+0x70bd90, called once per game update with a hard-coded 30) is
    //    replaced by FrameLimiter: it paces to 60 and sets that float to the measured frame period (1/60..1/20 s),
    //    so the game keeps normal speed at 60 fps and when heavy areas run slower. It also keeps the game's
    //    frame-skip mode off (with it, slow frames ran two updates per shown frame).
    // Installed at every frame rate (30 included), so the cap can be changed while the game runs (in-game
    // settings menu): FrameLimiter reads the configured rate every frame.
    {
        std::string err;
        float* step = reinterpret_cast<float*>(rt.main.ScratchPage());
        *step = 1.0f / 30.0f;
        const uint32_t adrp = rt.main.EncodeAdrp(b.step_adrp, 8, reinterpret_cast<uintptr_t>(step));
        const bool adrp_ok = rt.main.PatchInstruction(b.step_adrp, b.step_adrp_insn, adrp, &err);
        bool ok = adrp_ok && rt.main.PatchInstruction(b.step_adrp + 4, b.step_ldr_insn, 0xBD400108, &err);  // ldr s8, [x8]
        if (ok) ok = rt.main.HookFunction(b.limiter, reinterpret_cast<void*>(&FrameLimiter), &err);
        if (ok) {
            g_game_step = step;
            if (rt.config.dynamic_fps)
                LOGI("hooks", "frame rate: own frame limiter, fps auto (60 / 30 / 20 fps, not below %d; changeable in game), "
                     "game time step follows the frame time", MinFps());
            else
                LOGI("hooks", "frame rate: own frame limiter at %d fps (changeable in game), game time step follows the frame time",
                     std::clamp(rt.config.target_fps, 30, 60));
        } else {
            LOGW("hooks", "60 fps patches failed (%s): staying at 30 fps", err.c_str());
            rt.main.PatchInstruction(b.step_adrp + 4, 0xBD400108, b.step_ldr_insn, &err);  // undo what applied
            if (adrp_ok) rt.main.PatchInstruction(b.step_adrp, adrp, b.step_adrp_insn, &err);
            rt.config.target_fps = 30;
            rt.config.dynamic_fps = false;
        }
    }
    // Aspect ratio (offsets below are 1.0.0's; kBuilds has each build's). Two places set the game's resolution, both
    // with built-in defaults:
    // - the render resolution, read from the game's settings with defaults 1280 / 720 (main+0x6a16b0:
    //   "Normal.Resolution.Width" main+0x6a170c mov w2, #1280; "...Height" main+0x6a1758 mov w2, #720), and
    //   again by the width getter main+0x6a18cc (main+0x6a18f8 mov w2, #1280) whose callers (main+0x101d80,
    //   +0x66e99c, +0x66e9cc) size the post-processing buffers and their copies of the scene. It
    //   also becomes the screen size floats (main+0x1af4a7c / +0x1af4a84) the cameras take their aspect ratio
    //   from (e.g. main+0xbca88), so the 3D view widens (or narrows) instead of stretching;
    // - the handheld display mode (main+0x716158: main+0x716170 mov w1, #1280; +0x716178 mov w2, #720).
    // Frame buffers are allocated at 1920x1080 (main+0x70a20c), the limit. The height always stays 720: the
    // game's half- and quarter-size buffers (640x360, 320x180) keep their 720-line sizes whatever the render
    // resolution, and a 4:3 test at 1280x960 died silently on loading a save. Only the width changes: wider
    // than 16:9 shows more at the sides, narrower (4:3: 960x720) less.
    // Render scale (settings render_scale, 0.5 - 1.0): the same resolution settings at a lower size, height and
    // width together (the height also has a getter with its own default, main+0x6a193c: main+0x6a1968 mov w2, #720).
    // The game draws less and the frame is enlarged to the screen at the end. Heights stay at or below 720 (the
    // fixed half- and quarter-size buffers are then larger than needed, which is harmless).
    if (!rt.config.docked) {
        float ar = 16.0f / 9.0f;
        bool ar_ok = true;
        if (rt.config.aspect_ratio == "auto") {
            ar = rt.config.screen_aspect;
        } else if (rt.config.aspect_ratio != "16:9") {
            float w = 0, h = 0;
            if (sscanf(rt.config.aspect_ratio.c_str(), "%f:%f", &w, &h) == 2 && h > 0) ar = w / h;
            else ar = 0;
        }
        if (!(ar > 0.5f && ar < 4.0f)) {
            LOGW("hooks", "aspect_ratio %s: not understood (or screen size unknown), staying at 16:9",
                 rt.config.aspect_ratio.c_str());
            ar = 16.0f / 9.0f;
            ar_ok = false;
        }
        const float scale = std::clamp(rt.config.render_scale, 0.5f, 1.0f);
        const int height = std::clamp(int(std::lround(720.0f * scale / 8.0f)) * 8, 360, 720);
        const int width = std::clamp(int(std::lround(float(height) * ar / 8.0f)) * 8, 320, 1920);
        if (width != 1280 || height != 720) {
            std::string err;
            auto mov = [](int rd, int v) { return 0x52800000u | (uint32_t(v) << 5) | uint32_t(rd); };
            bool ok = true;
            if (width != 1280)
                ok = ok && rt.main.PatchInstruction(b.width_cfg, 0x5280A002, mov(2, width), &err) &&
                     rt.main.PatchInstruction(b.width_get, 0x5280A002, mov(2, width), &err) &&
                     rt.main.PatchInstruction(b.display_width, 0x5280A001, mov(1, width), &err);
            if (height != 720)
                ok = ok && rt.main.PatchInstruction(b.height_cfg, 0x52805A02, mov(2, height), &err) &&
                     rt.main.PatchInstruction(b.height_get, 0x52805A02, mov(2, height), &err) &&
                     rt.main.PatchInstruction(b.display_height, 0x52805A02, mov(2, height), &err);
            if (ok) {
                LOGI("hooks", "aspect ratio %s (%.3f), render scale %.2f: the game renders at %dx%d (was 1280x720)",
                     ar_ok ? rt.config.aspect_ratio.c_str() : "16:9", ar, scale, width, height);
                g_aspect = float(width) / float(height);
                rt.config.render_width = width;
                rt.config.render_height = height;
                if (std::fabs(g_aspect - 16.0f / 9.0f) > 0.01f) std::thread(CameraAspectThread).detach();
            } else {
                LOGW("hooks", "resolution patch failed: %s", err.c_str());
            }
        }
    }
    if (rt.config.skip_intro) SkipIntroLogos(rt, b);
    if (rt.config.performance_mode) ApplyPerformanceMode(rt, b);
    const Hook hooks[] = {
        {b.log, reinterpret_cast<void*>(&EngineLog), "engine log"},
        {b.vlog, reinterpret_cast<void*>(&EngineLogV), "engine vlog"},
        {b.panic, reinterpret_cast<void*>(&EnginePanic), "DL_PANIC"},
    };
    for (const Hook& h : hooks) {
        std::string err;
        if (rt.main.HookFunction(h.offset, h.fn, &err))
            LOGI("hooks", "hooked %s at main+0x%lx", h.name, (unsigned long)h.offset);
        else
            LOGW("hooks", "could not hook %s: %s", h.name, err.c_str());
    }
}

}  // namespace dsr
