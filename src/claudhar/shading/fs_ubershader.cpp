// Copyright 2026 Claudhar Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

// Generates the PICA fragment ubershader and fills its configuration buffer; see
// fs_ubershader.h. The GLSL body below mirrors, function by function, Azahar's
// generators in video_core/shader/generator (spv_fs_shader_gen.cpp and
// glsl_fs_shader_gen.cpp): each place where they emit code for a configuration value, the
// ubershader branches on that value instead. Field positions in the configuration buffer
// are not written by hand: they are generated from the C++ structures with offsetof and
// the BitField position constants, so they cannot drift from Azahar's layout.

#include <cstddef>
#include <cstring>
#include <fmt/format.h>

#include "claudhar/shading/fs_ubershader.h"
#include "video_core/shader/generator/glsl_fs_shader_gen.h"

namespace Claudhar::Shading {

namespace {

using Pica::LightingRegs;
using Pica::Shader::FramebufferConfig;
using Pica::Shader::FSConfig;
using Pica::Shader::Light;
using Pica::Shader::LightConfig;
using Pica::Shader::LutConfig;
using Pica::Shader::ProcTexConfig;
using Pica::Shader::Profile;
using Pica::Shader::TextureBorder;
using Pica::Shader::TextureConfig;
using TevStageConfig = Pica::TexturingRegs::TevStageConfig;
using TextureType = Pica::TexturingRegs::TextureConfig::TextureType;

// Byte offsets of the parts of the configuration buffer.
constexpr std::size_t FB = offsetof(FSConfig, framebuffer) + offsetof(FramebufferConfig, raw);
constexpr std::size_t TEX = offsetof(FSConfig, texture);
constexpr std::size_t TEX_RAW = TEX + offsetof(TextureConfig, raw);
constexpr std::size_t BORDER = TEX + offsetof(TextureConfig, texture_border_color);
constexpr std::size_t TEV = TEX + offsetof(TextureConfig, tev_stages);
constexpr std::size_t LIGHT = offsetof(FSConfig, lighting);
constexpr std::size_t LIGHT_RAW = LIGHT + offsetof(LightConfig, raw);
constexpr std::size_t LIGHTS = LIGHT + offsetof(LightConfig, lights);
constexpr std::size_t PROCTEX = offsetof(FSConfig, proctex);
constexpr std::size_t PROCTEX_RAW = PROCTEX + offsetof(ProcTexConfig, raw);

// The combiner stages are read as whole uvec4s: sources, modifiers, operations, scales.
static_assert(TEV % 16 == 0 && sizeof(Pica::Shader::TevStageConfigRaw) == 16);
static_assert((PROCTEX + offsetof(ProcTexConfig, lod_min)) / 4 ==
                  (PROCTEX + offsetof(ProcTexConfig, lod_max)) / 4,
              "both LOD limits are read from one word");

/// The GLSL expression for the u32 at a byte offset of the configuration buffer.
std::string Word(std::size_t offset) {
    return fmt::format("uber_cfg[{}][{}]", offset / 16, offset % 16 / 4);
}

/// Appends a macro that extracts the BitField `Field` from the u32 at `offset`.
template <typename Field>
void DefineField(std::string& out, std::string_view name, std::size_t offset) {
    out += fmt::format("#define {} bitfieldExtract({}, {}, {})\n", name, Word(offset),
                       Field::position, Field::bits);
}

/// Appends a macro that extracts the BitField `Field` from a value given as an argument.
template <typename Field>
void DefineFieldOf(std::string& out, std::string_view name) {
    out += fmt::format("#define {}(v) bitfieldExtract(v, {}, {})\n", name, Field::position,
                       Field::bits);
}

/// Appends the type, absolute-input and scale macros of one lighting lookup table.
void DefineLut(std::string& out, std::string_view name, std::size_t lut_offset) {
    const std::size_t raw = LIGHT + lut_offset + offsetof(LutConfig, raw);
    DefineField<decltype(LutConfig::type)>(out, fmt::format("LUT_{}_TYPE", name), raw);
    DefineField<decltype(LutConfig::abs_input)>(out, fmt::format("LUT_{}_ABS", name), raw);
    out += fmt::format("#define LUT_{}_SCALE uintBitsToFloat({})\n", name,
                       Word(LIGHT + lut_offset + offsetof(LutConfig, scale_bits)));
}

/// Appends the macros that locate every configuration field the GLSL body reads.
void DefineConfigFields(std::string& out) {
    using FBC = FramebufferConfig;
    DefineField<decltype(FBC::alpha_test_func)>(out, "ALPHA_TEST_FUNC", FB);
    DefineField<decltype(FBC::scissor_test_mode)>(out, "SCISSOR_MODE", FB);
    DefineField<decltype(FBC::depthmap_enable)>(out, "DEPTHMAP_ENABLE", FB);
    DefineField<decltype(FBC::logic_op)>(out, "LOGIC_OP", FB);

    DefineField<decltype(TextureConfig::texture0_type)>(out, "TEX0_TYPE", TEX_RAW);
    DefineField<decltype(TextureConfig::texture2_use_coord1)>(out, "TEX2_USE_COORD1", TEX_RAW);
    DefineField<decltype(TextureConfig::combiner_buffer_input)>(out, "BUFFER_INPUT", TEX_RAW);
    DefineField<decltype(TextureConfig::fog_mode)>(out, "FOG_MODE", TEX_RAW);
    DefineField<decltype(TextureConfig::fog_flip)>(out, "FOG_FLIP", TEX_RAW);
    DefineField<decltype(TextureConfig::shadow_texture_orthographic)>(out, "SHADOW_ORTHO", TEX_RAW);
    for (u32 i = 0; i < 3; i++) {
        const std::size_t border = BORDER + i * sizeof(TextureBorder);
        DefineField<decltype(TextureBorder::enable_s)>(out, fmt::format("BORDER_S{}", i), border);
        DefineField<decltype(TextureBorder::enable_t)>(out, fmt::format("BORDER_T{}", i), border);
    }

    out += fmt::format("#define TEV_VEC {}\n", TEV / 16);
    DefineFieldOf<decltype(TevStageConfig::color_source1)>(out, "COLOR_SOURCE1");
    DefineFieldOf<decltype(TevStageConfig::color_source2)>(out, "COLOR_SOURCE2");
    DefineFieldOf<decltype(TevStageConfig::color_source3)>(out, "COLOR_SOURCE3");
    DefineFieldOf<decltype(TevStageConfig::alpha_source1)>(out, "ALPHA_SOURCE1");
    DefineFieldOf<decltype(TevStageConfig::alpha_source2)>(out, "ALPHA_SOURCE2");
    DefineFieldOf<decltype(TevStageConfig::alpha_source3)>(out, "ALPHA_SOURCE3");
    DefineFieldOf<decltype(TevStageConfig::color_modifier1)>(out, "COLOR_MODIFIER1");
    DefineFieldOf<decltype(TevStageConfig::color_modifier2)>(out, "COLOR_MODIFIER2");
    DefineFieldOf<decltype(TevStageConfig::color_modifier3)>(out, "COLOR_MODIFIER3");
    DefineFieldOf<decltype(TevStageConfig::alpha_modifier1)>(out, "ALPHA_MODIFIER1");
    DefineFieldOf<decltype(TevStageConfig::alpha_modifier2)>(out, "ALPHA_MODIFIER2");
    DefineFieldOf<decltype(TevStageConfig::alpha_modifier3)>(out, "ALPHA_MODIFIER3");
    DefineFieldOf<decltype(TevStageConfig::color_op)>(out, "COLOR_OP");
    DefineFieldOf<decltype(TevStageConfig::alpha_op)>(out, "ALPHA_OP");
    DefineFieldOf<decltype(TevStageConfig::color_scale)>(out, "COLOR_SCALE");
    DefineFieldOf<decltype(TevStageConfig::alpha_scale)>(out, "ALPHA_SCALE");

    using LC = LightConfig;
    DefineField<decltype(LC::enable)>(out, "LIGHTING_ENABLE", LIGHT_RAW);
    DefineField<decltype(LC::src_num)>(out, "LIGHT_COUNT", LIGHT_RAW);
    DefineField<decltype(LC::bump_mode)>(out, "BUMP_MODE", LIGHT_RAW);
    DefineField<decltype(LC::bump_selector)>(out, "BUMP_SELECTOR", LIGHT_RAW);
    DefineField<decltype(LC::bump_renorm)>(out, "BUMP_RENORM", LIGHT_RAW);
    DefineField<decltype(LC::clamp_highlights)>(out, "CLAMP_HIGHLIGHTS", LIGHT_RAW);
    DefineField<decltype(LC::config)>(out, "LIGHTING_CONFIG", LIGHT_RAW);
    DefineField<decltype(LC::enable_primary_alpha)>(out, "PRIMARY_ALPHA", LIGHT_RAW);
    DefineField<decltype(LC::enable_secondary_alpha)>(out, "SECONDARY_ALPHA", LIGHT_RAW);
    DefineField<decltype(LC::enable_shadow)>(out, "LIGHT_SHADOW", LIGHT_RAW);
    DefineField<decltype(LC::shadow_primary)>(out, "SHADOW_PRIMARY", LIGHT_RAW);
    DefineField<decltype(LC::shadow_secondary)>(out, "SHADOW_SECONDARY", LIGHT_RAW);
    DefineField<decltype(LC::shadow_invert)>(out, "SHADOW_INVERT", LIGHT_RAW);
    DefineField<decltype(LC::shadow_alpha)>(out, "SHADOW_ALPHA", LIGHT_RAW);
    DefineField<decltype(LC::shadow_selector)>(out, "SHADOW_SELECTOR", LIGHT_RAW);
    DefineLut(out, "D0", offsetof(LC, lut_d0));
    DefineLut(out, "D1", offsetof(LC, lut_d1));
    DefineLut(out, "SP", offsetof(LC, lut_sp));
    DefineLut(out, "FR", offsetof(LC, lut_fr));
    DefineLut(out, "RR", offsetof(LC, lut_rr));
    DefineLut(out, "RG", offsetof(LC, lut_rg));
    DefineLut(out, "RB", offsetof(LC, lut_rb));
    out += fmt::format("#define LIGHTS_BYTE {}\n", LIGHTS);
    DefineFieldOf<decltype(Light::num)>(out, "LIGHT_NUM");
    DefineFieldOf<decltype(Light::directional)>(out, "LIGHT_DIRECTIONAL");
    DefineFieldOf<decltype(Light::two_sided_diffuse)>(out, "LIGHT_TWO_SIDED");
    DefineFieldOf<decltype(Light::dist_atten_enable)>(out, "LIGHT_DIST_ATTEN");
    DefineFieldOf<decltype(Light::spot_atten_enable)>(out, "LIGHT_SPOT_ATTEN");
    DefineFieldOf<decltype(Light::geometric_factor_0)>(out, "LIGHT_GEO_0");
    DefineFieldOf<decltype(Light::geometric_factor_1)>(out, "LIGHT_GEO_1");
    DefineFieldOf<decltype(Light::shadow_enable)>(out, "LIGHT_SHADOW_ENABLE");

    using PC = ProcTexConfig;
    DefineField<decltype(PC::enable)>(out, "PT_ENABLE", PROCTEX_RAW);
    DefineField<decltype(PC::coord)>(out, "PT_COORD", PROCTEX_RAW);
    DefineField<decltype(PC::u_clamp)>(out, "PT_U_CLAMP", PROCTEX_RAW);
    DefineField<decltype(PC::v_clamp)>(out, "PT_V_CLAMP", PROCTEX_RAW);
    DefineField<decltype(PC::color_combiner)>(out, "PT_COLOR_COMBINER", PROCTEX_RAW);
    DefineField<decltype(PC::alpha_combiner)>(out, "PT_ALPHA_COMBINER", PROCTEX_RAW);
    DefineField<decltype(PC::lut_filter)>(out, "PT_FILTER", PROCTEX_RAW);
    DefineField<decltype(PC::separate_alpha)>(out, "PT_SEPARATE_ALPHA", PROCTEX_RAW);
    DefineField<decltype(PC::noise_enable)>(out, "PT_NOISE", PROCTEX_RAW);
    DefineField<decltype(PC::u_shift)>(out, "PT_U_SHIFT", PROCTEX_RAW);
    DefineField<decltype(PC::v_shift)>(out, "PT_V_SHIFT", PROCTEX_RAW);
    out += fmt::format("#define PT_LUT_WIDTH int({})\n", Word(PROCTEX + offsetof(PC, lut_width)));
    out +=
        fmt::format("#define PT_LUT_OFFSET0 int({})\n", Word(PROCTEX + offsetof(PC, lut_offset0)));
    out +=
        fmt::format("#define PT_LUT_OFFSET1 int({})\n", Word(PROCTEX + offsetof(PC, lut_offset1)));
    out +=
        fmt::format("#define PT_LUT_OFFSET2 int({})\n", Word(PROCTEX + offsetof(PC, lut_offset2)));
    out +=
        fmt::format("#define PT_LUT_OFFSET3 int({})\n", Word(PROCTEX + offsetof(PC, lut_offset3)));
    constexpr std::size_t lod_min = PROCTEX + offsetof(PC, lod_min);
    constexpr std::size_t lod_max = PROCTEX + offsetof(PC, lod_max);
    out += fmt::format("#define PT_LOD_MIN bitfieldExtract({}, {}, 16)\n", Word(lod_min),
                       lod_min % 4 * 8);
    out += fmt::format("#define PT_LOD_MAX bitfieldExtract({}, {}, 16)\n", Word(lod_max),
                       lod_max % 4 * 8);

    out += fmt::format("#define TEV_PASS_THROUGH bitfieldExtract({}, 0, 6)\n",
                       Word(offsetof(UberConfig, tev)));
    out += fmt::format("#define TEV_STAGES int(bitfieldExtract({}, 8, 4))\n",
                       Word(offsetof(UberConfig, tev)));
    out += fmt::format("#define TEXTURES_USED {}\n", Word(offsetof(UberConfig, textures)));
    out += fmt::format("#define LUTS_USED {}\n", Word(offsetof(UberConfig, luts)));
}

// Bits of UberConfig::luts, in the order of the lookup tables in LightConfig.
constexpr u32 LUT_D0 = 1u << 0, LUT_D1 = 1u << 1, LUT_SP = 1u << 2, LUT_FR = 1u << 3,
              LUT_RR = 1u << 4, LUT_RG = 1u << 5, LUT_RB = 1u << 6;

/// Same test as both Azahar generators use to omit a stage's code.
bool IsPassThrough(const TevStageConfig& stage) {
    return stage.color_op == TevStageConfig::Operation::Replace &&
           stage.alpha_op == TevStageConfig::Operation::Replace &&
           stage.color_source1 == TevStageConfig::Source::Previous &&
           stage.alpha_source1 == TevStageConfig::Source::Previous &&
           stage.color_modifier1 == TevStageConfig::ColorModifier::SourceColor &&
           stage.alpha_modifier1 == TevStageConfig::AlphaModifier::SourceAlpha &&
           stage.GetColorMultiplier() == 1 && stage.GetAlphaMultiplier() == 1;
}

// The GLSL body. Configuration-dependent code in Azahar's generators becomes a branch on
// a configuration field here; every branch is uniform across a draw. MAD(a, b, c) is
// fma() where the SPIR-V generator uses OpFma and a * b + c where the GLSL generator
// writes that, so both rounding behaviors are reproduced exactly.
constexpr std::string_view BODY = R"(
#if GLSL_SEMANTICS
#define MAD(a, b, c) ((a) * (b) + (c))
#else
#define MAD(a, b, c) fma(a, b, c)
#endif

vec4 rounded_primary_color;
vec4 primary_fragment_color = vec4(0.0);
vec4 secondary_fragment_color = vec4(0.0);
vec4 combiner_buffer = vec4(0.0);
vec4 combiner_output = vec4(0.0);
// Texture unit results, sampled once before lighting and the combiners read them.
vec4 tex_unit0 = vec4(0.0);
vec4 tex_unit1 = vec4(0.0);
vec4 tex_unit2 = vec4(0.0);
vec4 tex_unit3 = vec4(0.0);

float byteround(float x) {
    return round(x * 255.0) * (1.0 / 255.0);
}

vec3 byteround(vec3 x) {
    return round(x * 255.0) * (1.0 / 255.0);
}

vec4 byteround(vec4 x) {
    return round(x * 255.0) * (1.0 / 255.0);
}

vec3 quaternion_rotate(vec4 q, vec3 v) {
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

float getLod(vec2 coord) {
    vec2 d = max(abs(dFdx(coord)), abs(dFdy(coord)));
    return log2(max(d.x, d.y));
}

vec4 TexUnit(uint unit) {
    switch (unit) {
    case 0u: return tex_unit0;
    case 1u: return tex_unit1;
    case 2u: return tex_unit2;
    default: return tex_unit3;
    }
}

// ---- Shadow textures (DefineShadowHelpers / SampleShadow) ----

#if TEX0_CLASS == TEX0_SHADOW_2D || TEX0_CLASS == TEX0_SHADOW_CUBE
float CompareShadow(uint pixel, uint z) {
    uvec2 p = uvec2(pixel >> 8, pixel & 0xFFu);
    return mix(float(p.y) * (1.0 / 255.0), 0.0, p.x <= z);
}

float mix2(vec4 s, vec2 a) {
    vec2 t = mix(s.xy, s.zw, a.yy);
    return mix(t.x, t.y, a.x);
}
#endif

#if TEX0_CLASS == TEX0_SHADOW_2D
float SampleShadow2D(ivec2 uv, uint z) {
    if (any(bvec4(lessThan(uv, ivec2(0)), greaterThanEqual(uv, textureSize(tex0, 0)))))
        return 1.0;
    return CompareShadow(texelFetch(tex0, uv, 0).x, z);
}

vec4 shadowTexture(vec2 uv, float w) {
    if (SHADOW_ORTHO == 0u) {
        uv /= w;
    }
    uint z = uint(max(0, int(min(abs(w), 1.0) * float(0xFFFFFF)) - shadow_texture_bias));
    vec2 coord = MAD(vec2(textureSize(tex0, 0)), uv, -vec2(0.5));
    vec2 coord_floor = floor(coord);
    vec2 f = coord - coord_floor;
    ivec2 i = ivec2(coord_floor);
    vec4 s = vec4(
        SampleShadow2D(i              , z),
        SampleShadow2D(i + ivec2(1, 0), z),
        SampleShadow2D(i + ivec2(0, 1), z),
        SampleShadow2D(i + ivec2(1, 1), z));
    return vec4(mix2(s, f));
}
#endif

#if TEX0_CLASS == TEX0_SHADOW_CUBE
uvec4 SampleShadowCube(int face, ivec2 i00, ivec2 i10, ivec2 i01, ivec2 i11) {
    return uvec4(
        texelFetch(tex0[face], i00, 0).r,
        texelFetch(tex0[face], i10, 0).r,
        texelFetch(tex0[face], i01, 0).r,
        texelFetch(tex0[face], i11, 0).r);
}

vec4 shadowTextureCube(vec2 uv, float w) {
    ivec2 size = textureSize(tex0[0], 0);
    vec3 c = vec3(uv, w);
    vec3 a = abs(c);
    if (a.x > a.y && a.x > a.z) {
        w = a.x;
        uv = -c.zy;
        if (c.x < 0.0) uv.x = -uv.x;
    } else if (a.y > a.z) {
        w = a.y;
        uv = c.xz;
        if (c.y < 0.0) uv.y = -uv.y;
    } else {
        w = a.z;
        uv = -c.xy;
        if (c.z > 0.0) uv.x = -uv.x;
    }
    uint z = uint(max(0, int(min(w, 1.0) * float(0xFFFFFF)) - shadow_texture_bias));
    vec2 coord = vec2(size) * (uv / w * vec2(0.5) + vec2(0.5)) - vec2(0.5);
    vec2 coord_floor = floor(coord);
    vec2 f = coord - coord_floor;
    ivec2 i00 = ivec2(coord_floor);
    ivec2 i10 = i00 + ivec2(1, 0);
    ivec2 i01 = i00 + ivec2(0, 1);
    ivec2 i11 = i00 + ivec2(1, 1);
    ivec2 cmin = ivec2(0), cmax = size - ivec2(1, 1);
    i00 = clamp(i00, cmin, cmax);
    i10 = clamp(i10, cmin, cmax);
    i01 = clamp(i01, cmin, cmax);
    i11 = clamp(i11, cmin, cmax);
    uvec4 pixels;
    if (a.x > a.y && a.x > a.z) {
        if (c.x > 0.0)
            pixels = SampleShadowCube(0, i00, i10, i01, i11);
        else
            pixels = SampleShadowCube(1, i00, i10, i01, i11);
    } else if (a.y > a.z) {
        if (c.y > 0.0)
            pixels = SampleShadowCube(2, i00, i10, i01, i11);
        else
            pixels = SampleShadowCube(3, i00, i10, i01, i11);
    } else {
        if (c.z > 0.0)
            pixels = SampleShadowCube(4, i00, i10, i01, i11);
        else
            pixels = SampleShadowCube(5, i00, i10, i01, i11);
    }
    vec4 s = vec4(
        CompareShadow(pixels.x, z),
        CompareShadow(pixels.y, z),
        CompareShadow(pixels.z, z),
        CompareShadow(pixels.w, z));
    return vec4(mix2(s, f));
}
#endif

// ---- Procedural texture (DefineProcTexSampler / ProcTexSampler) ----

float ProcTexLookupLUT(int offset, float coord) {
    coord *= 128.0;
    float index_i = clamp(floor(coord), 0.0, 127.0);
    float index_f = coord - index_i;
    vec2 entry = texelFetch(texture_buffer_lut_rg, int(index_i) + offset).rg;
    return clamp(MAD(entry.g, index_f, entry.r), 0.0, 1.0);
}

int ProcTexNoiseRand1D(int v) {
    const int table[] = int[](0,4,10,8,4,9,7,12,5,15,13,14,11,15,2,11);
    return ((v % 9 + 2) * 3 & 0xF) ^ table[(v / 9) & 0xF];
}

float ProcTexNoiseRand2D(vec2 point) {
    const int table[] = int[](10,2,15,8,0,7,4,5,5,13,2,6,13,9,3,14);
    int u2 = ProcTexNoiseRand1D(int(point.x));
    int v2 = ProcTexNoiseRand1D(int(point.y));
    v2 += ((u2 & 3) == 1) ? 4 : 0;
    v2 ^= (u2 & 1) * 6;
    v2 += 10 + u2;
    v2 &= 0xF;
    v2 ^= table[u2];
    return MAD(float(v2), 2.0 / 15.0, -1.0);
}

float ProcTexNoiseCoef(vec2 x) {
    vec2 grid  = 9.0 * proctex_noise_f * abs(x + proctex_noise_p);
    vec2 point = floor(grid);
    vec2 frac  = grid - point;

    float g0 = ProcTexNoiseRand2D(point) * (frac.x + frac.y);
    float g1 = ProcTexNoiseRand2D(point + vec2(1.0, 0.0)) * (frac.x + frac.y - 1.0);
    float g2 = ProcTexNoiseRand2D(point + vec2(0.0, 1.0)) * (frac.x + frac.y - 1.0);
    float g3 = ProcTexNoiseRand2D(point + vec2(1.0, 1.0)) * (frac.x + frac.y - 2.0);

    float x_noise = ProcTexLookupLUT(proctex_noise_lut_offset, frac.x);
    float y_noise = ProcTexLookupLUT(proctex_noise_lut_offset, frac.y);
    float x0 = mix(g0, g1, x_noise);
    float x1 = mix(g2, g3, x_noise);
    return mix(x0, x1, y_noise);
}

vec4 SampleProcTexColor(float lut_coord, int level) {
    int lut_width = PT_LUT_WIDTH >> level;
    // Offsets for level 4-7 seem to be hardcoded
    int lut_offsets[8] = int[](PT_LUT_OFFSET0, PT_LUT_OFFSET1, PT_LUT_OFFSET2, PT_LUT_OFFSET3,
                               0xF0, 0xF8, 0xFC, 0xFE);
    int lut_offset = lut_offsets[level];
    // For the color lut, coord=0.0 is lut[offset] and coord=1.0 is lut[offset+width-1]
    lut_coord *= float(lut_width - 1);
    switch (PT_FILTER) {
    case 1u: case 3u: case 5u: { // Linear, LinearMipmapNearest, LinearMipmapLinear
        int lut_index_i = int(lut_coord) + lut_offset;
        float lut_index_f = fract(lut_coord);
        return texelFetch(texture_buffer_lut_rgba, lut_index_i + proctex_lut_offset) +
               lut_index_f * texelFetch(texture_buffer_lut_rgba, lut_index_i + proctex_diff_lut_offset);
    }
    case 0u: case 2u: case 4u: // Nearest, NearestMipmapNearest, NearestMipmapLinear
        lut_coord += float(lut_offset);
        return texelFetch(texture_buffer_lut_rgba, int(round(lut_coord)) + proctex_lut_offset);
    default:
        return vec4(0.0);
    }
}

float ProcTexShiftOffset(float v, uint mode, uint clamp_mode) {
    float offset = clamp_mode == 3u ? 1.0 : 0.5; // MirroredRepeat
    switch (mode) {
    case 1u: return offset * float((int(v) / 2) % 2);       // Odd
    case 2u: return offset * float(((int(v) + 1) / 2) % 2); // Even
    default: return 0.0;
    }
}

float ProcTexClamp(float v, uint mode) {
    switch (mode) {
    case 0u: return v > 1.0 ? 0.0 : v;                            // ToZero
    case 2u: return fract(v);                                     // SymmetricalRepeat
    case 3u: return int(v) % 2 == 0 ? fract(v) : 1.0 - fract(v);  // MirroredRepeat
    case 4u: return v > 0.5 ? 1.0 : 0.0;                          // Pulse
    default: return min(v, 1.0);                                  // ToEdge
    }
}

float ProcTexCombineAndMap(uint combiner, float u, float v, int offset) {
    float combined;
    switch (combiner) {
    case 0u: combined = u; break;
    case 1u: combined = u * u; break;
    case 2u: combined = v; break;
    case 3u: combined = v * v; break;
    case 4u: combined = (u + v) * 0.5; break;
    case 5u: combined = MAD(u, u, v * v) * 0.5; break;
    case 6u: combined = min(sqrt(MAD(u, u, v * v)), 1.0); break;
    case 7u: combined = min(u, v); break;
    case 8u: combined = max(u, v); break;
    case 9u: combined = min(MAD(u + v, 0.5, sqrt(MAD(u, u, v * v))) * 0.5, 1.0); break;
    default: combined = 0.0; break;
    }
    return ProcTexLookupLUT(offset, combined);
}

vec4 ProcTex() {
    uint coord = PT_COORD;
    vec2 uv = abs(coord == 1u ? texcoord1 : (coord == 2u ? texcoord2 : texcoord0));

    // This LOD formula is the same as the LOD upper limit defined in OpenGL.
    // f(x, y) <= m_u + m_v + m_w
    // (See OpenGL 4.6 spec, 8.14.1 - Scale Factor and Level-of-Detail)
    vec2 duv = max(abs(dFdx(uv)), abs(dFdy(uv)));
    // unlike normal texture, the bias is inside the log2
    float lod = log2(abs(float(PT_LUT_WIDTH) * proctex_bias) * (duv.x + duv.y));
    lod = proctex_bias == 0.0 ? 0.0 : lod;
    lod = clamp(lod, max(0.0, float(PT_LOD_MIN)), min(7.0, float(PT_LOD_MAX)));

    // Get shift offset before noise generation
    float u_shift = ProcTexShiftOffset(uv.y, PT_U_SHIFT, PT_U_CLAMP);
    float v_shift = ProcTexShiftOffset(uv.x, PT_V_SHIFT, PT_V_CLAMP);

    if (PT_NOISE != 0u) {
        uv += proctex_noise_a * ProcTexNoiseCoef(uv);
        uv = abs(uv);
    }

    float u = ProcTexClamp(uv.x + u_shift, PT_U_CLAMP);
    float v = ProcTexClamp(uv.y + v_shift, PT_V_CLAMP);

    float lut_coord = ProcTexCombineAndMap(PT_COLOR_COMBINER, u, v, proctex_color_map_offset);
    vec4 final_color;
    switch (PT_FILTER) {
    case 2u: case 3u: // NearestMipmapNearest, LinearMipmapNearest
        final_color = SampleProcTexColor(lut_coord, int(round(lod)));
        break;
    case 4u: case 5u: { // NearestMipmapLinear, LinearMipmapLinear
        int lod_i = int(lod);
        float lod_f = fract(lod);
        final_color = mix(SampleProcTexColor(lut_coord, lod_i),
                          SampleProcTexColor(lut_coord, lod_i + 1), lod_f);
        break;
    }
    default: // Nearest, Linear
        final_color = SampleProcTexColor(lut_coord, 0);
        break;
    }

    if (PT_SEPARATE_ALPHA != 0u) {
        // In separate alpha mode, the alpha channel skips the color LUT look up stage and
        // uses the output of CombineAndMap directly.
        final_color.a = ProcTexCombineAndMap(PT_ALPHA_COMBINER, u, v, proctex_alpha_map_offset);
    }
    return final_color;
}

// ---- Texture units (DefineTexUnitSampler / DefineTexSampler) ----

vec4 SampleTexUnit0() {
    uint type = TEX0_TYPE;
    if (type == 5u) { // Disabled
        return vec4(0.0);
    }
    if (BORDER_S0 != 0u && (texcoord0.x < 0.0 || texcoord0.x > 1.0)) {
        return tex_border_color[0];
    }
    if (BORDER_T0 != 0u && (texcoord0.y < 0.0 || texcoord0.y > 1.0)) {
        return tex_border_color[0];
    }
#if TEX0_CLASS == TEX0_PLAIN
    if (type == 0u) { // Texture2D
        return textureLod(tex0, texcoord0, getLod(texcoord0 * vec2(textureSize(tex0, 0))) + tex_lod_bias[0]);
    }
    if (type == 3u) { // Projection2D
        return textureProj(tex0, vec3(texcoord0, texcoord0_w));
    }
#if GLSL_SEMANTICS
    return texture(tex0, texcoord0);
#else
    return vec4(0.0);
#endif
#elif TEX0_CLASS == TEX0_CUBE
    return texture(tex0, vec3(texcoord0, texcoord0_w));
#elif TEX0_CLASS == TEX0_SHADOW_2D
    return shadowTexture(texcoord0, texcoord0_w);
#else
    return shadowTextureCube(texcoord0, texcoord0_w);
#endif
}

vec4 SampleTexUnit1() {
    if (BORDER_S1 != 0u && (texcoord1.x < 0.0 || texcoord1.x > 1.0)) {
        return tex_border_color[1];
    }
    if (BORDER_T1 != 0u && (texcoord1.y < 0.0 || texcoord1.y > 1.0)) {
        return tex_border_color[1];
    }
    return textureLod(tex1, texcoord1, getLod(texcoord1 * vec2(textureSize(tex1, 0))) + tex_lod_bias[1]);
}

vec4 SampleTexUnit2() {
    vec2 coord = TEX2_USE_COORD1 != 0u ? texcoord1 : texcoord2;
    if (BORDER_S2 != 0u && (coord.x < 0.0 || coord.x > 1.0)) {
        return tex_border_color[2];
    }
    if (BORDER_T2 != 0u && (coord.y < 0.0 || coord.y > 1.0)) {
        return tex_border_color[2];
    }
    return textureLod(tex2, coord, getLod(coord * vec2(textureSize(tex2, 0))) + tex_lod_bias[2]);
}

vec4 SampleTexUnit3() {
    return PT_ENABLE != 0u ? ProcTex() : vec4(0.0);
}

// ---- Fragment lighting (WriteLighting) ----

uint LightSlot(uint slot) {
    uint byte_offset = uint(LIGHTS_BYTE) + 2u * slot;
    return bitfieldExtract(uber_cfg[byte_offset >> 4][(byte_offset >> 2) & 3u],
                           int(byte_offset & 3u) * 8, 16);
}

float LookupLightingLUT(int lut_index, int index, float delta) {
    vec2 entry = texelFetch(texture_buffer_lut_lf, lighting_lut_offset[lut_index >> 2][lut_index & 3] + index).rg;
    return MAD(entry.g, delta, entry.r);
}

float LookupLightingLUTUnsigned(int lut_index, float pos) {
    int index = int(clamp(floor(pos * 256.0), 0.0, 255.0));
    float delta = MAD(pos, 256.0, -float(index));
    return LookupLightingLUT(lut_index, index, delta);
}

float LookupLightingLUTSigned(int lut_index, float pos) {
    int index = int(clamp(floor(pos * 128.0), -128.0, 127.0));
    float delta = MAD(pos, 128.0, -float(index));
    if (index < 0) index += 256;
    return LookupLightingLUT(lut_index, index, delta);
}

// The values a lighting lookup table can be indexed by (get_lut_value in the generators).
struct LightingVectors {
    vec3 normal;
    vec3 tangent;
    vec3 light_vector;
    vec3 spot_dir;
    vec3 half_vector;
};

float LutValue(int lut_index, uint input_type, bool abs_input, bool two_sided, LightingVectors lv) {
    float index;
    switch (input_type) {
    case 0u: index = dot(lv.normal, normalize(lv.half_vector)); break;               // NH
    case 1u: index = dot(normalize(view), normalize(lv.half_vector)); break;         // VH
    case 2u: index = dot(lv.normal, normalize(view)); break;                         // NV
    case 3u: index = dot(lv.light_vector, lv.normal); break;                         // LN
    case 4u: index = dot(lv.light_vector, lv.spot_dir); break;                       // SP
    case 5u: // CP, only available with configuration 7
        if (LIGHTING_CONFIG == 8u) {
            // The half angle vector projection is confirmed not normalized before the dot
            // product. The result is in fact not cos(phi) as the name suggested.
            vec3 half_angle_proj = normalize(lv.half_vector) -
                                   lv.normal * dot(lv.normal, normalize(lv.half_vector));
            index = dot(half_angle_proj, lv.tangent);
        } else {
            index = 0.0;
        }
        break;
    default: index = 0.0; break;
    }
    if (abs_input) {
        // LUT index is in the range of (0.0, 1.0)
        index = two_sided ? abs(index) : max(index, 0.0);
        return LookupLightingLUTUnsigned(lut_index, index);
    }
    // LUT index is in the range of (-1.0, 1.0)
    return LookupLightingLUTSigned(lut_index, index);
}

void WriteLighting() {
    vec4 diffuse_sum = vec4(0.0, 0.0, 0.0, 1.0);
    vec4 specular_sum = vec4(0.0, 0.0, 0.0, 1.0);
    float clamp_highlights = 1.0;
    float geo_factor = 1.0;
    uint luts = LUTS_USED;

    // Compute fragment normals and tangents
    vec3 surface_normal = vec3(0.0, 0.0, 1.0);
    vec3 surface_tangent = vec3(1.0, 0.0, 0.0);
#if CUSTOM_NORMAL
    surface_normal = 2.0 * (texture(tex_normal, texcoord0)).rgb - 1.0;
#else
    uint bump_mode = BUMP_MODE;
    if (bump_mode == 1u) { // NormalMap
        surface_normal = 2.0 * TexUnit(BUMP_SELECTOR).rgb - 1.0;
        // Recompute Z-component of perturbation if 'renorm' is enabled, this provides a
        // higher precision result
        if (BUMP_RENORM != 0u) {
            surface_normal.z = sqrt(max(1.0 - MAD(surface_normal.x, surface_normal.x,
                                                  surface_normal.y * surface_normal.y), 0.0));
        }
    } else if (bump_mode == 2u) { // TangentMap
        surface_tangent = 2.0 * TexUnit(BUMP_SELECTOR).rgb - 1.0;
    }
#endif

#if BARYCENTRIC
    // Quaternion correction from the three vertices' quaternions
    vec4 normquat_0 = normquats[0];
    vec4 normquat_1 = mix(normquats[1], -normquats[1], bvec4(dot(normquats[0], normquats[1]) < 0.0));
    vec4 normquat_2 = mix(normquats[2], -normquats[2], bvec4(dot(normquats[0], normquats[2]) < 0.0));
#if GLSL_SEMANTICS
    vec4 normquat = gl_BaryCoord.x * normquat_0 + gl_BaryCoord.y * normquat_1 +
                    gl_BaryCoord.z * normquat_2;
#else
    vec4 normquat = normquat_0 * gl_BaryCoord.x +
                    (normquat_1 * gl_BaryCoord.y + normquat_2 * gl_BaryCoord.z);
#endif
#endif

    // Rotate the surface-local normal by the interpolated normal quaternion to convert it
    // to eyespace.
    vec4 normalized_normquat = normalize(normquat);
    LightingVectors lv;
    lv.normal = quaternion_rotate(normalized_normquat, surface_normal);
    lv.tangent = quaternion_rotate(normalized_normquat, surface_tangent);

    vec4 shadow = vec4(1.0);
    if (LIGHT_SHADOW != 0u) {
        shadow = TexUnit(SHADOW_SELECTOR);
        if (SHADOW_INVERT != 0u) {
            shadow = vec4(1.0) - shadow;
        }
    }

    uint light_count = min(LIGHT_COUNT, 8u);
    for (uint light_index = 0u; light_index < light_count; ++light_index) {
        uint light = LightSlot(light_index);
        uint num = LIGHT_NUM(light);
        // Azahar's generators read two-sidedness for lookup-table inputs from the slot
        // numbered like the light, not from the light's own slot; this reproduces that.
        bool lut_two_sided = LIGHT_TWO_SIDED(LightSlot(num)) != 0u;

        // Compute light vector (directional or positional)
        vec3 light_position = light_src[num].position;
        lv.light_vector = LIGHT_DIRECTIONAL(light) != 0u ? light_position : light_position + view;
        float light_distance = length(lv.light_vector);
        lv.light_vector = normalize(lv.light_vector);
        lv.spot_dir = light_src[num].spot_direction;
        lv.half_vector = normalize(view) + lv.light_vector;

        // Compute dot product of light_vector and normal, adjust if lighting is one-sided or
        // two-sided
        float dot_product = LIGHT_TWO_SIDED(light) != 0u ? abs(dot(lv.light_vector, lv.normal))
                                                         : max(dot(lv.light_vector, lv.normal), 0.0);

        // If enabled, clamp specular component if lighting result is zero
        if (CLAMP_HIGHLIGHTS != 0u) {
            clamp_highlights = sign(dot_product);
        }

        // If enabled, compute spot light attenuation value
        float spot_atten = 1.0;
        if (LIGHT_SPOT_ATTEN(light) != 0u && (luts & LUT_SP_BIT) != 0u) {
            spot_atten = LUT_SP_SCALE * LutValue(8 + int(num), LUT_SP_TYPE, LUT_SP_ABS != 0u,
                                                 lut_two_sided, lv);
        }

        // If enabled, compute distance attenuation value
        float dist_atten = 1.0;
        if (LIGHT_DIST_ATTEN(light) != 0u) {
            float index = clamp(MAD(light_src[num].dist_atten_scale, light_distance,
                                    light_src[num].dist_atten_bias), 0.0, 1.0);
            dist_atten = LookupLightingLUTUnsigned(16 + int(num), index);
        }

        bool geo_0 = LIGHT_GEO_0(light) != 0u;
        bool geo_1 = LIGHT_GEO_1(light) != 0u;
        if (geo_0 || geo_1) {
            geo_factor = dot(lv.half_vector, lv.half_vector);
            geo_factor = geo_factor == 0.0 ? 0.0 : min(dot_product / geo_factor, 1.0);
        }

        // Specular 0 component
        float d0_lut_value = 1.0;
        if ((luts & LUT_D0_BIT) != 0u) {
            d0_lut_value = LUT_D0_SCALE * LutValue(0, LUT_D0_TYPE, LUT_D0_ABS != 0u, lut_two_sided, lv);
        }
        vec3 specular_0 = d0_lut_value * light_src[num].specular_0;
        if (geo_0) {
            specular_0 = specular_0 * geo_factor;
        }

        // If enabled, lookup ReflectRed value, otherwise, 1.0 is used
        vec3 refl_value;
        refl_value.r = 1.0;
        if ((luts & LUT_RR_BIT) != 0u) {
            refl_value.r = LUT_RR_SCALE * LutValue(6, LUT_RR_TYPE, LUT_RR_ABS != 0u, lut_two_sided, lv);
        }
        // If enabled, lookup ReflectGreen value, otherwise, ReflectRed value is used
        refl_value.g = refl_value.r;
        if ((luts & LUT_RG_BIT) != 0u) {
            refl_value.g = LUT_RG_SCALE * LutValue(5, LUT_RG_TYPE, LUT_RG_ABS != 0u, lut_two_sided, lv);
        }
        // If enabled, lookup ReflectBlue value, otherwise, ReflectRed value is used
        refl_value.b = refl_value.r;
        if ((luts & LUT_RB_BIT) != 0u) {
            refl_value.b = LUT_RB_SCALE * LutValue(4, LUT_RB_TYPE, LUT_RB_ABS != 0u, lut_two_sided, lv);
        }

        // Specular 1 component
        float d1_lut_value = 1.0;
        if ((luts & LUT_D1_BIT) != 0u) {
            d1_lut_value = LUT_D1_SCALE * LutValue(1, LUT_D1_TYPE, LUT_D1_ABS != 0u, lut_two_sided, lv);
        }
        vec3 specular_1 = d1_lut_value * refl_value * light_src[num].specular_1;
        if (geo_1) {
            specular_1 = specular_1 * geo_factor;
        }

        // Fresnel. Only the last entry in the light slots applies the Fresnel factor.
        if (light_index == light_count - 1u && (luts & LUT_FR_BIT) != 0u) {
            float value = LUT_FR_SCALE * LutValue(3, LUT_FR_TYPE, LUT_FR_ABS != 0u, lut_two_sided, lv);
            if (PRIMARY_ALPHA != 0u) {
                diffuse_sum.a = value;
            }
            if (SECONDARY_ALPHA != 0u) {
                specular_sum.a = value;
            }
        }

        bool light_shadow = LIGHT_SHADOW_ENABLE(light) != 0u;
        vec3 diffuse = light_src[num].diffuse * dot_product;
        if (SHADOW_PRIMARY != 0u && light_shadow) {
            diffuse = diffuse * shadow.rgb;
        }
        diffuse_sum.rgb += (diffuse + light_src[num].ambient) * dist_atten * spot_atten;

        vec3 specular = (specular_0 + specular_1) * clamp_highlights * dist_atten * spot_atten;
        if (SHADOW_SECONDARY != 0u && light_shadow) {
            specular = specular * shadow.rgb;
        }
        specular_sum.rgb += specular;
    }

    // Apply shadow attenuation to alpha components if enabled
    if (SHADOW_ALPHA != 0u) {
        if (PRIMARY_ALPHA != 0u) {
            diffuse_sum.a *= shadow.a;
        }
        if (SECONDARY_ALPHA != 0u) {
            specular_sum.a *= shadow.a;
        }
    }

    // Sum final lighting result
    diffuse_sum.rgb += lighting_global_ambient;
    primary_fragment_color = clamp(diffuse_sum, vec4(0.0), vec4(1.0));
    secondary_fragment_color = clamp(specular_sum, vec4(0.0), vec4(1.0));
}

// ---- Texture combiners (WriteTevStage and its helpers) ----

vec4 GetSource(uint source, int stage) {
    switch (source) {
    case 0u: return rounded_primary_color;
    case 1u: return primary_fragment_color;
    case 2u: return secondary_fragment_color;
    case 3u: return tex_unit0;
    case 4u: return tex_unit1;
    case 5u: return tex_unit2;
    case 6u: return tex_unit3;
    case 13u: return combiner_buffer;
    case 14u: return const_color[stage];
    case 15u: return combiner_output;
    default: return vec4(0.0);
    }
}

vec3 ColorModifier(uint modifier, vec4 c) {
    switch (modifier) {
    case 0u: return c.rgb;
    case 1u: return vec3(1.0) - c.rgb;
    case 2u: return c.aaa;
    case 3u: return vec3(1.0) - c.aaa;
    case 4u: return c.rrr;
    case 5u: return vec3(1.0) - c.rrr;
    case 8u: return c.ggg;
    case 9u: return vec3(1.0) - c.ggg;
    case 12u: return c.bbb;
    case 13u: return vec3(1.0) - c.bbb;
#if GLSL_SEMANTICS
    default: return vec3(0.0);
#else
    default: return vec3(1.0);
#endif
    }
}

float AlphaModifier(uint modifier, vec4 c) {
    switch (modifier) {
    case 0u: return c.a;
    case 1u: return 1.0 - c.a;
    case 2u: return c.r;
    case 3u: return 1.0 - c.r;
    case 4u: return c.g;
    case 5u: return 1.0 - c.g;
    case 6u: return c.b;
    default: return 1.0 - c.b;
    }
}

vec3 ColorCombiner(uint op, vec3 r1, vec3 r2, vec3 r3) {
    vec3 c;
    switch (op) {
    case 0u: c = r1; break;
    case 1u: c = r1 * r2; break;
    case 2u: c = r1 + r2; break;
    case 3u: c = r1 + r2 - vec3(128.0 / 255.0); break; // AddSigned: 128/255, as the generators
    case 4u: c = mix(r2, r1, r3); break;
    case 5u: c = r1 - r2; break;
    case 6u: case 7u: c = vec3(dot(r1 - vec3(0.5), r2 - vec3(0.5)) * 4.0); break;
    case 8u: c = fma(r1, r2, r3); break;
    case 9u: c = min(r1 + r2, vec3(1.0)) * r3; break;
    default: c = vec3(0.0); break;
    }
    return clamp(c, vec3(0.0), vec3(1.0));
}

float AlphaCombiner(uint op, float r1, float r2, float r3) {
    float c;
    switch (op) {
    case 0u: c = r1; break;
    case 1u: c = r1 * r2; break;
    case 2u: c = r1 + r2; break;
    case 3u: c = r1 + r2 - (128.0 / 255.0); break;
    case 4u: c = mix(r2, r1, r3); break;
    case 5u: c = r1 - r2; break;
    case 8u: c = fma(r1, r2, r3); break;
    case 9u: c = min(r1 + r2, 1.0) * r3; break;
    default: c = 0.0; break;
    }
    return clamp(c, 0.0, 1.0);
}

void WriteTevStages() {
    vec4 next_combiner_buffer = tev_combiner_buffer_color;
    uint pass_through = TEV_PASS_THROUGH;
    uint buffer_input = BUFFER_INPUT;
    // Stages after the last one that computes anything cannot change the output.
    int stage_count = TEV_STAGES;
    for (int stage = 0; stage < stage_count; ++stage) {
        uvec4 tev = uber_cfg[TEV_VEC + stage];
        if (((pass_through >> stage) & 1u) == 0u) {
            uint color_source1 = COLOR_SOURCE1(tev.x);
            uint color_source2 = COLOR_SOURCE2(tev.x);
            uint color_source3 = COLOR_SOURCE3(tev.x);
            uint alpha_source1 = ALPHA_SOURCE1(tev.x);
            uint alpha_source2 = ALPHA_SOURCE2(tev.x);
            uint alpha_source3 = ALPHA_SOURCE3(tev.x);
            // The first stage reads its third source where it is told to read Previous.
            if (stage == 0) {
                color_source1 = color_source1 == 15u ? color_source3 : color_source1;
                color_source2 = color_source2 == 15u ? color_source3 : color_source2;
                alpha_source1 = alpha_source1 == 15u ? alpha_source3 : alpha_source1;
                alpha_source2 = alpha_source2 == 15u ? alpha_source3 : alpha_source2;
            }
            vec3 color_results_1 = ColorModifier(COLOR_MODIFIER1(tev.y), GetSource(color_source1, stage));
            vec3 color_results_2 = ColorModifier(COLOR_MODIFIER2(tev.y), GetSource(color_source2, stage));
            vec3 color_results_3 = ColorModifier(COLOR_MODIFIER3(tev.y), GetSource(color_source3, stage));

            // Round the output of each TEV stage to maintain the PICA's 8 bits of precision
            uint color_op = COLOR_OP(tev.z);
            vec3 color_output = byteround(ColorCombiner(color_op, color_results_1, color_results_2, color_results_3));
            float alpha_output;
            if (color_op == 7u) {
                // result of Dot3_RGBA operation is also placed to the alpha component
                alpha_output = color_output.r;
            } else {
                float alpha_results_1 = AlphaModifier(ALPHA_MODIFIER1(tev.y), GetSource(alpha_source1, stage));
                float alpha_results_2 = AlphaModifier(ALPHA_MODIFIER2(tev.y), GetSource(alpha_source2, stage));
                float alpha_results_3 = AlphaModifier(ALPHA_MODIFIER3(tev.y), GetSource(alpha_source3, stage));
                alpha_output = byteround(AlphaCombiner(ALPHA_OP(tev.z), alpha_results_1, alpha_results_2, alpha_results_3));
            }

            uint color_scale = COLOR_SCALE(tev.w);
            uint alpha_scale = ALPHA_SCALE(tev.w);
            float color_multiplier = color_scale < 3u ? float(1u << color_scale) : 1.0;
            float alpha_multiplier = alpha_scale < 3u ? float(1u << alpha_scale) : 1.0;
            combiner_output = vec4(clamp(color_output * color_multiplier, vec3(0.0), vec3(1.0)),
                                   clamp(alpha_output * alpha_multiplier, 0.0, 1.0));
        }

        combiner_buffer = next_combiner_buffer;
        if (stage < 4) {
            if (((buffer_input >> stage) & 1u) != 0u) {
                next_combiner_buffer.rgb = combiner_output.rgb;
            }
            if (((buffer_input >> (4 + stage)) & 1u) != 0u) {
                next_combiner_buffer.a = combiner_output.a;
            }
        }
    }
}

#if SHADOW_RENDERING
uint EncodeShadow(uvec2 pixel) {
    return (pixel.x << 8) | pixel.y;
}

uint UpdateShadow(uint pixel, uint d, uint s) {
    uvec2 ref = uvec2(pixel >> 8, pixel & 0xFFu);
    if (d < ref.x) {
        if (s == 0u) {
            ref.x = d;
        } else {
            s = uint(float(s) / (shadow_bias_constant + shadow_bias_linear * float(d) / float(ref.x)));
            ref.y = min(s, ref.y);
        }
    }
    return EncodeShadow(ref);
}
#endif

void main() {
    // Do not do any sort of processing if it's obvious we're not going to pass the alpha test
    uint alpha_test_func = ALPHA_TEST_FUNC;
    if (alpha_test_func == 0u) {
        discard;
    }

    // We round the interpolated primary color to the nearest 1/255th
    // This maintains the PICA's 8 bits of precision
    rounded_primary_color = byteround(primary_color);

    // The PICA depth range is [-1, 0]; undo the host transformation and apply the PICA one.
    float z_over_w = -gl_FragCoord.z;
    float depth = MAD(z_over_w, depth_scale, depth_offset);
    if (DEPTHMAP_ENABLE == 0u) { // WBuffering
        depth /= gl_FragCoord.w;
    }
#if !SHADOW_RENDERING
#if GLSL_SEMANTICS
    // The GLSL generator writes no depth for gas, which leaves the rasterized depth.
    gl_FragDepth = FOG_MODE == 7u ? gl_FragCoord.z : depth;
#else
    gl_FragDepth = depth;
#endif
#endif

    uint scissor_mode = SCISSOR_MODE;
    if (scissor_mode != 0u) {
        bool inside = gl_FragCoord.x >= float(scissor_x1) && gl_FragCoord.y >= float(scissor_y1) &&
                      gl_FragCoord.x < float(scissor_x2) && gl_FragCoord.y < float(scissor_y2);
        // Include (3) keeps only the pixels inside the box; the other modes discard them.
        if (scissor_mode == 3u ? !inside : inside) {
            discard;
        }
    }

    uint textures = TEXTURES_USED;
    if ((textures & 1u) != 0u) tex_unit0 = SampleTexUnit0();
    if ((textures & 2u) != 0u) tex_unit1 = SampleTexUnit1();
    if ((textures & 4u) != 0u) tex_unit2 = SampleTexUnit2();
    if ((textures & 8u) != 0u) tex_unit3 = SampleTexUnit3();

    if (LIGHTING_ENABLE != 0u) {
        WriteLighting();
    }

    WriteTevStages();

    if (alpha_test_func >= 2u) {
        int alpha = int(combiner_output.a * 255.0);
        bool fail;
        switch (alpha_test_func) {
        case 2u: fail = alpha != alphatest_ref; break; // Equal
        case 3u: fail = alpha == alphatest_ref; break; // NotEqual
        case 4u: fail = alpha >= alphatest_ref; break; // LessThan
        case 5u: fail = alpha > alphatest_ref; break;  // LessThanOrEqual
        case 6u: fail = alpha <= alphatest_ref; break; // GreaterThan
        default: fail = alpha < alphatest_ref; break;  // GreaterThanOrEqual
        }
        if (fail) {
            discard;
        }
    }

    uint fog_mode = FOG_MODE;
    if (fog_mode == 5u) { // Fog
        float fog_index = (FOG_FLIP != 0u ? 1.0 - depth : depth) * 128.0;
        float fog_i = clamp(floor(fog_index), 0.0, 127.0);
        float fog_f = fog_index - fog_i;
        vec2 fog_lut_entry = texelFetch(texture_buffer_lut_lf, int(fog_i) + fog_lut_offset).rg;
        float fog_factor = clamp(MAD(fog_f, fog_lut_entry.g, fog_lut_entry.r), 0.0, 1.0);
        combiner_output.rgb = mix(fog_color.rgb, combiner_output.rgb, fog_factor);
    } else if (fog_mode == 7u) { // Gas, unimplemented
        // Replace the output color with a transparent pixel (just discarding the pixel
        // causes graphical issues in some MH games).
        color = vec4(0.0);
        return;
    }

    uint logic_op = LOGIC_OP;
#if SHADOW_RENDERING
    uint d = uint(clamp(depth, 0.0, 1.0) * float(0xFFFFFF));
    uint s = uint(combiner_output.g * float(0xFF));
    ivec2 image_coord = ivec2(gl_FragCoord.xy);
#if INTERLOCK
    beginInvocationInterlockARB();
    uint old_shadow = imageLoad(shadow_buffer, image_coord).x;
    uint new_shadow = UpdateShadow(old_shadow, d, s);
    imageStore(shadow_buffer, image_coord, uvec4(new_shadow));
    endInvocationInterlockARB();
#else
    uint old = imageLoad(shadow_buffer, image_coord).x;
    uint new1;
    uint old2;
    do {
        old2 = old;
        new1 = UpdateShadow(old, d, s);
    } while ((old = imageAtomicCompSwap(shadow_buffer, image_coord, old, new1)) != old2);
#endif
#else
    // Round the final fragment color to maintain the PICA's 8 bits of precision
    color = byteround(combiner_output);
#endif
    // Logic operations the driver cannot do: Clear and Set replace the color; NoOp is done
    // with the color write mask and the others are not emulated.
    if (logic_op == 0u) {
        color = vec4(0.0);
    } else if (logic_op == 4u) {
        color = vec4(1.0);
    }
}
)";

} // namespace

UberKey UberKey::FromIndex(u32 index) {
    return UberKey{
        .tex0 = static_cast<Tex0>(index & 3u),
        .shadow_rendering = (index & 4u) != 0,
        .custom_normal = (index & 8u) != 0,
    };
}

bool UsesGlslSemantics(const UberKey& key, const Profile& profile) {
    // Same choice as ShaderDiskCache::UseFragmentShader and FSConfig::UsesSpirvIncompatibleConfig.
    return !profile.vk_use_spirv_generator || key.tex0 == UberKey::Tex0::ShadowCube ||
           key.shadow_rendering;
}

UberKey MakeUberKey(const FSConfig& config, const Pica::Shader::UserConfig& user,
                    const Profile& profile) {
    UberKey key;
    switch (config.texture.texture0_type.Value()) {
    case TextureType::TextureCube:
        key.tex0 = UberKey::Tex0::Cube;
        break;
    case TextureType::Shadow2D:
        key.tex0 = UberKey::Tex0::Shadow2D;
        break;
    case TextureType::ShadowCube:
        key.tex0 = UberKey::Tex0::ShadowCube;
        break;
    default:
        key.tex0 = UberKey::Tex0::Plain;
        break;
    }
    key.shadow_rendering = config.framebuffer.shadow_rendering != 0;
    // The SPIR-V generator never reads the custom normal map; only GLSL shaders do.
    key.custom_normal = user.use_custom_normal != 0 && config.lighting.enable != 0 &&
                        UsesGlslSemantics(key, profile);
    return key;
}

UberConfig MakeUberConfig(const FSConfig& config) {
    using Source = TevStageConfig::Source;
    UberConfig result;
    static_assert(sizeof(result.config) == sizeof(FSConfig));
    std::memcpy(result.config.data(), &config, sizeof(FSConfig));

    u32 textures = 0;
    const auto read = [&textures](Source source) {
        const u32 value = static_cast<u32>(source);
        if (value >= static_cast<u32>(Source::Texture0) &&
            value <= static_cast<u32>(Source::Texture3)) {
            textures |= 1u << (value - static_cast<u32>(Source::Texture0));
        }
    };
    u32 stages = 0;
    for (u32 i = 0; i < config.texture.tev_stages.size(); i++) {
        const TevStageConfig stage = config.texture.tev_stages[i];
        if (IsPassThrough(stage)) {
            result.tev |= 1u << i;
            continue;
        }
        stages = i + 1;
        // The first stage reads its third source where it is told to read Previous.
        const auto color_source = [&](Source source) {
            return i == 0 && source == Source::Previous ? stage.color_source3.Value() : source;
        };
        const auto alpha_source = [&](Source source) {
            return i == 0 && source == Source::Previous ? stage.alpha_source3.Value() : source;
        };
        read(color_source(stage.color_source1));
        read(color_source(stage.color_source2));
        read(stage.color_source3);
        if (stage.color_op != TevStageConfig::Operation::Dot3_RGBA) {
            read(alpha_source(stage.alpha_source1));
            read(alpha_source(stage.alpha_source2));
            read(stage.alpha_source3);
        }
    }
    result.tev |= stages << 8;

    const LightConfig& lighting = config.lighting;
    if (lighting.enable) {
        const auto bump_mode = lighting.bump_mode.Value();
        if (bump_mode == LightingRegs::LightingBumpMode::NormalMap ||
            bump_mode == LightingRegs::LightingBumpMode::TangentMap) {
            textures |= 1u << lighting.bump_selector;
        }
        if (lighting.enable_shadow) {
            textures |= 1u << lighting.shadow_selector;
        }

        // A table is used when it is enabled and the lighting configuration has it.
        const auto config_has = [&](LightingRegs::LightingSampler sampler) {
            return LightingRegs::IsLightingSamplerSupported(lighting.config, sampler);
        };
        using Sampler = LightingRegs::LightingSampler;
        const std::array<std::pair<const LutConfig&, Sampler>, 7> luts{{
            {lighting.lut_d0, Sampler::Distribution0},
            {lighting.lut_d1, Sampler::Distribution1},
            {lighting.lut_sp, Sampler::SpotlightAttenuation},
            {lighting.lut_fr, Sampler::Fresnel},
            {lighting.lut_rr, Sampler::ReflectRed},
            {lighting.lut_rg, Sampler::ReflectGreen},
            {lighting.lut_rb, Sampler::ReflectBlue},
        }};
        for (u32 i = 0; i < luts.size(); i++) {
            // The generators read the spotlight table whenever the configuration has it;
            // its enable bit is a placeholder that LightConfig always sets.
            const bool enabled =
                luts[i].second == Sampler::SpotlightAttenuation || luts[i].first.enable;
            if (enabled && config_has(luts[i].second)) {
                result.luts |= 1u << i;
            }
        }
    }
    result.textures = textures;
    return result;
}

std::string GenerateFragmentUbershader(const UberKey& key, const Profile& profile) {
    const bool glsl_semantics = UsesGlslSemantics(key, profile);
    const bool barycentric = profile.has_fragment_shader_barycentric;
    const bool interlock = key.shadow_rendering && profile.has_fragment_shader_interlock;

    std::string out;
    out.reserve(48 * 1024);
    out += "#extension GL_ARB_separate_shader_objects : enable\n";
    if (barycentric) {
        out += "#extension GL_EXT_fragment_shader_barycentric : enable\n";
    }
    if (interlock) {
        out += "#extension GL_ARB_fragment_shader_interlock : enable\n";
    }
    out += fmt::format("#define GLSL_SEMANTICS {}\n", glsl_semantics ? 1 : 0);
    out += fmt::format("#define BARYCENTRIC {}\n", barycentric ? 1 : 0);
    out += fmt::format("#define INTERLOCK {}\n", interlock ? 1 : 0);
    out += fmt::format("#define SHADOW_RENDERING {}\n", key.shadow_rendering ? 1 : 0);
    out += fmt::format("#define CUSTOM_NORMAL {}\n", key.custom_normal ? 1 : 0);
    out += "#define TEX0_PLAIN 0\n#define TEX0_CUBE 1\n#define TEX0_SHADOW_2D 2\n"
           "#define TEX0_SHADOW_CUBE 3\n";
    out += fmt::format("#define TEX0_CLASS {}\n", static_cast<u32>(key.tex0));
    out += fmt::format("#define LUT_D0_BIT {}u\n#define LUT_D1_BIT {}u\n#define LUT_SP_BIT {}u\n"
                       "#define LUT_FR_BIT {}u\n#define LUT_RR_BIT {}u\n#define LUT_RG_BIT {}u\n"
                       "#define LUT_RB_BIT {}u\n",
                       LUT_D0, LUT_D1, LUT_SP, LUT_FR, LUT_RR, LUT_RG, LUT_RB);

    // Inputs and output: the same locations as Azahar's generators (Semantic enum).
    out += R"(
layout (location = 1) in vec4 primary_color;
layout (location = 2) in vec2 texcoord0;
layout (location = 3) in vec2 texcoord1;
layout (location = 4) in vec2 texcoord2;
layout (location = 5) in float texcoord0_w;
layout (location = 7) in vec3 view;
layout (location = 0) out vec4 color;
)";
    out += barycentric ? "layout (location = 6) pervertexEXT in vec4 normquats[];\n"
                         "#define gl_BaryCoord gl_BaryCoordEXT\n"
                       : "layout (location = 6) in vec4 normquat;\n";

    // Bindings: Azahar's, then the configuration buffer.
    out += Pica::Shader::Generator::GLSL::GetFSUniformBlockDef();
    out += "layout(set = 0, binding = 3) uniform samplerBuffer texture_buffer_lut_lf;\n"
           "layout(set = 0, binding = 4) uniform samplerBuffer texture_buffer_lut_rg;\n"
           "layout(set = 0, binding = 5) uniform samplerBuffer texture_buffer_lut_rgba;\n";
    out += fmt::format("layout(set = 0, binding = {}, std140) uniform fs_uber_config {{\n"
                       "    uvec4 uber_cfg[{}];\n}};\n",
                       UBER_CONFIG_BINDING, sizeof(UberConfig) / 16);
    switch (key.tex0) {
    case UberKey::Tex0::Plain:
        out += "layout(set = 1, binding = 0) uniform sampler2D tex0;\n";
        break;
    case UberKey::Tex0::Cube:
        out += "layout(set = 1, binding = 0) uniform samplerCube tex0;\n";
        break;
    case UberKey::Tex0::Shadow2D:
        out += "layout(set = 1, binding = 0) uniform usampler2D tex0;\n";
        break;
    case UberKey::Tex0::ShadowCube:
        out += "layout(set = 1, binding = 0) uniform usampler2D tex0[6];\n";
        break;
    }
    out += "layout(set = 1, binding = 1) uniform sampler2D tex1;\n"
           "layout(set = 1, binding = 2) uniform sampler2D tex2;\n";
    if (key.shadow_rendering) {
        out += "layout(set = 2, binding = 0, r32ui) uniform uimage2D shadow_buffer;\n";
    }
    if (key.custom_normal) {
        out += "layout(set = 2, binding = 1) uniform sampler2D tex_normal;\n";
    }

    DefineConfigFields(out);
    out += BODY;
    return out;
}

} // namespace Claudhar::Shading
