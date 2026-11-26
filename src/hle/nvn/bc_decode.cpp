#include "hle/nvn/bc_decode.h"

#include <algorithm>
#include <utility>
#include <cstring>

namespace dsr::hle::nvn {
namespace {

void Rgb565(uint16_t c, int* r, int* g, int* b) {
    *r = ((c >> 11) & 31) * 255 / 31;
    *g = ((c >> 5) & 63) * 255 / 63;
    *b = (c & 31) * 255 / 31;
}

// BC1 colour block into 16 RGBA8 texels. `force4` (BC2/BC3 colour): always the four-colour mode.
void ColorBlock(const uint8_t* b, uint8_t out[16][4], bool force4) {
    const uint16_t c0 = uint16_t(b[0] | b[1] << 8), c1 = uint16_t(b[2] | b[3] << 8);
    int r[4], g[4], bl[4], a[4] = {255, 255, 255, 255};
    Rgb565(c0, &r[0], &g[0], &bl[0]);
    Rgb565(c1, &r[1], &g[1], &bl[1]);
    if (c0 > c1 || force4) {
        r[2] = (2 * r[0] + r[1]) / 3, g[2] = (2 * g[0] + g[1]) / 3, bl[2] = (2 * bl[0] + bl[1]) / 3;
        r[3] = (r[0] + 2 * r[1]) / 3, g[3] = (g[0] + 2 * g[1]) / 3, bl[3] = (bl[0] + 2 * bl[1]) / 3;
    } else {
        r[2] = (r[0] + r[1]) / 2, g[2] = (g[0] + g[1]) / 2, bl[2] = (bl[0] + bl[1]) / 2;
        r[3] = g[3] = bl[3] = 0;
        a[3] = 0;
    }
    const uint32_t idx = uint32_t(b[4]) | uint32_t(b[5]) << 8 | uint32_t(b[6]) << 16 | uint32_t(b[7]) << 24;
    for (int i = 0; i < 16; i++) {
        const int k = (idx >> (2 * i)) & 3;
        out[i][0] = uint8_t(r[k]);
        out[i][1] = uint8_t(g[k]);
        out[i][2] = uint8_t(bl[k]);
        out[i][3] = uint8_t(a[k]);
    }
}

// BC3 alpha / BC4 / BC5 channel block (8 bytes) into 16 values (0..255, or -127..127 when signed).
void ChannelBlock(const uint8_t* b, int out[16], bool is_signed) {
    int e[8];
    if (is_signed) {
        e[0] = std::max<int>(-127, int8_t(b[0]));
        e[1] = std::max<int>(-127, int8_t(b[1]));
    } else {
        e[0] = b[0];
        e[1] = b[1];
    }
    if (e[0] > e[1]) {
        for (int i = 1; i < 7; i++) e[i + 1] = ((7 - i) * e[0] + i * e[1]) / 7;
    } else {
        for (int i = 1; i < 5; i++) e[i + 1] = ((5 - i) * e[0] + i * e[1]) / 5;
        e[6] = is_signed ? -127 : 0;
        e[7] = is_signed ? 127 : 255;
    }
    uint64_t idx = 0;
    for (int i = 0; i < 6; i++) idx |= uint64_t(b[2 + i]) << (8 * i);
    for (int i = 0; i < 16; i++) out[i] = e[(idx >> (3 * i)) & 7];
}

// ---- BC7 and BC6H (software decoding, so these formats work on any driver) ----
// The partition tables and the BC6H bit layouts are the ones of the Direct3D BC7 / BC6H specification.

const uint16_t kP2[64] = {
    0xcccc, 0x8888, 0xeeee, 0xecc8, 0xc880, 0xfeec, 0xfec8, 0xec80, 0xc800, 0xffec,
    0xfe80, 0xe800, 0xffe8, 0xff00, 0xfff0, 0xf000, 0xf710, 0x008e, 0x7100, 0x08ce,
    0x008c, 0x7310, 0x3100, 0x8cce, 0x088c, 0x3110, 0x6666, 0x366c, 0x17e8, 0x0ff0,
    0x718e, 0x399c, 0xaaaa, 0xf0f0, 0x5a5a, 0x33cc, 0x3c3c, 0x55aa, 0x9696, 0xa55a,
    0x73ce, 0x13c8, 0x324c, 0x3bdc, 0x6996, 0xc33c, 0x9966, 0x0660, 0x0272, 0x04e4,
    0x4e40, 0x2720, 0xc936, 0x936c, 0x39c6, 0x639c, 0x9336, 0x9cc6, 0x817e, 0xe718,
    0xccf0, 0x0fcc, 0x7744, 0xee22,
};
const uint32_t kP3[64] = {
    0xaa685050, 0x6a5a5040, 0x5a5a4200, 0x5450a0a8, 0xa5a50000, 0xa0a05050,
    0x5555a0a0, 0x5a5a5050, 0xaa550000, 0xaa555500, 0xaaaa5500, 0x90909090,
    0x94949494, 0xa4a4a4a4, 0xa9a59450, 0x2a0a4250, 0xa5945040, 0x0a425054,
    0xa5a5a500, 0x55a0a0a0, 0xa8a85454, 0x6a6a4040, 0xa4a45000, 0x1a1a0500,
    0x0050a4a4, 0xaaa59090, 0x14696914, 0x69691400, 0xa08585a0, 0xaa821414,
    0x50a4a450, 0x6a5a0200, 0xa9a58000, 0x5090a0a8, 0xa8a09050, 0x24242424,
    0x00aa5500, 0x24924924, 0x24499224, 0x50a50a50, 0x500aa550, 0xaaaa4444,
    0x66660000, 0xa5a0a5a0, 0x50a050a0, 0x69286928, 0x44aaaa44, 0x66666600,
    0xaa444444, 0x54a854a8, 0x95809580, 0x96969600, 0xa85454a8, 0x80959580,
    0xaa141414, 0x96960000, 0xaaaa1414, 0xa05050a0, 0xa0a5a5a0, 0x96000000,
    0x40804080, 0xa9a8a9a8, 0xaaaaaa44, 0x2a4a5254,
};
const uint8_t kAnchor2[64] = {
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
    15, 2, 8, 2, 2, 8, 8, 15, 2, 8, 2, 2, 8, 8, 2, 2,
    15, 15, 6, 8, 2, 8, 15, 15, 2, 8, 2, 2, 2, 15, 15, 6,
    6, 2, 6, 8, 15, 15, 2, 2, 15, 15, 15, 15, 15, 2, 2, 15,
};
const uint8_t kAnchor3a[64] = {
    3, 3, 15, 15, 8, 3, 15, 15, 8, 8, 6, 6, 6, 5, 3, 3,
    3, 3, 8, 15, 3, 3, 6, 10, 5, 8, 8, 6, 8, 5, 15, 15,
    8, 15, 3, 5, 6, 10, 8, 15, 15, 3, 15, 5, 15, 15, 15, 15,
    3, 15, 5, 5, 5, 8, 5, 10, 5, 10, 8, 13, 15, 12, 3, 3,
};
const uint8_t kAnchor3b[64] = {
    15, 8, 8, 3, 15, 15, 3, 8, 15, 15, 15, 15, 15, 15, 15, 8,
    15, 8, 15, 3, 15, 8, 15, 8, 3, 15, 6, 10, 15, 15, 10, 8,
    15, 3, 15, 10, 10, 8, 9, 10, 6, 15, 8, 15, 3, 6, 6, 8,
    15, 3, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3, 15, 15, 8,
};
const uint8_t kW2[4] = {0, 21, 43, 64};
const uint8_t kW3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
const uint8_t kW4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

int Weight(int bits, int idx) { return bits == 2 ? kW2[idx] : bits == 3 ? kW3[idx] : kW4[idx]; }

struct BitReader {
    const uint8_t* d;
    int pos = 0;
    int Get(int n) {
        int v = 0;
        for (int i = 0; i < n; i++, pos++) v |= ((d[pos >> 3] >> (pos & 7)) & 1) << i;
        return v;
    }
};

int SubsetOf(int ns, int partition, int i) {
    if (ns == 2) return (kP2[partition] >> i) & 1;
    if (ns == 3) return (kP3[partition] >> (2 * i)) & 3;
    return 0;
}

bool IsAnchor(int ns, int partition, int i) {
    if (i == 0) return true;
    if (ns == 2) return i == kAnchor2[partition];
    if (ns == 3) return i == kAnchor3a[partition] || i == kAnchor3b[partition];
    return false;
}

struct Bc7Mode {
    int ns, pb, rb, isb, cb, ab;
    int unique_pbits;  // one p-bit per endpoint
    int shared_pbits;  // one p-bit per subset
    int ib, ib2;       // index bits (ib2: the second index set of the modes 4 and 5)
};
const Bc7Mode kBc7[8] = {
    {3, 4, 0, 0, 4, 0, 1, 0, 3, 0}, {2, 6, 0, 0, 6, 0, 0, 1, 3, 0}, {3, 6, 0, 0, 5, 0, 0, 0, 2, 0},
    {2, 6, 0, 0, 7, 0, 1, 0, 2, 0}, {1, 0, 2, 1, 5, 6, 0, 0, 2, 3}, {1, 0, 2, 0, 7, 8, 0, 0, 2, 2},
    {1, 0, 0, 0, 7, 7, 1, 0, 4, 0}, {2, 6, 0, 0, 5, 5, 1, 0, 2, 0},
};

int Expand8(int v, int bits) { return (v << (8 - bits)) | (v >> (2 * bits - 8)); }
int Lerp7(int a, int b, int w) { return (a * (64 - w) + b * w + 32) >> 6; }

void Bc7Block(const uint8_t* src, uint8_t out[16][4]) {
    BitReader br{src};
    int mode = 0;
    while (mode < 8 && !br.Get(1)) mode++;
    if (mode >= 8) {  // reserved mode: transparent black
        memset(out, 0, 64);
        return;
    }
    const Bc7Mode& m = kBc7[mode];
    const int partition = br.Get(m.pb);
    const int rotation = br.Get(m.rb);
    const int index_sel = br.Get(m.isb);
    const int ne = m.ns * 2;
    int ep[6][4];
    for (int c = 0; c < 3; c++)
        for (int i = 0; i < ne; i++) ep[i][c] = br.Get(m.cb);
    for (int i = 0; i < ne; i++) ep[i][3] = m.ab ? br.Get(m.ab) : 255;
    int pbit[6] = {};
    const bool has_p = m.unique_pbits || m.shared_pbits;
    if (m.unique_pbits)
        for (int i = 0; i < ne; i++) pbit[i] = br.Get(1);
    else if (m.shared_pbits)
        for (int s = 0; s < m.ns; s++) pbit[s * 2] = pbit[s * 2 + 1] = br.Get(1);
    const int cbits = m.cb + (has_p ? 1 : 0);
    const int abits = m.ab ? m.ab + (has_p ? 1 : 0) : 0;
    for (int i = 0; i < ne; i++) {
        for (int c = 0; c < 3; c++) ep[i][c] = Expand8(has_p ? (ep[i][c] << 1) | pbit[i] : ep[i][c], cbits);
        if (m.ab) ep[i][3] = Expand8(has_p ? (ep[i][3] << 1) | pbit[i] : ep[i][3], abits);
    }
    int idx1[16], idx2[16];
    for (int i = 0; i < 16; i++) idx1[i] = br.Get(IsAnchor(m.ns, partition, i) ? m.ib - 1 : m.ib);
    if (m.ib2)
        for (int i = 0; i < 16; i++) idx2[i] = br.Get(i == 0 ? m.ib2 - 1 : m.ib2);
    for (int i = 0; i < 16; i++) {
        const int s = SubsetOf(m.ns, partition, i) * 2;
        int cb_bits = m.ib, ci = idx1[i], ab_bits = m.ib, ai = idx1[i];
        if (m.ib2) {
            // modes 4 and 5: colour and alpha have an index set of their own
            if (index_sel) {
                cb_bits = m.ib2; ci = idx2[i]; ab_bits = m.ib; ai = idx1[i];
            } else {
                cb_bits = m.ib; ci = idx1[i]; ab_bits = m.ib2; ai = idx2[i];
            }
        }
        int px[4];
        const int cw = Weight(cb_bits, ci), aw = Weight(ab_bits, ai);
        for (int c = 0; c < 3; c++) px[c] = Lerp7(ep[s][c], ep[s + 1][c], cw);
        px[3] = m.ab ? Lerp7(ep[s][3], ep[s + 1][3], aw) : 255;
        if (rotation) std::swap(px[3], px[rotation - 1]);
        for (int c = 0; c < 4; c++) out[i][c] = uint8_t(px[c]);
    }
}

struct Bc6Mode {
    int ns, transformed, pb, epb, rb, gb, bb;
};
const Bc6Mode kBc6[14] = {
    {2, 1, 5, 10, 5, 5, 5}, {2, 1, 5, 7, 6, 6, 6}, {2, 1, 5, 11, 5, 4, 4}, {2, 1, 5, 11, 4, 5, 4},
    {2, 1, 5, 11, 4, 4, 5}, {2, 1, 5, 9, 5, 5, 5}, {2, 1, 5, 8, 6, 5, 5}, {2, 1, 5, 8, 5, 6, 5},
    {2, 1, 5, 8, 5, 5, 6}, {2, 0, 5, 6, 6, 6, 6}, {1, 0, 0, 10, 10, 10, 10}, {1, 1, 0, 11, 9, 9, 9},
    {1, 1, 0, 12, 8, 8, 8}, {1, 1, 0, 16, 4, 4, 4},
};
// For each mode: the position in the endpoint words (word = value >> 4, bit = value & 15) of each bit after the mode.
const uint8_t kBc6Pack[14][75] = {
    {116, 132, 180, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 48, 49, 50, 51, 52, 164, 112, 113, 114, 115, 64, 65, 66, 67, 68, 176, 160, 161, 162, 163, 80, 81, 82, 83, 84, 177, 128, 129, 130, 131, 96, 97, 98, 99, 100, 178, 144, 145, 146, 147, 148, 179},
    {117, 164, 165, 0, 1, 2, 3, 4, 5, 6, 176, 177, 132, 16, 17, 18, 19, 20, 21, 22, 133, 178, 116, 32, 33, 34, 35, 36, 37, 38, 179, 181, 180, 48, 49, 50, 51, 52, 53, 112, 113, 114, 115, 64, 65, 66, 67, 68, 69, 160, 161, 162, 163, 80, 81, 82, 83, 84, 85, 128, 129, 130, 131, 96, 97, 98, 99, 100, 101, 144, 145, 146, 147, 148, 149},
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 48, 49, 50, 51, 52, 10, 112, 113, 114, 115, 64, 65, 66, 67, 26, 176, 160, 161, 162, 163, 80, 81, 82, 83, 42, 177, 128, 129, 130, 131, 96, 97, 98, 99, 100, 178, 144, 145, 146, 147, 148, 179},
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 48, 49, 50, 51, 10, 164, 112, 113, 114, 115, 64, 65, 66, 67, 68, 26, 160, 161, 162, 163, 80, 81, 82, 83, 42, 177, 128, 129, 130, 131, 96, 97, 98, 99, 176, 178, 144, 145, 146, 147, 116, 179},
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 48, 49, 50, 51, 10, 132, 112, 113, 114, 115, 64, 65, 66, 67, 26, 176, 160, 161, 162, 163, 80, 81, 82, 83, 84, 42, 128, 129, 130, 131, 96, 97, 98, 99, 177, 178, 144, 145, 146, 147, 180, 179},
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 132, 16, 17, 18, 19, 20, 21, 22, 23, 24, 116, 32, 33, 34, 35, 36, 37, 38, 39, 40, 180, 48, 49, 50, 51, 52, 164, 112, 113, 114, 115, 64, 65, 66, 67, 68, 176, 160, 161, 162, 163, 80, 81, 82, 83, 84, 177, 128, 129, 130, 131, 96, 97, 98, 99, 100, 178, 144, 145, 146, 147, 148, 179},
    {0, 1, 2, 3, 4, 5, 6, 7, 164, 132, 16, 17, 18, 19, 20, 21, 22, 23, 178, 116, 32, 33, 34, 35, 36, 37, 38, 39, 179, 180, 48, 49, 50, 51, 52, 53, 112, 113, 114, 115, 64, 65, 66, 67, 68, 176, 160, 161, 162, 163, 80, 81, 82, 83, 84, 177, 128, 129, 130, 131, 96, 97, 98, 99, 100, 101, 144, 145, 146, 147, 148, 149},
    {0, 1, 2, 3, 4, 5, 6, 7, 176, 132, 16, 17, 18, 19, 20, 21, 22, 23, 117, 116, 32, 33, 34, 35, 36, 37, 38, 39, 165, 180, 48, 49, 50, 51, 52, 164, 112, 113, 114, 115, 64, 65, 66, 67, 68, 69, 160, 161, 162, 163, 80, 81, 82, 83, 84, 177, 128, 129, 130, 131, 96, 97, 98, 99, 100, 178, 144, 145, 146, 147, 148, 179},
    {0, 1, 2, 3, 4, 5, 6, 7, 177, 132, 16, 17, 18, 19, 20, 21, 22, 23, 133, 116, 32, 33, 34, 35, 36, 37, 38, 39, 181, 180, 48, 49, 50, 51, 52, 164, 112, 113, 114, 115, 64, 65, 66, 67, 68, 176, 160, 161, 162, 163, 80, 81, 82, 83, 84, 85, 128, 129, 130, 131, 96, 97, 98, 99, 100, 178, 144, 145, 146, 147, 148, 179},
    {0, 1, 2, 3, 4, 5, 164, 176, 177, 132, 16, 17, 18, 19, 20, 21, 117, 133, 178, 116, 32, 33, 34, 35, 36, 37, 165, 179, 181, 180, 48, 49, 50, 51, 52, 53, 112, 113, 114, 115, 64, 65, 66, 67, 68, 69, 160, 161, 162, 163, 80, 81, 82, 83, 84, 85, 128, 129, 130, 131, 96, 97, 98, 99, 100, 101, 144, 145, 146, 147, 148, 149},
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89},
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 48, 49, 50, 51, 52, 53, 54, 55, 56, 10, 64, 65, 66, 67, 68, 69, 70, 71, 72, 26, 80, 81, 82, 83, 84, 85, 86, 87, 88, 42},
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 48, 49, 50, 51, 52, 53, 54, 55, 11, 10, 64, 65, 66, 67, 68, 69, 70, 71, 27, 26, 80, 81, 82, 83, 84, 85, 86, 87, 43, 42},
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 48, 49, 50, 51, 15, 14, 13, 12, 11, 10, 64, 65, 66, 67, 31, 30, 29, 28, 27, 26, 80, 81, 82, 83, 47, 46, 45, 44, 43, 42},
};

int SignExtend(int v, int bits) { return (v & (1 << (bits - 1))) ? v | -(1 << bits) : v; }

int Bc6Unquant(int v, int bits, bool is_signed) {
    if (!is_signed) {
        if (bits >= 15) return v;
        if (v == 0) return 0;
        if (v == (1 << bits) - 1) return 0xffff;
        return ((v << 15) + 0x4000) >> (bits - 1);
    }
    if (bits >= 16) return v;
    const bool neg = v < 0;
    if (neg) v = -v;
    if (v != 0) v = v >= (1 << (bits - 1)) - 1 ? 0x7fff : ((v << 15) + 0x4000) >> (bits - 1);
    return neg ? -v : v;
}

uint16_t Bc6Finish(int v, bool is_signed) {
    if (!is_signed) return uint16_t((v * 31) >> 6);
    return v < 0 ? uint16_t(0x8000 | ((-v * 31) >> 5)) : uint16_t((v * 31) >> 5);
}

// Writes 4 half floats (RGBA, A = 1) per texel.
void Bc6Block(const uint8_t* src, bool is_signed, uint8_t out[16][8]) {
    int mode = src[0] & 0x1f;
    int bit = 5, epbits = 75, ib = 3;
    if ((mode & 3) < 2) {
        mode &= 3;
        bit = 2;
    } else if ((mode & 3) == 2) {
        mode = 2 + (mode >> 2);
        epbits = 72;
    } else {
        mode = 10 + (mode >> 2);
        epbits = 60;
        ib = 4;
    }
    if (mode >= 14) {  // reserved: black
        memset(out, 0, 16 * 8);
        return;
    }
    const Bc6Mode& m = kBc6[mode];
    BitReader br{src, bit};
    int e[12] = {};
    for (int i = 0; i < epbits; i++) {
        const int d = kBc6Pack[mode][i];
        e[d >> 4] |= br.Get(1) << (d & 15);
    }
    const int partition = br.Get(m.pb);
    const int mask = (1 << m.epb) - 1;
    if (is_signed)
        for (int c = 0; c < 3; c++) e[c] = SignExtend(e[c], m.epb);
    if (is_signed || m.transformed)
        for (int i = 3; i < m.ns * 6; i += 3) {
            e[i] = SignExtend(e[i], m.rb);
            e[i + 1] = SignExtend(e[i + 1], m.gb);
            e[i + 2] = SignExtend(e[i + 2], m.bb);
        }
    if (m.transformed)
        for (int i = 3; i < m.ns * 6; i += 3)
            for (int c = 0; c < 3; c++) {
                int v = (e[i + c] + e[c]) & mask;
                e[i + c] = is_signed ? SignExtend(v, m.epb) : v;
            }
    int u[12];
    for (int i = 0; i < m.ns * 6; i++) u[i] = Bc6Unquant(e[i], m.epb, is_signed);
    for (int i = 0; i < 16; i++) {
        const int s = SubsetOf(m.ns, partition, i) * 6;
        const int w = Weight(ib, br.Get(IsAnchor(m.ns, partition, i) ? ib - 1 : ib));
        uint16_t h[4];
        for (int c = 0; c < 3; c++) h[c] = Bc6Finish((u[s + c] * (64 - w) + u[s + 3 + c] * w + 32) >> 6, is_signed);
        h[3] = 0x3c00;
        memcpy(out[i], h, 8);
    }
}

}  // namespace

int BcDecodedBytes(BcKind k) {
    switch (k) {
        case BcKind::BC4U:
        case BcKind::BC4S: return 1;
        case BcKind::BC5U:
        case BcKind::BC5S: return 2;
        case BcKind::BC6H_U:
        case BcKind::BC6H_S: return 8;
        default: return 4;
    }
}

void BcDecode(BcKind k, const uint8_t* blocks, int blocks_x, int blocks_y, uint8_t* out, int width, int height) {
    const int bpp = BcDecodedBytes(k);
    const int block_bytes = (k == BcKind::BC1 || k == BcKind::BC4U || k == BcKind::BC4S) ? 8 : 16;
    const size_t row = size_t(width) * size_t(bpp);
    for (int by = 0; by < blocks_y; by++)
        for (int bx = 0; bx < blocks_x; bx++) {
            const uint8_t* b = blocks + (size_t(by) * size_t(blocks_x) + size_t(bx)) * size_t(block_bytes);
            uint8_t texels[16][8] = {};
            switch (k) {
                case BcKind::BC1: {
                    uint8_t c[16][4];
                    ColorBlock(b, c, false);
                    for (int i = 0; i < 16; i++) memcpy(texels[i], c[i], 4);
                    break;
                }
                case BcKind::BC2: {
                    uint8_t c[16][4];
                    ColorBlock(b + 8, c, true);
                    for (int i = 0; i < 16; i++) {
                        memcpy(texels[i], c[i], 3);
                        const int a4 = (b[i / 2] >> (4 * (i & 1))) & 15;
                        texels[i][3] = uint8_t(a4 * 17);
                    }
                    break;
                }
                case BcKind::BC3: {
                    uint8_t c[16][4];
                    int a[16];
                    ChannelBlock(b, a, false);
                    ColorBlock(b + 8, c, true);
                    for (int i = 0; i < 16; i++) {
                        memcpy(texels[i], c[i], 3);
                        texels[i][3] = uint8_t(a[i]);
                    }
                    break;
                }
                case BcKind::BC4U:
                case BcKind::BC4S: {
                    int v[16];
                    ChannelBlock(b, v, k == BcKind::BC4S);
                    for (int i = 0; i < 16; i++) texels[i][0] = uint8_t(v[i]);
                    break;
                }
                case BcKind::BC5U:
                case BcKind::BC5S: {
                    int x[16], y[16];
                    ChannelBlock(b, x, k == BcKind::BC5S);
                    ChannelBlock(b + 8, y, k == BcKind::BC5S);
                    for (int i = 0; i < 16; i++) {
                        texels[i][0] = uint8_t(x[i]);
                        texels[i][1] = uint8_t(y[i]);
                    }
                    break;
                }
                case BcKind::BC6H_U:
                case BcKind::BC6H_S: Bc6Block(b, k == BcKind::BC6H_S, texels); break;
                default: {  // BC7
                    uint8_t c[16][4];
                    Bc7Block(b, c);
                    for (int i = 0; i < 16; i++) memcpy(texels[i], c[i], 4);
                    break;
                }
            }
            for (int y = 0; y < 4; y++) {
                const int py = by * 4 + y;
                if (py >= height) break;
                for (int x = 0; x < 4; x++) {
                    const int px = bx * 4 + x;
                    if (px >= width) break;
                    memcpy(out + size_t(py) * row + size_t(px) * size_t(bpp), texels[y * 4 + x], size_t(bpp));
                }
            }
        }
}

}  // namespace dsr::hle::nvn
