#pragma once
// Process-wide state for the native runtime: loaded modules, configured paths, guest allocator hooks.

#include <cstdint>
#include <memory>
#include <string>

#include "loader/module.h"
#include "loader/nso.h"

namespace dsr {

struct RuntimeConfig {
    std::string game_dir;     // directory holding exefs/ and the romfs contents (dvdbnd*.bdt, ...)
    std::string save_dir;     // writable directory for save data
    std::string cache_dir;    // writable directory for caches (shaders etc.)
    bool dry_run = false;     // map + relocate + bind only, never execute guest code
    bool abort_on_unimplemented = false;
    bool docked = false;      // nn::oe::GetOperationMode: false = handheld (720p), true = console
    std::string language = "en-US";
    // User settings (settings.txt next to the game folder on Android).
    int target_fps = 60;          // presentation pace: 30 (the Switch rate) .. 60; with dynamic_fps the highest rate
    // fps = auto: the cap follows what the device sustains (60, 30, 20 fps, not below min_fps), the game's time step
    // follows the frame time, so the game keeps its speed (dynamic fps, after PR #13).
    bool dynamic_fps = false;
    int min_fps = 20;             // lowest rate fps = auto may use (15..30); also the longest game time step
    bool async_pipelines = true;  // compile new GPU pipelines off the render thread (no hitch; the draw waits a frame)
    // world_capture: like auto_capture's frame captures (dsr_frame_*.bin and the "pass table" lines in the log) but only
    // from frames with a lot of 3D (>= 300 draws), at most 3, and without auto_capture's shader dumps to disk.
    bool world_capture = false;
    bool auto_capture = false;    // diagnostics: save a few frame captures automatically during play
    bool profile = true;          // diagnostics: sampling CPU profile of the busy threads, logged every 10 s
    float gamma = 1.0f;           // extra display gamma applied through the game's brightness ramp (> 1 lifts shadows)
    std::string present_mode = "fifo";  // swapchain present mode: fifo (vsync, default), mailbox, fifo_relaxed, immediate (FIFO if unsupported)
    bool lrz_workaround = false;
    bool shader_depth_test = true;  // depth tests against gl_FragDepth-written depth done in the shader (square holes)  // refresh depth buffers written by gl_FragDepth before reuse (square holes)
    int hole_test = 0;  // square-hole diagnostics (bits): 1 = effects without depth test, 2 = uncompressed depth, 4 = no write-masked colour targets, 8 = one render pass per draw into reduced-size targets, 16 = depth textures read from copies, 64 = D24S8 depth, 128 = CPU waits for GPU idle at each slot, 256 = uncompressed colour targets, 512 = no null colour attachments, 1024 = effect shaders output debug values, 2048 = no depth test in reduced-size effect passes
    bool audio_smooth = false;  // release audio buffers at a smoothed pace (experiment; movies were not timed by it)
    // Older Adreno GPUs: storage usage and mutable format only where needed, so images keep their bandwidth
    // compression. Off by default: purple streaks on the OnePlus 6T (Adreno 630) for ~5% less GPU time.
    bool gpu_compression = false;
    // Aspect ratio of the game's handheld (720p) output: "16:9" (the game's own), "auto" (the screen's, from
    // screen_aspect) or "W:H". Experimental: widens the display resolution the game sets up (main+0x716170).
    std::string aspect_ratio = "16:9";
    float screen_aspect = 0;  // width / height of the screen (Android: the window), 0 = unknown
    bool hud_box = true;      // other aspect ratios: keep the HUD and menus in a centred 16:9 box
    // hud_full_res = 1: the game's HUD and menus (its blended 2D layer) are drawn into a screen-sized image on top of
    // the enlarged 3D picture, instead of into the game's frame (render size) that the screen then stretches: text
    // and icons stay sharp at any render_scale / aspect_ratio. Costs one enlarging blit and one screen-sized copy per
    // frame; it only applies when the screen is at least 1.2x the game's frame.
    bool hud_full_res = true;
    int render_width = 1280;  // the game's render size once aspect_ratio and render_scale are applied
    int render_height = 720;
    float render_scale = 1.0f;  // settings render_scale: 0.30 - 1.0 of the game's 720-line resolution
    // GPU driver: settings.txt `gpu_driver` (a file in DSRNative/drivers, or "system"). The Android front end
    // copies (or unzips) it into the app's private folder: gpu_driver_dir (ends in '/') holds gpu_driver_lib.
    // Empty gpu_driver_lib: the system driver.
    std::string gpu_driver = "system";
    std::string gpu_driver_dir, gpu_driver_lib;
    std::string native_lib_dir;  // the app's native library folder (libadrenotools loads its hooks from there)
    bool bc_enabler = true;      // Adreno 6xx system drivers: patch in the BC texture formats (BCeNabler)
    // The swapchain has the game's frame size and the system compositor scales it to the screen, instead of the
    // GPU filling a screen-sized image each frame.
    bool compositor_scaling = true;
    // GPUs without BC textures (Mali; Adreno 6xx without BCeNabler): BC1 textures are re-encoded as ETC2 (same size
    // as BC1) instead of decoded to RGBA8 (4x larger), when the GPU has ETC2 (bc_etc2); BC3 too with bc3_etc2
    // (4x smaller than RGBA8, but lossier: off by default).
    bool bc_etc2 = true;
    bool bc3_etc2 = false;
    // bc_astc = 1: BC1 and BC3 textures are re-encoded as ASTC 4x4 instead (GPUs with ASTC LDR: all Mali). 8 bits per
    // texel: twice the size of BC1 as ETC2, the same as BC3; BC1 stays close to the source and alpha is native.
    bool bc_astc = false;
    // astc_block: the ASTC block size (4, 6 or 8) of converted textures: 8, 3.56 or 2 bits per texel. bc5_astc = 1:
    // BC5 (normal maps) also becomes ASTC (luminance + alpha, two weight planes) instead of staying RG8 (16 bits).
    int astc_block = 4;
    bool bc5_astc = false;
    // Diagnostics. shadow_compare: how the shadow map lookups compare (0 = the game's function as Vulkan's;
    // 1 = operands swapped; 2 = always 1 (shadowed); 3 = always 0 (lit)).
    int shadow_compare = 0;
    // game_hooks = 0: none of the engine patches (frame rate, resolution, log hooks) are installed.
    bool game_hooks = true;
    // render_pass_objects = 1: VkRenderPass / VkFramebuffer objects even when the driver has dynamic rendering
    // (diagnostics for drivers whose dynamic rendering misbehaves, e.g. Mali flicker reports).
    bool render_pass_objects = false;
    // mali_barriers = 1 (PR #13): narrow layout-transition barriers (destination = the stages that use the image) and
    // render targets made readable when their pass ends, so a tile-based GPU (Mali) can overlap passes. Off by default.
    bool mali_barriers = false;
    // performance_mode = 1: the engine's own lower-quality options: low-quality effects, 1024x1024 shadow maps instead
    // of 2048x2048 (read at start-up). The menu also sets render_scale 0.75 when it is turned on.
    bool performance_mode = false;
    // shadow_quality: 0 = no shadows (the shadow map pass is not drawn at all and lookups always say "lit"),
    // 1 = 512x512 maps, 2 = 1024x1024, 3 = 2048x2048 (the game's own; default). performance_mode with the default
    // gives 1024 as before.
    int shadow_quality = 3;
    // low_effects = 1: the engine's low-quality effects path (smoke, fog, glow resolved at lower cost), without the
    // rest of performance_mode.
    bool low_effects = false;
    // texture_mip_skip = 1: big BC textures (512x512 and up, with mip levels) are loaded without their most detailed
    // level: half the width and height, ~75% less data to convert, upload and sample; softer up close.
    int texture_mip_skip = 0;
    // scale_filter: how the picture is enlarged to the screen. 0 = nearest (sharp pixels), 1 = bilinear (default)
    int scale_filter = 1;
    // upscaler: how the 3D picture is enlarged to the HUD image (needs hud_full_res and a render scale under ~83%):
    // 0 = the plain scale_filter blit, 1 = FSR 1 EASU, 2 = EASU + RCAS sharpening (fsr_sharpness 0..100 %).
    int upscaler = 0;
    // skip_fs: fragment-program hashes (hex, separated by commas) whose draws are left out, e.g. a bloom chain (see
    // the "capture pass" lines of a frame capture for the hashes). Their targets are cleared to zero instead.
    std::string skip_fs;
    bool skip_bloom = false;  // leaves out the bloom / glow chain (its fragment programs are added to skip_fs)
    bool skip_post = false;   // experimental: also leaves out the other small full-screen post passes (added to skip_fs)
    // skip_post_extra: more fragment-program hashes (hex, separated by commas) with the skip_post rule: left out only
    // on targets at most half the 3D world's area, never on full-size passes (so they cannot black out the picture).
    // Works without skip_post; the hashes come from the "post census" log line (settings profile 1).
    std::string skip_post_extra;
    // skip_white: fragment-program hashes (hex, separated by commas) left out like skip_post_extra (small targets
    // only; they do not need to be repeated there), but their target is filled with WHITE (1.0) instead of zero.
    // For passes that make a mask the composition multiplies the picture by (ambient occlusion, soft shadows): zero
    // blacks the picture out, white means "nothing occluded". Reads of such a target are served by a 1x1 white image.
    std::string skip_white;
    // skip_post_plus (menu toggle): leaves out 16 more post-processing / low-resolution effect programs, with the
    // skip_post rule (small targets only). Tested on a Mali-G57 (A15) with the picture intact.
    bool skip_post_plus = false;
    // skip_hidden = 1: world draws that covered no pixel in the last frames (found with occlusion queries) are not
    // drawn, and are tried again every few frames. Fewer vertices to shade and tile on a weak GPU (Mali).
    // skip_hidden = 2 (extended) also judges blended draws that are depth tested (effects, needing one more hidden test
    // before they are left out), non-indexed draws, every primitive type and depth targets from 128 pixels wide.
    // skip_hidden = 3 is level 2 without the blink of objects coming into view: a retested hidden draw stays drawn until
    // the result of its query is in (it used to disappear again for 1-3 frames), the results are read as soon as the
    // GPU has finished, and the retests get rarer the longer a draw stays hidden (4, 6, then 8 frames while the view
    // moves; 6, 9, then 12 when it is still). An object that comes into view can appear a few frames late, never twice. Draws that covered a large part of their
    // target (sky, backdrop, fog) are never left out for ~8 s after that, so the background does not come back late.
    int skip_hidden = 0;
    // skip_prepass = 1: the game draws its world twice, a depth-only pass and then the colour pass testing against it
    // (depth func LEQUAL, no depth write). The depth-only draws are left out and the colour draws write the depth
    // themselves: half the vertex work and one render pass less per frame. Learned from the previous frame (a depth
    // buffer with >= 16 depth-only and >= 16 colour draws).
    bool skip_prepass = false;
    int fsr_sharpness = 70;
    // Post-processing at the game's render size, in the HUD path (needs hud_full_res = 1; read every frame):
    // fxaa: 0 = off, 1 = FXAA 3.11 fast (6 steps; for a Mali), 2 = quality (12 steps; from gu-native).
    int fxaa = 0;
    // hud_max_height: 0 = the picture (and HUD) at the screen's size; N (>= 360) = at most N lines tall, enlarged to the
    // screen by the compositor (fewer pixels for FSR / the HUD on a weak GPU).
    int hud_max_height = 0;
    // skip_intro = 1: the company logos at the start are skipped (straight to the title movie).
    bool skip_intro = false;
    int max_anisotropy = 0;  // limit on the game's anisotropic texture filtering (0 = the game's own level)
    bool fast_cores = true;  // keep the game's frame-critical threads off the efficiency cores
    bool record_thread = true;  // record Vulkan commands on a second thread (draw preparation stays on the render thread)
    bool pin_render_thread = false;  // keep the render thread on the fastest CPU core(s) (slower on 8 Gen 2: off)
    float exposure = 1.37f;       // scales the tone-mapping exposure (1.0 = as the game draws it)
};

struct Runtime {
    RuntimeConfig config;
    loader::NsoImage main_nso;
    loader::NsoImage sdk_nso;          // parsed (not mapped) to source data constants such as nn::util tables
    bool have_sdk = false;
    loader::Module main;

    // Guest allocator entry points exported by main (the game replaces malloc & co).
    uintptr_t guest_malloc = 0, guest_free = 0, guest_calloc = 0, guest_realloc = 0, guest_aligned_alloc = 0;
};

Runtime& GetRuntime();

// Loads main (and optionally sdk for data), binds imports. Returns false with *err on failure.
bool LoadGame(const RuntimeConfig& cfg, std::string* err);

// Runs the guest startup sequence (nninitStartup, static constructors, nnMain) on a dedicated
// thread sized like the Switch main thread. Blocks until nnMain returns.
int RunGame();

// Patches engine functions of this game build (log/panic output, frame rate). Called by LoadGame.
void InstallGameHooks();
// The game version the hooks recognised ("1.0.0", "1.0.3"), or null for an unknown build.
const char* GameVersion();
// The game's current per-frame time step in seconds (1/30 unless the 60 fps patches are active).
float GameFrameTime();
// The game's movie player shows one video frame per game frame (30 fps on the Switch): while a video
// plays, frames are paced at 30 instead of the configured rate (at 60 the video ran at double speed and
// stopped every few frames to wait for its sound).
void SetMoviePlaying(bool playing);
int FrameRateNow();  // the rate the frame limiter paces to now (the cap, or the game version's video rate)
// Smoothed GPU time of one submission in ms (0 = not known yet), from the Vulkan backend's GPU timestamps: what the
// dynamic frame rate (fps = auto) weighs against the frame period.
void ReportGpuFrameTime(double ms);
// Whether the GPU samples BC textures itself (natively or through BCeNabler): 1 yes, 0 no (converted on the CPU),
// -1 not known yet (no Vulkan device). The menu turns texture_mip_skip on with performance mode only without BC.
void SetGpuHasBc(bool has);
int GpuHasBc();
bool FrameRateAdjustable();  // the frame rate patches are in: config.target_fps may change while the game runs

// Prints an import binding summary (implemented / unimplemented / data) to the log.
void LogImportReport(bool list_unimplemented);

// Guest heap helpers used by HLE code that must hand memory to the game (strdup, operator new, ...).
void* GuestMalloc(size_t size);
void GuestFree(void* p);
void* GuestAlignedAlloc(size_t align, size_t size);

}  // namespace dsr
