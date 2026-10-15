// Copyright 2026 Claudhar Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

// Vulkan half of the device probe. It answers the questions the zero-compile
// renderer design depends on, on the device's real driver:
//  - Which features exist for making blending dynamic (method A), reading the
//    framebuffer in the shader (method C) or linking prebuilt pipeline parts
//    (graphics pipeline libraries)?
//  - How long does a cold ubershader pipeline take, are pipelines that differ
//    only in blend state cheap after it, and how fast does a "Preparing graphics"
//    pass build a whole variant set on all cores (method B)?
//  - Does the saved pipeline cache really make the next build compile-free?
//  - What does the ubershader cost per pixel against a specialized shader?
// It uses its own instance, device and dispatcher and destroys them on return.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <random>
#include <set>
#include <span>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>
#include <fmt/format.h>
#include "claudhar/probe/probe.h"
#include "common/common_types.h"
#include "common/dynamic_library/dynamic_library.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Claudhar::Probe {
namespace {

using Clock = std::chrono::steady_clock;
using Dispatch = vk::detail::DispatchLoaderDynamic;

constexpr vk::Format ColorFormat = vk::Format::eR8G8B8A8Unorm;
constexpr u32 Width = 800, Height = 480; // The 3DS top screen at 2x.
constexpr u32 TextureSize = 256;

/// Milliseconds since start.
double MsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

/// Full-screen triangle per instance; each instance is one overdraw layer. The
/// varyings stand in for the colors, coordinates and vectors of 3DS vertices.
constexpr const char* VertexShader = R"(#version 450
layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_uv0;
layout(location = 2) out vec2 v_uv1;
layout(location = 3) out vec2 v_uv2;
layout(location = 4) out vec3 v_normal;
layout(location = 5) out vec3 v_view;
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    float layer = float(gl_InstanceIndex);
    gl_Position = vec4(p * 2.0 - 1.0, 0.5, 1.0);
    v_color = vec4(p * 0.5, 0.5 + 0.01 * layer, 0.9);
    v_uv0 = p * 2.0 + layer * 0.01;
    v_uv1 = p.yx * 3.0;
    v_uv2 = p * 0.5;
    v_normal = vec3(p - 0.5, 1.0);
    v_view = vec3(0.3, -0.2, 1.0) + vec3(p, 0.0);
}
)";

// The probe's fragment shaders are assembled from the pieces below:
//   ubershader:  header + interface + UberConfig  + common + UberMain
//   specialized: header + SpecializedConstants() + interface + common + UberMain
//   compact:     header + interface + CompactConfig + common + CompactMain
// The ubershader and the compact ubershader read the configuration from uniform
// buffers; the specialized shader bakes the same configuration in as constants.

/// Inputs from the vertex shader, the output, the textures and the lookup tables.
constexpr const char* FragmentInterface = R"(
layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv0;
layout(location = 2) in vec2 v_uv1;
layout(location = 3) in vec2 v_uv2;
layout(location = 4) in vec3 v_normal;
layout(location = 5) in vec3 v_view;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 1) uniform sampler2D tex[3];
layout(set = 0, binding = 2) uniform samplerBuffer lut;
)";

/// The ubershader's configuration: packed combiner stages decoded with switches.
constexpr const char* UberConfig = R"(
#ifndef SPECIALIZED
layout(set = 0, binding = 0, std140) uniform Config {
    uvec4 stage_cfg[6];
    vec4 stage_const[6];
    uvec4 misc_cfg;
    vec4 light_dir[8];
    vec4 light_diffuse[8];
    vec4 light_specular[8];
};
#endif
)";

/// The compact ubershader's configuration, decoded on the CPU into indices, masks
/// and coefficients so the shader needs no switch per operand or operation.
constexpr const char* CompactConfig = R"(
layout(set = 0, binding = 3, std140) uniform Compact {
    uvec4 c_src[6][2];    // [0].xyz color and [1].xyz alpha sources: candidate indices
    vec4 c_rgb_sel[6][3]; // color operand channel weights; zero when it passes rgb through
    vec4 c_rgb_pass[6];   // xyz: 1 when the color operand passes rgb through
    vec4 c_rgb_inv[6];    // xyz: 1 when the color operand is 1 - x
    vec4 c_a_sel[6][3];   // alpha operand channel weights
    vec4 c_a_inv[6];      // xyz: 1 when the alpha operand is 1 - x
    vec4 c_rgb_k[6][2];   // k0 a + k1 b + k2 c + k3 ab + k4 ac + k5 bc + k6; [1].w: dot3
    vec4 c_a_k[6][2];     // the same for alpha
    vec4 c_scale[6];      // x, y: color and alpha scale; z, w: 1 to update the buffer
    vec4 stage_const[6];
    uvec4 misc_cfg;
    vec4 light_dir[8];
    vec4 light_diffuse[8];
    vec4 light_specular[8];
};
)";

/// Lighting, fog and alpha test, shared by every probe fragment shader.
constexpr const char* FragmentCommon = R"(
// Per-light diffuse plus specular from two lookup tables (D0 and RR).
void Lighting(out vec4 ld, out vec4 ls) {
    vec3 n = normalize(v_normal);
    vec3 view = normalize(v_view);
    vec3 diffuse = vec3(0.1);
    vec3 specular = vec3(0.0);
    for (uint i = 0u; i < misc_cfg.y; ++i) {
        vec3 l = normalize(light_dir[i].xyz);
        float nl = max(dot(n, l), 0.0);
        float d0 = texelFetch(lut, int(clamp(dot(n, normalize(l + view)), 0.0, 1.0) * 255.0)).r;
        float rr = texelFetch(lut, 256 + int(clamp(dot(view, n), 0.0, 1.0) * 255.0)).r;
        diffuse += light_diffuse[i].rgb * nl;
        specular += light_specular[i].rgb * d0 * rr * nl;
    }
    ld = vec4(clamp(diffuse, 0.0, 1.0), 1.0);
    ls = vec4(clamp(specular, 0.0, 1.0), 1.0);
}

// Fog, then the alpha test, which may discard the fragment.
vec4 Finish(vec4 color) {
    if (misc_cfg.w != 0u) {
        float f = texelFetch(lut, 512 + int(clamp(gl_FragCoord.z, 0.0, 1.0) * 255.0)).r;
        color.rgb = mix(vec3(0.5, 0.6, 0.7), color.rgb, f);
    }
    // Never true (fragment centers are never whole numbers), but makes each probe
    // run's binary unique, so no driver cache can hide a cold compile.
    if (gl_FragCoord.x == float(NONCE)) color.r = 1.0 - color.r;
    bool pass;
    switch (misc_cfg.z) {
    case 0u: pass = false; break;
    case 1u: pass = true; break;
    case 2u: pass = color.a == 0.1; break;
    case 3u: pass = color.a != 0.1; break;
    case 4u: pass = color.a < 0.1; break;
    case 5u: pass = color.a <= 0.1; break;
    case 6u: pass = color.a > 0.1; break;
    default: pass = color.a >= 0.1; break;
    }
    if (!pass) discard;
    return color;
}
)";

/// The ubershader's combiner stages: each source, operand and operation is chosen
/// by a switch on the packed configuration.
constexpr const char* UberMain = R"(
vec4 Source(uint s, vec4 prev, vec4 buf, vec4 k, vec4 t0, vec4 t1, vec4 t2, vec4 ld, vec4 ls) {
    switch (s) {
    case 0u: return v_color;
    case 1u: return ld;
    case 2u: return ls;
    case 3u: return t0;
    case 4u: return t1;
    case 5u: return t2;
    case 13u: return buf;
    case 14u: return k;
    default: return prev;
    }
}

vec3 OperandRgb(uint op, vec4 v) {
    switch (op) {
    case 0u: return v.rgb;
    case 1u: return 1.0 - v.rgb;
    case 2u: return v.aaa;
    case 3u: return vec3(1.0 - v.a);
    case 4u: return v.rrr;
    case 5u: return vec3(1.0 - v.r);
    case 8u: return v.ggg;
    case 9u: return vec3(1.0 - v.g);
    case 12u: return v.bbb;
    default: return vec3(1.0 - v.b);
    }
}

float OperandA(uint op, vec4 v) {
    switch (op) {
    case 0u: return v.a;
    case 1u: return 1.0 - v.a;
    case 2u: return v.r;
    case 3u: return 1.0 - v.r;
    case 4u: return v.g;
    case 5u: return 1.0 - v.g;
    case 6u: return v.b;
    default: return 1.0 - v.b;
    }
}

vec3 CombineRgb(uint op, vec3 a, vec3 b, vec3 c) {
    switch (op) {
    case 0u: return a;
    case 1u: return a * b;
    case 2u: return a + b;
    case 3u: return a + b - 0.5;
    case 4u: return mix(b, a, c);
    case 5u: return a - b;
    case 6u: case 7u: return vec3(min(dot(a - 0.5, b - 0.5) * 4.0, 1.0));
    case 8u: return a * b + c;
    default: return (a + b) * c;
    }
}

float CombineA(uint op, float a, float b, float c) {
    switch (op) {
    case 0u: return a;
    case 1u: return a * b;
    case 2u: return a + b;
    case 3u: return a + b - 0.5;
    case 4u: return mix(b, a, c);
    case 5u: return a - b;
    case 8u: return a * b + c;
    default: return (a + b) * c;
    }
}

void main() {
    vec4 t0 = texture(tex[0], v_uv0);
    vec4 t1 = texture(tex[1], v_uv1);
    vec4 t2 = texture(tex[2], v_uv2);
    vec4 ld, ls;
    Lighting(ld, ls);

    // Texture combiner stages. The combiner buffer lags one stage, as on the 3DS.
    vec4 prev = v_color;
    vec4 buf = vec4(0.0);
    vec4 next_buf = vec4(0.0);
    for (uint i = 0u; i < misc_cfg.x; ++i) {
        uvec4 c = stage_cfg[i];
        vec4 k = stage_const[i];
        buf = next_buf;
        vec3 rgb = CombineRgb(bitfieldExtract(c.z, 0, 4),
            OperandRgb(bitfieldExtract(c.y, 0, 4), Source(bitfieldExtract(c.x, 0, 4), prev, buf, k, t0, t1, t2, ld, ls)),
            OperandRgb(bitfieldExtract(c.y, 4, 4), Source(bitfieldExtract(c.x, 4, 4), prev, buf, k, t0, t1, t2, ld, ls)),
            OperandRgb(bitfieldExtract(c.y, 8, 4), Source(bitfieldExtract(c.x, 8, 4), prev, buf, k, t0, t1, t2, ld, ls)));
        float a = CombineA(bitfieldExtract(c.z, 4, 4),
            OperandA(bitfieldExtract(c.y, 12, 3), Source(bitfieldExtract(c.x, 12, 4), prev, buf, k, t0, t1, t2, ld, ls)),
            OperandA(bitfieldExtract(c.y, 15, 3), Source(bitfieldExtract(c.x, 16, 4), prev, buf, k, t0, t1, t2, ld, ls)),
            OperandA(bitfieldExtract(c.y, 18, 3), Source(bitfieldExtract(c.x, 20, 4), prev, buf, k, t0, t1, t2, ld, ls)));
        prev = vec4(clamp(rgb * float(1u << bitfieldExtract(c.z, 8, 2)), 0.0, 1.0),
                    clamp(a * float(1u << bitfieldExtract(c.z, 10, 2)), 0.0, 1.0));
        if (bitfieldExtract(c.z, 12, 1) != 0u) next_buf.rgb = prev.rgb;
        if (bitfieldExtract(c.z, 13, 1) != 0u) next_buf.a = prev.a;
    }
    o_color = Finish(prev);
}
)";

/// The compact ubershader's combiner stages: sources are read from a candidate list
/// by index, operands are masks and every operation but dot3 is one weighted sum,
/// so all configurations run the same short instruction sequence.
constexpr const char* CompactMain = R"(
void main() {
    vec4 ld, ls;
    Lighting(ld, ls);
    // Candidate sources, in the order the compact configuration indexes them.
    vec4 cand[9];
    cand[0] = v_color;
    cand[1] = ld;
    cand[2] = ls;
    cand[3] = texture(tex[0], v_uv0);
    cand[4] = texture(tex[1], v_uv1);
    cand[5] = texture(tex[2], v_uv2);
    vec4 prev = v_color;
    vec4 next_buf = vec4(0.0);
    for (uint i = 0u; i < misc_cfg.x; ++i) {
        cand[6] = next_buf; // The combiner buffer lags one stage, as on the 3DS.
        cand[7] = stage_const[i];
        cand[8] = prev;
        vec3 x[3];
        float y[3];
        for (int j = 0; j < 3; ++j) {
            vec4 s = cand[c_src[i][0][j]];
            vec3 v = c_rgb_pass[i][j] * s.rgb + vec3(dot(s, c_rgb_sel[i][j]));
            x[j] = mix(v, 1.0 - v, c_rgb_inv[i][j]);
            float w = dot(cand[c_src[i][1][j]], c_a_sel[i][j]);
            y[j] = mix(w, 1.0 - w, c_a_inv[i][j]);
        }
        vec4 k0 = c_rgb_k[i][0];
        vec4 k1 = c_rgb_k[i][1];
        vec3 rgb = k0.x * x[0] + k0.y * x[1] + k0.z * x[2] + k0.w * x[0] * x[1] +
                   k1.x * x[0] * x[2] + k1.y * x[1] * x[2] + k1.z;
        if (k1.w != 0.0) rgb = vec3(min(dot(x[0] - 0.5, x[1] - 0.5) * 4.0, 1.0));
        vec4 q0 = c_a_k[i][0];
        vec4 q1 = c_a_k[i][1];
        float a = q0.x * y[0] + q0.y * y[1] + q0.z * y[2] + q0.w * y[0] * y[1] +
                  q1.x * y[0] * y[2] + q1.y * y[1] * y[2] + q1.z;
        vec4 sc = c_scale[i];
        prev = vec4(clamp(rgb * sc.x, 0.0, 1.0), clamp(a * sc.y, 0.0, 1.0));
        next_buf = vec4(mix(next_buf.rgb, prev.rgb, sc.z), mix(next_buf.a, prev.a, sc.w));
    }
    o_color = Finish(prev);
}
)";

/// The shader configuration, laid out exactly like the std140 Config block.
struct Config {
    std::array<std::array<u32, 4>, 6> stage_cfg{};
    std::array<std::array<float, 4>, 6> stage_const{};
    std::array<u32, 4> misc_cfg{};
    std::array<std::array<float, 4>, 8> light_dir{}, light_diffuse{}, light_specular{};
};

/// Packs one combiner stage: three color and three alpha sources, their operands,
/// the two combine operations, scales and combiner-buffer updates.
std::array<u32, 4> Stage(std::array<u32, 6> src, std::array<u32, 6> operand, u32 rgb_op, u32 a_op,
                         u32 rgb_scale, bool buffer_rgb, bool buffer_a) {
    return {src[0] | src[1] << 4 | src[2] << 8 | src[3] << 12 | src[4] << 16 | src[5] << 20,
            operand[0] | operand[1] << 4 | operand[2] << 8 | operand[3] << 12 | operand[4] << 15 |
                operand[5] << 18,
            rgb_op | a_op << 4 | rgb_scale << 8 | u32{buffer_rgb} << 12 | u32{buffer_a} << 13, 0};
}

/// A typical lit, textured, alpha-tested 3DS material: four stages, two lights, fog.
Config TestConfig() {
    Config c{};
    // Sources: 0 vertex color, 1 lit diffuse, 2 lit specular, 3-5 textures,
    // 13 combiner buffer, 14 constant, 15 previous stage.
    c.stage_cfg[0] = Stage({3, 1, 15, 3, 0, 15}, {}, 1, 1, 0, false, false);
    c.stage_cfg[1] = Stage({15, 2, 15, 15, 15, 15}, {}, 2, 0, 0, true, true);
    c.stage_cfg[2] = Stage({15, 4, 14, 15, 14, 15}, {0, 0, 2, 0, 0, 0}, 4, 1, 0, false, false);
    c.stage_cfg[3] = Stage({15, 13, 5, 15, 13, 15}, {}, 8, 2, 0, false, false);
    c.stage_const[2] = {0.8f, 0.7f, 0.6f, 0.5f};
    c.misc_cfg = {4, 2, 6, 1}; // Stages, lights, alpha test "greater", fog on.
    c.light_dir[0] = {0.3f, 0.5f, 1.0f, 0.0f};
    c.light_dir[1] = {-0.6f, 0.2f, 0.8f, 0.0f};
    c.light_diffuse[0] = {0.6f, 0.6f, 0.6f, 0.0f};
    c.light_diffuse[1] = {0.3f, 0.2f, 0.4f, 0.0f};
    c.light_specular[0] = {0.5f, 0.5f, 0.5f, 0.0f};
    c.light_specular[1] = {0.2f, 0.3f, 0.1f, 0.0f};
    return c;
}

/// GLSL constant declarations that bake a Config into the specialized shader.
std::string SpecializedConstants(const Config& c) {
    const auto list = [](const auto& rows, const char* type, const char* suffix) {
        std::string out;
        for (const auto& row : rows)
            out += fmt::format("{}{}({}{}, {}{}, {}{}, {}{})", out.empty() ? "" : ", ", type,
                               row[0], suffix, row[1], suffix, row[2], suffix, row[3], suffix);
        return out;
    };
    return fmt::format("#define SPECIALIZED\n"
                       "const uvec4 stage_cfg[6] = uvec4[6]({});\n"
                       "const vec4 stage_const[6] = vec4[6]({});\n"
                       "const uvec4 misc_cfg = {};\n"
                       "const vec4 light_dir[8] = vec4[8]({});\n"
                       "const vec4 light_diffuse[8] = vec4[8]({});\n"
                       "const vec4 light_specular[8] = vec4[8]({});\n",
                       list(c.stage_cfg, "uvec4", "u"), list(c.stage_const, "vec4", ""),
                       list(std::array<std::array<u32, 4>, 1>{c.misc_cfg}, "uvec4", "u"),
                       list(c.light_dir, "vec4", ""), list(c.light_diffuse, "vec4", ""),
                       list(c.light_specular, "vec4", ""));
}

/// The compact ubershader's configuration, laid out exactly like its std140 block.
struct CompactData {
    std::array<std::array<std::array<u32, 4>, 2>, 6> src{};
    std::array<std::array<std::array<float, 4>, 3>, 6> rgb_sel{};
    std::array<std::array<float, 4>, 6> rgb_pass{}, rgb_inv{};
    std::array<std::array<std::array<float, 4>, 3>, 6> a_sel{};
    std::array<std::array<float, 4>, 6> a_inv{};
    std::array<std::array<std::array<float, 4>, 2>, 6> rgb_k{}, a_k{};
    std::array<std::array<float, 4>, 6> scale{}, stage_const{};
    std::array<u32, 4> misc_cfg{};
    std::array<std::array<float, 4>, 8> light_dir{}, light_diffuse{}, light_specular{};
};

/// Combine coefficients (k0 a + k1 b + k2 c + k3 ab + k4 ac + k5 bc + k6, then a dot3
/// flag) for one PICA combine operation, matching UberMain's CombineRgb/CombineA.
std::array<std::array<float, 4>, 2> Coefficients(u32 op, bool color) {
    switch (op) {
    case 0:
        return {{{1, 0, 0, 0}, {0, 0, 0, 0}}}; // a
    case 1:
        return {{{0, 0, 0, 1}, {0, 0, 0, 0}}}; // a * b
    case 2:
        return {{{1, 1, 0, 0}, {0, 0, 0, 0}}}; // a + b
    case 3:
        return {{{1, 1, 0, 0}, {0, 0, -0.5f, 0}}}; // a + b - 0.5
    case 4:
        return {{{0, 1, 0, 0}, {1, -1, 0, 0}}}; // mix(b, a, c) = b + ac - bc
    case 5:
        return {{{1, -1, 0, 0}, {0, 0, 0, 0}}}; // a - b
    case 6:
    case 7:
        if (color)
            return {{{0, 0, 0, 0}, {0, 0, 0, 1}}}; // dot3
        [[fallthrough]];
    default:
        return {{{0, 0, 0, 0}, {1, 1, 0, 0}}}; // (a + b) * c
    case 8:
        return {{{0, 0, 1, 1}, {0, 0, 0, 0}}}; // a * b + c
    }
}

/// Decodes the ubershader's packed configuration into the compact shader's indices,
/// masks and coefficients, so both shaders draw the same material.
CompactData ToCompact(const Config& c) {
    CompactData k{};
    // Candidate list order in CompactMain: sources 0-5 as is, then buffer, constant, previous.
    const auto candidate = [](u32 code) {
        return code <= 5 ? code : code == 13 ? 6U : code == 14 ? 7U : 8U;
    };
    for (std::size_t i = 0; i < 6; ++i) {
        const auto& [x, y, z, w] = c.stage_cfg[i];
        for (u32 j = 0; j < 3; ++j) {
            k.src[i][0][j] = candidate(x >> (4 * j) & 15);
            k.src[i][1][j] = candidate(x >> (12 + 4 * j) & 15);
            // Color operands: rgb, aaa, rrr, ggg or bbb, each optionally inverted.
            const u32 op = y >> (4 * j) & 15;
            const int channel = op <= 1 ? -1 : op <= 3 ? 3 : op <= 5 ? 0 : op <= 9 ? 1 : 2;
            k.rgb_pass[i][j] = channel < 0 ? 1.0f : 0.0f;
            if (channel >= 0)
                k.rgb_sel[i][j][channel] = 1.0f;
            const bool known = op <= 5 || op == 8 || op == 9 || op == 12;
            k.rgb_inv[i][j] = known ? static_cast<float>(op & 1) : 1.0f;
            // Alpha operands: a, r, g or b, each optionally inverted.
            const u32 alpha_op = y >> (12 + 3 * j) & 7;
            k.a_sel[i][j][alpha_op < 2 ? 3 : (alpha_op >> 1) - 1] = 1.0f;
            k.a_inv[i][j] = static_cast<float>(alpha_op & 1);
        }
        k.rgb_k[i] = Coefficients(z & 15, true);
        k.a_k[i] = Coefficients(z >> 4 & 15, false);
        k.scale[i] = {static_cast<float>(1U << (z >> 8 & 3)),
                      static_cast<float>(1U << (z >> 10 & 3)), static_cast<float>(z >> 12 & 1),
                      static_cast<float>(z >> 13 & 1)};
        (void)w;
    }
    k.stage_const = c.stage_const;
    k.misc_cfg = c.misc_cfg;
    k.light_dir = c.light_dir;
    k.light_diffuse = c.light_diffuse;
    k.light_specular = c.light_specular;
    return k;
}

/// The probe's three fragment shaders.
enum class Shader { Uber, Compact, Specialized };

/// Assembles one probe fragment shader's GLSL. The nonce makes its binary unique.
std::string FragmentSource(Shader shader, u32 nonce) {
    std::string out = fmt::format("#version 450\n#define NONCE {}u\n", nonce);
    if (shader == Shader::Specialized)
        out += SpecializedConstants(TestConfig());
    out += FragmentInterface;
    out += shader == Shader::Compact ? CompactConfig : UberConfig;
    out += FragmentCommon;
    out += shader == Shader::Compact ? CompactMain : UberMain;
    return out;
}

/// The probe's Vulkan objects. Destroyers run in reverse creation order, then the
/// device and instance, whatever path the probe leaves by.
struct Context {
    Common::DynamicLibrary library;
    Dispatch d;
    vk::Instance instance;
    vk::PhysicalDevice gpu;
    vk::Device device;
    vk::Queue queue;
    u32 queue_family = 0;
    vk::PhysicalDeviceProperties props;
    u32 timestamp_bits = 0; ///< Valid bits of the graphics queue's timestamps.
    bool logic_op = false, dynamic_blend = false, dynamic_logic_op = false;
    bool pipeline_library = false, cache_control = false;
    std::vector<std::function<void()>> destroyers;

    // Uses the raw wait so a lost device cannot throw out of the destructor.
    ~Context() {
        if (device)
            d.vkDeviceWaitIdle(device);
        for (auto it = destroyers.rbegin(); it != destroyers.rend(); ++it)
            (*it)();
        if (device)
            device.destroy(nullptr, d);
        if (instance)
            instance.destroy(nullptr, d);
    }

    /// Registers how to destroy a handle created from the device.
    template <typename Handle>
    Handle Own(Handle handle) {
        destroyers.emplace_back([this, handle] { device.destroy(handle, nullptr, d); });
        return handle;
    }
};

/// Allocates and binds memory with the wanted properties for a buffer or image.
template <typename Handle>
void Bind(Context& c, Handle handle, vk::MemoryPropertyFlags want, void** mapped = nullptr) {
    constexpr bool is_buffer = std::is_same_v<Handle, vk::Buffer>;
    vk::MemoryRequirements req;
    if constexpr (is_buffer)
        req = c.device.getBufferMemoryRequirements(handle, c.d);
    else
        req = c.device.getImageMemoryRequirements(handle, c.d);
    const auto types = c.gpu.getMemoryProperties(c.d);
    for (u32 i = 0; i < types.memoryTypeCount; ++i) {
        if (!(req.memoryTypeBits >> i & 1) || (types.memoryTypes[i].propertyFlags & want) != want)
            continue;
        const auto memory = c.device.allocateMemory(
            {.allocationSize = req.size, .memoryTypeIndex = i}, nullptr, c.d);
        c.destroyers.emplace_back([&c, memory] { c.device.freeMemory(memory, nullptr, c.d); });
        if constexpr (is_buffer)
            c.device.bindBufferMemory(handle, memory, 0, c.d);
        else
            c.device.bindImageMemory(handle, memory, 0, c.d);
        if (mapped)
            *mapped = c.device.mapMemory(memory, 0, req.size, {}, c.d);
        return;
    }
    throw std::runtime_error("no memory type with the needed properties");
}

/// A host-visible, coherent buffer filled with data (or zeroed); returns its mapping.
vk::Buffer HostBuffer(Context& c, vk::BufferUsageFlags usage, std::size_t size, const void* data,
                      void** mapped_out = nullptr) {
    const auto buffer = c.Own(c.device.createBuffer({.size = size, .usage = usage}, nullptr, c.d));
    void* mapped = nullptr;
    Bind(c, buffer,
         vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
         &mapped);
    data ? std::memcpy(mapped, data, size) : std::memset(mapped, 0, size);
    if (mapped_out)
        *mapped_out = mapped;
    return buffer;
}

/// A device-local 2D RGBA8 image with a view.
std::pair<vk::Image, vk::ImageView> DeviceImage(Context& c, u32 width, u32 height,
                                                vk::ImageUsageFlags usage) {
    const auto image = c.Own(c.device.createImage({.imageType = vk::ImageType::e2D,
                                                   .format = ColorFormat,
                                                   .extent = {width, height, 1},
                                                   .mipLevels = 1,
                                                   .arrayLayers = 1,
                                                   .samples = vk::SampleCountFlagBits::e1,
                                                   .tiling = vk::ImageTiling::eOptimal,
                                                   .usage = usage},
                                                  nullptr, c.d));
    Bind(c, image, vk::MemoryPropertyFlagBits::eDeviceLocal);
    const auto view = c.Own(c.device.createImageView(
        {.image = image,
         .viewType = vk::ImageViewType::e2D,
         .format = ColorFormat,
         .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}},
        nullptr, c.d));
    return {image, view};
}

/// Records a layout change of a whole color image, waiting on all earlier work.
void Transition(Context& c, vk::CommandBuffer cmd, vk::Image image, vk::ImageLayout from,
                vk::ImageLayout to) {
    const vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .oldLayout = from,
        .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier}, c.d);
}

/// Compiles GLSL to a shader module, timing the CPU-side GLSL to SPIR-V step.
vk::ShaderModule Module(Context& c, const std::string& source, vk::ShaderStageFlagBits stage,
                        double* glslang_ms = nullptr) {
    const auto start = Clock::now();
    const auto spirv = Vulkan::CompileGLSL(source, stage);
    if (glslang_ms)
        *glslang_ms = MsSince(start);
    if (spirv.empty())
        throw std::runtime_error("probe shader failed to compile");
    return c.Own(c.device.createShaderModule(
        {.codeSize = spirv.size() * sizeof(u32), .pCode = spirv.data()}, nullptr, c.d));
}

/// One blend state of the kind the 3DS can set: factors, equations, write mask or
/// a logic op. Deterministic, so a variant index always means the same state.
struct Variant {
    vk::PipelineColorBlendAttachmentState blend;
    bool logic = false;
    vk::LogicOp op = vk::LogicOp::eCopy;
};

Variant MakeVariant(const Context& c, u32 i) {
    using F = vk::BlendFactor;
    using O = vk::BlendOp;
    static constexpr std::array factors{F::eZero,
                                        F::eOne,
                                        F::eSrcColor,
                                        F::eOneMinusSrcColor,
                                        F::eDstColor,
                                        F::eOneMinusDstColor,
                                        F::eSrcAlpha,
                                        F::eOneMinusSrcAlpha,
                                        F::eDstAlpha,
                                        F::eOneMinusDstAlpha,
                                        F::eConstantColor,
                                        F::eOneMinusConstantColor,
                                        F::eConstantAlpha,
                                        F::eOneMinusConstantAlpha,
                                        F::eSrcAlphaSaturate};
    static constexpr std::array ops{O::eAdd, O::eSubtract, O::eReverseSubtract, O::eMin, O::eMax};
    using C = vk::ColorComponentFlagBits;
    Variant v;
    v.blend = {.blendEnable = VK_TRUE,
               .srcColorBlendFactor = factors[(i * 7) % 15],
               .dstColorBlendFactor = factors[(i * 11 + 3) % 15],
               .colorBlendOp = ops[i % 5],
               .srcAlphaBlendFactor = factors[(i * 5 + 1) % 15],
               .dstAlphaBlendFactor = factors[(i * 3 + 7) % 15],
               .alphaBlendOp = ops[(i / 5) % 5],
               .colorWriteMask =
                   i % 4 == 3 ? C::eR | C::eG | C::eB : C::eR | C::eG | C::eB | C::eA};
    if (i % 8 == 7 && c.logic_op) {
        v.logic = true;
        v.op = static_cast<vk::LogicOp>((i / 8) % 16);
        v.blend.blendEnable = VK_FALSE;
    }
    return v;
}

/// The ordinary alpha blend used by the per-pixel cost test.
Variant AlphaBlend() {
    using F = vk::BlendFactor;
    Variant v;
    v.blend = {.blendEnable = VK_TRUE,
               .srcColorBlendFactor = F::eSrcAlpha,
               .dstColorBlendFactor = F::eOneMinusSrcAlpha,
               .colorBlendOp = vk::BlendOp::eAdd,
               .srcAlphaBlendFactor = F::eOne,
               .dstAlphaBlendFactor = F::eZero,
               .alphaBlendOp = vk::BlendOp::eAdd,
               .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                 vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA};
    return v;
}

/// Fixed state shared by every probe pipeline; only blending varies.
struct FixedState {
    vk::PipelineVertexInputStateCreateInfo vertex_input{};
    vk::PipelineInputAssemblyStateCreateInfo assembly{.topology =
                                                          vk::PrimitiveTopology::eTriangleList};
    vk::PipelineViewportStateCreateInfo viewport{.viewportCount = 1, .scissorCount = 1};
    vk::PipelineRasterizationStateCreateInfo raster{.cullMode = vk::CullModeFlagBits::eNone,
                                                    .lineWidth = 1.0f};
    vk::PipelineMultisampleStateCreateInfo multisample{.rasterizationSamples =
                                                           vk::SampleCountFlagBits::e1};
    vk::PipelineDepthStencilStateCreateInfo depth_stencil{}; ///< No depth: the probe has none.
};

/// Throws with the call's name when a Vulkan call that reports a result fails.
void Check(vk::Result result, const char* what) {
    if (result != vk::Result::eSuccess)
        throw std::runtime_error(fmt::format("{}: {}", what, vk::to_string(result)));
}

/// The outcome of one pipeline build.
struct Build {
    vk::Pipeline pipeline;
    double ms = 0.0;
    bool compile_required = false; ///< Refused because it would have compiled.
    bool cache_hit = false;        ///< The driver says the pipeline cache supplied it.
};

/// Builds one complete pipeline for a blend variant and times it. Safe to call from
/// several threads at once; the caller owns the returned pipeline. With
/// fail_on_compile it reports whether the build would have needed a compile instead
/// of compiling; with dynamic_blend, blending is left to command-buffer state.
Build CreatePipeline(const Context& c, vk::ShaderModule vs, vk::ShaderModule fs,
                     vk::PipelineLayout layout, vk::PipelineCache cache, const Variant& v,
                     bool fail_on_compile = false, bool dynamic_blend = false) {
    const FixedState fixed;
    const std::array stages{
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eVertex, .module = vs, .pName = "main"},
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment, .module = fs, .pName = "main"}};
    const vk::PipelineColorBlendStateCreateInfo blend{
        .logicOpEnable = v.logic, .logicOp = v.op, .attachmentCount = 1, .pAttachments = &v.blend};
    std::vector<vk::DynamicState> dynamic{vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    if (dynamic_blend) {
        dynamic.insert(dynamic.end(), {vk::DynamicState::eColorBlendEnableEXT,
                                       vk::DynamicState::eColorBlendEquationEXT,
                                       vk::DynamicState::eColorWriteMaskEXT});
        if (c.dynamic_logic_op)
            dynamic.insert(dynamic.end(),
                           {vk::DynamicState::eLogicOpEnableEXT, vk::DynamicState::eLogicOpEXT});
    }
    const vk::PipelineDynamicStateCreateInfo dynamic_state{
        .dynamicStateCount = static_cast<u32>(dynamic.size()), .pDynamicStates = dynamic.data()};
    vk::PipelineCreationFeedback feedback{};
    const vk::PipelineCreationFeedbackCreateInfo feedback_info{.pPipelineCreationFeedback =
                                                                   &feedback};
    const vk::PipelineRenderingCreateInfo rendering{.pNext = &feedback_info,
                                                    .colorAttachmentCount = 1,
                                                    .pColorAttachmentFormats = &ColorFormat};
    const vk::GraphicsPipelineCreateInfo info{
        .pNext = &rendering,
        .flags = fail_on_compile ? vk::PipelineCreateFlagBits::eFailOnPipelineCompileRequired
                                 : vk::PipelineCreateFlags{},
        .stageCount = static_cast<u32>(stages.size()),
        .pStages = stages.data(),
        .pVertexInputState = &fixed.vertex_input,
        .pInputAssemblyState = &fixed.assembly,
        .pViewportState = &fixed.viewport,
        .pRasterizationState = &fixed.raster,
        .pMultisampleState = &fixed.multisample,
        .pColorBlendState = &blend,
        .pDynamicState = &dynamic_state,
        .layout = layout};
    Build build;
    const auto start = Clock::now();
    const auto result =
        c.device.createGraphicsPipelines(cache, 1, &info, nullptr, &build.pipeline, c.d);
    build.ms = MsSince(start);
    build.compile_required = result == vk::Result::ePipelineCompileRequired;
    if (!build.compile_required)
        Check(result, "pipeline creation");
    build.cache_hit = static_cast<bool>(
        feedback.flags & vk::PipelineCreationFeedbackFlagBits::eApplicationPipelineCacheHit);
    return build;
}

/// Destroys the pipelines of a finished timing set; only their timings are kept.
void Discard(const Context& c, const std::vector<Build>& builds) {
    for (const auto& build : builds)
        if (build.pipeline)
            c.device.destroy(build.pipeline, nullptr, c.d);
}

/// Builds the first `count` blend variants on `threads` threads, as a "Preparing
/// graphics" screen would with all cores, and returns the builds and wall time.
/// Pipeline i uses fragment module fs[i % fs.size()].
std::pair<std::vector<Build>, double> BuildAll(const Context& c, vk::ShaderModule vs,
                                               std::span<const vk::ShaderModule> fs,
                                               vk::PipelineLayout layout, vk::PipelineCache cache,
                                               u32 count, u32 threads, bool fail_on_compile) {
    std::vector<Build> builds(count);
    std::atomic<u32> next{0};
    std::atomic<bool> failed{false};
    const auto start = Clock::now();
    std::vector<std::thread> pool;
    for (u32 t = 0; t < threads; ++t) {
        pool.emplace_back([&] {
            for (u32 i; (i = next++) < count;) {
                try {
                    builds[i] = CreatePipeline(c, vs, fs[i % fs.size()], layout, cache,
                                               MakeVariant(c, i), fail_on_compile);
                } catch (...) {
                    failed = true;
                }
            }
        });
    }
    for (auto& thread : pool)
        thread.join();
    const double wall = MsSince(start);
    if (failed) {
        Discard(c, builds);
        throw std::runtime_error("a pipeline build failed during the variant set");
    }
    return {builds, wall};
}

/// One line summarizing a set of builds: count, time spread and driver flags.
std::string Summary(const std::string& label, const std::vector<Build>& builds, double wall_ms) {
    std::vector<double> ms;
    u32 hits = 0, refused = 0;
    for (const auto& build : builds) {
        ms.push_back(build.ms);
        hits += build.cache_hit;
        refused += build.compile_required;
    }
    if (ms.empty())
        return label + ": none\n";
    std::sort(ms.begin(), ms.end());
    return fmt::format("{}: n={} min={:.2f} median={:.2f} max={:.2f} ms, wall {:.1f} ms, "
                       "cache_hits={} compile_required={}\n",
                       label, ms.size(), ms.front(), ms[ms.size() / 2], ms.back(), wall_ms, hits,
                       refused);
}

/// "major.minor.patch" of a Vulkan version number.
std::string Version(u32 v) {
    return fmt::format("{}.{}.{}", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v),
                       VK_API_VERSION_PATCH(v));
}

/// Loads the system Vulkan driver, reports the device, its driver and the features
/// that matter, and creates a device with what the timing tests use. Leaves
/// c.device empty when the device is below Vulkan 1.3, which the tests need.
void CreateDevice(Context& c, std::string& out) {
    if (!c.library.Load(Common::DynamicLibrary::GetLibraryName("vulkan", 1)) &&
        !c.library.Load(Common::DynamicLibrary::GetLibraryName("vulkan")))
        throw std::runtime_error("no Vulkan library");
    const auto get_proc = c.library.GetSymbol<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
    if (!get_proc)
        throw std::runtime_error("no vkGetInstanceProcAddr");
    c.d.init(get_proc);
    const u32 loader =
        c.d.vkEnumerateInstanceVersion ? vk::enumerateInstanceVersion(c.d) : VK_API_VERSION_1_0;
    const vk::ApplicationInfo app{.pApplicationName = "Claudhar probe",
                                  .apiVersion = std::min(loader, VK_API_VERSION_1_3)};
    c.instance = vk::createInstance({.pApplicationInfo = &app}, nullptr, c.d);
    c.d.init(c.instance);
    out +=
        fmt::format("loader: {} (system driver; custom drivers are not probed)\n", Version(loader));

    // Prefer a real GPU over a CPU implementation such as lavapipe.
    const auto gpus = c.instance.enumeratePhysicalDevices(c.d);
    if (gpus.empty())
        throw std::runtime_error("no Vulkan devices");
    for (const auto gpu : gpus) {
        const auto props = gpu.getProperties(c.d);
        out += fmt::format("device: {} ({}), Vulkan {}\n", props.deviceName.data(),
                           vk::to_string(props.deviceType), Version(props.apiVersion));
    }
    const auto real = std::find_if(gpus.begin(), gpus.end(), [&c](vk::PhysicalDevice gpu) {
        return gpu.getProperties(c.d).deviceType != vk::PhysicalDeviceType::eCpu;
    });
    c.gpu = real != gpus.end() ? *real : gpus.front();
    c.props = c.gpu.getProperties(c.d);
    if (c.props.apiVersion < VK_API_VERSION_1_3) {
        out += "Vulkan 1.3 is not available; pipeline and shader tests skipped.\n";
        return;
    }

    std::set<std::string> extensions;
    for (const auto& e : c.gpu.enumerateDeviceExtensionProperties(nullptr, c.d))
        extensions.insert(e.extensionName.data());
    const auto has = [&](const char* name) { return extensions.contains(name); };

    // Identity: the driver build and the cache UUID that changes with it, which is
    // what will trigger "Preparing graphics" again after a driver update.
    vk::PhysicalDeviceDriverProperties driver{};
    vk::PhysicalDeviceVulkan11Properties p11{.pNext = &driver};
    vk::PhysicalDeviceGraphicsPipelineLibraryPropertiesEXT gpl_props{};
    if (has(VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME))
        driver.pNext = &gpl_props;
    vk::PhysicalDeviceProperties2 p2{.pNext = &p11};
    c.gpu.getProperties2(&p2, c.d);
    std::string uuid;
    for (const u8 byte : c.props.pipelineCacheUUID)
        uuid += fmt::format("{:02x}", byte);
    out += fmt::format("selected: {} vendor={:#06x} device={:#x}\n", c.props.deviceName.data(),
                       c.props.vendorID, c.props.deviceID);
    out += fmt::format("driver: {} | {} | version {:#x} | conformance {}.{}\n",
                       driver.driverName.data(), driver.driverInfo.data(), c.props.driverVersion,
                       driver.conformanceVersion.major, driver.conformanceVersion.minor);
    out += fmt::format("pipeline cache uuid: {}\n", uuid);

    // Features, chained only for extensions the device has.
    vk::PhysicalDeviceVulkan13Features f13{};
    vk::PhysicalDeviceVulkan12Features f12{.pNext = &f13};
    vk::PhysicalDeviceFeatures2 f2{.pNext = &f12};
    vk::PhysicalDeviceExtendedDynamicState2FeaturesEXT eds2{};
    vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT eds3{};
    vk::PhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gpl{};
    vk::PhysicalDeviceShaderObjectFeaturesEXT shader_object{};
    vk::PhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT raster_order{};
    vk::PhysicalDeviceShaderTileImageFeaturesEXT tile_image{};
    vk::PhysicalDeviceDynamicRenderingLocalReadFeaturesKHR local_read{};
    vk::PhysicalDeviceFragmentShaderInterlockFeaturesEXT interlock{};
    void** tail = &f13.pNext;
    const auto chain = [&tail](auto& features, bool present) {
        if (present) {
            *tail = &features;
            tail = &features.pNext;
        }
    };
    chain(eds2, has(VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME));
    chain(eds3, has(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME));
    chain(gpl, has(VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME));
    chain(shader_object, has(VK_EXT_SHADER_OBJECT_EXTENSION_NAME));
    chain(raster_order, has(VK_EXT_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME) ||
                            has(VK_ARM_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME));
    chain(tile_image, has(VK_EXT_SHADER_TILE_IMAGE_EXTENSION_NAME));
    chain(local_read, has(VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME));
    chain(interlock, has(VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME));
    c.gpu.getFeatures2(&f2, c.d);

    c.logic_op = f2.features.logicOp;
    c.cache_control = f13.pipelineCreationCacheControl;
    c.dynamic_blend = eds3.extendedDynamicState3ColorBlendEnable &&
                      eds3.extendedDynamicState3ColorBlendEquation &&
                      eds3.extendedDynamicState3ColorWriteMask;
    c.dynamic_logic_op = c.dynamic_blend && c.logic_op && eds3.extendedDynamicState3LogicOpEnable &&
                         eds2.extendedDynamicState2LogicOp;
    c.pipeline_library = gpl.graphicsPipelineLibrary;
    const auto yes = [](bool value) { return value ? "yes" : "no"; };
    out += fmt::format(
        "core: dynamicRendering={} synchronization2={} pipelineCreationCacheControl={} "
        "logicOp={} dualSrcBlend={} shaderFloat16={} shaderInt8={}\n",
        yes(f13.dynamicRendering), yes(f13.synchronization2), yes(c.cache_control), yes(c.logic_op),
        yes(f2.features.dualSrcBlend), yes(f12.shaderFloat16), yes(f12.shaderInt8));
    out += fmt::format("method A, dynamic blend state (EDS3): {}; dynamic logic ops: {}\n",
                       yes(c.dynamic_blend), yes(c.dynamic_logic_op));
    out += "method B, prebuilt blend variants: always possible; timed below\n";
    out += fmt::format("method C, blend in shader: raster-order color access={} tile image "
                       "color read={} local read={} pixel interlock={}\n",
                       yes(raster_order.rasterizationOrderColorAttachmentAccess),
                       yes(tile_image.shaderTileImageColorReadAccess),
                       yes(local_read.dynamicRenderingLocalRead),
                       yes(interlock.fragmentShaderPixelInterlock));
    out += fmt::format("pipeline libraries: {} (fast linking: {}); shader objects: {}; "
                       "pipeline binaries: {}\n",
                       yes(c.pipeline_library), yes(gpl_props.graphicsPipelineLibraryFastLinking),
                       yes(shader_object.shaderObject),
                       yes(has(VK_KHR_PIPELINE_BINARY_EXTENSION_NAME)));

    const auto& limits = c.props.limits;
    out += fmt::format("limits: maxUniformBufferRange={} maxStorageBufferRange={} "
                       "maxTexelBufferElements={} maxPushConstantsSize={} maxImageDimension2D={} "
                       "subgroupSize={} timestampPeriod={}\n",
                       limits.maxUniformBufferRange, limits.maxStorageBufferRange,
                       limits.maxTexelBufferElements, limits.maxPushConstantsSize,
                       limits.maxImageDimension2D, p11.subgroupSize, limits.timestampPeriod);
    const auto memory = c.gpu.getMemoryProperties(c.d);
    for (u32 i = 0; i < memory.memoryHeapCount; ++i)
        out += fmt::format("memory heap {}: {} MiB{}\n", i, memory.memoryHeaps[i].size >> 20,
                           memory.memoryHeaps[i].flags & vk::MemoryHeapFlagBits::eDeviceLocal
                               ? " device-local"
                               : "");
    for (const auto format : {vk::Format::eD24UnormS8Uint, vk::Format::eD32SfloatS8Uint})
        out += fmt::format(
            "{} depth attachment: {}\n", vk::to_string(format),
            yes(static_cast<bool>(c.gpu.getFormatProperties(format, c.d).optimalTilingFeatures &
                                  vk::FormatFeatureFlagBits::eDepthStencilAttachment)));
    std::string list;
    for (const auto& name : extensions)
        list += (list.empty() ? "" : " ") + name;
    out += fmt::format("extensions ({}): {}\n", extensions.size(), list);

    // A graphics queue, and a device with only what the tests below use.
    const auto families = c.gpu.getQueueFamilyProperties(c.d);
    const auto graphics = std::find_if(families.begin(), families.end(), [](const auto& f) {
        return static_cast<bool>(f.queueFlags & vk::QueueFlagBits::eGraphics);
    });
    if (graphics == families.end())
        throw std::runtime_error("no graphics queue");
    c.queue_family = static_cast<u32>(graphics - families.begin());
    c.timestamp_bits = graphics->timestampValidBits;

    vk::PhysicalDeviceVulkan13Features want13{};
    want13.pipelineCreationCacheControl = c.cache_control;
    want13.synchronization2 = VK_TRUE;
    want13.dynamicRendering = VK_TRUE;
    vk::PhysicalDeviceFeatures2 want{.pNext = &want13};
    want.features.logicOp = c.logic_op;
    vk::PhysicalDeviceExtendedDynamicState2FeaturesEXT want_eds2{};
    vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT want_eds3{};
    vk::PhysicalDeviceGraphicsPipelineLibraryFeaturesEXT want_gpl{};
    std::vector<const char*> enabled;
    tail = &want13.pNext;
    if (c.dynamic_blend) {
        want_eds3.extendedDynamicState3ColorBlendEnable = VK_TRUE;
        want_eds3.extendedDynamicState3ColorBlendEquation = VK_TRUE;
        want_eds3.extendedDynamicState3ColorWriteMask = VK_TRUE;
        want_eds3.extendedDynamicState3LogicOpEnable = c.dynamic_logic_op;
        chain(want_eds3, true);
        enabled.push_back(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
    }
    if (c.dynamic_logic_op) {
        want_eds2.extendedDynamicState2LogicOp = VK_TRUE;
        chain(want_eds2, true);
        enabled.push_back(VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME);
    }
    if (c.pipeline_library) {
        want_gpl.graphicsPipelineLibrary = VK_TRUE;
        chain(want_gpl, true);
        enabled.push_back(VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
        enabled.push_back(VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME);
    }
    const float priority = 1.0f;
    const vk::DeviceQueueCreateInfo queue_info{
        .queueFamilyIndex = c.queue_family, .queueCount = 1, .pQueuePriorities = &priority};
    c.device = c.gpu.createDevice({.pNext = &want,
                                   .queueCreateInfoCount = 1,
                                   .pQueueCreateInfos = &queue_info,
                                   .enabledExtensionCount = static_cast<u32>(enabled.size()),
                                   .ppEnabledExtensionNames = enabled.data()},
                                  nullptr, c.d);
    c.d.init(c.device);
    c.queue = c.device.getQueue(c.queue_family, 0, c.d);
}

/// A random nonce for FragmentSource, so each build's binary is new to the driver.
u32 Nonce(std::mt19937& random) {
    return 2 + random() % 0x7fffffff;
}

/// Method B timings: cold compiles of each shader kind, new blend variants of a
/// compiled shader, a whole cold variant set and a set of distinct shaders on all
/// cores, and whether the saved cache makes the next launch compile-free.
std::string ProbePipelines(Context& c, vk::PipelineLayout layout, u32 variants) {
    std::string out = "-- pipeline builds: ubershader with blend variants --\n";
    std::mt19937 random{std::random_device{}()};
    const auto vs = Module(c, VertexShader, vk::ShaderStageFlagBits::eVertex);
    const auto fragment = [&](Shader shader, double* glslang_ms = nullptr) {
        return Module(c, FragmentSource(shader, Nonce(random)), vk::ShaderStageFlagBits::eFragment,
                      glslang_ms);
    };
    double glslang_ms = 0.0;
    const auto fs = fragment(Shader::Uber, &glslang_ms);
    out += fmt::format("ubershader GLSL to SPIR-V on the CPU: {:.1f} ms\n", glslang_ms);

    // One cold pipeline per shader kind, each with a fresh cache: what one compile costs.
    const auto cold_compile = [&](vk::ShaderModule module) {
        const auto cache = c.Own(c.device.createPipelineCache({}, nullptr, c.d));
        const Build build = CreatePipeline(c, vs, module, layout, cache, MakeVariant(c, 0));
        Discard(c, {build});
        return build.ms;
    };
    const double compact_ms = cold_compile(fragment(Shader::Compact));
    const double special_ms = cold_compile(fragment(Shader::Specialized));

    // One thread: the first build compiles the ubershader; every later variant differs
    // only in blending and must be served from the cache without compiling.
    const auto cache = c.Own(c.device.createPipelineCache({}, nullptr, c.d));
    const Build first = CreatePipeline(c, vs, fs, layout, cache, MakeVariant(c, 0));
    Discard(c, {first});
    out += fmt::format("cold compile, one pipeline: ubershader {:.1f} ms, compact {:.1f} ms, "
                       "specialized {:.1f} ms\n",
                       first.ms, compact_ms, special_ms);
    const auto [after, after_wall] =
        BuildAll(c, vs, {&fs, 1}, layout, cache, variants, 1, c.cache_control);
    Discard(c, after);
    out += Summary(c.cache_control ? "new blend variants after it, compiles refused"
                                   : "new blend variants after it",
                   after, after_wall);

    // A new shader and an empty cache, every variant built across all cores.
    const u32 threads = std::max(1U, std::thread::hardware_concurrency());
    const std::string source = FragmentSource(Shader::Uber, Nonce(random));
    const auto cold_fs = Module(c, source, vk::ShaderStageFlagBits::eFragment);
    const auto cold_cache = c.Own(c.device.createPipelineCache({}, nullptr, c.d));
    const auto [cold, cold_wall] =
        BuildAll(c, vs, {&cold_fs, 1}, layout, cold_cache, variants, threads, false);
    Discard(c, cold);
    out += Summary(fmt::format("cold set on {} threads", threads), cold, cold_wall);

    // Distinct shaders built across all cores: how many a "Preparing graphics" screen
    // can compile per second.
    std::vector<vk::ShaderModule> distinct;
    for (u32 i = 0; i < (variants >= 64 ? 32U : 4U); ++i)
        distinct.push_back(fragment(Shader::Specialized));
    const auto distinct_cache = c.Own(c.device.createPipelineCache({}, nullptr, c.d));
    const auto [shaders, shaders_wall] = BuildAll(
        c, vs, distinct, layout, distinct_cache, static_cast<u32>(distinct.size()), threads, false);
    Discard(c, shaders);
    out += Summary(fmt::format("distinct specialized shaders on {} threads", threads), shaders,
                   shaders_wall);
    out += fmt::format("shader compile throughput: {:.0f} per second\n",
                       1000.0 * static_cast<double>(distinct.size()) / shaders_wall);

    // Method A: one pipeline whose blending is set per draw.
    if (c.dynamic_blend) {
        const Build dynamic =
            CreatePipeline(c, vs, cold_fs, layout, cold_cache, MakeVariant(c, 0), false, true);
        Discard(c, {dynamic});
        out += fmt::format("dynamic-blend pipeline (method A): {:.1f} ms\n", dynamic.ms);
    }

    // The next launch: a new module from the same source and a cache created from the
    // saved data. Every build should be served without compiling. The empty-cache run
    // shows whether the driver keeps its own cache as well.
    const auto data = c.device.getPipelineCacheData(cold_cache, c.d);
    out += fmt::format("saved pipeline cache: {} KiB for {} pipelines\n", data.size() / 1024,
                       variants);
    if (!c.cache_control) {
        out += "warm-cache test skipped: pipelineCreationCacheControl unsupported\n";
        return out;
    }
    const auto warm_fs = Module(c, source, vk::ShaderStageFlagBits::eFragment);
    const auto warm_cache = c.Own(c.device.createPipelineCache(
        {.initialDataSize = data.size(), .pInitialData = data.data()}, nullptr, c.d));
    const auto [warm, warm_wall] =
        BuildAll(c, vs, {&warm_fs, 1}, layout, warm_cache, variants, 1, true);
    Discard(c, warm);
    out += Summary("next launch, saved cache, compiles refused", warm, warm_wall);
    const auto empty_cache = c.Own(c.device.createPipelineCache({}, nullptr, c.d));
    const auto [empty, empty_wall] =
        BuildAll(c, vs, {&warm_fs, 1}, layout, empty_cache, variants, 1, true);
    Discard(c, empty);
    out += Summary("same, empty cache (driver's own cache only)", empty, empty_wall);
    return out;
}

/// Graphics pipeline libraries: the shader parts are built once, then each blend
/// variant needs only a small output part and a link. Times both, and whether the
/// link ever needs a compile.
std::string ProbeLibraries(Context& c, vk::PipelineLayout layout, u32 variants) {
    std::string out = "-- pipeline libraries: shared shader parts, per-variant link --\n";
    std::mt19937 random{std::random_device{}()};
    const auto vs = Module(c, VertexShader, vk::ShaderStageFlagBits::eVertex);
    const auto fs =
        Module(c, FragmentSource(Shader::Uber, Nonce(random)), vk::ShaderStageFlagBits::eFragment);
    const FixedState fixed;
    const vk::PipelineRenderingCreateInfo rendering{.colorAttachmentCount = 1,
                                                    .pColorAttachmentFormats = &ColorFormat};
    const auto part = [&](vk::GraphicsPipelineLibraryFlagsEXT flags,
                          vk::GraphicsPipelineCreateInfo info) {
        const vk::GraphicsPipelineLibraryCreateInfoEXT library{.pNext = &rendering, .flags = flags};
        info.pNext = &library;
        info.flags = vk::PipelineCreateFlagBits::eLibraryKHR;
        info.layout = layout;
        vk::Pipeline pipeline;
        const auto start = Clock::now();
        Check(c.device.createGraphicsPipelines({}, 1, &info, nullptr, &pipeline, c.d),
              "pipeline library part");
        return std::pair{pipeline, MsSince(start)};
    };
    using Part = vk::GraphicsPipelineLibraryFlagBitsEXT;
    const vk::PipelineShaderStageCreateInfo vertex{
        .stage = vk::ShaderStageFlagBits::eVertex, .module = vs, .pName = "main"};
    const vk::PipelineShaderStageCreateInfo fragment{
        .stage = vk::ShaderStageFlagBits::eFragment, .module = fs, .pName = "main"};
    const std::array viewport_state{vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    const vk::PipelineDynamicStateCreateInfo dynamic{.dynamicStateCount = 2,
                                                     .pDynamicStates = viewport_state.data()};
    const auto [input, input_ms] =
        part(Part::eVertexInputInterface,
             {.pVertexInputState = &fixed.vertex_input, .pInputAssemblyState = &fixed.assembly});
    const auto [raster, raster_ms] =
        part(Part::ePreRasterizationShaders, {.stageCount = 1,
                                              .pStages = &vertex,
                                              .pViewportState = &fixed.viewport,
                                              .pRasterizationState = &fixed.raster,
                                              .pDynamicState = &dynamic});
    const auto [shading, shading_ms] =
        part(Part::eFragmentShader, {.stageCount = 1,
                                     .pStages = &fragment,
                                     .pMultisampleState = &fixed.multisample,
                                     .pDepthStencilState = &fixed.depth_stencil});
    c.Own(input);
    c.Own(raster);
    c.Own(shading);
    out += fmt::format("shared parts: vertex input {:.2f} ms, vertex shader {:.1f} ms, "
                       "ubershader {:.1f} ms (cold)\n",
                       input_ms, raster_ms, shading_ms);

    std::vector<Build> links;
    const auto start = Clock::now();
    for (u32 i = 0; i < variants; ++i) {
        const Variant v = MakeVariant(c, i);
        const vk::PipelineColorBlendStateCreateInfo blend{.logicOpEnable = v.logic,
                                                          .logicOp = v.op,
                                                          .attachmentCount = 1,
                                                          .pAttachments = &v.blend};
        const auto [output, output_ms] =
            part(Part::eFragmentOutputInterface,
                 {.pMultisampleState = &fixed.multisample, .pColorBlendState = &blend});
        const std::array parts{input, raster, shading, output};
        const vk::PipelineLibraryCreateInfoKHR library{.libraryCount = 4,
                                                       .pLibraries = parts.data()};
        vk::GraphicsPipelineCreateInfo info{
            .pNext = &library,
            .flags = c.cache_control ? vk::PipelineCreateFlagBits::eFailOnPipelineCompileRequired
                                     : vk::PipelineCreateFlags{},
            .layout = layout};
        // A link the driver refuses without compiling is counted, then timed for real.
        Build link;
        auto link_start = Clock::now();
        auto result = c.device.createGraphicsPipelines({}, 1, &info, nullptr, &link.pipeline, c.d);
        link.compile_required = result == vk::Result::ePipelineCompileRequired;
        if (link.compile_required) {
            info.flags = {};
            link_start = Clock::now();
            result = c.device.createGraphicsPipelines({}, 1, &info, nullptr, &link.pipeline, c.d);
        }
        link.ms = output_ms + MsSince(link_start);
        Check(result, "pipeline library link");
        c.device.destroy(output, nullptr, c.d);
        links.push_back(link);
    }
    Discard(c, links);
    out += Summary("per variant, output part + link (compile_required = link compiled)", links,
                   MsSince(start));
    return out;
}

/// What each fragment shader costs per pixel: the same material drawn by the
/// ubershader, the compact ubershader and the specialized shader, full-screen at 2x
/// with overdraw, timed on the GPU. One layer of each is read back to prove all three
/// compute the same image, so the timings compare like with like.
std::string ProbeShaderCost(Context& c, vk::DescriptorSetLayout set_layout,
                            vk::PipelineLayout layout, u32 layers, u32 runs) {
    std::string out = "-- per-pixel cost: ubershaders against the specialized shader --\n";
    const Config config = TestConfig();
    const CompactData compact = ToCompact(config);
    constexpr std::array names{"ubershader", "compact ubershader", "specialized"};
    constexpr std::array shaders{Shader::Uber, Shader::Compact, Shader::Specialized};
    constexpr u32 count = static_cast<u32>(shaders.size());
    const auto vs = Module(c, VertexShader, vk::ShaderStageFlagBits::eVertex);
    std::array<vk::Pipeline, count> pipelines;
    for (u32 k = 0; k < count; ++k) {
        const auto fs =
            Module(c, FragmentSource(shaders[k], 2), vk::ShaderStageFlagBits::eFragment);
        pipelines[k] = c.Own(CreatePipeline(c, vs, fs, layout, {}, AlphaBlend()).pipeline);
    }

    // Inputs the shaders read: both configurations, the lookup tables and a texture.
    const auto ubo =
        HostBuffer(c, vk::BufferUsageFlagBits::eUniformBuffer, sizeof(config), &config);
    const auto compact_ubo =
        HostBuffer(c, vk::BufferUsageFlagBits::eUniformBuffer, sizeof(compact), &compact);
    std::array<float, 768> lut;
    for (u32 i = 0; i < 256; ++i) {
        lut[i] = 0.5f + 0.5f * std::sin(static_cast<float>(i) * 0.05f);
        lut[256 + i] = static_cast<float>(i) / 255.0f;
        lut[512 + i] = 1.0f - static_cast<float>(i) / 255.0f;
    }
    const auto lut_buffer =
        HostBuffer(c, vk::BufferUsageFlagBits::eUniformTexelBuffer, sizeof(lut), lut.data());
    const auto lut_view = c.Own(c.device.createBufferView(
        {.buffer = lut_buffer, .format = vk::Format::eR32Sfloat, .range = VK_WHOLE_SIZE}, nullptr,
        c.d));
    std::vector<u32> texels(TextureSize * TextureSize);
    u32 x = 12345;
    for (auto& texel : texels) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        texel = x;
    }
    const auto staging = HostBuffer(c, vk::BufferUsageFlagBits::eTransferSrc,
                                    texels.size() * sizeof(u32), texels.data());
    const auto [texture, texture_view] =
        DeviceImage(c, TextureSize, TextureSize,
                    vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst);
    const auto [target, target_view] = DeviceImage(c, Width, Height,
                                                   vk::ImageUsageFlagBits::eColorAttachment |
                                                       vk::ImageUsageFlagBits::eTransferSrc);
    const auto sampler =
        c.Own(c.device.createSampler({.magFilter = vk::Filter::eLinear,
                                      .minFilter = vk::Filter::eLinear,
                                      .addressModeU = vk::SamplerAddressMode::eRepeat,
                                      .addressModeV = vk::SamplerAddressMode::eRepeat,
                                      .addressModeW = vk::SamplerAddressMode::eRepeat},
                                     nullptr, c.d));
    std::array<void*, count> readback_data{};
    std::array<vk::Buffer, count> readback;
    for (u32 k = 0; k < count; ++k)
        readback[k] = HostBuffer(c, vk::BufferUsageFlagBits::eTransferDst, Width * Height * 4,
                                 nullptr, &readback_data[k]);

    const std::array sizes{vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, 2},
                           vk::DescriptorPoolSize{vk::DescriptorType::eCombinedImageSampler, 3},
                           vk::DescriptorPoolSize{vk::DescriptorType::eUniformTexelBuffer, 1}};
    const auto pool = c.Own(c.device.createDescriptorPool(
        {.maxSets = 1, .poolSizeCount = static_cast<u32>(sizes.size()), .pPoolSizes = sizes.data()},
        nullptr, c.d));
    vk::DescriptorSet set;
    const vk::DescriptorSetAllocateInfo set_info{
        .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &set_layout};
    Check(c.device.allocateDescriptorSets(&set_info, &set, c.d), "descriptor set");
    const vk::DescriptorBufferInfo ubo_info{ubo, 0, VK_WHOLE_SIZE};
    const vk::DescriptorBufferInfo compact_info{compact_ubo, 0, VK_WHOLE_SIZE};
    const vk::DescriptorImageInfo image_info{sampler, texture_view,
                                             vk::ImageLayout::eShaderReadOnlyOptimal};
    const std::array image_infos{image_info, image_info, image_info};
    const std::array writes{
        vk::WriteDescriptorSet{.dstSet = set,
                               .dstBinding = 0,
                               .descriptorCount = 1,
                               .descriptorType = vk::DescriptorType::eUniformBuffer,
                               .pBufferInfo = &ubo_info},
        vk::WriteDescriptorSet{.dstSet = set,
                               .dstBinding = 1,
                               .descriptorCount = 3,
                               .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                               .pImageInfo = image_infos.data()},
        vk::WriteDescriptorSet{.dstSet = set,
                               .dstBinding = 2,
                               .descriptorCount = 1,
                               .descriptorType = vk::DescriptorType::eUniformTexelBuffer,
                               .pTexelBufferView = &lut_view},
        vk::WriteDescriptorSet{.dstSet = set,
                               .dstBinding = 3,
                               .descriptorCount = 1,
                               .descriptorType = vk::DescriptorType::eUniformBuffer,
                               .pBufferInfo = &compact_info}};
    c.device.updateDescriptorSets(writes, nullptr, c.d);

    // One command buffer: upload the texture, alternate timed runs of the shaders,
    // then render one layer of each for the readback comparison.
    const u32 queries = runs * count * 2;
    const auto query_pool = c.Own(c.device.createQueryPool(
        {.queryType = vk::QueryType::eTimestamp, .queryCount = queries}, nullptr, c.d));
    const auto command_pool =
        c.Own(c.device.createCommandPool({.queueFamilyIndex = c.queue_family}, nullptr, c.d));
    vk::CommandBuffer cmd;
    const vk::CommandBufferAllocateInfo cmd_info{.commandPool = command_pool,
                                                 .level = vk::CommandBufferLevel::ePrimary,
                                                 .commandBufferCount = 1};
    Check(c.device.allocateCommandBuffers(&cmd_info, &cmd, c.d), "command buffer");
    cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit}, c.d);
    Transition(c, cmd, texture, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal);
    const vk::BufferImageCopy upload{.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                     .imageExtent = {TextureSize, TextureSize, 1}};
    cmd.copyBufferToImage(staging, texture, vk::ImageLayout::eTransferDstOptimal, upload, c.d);
    Transition(c, cmd, texture, vk::ImageLayout::eTransferDstOptimal,
               vk::ImageLayout::eShaderReadOnlyOptimal);
    Transition(c, cmd, target, vk::ImageLayout::eUndefined,
               vk::ImageLayout::eColorAttachmentOptimal);
    cmd.resetQueryPool(query_pool, 0, queries, c.d);
    const vk::Rect2D area{{0, 0}, {Width, Height}};
    const auto draw = [&](vk::Pipeline pipeline, u32 instances) {
        const vk::RenderingAttachmentInfo color{.imageView = target_view,
                                                .imageLayout =
                                                    vk::ImageLayout::eColorAttachmentOptimal,
                                                .loadOp = vk::AttachmentLoadOp::eClear,
                                                .storeOp = vk::AttachmentStoreOp::eStore};
        cmd.beginRendering({.renderArea = area,
                            .layerCount = 1,
                            .colorAttachmentCount = 1,
                            .pColorAttachments = &color},
                           c.d);
        cmd.setViewport(0,
                        vk::Viewport{0.0f, 0.0f, static_cast<float>(Width),
                                     static_cast<float>(Height), 0.0f, 1.0f},
                        c.d);
        cmd.setScissor(0, area, c.d);
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline, c.d);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, 0, set, nullptr, c.d);
        cmd.draw(3, instances, 0, 0, c.d);
        cmd.endRendering(c.d);
    };
    for (u32 run = 0; run < runs; ++run) {
        for (u32 k = 0; k < count; ++k) {
            const u32 query = (run * count + k) * 2;
            cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, query_pool, query, c.d);
            draw(pipelines[k], layers);
            cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, query_pool, query + 1,
                                c.d);
            // Keeps runs from overlapping on the GPU.
            Transition(c, cmd, target, vk::ImageLayout::eColorAttachmentOptimal,
                       vk::ImageLayout::eColorAttachmentOptimal);
        }
    }
    for (u32 k = 0; k < count; ++k) {
        draw(pipelines[k], 1);
        Transition(c, cmd, target, vk::ImageLayout::eColorAttachmentOptimal,
                   vk::ImageLayout::eTransferSrcOptimal);
        const vk::BufferImageCopy copy{
            .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .imageExtent = {Width, Height, 1}};
        cmd.copyImageToBuffer(target, vk::ImageLayout::eTransferSrcOptimal, readback[k], copy, c.d);
        Transition(c, cmd, target, vk::ImageLayout::eTransferSrcOptimal,
                   vk::ImageLayout::eColorAttachmentOptimal);
    }
    const vk::MemoryBarrier2 to_host{.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                                     .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                                     .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                                     .dstAccessMask = vk::AccessFlagBits2::eHostRead};
    cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &to_host}, c.d);
    cmd.end(c.d);
    const auto fence = c.Own(c.device.createFence({}, nullptr, c.d));
    c.queue.submit(vk::SubmitInfo{.commandBufferCount = 1, .pCommandBuffers = &cmd}, fence, c.d);
    Check(c.device.waitForFences(fence, VK_TRUE, 120'000'000'000ULL, c.d), "GPU work");

    std::vector<u64> stamps(queries);
    Check(c.device.getQueryPoolResults(
              query_pool, 0, queries, stamps.size() * sizeof(u64), stamps.data(), sizeof(u64),
              vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWait, c.d),
          "timestamps");
    const u64 mask = c.timestamp_bits >= 64 ? ~0ULL : (1ULL << c.timestamp_bits) - 1;
    std::array<double, count> best, first{};
    best.fill(1e30);
    for (u32 run = 0; run < runs; ++run)
        for (u32 k = 0; k < count; ++k) {
            const u32 query = (run * count + k) * 2;
            const double ms = static_cast<double>((stamps[query + 1] - stamps[query]) & mask) *
                              c.props.limits.timestampPeriod / 1e6;
            best[k] = std::min(best[k], ms);
            first[k] = run == 0 ? ms : first[k];
        }

    // Largest per-channel difference of each image from the specialized shader's.
    const auto difference = [&](u32 k) {
        int worst = 0;
        const auto* a = static_cast<const u8*>(readback_data[k]);
        const auto* b = static_cast<const u8*>(readback_data[count - 1]);
        for (u32 i = 0; i < Width * Height * 4; ++i)
            worst = std::max(worst, std::abs(int{a[i]} - int{b[i]}));
        return worst;
    };
    const double pixels = static_cast<double>(Width) * Height * layers;
    out += fmt::format("{}x{} with {} layers, best of {} runs\n", Width, Height, layers, runs);
    for (u32 k = 0; k < count; ++k) {
        // Time above the best in the first run is work the driver did at first draw,
        // such as a deferred compile, which would be a pause in a game.
        out += fmt::format("{}: {:.3f} ms ({:.2f} ns per pixel), {:.2f}x specialized, first run "
                           "{:.3f} ms",
                           names[k], best[k], best[k] * 1e6 / pixels, best[k] / best[count - 1],
                           first[k]);
        if (k + 1 < count) {
            const int worst = difference(k);
            out += fmt::format(", image difference {} ({})", worst,
                               worst <= 2 ? "same image" : "MISMATCH: timings not comparable");
        }
        out += '\n';
    }
    return out;
}

} // Anonymous namespace

std::string ReportVulkan(const Options& options) {
    std::string out = "== Vulkan ==\n";
    try {
        Context c;
        CreateDevice(c, out);
        if (!c.device)
            return out;
        // The resource layout every probe shader uses: the ubershader's configuration,
        // three textures, the lookup tables and the compact ubershader's configuration.
        const std::array bindings{
            vk::DescriptorSetLayoutBinding{0, vk::DescriptorType::eUniformBuffer, 1,
                                           vk::ShaderStageFlagBits::eFragment},
            vk::DescriptorSetLayoutBinding{1, vk::DescriptorType::eCombinedImageSampler, 3,
                                           vk::ShaderStageFlagBits::eFragment},
            vk::DescriptorSetLayoutBinding{2, vk::DescriptorType::eUniformTexelBuffer, 1,
                                           vk::ShaderStageFlagBits::eFragment},
            vk::DescriptorSetLayoutBinding{3, vk::DescriptorType::eUniformBuffer, 1,
                                           vk::ShaderStageFlagBits::eFragment}};
        const auto set_layout = c.Own(c.device.createDescriptorSetLayout(
            {.bindingCount = static_cast<u32>(bindings.size()), .pBindings = bindings.data()},
            nullptr, c.d));
        const auto layout = c.Own(c.device.createPipelineLayout(
            {.setLayoutCount = 1, .pSetLayouts = &set_layout}, nullptr, c.d));
        const u32 variants = options.quick ? 8 : 64;
        out += ProbePipelines(c, layout, variants);
        if (c.pipeline_library)
            out += ProbeLibraries(c, layout, variants);
        out +=
            ProbeShaderCost(c, set_layout, layout, options.quick ? 2 : 16, options.quick ? 1 : 5);
    } catch (const std::exception& e) {
        out += fmt::format("error: {}\n", e.what());
    }
    return out;
}

} // namespace Claudhar::Probe
