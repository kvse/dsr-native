#pragma once
// Maps a parsed NSO into this process, applies relocations and binds imports to host (HLE) code.

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "loader/nso.h"

namespace dsr::loader {

// What an import was bound to (for diagnostics).
enum class BindKind : uint8_t { HleFunction, Unimplemented, HleData, DummyData, Absolute };

struct ImportBinding {
    uint32_t sym_index;
    std::string name;
    BindKind kind;
    uintptr_t address;   // what the GOT/vtable slot receives
    uint32_t ref_count;  // number of relocations that referenced it
};

// Supplies host implementations for imported symbols. Return 0/nullptr if not available.
struct ImportResolver {
    std::function<void*(const std::string& mangled)> find_function;
    std::function<void*(const std::string& mangled, uint64_t size_hint)> find_data;
    // Called (via assembly) when the guest calls an import that has no host implementation.
    // The thunk passes the index into Module::imports() so the handler can name it.
    void* unimplemented_entry = nullptr;
};

class Module {
public:
    Module() = default;
    ~Module();
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;

    // Reserve, copy, relocate, bind, protect. On failure returns false with *err set.
    bool Load(const NsoImage& nso, const ImportResolver& resolver, std::string* err);

    uintptr_t base() const { return base_; }
    size_t size() const { return size_; }
    const NsoImage& nso() const { return *nso_; }
    uintptr_t Addr(uint64_t image_offset) const { return base_ + image_offset; }
    bool Contains(uintptr_t a) const { return a >= base_ && a < base_ + size_; }
    const std::vector<ImportBinding>& imports() const { return imports_; }
    const ImportBinding* ImportByIndex(size_t i) const { return i < imports_.size() ? &imports_[i] : nullptr; }

    // Exported symbol lookup (image-relative value → absolute).
    uintptr_t Export(const std::string& name) const;

    // Overwrites the start of the guest function at `image_offset` with a jump to `host_fn` (through an
    // x18-restoring thunk), so the host function replaces it entirely. Must be called before guest code
    // runs. The host function must have the guest function's exact AAPCS64 signature.
    bool HookFunction(uint64_t image_offset, void* host_fn, std::string* err);

    // Replaces one instruction in .text, only if it currently is `expected`. Before guest code runs.
    bool PatchInstruction(uint64_t image_offset, uint32_t expected, uint32_t replacement, std::string* err);

    // True if [image_offset, +size) overlaps code changed by HookFunction / PatchInstruction.
    bool Patched(uint64_t image_offset, size_t size) const;
    // Writes into the read-only part of the image (.text / .rodata) while the game runs (cheat codes): the pages are
    // made writable and executable for the copy, then restored. False if outside or the protection cannot change.
    bool WriteReadOnly(uint64_t image_offset, const void* data, size_t size);

    // A zeroed, writable host page mapped right after the image (within ADRP range of all guest code): for
    // variables that patched guest code reads (e.g. the game's time step).
    uintptr_t ScratchPage() const { return base_ + size_; }
    // "adrp xd, page" placed at image offset `pc_offset`, addressing `target` (any address within +-4 GB).
    uint32_t EncodeAdrp(uint64_t pc_offset, int rd, uintptr_t target) const;

private:
    bool MapAndCopy(std::string* err);
    bool Relocate(const ImportResolver& resolver, std::string* err);
    bool Protect(std::string* err);

    const NsoImage* nso_ = nullptr;
    uintptr_t base_ = 0;
    size_t size_ = 0;       // reserved size (page aligned)
    size_t host_page_ = 4096;
    std::vector<ImportBinding> imports_;
    void* thunk_block_ = nullptr;
    size_t thunk_block_size_ = 0;
    std::vector<void*> dummy_data_;
    std::vector<std::pair<uint64_t, uint64_t>> patched_;  // [begin, end) image offsets
};

// Host page size (4K or 16K on Android).
size_t HostPageSize();

}  // namespace dsr::loader
