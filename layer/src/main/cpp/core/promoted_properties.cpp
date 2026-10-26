#include "promoted_properties.h"
#include "driver_loader.h"
#include "native_instance_state.h"
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {
struct PropertyField {
  size_t aggregate, member, size;
};
struct PropertyGroup {
  unsigned version;
  VkStructureType type;
  const char *extension;
  bool instance;
  const PropertyField *fields;
  size_t count;
  std::shared_ptr<VkBaseOutStructure> (*create)(VkStructureType);
  void (*reset)(VkBaseOutStructure *);
};
template <class T>
std::shared_ptr<VkBaseOutStructure> make_property(VkStructureType type) {
  auto value = std::make_shared<T>();
  value->sType = type;
  return {value, reinterpret_cast<VkBaseOutStructure *>(value.get())};
}
template <class T> void reset_property(VkBaseOutStructure *base) {
  auto next = base->pNext;
  auto type = base->sType;
  *reinterpret_cast<T *>(base) = {};
  base->sType = type;
  base->pNext = next;
}
#include "promoted_property_metadata.inc"

const PropertyGroup *group_for(VkStructureType type) {
  for (auto &group : property_groups)
    if (group.type == type)
      return &group;
  return nullptr;
}
template <class Enumerate>
std::set<std::string> enumerate_extensions(Enumerate enumerate) {
  std::set<std::string> names;
  for (unsigned attempt = 0; attempt < 8; ++attempt) {
    uint32_t count = 0;
    if (enumerate(&count, nullptr) != VK_SUCCESS)
      break;
    std::vector<VkExtensionProperties> values(count);
    auto result = count ? enumerate(&count, values.data()) : VK_SUCCESS;
    if (result == VK_INCOMPLETE)
      continue;
    if (result == VK_SUCCESS)
      for (uint32_t i = 0; i < count; ++i)
        names.insert(values[i].extensionName);
    break;
  }
  return names;
}
using Links =
    std::vector<std::pair<VkBaseOutStructure *, VkBaseOutStructure *>>;
Links save_links(void *head) {
  Links result;
  for (auto node = static_cast<VkBaseOutStructure *>(head); node;
       node = node->pNext)
    result.emplace_back(node, node->pNext);
  return result;
}
void restore_links(const Links &links) {
  for (auto entry : links)
    entry.first->pNext = entry.second;
}
} // namespace

PFN_vkGetPhysicalDeviceProperties2
native_properties2_proc(VkPhysicalDevice physical) {
  auto instance = vkfix::native_instance_for_physical(physical);
  if (!instance)
    instance = get_last_instance();
  if (auto gipa = get_real_instance_proc_addr()) {
    auto proc = (PFN_vkGetPhysicalDeviceProperties2)gipa(
        instance, "vkGetPhysicalDeviceProperties2");
    return proc ? proc
                : (PFN_vkGetPhysicalDeviceProperties2)gipa(
                      instance, "vkGetPhysicalDeviceProperties2KHR");
  }
  auto proc = (PFN_vkGetPhysicalDeviceProperties2)get_real_proc(
      instance, VK_NULL_HANDLE, "vkGetPhysicalDeviceProperties2");
  return proc ? proc
              : (PFN_vkGetPhysicalDeviceProperties2)get_real_proc(
                    instance, VK_NULL_HANDLE,
                    "vkGetPhysicalDeviceProperties2KHR");
}

struct PromotedPropertyQuery::State {
  VkPhysicalDevice physical;
  VkInstance instance;
  VkPhysicalDeviceProperties2 *root;
  uint32_t api = VK_API_VERSION_1_0;
  std::set<std::string> device_extensions, instance_extensions;
  Links original;
  std::vector<std::shared_ptr<VkBaseOutStructure>> expanded;
  VkPhysicalDeviceVulkan11Properties *v11 = nullptr;
  VkPhysicalDeviceVulkan12Properties *v12 = nullptr;
  bool supported(const PropertyGroup &group) const {
    return api >= (group.version == 11 ? VK_API_VERSION_1_1
                                       : VK_API_VERSION_1_2) ||
           (*group.extension &&
            (group.instance ? instance_extensions : device_extensions)
                .count(group.extension));
  }
  State(VkPhysicalDevice physical, VkPhysicalDeviceProperties2 *output)
      : physical(physical), instance(vkfix::native_instance_for_physical(physical)), root(output) {
    if (!instance) instance = get_last_instance();
    if (!root)
      return;
    auto props = (PFN_vkGetPhysicalDeviceProperties)get_real_proc(
        instance, VK_NULL_HANDLE, "vkGetPhysicalDeviceProperties");
    if (props) {
      VkPhysicalDeviceProperties native{};
      props(physical, &native);
      api = native.apiVersion;
    }
    if (api >= VK_API_VERSION_1_2)
      return;
    auto dev_ext = (PFN_vkEnumerateDeviceExtensionProperties)get_real_proc(
        instance, VK_NULL_HANDLE,
        "vkEnumerateDeviceExtensionProperties");
    if (dev_ext)
      device_extensions =
          enumerate_extensions([&](uint32_t *n, VkExtensionProperties *e) {
            return dev_ext(physical, nullptr, n, e);
          });
    if (api < VK_API_VERSION_1_1) {
      auto context = vkfix::native_state_for_physical(physical);
      auto inst_ext = (PFN_vkEnumerateInstanceExtensionProperties)get_real_proc(
          VK_NULL_HANDLE, VK_NULL_HANDLE,
          "vkEnumerateInstanceExtensionProperties");
      if (context)
        instance_extensions = context->enabled_extensions;
      else if (inst_ext)
        instance_extensions =
            enumerate_extensions([&](uint32_t *n, VkExtensionProperties *e) {
              return inst_ext(nullptr, n, e);
            });
    }
    original = save_links(root->pNext);
    VkBaseOutStructure *head = nullptr;
    auto tail = &head;
    for (auto entry : original) {
      auto node = entry.first;
      if (node->sType ==
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES) {
        v11 = reinterpret_cast<VkPhysicalDeviceVulkan11Properties *>(node);
        auto next = v11->pNext;
        *v11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
        v11->pNext = next;
      } else if (node->sType ==
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES) {
        v12 = reinterpret_cast<VkPhysicalDeviceVulkan12Properties *>(node);
        auto next = v12->pNext;
        *v12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
        v12->pNext = next;
      } else {
        if (auto group = group_for(node->sType))
          group->reset(node);
        *tail = node;
        tail = &node->pNext;
      }
    }
    *tail = nullptr;
    root->pNext = head;
    for (auto &group : property_groups) {
      if (!(group.version == 11 ? (void *)v11 : (void *)v12))
        continue;
      // Valid aggregate chains do not also contain promoted children.
      // Reuse an existing child defensively, avoiding duplicate sTypes.
      auto found =
          std::find_if(original.begin(), original.end(), [&](auto entry) {
            return entry.first->sType == group.type;
          });
      if (found != original.end())
        continue;
      expanded.push_back(group.create(group.type));
      auto node = expanded.back().get();
      node->pNext = static_cast<VkBaseOutStructure *>(root->pNext);
      root->pNext = node;
    }
  }
  void query_native() {
    if (!root)
      return;
    auto links = save_links(root->pNext);
    VkBaseOutStructure *head = nullptr;
    auto tail = &head;
    for (auto entry : links) {
      auto group = group_for(entry.first->sType);
      if (api >= VK_API_VERSION_1_2 || !group || supported(*group)) {
        *tail = entry.first;
        tail = &entry.first->pNext;
      }
    }
    *tail = nullptr;
    root->pNext = head;
    if (auto proc = native_properties2_proc(physical))
      proc(physical, root);
    else if (auto proc = (PFN_vkGetPhysicalDeviceProperties)get_real_proc(
                 instance, VK_NULL_HANDLE,
                 "vkGetPhysicalDeviceProperties"))
      proc(physical, &root->properties);
    restore_links(links);
    root->pNext = links.empty() ? nullptr : links.front().first;
  }
  VkSampleCountFlags integer_samples() const {
    auto format_props = (PFN_vkGetPhysicalDeviceFormatProperties)get_real_proc(
        instance, VK_NULL_HANDLE,
        "vkGetPhysicalDeviceFormatProperties");
    auto image_props =
        (PFN_vkGetPhysicalDeviceImageFormatProperties)get_real_proc(
            instance, VK_NULL_HANDLE,
            "vkGetPhysicalDeviceImageFormatProperties");
    if (!format_props || !image_props)
      return VK_SAMPLE_COUNT_1_BIT;
    VkSampleCountFlags result = ~VkSampleCountFlags(0);
    bool found = false;
    for (auto format : integer_color_formats) {
      VkFormatProperties props{};
      format_props(physical, format, &props);
      if (!(props.optimalTilingFeatures &
            VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))
        continue;
      VkImageFormatProperties image{};
      if (image_props(
              physical, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0, &image) == VK_SUCCESS) {
        result &= image.sampleCounts;
        found = true;
      }
    }
    return found ? result : VK_SAMPLE_COUNT_1_BIT;
  }
  ~State() {
    if (!root || api >= VK_API_VERSION_1_2)
      return;
    // Module post-hooks have now filled any genuinely emulated properties.
    for (auto node = static_cast<VkBaseOutStructure *>(root->pNext); node;
         node = node->pNext) {
      auto group = group_for(node->sType);
      if (!group)
        continue;
      void *aggregate = group->version == 11 ? (void *)v11 : (void *)v12;
      if (!aggregate)
        continue;
      for (size_t i = 0; i < group->count; ++i) {
        auto field = group->fields[i];
        memcpy(static_cast<char *>(aggregate) + field.aggregate,
               reinterpret_cast<char *>(node) + field.member, field.size);
      }
    }
    if (v12)
      v12->framebufferIntegerColorSampleCounts = integer_samples();
    restore_links(original);
    root->pNext = original.empty() ? nullptr : original.front().first;
  }
};
PromotedPropertyQuery::PromotedPropertyQuery(
    VkPhysicalDevice physical, VkPhysicalDeviceProperties2 *output)
    : state(std::make_unique<State>(physical, output)) {}
PromotedPropertyQuery::~PromotedPropertyQuery() = default;
void PromotedPropertyQuery::query_native() { state->query_native(); }
uint32_t PromotedPropertyQuery::native_api_version() const { return state->api; }
