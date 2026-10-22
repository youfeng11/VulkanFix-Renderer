#include "shader/shader_rewriter.h"

#include "vk_common.h"

#include <spirv-tools/libspirv.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

namespace {

constexpr uint32_t kSpirvMagic = 0x07230203u;
constexpr size_t kSpirvHeaderWords = 5;
constexpr size_t kMaxCachedModules = 4096;

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

}  // namespace

ShaderRewriter& ShaderRewriter::get() {
    static ShaderRewriter s_instance;
    return s_instance;
}

void ShaderRewriter::ensure_initialized() {
    std::call_once(m_init_once, []() {
        ShaderRewriter& self = get();

        const char* enabled = std::getenv("VULKAN_FIX_SHADER_REWRITE");
        if (enabled != nullptr &&
            (std::strcmp(enabled, "0") == 0 || strcasecmp(enabled, "false") == 0 ||
             strcasecmp(enabled, "off") == 0 || strcasecmp(enabled, "skip") == 0)) {
            self.m_enabled = false;
        }

        const char* forced = std::getenv("VULKAN_FIX_SPIRV_MAX_VERSION");
        self.m_env_target = parse_version(forced);

        if (!self.m_enabled) {
            LOGI("ShaderRewriter: disabled by VULKAN_FIX_SHADER_REWRITE");
        } else if (self.m_env_target != 0) {
            LOGI("ShaderRewriter: forcing SPIR-V target %s (VULKAN_FIX_SPIRV_MAX_VERSION)",
                 spirv_version_string(self.m_env_target));
        } else {
            LOGI("ShaderRewriter: enabled (per-device SPIR-V downgrade with validation)");
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
    // FNV-1a over the module words, mixed with the target version.
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

    uint32_t target = (m_env_target != 0) ? m_env_target : targetVersion;
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

    const uint32_t saved = code[1];
    code[1] = target;

    if (!validate(code, target)) {
        code[1] = saved;
        {
            std::lock_guard<std::mutex> lock(m_cache_mutex);
            if (m_cache.size() >= kMaxCachedModules) m_cache.clear();
            m_cache[key] = CacheEntry{false, {}};
        }
        const uint64_t rejected = m_rejected.fetch_add(1, std::memory_order_relaxed) + 1;
        LOG_OPT_DEBUG("ShaderRewriter: %s -> %s downgrade rejected by validator, keeping original",
                      spirv_version_string(current), spirv_version_string(target));
        if ((rejected & 0x7F) == 0) {
            LOGI("ShaderRewriter: %llu modules rejected (kept original), %llu downgraded",
                 static_cast<unsigned long long>(rejected),
                 static_cast<unsigned long long>(m_rewritten.load(std::memory_order_relaxed)));
        }
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_cache_mutex);
        if (m_cache.size() >= kMaxCachedModules) m_cache.clear();
        m_cache[key] = CacheEntry{true, code};
    }
    const uint64_t rewritten = m_rewritten.fetch_add(1, std::memory_order_relaxed) + 1;
    LOG_OPT_DEBUG("ShaderRewriter: downgraded SPIR-V %s -> %s",
                  spirv_version_string(current), spirv_version_string(target));
    if ((rewritten & 0x7F) == 0) {
        LOGI("ShaderRewriter: %llu modules downgraded, %llu rejected",
             static_cast<unsigned long long>(rewritten),
             static_cast<unsigned long long>(m_rejected.load(std::memory_order_relaxed)));
    }
    return true;
}
