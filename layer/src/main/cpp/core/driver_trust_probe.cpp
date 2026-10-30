#include "driver_trust.h"
#include "driver_trust_shaders.h"
#include "native_probe_resources.h"
#include "driver_probe_process.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#ifndef _WIN32
#include <dlfcn.h>
#endif

namespace vkfix {
namespace {
struct NativeContext {
    VkInstance instance;
    VkDevice device{};
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkGetDeviceProcAddr gdpa{};
    bool pending=false, retain_device=false;
    template<class T> T get(const char *name) const {
        auto fn=gipa(instance,name);
        if (!fn) throw std::runtime_error(std::string("native probe missing ")+name);
        return reinterpret_cast<T>(fn);
    }
    template<class T> T dev(const char *name) const {
        auto fn=gdpa ? gdpa(device,name) : nullptr;
        if (!fn) throw std::runtime_error(std::string("native probe missing ")+name);
        return reinterpret_cast<T>(fn);
    }
    ~NativeContext() {
        // A timed-out native submission may still access all of this device's
        // resources. No idle wait or destruction is safe then. Memoization
        // prevents repeating the probe on this identity in the process.
        if (device && !pending) {
            auto destroy=reinterpret_cast<PFN_vkDestroyDevice>(gdpa(device,"vkDestroyDevice"));
            if (destroy) destroy(device,nullptr);
        }
    }
};
using Resources=nativeprobe::Resources<NativeContext>;

DriverProbeAssessment probe_sampler_aliases(NativeContext &p,VkPhysicalDevice physical,uint32_t family) {
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
#include "driver_tiler_probe.inc"
} // namespace

int driver_trust_worker(const char *mode,const std::string &identity) {
#ifdef _WIN32
    return 2;
#else
    try {
        const bool tiler=std::strcmp(mode,"tiler-subject")==0 || std::strcmp(mode,"tiler-control")==0;
        const bool subject=std::strcmp(mode,"push-subject")==0 || std::strcmp(mode,"tiler-subject")==0;
        if (!tiler && !subject && std::strcmp(mode,"push-control")) return 2;
        const char *backend="libvulkan.so";
#ifdef VKFIX_DRIVER_TRUST_TESTING
        // Only a separately built test executable can load the forwarding
        // fixture. The production APK worker always uses the system loader.
        if (const char *test=std::getenv("VKFIX_TRUST_TEST_DRIVER")) backend=test;
        setenv("VKFIX_TEST_PROBE_PHASE",mode,1);
#endif
        auto library=dlopen(backend,RTLD_NOW|RTLD_LOCAL);
        if (!library) return 2;
        auto gipa=reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library,"vkGetInstanceProcAddr"));
        if (!gipa) return 2;
        NativeContext p{VK_NULL_HANDLE,{},gipa};
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion=VK_API_VERSION_1_0;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo=&app;
        nativeprobe::check(p.get<PFN_vkCreateInstance>("vkCreateInstance")(&ici,nullptr,&p.instance),"worker instance");
        uint32_t count=0;
        auto enumerate=p.get<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
        nativeprobe::check(enumerate(p.instance,&count,nullptr),"worker devices");
        if (!count || count>256) return 2;
        std::vector<VkPhysicalDevice> physicals(count);
        nativeprobe::check(enumerate(p.instance,&count,physicals.data()),"worker physicals");
        VkPhysicalDevice physical{};
        for (auto candidate:physicals) {
            VkPhysicalDeviceProperties properties{};
            p.get<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(candidate,&properties);
            if (driver_probe_identity(properties,nativeprobe::revision)==identity) { physical=candidate; break; }
        }
        if (!physical) { std::puts("worker identity/revision mismatch"); return 2; }
        auto query=p.get<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties");
        count=0; query(physical,&count,nullptr);
        if (!count || count>65536) return 2;
        std::vector<VkQueueFamilyProperties> families(count); query(physical,&count,families.data());
        uint32_t family=0;
        const auto queue_flags=tiler ? VK_QUEUE_GRAPHICS_BIT : VK_QUEUE_COMPUTE_BIT;
        while (family<count && (!families[family].queueCount || !(families[family].queueFlags&queue_flags))) ++family;
        if (family==count) return 2;
        float priority=1;
        VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qi.queueFamilyIndex=family; qi.queueCount=1; qi.pQueuePriorities=&priority;
        VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; ci.queueCreateInfoCount=1; ci.pQueueCreateInfos=&qi;
        const char *extension=VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME;
        if (subject && !tiler) { ci.enabledExtensionCount=1; ci.ppEnabledExtensionNames=&extension; }
        VkPhysicalDeviceFeatures requested{};
        if (tiler) {
            VkPhysicalDeviceFeatures supported{};
            p.get<PFN_vkGetPhysicalDeviceFeatures>("vkGetPhysicalDeviceFeatures")(physical,&supported);
            VkPhysicalDeviceProperties properties{};
            p.get<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(physical,&properties);
            requested.multiDrawIndirect=supported.multiDrawIndirect && properties.limits.maxDrawIndirectCount>=8;
            ci.pEnabledFeatures=&requested;
        }
        p.gdpa=p.get<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
        nativeprobe::check(p.get<PFN_vkCreateDevice>("vkCreateDevice")(physical,&ci,nullptr,&p.device),"worker device");
        const bool passed=tiler ? probe_tiler_pressure(p,physical,family,subject,requested.multiDrawIndirect) : probe_push_transition(p,physical,family,subject);
        std::puts(passed ? "VKFIX_PROBE_PASS" : "VKFIX_PROBE_MISMATCH");
        return passed ? 0 : 3;
        // Instance lives to worker exit; incomplete GPU work is never destroyed.
    } catch (const std::exception &error) {
        std::printf("worker setup/execution error: %.512s\n",error.what());
        return 2;
    }
#endif
}

DriverTrustReport native_driver_trust(VkInstance instance,VkPhysicalDevice physical,PFN_vkGetInstanceProcAddr gipa) {
    DriverTrustReport unavailable;
    if (!instance || !physical || !gipa) return unavailable;
    auto props=reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(instance,"vkGetPhysicalDeviceProperties"));
    if (!props) return unavailable;
    VkPhysicalDeviceProperties properties{}; props(physical,&properties);
    static DriverTrustCache cache;
    auto identity=driver_identity(properties,nativeprobe::revision);
    identity.physical_cookie=reinterpret_cast<uintptr_t>(physical);
    identity.provider_cookie=reinterpret_cast<uintptr_t>(gipa);
    return cache.get(identity,[=] {
        DriverTrustReport report;
        NativeContext p{instance,{},gipa};
        auto query=p.get<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties");
        uint32_t count=0; query(physical,&count,nullptr);
        if (!count || count>65536) throw std::runtime_error("native probe has no queue families");
        std::vector<VkQueueFamilyProperties> families(count); query(physical,&count,families.data());
        uint32_t family=0;
        while (family<count && (!families[family].queueCount || !(families[family].queueFlags&VK_QUEUE_COMPUTE_BIT))) ++family;
        if (family==count) throw std::runtime_error("native probe has no compute queue");
        p.gdpa=p.get<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
        float priority=1;
        VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qi.queueFamilyIndex=family; qi.queueCount=1; qi.pQueuePriorities=&priority;
        VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; ci.queueCreateInfoCount=1; ci.pQueueCreateInfos=&qi;
        nativeprobe::check(p.get<PFN_vkCreateDevice>("vkCreateDevice")(physical,&ci,nullptr,&p.device),"native probe device");
        // One unavailable probe must not suppress unrelated measurements.
        auto run=[&](DriverProbeAssessment &assessment,const auto &probe) {
            if (p.pending) { assessment.detail="previous native probe did not complete"; return; }
            try { assessment=probe(); }
            catch (const std::exception &error) { assessment.detail=std::string(error.what()).substr(0,4096); }
        };
        run(report.large_uniform,[&]{ return probe_large_uniform(p,physical,family,properties.limits); });
        run(report.sampler_aliases,[&]() -> DriverProbeAssessment {
            if (properties.limits.maxPerStageDescriptorSamplers<128 || properties.limits.maxDescriptorSetSamplers<128 ||
                properties.limits.maxPerStageDescriptorSampledImages<2 || properties.limits.maxDescriptorSetSampledImages<2 ||
                properties.limits.maxPerStageResources<3)
                return {DriverProbeVerdict::Inconclusive,"probe requires 128 samplers, two sampled images and one SSBO"};
            VkImageFormatProperties format{};
            auto supported=p.get<PFN_vkGetPhysicalDeviceImageFormatProperties>("vkGetPhysicalDeviceImageFormatProperties")(
                physical,VK_FORMAT_D32_SFLOAT,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,0,&format);
            if (supported!=VK_SUCCESS || format.maxExtent.width<4 || format.maxExtent.height<4)
                return {DriverProbeVerdict::Inconclusive,"D32 sampled/transfer control is unsupported"};
            return probe_sampler_aliases(p,physical,family);
        });
        run(report.push_descriptor,[&]() -> DriverProbeAssessment {
            auto enumerate=p.get<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
            uint32_t size=0;
            nativeprobe::check(enumerate(physical,nullptr,&size,nullptr),"probe native extensions");
            if (size>65536) throw std::runtime_error("invalid native extension count");
            std::vector<VkExtensionProperties> extensions(size);
            nativeprobe::check(enumerate(physical,nullptr,&size,extensions.data()),"probe native extension list");
            bool supported=false;
            for (const auto &ext:extensions) supported|=std::strcmp(ext.extensionName,VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)==0;
            if (!supported) return {DriverProbeVerdict::Inconclusive,"native push descriptors unsupported; missing feature uses emulation"};
            return isolated_push_descriptor_probe(properties,nativeprobe::revision);
        });
        run(report.tiler_budget,[&]{ return isolated_tiler_probe(properties,nativeprobe::revision); });
        return report;
    });
}
} // namespace vkfix
