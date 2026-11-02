#pragma once
#include "native_probe_resources.h"
#include <map>
#include <set>
#include <sstream>
namespace vkfix::nativeprobe {
template<class Context>
std::string subgroup_basic(Context& p,VkPhysicalDevice physical,uint32_t family,
    const uint32_t* words,size_t word_count,const VkPhysicalDeviceSubgroupProperties& properties,
    bool specialized,bool dispatch_base=false,bool expect_single=false,
    const char* entry="main",uint32_t invocations=12) {
    Resources<Context> gpu(p,physical,family);
    const uint32_t first=dispatch_base?1:0,groups=2,stride=12,fields=8;
    const uint32_t count=(first+groups)*stride*fields;
    auto output=gpu.buffer(count*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::memset(output.mapped,0,count*4);
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE}; range.memory=output.memory; range.size=VK_WHOLE_SIZE;
    check(p.template dev<PFN_vkFlushMappedMemoryRanges>("vkFlushMappedMemoryRanges")(p.device,1,&range),"flush subgroup output");
    VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo set_ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_ci.bindingCount=1; set_ci.pBindings=&binding;
    VkDescriptorSetLayout set_layout{};
    check(p.template dev<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(p.device,&set_ci,nullptr,&set_layout),"subgroup set layout");
    auto destroy_set=p.template dev<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout");
    gpu.objects.push_back([=,&p] { destroy_set(p.device,set_layout,nullptr); });
    VkPipelineLayoutCreateInfo layout_ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; layout_ci.setLayoutCount=1; layout_ci.pSetLayouts=&set_layout;
    VkPipelineLayout layout{};
    check(p.template dev<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(p.device,&layout_ci,nullptr,&layout),"subgroup pipeline layout");
    auto destroy_layout=p.template dev<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout");
    gpu.objects.push_back([=,&p] { destroy_layout(p.device,layout,nullptr); });
    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1};
    VkDescriptorPoolCreateInfo pool_ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_ci.maxSets=1; pool_ci.poolSizeCount=1; pool_ci.pPoolSizes=&pool_size;
    VkDescriptorPool pool{};
    check(p.template dev<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(p.device,&pool_ci,nullptr,&pool),"subgroup pool");
    auto destroy_pool=p.template dev<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool");
    gpu.objects.push_back([=,&p] { destroy_pool(p.device,pool,nullptr); });
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool=pool; allocate.descriptorSetCount=1; allocate.pSetLayouts=&set_layout;
    VkDescriptorSet set{};
    check(p.template dev<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(p.device,&allocate,&set),"subgroup descriptor set");
    VkDescriptorBufferInfo buffer{output.handle,0,count*4};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; write.dstSet=set; write.dstBinding=0;
    write.descriptorCount=1; write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; write.pBufferInfo=&buffer;
    p.template dev<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(p.device,1,&write,0,nullptr);
    VkShaderModuleCreateInfo shader_ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; shader_ci.pCode=words; shader_ci.codeSize=word_count*4;
    VkShaderModule shader{};
    check(p.template dev<PFN_vkCreateShaderModule>("vkCreateShaderModule")(p.device,&shader_ci,nullptr,&shader),"subgroup shader");
    auto destroy_shader=p.template dev<PFN_vkDestroyShaderModule>("vkDestroyShaderModule");
    gpu.objects.push_back([=,&p] { destroy_shader(p.device,shader,nullptr); });
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; ci.layout=layout;
    ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}; ci.stage.module=shader;
    ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT; ci.stage.pName=entry;
    if (dispatch_base) ci.flags=VK_PIPELINE_CREATE_DISPATCH_BASE_BIT;
    const uint32_t sizes[3]{2,3,2};
    VkSpecializationMapEntry entries[3]{{4,0,4},{5,4,4},{6,8,4}};
    VkSpecializationInfo spec{3,entries,sizeof sizes,sizes};
    if (specialized) ci.stage.pSpecializationInfo=&spec;
    VkPipeline pipeline{};
    check(p.template dev<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(p.device,{},1,&ci,nullptr,&pipeline),"subgroup pipeline");
    destroy_shader(p.device,shader,nullptr); gpu.objects.pop_back();
    auto destroy_pipeline=p.template dev<PFN_vkDestroyPipeline>("vkDestroyPipeline");
    gpu.objects.push_back([=,&p] { destroy_pipeline(p.device,pipeline,nullptr); });
    auto cmd=gpu.begin();
    p.template dev<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
    p.template dev<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,nullptr);
    if (dispatch_base) p.template dev<PFN_vkCmdDispatchBase>("vkCmdDispatchBase")(cmd,first,0,0,groups,1,1);
    else p.template dev<PFN_vkCmdDispatch>("vkCmdDispatch")(cmd,groups,1,1);
    gpu.host_barrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT);
    gpu.submit(cmd); gpu.invalidate(output);
    const auto* values=static_cast<const uint32_t*>(output.mapped);
    for (uint32_t i=0;i<first*stride*fields;++i) if (values[i]) return "dispatch-base wrote before the requested workgroup";
    for (uint32_t group=first;group<first+groups;++group) {
        std::map<uint32_t,std::map<uint32_t,uint32_t>> lanes;
        uint32_t subgroups=0;
        for (uint32_t local=0;local<invocations;++local) {
            const auto* row=values+(group*stride+local)*fields;
            if (row[0]!=properties.subgroupSize || row[1]>=row[0] || !row[3] || row[2]>=row[3] || row[4]>1 ||
                row[5]!=0 || row[6]!=100 || row[7]!=42+local || (subgroups && subgroups!=row[3]) ||
                (expect_single && (row[0]!=1 || row[1] || row[2]!=local || row[3]!=invocations || row[4]!=1))) {
                std::ostringstream error; error<<"workgroup "<<group<<" invocation "<<local<<" readback:";
                for (unsigned field=0;field<fields;++field) error<<' '<<row[field];
                return error.str();
            }
            subgroups=row[3];
            if (!lanes[row[2]].emplace(row[1],row[4]).second) return "duplicate subgroup-local ID in a workgroup";
        }
        for (uint32_t i=invocations*fields;i<stride*fields;++i)
            if (values[group*stride*fields+i]) return "entry point wrote beyond its local invocation count";
        for (const auto& subgroup:lanes) for (const auto& lane:subgroup.second)
            if (lane.second!=(lane.first==subgroup.second.begin()->first)) return "elect did not select the lowest active lane";
    }
    return {};
}
}
