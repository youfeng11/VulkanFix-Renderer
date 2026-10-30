#include "extension_dependencies.h"
#include <algorithm>
#include <cstring>
#include <iterator>

namespace vkfix {
namespace {
enum class DependencyKind { Extension, Version, And, Or };
struct DependencyNode {
  DependencyKind kind;
  uint32_t a;
  uint32_t b;
};
struct ExtensionRecord {
  const char *name;
  bool instance;
  uint32_t promoted_version;
  int promoted_extension;
  int dependency;
};
#include "extension_dependency_metadata.inc"
const ExtensionRecord *find(const char *name) {
  auto it = std::lower_bound(std::begin(extension_records),
                             std::end(extension_records), name,
                             [](const auto &record, const char *value) {
                               return strcmp(record.name, value) < 0;
                             });
  return it != std::end(extension_records) && !strcmp(it->name, name) ? it
                                                                      : nullptr;
}
struct Selection {
  std::vector<const char *> names;
  std::set<std::string> selected;
  std::set<const ExtensionRecord *> active, complete;
  std::string missing;
};
struct Resolver {
  const ExtensionEnvironment &environment;
  bool expression(int index, Selection &state) const {
    if (index < 0)
      return true;
    const auto &node = dependency_nodes[index];
    switch (node.kind) {
    case DependencyKind::Extension:
      return require(extension_records[node.a], state, false);
    case DependencyKind::Version: {
      const auto version = environment.instance_creation
                               ? environment.instance_api
                               : environment.device_api;
      if (VK_API_VERSION_VARIANT(version) == 0 && version >= node.a)
        return true;
      state.missing = "Vulkan core version " +
                      std::to_string(VK_API_VERSION_MAJOR(node.a)) + "." +
                      std::to_string(VK_API_VERSION_MINOR(node.a));
      return false;
    }
    case DependencyKind::And:
      return expression(node.a, state) && expression(node.b, state);
    case DependencyKind::Or: {
      auto left = state, right = state;
      const bool a = expression(node.a, left), b = expression(node.b, right);
      if (a && (!b || left.names.size() <= right.names.size())) {
        state = std::move(left);
        return true;
      }
      if (b) {
        state = std::move(right);
        return true;
      }
      state.missing = left.missing + " or " + right.missing;
      return false;
    }
    }
    return false;
  }
  bool require(const ExtensionRecord &record, Selection &state,
               bool root) const {
    if (!root) {
      const uint32_t api =
          record.instance ? environment.instance_api : environment.device_api;
      if (record.promoted_version && VK_API_VERSION_VARIANT(api) == 0 &&
          api >= record.promoted_version)
        return true;
      if (record.instance && environment.enabled_instance.count(record.name))
        return true;
    }
    if (state.complete.count(&record))
      return true;
    if (state.active.count(&record))
      return state.selected.count(record.name) != 0;
    if (record.instance != environment.instance_creation ||
        !environment.available.count(record.name)) {
      if (!root && record.promoted_extension >= 0) {
        state.active.insert(&record);
        if (require(extension_records[record.promoted_extension], state,
                    false)) {
          state.active.erase(&record);
          return true;
        }
        state.active.erase(&record);
      }
      state.missing = record.name;
      return false;
    }
    if (state.selected.insert(record.name).second)
      state.names.push_back(record.name);
    state.active.insert(&record);
    if (!expression(record.dependency, state))
      return false;
    state.active.erase(&record);
    state.complete.insert(&record);
    return true;
  }
};
} // namespace

ExtensionDependencyPlan
close_extension_dependencies(const std::vector<const char *> &requested,
                             const ExtensionEnvironment &environment) {
  ExtensionDependencyPlan plan;
  Selection state;
  for (auto name : requested) {
    if (!name || !environment.available.count(name)) {
      plan.result = VK_ERROR_EXTENSION_NOT_PRESENT;
      plan.missing = name ? name : "null extension name";
      return plan;
    }
    if (state.selected.insert(name).second)
      state.names.push_back(name);
  }
  Resolver resolver{environment};
  for (auto name : requested) {
    if (const auto record = find(name)) {
      if (!resolver.require(*record, state, true)) {
        plan.result = VK_ERROR_EXTENSION_NOT_PRESENT;
        plan.missing = name + std::string(" requires ") + state.missing;
        return plan;
      }
    }
    // Unknown, natively enumerated extensions are passed through. Their
    // future dependency rules are left to that native implementation.
  }
  plan.names = std::move(state.names);
  return plan;
}
std::vector<const char *>
promoted_instance_extensions(uint32_t public_api,
                             const ExtensionEnvironment &environment) {
  std::vector<const char *> names;
  for (const auto &record : extension_records)
    if (record.instance && record.promoted_version &&
        record.promoted_version <= public_api &&
        record.promoted_version > environment.instance_api &&
        environment.available.count(record.name))
      names.push_back(record.name);
  return names;
}
std::vector<const char *>
vulkan11_device_extensions(uint32_t public_api,
                           const ExtensionEnvironment &environment) {
  std::vector<const char *> names;
  if (public_api < VK_API_VERSION_1_1 ||
      environment.device_api >= VK_API_VERSION_1_1) return names;
  for (const auto &record : extension_records) {
    if (record.instance || record.promoted_version != VK_API_VERSION_1_1 ||
        !environment.available.count(record.name)) continue;
    // Missing prerequisites for an optional native route must not prevent
    // creation of a device that can use the layer's fallback instead.
    auto candidate = names;
    candidate.push_back(record.name);
    auto plan = close_extension_dependencies(candidate, environment);
    if (plan.result == VK_SUCCESS) names = std::move(plan.names);
  }
  return names;
}
} // namespace vkfix
