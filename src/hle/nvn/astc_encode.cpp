#include "hle/nvn/astc_encode.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dsr::hle::nvn {

bool IsAstcEncodable(int f) { return f == 65 || f == 66 || f == 69 || f == 70 || f == 68 || f == 72; }

bool IsAstcLaEncodable(int f) { return f == 75; }

int AstcBlockDim(int setting) { return setting == 6 || setting == 8 ? setting : 4; }

size_t AstcLevelBytes(int width, int height, int depth, int block) {
    const int b = AstcBlockDim(block);
    return size_t((std::max(1, width) + b - 1) / b) * size_t((std::max(1, height) + b - 1) / b) *
           size_t(std::max(1, depth)) * 16;
}

namespace {

// ASTC block layout used here (128 bits, bit 0 = least significant bit of byte 0):
//   [0..10]   block mode 0x42: 2D, 4x4 weight grid, one plane, weights of 2 bits (values 0..3)
//   [11..12]  partitions - 1 = 0
//   [13..16]  colour endpoint mode: 8 (RGB direct) or 12 (RGBA direct)
//   [17..]    endpoint values, 8 bits each: r0 r1 g0 g1 b0 b1 (a0 a1), range 256 because 79 bits are free
//   [127..96] the 32 weight bits, in reverse order (weight 0's lowest bit is bit 127)
constexpr uint32_t kBlockMode = 0x42;
constexpr int kWeight[4] = {0, 21, 43, 64};  // the 2-bit weights as the decoder unquantizes them (of 64)

void PutBits(uint8_t* b, int pos, uint32_t v, int n) {
    for (int i = 0; i < n; i++)
        if ((v >> i) & 1) b[(pos + i) >> 3] |= uint8_t(1u << ((pos + i) & 7));
}
uint32_t GetBits(const uint8_t* b, int pos, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v |= uint32_t((b[(pos + i) >> 3] >> ((pos + i) & 7)) & 1) << i;
    return v;
}

// A decoded channel between endpoints e0 and e1 at weight w (0..64), as the specification's LDR decode does.
inline int Lerp(int e0, int e1, int w) {
    const int c = (e0 * 257 * (64 - w) + e1 * 257 * w + 32) >> 6;
    return c >> 8;
}

inline int ClampByte(float v) {
    const int i = int(v + 0.5f);
    return i < 0 ? 0 : i > 255 ? 255 : i;
}

struct Candidate {
    int e0[4], e1[4];
    uint8_t q[16];
    float err;
};

// Assigns every texel its best weight for the (integer) endpoints; returns the squared error.
float Assign(const float p[16][4], int nc, const float cw[4], const int e0[4], const int e1[4], uint8_t q[16]) {
    int dec[4][4];
    for (int k = 0; k < 4; k++)
        for (int c = 0; c < nc; c++) dec[k][c] = Lerp(e0[c], e1[c], kWeight[k]);
    float total = 0;
    for (int i = 0; i < 16; i++) {
        float best = 1e30f;
        int bk = 0;
        for (int k = 0; k < 4; k++) {
            float d = 0;
            for (int c = 0; c < nc; c++) {
                const float e = p[i][c] - float(dec[k][c]);
                d += e * e * cw[c];
            }
            if (d < best) best = d, bk = k;
        }
        q[i] = uint8_t(bk);
        total += best;
    }
    return total;
}

void EncodeBlock(const uint8_t px[16][4], uint8_t out[16]) {
    int amin = 255;
    for (int i = 0; i < 16; i++) amin = std::min<int>(amin, px[i][3]);
    const bool opaque = amin == 255;
    const int nc = opaque ? 3 : 4;
    const float cw[4] = {1, 1, 1, 4};  // alpha counts more: cut-outs must keep their shape

    float p[16][4];
    for (int i = 0; i < 16; i++)
        for (int c = 0; c < 4; c++) p[i][c] = float(px[i][c]);
    if (!opaque) {  // the colour of (nearly) transparent texels is not worth bits: use the mean of the others
        float sum[3] = {0, 0, 0};
        int n = 0;
        for (int i = 0; i < 16; i++)
            if (px[i][3] >= 16) {
                for (int c = 0; c < 3; c++) sum[c] += p[i][c];
                n++;
            }
        if (n > 0 && n < 16)
            for (int i = 0; i < 16; i++)
                if (px[i][3] < 16)
                    for (int c = 0; c < 3; c++) p[i][c] = sum[c] / float(n);
    }

    // Principal axis of the texels (power iteration on the covariance).
    float mean[4] = {0, 0, 0, 0}, lo[4], hi[4];
    for (int c = 0; c < nc; c++) lo[c] = 255, hi[c] = 0;
    for (int i = 0; i < 16; i++)
        for (int c = 0; c < nc; c++) {
            mean[c] += p[i][c] * (1.0f / 16);
            lo[c] = std::min(lo[c], p[i][c]);
            hi[c] = std::max(hi[c], p[i][c]);
        }
    float cov[4][4] = {};
    for (int i = 0; i < 16; i++)
        for (int a = 0; a < nc; a++)
            for (int b = 0; b < nc; b++) cov[a][b] += (p[i][a] - mean[a]) * (p[i][b] - mean[b]);
    float axis[4] = {0, 0, 0, 0};
    for (int c = 0; c < nc; c++) axis[c] = (hi[c] - lo[c]) * (c == 3 ? 2.0f : 1.0f) + 1e-3f;
    for (int it = 0; it < 8; it++) {
        float v[4] = {0, 0, 0, 0}, len = 0;
        for (int a = 0; a < nc; a++)
            for (int b = 0; b < nc; b++) v[a] += cov[a][b] * axis[b];
        for (int c = 0; c < nc; c++) len += v[c] * v[c];
        len = std::sqrt(len);
        if (len < 1e-6f) break;
        for (int c = 0; c < nc; c++) axis[c] = v[c] / len;
    }
    float tmin = 1e30f, tmax = -1e30f;
    for (int i = 0; i < 16; i++) {
        float t = 0;
        for (int c = 0; c < nc; c++) t += (p[i][c] - mean[c]) * axis[c];
        tmin = std::min(tmin, t);
        tmax = std::max(tmax, t);
    }

    float f0[4], f1[4];
    for (int c = 0; c < nc; c++) {
        f0[c] = mean[c] + axis[c] * tmin;
        f1[c] = mean[c] + axis[c] * tmax;
    }

    // Alternate between the best weights for the endpoints and the best endpoints (least squares) for the weights.
    Candidate best;
    best.err = 1e30f;
    for (int c = 0; c < 4; c++) best.e0[c] = best.e1[c] = 255;
    memset(best.q, 0, sizeof(best.q));
    for (int iter = 0; iter < 4; iter++) {
        Candidate cand;
        for (int c = 0; c < 4; c++) cand.e0[c] = cand.e1[c] = 255;
        for (int c = 0; c < nc; c++) cand.e0[c] = ClampByte(f0[c]), cand.e1[c] = ClampByte(f1[c]);
        cand.err = Assign(p, nc, cw, cand.e0, cand.e1, cand.q);
        if (cand.err < best.err) best = cand;
        if (cand.err == 0) break;
        double a00 = 0, a01 = 0, a11 = 0;
        for (int i = 0; i < 16; i++) {
            const double w = kWeight[cand.q[i]] / 64.0;
            a00 += (1 - w) * (1 - w);
            a01 += (1 - w) * w;
            a11 += w * w;
        }
        const double det = a00 * a11 - a01 * a01;
        if (det < 1e-6) break;  // one weight everywhere: nothing to refine
        for (int c = 0; c < nc; c++) {
            double b0 = 0, b1 = 0;
            for (int i = 0; i < 16; i++) {
                const double w = kWeight[cand.q[i]] / 64.0;
                b0 += (1 - w) * p[i][c];
                b1 += w * p[i][c];
            }
            f0[c] = float((a11 * b0 - a01 * b1) / det);
            f1[c] = float((a00 * b1 - a01 * b0) / det);
        }
    }

    // CEM 8 / 12 read the endpoints in swapped, "blue contracted" form when the second one's RGB sum is smaller:
    // keep the first the darker (swapping the weights too; the weights are symmetric).
    if (best.e1[0] + best.e1[1] + best.e1[2] < best.e0[0] + best.e0[1] + best.e0[2]) {
        for (int c = 0; c < 4; c++) std::swap(best.e0[c], best.e1[c]);
        for (int i = 0; i < 16; i++) best.q[i] = uint8_t(3 - best.q[i]);
    }

    memset(out, 0, 16);
    PutBits(out, 0, kBlockMode, 11);
    PutBits(out, 11, 0, 2);
    PutBits(out, 13, opaque ? 8 : 12, 4);
    int pos = 17;
    for (int c = 0; c < nc; c++) {
        PutBits(out, pos, uint32_t(best.e0[c]), 8);
        PutBits(out, pos + 8, uint32_t(best.e1[c]), 8);
        pos += 16;
    }
    for (int i = 0; i < 16; i++)
        for (int b = 0; b < 2; b++)
            if ((best.q[i] >> b) & 1) {
                const int bit = 127 - (i * 2 + b);
                out[bit >> 3] |= uint8_t(1u << (bit & 7));
            }
}

}  // namespace

void EncodeAstc4x4(uint8_t* out, const uint8_t* rgba, int width, int height, int depth) {
    const int bxn = (width + 3) / 4, byn = (height + 3) / 4;
    for (int z = 0; z < std::max(1, depth); z++) {
        const uint8_t* slice = rgba + size_t(z) * size_t(width) * size_t(height) * 4;
        for (int by = 0; by < byn; by++)
            for (int bx = 0; bx < bxn; bx++) {
                uint8_t px[16][4];
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++) {
                        const int sx = std::min(bx * 4 + x, width - 1), sy = std::min(by * 4 + y, height - 1);
                        memcpy(px[y * 4 + x], slice + (size_t(sy) * size_t(width) + size_t(sx)) * 4, 4);
                    }
                EncodeBlock(px, out + ((size_t(z) * byn + by) * bxn + bx) * 16);
            }
    }
}

bool DecodeAstc4x4Block(const uint8_t* b, uint8_t out[16][4]) {
    if (GetBits(b, 0, 11) != kBlockMode || GetBits(b, 11, 2) != 0) return false;
    const uint32_t cem = GetBits(b, 13, 4);
    if (cem != 8 && cem != 12) return false;
    const int n = cem == 8 ? 6 : 8;
    int v[8];
    for (int i = 0; i < n; i++) v[i] = int(GetBits(b, 17 + 8 * i, 8));
    int e0[4] = {v[0], v[2], v[4], 255}, e1[4] = {v[1], v[3], v[5], 255};
    if (n == 8) e0[3] = v[6], e1[3] = v[7];
    if (v[1] + v[3] + v[5] < v[0] + v[2] + v[4]) {  // blue contraction, endpoints exchanged
        const int a0[4] = {(v[1] + v[5]) >> 1, (v[3] + v[5]) >> 1, v[5], n == 8 ? v[7] : 255};
        const int a1[4] = {(v[0] + v[4]) >> 1, (v[2] + v[4]) >> 1, v[4], n == 8 ? v[6] : 255};
        memcpy(e0, a0, sizeof(e0));
        memcpy(e1, a1, sizeof(e1));
    }
    for (int i = 0; i < 16; i++) {
        int q = 0;
        for (int k = 0; k < 2; k++) {
            const int bit = 127 - (i * 2 + k);
            q |= ((b[bit >> 3] >> (bit & 7)) & 1) << k;
        }
        for (int c = 0; c < 4; c++) out[i][c] = uint8_t(Lerp(e0[c], e1[c], kWeight[q]));
    }
    return true;
}

// ---- 6x6 and 8x8 blocks, and two-channel (BC5) textures ------------------------------------------------------------
// The same block layout as above, with a weight grid that need not be as big as the block: the decoder interpolates
// the grid's weights to every texel (ASTC "weight infill": Ds = (1024 + w / 2) / (w - 1), bilinear in 1/16ths).
// Block mode bits written: [4:0] = 0b00010 (2-bit weights), [6:5] = grid height - 2, [8:7] = grid width - 4,
// [10] = dual plane. The decoder takes nw * 2 bits of weights from the top of the block (twice that in dual plane),
// the endpoint values from bit 17 up, and picks the biggest value range that fits below the weights: the grids used
// leave room for 8 bits per value (48 for CEM 8, 64 for CEM 12, 32 for CEM 4), so the ranges are plain bit fields.
namespace {

struct Grid {
    int bw = 4, gw = 4, gh = 4, nw = 16;
    uint32_t mode = kBlockMode;
    uint8_t cnt[40] = {};                       // texels each weight reaches
    uint8_t tx[40][64] = {};                    // ... which
    uint8_t tc[40][64] = {};                    // ... and by how many 1/16ths
    uint8_t idx[64][4] = {}, coef[64][4] = {};  // per texel: the four weights it mixes, and how much of each
};

Grid MakeGrid(int bw, int gw, int gh) {
    Grid g;
    g.bw = bw, g.gw = gw, g.gh = gh, g.nw = gw * gh;
    g.mode = (uint32_t(gw - 4) << 7) | (uint32_t(gh - 2) << 5) | 0x02;
    const int ds = (1024 + bw / 2) / (bw - 1);
    for (int y = 0; y < bw; y++)
        for (int x = 0; x < bw; x++) {
            const int gs = (ds * x * (gw - 1) + 32) >> 6, gt = (ds * y * (gh - 1) + 32) >> 6;
            const int js = gs >> 4, fs = gs & 15, jt = gt >> 4, ft = gt & 15;
            const int v0 = js + gw * jt;
            const int w11 = (fs * ft + 8) >> 4, w10 = ft - w11, w01 = fs - w11, w00 = 16 - fs - ft + w11;
            const int vi[4] = {v0, v0 + 1, v0 + gw, v0 + gw + 1}, wc[4] = {w00, w01, w10, w11};
            const int t = y * bw + x;
            for (int k = 0; k < 4; k++) {
                g.idx[t][k] = uint8_t(std::min(vi[k], g.nw - 1));  // (a weight past the grid has a weight of 0)
                g.coef[t][k] = uint8_t(wc[k]);
                if (wc[k] > 0) {
                    const int w = g.idx[t][k];
                    g.tx[w][g.cnt[w]] = uint8_t(t);
                    g.tc[w][g.cnt[w]] = uint8_t(wc[k]);
                    g.cnt[w]++;
                }
            }
        }
    return g;
}

// Grids: opaque RGB (CEM 8) 6x5 for a 6x6 block, 5x5 for 8x8; RGBA (CEM 12) 4x5; the two planes of BC5 4x4.
const Grid& GridFor(int bw, int kind) {  // kind 0: RGB, 1: RGBA, 2: LA
    static const Grid kG[3][3] = {
        {MakeGrid(4, 4, 4), MakeGrid(6, 6, 5), MakeGrid(8, 5, 5)},
        {MakeGrid(4, 4, 4), MakeGrid(6, 4, 5), MakeGrid(8, 4, 5)},
        {MakeGrid(4, 4, 4), MakeGrid(6, 4, 4), MakeGrid(8, 4, 4)},
    };
    return kG[kind][bw == 6 ? 1 : bw == 8 ? 2 : 0];
}

struct Fit {
    int e0[4], e1[4];
    uint8_t q[40];
    float err;
};

// Weights (0..3 per grid point) and 8-bit endpoints for the texels p (n = bw * bw, nc channels used) on grid g.
void FitBlock(const float p[64][4], int nc, const float cw[4], const Grid& g, Fit& out) {
    const int n = g.bw * g.bw;
    float mean[4] = {0, 0, 0, 0}, lo[4], hi[4];
    for (int c = 0; c < nc; c++) lo[c] = 255, hi[c] = 0;
    for (int i = 0; i < n; i++)
        for (int c = 0; c < nc; c++) {
            mean[c] += p[i][c] * (1.0f / float(n));
            lo[c] = std::min(lo[c], p[i][c]);
            hi[c] = std::max(hi[c], p[i][c]);
        }
    float cov[4][4] = {};
    for (int i = 0; i < n; i++)
        for (int a = 0; a < nc; a++)
            for (int b = 0; b < nc; b++) cov[a][b] += (p[i][a] - mean[a]) * (p[i][b] - mean[b]);
    float axis[4] = {0, 0, 0, 0};
    for (int c = 0; c < nc; c++) axis[c] = (hi[c] - lo[c]) * (c == 3 ? 2.0f : 1.0f) + 1e-3f;
    for (int it = 0; it < 8; it++) {
        float v[4] = {0, 0, 0, 0}, len = 0;
        for (int a = 0; a < nc; a++)
            for (int b = 0; b < nc; b++) v[a] += cov[a][b] * axis[b];
        for (int c = 0; c < nc; c++) len += v[c] * v[c];
        len = std::sqrt(len);
        if (len < 1e-6f) break;
        for (int c = 0; c < nc; c++) axis[c] = v[c] / len;
    }
    float tpos[64], tmin = 1e30f, tmax = -1e30f;
    for (int i = 0; i < n; i++) {
        float t = 0;
        for (int c = 0; c < nc; c++) t += (p[i][c] - mean[c]) * axis[c];
        tpos[i] = t;
        tmin = std::min(tmin, t);
        tmax = std::max(tmax, t);
    }
    float f0[4], f1[4];
    for (int c = 0; c < nc; c++) {
        f0[c] = mean[c] + axis[c] * tmin;
        f1[c] = mean[c] + axis[c] * tmax;
    }
    // First weights: every grid point takes the mean position of the texels around it.
    static const int kLevel[4] = {0, 21, 43, 64};
    uint8_t q[40];
    const float span = tmax - tmin;
    for (int w = 0; w < g.nw; w++) {
        float num = 0, den = 0;
        for (int k = 0; k < g.cnt[w]; k++) {
            const int t = g.tx[w][k];
            num += float(g.tc[w][k]) * (span > 1e-3f ? (tpos[t] - tmin) / span : 0.5f);
            den += float(g.tc[w][k]);
        }
        const float u = den > 0 ? num / den : 0.5f;
        q[w] = uint8_t(std::clamp(int(u * 3.0f + 0.5f), 0, 3));
    }

    out.err = 1e30f;
    for (int c = 0; c < 4; c++) out.e0[c] = out.e1[c] = 255;
    memset(out.q, 0, sizeof(out.q));
    for (int iter = 0; iter < 3; iter++) {
        int e0[4] = {255, 255, 255, 255}, e1[4] = {255, 255, 255, 255};
        for (int c = 0; c < nc; c++) e0[c] = ClampByte(f0[c]), e1[c] = ClampByte(f1[c]);
        float lut[4][65];  // the decoded channel at every interpolated weight
        for (int c = 0; c < nc; c++)
            for (int w = 0; w <= 64; w++) lut[c][w] = float(Lerp(e0[c], e1[c], w));
        auto texel_err = [&](int t, int wt) {
            float d = 0;
            for (int c = 0; c < nc; c++) {
                const float e = p[t][c] - lut[c][wt];
                d += e * e * cw[c];
            }
            return d;
        };
        int sum[64];  // sum of coefficient * weight value per texel (1/16ths of 0..64)
        float terr[64];
        for (int t = 0; t < n; t++) {
            int s = 0;
            for (int k = 0; k < 4; k++) s += int(g.coef[t][k]) * kLevel[q[g.idx[t][k]]];
            sum[t] = s;
            terr[t] = texel_err(t, (s + 8) >> 4);
        }
        // Coordinate descent: each weight takes the level that lowers the error of the texels it reaches.
        for (int pass = 0; pass < 2; pass++) {
            bool changed = false;
            for (int w = 0; w < g.nw; w++) {
                const int cur = q[w];
                float best_delta = 0;
                int best_level = cur;
                for (int L = 0; L < 4; L++) {
                    if (L == cur) continue;
                    float delta = 0;
                    for (int k = 0; k < g.cnt[w]; k++) {
                        const int t = g.tx[w][k];
                        const int s = sum[t] + int(g.tc[w][k]) * (kLevel[L] - kLevel[cur]);
                        delta += texel_err(t, (s + 8) >> 4) - terr[t];
                    }
                    if (delta < best_delta) best_delta = delta, best_level = L;
                }
                if (best_level != cur) {
                    for (int k = 0; k < g.cnt[w]; k++) {
                        const int t = g.tx[w][k];
                        sum[t] += int(g.tc[w][k]) * (kLevel[best_level] - kLevel[cur]);
                        terr[t] = texel_err(t, (sum[t] + 8) >> 4);
                    }
                    q[w] = uint8_t(best_level);
                    changed = true;
                }
            }
            if (!changed) break;
        }
        float err = 0;
        for (int t = 0; t < n; t++) err += terr[t];
        if (err < out.err) {
            out.err = err;
            memcpy(out.e0, e0, sizeof(e0));
            memcpy(out.e1, e1, sizeof(e1));
            memcpy(out.q, q, sizeof(q));
        }
        if (err == 0) break;
        // Best endpoints (least squares) for the texels' interpolated weights.
        double a00 = 0, a01 = 0, a11 = 0;
        double wv[64];
        for (int t = 0; t < n; t++) {
            wv[t] = double((sum[t] + 8) >> 4) / 64.0;
            a00 += (1 - wv[t]) * (1 - wv[t]);
            a01 += (1 - wv[t]) * wv[t];
            a11 += wv[t] * wv[t];
        }
        const double det = a00 * a11 - a01 * a01;
        if (det < 1e-6) break;
        for (int c = 0; c < nc; c++) {
            double b0 = 0, b1 = 0;
            for (int t = 0; t < n; t++) {
                b0 += (1 - wv[t]) * p[t][c];
                b1 += wv[t] * p[t][c];
            }
            f0[c] = float((a11 * b0 - a01 * b1) / det);
            f1[c] = float((a00 * b1 - a01 * b0) / det);
        }
    }
}

void PutWeights(uint8_t* out, const uint8_t* q, int nw, int planes, const uint8_t* q2) {
    for (int i = 0; i < nw; i++)
        for (int pl = 0; pl < planes; pl++) {
            const uint8_t v = pl ? q2[i] : q[i];
            for (int b = 0; b < 2; b++)
                if ((v >> b) & 1) {
                    const int bit = 127 - ((i * planes + pl) * 2 + b);
                    out[bit >> 3] |= uint8_t(1u << (bit & 7));
                }
        }
}

void EncodeBlockGrid(const uint8_t* px, int bw, uint8_t out[16]) {  // px: bw * bw RGBA texels
    const int n = bw * bw;
    int amin = 255;
    for (int i = 0; i < n; i++) amin = std::min<int>(amin, px[i * 4 + 3]);
    const bool opaque = amin == 255;
    const int nc = opaque ? 3 : 4;
    const float cw[4] = {1, 1, 1, 4};
    float p[64][4];
    for (int i = 0; i < n; i++)
        for (int c = 0; c < 4; c++) p[i][c] = float(px[i * 4 + c]);
    if (!opaque) {  // (nearly) transparent texels take the mean colour of the others
        float sum[3] = {0, 0, 0};
        int cnt = 0;
        for (int i = 0; i < n; i++)
            if (px[i * 4 + 3] >= 16) {
                for (int c = 0; c < 3; c++) sum[c] += p[i][c];
                cnt++;
            }
        if (cnt > 0 && cnt < n)
            for (int i = 0; i < n; i++)
                if (px[i * 4 + 3] < 16)
                    for (int c = 0; c < 3; c++) p[i][c] = sum[c] / float(cnt);
    }
    const Grid& g = GridFor(bw, opaque ? 0 : 1);
    Fit f;
    FitBlock(p, nc, cw, g, f);
    // CEM 8 / 12: keep the first endpoint the darker (no blue contraction); the weights are mirrored with them.
    if (f.e1[0] + f.e1[1] + f.e1[2] < f.e0[0] + f.e0[1] + f.e0[2]) {
        for (int c = 0; c < 4; c++) std::swap(f.e0[c], f.e1[c]);
        for (int i = 0; i < g.nw; i++) f.q[i] = uint8_t(3 - f.q[i]);
    }
    memset(out, 0, 16);
    PutBits(out, 0, g.mode, 11);
    PutBits(out, 11, 0, 2);
    PutBits(out, 13, opaque ? 8 : 12, 4);
    int pos = 17;
    for (int c = 0; c < nc; c++) {
        PutBits(out, pos, uint32_t(f.e0[c]), 8);
        PutBits(out, pos + 8, uint32_t(f.e1[c]), 8);
        pos += 16;
    }
    PutWeights(out, f.q, g.nw, 1, nullptr);
}

void EncodeBlockLA(const uint8_t* px, int bw, uint8_t out[16]) {  // px: bw * bw (R, G) texels
    const int n = bw * bw;
    const Grid& g = GridFor(bw, 2);
    const float cw[4] = {1, 0, 0, 0};
    float p[64][4] = {};
    Fit fl, fa;
    for (int i = 0; i < n; i++) p[i][0] = float(px[i * 2]);
    FitBlock(p, 1, cw, g, fl);
    for (int i = 0; i < n; i++) p[i][0] = float(px[i * 2 + 1]);
    FitBlock(p, 1, cw, g, fa);
    memset(out, 0, 16);
    PutBits(out, 0, g.mode | (1u << 10), 11);  // dual plane
    PutBits(out, 11, 0, 2);
    PutBits(out, 13, 4, 4);  // CEM 4: luminance + alpha, direct: L0 L1 A0 A1
    PutBits(out, 17, uint32_t(fl.e0[0]), 8);
    PutBits(out, 25, uint32_t(fl.e1[0]), 8);
    PutBits(out, 33, uint32_t(fa.e0[0]), 8);
    PutBits(out, 41, uint32_t(fa.e1[0]), 8);
    PutBits(out, 128 - g.nw * 4 - 2, 3, 2);  // colour component selector: the second plane is alpha
    PutWeights(out, fl.q, g.nw, 2, fa.q);
}

template <typename F>
void EncodeBlocks(uint8_t* out, const uint8_t* src, int bpp, int width, int height, int depth, int bw, F encode) {
    const int bxn = (width + bw - 1) / bw, byn = (height + bw - 1) / bw;
    uint8_t px[64 * 4];
    for (int z = 0; z < std::max(1, depth); z++) {
        const uint8_t* slice = src + size_t(z) * size_t(width) * size_t(height) * size_t(bpp);
        for (int by = 0; by < byn; by++)
            for (int bx = 0; bx < bxn; bx++) {
                for (int y = 0; y < bw; y++)
                    for (int x = 0; x < bw; x++) {
                        const int sx = std::min(bx * bw + x, width - 1), sy = std::min(by * bw + y, height - 1);
                        memcpy(px + size_t(y * bw + x) * size_t(bpp), slice + (size_t(sy) * size_t(width) + size_t(sx)) * size_t(bpp),
                               size_t(bpp));
                    }
                encode(px, bw, out + ((size_t(z) * size_t(byn) + size_t(by)) * size_t(bxn) + size_t(bx)) * 16);
            }
    }
}

}  // namespace

void EncodeAstc(uint8_t* out, const uint8_t* rgba, int width, int height, int depth, int block) {
    const int bw = AstcBlockDim(block);
    if (bw == 4) {
        EncodeAstc4x4(out, rgba, width, height, depth);
        return;
    }
    EncodeBlocks(out, rgba, 4, width, height, depth, bw, EncodeBlockGrid);
}

void EncodeAstcLA(uint8_t* out, const uint8_t* rg, int width, int height, int depth, int block) {
    EncodeBlocks(out, rg, 2, width, height, depth, AstcBlockDim(block), EncodeBlockLA);
}

bool DecodeAstcBlock(const uint8_t* b, int block, uint8_t out[64][4]) {
    const int bw = AstcBlockDim(block);
    const uint32_t mode = GetBits(b, 0, 11);
    const bool dual = (mode >> 10) & 1;
    if ((mode & 0x21F) != 0x02 || GetBits(b, 11, 2) != 0) return false;  // 2-bit weights, grid layout 00
    const int gw = int((mode >> 7) & 3) + 4, gh = int((mode >> 5) & 3) + 2;
    const uint32_t cem = GetBits(b, 13, 4);
    const int nvals = cem == 4 ? 4 : cem == 8 ? 6 : cem == 12 ? 8 : 0;
    if (!nvals || (dual && cem != 4)) return false;
    int v[8];
    for (int i = 0; i < nvals; i++) v[i] = int(GetBits(b, 17 + 8 * i, 8));
    const int planes = dual ? 2 : 1, nw = gw * gh;
    const int wbits = nw * planes * 2;
    const Grid g = MakeGrid(bw, gw, gh);
    int ccs = 0;
    if (dual) ccs = int(GetBits(b, 128 - wbits - 2, 2));
    uint8_t wq[40][2] = {};
    for (int i = 0; i < nw; i++)
        for (int pl = 0; pl < planes; pl++) {
            int q = 0;
            for (int k = 0; k < 2; k++) {
                const int bit = 127 - ((i * planes + pl) * 2 + k);
                q |= ((b[bit >> 3] >> (bit & 7)) & 1) << k;
            }
            wq[i][pl] = uint8_t(q);
        }
    int e0[4], e1[4];
    if (cem == 4) {
        e0[0] = e0[1] = e0[2] = v[0], e1[0] = e1[1] = e1[2] = v[1], e0[3] = v[2], e1[3] = v[3];
    } else {
        e0[0] = v[0], e1[0] = v[1], e0[1] = v[2], e1[1] = v[3], e0[2] = v[4], e1[2] = v[5];
        e0[3] = e1[3] = 255;
        if (nvals == 8) e0[3] = v[6], e1[3] = v[7];
        if (v[1] + v[3] + v[5] < v[0] + v[2] + v[4]) {  // blue contraction, endpoints exchanged
            const int a0[4] = {(v[1] + v[5]) >> 1, (v[3] + v[5]) >> 1, v[5], nvals == 8 ? v[7] : 255};
            const int a1[4] = {(v[0] + v[4]) >> 1, (v[2] + v[4]) >> 1, v[4], nvals == 8 ? v[6] : 255};
            memcpy(e0, a0, sizeof(e0));
            memcpy(e1, a1, sizeof(e1));
        }
    }
    for (int t = 0; t < bw * bw; t++) {
        int wt[2] = {0, 0};
        for (int pl = 0; pl < planes; pl++) {
            int s = 0;
            for (int k = 0; k < 4; k++) s += int(g.coef[t][k]) * kWeight[wq[g.idx[t][k]][pl]];
            wt[pl] = (s + 8) >> 4;
        }
        for (int c = 0; c < 4; c++) out[t][c] = uint8_t(Lerp(e0[c], e1[c], wt[dual && c == ccs ? 1 : 0]));
    }
    return true;
}

}  // namespace dsr::hle::nvn
