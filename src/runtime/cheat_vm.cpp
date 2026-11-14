#include "runtime/cheat_vm.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dsr::cheats {
namespace {

// One decoded code. Field names follow the format's description; unused fields stay zero.
struct Op {
    uint32_t type = 0;          // 0x0..0x9, 0xA, 0xC0..0xC4, 0xFF0, 0xFF1, 0xFFF
    int width = 0;              // T: 1, 2, 4 or 8 bytes
    int mem = 0;                // M: memory region
    int reg = 0, reg2 = 0, reg3 = 0;
    int cond = 0;               // C: 1 >, 2 >=, 3 <, 4 <=, 5 ==, 6 !=
    int mode = 0;               // per type: load source, offset type, comparison operand, math op, ...
    bool flag = false, flag2 = false;
    uint64_t addr = 0, value = 0;
    bool begins_block = false;  // 1, 8, C0, C4
    bool is_else = false;       // 2: 21000000
    bool loop_start = false;    // 3
};

bool ValidWidth(int w) { return w == 1 || w == 2 || w == 4 || w == 8; }

uint64_t Mask(uint64_t v, int width) {
    switch (width) {
        case 1: return v & 0xFF;
        case 2: return v & 0xFFFF;
        case 4: return v & 0xFFFFFFFFull;
        default: return v;
    }
}

// Decodes the code at `ip` and advances it. False: unknown type, a bad width or the code ends early (`why` set).
bool Decode(const std::vector<uint32_t>& code, size_t* ip, Op* op, std::string* why) {
    *op = Op{};
    const size_t start = *ip;
    auto next = [&](uint32_t* w) {
        if (*ip >= code.size()) return false;
        *w = code[(*ip)++];
        return true;
    };
    auto value_of = [&](int width, uint64_t* v) {  // 1 word, or 2 (high word first) for 8 bytes
        uint32_t hi = 0, lo = 0;
        if (!next(&hi)) return false;
        if (width == 8) {
            if (!next(&lo)) return false;
            *v = uint64_t(hi) << 32 | lo;
        } else {
            *v = hi;
        }
        return true;
    };
    uint32_t w = 0;
    if (!next(&w)) return false;
    auto fail = [&](const char* what) {
        char buf[96];
        snprintf(buf, sizeof(buf), "%s at word %zu (%08X)", what, start + 1, w);
        if (why) *why = buf;
        return false;
    };
    const uint32_t nib = w >> 28;
    op->type = nib;
    if (nib >= 0xC) op->type = w >> 24;
    if (op->type >= 0xF0) op->type = w >> 20;
    const auto n = [w](int shift) { return int((w >> shift) & 0xF); };
    uint32_t x = 0;
    switch (op->type) {
        case 0x0:  // 0TMR00AA AAAAAAAA VVVVVVVV (VVVVVVVV): store a value at region + A + register R
            op->width = n(24), op->mem = n(20), op->reg = n(16);
            if (!next(&x)) return fail("cut off");
            op->addr = uint64_t(w & 0xFF) << 32 | x;
            if (!ValidWidth(op->width)) return fail("bad width");
            if (!value_of(op->width, &op->value)) return fail("cut off");
            return true;
        case 0x1:  // 1TMCXrAA AAAAAAAA VVVVVVVV (VVVVVVVV): if [region + A (+ register r)] C value
            op->width = n(24), op->mem = n(20), op->cond = n(16), op->flag = n(12) != 0, op->reg = n(8);
            if (!next(&x)) return fail("cut off");
            op->addr = uint64_t(w & 0xFF) << 32 | x;
            if (!ValidWidth(op->width) || op->cond < 1 || op->cond > 6) return fail("bad width or condition");
            if (!value_of(op->width, &op->value)) return fail("cut off");
            op->begins_block = true;
            return true;
        case 0x2:  // 20000000 end of a conditional block, 21000000 else
            op->is_else = n(24) == 1;
            return true;
        case 0x3:  // 300R0000 VVVVVVVV loop start (R = count), 310R0000 loop end
            op->loop_start = n(24) == 0;
            op->reg = n(16);
            if (op->loop_start && !next(&x)) return fail("cut off");
            op->value = x;
            return true;
        case 0x4: {  // 400R0000 VVVVVVVV VVVVVVVV: register R = value
            op->reg = n(16);
            uint32_t hi, lo;
            if (!next(&hi) || !next(&lo)) return fail("cut off");
            op->value = uint64_t(hi) << 32 | lo;
            return true;
        }
        case 0x5:  // 5TMRIsAA AAAAAAAA: register R = [source + A]; I: 0 region, 1 register R, 2 register s,
                   // 3 region + register s
            op->width = n(24), op->mem = n(20), op->reg = n(16), op->mode = n(12), op->reg2 = n(8);
            if (!next(&x)) return fail("cut off");
            op->addr = uint64_t(w & 0xFF) << 32 | x;
            if (!ValidWidth(op->width) || op->mode > 3) return fail("bad width or source");
            return true;
        case 0x6: {  // 6T0RIor0 VVVVVVVV VVVVVVVV: [register R (+ register r if o)] = value; I: R += T
            op->width = n(24), op->reg = n(16), op->flag = n(12) != 0, op->flag2 = n(8) != 0, op->reg2 = n(4);
            uint32_t hi, lo;
            if (!next(&hi) || !next(&lo)) return fail("cut off");
            op->value = uint64_t(hi) << 32 | lo;
            if (!ValidWidth(op->width)) return fail("bad width");
            return true;
        }
        case 0x7:  // 7T0RC000 VVVVVVVV: register R = R (C: + - * << >>) value
            op->width = n(24), op->reg = n(16), op->mode = n(12);
            if (!next(&x)) return fail("cut off");
            op->value = x;
            if (!ValidWidth(op->width) || op->mode > 4) return fail("bad width or operation");
            return true;
        case 0x8:  // 8kkkkkkk: if all these buttons are held
            op->value = w & 0x0FFFFFFF;
            op->begins_block = true;
            return true;
        case 0x9:  // 9TCRS0s0 / 9TCRS100 VVVVVVVV (VVVVVVVV): register R = register S (op) register s / value
            op->width = n(24), op->mode = n(20), op->reg = n(16), op->reg2 = n(12), op->flag = n(8) != 0;
            if (!ValidWidth(op->width) || op->mode > 9) return fail("bad width or operation");
            if (op->flag) {
                if (!value_of(op->width, &op->value)) return fail("cut off");
            } else {
                op->reg3 = n(4);
            }
            return true;
        case 0xA:  // ATSRIOxa (aaaaaaaa): [address] = register S; address by O from register R; I: R += T
            op->width = n(24), op->reg2 = n(20), op->reg = n(16), op->flag = n(12) != 0, op->mode = n(8);
            op->reg3 = n(4);  // offset register (O = 1) or region (O = 3, 4, 5)
            op->mem = n(4);
            if (!ValidWidth(op->width) || op->mode > 5) return fail("bad width or address type");
            if (op->mode == 2 || op->mode == 4 || op->mode == 5) {
                if (!next(&x)) return fail("cut off");
                op->addr = uint64_t(w & 0xF) << 32 | x;
            }
            return true;
        case 0xC0:  // C0TcSX..: if register S c operand; X: 0 [region + a], 1 [region + register], 2 [register + a],
                    // 3 [register + register], 4 value, 5 other register
            op->width = n(20), op->cond = n(16), op->reg = n(12), op->mode = n(8);
            if (!ValidWidth(op->width) || op->cond < 1 || op->cond > 6 || op->mode > 5)
                return fail("bad width, condition or operand");
            op->mem = op->reg2 = n(4);
            op->reg3 = n(0);
            if (op->mode == 0 || op->mode == 2) {
                if (!next(&x)) return fail("cut off");
                op->addr = uint64_t(w & 0xF) << 32 | x;
            } else if (op->mode == 4) {
                if (!value_of(op->width, &op->value)) return fail("cut off");
            }
            op->begins_block = true;
            return true;
        case 0xC1:  // C10D0Sx0: x 0: register D = saved S, 1: saved D = register S, 2: saved D = 0, 3: register D = 0
            op->reg = n(16), op->reg2 = n(8), op->mode = n(4);
            if (op->mode > 3) return fail("bad operation");
            return true;
        case 0xC2:  // C2x0MMMM: the same for every register in the mask
            op->mode = n(20);
            op->value = w & 0xFFFF;
            if (op->mode > 3) return fail("bad operation");
            return true;
        case 0xC3:  // C3000XXr: static register XX: read into register r (XX < 0x80) or written from it
            op->mode = int((w >> 4) & 0xFF), op->reg = n(0);
            return true;
        case 0xC4: {  // C4r00000 kkkkkkkk kkkkkkkk: if these buttons are held (r = 0: only when just pressed)
            op->flag = n(20) != 0;
            uint32_t hi, lo;
            if (!next(&hi) || !next(&lo)) return fail("cut off");
            op->value = uint64_t(hi) << 32 | lo;
            op->begins_block = true;
            return true;
        }
        case 0xFF0:  // pause / resume the game: not supported (nothing happens)
        case 0xFF1:
            return true;
        case 0xFFF:  // FFFTIX..: log a value; X: 0 [region + a], 1 [region + register], 2 [register + a],
                     // 3 [register + register], 4 register
            op->width = n(16), op->reg = n(12) /* log id */, op->mode = n(8);
            op->mem = op->reg2 = n(4);
            op->reg3 = n(0);
            if (!ValidWidth(op->width) || op->mode > 4) return fail("bad width or operand");
            if (op->mode == 0 || op->mode == 2) {
                if (!next(&x)) return fail("cut off");
                op->addr = uint64_t(w & 0xF) << 32 | x;
            }
            return true;
        default:
            return fail("unknown code type");
    }
}

bool Compare(uint64_t a, int cond, uint64_t b) {
    switch (cond) {
        case 1: return a > b;
        case 2: return a >= b;
        case 3: return a < b;
        case 4: return a <= b;
        case 5: return a == b;
        default: return a != b;
    }
}

bool IsHex(const std::string& s) {
    if (s.size() != 8) return false;
    for (char c : s)
        if (!isxdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

}  // namespace

int OpenBlocks(const std::vector<uint32_t>& code) {
    size_t ip = 0;
    Op op;
    int depth = 0;
    while (ip < code.size() && Decode(code, &ip, &op, nullptr)) {
        if (op.begins_block) depth++;
        if (op.type == 0x2 && !op.is_else && depth > 0) depth--;
    }
    return depth;
}

std::string Validate(const std::vector<uint32_t>& code) {
    size_t ip = 0;
    Op op;
    std::string why;
    int depth = 0;
    while (ip < code.size()) {
        if (!Decode(code, &ip, &op, &why)) return why;
        if (op.begins_block) depth++;
        if (op.type == 0x2 && !op.is_else) depth--;
        if (depth < 0) return "a block ends (20000000) that was not started";
    }
    return std::string();
}

std::vector<Cheat> Parse(const std::string& text) {
    std::vector<Cheat> out;
    Cheat* cur = nullptr;
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t line_start = pos;
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line_start == 0 && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line = line.substr(3);  // UTF-8 BOM
        const size_t b = line.find_first_not_of(" \t");
        std::string s = b == std::string::npos ? std::string() : line.substr(b, line.find_last_not_of(" \t") - b + 1);
        if (!s.empty() && (s[0] == '[' || s[0] == '{')) {
            if (cur) cur->end = line_start;
            const char close = s[0] == '[' ? ']' : '}';
            const size_t c = s.find(close);
            out.emplace_back();
            cur = &out.back();
            cur->begin = line_start;
            cur->master = s[0] == '{';
            cur->name = s.substr(1, (c == std::string::npos ? s.size() : c) - 1);
            for (char& ch : cur->name)
                if (ch == '\t' || ch == '\r') ch = ' ';
            if (cur->name.find_first_not_of(' ') == std::string::npos) cur->name = cur->master ? "Master code" : "Cheat";
            if (c == std::string::npos) continue;
            s = s.substr(c + 1);  // codes may follow on the same line
        }
        if (!cur) continue;  // before the first header (e.g. a title line)
        if (s.empty() || s[0] == '#' || s[0] == ';' || s.compare(0, 2, "//") == 0) continue;
        size_t i = 0;
        while (i < s.size()) {
            while (i < s.size() && isspace(static_cast<unsigned char>(s[i]))) i++;
            size_t j = i;
            while (j < s.size() && !isspace(static_cast<unsigned char>(s[j]))) j++;
            if (j > i) {
                const std::string word = s.substr(i, j - i);
                if (IsHex(word)) {
                    cur->code.push_back(uint32_t(strtoul(word.c_str(), nullptr, 16)));
                } else if (cur->error.empty()) {
                    cur->error = "\"" + word.substr(0, 24) + "\" is not a code (8 hex digits)";
                }
            }
            i = j;
        }
    }
    if (cur) cur->end = text.size();
    for (Cheat& c : out) {
        c.text = text.substr(c.begin, c.end - c.begin);
        if (c.error.empty() && c.code.empty() && !c.master) c.error = "no codes";
        if (c.error.empty() && c.code.size() > 4096) c.error = "too long";
        if (c.error.empty()) c.error = Validate(c.code);
    }
    return out;
}

bool Vm::Execute(const std::vector<uint32_t>& code, Memory& mem, uint64_t keys, uint64_t prev_keys,
                 size_t max_steps) {
    memset(regs_, 0, sizeof(regs_));
    memset(saved_, 0, sizeof(saved_));
    memset(loop_tops_, 0, sizeof(loop_tops_));
    int depth = 0;
    size_t ip = 0, steps = 0;
    Op op;

    auto region = [&](int m) { return mem.RegionBase(m); };
    auto read = [&](uint64_t a, int width, uint64_t* v) {
        uint64_t r = 0;
        if (!mem.Read(a, &r, size_t(width))) {
            failed_reads_++;
            return false;
        }
        *v = Mask(r, width);  // little-endian: the low bytes
        return true;
    };
    auto write = [&](uint64_t a, int width, uint64_t v) {
        if (!mem.Write(a, &v, size_t(width))) failed_writes_++;
    };
    // Skips to the end of the current block (depth - 1). An if stops at its else (at the same depth) instead.
    auto skip_block = [&](bool is_if) {
        const int want = depth - 1;
        Op s;
        while (depth > want) {
            if (!Decode(code, &ip, &s, nullptr)) return false;
            if (s.begins_block) {
                depth++;
            } else if (s.type == 0x2) {
                if (!s.is_else)
                    depth--;
                else if (is_if && depth - 1 == want)
                    return true;  // continue in the else branch
            }
        }
        return true;
    };

    while (ip < code.size()) {
        if (++steps > max_steps) return false;
        if (!Decode(code, &ip, &op, nullptr)) return false;
        bool cond = true;
        switch (op.type) {
            case 0x0:
                write(region(op.mem) + op.addr + regs_[op.reg], op.width, op.value);
                break;
            case 0x1: {
                uint64_t v = 0;
                const uint64_t a = region(op.mem) + op.addr + (op.flag ? regs_[op.reg] : 0);
                cond = read(a, op.width, &v) && Compare(v, op.cond, Mask(op.value, op.width));
                break;
            }
            case 0x2:
                if (op.is_else) {  // the if branch ran: skip the else branch
                    if (!skip_block(false)) return false;
                } else if (depth > 0) {
                    depth--;
                }
                break;
            case 0x3:
                if (op.loop_start) {
                    regs_[op.reg] = op.value;
                    loop_tops_[op.reg] = ip;
                } else if (regs_[op.reg] > 1) {
                    regs_[op.reg]--;
                    ip = loop_tops_[op.reg];
                } else {
                    regs_[op.reg] = 0;
                }
                break;
            case 0x4:
                regs_[op.reg] = op.value;
                break;
            case 0x5: {
                uint64_t a;
                switch (op.mode) {
                    case 1: a = regs_[op.reg] + op.addr; break;
                    case 2: a = regs_[op.reg2] + op.addr; break;
                    case 3: a = region(op.mem) + op.addr + regs_[op.reg2]; break;
                    default: a = region(op.mem) + op.addr; break;
                }
                uint64_t v = 0;
                regs_[op.reg] = read(a, op.width, &v) ? v : 0;
                break;
            }
            case 0x6:
                write(regs_[op.reg] + (op.flag2 ? regs_[op.reg2] : 0), op.width, op.value);
                if (op.flag) regs_[op.reg] += uint64_t(op.width);
                break;
            case 0x7: {
                uint64_t& r = regs_[op.reg];
                switch (op.mode) {
                    case 0: r += op.value; break;
                    case 1: r -= op.value; break;
                    case 2: r *= op.value; break;
                    case 3: r = op.value >= 64 ? 0 : r << op.value; break;
                    default: r = op.value >= 64 ? 0 : r >> op.value; break;
                }
                r = Mask(r, op.width);
                break;
            }
            case 0x8:
                cond = (keys & op.value) == op.value;
                break;
            case 0x9: {
                const uint64_t a = regs_[op.reg2], b = op.flag ? op.value : regs_[op.reg3];
                uint64_t r = 0;
                switch (op.mode) {
                    case 0: r = a + b; break;
                    case 1: r = a - b; break;
                    case 2: r = a * b; break;
                    case 3: r = b >= 64 ? 0 : a << b; break;
                    case 4: r = b >= 64 ? 0 : a >> b; break;
                    case 5: r = a & b; break;
                    case 6: r = a | b; break;
                    case 7: r = ~a; break;
                    case 8: r = a ^ b; break;
                    default: r = a; break;
                }
                regs_[op.reg] = Mask(r, op.width);
                break;
            }
            case 0xA: {
                const uint64_t base = regs_[op.reg];
                uint64_t a = base;
                switch (op.mode) {
                    case 1: a = base + regs_[op.reg3]; break;
                    case 2: a = base + op.addr; break;
                    case 3: a = region(op.mem) + base; break;
                    case 4: a = region(op.mem) + op.addr; break;
                    case 5: a = region(op.mem) + base + op.addr; break;
                    default: break;
                }
                write(a, op.width, regs_[op.reg2]);
                if (op.flag) regs_[op.reg] += uint64_t(op.width);
                break;
            }
            case 0xC0: {
                const uint64_t src = Mask(regs_[op.reg], op.width);
                uint64_t cmp = 0;
                bool ok = true;
                switch (op.mode) {
                    case 0: ok = read(region(op.mem) + op.addr, op.width, &cmp); break;
                    case 1: ok = read(region(op.mem) + regs_[op.reg3], op.width, &cmp); break;
                    case 2: ok = read(regs_[op.reg2] + op.addr, op.width, &cmp); break;
                    case 3: ok = read(regs_[op.reg2] + regs_[op.reg3], op.width, &cmp); break;
                    case 4: cmp = Mask(op.value, op.width); break;
                    default: cmp = Mask(regs_[op.reg2], op.width); break;
                }
                cond = ok && Compare(src, op.cond, cmp);
                break;
            }
            case 0xC1:
                switch (op.mode) {
                    case 0: regs_[op.reg] = saved_[op.reg2]; break;
                    case 1: saved_[op.reg] = regs_[op.reg2]; break;
                    case 2: saved_[op.reg] = 0; break;
                    default: regs_[op.reg] = 0; break;
                }
                break;
            case 0xC2:
                for (int i = 0; i < 16; i++) {
                    if (!(op.value & (1u << i))) continue;
                    switch (op.mode) {
                        case 0: regs_[i] = saved_[i]; break;
                        case 1: saved_[i] = regs_[i]; break;
                        case 2: saved_[i] = 0; break;
                        default: regs_[i] = 0; break;
                    }
                }
                break;
            case 0xC3:
                if (op.mode < 0x80)
                    regs_[op.reg] = static_regs_[op.mode];
                else
                    static_regs_[op.mode] = regs_[op.reg];
                break;
            case 0xC4:
                cond = (keys & op.value) == op.value && (op.flag || (prev_keys & op.value) != op.value);
                break;
            case 0xFFF: {
                uint64_t v = 0;
                bool ok = true;
                switch (op.mode) {
                    case 0: ok = read(region(op.mem) + op.addr, op.width, &v); break;
                    case 1: ok = read(region(op.mem) + regs_[op.reg3], op.width, &v); break;
                    case 2: ok = read(regs_[op.reg2] + op.addr, op.width, &v); break;
                    case 3: ok = read(regs_[op.reg2] + regs_[op.reg3], op.width, &v); break;
                    default: v = Mask(regs_[op.reg2], op.width); break;
                }
                char buf[64];
                if (ok)
                    snprintf(buf, sizeof(buf), "log %X: 0x%llX", op.reg, (unsigned long long)v);
                else
                    snprintf(buf, sizeof(buf), "log %X: unreadable", op.reg);
                mem.Log(buf);
                break;
            }
            default:  // FF0 / FF1: pause / resume, not supported
                break;
        }
        if (op.begins_block) {
            depth++;
            if (!cond && !skip_block(true)) return false;
        }
    }
    return true;
}

}  // namespace dsr::cheats
