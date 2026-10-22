#include "shader/shader_rewriter.h"

#include "vk_common.h"

#include <spirv-tools/libspirv.hpp>
#include <spirv/unified1/spirv.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

namespace {

constexpr uint32_t kSpirvMagic = 0x07230203u;
constexpr size_t kSpirvHeaderWords = 5;
constexpr size_t kMaxCachedModules = 4096;

// Optional capabilities that older drivers frequently reject. They are only
// dropped when a version downgrade is already happening, and the validator
// below decides whether the module really does not need them.
const uint32_t kDefaultStripCaps[] = {
    static_cast<uint32_t>(spv::CapabilityShaderNonUniform),
    static_cast<uint32_t>(spv::CapabilityRuntimeDescriptorArray),
    static_cast<uint32_t>(spv::CapabilityInputAttachmentArrayDynamicIndexing),
    static_cast<uint32_t>(spv::CapabilityUniformTexelBufferArrayDynamicIndexing),
    static_cast<uint32_t>(spv::CapabilityStorageTexelBufferArrayDynamicIndexing),
    static_cast<uint32_t>(spv::CapabilityUniformBufferArrayNonUniformIndexing),
    static_cast<uint32_t>(spv::CapabilitySampledImageArrayNonUniformIndexing),
    static_cast<uint32_t>(spv::CapabilityStorageBufferArrayNonUniformIndexing),
    static_cast<uint32_t>(spv::CapabilityStorageImageArrayNonUniformIndexing),
    static_cast<uint32_t>(spv::CapabilityInputAttachmentArrayNonUniformIndexing),
    static_cast<uint32_t>(spv::CapabilityUniformTexelBufferArrayNonUniformIndexing),
    static_cast<uint32_t>(spv::CapabilityStorageTexelBufferArrayNonUniformIndexing),
    static_cast<uint32_t>(spv::CapabilityStorageBuffer16BitAccess),
    static_cast<uint32_t>(spv::CapabilityUniformAndStorageBuffer16BitAccess),
    static_cast<uint32_t>(spv::CapabilityStoragePushConstant16),
    static_cast<uint32_t>(spv::CapabilityStorageInputOutput16),
    static_cast<uint32_t>(spv::CapabilityStorageBuffer8BitAccess),
    static_cast<uint32_t>(spv::CapabilityUniformAndStorageBuffer8BitAccess),
    static_cast<uint32_t>(spv::CapabilityStoragePushConstant8),
    static_cast<uint32_t>(spv::CapabilityGroupNonUniform),
    static_cast<uint32_t>(spv::CapabilityGroupNonUniformVote),
    static_cast<uint32_t>(spv::CapabilityGroupNonUniformArithmetic),
    static_cast<uint32_t>(spv::CapabilityGroupNonUniformBallot),
    static_cast<uint32_t>(spv::CapabilityGroupNonUniformShuffle),
    static_cast<uint32_t>(spv::CapabilityGroupNonUniformShuffleRelative),
    static_cast<uint32_t>(spv::CapabilityGroupNonUniformClustered),
    static_cast<uint32_t>(spv::CapabilityGroupNonUniformQuad),
    static_cast<uint32_t>(spv::CapabilityPhysicalStorageBufferAddresses),
    static_cast<uint32_t>(spv::CapabilityVulkanMemoryModel),
    static_cast<uint32_t>(spv::CapabilityVulkanMemoryModelDeviceScope),
    static_cast<uint32_t>(spv::CapabilityDemoteToHelperInvocation),
};

const char* const kDefaultStripExtensions[] = {
    "SPV_EXT_descriptor_indexing",
    "SPV_KHR_16bit_storage",
    "SPV_KHR_8bit_storage",
    "SPV_KHR_vulkan_memory_model",
    "SPV_EXT_physical_storage_buffer",
};

bool env_truthy(const char* value) {
    if (value == nullptr || value[0] == '\0') return false;
    return std::strcmp(value, "1") == 0 || strcasecmp(value, "true") == 0 ||
           strcasecmp(value, "on") == 0 || strcasecmp(value, "yes") == 0;
}

uint32_t parse_version(const char* text) {
    if (text == nullptr || text[0] == '\0') return 0;
    unsigned major = 0;
    unsigned minor = 0;
    if (std::sscanf(text, "%u.%u", &major, &minor) != 2) return 0;
    if (major > 1 || minor > 6) return 0;
    return (major << 16) | (minor << 8);
}

spv_target_env env_for_version(uint32_t version) {
    switch (version) {
        case 0x00010000u: return SPV_ENV_VULKAN_1_0;
        case 0x00010300u: return SPV_ENV_VULKAN_1_1;
        case 0x00010500u: return SPV_ENV_VULKAN_1_2;
        case 0x00010600u: return SPV_ENV_VULKAN_1_3;
        default: break;
    }
    switch ((version >> 8) & 0xFFu) {
        case 0: return SPV_ENV_UNIVERSAL_1_0;
        case 1: return SPV_ENV_UNIVERSAL_1_1;
        case 2: return SPV_ENV_UNIVERSAL_1_2;
        case 3: return SPV_ENV_UNIVERSAL_1_3;
        case 4: return SPV_ENV_UNIVERSAL_1_4;
        case 5: return SPV_ENV_UNIVERSAL_1_5;
        default: return SPV_ENV_UNIVERSAL_1_6;
    }
}

// Name table for VULKAN_FIX_SPIRV_STRIP_CAPS. Kept explicit so we do not depend
// on spv::CapabilityToString (which is behind SPV_ENABLE_UTILITY_CODE).
#define VKFIX_CAP(name) {#name, spv::Capability##name}
const struct {
    const char* name;
    spv::Capability capability;
} kKnownCapabilities[] = {
    VKFIX_CAP(ShaderNonUniform),
    VKFIX_CAP(RuntimeDescriptorArray),
    VKFIX_CAP(InputAttachmentArrayDynamicIndexing),
    VKFIX_CAP(UniformTexelBufferArrayDynamicIndexing),
    VKFIX_CAP(StorageTexelBufferArrayDynamicIndexing),
    VKFIX_CAP(UniformBufferArrayNonUniformIndexing),
    VKFIX_CAP(SampledImageArrayNonUniformIndexing),
    VKFIX_CAP(StorageBufferArrayNonUniformIndexing),
    VKFIX_CAP(StorageImageArrayNonUniformIndexing),
    VKFIX_CAP(InputAttachmentArrayNonUniformIndexing),
    VKFIX_CAP(UniformTexelBufferArrayNonUniformIndexing),
    VKFIX_CAP(StorageTexelBufferArrayNonUniformIndexing),
    VKFIX_CAP(StorageBuffer16BitAccess),
    VKFIX_CAP(UniformAndStorageBuffer16BitAccess),
    VKFIX_CAP(StoragePushConstant16),
    VKFIX_CAP(StorageInputOutput16),
    VKFIX_CAP(StorageBuffer8BitAccess),
    VKFIX_CAP(UniformAndStorageBuffer8BitAccess),
    VKFIX_CAP(StoragePushConstant8),
    VKFIX_CAP(GroupNonUniform),
    VKFIX_CAP(GroupNonUniformVote),
    VKFIX_CAP(GroupNonUniformArithmetic),
    VKFIX_CAP(GroupNonUniformBallot),
    VKFIX_CAP(GroupNonUniformShuffle),
    VKFIX_CAP(GroupNonUniformShuffleRelative),
    VKFIX_CAP(GroupNonUniformClustered),
    VKFIX_CAP(GroupNonUniformQuad),
    VKFIX_CAP(PhysicalStorageBufferAddresses),
    VKFIX_CAP(VulkanMemoryModel),
    VKFIX_CAP(VulkanMemoryModelDeviceScope),
    VKFIX_CAP(DemoteToHelperInvocation),
};
#undef VKFIX_CAP

// Resolve a capability by name (known table) or by decimal id.
uint32_t resolve_capability(const std::string& token) {
    if (token.empty()) return 0;
    char* end = nullptr;
    const unsigned long numeric = std::strtoul(token.c_str(), &end, 10);
    if (end != nullptr && *end == '\0' && numeric < 0x10000ul) {
        return static_cast<uint32_t>(numeric);
    }
    for (const auto& entry : kKnownCapabilities) {
        if (token == entry.name) return static_cast<uint32_t>(entry.capability);
    }
    return 0;
}

void split_csv(const char* text, std::vector<std::string>& out) {
    if (text == nullptr) return;
    std::string current;
    for (const char* p = text;; ++p) {
        const char ch = *p;
        if (ch == ',' || ch == ';' || ch == '\0') {
            if (!current.empty()) out.push_back(current);
            current.clear();
            if (ch == '\0') break;
        } else if (ch != ' ' && ch != '\t') {
            current.push_back(ch);
        }
    }
}

bool is_debug_opcode(uint16_t opcode) {
    switch (opcode) {
        case 2:   // OpSourceContinued
        case 3:   // OpSource
        case 4:   // OpSourceExtension
        case 5:   // OpName
        case 6:   // OpMemberName
        case 7:   // OpString
        case 8:   // OpLine
        case 317: // OpNoLine
        case 330: // OpModuleProcessed
            return true;
        default:
            return false;
    }
}

std::string read_literal_string(const uint32_t* words, size_t word_count) {
    const char* bytes = reinterpret_cast<const char*>(words);
    const size_t max_len = word_count * sizeof(uint32_t);
    size_t len = 0;
    while (len < max_len && bytes[len] != '\0') ++len;
    return std::string(bytes, len);
}

}  // namespace

ShaderRewriter& ShaderRewriter::get() {
    static ShaderRewriter s_instance;
    return s_instance;
}

void ShaderRewriter::ensure_initialized() {
    std::call_once(m_init_once, []() {
        ShaderRewriter& self = get();

        if (const char* enabled = std::getenv("VULKAN_FIX_SHADER_REWRITE"); enabled != nullptr) {
            self.m_enabled = env_truthy(enabled);
        }
        self.m_env_target = parse_version(std::getenv("VULKAN_FIX_SPIRV_MAX_VERSION"));
        self.m_strip_debug = env_truthy(std::getenv("VULKAN_FIX_SPIRV_STRIP_DEBUG"));

        const bool keep_caps = env_truthy(std::getenv("VULKAN_FIX_SPIRV_KEEP_CAPS"));
        if (!keep_caps) {
            for (uint32_t cap : kDefaultStripCaps) self.m_strip_caps.insert(cap);
            for (const char* ext : kDefaultStripExtensions) self.m_strip_extensions.emplace_back(ext);
        }

        std::vector<std::string> tokens;
        split_csv(std::getenv("VULKAN_FIX_SPIRV_STRIP_CAPS"), tokens);
        for (const std::string& token : tokens) {
            const uint32_t cap = resolve_capability(token);
            if (cap != 0) {
                self.m_strip_caps.insert(cap);
            } else {
                LOGW("ShaderRewriter: unknown capability '%s' in VULKAN_FIX_SPIRV_STRIP_CAPS", token.c_str());
            }
        }
        tokens.clear();
        split_csv(std::getenv("VULKAN_FIX_SPIRV_STRIP_EXTENSIONS"), tokens);
        for (const std::string& token : tokens) self.m_strip_extensions.push_back(token);

        if (!self.m_enabled) {
            LOGI("ShaderRewriter: disabled by VULKAN_FIX_SHADER_REWRITE");
        } else {
            LOGI("ShaderRewriter: enabled (target %s, %zu strip capabilities, %zu strip extensions, debug %s)",
                 self.m_env_target != 0 ? spirv_version_string(self.m_env_target) : "per-device",
                 self.m_strip_caps.size(), self.m_strip_extensions.size(),
                 self.m_strip_debug ? "stripped" : "kept");
        }
    });
}

uint32_t ShaderRewriter::api_version_to_spirv(uint32_t apiVersion) {
    // Vulkan API version -> maximum SPIR-V version (Vulkan spec, "SPIR-V
    // versions"): 1.0 -> 1.0, 1.1 -> 1.3, 1.2 -> 1.5, 1.3 -> 1.6.
    if (apiVersion >= VK_API_VERSION_1_3) return 0x00010600u;
    if (apiVersion >= VK_API_VERSION_1_2) return 0x00010500u;
    if (apiVersion >= VK_API_VERSION_1_1) return 0x00010300u;
    return 0x00010000u;
}

const char* ShaderRewriter::spirv_version_string(uint32_t version) {
    static thread_local char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%u.%u", (version >> 16) & 0xFFu, (version >> 8) & 0xFFu);
    return buffer;
}

uint64_t ShaderRewriter::hash_code(const std::vector<uint32_t>& code, uint32_t target) {
    uint64_t hash = 1469598103934665603ull;
    for (uint32_t word : code) {
        hash ^= word;
        hash *= 1099511628211ull;
    }
    hash ^= static_cast<uint64_t>(target) << 32;
    hash *= 1099511628211ull;
    return hash;
}

bool ShaderRewriter::validate(const std::vector<uint32_t>& code, uint32_t version) {
    spvtools::SpirvTools tools(env_for_version(version));
    tools.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t&, const char* message) {
        if (message != nullptr) {
            LOG_OPT_DEBUG("spirv-val: %s", message);
        }
    });
    return tools.Validate(code);
}

bool ShaderRewriter::rewrite(std::vector<uint32_t>& code, uint32_t targetVersion) {
    ensure_initialized();
    if (!m_enabled) return false;

    const uint32_t target = (m_env_target != 0) ? m_env_target : targetVersion;
    if (target == 0) return false;
    if (code.size() < kSpirvHeaderWords || code[0] != kSpirvMagic) return false;

    const uint32_t current = code[1];
    if (current <= target) return false;

    const uint64_t key = hash_code(code, target);
    {
        std::lock_guard<std::mutex> lock(m_cache_mutex);
        auto it = m_cache.find(key);
        if (it != m_cache.end()) {
            if (!it->second.changed) return false;
            code = it->second.code;
            return true;
        }
    }

    const std::vector<uint32_t> original = code;
    std::vector<uint32_t> output;
    output.reserve(code.size());
    output.insert(output.end(), code.begin(), code.begin() + kSpirvHeaderWords);

    bool dropped_declaration = false;
    size_t offset = kSpirvHeaderWords;
    bool malformed = false;
    while (offset < code.size()) {
        const uint32_t word = code[offset];
        const uint32_t word_count = word >> 16;
        const uint16_t opcode = static_cast<uint16_t>(word & 0xFFFFu);
        if (word_count == 0 || offset + word_count > code.size()) {
            malformed = true;
            break;
        }

        bool drop = false;
        if (m_strip_debug && is_debug_opcode(opcode)) {
            drop = true;
        } else if (opcode == static_cast<uint16_t>(spv::OpCapability) && word_count >= 2) {
            drop = m_strip_caps.find(code[offset + 1]) != m_strip_caps.end();
        } else if (opcode == static_cast<uint16_t>(spv::OpExtension) && word_count >= 2) {
            const std::string name = read_literal_string(&code[offset + 1], word_count - 1);
            for (const std::string& stripped : m_strip_extensions) {
                if (name == stripped) {
                    drop = true;
                    break;
                }
            }
        }

        if (drop) {
            dropped_declaration = true;
        } else {
            output.insert(output.end(), code.begin() + offset, code.begin() + offset + word_count);
        }
        offset += word_count;
    }

    if (malformed) {
        LOG_OPT_DEBUG("ShaderRewriter: malformed module, keeping original");
        return false;
    }

    output[1] = target;

    if (!validate(output, target)) {
        {
            std::lock_guard<std::mutex> lock(m_cache_mutex);
            if (m_cache.size() >= kMaxCachedModules) m_cache.clear();
            m_cache[key] = CacheEntry{false, {}};
        }
        const uint64_t rejected = m_rejected.fetch_add(1, std::memory_order_relaxed) + 1;
        LOG_OPT_DEBUG("ShaderRewriter: %s -> %s rejected by validator, keeping original",
                      spirv_version_string(current), spirv_version_string(target));
        if ((rejected & 0x7F) == 0) {
            LOGI("ShaderRewriter: %llu modules rejected (kept original), %llu downgraded",
                 static_cast<unsigned long long>(rejected),
                 static_cast<unsigned long long>(m_rewritten.load(std::memory_order_relaxed)));
        }
        return false;
    }

    code = std::move(output);
    {
        std::lock_guard<std::mutex> lock(m_cache_mutex);
        if (m_cache.size() >= kMaxCachedModules) m_cache.clear();
        m_cache[key] = CacheEntry{true, code};
    }
    const uint64_t rewritten = m_rewritten.fetch_add(1, std::memory_order_relaxed) + 1;
    LOG_OPT_DEBUG("ShaderRewriter: downgraded SPIR-V %s -> %s (declarations stripped: %d)",
                  spirv_version_string(current), spirv_version_string(target),
                  dropped_declaration ? 1 : 0);
    if ((rewritten & 0x7F) == 0) {
        LOGI("ShaderRewriter: %llu modules downgraded, %llu rejected",
             static_cast<unsigned long long>(rewritten),
             static_cast<unsigned long long>(m_rejected.load(std::memory_order_relaxed)));
    }
    return true;
}
