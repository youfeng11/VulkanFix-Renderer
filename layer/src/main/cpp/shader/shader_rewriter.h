#ifndef SHADER_REWRITER_H
#define SHADER_REWRITER_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

/**
 * SPIR-V downgrade rewriter.
 *
 * Vulkan drivers accept only SPIR-V modules whose version is <= the maximum
 * implied by their API version (1.0 -> SPIR-V 1.0, 1.1 -> 1.3, 1.2 -> 1.5,
 * 1.3 -> 1.6). Applications built against a newer SDK often emit 1.5/1.6 even
 * when the underlying driver could execute the same instructions at a lower
 * version. This rewriter lowers the module header and validates the result
 * with SPIRV-Tools; if validation fails the original module is kept untouched,
 * so a downgrade can never turn a working shader into a broken one.
 *
 * Enabled by default; VULKAN_FIX_SHADER_REWRITE=0 disables it.
 * VULKAN_FIX_SPIRV_MAX_VERSION=1.0|1.1|1.2|1.3|1.4|1.5|1.6 overrides the
 * per-device target (useful for testing on a modern device).
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
    ~ShaderRewriter() = default;
    ShaderRewriter(const ShaderRewriter&) = delete;
    ShaderRewriter& operator=(const ShaderRewriter&) = delete;

    void ensure_initialized();
    static bool validate(const std::vector<uint32_t>& code, uint32_t version);
    static uint64_t hash_code(const std::vector<uint32_t>& code, uint32_t target);

    struct CacheEntry {
        bool changed = false;
        std::vector<uint32_t> code;
    };

    std::once_flag m_init_once;
    bool m_enabled = true;
    uint32_t m_env_target = 0;  // 0 = use the per-device target

    std::mutex m_cache_mutex;
    std::unordered_map<uint64_t, CacheEntry> m_cache;

    std::atomic<uint64_t> m_rewritten{0};
    std::atomic<uint64_t> m_rejected{0};
};

#endif  // SHADER_REWRITER_H
