#pragma once
// Cheat codes in the Atmosphère format (the one Eden, Ryujinx and the Switch's own cheat loader read), written from
// the format's public description:
//
//   [Cheat name]            a cheat, off until turned on
//   0400000A 00123456 ...   its codes: 32-bit hex words, any spacing / line breaks
//   {Master code}           always on, run before the others
//
// Codes run in a small virtual machine 12 times a second: they read and write the game's memory (relative to the
// start of the program, "main", or of the heap), keep values in 16 registers, branch on comparisons and on the
// buttons held, and loop. This file has the parser and the machine; cheats.cpp connects them to the game.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dsr::cheats {

struct Cheat {
    std::string name;
    bool master = false;              // {name}: always on
    std::vector<uint32_t> code;
    std::string error;                // why it cannot be used (bad hex, unknown code type, ...); empty = fine
    std::string text;                 // the block as written (header and code lines)
    size_t begin = 0, end = 0;        // where `text` is in the parsed text (byte offsets), for deleting it
};

// Splits a cheat file into cheats. Lines before the first header and lines starting with '#', ';' or "//" are
// ignored. A cheat with a word that is not 8 hex digits, or whose codes do not decode, gets `error` set.
std::vector<Cheat> Parse(const std::string& text);

// Checks that `code` decodes from start to end (known code types, nothing cut off). Empty = fine.
std::string Validate(const std::vector<uint32_t>& code);
// Conditional blocks `code` leaves open at its end (they need 20000000 each before another cheat's codes).
int OpenBlocks(const std::vector<uint32_t>& code);

// What the machine reads and writes. Regions: 0 = main (the program), 1 = heap, 2 = alias, 3 = ASLR space.
class Memory {
public:
    virtual ~Memory() = default;
    virtual uint64_t RegionBase(int region) = 0;  // 0: not available (addresses then fail Read / Write)
    virtual bool Read(uint64_t addr, void* out, size_t size) = 0;
    virtual bool Write(uint64_t addr, const void* in, size_t size) = 0;
    virtual void Log(const std::string& line) = 0;  // debug-log code type (FFF), failures
};

// Controller bits (nn::hid::NpadButton): A..Down 0-15, left stick directions 16-19 (left, up, right, down), right
// stick 20-23.
class Vm {
public:
    // Runs `code` once from the start: registers cleared (static registers 0x00..0xFF are kept). `keys`: buttons
    // held now, `prev_keys`: at the previous run (for C4 without auto-repeat). Stops after `max_steps` codes (a loop
    // that never ends). Returns false if it stopped early (a decode error or the step limit).
    bool Execute(const std::vector<uint32_t>& code, Memory& mem, uint64_t keys, uint64_t prev_keys,
                 size_t max_steps = 200000);

    uint64_t Register(int i) const { return regs_[i & 15]; }
    uint64_t StaticRegister(int i) const { return static_regs_[i & 0xFF]; }
    void SetStaticRegister(int i, uint64_t v) { static_regs_[i & 0xFF] = v; }
    size_t failed_reads() const { return failed_reads_; }
    size_t failed_writes() const { return failed_writes_; }

private:
    uint64_t regs_[16] = {}, saved_[16] = {}, static_regs_[256] = {};
    size_t loop_tops_[16] = {};
    size_t failed_reads_ = 0, failed_writes_ = 0;
};

}  // namespace dsr::cheats
