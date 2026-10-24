#ifndef SHADER_REWRITER_H
#define SHADER_REWRITER_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/**
 * SPIR-V downgrade rewriter.
 *
 * Vulkan drivers accept only SPIR-V modules whose version is <= the maximum
 * implied by their API version (1.0 -> SPIR-V 1.0, 1.1 -> 1.3, 1.2 -> 1.5,
 * 1.3 -> 1.6). Applications built against a newer SDK often emit 1.5/1.6 even
 * when the underlying driver could execute the same instructions at a lower
 * version.
 *
 * The pass pipeline is:
 *   1. drop debug instructions (optional, VULKAN_FIX_SPIRV_STRIP_DEBUG=1);
 *   2. drop OpCapability / OpExtension declarations from the strip set;
 *   3. lower the module version word;
 *   4. validate with SPIRV-Tools (Vulkan environment of the target version).
 * If validation fails the original module is restored untouched, so a downgrade
 * can never turn a working shader into a broken one.
 *
 * Defaults:
 *   - optional high-version capabilities (descriptor indexing, 8/16-bit
 *     storage, subgroup, physical storage buffer, memory model, demote) are
 *     attempted for removal; anything actually used makes validation fail and
 *     the module is kept as-is;
 *   - VULKAN_FIX_SPIRV_KEEP_CAPS=1 keeps all capability declarations;
 *   - VULKAN_FIX_SHADER_REWRITE=0 disables the whole pass;
 *   - VULKAN_FIX_SPIRV_MAX_VERSION=1.0|1.1|...|1.6 overrides the target;
 *   - VULKAN_FIX_SPIRV_STRIP_CAPS=NameOrId,... adds capabilities to the set;
 *   - VULKAN_FIX_SPIRV_STRIP_EXTENSIONS=SPV_x,... adds extension names.
 */
class ShaderRewriter {
public:
    static ShaderRewriter& get();

    // Rewrites `code` in place when a validated downgrade is possible.
    // Returns true when `code` was modified.
    bool rewrite(std::vector<uint32_t>& code, uint32_t targetVersion);

    // Maximum SPIR-V version implied by a Vulkan apiVersion.
    static uint32_t api_version_to_spirv(uint32_t apiVersion);

    // "1.3" style formatting for logs (thread-local buffer).
    static const char* spirv_version_string(uint32_t version);

private:
    ShaderRewriter() = default;
    ~ShaderRewriter();
    ShaderRewriter(const ShaderRewriter&) = delete;
    ShaderRewriter& operator=(const ShaderRewriter&) = delete;

    void ensure_initialized();
    static bool validate(const std::vector<uint32_t>& code, uint32_t version);
    static uint64_t hash_code(const std::vector<uint32_t>& code, uint32_t target);
    void load_disk_cache();
    void save_disk_cache();

    struct CacheEntry {
        bool changed = false;
        std::vector<uint32_t> code;
    };

    std::once_flag m_init_once;
    bool m_enabled = true;
    bool m_strip_debug = false;
    uint32_t m_env_target = 0;  // 0 = use the per-device target
    std::unordered_set<uint32_t> m_strip_caps;
    std::vector<std::string> m_strip_extensions;

    std::mutex m_cache_mutex;
    std::unordered_map<uint64_t, CacheEntry> m_cache;
    size_t m_cache_bytes = 0;
    std::string m_cache_path;
    bool m_disk_cache_ready = false;
    std::atomic<uint32_t> m_new_entries{0};
    static constexpr size_t kMaxCachedModules = 4096;
    static constexpr size_t kMaxCacheBytes = 8 * 1024 * 1024;

    std::atomic<uint64_t> m_rewritten{0};
    std::atomic<uint64_t> m_rejected{0};
};

#endif  // SHADER_REWRITER_H

