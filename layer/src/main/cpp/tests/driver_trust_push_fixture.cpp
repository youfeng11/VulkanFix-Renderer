// Test-only Vulkan forwarding backend. Runs real GPU pipelines/readbacks while
// supplying push descriptors through ordinary native descriptor sets. Never APK payload.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <dlfcn.h>
#include <unordered_map>
#include <vector>

namespace {
PFN_vkGetInstanceProcAddr native() {
    static auto proc=[] {
        auto library=dlopen("libvulkan.so",RTLD_NOW|RTLD_LOCAL);
        return library ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library,"vkGetInstanceProcAddr")) : nullptr;
    }();
    return proc;
}
PFN_vkGetDeviceProcAddr gdpa{};
PFN_vkCreateDevice create_device{};
PFN_vkEnumerateDeviceExtensionProperties enumerate_extensions{};
VkDevice device{};
std::unordered_map<VkPipelineLayout,std::vector<VkDescriptorSetLayout>> layouts;
std::vector<VkDescriptorPool> pools;
template<class T> T dev(const char *name) { return reinterpret_cast<T>(gdpa(device,name)); }
bool fault(const char *value) {
    const auto mode=std::getenv("VKFIX_TRUST_PUSH_FAULT"); return mode && !std::strcmp(mode,value);
}
bool phase(const char *value) {
    const auto mode=std::getenv("VKFIX_TEST_PROBE_PHASE"); return mode && !std::strcmp(mode,value);
}
VKAPI_ATTR VkResult VKAPI_CALL extensions(VkPhysicalDevice physical,const char *layer,uint32_t *count,VkExtensionProperties *out) {
    if (layer) return enumerate_extensions(physical,layer,count,out);
    uint32_t size=0;
    auto result=enumerate_extensions(physical,nullptr,&size,nullptr);
    if (result!=VK_SUCCESS) return result;
    std::vector<VkExtensionProperties> values(size);
    result=enumerate_extensions(physical,nullptr,&size,values.data());
    if (result!=VK_SUCCESS) return result;
    values.resize(size);
    bool present=false;
    for (const auto &value:values) present|=!std::strcmp(value.extensionName,VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
    if (!present) {
        VkExtensionProperties push{}; std::strcpy(push.extensionName,VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
        push.specVersion=VK_KHR_PUSH_DESCRIPTOR_SPEC_VERSION; values.push_back(push);
    }
    if (!out) { *count=uint32_t(values.size()); return VK_SUCCESS; }
    const auto copied=std::min<uint32_t>(*count,uint32_t(values.size()));
    std::copy_n(values.data(),copied,out); *count=copied;
    return copied<values.size() ? VK_INCOMPLETE : VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL create(VkPhysicalDevice physical,const VkDeviceCreateInfo *info,const VkAllocationCallbacks *allocator,VkDevice *out) {
    auto modified=*info;
    std::vector<const char*> kept;
    for (uint32_t i=0;i<info->enabledExtensionCount;++i)
        if (std::strcmp(info->ppEnabledExtensionNames[i],VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) kept.push_back(info->ppEnabledExtensionNames[i]);
    modified.enabledExtensionCount=uint32_t(kept.size()); modified.ppEnabledExtensionNames=kept.data();
    const auto result=create_device(physical,&modified,allocator,out);
    if (result==VK_SUCCESS) device=*out;
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL set_layout(VkDevice d,const VkDescriptorSetLayoutCreateInfo *info,const VkAllocationCallbacks *allocator,VkDescriptorSetLayout *out) {
    auto modified=*info; modified.flags&=~VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    return dev<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(d,&modified,allocator,out);
}
VKAPI_ATTR VkResult VKAPI_CALL pipeline_layout(VkDevice d,const VkPipelineLayoutCreateInfo *info,const VkAllocationCallbacks *allocator,VkPipelineLayout *out) {
    auto result=dev<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(d,info,allocator,out);
    if (result==VK_SUCCESS && info->setLayoutCount) layouts[*out]={info->pSetLayouts,info->pSetLayouts+info->setLayoutCount};
    return result;
}
VKAPI_ATTR void VKAPI_CALL destroy(VkDevice d,const VkAllocationCallbacks *allocator) {
    for (auto pool:pools) dev<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool")(d,pool,nullptr);
    pools.clear(); layouts.clear();
    dev<PFN_vkDestroyDevice>("vkDestroyDevice")(d,allocator);
}
VKAPI_ATTR void VKAPI_CALL dispatch(VkCommandBuffer cb,uint32_t x,uint32_t y,uint32_t z) {
    if ((fault("control-mismatch") && phase("push-control")) ||
        (fault("subject-mismatch") && phase("push-subject"))) return;
    dev<PFN_vkCmdDispatch>("vkCmdDispatch")(cb,x,y,z);
}
VKAPI_ATTR void VKAPI_CALL push(VkCommandBuffer cb,VkPipelineBindPoint bind,VkPipelineLayout layout,uint32_t index,uint32_t count,const VkWriteDescriptorSet *writes) {
    if (fault("subject-crash")) raise(SIGSEGV);
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1};
    VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; ci.maxSets=1; ci.poolSizeCount=1; ci.pPoolSizes=&size;
    VkDescriptorPool pool{};
    if (dev<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(device,&ci,nullptr,&pool)!=VK_SUCCESS) std::abort();
    pools.push_back(pool);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; ai.descriptorPool=pool; ai.descriptorSetCount=1;
    if (!layouts.count(layout) || index>=layouts[layout].size()) std::abort();
    ai.pSetLayouts=&layouts[layout][index];
    VkDescriptorSet set{};
    if (dev<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(device,&ai,&set)!=VK_SUCCESS) std::abort();
    std::vector<VkWriteDescriptorSet> updates(writes,writes+count);
    for (auto &write:updates) write.dstSet=set;
    dev<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(device,count,updates.data(),0,nullptr);
    dev<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(cb,bind,layout,index,1,&set,0,nullptr);
}
bool suppress_tiler_draw() {
    if (fault("subject-crash") && phase("tiler-subject")) raise(SIGSEGV);
    return (fault("control-mismatch") && phase("tiler-control")) ||
           (fault("subject-mismatch") && phase("tiler-subject"));
}
VKAPI_ATTR void VKAPI_CALL draw(VkCommandBuffer cb,uint32_t vertices,uint32_t instances,uint32_t first,uint32_t first_instance) {
    if (!suppress_tiler_draw()) dev<PFN_vkCmdDraw>("vkCmdDraw")(cb,vertices,instances,first,first_instance);
}
VKAPI_ATTR void VKAPI_CALL indirect(VkCommandBuffer cb,VkBuffer buffer,VkDeviceSize offset,uint32_t count,uint32_t stride) {
    if (!suppress_tiler_draw()) dev<PFN_vkCmdDrawIndirect>("vkCmdDrawIndirect")(cb,buffer,offset,count,stride);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL get_device(VkDevice d,const char *name) {
    if (!std::strcmp(name,"vkCreateDescriptorSetLayout")) return reinterpret_cast<PFN_vkVoidFunction>(set_layout);
    if (!std::strcmp(name,"vkCreatePipelineLayout")) return reinterpret_cast<PFN_vkVoidFunction>(pipeline_layout);
    if (!std::strcmp(name,"vkCmdPushDescriptorSetKHR")) return reinterpret_cast<PFN_vkVoidFunction>(push);
    if (!std::strcmp(name,"vkCmdDispatch")) return reinterpret_cast<PFN_vkVoidFunction>(dispatch);
    if (!std::strcmp(name,"vkDestroyDevice")) return reinterpret_cast<PFN_vkVoidFunction>(destroy);
    if (!std::strcmp(name,"vkCmdDraw")) return reinterpret_cast<PFN_vkVoidFunction>(draw);
    if (!std::strcmp(name,"vkCmdDrawIndirect")) return reinterpret_cast<PFN_vkVoidFunction>(indirect);
    return gdpa(d,name);
}
}
extern "C" __attribute__((visibility("default"))) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance,const char *name) {
    auto gipa=native(); if (!gipa || !name) return nullptr;
    if (!std::strcmp(name,"vkEnumerateDeviceExtensionProperties")) {
        enumerate_extensions=reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(gipa(instance,name));
        return reinterpret_cast<PFN_vkVoidFunction>(extensions);
    }
    if (!std::strcmp(name,"vkCreateDevice")) {
        create_device=reinterpret_cast<PFN_vkCreateDevice>(gipa(instance,name));
        return reinterpret_cast<PFN_vkVoidFunction>(create);
    }
    if (!std::strcmp(name,"vkGetDeviceProcAddr")) {
        gdpa=reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(instance,name));
        return reinterpret_cast<PFN_vkVoidFunction>(get_device);
    }
    return gipa(instance,name);
}
