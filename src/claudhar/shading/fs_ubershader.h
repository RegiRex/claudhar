// Copyright 2026 Claudhar Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

// The PICA fragment ubershader. Azahar compiles one fragment shader per fragment
// configuration, which is what makes cold first runs pause (or skip draws). The
// ubershader instead reads the configuration from a uniform buffer at run time, so a
// handful of variants compiled before play can draw every configuration. It has the same
// inputs, outputs and bindings as Azahar's specialized shaders, plus one uniform buffer
// for the configuration, and computes the same values in the same order so the two give
// the same pixels and depth.
//
// The arithmetic follows the generator Azahar uses for the configuration on this device:
// the SPIR-V generator by default, or the GLSL generator when the SPIR-V one is off or
// cannot express the configuration (shadow-map rendering, shadow cube textures). Where
// the two differ only in rounding (fused multiply-add) the ubershader uses that
// generator's form; where they differ in meaning, the difference is listed in
// docs/PHASE1_UBERSHADER.md.

#pragma once

#include <array>
#include <string>

#include "common/common_types.h"
#include "video_core/shader/generator/pica_fs_config.h"
#include "video_core/shader/generator/profile.h"

namespace Claudhar::Shading {

/// Binding of the ubershader's configuration buffer in descriptor set 0. Bindings 0-5 are
/// Azahar's; specialized shaders do not declare this one.
constexpr u32 UBER_CONFIG_BINDING = 6;

/// The parts of a configuration that change what the ubershader declares or which
/// generator's arithmetic it must follow. Each combination is a separate variant; all are
/// compiled before play. Everything else is read from the configuration buffer.
struct UberKey {
    /// How texture unit 0 is declared: a 2D sampler (2D, projected and disabled textures),
    /// a cube sampler, a shadow map, or the six faces of a shadow cube.
    enum class Tex0 : u8 { Plain, Cube, Shadow2D, ShadowCube };

    Tex0 tex0 = Tex0::Plain;
    bool shadow_rendering = false; ///< Writes a shadow map instead of color and depth.
    bool custom_normal = false;    ///< Lighting reads a custom-texture normal map.

    /// The variant's position in a table of NUM_VARIANTS entries.
    [[nodiscard]] u32 Index() const {
        return static_cast<u32>(tex0) | (shadow_rendering ? 4u : 0u) | (custom_normal ? 8u : 0u);
    }

    /// The key at a table position; the inverse of Index().
    static UberKey FromIndex(u32 index);

    static constexpr u32 NUM_VARIANTS = 16;
};

/// True when Azahar generates this variant's specialized shaders with the GLSL generator
/// rather than the SPIR-V one, so the ubershader must follow GLSL's arithmetic. Called on
/// the emulation thread and on worker threads; reads only its arguments.
[[nodiscard]] bool UsesGlslSemantics(const UberKey& key, const Pica::Shader::Profile& profile);

/// Picks the variant that draws a configuration. `config` is as built from the registers
/// (before ApplyProfile). The SPIR-V generator ignores custom normal maps, so the key only
/// asks for one where Azahar would use it. Called on the emulation thread for each draw
/// that uses the ubershader; touches nothing.
[[nodiscard]] UberKey MakeUberKey(const Pica::Shader::FSConfig& config,
                                  const Pica::Shader::UserConfig& user,
                                  const Pica::Shader::Profile& profile);

/// The contents of the configuration buffer: the fragment configuration after
/// ApplyProfile, byte for byte, followed by values the CPU derives from it once so every
/// pixel does not have to.
struct UberConfig {
    std::array<u32, sizeof(Pica::Shader::FSConfig) / 4> config{};
    u32 tev = 0;      ///< Bits 0-5: stage i passes its input through. Bits 8-11: stages run.
    u32 textures = 0; ///< Bit i: texture unit i is read by the combiners or the lighting.
    u32 luts = 0;     ///< Bits 0-6: D0, D1, SP, FR, RR, RG, RB lookup tables are in use.
    u32 padding = 0;
};
static_assert(sizeof(UberConfig) % 16 == 0, "the buffer is declared as an array of uvec4");

/// Fills the configuration buffer for a configuration that has had ApplyProfile applied.
/// Called on the emulation thread when the ubershader draws a new configuration; reads
/// only its argument.
[[nodiscard]] UberConfig MakeUberConfig(const Pica::Shader::FSConfig& applied_config);

/// Generates the GLSL source of one ubershader variant for this device's profile. Called
/// once per variant before play; the result goes to CompileGLSL like Azahar's shaders.
[[nodiscard]] std::string GenerateFragmentUbershader(const UberKey& key,
                                                     const Pica::Shader::Profile& profile);

} // namespace Claudhar::Shading
