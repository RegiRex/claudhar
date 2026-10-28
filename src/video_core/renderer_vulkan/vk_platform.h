// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.
// Modified by the Claudhar Project in 2026; changes are marked "Claudhar:".

#pragma once

#include <memory>
#include <variant>

#include "common/common_types.h"
#include "common/dynamic_library/dynamic_library.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Frontend {
class EmuWindow;
class GraphicsContext;
enum class WindowSystemType : u8;
} // namespace Frontend

namespace Vulkan {

constexpr u32 TargetVulkanApiVersion = VK_API_VERSION_1_1;

using DebugCallback =
    std::variant<vk::UniqueDebugUtilsMessengerEXT, vk::UniqueDebugReportCallbackEXT>;

std::shared_ptr<Common::DynamicLibrary> OpenLibrary(
    [[maybe_unused]] Frontend::GraphicsContext* context = nullptr);

vk::SurfaceKHR CreateSurface(vk::Instance instance, const Frontend::EmuWindow& emu_window);

vk::UniqueInstance CreateInstance(const Common::DynamicLibrary& library,
                                  Frontend::WindowSystemType window_type, bool enable_validation,
                                  bool dump_command_buffers);

DebugCallback CreateDebugCallback(vk::Instance instance, bool& debug_utils_supported);

// Claudhar: fill a dispatcher other than the renderer's (the device probe's) with the same
// code the renderer uses. DispatchLoaderDynamic::init is an inline function whose body
// depends on the VK_USE_PLATFORM_* macros of the file that calls it. Calling it from a file
// with different macros adds a second, different definition, and the linker may keep that
// one for the renderer too: the probe's copy, without the Android surface function, made
// every game crash in CreateSurface. These helpers keep each init call in the same file as
// the renderer's own call, so only one definition exists. Use them instead of init().
void InitDispatcher(vk::detail::DispatchLoaderDynamic& dispatch,
                    PFN_vkGetInstanceProcAddr get_instance_proc_addr);
void InitDispatcher(vk::detail::DispatchLoaderDynamic& dispatch, vk::Instance instance);
void InitDispatcher(vk::detail::DispatchLoaderDynamic& dispatch, vk::Device device);

template <typename T>
concept VulkanHandleType = vk::isVulkanHandleType<T>::value;

template <VulkanHandleType HandleType>
void SetObjectName(vk::Device device, const HandleType& handle, std::string_view debug_name) {
    const vk::DebugUtilsObjectNameInfoEXT name_info = {
        .objectType = HandleType::objectType,
        .objectHandle = reinterpret_cast<u64>(static_cast<typename HandleType::NativeType>(handle)),
        .pObjectName = debug_name.data(),
    };
    device.setDebugUtilsObjectNameEXT(name_info);
}

template <VulkanHandleType HandleType, typename... Args>
void SetObjectName(vk::Device device, const HandleType& handle, const char* format,
                   const Args&... args) {
    const std::string debug_name = fmt::vformat(format, fmt::make_format_args(args...));
    SetObjectName(device, handle, debug_name);
}

} // namespace Vulkan
