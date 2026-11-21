#include "runtime/runtime.h"

#include <cxxabi.h>
#include <pthread.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>

#include "arch/arch.h"
#include "common/crash.h"
#include "common/log.h"
#include "common/platform.h"
#include "hle/os/os.h"
#include "hle/registry.h"

namespace dsr {

namespace {
constexpr const char* kTag = "runtime";
constexpr size_t kMainThreadStack = 0x100000;  // from main.npdm
constexpr int kMainThreadPriority = 44;
constexpr size_t kHostStackExtra = 512 * 1024;

std::string Demangle(const std::string& s) {
    if (s.rfind("_Z", 0) != 0) return s;
    int status = 0;
    char* d = abi::__cxa_demangle(s.c_str(), nullptr, nullptr, &status);
    if (status != 0 || !d) return s;
    std::string r(d);
    free(d);
    return r;
}

// Data imports that the SDK defines (e.g. nn::util math tables) are copied out of the sdk NSO.
std::map<std::string, std::vector<uint8_t>>& SdkDataCopies() {
    static auto* m = new std::map<std::string, std::vector<uint8_t>>;
    return *m;
}

void* FindSdkData(const std::string& name, uint64_t size_hint) {
    Runtime& rt = GetRuntime();
    if (!rt.have_sdk) return nullptr;
    for (const auto& s : rt.sdk_nso.symbols) {
        if (!s.defined() || s.name != name || s.type != loader::STT_OBJECT) continue;
        uint64_t size = s.size ? s.size : size_hint;
        if (s.value + size > rt.sdk_nso.image.size()) return nullptr;
        auto& copy = SdkDataCopies()[name];
        copy.assign(rt.sdk_nso.image.begin() + s.value, rt.sdk_nso.image.begin() + s.value + size);
        copy.resize(std::max<size_t>(copy.size(), 16));
        LOGD(kTag, "data import %s copied from sdk (%llu bytes)", name.c_str(), (unsigned long long)size);
        return copy.data();
    }
    return nullptr;
}
}  // namespace

Runtime& GetRuntime() {
    static Runtime rt;
    return rt;
}

bool LoadGame(const RuntimeConfig& cfg, std::string* err) {
    Runtime& rt = GetRuntime();
    rt.config = cfg;
    std::vector<uint8_t> file;
    const std::string exefs = cfg.game_dir + "/exefs/";
    if (!loader::ReadFile(exefs + "main", &file)) {
        *err = "cannot read " + exefs + "main";
        return false;
    }
    if (!loader::ParseNso(file, "main", &rt.main_nso, err)) return false;
    LOGI(kTag, "main: build id %.16s..., %zu symbols, %zu relocations", rt.main_nso.build_id_hex.c_str(),
         rt.main_nso.symbols.size(), rt.main_nso.relocs.size());

    if (loader::ReadFile(exefs + "sdk", &file)) {
        std::string sdk_err;
        rt.have_sdk = loader::ParseNso(file, "sdk", &rt.sdk_nso, &sdk_err);
        if (!rt.have_sdk) LOGW(kTag, "sdk present but unparsable: %s", sdk_err.c_str());
    } else {
        LOGW(kTag, "exefs/sdk not found; SDK data constants will be zero-filled");
    }

    loader::ImportResolver resolver;
    resolver.find_function = [](const std::string& n) { return hle::FindFunction(n); };
    resolver.find_data = [](const std::string& n, uint64_t size) -> void* {
        if (void* p = hle::FindData(n)) return p;
        return FindSdkData(n, size);
    };
    resolver.unimplemented_entry = arch::UnimplementedEntry();
    if (!rt.main.Load(rt.main_nso, resolver, err)) return false;
    RegisterCrashModule("main", rt.main.base(), rt.main.size());

    rt.guest_malloc = rt.main.Export("malloc");
    rt.guest_free = rt.main.Export("free");
    rt.guest_calloc = rt.main.Export("calloc");
    rt.guest_realloc = rt.main.Export("realloc");
    rt.guest_aligned_alloc = rt.main.Export("aligned_alloc");
    if (!rt.guest_malloc || !rt.guest_free || !rt.guest_aligned_alloc) {
        *err = "main does not export malloc/free/aligned_alloc";
        return false;
    }
    InstallGameHooks();
    return true;
}

void LogImportReport(bool list_unimplemented) {
    Runtime& rt = GetRuntime();
    size_t counts[5] = {};
    std::map<std::string, std::vector<std::string>> missing;  // namespace -> names
    for (const auto& imp : rt.main.imports()) {
        counts[size_t(imp.kind)]++;
        if (imp.kind == loader::BindKind::Unimplemented || imp.kind == loader::BindKind::DummyData) {
            std::string d = Demangle(imp.name);
            std::string group = d.rfind("nn::", 0) == 0 ? d.substr(0, d.find("::", 4)) : d.substr(0, d.find("::"));
            if (group.size() > 24 || group == d) group = "(other)";
            missing[group].push_back(d);
        }
    }
    LOGI(kTag, "imports: %zu total | %zu HLE functions | %zu unimplemented functions | %zu HLE data | "
               "%zu dummy data | %zu weak-null",
         rt.main.imports().size(), counts[0], counts[1], counts[2], counts[3], counts[4]);
    for (const auto& [group, names] : missing) {
        LOGI(kTag, "  missing %-14s %zu", group.c_str(), names.size());
        if (list_unimplemented)
            for (const auto& n : names) LOGD(kTag, "      %s", n.c_str());
    }
}

void* GuestMalloc(size_t size) {
    return reinterpret_cast<void*>(arch::GuestCall<uintptr_t>(reinterpret_cast<void*>(GetRuntime().guest_malloc), size));
}
void GuestFree(void* p) { arch::GuestCall<void>(reinterpret_cast<void*>(GetRuntime().guest_free), p); }
void* GuestAlignedAlloc(size_t align, size_t size) {
    return reinterpret_cast<void*>(
        arch::GuestCall<uintptr_t>(reinterpret_cast<void*>(GetRuntime().guest_aligned_alloc), align, size));
}

namespace {
int g_exit_code = 0;

void* GameThread(void*) {
    Runtime& rt = GetRuntime();
    hle::os::AdoptMainThread("MainThread", kMainThreadStack, kMainThreadPriority);
    platform::PreferFastCores("game main thread", true);

    const uintptr_t init_startup = rt.main.Export("nninitStartup");
    const uintptr_t init = rt.main.Export("_init");
    const uintptr_t nn_main = rt.main.Export("nnMain");
    if (!nn_main) FATAL(kTag, "main does not export nnMain");

    if (init_startup) {
        LOGI(kTag, "calling nninitStartup @ main+0x%lx", (unsigned long)(init_startup - rt.main.base()));
        arch::GuestCall<void>(reinterpret_cast<void*>(init_startup));
    }
    if (init) {
        LOGI(kTag, "running static constructors (_init)");
        arch::GuestCall<void>(reinterpret_cast<void*>(init));
    }
    LOGI(kTag, "entering nnMain @ main+0x%lx", (unsigned long)(nn_main - rt.main.base()));
    arch::GuestCall<void>(reinterpret_cast<void*>(nn_main));
    LOGI(kTag, "nnMain returned");
    return nullptr;
}
}  // namespace

int RunGame() {
    Runtime& rt = GetRuntime();
    if (rt.config.dry_run || !arch::CanExecuteGuest()) {
        LOGW(kTag, "dry run: guest code is not executed");
        return 0;
    }
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kMainThreadStack + kHostStackExtra);
    pthread_t t;
    if (pthread_create(&t, &attr, GameThread, nullptr) != 0) FATAL(kTag, "cannot create game thread");
    pthread_attr_destroy(&attr);
    pthread_setname_np(t, "MainThread");
    pthread_join(t, nullptr);
    return g_exit_code;
}

}  // namespace dsr
