// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.
// Modified by the Claudhar Project in 2026; changes are marked "Claudhar:".

#pragma once

#include <bitset>
#include <optional>

#include "claudhar/shading/draw_ledger.h"
#include "claudhar/shading/fs_ubershader.h"

#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"
#include "video_core/renderer_vulkan/vk_shader_disk_cache.h"
#include "video_core/shader/generator/pica_fs_config.h"
#include "video_core/shader/generator/profile.h"
#include "video_core/shader/generator/shader_gen.h"

namespace Pica {
struct RegsInternal;
struct ShaderSetup;
} // namespace Pica

namespace Vulkan {

class Instance;
class Scheduler;
class RenderManager;
class DescriptorUpdateQueue;

enum class DescriptorHeapType : u32 {
    Buffer,
    Texture,
    Utility,
};

/// Claudhar: how the next draw's fragments are shaded, chosen without waiting for a compile.
enum class FragmentPath : u8 {
    Specialized, ///< Azahar's pipeline for this configuration is built.
    Ubershader,  ///< It is not built yet; the ubershader draws.
    CpuVertices, ///< A GPU-vertex draw with no pipeline built yet: shade vertices on the CPU.
};

/**
 * Stores a collection of rasterizer pipelines used during rendering.
 */
class PipelineCache {
    static constexpr u32 NumRasterizerSets = 3;
    static constexpr u32 NumDescriptorHeaps = 3;
    // Claudhar: a fourth dynamic uniform buffer holds the ubershader's configuration.
    static constexpr u32 NumDynamicOffsets = 4;

public:
    /// Claudhar: the dynamic offset slot of the ubershader configuration buffer (binding 6;
    /// slots follow the order of the dynamic bindings 0, 1, 2 and 6).
    static constexpr u8 UberConfigOffsetSlot = 3;

    explicit PipelineCache(const Instance& instance, Scheduler& scheduler,
                           RenderManager& renderpass_cache, DescriptorUpdateQueue& update_queue);
    ~PipelineCache();

    /// Acquires and binds a free descriptor set from the appropriate heap.
    vk::DescriptorSet Acquire(DescriptorHeapType type) {
        const u32 index = static_cast<u32>(type);
        const auto descriptor_set = descriptor_heaps[index].Commit();
        bound_descriptor_sets[index] = descriptor_set;
        return descriptor_set;
    }

    /// Sets the dynamic offset for the uniform buffer at binding
    void UpdateRange(u8 binding, u32 offset) {
        offsets[binding] = offset;
    }

    /// Loads the driver pipeline cache and the disk shader cache
    void LoadCache(const std::atomic_bool& stop_loading = std::atomic_bool{false},
                   const VideoCore::DiskResourceLoadCallback& callback = {});

    /// Switches the driver pipeline cache and the shader disk cache to the specified title
    void SwitchCache(u64 title_id, const std::atomic_bool& stop_loading = std::atomic_bool{false},
                     const VideoCore::DiskResourceLoadCallback& callback = {});

    // Claudhar: BindPipeline(info, wait_built) waited for a compile or reported a skipped
    // draw. Choosing and binding are now separate so the choice, which never waits, is
    // known before the uniforms are uploaded.

    /// Chooses the pipeline for the next draw from the current shaders and `info`: the
    /// specialized one when it is built, otherwise the fragment ubershader. A GPU-vertex draw
    /// uses the ubershader with its own vertex shader once that pipeline is built, and is
    /// returned to the CPU until then. `specialize` false skips building a specialized
    /// pipeline, for the CPU-vertex retry of a GPU-vertex draw. Emulation thread only.
    FragmentPath SelectPipeline(PipelineInfo& info, bool gpu_vertices, bool specialize);

    /// Records binding the pipeline chosen by SelectPipeline, with the dynamic state.
    void BindPipeline(PipelineInfo& info);

    /// When the ubershader was chosen for a configuration other than the last one taken,
    /// fills `out` with its configuration buffer and returns true.
    bool TakeUberConfig(Claudhar::Shading::UberConfig& out);

    /// The software vertex layout, used to build ubershader pipelines before play.
    void SetSoftwareVertexLayout(const VertexLayout& layout) {
        software_layout = layout;
    }

    /// The draw counters, which the rasterizer also updates.
    Claudhar::Shading::DrawLedger& Ledger() {
        return ledger;
    }

    Pica::Shader::Generator::ExtraVSConfig CalcExtraConfig(
        const Pica::Shader::Generator::PicaVSConfig& config);

    /// Binds a PICA decompiled vertex shader
    bool UseProgrammableVertexShader(const Pica::RegsInternal& regs, Pica::ShaderSetup& setup,
                                     const VertexLayout& layout);

    /// Binds a passthrough vertex shader
    void UseTrivialVertexShader();

    /// Binds a PICA decompiled geometry shader
    bool UseFixedGeometryShader(const Pica::RegsInternal& regs);

    /// Binds a passthrough geometry shader
    void UseTrivialGeometryShader();

    /// Binds a fragment shader generated from PICA state
    void UseFragmentShader(const Pica::RegsInternal& regs, const Pica::Shader::UserConfig& user);

    /// Gets the current program ID
    u64 GetProgramID() const {
        return current_program_id;
    }

    void SetProgramID(u64 program_id) {
        current_program_id = program_id;
    }

    void SetAccurateMul(bool _accurate_mul) {
        profile.enable_accurate_mul = _accurate_mul;
    }

private:
    friend ShaderDiskCache;

    /// Loads the driver pipeline cache
    void LoadDriverPipelineDiskCache(const std::atomic_bool& stop_loading = std::atomic_bool{false},
                                     const VideoCore::DiskResourceLoadCallback& callback = {});

    /// Stores the generated pipeline cache
    void SaveDriverPipelineDiskCache();

    /// Loads the shader disk cache
    void LoadDiskCache(const std::atomic_bool& stop_loading = std::atomic_bool{false},
                       const VideoCore::DiskResourceLoadCallback& callback = {});

    /// Switches the disk cache at runtime to use a different title ID
    void SwitchDiskCache(u64 title_id, const std::atomic_bool& stop_loading,
                         const VideoCore::DiskResourceLoadCallback& callback);

    /// Builds the rasterizer pipeline layout
    void BuildLayout();

    /// Returns true when the disk data can be used by the current driver
    bool IsCacheValid(std::span<const u8> cache_data) const;

    /// Create pipeline cache directories. Returns true on success.
    bool EnsureDirectories() const;

    /// Returns the Vulkan shader directory
    std::string GetVulkanDir() const;

    /// Returns the pipeline cache storage dir
    std::string GetPipelineCacheDir() const;

    /// Returns the transferable shader dir
    std::string GetTransferableDir() const;

    /// Claudhar: compiles every ubershader variant and creates its pipelines for each render
    /// target format, on all cores, while the game loads. Runs once per renderer.
    void PrepareUbershaders(const std::atomic_bool& stop_loading,
                            const VideoCore::DiskResourceLoadCallback& callback);

    /// Claudhar: the ubershader pipeline for `info` and the current fragment configuration.
    GraphicsPipeline* UberPipeline(const PipelineInfo& info);

    /// Claudhar: the pipeline with the current vertex shader and the fragment ubershader
    /// for a GPU-vertex draw, or null while it is being built.
    GraphicsPipeline* GpuVertexUberPipeline(const PipelineInfo& info);

    /// Claudhar: TryBuildCached, timed for the ledger, since it runs on the emulation thread.
    bool TryBuildCached(GraphicsPipeline* pipeline);

    /// Claudhar: loads and saves the ubershader pipelines' own driver cache. It is kept
    /// apart from the per-game cache so it survives switching games and applets.
    void LoadUberPipelineCache();
    void SaveUberPipelineCache();

private:
    const Instance& instance;
    Scheduler& scheduler;
    RenderManager& renderpass_cache;
    DescriptorUpdateQueue& update_queue;

    Pica::Shader::Profile profile{};
    vk::UniquePipelineCache driver_pipeline_cache;
    vk::UniquePipelineLayout pipeline_layout;
    std::size_t num_worker_threads;
    Common::ThreadWorker pipeline_workers;
    Common::ThreadWorker shader_workers;
    /// Claudhar: builds GPU-vertex ubershader pipelines, which take GPU-vertex draws off the
    /// CPU, apart from the specialized ones so they are not queued behind them.
    Common::ThreadWorker uber_workers;
    PipelineInfo current_info{};
    GraphicsPipeline* current_pipeline{};
    std::array<DescriptorHeap, NumDescriptorHeaps> descriptor_heaps;
    std::array<vk::DescriptorSet, NumRasterizerSets> bound_descriptor_sets{};
    std::array<u32, NumDynamicOffsets> offsets{};

    std::array<u64, MAX_SHADER_STAGES> shader_hashes;
    std::array<Shader*, MAX_SHADER_STAGES> current_shaders;

    Shader trivial_vertex_shader;

    u64 current_program_id{0};
    std::vector<std::shared_ptr<ShaderDiskCache>> disk_caches;
    std::shared_ptr<ShaderDiskCache> curr_disk_cache{};

    // Claudhar: zero-pause shading state.
    Claudhar::Shading::DrawLedger ledger;
    std::optional<Pica::Shader::FSConfig> fs_config; ///< Of the current draw, before ApplyProfile.
    Pica::Shader::UserConfig fs_user{};
    std::optional<Pica::Shader::FSConfig> taken_uber_config; ///< Last one TakeUberConfig gave.
    GraphicsPipeline* selected_pipeline{};
    bool uber_selected{};
    VertexLayout software_layout{};
    std::array<std::unique_ptr<Shader>, Claudhar::Shading::UberKey::NUM_VARIANTS> uber_shaders;
    tsl::robin_map<u64, std::unique_ptr<GraphicsPipeline>, Common::IdentityHash<u64>>
        uber_pipelines;
    vk::UniquePipelineCache uber_pipeline_cache;
    std::string uber_pipeline_cache_path;
};

} // namespace Vulkan
