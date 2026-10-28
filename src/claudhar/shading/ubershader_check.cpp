// Copyright 2026 Claudhar Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

// claudhar-ubershader-check: proves on a Vulkan device that the fragment ubershader draws
// what Azahar's specialized shaders draw. For many random fragment configurations it
// renders one quad with the specialized shader Azahar would build (SPIR-V or GLSL
// generator, chosen as ShaderDiskCache does) and the same quad with the ubershader, and
// compares the color, the depth and, for shadow-map rendering, the shadow map. CI runs it
// on Mesa's lavapipe; on a phone the device probe will run the same comparison later.
//
// The Vulkan helpers here are deliberately minimal and local: one device, one queue, one
// command buffer, everything destroyed when main returns.

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <functional>
#include <random>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
#include <fmt/format.h>

#include "claudhar/shading/fs_ubershader.h"
#include "common/common_types.h"
#include "common/dynamic_library/dynamic_library.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/shader/generator/glsl_fs_shader_gen.h"
#include "video_core/shader/generator/shader_uniforms.h"
#include "video_core/shader/generator/spv_fs_shader_gen.h"

namespace {

using namespace Claudhar::Shading;
using Pica::Shader::FSConfig;
using Pica::Shader::Profile;
using Pica::Shader::UserConfig;
using Pica::Shader::Generator::FSUniformData;
using Pica::Shader::Generator::LightSrc;
using TevStageConfig = Pica::TexturingRegs::TevStageConfig;
// Filled only through Vulkan::InitDispatcher (see vk_platform.h).
using Dispatch = vk::detail::DispatchLoaderDynamic;

constexpr u32 Size = 64;       // Render target width and height.
constexpr u32 TexSize = 32;    // Width and height of every test texture.
constexpr u32 Vertices = 6;    // Two triangles covering the target.
constexpr u32 VertexVec4s = 6; // Position, color, texcoords, texcoord2+w, normquat, view.
constexpr u32 LfEntries = 24 * 256 + 128; // Lighting tables, then fog (RG32F).
constexpr u32 RgEntries = 3 * 128;        // Proctex noise, color map, alpha map (RG32F).
constexpr u32 RgbaEntries = 2 * 256;      // Proctex color table and differences (RGBA32F).
constexpr vk::Format ColorFormat = vk::Format::eR8G8B8A8Unorm;
constexpr vk::Format DepthFormat = vk::Format::eD32Sfloat;

/// Passes the vertices' varyings to the fragment shader at Azahar's locations.
constexpr const char* VertexShader = R"(
layout(set = 0, binding = 0, std140) uniform vs_data { vec4 v[36]; };
layout(location = 1) out vec4 primary_color;
layout(location = 2) out vec2 texcoord0;
layout(location = 3) out vec2 texcoord1;
layout(location = 4) out vec2 texcoord2;
layout(location = 5) out float texcoord0_w;
layout(location = 6) out vec4 normquat;
layout(location = 7) out vec3 view;
void main() {
    int b = gl_VertexIndex * 6;
    gl_Position = v[b];
    primary_color = v[b + 1];
    texcoord0 = v[b + 2].xy;
    texcoord1 = v[b + 2].zw;
    texcoord2 = v[b + 3].xy;
    texcoord0_w = v[b + 3].z;
    normquat = v[b + 4];
    view = v[b + 5].xyz;
}
)";

void Check(bool ok, const char* what) {
    if (!ok)
        throw std::runtime_error(what);
}

/// The harness's Vulkan objects; destroyers run in reverse order at exit.
struct Context {
    Common::DynamicLibrary library;
    Dispatch d;
    vk::Instance instance;
    vk::PhysicalDevice gpu;
    vk::Device device;
    vk::Queue queue;
    u32 family = 0;
    vk::CommandPool pool;
    vk::CommandBuffer cmd;
    vk::Fence fence;
    std::vector<std::function<void()>> destroyers;

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

    template <typename Handle>
    Handle Own(Handle handle) {
        destroyers.emplace_back([this, handle] { device.destroy(handle, nullptr, d); });
        return handle;
    }

    /// Allocates memory with the wanted properties, binds it and optionally maps it.
    template <typename Handle>
    void* Bind(Handle handle, vk::MemoryPropertyFlags want) {
        vk::MemoryRequirements req;
        if constexpr (std::is_same_v<Handle, vk::Buffer>)
            req = device.getBufferMemoryRequirements(handle, d);
        else
            req = device.getImageMemoryRequirements(handle, d);
        const auto types = gpu.getMemoryProperties(d);
        for (u32 i = 0; i < types.memoryTypeCount; ++i) {
            if (!(req.memoryTypeBits >> i & 1) ||
                (types.memoryTypes[i].propertyFlags & want) != want)
                continue;
            const auto memory = device.allocateMemory(
                {.allocationSize = req.size, .memoryTypeIndex = i}, nullptr, d);
            destroyers.emplace_back([this, memory] { device.freeMemory(memory, nullptr, d); });
            if constexpr (std::is_same_v<Handle, vk::Buffer>)
                device.bindBufferMemory(handle, memory, 0, d);
            else
                device.bindImageMemory(handle, memory, 0, d);
            return want & vk::MemoryPropertyFlagBits::eHostVisible
                       ? device.mapMemory(memory, 0, req.size, {}, d)
                       : nullptr;
        }
        throw std::runtime_error("no memory type with the needed properties");
    }

    /// A host-visible, coherent buffer and its mapping.
    std::pair<vk::Buffer, u8*> Buffer(vk::BufferUsageFlags usage, std::size_t size) {
        const auto buffer = Own(device.createBuffer({.size = size, .usage = usage}, nullptr, d));
        auto* mapped =
            static_cast<u8*>(Bind(buffer, vk::MemoryPropertyFlagBits::eHostVisible |
                                              vk::MemoryPropertyFlagBits::eHostCoherent));
        std::memset(mapped, 0, size);
        return {buffer, mapped};
    }

    /// A device-local image (2D, or a cube when layers is 6) and a view of all its layers.
    std::pair<vk::Image, vk::ImageView> Image(vk::Format format, u32 width, u32 layers,
                                              vk::ImageUsageFlags usage,
                                              vk::ImageViewType view_type) {
        const auto aspect = format == DepthFormat ? vk::ImageAspectFlagBits::eDepth
                                                  : vk::ImageAspectFlagBits::eColor;
        const auto image =
            Own(device.createImage({.flags = layers == 6 ? vk::ImageCreateFlagBits::eCubeCompatible
                                                         : vk::ImageCreateFlags{},
                                    .imageType = vk::ImageType::e2D,
                                    .format = format,
                                    .extent = {width, width, 1},
                                    .mipLevels = 1,
                                    .arrayLayers = layers,
                                    .samples = vk::SampleCountFlagBits::e1,
                                    .tiling = vk::ImageTiling::eOptimal,
                                    .usage = usage},
                                   nullptr, d));
        Bind(image, vk::MemoryPropertyFlagBits::eDeviceLocal);
        const auto view =
            Own(device.createImageView({.image = image,
                                        .viewType = view_type,
                                        .format = format,
                                        .subresourceRange = {aspect, 0, 1, 0, layers}},
                                       nullptr, d));
        return {image, view};
    }

    /// Records commands into the one command buffer, submits them and waits.
    void Run(const std::function<void(vk::CommandBuffer)>& record) {
        cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit}, d);
        record(cmd);
        cmd.end(d);
        queue.submit(vk::SubmitInfo{.commandBufferCount = 1, .pCommandBuffers = &cmd}, fence, d);
        Check(device.waitForFences(fence, true, UINT64_MAX, d) == vk::Result::eSuccess, "fence");
        device.resetFences(fence, d);
    }
};

/// Records a layout change of a whole image, waiting on all earlier work.
void Transition(Context& c, vk::CommandBuffer cmd, vk::Image image, vk::ImageAspectFlags aspect,
                u32 layers, vk::ImageLayout from, vk::ImageLayout to) {
    const vk::ImageMemoryBarrier2 barrier{.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                                          .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                                          .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                                          .dstAccessMask = vk::AccessFlagBits2::eMemoryRead |
                                                           vk::AccessFlagBits2::eMemoryWrite,
                                          .oldLayout = from,
                                          .newLayout = to,
                                          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                          .image = image,
                                          .subresourceRange = {aspect, 0, 1, 0, layers}};
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier}, c.d);
}

/// Creates the instance and a device with what the shaders need.
void CreateDevice(Context& c) {
    Check(c.library.Load(Common::DynamicLibrary::GetLibraryName("vulkan", 1)) ||
              c.library.Load(Common::DynamicLibrary::GetLibraryName("vulkan")),
          "no Vulkan library");
    const auto get_proc = c.library.GetSymbol<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
    Check(get_proc != nullptr, "no vkGetInstanceProcAddr");
    Vulkan::InitDispatcher(c.d, get_proc);
    const vk::ApplicationInfo app{.pApplicationName = "claudhar-ubershader-check",
                                  .apiVersion = VK_API_VERSION_1_3};
    c.instance = vk::createInstance({.pApplicationInfo = &app}, nullptr, c.d);
    Vulkan::InitDispatcher(c.d, c.instance);
    const auto gpus = c.instance.enumeratePhysicalDevices(c.d);
    Check(!gpus.empty(), "no Vulkan devices");
    c.gpu = gpus.front();
    const auto props = c.gpu.getProperties(c.d);
    Check(props.apiVersion >= VK_API_VERSION_1_3, "Vulkan 1.3 is required");
    fmt::print("device: {}\n", props.deviceName.data());

    const auto families = c.gpu.getQueueFamilyProperties(c.d);
    const auto graphics = std::find_if(families.begin(), families.end(), [](const auto& f) {
        return static_cast<bool>(f.queueFlags & vk::QueueFlagBits::eGraphics);
    });
    Check(graphics != families.end(), "no graphics queue");
    c.family = static_cast<u32>(graphics - families.begin());

    vk::PhysicalDeviceVulkan13Features f13{.synchronization2 = true, .dynamicRendering = true};
    vk::PhysicalDeviceFeatures2 features{.pNext = &f13};
    features.features.fragmentStoresAndAtomics = true; // Shadow-map rendering.
    const float priority = 1.0f;
    const vk::DeviceQueueCreateInfo queue_info{
        .queueFamilyIndex = c.family, .queueCount = 1, .pQueuePriorities = &priority};
    c.device = c.gpu.createDevice(
        {.pNext = &features, .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info}, nullptr,
        c.d);
    Vulkan::InitDispatcher(c.d, c.device);
    c.queue = c.device.getQueue(c.family, 0, c.d);
    c.pool = c.Own(c.device.createCommandPool(
        {.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer, .queueFamilyIndex = c.family},
        nullptr, c.d));
    c.cmd = c.device.allocateCommandBuffers({.commandPool = c.pool, .commandBufferCount = 1}, c.d)
                .front();
    c.fence = c.Own(c.device.createFence({}, nullptr, c.d));
}

/// Makes a shader module from SPIR-V; throws if there is none (a failed compile).
vk::ShaderModule Module(Context& c, std::span<const u32> spirv) {
    Check(!spirv.empty(), "shader failed to compile");
    return c.device.createShaderModule(
        {.codeSize = spirv.size() * sizeof(u32), .pCode = spirv.data()}, nullptr, c.d);
}

/// Everything one rendering needs, created once.
struct Scene {
    std::array<vk::DescriptorSetLayout, 3> set_layouts;
    vk::PipelineLayout layout;
    std::array<vk::DescriptorSet, 3> sets;
    vk::ShaderModule vertex;
    vk::Buffer vs_buffer, fs_buffer, uber_buffer, lf_buffer, rg_buffer, rgba_buffer, readback;
    u8 *vs_data, *fs_data, *uber_data, *lf_data, *rg_data, *rgba_data, *readback_data;
    vk::BufferView lf_view, rg_view, rgba_view;
    vk::Sampler linear, nearest;
    vk::Image color, depth, shadow_buffer, tex[3], cube, shadow2d, shadow_faces[6], normal;
    vk::ImageView color_view, depth_view, shadow_buffer_view, tex_view[3], cube_view, shadow2d_view,
        shadow_face_view[6], normal_view;
};

/// Byte offsets of the parts of the readback buffer.
constexpr std::size_t ReadColor = 0, ReadDepth = Size * Size * 4, ReadShadow = 2 * Size * Size * 4;

/// Fills an image with bytes through a staging copy and leaves it shader-readable.
void Upload(Context& c, Scene& s, vk::Image image, u32 layers, std::span<const u8> bytes,
            vk::ImageLayout final_layout) {
    std::memcpy(s.readback_data, bytes.data(), bytes.size());
    c.Run([&](vk::CommandBuffer cmd) {
        Transition(c, cmd, image, vk::ImageAspectFlagBits::eColor, layers,
                   vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal);
        const vk::BufferImageCopy copy{
            .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, layers},
            .imageExtent = {TexSize, TexSize, 1}};
        cmd.copyBufferToImage(s.readback, image, vk::ImageLayout::eTransferDstOptimal, copy, c.d);
        Transition(c, cmd, image, vk::ImageAspectFlagBits::eColor, layers,
                   vk::ImageLayout::eTransferDstOptimal, final_layout);
    });
}

/// Creates the descriptor layouts, buffers, textures and render targets.
void CreateScene(Context& c, Scene& s, std::mt19937& random) {
    using Type = vk::DescriptorType;
    const auto fragment = vk::ShaderStageFlagBits::eFragment;
    const std::array<vk::DescriptorSetLayoutBinding, 6> set0{{
        {0, Type::eUniformBuffer, 1, vk::ShaderStageFlagBits::eVertex},
        {2, Type::eUniformBuffer, 1, fragment},
        {3, Type::eUniformTexelBuffer, 1, fragment},
        {4, Type::eUniformTexelBuffer, 1, fragment},
        {5, Type::eUniformTexelBuffer, 1, fragment},
        {UBER_CONFIG_BINDING, Type::eUniformBuffer, 1, fragment},
    }};
    // Texture unit 0 has six elements so a shadow cube's faces fit; other shaders use one.
    const std::array<vk::DescriptorSetLayoutBinding, 3> set1{{
        {0, Type::eCombinedImageSampler, 6, fragment},
        {1, Type::eCombinedImageSampler, 1, fragment},
        {2, Type::eCombinedImageSampler, 1, fragment},
    }};
    const std::array<vk::DescriptorSetLayoutBinding, 2> set2{{
        {0, Type::eStorageImage, 1, fragment},
        {1, Type::eCombinedImageSampler, 1, fragment},
    }};
    s.set_layouts[0] = c.Own(c.device.createDescriptorSetLayout(
        {.bindingCount = static_cast<u32>(set0.size()), .pBindings = set0.data()}, nullptr, c.d));
    s.set_layouts[1] = c.Own(c.device.createDescriptorSetLayout(
        {.bindingCount = static_cast<u32>(set1.size()), .pBindings = set1.data()}, nullptr, c.d));
    s.set_layouts[2] = c.Own(c.device.createDescriptorSetLayout(
        {.bindingCount = static_cast<u32>(set2.size()), .pBindings = set2.data()}, nullptr, c.d));
    s.layout = c.Own(c.device.createPipelineLayout(
        {.setLayoutCount = 3, .pSetLayouts = s.set_layouts.data()}, nullptr, c.d));
    const std::array<vk::DescriptorPoolSize, 4> sizes{{
        {Type::eUniformBuffer, 3},
        {Type::eUniformTexelBuffer, 3},
        {Type::eCombinedImageSampler, 9},
        {Type::eStorageImage, 1},
    }};
    const auto pool = c.Own(c.device.createDescriptorPool(
        {.maxSets = 3, .poolSizeCount = static_cast<u32>(sizes.size()), .pPoolSizes = sizes.data()},
        nullptr, c.d));
    const auto sets = c.device.allocateDescriptorSets(
        {.descriptorPool = pool, .descriptorSetCount = 3, .pSetLayouts = s.set_layouts.data()},
        c.d);
    std::copy(sets.begin(), sets.end(), s.sets.begin());

    s.vertex =
        c.Own(Module(c, Vulkan::CompileGLSL(VertexShader, vk::ShaderStageFlagBits::eVertex)));

    const auto ubo = vk::BufferUsageFlagBits::eUniformBuffer;
    const auto texel = vk::BufferUsageFlagBits::eUniformTexelBuffer;
    std::tie(s.vs_buffer, s.vs_data) = c.Buffer(ubo, Vertices * VertexVec4s * 16);
    std::tie(s.fs_buffer, s.fs_data) = c.Buffer(ubo, sizeof(FSUniformData));
    std::tie(s.uber_buffer, s.uber_data) = c.Buffer(ubo, sizeof(UberConfig));
    std::tie(s.lf_buffer, s.lf_data) = c.Buffer(texel, LfEntries * 8);
    std::tie(s.rg_buffer, s.rg_data) = c.Buffer(texel, RgEntries * 8);
    std::tie(s.rgba_buffer, s.rgba_data) = c.Buffer(texel, RgbaEntries * 16);
    std::tie(s.readback, s.readback_data) =
        c.Buffer(vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eTransferSrc,
                 ReadShadow + Size * Size * 4);
    const auto view = [&](vk::Buffer buffer, vk::Format format) {
        return c.Own(c.device.createBufferView(
            {.buffer = buffer, .format = format, .range = VK_WHOLE_SIZE}, nullptr, c.d));
    };
    s.lf_view = view(s.lf_buffer, vk::Format::eR32G32Sfloat);
    s.rg_view = view(s.rg_buffer, vk::Format::eR32G32Sfloat);
    s.rgba_view = view(s.rgba_buffer, vk::Format::eR32G32B32A32Sfloat);

    // Lookup tables: values in [0, 1] with small differences, like real tables.
    std::uniform_real_distribution<float> unit(0.0f, 1.0f), diff(-0.004f, 0.004f);
    for (u32 i = 0; i < LfEntries; ++i) {
        const float pair[2] = {unit(random), diff(random)};
        std::memcpy(s.lf_data + i * 8, pair, 8);
    }
    for (u32 i = 0; i < RgEntries; ++i) {
        const float pair[2] = {unit(random), diff(random)};
        std::memcpy(s.rg_data + i * 8, pair, 8);
    }
    for (u32 i = 0; i < RgbaEntries; ++i) {
        const float quad[4] = {unit(random), unit(random), unit(random), unit(random)};
        std::memcpy(s.rgba_data + i * 16, quad, 16);
    }

    s.linear = c.Own(c.device.createSampler({.magFilter = vk::Filter::eLinear,
                                             .minFilter = vk::Filter::eLinear,
                                             .addressModeU = vk::SamplerAddressMode::eRepeat,
                                             .addressModeV = vk::SamplerAddressMode::eRepeat,
                                             .addressModeW = vk::SamplerAddressMode::eRepeat,
                                             .maxLod = 0.0f},
                                            nullptr, c.d));
    s.nearest = c.Own(c.device.createSampler({.maxLod = 0.0f}, nullptr, c.d));

    using Usage = vk::ImageUsageFlagBits;
    const auto sampled = Usage::eSampled | Usage::eTransferDst;
    std::tie(s.color, s.color_view) =
        c.Image(ColorFormat, Size, 1, Usage::eColorAttachment | Usage::eTransferSrc,
                vk::ImageViewType::e2D);
    std::tie(s.depth, s.depth_view) =
        c.Image(DepthFormat, Size, 1, Usage::eDepthStencilAttachment | Usage::eTransferSrc,
                vk::ImageViewType::e2D);
    std::tie(s.shadow_buffer, s.shadow_buffer_view) = c.Image(
        vk::Format::eR32Uint, Size, 1, Usage::eStorage | Usage::eTransferSrc | Usage::eTransferDst,
        vk::ImageViewType::e2D);
    std::vector<u8> bytes(TexSize * TexSize * 4 * 6);
    const auto fill = [&](u32 layers) {
        for (u32 i = 0; i < TexSize * TexSize * 4 * layers; ++i)
            bytes[i] = static_cast<u8>(random());
        return std::span<const u8>(bytes.data(), TexSize * TexSize * 4 * layers);
    };
    const auto read_only = vk::ImageLayout::eShaderReadOnlyOptimal;
    for (u32 i = 0; i < 3; ++i) {
        std::tie(s.tex[i], s.tex_view[i]) =
            c.Image(ColorFormat, TexSize, 1, sampled, vk::ImageViewType::e2D);
        Upload(c, s, s.tex[i], 1, fill(1), read_only);
    }
    std::tie(s.cube, s.cube_view) =
        c.Image(ColorFormat, TexSize, 6, sampled, vk::ImageViewType::eCube);
    Upload(c, s, s.cube, 6, fill(6), read_only);
    std::tie(s.normal, s.normal_view) =
        c.Image(ColorFormat, TexSize, 1, sampled, vk::ImageViewType::e2D);
    Upload(c, s, s.normal, 1, fill(1), read_only);
    // Shadow maps hold 24-bit depth and 8-bit intensity per texel. The 2D one is also bound
    // as the SPIR-V generator's storage image, so it is kept in the general layout.
    std::tie(s.shadow2d, s.shadow2d_view) = c.Image(
        vk::Format::eR32Uint, TexSize, 1, sampled | Usage::eStorage, vk::ImageViewType::e2D);
    Upload(c, s, s.shadow2d, 1, fill(1), vk::ImageLayout::eGeneral);
    for (u32 i = 0; i < 6; ++i) {
        std::tie(s.shadow_faces[i], s.shadow_face_view[i]) =
            c.Image(vk::Format::eR32Uint, TexSize, 1, sampled, vk::ImageViewType::e2D);
        Upload(c, s, s.shadow_faces[i], 1, fill(1), read_only);
    }

    // Buffers do not change; textures are bound per test because unit 0's type varies.
    const vk::DescriptorBufferInfo vs_info{s.vs_buffer, 0, VK_WHOLE_SIZE};
    const vk::DescriptorBufferInfo fs_info{s.fs_buffer, 0, VK_WHOLE_SIZE};
    const vk::DescriptorBufferInfo uber_info{s.uber_buffer, 0, VK_WHOLE_SIZE};
    const std::array<vk::WriteDescriptorSet, 6> writes{{
        {.dstSet = s.sets[0],
         .dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = Type::eUniformBuffer,
         .pBufferInfo = &vs_info},
        {.dstSet = s.sets[0],
         .dstBinding = 2,
         .descriptorCount = 1,
         .descriptorType = Type::eUniformBuffer,
         .pBufferInfo = &fs_info},
        {.dstSet = s.sets[0],
         .dstBinding = 3,
         .descriptorCount = 1,
         .descriptorType = Type::eUniformTexelBuffer,
         .pTexelBufferView = &s.lf_view},
        {.dstSet = s.sets[0],
         .dstBinding = 4,
         .descriptorCount = 1,
         .descriptorType = Type::eUniformTexelBuffer,
         .pTexelBufferView = &s.rg_view},
        {.dstSet = s.sets[0],
         .dstBinding = 5,
         .descriptorCount = 1,
         .descriptorType = Type::eUniformTexelBuffer,
         .pTexelBufferView = &s.rgba_view},
        {.dstSet = s.sets[0],
         .dstBinding = UBER_CONFIG_BINDING,
         .descriptorCount = 1,
         .descriptorType = Type::eUniformBuffer,
         .pBufferInfo = &uber_info},
    }};
    c.device.updateDescriptorSets(writes, {}, c.d);
}

/// Binds texture unit 0 as the variant declares it, and the utility images.
void BindTextures(Context& c, Scene& s, const UberKey& key) {
    using Type = vk::DescriptorType;
    const auto read_only = vk::ImageLayout::eShaderReadOnlyOptimal;
    std::array<vk::DescriptorImageInfo, 6> unit0;
    for (u32 i = 0; i < 6; ++i) {
        switch (key.tex0) {
        case UberKey::Tex0::Plain:
            unit0[i] = {s.linear, s.tex_view[0], read_only};
            break;
        case UberKey::Tex0::Cube:
            unit0[i] = {s.linear, s.cube_view, read_only};
            break;
        case UberKey::Tex0::Shadow2D:
            unit0[i] = {s.nearest, s.shadow2d_view, vk::ImageLayout::eGeneral};
            break;
        case UberKey::Tex0::ShadowCube:
            unit0[i] = {s.nearest, s.shadow_face_view[i], read_only};
            break;
        }
    }
    const vk::DescriptorImageInfo unit1{s.linear, s.tex_view[1], read_only};
    const vk::DescriptorImageInfo unit2{s.linear, s.tex_view[2], read_only};
    // Shadow-map rendering writes the shadow buffer; otherwise the SPIR-V generator reads
    // 2D shadow maps from this binding, as the renderer binds them.
    const vk::DescriptorImageInfo storage{{},
                                          key.shadow_rendering ? s.shadow_buffer_view
                                                               : s.shadow2d_view,
                                          vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo normal{s.linear, s.normal_view, read_only};
    const std::array<vk::WriteDescriptorSet, 5> writes{{
        {.dstSet = s.sets[1],
         .dstBinding = 0,
         .descriptorCount = 6,
         .descriptorType = Type::eCombinedImageSampler,
         .pImageInfo = unit0.data()},
        {.dstSet = s.sets[1],
         .dstBinding = 1,
         .descriptorCount = 1,
         .descriptorType = Type::eCombinedImageSampler,
         .pImageInfo = &unit1},
        {.dstSet = s.sets[1],
         .dstBinding = 2,
         .descriptorCount = 1,
         .descriptorType = Type::eCombinedImageSampler,
         .pImageInfo = &unit2},
        {.dstSet = s.sets[2],
         .dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = Type::eStorageImage,
         .pImageInfo = &storage},
        {.dstSet = s.sets[2],
         .dstBinding = 1,
         .descriptorCount = 1,
         .descriptorType = Type::eCombinedImageSampler,
         .pImageInfo = &normal},
    }};
    c.device.updateDescriptorSets(writes, {}, c.d);
}

/// A pipeline with the test vertex shader and a fragment shader: depth always passes and
/// is written, blending is off, and color is written unless the shader renders shadows.
vk::Pipeline MakePipeline(Context& c, Scene& s, vk::ShaderModule fragment, bool write_color) {
    const std::array<vk::PipelineShaderStageCreateInfo, 2> stages{{
        {.stage = vk::ShaderStageFlagBits::eVertex, .module = s.vertex, .pName = "main"},
        {.stage = vk::ShaderStageFlagBits::eFragment, .module = fragment, .pName = "main"},
    }};
    const vk::PipelineVertexInputStateCreateInfo vertex_input{};
    const vk::PipelineInputAssemblyStateCreateInfo assembly{
        .topology = vk::PrimitiveTopology::eTriangleList};
    const vk::Viewport viewport{0.0f, 0.0f, static_cast<float>(Size), static_cast<float>(Size),
                                0.0f, 1.0f};
    const vk::Rect2D scissor{{0, 0}, {Size, Size}};
    const vk::PipelineViewportStateCreateInfo viewport_state{
        .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1, .pScissors = &scissor};
    const vk::PipelineRasterizationStateCreateInfo raster{.cullMode = vk::CullModeFlagBits::eNone,
                                                          .lineWidth = 1.0f};
    const vk::PipelineMultisampleStateCreateInfo multisample{};
    const vk::PipelineDepthStencilStateCreateInfo depth{.depthTestEnable = true,
                                                        .depthWriteEnable = true,
                                                        .depthCompareOp = vk::CompareOp::eAlways};
    const vk::PipelineColorBlendAttachmentState attachment{
        .colorWriteMask = write_color
                              ? vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                    vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA
                              : vk::ColorComponentFlags{}};
    const vk::PipelineColorBlendStateCreateInfo blend{.attachmentCount = 1,
                                                      .pAttachments = &attachment};
    const vk::PipelineRenderingCreateInfo rendering{.colorAttachmentCount = 1,
                                                    .pColorAttachmentFormats = &ColorFormat,
                                                    .depthAttachmentFormat = DepthFormat};
    const vk::GraphicsPipelineCreateInfo info{.pNext = &rendering,
                                              .stageCount = 2,
                                              .pStages = stages.data(),
                                              .pVertexInputState = &vertex_input,
                                              .pInputAssemblyState = &assembly,
                                              .pViewportState = &viewport_state,
                                              .pRasterizationState = &raster,
                                              .pMultisampleState = &multisample,
                                              .pDepthStencilState = &depth,
                                              .pColorBlendState = &blend,
                                              .layout = s.layout};
    const auto result = c.device.createGraphicsPipeline({}, info, nullptr, c.d);
    Check(result.result == vk::Result::eSuccess, "pipeline creation failed");
    return result.value;
}

/// Draws the quad with a pipeline into cleared targets and copies color, depth and the
/// shadow buffer into the readback buffer.
void Render(Context& c, Scene& s, vk::Pipeline pipeline) {
    c.Run([&](vk::CommandBuffer cmd) {
        const auto color = vk::ImageAspectFlagBits::eColor;
        Transition(c, cmd, s.color, color, 1, vk::ImageLayout::eUndefined,
                   vk::ImageLayout::eColorAttachmentOptimal);
        Transition(c, cmd, s.depth, vk::ImageAspectFlagBits::eDepth, 1, vk::ImageLayout::eUndefined,
                   vk::ImageLayout::eDepthAttachmentOptimal);
        Transition(c, cmd, s.shadow_buffer, color, 1, vk::ImageLayout::eUndefined,
                   vk::ImageLayout::eGeneral);
        // Shadow buffer texels start at the farthest depth with full intensity.
        cmd.clearColorImage(s.shadow_buffer, vk::ImageLayout::eGeneral,
                            vk::ClearColorValue{}.setUint32({0xFFFFFFFFu, 0, 0, 0}),
                            vk::ImageSubresourceRange{color, 0, 1, 0, 1}, c.d);
        Transition(c, cmd, s.shadow_buffer, color, 1, vk::ImageLayout::eGeneral,
                   vk::ImageLayout::eGeneral);
        const vk::RenderingAttachmentInfo color_attachment{
            .imageView = s.color_view,
            .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .loadOp = vk::AttachmentLoadOp::eClear,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .clearValue = vk::ClearColorValue{std::array<float, 4>{0.1f, 0.2f, 0.3f, 0.4f}}};
        const vk::RenderingAttachmentInfo depth_attachment{
            .imageView = s.depth_view,
            .imageLayout = vk::ImageLayout::eDepthAttachmentOptimal,
            .loadOp = vk::AttachmentLoadOp::eClear,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .clearValue = vk::ClearValue{}.setDepthStencil({0.125f, 0})};
        cmd.beginRendering({.renderArea = {{0, 0}, {Size, Size}},
                            .layerCount = 1,
                            .colorAttachmentCount = 1,
                            .pColorAttachments = &color_attachment,
                            .pDepthAttachment = &depth_attachment},
                           c.d);
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline, c.d);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, s.layout, 0, s.sets, {}, c.d);
        cmd.draw(Vertices, 1, 0, 0, c.d);
        cmd.endRendering(c.d);
        Transition(c, cmd, s.color, color, 1, vk::ImageLayout::eColorAttachmentOptimal,
                   vk::ImageLayout::eTransferSrcOptimal);
        Transition(c, cmd, s.depth, vk::ImageAspectFlagBits::eDepth, 1,
                   vk::ImageLayout::eDepthAttachmentOptimal, vk::ImageLayout::eTransferSrcOptimal);
        Transition(c, cmd, s.shadow_buffer, color, 1, vk::ImageLayout::eGeneral,
                   vk::ImageLayout::eTransferSrcOptimal);
        const auto copy = [&](vk::Image image, vk::ImageAspectFlags aspect, std::size_t offset) {
            const vk::BufferImageCopy region{.bufferOffset = offset,
                                             .imageSubresource = {aspect, 0, 0, 1},
                                             .imageExtent = {Size, Size, 1}};
            cmd.copyImageToBuffer(image, vk::ImageLayout::eTransferSrcOptimal, s.readback, region,
                                  c.d);
        };
        copy(s.color, color, ReadColor);
        copy(s.depth, vk::ImageAspectFlagBits::eDepth, ReadDepth);
        copy(s.shadow_buffer, color, ReadShadow);
        const vk::MemoryBarrier2 to_host{.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                                         .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                                         .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                                         .dstAccessMask = vk::AccessFlagBits2::eHostRead};
        cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &to_host}, c.d);
    });
}

/// A random configuration for one variant. Values the generators treat as unknown are
/// included where Azahar only logs them, and left out where its generators would emit
/// invalid code (logic ops other than those it emulates, unknown texture types, lookup
/// table inputs and procedural texture filters).
FSConfig RandomConfig(std::mt19937& r, const UberKey& key, bool glsl) {
    const auto pick = [&r](u32 n) { return static_cast<u32>(r() % n); };
    const auto chance = [&r](u32 percent) { return r() % 100 < percent; };
    Pica::RegsInternal regs{};
    FSConfig c{regs};

    auto& fb = c.framebuffer;
    fb.alpha_test_func.Assign(static_cast<Pica::FramebufferRegs::CompareFunc>(
        chance(4) ? 0 : (chance(50) ? 1 : 2 + pick(6))));
    fb.scissor_test_mode.Assign(
        static_cast<Pica::RasterizerRegs::ScissorMode>(chance(60) ? 0 : 1 + pick(3)));
    fb.depthmap_enable.Assign(static_cast<Pica::RasterizerRegs::DepthBuffering>(pick(2)));
    fb.shadow_rendering.Assign(key.shadow_rendering);
    fb.alphablend_enable.Assign(chance(50));
    // Emulated logic ops: Clear, Copy, Set, NoOp (CopyInverted breaks the GLSL generator).
    constexpr std::array logic_ops{0u, 3u, 4u, 6u, 5u};
    fb.requested_logic_op =
        static_cast<Pica::FramebufferRegs::LogicOp>(logic_ops[pick(glsl ? 4 : 5)]);

    auto& tex = c.texture;
    using TT = Pica::TexturingRegs::TextureConfig::TextureType;
    switch (key.tex0) {
    case UberKey::Tex0::Plain: {
        constexpr std::array plain{TT::Texture2D, TT::Texture2D, TT::Projection2D, TT::Disabled};
        tex.texture0_type.Assign(plain[pick(4)]);
        break;
    }
    case UberKey::Tex0::Cube:
        tex.texture0_type.Assign(TT::TextureCube);
        break;
    case UberKey::Tex0::Shadow2D:
        tex.texture0_type.Assign(TT::Shadow2D);
        break;
    case UberKey::Tex0::ShadowCube:
        tex.texture0_type.Assign(TT::ShadowCube);
        break;
    }
    tex.texture2_use_coord1.Assign(pick(2));
    tex.combiner_buffer_input.Assign(pick(256));
    constexpr std::array fog_modes{0u, 0u, 5u, 5u, 7u, 3u};
    tex.fog_mode.Assign(static_cast<Pica::TexturingRegs::FogMode>(fog_modes[pick(6)]));
    tex.fog_flip.Assign(pick(2));
    tex.shadow_texture_orthographic.Assign(pick(2));
    for (u32 i = 0; i < 3; ++i) {
        tex.requested_wrap[i].s =
            static_cast<Pica::TexturingRegs::TextureConfig::WrapMode>(pick(4));
        tex.requested_wrap[i].t =
            static_cast<Pica::TexturingRegs::TextureConfig::WrapMode>(pick(4));
    }
    constexpr std::array sources{0u, 1u, 2u, 3u, 4u, 5u, 6u, 13u, 14u, 15u, 15u, 9u};
    for (auto& raw : tex.tev_stages) {
        TevStageConfig stage{};
        if (chance(30)) { // A pass-through stage.
            stage.sources_raw = 0xF000F;
        } else {
            const auto source = [&] { return sources[pick(chance(97) ? 11 : 12)]; };
            stage.sources_raw = source() | source() << 4 | source() << 8 | source() << 16 |
                                source() << 20 | source() << 24;
            stage.modifiers_raw = pick(16) | pick(16) << 4 | pick(16) << 8 | pick(8) << 12 |
                                  pick(8) << 16 | pick(8) << 20;
            stage.ops_raw = pick(chance(97) ? 10 : 16) | pick(chance(97) ? 10 : 16) << 16;
            stage.scales_raw = pick(4) | pick(4) << 16;
        }
        raw.sources_raw = stage.sources_raw;
        raw.modifiers_raw = stage.modifiers_raw;
        raw.ops_raw = stage.ops_raw;
        raw.scales_raw = stage.scales_raw;
        // Same masking as TextureConfig's constructor.
        if (stage.color_op == TevStageConfig::Operation::Dot3_RGBA) {
            raw.sources_raw &= 0xFFF;
            raw.modifiers_raw &= 0xFFF;
            raw.ops_raw &= 0xF;
        }
    }

    auto& l = c.lighting;
    if (chance(55)) {
        l.enable.Assign(1);
        l.src_num.Assign(1 + pick(8));
        l.bump_mode.Assign(static_cast<Pica::LightingRegs::LightingBumpMode>(pick(3)));
        l.bump_selector.Assign(pick(4));
        l.bump_renorm.Assign(pick(2));
        l.clamp_highlights.Assign(pick(2));
        constexpr std::array configs{0u, 1u, 2u, 3u, 4u, 5u, 6u, 8u};
        l.config.Assign(static_cast<Pica::LightingRegs::LightingConfig>(configs[pick(8)]));
        l.enable_primary_alpha.Assign(pick(2));
        l.enable_secondary_alpha.Assign(pick(2));
        l.enable_shadow.Assign(chance(30));
        if (l.enable_shadow) {
            l.shadow_primary.Assign(pick(2));
            l.shadow_secondary.Assign(pick(2));
            l.shadow_invert.Assign(pick(2));
            l.shadow_alpha.Assign(pick(2));
            l.shadow_selector.Assign(pick(4));
        }
        constexpr std::array scales{0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f};
        for (auto* lut :
             {&l.lut_d0, &l.lut_d1, &l.lut_sp, &l.lut_fr, &l.lut_rr, &l.lut_rg, &l.lut_rb}) {
            lut->enable.Assign(lut == &l.lut_sp || chance(60));
            if (lut->enable) {
                lut->abs_input.Assign(pick(2));
                lut->type.Assign(static_cast<Pica::LightingRegs::LightingLutInput>(pick(6)));
                lut->SetScale(scales[pick(6)]);
            }
        }
        for (u32 i = 0; i < l.src_num; ++i) {
            l.lights[i].raw = static_cast<u16>(r() & 0x3FF);
        }
    }

    auto& p = c.proctex;
    if (chance(35)) {
        p.enable.Assign(1);
        p.coord.Assign(pick(4));
        p.u_clamp.Assign(static_cast<Pica::TexturingRegs::ProcTexClamp>(pick(chance(95) ? 5 : 8)));
        p.v_clamp.Assign(static_cast<Pica::TexturingRegs::ProcTexClamp>(pick(chance(95) ? 5 : 8)));
        p.color_combiner.Assign(
            static_cast<Pica::TexturingRegs::ProcTexCombiner>(pick(chance(95) ? 10 : 16)));
        p.alpha_combiner.Assign(
            static_cast<Pica::TexturingRegs::ProcTexCombiner>(pick(chance(95) ? 10 : 16)));
        p.lut_filter.Assign(static_cast<Pica::TexturingRegs::ProcTexFilter>(pick(6)));
        p.separate_alpha.Assign(pick(2));
        p.noise_enable.Assign(pick(2));
        p.u_shift.Assign(static_cast<Pica::TexturingRegs::ProcTexShift>(pick(chance(95) ? 3 : 4)));
        p.v_shift.Assign(static_cast<Pica::TexturingRegs::ProcTexShift>(pick(chance(95) ? 3 : 4)));
        constexpr std::array widths{16, 32, 64, 128};
        p.lut_width = widths[pick(4)];
        p.lut_offset0 = static_cast<s32>(pick(256 - p.lut_width));
        p.lut_offset1 = static_cast<s32>(pick(256 - p.lut_width));
        p.lut_offset2 = static_cast<s32>(pick(256 - p.lut_width));
        p.lut_offset3 = static_cast<s32>(pick(256 - p.lut_width));
        // LOD 7 would read an eighth level that does not exist in either shader.
        p.lod_min = static_cast<u16>(pick(4));
        p.lod_max = static_cast<u16>(p.lod_min + pick(7 - p.lod_min));
        // A mipmapped procedural texture takes its LOD from screen-space derivatives, which
        // Vulkan leaves undefined next to pixels the scissor test has already discarded.
        // Both shaders then differ along the scissor edge however they are written, so the
        // two features are not combined here.
        if (p.lut_filter.Value() >= Pica::TexturingRegs::ProcTexFilter::NearestMipmapNearest) {
            fb.scissor_test_mode.Assign(Pica::RasterizerRegs::ScissorMode::Disabled);
        }
    }
    return c;
}

/// Random uniform data in the ranges real games use.
void RandomUniforms(std::mt19937& r, Scene& s, bool cube_coords) {
    std::uniform_real_distribution<float> unit(0.0f, 1.0f), wide(-2.0f, 2.0f);
    const auto vec3 = [&](float lo, float hi) {
        std::uniform_real_distribution<float> range(lo, hi);
        return Common::Vec3f{range(r), range(r), range(r)};
    };
    const auto byte_color = [&] {
        return Common::Vec4f{(r() % 256) / 255.0f, (r() % 256) / 255.0f, (r() % 256) / 255.0f,
                             (r() % 256) / 255.0f};
    };
    FSUniformData fs{};
    fs.framebuffer_scale = 1;
    fs.alphatest_ref = static_cast<int>(r() % 256);
    fs.depth_scale = std::uniform_real_distribution<float>(-0.7f, -0.3f)(r);
    fs.depth_offset = std::uniform_real_distribution<float>(0.0f, 0.15f)(r);
    fs.shadow_bias_constant = std::uniform_real_distribution<float>(0.5f, 1.5f)(r);
    fs.shadow_bias_linear = std::uniform_real_distribution<float>(0.0f, 0.5f)(r);
    fs.scissor_x1 = static_cast<int>(r() % 24);
    fs.scissor_y1 = static_cast<int>(r() % 24);
    fs.scissor_x2 = fs.scissor_x1 + 16 + static_cast<int>(r() % 24);
    fs.scissor_y2 = fs.scissor_y1 + 16 + static_cast<int>(r() % 24);
    fs.fog_lut_offset = 24 * 256;
    fs.proctex_noise_lut_offset = 0;
    fs.proctex_color_map_offset = 128;
    fs.proctex_alpha_map_offset = 256;
    fs.proctex_lut_offset = 0;
    fs.proctex_diff_lut_offset = 256;
    fs.proctex_bias = r() % 4 == 0 ? 0.0f : std::uniform_real_distribution<float>(0.1f, 4.0f)(r);
    fs.shadow_texture_bias = static_cast<int>(r() % 4096);
    for (u32 i = 0; i < 6; ++i)
        fs.lighting_lut_offset[i] = {
            static_cast<int>(i * 4) * 256, static_cast<int>(i * 4 + 1) * 256,
            static_cast<int>(i * 4 + 2) * 256, static_cast<int>(i * 4 + 3) * 256};
    fs.fog_color = vec3(0.0f, 1.0f);
    fs.proctex_noise_f = {std::uniform_real_distribution<float>(0.0f, 1.0f)(r),
                          std::uniform_real_distribution<float>(0.0f, 1.0f)(r)};
    fs.proctex_noise_a = {std::uniform_real_distribution<float>(-0.3f, 0.3f)(r),
                          std::uniform_real_distribution<float>(-0.3f, 0.3f)(r)};
    fs.proctex_noise_p = {unit(r), unit(r)};
    fs.lighting_global_ambient = vec3(0.0f, 0.3f);
    for (LightSrc& light : fs.light_src) {
        light.specular_0 = vec3(0.0f, 1.0f);
        light.specular_1 = vec3(0.0f, 1.0f);
        light.diffuse = vec3(0.0f, 1.0f);
        light.ambient = vec3(0.0f, 0.5f);
        light.position = vec3(-2.0f, 2.0f);
        light.spot_direction = vec3(-1.0f, 1.0f).Normalized();
        light.dist_atten_bias = std::uniform_real_distribution<float>(0.0f, 0.5f)(r);
        light.dist_atten_scale = std::uniform_real_distribution<float>(0.0f, 0.5f)(r);
    }
    for (auto& color : fs.const_color)
        color = byte_color();
    fs.tev_combiner_buffer_color = byte_color();
    fs.tex_lod_bias = vec3(-1.0f, 1.0f);
    for (auto& color : fs.tex_border_color)
        color = byte_color();
    std::memcpy(s.fs_data, &fs, sizeof(fs));

    // A quad over the whole target with a different value at each corner of each triangle.
    constexpr std::array<std::array<float, 2>, Vertices> corners{
        {{-1, -1}, {1, -1}, {-1, 1}, {-1, 1}, {1, -1}, {1, 1}}};
    std::array<Common::Vec4f, Vertices * VertexVec4s> v;
    for (u32 i = 0; i < Vertices; ++i) {
        const float w = std::uniform_real_distribution<float>(1.0f, 1.25f)(r);
        const float z = std::uniform_real_distribution<float>(0.1f, 0.9f)(r);
        const auto tc = [&] { return std::uniform_real_distribution<float>(-0.25f, 1.25f)(r); };
        const float tc_w =
            cube_coords ? wide(r) * 0.75f : std::uniform_real_distribution<float>(0.5f, 1.5f)(r);
        v[i * 6 + 0] = {corners[i][0] * w, corners[i][1] * w, z * w, w};
        v[i * 6 + 1] = {unit(r), unit(r), unit(r), unit(r)};
        v[i * 6 + 2] = {tc(), tc(), tc(), tc()};
        v[i * 6 + 3] = {tc(), tc(), tc_w, 0.0f};
        v[i * 6 + 4] = {wide(r), wide(r), wide(r), wide(r)};
        v[i * 6 + 5] = {wide(r), wide(r), wide(r), 0.0f};
    }
    std::memcpy(s.vs_data, v.data(), sizeof(v));
}

/// Differences between the specialized and the ubershader rendering of one test.
/// The depth the target is cleared to; a pixel that keeps it was discarded.
constexpr float ClearDepth = 0.125f;

/// Differences between the specialized and the ubershader rendering of one test.
struct Difference {
    u32 color_pixels = 0;  ///< Pixels whose color differs at all.
    u32 color_big = 0;     ///< Pixels drawn by both whose color differs by more than one step.
    u32 color_max = 0;     ///< Largest difference of one channel of a pixel drawn by both.
    u32 flips = 0;         ///< Pixels one shader discarded and the other drew.
    u32 depth_values = 0;  ///< Pixels drawn by both whose depth differs in any bit.
    u32 shadow_pixels = 0; ///< Shadow texels that differ at all.
    u32 shadow_depth_pixels = 0; ///< Shadow texels whose 24-bit depth differs.
    u32 shadow_big = 0;          ///< Shadow texels whose intensity differs by more than one.
    u32 shadow_max = 0;          ///< Largest difference of a shadow texel's intensity.
    std::string examples;        ///< The first few differing values, specialized then ubershader.

    [[nodiscard]] bool Identical() const {
        return color_pixels == 0 && flips == 0 && depth_values == 0 && shadow_pixels == 0;
    }
};

Difference Compare(std::span<const u8> a, std::span<const u8> b, bool shadow_rendering) {
    Difference diff;
    u32 shown = 0;
    const auto word = [](std::span<const u8> data, std::size_t offset) {
        u32 value;
        std::memcpy(&value, &data[offset], 4);
        return value;
    };
    const u32 clear_depth = std::bit_cast<u32>(ClearDepth);
    for (u32 i = 0; i < Size * Size; ++i) {
        u32 largest = 0;
        for (u32 k = 0; k < 4; ++k) {
            const u32 x = a[ReadColor + i * 4 + k], y = b[ReadColor + i * 4 + k];
            largest = std::max(largest, x > y ? x - y : y - x);
        }
        const u32 depth_a = word(a, ReadDepth + i * 4), depth_b = word(b, ReadDepth + i * 4);
        const u32 shadow_a = word(a, ReadShadow + i * 4), shadow_b = word(b, ReadShadow + i * 4);
        const bool flip = (depth_a == clear_depth) != (depth_b == clear_depth);
        diff.flips += flip;
        diff.depth_values += !flip && depth_a != depth_b;
        // Shadow-map rendering writes no color; the target then holds undefined values.
        if (!shadow_rendering && largest != 0) {
            diff.color_pixels++;
            if (!flip) {
                diff.color_big += largest > 1;
                diff.color_max = std::max(diff.color_max, largest);
            }
        }
        // A discarded fragment leaves its shadow texel alone, so flips are excluded here too.
        diff.shadow_pixels += shadow_a != shadow_b;
        const u32 intensity_a = shadow_a & 0xFF, intensity_b = shadow_b & 0xFF;
        const u32 intensity =
            intensity_a > intensity_b ? intensity_a - intensity_b : intensity_b - intensity_a;
        if (!flip) {
            diff.shadow_depth_pixels += (shadow_a >> 8) != (shadow_b >> 8);
            diff.shadow_big += intensity > 1;
            diff.shadow_max = std::max(diff.shadow_max, intensity);
        }
        if ((largest > 1 && !shadow_rendering) || depth_a != depth_b || intensity > 1 ||
            (shadow_a >> 8) != (shadow_b >> 8)) {
            if (shown++ < 3) {
                diff.examples +=
                    fmt::format("  pixel ({}, {}): color {:08x}/{:08x} depth {:08x}/{:08x} shadow "
                                "{:08x}/{:08x}\n",
                                i % Size, i / Size, word(a, ReadColor + i * 4),
                                word(b, ReadColor + i * 4), depth_a, depth_b, shadow_a, shadow_b);
            }
        }
    }
    return diff;
}

/// The whole configuration as hex words, for --replay.
std::string Hex(const FSConfig& c) {
    std::array<u32, sizeof(FSConfig) / 4> words;
    std::memcpy(words.data(), &c, sizeof(FSConfig));
    std::string out;
    for (const u32 word : words)
        out += fmt::format("{:08x}", word);
    return out;
}

/// The configuration that Hex printed.
FSConfig FromHex(const std::string& hex) {
    Check(hex.size() == sizeof(FSConfig) * 2, "--replay needs the hex string a report printed");
    std::array<u32, sizeof(FSConfig) / 4> words;
    for (std::size_t i = 0; i < words.size(); ++i)
        words[i] = static_cast<u32>(std::stoul(hex.substr(i * 8, 8), nullptr, 16));
    const Pica::RegsInternal regs{};
    FSConfig config{regs};
    std::memcpy(&config, words.data(), sizeof(FSConfig));
    return config;
}

/// Two lines describing a configuration: the main fields, then all of it for --replay.
std::string Describe(const FSConfig& c) {
    std::string out =
        fmt::format("fb={:08x} tex={:08x} light={:08x} proctex={:08x} tev=", c.framebuffer.raw,
                    c.texture.raw, c.lighting.raw, c.proctex.raw);
    for (const auto& stage : c.texture.tev_stages)
        out += fmt::format("[{:x} {:x} {:x} {:x}]", stage.sources_raw, stage.modifiers_raw,
                           stage.ops_raw, stage.scales_raw);
    return out + "\n  config " + Hex(c);
}

struct Options {
    u32 count = 400;
    u32 seed = 1;
    bool verbose = false;
    bool dump = false;     ///< Write the specialized GLSL of minimized failures to files.
    bool minimize = false; ///< Reduce each failure to the fewest features that still fail.
    u32 max_minimized = 8; ///< Failures to minimize per suite; minimizing is slow.
    bool strict = false;   ///< Fail on any difference beyond one step, to investigate it.
    std::string replay;    ///< Run only this configuration (as printed) at test number `seed`.
};

/// Runs the comparison for one generator choice; returns the number of failed tests.
/// Renders configurations with both shaders and compares them, keeping one ubershader
/// pipeline per variant for the whole suite.
struct Tester {
    Context& c;
    Scene& s;
    Profile profile;
    std::array<vk::Pipeline, UberKey::NUM_VARIANTS> uber{};

    /// Whether Azahar builds this configuration with the SPIR-V generator.
    bool UsesSpirv(const FSConfig& config) const {
        return profile.vk_use_spirv_generator && !config.UsesSpirvIncompatibleConfig();
    }

    /// The ubershader pipeline of a variant, built on first use.
    vk::Pipeline Ubershader(const UberKey& key) {
        vk::Pipeline& pipeline = uber[key.Index()];
        if (pipeline)
            return pipeline;
        const auto source = GenerateFragmentUbershader(key, profile);
        const auto spirv = Vulkan::CompileGLSL(source, vk::ShaderStageFlagBits::eFragment);
        if (spirv.empty()) {
            // Keep the source so the compiler error can be reproduced with glslang.
            const auto path = fmt::format("ubershader_{}.frag", key.Index());
            if (std::FILE* file = std::fopen(path.c_str(), "w")) {
                std::fputs(source.c_str(), file);
                std::fclose(file);
            }
            fmt::print("ubershader variant {} failed to compile; source in {}\n", key.Index(),
                       path);
        }
        const auto module = c.Own(Module(c, spirv));
        pipeline = c.Own(MakePipeline(c, s, module, !key.shadow_rendering));
        return pipeline;
    }

    /// Draws one configuration with the specialized shader and the ubershader, with the
    /// uniforms and vertices already in the scene, and compares the results.
    Difference Test(const FSConfig& config, const UserConfig& user) {
        const UberKey key = MakeUberKey(config, user, profile);
        FSConfig applied = config;
        applied.ApplyProfile(profile);
        const UberConfig uber_config = MakeUberConfig(applied);
        std::memcpy(s.uber_data, &uber_config, sizeof(uber_config));
        BindTextures(c, s, key);

        // The specialized shader, from the generator ShaderDiskCache::UseFragmentShader uses.
        const std::vector<u32> spirv =
            UsesSpirv(config)
                ? Pica::Shader::Generator::SPIRV::GenerateFragmentShader(config, profile)
                : Vulkan::CompileGLSL(
                      Pica::Shader::Generator::GLSL::GenerateFragmentShader(config, user, profile),
                      vk::ShaderStageFlagBits::eFragment);
        const auto specialized_module = Module(c, spirv);
        const auto specialized = MakePipeline(c, s, specialized_module, !key.shadow_rendering);
        Render(c, s, specialized);
        constexpr std::size_t read_size = ReadShadow + Size * Size * 4;
        const std::vector<u8> expected(s.readback_data, s.readback_data + read_size);
        c.device.destroy(specialized, nullptr, c.d);
        c.device.destroy(specialized_module, nullptr, c.d);

        Render(c, s, Ubershader(key));
        return Compare(expected, {s.readback_data, read_size}, key.shadow_rendering);
    }
};

/// Pixels allowed to differ by more than one 8-bit step, or to be discarded by only one
/// shader: 2% of the target.
constexpr u32 Tolerance = Size * Size / 50;

/// Whether the ubershader matched the specialized shader closely enough. The driver may
/// fuse a multiply and an add into one rounding step in one shader and not in the other
/// (its choice depends on the shape of the code around them), so a channel can land one
/// 8-bit step apart; a later combiner stage can scale that up, and an alpha test can
/// change its decision. Those stay on a few pixels. A real difference in meaning shows on
/// many pixels, and depth, which is one expression in both shaders, must match exactly
/// wherever both drew, or surfaces drawn by both would fight.
bool Passes(const Difference& diff, bool strict) {
    if (strict) {
        // For investigating: any difference beyond one step fails.
        return diff.depth_values == 0 && diff.shadow_depth_pixels == 0 && diff.flips == 0 &&
               diff.color_big == 0 && diff.shadow_big == 0;
    }
    return diff.depth_values == 0 && diff.shadow_depth_pixels == 0 && diff.flips <= Tolerance &&
           diff.color_big <= Tolerance && diff.shadow_big <= Tolerance;
}

/// Simplifies a failing configuration one feature at a time, keeping each change that
/// still fails, and returns the smallest failing configuration found. This points at the
/// feature the two shaders disagree on.
FSConfig Minimize(Tester& tester, FSConfig config, const UserConfig& user, bool strict) {
    const Pica::RegsInternal zero_regs{};
    const FSConfig zero{zero_regs};
    std::vector<std::function<void(FSConfig&)>> steps{
        [&](FSConfig& c) { c.lighting = zero.lighting; },
        [&](FSConfig& c) { c.proctex = zero.proctex; },
        [](FSConfig& c) { c.texture.fog_mode.Assign(Pica::TexturingRegs::FogMode::None); },
        [](FSConfig& c) {
            c.framebuffer.alpha_test_func.Assign(Pica::FramebufferRegs::CompareFunc::Always);
        },
        [](FSConfig& c) {
            c.framebuffer.scissor_test_mode.Assign(Pica::RasterizerRegs::ScissorMode::Disabled);
        },
        [](FSConfig& c) {
            c.framebuffer.requested_logic_op = Pica::FramebufferRegs::LogicOp::Copy;
        },
        [](FSConfig& c) { c.texture.combiner_buffer_input.Assign(0); },
        [](FSConfig& c) {
            for (auto& wrap : c.texture.requested_wrap)
                wrap = {};
        },
        [](FSConfig& c) { c.framebuffer.depthmap_enable.Assign(Pica::RasterizerRegs::ZBuffering); },
        [](FSConfig& c) { c.lighting.src_num.Assign(1); },
        [](FSConfig& c) {
            c.lighting.bump_mode.Assign(Pica::LightingRegs::LightingBumpMode::None);
        },
        [](FSConfig& c) { c.lighting.enable_shadow.Assign(0); },
        [](FSConfig& c) { c.lighting.clamp_highlights.Assign(0); },
        [](FSConfig& c) { c.lighting.enable_primary_alpha.Assign(0); },
        [](FSConfig& c) { c.lighting.enable_secondary_alpha.Assign(0); },
        [](FSConfig& c) { c.proctex.noise_enable.Assign(0); },
        [](FSConfig& c) { c.proctex.separate_alpha.Assign(0); },
        [](FSConfig& c) {
            c.proctex.lut_filter.Assign(Pica::TexturingRegs::ProcTexFilter::Nearest);
        },
    };
    for (u32 i = 0; i < 8; ++i) {
        steps.push_back([i](FSConfig& c) { c.lighting.lights[i].raw &= 0x7; });
    }
    const std::array lut_members{
        &Pica::Shader::LightConfig::lut_d0, &Pica::Shader::LightConfig::lut_d1,
        &Pica::Shader::LightConfig::lut_sp, &Pica::Shader::LightConfig::lut_fr,
        &Pica::Shader::LightConfig::lut_rr, &Pica::Shader::LightConfig::lut_rg,
        &Pica::Shader::LightConfig::lut_rb};
    for (const auto member : lut_members) {
        steps.push_back([member](FSConfig& c) { (c.lighting.*member).raw = 0; });
    }
    for (u32 i = 0; i < 6; ++i) {
        steps.push_back([i](FSConfig& c) {
            c.texture.tev_stages[i] = {
                .sources_raw = 0xF000F, .modifiers_raw = 0, .ops_raw = 0, .scales_raw = 0};
        });
        steps.push_back([i](FSConfig& c) { c.texture.tev_stages[i].scales_raw = 0; });
        steps.push_back([i](FSConfig& c) { c.texture.tev_stages[i].ops_raw &= 0xF; });
        steps.push_back([i](FSConfig& c) { c.texture.tev_stages[i].ops_raw &= 0xF0000; });
        for (u32 field = 0; field < 6; ++field) {
            const u32 shift = field < 3 ? field * 4 : 16 + (field - 3) * 4;
            steps.push_back([i, shift](FSConfig& c) {
                c.texture.tev_stages[i].sources_raw &= ~(0xFu << shift);
            });
            const u32 modifier_shift = field < 3 ? field * 4 : 12 + (field - 3) * 4;
            steps.push_back([i, modifier_shift](FSConfig& c) {
                c.texture.tev_stages[i].modifiers_raw &= ~(0xFu << modifier_shift);
            });
        }
    }
    // Some steps undo each other (making a stage pass through sets source bits that other
    // steps clear), so a configuration is never tried twice.
    std::set<u64> tried{config.Hash()};
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& step : steps) {
            FSConfig candidate = config;
            step(candidate);
            if (!tried.insert(candidate.Hash()).second)
                continue;
            if (!Passes(tester.Test(candidate, user), strict)) {
                config = candidate;
                changed = true;
            }
        }
    }
    return config;
}

/// Runs the comparison for one generator choice; returns the number of failed tests.
u32 RunSuite(Context& c, Scene& s, const Options& options, bool spirv_generator) {
    Tester tester{c, s, {}};
    Profile& profile = tester.profile;
    profile.has_separable_shaders = true;
    profile.has_custom_border_color = false; // Exercise the shader's border colors.
    profile.has_logic_op = false;            // Exercise the shader's logic ops.
    profile.is_vulkan = true;
    profile.vk_use_spirv_generator = spirv_generator;

    std::mt19937 random{options.seed};
    u32 failures = 0, exact = 0;
    u32 worst_color = 0, worst_shadow = 0;
    std::array<u32, 3> by_size{}; // Tests whose largest color difference is 0, 1, more.
    for (u32 test = 0; test < options.count; ++test) {
        // Mostly the plain variant, which draws nearly everything in real games.
        UberKey key{};
        const u32 roll = random() % 20;
        key.tex0 =
            roll < 13 ? UberKey::Tex0::Plain : static_cast<UberKey::Tex0>(1 + (roll - 13) % 3);
        key.shadow_rendering = random() % 10 == 0;
        // Custom normal maps are only read by the GLSL generator.
        const UserConfig user{.raw = !spirv_generator && random() % 8 == 0 ? 1u : 0u};
        FSConfig config =
            RandomConfig(random, key, !spirv_generator || UsesGlslSemantics(key, profile));
        key = MakeUberKey(config, user, profile);
        RandomUniforms(random, s,
                       key.tex0 == UberKey::Tex0::Cube || key.tex0 == UberKey::Tex0::ShadowCube);
        // A replay draws the given configuration with this test's uniforms and vertices.
        if (!options.replay.empty() && test != options.count - 1)
            continue;
        if (!options.replay.empty())
            config = FromHex(options.replay);

        const Difference diff = tester.Test(config, user);
        const bool pass = Passes(diff, options.strict);
        exact += diff.Identical();
        worst_color = std::max(worst_color, diff.color_max);
        worst_shadow = std::max(worst_shadow, diff.shadow_max);
        by_size[std::min(diff.color_max, 2u)]++;
        failures += !pass;
        if (pass && !options.verbose)
            continue;
        const auto report = [&](const char* label, const FSConfig& shown, const Difference& d) {
            fmt::print("{} test {} variant {} ({}): color {} px, {} by more than 1 (max {}); "
                       "{} flips; depth {} px; shadow {} px, depth {} px, {} by more than 1 "
                       "(max {})\n  {}\n",
                       label, test, MakeUberKey(shown, user, profile).Index(),
                       tester.UsesSpirv(shown) ? "SPIR-V" : "GLSL", d.color_pixels, d.color_big,
                       d.color_max, d.flips, d.depth_values, d.shadow_pixels, d.shadow_depth_pixels,
                       d.shadow_big, d.shadow_max, Describe(shown));
            std::fputs(d.examples.c_str(), stdout);
        };
        report(pass ? "ok" : "MISMATCH", config, diff);
        if (!pass && options.minimize && failures <= options.max_minimized) {
            const FSConfig minimal = Minimize(tester, config, user, options.strict);
            report("  minimal", minimal, tester.Test(minimal, user));
            if (options.dump && !tester.UsesSpirv(minimal)) {
                // Keep the specialized GLSL to compare with the ubershader's.
                const auto path =
                    fmt::format("failure_{}_{}.frag", spirv_generator ? "spirv" : "glsl", test);
                if (std::FILE* file = std::fopen(path.c_str(), "w")) {
                    std::fputs(Pica::Shader::Generator::GLSL::GenerateFragmentShader(minimal, user,
                                                                                     profile)
                                   .c_str(),
                               file);
                    std::fclose(file);
                }
            }
        }
    }
    fmt::print("{} generator: {} tests, {} identical, {} failed; largest color difference {} "
               "(tests with 0 / 1 / more: {} / {} / {}), largest shadow difference {}\n",
               spirv_generator ? "SPIR-V" : "GLSL", options.count, exact, failures, worst_color,
               by_size[0], by_size[1], by_size[2], worst_shadow);
    return failures;
}

} // namespace

int main(int argc, char** argv) {
    // Progress lines appear as they happen even when the output goes to a file.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--count" && i + 1 < argc)
            options.count = static_cast<u32>(std::stoul(argv[++i]));
        else if (arg == "--seed" && i + 1 < argc)
            options.seed = static_cast<u32>(std::stoul(argv[++i]));
        else if (arg == "--verbose")
            options.verbose = true;
        else if (arg == "--dump")
            options.dump = true;
        else if (arg == "--minimize")
            options.minimize = true;
        else if (arg == "--strict")
            options.strict = true;
        else if (arg == "--replay" && i + 1 < argc)
            options.replay = argv[++i];
        else if (arg == "--max-minimized" && i + 1 < argc)
            options.max_minimized = static_cast<u32>(std::stoul(argv[++i]));
        else {
            std::fprintf(stderr,
                         "usage: %s [--count N] [--seed S] [--verbose] [--minimize] "
                         "[--max-minimized N] [--strict] [--replay HEX] [--dump]\n",
                         argv[0]);
            return 2;
        }
    }
    try {
        Context c;
        CreateDevice(c);
        Scene scene;
        std::mt19937 random{options.seed};
        CreateScene(c, scene, random);
        // Azahar's default (SPIR-V generator), then its GLSL option.
        const u32 failures = RunSuite(c, scene, options, true) + RunSuite(c, scene, options, false);
        return failures == 0 ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
