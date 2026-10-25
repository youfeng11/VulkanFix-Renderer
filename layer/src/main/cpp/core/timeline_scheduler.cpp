#include "timeline_scheduler.h"
#include "driver_loader.h"
#include "input_pnext_copy.h"
#include "vk_pnext.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <type_traits>
namespace vkfix {
namespace {
template <class T> std::vector<T> array(const T *p, size_t n) {
    return p && n ? std::vector<T>(p, p + n) : std::vector<T>{};
}
template <class T> T proc(VkDevice d, const char *name) {
    return reinterpret_cast<T>(get_real_proc(get_last_instance(), d, name));
}
struct Native {
    VkDevice device;
    // A late submission failure is exposed as logical device loss. Previously
    // submitted native work may still be running: leave its private objects to
    // vkDestroyDevice rather than destroying them while referenced by the GPU.
    bool release_resources = true;
    PFN_vkQueueSubmit submit;
    PFN_vkQueueBindSparse sparse;
    PFN_vkCreateSemaphore create_sem;
    PFN_vkDestroySemaphore destroy_sem;
    PFN_vkCreateFence create_fence;
    PFN_vkDestroyFence destroy_fence;
    PFN_vkGetFenceStatus fence_status;
    PFN_vkGetSemaphoreCounterValue counter;
    PFN_vkQueueWaitIdle queue_idle;
    PFN_vkDeviceWaitIdle device_idle;
    explicit Native(VkDevice d)
        : device(d), submit(proc<PFN_vkQueueSubmit>(d, "vkQueueSubmit")),
          sparse(proc<PFN_vkQueueBindSparse>(d, "vkQueueBindSparse")),
          create_sem(proc<PFN_vkCreateSemaphore>(d, "vkCreateSemaphore")),
          destroy_sem(proc<PFN_vkDestroySemaphore>(d, "vkDestroySemaphore")),
          create_fence(proc<PFN_vkCreateFence>(d, "vkCreateFence")),
          destroy_fence(proc<PFN_vkDestroyFence>(d, "vkDestroyFence")),
          fence_status(proc<PFN_vkGetFenceStatus>(d, "vkGetFenceStatus")),
          counter(proc<PFN_vkGetSemaphoreCounterValue>(d, "vkGetSemaphoreCounterValue")),
          queue_idle(proc<PFN_vkQueueWaitIdle>(d, "vkQueueWaitIdle")),
          device_idle(proc<PFN_vkDeviceWaitIdle>(d, "vkDeviceWaitIdle")) {
        if (!counter)
            counter = proc<PFN_vkGetSemaphoreCounterValue>(d, "vkGetSemaphoreCounterValueKHR");
    }
};
struct Binary {
    Native *native;
    VkSemaphore handle{};
    ~Binary() {
        if (handle && native->release_resources)
            native->destroy_sem(native->device, handle, nullptr);
    }
};
struct Release {
    uint64_t value;
    std::vector<std::shared_ptr<Binary>> tokens;
    std::vector<bool> acquired;
};
struct Semaphore {
    uint64_t value;
    std::shared_ptr<Release> release;
};
struct Wait {
    std::shared_ptr<Semaphore> semaphore;
    uint64_t value;
    uint32_t device_index = 0;
};
struct Signal {
    std::shared_ptr<Semaphore> semaphore;
    std::shared_ptr<Release> release;
};
struct BinaryOrder {
    uint64_t signals = 0, waits = 0, submitted_signals = 0, submitted_waits = 0;
};
struct BinaryOperation {
    std::shared_ptr<BinaryOrder> state;
    uint64_t epoch;
};
struct Snapshot {
    bool sparse = false;
    InputPNextCopy chain;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    VkBindSparseInfo bind{VK_STRUCTURE_TYPE_BIND_SPARSE_INFO};
    std::vector<VkCommandBuffer> commands;
    std::vector<VkSemaphore> waits, signals;
    std::vector<VkPipelineStageFlags> stages;
    std::vector<uint64_t> wait_values, signal_values;
    std::vector<uint32_t> wait_devices, signal_devices, command_devices;
    std::vector<VkSparseBufferMemoryBindInfo> buffers;
    std::vector<VkSparseImageOpaqueMemoryBindInfo> opaque_images;
    std::vector<VkSparseImageMemoryBindInfo> images;
    std::vector<std::vector<VkSparseMemoryBind>> buffer_binds, opaque_binds;
    std::vector<std::vector<VkSparseImageMemoryBind>> image_binds;
    VkTimelineSemaphoreSubmitInfo *timeline = nullptr;
    VkDeviceGroupSubmitInfo *group = nullptr;
    bool group_waits = false;
    uint32_t sparse_device = 0;
    VkResult copy(const VkSubmitInfo &info) {
        submit = info;
        auto result = chain.copy(info.pNext, InputChainKind::Submit, true);
        if (result != VK_SUCCESS)
            return result;
        commands = array(info.pCommandBuffers, info.commandBufferCount);
        waits = array(info.pWaitSemaphores, info.waitSemaphoreCount);
        signals = array(info.pSignalSemaphores, info.signalSemaphoreCount);
        stages = array(info.pWaitDstStageMask, info.waitSemaphoreCount);
        read_chain();
        return VK_SUCCESS;
    }
    VkResult copy(const VkBindSparseInfo &info) {
        sparse = true;
        bind = info;
        auto result = chain.copy(info.pNext, InputChainKind::BindSparse, true);
        if (result != VK_SUCCESS)
            return result;
        waits = array(info.pWaitSemaphores, info.waitSemaphoreCount);
        signals = array(info.pSignalSemaphores, info.signalSemaphoreCount);
        stages.assign(waits.size(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        buffers = array(info.pBufferBinds, info.bufferBindCount);
        opaque_images = array(info.pImageOpaqueBinds, info.imageOpaqueBindCount);
        images = array(info.pImageBinds, info.imageBindCount);
        for (auto &b : buffers)
            buffer_binds.push_back(array(b.pBinds, b.bindCount));
        for (auto &b : opaque_images)
            opaque_binds.push_back(array(b.pBinds, b.bindCount));
        for (auto &b : images)
            image_binds.push_back(array(b.pBinds, b.bindCount));
        for (size_t i = 0; i < buffers.size(); ++i)
            buffers[i].pBinds = buffer_binds[i].data();
        for (size_t i = 0; i < opaque_images.size(); ++i)
            opaque_images[i].pBinds = opaque_binds[i].data();
        for (size_t i = 0; i < images.size(); ++i)
            images[i].pBinds = image_binds[i].data();
        read_chain();
        return VK_SUCCESS;
    }
    void read_chain() {
        if (auto device = vku::find_pnext<VkDeviceGroupBindSparseInfo>(
                chain.head(), VK_STRUCTURE_TYPE_DEVICE_GROUP_BIND_SPARSE_INFO))
            sparse_device = device->resourceDeviceIndex;
        timeline = vku::find_pnext<VkTimelineSemaphoreSubmitInfo>(
            chain.head(), VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO);
        group = vku::find_pnext<VkDeviceGroupSubmitInfo>(
            chain.head(), VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO);
        wait_values.assign(waits.size(), 0);
        signal_values.assign(signals.size(), 0);
        if (timeline) {
            for (size_t i = 0; i < waits.size() && i < timeline->waitSemaphoreValueCount; ++i)
                wait_values[i] = timeline->pWaitSemaphoreValues[i];
            for (size_t i = 0; i < signals.size() && i < timeline->signalSemaphoreValueCount; ++i)
                signal_values[i] = timeline->pSignalSemaphoreValues[i];
        }
        if (group) {
            group_waits = group->waitSemaphoreCount != 0;
            wait_devices = array(group->pWaitSemaphoreDeviceIndices, group->waitSemaphoreCount);
            signal_devices =
                array(group->pSignalSemaphoreDeviceIndices, group->signalSemaphoreCount);
            command_devices = array(group->pCommandBufferDeviceMasks, group->commandBufferCount);
        }
    }
    void finalize() {
        const void *head = chain.head();
        if (timeline) {
            timeline->waitSemaphoreValueCount = uint32_t(wait_values.size());
            timeline->pWaitSemaphoreValues = wait_values.data();
            timeline->signalSemaphoreValueCount = uint32_t(signal_values.size());
            timeline->pSignalSemaphoreValues = signal_values.data();
            if (std::none_of(wait_values.begin(), wait_values.end(),
                             [](auto v) { return v != 0; }) &&
                std::none_of(signal_values.begin(), signal_values.end(),
                             [](auto v) { return v != 0; }))
                vku::unlink_pnext(head, timeline->sType);
        }
        if (group) {
            group->waitSemaphoreCount = uint32_t(wait_devices.size());
            group->pWaitSemaphoreDeviceIndices = wait_devices.data();
            group->signalSemaphoreCount = uint32_t(signal_devices.size());
            group->pSignalSemaphoreDeviceIndices = signal_devices.data();
            group->commandBufferCount = uint32_t(command_devices.size());
            group->pCommandBufferDeviceMasks = command_devices.data();
        }
        if (sparse) {
            bind.pNext = head;
            bind.waitSemaphoreCount = uint32_t(waits.size());
            bind.pWaitSemaphores = waits.data();
            bind.signalSemaphoreCount = uint32_t(signals.size());
            bind.pSignalSemaphores = signals.data();
            bind.pBufferBinds = buffers.data();
            bind.pImageOpaqueBinds = opaque_images.data();
            bind.pImageBinds = images.data();
        } else {
            submit.pNext = head;
            submit.waitSemaphoreCount = uint32_t(waits.size());
            submit.pWaitSemaphores = waits.data();
            submit.pWaitDstStageMask = stages.data();
            submit.signalSemaphoreCount = uint32_t(signals.size());
            submit.pSignalSemaphores = signals.data();
            submit.pCommandBuffers = commands.data();
        }
    }
};
struct Job {
    Native *native;
    Snapshot snapshot;
    std::vector<Wait> waits;
    std::vector<Signal> signals;
    std::vector<std::shared_ptr<Release>> acquires;
    std::vector<BinaryOperation> binary_waits, binary_signals;
    VkFence completion{}, app_fence{};
    bool tail = false;
    ~Job() {
        if (completion && native->release_resources)
            native->destroy_fence(native->device, completion, nullptr);
    }
};
struct Queue {
    VkQueue handle;
    std::list<std::shared_ptr<Job>> staged, pending;
    std::shared_ptr<std::recursive_mutex> io = std::make_shared<std::recursive_mutex>();
};
struct Device {
    Native native;
    std::mutex mutex;
    std::recursive_mutex fence_io;
    std::condition_variable cv;
    std::vector<Queue> queues;
    std::map<uint64_t, std::shared_ptr<Semaphore>> semaphores;
    std::map<uint64_t, std::shared_ptr<BinaryOrder>> binaries;
    std::thread worker;
    VkResult failure = VK_SUCCESS;
    bool stopping = false;
    uint32_t physical_count = 1;
    explicit Device(VkDevice d) : native(d) {}
    void fail() {
        failure = VK_ERROR_DEVICE_LOST;
        native.release_resources = false;
        cv.notify_all();
    }
    void poll() {
        for (auto &queue : queues)
            for (auto it = queue.pending.begin(); it != queue.pending.end();) {
                auto &job = *it;
                auto result = native.fence_status(native.device, job->completion);
                if (result == VK_NOT_READY) {
                    ++it;
                    continue;
                }
                if (result != VK_SUCCESS) {
                    fail();
                    return;
                }
                for (auto &signal : job->signals)
                    if (signal.release->value > signal.semaphore->value) {
                        signal.semaphore->value = signal.release->value;
                        signal.semaphore->release = signal.release;
                    }
                it = queue.pending.erase(it);
                cv.notify_all();
            }
    }
    bool ready(const Job &job) const {
        for (auto &wait : job.waits)
            if (wait.semaphore->value < wait.value)
                return false;
        for (auto &wait : job.binary_waits)
            if (wait.state->submitted_signals < wait.epoch)
                return false;
        for (auto &signal : job.binary_signals) {
            auto previous = signal.epoch - 1;
            if (signal.state->submitted_waits >= previous)
                continue;
            bool same_batch = false;
            for (auto &wait : job.binary_waits)
                same_batch |= wait.state == signal.state && wait.epoch >= previous;
            if (!same_batch)
                return false;
        }
        return true;
    }
    bool dispatch(size_t index) {
        auto &queue = queues[index];
        auto job = queue.staged.front();
        if (!ready(*job))
            return false;
        // Present/maintenance callbacks may hold io and reenter scheduler state.
        // Never wait for io while holding mutex; retry on a later worker pass.
        std::unique_lock<std::recursive_mutex> io(*queue.io, std::try_to_lock);
        if (!io.owns_lock())
            return false;
        // A public fence wait/status callback may reenter scheduler state.
        // Never block on its native access while holding the state mutex.
        std::unique_lock<std::recursive_mutex> fence_access(fence_io, std::defer_lock);
        if (job->tail && !fence_access.try_lock())
            return false;
        auto &s = job->snapshot;
        std::set<VkSemaphore> tokens;
        for (auto &wait : job->waits) {
            // Sparse binding waits have execution scope only. The published
            // counter already proves completion of the qualifying signal;
            // do not consume a queue-wide GPU memory-acquire token for them.
            if (s.sparse)
                continue;
            auto release = wait.semaphore->release;
            auto slot = index * physical_count + wait.device_index;
            if (!release || release->acquired[slot])
                continue;
            auto token = release->tokens[slot]->handle;
            if (!tokens.insert(token).second)
                continue;
            s.waits.push_back(token);
            s.wait_values.push_back(0);
            s.stages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            if (s.group_waits)
                s.wait_devices.push_back(wait.device_index);
            job->acquires.push_back(release);
        }
        s.finalize();
        VkResult result;
        if (job->tail)
            result = s.sparse ? native.sparse(queue.handle, 0, nullptr, job->app_fence)
                              : native.submit(queue.handle, 0, nullptr, job->app_fence);
        else
            result = s.sparse ? native.sparse(queue.handle, 1, &s.bind, job->completion)
                              : native.submit(queue.handle, 1, &s.submit, job->completion);
        if (result != VK_SUCCESS) {
            fail();
            return true;
        }
        if (!s.sparse)
            for (auto &wait : job->waits)
                if (wait.semaphore->release)
                    wait.semaphore->release->acquired[index * physical_count + wait.device_index] =
                        true;
        for (auto &operation : job->binary_waits)
            operation.state->submitted_waits =
                std::max(operation.state->submitted_waits, operation.epoch);
        for (auto &operation : job->binary_signals)
            operation.state->submitted_signals =
                std::max(operation.state->submitted_signals, operation.epoch);
        if (job->tail)
            queue.staged.pop_front();
        else
            queue.pending.splice(queue.pending.end(), queue.staged, queue.staged.begin());
        cv.notify_all();
        return true;
    }
    void run() {
        std::unique_lock<std::mutex> lock(mutex);
        while (!stopping) {
            if (failure != VK_SUCCESS) {
                cv.wait(lock, [&] { return stopping; });
                continue;
            }
            try {
                poll();
                if (failure != VK_SUCCESS)
                    continue;
                bool dispatched = false;
                for (size_t i = 0; i < queues.size() && failure == VK_SUCCESS; ++i)
                    if (!queues[i].staged.empty() && ready(*queues[i].staged.front()))
                        dispatched = dispatch(i) || dispatched;
                if (dispatched)
                    continue;
            } catch (...) {
                fail();
            }
            cv.wait_for(lock, std::chrono::milliseconds(1));
        }
    }
    std::shared_ptr<Release> release(uint64_t value, VkResult &result) {
        auto record = std::make_shared<Release>();
        record->value = value;
        record->acquired.assign(queues.size() * physical_count, false);
        for (size_t i = 0; i < queues.size() * physical_count; ++i) {
            auto token = std::make_shared<Binary>();
            token->native = &native;
            VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            result = native.create_sem(native.device, &info, nullptr, &token->handle);
            if (result != VK_SUCCESS)
                return {};
            record->tokens.push_back(std::move(token));
        }
        return record;
    }
    template <class Info>
    VkResult enqueue(size_t q, uint32_t count, const Info *infos, VkFence fence) {
        std::lock_guard<std::mutex> lock(mutex);
        if (failure != VK_SUCCESS)
            return failure;
        std::list<std::shared_ptr<Job>> transaction;
        struct Reservation {
            std::map<BinaryOrder *, BinaryOrder> before;
            bool committed = false;
            void touch(BinaryOrder *order) {
                before.emplace(order, *order);
            }
            ~Reservation() {
                if (!committed)
                    for (auto &entry : before)
                        *entry.first = entry.second;
            }
        } reservation;
        try {
            for (uint32_t n = 0; n < count; ++n) {
                auto job = std::make_shared<Job>();
                job->native = &native;
                auto result = job->snapshot.copy(infos[n]);
                if (result != VK_SUCCESS)
                    return result;
                auto &s = job->snapshot;
                if (s.sparse && !native.sparse)
                    return VK_ERROR_FEATURE_NOT_PRESENT;
                auto waits = std::move(s.waits), signals = std::move(s.signals);
                auto wvalues = std::move(s.wait_values), svalues = std::move(s.signal_values);
                auto stages = std::move(s.stages);
                auto wdevices = std::move(s.wait_devices), sdevices = std::move(s.signal_devices);
                if (s.sparse && s.sparse_device >= physical_count)
                    return VK_ERROR_INITIALIZATION_FAILED;
                s.waits.reserve(waits.size());
                s.wait_values.reserve(waits.size());
                s.stages.reserve(waits.size());
                for (size_t i = 0; i < waits.size(); ++i) {
                    auto it = semaphores.find((uint64_t)waits[i]);
                    if (it != semaphores.end()) {
                        auto index = s.sparse           ? s.sparse_device
                                     : wdevices.empty() ? 0
                                                        : wdevices[i];
                        if (index >= physical_count)
                            return VK_ERROR_INITIALIZATION_FAILED;
                        job->waits.push_back({it->second, wvalues[i], index});
                    } else {
                        auto binary = binaries.find((uint64_t)waits[i]);
                        if (binary != binaries.end()) {
                            auto &order = *binary->second;
                            reservation.touch(&order);
                            auto epoch = ++order.waits;
                            // A valid wait with no wrapped producer refers to
                            // an already-issued external/acquire signal.
                            if (order.signals < epoch) {
                                order.signals = epoch;
                                order.submitted_signals = std::max(order.submitted_signals, epoch);
                            }
                            job->binary_waits.push_back({binary->second, epoch});
                        }
                        s.waits.push_back(waits[i]);
                        s.wait_values.push_back(wvalues[i]);
                        s.stages.push_back(stages[i]);
                        if (!wdevices.empty())
                            s.wait_devices.push_back(wdevices[i]);
                    }
                }
                for (size_t i = 0; i < signals.size(); ++i) {
                    auto it = semaphores.find((uint64_t)signals[i]);
                    if (it != semaphores.end()) {
                        auto record = release(svalues[i], result);
                        if (!record)
                            return result;
                        for (auto &token : record->tokens) {
                            s.signals.push_back(token->handle);
                            s.signal_values.push_back(0);
                            if (!sdevices.empty())
                                s.signal_devices.push_back(sdevices[i]);
                        }
                        job->signals.push_back({it->second, std::move(record)});
                    } else {
                        auto binary = binaries.find((uint64_t)signals[i]);
                        if (binary != binaries.end()) {
                            reservation.touch(binary->second.get());
                            job->binary_signals.push_back(
                                {binary->second, ++binary->second->signals});
                        }
                        s.signals.push_back(signals[i]);
                        s.signal_values.push_back(svalues[i]);
                        if (!sdevices.empty())
                            s.signal_devices.push_back(sdevices[i]);
                    }
                }
                job->acquires.reserve(job->waits.size());
                VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
                result = native.create_fence(native.device, &fci, nullptr, &job->completion);
                if (result != VK_SUCCESS)
                    return result;
                transaction.push_back(std::move(job));
            }
            if (fence) {
                auto job = std::make_shared<Job>();
                job->native = &native;
                job->tail = true;
                job->app_fence = fence;
                job->snapshot.sparse = std::is_same<Info, VkBindSparseInfo>::value;
                if (job->snapshot.sparse && !native.sparse)
                    return VK_ERROR_FEATURE_NOT_PRESENT;
                transaction.push_back(std::move(job));
            }
            queues[q].staged.splice(queues[q].staged.end(), transaction);
            reservation.committed = true;
        } catch (const std::bad_alloc &) {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        cv.notify_all();
        return VK_SUCCESS;
    }
};
} // namespace
struct TimelineScheduler::State {
    std::mutex mutex;
    std::map<uint64_t, std::shared_ptr<Device>> devices;
    std::map<uint64_t, std::pair<std::weak_ptr<Device>, size_t>> queues;
    std::shared_ptr<Device> device(VkDevice d) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = devices.find((uint64_t)(uintptr_t)d);
        return it == devices.end() ? nullptr : it->second;
    }
    std::pair<std::shared_ptr<Device>, size_t> queue(VkQueue q) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = queues.find((uint64_t)(uintptr_t)q);
        return it == queues.end() ? std::make_pair(std::shared_ptr<Device>{}, size_t(0))
                                  : std::make_pair(it->second.first.lock(), it->second.second);
    }
};
TimelineScheduler &TimelineScheduler::get() {
    static TimelineScheduler value;
    return value;
}
TimelineScheduler::TimelineScheduler() : state(std::make_unique<State>()) {}
TimelineScheduler::~TimelineScheduler() {
    while (!state->devices.empty())
        stop((VkDevice)(uintptr_t)state->devices.begin()->first);
}
VkResult TimelineScheduler::start(VkDevice device, const VkDeviceCreateInfo &info) {
    try {
        auto d = std::make_shared<Device>(device);
        if (auto group = vku::find_pnext<VkDeviceGroupDeviceCreateInfo>(
                info.pNext, VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO))
            d->physical_count = std::max(1u, group->physicalDeviceCount);
        if (!d->native.submit || !d->native.create_sem || !d->native.destroy_sem ||
            !d->native.create_fence || !d->native.destroy_fence || !d->native.fence_status)
            return VK_ERROR_INITIALIZATION_FAILED;
        auto get = proc<PFN_vkGetDeviceQueue>(device, "vkGetDeviceQueue");
        auto get2 = proc<PFN_vkGetDeviceQueue2>(device, "vkGetDeviceQueue2");
        std::set<VkQueue> seen;
        for (uint32_t i = 0; i < info.queueCreateInfoCount; ++i)
            for (uint32_t n = 0; n < info.pQueueCreateInfos[i].queueCount; ++n) {
                auto &q = info.pQueueCreateInfos[i];
                VkQueue handle{};
                if (q.flags && get2) {
                    VkDeviceQueueInfo2 qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2};
                    qi.flags = q.flags;
                    qi.queueFamilyIndex = q.queueFamilyIndex;
                    qi.queueIndex = n;
                    get2(device, &qi, &handle);
                } else if (!q.flags && get)
                    get(device, q.queueFamilyIndex, n, &handle);
                if (!handle)
                    return VK_ERROR_INITIALIZATION_FAILED;
                if (seen.insert(handle).second)
                    d->queues.push_back({handle, {}, {}});
            }
        if (d->queues.empty())
            return VK_ERROR_INITIALIZATION_FAILED;
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->devices.count((uint64_t)(uintptr_t)device))
            return VK_ERROR_INITIALIZATION_FAILED;
        state->devices[(uint64_t)(uintptr_t)device] = d;
        try {
            for (size_t i = 0; i < d->queues.size(); ++i)
                state->queues[(uint64_t)(uintptr_t)d->queues[i].handle] = {d, i};
            d->worker = std::thread([d] { d->run(); });
        } catch (...) {
            for (auto &q : d->queues)
                state->queues.erase((uint64_t)(uintptr_t)q.handle);
            state->devices.erase((uint64_t)(uintptr_t)device);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        return VK_SUCCESS;
    } catch (const std::bad_alloc &) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}
void TimelineScheduler::stop(VkDevice device) {
    auto d = state->device(device);
    if (!d)
        return;
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        d->stopping = true;
        d->cv.notify_all();
    }
    if (d->worker.joinable())
        d->worker.join();
    if (d->failure == VK_SUCCESS && d->native.device_idle)
        d->native.device_idle(device);
    std::lock_guard<std::mutex> lock(state->mutex);
    for (auto &q : d->queues)
        state->queues.erase((uint64_t)(uintptr_t)q.handle);
    state->devices.erase((uint64_t)(uintptr_t)device);
}
bool TimelineScheduler::active(VkDevice device) {
    return bool(state->device(device));
}
VkResult TimelineScheduler::error(VkDevice device) {
    auto d = state->device(device);
    if (!d)
        return VK_SUCCESS;
    std::lock_guard<std::mutex> lock(d->mutex);
    return d->failure;
}
void TimelineScheduler::add(VkDevice device, VkSemaphore semaphore, uint64_t initial) {
    auto d = state->device(device);
    if (!d)
        return;
    std::lock_guard<std::mutex> lock(d->mutex);
    d->semaphores[(uint64_t)semaphore] = std::make_shared<Semaphore>(Semaphore{initial, {}});
}
void TimelineScheduler::add_binary(VkDevice device, VkSemaphore semaphore) {
    auto d = state->device(device);
    if (!d)
        return;
    std::lock_guard<std::mutex> lock(d->mutex);
    d->binaries[(uint64_t)semaphore] = std::make_shared<BinaryOrder>();
}
void TimelineScheduler::remove(VkDevice device, VkSemaphore semaphore) {
    auto d = state->device(device);
    if (!d)
        return;
    std::lock_guard<std::mutex> lock(d->mutex);
    d->semaphores.erase((uint64_t)semaphore);
    d->binaries.erase((uint64_t)semaphore);
}
bool TimelineScheduler::contains(VkDevice device, VkSemaphore semaphore) {
    auto d = state->device(device);
    if (!d)
        return false;
    std::lock_guard<std::mutex> lock(d->mutex);
    return d->semaphores.count((uint64_t)semaphore);
}
bool TimelineScheduler::counter(VkDevice device, VkSemaphore semaphore, uint64_t *value,
                                VkResult &result) {
    if (!value) {
        result = VK_ERROR_INITIALIZATION_FAILED;
        return true;
    }
    auto d = state->device(device);
    if (!d)
        return false;
    std::lock_guard<std::mutex> lock(d->mutex);
    auto it = d->semaphores.find((uint64_t)semaphore);
    if (it == d->semaphores.end())
        return false;
    d->poll();
    result = d->failure;
    if (result == VK_SUCCESS)
        *value = it->second->value;
    return true;
}
bool TimelineScheduler::signal(VkDevice device, const VkSemaphoreSignalInfo *info,
                               VkResult &result) {
    auto d = state->device(device);
    if (!d || !info)
        return false;
    std::lock_guard<std::mutex> lock(d->mutex);
    auto it = d->semaphores.find((uint64_t)info->semaphore);
    if (it == d->semaphores.end())
        return false;
    result = d->failure;
    if (result == VK_SUCCESS && info->value > it->second->value) {
        it->second->value = info->value;
        it->second->release.reset();
    }
    d->cv.notify_all();
    return true;
}
bool TimelineScheduler::wait(VkDevice device, const VkSemaphoreWaitInfo *info, uint64_t timeout,
                             VkResult &result) {
    auto d = state->device(device);
    if (!d || !info)
        return false;
    std::unique_lock<std::mutex> lock(d->mutex);
    bool emulated = false;
    for (uint32_t i = 0; i < info->semaphoreCount; ++i)
        emulated |= d->semaphores.count((uint64_t)info->pSemaphores[i]) != 0;
    if (!emulated)
        return false;
    auto began = std::chrono::steady_clock::now();
    bool any = info->flags & VK_SEMAPHORE_WAIT_ANY_BIT;
    while (true) {
        d->poll();
        if (d->failure != VK_SUCCESS) {
            result = d->failure;
            return true;
        }
        bool satisfied = !any;
        for (uint32_t i = 0; i < info->semaphoreCount; ++i) {
            uint64_t value = 0;
            auto it = d->semaphores.find((uint64_t)info->pSemaphores[i]);
            if (it != d->semaphores.end())
                value = it->second->value;
            else {
                result = d->native.counter ? d->native.counter(device, info->pSemaphores[i], &value)
                                           : VK_ERROR_FEATURE_NOT_PRESENT;
                if (result != VK_SUCCESS)
                    return true;
            }
            if (any)
                satisfied |= value >= info->pValues[i];
            else
                satisfied &= value >= info->pValues[i];
        }
        if (satisfied) {
            result = VK_SUCCESS;
            return true;
        }
        auto elapsed = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now() - began)
                                    .count());
        if (timeout != UINT64_MAX && elapsed >= timeout) {
            result = VK_TIMEOUT;
            return true;
        }
        auto slice =
            timeout == UINT64_MAX ? 1000000ULL : std::min<uint64_t>(timeout - elapsed, 1000000ULL);
        d->cv.wait_for(lock, std::chrono::nanoseconds(slice));
    }
}
bool TimelineScheduler::submit(VkQueue queue, uint32_t count, const VkSubmitInfo *infos,
                               VkFence fence, VkResult &result) {
    auto q = state->queue(queue);
    if (!q.first)
        return false;
    result = q.first->enqueue(q.second, count, infos, fence);
    return true;
}
bool TimelineScheduler::bind_sparse(VkQueue queue, uint32_t count, const VkBindSparseInfo *infos,
                                    VkFence fence, VkResult &result) {
    auto q = state->queue(queue);
    if (!q.first)
        return false;
    result = q.first->enqueue(q.second, count, infos, fence);
    return true;
}
VkResult TimelineScheduler::drain(VkQueue queue) {
    auto q = state->queue(queue);
    if (!q.first)
        return VK_SUCCESS;
    std::unique_lock<std::mutex> lock(q.first->mutex);
    q.first->cv.wait(lock, [&] {
        return q.first->failure != VK_SUCCESS || q.first->queues[q.second].staged.empty();
    });
    return q.first->failure;
}
bool TimelineScheduler::queue_idle(VkQueue queue, VkResult &result) {
    auto q = state->queue(queue);
    if (!q.first)
        return false;
    result = drain(queue);
    if (result != VK_SUCCESS)
        return true;
    {
        std::lock_guard<std::recursive_mutex> io(*q.first->queues[q.second].io);
        result = q.first->native.queue_idle ? q.first->native.queue_idle(queue)
                                            : VK_ERROR_INITIALIZATION_FAILED;
    }
    std::lock_guard<std::mutex> lock(q.first->mutex);
    if (result == VK_SUCCESS) {
        q.first->poll();
        result = q.first->failure;
    } else
        q.first->fail();
    return true;
}
bool TimelineScheduler::device_idle(VkDevice device, VkResult &result) {
    auto d = state->device(device);
    if (!d)
        return false;
    {
        std::unique_lock<std::mutex> lock(d->mutex);
        d->cv.wait(lock, [&] {
            if (d->failure != VK_SUCCESS)
                return true;
            for (auto &q : d->queues)
                if (!q.staged.empty())
                    return false;
            return true;
        });
        result = d->failure;
    }
    if (result != VK_SUCCESS)
        return true;
    {
        std::vector<std::unique_lock<std::recursive_mutex>> io;
        for (auto &q : d->queues)
            io.emplace_back(*q.io);
        result =
            d->native.device_idle ? d->native.device_idle(device) : VK_ERROR_INITIALIZATION_FAILED;
    }
    std::lock_guard<std::mutex> lock(d->mutex);
    if (result == VK_SUCCESS) {
        d->poll();
        result = d->failure;
    } else
        d->fail();
    return true;
}
VkResult TimelineScheduler::native_queue_call(VkQueue queue,
                                              const std::function<VkResult()> &call) {
    auto q = state->queue(queue);
    if (!q.first)
        return call();
    std::lock_guard<std::recursive_mutex> io(*q.first->queues[q.second].io);
    return call();
}
VkResult TimelineScheduler::native_fence_call(VkDevice device,
                                              const std::function<VkResult()> &call) {
    auto d = state->device(device);
    if (!d) return call();
    std::lock_guard<std::recursive_mutex> access(d->fence_io);
    return call();
}
bool TimelineScheduler::submit_internal(VkQueue queue, uint32_t count, const VkSubmitInfo *infos,
                                        VkFence fence, VkResult &result) {
    auto q = state->queue(queue);
    if (!q.first)
        return false;
    std::lock_guard<std::recursive_mutex> io(*q.first->queues[q.second].io);
    result = q.first->native.submit(queue, count, infos, fence);
    return true;
}
VkResult TimelineScheduler::before_present(VkQueue queue, const VkPresentInfoKHR *info) {
    auto q = state->queue(queue);
    if (!q.first)
        return VK_SUCCESS;
    auto &d = *q.first;
    std::unique_lock<std::mutex> lock(d.mutex);
    std::vector<BinaryOperation> waits;
    try {
        if (info)
            for (uint32_t i = 0; i < info->waitSemaphoreCount; ++i) {
                auto it = d.binaries.find((uint64_t)info->pWaitSemaphores[i]);
                if (it != d.binaries.end())
                    waits.push_back({it->second, it->second->waits + 1});
            }
    } catch (const std::bad_alloc &) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    d.cv.wait(lock, [&] {
        if (d.failure != VK_SUCCESS)
            return true;
        if (!d.queues[q.second].staged.empty())
            return false;
        for (auto &w : waits)
            if (w.state->signals >= w.epoch && w.state->submitted_signals < w.epoch)
                return false;
        return true;
    });
    if (d.failure != VK_SUCCESS)
        return d.failure;
    for (auto &w : waits) {
        w.state->waits = w.epoch;
        if (w.state->signals < w.epoch) {
            w.state->signals = w.epoch;
            w.state->submitted_signals = w.epoch;
        }
    }
    d.cv.notify_all();
    return VK_SUCCESS;
}
void TimelineScheduler::after_present(VkQueue queue, const VkPresentInfoKHR *info,
                                      VkResult result) {
    auto q = state->queue(queue);
    if (!q.first || !info)
        return;
    std::lock_guard<std::mutex> lock(q.first->mutex);
    const bool oom =
        result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY;
    const bool enqueued = result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR ||
                          result == VK_ERROR_OUT_OF_DATE_KHR ||
                          result == VK_ERROR_SURFACE_LOST_KHR ||
                          result == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT ||
                          result == VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT;
    if (!oom && !enqueued) {
        q.first->fail();
        return;
    }
    for (uint32_t i = 0; i < info->waitSemaphoreCount; ++i) {
        auto it = q.first->binaries.find((uint64_t)info->pWaitSemaphores[i]);
        if (it != q.first->binaries.end()) {
            auto &order = *it->second;
            // Only the native present call has actually submitted this wait.
            // Do not let another queue re-signal the binary before that call.
            if (oom)
                --order.waits;
            else
                order.submitted_waits = std::max(order.submitted_waits, order.waits);
        }
    }
    q.first->cv.notify_all();
}
} // namespace vkfix
