#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#include <vulkan/vulkan.h>
static unsigned failures = 0;
static void check(bool ok, const char *name) {
    printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok)
        ++failures;
}
static bool success(VkResult result, const char *name) {
    if (result == VK_SUCCESS)
        return true;
    printf("FAIL: %s (%d)\n", name, result);
    ++failures;
    return false;
}
struct Resources {
    VkDevice device{};
    VkQueue queues[2]{};
    unsigned queue_count = 1;
    VkSemaphore timeline{}, binary{};
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    VkCommandPool pool{};
    VkCommandBuffer commands[4]{};
    void *mapped{};
    bool coherent = false;
};
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setenv("VULKAN_FIX_EMULATE_TIMELINE_SEMAPHORE", "force", 1);
    setenv("VULKAN_FIX_DISABLE_PIPELINE_CACHE", "1", 1);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance{};
    if (!success(vkCreateInstance(&ici, nullptr, &instance), "create timeline instance"))
        return 1;
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> physicals(count);
    if (!count || !success(vkEnumeratePhysicalDevices(instance, &count, physicals.data()),
                           "enumerate timeline device"))
        return 1;
    auto physical = physicals.front();
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> properties(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, properties.data());
    uint32_t family = 0;
    while (family < count &&
           !(properties[family].queueFlags &
             (VK_QUEUE_TRANSFER_BIT | VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)))
        ++family;
    if (family == count)
        return 1;
    auto create = [&](Resources &r, unsigned requested) {
        float priorities[]{1, 1};
        r.queue_count = std::min(requested, properties[family].queueCount);
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = family;
        qci.queueCount = r.queue_count;
        qci.pQueuePriorities = priorities;
        VkPhysicalDeviceVulkan12Features feature{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        feature.timelineSemaphore = VK_TRUE;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &feature};
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        if (!success(vkCreateDevice(physical, &dci, nullptr, &r.device),
                     "create forced timeline device"))
            return false;
        for (unsigned i = 0; i < r.queue_count; ++i)
            vkGetDeviceQueue(r.device, family, i, &r.queues[i]);
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkExportSemaphoreCreateInfo parent{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO, &type};
        auto saved_parent = parent;
        auto saved_type = type;
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &parent};
        auto semaphore_result = vkCreateSemaphore(r.device, &sci, nullptr, &r.timeline);
        if (!success(semaphore_result, "create emulated timeline")) {
            VkSemaphore probe{};
            parent.pNext = nullptr;
            printf("Diagnostic binary export-zero result: %d\n",
                   vkCreateSemaphore(r.device, &sci, nullptr, &probe));
            if (probe)
                vkDestroySemaphore(r.device, probe, nullptr);
            probe = VK_NULL_HANDLE;
            sci.pNext = &type;
            printf("Diagnostic timeline type-only result: %d\n",
                   vkCreateSemaphore(r.device, &sci, nullptr, &probe));
            if (probe)
                vkDestroySemaphore(r.device, probe, nullptr);
            probe = VK_NULL_HANDLE;
            sci.pNext = nullptr;
            printf("Diagnostic binary plain result: %d\n",
                   vkCreateSemaphore(r.device, &sci, nullptr, &probe));
            if (probe)
                vkDestroySemaphore(r.device, probe, nullptr);
            return false;
        }
        check(!memcmp(&parent, &saved_parent, sizeof(parent)) &&
                  !memcmp(&type, &saved_type, sizeof(type)),
              "real semaphore creation preserves a type node behind another input node");
        sci.pNext = nullptr;
        if (!success(vkCreateSemaphore(r.device, &sci, nullptr, &r.binary),
                     "create native binary semaphore"))
            return false;
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = 512;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (!success(vkCreateBuffer(r.device, &bci, nullptr, &r.buffer), "create transfer buffer"))
            return false;
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(r.device, r.buffer, &req);
        VkPhysicalDeviceMemoryProperties mem{};
        vkGetPhysicalDeviceMemoryProperties(physical, &mem);
        uint32_t type_index = 0;
        for (; type_index < mem.memoryTypeCount; ++type_index)
            if ((req.memoryTypeBits & (1u << type_index)) &&
                (mem.memoryTypes[type_index].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
                break;
        if (type_index == mem.memoryTypeCount)
            return false;
        r.coherent =
            mem.memoryTypes[type_index].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = type_index;
        if (!success(vkAllocateMemory(r.device, &mai, nullptr, &r.memory),
                     "allocate transfer memory"))
            return false;
        vkBindBufferMemory(r.device, r.buffer, r.memory, 0);
        vkMapMemory(r.device, r.memory, 0, VK_WHOLE_SIZE, 0, &r.mapped);
        VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpci.queueFamilyIndex = family;
        vkCreateCommandPool(r.device, &cpci, nullptr, &r.pool);
        VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbai.commandPool = r.pool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 4;
        return success(vkAllocateCommandBuffers(r.device, &cbai, r.commands),
                       "allocate transfer command buffers");
    };
    Resources a, b;
    if (!create(a, 2) || !create(b, 1))
        return 1;
    printf("Primary device queues: %u\n", a.queue_count);
    auto begin = [&](VkCommandBuffer cmd) {
        VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        return success(vkBeginCommandBuffer(cmd, &info), "begin transfer command");
    };
    auto host_barrier = [&](VkCommandBuffer cmd) {
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                             &barrier, 0, nullptr, 0, nullptr);
    };
    begin(a.commands[0]);
    vkCmdFillBuffer(a.commands[0], a.buffer, 0, 128, 0x12345678);
    host_barrier(a.commands[0]);
    vkEndCommandBuffer(a.commands[0]);
    VkTimelineSemaphoreSubmitInfo first_timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    uint64_t one = 1;
    first_timeline.signalSemaphoreValueCount = 1;
    first_timeline.pSignalSemaphoreValues = &one;
    VkSubmitInfo first{VK_STRUCTURE_TYPE_SUBMIT_INFO, &first_timeline};
    first.commandBufferCount = 1;
    first.pCommandBuffers = &a.commands[0];
    first.signalSemaphoreCount = 1;
    first.pSignalSemaphores = &a.timeline;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence app_fence{};
    vkCreateFence(a.device, &fci, nullptr, &app_fence);
    if (!success(vkQueueSubmit(a.queues[0], 1, &first, app_fence),
                 "submit fill and timeline signal"))
        return 1;
    if (!success(vkWaitForFences(a.device, 1, &app_fence, VK_TRUE, 5000000000ULL),
                 "wait application fence"))
        return 1;
    vkResetFences(a.device, 1, &app_fence);
    uint64_t counter = 0;
    check(vkGetSemaphoreCounterValue(a.device, a.timeline, &counter) == VK_SUCCESS && counter == 1,
          "timeline completion survives application fence reset on the real driver");

    begin(a.commands[1]);
    VkBufferCopy region{0, 128, 128};
    vkCmdCopyBuffer(a.commands[1], a.buffer, a.buffer, 1, &region);
    host_barrier(a.commands[1]);
    vkEndCommandBuffer(a.commands[1]);
    VkSemaphore signals[]{a.timeline, a.binary};
    uint64_t values[]{2, 0};
    VkTimelineSemaphoreSubmitInfo second_timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    second_timeline.waitSemaphoreValueCount = 1;
    second_timeline.pWaitSemaphoreValues = &one;
    second_timeline.signalSemaphoreValueCount = 2;
    second_timeline.pSignalSemaphoreValues = values;
    VkProtectedSubmitInfo protected_info{VK_STRUCTURE_TYPE_PROTECTED_SUBMIT_INFO, &second_timeline};
    VkPipelineStageFlags transfer = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo second{VK_STRUCTURE_TYPE_SUBMIT_INFO, &protected_info};
    second.commandBufferCount = 1;
    second.pCommandBuffers = &a.commands[1];
    second.waitSemaphoreCount = 1;
    second.pWaitSemaphores = &a.timeline;
    second.pWaitDstStageMask = &transfer;
    second.signalSemaphoreCount = 2;
    second.pSignalSemaphores = signals;
    auto saved_second = second;
    auto saved_protected = protected_info;
    auto saved_timeline = second_timeline;
    check(vkQueueSubmit(a.queues[a.queue_count - 1], 1, &second, VK_NULL_HANDLE) == VK_SUCCESS &&
              !memcmp(&second, &saved_second, sizeof(second)) &&
              !memcmp(&protected_info, &saved_protected, sizeof(protected_info)) &&
              !memcmp(&second_timeline, &saved_timeline, sizeof(second_timeline)),
          "mixed binary/timeline GPU submission preserves every input node");
    begin(a.commands[2]);
    region = {128, 256, 128};
    vkCmdCopyBuffer(a.commands[2], a.buffer, a.buffer, 1, &region);
    host_barrier(a.commands[2]);
    vkEndCommandBuffer(a.commands[2]);
    uint64_t zero = 0, three = 3;
    VkTimelineSemaphoreSubmitInfo third_timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    third_timeline.waitSemaphoreValueCount = third_timeline.signalSemaphoreValueCount = 1;
    third_timeline.pWaitSemaphoreValues = &zero;
    third_timeline.pSignalSemaphoreValues = &three;
    VkSubmitInfo third{VK_STRUCTURE_TYPE_SUBMIT_INFO, &third_timeline};
    third.waitSemaphoreCount = third.signalSemaphoreCount = 1;
    third.pWaitSemaphores = &a.binary;
    third.pWaitDstStageMask = &transfer;
    third.pSignalSemaphores = &a.timeline;
    third.commandBufferCount = 1;
    third.pCommandBuffers = &a.commands[2];
    if (!success(vkQueueSubmit(a.queues[0], 1, &third, VK_NULL_HANDLE),
                 "submit binary wait and timeline signal"))
        return 1;
    VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wait.semaphoreCount = 1;
    wait.pSemaphores = &a.timeline;
    wait.pValues = &three;
    check(vkWaitSemaphores(a.device, &wait, 5000000000ULL) == VK_SUCCESS,
          "host timeline wait observes the completed GPU transfer chain");
    auto invalidate = [&] {
        if (!a.coherent) {
            VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
            range.memory = a.memory;
            range.size = VK_WHOLE_SIZE;
            vkInvalidateMappedMemoryRanges(a.device, 1, &range);
        }
    };
    invalidate();
    bool data_ok = true;
    auto data = static_cast<uint32_t *>(a.mapped);
    for (unsigned i = 0; i < 96; ++i)
        data_ok &= data[i] == 0x12345678;
    check(data_ok, "timeline and binary waits protect actual filled and copied bytes");

    VkEventCreateInfo eci{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
    VkEvent host_event{};
    vkCreateEvent(a.device, &eci, nullptr, &host_event);
    // Host events must be signaled before submitting their waiting command.
    // Use a timeline future wait to gate this submission, not a pending host
    // event wait (which violates queue forward progress / vkSetEvent-09543).
    if (!success(vkSetEvent(a.device, host_event), "signal host event before submission"))
        return 1;
    VkSemaphoreTypeCreateInfo gate_type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    gate_type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo gate_create{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &gate_type};
    VkSemaphore gate{};
    if (!success(vkCreateSemaphore(a.device, &gate_create, nullptr, &gate), "create future gate"))
        return 1;
    begin(a.commands[3]);
    vkCmdWaitEvents(a.commands[3], 1, &host_event, VK_PIPELINE_STAGE_HOST_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0, nullptr, 0, nullptr, 0, nullptr);
    vkCmdFillBuffer(a.commands[3], a.buffer, 0, 128, 0xabcdef01);
    host_barrier(a.commands[3]);
    vkEndCommandBuffer(a.commands[3]);
    uint64_t ten = 10;
    first_timeline.pSignalSemaphoreValues = &ten;
    uint64_t gate_value = 1;
    first_timeline.waitSemaphoreValueCount = 1;
    first_timeline.pWaitSemaphoreValues = &gate_value;
    first.waitSemaphoreCount = 1;
    first.pWaitSemaphores = &gate;
    first.pWaitDstStageMask = &transfer;
    first.pCommandBuffers = &a.commands[3];
    if (!success(vkQueueSubmit(a.queues[0], 1, &first, VK_NULL_HANDLE),
                 "submit future-gated timeline signal"))
        return 1;
    auto release_gate = [&] {
        VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
        signal.semaphore = gate;
        signal.value = 1;
        return vkSignalSemaphore(a.device, &signal);
    };
    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool released = false;
    std::thread guard([&] {
        std::unique_lock<std::mutex> lock(gate_mutex);
        if (!gate_cv.wait_for(lock, std::chrono::seconds(2), [&] { return released; })) {
            release_gate();
            released = true;
        }
    });
    if (a.queue_count > 1)
        check(vkQueueWaitIdle(a.queues[1]) == VK_SUCCESS &&
                  vkGetSemaphoreCounterValue(a.device, a.timeline, &counter) == VK_SUCCESS &&
                  counter == 3,
              "another queue on the same real device never publishes a future-gated signal");
    check(vkQueueWaitIdle(b.queues[0]) == VK_SUCCESS &&
              vkGetSemaphoreCounterValue(a.device, a.timeline, &counter) == VK_SUCCESS &&
              counter == 3,
          "another real device's queue idle never publishes a future-gated signal");
    check(vkDeviceWaitIdle(b.device) == VK_SUCCESS &&
              vkGetSemaphoreCounterValue(a.device, a.timeline, &counter) == VK_SUCCESS &&
              counter == 3,
          "another real device's idle never publishes a future-gated signal");
    {
        std::lock_guard<std::mutex> lock(gate_mutex);
        if (!released)
            release_gate();
        released = true;
    }
    gate_cv.notify_all();
    guard.join();
    wait.pValues = &ten;
    check(vkWaitSemaphores(a.device, &wait, 5000000000ULL) == VK_SUCCESS,
          "releasing the future gate allows the owning timeline to complete");
    invalidate();
    check(data[0] == 0xabcdef01,
          "future-gated GPU write is visible after its timeline completion");
    first.waitSemaphoreCount = first_timeline.waitSemaphoreValueCount = 0;
    first.pWaitSemaphores = nullptr;
    first.pWaitDstStageMask = nullptr;
    first_timeline.pWaitSemaphoreValues = nullptr;
    VkSemaphoreTypeCreateInfo future_type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    future_type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo future_create{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &future_type};
    VkSemaphore future{};
    if (!success(vkCreateSemaphore(a.device, &future_create, nullptr, &future),
                 "create future-value timeline"))
        return 1;
    VkCommandBuffer extra[7]{};
    VkCommandBufferAllocateInfo allocate_extra{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate_extra.commandPool = a.pool;
    allocate_extra.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate_extra.commandBufferCount = 7;
    if (!success(vkAllocateCommandBuffers(a.device, &allocate_extra, extra),
                 "allocate asynchronous transfer commands"))
        return 1;
    begin(extra[0]);
    vkCmdFillBuffer(extra[0], a.buffer, 384, 128, 0x44556677);
    host_barrier(extra[0]);
    vkEndCommandBuffer(extra[0]);
    begin(extra[1]);
    vkCmdFillBuffer(extra[1], a.buffer, 0, 128, 0x8899aabb);
    host_barrier(extra[1]);
    vkEndCommandBuffer(extra[1]);
    begin(extra[2]);
    vkCmdFillBuffer(extra[2], a.buffer, 384, 128, 0x1234abcd);
    host_barrier(extra[2]);
    vkEndCommandBuffer(extra[2]);
    begin(extra[3]);
    vkCmdFillBuffer(extra[3], a.buffer, 0, 128, 0xdeadbeef);
    vkEndCommandBuffer(extra[3]);
    for (unsigned i = 4; i < 7; ++i) {
        begin(extra[i]);
        VkBufferCopy copy{0, VkDeviceSize((i - 3) * 128), 128};
        vkCmdCopyBuffer(extra[i], a.buffer, a.buffer, 1, &copy);
        host_barrier(extra[i]);
        vkEndCommandBuffer(extra[i]);
    }
    VkFence follow_fence{};
    vkCreateFence(a.device, &fci, nullptr, &follow_fence);
    uint64_t five = 5, twenty = 20;
    VkTimelineSemaphoreSubmitInfo future_info{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    future_info.waitSemaphoreValueCount = future_info.signalSemaphoreValueCount = 1;
    future_info.pWaitSemaphoreValues = &five;
    future_info.pSignalSemaphoreValues = &twenty;
    uint32_t device_zero = 0, device_mask = 1;
    VkDeviceGroupSubmitInfo device_group{VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO, &future_info};
    device_group.waitSemaphoreCount = device_group.signalSemaphoreCount =
        device_group.commandBufferCount = 1;
    device_group.pWaitSemaphoreDeviceIndices = device_group.pSignalSemaphoreDeviceIndices =
        &device_zero;
    device_group.pCommandBufferDeviceMasks = &device_mask;
    VkSemaphore future_wait_handle = future;
    VkCommandBuffer mutable_command = extra[0];
    VkSubmitInfo queued{VK_STRUCTURE_TYPE_SUBMIT_INFO, &device_group};
    queued.waitSemaphoreCount = queued.signalSemaphoreCount = queued.commandBufferCount = 1;
    queued.pWaitSemaphores = &future_wait_handle;
    queued.pSignalSemaphores = &a.timeline;
    queued.pWaitDstStageMask = &transfer;
    queued.pCommandBuffers = &mutable_command;
    auto started = std::chrono::steady_clock::now();
    auto queued_result = vkQueueSubmit(a.queues[0], 1, &queued, app_fence);
    auto queued_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - started)
                         .count();
    check(queued_result == VK_SUCCESS && queued_ms < 100,
          "future-value queue submission returns before the host signal exists");
    check(vkGetFenceStatus(a.device, app_fence) == VK_NOT_READY,
          "a deferred submission does not prematurely signal its application fence");
    // The application can reuse every input immediately after vkQueueSubmit.
    five = UINT64_MAX;
    twenty = UINT64_MAX;
    future_wait_handle = VK_NULL_HANDLE;
    mutable_command = VK_NULL_HANDLE;
    device_zero = 7;
    device_mask = 0;
    VkSubmitInfo follow{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    follow.commandBufferCount = 1;
    follow.pCommandBuffers = &extra[1];
    if (!success(vkQueueSubmit(a.queues[0], 1, &follow, follow_fence),
                 "enqueue non-timeline work behind future wait"))
        return 1;
    check(vkGetFenceStatus(a.device, follow_fence) == VK_NOT_READY,
          "ordinary submissions cannot overtake a deferred timeline batch");
    VkSemaphoreSignalInfo host_future{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
    host_future.semaphore = future;
    host_future.value = 5;
    if (!success(vkSignalSemaphore(a.device, &host_future),
                 "release the future-value host dependency"))
        return 1;
    check(vkWaitForFences(a.device, 1, &app_fence, VK_TRUE, 5000000000ULL) == VK_SUCCESS &&
              vkWaitForFences(a.device, 1, &follow_fence, VK_TRUE, 5000000000ULL) == VK_SUCCESS,
          "deferred and following application fences complete after release");
    invalidate();
    check(data[96] == 0x44556677 && data[0] == 0x8899aabb &&
              vkGetSemaphoreCounterValue(a.device, a.timeline, &counter) == VK_SUCCESS &&
              counter == 20,
          "delayed dispatch retains command arrays, values, device masks, and FIFO order after "
          "input reuse");
    vkResetFences(a.device, 1, &follow_fence);
    uint64_t six = 6, thirty = 30;
    VkTimelineSemaphoreSubmitInfo batch_values[2]{
        {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO},
        {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO}};
    batch_values[0].signalSemaphoreValueCount = 1;
    batch_values[0].pSignalSemaphoreValues = &six;
    batch_values[1].waitSemaphoreValueCount = batch_values[1].signalSemaphoreValueCount = 1;
    batch_values[1].pWaitSemaphoreValues = &six;
    batch_values[1].pSignalSemaphoreValues = &thirty;
    VkSubmitInfo batches[2]{{VK_STRUCTURE_TYPE_SUBMIT_INFO, &batch_values[0]},
                            {VK_STRUCTURE_TYPE_SUBMIT_INFO, &batch_values[1]}};
    batches[0].signalSemaphoreCount = 1;
    batches[0].pSignalSemaphores = &future;
    batches[1].waitSemaphoreCount = batches[1].signalSemaphoreCount =
        batches[1].commandBufferCount = 1;
    batches[1].pWaitSemaphores = &future;
    batches[1].pWaitDstStageMask = &transfer;
    batches[1].pSignalSemaphores = &a.timeline;
    batches[1].pCommandBuffers = &extra[2];
    started = std::chrono::steady_clock::now();
    auto batched_result = vkQueueSubmit(a.queues[0], 2, batches, follow_fence);
    queued_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started)
                    .count();
    check(batched_result == VK_SUCCESS && queued_ms < 100 &&
              vkWaitForFences(a.device, 1, &follow_fence, VK_TRUE, 5000000000ULL) == VK_SUCCESS,
          "signal-before-wait batches in one call execute without a host dependency deadlock");
    invalidate();
    check(data[96] == 0x1234abcd &&
              vkGetSemaphoreCounterValue(a.device, a.timeline, &counter) == VK_SUCCESS &&
              counter == 30,
          "same-call dependent batches publish the right data and final value");
    if (a.queue_count > 1) {
        vkResetFences(a.device, 1, &follow_fence);
        uint64_t seven = 7, forty = 40;
        VkTimelineSemaphoreSubmitInfo consumer_values{
            VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        consumer_values.waitSemaphoreValueCount = consumer_values.signalSemaphoreValueCount = 1;
        consumer_values.pWaitSemaphoreValues = &seven;
        consumer_values.pSignalSemaphoreValues = &forty;
        VkSubmitInfo consumer{VK_STRUCTURE_TYPE_SUBMIT_INFO, &consumer_values};
        consumer.waitSemaphoreCount = consumer.signalSemaphoreCount = consumer.commandBufferCount =
            1;
        consumer.pWaitSemaphores = &future;
        consumer.pWaitDstStageMask = &transfer;
        consumer.pSignalSemaphores = &a.timeline;
        consumer.pCommandBuffers = &extra[4];
        VkTimelineSemaphoreSubmitInfo producer_values{
            VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        producer_values.signalSemaphoreValueCount = 1;
        producer_values.pSignalSemaphoreValues = &seven;
        VkSubmitInfo producer{VK_STRUCTURE_TYPE_SUBMIT_INFO, &producer_values};
        producer.signalSemaphoreCount = producer.commandBufferCount = 1;
        producer.pSignalSemaphores = &future;
        producer.pCommandBuffers = &extra[3];
        VkResult producer_result = VK_ERROR_UNKNOWN;
        std::thread producer_thread([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            producer_result = vkQueueSubmit(a.queues[1], 1, &producer, VK_NULL_HANDLE);
        });
        started = std::chrono::steady_clock::now();
        auto consumer_result = vkQueueSubmit(a.queues[0], 1, &consumer, follow_fence);
        queued_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started)
                        .count();
        auto consumer_complete =
            vkWaitForFences(a.device, 1, &follow_fence, VK_TRUE, 5000000000ULL);
        producer_thread.join();
        check(consumer_result == VK_SUCCESS && queued_ms < 100 && producer_result == VK_SUCCESS &&
                  consumer_complete == VK_SUCCESS,
              "a wait submitted before another queue's GPU signal returns immediately and later "
              "executes");
        invalidate();
        check(data[32] == 0xdeadbeef && data[63] == 0xdeadbeef,
              "native binary release/acquire tokens make cross-queue transfer writes visible "
              "without a producer host barrier");
        for (unsigned i = 0; i < 2; ++i) {
            vkResetFences(a.device, 1, &follow_fence);
            forty = 41 + i;
            consumer.pCommandBuffers = &extra[5 + i];
            auto q = a.queues[i == 0 ? 1 : 0];
            check(vkQueueSubmit(q, 1, &consumer, follow_fence) == VK_SUCCESS &&
                      vkWaitForFences(a.device, 1, &follow_fence, VK_TRUE, 5000000000ULL) ==
                          VK_SUCCESS,
                  i == 0 ? "another queue independently acquires the same timeline release"
                         : "a repeated wait on an already-acquired value never consumes a binary "
                           "token twice");
        }
        invalidate();
        check(data[64] == 0xdeadbeef && data[96] == 0xdeadbeef,
              "fan-out and repeated timeline waits retain the transferred bytes");
    }
    vkDeviceWaitIdle(a.device);
    vkDestroySemaphore(a.device, future, nullptr);
    vkDestroyFence(a.device, follow_fence, nullptr);
    vkDestroySemaphore(a.device, gate, nullptr);
    vkDestroyEvent(a.device, host_event, nullptr);
    vkDestroyFence(a.device, app_fence, nullptr);
    auto destroy = [&](Resources &r) {
        vkDestroyCommandPool(r.device, r.pool, nullptr);
        vkDestroySemaphore(r.device, r.binary, nullptr);
        vkDestroySemaphore(r.device, r.timeline, nullptr);
        vkUnmapMemory(r.device, r.memory);
        vkDestroyBuffer(r.device, r.buffer, nullptr);
        vkFreeMemory(r.device, r.memory, nullptr);
        vkDestroyDevice(r.device, nullptr);
    };
    destroy(a);
    destroy(b);
    vkDestroyInstance(instance, nullptr);
    printf("Timeline GPU failures: %u\n", failures);
    return failures ? 1 : 0;
}
