#include <vulkan/vulkan.h>
#include <spirv-tools/libspirv.hpp>
#include "sampler_alias_fixture.h"
#include <dlfcn.h>
#include <link.h>
#include <sys/prctl.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

static unsigned validation_errors=0;
static VKAPI_ATTR VkBool32 VKAPI_CALL validation_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT* data,void*){
 if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)validation_errors++;
 printf("VALIDATION %s: %s\n",data->pMessageIdName?data->pMessageIdName:"",data->pMessage?data->pMessage:"");
 return VK_FALSE;
}
static void ok(VkResult r,const char* w){if(r!=VK_SUCCESS){printf("ERROR %s VkResult=%d\n",w,r);throw std::runtime_error(w);}}
static std::vector<uint32_t> read_spv(const char* path){
 std::ifstream f(path,std::ios::binary|std::ios::ate); if(!f)throw std::runtime_error("open shader");
 auto n=f.tellg();if(n<20 || size_t(n)%4)throw std::runtime_error("shader length");
 std::vector<uint32_t> w(size_t(n)/4);f.seekg(0);f.read(reinterpret_cast<char*>(w.data()),n);return w;
}
static int inspect_module(dl_phdr_info* p,size_t,void* data){
 const char* n=p->dlpi_name;
 if(strstr(n,"vulkan")||strstr(n,"mali")||strstr(n,"Mali")||strstr(n,"renderdoc")||strstr(n,"VkLayer"))printf("MODULE %s\n",n);
 if(strstr(n,"vulkanfix")) *static_cast<bool*>(data)=true;
 if(strstr(n,"renderdoc")) throw std::runtime_error("RenderDoc must not be loaded");
 return 0;
}
struct H{
 VkInstance inst{};VkDebugUtilsMessengerEXT messenger{};VkPhysicalDevice phys{};VkDevice dev{};VkQueue queue{};uint32_t family{};
 VkDescriptorSetLayout sl{};VkPipelineLayout pl{};VkDescriptorPool dp{};VkDescriptorSet set{};
 VkCommandPool cp{};VkCommandBuffer cmd{};VkFence fence{};
 VkSampler sampler[2]{};VkImage image[2]{};VkImageView view[2]{};VkDeviceMemory imem[2]{};
 VkBuffer buffer{};VkDeviceMemory bmem{};void* mapped{};
 uint32_t memtype(uint32_t bits,VkMemoryPropertyFlags flags){
  VkPhysicalDeviceMemoryProperties m{};vkGetPhysicalDeviceMemoryProperties(phys,&m);
  for(uint32_t i=0;i<m.memoryTypeCount;i++)if((bits&(1u<<i))&&(m.memoryTypes[i].propertyFlags&flags)==flags)return i;
  throw std::runtime_error("no memory type");
 }
 ~H(){
  if(dev){vkDeviceWaitIdle(dev);if(fence)vkDestroyFence(dev,fence,nullptr);if(cp)vkDestroyCommandPool(dev,cp,nullptr);
   if(dp)vkDestroyDescriptorPool(dev,dp,nullptr);if(pl)vkDestroyPipelineLayout(dev,pl,nullptr);if(sl)vkDestroyDescriptorSetLayout(dev,sl,nullptr);
   if(mapped)vkUnmapMemory(dev,bmem);if(buffer)vkDestroyBuffer(dev,buffer,nullptr);if(bmem)vkFreeMemory(dev,bmem,nullptr);
   for(int i=0;i<2;i++){if(view[i])vkDestroyImageView(dev,view[i],nullptr);if(image[i])vkDestroyImage(dev,image[i],nullptr);
    if(imem[i])vkFreeMemory(dev,imem[i],nullptr);if(sampler[i])vkDestroySampler(dev,sampler[i],nullptr);}
   vkDestroyDevice(dev,nullptr);}
  if(messenger)reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(inst,"vkDestroyDebugUtilsMessengerEXT"))(inst,messenger,nullptr);
  if(inst)vkDestroyInstance(inst,nullptr);
 }
 void init(){
  if(std::getenv("IRIS_ENABLE_VALIDATION") && prctl(PR_SET_DUMPABLE,1,0,0,0)!=0)throw std::runtime_error("enable diagnostic layer discovery");
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="Native sampler alias attribution";app.apiVersion=VK_API_VERSION_1_1;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ici.pApplicationInfo=&app;
  const bool validate=std::getenv("IRIS_ENABLE_VALIDATION")!=nullptr;
  const char* layer="VK_LAYER_KHRONOS_validation";const char* ext=VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
  VkDebugUtilsMessengerCreateInfoEXT dbg{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
  dbg.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
  dbg.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
  dbg.pfnUserCallback=validation_message;
  if(validate){ici.enabledLayerCount=1;ici.ppEnabledLayerNames=&layer;ici.enabledExtensionCount=1;ici.ppEnabledExtensionNames=&ext;ici.pNext=&dbg;}
  ok(vkCreateInstance(&ici,nullptr,&inst),"instance");
  if(validate){auto create=reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(inst,"vkCreateDebugUtilsMessengerEXT"));
   if(!create)throw std::runtime_error("debug utils missing");ok(create(inst,&dbg,nullptr,&messenger),"validation messenger");printf("VALIDATION_LAYER_EXPLICITLY_ENABLED\n");}
  uint32_t n=0;ok(vkEnumeratePhysicalDevices(inst,&n,nullptr),"GPU count");if(!n)throw std::runtime_error("no GPU");
  std::vector<VkPhysicalDevice> g(n);ok(vkEnumeratePhysicalDevices(inst,&n,g.data()),"GPU");phys=g[0];
  VkPhysicalDeviceProperties p{};vkGetPhysicalDeviceProperties(phys,&p);
  printf("DEVICE %s api=%u driver=%u vendor=%u samplerLimit=%u imageLimit=%u\n",p.deviceName,p.apiVersion,p.driverVersion,p.vendorID,p.limits.maxPerStageDescriptorSamplers,p.limits.maxPerStageDescriptorSampledImages);
  vkGetPhysicalDeviceQueueFamilyProperties(phys,&n,nullptr);std::vector<VkQueueFamilyProperties> fs(n);vkGetPhysicalDeviceQueueFamilyProperties(phys,&n,fs.data());
  while(family<n && !(fs[family].queueFlags&VK_QUEUE_COMPUTE_BIT))family++;if(family==n)throw std::runtime_error("no compute queue");
  float priority=1;VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qi.queueFamilyIndex=family;qi.queueCount=1;qi.pQueuePriorities=&priority;
  VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};di.queueCreateInfoCount=1;di.pQueueCreateInfos=&qi;
  ok(vkCreateDevice(phys,&di,nullptr,&dev),"device");vkGetDeviceQueue(dev,family,0,&queue);
  VkFormatProperties fp{};vkGetPhysicalDeviceFormatProperties(phys,VK_FORMAT_D32_SFLOAT,&fp);
  if(!(fp.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))throw std::runtime_error("D32 sampling missing");
  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=16;bi.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  ok(vkCreateBuffer(dev,&bi,nullptr,&buffer),"buffer");VkMemoryRequirements req{};vkGetBufferMemoryRequirements(dev,buffer,&req);
  VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ma.allocationSize=req.size;ma.memoryTypeIndex=memtype(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ok(vkAllocateMemory(dev,&ma,nullptr,&bmem),"buffer memory");ok(vkBindBufferMemory(dev,buffer,bmem,0),"bind buffer");ok(vkMapMemory(dev,bmem,0,VK_WHOLE_SIZE,0,&mapped),"map");
  for(int i=0;i<2;i++){
   VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ii.imageType=VK_IMAGE_TYPE_2D;ii.format=VK_FORMAT_D32_SFLOAT;
   ii.extent={4,4,1};ii.mipLevels=ii.arrayLayers=1;ii.samples=VK_SAMPLE_COUNT_1_BIT;ii.tiling=VK_IMAGE_TILING_OPTIMAL;
   ii.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
   ok(vkCreateImage(dev,&ii,nullptr,&image[i]),"image");vkGetImageMemoryRequirements(dev,image[i],&req);
   ma.allocationSize=req.size;ma.memoryTypeIndex=memtype(req.memoryTypeBits,0);
   ok(vkAllocateMemory(dev,&ma,nullptr,&imem[i]),"image memory");ok(vkBindImageMemory(dev,image[i],imem[i],0),"bind image");
   VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=image[i];vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=ii.format;
   vi.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1};ok(vkCreateImageView(dev,&vi,nullptr,&view[i]),"view");
   VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};si.magFilter=si.minFilter=VK_FILTER_NEAREST;si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
   si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;si.maxLod=0;si.compareEnable=i;si.compareOp=VK_COMPARE_OP_LESS_OR_EQUAL;
   ok(vkCreateSampler(dev,&si,nullptr,&sampler[i]),"sampler");
  }
  VkDescriptorSetLayoutBinding bs[]={{0,VK_DESCRIPTOR_TYPE_SAMPLER,128,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},{1,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,2,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},{2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
  VkDescriptorSetLayoutCreateInfo sc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};sc.bindingCount=3;sc.pBindings=bs;ok(vkCreateDescriptorSetLayout(dev,&sc,nullptr,&sl),"set layout");
  VkPipelineLayoutCreateInfo lc{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};lc.setLayoutCount=1;lc.pSetLayouts=&sl;ok(vkCreatePipelineLayout(dev,&lc,nullptr,&pl),"pipeline layout");
  VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_SAMPLER,128},{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,2},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}};
  VkDescriptorPoolCreateInfo pc{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pc.maxSets=1;pc.poolSizeCount=3;pc.pPoolSizes=sizes;ok(vkCreateDescriptorPool(dev,&pc,nullptr,&dp),"descriptor pool");
  VkDescriptorSetAllocateInfo sa{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};sa.descriptorPool=dp;sa.descriptorSetCount=1;sa.pSetLayouts=&sl;ok(vkAllocateDescriptorSets(dev,&sa,&set),"set");
  VkDescriptorImageInfo samplers[128]{};for(int i=0;i<128;i++)samplers[i].sampler=sampler[i==2?1:0];
  VkDescriptorImageInfo images[2]{};for(int i=0;i<2;i++){images[i].imageView=view[i];images[i].imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;}
  VkDescriptorBufferInfo bo{buffer,0,16};VkWriteDescriptorSet wr[3]{};
  for(int i=0;i<3;i++){wr[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;wr[i].dstSet=set;wr[i].dstBinding=i;wr[i].descriptorCount=bs[i].descriptorCount;wr[i].descriptorType=bs[i].descriptorType;}
  wr[0].pImageInfo=samplers;wr[1].pImageInfo=images;wr[2].pBufferInfo=&bo;vkUpdateDescriptorSets(dev,3,wr,0,nullptr);
  VkCommandPoolCreateInfo cc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};cc.queueFamilyIndex=family;cc.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;ok(vkCreateCommandPool(dev,&cc,nullptr,&cp),"command pool");
  VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=cp;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;ok(vkAllocateCommandBuffers(dev,&ca,&cmd),"command");
  VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};ok(vkCreateFence(dev,&fc,nullptr,&fence),"fence");
  bool bad=false;dl_iterate_phdr(inspect_module,&bad);if(!bad)throw std::runtime_error("VulkanFix wrapper was not loaded");
  Dl_info info{};dladdr(reinterpret_cast<void*>(vkCreateInstance),&info);printf("CREATE_INSTANCE_FROM %s\n",info.dli_fname?info.dli_fname:"unknown");
 }
 int run(const char* path, const std::vector<uint32_t>& words, int& sequence){
  VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};sm.codeSize=words.size()*4;sm.pCode=words.data();
  VkShaderModule mod{};ok(vkCreateShaderModule(dev,&sm,nullptr,&mod),"shader");
  VkComputePipelineCreateInfo pc{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};pc.layout=pl;pc.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  pc.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;pc.stage.module=mod;pc.stage.pName="main";VkPipeline pipe{};
  ok(vkCreateComputePipelines(dev,VK_NULL_HANDLE,1,&pc,nullptr,&pipe),"pipeline");vkDestroyShaderModule(dev,mod,nullptr);
  int mismatches=0;
  for(int j=0;j<16;j++,sequence++){
   float a=.25f+j*.01f,b=.75f-j*.01f;std::memset(mapped,0xcd,16);
   ok(vkResetCommandBuffer(cmd,0),"reset");VkCommandBufferBeginInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};ok(vkBeginCommandBuffer(cmd,&cb),"begin");
   for(int i=0;i<2;i++){
    VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};ib.oldLayout=sequence?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED;ib.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    ib.srcAccessMask=sequence?VK_ACCESS_SHADER_READ_BIT:0;ib.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;ib.srcQueueFamilyIndex=ib.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    ib.image=image[i];ib.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1};
    vkCmdPipelineBarrier(cmd,sequence?VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&ib);
    VkClearDepthStencilValue clear{i?b:a,0};vkCmdClearDepthStencilImage(cmd,image[i],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&clear,1,&ib.subresourceRange);
    ib.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;ib.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;ib.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;ib.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&ib);
   }
   vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipe);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pl,0,1,&set,0,nullptr);vkCmdDispatch(cmd,1,1,1);
   VkBufferMemoryBarrier bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};bb.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;bb.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
   bb.srcQueueFamilyIndex=bb.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;bb.buffer=buffer;bb.offset=0;bb.size=16;
   vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&bb,0,nullptr);
   ok(vkEndCommandBuffer(cmd),"end");ok(vkResetFences(dev,1,&fence),"reset fence");VkSubmitInfo sub{VK_STRUCTURE_TYPE_SUBMIT_INFO};sub.commandBufferCount=1;sub.pCommandBuffers=&cmd;
   ok(vkQueueSubmit(queue,1,&sub,fence),"submit");ok(vkWaitForFences(dev,1,&fence,VK_TRUE,10000000000ULL),"wait");
   float actual[4];std::memcpy(actual,mapped,16);float expected[]={a,1.f,b,(a+b)*.5f};
   for(int k=0;k<4;k++)if(!std::isfinite(actual[k])||std::fabs(actual[k]-expected[k])>.0001f){
    if(mismatches<4)printf("MISMATCH %s frame=%d lane=%d actual=%.8f expected=%.8f\n",path,j,k,actual[k],expected[k]);mismatches++;
   }
   if(j==0)printf("VALUES %s %.8f %.8f %.8f %.8f\n",path,actual[0],actual[1],actual[2],actual[3]);
  }
  vkDestroyPipeline(dev,pipe,nullptr);printf("RESULT %s mismatches=%d tested_values=64\n",path,mismatches);return mismatches;
 }
};
int main(int argc, char** argv) try {
    setenv("VULKAN_FIX_DISABLE_PIPELINE_CACHE", "1", 1);
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc != 1 && argc != 3)
        throw std::runtime_error("usage: sampler_alias_dedup_gpu_test [aliased.spv dedup.spv]");
    auto assemble = [](const char* source) {
        spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
        std::vector<uint32_t> words;
        if (!tools.Assemble(source, &words) || !tools.Validate(words))
            throw std::runtime_error("invalid built-in sampler fixture");
        return words;
    };
    const auto aliased = argc == 3 ? read_spv(argv[1]) : assemble(kAliasedSamplerShader);
    const auto canonical = argc == 3 ? read_spv(argv[2]) : assemble(kCanonicalSamplerShader);
    Dl_info info{};
    dladdr(reinterpret_cast<void*>(vkCreateShaderModule), &info);
    if (!info.dli_fname || !strstr(info.dli_fname, "libvulkanfix.so"))
        throw std::runtime_error("shader module entrypoint does not resolve to VulkanFix");
    printf("CREATE_SHADER_MODULE_FROM %s\n", info.dli_fname);
    bool passed = false;
    {
        H h;
        h.init();
        int sequence = 0;
        const int alias = h.run("aliased", aliased, sequence);
        const int dedup = h.run("canonical", canonical, sequence);
        const int repeat = h.run("aliased-repeat", aliased, sequence);
        const char* override_value = getenv("VULKAN_FIX_DEDUP_SAMPLER_ALIASES");
        const bool disabled = override_value && !strcmp(override_value, "0");
        passed = disabled ? (alias == 48 && repeat == 48 && dedup == 0)
                          : (alias == 0 && repeat == 0 && dedup == 0);
        printf("RESULTS alias=%d dedup=%d alias_repeat=%d\n", alias, dedup, repeat);
    }
    printf("VALIDATION_ERRORS %u\n", validation_errors);
    passed = passed && !validation_errors;
    printf("VULKANFIX_SAMPLER_REGRESSION %s\n", passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
} catch (const std::exception& e) {
    printf("ERROR %s\n", e.what());
    return 2;
}
