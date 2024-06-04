// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <boost/container/small_vector.hpp>
#include "common/alignment.h"
#include "core/memory.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_stream_buffer.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {

ComputePipeline::ComputePipeline(const Instance& instance_, Scheduler& scheduler_,
                                 vk::PipelineCache pipeline_cache, const Shader::Info* info_,
                                 vk::ShaderModule module)
    : instance{instance_}, scheduler{scheduler_}, info{*info_} {
    const vk::PipelineShaderStageCreateInfo shader_ci = {
        .stage = vk::ShaderStageFlagBits::eCompute,
        .module = module,
        .pName = "main",
    };

    u32 binding{};
    boost::container::small_vector<vk::DescriptorSetLayoutBinding, 32> bindings;
    for (const auto& buffer : info.buffers) {
        bindings.push_back({
            .binding = binding++,
            .descriptorType = buffer.is_storage ? vk::DescriptorType::eStorageBuffer
                                                : vk::DescriptorType::eUniformBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
    }
    for (const auto& image : info.images) {
        bindings.push_back({
            .binding = binding++,
            .descriptorType = vk::DescriptorType::eSampledImage,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
    }
    for (const auto& sampler : info.samplers) {
        bindings.push_back({
            .binding = binding++,
            .descriptorType = vk::DescriptorType::eSampler,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
    }

    const vk::DescriptorSetLayoutCreateInfo desc_layout_ci = {
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    };
    desc_layout = instance.GetDevice().createDescriptorSetLayoutUnique(desc_layout_ci);

    const vk::DescriptorSetLayout set_layout = *desc_layout;
    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = 1U,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 0,
        .pPushConstantRanges = nullptr,
    };
    pipeline_layout = instance.GetDevice().createPipelineLayoutUnique(layout_info);

    const vk::ComputePipelineCreateInfo compute_pipeline_ci = {
        .stage = shader_ci,
        .layout = *pipeline_layout,
    };
    auto result =
        instance.GetDevice().createComputePipelineUnique(pipeline_cache, compute_pipeline_ci);
    if (result.result == vk::Result::eSuccess) {
        pipeline = std::move(result.value);
    } else {
        UNREACHABLE_MSG("Graphics pipeline creation failed!");
    }
}

ComputePipeline::~ComputePipeline() = default;

void ComputePipeline::BindResources(Core::MemoryManager* memory, StreamBuffer& staging,
                                    VideoCore::TextureCache& texture_cache) const {
    static constexpr u64 MinUniformAlignment = 64;

    const auto map_staging = [&](auto src, size_t size) {
        const auto [data, offset, _] = staging.Map(size, MinUniformAlignment);
        std::memcpy(data, reinterpret_cast<const void*>(src), size);
        staging.Commit(size);
        return offset;
    };

    // Bind resource buffers and textures.
    boost::container::static_vector<vk::DescriptorBufferInfo, 4> buffer_infos;
    boost::container::static_vector<vk::DescriptorImageInfo, 8> image_infos;
    boost::container::small_vector<vk::WriteDescriptorSet, 16> set_writes;
    u32 binding{};

    for (const auto& buffer : info.buffers) {
        const auto vsharp = info.ReadUd<AmdGpu::Buffer>(buffer.sgpr_base, buffer.dword_offset);
        const u32 size = vsharp.GetSize();
        const VAddr addr = vsharp.base_address.Value();
        texture_cache.OnCpuWrite(addr);
        const u32 offset = map_staging(addr, size);
        //const auto [vk_buffer, offset] = memory->GetVulkanBuffer(addr);
        buffer_infos.emplace_back(staging.Handle(), offset, size);
        set_writes.push_back({
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = binding++,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = buffer.is_storage ? vk::DescriptorType::eStorageBuffer
                                                : vk::DescriptorType::eUniformBuffer,
            .pBufferInfo = &buffer_infos.back(),
        });
    }

    for (const auto& image : info.images) {
        const auto tsharp = info.ReadUd<AmdGpu::Image>(image.sgpr_base, image.dword_offset);
        const auto& image_view = texture_cache.FindImageView(tsharp);
        image_infos.emplace_back(VK_NULL_HANDLE, *image_view.image_view, vk::ImageLayout::eGeneral);
        set_writes.push_back({
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = binding++,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eSampledImage,
            .pImageInfo = &image_infos.back(),
        });
    }
    for (const auto& sampler : info.samplers) {
        const auto ssharp = info.ReadUd<AmdGpu::Sampler>(sampler.sgpr_base, sampler.dword_offset);
        const auto vk_sampler = texture_cache.GetSampler(ssharp);
        image_infos.emplace_back(vk_sampler, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
        set_writes.push_back({
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = binding++,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eSampler,
            .pImageInfo = &image_infos.back(),
        });
    }

    if (!set_writes.empty()) {
        const auto cmdbuf = scheduler.CommandBuffer();
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pipeline_layout, 0,
                                    set_writes);
    }
}

} // namespace Vulkan
