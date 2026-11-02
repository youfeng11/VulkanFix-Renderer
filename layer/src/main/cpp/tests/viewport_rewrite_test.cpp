#include <cstdint>
#include "diagnostics/driver_post_shaders.h"
#include "shader/shader_rewriter.h"
#include "dispatch_base_fixture.h"
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
    tools.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t& position, const char* message) {
        std::printf("SPIR-V word %zu: %s\n", position.index, message);
    });
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
    struct PositionCase { const uint32_t* words; size_t size; uint32_t model; };
    const PositionCase position_cases[] = {
        {post_viewport_stage_vert, sizeof post_viewport_stage_vert, 0},
        {post_viewport_stage_geom, sizeof post_viewport_stage_geom, 3},
        {post_viewport_stage_tese, sizeof post_viewport_stage_tese, 2}};
    for (const auto& item : position_cases) {
        std::vector<uint32_t> fixture(item.words, item.words + item.size / 4);
        check(tools.Validate(fixture) && ShaderRewriter::get().flip_position_y(fixture, "main", item.model) &&
                  tools.Validate(fixture),
              "Position reads, geometry helper emission and tessellation exits remain valid");
        std::vector<uint32_t> missing(item.words, item.words + item.size / 4);
        const auto original = missing;
        check(!ShaderRewriter::get().flip_position_y(missing, "missing", item.model) && missing == original,
              "an unmatched stage entry point cannot mutate the module");
    }
    std::vector<uint32_t> grouped(post_viewport_stage_vert, post_viewport_stage_vert +
                                 sizeof post_viewport_stage_vert / sizeof(uint32_t));
    const uint32_t group = grouped[3]++;
    bool grouped_position = false;
    for (size_t at = 5; at < grouped.size();) {
        const auto wc = grouped[at] >> 16;
        if ((grouped[at] & 0xffff) == 72 && wc == 5 && grouped[at + 3] == 11 && grouped[at + 4] == 0) {
            const uint32_t structure = grouped[at + 1], member = grouped[at + 2];
            const uint32_t annotation[] = {
                (2u << 16) | 73u, group, // DecorationGroup
                (4u << 16) | 71u, group, 11, 0, // BuiltIn Position
                (4u << 16) | 75u, group, structure, member}; // GroupMemberDecorate
            grouped.erase(grouped.begin() + at, grouped.begin() + at + wc);
            grouped.insert(grouped.begin() + at, std::begin(annotation), std::end(annotation));
            grouped_position = true;
            break;
        }
        at += wc;
    }
    check(grouped_position && tools.Validate(grouped) && ShaderRewriter::get().flip_position_y(grouped) &&
              tools.Validate(grouped), "Position supplied through a decoration group is transformed legally");
    auto truncated = grouped;
    truncated.pop_back();
    const auto damaged = truncated;
    check(!ShaderRewriter::get().flip_position_y(truncated) && truncated == damaged,
          "a truncated SPIR-V module is rejected without modifying caller data");
    for (unsigned shape = 0; shape < 4; ++shape) {
        std::vector<uint32_t> fixture;
        check(tools.Assemble(dispatch_base_fixture(shape == 1, shape == 2, shape == 3), &fixture) &&
                  tools.Validate(fixture), "non-unit dispatch fixture is legal Vulkan 1.1 SPIR-V");
        bool no_offset = true;
        check(ShaderRewriter::get().offset_invocation_ids(fixture, 2, 1, 1, "kernel", &no_offset) &&
                  !no_offset && tools.Validate(fixture),
              "non-unit/specialized/helper/CopyMemory dispatch preserves Phi and dynamic indexes");
    }
    std::printf("Viewport rewrite failures: %u\n", failures);
    return failures ? 1 : 0;
}
