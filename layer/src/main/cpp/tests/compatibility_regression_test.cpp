#include "core/driver_loader.h"
#include "core/layer_manager.h"
#include "core/vertex_store_emulator.h"
#include "modules/vulkan_1_0/vertex_pipeline_stores_and_atomics.h"
#include "modules/vulkan_1_2/buffer_device_address.h"
#include "shader/vertex_store_lowering.h"
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <spirv-tools/libspirv.hpp>
#include <thread>
void *timeline_mock_proc(const char *name);
void run_timeline_regressions(void (*check)(bool, const char *));
void* push_descriptor_mock_proc(const char* name);
void* mali_tiler_mock_proc(const char* name);
void run_mali_tiler_regressions(void (*check)(bool, const char*));
void run_dynamic_rendering_lifetime_regressions(void (*check)(bool, const char*));
void run_push_descriptor_regressions(void (*check)(bool, const char*));

// Replace only the driver boundary; exercise the actual modules and manager.
static bool native_atomics = false;
static bool native_address = false;
static bool second_dispatch = false;
static unsigned cloned_sets_created = 0, cloned_sets_destroyed = 0;
static unsigned cloned_layouts_created = 0, cloned_layouts_destroyed = 0;
static int failures = 0;
static void check(bool ok, const char *name) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok)
        ++failures;
}

static VKAPI_ATTR void VKAPI_CALL features(VkPhysicalDevice, VkPhysicalDeviceFeatures *out) {
    *out = {};
    out->vertexPipelineStoresAndAtomics = native_atomics;
}
static VKAPI_ATTR void VKAPI_CALL properties(VkPhysicalDevice, VkPhysicalDeviceProperties *out) {
    *out = {};
    out->apiVersion = VK_API_VERSION_1_0;
}
static VKAPI_ATTR void VKAPI_CALL memory_properties(VkPhysicalDevice,
                                                    VkPhysicalDeviceMemoryProperties *out) {
    *out = {};
}
static VKAPI_ATTR VkResult VKAPI_CALL create_set_layout(VkDevice,
                                                        const VkDescriptorSetLayoutCreateInfo *,
                                                        const VkAllocationCallbacks *,
                                                        VkDescriptorSetLayout *out) {
    *out = (VkDescriptorSetLayout)(uint64_t(0x9876) + cloned_sets_created++);
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL destroy_set_layout(VkDevice, VkDescriptorSetLayout,
                                                     const VkAllocationCallbacks *) {
    ++cloned_sets_destroyed;
}
static VKAPI_ATTR VkResult VKAPI_CALL create_pipeline_layout(VkDevice,
                                                             const VkPipelineLayoutCreateInfo *,
                                                             const VkAllocationCallbacks *,
                                                             VkPipelineLayout *out) {
    *out = (VkPipelineLayout)(uint64_t(0x40000) + cloned_layouts_created++);
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL destroy_pipeline_layout(VkDevice, VkPipelineLayout,
                                                          const VkAllocationCallbacks *) {
    ++cloned_layouts_destroyed;
}
static VKAPI_ATTR VkResult VKAPI_CALL extensions(VkPhysicalDevice, const char *, uint32_t *count,
                                                 VkExtensionProperties *) {
    *count = 0;
    return VK_SUCCESS;
}
static VKAPI_ATTR VkDeviceAddress VKAPI_CALL address(VkDevice, const VkBufferDeviceAddressInfo *) {
    return 0x12340000;
}
static VKAPI_ATTR void VKAPI_CALL draw_a(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t) {}
static VKAPI_ATTR void VKAPI_CALL draw_b(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t) {}

extern "C" {
void init_real_vulkan() {}
void register_vulkan_ptr() {}
void set_last_instance(VkInstance) {}
VkInstance get_last_instance() {
    return VK_NULL_HANDLE;
}
PFN_vkGetInstanceProcAddr get_real_instance_proc_addr() {
    return nullptr;
}
PFN_vkGetDeviceProcAddr get_real_device_proc_addr() {
    return nullptr;
}
void *get_real_proc(VkInstance, VkDevice, const char *name) {
    if (auto mock = mali_tiler_mock_proc(name)) return mock;
    if (auto mock = push_descriptor_mock_proc(name))
        return mock;
    if (auto mock = timeline_mock_proc(name))
        return mock;
    if (!strcmp(name, "vkGetPhysicalDeviceFeatures"))
        return (void *)features;
    if (!strcmp(name, "vkGetPhysicalDeviceProperties"))
        return (void *)properties;
    if (!strcmp(name, "vkGetPhysicalDeviceMemoryProperties"))
        return (void *)memory_properties;
    if (!strcmp(name, "vkCreateDescriptorSetLayout"))
        return (void *)create_set_layout;
    if (!strcmp(name, "vkDestroyDescriptorSetLayout"))
        return (void *)destroy_set_layout;
    if (!strcmp(name, "vkCreatePipelineLayout"))
        return (void *)create_pipeline_layout;
    if (!strcmp(name, "vkDestroyPipelineLayout"))
        return (void *)destroy_pipeline_layout;
    if (!strcmp(name, "vkEnumerateDeviceExtensionProperties"))
        return (void *)extensions;
    if (!strcmp(name, "vkGetBufferDeviceAddress") && native_address)
        return (void *)address;
    if (!strcmp(name, "vkCmdDraw"))
        return second_dispatch ? (void *)draw_b : (void *)draw_a;
    return nullptr;
}
}

static void test_atomics(VkPhysicalDevice phys) {
    const char *feature_env = "VULKAN_FIX_EMULATE_VERTEX_PIPELINE_STORES_AND_ATOMICS_FEATURE";
    const char *module_env = "VULKAN_FIX_EMULATE_VERTEX_PIPELINE_STORES_AND_ATOMICS";
    unsetenv("FORCE_EMULATE_VERTEX_PIPELINE_STORES_AND_ATOMICS");
    unsetenv("FORCE_EMULATE_VERTEX_PIPELINE_STORES_AND_ATOMICS_FEATURE");
    setenv(module_env, "auto", 1);
    for (const char *mode : {"auto", "skip", "force"}) {
        setenv(feature_env, mode, 1);
        for (bool native : {false, true}) {
            native_atomics = native;
            VertexPipelineStoresAndAtomicsModule module;
            const bool forced = !strcmp(mode, "force");
            const bool emulate = forced || (!native && !strcmp(mode, "auto"));
            VkPhysicalDeviceFeatures f{};
            features(phys, &f);
            module.on_get_features(phys, &f);
            check(f.vertexPipelineStoresAndAtomics == (native || emulate),
                  "auto supplies missing vertex atomics while skip preserves native support");
            VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            features(phys, &f2.features);
            module.on_post_get_features2(phys, &f2, nullptr);
            check(f2.features.vertexPipelineStoresAndAtomics == (native || emulate),
                  "features2 agrees with legacy feature query");
            VkPhysicalDeviceProperties p{};
            p.limits.maxPerStageDescriptorStorageBuffers = 1;
            p.limits.maxPerStageDescriptorStorageImages = 2;
            module.on_get_properties(phys, &p);
            check(p.limits.maxPerStageDescriptorStorageBuffers == 1u &&
                      p.limits.maxPerStageDescriptorStorageImages == 2u,
                  "descriptor limits remain native even when emulation is forced");
            VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            p2.properties.limits.maxPerStageDescriptorStorageBuffers = 1;
            p2.properties.limits.maxPerStageDescriptorStorageImages = 2;
            module.on_post_get_properties2(phys, &p2, nullptr);
            check(p2.properties.limits.maxPerStageDescriptorStorageBuffers ==
                          p.limits.maxPerStageDescriptorStorageBuffers &&
                      p2.properties.limits.maxPerStageDescriptorStorageImages ==
                          p.limits.maxPerStageDescriptorStorageImages,
                  "properties2 agrees with legacy limits");
            VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
            f.vertexPipelineStoresAndAtomics = VK_TRUE;
            f2.features.vertexPipelineStoresAndAtomics = VK_TRUE;
            ci.pNext = &f2;
            std::vector<const char *> exts;
            void *user = nullptr;
            module.on_pre_create_device(phys, &ci, &f, exts, user);
            check(f.vertexPipelineStoresAndAtomics == !emulate &&
                      f2.features.vertexPipelineStoresAndAtomics == !emulate,
                  "auto and force remove only the emulated feature from native device creation");
            auto device = (VkDevice)(uintptr_t)0x9000;
            // Selection belongs to the create call, not a later environment read.
            setenv(feature_env, emulate ? "skip" : "force", 1);
            module.on_post_create_device(phys, device, VK_SUCCESS, user);
            check(VertexStoreEmulator::get().enabled(device) == emulate &&
                      module.is_device_native(device) == (native && !emulate),
                  "device dispatch and compute/replay activation keep the create-time decision");
            module.on_destroy_device(device);
            check(!VertexStoreEmulator::get().enabled(device),
                  "device destruction removes automatic vertex emulation state");
            setenv(feature_env, mode, 1);
        }
    }
    setenv(module_env, "force", 1);
    setenv(feature_env, "skip", 1);
    VertexPipelineStoresAndAtomicsModule module;
    VkPhysicalDeviceFeatures f{};
    module.on_get_features(phys, &f);
    check(!f.vertexPipelineStoresAndAtomics, "feature skip overrides legacy module force");
    setenv(feature_env, "auto", 1);
    module.on_get_features(phys, &f);
    check(f.vertexPipelineStoresAndAtomics, "explicit legacy module force remains available");
    setenv(module_env, "auto", 1);
    unsetenv(feature_env);
    unsetenv(module_env);
    native_atomics = false;
    VertexPipelineStoresAndAtomicsModule automatic;
    f = {};
    automatic.on_get_features(phys, &f);
    check(f.vertexPipelineStoresAndAtomics,
          "unset environment automatically emulates missing driver vertex atomics");
    VkDeviceCreateInfo unrequested{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    void *unrequested_user = nullptr;
    std::vector<const char *> exts;
    f = {};
    automatic.on_pre_create_device(phys, &unrequested, &f, exts, unrequested_user);
    auto disabled = (VkDevice)(uintptr_t)0x9001;
    automatic.on_post_create_device(phys, disabled, VK_SUCCESS, unrequested_user);
    check(!VertexStoreEmulator::get().enabled(disabled),
          "a device that did not enable vertex atomics does not allocate emulation resources");
    automatic.on_destroy_device(disabled);
    setenv(module_env, "skip", 1);
    f = {};
    automatic.on_get_features(phys, &f);
    check(!f.vertexPipelineStoresAndAtomics, "legacy module skip disables automatic fallback");
    setenv(module_env, "auto", 1);
    native_atomics = true;
}

static void test_vertex_resource_lifetimes(VkPhysicalDevice physical) {
    auto device = (VkDevice)(uintptr_t)0xa000;
    auto &emulator = VertexStoreEmulator::get();
    auto sets_before = cloned_sets_created - cloned_sets_destroyed;
    auto layouts_before = cloned_layouts_created - cloned_layouts_destroyed;
    emulator.enable(device, physical);
    VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    auto set = (VkDescriptorSetLayout)0x123;
    auto layout = (VkPipelineLayout)0x456;
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &set;
    for (unsigned i = 0; i < 2000; ++i) {
        emulator.set_layout(device, set, &set_info);
        emulator.layout(device, layout, &layout_info);
        emulator.erase_set_layout(device, set);
        emulator.erase_layout(device, layout);
    }
    auto stats = emulator.memory_stats(device);
    check(stats.cloned_set_layouts == 0 && stats.cloned_pipeline_layouts == 0 &&
              cloned_sets_created - cloned_sets_destroyed == sets_before + 1 &&
              cloned_layouts_created - cloned_layouts_destroyed == layouts_before,
          "2000 layout create/destroy cycles retire native clones before device destruction");
    emulator.set_layout(device, set, &set_info);
    emulator.layout(device, layout, &layout_info);
    auto command = (VkCommandBuffer)(uintptr_t)0x789;
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = (VkCommandPool)0x234;
    allocate.commandBufferCount = 1;
    emulator.allocate_commands(device, &allocate, &command);
    auto descriptor = (VkDescriptorSet)0x345;
    emulator.bind_sets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &descriptor, 0,
                       nullptr);
    emulator.erase_layout(device, layout);
    emulator.erase_set_layout(device, set);
    stats = emulator.memory_stats(device);
    check(stats.cloned_set_layouts == 1 && stats.cloned_pipeline_layouts == 1,
          "recorded descriptor state keeps destroyed application layouts alive for later emulated "
          "draws");
    emulator.clear_commands(command);
    stats = emulator.memory_stats(device);
    check(stats.cloned_set_layouts == 0 && stats.cloned_pipeline_layouts == 0,
          "command reset releases the final cloned layout references");
    emulator.destroy_pool(device, allocate.commandPool);
    check(emulator.memory_stats(device).command_buffers == 0,
          "command pool destruction removes implicit command-buffer metadata");
    emulator.destroy_device(device);
    check(cloned_sets_created - cloned_sets_destroyed == sets_before &&
              cloned_layouts_created - cloned_layouts_destroyed == layouts_before,
          "device cleanup destroys each remaining native layout once");
}

static void test_addresses(VkPhysicalDevice phys, VkDevice device) {
    BufferDeviceAddressModule module;
    std::vector<VkExtensionProperties> exts;
    module.on_enumerate_device_extensions(phys, exts);
    check(exts.empty(), "no address extension injected on unsupported driver");
    VkExtensionProperties ext{};
    strcpy(ext.extensionName, VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
    exts.push_back(ext);
    module.on_enumerate_device_extensions(phys, exts);
    check(exts.size() == 1 && !strcmp(exts[0].extensionName, ext.extensionName),
          "native EXT address extension preserved without injecting KHR");
    std::vector<const char *> enabled{VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME};
    void *user = nullptr;
    module.on_pre_create_device(phys, nullptr, nullptr, enabled, user);
    check(enabled.size() == 1, "unsupported extension request is not silently stripped");
    VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    info.buffer = (VkBuffer)(uintptr_t)0x5678;
    VkDeviceAddress out = 1;
    module.on_get_buffer_device_address(device, &info, out);
    check(out == 0, "missing address implementation returns zero, never a buffer handle");
    native_address = true;
    LayerManager::get().init_device_dispatch_table(device);
    module.on_get_buffer_device_address(device, &info, out);
    check(out == 0x12340000, "real address implementation is forwarded");
    native_address = false;
}

static void test_dispatch_reuse(VkDevice device) {
    auto &manager = LayerManager::get();
    const VkDevice other = (VkDevice)(uintptr_t)0x2000;
    manager.init_device_dispatch_table(other);
    // Cache the secondary device on another thread, then reuse its handle.
    std::mutex mutex;
    std::condition_variable cv;
    int phase = 0;
    bool before = false, after = false, primary_after = false;
    std::thread reader([&] {
        std::unique_lock<std::mutex> lock(mutex);
        before = manager.get_dispatch_table(other).CmdDraw == draw_a;
        phase = 1;
        cv.notify_one();
        cv.wait(lock, [&] { return phase == 2; });
        after = manager.get_dispatch_table(other).CmdDraw == draw_b;
        primary_after = manager.get_dispatch_table(device).CmdDraw == draw_b;
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&] { return phase == 1; });
        manager.remove_device_dispatch_table(other);
        manager.remove_device_dispatch_table(device);
        second_dispatch = true;
        manager.init_device_dispatch_table(device);
        manager.init_device_dispatch_table(other);
        phase = 2;
    }
    cv.notify_one();
    reader.join();
    check(before && after, "cross-thread secondary dispatch cache refreshes on handle reuse");
    check(primary_after, "primary device replacement uses the new dispatch table");
    second_dispatch = false;
    manager.init_device_dispatch_table(other);
    const auto &retained = manager.get_dispatch_table(device);
    const auto &different = manager.get_dispatch_table(other);
    check(retained.CmdDraw == draw_b && different.CmdDraw == draw_a,
          "looking up another device preserves an outstanding table reference");
    manager.remove_device_dispatch_table(other);
    manager.remove_device_dispatch_table(device);
}

static void test_vertex_lowering() {
    const char *text = R"(
OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Vertex %main "main" %vertex %position
OpDecorate %vertex BuiltIn VertexIndex
OpDecorate %position BuiltIn Position
OpDecorate %data DescriptorSet 0
OpDecorate %data Binding 0
OpDecorate %Block BufferBlock
OpMemberDecorate %Block 0 Offset 0
%void = OpTypeVoid
%fn = OpTypeFunction %void
%uint = OpTypeInt 32 0
%int = OpTypeInt 32 1
%float = OpTypeFloat 32
%v4 = OpTypeVector %float 4
%Block = OpTypeStruct %uint
%ptrblock = OpTypePointer Uniform %Block
%ptruint = OpTypePointer Uniform %uint
%ptrin = OpTypePointer Input %int
%ptrout = OpTypePointer Output %v4
%zero = OpConstant %uint 0
%one = OpConstant %uint 1
%fzero = OpConstant %float 0
%fone = OpConstant %float 1
%data = OpVariable %ptrblock Uniform
%vertex = OpVariable %ptrin Input
%position = OpVariable %ptrout Output
%main = OpFunction %void None %fn
%label = OpLabel
%counter = OpAccessChain %ptruint %data %zero
%old = OpAtomicIAdd %uint %counter %one %zero %one
%x = OpConvertUToF %float %old
%pos = OpCompositeConstruct %v4 %x %fzero %fzero %fone
OpStore %position %pos
OpReturn
OpFunctionEnd
)";
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_0);
    std::vector<uint32_t> words;
    check(tools.Assemble(text, &words), "assemble vertex atomic-return fixture");
    vkfix::VertexStoreProgram program;
    const bool ok = vkfix::lower_vertex_stores(words, "main", 1, {}, program);
    if (!ok)
        std::printf("Lowering diagnostic: %s\n", program.error.c_str());
    check(ok && program.output_words == 4, "lower atomic vertex invocation and position replay");
    check(ok && tools.Validate(program.compute) && tools.Validate(program.replay),
          "both generated stages validate for Vulkan 1.0");
    auto version13 = words;
    version13[1] = 0x10300;
    check(vkfix::lower_vertex_stores(version13, "main", 1, {}, program),
          "lower SPIR-V 1.3 vertex shader for Vulkan 1.1 drivers");
    std::string modern = text;
    auto replace_modern = [&](const std::string &from, const std::string &to) {
        modern.replace(modern.find(from), from.size(), to);
    };
    replace_modern("OpEntryPoint Vertex %main \"main\" %vertex %position",
                   "OpEntryPoint Vertex %main \"main\" %vertex %position %data");
    replace_modern("OpDecorate %Block BufferBlock", "OpDecorate %Block Block");
    replace_modern("%ptrblock = OpTypePointer Uniform %Block",
                   "%ptrblock = OpTypePointer StorageBuffer %Block");
    replace_modern("%ptruint = OpTypePointer Uniform %uint",
                   "%ptruint = OpTypePointer StorageBuffer %uint");
    replace_modern("%data = OpVariable %ptrblock Uniform",
                   "%data = OpVariable %ptrblock StorageBuffer");
    spvtools::SpirvTools tools12(SPV_ENV_VULKAN_1_2);
    std::vector<uint32_t> modern_words;
    const bool modern_ok = tools12.Assemble(modern, &modern_words) &&
                           vkfix::lower_vertex_stores(modern_words, "main", 1, {}, program);
    if (!modern_ok)
        std::printf("SPIR-V 1.5 diagnostic: %s\n", program.error.c_str());
    check(modern_ok, "lower SPIR-V 1.5 with complete entry-point resource interfaces");
    check(vkfix::vertex_has_storage_effects(words), "detect vertex storage atomic operations");
    std::string readonly = text;
    const std::string atomic = "%old = OpAtomicIAdd %uint %counter %one %zero %one";
    readonly.replace(readonly.find(atomic), atomic.size(), "%old = OpLoad %uint %counter");
    std::vector<uint32_t> read_words;
    check(tools.Assemble(readonly, &read_words) && !vkfix::vertex_has_storage_effects(read_words),
          "read-only SSBO vertex shader stays on the native path");
    std::string block = text;
    auto replace = [&](const std::string &from, const std::string &to) {
        block.replace(block.find(from), from.size(), to);
    };
    replace("OpDecorate %position BuiltIn Position",
            "OpDecorate %PerVertex Block\nOpMemberDecorate %PerVertex 0 BuiltIn Position\n"
            "OpMemberDecorate %PerVertex 1 BuiltIn ClipDistance");
    replace("%v4 = OpTypeVector %float 4",
            "%v4 = OpTypeVector %float 4\n%length = OpConstant %uint 1\n"
            "%clip = OpTypeArray %float %length\n%PerVertex = OpTypeStruct %v4 %clip");
    replace("%ptrout = OpTypePointer Output %v4",
            "%ptrout = OpTypePointer Output %PerVertex\n%ptrposition = OpTypePointer Output %v4");
    replace("OpStore %position %pos",
            "%positionMember = OpAccessChain %ptrposition %position %zero\n"
            "OpStore %positionMember %pos");
    std::vector<uint32_t> block_words;
    const bool block_ok = tools.Assemble(block, &block_words) &&
                          vkfix::lower_vertex_stores(block_words, "main", 1, {}, program);
    if (!block_ok)
        std::printf("Block lowering diagnostic: %s\n", program.error.c_str());
    check(block_ok && program.output_words == 4,
          "gl_PerVertex replay never writes previously unwritten clip distances");
    words.resize(3);
    check(!vkfix::lower_vertex_stores(words, "main", 1, {}, program),
          "reject truncated shader before driver submission");
}

int main() {
    setenv("VULKAN_FIX_LOG_PATH", "/dev/null", 1);
    const auto phys = (VkPhysicalDevice)(uintptr_t)0x100;
    const auto device = (VkDevice)(uintptr_t)0x1000;
    test_atomics(phys);
    test_vertex_resource_lifetimes(phys);
    test_addresses(phys, device);
    test_dispatch_reuse(device);
    test_vertex_lowering();
    run_timeline_regressions(check);
    run_push_descriptor_regressions(check);
    run_mali_tiler_regressions(check);
    run_dynamic_rendering_lifetime_regressions(check);
    std::printf("Regression failures: %d\n", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
