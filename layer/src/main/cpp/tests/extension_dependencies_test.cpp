#include "core/extension_dependencies.h"
#include "core/native_instance_state.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

namespace {
unsigned failures = 0, checks = 0;
void check(bool condition, const char *name) {
  ++checks;
  printf("%s: %s\n", condition ? "PASS" : "FAIL", name);
  if (!condition)
    ++failures;
}
bool has(const vkfix::ExtensionDependencyPlan &plan, const char *name) {
  for (auto entry : plan.names)
    if (!strcmp(entry, name))
      return true;
  return false;
}
vkfix::ExtensionEnvironment legacy() {
  vkfix::ExtensionEnvironment environment;
  environment.enabled_instance = {"VK_KHR_get_physical_device_properties2",
                                  "VK_KHR_device_group_creation"};
  return environment;
}
} // namespace
int main() {
  using namespace vkfix;
  auto environment = legacy();
  environment.available = {"VK_KHR_8bit_storage",
                           "VK_KHR_storage_buffer_storage_class"};
  auto plan =
      close_extension_dependencies({"VK_KHR_8bit_storage"}, environment);
  check(plan.result == VK_SUCCESS && plan.names.size() == 2 &&
            has(plan, "VK_KHR_storage_buffer_storage_class"),
        "Vulkan 1.0 8-bit storage enables its storage-class dependency");
  check(!has(plan, "VK_KHR_get_physical_device_properties2"),
        "instance dependencies never leak into the device extension array");
  environment.enabled_instance.clear();
  plan = close_extension_dependencies({"VK_KHR_8bit_storage"}, environment);
  check(plan.result == VK_ERROR_EXTENSION_NOT_PRESENT && plan.names.empty() &&
            plan.missing.find("VK_KHR_get_physical_device_properties2") !=
                std::string::npos,
        "missing instance enablement rejects the transaction without partial "
        "additions");
  environment = legacy();
  environment.available = {"VK_KHR_8bit_storage"};
  plan = close_extension_dependencies({"VK_KHR_8bit_storage"}, environment);
  check(plan.result == VK_ERROR_EXTENSION_NOT_PRESENT,
        "advertising a child alone does not imply native dependency support");
  environment.device_api = VK_API_VERSION_1_1;
  environment.instance_api = VK_API_VERSION_1_1;
  environment.enabled_instance.clear();
  plan = close_extension_dependencies({"VK_KHR_8bit_storage"}, environment);
  check(plan.result == VK_SUCCESS && plan.names.size() == 1,
        "a core-version alternative avoids unnecessary extension names");

  environment = legacy();
  environment.available = {"VK_KHR_sampler_ycbcr_conversion",
                           "VK_KHR_maintenance1", "VK_KHR_bind_memory2",
                           "VK_KHR_get_memory_requirements2"};
  plan = close_extension_dependencies({"VK_KHR_sampler_ycbcr_conversion"},
                                      environment);
  check(plan.result == VK_SUCCESS && plan.names.size() == 4,
        "YCbCr enables every required Vulkan 1.0 memory and maintenance "
        "extension");
  environment = legacy();
  environment.available = {"VK_KHR_imageless_framebuffer",
                           "VK_KHR_maintenance2", "VK_KHR_image_format_list"};
  plan = close_extension_dependencies({"VK_KHR_imageless_framebuffer"},
                                      environment);
  check(plan.result == VK_SUCCESS && plan.names.size() == 3,
        "nested AND/OR dependencies close the Vulkan 1.0 imageless path");
  environment.instance_api = environment.device_api = VK_API_VERSION_1_1;
  environment.enabled_instance.clear();
  environment.available.erase("VK_KHR_maintenance2");
  plan = close_extension_dependencies({"VK_KHR_imageless_framebuffer"},
                                      environment);
  check(plan.result == VK_SUCCESS && plan.names.size() == 2 &&
            has(plan, "VK_KHR_image_format_list"),
        "Vulkan 1.1 imageless still requires image-format-list");
  environment.instance_api = environment.device_api = VK_API_VERSION_1_2;
  environment.available.erase("VK_KHR_image_format_list");
  plan = close_extension_dependencies({"VK_KHR_imageless_framebuffer"},
                                      environment);
  check(plan.result == VK_SUCCESS && plan.names.size() == 1,
        "Vulkan 1.2 satisfies the full imageless dependency expression");

  environment = legacy();
  environment.available = {
      "VK_KHR_dynamic_rendering", "VK_KHR_depth_stencil_resolve",
      "VK_KHR_create_renderpass2", "VK_KHR_multiview", "VK_KHR_maintenance2"};
  plan =
      close_extension_dependencies({"VK_KHR_dynamic_rendering"}, environment);
  check(plan.result == VK_SUCCESS && plan.names.size() == 5,
        "recursive rendering dependencies include renderpass2, multiview and "
        "maintenance");
  environment.device_api = VK_API_VERSION_1_2;
  environment.available = {"VK_KHR_dynamic_rendering"};
  plan =
      close_extension_dependencies({"VK_KHR_dynamic_rendering"}, environment);
  check(plan.result == VK_SUCCESS && plan.names.size() == 1,
        "a successful OR branch discards additions from the failed branch");
  environment.available = {"VK_KHR_buffer_device_address",
                           "VK_KHR_device_group"};
  environment.device_api = VK_API_VERSION_1_0;
  plan = close_extension_dependencies({"VK_KHR_buffer_device_address"},
                                      environment);
  check(plan.result == VK_SUCCESS && plan.names.size() == 2,
        "device-group's instance promotion dependency is satisfied by "
        "enablement");
  environment.enabled_instance.erase("VK_KHR_device_group_creation");
  plan = close_extension_dependencies({"VK_KHR_buffer_device_address"},
                                      environment);
  check(plan.result == VK_ERROR_EXTENSION_NOT_PRESENT,
        "device-group creation cannot be enabled retroactively on a device");

  environment = legacy();
  environment.available = {"VK_KHR_8bit_storage",
                           "VK_KHR_storage_buffer_storage_class",
                           "VK_FUTURE_native_extension"};
  const std::vector<const char *> requests{"VK_KHR_8bit_storage",
                                           "VK_KHR_8bit_storage",
                                           "VK_FUTURE_native_extension"};
  auto original = environment;
  plan = close_extension_dependencies(requests, environment);
  check(plan.result == VK_SUCCESS && plan.names.size() == 3 &&
            requests.size() == 3 && environment.available == original.available,
        "duplicate requests are removed without modifying caller inputs or "
        "native support");
  check(has(plan, "VK_FUTURE_native_extension"),
        "an unknown but natively enumerated extension remains available to its "
        "driver");
  plan = close_extension_dependencies({"VK_FUTURE_missing_extension"},
                                      environment);
  check(plan.result == VK_ERROR_EXTENSION_NOT_PRESENT,
        "an unrecognized request is not silently accepted without native "
        "support");

  environment = {};
  environment.instance_creation = true;
  environment.available = {"VK_KHR_get_physical_device_properties2",
                           "VK_KHR_external_memory_capabilities",
                           "VK_KHR_external_fence_capabilities",
                           "VK_KHR_external_semaphore_capabilities",
                           "VK_KHR_device_group_creation"};
  auto promoted = promoted_instance_extensions(VK_API_VERSION_1_2, environment);
  plan = close_extension_dependencies(promoted, environment);
  check(plan.result == VK_SUCCESS && plan.names.size() == 5,
        "all five promoted instance extensions are enabled for a Vulkan 1.0 "
        "loader");
  check(promoted_instance_extensions(VK_API_VERSION_1_0, environment).empty(),
        "a Vulkan 1.0 application does not implicitly enable later core "
        "instance behavior");
  environment.instance_api = VK_API_VERSION_1_1;
  check(promoted_instance_extensions(VK_API_VERSION_1_2, environment).empty(),
        "core instance support avoids redundant promoted names");
  environment = {};
  environment.instance_creation = true;
  environment.available = {"VK_KHR_android_surface", "VK_KHR_surface"};
  plan = close_extension_dependencies({"VK_KHR_android_surface"}, environment);
  check(plan.result == VK_SUCCESS && plan.names.size() == 2,
        "instance creation closes platform-surface dependencies");
  environment.available.insert("VK_KHR_8bit_storage");
  plan = close_extension_dependencies({"VK_KHR_8bit_storage"}, environment);
  check(plan.result == VK_ERROR_EXTENSION_NOT_PRESENT,
        "a device extension cannot enter the native instance extension array");

  auto a = (VkInstance)(uintptr_t)0x101, b = (VkInstance)(uintptr_t)0x102;
  auto pa = (VkPhysicalDevice)(uintptr_t)0x201,
       pb = (VkPhysicalDevice)(uintptr_t)0x202;
  remember_native_instance(a, VK_API_VERSION_1_0, VK_API_VERSION_1_0,
                           {"VK_KHR_get_physical_device_properties2"});
  remember_native_instance(b, VK_API_VERSION_1_2, VK_API_VERSION_1_1, {});
  remember_physical_devices(a, 1, &pa);
  remember_physical_devices(b, 1, &pb);
  auto stateA = native_state_for_physical(pa),
       stateB = native_state_for_physical(pb);
  check(stateA && stateB && stateA->enabled_extensions.size() == 1 &&
            stateB->enabled_extensions.empty() &&
            stateB->target_api == VK_API_VERSION_1_2 &&
            stateB->instance_api == VK_API_VERSION_1_1,
        "native instance version and target device API remain distinct and "
        "isolated");
  forget_native_instance(a);
  check(
      !native_state_for_physical(pa) && native_state_for_physical(pb) &&
          native_instance_for_physical(pb) == b,
      "destroying one instance removes only its physical-device associations");
  remember_native_instance(a, VK_API_VERSION_1_1, VK_API_VERSION_1_1, {});
  remember_physical_devices(a, 1, &pa);
  check(native_state_for_physical(pa)->enabled_extensions.empty() &&
            stateA->enabled_extensions.size() == 1,
        "reused instance and physical handles do not inherit old enablement or "
        "invalidate snapshots");
  std::atomic<bool> isolated{true};
  std::vector<std::thread> workers;
  for (uintptr_t i = 0; i < 8; ++i)
    workers.emplace_back([&, i] {
      const auto instance = (VkInstance)(0x1000 + i);
      const auto physical = (VkPhysicalDevice)(0x2000 + i);
      for (unsigned n = 0; n < 100; ++n) {
        remember_native_instance(instance, VK_API_VERSION_1_0,
                                 VK_API_VERSION_1_0, {i & 1 ? "odd" : "even"});
        remember_physical_devices(instance, 1, &physical);
        auto state = native_state_for_physical(physical);
        if (!state || !state->enabled_extensions.count(i & 1 ? "odd" : "even"))
          isolated = false;
        auto local = legacy();
        local.available = {"VK_KHR_8bit_storage",
                           "VK_KHR_storage_buffer_storage_class"};
        if (close_extension_dependencies({"VK_KHR_8bit_storage"}, local)
                .result != VK_SUCCESS)
          isolated = false;
        forget_native_instance(instance);
        if (native_state_for_physical(physical))
          isolated = false;
      }
    });
  for (auto &worker : workers)
    worker.join();
  check(isolated,
        "concurrent instance lifecycles and dependency plans remain isolated");
  forget_native_instance(a);
  forget_native_instance(b);
  printf("Extension dependency checks: %u, failures: %u\n", checks, failures);
  return failures ? 1 : 0;
}
