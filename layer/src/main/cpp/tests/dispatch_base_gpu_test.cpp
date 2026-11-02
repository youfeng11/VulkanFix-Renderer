#include "core/native_probe_resources.h"
#include "dispatch_base_fixture.h"
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <spirv-tools/libspirv.hpp>

using vkfix::nativeprobe::check;
struct Context {
    PFN_vkGetInstanceProcAddr gipa{};
    PFN_vkGetDeviceProcAddr gdpa{};
    VkInstance instance{};
    VkDevice device{};
    bool pending = false, retain_device = false;
    template<class T> T get(const char* name) {
        auto function = reinterpret_cast<T>(gipa(instance, name));
        if (!function) throw std::runtime_error(std::string("missing ") + name);
        return function;
    }
    template<class T> T dev(const char* name) {
        auto function = reinterpret_cast<T>(gdpa(device, name));
        if (!function) throw std::runtime_error(std::string("missing ") + name);
        return function;
    }
};

void run(Context& p, VkPhysicalDevice physical, uint32_t family, unsigned shape) {
    vkfix::nativeprobe::Resources<Context> gpu(p, physical, family);
    auto output = gpu.buffer(4096 * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = 1; set_info.pBindings = &binding;
    VkDescriptorSetLayout set_layout{};
    check(p.dev<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(
        p.device, &set_info, nullptr, &set_layout), "set layout");
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 1; layout_info.pSetLayouts = &set_layout;
    VkPipelineLayout source_layout{}, binding_layout{};
    auto create_layout = p.dev<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout");
    auto destroy_layout = p.dev<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout");
    check(create_layout(p.device, &layout_info, nullptr, &source_layout), "source layout");
    check(create_layout(p.device, &layout_info, nullptr, &binding_layout), "compatible binding layout");
    gpu.objects.push_back([=, &p] { destroy_layout(p.device, binding_layout, nullptr); });
    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = 1; pool_info.poolSizeCount = 1; pool_info.pPoolSizes = &pool_size;
    VkDescriptorPool pool{};
    check(p.dev<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(p.device, &pool_info, nullptr, &pool), "descriptor pool");
    auto destroy_pool = p.dev<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool");
    gpu.objects.push_back([=, &p] { destroy_pool(p.device, pool, nullptr); });
    VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocation.descriptorPool = pool; allocation.descriptorSetCount = 1; allocation.pSetLayouts = &set_layout;
    VkDescriptorSet set{};
    check(p.dev<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(p.device, &allocation, &set), "descriptor set");
    p.dev<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")(p.device, set_layout, nullptr);
    VkDescriptorBufferInfo buffer{output.handle, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set; write.descriptorCount = 1; write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; write.pBufferInfo = &buffer;
    p.dev<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(p.device, 1, &write, 0, nullptr);
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
    std::vector<uint32_t> words;
    if (!tools.Assemble(dispatch_base_fixture(shape == 1, shape == 2, shape == 3), &words) || !tools.Validate(words))
        throw std::runtime_error("invalid dispatch fixture");
    VkShaderModuleCreateInfo shader_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shader_info.codeSize = words.size() * 4; shader_info.pCode = words.data();
    VkShaderModule shader{};
    check(p.dev<PFN_vkCreateShaderModule>("vkCreateShaderModule")(p.device, &shader_info, nullptr, &shader), "shader");
    VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.flags = VK_PIPELINE_CREATE_DISPATCH_BASE_BIT;
    info.layout = source_layout;
    info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; info.stage.module = shader; info.stage.pName = "kernel";
    const uint32_t size_x = 8;
    VkSpecializationMapEntry entry{0, 0, sizeof(size_x)};
    VkSpecializationInfo specialization{1, &entry, sizeof(size_x), &size_x};
    if (shape == 1) info.stage.pSpecializationInfo = &specialization;
    VkPipeline pipeline{};
    check(p.dev<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(p.device, {}, 1, &info, nullptr, &pipeline), "pipeline");
    auto destroy_pipeline = p.dev<PFN_vkDestroyPipeline>("vkDestroyPipeline");
    gpu.objects.push_back([=, &p] { destroy_pipeline(p.device, pipeline, nullptr); });
    // Both objects may legally die before any command using this pipeline.
    p.dev<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(p.device, shader, nullptr);
    destroy_layout(p.device, source_layout, nullptr);
    auto cmd = gpu.begin();
    p.dev<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    p.dev<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(
        cmd, VK_PIPELINE_BIND_POINT_COMPUTE, binding_layout, 0, 1, &set, 0, nullptr);
    p.dev<PFN_vkCmdDispatchBase>("vkCmdDispatchBase")(cmd, 2, 1, 1, 2, 2, 1);
    gpu.host_barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    gpu.submit(cmd); gpu.invalidate(output);
    const auto values = static_cast<const uint32_t*>(output.mapped);
    for (unsigned index = 0; index < 4096; ++index) {
        const unsigned x = index % 64, y = (index % 2048) / 64, z = index / 2048;
        uint32_t expected = 0xa5a5a5a5u;
        if (x >= 16 && x < 32 && y >= 2 && y < 6 && z == 1) {
            const uint32_t component = (x % 8) % 3;
            const uint32_t selected = component == 0 ? x : component == 1 ? y : z;
            expected = (x / 8) * 65536 + (y / 2) * 256 + z + selected;
        }
        if (values[index] != expected) {
            char error[192];
            snprintf(error, sizeof(error), "shape %u index %u: actual=%u expected=%u", shape, index, values[index], expected);
            throw std::runtime_error(error);
        }
    }
    printf("PASS: shape=%u, local=(8,2,1), base=(2,1,1), Phi/dynamic index and destroyed shader/layout\n", shape);
}
int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setenv("VULKAN_FIX_EMULATE_DISPATCH_BASE", "1", 1);
    setenv("VULKAN_FIX_DRIVER_PATH", "./libdispatch_base_test_driver.so", 1);
    setenv("VULKAN_FIX_DISABLE_PIPELINE_CACHE", "1", 1);
    setenv("VULKAN_FIX_LOG_PATH", "./dispatch-base-gpu-layer.log", 1);
    Context p;
    try {
        auto provider = dlopen("./libdispatch_base_test_driver.so", RTLD_NOW | RTLD_LOCAL);
        if (!provider) throw std::runtime_error(dlerror());
        auto kernel_count = (unsigned(*)())dlsym(provider, "dispatch_base_test_kernel_pipelines");
        if (!kernel_count) throw std::runtime_error("missing native pipeline counter");
        auto library = dlopen(argc > 1 ? argv[1] : "./libvulkanfix.so", RTLD_NOW | RTLD_LOCAL);
        if (!library) throw std::runtime_error(dlerror());
        p.gipa = (PFN_vkGetInstanceProcAddr)dlsym(library, "vkGetInstanceProcAddr");
        if (!p.gipa) throw std::runtime_error("missing gipa");
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo instance{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; instance.pApplicationInfo = &app;
        check(p.get<PFN_vkCreateInstance>("vkCreateInstance")(&instance, nullptr, &p.instance), "instance");
        uint32_t count = 0;
        auto enumerate = p.get<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
        check(enumerate(p.instance, &count, nullptr), "physical count");
        std::vector<VkPhysicalDevice> physicals(count);
        check(enumerate(p.instance, &count, physicals.data()), "physicals");
        if (physicals.empty()) throw std::runtime_error("no physical device");
        const auto physical = physicals.front();
        auto families = p.get<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties");
        families(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> properties(count); families(physical, &count, properties.data());
        uint32_t family = count;
        for (uint32_t i = 0; i < count; ++i) if (properties[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { family = i; break; }
        if (family == count) throw std::runtime_error("no compute queue");
        float priority = 1;
        VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue.queueFamilyIndex = family; queue.queueCount = 1; queue.pQueuePriorities = &priority;
        VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; device.queueCreateInfoCount = 1; device.pQueueCreateInfos = &queue;
        check(p.get<PFN_vkCreateDevice>("vkCreateDevice")(physical, &device, nullptr, &p.device), "device");
        p.gdpa = p.get<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
        for (unsigned shape = 0; shape < 4; ++shape) run(p, physical, family, shape);
        if (kernel_count() != 8) throw std::runtime_error("GPU tests did not create all four emulated pipeline variants");
        puts("PASS: native dispatch-base entries hidden; all four private variants executed on the GPU");
        p.dev<PFN_vkDestroyDevice>("vkDestroyDevice")(p.device, nullptr);
        p.get<PFN_vkDestroyInstance>("vkDestroyInstance")(p.instance, nullptr);
        puts("Dispatch-base GPU failures: 0");
        return 0;
    } catch (const std::exception& error) { printf("FAIL: %s\n", error.what()); return 1; }
}
