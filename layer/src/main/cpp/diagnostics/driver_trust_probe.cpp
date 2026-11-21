#include "../core/driver_trust.h"
#include "../core/driver_trust_shaders.h"
#include "../core/native_probe_resources.h"
#include "../core/native_msaa_resolve_probe.h"
#include "../core/native_image_slice_probe.h"
#include "../core/native_sync2_event_probe.h"
#include "../core/native_timeline_queue_probe.h"
#include "../core/driver_probe_process.h"
#include "native_discard_state_probe.h"
#include "native_depth_resolve_probe.h"
#include "native_e5_transfer_probe.h"
#include "native_probe_session.h"
#include "native_queue2_probe.h"
#include "native_timestamp_probe.h"
#include "query_reset_probe.h"
#include "../core/storage_buffer_array_probe.h"
#include "storage_arrays_shader.h"
#include "storage_arrays_static_shader.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#ifndef _WIN32
#include <dlfcn.h>
#endif

namespace vkfix {
namespace {
using nativeprobe::NativeContext;
using Resources=nativeprobe::Resources<NativeContext>;
// The generated revision hashes shaders, native probes and process protocol.
constexpr uint64_t probe_revision = nativeprobe::revision;

DriverProbeAssessment probe_sampler_aliases(NativeContext &p,VkPhysicalDevice physical,uint32_t family) {
    VkPhysicalDeviceProperties properties{};
    p.get<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(physical,&properties);
    const auto &limits=properties.limits;
    if (limits.maxPerStageDescriptorSamplers<128 || limits.maxDescriptorSetSamplers<128 ||
        limits.maxPerStageDescriptorSampledImages<2 || limits.maxDescriptorSetSampledImages<2 || limits.maxPerStageResources<3)
        return {DriverProbeVerdict::Inconclusive,"probe requires 128 samplers, two sampled images and one SSBO"};
    VkImageFormatProperties format{};
    if (p.get<PFN_vkGetPhysicalDeviceImageFormatProperties>("vkGetPhysicalDeviceImageFormatProperties")(
        physical,VK_FORMAT_D32_SFLOAT,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,0,&format)!=VK_SUCCESS ||
        format.maxExtent.width<4 || format.maxExtent.height<4)
        return {DriverProbeVerdict::Inconclusive,"D32 sampled/transfer control is unsupported"};
    Resources gpu(p,physical,family);
    auto output=gpu.buffer(16,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    VkImage images[2]{}; VkImageView views[2]{}; VkSampler samplers[2]{};
    for (unsigned i=0;i<2;++i) {
        auto destroy_image=p.dev<PFN_vkDestroyImage>("vkDestroyImage");
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType=VK_IMAGE_TYPE_2D; ci.format=VK_FORMAT_D32_SFLOAT; ci.extent={4,4,1};
        ci.mipLevels=ci.arrayLayers=1; ci.samples=VK_SAMPLE_COUNT_1_BIT; ci.tiling=VK_IMAGE_TILING_OPTIMAL;
        ci.usage=VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        nativeprobe::check(p.dev<PFN_vkCreateImage>("vkCreateImage")(p.device,&ci,nullptr,&images[i]),"native probe image");
        const auto image=images[i];
        gpu.objects.push_back([=,&p]{ destroy_image(p.device,image,nullptr); });
        VkMemoryRequirements mr{};
        p.dev<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(p.device,image,&mr);
        auto memory=gpu.allocate(mr,0);
        nativeprobe::check(p.dev<PFN_vkBindImageMemory>("vkBindImageMemory")(p.device,image,memory,0),"native probe image binding");
        auto destroy_view=p.dev<PFN_vkDestroyImageView>("vkDestroyImageView");
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image=image;
        vi.viewType=VK_IMAGE_VIEW_TYPE_2D; vi.format=ci.format; vi.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1};
        nativeprobe::check(p.dev<PFN_vkCreateImageView>("vkCreateImageView")(p.device,&vi,nullptr,&views[i]),"native probe view");
        const auto view=views[i]; gpu.objects.push_back([=,&p]{ destroy_view(p.device,view,nullptr); });
        auto destroy_sampler=p.dev<PFN_vkDestroySampler>("vkDestroySampler");
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter=si.minFilter=VK_FILTER_NEAREST; si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.compareEnable=i; si.compareOp=VK_COMPARE_OP_LESS_OR_EQUAL;
        nativeprobe::check(p.dev<PFN_vkCreateSampler>("vkCreateSampler")(p.device,&si,nullptr,&samplers[i]),"native probe sampler");
        const auto sampler=samplers[i]; gpu.objects.push_back([=,&p]{ destroy_sampler(p.device,sampler,nullptr); });
    }
    VkDescriptorSetLayoutBinding bindings[]{{0,VK_DESCRIPTOR_TYPE_SAMPLER,128,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
        {1,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,2,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
        {2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
    auto destroy_layout=p.dev<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout");
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; li.bindingCount=3; li.pBindings=bindings;
    VkDescriptorSetLayout set_layout{};
    nativeprobe::check(p.dev<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(p.device,&li,nullptr,&set_layout),"native probe descriptor layout");
    gpu.objects.push_back([=,&p]{ destroy_layout(p.device,set_layout,nullptr); });
    auto destroy_pipeline_layout=p.dev<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout");
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount=1; pli.pSetLayouts=&set_layout;
    VkPipelineLayout layout{};
    nativeprobe::check(p.dev<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(p.device,&pli,nullptr,&layout),"native probe pipeline layout");
    gpu.objects.push_back([=,&p]{ destroy_pipeline_layout(p.device,layout,nullptr); });
    auto destroy_pool=p.dev<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool");
    VkDescriptorPoolSize sizes[]{{VK_DESCRIPTOR_TYPE_SAMPLER,128},{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,2},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pi.maxSets=1; pi.poolSizeCount=3; pi.pPoolSizes=sizes;
    VkDescriptorPool pool{};
    nativeprobe::check(p.dev<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(p.device,&pi,nullptr,&pool),"native probe descriptor pool");
    gpu.objects.push_back([=,&p]{ destroy_pool(p.device,pool,nullptr); });
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; ai.descriptorPool=pool; ai.descriptorSetCount=1; ai.pSetLayouts=&set_layout;
    VkDescriptorSet set{};
    nativeprobe::check(p.dev<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(p.device,&ai,&set),"native probe descriptor allocation");
    VkDescriptorImageInfo sampler_info[128]{};
    for (unsigned i=0;i<128;++i) sampler_info[i].sampler=samplers[i==2 ? 1 : 0];
    VkDescriptorImageInfo image_info[2]{};
    for (unsigned i=0;i<2;++i) { image_info[i].imageView=views[i]; image_info[i].imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; }
    VkDescriptorBufferInfo buffer_info{output.handle,0,16};
    VkWriteDescriptorSet writes[3]{};
    for (unsigned i=0;i<3;++i) {
        writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet=set; writes[i].dstBinding=i;
        writes[i].descriptorCount=bindings[i].descriptorCount; writes[i].descriptorType=bindings[i].descriptorType;
    }
    writes[0].pImageInfo=sampler_info; writes[1].pImageInfo=image_info; writes[2].pBufferInfo=&buffer_info;
    p.dev<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(p.device,3,writes,0,nullptr);
    bool initialized=false;
    auto execute=[&](const uint32_t *code,size_t bytes) {
        auto destroy_shader=p.dev<PFN_vkDestroyShaderModule>("vkDestroyShaderModule");
        VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; sm.codeSize=bytes; sm.pCode=code;
        VkShaderModule module{};
        nativeprobe::check(p.dev<PFN_vkCreateShaderModule>("vkCreateShaderModule")(p.device,&sm,nullptr,&module),"native probe shader");
        gpu.objects.push_back([=,&p]{ destroy_shader(p.device,module,nullptr); });
        auto destroy_pipeline=p.dev<PFN_vkDestroyPipeline>("vkDestroyPipeline");
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.layout=layout; ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT; ci.stage.module=module; ci.stage.pName="main";
        VkPipeline pipeline{};
        auto result=p.dev<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(p.device,VK_NULL_HANDLE,1,&ci,nullptr,&pipeline);
        if (pipeline) gpu.objects.push_back([=,&p]{ destroy_pipeline(p.device,pipeline,nullptr); });
        nativeprobe::check(result,"native probe compute pipeline");
        bool matched=true;
        for (unsigned trial=0;trial<3;++trial) {
            const float a=trial==0 ? 0.25f : trial==1 ? 0.375f : 0.625f;
            const float b=1.f-a;
            const float expected[]{a,b>=0.5f ? 1.f : 0.f,b,0.5f};
            std::memset(output.mapped,0xcd,16);
            VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE}; range.memory=output.memory; range.size=VK_WHOLE_SIZE;
            nativeprobe::check(p.dev<PFN_vkFlushMappedMemoryRanges>("vkFlushMappedMemoryRanges")(p.device,1,&range),"native probe sentinel");
            auto cb=gpu.begin();
            for (unsigned i=0;i<2;++i) {
                VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                barrier.image=images[i]; barrier.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1};
                barrier.oldLayout=initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
                barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                barrier.srcAccessMask=initialized ? VK_ACCESS_SHADER_READ_BIT : 0; barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
                barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
                auto transition=p.dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
                transition(cb,initialized ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
                VkClearDepthStencilValue depth{i ? b : a,0};
                p.dev<PFN_vkCmdClearDepthStencilImage>("vkCmdClearDepthStencilImage")(cb,images[i],barrier.newLayout,&depth,1,&barrier.subresourceRange);
                barrier.oldLayout=barrier.newLayout; barrier.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
                transition(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
            }
            p.dev<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
            p.dev<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(cb,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,nullptr);
            p.dev<PFN_vkCmdDispatch>("vkCmdDispatch")(cb,1,1,1);
            gpu.host_barrier(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT);
            gpu.submit(cb); gpu.invalidate(output); initialized=true;
            float actual[4]; std::memcpy(actual,output.mapped,16);
            for (unsigned lane=0;lane<4;++lane)
                matched &= std::isfinite(actual[lane]) && std::fabs(actual[lane]-expected[lane])<0.0001f;
        }
        return matched;
    };
    const bool before=execute(nativeprobe::sampler_control,sizeof nativeprobe::sampler_control);
    if (!before) return assess_driver_probe(false,false,false);
    const bool subject=execute(nativeprobe::sampler_subject,sizeof nativeprobe::sampler_subject);
    const bool after=execute(nativeprobe::sampler_control,sizeof nativeprobe::sampler_control);
    return assess_driver_probe(before,subject,after);
}

DriverProbeAssessment probe_large_uniform(NativeContext &p,VkPhysicalDevice physical,uint32_t family,
                                         const VkPhysicalDeviceLimits &limits) {
    if (limits.maxUniformBufferRange<nativeprobe::uniform_bytes ||
        limits.minUniformBufferOffsetAlignment>65536)
        return {DriverProbeVerdict::Inconclusive,"large UBO control exceeds device limits"};
    Resources gpu(p,physical,family);
    const uint32_t alignment=std::max(1u,uint32_t(limits.minUniformBufferOffsetAlignment));
    const uint32_t dynamic_offset=(256+alignment-1)/alignment*alignment;
    auto input=gpu.buffer(dynamic_offset*2+nativeprobe::uniform_bytes,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    auto output=gpu.buffer(16,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    VkDescriptorSetLayoutBinding bindings[]{{0,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
        {1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
    auto destroy_layout=p.dev<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout");
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; li.bindingCount=2; li.pBindings=bindings;
    VkDescriptorSetLayout set_layout{};
    nativeprobe::check(p.dev<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(p.device,&li,nullptr,&set_layout),"UBO probe descriptor layout");
    gpu.objects.push_back([=,&p]{ destroy_layout(p.device,set_layout,nullptr); });
    auto destroy_pipeline_layout=p.dev<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout");
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount=1; pli.pSetLayouts=&set_layout;
    VkPipelineLayout layout{};
    nativeprobe::check(p.dev<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(p.device,&pli,nullptr,&layout),"UBO probe pipeline layout");
    gpu.objects.push_back([=,&p]{ destroy_pipeline_layout(p.device,layout,nullptr); });
    auto destroy_pool=p.dev<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool");
    VkDescriptorPoolSize sizes[]{{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pi.maxSets=1; pi.poolSizeCount=2; pi.pPoolSizes=sizes;
    VkDescriptorPool pool{};
    nativeprobe::check(p.dev<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(p.device,&pi,nullptr,&pool),"UBO probe pool");
    gpu.objects.push_back([=,&p]{ destroy_pool(p.device,pool,nullptr); });
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; ai.descriptorPool=pool; ai.descriptorSetCount=1; ai.pSetLayouts=&set_layout;
    VkDescriptorSet set{};
    nativeprobe::check(p.dev<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(p.device,&ai,&set),"UBO probe descriptor allocation");
    VkDescriptorBufferInfo infos[]{{input.handle,0,nativeprobe::uniform_bytes},{output.handle,0,16}};
    VkWriteDescriptorSet writes[2]{};
    for (unsigned i=0;i<2;++i) {
        writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet=set; writes[i].dstBinding=i;
        writes[i].descriptorCount=1; writes[i].descriptorType=bindings[i].descriptorType; writes[i].pBufferInfo=&infos[i];
    }
    p.dev<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(p.device,2,writes,0,nullptr);
    auto execute=[&](const uint32_t *code,size_t bytes) {
        auto destroy_shader=p.dev<PFN_vkDestroyShaderModule>("vkDestroyShaderModule");
        VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; sm.codeSize=bytes; sm.pCode=code;
        VkShaderModule module{};
        nativeprobe::check(p.dev<PFN_vkCreateShaderModule>("vkCreateShaderModule")(p.device,&sm,nullptr,&module),"UBO probe shader");
        gpu.objects.push_back([=,&p]{ destroy_shader(p.device,module,nullptr); });
        auto destroy_pipeline=p.dev<PFN_vkDestroyPipeline>("vkDestroyPipeline");
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.layout=layout; ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT; ci.stage.module=module; ci.stage.pName="main";
        VkPipeline pipeline{};
        auto result=p.dev<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(p.device,VK_NULL_HANDLE,1,&ci,nullptr,&pipeline);
        if (pipeline) gpu.objects.push_back([=,&p]{ destroy_pipeline(p.device,pipeline,nullptr); });
        nativeprobe::check(result,"UBO probe pipeline");
        bool matched=true;
        for (unsigned trial=0;trial<4;++trial) {
            const uint32_t offset=dynamic_offset*(1+trial%2);
            const float expected[]{0.25f+trial*0.125f,0.5f+trial*0.0625f,0.75f-trial*0.125f,1.f};
            std::memset(input.mapped,0xcd,dynamic_offset*2+nativeprobe::uniform_bytes);
            auto ubo=static_cast<char*>(input.mapped)+offset;
            std::memcpy(ubo+nativeprobe::uniform_offsets[0],expected,8);
            std::memcpy(ubo+nativeprobe::uniform_offsets[1],expected+2,4);
            std::memcpy(ubo+nativeprobe::uniform_offsets[2],expected+3,4);
            std::memset(output.mapped,0xcd,16);
            VkMappedMemoryRange ranges[2]{};
            for (auto &range:ranges) { range.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE; range.size=VK_WHOLE_SIZE; }
            ranges[0].memory=input.memory; ranges[1].memory=output.memory;
            nativeprobe::check(p.dev<PFN_vkFlushMappedMemoryRanges>("vkFlushMappedMemoryRanges")(p.device,2,ranges),"UBO probe inputs");
            auto cb=gpu.begin();
            p.dev<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
            p.dev<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(cb,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,1,&offset);
            p.dev<PFN_vkCmdDispatch>("vkCmdDispatch")(cb,1,1,1);
            gpu.host_barrier(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT);
            gpu.submit(cb); gpu.invalidate(output);
            float actual[4]; std::memcpy(actual,output.mapped,16);
            for (unsigned lane=0;lane<4;++lane)
                matched &= std::isfinite(actual[lane]) && std::fabs(actual[lane]-expected[lane])<0.0001f;
        }
        return matched;
    };
    const bool before=execute(nativeprobe::uniform_control,sizeof nativeprobe::uniform_control);
    if (!before) return assess_driver_probe(false,false,false);
    const bool subject=execute(nativeprobe::uniform_subject,sizeof nativeprobe::uniform_subject);
    const bool after=execute(nativeprobe::uniform_control,sizeof nativeprobe::uniform_control);
    return assess_driver_probe(before,subject,after);
}

bool probe_push_transition(NativeContext &p,VkPhysicalDevice physical,uint32_t family,bool subject) {
    Resources gpu(p,physical,family);
    auto output=gpu.buffer(16,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    auto create_set=p.dev<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout");
    auto destroy_set=p.dev<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout");
    VkDescriptorSetLayoutCreateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    VkDescriptorSetLayout empty{},data{};
    nativeprobe::check(create_set(p.device,&si,nullptr,&empty),"push empty set");
    gpu.objects.push_back([=,&p]{ destroy_set(p.device,empty,nullptr); });
    VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    si.bindingCount=1; si.pBindings=&binding;
    si.flags=subject ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0;
    nativeprobe::check(create_set(p.device,&si,nullptr,&data),"push data set");
    gpu.objects.push_back([=,&p]{ destroy_set(p.device,data,nullptr); });
    auto create_layout=p.dev<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout");
    auto destroy_layout=p.dev<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout");
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout layouts[2]{};
    nativeprobe::check(create_layout(p.device,&li,nullptr,&layouts[0]),"push old layout");
    const auto old_layout=layouts[0]; gpu.objects.push_back([=,&p]{ destroy_layout(p.device,old_layout,nullptr); });
    VkDescriptorSetLayout sets[]{empty,data};
    VkPushConstantRange seed{VK_SHADER_STAGE_COMPUTE_BIT,0,4};
    li.setLayoutCount=2; li.pSetLayouts=sets; li.pushConstantRangeCount=1; li.pPushConstantRanges=&seed;
    nativeprobe::check(create_layout(p.device,&li,nullptr,&layouts[1]),"push target layout");
    const auto target_layout=layouts[1]; gpu.objects.push_back([=,&p]{ destroy_layout(p.device,target_layout,nullptr); });
    VkPipeline pipelines[2]{};
    for (unsigned i=0;i<2;++i) {
        auto destroy_shader=p.dev<PFN_vkDestroyShaderModule>("vkDestroyShaderModule");
        VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        sm.pCode=i ? nativeprobe::push_output : nativeprobe::push_empty;
        sm.codeSize=i ? sizeof nativeprobe::push_output : sizeof nativeprobe::push_empty;
        VkShaderModule module{};
        nativeprobe::check(p.dev<PFN_vkCreateShaderModule>("vkCreateShaderModule")(p.device,&sm,nullptr,&module),"push probe shader");
        gpu.objects.push_back([=,&p]{ destroy_shader(p.device,module,nullptr); });
        auto destroy_pipeline=p.dev<PFN_vkDestroyPipeline>("vkDestroyPipeline");
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.layout=layouts[i]; ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT; ci.stage.module=module; ci.stage.pName="main";
        auto result=p.dev<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(p.device,VK_NULL_HANDLE,1,&ci,nullptr,&pipelines[i]);
        const auto pipeline=pipelines[i];
        if (pipeline) gpu.objects.push_back([=,&p]{ destroy_pipeline(p.device,pipeline,nullptr); });
        nativeprobe::check(result,"push probe pipeline");
    }
    VkDescriptorBufferInfo buffer{output.handle,0,16};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstBinding=0; write.descriptorCount=1; write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; write.pBufferInfo=&buffer;
    VkDescriptorSet set{};
    if (!subject) {
        auto destroy_pool=p.dev<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool");
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1};
        VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; ci.maxSets=1; ci.poolSizeCount=1; ci.pPoolSizes=&size;
        VkDescriptorPool pool{};
        nativeprobe::check(p.dev<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(p.device,&ci,nullptr,&pool),"push control pool");
        gpu.objects.push_back([=,&p]{ destroy_pool(p.device,pool,nullptr); });
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; ai.descriptorPool=pool; ai.descriptorSetCount=1; ai.pSetLayouts=&data;
        nativeprobe::check(p.dev<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(p.device,&ai,&set),"push control set");
        write.dstSet=set;
        p.dev<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(p.device,1,&write,0,nullptr);
    }
    auto push=subject ? p.dev<PFN_vkCmdPushDescriptorSetKHR>("vkCmdPushDescriptorSetKHR") : nullptr;
    // Do not classify a compiler, allocation or loader crash as this transition
    // defect. The parent accepts a subject signal only after this marker.
    std::puts("VKFIX_PROBE_READY"); std::fflush(stdout);
    bool matched=true;
    for (unsigned trial=0;trial<4;++trial) {
        std::memset(output.mapped,0xcd,16);
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE}; range.memory=output.memory; range.size=VK_WHOLE_SIZE;
        nativeprobe::check(p.dev<PFN_vkFlushMappedMemoryRanges>("vkFlushMappedMemoryRanges")(p.device,1,&range),"push sentinel");
        auto cb=gpu.begin();
        p.dev<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipelines[0]);
        // Legal to set descriptors against the supplied future layout while
        // a pipeline with no sets is bound. Bind that future pipeline before
        // dispatch (Vulkan descriptor-set pipeline-layout compatibility).
        if (subject) push(cb,VK_PIPELINE_BIND_POINT_COMPUTE,layouts[1],1,1,&write);
        else p.dev<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(cb,VK_PIPELINE_BIND_POINT_COMPUTE,layouts[1],1,1,&set,0,nullptr);
        p.dev<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipelines[1]);
        const uint32_t value=0x56465800u+trial*16;
        p.dev<PFN_vkCmdPushConstants>("vkCmdPushConstants")(cb,layouts[1],VK_SHADER_STAGE_COMPUTE_BIT,0,4,&value);
        p.dev<PFN_vkCmdDispatch>("vkCmdDispatch")(cb,1,1,1);
        gpu.host_barrier(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT);
        gpu.submit(cb); gpu.invalidate(output);
        for (unsigned lane=0;lane<4;++lane) matched &= static_cast<uint32_t*>(output.mapped)[lane]==value+lane;
    }
    return matched;
}
#include "../core/driver_tiler_probe.inc"
#include "../core/driver_push_graphics_probe.inc"

void worker_ready() {
    std::puts("VKFIX_PROBE_READY");
    std::fflush(stdout);
}

int worker_readback(const std::string &error) {
    if (!error.empty()) std::printf("VKFIX_PROBE_DETAIL:%.512s\n",error.c_str());
    std::puts(error.empty() ? "VKFIX_PROBE_PASS" : "VKFIX_PROBE_MISMATCH");
    return error.empty() ? 0 : 3;
}
} // namespace

int driver_trust_worker(const char *mode,const std::string &identity) {
#ifdef _WIN32
    return 2;
#else
    try {
        const bool tiler=std::strcmp(mode,"tiler-subject")==0 || std::strcmp(mode,"tiler-control")==0;
        const bool vertex_compute=std::strcmp(mode,"push-vertex-compute-subject")==0 || std::strcmp(mode,"push-vertex-compute-control")==0;
        const bool vertex=vertex_compute || std::strcmp(mode,"push-vertex-subject")==0 || std::strcmp(mode,"push-vertex-control")==0;
        const bool graphics=vertex || std::strcmp(mode,"push-graphics-subject")==0 || std::strcmp(mode,"push-graphics-control")==0;
        const bool sampler=std::strcmp(mode,"sampler")==0;
        const bool uniform=std::strcmp(mode,"uniform")==0;
        const bool msaa=std::strcmp(mode,"msaa")==0;
        const bool slice_subject=std::strcmp(mode,"image-slices-subject")==0;
        const bool image_slices=slice_subject || std::strcmp(mode,"image-slices-control")==0;
        const bool discard_subject=std::strcmp(mode,"discard-state-subject")==0;
        const bool discard_reissue=std::strcmp(mode,"discard-state-reemit")==0;
        const bool discard_state=discard_subject || discard_reissue || std::strcmp(mode,"discard-state-rasterized")==0;
        const bool storage_static_subject = !std::strcmp(mode, "storage-array-static-subject");
        const bool storage_static = storage_static_subject || !std::strcmp(mode, "storage-array-static-control");
        const bool storage_dynamic_subject = !std::strcmp(mode, "storage-array-dynamic-subject");
        const bool storage_dynamic = storage_dynamic_subject || !std::strcmp(mode, "storage-array-dynamic-control");
        const bool storage_array = storage_static || storage_dynamic;
        const bool depth_resolve_subject = !std::strcmp(mode, "depth-resolve-subject");
        const bool depth_resolve = depth_resolve_subject || !std::strcmp(mode, "depth-resolve-control");
        const bool e5_buffer = !std::strcmp(mode, "e5-buffer-subject");
        const bool e5_image = !std::strcmp(mode, "e5-image-subject");
        const bool e5_transfer = e5_buffer || e5_image || !std::strcmp(mode, "e5-control");
        const bool queue2_subject = !std::strcmp(mode, "queue2-subject");
        const bool queue2 = queue2_subject || !std::strcmp(mode, "queue2-control");
        const bool timeline=std::strcmp(mode,"timeline-subject")==0;
        const bool timeline_queue=timeline || std::strcmp(mode,"timeline-control")==0;
        const bool sync2_event=std::strcmp(mode,"sync2-events-subject")==0;
        const bool sync2_reset_control=std::strcmp(mode,"sync2-events-reset-control")==0;
        const bool sync2_events=sync2_event || sync2_reset_control || std::strcmp(mode,"sync2-events-control")==0;
        const bool sync2_submit=std::strcmp(mode,"sync2-submit-subject")==0;
        const bool timestamps=std::strcmp(mode,"timestamps")==0;
        const bool host_query_reset=std::strcmp(mode,"host-query-reset-subject")==0;
        const bool query_reset=host_query_reset || std::strcmp(mode,"host-query-reset-control")==0;
        const bool sync2=sync2_event || sync2_reset_control || sync2_submit;
        const bool subject=std::strcmp(mode,"push-subject")==0 || std::strcmp(mode,"tiler-subject")==0 ||
                           std::strcmp(mode,"push-graphics-subject")==0 || std::strcmp(mode,"push-vertex-subject")==0 ||
                           std::strcmp(mode,"push-vertex-compute-subject")==0;
        if (!tiler && !graphics && !sampler && !uniform && !msaa && !image_slices && !discard_state && !storage_array &&
            !depth_resolve && !e5_transfer && !queue2 && !timeline_queue &&
            !sync2_events && !sync2_submit && !timestamps && !query_reset &&
            !subject && std::strcmp(mode,"push-control")) return 2;
        const char *backend="libvulkan.so";
#ifdef VKFIX_DRIVER_TRUST_TESTING
        // Only a separately built test executable can load the forwarding
        // fixture. The production APK worker always uses the system loader.
#ifdef VKFIX_PROBE_TEST_CLIENT
        if (const char *test = std::getenv("VKFIX_TRUST_TEST_CLIENT_DRIVER")) backend = test;
#else
        if (const char *test=std::getenv("VKFIX_TRUST_TEST_DRIVER")) backend=test;
#endif
        setenv("VKFIX_TEST_PROBE_PHASE",mode,1);
#endif
        auto library=dlopen(backend,RTLD_NOW|RTLD_LOCAL);
        if (!library) return 2;
        auto gipa=reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library,"vkGetInstanceProcAddr"));
        if (!gipa) return 2;
        VkQueueFlags queue_flags = VK_QUEUE_COMPUTE_BIT;
        if (tiler || graphics || msaa || image_slices || discard_state || depth_resolve)
            queue_flags = VK_QUEUE_GRAPHICS_BIT;
        if (sync2_events || timeline_queue || sync2_submit || queue2 || timestamps || query_reset)
            queue_flags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
        if (timeline_queue || sync2_submit || queue2) queue_flags |= VK_QUEUE_TRANSFER_BIT;
        NativeContext p(gipa);
        p.select(identity, probe_revision, queue_flags, sync2 ? VK_API_VERSION_1_3 : VK_API_VERSION_1_2,
                 timestamps || query_reset);
        const auto physical = p.physical;
        const auto family = p.family;
        const auto &properties = p.properties;
        if (p.api < VK_API_VERSION_1_1 || properties.apiVersion < VK_API_VERSION_1_1) {
            if (storage_array) { std::puts("aggregate StorageBuffer probe requires Vulkan 1.1"); return 2; }
            if (image_slices) { std::puts("packed image slice probe requires Vulkan 1.1"); return 2; }
            if (queue2) { std::puts("queue2 probe requires Vulkan 1.1"); return 2; }
        }
        float priority=1;
        VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qi.queueFamilyIndex=family; qi.queueCount=1; qi.pQueuePriorities=&priority;
        VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; ci.queueCreateInfoCount=1; ci.pQueueCreateInfos=&qi;
        const char *extension=nullptr;
        if (subject && !tiler) extension=VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME;
        const bool query_reset_core = p.api >= VK_API_VERSION_1_2 && properties.apiVersion >= VK_API_VERSION_1_2;
        VkPhysicalDeviceHostQueryResetFeatures query_reset_feature{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES};
        if (host_query_reset) {
            if (p.api < VK_API_VERSION_1_1 || (!query_reset_core && !p.has_extension(VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME))) {
                std::puts("native host query reset unavailable"); return 2;
            }
            VkPhysicalDeviceFeatures2 available{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &query_reset_feature};
            p.get<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(physical, &available);
            if (!query_reset_feature.hostQueryReset) {
                std::puts("native hostQueryReset feature unavailable"); return 2;
            }
            if (!query_reset_core) extension = VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME;
            ci.pNext = &query_reset_feature;
        }
        const bool timeline_core=p.api>=VK_API_VERSION_1_2 && properties.apiVersion>=VK_API_VERSION_1_2;
        VkPhysicalDeviceTimelineSemaphoreFeatures timeline_feature{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
        if (timeline) {
            if (p.api<VK_API_VERSION_1_1) {
                std::puts("timeline queue probe requires a Vulkan 1.1 instance"); return 2;
            }
            if (!timeline_core && !p.has_extension(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME)) {
                std::puts("native timeline extension unavailable"); return 2;
            }
            if (!timeline_core) extension=VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME;
            VkPhysicalDeviceFeatures2 available{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            available.pNext=&timeline_feature;
            p.get<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(physical,&available);
            if (!timeline_feature.timelineSemaphore) { std::puts("native timeline feature unavailable"); return 2; }
            ci.pNext=&timeline_feature;
        }
        auto sync2_route=nativeprobe::Sync2Route::Legacy;
        VkPhysicalDeviceSynchronization2Features sync2_feature{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
        if (sync2) {
            if (p.api<VK_API_VERSION_1_1) { std::puts("synchronization2 requires a Vulkan 1.1 instance"); return 2; }
            const bool sync2_core=p.api>=VK_API_VERSION_1_3 && properties.apiVersion>=VK_API_VERSION_1_3;
            if (!sync2_core && !p.has_extension(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME)) {
                std::puts("native synchronization2 extension unavailable"); return 2;
            }
            if (!sync2_core) extension=VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME;
            VkPhysicalDeviceFeatures2 available{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            available.pNext=&sync2_feature;
            p.get<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(physical,&available);
            if (!sync2_feature.synchronization2) { std::puts("native synchronization2 feature unavailable"); return 2; }
            ci.pNext=&sync2_feature;
            sync2_route=sync2_core ? nativeprobe::Sync2Route::Core : nativeprobe::Sync2Route::KHR;
        }
        if (extension) { ci.enabledExtensionCount=1; ci.ppEnabledExtensionNames=&extension; }
        const char *depth_extensions[]{VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME, VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME};
        const bool depth_resolve_core = p.api >= VK_API_VERSION_1_2 && properties.apiVersion >= VK_API_VERSION_1_2;
        VkPhysicalDeviceDepthStencilResolveProperties resolve_modes{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES};
        if (depth_resolve) {
            if (p.api < VK_API_VERSION_1_1 || properties.apiVersion < VK_API_VERSION_1_1) {
                std::puts("depth/stencil resolve probe requires Vulkan 1.1");
                return 2;
            }
            if (!depth_resolve_core) {
                for (const auto *name : depth_extensions) if (!p.has_extension(name)) {
                    std::printf("native %s unavailable\n", name);
                    return 2;
                }
                ci.enabledExtensionCount = 2;
                ci.ppEnabledExtensionNames = depth_extensions;
            }
            VkPhysicalDeviceProperties2 queried{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &resolve_modes};
            p.get<PFN_vkGetPhysicalDeviceProperties2>("vkGetPhysicalDeviceProperties2")(physical, &queried);
        }
        VkPhysicalDeviceFeatures requested{};
        if (storage_dynamic) {
            VkPhysicalDeviceFeatures supported{};
            p.get<PFN_vkGetPhysicalDeviceFeatures>("vkGetPhysicalDeviceFeatures")(physical, &supported);
            if (!supported.shaderStorageBufferArrayDynamicIndexing) {
                std::puts("native storage-buffer array dynamic indexing unavailable");
                return 2;
            }
            requested.shaderStorageBufferArrayDynamicIndexing = VK_TRUE;
            ci.pEnabledFeatures = &requested;
        }
        VkPhysicalDeviceDescriptorIndexingFeatures indexed{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
        bool use_indexed_heap=false;
        if (graphics && p.api>=VK_API_VERSION_1_2) {
            if (properties.apiVersion>=VK_API_VERSION_1_2) {
                VkPhysicalDeviceFeatures2 available{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; available.pNext=&indexed;
                p.get<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(physical,&available);
                use_indexed_heap=indexed.descriptorBindingSampledImageUpdateAfterBind && indexed.descriptorBindingStorageBufferUpdateAfterBind &&
                    indexed.descriptorBindingPartiallyBound && indexed.descriptorBindingUpdateUnusedWhilePending;
                indexed={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
                if (use_indexed_heap) {
                    indexed.descriptorBindingSampledImageUpdateAfterBind=indexed.descriptorBindingStorageBufferUpdateAfterBind=VK_TRUE;
                    indexed.descriptorBindingPartiallyBound=indexed.descriptorBindingUpdateUnusedWhilePending=VK_TRUE;
                    ci.pNext=&indexed;
                }
            }
        }
        if (tiler) {
            VkPhysicalDeviceFeatures supported{};
            p.get<PFN_vkGetPhysicalDeviceFeatures>("vkGetPhysicalDeviceFeatures")(physical,&supported);
            VkPhysicalDeviceProperties properties{};
            p.get<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(physical,&properties);
            requested.multiDrawIndirect=supported.multiDrawIndirect && properties.limits.maxDrawIndirectCount>=8;
            ci.pEnabledFeatures=&requested;
        }
        p.create_device(ci);
        if (query_reset) {
            Resources gpu(p, physical, family);
            const char *reset = !host_query_reset ? nullptr :
                query_reset_core ? "vkResetQueryPool" : "vkResetQueryPoolEXT";
            return worker_readback(nativeprobe::probe_host_query_reset(p, gpu, reset, worker_ready));
        }
        if (timestamps) {
            const auto result = nativeprobe::probe_timestamps(
                p, physical, family, p.timestamp_valid_bits, properties.limits.timestampPeriod, worker_ready);
            std::printf("VKFIX_PROBE_DETAIL:%.512s\n", result.detail.c_str());
            std::puts(result.passed ? "VKFIX_PROBE_PASS" : "VKFIX_PROBE_MISMATCH");
            return result.passed ? 0 : 3;
        }
        if (queue2) {
            const auto result = nativeprobe::probe_queue2(p, physical, family, queue2_subject, worker_ready);
            std::printf("VKFIX_PROBE_DETAIL:%.512s\n", result.detail.c_str());
            std::puts(result.passed ? "VKFIX_PROBE_PASS" :
                result.null_queue2 ? "VKFIX_PROBE_KNOWN_MISMATCH" : "VKFIX_PROBE_MISMATCH");
            return result.passed ? 0 : 3;
        }
        if (timeline_queue || sync2_submit)
            return worker_readback(nativeprobe::probe_timeline_queue(p,physical,family,timeline,timeline_core,worker_ready,sync2_route));
        if (sync2_events) {
            const auto result = nativeprobe::probe_sync2_events(
                p, physical, family, sync2_route, sync2_reset_control, worker_ready);
            if (!sync2_event || !result.reset_left_event_set) return worker_readback(result.error);
            std::printf("VKFIX_PROBE_DETAIL:%s\n", result.error.c_str());
            std::puts("VKFIX_PROBE_KNOWN_MISMATCH");
            return 3;
        }
        if (image_slices) {
            const auto result = nativeprobe::probe_image_slices(p, physical, family, !slice_subject, worker_ready);
            if (!result.only_first_slice_written) return worker_readback(result.error);
            std::printf("VKFIX_PROBE_DETAIL:%.512s\n", result.error.c_str());
            std::puts("VKFIX_PROBE_KNOWN_MISMATCH");
            return 3;
        }
        if (discard_state) {
            using Mode = nativeprobe::DiscardStateMode;
            const auto result = nativeprobe::probe_discard_state(p, physical, family,
                discard_subject ? Mode::Inherited : discard_reissue ? Mode::Reissued : Mode::Rasterized, worker_ready);
            std::printf("VKFIX_PROBE_DETAIL:%.512s\n", result.detail.c_str());
            std::puts(result.passed ? "VKFIX_PROBE_PASS" :
                result.lost_state ? "VKFIX_PROBE_KNOWN_MISMATCH" : "VKFIX_PROBE_MISMATCH");
            return result.passed ? 0 : 3;
        }
        if (e5_transfer) {
            using Route = nativeprobe::E5ReadRoute;
            const auto route = e5_buffer ? Route::Buffer : e5_image ? Route::Image : Route::IntegerView;
            const auto result = nativeprobe::probe_e5_transfer(p, physical, family, route, worker_ready);
            std::printf("VKFIX_PROBE_DETAIL:%.512s\n", result.detail.c_str());
            if (!result.ran) return 2;
            std::puts(result.passed ? "VKFIX_PROBE_PASS" :
                route != Route::IntegerView && result.canonicalized ? "VKFIX_PROBE_KNOWN_MISMATCH" : "VKFIX_PROBE_MISMATCH");
            return result.passed ? 0 : 3;
        }
        if (storage_array) {
            using namespace nativeprobe;
            const auto run = [&](const auto &code) {
                return storage_buffer_array(p, physical, family, code, sizeof code / sizeof(uint32_t), true,
                                            false, nullptr, 0, worker_ready);
            };
            const auto result = storage_static ?
                (storage_static_subject ? run(post_storage_arrays_static_comp) : run(storage_array_static_control)) :
                (storage_dynamic_subject ? run(post_storage_arrays_comp) : run(storage_array_dynamic_control));
            if (!result.aggregate_store_lost || !(storage_static_subject || storage_dynamic_subject))
                return worker_readback(result.error);
            std::printf("VKFIX_PROBE_DETAIL:whole-array store did not restore descriptor 2; %.430s\n", result.error.c_str());
            std::puts("VKFIX_PROBE_KNOWN_MISMATCH");
            return 3;
        }
        if (depth_resolve) {
            const auto result = nativeprobe::probe_depth_resolve(p, physical, family, depth_resolve_core,
                depth_resolve_subject, resolve_modes, worker_ready);
            if (!result.formats) { std::puts("no 4x sampled depth/stencil format supports sample-zero resolve"); return 2; }
            std::printf("VKFIX_PROBE_DETAIL:%.512s\n", result.detail.c_str());
            std::puts(result.passed ? "VKFIX_PROBE_PASS" :
                depth_resolve_subject && result.target_unwritten ? "VKFIX_PROBE_KNOWN_MISMATCH" : "VKFIX_PROBE_MISMATCH");
            return result.passed ? 0 : 3;
        }
        if (sampler || uniform || msaa) {
            std::puts("VKFIX_PROBE_READY"); std::fflush(stdout);
            VkPhysicalDeviceProperties properties{};
            p.get<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(physical,&properties);
            const auto assessment=sampler ? probe_sampler_aliases(p,physical,family) :
                uniform ? probe_large_uniform(p,physical,family,properties.limits) :
                nativeprobe::probe_msaa_array_resolve(p,physical,family);
            std::printf("VKFIX_PROBE_DETAIL:%.512s\n",assessment.detail.c_str());
            if (assessment.verdict==DriverProbeVerdict::Healthy) { std::puts("VKFIX_PROBE_PASS"); return 0; }
            if (assessment.verdict==DriverProbeVerdict::RepairConfirmed) { std::puts("VKFIX_PROBE_REPAIR"); return 4; }
            return 2;
        }
        const bool passed=tiler ? probe_tiler_pressure(p,physical,family,subject,requested.multiDrawIndirect) :
            graphics ? probe_push_graphics(p,physical,family,subject,use_indexed_heap,vertex ? (vertex_compute ? 2u : 1u) : 0u) : probe_push_transition(p,physical,family,subject);
        std::puts(passed ? "VKFIX_PROBE_PASS" : "VKFIX_PROBE_MISMATCH");
        return passed ? 0 : 3;
        // Instance lives to worker exit; incomplete GPU work is never destroyed.
    } catch (const std::exception &error) {
        std::printf("worker setup/execution error: %.512s\n",error.what());
        return 2;
    }
#endif
}

} // namespace vkfix
