#include "loader/module.h"

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#include "arch/arch.h"
#include "common/log.h"
#include "loader/dynamic_thunks.h"
#include "loader/thunks.h"

namespace dsr::loader {

namespace {
constexpr const char* kTag = "loader";
constexpr uint8_t STB_WEAK = 2;

size_t AlignUp(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }
}  // namespace

size_t HostPageSize() {
    long p = sysconf(_SC_PAGESIZE);
    return p > 0 ? size_t(p) : 4096;
}

Module::~Module() {
    if (base_) munmap(reinterpret_cast<void*>(base_), size_ + host_page_);
    if (thunk_block_) munmap(thunk_block_, thunk_block_size_);
    for (void* p : dummy_data_) free(p);
}

uintptr_t Module::Export(const std::string& name) const {
    uint64_t v = nso_->FindExport(name);
    return v ? base_ + v : 0;
}

bool Module::HookFunction(uint64_t off, void* host_fn, std::string* err) {
    if (off % 4 || off + 16 > nso_->text.size) {
        *err = "hook offset outside .text";
        return false;
    }
    void* thunk = MakeDynamicThunk(host_fn, 0);
    // ldr x16, #8 ; br x16 ; .quad thunk   (x16/x17 are free at a function entry)
    uint8_t patch[16];
    const uint32_t ldr = 0x58000050, br = 0xD61F0200;
    const uint64_t target = reinterpret_cast<uint64_t>(thunk);
    memcpy(patch, &ldr, 4);
    memcpy(patch + 4, &br, 4);
    memcpy(patch + 8, &target, 8);
    uintptr_t addr = base_ + off;
    uintptr_t page_lo = addr & ~(host_page_ - 1);
    uintptr_t page_hi = (addr + sizeof(patch) + host_page_ - 1) & ~(host_page_ - 1);
    if (mprotect(reinterpret_cast<void*>(page_lo), page_hi - page_lo, PROT_READ | PROT_WRITE) != 0) {
        *err = "mprotect(RW) for hook failed";
        return false;
    }
    memcpy(reinterpret_cast<void*>(addr), patch, sizeof(patch));
    if (mprotect(reinterpret_cast<void*>(page_lo), page_hi - page_lo, PROT_READ | PROT_EXEC) != 0) {
        *err = "mprotect(RX) after hook failed";
        return false;
    }
    arch::FlushICache(reinterpret_cast<void*>(addr), sizeof(patch));
    patched_.push_back({off, off + sizeof(patch)});
    return true;
}

uint32_t Module::EncodeAdrp(uint64_t pc_offset, int rd, uintptr_t target) const {
    const int64_t pages = int64_t(target >> 12) - int64_t((base_ + pc_offset) >> 12);
    const uint32_t imm = uint32_t(pages) & 0x1FFFFF;
    return 0x90000000u | ((imm & 3u) << 29) | ((imm >> 2) << 5) | uint32_t(rd);
}

bool Module::PatchInstruction(uint64_t off, uint32_t expected, uint32_t replacement, std::string* err) {
    if (off % 4 || off + 4 > nso_->text.size) {
        *err = "patch offset outside .text";
        return false;
    }
    const uintptr_t addr = base_ + off;
    uint32_t current;
    memcpy(&current, reinterpret_cast<const void*>(addr), 4);
    if (current != expected) {
        char buf[96];
        snprintf(buf, sizeof(buf), "instruction is %08x, expected %08x", current, expected);
        *err = buf;
        return false;
    }
    const uintptr_t page_lo = addr & ~(host_page_ - 1);
    if (mprotect(reinterpret_cast<void*>(page_lo), host_page_, PROT_READ | PROT_WRITE) != 0) {
        *err = "mprotect(RW) for patch failed";
        return false;
    }
    memcpy(reinterpret_cast<void*>(addr), &replacement, 4);
    if (mprotect(reinterpret_cast<void*>(page_lo), host_page_, PROT_READ | PROT_EXEC) != 0) {
        *err = "mprotect(RX) after patch failed";
        return false;
    }
    arch::FlushICache(reinterpret_cast<void*>(addr), 4);
    patched_.push_back({off, off + 4});
    return true;
}

bool Module::Patched(uint64_t off, size_t size) const {
    for (const auto& [b, e] : patched_)
        if (off < e && off + size > b) return true;
    return false;
}

bool Module::WriteReadOnly(uint64_t off, const void* data, size_t size) {
    const uint64_t ro_end = nso_->data.mem_offset;  // .text and .rodata come before .data
    if (size == 0 || off >= ro_end || size > ro_end - off) return false;
    const uintptr_t addr = base_ + off;
    const uintptr_t page_lo = addr & ~(host_page_ - 1);
    const uintptr_t page_hi = (addr + size + host_page_ - 1) & ~(host_page_ - 1);
    // Other threads may be running code on these pages: they stay executable throughout.
    if (mprotect(reinterpret_cast<void*>(page_lo), page_hi - page_lo, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
        return false;
    memcpy(reinterpret_cast<void*>(addr), data, size);
    for (uintptr_t p = page_lo; p < page_hi; p += host_page_) {
        const bool text = p - base_ < uint64_t(nso_->text.mem_offset) + nso_->text.size;
        mprotect(reinterpret_cast<void*>(p), host_page_, text ? PROT_READ | PROT_EXEC : PROT_READ);
    }
    arch::FlushICache(reinterpret_cast<void*>(addr), size);
    return true;
}

bool Module::Load(const NsoImage& nso, const ImportResolver& resolver, std::string* err) {
    nso_ = &nso;
    host_page_ = HostPageSize();
    if (!MapAndCopy(err)) return false;
    if (!Relocate(resolver, err)) return false;
    if (!Protect(err)) return false;
    LOGI(kTag, "%s loaded at 0x%lx (size 0x%zx, host page %zu)", nso.name.c_str(), (unsigned long)base_, size_,
         host_page_);
    return true;
}

bool Module::MapAndCopy(std::string* err) {
    size_ = AlignUp(nso_->image_size(), host_page_);
    // One more page after the image: the scratch page (ScratchPage), zeroed and writable.
    void* p = mmap(nullptr, size_ + host_page_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        *err = "mmap of module image failed";
        return false;
    }
    base_ = reinterpret_cast<uintptr_t>(p);
    memcpy(p, nso_->image.data(), nso_->image.size());  // bss stays zero from MAP_ANONYMOUS
    return true;
}

bool Module::Relocate(const ImportResolver& resolver, std::string* err) {
    const auto& syms = nso_->symbols;

    // Pass 1: collect distinct imported symbols that are referenced.
    std::unordered_map<uint32_t, size_t> import_of_sym;
    for (const Reloc& r : nso_->relocs) {
        if (r.type == R_AARCH64_RELATIVE || r.sym == 0) continue;
        if (syms[r.sym].defined()) continue;
        auto [it, inserted] = import_of_sym.emplace(r.sym, imports_.size());
        if (inserted) imports_.push_back({r.sym, syms[r.sym].name, BindKind::Unimplemented, 0, 0});
        imports_[it->second].ref_count++;
    }

    // Pass 2: decide what each import binds to; functions get thunks.
    size_t n_funcs = 0;
    for (auto& imp : imports_)
        if (syms[imp.sym_index].type == STT_FUNC) n_funcs++;
    thunk_block_size_ = AlignUp(std::max<size_t>(1, n_funcs) * kThunkSize, host_page_);
    void* tb = mmap(nullptr, thunk_block_size_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (tb == MAP_FAILED) {
        *err = "mmap of thunk block failed";
        return false;
    }
    thunk_block_ = tb;
    uint8_t* tcur = static_cast<uint8_t*>(tb);
    const uint64_t restore = reinterpret_cast<uint64_t>(arch::RestoreHostX18Entry());

    for (size_t i = 0; i < imports_.size(); i++) {
        ImportBinding& imp = imports_[i];
        const Symbol& s = syms[imp.sym_index];
        if (s.type == STT_FUNC) {
            void* host = resolver.find_function ? resolver.find_function(s.name) : nullptr;
            uint64_t target = reinterpret_cast<uint64_t>(host ? host : resolver.unimplemented_entry);
            imp.kind = host ? BindKind::HleFunction : BindKind::Unimplemented;
            EmitImportThunk(tcur, restore, i, target);
            imp.address = reinterpret_cast<uintptr_t>(tcur);
            tcur += kThunkSize;
        } else {
            void* host = resolver.find_data ? resolver.find_data(s.name, s.size) : nullptr;
            if (host) {
                imp.kind = BindKind::HleData;
                imp.address = reinterpret_cast<uintptr_t>(host);
            } else if (s.bind == STB_WEAK) {
                imp.kind = BindKind::Absolute;
                imp.address = 0;
            } else {
                // Unknown data: give it a private zeroed block so reads do not fault.
                size_t sz = std::max<size_t>(s.size, 0x100);
                void* blk = calloc(1, sz);
                dummy_data_.push_back(blk);
                imp.kind = BindKind::DummyData;
                imp.address = reinterpret_cast<uintptr_t>(blk);
            }
        }
    }
    if (mprotect(thunk_block_, thunk_block_size_, PROT_READ | PROT_EXEC) != 0) {
        *err = "mprotect(RX) of thunk block failed";
        return false;
    }
    arch::FlushICache(thunk_block_, thunk_block_size_);

    // Pass 3: apply relocations.
    const uint64_t limit = nso_->image_size();
    for (const Reloc& r : nso_->relocs) {
        if (r.offset + 8 > limit) {
            *err = "relocation outside module image";
            return false;
        }
        auto* slot = reinterpret_cast<uint64_t*>(base_ + r.offset);
        switch (r.type) {
            case R_AARCH64_RELATIVE:
                *slot = base_ + r.addend;
                break;
            case R_AARCH64_ABS64:
            case R_AARCH64_GLOB_DAT:
            case R_AARCH64_JUMP_SLOT: {
                const Symbol& s = syms[r.sym];
                uint64_t v;
                if (s.defined()) {
                    v = base_ + s.value;
                } else {
                    const ImportBinding& imp = imports_[import_of_sym.at(r.sym)];
                    if (imp.kind == BindKind::Absolute && imp.address == 0) {
                        *slot = 0;
                        break;
                    }
                    v = imp.address;
                }
                *slot = v + (r.type == R_AARCH64_ABS64 || r.type == R_AARCH64_GLOB_DAT ? r.addend : 0);
                break;
            }
            default:
                *err = "unsupported relocation type " + std::to_string(r.type);
                return false;
        }
    }
    return true;
}

bool Module::Protect(std::string* err) {
    // Segment permissions are defined at 4 KiB granularity; on 16 KiB-page devices a host page can
    // straddle two segments, so each host page receives the union of the permissions it needs.
    struct Range { uint64_t start, end; int prot; };
    const Range ranges[] = {
        {nso_->text.mem_offset, uint64_t(nso_->text.mem_offset) + nso_->text.size, PROT_READ | PROT_EXEC},
        {nso_->rodata.mem_offset, uint64_t(nso_->rodata.mem_offset) + nso_->rodata.size, PROT_READ},
        {nso_->data.mem_offset, nso_->image_size(), PROT_READ | PROT_WRITE},
    };
    const size_t npages = size_ / host_page_;
    std::vector<int> prot(npages, PROT_NONE);
    for (const Range& r : ranges) {
        if (r.end <= r.start) continue;
        for (size_t pg = r.start / host_page_; pg <= (r.end - 1) / host_page_ && pg < npages; pg++) prot[pg] |= r.prot;
    }
    size_t rwx_pages = 0;
    for (size_t pg = 0; pg < npages;) {
        size_t run = pg;
        while (run < npages && prot[run] == prot[pg]) run++;
        if ((prot[pg] & PROT_WRITE) && (prot[pg] & PROT_EXEC)) rwx_pages += run - pg;
        if (mprotect(reinterpret_cast<void*>(base_ + pg * host_page_), (run - pg) * host_page_, prot[pg]) != 0) {
            *err = "mprotect of module pages failed";
            return false;
        }
        pg = run;
    }
    if (rwx_pages) LOGW(kTag, "%zu host page(s) needed RWX (segment boundary inside a page)", rwx_pages);
    arch::FlushICache(reinterpret_cast<void*>(base_), nso_->text.size);
    return true;
}

}  // namespace dsr::loader
