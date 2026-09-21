#ifndef VK_PNEXT_H
#define VK_PNEXT_H

#include "vk_common.h"
#include <vector>
#include <cstring>

namespace vku {

/**
 * Find a structure in the pNext chain matching the specified sType.
 */
template <typename T>
inline T* find_pnext(void* pNext, VkStructureType sType) {
    void* curr = pNext;
    while (curr != nullptr) {
        auto* header = static_cast<VkBaseOutStructure*>(curr);
        if (header->sType == sType) {
            return reinterpret_cast<T*>(header);
        }
        curr = header->pNext;
    }
    return nullptr;
}

template <typename T>
inline const T* find_pnext(const void* pNext, VkStructureType sType) {
    const void* curr = pNext;
    while (curr != nullptr) {
        auto* header = static_cast<const VkBaseInStructure*>(curr);
        if (header->sType == sType) {
            return reinterpret_cast<const T*>(header);
        }
        curr = header->pNext;
    }
    return nullptr;
}

/**
 * Check if a structure with the specified sType exists in the pNext chain.
 */
inline bool has_pnext(const void* pNext, VkStructureType sType) {
    return find_pnext<VkBaseInStructure>(pNext, sType) != nullptr;
}

/**
 * Unlink the first structure with the specified sType from the pNext chain.
 * Returns the unlinked structure pointer, or nullptr if not found.
 */
template <typename T = void>
inline T* unlink_pnext(void*& pNextHead, VkStructureType sType) {
    void** curr = &pNextHead;
    while (*curr != nullptr) {
        auto* header = static_cast<VkBaseOutStructure*>(*curr);
        if (header->sType == sType) {
            *curr = header->pNext;
            header->pNext = nullptr;
            return reinterpret_cast<T*>(header);
        }
        curr = reinterpret_cast<void**>(&header->pNext);
    }
    return nullptr;
}

template <typename T = void>
inline T* unlink_pnext(const void*& pNextHead, VkStructureType sType) {
    void* head = const_cast<void*>(pNextHead);
    T* res = unlink_pnext<T>(head, sType);
    pNextHead = head;
    return res;
}

/**
 * Relink a previously unlinked structure back to the front of the pNext chain.
 */
template <typename T = void>
inline T* relink_pnext(void*& pNextHead, void* unlinkedNode) {
    if (!unlinkedNode) return nullptr;
    auto* header = static_cast<VkBaseOutStructure*>(unlinkedNode);
    header->pNext = static_cast<VkBaseOutStructure*>(pNextHead);
    pNextHead = header;
    return reinterpret_cast<T*>(header);
}

template <typename T = void>
inline T* relink_pnext(const void*& pNextHead, void* unlinkedNode) {
    void* head = const_cast<void*>(pNextHead);
    T* res = relink_pnext<T>(head, unlinkedNode);
    pNextHead = head;
    return res;
}

/**
 * Find a structure in a const pNext chain and return a mutable pointer.
 */
template <typename T>
inline T* find_pnext_mut(const void* pNext, VkStructureType sType) {
    return const_cast<T*>(find_pnext<T>(pNext, sType));
}

/**
 * Remove a structure with the specified sType from the pNext chain.
 */
inline bool remove_pnext(void*& pNextHead, VkStructureType sType) {
    return unlink_pnext(pNextHead, sType) != nullptr;
}

inline bool remove_pnext(const void*& pNextHead, VkStructureType sType) {
    void* head = const_cast<void*>(pNextHead);
    bool res = unlink_pnext(head, sType) != nullptr;
    pNextHead = head;
    return res;
}

/**
 * Helper to check if an extension is already present in a vector of VkExtensionProperties.
 */
inline bool has_extension(const std::vector<VkExtensionProperties>& extensions, const char* name) {
    for (const auto& e : extensions) {
        if (std::strcmp(e.extensionName, name) == 0) return true;
    }
    return false;
}

/**
 * Helper to strip an extension from a vector of enabled extension strings.
 * Returns true if an extension was removed.
 */
inline bool strip_extension(std::vector<const char*>& extensions, const char* name) {
    bool stripped = false;
    for (auto it = extensions.begin(); it != extensions.end(); ) {
        if (std::strcmp(*it, name) == 0) {
            it = extensions.erase(it);
            stripped = true;
        } else {
            ++it;
        }
    }
    return stripped;
}

} // namespace vku

#endif // VK_PNEXT_H
