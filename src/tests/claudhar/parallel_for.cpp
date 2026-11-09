// Copyright 2026 Claudhar Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

// Tests the parallel loop that shades CPU-vertex draws: every index runs exactly once, also
// when chunks finish out of order, and PICA vertex shaders give the same bits on several
// threads, each with its own shader unit, as on one, with the JIT and the interpreter.

#include "common/arch.h"
#if CITRA_ARCH(x86_64) || CITRA_ARCH(arm64)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <nihstro/inline_assembly.h>
#include "claudhar/jobs/parallel_for.h"
#include "video_core/pica/shader_setup.h"
#include "video_core/pica/shader_unit.h"
#include "video_core/shader/shader.h"

using Claudhar::Jobs::ParallelFor;
using DestRegister = nihstro::DestRegister;
using OpCode = nihstro::OpCode;
using SourceRegister = nihstro::SourceRegister;

TEST_CASE("ParallelFor runs every index once", "[claudhar][jobs]") {
    ParallelFor jobs{3, "test"};
    REQUIRE(jobs.Threads() == 4);
    for (const u32 count : {0u, 1u, 15u, 16u, 17u, 64u, 1000u, 4097u}) {
        for (const u32 chunk : {1u, 16u, 64u, 5000u}) {
            std::vector<std::atomic<u32>> hits(count);
            // Catch's checks are not thread-safe, so the threads only note a bad range.
            std::atomic<bool> bad_range{false};
            // Repeated, so a worker late from one loop meets the next one.
            for (u32 repeat = 0; repeat < 50; ++repeat) {
                jobs.Run(count, chunk, [&](u32 begin, u32 end) {
                    if (begin >= end || end > count || end - begin > chunk) {
                        bad_range = true;
                        return;
                    }
                    for (u32 i = begin; i < end; ++i) {
                        hits[i].fetch_add(1, std::memory_order_relaxed);
                    }
                });
            }
            REQUIRE_FALSE(bad_range.load());
            for (u32 i = 0; i < count; ++i) {
                REQUIRE(hits[i].load() == 50);
            }
        }
    }
}

// Back-to-back loops of changing sizes, as draws come: a worker still leaving one loop when
// the next is set up once claimed a chunk that did not exist and hung the caller after a few
// hundred loops.
TEST_CASE("ParallelFor survives many loops of changing sizes", "[claudhar][jobs]") {
    ParallelFor jobs{3, "test"};
    std::vector<u32> data(5000);
    u32 wrong = 0;
    for (u32 loop = 1; loop <= 20000; ++loop) {
        const u32 count = 1 + loop * 37 % 5000;
        jobs.Run(count, 16, [&](u32 begin, u32 end) {
            for (u32 i = begin; i < end; ++i) {
                data[i] = loop;
            }
        });
        for (u32 i = 0; i < count; ++i) {
            wrong += data[i] != loop;
        }
    }
    REQUIRE(wrong == 0);
}

TEST_CASE("ParallelFor waits for slow chunks", "[claudhar][jobs]") {
    ParallelFor jobs{3, "test"};
    std::vector<u32> results(64, 0);
    jobs.Run(64, 1, [&](u32 begin, u32) {
        // Uneven chunks, longer than the caller's spinning, so it also sleeps.
        if (begin % 7 == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        results[begin] = begin + 1;
    });
    for (u32 i = 0; i < 64; ++i) {
        REQUIRE(results[i] == i + 1);
    }
}

TEST_CASE("ParallelFor without workers runs on the caller", "[claudhar][jobs]") {
    ParallelFor jobs{0, "test"};
    const auto caller = std::this_thread::get_id();
    u32 total = 0;
    bool elsewhere = false;
    jobs.Run(100, 10, [&](u32 begin, u32 end) {
        elsewhere |= std::this_thread::get_id() != caller;
        total += end - begin;
    });
    REQUIRE_FALSE(elsewhere);
    REQUIRE(total == 100);
}

namespace {

/// A vertex shader with arithmetic, uniforms, a call and the special functions, whose
/// output differs for every input.
std::unique_ptr<Pica::ShaderSetup> TestProgram() {
    const auto in0 = SourceRegister::MakeInput(0);
    const auto in1 = SourceRegister::MakeInput(1);
    const auto uniform = SourceRegister::MakeFloat(0);
    const auto temp = DestRegister::MakeTemporary(0);
    const auto temp_in = SourceRegister::MakeTemporary(0);
    const auto out0 = DestRegister::MakeOutput(0);
    const auto out1 = DestRegister::MakeOutput(1);
    const auto shbin = nihstro::InlineAsm::CompileToRawBinary({
        {OpCode::Id::MUL, temp, in0, uniform},
        {OpCode::Id::ADD, temp, temp_in, in1},
        {OpCode::Id::DP4, out0, temp_in, in0},
        {OpCode::Id::RSQ, temp, temp_in},
        {OpCode::Id::EX2, out1, temp_in},
        {OpCode::Id::END},
    });
    auto setup = std::make_unique<Pica::ShaderSetup>();
    Pica::ProgramCode program{};
    Pica::SwizzleData swizzle{};
    std::transform(shbin.program.begin(), shbin.program.end(), program.begin(),
                   [](const auto& x) { return x.hex; });
    std::transform(shbin.swizzle_table.begin(), shbin.swizzle_table.end(), swizzle.begin(),
                   [](const auto& x) { return x.hex; });
    setup->UpdateProgramCode(program);
    setup->UpdateSwizzleData(swizzle);
    setup->uniforms.f[0] =
        Common::Vec4<Pica::f24>{Pica::f24::FromFloat32(0.5f), Pica::f24::FromFloat32(-2.0f),
                                Pica::f24::FromFloat32(3.0f), Pica::f24::FromFloat32(0.25f)};
    return setup;
}

/// Runs the shader for vertex `i` on `unit` and returns its two outputs.
std::array<Common::Vec4<Pica::f24>, 2> Shade(const Pica::ShaderEngine& engine,
                                             const Pica::ShaderSetup& setup, Pica::ShaderUnit& unit,
                                             u32 i) {
    const float x = static_cast<float>(i) * 0.37f - 40.0f;
    for (u32 reg = 0; reg < 2; ++reg) {
        unit.input[reg] = Common::Vec4<Pica::f24>{
            Pica::f24::FromFloat32(x + reg), Pica::f24::FromFloat32(x * 0.5f - reg),
            Pica::f24::FromFloat32(1.0f / (1.0f + i)), Pica::f24::FromFloat32(x * x * 0.01f)};
    }
    engine.Run(setup, unit);
    return {unit.output[unit.output_bank][0], unit.output[unit.output_bank][1]};
}

} // namespace

TEST_CASE("Vertex shaders shade the same on several threads", "[claudhar][jobs]") {
    constexpr u32 vertices = 2000;
    for (const bool jit : {true, false}) {
        auto setup = TestProgram();
        const auto engine = Pica::CreateEngine(jit);
        engine->SetupBatch(*setup, 0);

        std::vector<std::array<Common::Vec4<Pica::f24>, 2>> serial(vertices);
        Pica::ShaderUnit unit;
        for (u32 i = 0; i < vertices; ++i) {
            serial[i] = Shade(*engine, *setup, unit, i);
        }

        ParallelFor jobs{3, "test"};
        std::vector<std::array<Common::Vec4<Pica::f24>, 2>> parallel(vertices);
        jobs.Run(vertices, 16, [&](u32 begin, u32 end) {
            Pica::ShaderUnit chunk_unit;
            for (u32 i = begin; i < end; ++i) {
                parallel[i] = Shade(*engine, *setup, chunk_unit, i);
            }
        });
        REQUIRE(std::memcmp(serial.data(), parallel.data(), vertices * sizeof(serial[0])) == 0);
    }
}

#endif // CITRA_ARCH(x86_64) || CITRA_ARCH(arm64)
