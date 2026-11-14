#include "runtime/cheats.h"

#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "common/log.h"
#include "common/platform.h"
#include "hle/input.h"
#include "hle/os/heap.h"
#include "runtime/cheat_vm.h"
#include "runtime/runtime.h"

namespace dsr::cheats {
namespace {
constexpr const char* kTag = "cheats";

struct Entry {
    Cheat cheat;
    std::string file;
    size_t index_in_file = 0;
    bool enabled = false;
};

std::mutex g_mutex;
std::string g_dir, g_build;  // g_build: 16 hex digits, upper case
std::vector<Entry> g_entries;
std::mutex g_program_mutex;
std::shared_ptr<const std::vector<uint32_t>> g_program;  // master codes, then the enabled cheats
std::atomic<bool> g_started{false};

std::string OwnFile() { return g_dir + "/" + g_build + ".txt"; }
std::string EnabledFile() { return g_dir + "/" + g_build + ".enabled"; }

bool ReadFile(const std::string& path, std::string* out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    out->clear();
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && out->size() < (4u << 20)) out->append(buf, n);
    fclose(f);
    return true;
}

bool WriteFile(const std::string& path, const std::string& data) {
    const std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    if (fclose(f) != 0 || !ok || rename(tmp.c_str(), path.c_str()) != 0) {
        remove(tmp.c_str());
        return false;
    }
    return true;
}

std::string Lower(std::string s) {
    for (char& c : s) c = char(tolower(static_cast<unsigned char>(c)));
    return s;
}

// Cheat files for the build below `dir` (up to 5 folders deep), the build's own file first.
void FindFiles(const std::string& dir, int depth, const std::string& want, std::vector<std::string>* out) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    std::vector<std::string> names;
    while (dirent* e = readdir(d))
        if (e->d_name[0] != '.') names.push_back(e->d_name);
    closedir(d);
    std::sort(names.begin(), names.end());
    for (const std::string& n : names) {
        const std::string path = dir + "/" + n;
        struct stat st;
        if (stat(path.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 5) FindFiles(path, depth + 1, want, out);
        } else if (S_ISREG(st.st_mode) && Lower(n) == want) {
            out->push_back(path);
        }
    }
}

std::set<std::string> ReadEnabled() {
    std::set<std::string> names;
    std::string text;
    if (!ReadFile(EnabledFile(), &text)) return names;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(pos, e - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) names.insert(line);
        pos = e + 1;
    }
    return names;
}

void SaveEnabled() {
    std::string text;
    for (const Entry& e : g_entries)
        if (e.enabled && !e.cheat.master) text += e.cheat.name + "\n";
    if (!WriteFile(EnabledFile(), text)) LOGW(kTag, "could not save which cheats are on (%s)", EnabledFile().c_str());
}

// The program run 12 times a second: master codes first (Atmosphère runs them first too), then the enabled cheats,
// each with the conditional blocks it leaves open closed.
void Rebuild() {
    auto prog = std::make_shared<std::vector<uint32_t>>();
    for (int pass = 0; pass < 2; pass++)
        for (const Entry& e : g_entries) {
            if (!e.cheat.error.empty() || e.cheat.master != (pass == 0) || (!e.cheat.master && !e.enabled)) continue;
            prog->insert(prog->end(), e.cheat.code.begin(), e.cheat.code.end());
            for (int i = OpenBlocks(e.cheat.code); i > 0; i--) prog->push_back(0x20000000);
        }
    std::lock_guard<std::mutex> lk(g_program_mutex);
    g_program = std::move(prog);
}

// (Re)reads every cheat file for the build. Under g_mutex.
void Load() {
    const std::set<std::string> on = ReadEnabled();
    std::vector<std::string> files;
    FindFiles(g_dir, 0, Lower(g_build) + ".txt", &files);
    const std::string own = OwnFile();
    std::stable_partition(files.begin(), files.end(), [&](const std::string& f) { return Lower(f) == Lower(own); });
    g_entries.clear();
    for (const std::string& f : files) {
        std::string text;
        if (!ReadFile(f, &text)) continue;
        std::vector<Cheat> cheats = Parse(text);
        for (size_t i = 0; i < cheats.size(); i++) {
            Entry e;
            e.cheat = std::move(cheats[i]);
            e.file = f;
            e.index_in_file = i;
            e.enabled = e.cheat.error.empty() && (e.cheat.master || on.count(e.cheat.name) > 0);
            g_entries.push_back(std::move(e));
        }
    }
    Rebuild();
}

// The game's memory as the cheat machine sees it: main (the whole loaded program) and the heap. Anything else
// fails; writes into instructions our own hooks changed are refused (they would undo them).
class GameMemory final : public Memory {
public:
    uint64_t RegionBase(int region) override {
        if (region == 0) return GetRuntime().main.base();
        if (region == 1) {
            uintptr_t base = 0;
            size_t size = 0;
            hle::os::GuestHeapRange(&base, &size);
            return base;
        }
        if (!alias_logged_) {
            alias_logged_ = true;
            LOGW(kTag, "a cheat uses the %s region, which does not exist here: its reads and writes are skipped",
                 region == 2 ? "alias" : region == 3 ? "ASLR" : "unknown");
        }
        return 0;
    }
    bool Read(uint64_t addr, void* out, size_t size) override {
        if (!Valid(addr, size)) return false;
        memcpy(out, reinterpret_cast<const void*>(uintptr_t(addr)), size);
        return true;
    }
    bool Write(uint64_t addr, const void* in, size_t size) override {
        if (!Valid(addr, size)) return false;
        loader::Module& m = GetRuntime().main;
        if (m.Contains(uintptr_t(addr))) {
            const uint64_t off = addr - m.base();
            if (off < m.nso().data.mem_offset) {  // code or read-only data
                if (memcmp(reinterpret_cast<const void*>(uintptr_t(addr)), in, size) == 0) return true;
                if (m.Patched(off, size)) {
                    if (patched_logged_.insert(off).second && patched_logged_.size() <= 8)
                        LOGW(kTag, "a cheat writes main+0x%llx, which DSR Native already changes (frame rate, intro "
                             "skip, ...): skipped", (unsigned long long)off);
                    return false;
                }
                if (!m.WriteReadOnly(off, in, size)) {
                    if (!protect_logged_) LOGW(kTag, "could not make main+0x%llx writable", (unsigned long long)off);
                    protect_logged_ = true;
                    return false;
                }
                return true;
            }
        }
        memcpy(reinterpret_cast<void*>(uintptr_t(addr)), in, size);
        return true;
    }
    void Log(const std::string& line) override {
        if (logs_++ < 200) LOGI(kTag, "%s", line.c_str());
    }

private:
    static bool Valid(uint64_t addr, size_t size) {
        const loader::Module& m = GetRuntime().main;
        const uint64_t mb = m.base(), me = mb + m.nso().image_size();
        if (addr >= mb && addr < me && size <= me - addr) return true;
        uintptr_t hb = 0;
        size_t hs = 0;
        hle::os::GuestHeapRange(&hb, &hs);
        return hb && addr >= hb && addr < hb + hs && size <= hb + hs - addr;
    }
    bool alias_logged_ = false, protect_logged_ = false;
    std::set<uint64_t> patched_logged_;
    size_t logs_ = 0;
};

// Buttons held now as the cheat format numbers them: the controller and touch buttons, and the stick directions.
uint64_t Keys() {
    const input::PadState p = input::GetPad(nullptr);
    uint64_t k = p.buttons & 0xFFFF;
    constexpr int32_t kHalf = 16384;
    if (p.lx < -kHalf) k |= 1ull << 16;
    if (p.ly > kHalf) k |= 1ull << 17;
    if (p.lx > kHalf) k |= 1ull << 18;
    if (p.ly < -kHalf) k |= 1ull << 19;
    if (p.rx < -kHalf) k |= 1ull << 20;
    if (p.ry > kHalf) k |= 1ull << 21;
    if (p.rx > kHalf) k |= 1ull << 22;
    if (p.ry < -kHalf) k |= 1ull << 23;
    return k;
}

void Run() {
    pthread_setname_np(pthread_self(), "dsr-cheats");
    Vm vm;
    GameMemory mem;
    uint64_t prev = 0;
    size_t reads = 0, writes = 0;
    bool stopped_logged = false;
    auto next = std::chrono::steady_clock::now();
    auto report = next + std::chrono::seconds(10);
    for (;;) {
        next += std::chrono::nanoseconds(1'000'000'000 / 12);  // 12 times a second, as on the Switch
        std::this_thread::sleep_until(next);
        const auto now = std::chrono::steady_clock::now();
        if (now > next + std::chrono::seconds(1)) next = now;
        if (platform::IsPaused()) continue;  // the menu is open or the app is in the background
        std::shared_ptr<const std::vector<uint32_t>> prog;
        {
            std::lock_guard<std::mutex> lk(g_program_mutex);
            prog = g_program;
        }
        if (!prog || prog->empty()) continue;
        const uint64_t keys = Keys();
        if (!vm.Execute(*prog, mem, keys, prev) && !stopped_logged) {
            stopped_logged = true;
            LOGW(kTag, "the cheats did not finish (a loop that does not end?): stopped after 200000 codes");
        }
        prev = keys;
        if (now >= report) {
            report = now + std::chrono::seconds(10);
            if (vm.failed_reads() != reads || vm.failed_writes() != writes)
                LOGI(kTag, "last 10 s: %zu reads and %zu writes outside the game's memory skipped (a pointer not "
                     "set yet, or a cheat for another version)", vm.failed_reads() - reads, vm.failed_writes() - writes);
            reads = vm.failed_reads();
            writes = vm.failed_writes();
        }
    }
}

void LogState(const char* what) {
    size_t on = 0, bad = 0;
    for (const Entry& e : g_entries) {
        on += e.enabled;
        bad += !e.cheat.error.empty();
    }
    LOGI(kTag, "%s: %zu cheats for build %s (%zu on, %zu unusable)", what, g_entries.size(), g_build.c_str(), on, bad);
    for (const Entry& e : g_entries)
        if (!e.cheat.error.empty())
            LOGW(kTag, "  \"%s\" (%s) cannot be used: %s", e.cheat.name.c_str(), e.file.c_str(), e.cheat.error.c_str());
        else if (e.enabled)
            LOGI(kTag, "  on: %s%s", e.cheat.name.c_str(), e.cheat.master ? " (master code)" : "");
}
}  // namespace

void Start(const std::string& dir) {
    if (dir.empty() || g_started.exchange(true)) return;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_dir = dir;
        g_build = GetRuntime().main_nso.build_id_hex.substr(0, 16);
        for (char& c : g_build) c = char(toupper(static_cast<unsigned char>(c)));
        mkdir(dir.c_str(), 0775);
        Load();
        if (g_entries.empty())
            LOGI(kTag, "no cheats for build %s (put them in %s)", g_build.c_str(), OwnFile().c_str());
        else
            LogState("loaded");
    }
    std::thread(Run).detach();
}

std::string List() {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_build.empty()) return std::string();
    std::string out = "build\t" + g_build + "\t" + OwnFile() + "\n";
    for (const Entry& e : g_entries)
        out += std::string(e.enabled ? "1" : "0") + "\t" + (e.cheat.master ? "1" : "0") + "\t" + e.cheat.name + "\t" +
               e.cheat.error + "\n";
    return out;
}

bool SetEnabled(size_t index, bool on) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (index >= g_entries.size()) return false;
    Entry& e = g_entries[index];
    if (!e.cheat.error.empty() || e.cheat.master) return false;
    e.enabled = on;
    // Cheats with the same name (in two files) go on and off together: the saved list is by name.
    for (Entry& o : g_entries)
        if (o.cheat.name == e.cheat.name && o.cheat.error.empty() && !o.cheat.master) o.enabled = on;
    SaveEnabled();
    Rebuild();
    LOGI(kTag, "\"%s\" %s", e.cheat.name.c_str(), on ? "on" : "off");
    return true;
}

std::string Add(const std::string& text) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_build.empty()) return "The game has not started yet.";
    const std::vector<Cheat> cheats = Parse(text);
    if (cheats.empty()) return "No cheats found: each cheat starts with its name in brackets, e.g. [Infinite HP].";
    size_t bad = 0;
    for (const Cheat& c : cheats) bad += !c.error.empty();
    std::string data;
    ReadFile(OwnFile(), &data);
    if (!data.empty() && data.back() != '\n') data += '\n';
    if (!data.empty()) data += '\n';
    for (const Cheat& c : cheats) {
        data += c.text;
        if (data.back() != '\n') data += '\n';
    }
    if (!WriteFile(OwnFile(), data)) return "Could not write " + OwnFile() + ".";
    Load();
    LogState("cheats added");
    return "+" + std::to_string(cheats.size()) + "\t" + std::to_string(bad);
}

bool Remove(size_t index) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (index >= g_entries.size()) return false;
    const Entry e = g_entries[index];
    std::string text;
    if (!ReadFile(e.file, &text)) return false;
    const std::vector<Cheat> cheats = Parse(text);
    if (e.index_in_file >= cheats.size() || cheats[e.index_in_file].name != e.cheat.name) return false;
    const Cheat& c = cheats[e.index_in_file];
    text.erase(c.begin, c.end - c.begin);
    if (!WriteFile(e.file, text)) return false;
    Load();
    LOGI(kTag, "\"%s\" deleted from %s", e.cheat.name.c_str(), e.file.c_str());
    return true;
}

std::string Check(const std::string& text) {
    const std::vector<Cheat> cheats = Parse(text);
    if (cheats.empty()) return "No cheat found: a cheat starts with its name in brackets, e.g. [Infinite HP].";
    for (const Cheat& c : cheats)
        if (!c.error.empty()) return c.name + ": " + c.error;
    return std::string();
}

std::string Text(size_t index) {
    std::lock_guard<std::mutex> lk(g_mutex);
    return index < g_entries.size() ? g_entries[index].cheat.text : std::string();
}

}  // namespace dsr::cheats
