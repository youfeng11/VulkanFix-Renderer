#include "core/native_probe_resources.h"
#include "dispatch_base_fixture.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <spirv-tools/libspirv.hpp>

using vkfix::nativeprobe::check;
VKAPI_ATTR void* VKAPI_CALL reject_allocation(void* data, size_t, size_t, VkSystemAllocationScope) {
    ++*static_cast<unsigned*>(data); return nullptr;
}
VKAPI_ATTR void* VKAPI_CALL reject_reallocation(void*, void*, size_t, size_t, VkSystemAllocationScope) { return nullptr; }
VKAPI_ATTR void VKAPI_CALL reject_free(void*, void*) {}
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

void run(Context& p, VkPhysicalDevice physical, uint32_t family, unsigned shape, bool khr, bool templates, bool requirements2, bool allocator_failure) {
    vkfix::nativeprobe::Resources<Context> gpu(p, physical, family);
    auto output = gpu.buffer(4096 * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    if (requirements2) {
        VkMemoryRequirements legacy{};
        p.dev<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(p.device, output.handle, &legacy);
        VkMemoryDedicatedRequirements dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
        VkMemoryRequirements2 result{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &dedicated};
        VkBufferMemoryRequirementsInfo2 info{VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2, nullptr, output.handle};
        p.dev<PFN_vkGetBufferMemoryRequirements2>(khr ? "vkGetBufferMemoryRequirements2KHR" : "vkGetBufferMemoryRequirements2")(p.device, &info, &result);
        if (result.memoryRequirements.size != legacy.size || result.memoryRequirements.alignment != legacy.alignment ||
            result.memoryRequirements.memoryTypeBits != legacy.memoryTypeBits || result.pNext != &dedicated || dedicated.pNext)
            throw std::runtime_error("requirements2 lost the real buffer requirements or output headers");
    }
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
    VkDescriptorBufferInfo buffer{output.handle, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set; write.descriptorCount = 1; write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; write.pBufferInfo = &buffer;
    if (templates) {
        struct Payload { uint64_t prefix[2]{}; VkDescriptorBufferInfo buffer; } payload{{}, buffer};
        VkDescriptorUpdateTemplateEntry entry{0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, offsetof(Payload, buffer), sizeof(Payload)};
        VkDescriptorUpdateTemplateCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO};
        ci.templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET;
        ci.descriptorSetLayout = set_layout; ci.descriptorUpdateEntryCount = 1; ci.pDescriptorUpdateEntries = &entry;
        auto create_template = p.dev<PFN_vkCreateDescriptorUpdateTemplate>(khr ? "vkCreateDescriptorUpdateTemplateKHR" : "vkCreateDescriptorUpdateTemplate");
        auto destroy_template = p.dev<PFN_vkDestroyDescriptorUpdateTemplate>(khr ? "vkDestroyDescriptorUpdateTemplateKHR" : "vkDestroyDescriptorUpdateTemplate");
        if (allocator_failure) {
            unsigned requests = 0;
            const VkAllocationCallbacks allocator{&requests, reject_allocation, reject_reallocation, reject_free, nullptr, nullptr};
            VkDescriptorUpdateTemplate denied = (VkDescriptorUpdateTemplate)uint64_t(0x123);
            const auto result = create_template(p.device, &ci, &allocator, &denied);
            if (result == VK_SUCCESS && denied) destroy_template(p.device, denied, &allocator);
            if (result != VK_ERROR_OUT_OF_HOST_MEMORY || denied || !requests)
                throw std::runtime_error("template creation ignored a rejecting application allocator or published a handle on OOM");
        }
        VkDescriptorUpdateTemplate handle{};
        check(create_template(p.device, &ci, nullptr, &handle), "descriptor template");
        p.dev<PFN_vkUpdateDescriptorSetWithTemplate>(khr ? "vkUpdateDescriptorSetWithTemplateKHR" : "vkUpdateDescriptorSetWithTemplate")(p.device, set, handle, &payload);
        payload.buffer.buffer = VK_NULL_HANDLE;
        destroy_template(p.device, handle, nullptr);
    } else p.dev<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(p.device, 1, &write, 0, nullptr);
    p.dev<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")(p.device, set_layout, nullptr);
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
    p.dev<PFN_vkCmdSetDeviceMask>(khr ? "vkCmdSetDeviceMaskKHR" : "vkCmdSetDeviceMask")(cmd,1);
    p.dev<PFN_vkCmdDispatchBase>(khr ? "vkCmdDispatchBaseKHR" : "vkCmdDispatchBase")(cmd, 2, 1, 1, 2, 2, 1);
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
    auto flag = [&](const char* name) { for (int i = 2; i < argc; ++i) if (!strcmp(argv[i],name)) return true; return false; };
    const bool native_khr = flag("--native-khr"), khr_bindings = flag("--khr-bindings");
    const bool templates = flag("--templates"), requirements2 = flag("--requirements2"), khr_resources = flag("--khr-resources");
    const bool hide_templates = flag("--hide-templates");
    const bool allocator_failure = flag("--allocator-failure");
    if (khr_resources && !native_khr) return 2;
    if (allocator_failure && (!hide_templates || !templates)) return 2;
    setenv("VULKANFIX_TEST_KHR_RESOURCES", khr_resources ? "1" : "0", 1);
    setenv("VULKANFIX_TEST_HIDE_TEMPLATES", hide_templates ? "1" : "0", 1);
    if (native_khr) unsetenv("VULKAN_FIX_EMULATE_DISPATCH_BASE");
    else setenv("VULKAN_FIX_EMULATE_DISPATCH_BASE", "1", 1);
    setenv("VULKANFIX_TEST_NATIVE_GROUP_KHR",native_khr ? "1" : "0",1);
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
        std::vector<const char*> extensions;
        if (khr_bindings) {
            extensions.push_back(VK_KHR_DEVICE_GROUP_EXTENSION_NAME);
            if (templates) extensions.push_back(VK_KHR_DESCRIPTOR_UPDATE_TEMPLATE_EXTENSION_NAME);
            if (requirements2) extensions.push_back(VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME);
        }
        device.enabledExtensionCount = uint32_t(extensions.size()); device.ppEnabledExtensionNames = extensions.data();
        check(p.get<PFN_vkCreateDevice>("vkCreateDevice")(physical, &device, nullptr, &p.device), "device");
        p.gdpa = p.get<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
        auto queue2_probes = (unsigned(*)())dlsym(provider,"dispatch_base_test_queue2_probes");
        if (!queue2_probes) throw std::runtime_error("missing queue2 boundary counter");
        const auto probes_before = queue2_probes();
        VkDeviceQueueInfo2 queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2}; queue_info.queueFamilyIndex = family;
        VkQueue ordinary{}, version2{};
        p.dev<PFN_vkGetDeviceQueue2>("vkGetDeviceQueue2")(p.device,&queue_info,&version2);
        p.dev<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(p.device,family,0,&ordinary);
        if (!version2 || ordinary != version2 || queue2_probes() != probes_before) {
            char error[192];
            snprintf(error,sizeof(error),"flags==0 queue2: nonnull=%u equal=%u native-core-probes=%u (expected 1,1,0)",
                unsigned(version2 != VK_NULL_HANDLE),unsigned(ordinary == version2),queue2_probes()-probes_before);
            throw std::runtime_error(error);
        }
        puts("PASS: flags==0 queue2 matches the real ordinary queue; core queue2 probe calls=0");
        for (unsigned shape = 0; shape < 4; ++shape) run(p, physical, family, shape, khr_bindings, templates, requirements2, allocator_failure);
        if (templates || requirements2) {
            auto bad = (unsigned(*)())dlsym(provider, "dispatch_base_test_bad_resources");
            if (!bad || bad()) throw std::runtime_error("unenabled Core/KHR resource command stub was called");
            const unsigned route = native_khr && khr_resources ? 1 : 0;
            const bool native_resources = !native_khr || khr_resources;
            auto counter = [&](const char* name, unsigned r) { auto f = (unsigned(*)(unsigned))dlsym(provider, name); if (!f) throw std::runtime_error("missing resource counter"); return f(r); };
            if (requirements2 && (counter("dispatch_base_test_requirements", route) != (native_resources ? 4u : 0u) ||
                                 counter("dispatch_base_test_requirements", 1-route))) throw std::runtime_error("wrong requirements2 native backend");
            for (auto name : {"dispatch_base_test_template_creates", "dispatch_base_test_template_updates", "dispatch_base_test_template_destroys"})
                if (templates && (counter(name, route) != (native_resources && !hide_templates ? 4u : 0u) || counter(name, 1-route))) throw std::runtime_error("wrong native template lifecycle backend");
            puts("PASS: resource Core/KHR/legacy route counters and actual GPU descriptor readbacks agree");
        }
        if (kernel_count() != (native_khr ? 4u : 8u)) throw std::runtime_error("wrong native/private pipeline count");
        if (native_khr) {
            auto count = [&](const char* name) {
                auto fn = (unsigned(*)())dlsym(provider,name);
                if (!fn) throw std::runtime_error("missing native group admission counter");
                return fn();
            };
            if (count("dispatch_base_test_bad_commands") || count("dispatch_base_test_native_dispatches") != 4 ||
                count("dispatch_base_test_native_masks") != 4)
                throw std::runtime_error("native KHR group route used an unenabled spelling or lost a command");
            puts("PASS: four native KHR dispatches/masks executed without private variants; core-stub calls=0");
        } else puts("PASS: native dispatch-base entries hidden; all four private variants executed on the GPU");
        p.dev<PFN_vkDestroyDevice>("vkDestroyDevice")(p.device, nullptr);
        p.get<PFN_vkDestroyInstance>("vkDestroyInstance")(p.instance, nullptr);
        puts("Dispatch-base GPU failures: 0");
        return 0;
    } catch (const std::exception& error) { printf("FAIL: %s\n", error.what()); return 1; }
}
