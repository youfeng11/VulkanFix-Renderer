#include "promoted_features.h"
#include "driver_loader.h"
#include "native_instance_state.h"
#include "subgroup_broadcast.h"
#include <algorithm>
#include <cstddef>
#include <iterator>
#include <set>
#include <string>
#include <utility>

namespace {
struct FeatureField {
  size_t aggregate, member;
};
struct FeatureGroup {
  unsigned version;
  VkStructureType type;
  size_t size;
  const char *extension;
  uint32_t promoted;
  const FeatureField *fields;
  size_t count;
  void *(*create)();
  void (*destroy)(void *);
};
struct FeatureAlias {
  size_t member;
  const char *extension;
};
template <class T> void *new_feature() { return new T{}; }
template <class T> void delete_feature(void *pointer) {
  delete static_cast<T *>(pointer);
}
struct OwnedNode {
  std::unique_ptr<void, void (*)(void *)> storage{nullptr, [](void *) {}};
  VkBaseOutStructure *get() const {
    return static_cast<VkBaseOutStructure *>(storage.get());
  }
};
template <class T> OwnedNode clone_node(const VkBaseOutStructure *node) {
  return {std::unique_ptr<void, void (*)(void *)>(
      new T(*reinterpret_cast<const T *>(node)), delete_feature<T>)};
}
OwnedNode make_node(const FeatureGroup &group) {
  OwnedNode result{
      std::unique_ptr<void, void (*)(void *)>(group.create(), group.destroy)};
  result.get()->sType = group.type;
  return result;
}
#include "promoted_feature_metadata.inc"

VkBool32 read_flag(const void *object, size_t offset) {
  VkBool32 value;
  memcpy(&value, static_cast<const char *>(object) + offset, sizeof(value));
  return value;
}
void write_flag(void *object, size_t offset, VkBool32 value) {
  memcpy(static_cast<char *>(object) + offset, &value, sizeof(value));
}
const FeatureGroup *group_for(VkStructureType type) {
  for (const auto &group : feature_groups)
    if (group.type == type)
      return &group;
  return nullptr;
}
struct NativeFeatures {
  uint32_t api = VK_API_VERSION_1_0;
  VkPhysicalDeviceVulkan11Features v11{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
  VkPhysicalDeviceVulkan12Features v12{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  VkPhysicalDeviceFeatures base{};
  std::set<std::string> extensions;
  std::vector<OwnedNode> nodes;
  const void *aggregate(unsigned version) const {
    return version == 11 ? (const void *)&v11 : &v12;
  }
  void *aggregate(unsigned version) {
    return version == 11 ? (void *)&v11 : &v12;
  }
  bool native(const FeatureGroup &group) const {
    return api >= group.promoted ||
           (*group.extension && extensions.count(group.extension));
  }
  explicit NativeFeatures(VkPhysicalDevice physical) {
    auto instance = vkfix::native_instance_for_physical(physical);
    if (!instance)
      instance = get_last_instance();
    auto properties = (PFN_vkGetPhysicalDeviceProperties)get_real_proc(
        instance, VK_NULL_HANDLE, "vkGetPhysicalDeviceProperties");
    if (properties) {
      VkPhysicalDeviceProperties p{};
      properties(physical, &p);
      api = p.apiVersion;
    }
    auto enumerate = (PFN_vkEnumerateDeviceExtensionProperties)get_real_proc(
        instance, VK_NULL_HANDLE, "vkEnumerateDeviceExtensionProperties");
    if (enumerate)
      for (unsigned attempt = 0; attempt < 8; ++attempt) {
        uint32_t count = 0;
        if (enumerate(physical, nullptr, &count, nullptr) != VK_SUCCESS)
          break;
        std::vector<VkExtensionProperties> values(count);
        VkResult result =
            count ? enumerate(physical, nullptr, &count, values.data())
                  : VK_SUCCESS;
        if (result == VK_INCOMPLETE)
          continue;
        if (result == VK_SUCCESS)
          for (uint32_t i = 0; i < count; ++i)
            extensions.insert(values[i].extensionName);
        break;
      }
    VkPhysicalDeviceFeatures2 query{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    if (api >= VK_API_VERSION_1_2) {
      v11.pNext = &v12;
      query.pNext = &v11;
    } else {
      for (const auto &group : feature_groups)
        if (native(group)) {
          nodes.push_back(make_node(group));
          auto node = nodes.back().get();
          node->pNext = (VkBaseOutStructure *)query.pNext;
          query.pNext = node;
        }
    }
    if (auto query2 = native_features2_proc(physical))
      query2(physical, &query);
    else if (auto query1 = (PFN_vkGetPhysicalDeviceFeatures)get_real_proc(
                 instance, VK_NULL_HANDLE, "vkGetPhysicalDeviceFeatures"))
      query1(physical, &query.features);
    base = query.features;
    if (api < VK_API_VERSION_1_2) {
      for (auto &node : nodes) {
        const auto &group = *group_for(node.get()->sType);
        for (size_t i = 0; i < group.count; ++i)
          write_flag(aggregate(group.version), group.fields[i].aggregate,
                     read_flag(node.get(), group.fields[i].member));
      }
      for (const auto &alias : feature_aliases)
        write_flag(&v12, alias.member,
                   *alias.extension && extensions.count(alias.extension));
      // EXT_descriptor_indexing alone does not promise Vulkan 1.2's
      // minimum set. Every mandatory member must also be supported.
      v12.descriptorIndexing =
          v12.descriptorIndexing &&
          base.shaderSampledImageArrayDynamicIndexing &&
          base.shaderStorageBufferArrayDynamicIndexing &&
          v12.shaderUniformTexelBufferArrayDynamicIndexing &&
          v12.shaderStorageTexelBufferArrayDynamicIndexing &&
          v12.shaderSampledImageArrayNonUniformIndexing &&
          v12.shaderStorageBufferArrayNonUniformIndexing &&
          v12.shaderUniformTexelBufferArrayNonUniformIndexing &&
          v12.descriptorBindingSampledImageUpdateAfterBind &&
          v12.descriptorBindingStorageImageUpdateAfterBind &&
          v12.descriptorBindingStorageBufferUpdateAfterBind &&
          v12.descriptorBindingUniformTexelBufferUpdateAfterBind &&
          v12.descriptorBindingStorageTexelBufferUpdateAfterBind &&
          v12.descriptorBindingUpdateUnusedWhilePending &&
          v12.descriptorBindingPartiallyBound && v12.runtimeDescriptorArray;
      v12.subgroupBroadcastDynamicId =
          bool(vkfix::subgroup_broadcast_support(physical));
    }
    v11.pNext = nullptr;
    v12.pNext = nullptr;
  }
  void fill(void *target, const FeatureGroup &group) const {
    for (size_t i = 0; i < group.count; ++i)
      write_flag(
          target, group.fields[i].member,
          read_flag(aggregate(group.version), group.fields[i].aggregate));
  }
};
bool emulated(const FeatureGroup &group, const FeatureField &field) {
  return group.version == 12 &&
         (field.aggregate ==
              offsetof(VkPhysicalDeviceVulkan12Features, timelineSemaphore) ||
          field.aggregate ==
              offsetof(VkPhysicalDeviceVulkan12Features, hostQueryReset));
}
void add_extension(std::vector<const char *> &extensions, const char *name) {
  if (!*name)
    return;
  if (std::none_of(extensions.begin(), extensions.end(),
                   [&](const char *e) { return !strcmp(e, name); }))
    extensions.push_back(name);
}
} // namespace

PFN_vkGetPhysicalDeviceFeatures2
native_features2_proc(VkPhysicalDevice physical) {
  auto instance = vkfix::native_instance_for_physical(physical);
  if (!instance)
    instance = get_last_instance();
  if (auto instance_proc = get_real_instance_proc_addr()) {
    auto query = (PFN_vkGetPhysicalDeviceFeatures2)instance_proc(
        instance, "vkGetPhysicalDeviceFeatures2");
    if (!query)
      query = (PFN_vkGetPhysicalDeviceFeatures2)instance_proc(
          instance, "vkGetPhysicalDeviceFeatures2KHR");
    return query;
  }
  // Also supports deterministic mock-driver tests without loading a library.
  auto query = (PFN_vkGetPhysicalDeviceFeatures2)get_real_proc(
      instance, VK_NULL_HANDLE, "vkGetPhysicalDeviceFeatures2");
  return query
             ? query
             : (PFN_vkGetPhysicalDeviceFeatures2)get_real_proc(
                   instance, VK_NULL_HANDLE, "vkGetPhysicalDeviceFeatures2KHR");
}

struct PromotedFeatureQuery::State {
  VkPhysicalDeviceFeatures2 *root;
  NativeFeatures native;
  std::vector<std::pair<VkBaseOutStructure *, VkBaseOutStructure *>> links;
  std::vector<OwnedNode> expanded;
  VkPhysicalDeviceVulkan11Features *v11 = nullptr;
  VkPhysicalDeviceVulkan12Features *v12 = nullptr;
  State(VkPhysicalDevice physical, VkPhysicalDeviceFeatures2 *output)
      : root(output), native(physical) {
    if (!root || native.api >= VK_API_VERSION_1_2)
      return;
    auto node = (VkBaseOutStructure *)root->pNext;
    while (node) {
      links.emplace_back(node, node->pNext);
      if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES)
        v11 = (VkPhysicalDeviceVulkan11Features *)node;
      if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)
        v12 = (VkPhysicalDeviceVulkan12Features *)node;
      if (auto group = group_for(node->sType))
        native.fill(node, *group);
      node = node->pNext;
    }
    // Rebuild without aggregates. Retain unknown extension outputs and
    // their exact original links for restoration after all module hooks.
    VkBaseOutStructure *head = nullptr;
    VkBaseOutStructure **tail = &head;
    for (auto entry : links)
      if (entry.first != (void *)v11 && entry.first != (void *)v12) {
        *tail = entry.first;
        tail = &entry.first->pNext;
      }
    *tail = nullptr;
    root->pNext = head;
    for (const auto &group : feature_groups) {
      if (!(group.version == 11 ? (void *)v11 : (void *)v12))
        continue;
      expanded.push_back(make_node(group));
      auto feature = expanded.back().get();
      native.fill(feature, group);
      feature->pNext = (VkBaseOutStructure *)root->pNext;
      root->pNext = feature;
    }
  }
  ~State() {
    if (!root || links.empty())
      return;
    if (v11) {
      auto next = v11->pNext;
      *v11 = native.v11;
      v11->pNext = next;
    }
    if (v12) {
      auto next = v12->pNext;
      *v12 = native.v12;
      v12->pNext = next;
    }
    for (auto &node : expanded) {
      const auto &group = *group_for(node.get()->sType);
      void *target = group.version == 11 ? (void *)v11 : (void *)v12;
      for (size_t i = 0; i < group.count; ++i)
        write_flag(target, group.fields[i].aggregate,
                   read_flag(node.get(), group.fields[i].member));
    }
    for (auto entry : links)
      entry.first->pNext = entry.second;
    root->pNext = links.front().first;
  }
};
PromotedFeatureQuery::PromotedFeatureQuery(VkPhysicalDevice physical,
                                           VkPhysicalDeviceFeatures2 *features)
    : state(std::make_unique<State>(physical, features)) {}
PromotedFeatureQuery::~PromotedFeatureQuery() = default;

struct PromotedDeviceFeatures::State {
  NativeFeatures native;
  VkResult status = VK_SUCCESS;
  std::vector<OwnedNode> nodes;
  std::vector<VkBaseOutStructure *> chain;
  State(VkPhysicalDevice physical, VkDeviceCreateInfo &info,
        std::vector<const char *> &extensions)
      : native(physical) {
    const VkPhysicalDeviceVulkan11Features *v11 = nullptr;
    const VkPhysicalDeviceVulkan12Features *v12 = nullptr;
    std::set<const void *> seen;
    for (auto node = (const VkBaseOutStructure *)info.pNext; node;
         node = node->pNext) {
      if (!seen.insert(node).second) {
        status = VK_ERROR_INITIALIZATION_FAILED;
        return;
      }
      auto copy = clone_input(node);
      if (!copy.get()) {
        status = VK_ERROR_EXTENSION_NOT_PRESENT;
        return;
      }
      auto target = copy.get();
      nodes.push_back(std::move(copy));
      if (native.api < VK_API_VERSION_1_1 &&
          target->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
        // The legacy driver does not need to understand Features2 in
        // order to enable its base feature bits. The cloned payload
        // outlives the native call and remains writable by modules.
        info.pEnabledFeatures =
            &reinterpret_cast<VkPhysicalDeviceFeatures2 *>(target)->features;
        continue;
      }
      if (target->sType ==
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES)
        v11 = (VkPhysicalDeviceVulkan11Features *)target;
      if (target->sType ==
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)
        v12 = (VkPhysicalDeviceVulkan12Features *)target;
      if (native.api >= VK_API_VERSION_1_2 ||
          (target != (const void *)v11 && target != (const void *)v12))
        chain.push_back(target);
    }
    if (native.api < VK_API_VERSION_1_2) {
      for (const auto &group : feature_groups) {
        const void *aggregate =
            group.version == 11 ? (const void *)v11 : (const void *)v12;
        VkBaseOutStructure *target = nullptr;
        for (auto node : chain)
          if (node->sType == group.type)
            target = node;
        if (aggregate) {
          auto copy = make_node(group);
          target = copy.get();
          for (size_t i = 0; i < group.count; ++i)
            write_flag(target, group.fields[i].member,
                       read_flag(aggregate, group.fields[i].aggregate));
          nodes.push_back(std::move(copy));
          chain.push_back(target);
        }
        if (!target)
          continue;
        bool requested_native = false;
        bool requested_any = false;
        for (size_t i = 0; i < group.count; ++i)
          if (read_flag(target, group.fields[i].member)) {
            requested_any = true;
            if (read_flag(native.aggregate(group.version),
                          group.fields[i].aggregate))
              requested_native = true;
            else if (!emulated(group, group.fields[i])) {
              status = VK_ERROR_FEATURE_NOT_PRESENT;
              return;
            }
          }
        if (!requested_any) {
          chain.erase(std::remove(chain.begin(), chain.end(), target),
                      chain.end());
          continue;
        }
        if (requested_native && native.api < group.promoted)
          add_extension(extensions, group.extension);
      }
      if (v12)
        for (const auto &alias : feature_aliases)
          if (read_flag(v12, alias.member)) {
            if (!read_flag(&native.v12, alias.member)) {
              status = VK_ERROR_FEATURE_NOT_PRESENT;
              return;
            }
            add_extension(extensions, alias.extension);
          }
    }
    for (size_t i = 0; i < chain.size(); ++i)
      chain[i]->pNext = i + 1 < chain.size() ? chain[i + 1] : nullptr;
    info.pNext = chain.empty() ? nullptr : chain.front();
  }
};
PromotedDeviceFeatures::PromotedDeviceFeatures(
    VkPhysicalDevice physical, VkDeviceCreateInfo &info,
    std::vector<const char *> &extensions)
    : state(std::make_unique<State>(physical, info, extensions)) {}
PromotedDeviceFeatures::~PromotedDeviceFeatures() = default;
VkResult PromotedDeviceFeatures::result() const { return state->status; }
