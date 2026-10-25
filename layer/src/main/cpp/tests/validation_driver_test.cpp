#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <vector>
#include <vulkan/vulkan.h>
#define REQUIRE(x)                                                             \
  do {                                                                         \
    if (!(x)) {                                                                \
      printf("FAIL line %d: %s\n", __LINE__, #x);                              \
      return 1;                                                                \
    }                                                                          \
  } while (0)
int main() {
  const char *block = getenv("VULKAN_FIX_VALIDATION_BLOCK_INVALID");
  REQUIRE(block && !strcmp(block, "1"));
  auto lib =
      dlopen("./libvulkan_fix_validation_driver.so", RTLD_NOW | RTLD_LOCAL);
  REQUIRE(lib);
  auto gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
  auto gdpa = (PFN_vkGetDeviceProcAddr)dlsym(lib, "vkGetDeviceProcAddr");
  REQUIRE(gipa && gdpa);
  VkInstance instances[2]{};
  uint32_t family_indices[2]{};
  VkDevice devices[2]{};
  for (unsigned n = 0; n < 2; ++n) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    auto create = (PFN_vkCreateInstance)gipa(nullptr, "vkCreateInstance");
    REQUIRE(create && create(&ci, nullptr, &instances[n]) == VK_SUCCESS);
    auto enumerate = (PFN_vkEnumeratePhysicalDevices)gipa(
        instances[n], "vkEnumeratePhysicalDevices");
    uint32_t count = 0;
    REQUIRE(enumerate(instances[n], &count, nullptr) == VK_SUCCESS && count);
    std::vector<VkPhysicalDevice> physical(count);
    REQUIRE(enumerate(instances[n], &count, physical.data()) == VK_SUCCESS);
    auto family = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa(
        instances[n], "vkGetPhysicalDeviceQueueFamilyProperties");
    family(physical[0], &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    family(physical[0], &count, families.data());
    uint32_t selected = 0;
    while (selected < count && !families[selected].queueCount)
      ++selected;
    REQUIRE(selected < count);
    family_indices[n] = selected;
    float priority = 1;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = selected;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    auto cd = (PFN_vkCreateDevice)gipa(instances[n], "vkCreateDevice");
    REQUIRE(cd && cd(physical[0], &di, nullptr, &devices[n]) == VK_SUCCESS);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = 16;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer buffer{};
    auto cb = (PFN_vkCreateBuffer)gdpa(devices[n], "vkCreateBuffer");
    REQUIRE(cb && cb(devices[n], &bi, nullptr, &buffer) == VK_SUCCESS);
    ((PFN_vkDestroyBuffer)gdpa(devices[n], "vkDestroyBuffer"))(devices[n],
                                                               buffer, nullptr);
  }
  ((PFN_vkDestroyDevice)gdpa(devices[0], "vkDestroyDevice"))(devices[0],
                                                             nullptr);
  ((PFN_vkDestroyInstance)gipa(instances[0], "vkDestroyInstance"))(instances[0],
                                                                   nullptr);
  VkBufferCreateInfo invalid{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  invalid.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  VkBuffer buffer{};
  auto cb = (PFN_vkCreateBuffer)gdpa(devices[1], "vkCreateBuffer");
  auto result = cb(devices[1], &invalid, nullptr, &buffer);
  REQUIRE(result == VK_ERROR_VALIDATION_FAILED_EXT);
  printf("PASS: two instances/devices stay isolated, intentional zero-size "
         "buffer is blocked by "
         "VVL (%d)\n",
         result);
  if (getenv("VK_VALIDATION_VALIDATE_SYNC") &&
      !strcmp(getenv("VK_VALIDATION_VALIDATE_SYNC"), "1")) {
    auto messages = (uint64_t(*)(const char *))dlsym(
        lib, "vulkanfixValidationMessageCount");
    REQUIRE(messages);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = 64;
    bi.usage =
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    REQUIRE(cb(devices[1], &bi, nullptr, &buffer) == VK_SUCCESS);
    VkMemoryRequirements req{};
    ((PFN_vkGetBufferMemoryRequirements)gdpa(
        devices[1], "vkGetBufferMemoryRequirements"))(devices[1], buffer, &req);
    uint32_t memory_type = 0;
    while (memory_type < 32 && !(req.memoryTypeBits & (1u << memory_type)))
      ++memory_type;
    REQUIRE(memory_type < 32);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memory_type;
    VkDeviceMemory memory{};
    REQUIRE(((PFN_vkAllocateMemory)gdpa(devices[1], "vkAllocateMemory"))(
                devices[1], &ai, nullptr, &memory) == VK_SUCCESS);
    REQUIRE(((PFN_vkBindBufferMemory)gdpa(devices[1], "vkBindBufferMemory"))(
                devices[1], buffer, memory, 0) == VK_SUCCESS);
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.queueFamilyIndex = family_indices[1];
    VkCommandPool pool{};
    REQUIRE(((PFN_vkCreateCommandPool)gdpa(devices[1], "vkCreateCommandPool"))(
                devices[1], &pi, nullptr, &pool) == VK_SUCCESS);
    VkCommandBufferAllocateInfo ca{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ca.commandPool = pool;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = 1;
    VkCommandBuffer cmd{};
    REQUIRE(((PFN_vkAllocateCommandBuffers)gdpa(
                devices[1], "vkAllocateCommandBuffers"))(devices[1], &ca,
                                                         &cmd) == VK_SUCCESS);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    REQUIRE(
        ((PFN_vkBeginCommandBuffer)gdpa(devices[1], "vkBeginCommandBuffer"))(
            cmd, &begin) == VK_SUCCESS);
    ((PFN_vkCmdFillBuffer)gdpa(devices[1], "vkCmdFillBuffer"))(cmd, buffer, 0,
                                                               64, 0x12345678);
    const uint64_t before = messages("SYNC-HAZARD");
    VkBufferCopy copy{0, 16, 16};
    // Deliberately missing a dependency. With block-invalid=1 the validator
    // rejects the command; this command buffer is never submitted to a GPU.
    ((PFN_vkCmdCopyBuffer)gdpa(devices[1], "vkCmdCopyBuffer"))(
        cmd, buffer, buffer, 1, &copy);
    REQUIRE(messages("SYNC-HAZARD") > before);
    REQUIRE(((PFN_vkEndCommandBuffer)gdpa(devices[1], "vkEndCommandBuffer"))(
                cmd) == VK_SUCCESS);
    ((PFN_vkDestroyCommandPool)gdpa(devices[1], "vkDestroyCommandPool"))(
        devices[1], pool, nullptr);
    ((PFN_vkDestroyBuffer)gdpa(devices[1], "vkDestroyBuffer"))(devices[1],
                                                               buffer, nullptr);
    ((PFN_vkFreeMemory)gdpa(devices[1], "vkFreeMemory"))(devices[1], memory,
                                                         nullptr);
    printf("PASS: synchronization validation catches an intentional transfer "
           "hazard before GPU "
           "submission\n");
  }
  ((PFN_vkDestroyDevice)gdpa(devices[1], "vkDestroyDevice"))(devices[1],
                                                             nullptr);
  ((PFN_vkDestroyInstance)gipa(instances[1], "vkDestroyInstance"))(instances[1],
                                                                   nullptr);
  return 0;
}
