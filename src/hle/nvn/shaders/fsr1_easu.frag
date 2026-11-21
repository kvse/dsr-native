// FidelityFX FSR 1 (EASU / RCAS), adapted from AMD's FidelityFX SDK 1.1.4 gpu/fsr1/ffx_fsr1.h, MIT license:
// Copyright (C) 2024 Advanced Micro Devices, Inc. See third_party/fsr1/LICENSE.txt. Only the types and helper
// functions the two filters use are defined here (the approximate reciprocals are exact ones).
#version 450
#define FfxFloat32 float
#define FfxFloat32x2 vec2
#define FfxFloat32x3 vec3
#define FfxFloat32x4 vec4
#define FfxUInt32x2 uvec2
#define FfxUInt32x4 uvec4
#define FfxInt32x2 ivec2
#define FfxBoolean bool
#define FFX_TRUE true
#define FFX_FALSE false
#define FFX_PARAMETER_IN in
#define FFX_PARAMETER_OUT out
#define FFX_PARAMETER_INOUT inout
#define ffxAsFloat uintBitsToFloat
#define ffxMin min
#define ffxMin3(a, b, c) min(a, min(b, c))
#define ffxMax3(a, b, c) max(a, max(b, c))
#define ffxSaturate(x) clamp(x, 0.0, 1.0)
#define ffxReciprocal(x) (1.0 / (x))
#define ffxApproximateReciprocal(x) (1.0 / (x))
#define ffxApproximateReciprocalMedium(x) (1.0 / (x))
#define ffxApproximateReciprocalSquareRoot(x) inversesqrt(x)
#define ffxBroadcast2(x) vec2(x)
#define ffxBroadcast3(x) vec3(x)
#define ffxBroadcast4(x) vec4(x)

layout(set = 0, binding = 0) uniform sampler2D uTex;
layout(push_constant) uniform PC { uvec4 con0; uvec4 con1; uvec4 con2; uvec4 con3; } pc;
layout(location = 0) out vec4 outColor;
vec4 FsrEasuRF(vec2 p) { return textureGather(uTex, p, 0); }
vec4 FsrEasuGF(vec2 p) { return textureGather(uTex, p, 1); }
vec4 FsrEasuBF(vec2 p) { return textureGather(uTex, p, 2); }
// Filtering for a given tap for the scalar.
void fsrEasuTapFloat(
    FFX_PARAMETER_INOUT FfxFloat32x3 accumulatedColor,   // Accumulated color, with negative lobe.
    FFX_PARAMETER_INOUT FfxFloat32 accumulatedWeight,    // Accumulated weight.
    FFX_PARAMETER_IN FfxFloat32x2 pixelOffset,           // Pixel offset from resolve position to tap.
    FFX_PARAMETER_IN FfxFloat32x2 gradientDirection,     // Gradient direction.
    FFX_PARAMETER_IN FfxFloat32x2 length,                // Length.
    FFX_PARAMETER_IN FfxFloat32 negativeLobeStrength,    // Negative lobe strength.
    FFX_PARAMETER_IN FfxFloat32 clippingPoint,           // Clipping point.
    FFX_PARAMETER_IN FfxFloat32x3 color)                 // Tap color.
{
    // Rotate offset by direction.
    FfxFloat32x2 rotatedOffset;
    rotatedOffset.x = (pixelOffset.x * (gradientDirection.x)) + (pixelOffset.y * gradientDirection.y);
    rotatedOffset.y = (pixelOffset.x * (-gradientDirection.y)) + (pixelOffset.y * gradientDirection.x);

    // Anisotropy.
    rotatedOffset *= length;

    // Compute distance^2.
    FfxFloat32 distanceSquared = rotatedOffset.x * rotatedOffset.x + rotatedOffset.y * rotatedOffset.y;

    // Limit to the window as at corner, 2 taps can easily be outside.
    distanceSquared = ffxMin(distanceSquared, clippingPoint);

    // Approximation of lancos2 without sin() or rcp(), or sqrt() to get x.
    //  (25/16 * (2/5 * x^2 - 1)^2 - (25/16 - 1)) * (1/4 * x^2 - 1)^2
    //  |_______________________________________|   |_______________|
    //                   base                             window
    // The general form of the 'base' is,
    //  (a*(b*x^2-1)^2-(a-1))
    // Where 'a=1/(2*b-b^2)' and 'b' moves around the negative lobe.
    FfxFloat32 weightB = FfxFloat32(2.0 / 5.0) * distanceSquared + FfxFloat32(-1.0);
    FfxFloat32 weightA = negativeLobeStrength * distanceSquared + FfxFloat32(-1.0);
    weightB *= weightB;
    weightA *= weightA;
    weightB = FfxFloat32(25.0 / 16.0) * weightB + FfxFloat32(-(25.0 / 16.0 - 1.0));
    FfxFloat32 weight = weightB * weightA;

    // Do weighted average.
    accumulatedColor += color * weight;
    accumulatedWeight += weight;
}

// Accumulate direction and length.
void fsrEasuSetFloat(
    FFX_PARAMETER_INOUT FfxFloat32x2 direction,
    FFX_PARAMETER_INOUT FfxFloat32 length,
    FFX_PARAMETER_IN FfxFloat32x2 pp,
    FFX_PARAMETER_IN FfxBoolean biS,
    FFX_PARAMETER_IN FfxBoolean biT,
    FFX_PARAMETER_IN FfxBoolean biU,
    FFX_PARAMETER_IN FfxBoolean biV,
    FFX_PARAMETER_IN FfxFloat32 lA,
    FFX_PARAMETER_IN FfxFloat32 lB,
    FFX_PARAMETER_IN FfxFloat32 lC,
    FFX_PARAMETER_IN FfxFloat32 lD,
    FFX_PARAMETER_IN FfxFloat32 lE)
{
    // Compute bilinear weight, branches factor out as predicates are compiler time immediates.
    //  s t
    //  u v
    FfxFloat32 weight = FfxFloat32(0.0);
    if (biS)
        weight = (FfxFloat32(1.0) - pp.x) * (FfxFloat32(1.0) - pp.y);
    if (biT)
        weight = pp.x * (FfxFloat32(1.0) - pp.y);
    if (biU)
        weight = (FfxFloat32(1.0) - pp.x) * pp.y;
    if (biV)
        weight = pp.x * pp.y;

    // Direction is the '+' diff.
    //    a
    //  b c d
    //    e
    // Then takes magnitude from abs average of both sides of 'c'.
    // Length converts gradient reversal to 0, smoothly to non-reversal at 1, shaped, then adding horz and vert terms.
    FfxFloat32 dc = lD - lC;
    FfxFloat32 cb = lC - lB;
    FfxFloat32 lengthX = max(abs(dc), abs(cb));
    lengthX = ffxApproximateReciprocal(lengthX);
    FfxFloat32 directionX = lD - lB;
    direction.x += directionX * weight;
    lengthX = ffxSaturate(abs(directionX) * lengthX);
    lengthX *= lengthX;
    length += lengthX * weight;

    // Repeat for the y axis.
    FfxFloat32 ec = lE - lC;
    FfxFloat32 ca = lC - lA;
    FfxFloat32 lengthY = max(abs(ec), abs(ca));
    lengthY = ffxApproximateReciprocal(lengthY);
    FfxFloat32 directionY = lE - lA;
    direction.y += directionY * weight;
    lengthY = ffxSaturate(abs(directionY) * lengthY);
    lengthY *= lengthY;
    length += lengthY * weight;
}

/// Apply edge-aware spatial upsampling using 32bit floating point precision calculations.
///
/// @param [out] outPixel               The computed color of a pixel.
/// @param [in]  integerPosition        Integer pixel position within the output.
/// @param [in]  con0                   The first constant value generated by <c><i>ffxFsrPopulateEasuConstants</i></c>.
/// @param [in]  con1                   The second constant value generated by <c><i>ffxFsrPopulateEasuConstants</i></c>.
/// @param [in]  con2                   The third constant value generated by <c><i>ffxFsrPopulateEasuConstants</i></c>.
/// @param [in]  con3                   The fourth constant value generated by <c><i>ffxFsrPopulateEasuConstants</i></c>.
/// 
/// @ingroup FSR
void ffxFsrEasuFloat(
    FFX_PARAMETER_OUT FfxFloat32x3 pix,
    FFX_PARAMETER_IN FfxUInt32x2 ip,
    FFX_PARAMETER_IN FfxUInt32x4 con0,
    FFX_PARAMETER_IN FfxUInt32x4 con1,
    FFX_PARAMETER_IN FfxUInt32x4 con2,
    FFX_PARAMETER_IN FfxUInt32x4 con3)
{
    // Get position of 'f'.
    FfxFloat32x2 pp = FfxFloat32x2(ip) * ffxAsFloat(con0.xy) + ffxAsFloat(con0.zw);
    FfxFloat32x2 fp = floor(pp);
    pp -= fp;

    // 12-tap kernel.
    //    b c
    //  e f g h
    //  i j k l
    //    n o
    // Gather 4 ordering.
    //  a b
    //  r g
    // For packed FP16, need either {rg} or {ab} so using the following setup for gather in all versions,
    //    a b    <- unused (z)
    //    r g
    //  a b a b
    //  r g r g
    //    a b
    //    r g    <- unused (z)
    // Allowing dead-code removal to remove the 'z's.
    FfxFloat32x2 p0 = fp * ffxAsFloat(con1.xy) + ffxAsFloat(con1.zw);

    // These are from p0 to avoid pulling two constants on pre-Navi hardware.
    FfxFloat32x2 p1    = p0 + ffxAsFloat(con2.xy);
    FfxFloat32x2 p2    = p0 + ffxAsFloat(con2.zw);
    FfxFloat32x2 p3    = p0 + ffxAsFloat(con3.xy);
    FfxFloat32x4 bczzR = FsrEasuRF(p0);
    FfxFloat32x4 bczzG = FsrEasuGF(p0);
    FfxFloat32x4 bczzB = FsrEasuBF(p0);
    FfxFloat32x4 ijfeR = FsrEasuRF(p1);
    FfxFloat32x4 ijfeG = FsrEasuGF(p1);
    FfxFloat32x4 ijfeB = FsrEasuBF(p1);
    FfxFloat32x4 klhgR = FsrEasuRF(p2);
    FfxFloat32x4 klhgG = FsrEasuGF(p2);
    FfxFloat32x4 klhgB = FsrEasuBF(p2);
    FfxFloat32x4 zzonR = FsrEasuRF(p3);
    FfxFloat32x4 zzonG = FsrEasuGF(p3);
    FfxFloat32x4 zzonB = FsrEasuBF(p3);

    // Simplest multi-channel approximate luma possible (luma times 2, in 2 FMA/MAD).
    FfxFloat32x4 bczzL = bczzB * ffxBroadcast4(0.5) + (bczzR * ffxBroadcast4(0.5) + bczzG);
    FfxFloat32x4 ijfeL = ijfeB * ffxBroadcast4(0.5) + (ijfeR * ffxBroadcast4(0.5) + ijfeG);
    FfxFloat32x4 klhgL = klhgB * ffxBroadcast4(0.5) + (klhgR * ffxBroadcast4(0.5) + klhgG);
    FfxFloat32x4 zzonL = zzonB * ffxBroadcast4(0.5) + (zzonR * ffxBroadcast4(0.5) + zzonG);

    // Rename.
    FfxFloat32 bL = bczzL.x;
    FfxFloat32 cL = bczzL.y;
    FfxFloat32 iL = ijfeL.x;
    FfxFloat32 jL = ijfeL.y;
    FfxFloat32 fL = ijfeL.z;
    FfxFloat32 eL = ijfeL.w;
    FfxFloat32 kL = klhgL.x;
    FfxFloat32 lL = klhgL.y;
    FfxFloat32 hL = klhgL.z;
    FfxFloat32 gL = klhgL.w;
    FfxFloat32 oL = zzonL.z;
    FfxFloat32 nL = zzonL.w;

    // Accumulate for bilinear interpolation.
    FfxFloat32x2 dir = ffxBroadcast2(0.0);
    FfxFloat32  len = FfxFloat32(0.0);
    fsrEasuSetFloat(dir, len, pp, FFX_TRUE,  FFX_FALSE, FFX_FALSE, FFX_FALSE, bL, eL, fL, gL, jL);
    fsrEasuSetFloat(dir, len, pp, FFX_FALSE, FFX_TRUE,  FFX_FALSE, FFX_FALSE, cL, fL, gL, hL, kL);
    fsrEasuSetFloat(dir, len, pp, FFX_FALSE, FFX_FALSE, FFX_TRUE,  FFX_FALSE, fL, iL, jL, kL, nL);
    fsrEasuSetFloat(dir, len, pp, FFX_FALSE, FFX_FALSE, FFX_FALSE, FFX_TRUE,  gL, jL, kL, lL, oL);

    // Normalize with approximation, and cleanup close to zero.
    FfxFloat32x2 dir2 = dir * dir;
    FfxFloat32 dirR = dir2.x + dir2.y;
    FfxBoolean zro  = dirR < FfxFloat32(1.0 / 32768.0);
    dirR = ffxApproximateReciprocalSquareRoot(dirR);
    dirR = zro ? FfxFloat32(1.0) : dirR;
    dir.x = zro ? FfxFloat32(1.0) : dir.x;
    dir *= ffxBroadcast2(dirR);

    // Transform from {0 to 2} to {0 to 1} range, and shape with square.
    len = len * FfxFloat32(0.5);
    len *= len;

    // Stretch kernel {1.0 vert|horz, to sqrt(2.0) on diagonal}.
    FfxFloat32 stretch = (dir.x * dir.x + dir.y * dir.y) * ffxApproximateReciprocal(max(abs(dir.x), abs(dir.y)));

    // Anisotropic length after rotation,
    //  x := 1.0 lerp to 'stretch' on edges
    //  y := 1.0 lerp to 2x on edges
    FfxFloat32x2 len2 = FfxFloat32x2(FfxFloat32(1.0) + (stretch - FfxFloat32(1.0)) * len, FfxFloat32(1.0) + FfxFloat32(-0.5) * len);

    // Based on the amount of 'edge',
    // the window shifts from +/-{sqrt(2.0) to slightly beyond 2.0}.
    FfxFloat32 lob = FfxFloat32(0.5) + FfxFloat32((1.0 / 4.0 - 0.04) - 0.5) * len;

    // Set distance^2 clipping point to the end of the adjustable window.
    FfxFloat32 clp = ffxApproximateReciprocal(lob);

    // Accumulation mixed with min/max of 4 nearest.
    //    b c
    //  e f g h
    //  i j k l
    //    n o
    FfxFloat32x3 min4 =
        ffxMin(ffxMin3(FfxFloat32x3(ijfeR.z, ijfeG.z, ijfeB.z), FfxFloat32x3(klhgR.w, klhgG.w, klhgB.w), FfxFloat32x3(ijfeR.y, ijfeG.y, ijfeB.y)),
               FfxFloat32x3(klhgR.x, klhgG.x, klhgB.x));
    FfxFloat32x3 max4 =
        max(ffxMax3(FfxFloat32x3(ijfeR.z, ijfeG.z, ijfeB.z), FfxFloat32x3(klhgR.w, klhgG.w, klhgB.w), FfxFloat32x3(ijfeR.y, ijfeG.y, ijfeB.y)), FfxFloat32x3(klhgR.x, klhgG.x, klhgB.x));

    // Accumulation.
    FfxFloat32x3 aC = ffxBroadcast3(0.0);
    FfxFloat32  aW = FfxFloat32(0.0);
    fsrEasuTapFloat(aC, aW, FfxFloat32x2(0.0, -1.0) - pp, dir, len2, lob, clp, FfxFloat32x3(bczzR.x, bczzG.x, bczzB.x));  // b
    fsrEasuTapFloat(aC, aW, FfxFloat32x2(1.0, -1.0) - pp, dir, len2, lob, clp, FfxFloat32x3(bczzR.y, bczzG.y, bczzB.y));  // c
    fsrEasuTapFloat(aC, aW, FfxFloat32x2(-1.0, 1.0) - pp, dir, len2, lob, clp, FfxFloat32x3(ijfeR.x, ijfeG.x, ijfeB.x));  // i
    fsrEasuTapFloat(aC, aW, FfxFloat32x2(0.0, 1.0) - pp, dir, len2, lob, clp, FfxFloat32x3(ijfeR.y, ijfeG.y, ijfeB.y));   // j
    fsrEasuTapFloat(aC, aW, FfxFloat32x2(0.0, 0.0) - pp, dir, len2, lob, clp, FfxFloat32x3(ijfeR.z, ijfeG.z, ijfeB.z));   // f
    fsrEasuTapFloat(aC, aW, FfxFloat32x2(-1.0, 0.0) - pp, dir, len2, lob, clp, FfxFloat32x3(ijfeR.w, ijfeG.w, ijfeB.w));  // e
    fsrEasuTapFloat(aC, aW, FfxFloat32x2(1.0, 1.0) - pp, dir, len2, lob, clp, FfxFloat32x3(klhgR.x, klhgG.x, klhgB.x));   // k
    fsrEasuTapFloat(aC, aW, FfxFloat32x2(2.0, 1.0) - pp, dir, len2, lob, clp, FfxFloat32x3(klhgR.y, klhgG.y, klhgB.y));   // l
    fsrEasuTapFloat(aC, aW, FfxFloat32x2(2.0, 0.0) - pp, dir, len2, lob, clp, FfxFloat32x3(klhgR.z, klhgG.z, klhgB.z));   // h
    fsrEasuTapFloat(aC, aW, FfxFloat32x2(1.0, 0.0) - pp, dir, len2, lob, clp, FfxFloat32x3(klhgR.w, klhgG.w, klhgB.w));   // g
    fsrEasuTapFloat(aC, aW, FfxFloat32x2(1.0, 2.0) - pp, dir, len2, lob, clp, FfxFloat32x3(zzonR.z, zzonG.z, zzonB.z));   // o
    fsrEasuTapFloat(aC, aW, FfxFloat32x2(0.0, 2.0) - pp, dir, len2, lob, clp, FfxFloat32x3(zzonR.w, zzonG.w, zzonB.w));   // n

    // Normalize and dering.
    pix = ffxMin(max4, max(min4, aC * ffxBroadcast3(ffxReciprocal(aW))));
}

void main() {
    vec3 c;
    ffxFsrEasuFloat(c, uvec2(gl_FragCoord.xy), pc.con0, pc.con1, pc.con2, pc.con3);
    outColor = vec4(c, 1.0);
}
