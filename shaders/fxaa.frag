#version 450
// FXAA 3.11 Quality (Timothy Lottes), edge thresholds 0.0312 / 0.063, no sub-pixel blur: edges are smoothed and textures
// stay sharp. Ported from gu-native's "fxaa.frag" (runtime/nvn/vulkan/shaders.c) with the luma taken from the colour
// (the game's own FXAA pass keeps it in alpha there). FXAA_STEPS 12 = quality, 6 = fast (for a Mali).
// Runs at the game's render size, before the picture is enlarged.
#ifndef FXAA_STEPS
#define FXAA_STEPS 12
#endif
layout(set = 0, binding = 0) uniform sampler2D t;
layout(location = 0) out vec4 o;
layout(push_constant) uniform Push { uint perceptual; } pc;  // 1: linear values (sRGB / float format): luma in gamma 2.0

#if FXAA_STEPS == 12
const float STEP[12] = float[](1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0);
#else
const float STEP[6] = float[](1.0, 1.5, 2.0, 2.0, 4.0, 12.0);
#endif
float LUMA(vec3 c) {
    if (pc.perceptual != 0u) c = sqrt(max(c, vec3(0.0)));
    return dot(c, vec3(0.299, 0.587, 0.114));
}
float luma(vec2 uv) { return LUMA(textureLod(t, uv, 0.0).rgb); }
#define AT(x, y) LUMA(textureLodOffset(t, pos, 0.0, ivec2(x, y)).rgb)
void main() {
    vec2 rcp = 1.0 / vec2(textureSize(t, 0)), pos = gl_FragCoord.xy * rcp;
    vec4 rgbaM = textureLod(t, pos, 0.0);
    float lumaM = LUMA(rgbaM.rgb), lumaN = AT(0, -1), lumaS = AT(0, 1), lumaW = AT(-1, 0), lumaE = AT(1, 0);
    float rangeMax = max(max(max(lumaN, lumaW), max(lumaE, lumaS)), lumaM);
    float range = rangeMax - min(min(min(lumaN, lumaW), min(lumaE, lumaS)), lumaM);
    if (range < max(0.0312, rangeMax * 0.063)) { o = rgbaM; return; }
    float lumaNW = AT(-1, -1), lumaNE = AT(1, -1), lumaSW = AT(-1, 1), lumaSE = AT(1, 1);
    float lumaNS = lumaN + lumaS, lumaWE = lumaW + lumaE;
    float edgeHorz = abs(lumaNW + lumaSW - 2.0 * lumaW) + 2.0 * abs(lumaNS - 2.0 * lumaM) + abs(lumaNE + lumaSE - 2.0 * lumaE);
    float edgeVert = abs(lumaSW + lumaSE - 2.0 * lumaS) + 2.0 * abs(lumaWE - 2.0 * lumaM) + abs(lumaNW + lumaNE - 2.0 * lumaN);
    bool horzSpan = edgeHorz >= edgeVert;
    float lengthSign = horzSpan ? rcp.y : rcp.x;
    if (!horzSpan) { lumaN = lumaW; lumaS = lumaE; }
    float gradientN = lumaN - lumaM, gradientS = lumaS - lumaM;
    bool pairN = abs(gradientN) >= abs(gradientS);
    float gradientScaled = max(abs(gradientN), abs(gradientS)) * 0.25;
    if (pairN) lengthSign = -lengthSign;
    float lumaHalf = ((pairN ? lumaN : lumaS) + lumaM) * 0.5;
    vec2 posB = pos + (horzSpan ? vec2(0.0, lengthSign) : vec2(lengthSign, 0.0)) * 0.5;
    vec2 off = horzSpan ? vec2(rcp.x, 0.0) : vec2(0.0, rcp.y);
    vec2 posN = posB - off * STEP[0], posP = posB + off * STEP[0];
    float endN = luma(posN) - lumaHalf, endP = luma(posP) - lumaHalf;
    bool doneN = abs(endN) >= gradientScaled, doneP = abs(endP) >= gradientScaled;
    for (int i = 1; i < FXAA_STEPS && !(doneN && doneP); i++) {
        if (!doneN) posN -= off * STEP[i];
        if (!doneP) posP += off * STEP[i];
        if (i == FXAA_STEPS - 1) break;  // the last step is taken without a look
        if (!doneN) { endN = luma(posN) - lumaHalf; doneN = abs(endN) >= gradientScaled; }
        if (!doneP) { endP = luma(posP) - lumaHalf; doneP = abs(endP) >= gradientScaled; }
    }
    float dstN = horzSpan ? pos.x - posN.x : pos.y - posN.y, dstP = horzSpan ? posP.x - pos.x : posP.y - pos.y;
    bool goodSpan = ((dstN < dstP ? endN : endP) < 0.0) != (lumaM < lumaHalf);
    float offset = (goodSpan ? 0.5 - min(dstN, dstP) / (dstN + dstP) : 0.0) * lengthSign;
    o = vec4(textureLod(t, pos + (horzSpan ? vec2(0.0, offset) : vec2(offset, 0.0)), 0.0).rgb, rgbaM.a);
}
