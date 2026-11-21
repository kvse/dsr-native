#pragma once
// CPU encoder for ASTC 4x4 (LDR) textures, an alternative to ETC2 (etc_encode.h) on GPUs that have no BC support but
// do have ASTC (all Mali GPUs; settings bc_astc). BC textures are decoded to RGBA8 and re-encoded, so they stay
// compressed on the GPU.
//
// Every block is a single-partition block with a 4x4 weight grid of 2-bit weights and 8-bit endpoints:
//  - opaque blocks: colour endpoint mode 8 (RGB direct). BC1 has four colours on a line (1/3 steps), which the four
//    ASTC weights (0, 21, 43, 64 of 64) reproduce to within a level or so.
//  - blocks with alpha: colour endpoint mode 12 (RGBA direct), one set of weights for colour and alpha. Colour of
//    (nearly) transparent texels is ignored by the fit. Lossier than the separate EAC alpha of ETC2 RGBA8 where
//    colour and alpha vary independently inside one block.
// 16 bytes per 4x4 block (8 bits per texel), twice a BC1 texture in ETC2 and the same as BC3.

#include <cstddef>
#include <cstdint>

namespace dsr::hle::nvn {

// NVN BC1 (65, 66, 69, 70) and BC3 (68, 72) formats: the ones converted to ASTC (RGBA).
bool IsAstcEncodable(int nvn_format);
// NVN BC5 UNORM (75): converted to ASTC as luminance + alpha with two weight planes (settings bc5_astc). The
// texture is then sampled through a view that maps (L, L, L, A) back to (R, G, 0, 1).
bool IsAstcLaEncodable(int nvn_format);

// settings astc_block: 4, 6 or 8 (anything else: 4). Square blocks of 128 bits: 8, 3.56 and 2 bits per texel.
int AstcBlockDim(int setting);

// Bytes of a level of width x height (x depth) texels: 16 per block of block x block texels.
size_t AstcLevelBytes(int width, int height, int depth, int block = 4);

// RGBA8 -> ASTC of block x block (4, 6 or 8) texels, slices of width x height texels one after another. 4x4 is
// EncodeAstc4x4. In the bigger blocks the weights are a coarser grid than the texels (the decoder interpolates
// them): opaque blocks use a 6x5 grid (6x6 block) or 5x5 (8x8), blocks with alpha 4x5. 8 bits per endpoint value.
void EncodeAstc(uint8_t* out, const uint8_t* rgba, int width, int height, int depth, int block);

// Two-channel texels (R, G: rows of width * 2 bytes) -> ASTC luminance+alpha blocks, dual plane: R is the
// luminance, G the alpha, each with its own endpoints and its own 4x4 weight grid.
void EncodeAstcLA(uint8_t* out, const uint8_t* rg, int width, int height, int depth, int block);

// Decodes one block of `block` x block texels written by EncodeAstc / EncodeAstcLA (and EncodeAstc4x4) into
// RGBA texels, row-major (y * block + x), for tests: the kinds of block this file writes only (false otherwise).
bool DecodeAstcBlock(const uint8_t* block_bytes, int block, uint8_t out[64][4]);

// Encodes `depth` slices of width x height RGBA8 texels (rows of width * 4 bytes), blocks row by row (each block's
// rows are row-major, as ASTC wants). Edge texels beyond the image are replicated from the nearest one.
void EncodeAstc4x4(uint8_t* out, const uint8_t* rgba, int width, int height, int depth);

// Decodes one block written by EncodeAstc4x4 into 16 RGBA texels, row-major (y * 4 + x): for tests. Other kinds of
// ASTC block are not understood (returns false).
bool DecodeAstc4x4Block(const uint8_t* block, uint8_t out[16][4]);

}  // namespace dsr::hle::nvn
