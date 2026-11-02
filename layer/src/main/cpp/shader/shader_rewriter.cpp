#include "shader/shader_rewriter.h"
#include "shader/storage_buffer_lowering.h"

#include "vk_common.h"

#include <spirv-tools/libspirv.hpp>
#include <spirv/unified1/spirv.hpp>

#include <algorithm>
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
    static_cast<uint32_t>(spv::CapabilityDemoteToHelperInvocation),
};

const char* const kDefaultStripExtensions[] = {
    "SPV_EXT_demote_to_helper_invocation",
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

// Core SPIR-V capabilities need their original extension declarations again
// when targeting a version preceding their promotion. Only use routes enabled
// on this logical device; advertised wrapper extensions do not establish one.
bool restore_promoted_extensions(std::vector<uint32_t>& code, uint32_t version,
                                 uint32_t native_extensions,
                                 const std::vector<std::string>& stripped) {
    std::unordered_set<uint32_t> capabilities;
    std::unordered_set<std::string> extensions;
    size_t insertion = kSpirvHeaderWords;
    for (size_t at = kSpirvHeaderWords; at < code.size();) {
        const uint32_t count = code[at] >> 16;
        const auto op = static_cast<spv::Op>(code[at] & 0xffffu);
        if (!count || count > code.size() - at) return false;
        if (op == spv::OpCapability && count == 2)
            capabilities.insert(code[at + 1]);
        if (op == spv::OpExtension && count >= 2)
            extensions.insert(read_literal_string(code.data() + at + 1, count - 1));
        if (op == spv::OpCapability || op == spv::OpExtension) insertion = at + count;
        at += count;
    }
    std::vector<uint32_t> declarations;
    auto has = [&](spv::Capability capability) {
        return capabilities.count(static_cast<uint32_t>(capability)) != 0;
    };
    auto add = [&](uint32_t flags, const char* extension) {
        if ((native_extensions & flags) != flags || extensions.count(extension) ||
            std::find(stripped.begin(), stripped.end(), extension) != stripped.end()) return;
        extensions.insert(extension);
        const size_t words = (std::strlen(extension) + 1 + 3) / 4;
        const size_t at = declarations.size();
        declarations.resize(at + words + 1, 0);
        declarations[at] = (static_cast<uint32_t>(words + 1) << 16) | spv::OpExtension;
        std::memcpy(declarations.data() + at + 1, extension, std::strlen(extension));
    };
    if (version < 0x00010300u) {
        if (vkfix::has_storage_buffer_class(code.data(), code.size()))
            add(vkfix::NativeStorageBufferClass, "SPV_KHR_storage_buffer_storage_class");
        if (has(spv::CapabilityMultiView))
            add(vkfix::NativeMultiview, "SPV_KHR_multiview");
        if (has(spv::CapabilityDeviceGroup))
            add(vkfix::NativeDeviceGroup, "SPV_KHR_device_group");
        if (has(spv::CapabilityDrawParameters))
            add(vkfix::NativeDrawParameters, "SPV_KHR_shader_draw_parameters");
        if (has(spv::CapabilityStorageBuffer16BitAccess) ||
            has(spv::CapabilityUniformAndStorageBuffer16BitAccess) ||
            has(spv::CapabilityStoragePushConstant16) || has(spv::CapabilityStorageInputOutput16))
            add(vkfix::Native16BitStorage, "SPV_KHR_16bit_storage");
        if (has(spv::CapabilityVariablePointers) || has(spv::CapabilityVariablePointersStorageBuffer)) {
            const uint32_t flags = vkfix::NativeVariablePointers | vkfix::NativeStorageBufferClass;
            add(flags, "SPV_KHR_storage_buffer_storage_class");
            add(flags, "SPV_KHR_variable_pointers");
        }
    }
    if (version < 0x00010500u &&
        (has(spv::CapabilityVulkanMemoryModel) || has(spv::CapabilityVulkanMemoryModelDeviceScope)))
        add(vkfix::NativeVulkanMemoryModel, "SPV_KHR_vulkan_memory_model");
    code.insert(code.begin() + insertion, declarations.begin(), declarations.end());
    return true;
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

        if (self.m_enabled) {
            self.m_disk_cache_ready = true;
            const bool custom_policy =
                std::getenv("VULKAN_FIX_SPIRV_STRIP_CAPS") != nullptr ||
                std::getenv("VULKAN_FIX_SPIRV_STRIP_EXTENSIONS") != nullptr ||
                std::getenv("VULKAN_FIX_SPIRV_MAX_VERSION") != nullptr ||
                std::getenv("VULKAN_FIX_SPIRV_KEEP_CAPS") != nullptr;
            if (!custom_policy) {
                const char* override_path = std::getenv("VULKAN_FIX_SHADER_CACHE_PATH");
                if (override_path != nullptr && override_path[0] != '\0') {
                    self.m_cache_path = override_path;
                } else if (const char* home = std::getenv("HOME")) {
                    self.m_cache_path = std::string(home) + "/vulkanfix_shader_cache.bin";
                }
                if (!self.m_cache_path.empty()) {
                    self.load_disk_cache();
                }
            }
        }
    });
}

ShaderRewriter::~ShaderRewriter() {
    save_disk_cache();
}

void ShaderRewriter::load_disk_cache() {
    constexpr uint32_t kDiskCacheMagic = 0x564B4658u;  // "VKFX"
    constexpr uint32_t kDiskCacheVersion = 6u;
    FILE* file = std::fopen(m_cache_path.c_str(), "rb");
    if (file == nullptr) return;

    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t count = 0;
    if (std::fread(&magic, sizeof(magic), 1, file) != 1 ||
        std::fread(&version, sizeof(version), 1, file) != 1 ||
        std::fread(&count, sizeof(count), 1, file) != 1 ||
        magic != kDiskCacheMagic || version != kDiskCacheVersion || count > 65536u) {
        std::fclose(file);
        return;
    }

    std::lock_guard<std::mutex> lock(m_cache_mutex);
    size_t loaded = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t hash = 0;
        uint8_t changed = 0;
        uint32_t words = 0;
        if (std::fread(&hash, sizeof(hash), 1, file) != 1 ||
            std::fread(&changed, sizeof(changed), 1, file) != 1 ||
            std::fread(&words, sizeof(words), 1, file) != 1 ||
            words > (4u * 1024u * 1024u)) {
            break;
        }
        CacheEntry entry;
        entry.changed = changed != 0;
        entry.code.resize(words);
        if (words != 0 && std::fread(entry.code.data(), sizeof(uint32_t), words, file) != words) {
            break;
        }
        const size_t bytes = entry.code.size() * sizeof(uint32_t);
        if (m_cache.size() >= kMaxCachedModules || m_cache_bytes + bytes > kMaxCacheBytes) {
            break;
        }
        m_cache[hash] = std::move(entry);
        m_cache_bytes += bytes;
        ++loaded;
    }
    std::fclose(file);
    if (loaded > 0) {
        LOGI("ShaderRewriter: loaded %zu shader rewrites from %s", loaded, m_cache_path.c_str());
    }
}

void ShaderRewriter::save_disk_cache() {
    if (m_cache_path.empty()) return;
    constexpr uint32_t kDiskCacheMagic = 0x564B4658u;  // "VKFX"
    constexpr uint32_t kDiskCacheVersion = 6u;

    std::lock_guard<std::mutex> lock(m_cache_mutex);
    if (m_cache.empty()) return;
    const std::string temporary = m_cache_path + ".tmp";
    FILE* file = std::fopen(temporary.c_str(), "wb");
    if (file == nullptr) return;

    const uint32_t magic = kDiskCacheMagic;
    const uint32_t version = kDiskCacheVersion;
    const uint32_t count = static_cast<uint32_t>(m_cache.size());
    std::fwrite(&magic, sizeof(magic), 1, file);
    std::fwrite(&version, sizeof(version), 1, file);
    std::fwrite(&count, sizeof(count), 1, file);
    for (const auto& entry : m_cache) {
        const uint64_t hash = entry.first;
        const uint8_t changed = entry.second.changed ? 1u : 0u;
        const uint32_t words = static_cast<uint32_t>(entry.second.code.size());
        std::fwrite(&hash, sizeof(hash), 1, file);
        std::fwrite(&changed, sizeof(changed), 1, file);
        std::fwrite(&words, sizeof(words), 1, file);
        if (words != 0) {
            std::fwrite(entry.second.code.data(), sizeof(uint32_t), words, file);
        }
    }
    std::fclose(file);
    std::rename(temporary.c_str(), m_cache_path.c_str());
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

uint64_t ShaderRewriter::hash_code(const std::vector<uint32_t>& code, uint32_t target,
                                 uint32_t native_extensions) {
    uint64_t hash = 1469598103934665603ull;
    for (uint32_t word : code) {
        hash ^= word;
        hash *= 1099511628211ull;
    }
    hash ^= static_cast<uint64_t>(target) << 32;
    hash *= 1099511628211ull;
    hash ^= native_extensions;
    hash *= 1099511628211ull;
    return hash;
}

bool ShaderRewriter::validate(const std::vector<uint32_t>& code, uint32_t version,
                             uint32_t native_extensions) {
    spvtools::SpirvTools tools(env_for_version(version));
    std::vector<std::string> messages;
    tools.SetMessageConsumer([&messages](spv_message_level_t, const char*, const spv_position_t&, const char* message) {
        if (message != nullptr && messages.size() < 8) {
            messages.emplace_back(message);
        }
    });
    spvtools::ValidatorOptions options;
    options.SetRelaxBlockLayout((native_extensions & vkfix::NativeRelaxedBlockLayout) != 0);
    const bool valid = tools.Validate(code.data(), code.size(), options);
    if (!valid) {
        static std::atomic<uint32_t> failures{0};
        const uint32_t index = failures.fetch_add(1, std::memory_order_relaxed);
        if (index < 3) {
            for (const std::string& message : messages) {
                LOGW("spirv-val reject: %s", message.c_str());
            }
        }
    }
    return valid;
}

bool ShaderRewriter::rewrite(std::vector<uint32_t>& code, uint32_t targetVersion,
                            uint32_t native_extensions) {
    ensure_initialized();
    if (!m_enabled) return false;

    const uint32_t target = (m_env_target != 0) ? m_env_target : targetVersion;
    if (target == 0) return false;
    if (code.size() < kSpirvHeaderWords || code[0] != kSpirvMagic) return false;

    const uint32_t current = code[1];
    const bool legacy_storage = target < 0x00010300u &&
        !(native_extensions & vkfix::NativeStorageBufferClass);
    if (current <= target && !(legacy_storage &&
        vkfix::has_storage_buffer_class(code.data(), code.size()))) return false;

    const uint64_t key = hash_code(code, target, native_extensions);
    {
        std::lock_guard<std::mutex> lock(m_cache_mutex);
        auto it = m_cache.find(key);
        if (it != m_cache.end()) {
            if (!it->second.changed) return false;
            code = it->second.code;
            return true;
        }
    }

    std::vector<uint32_t> lowered = code;
    if (legacy_storage && vkfix::has_storage_buffer_class(lowered.data(), lowered.size())) {
        std::string error;
        const auto result=vkfix::lower_storage_buffer_class(lowered,error);
        if (result==vkfix::StorageBufferLowering::NeedsSpecialization) return false;
        if (result == vkfix::StorageBufferLowering::Invalid) {
            LOGW("StorageBuffer lowering rejected: %s", error.c_str());
            return false;
        }
    }
    const auto& input = lowered;
    std::vector<uint32_t> output;
    output.reserve(input.size());
    output.insert(output.end(), input.begin(), input.begin() + kSpirvHeaderWords);

    // SPIR-V 1.4 relaxed OpEntryPoint interfaces to allow all global variables.
    // For 1.3 targets only Input/Output variables are legal, so collect each
    // variable's storage class and filter the interface lists below.
    std::unordered_map<uint32_t, uint32_t> variable_storage;
    uint32_t bool_type_id = 0;
    std::unordered_map<uint32_t, uint32_t> vector_components;
    std::unordered_map<uint32_t, uint32_t> vector_counts;
    std::unordered_map<uint32_t, uint32_t> value_types;
    uint32_t max_id = 0;
    {
        size_t scan = kSpirvHeaderWords;
        while (scan < input.size()) {
            const uint32_t scan_word = input[scan];
            const uint32_t scan_count = scan_word >> 16;
            const uint16_t scan_opcode = static_cast<uint16_t>(scan_word & 0xFFFFu);
            if (scan_count == 0 || scan + scan_count > input.size()) break;
            if (scan_opcode == static_cast<uint16_t>(spv::OpTypeBool) && scan_count >= 2) {
                bool_type_id = input[scan + 1];
                max_id = std::max(max_id, bool_type_id);
            } else if (scan_opcode == static_cast<uint16_t>(spv::OpTypeVector) && scan_count >= 4) {
                vector_components[input[scan + 1]] = input[scan + 2];
                vector_counts[input[scan + 1]] = input[scan + 3];
                max_id = std::max(max_id, input[scan + 1]);
            }
            if (scan_opcode == static_cast<uint16_t>(spv::OpVariable) && scan_count >= 4) {
                variable_storage[input[scan + 2]] = input[scan + 3];
            }
            if (scan_count >= 3) {
                value_types[input[scan + 2]] = input[scan + 1];
                max_id = std::max(max_id, input[scan + 2]);
            }
            scan += scan_count;
        }
    }

    bool dropped_declaration = false;
    size_t replaced_instructions = 0;
    size_t dropped_interfaces = 0;
    size_t type_insert_pos = 0;
    uint32_t next_id = (input.size() > 3 && input[3] > 0) ? input[3] - 1 : max_id;
    std::unordered_map<uint32_t, uint32_t> created_bool_vectors;
    size_t offset = kSpirvHeaderWords;
    bool malformed = false;
    while (offset < input.size()) {
        const uint32_t word = input[offset];
        const uint32_t word_count = word >> 16;
        const uint16_t opcode = static_cast<uint16_t>(word & 0xFFFFu);
        if (word_count == 0 || offset + word_count > input.size()) {
            malformed = true;
            break;
        }

        if (opcode == static_cast<uint16_t>(spv::OpFunction) && type_insert_pos == 0) {
            type_insert_pos = output.size();
        }

        bool drop = false;
        uint16_t replacement_opcode = opcode;
        // SPIR-V 1.6 termination instructions are not valid in the SPIR-V 1.3
        // modules accepted by this driver. OpKill is the valid 1.0 equivalent.
        if (opcode == static_cast<uint16_t>(spv::OpTerminateInvocation) ||
            opcode == static_cast<uint16_t>(spv::OpDemoteToHelperInvocation)) {
            replacement_opcode = static_cast<uint16_t>(spv::OpKill);
        }
        if ((m_strip_debug && is_debug_opcode(opcode)) ||
            (target < 0x00010100u && opcode == static_cast<uint16_t>(spv::OpModuleProcessed))) {
            drop = true;
        } else if (opcode == static_cast<uint16_t>(spv::OpCapability) && word_count >= 2) {
            drop = m_strip_caps.find(input[offset + 1]) != m_strip_caps.end();
        } else if (opcode == static_cast<uint16_t>(spv::OpExtension) && word_count >= 2) {
            const std::string name = read_literal_string(&input[offset + 1], word_count - 1);
            for (const std::string& stripped : m_strip_extensions) {
                if (name == stripped) {
                    drop = true;
                    break;
                }
            }
        }

        if (opcode == static_cast<uint16_t>(spv::OpImageFetch) && word_count >= 6) {
            const uint32_t image_operands = input[offset + 5];
            if ((image_operands & static_cast<uint32_t>(spv::ImageOperandsSignExtendMask)) != 0) {
                const uint32_t new_mask = image_operands & ~static_cast<uint32_t>(spv::ImageOperandsSignExtendMask);
                std::vector<uint32_t> fetch;
                fetch.push_back(0);
                for (size_t i = 1; i < 5; ++i) fetch.push_back(input[offset + i]);
                if (new_mask != 0) {
                    fetch.push_back(new_mask);
                    for (size_t i = 6; i < word_count; ++i) fetch.push_back(input[offset + i]);
                }
                fetch[0] = (static_cast<uint32_t>(fetch.size()) << 16) |
                           static_cast<uint32_t>(spv::OpImageFetch);
                output.insert(output.end(), fetch.begin(), fetch.end());
                ++replaced_instructions;
                offset += word_count;
                continue;
            }
        }
        if (opcode == static_cast<uint16_t>(spv::OpSelect) && word_count >= 6 && bool_type_id != 0) {
            const uint32_t result_type = input[offset + 1];
            auto count_it = vector_counts.find(result_type);
            auto component_it = vector_components.find(result_type);
            auto condition_it = value_types.find(input[offset + 3]);
            if (count_it != vector_counts.end() && component_it != vector_components.end() &&
                condition_it != value_types.end() && condition_it->second == bool_type_id) {
                uint32_t bool_vector = 0;
                for (const auto& vector : vector_counts) {
                    auto component = vector_components.find(vector.first);
                    if (component != vector_components.end() &&
                        component->second == bool_type_id && vector.second == count_it->second) {
                        bool_vector = vector.first;
                        break;
                    }
                }
                if (bool_vector == 0) {
                    auto created = created_bool_vectors.find(count_it->second);
                    if (created != created_bool_vectors.end()) {
                        bool_vector = created->second;
                    } else if (type_insert_pos != 0) {
                        const uint32_t type_id = ++next_id;
                        const uint32_t type_words[4] = {
                            (4u << 16) | static_cast<uint32_t>(spv::OpTypeVector),
                            type_id,
                            bool_type_id,
                            count_it->second,
                        };
                        output.insert(output.begin() + type_insert_pos, type_words, type_words + 4);
                        type_insert_pos += 4;
                        created_bool_vectors[count_it->second] = type_id;
                        bool_vector = type_id;
                    }
                }
                if (bool_vector != 0) {
                    const uint32_t condition = input[offset + 3];
                    const uint32_t construct_id = ++next_id;
                    const uint32_t construct_words = 3 + count_it->second;
                    std::vector<uint32_t> construct;
                    construct.push_back((construct_words << 16) | static_cast<uint32_t>(spv::OpCompositeConstruct));
                    construct.push_back(bool_vector);
                    construct.push_back(construct_id);
                    for (uint32_t i = 0; i < count_it->second; ++i) construct.push_back(condition);
                    output.insert(output.end(), construct.begin(), construct.end());
                    const size_t select_offset = output.size();
                    output.insert(output.end(), input.begin() + offset, input.begin() + offset + word_count);
                    output[select_offset + 3] = construct_id;
                    ++replaced_instructions;
                    offset += word_count;
                    continue;
                }
            }
        }
        if (!drop && opcode == static_cast<uint16_t>(spv::OpEntryPoint) && target < 0x00010400u) {
            size_t interface_start = 3;
            while (interface_start < word_count) {
                const uint32_t name_word = input[offset + interface_start];
                bool terminated = false;
                for (int byte = 0; byte < 4; ++byte) {
                    if (((name_word >> (8 * byte)) & 0xFFu) == 0) {
                        terminated = true;
                        break;
                    }
                }
                ++interface_start;
                if (terminated) break;
            }
            if (interface_start > word_count) interface_start = word_count;

            std::vector<uint32_t> entry;
            entry.reserve(word_count);
            entry.push_back(0);  // header filled in below
            entry.push_back(input[offset + 1]);  // execution model
            entry.push_back(input[offset + 2]);  // entry point id
            for (size_t i = 3; i < interface_start; ++i) {
                entry.push_back(input[offset + i]);
            }
            for (size_t i = interface_start; i < word_count; ++i) {
                const uint32_t interface_id = input[offset + i];
                auto variable = variable_storage.find(interface_id);
                if (variable != variable_storage.end() &&
                    (variable->second == static_cast<uint32_t>(spv::StorageClassInput) ||
                     variable->second == static_cast<uint32_t>(spv::StorageClassOutput))) {
                    entry.push_back(interface_id);
                } else {
                    ++dropped_interfaces;
                }
            }
            entry[0] = (static_cast<uint32_t>(entry.size()) << 16) |
                       static_cast<uint32_t>(spv::OpEntryPoint);
            output.insert(output.end(), entry.begin(), entry.end());
            offset += word_count;
            continue;
        }

        if (drop) {
            dropped_declaration = true;
        } else {
            const size_t output_offset = output.size();
            output.insert(output.end(), input.begin() + offset, input.begin() + offset + word_count);
            if (replacement_opcode != opcode) {
                output[output_offset] = (word & 0xFFFF0000u) | replacement_opcode;
                ++replaced_instructions;
            }
        }
        offset += word_count;
    }

    if (malformed) {
        LOG_OPT_DEBUG("ShaderRewriter: malformed module, keeping original");
        return false;
    }

    if (replaced_instructions > 0) {
        LOG_OPT_DEBUG("ShaderRewriter: replaced %zu SPIR-V 1.6 termination instruction(s) with OpKill",
                      replaced_instructions);
    }

    if (output.size() > 3 && next_id + 1 > output[3]) {
        output[3] = next_id + 1;
    }
    output[1] = target;

    if (!restore_promoted_extensions(output, target, native_extensions, m_strip_extensions)) return false;
    if (!validate(output, target, native_extensions)) {
        {
            std::lock_guard<std::mutex> lock(m_cache_mutex);
            if (m_cache.size() >= kMaxCachedModules) {
                m_cache.clear();
                m_cache_bytes = 0;
            }
            m_cache[key] = CacheEntry{false, {}};
        }
        if ((m_new_entries.fetch_add(1, std::memory_order_relaxed) & 31u) == 31u) {
            save_disk_cache();
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
    const size_t rewritten_bytes = code.size() * sizeof(uint32_t);
    if (rewritten_bytes <= kMaxCacheBytes / 2) {
        std::lock_guard<std::mutex> lock(m_cache_mutex);
        if (m_cache.size() >= kMaxCachedModules || m_cache_bytes + rewritten_bytes > kMaxCacheBytes) {
            m_cache.clear();
            m_cache_bytes = 0;
        }
        m_cache[key] = CacheEntry{true, code};
        m_cache_bytes += rewritten_bytes;
    }
    if (rewritten_bytes <= kMaxCacheBytes / 2 &&
        (m_new_entries.fetch_add(1, std::memory_order_relaxed) & 31u) == 31u) {
        save_disk_cache();
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

bool ShaderRewriter::uses_invocation_ids(const std::vector<uint32_t> &code) {
    if (code.size() < 5 || code[0] != 0x07230203u) return false;
    for (size_t i = 5; i < code.size();) {
        const uint32_t wc = code[i] >> 16;
        if (wc == 0 || i + wc > code.size()) return false;
        if ((code[i] & 0xffff) == 71 && wc >= 4 && code[i + 2] == 11 &&
            (code[i + 3] == spv::BuiltInGlobalInvocationId ||
             code[i + 3] == spv::BuiltInWorkgroupId))
            return true;
        i += wc;
    }
    return false;
}

bool ShaderRewriter::uses_viewport_index(const std::vector<uint32_t> &code) {
    if (code.size() < 5 || code[0] != 0x07230203u) return false;
    for (size_t i = 5; i < code.size();) {
        const uint32_t wc = code[i] >> 16;
        if (wc == 0 || i + wc > code.size()) return false;
        if ((code[i] & 0xffff) == 71 && wc >= 4 && code[i + 2] == 11 &&
            code[i + 3] == spv::BuiltInViewportIndex)
            return true;
        i += wc;
    }
    return false;
}

bool ShaderRewriter::constant_viewport_index(const std::vector<uint32_t> &code,
                                             uint32_t &index) {
    if (!uses_viewport_index(code)) return false;
    uint32_t variable = 0;
    std::unordered_map<uint32_t, uint32_t> constants;
    for (size_t i = 5; i < code.size();) {
        const uint32_t wc = code[i] >> 16;
        if (wc == 0 || i + wc > code.size()) return false;
        const uint32_t op = code[i] & 0xffff;
        if (op == 71 && wc >= 4 && code[i + 2] == 11 &&
            code[i + 3] == spv::BuiltInViewportIndex)
            variable = code[i + 1];
        if (op == 43 && wc >= 4) constants[code[i + 2]] = code[i + 3];
        i += wc;
    }
    if (!variable) return false;
    bool found = false;
    uint32_t value = 0;
    for (size_t i = 5; i < code.size();) {
        const uint32_t wc = code[i] >> 16;
        if (wc == 0 || i + wc > code.size()) return false;
        if ((code[i] & 0xffff) == 62 && wc >= 3 && code[i + 1] == variable) {
            auto constant = constants.find(code[i + 2]);
            if (constant == constants.end()) return false;
            if (found && constant->second != value) return false;
            value = constant->second;
            found = true;
        }
        i += wc;
    }
    if (!found) return false;
    index = value;
    return true;
}
