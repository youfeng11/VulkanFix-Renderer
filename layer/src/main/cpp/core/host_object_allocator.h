#pragma once

#include <atomic>
#include <cstddef>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>
#include <vulkan/vulkan.h>

namespace vkfix {
namespace detail {
// A separate, intrusive context survives the object AND its shared_ptr control
// block. All rebound allocators see the compatible allocator supplied at destroy.
struct HostObjectAllocationState {
    std::atomic<size_t> references{1};
    VkAllocationCallbacks callbacks{};
    bool provided = false;

    static void* allocate(const VkAllocationCallbacks* callbacks, size_t size, size_t alignment) {
        if (!callbacks) return ::operator new(size);
        void* memory = callbacks->pfnAllocation(callbacks->pUserData, size, alignment, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
        if (!memory) throw std::bad_alloc();
        return memory;
    }
    static void free(const VkAllocationCallbacks* callbacks, void* memory) noexcept {
        if (callbacks) callbacks->pfnFree(callbacks->pUserData, memory);
        else ::operator delete(memory);
    }
    void release() noexcept {
        if (references.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
        const auto saved = callbacks;
        const bool useCallbacks = provided;
        this->~HostObjectAllocationState();
        free(useCallbacks ? &saved : nullptr, this);
    }
};
} // namespace detail

template<class T> class HostObjectAllocator {
    template<class> friend class HostObjectAllocator;
    using State = detail::HostObjectAllocationState;
    State* state_ = nullptr;
    void retain() noexcept { if (state_) state_->references.fetch_add(1, std::memory_order_relaxed); }
    explicit HostObjectAllocator(State* state) noexcept : state_(state) {}
public:
    using value_type = T;
    using propagate_on_container_move_assignment = std::true_type;
    HostObjectAllocator() noexcept = default;
    HostObjectAllocator(const HostObjectAllocator& other) noexcept : state_(other.state_) { retain(); }
    template<class U> HostObjectAllocator(const HostObjectAllocator<U>& other) noexcept : state_(other.state_) { retain(); }
    HostObjectAllocator(HostObjectAllocator&& other) noexcept : state_(std::exchange(other.state_, nullptr)) {}
    ~HostObjectAllocator() { if (state_) state_->release(); }
    HostObjectAllocator& operator=(HostObjectAllocator other) noexcept { std::swap(state_, other.state_); return *this; }

    static HostObjectAllocator for_object(const VkAllocationCallbacks* callbacks) {
        void* memory = State::allocate(callbacks, sizeof(State), alignof(State));
        auto state = new (memory) State;
        if (callbacks) { state->callbacks = *callbacks; state->provided = true; }
        return HostObjectAllocator(state);
    }
    // Valid usage requires compatible create/destroy allocators. The callback
    // functions and pUserData may differ, so retain the current command's copy.
    void set_destroy_callbacks(const VkAllocationCallbacks* callbacks) noexcept {
        if (state_ && callbacks) { state_->callbacks = *callbacks; state_->provided = true; }
    }
    T* allocate(size_t count) {
        static_assert(alignof(T) <= alignof(std::max_align_t), "default allocation must satisfy the stored type");
        if (count > std::numeric_limits<size_t>::max() / sizeof(T)) throw std::bad_alloc();
        const auto callbacks = state_ && state_->provided ? &state_->callbacks : nullptr;
        return static_cast<T*>(State::allocate(callbacks, count ? count * sizeof(T) : 1, alignof(T)));
    }
    void deallocate(T* memory, size_t) noexcept {
        State::free(state_ && state_->provided ? &state_->callbacks : nullptr, memory);
    }
    template<class U> bool operator==(const HostObjectAllocator<U>& other) const noexcept { return state_ == other.state_; }
    template<class U> bool operator!=(const HostObjectAllocator<U>& other) const noexcept { return !(*this == other); }
};
} // namespace vkfix
