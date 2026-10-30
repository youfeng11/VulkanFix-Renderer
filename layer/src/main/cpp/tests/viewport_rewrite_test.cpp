#include <cstdint>
#include "diagnostics/driver_post_shaders.h"
#include "shader/shader_rewriter.h"
#include <cstdio>
#include <vector>
#include <spirv-tools/libspirv.hpp>

namespace {
unsigned failures = 0;
void check(bool ok, const char *what) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    std::fflush(stdout);
    failures += !ok;
}
} // namespace

int main() {
    std::vector<uint32_t> vertex(post_draw_vert,
                                 post_draw_vert + sizeof post_draw_vert / sizeof(uint32_t));
    const auto original_size = vertex.size();
    check(ShaderRewriter::get().flip_position_y(vertex),
          "vertex shader with a direct Position store is rewritten");
    check(vertex.size() > original_size, "rewrite inserts Position Y negation instructions");
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
    check(tools.Validate(vertex), "rewritten vertex module validates in Vulkan 1.1");

    std::vector<uint32_t> compute(post_push_comp,
                                  post_push_comp + sizeof post_push_comp / sizeof(uint32_t));
    check(!ShaderRewriter::get().flip_position_y(compute),
          "a module without a Position output is left untouched");

    std::vector<uint32_t> dispatch_base(post_push_comp,
                                        post_push_comp + sizeof post_push_comp / sizeof(uint32_t));
    check(ShaderRewriter::get().offset_invocation_ids(dispatch_base, 1, 0, 0),
          "GlobalInvocationID loads receive the dispatch base");
    check(tools.Validate(dispatch_base),
          "dispatch-base rewritten module validates in Vulkan 1.1");
    std::vector<uint32_t> zero_base(post_push_comp,
                                    post_push_comp + sizeof post_push_comp / sizeof(uint32_t));
    check(!ShaderRewriter::get().offset_invocation_ids(zero_base, 0, 0, 0),
          "a zero dispatch base leaves the module untouched");

    std::vector<uint32_t> vector_base(
        post_dispatch_vector_comp,
        post_dispatch_vector_comp + sizeof post_dispatch_vector_comp / sizeof(uint32_t));
    check(ShaderRewriter::get().offset_invocation_ids(vector_base, 2, 3, 0),
          "whole-vector GlobalInvocationID loads receive the dispatch base");
    check(tools.Validate(vector_base),
          "vector dispatch-base module validates in Vulkan 1.1");

    std::vector<uint32_t> workgroup_base(
        post_dispatch_workgroup_comp,
        post_dispatch_workgroup_comp + sizeof post_dispatch_workgroup_comp / sizeof(uint32_t));
    check(ShaderRewriter::get().offset_invocation_ids(workgroup_base, 1, 2, 0),
          "WorkGroupID loads receive the dispatch base");
    check(tools.Validate(workgroup_base),
          "WorkGroupID dispatch-base module validates in Vulkan 1.1");

    std::vector<uint32_t> image_base(
        post_dispatch_image_comp,
        post_dispatch_image_comp + sizeof post_dispatch_image_comp / sizeof(uint32_t));
    check(ShaderRewriter::get().offset_invocation_ids(image_base, 1, 1, 0),
          "imageStore coordinates receive the dispatch base");
    check(tools.Validate(image_base),
          "image dispatch-base module validates in Vulkan 1.1");
    std::vector<uint32_t> no_ids(post_spirv14_comp,
                                 post_spirv14_comp + sizeof post_spirv14_comp / sizeof(uint32_t));
    check(!ShaderRewriter::uses_invocation_ids(no_ids),
          "a module without invocation-ID builtins is detected");
    std::vector<uint32_t> with_ids(post_push_comp,
                                   post_push_comp + sizeof post_push_comp / sizeof(uint32_t));
    check(ShaderRewriter::uses_invocation_ids(with_ids),
          "a module using GlobalInvocationID is detected");
    std::vector<uint32_t> convert_base(
        post_dispatch_convert_comp,
        post_dispatch_convert_comp + sizeof post_dispatch_convert_comp / sizeof(uint32_t));
    check(ShaderRewriter::get().offset_invocation_ids(convert_base, 1, 0, 0),
          "conversion operands receive the dispatch base");
    check(tools.Validate(convert_base),
          "conversion dispatch-base module validates in Vulkan 1.1");
    std::vector<uint32_t> viewport_index(
        post_viewport_index_vert,
        post_viewport_index_vert + sizeof post_viewport_index_vert / sizeof(uint32_t));
    check(ShaderRewriter::get().uses_viewport_index(viewport_index),
          "a module writing gl_ViewportIndex is detected");
    uint32_t viewport_slot = 0;
    check(ShaderRewriter::get().constant_viewport_index(viewport_index, viewport_slot) &&
              viewport_slot == 1,
          "a constant gl_ViewportIndex store is extracted");
    check(ShaderRewriter::get().flip_position_y(viewport_index),
          "a ViewportIndex shader's Position store is rewritten");
    check(tools.Validate(viewport_index),
          "ViewportIndex flip module validates in Vulkan 1.1");
    std::printf("Viewport rewrite failures: %u\n", failures);
    return failures ? 1 : 0;
}
