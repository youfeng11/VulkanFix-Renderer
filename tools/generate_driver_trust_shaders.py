"""Bake controlled native-driver defect probes, independently of runtime repair code."""
import argparse
import hashlib
from pathlib import Path
import re
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
DEST = ROOT / "layer/src/main/cpp/core/driver_trust_shaders.h"


def uniform_fixture():
    """Mixed-layout Iris-style frame block, including live members above 256."""
    offsets = []
    offset = 0
    decorations = []
    types = []
    for i in range(305):
        kind = i % 5
        alignment = 16 if kind in (1, 3) else 8 if kind == 4 else 4
        offset = (offset + alignment - 1) // alignment * alignment
        offsets.append(offset)
        decorations.append(f"OpMemberDecorate %Frame {i} Offset {offset}")
        if kind == 3:
            decorations.extend((f"OpMemberDecorate %Frame {i} ColMajor", f"OpMemberDecorate %Frame {i} MatrixStride 16"))
        offset += {1: 12, 3: 64, 4: 8}.get(kind, 4)
        types.append({1: "%v3", 3: "%mat4", 4: "%v2"}.get(kind, "%float"))
    source = '''OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main"
OpExecutionMode %main LocalSize 1 1 1
OpDecorate %Frame Block
OpDecorate %Output BufferBlock
OpMemberDecorate %Output 0 Offset 0
OpDecorate %frame DescriptorSet 0
OpDecorate %frame Binding 0
OpDecorate %output DescriptorSet 0
OpDecorate %output Binding 1
''' + "\n".join(decorations) + '''
%void = OpTypeVoid
%fn = OpTypeFunction %void
%float = OpTypeFloat 32
%uint = OpTypeInt 32 0
%v2 = OpTypeVector %float 2
%v3 = OpTypeVector %float 3
%v4 = OpTypeVector %float 4
%mat4 = OpTypeMatrix %v4 4
%Frame = OpTypeStruct ''' + " ".join(types) + '''
%Output = OpTypeStruct %v4
%frameptr = OpTypePointer Uniform %Frame
%outputptr = OpTypePointer Uniform %Output
%uniform2 = OpTypePointer Uniform %v2
%uniform1 = OpTypePointer Uniform %float
%storage4 = OpTypePointer Uniform %v4
%zero = OpConstant %uint 0
%two = OpConstant %uint 2
%last = OpConstant %uint 299
%frame = OpVariable %frameptr Uniform
%output = OpVariable %outputptr Uniform
%main = OpFunction %void None %fn
%entry = OpLabel
%wp = OpAccessChain %uniform2 %frame %last
%window = OpLoad %v2 %wp
%xp = OpAccessChain %uniform1 %frame %zero
%yp = OpAccessChain %uniform1 %frame %two
%x = OpLoad %float %xp
%y = OpLoad %float %yp
%w0 = OpCompositeExtract %float %window 0
%w1 = OpCompositeExtract %float %window 1
%result = OpCompositeConstruct %v4 %w0 %w1 %x %y
%dest = OpAccessChain %storage4 %output %zero
OpStore %dest %result
OpReturn
OpFunctionEnd
'''
    return source, offsets, offset


def generate(tool_dir):
    fixture = (ROOT / "layer/src/main/cpp/tests/sampler_alias_fixture.h").read_text(encoding="utf-8")
    sources = {}
    for name, variable in (("sampler_subject", "kAliasedSamplerShader"), ("sampler_control", "kCanonicalSamplerShader")):
        source = re.search(variable + r' = R"spv\((.*?)\)spv";', fixture, re.S).group(1)
        # Core Vulkan 1.0 storage-buffer representation; no opt-in device feature.
        source = source.replace("OpDecorate %Out Block", "OpDecorate %Out BufferBlock")
        sources[name] = source.replace("StorageBuffer", "Uniform")
    sources["uniform_subject"], uniform_offsets, uniform_bytes = uniform_fixture()
    sources["push_empty"] = '''OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main"
OpExecutionMode %main LocalSize 1 1 1
%void = OpTypeVoid
%fn = OpTypeFunction %void
%main = OpFunction %void None %fn
%entry = OpLabel
OpReturn
OpFunctionEnd
'''
    sources["push_output"] = '''OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main"
OpExecutionMode %main LocalSize 1 1 1
OpDecorate %Out BufferBlock
OpMemberDecorate %Out 0 Offset 0
OpDecorate %buffer DescriptorSet 1
OpDecorate %buffer Binding 0
OpDecorate %Seed Block
OpMemberDecorate %Seed 0 Offset 0
%void = OpTypeVoid
%fn = OpTypeFunction %void
%uint = OpTypeInt 32 0
%v4 = OpTypeVector %uint 4
%Out = OpTypeStruct %v4
%Seed = OpTypeStruct %uint
%outptr = OpTypePointer Uniform %Out
%lane = OpTypePointer Uniform %v4
%seedptr = OpTypePointer PushConstant %Seed
%scalar = OpTypePointer PushConstant %uint
%zero = OpConstant %uint 0
%one = OpConstant %uint 1
%two = OpConstant %uint 2
%three = OpConstant %uint 3
%buffer = OpVariable %outptr Uniform
%seed = OpVariable %seedptr PushConstant
%main = OpFunction %void None %fn
%entry = OpLabel
%sp = OpAccessChain %scalar %seed %zero
%s = OpLoad %uint %sp
%a = OpIAdd %uint %s %one
%b = OpIAdd %uint %s %two
%c = OpIAdd %uint %s %three
%result = OpCompositeConstruct %v4 %s %a %b %c
%dest = OpAccessChain %lane %buffer %zero
OpStore %dest %result
OpReturn
OpFunctionEnd
'''
    binaries = {}
    with tempfile.TemporaryDirectory(prefix="vulkanfix-driver-trust-") as temporary:
        for name, source in sources.items():
            assembly = Path(temporary) / (name + ".spvasm")
            binary = Path(temporary) / (name + ".spv")
            assembly.write_text(source, encoding="utf-8")
            subprocess.run([str(tool_dir / "spirv-as"), "--target-env", "vulkan1.0", str(assembly), "-o", str(binary)], check=True)
            subprocess.run([str(tool_dir / "spirv-val"), "--target-env", "vulkan1.0", str(binary)], check=True)
            binaries[name] = binary.read_bytes()
        graphics = {
            "tiler_vert": ("vert", '''#version 450
void main() {
    const vec2 p[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));
    gl_Position=vec4(p[gl_VertexIndex%3],0,1);
}
'''),
            "tiler_frag": ("frag", '''#version 450
layout(push_constant) uniform Color { vec4 color; } pc;
layout(location=0) out vec4 result;
void main() { result=pc.color; }
''')}
        for name, (stage, source) in graphics.items():
            shader = Path(temporary) / (name + "." + stage)
            binary = Path(temporary) / (name + ".spv")
            shader.write_text(source, encoding="utf-8")
            subprocess.run([str(tool_dir / "glslc"), "--target-env=vulkan1.0", "-O", str(shader), "-o", str(binary)], check=True)
            subprocess.run([str(tool_dir / "spirv-val"), "--target-env", "vulkan1.0", str(binary)], check=True)
            binaries[name] = binary.read_bytes()
        control = Path(temporary) / "uniform_control.spv"
        subprocess.run([str(tool_dir / "spirv-opt"), "--target-env=vulkan1.0", "--eliminate-dead-members",
                        str(Path(temporary) / "uniform_subject.spv"), "-o", str(control)], check=True)
        subprocess.run([str(tool_dir / "spirv-val"), "--target-env", "vulkan1.0", str(control)], check=True)
        binaries["uniform_control"] = control.read_bytes()
    # The worker must match probe semantics, not merely shader bytes. A change
    # from a small draw to a larger stress sequence must invalidate old workers
    # even when its SPIR-V stays identical. Normalize checkout line endings.
    probe_sources = (
        "core/driver_trust_probe.cpp", "core/driver_probe_process.cpp",
        "core/driver_tiler_probe.inc", "core/native_probe_resources.h",
        "core/driver_trust.h", "diagnostics/driver_trust_worker.cpp")
    digest_input = b"".join(binaries.values())
    for relative in probe_sources:
        source = ROOT / "layer/src/main/cpp" / relative
        digest_input += relative.encode() + b"\0" + source.read_text(encoding="utf-8").encode("utf-8")
    digest = hashlib.sha256(digest_input).hexdigest()
    lines = ["// Generated by tools/generate_driver_trust_shaders.py; do not edit.", "#pragma once", "#include <cstdint>",
             "namespace vkfix::nativeprobe {", f"inline constexpr uint64_t revision = 0x{digest[:16]}ull;"]
    lines.extend((f"inline constexpr uint32_t uniform_bytes = {uniform_bytes};",
                  f"inline constexpr uint32_t uniform_offsets[] = {{{uniform_offsets[299]}, {uniform_offsets[0]}, {uniform_offsets[2]}}};"))
    for name, data in binaries.items():
        words = struct.unpack(f"<{len(data)//4}I", data)
        lines.append(f"inline constexpr uint32_t {name}[] = {{")
        for index in range(0, len(words), 8):
            lines.append("    " + ", ".join(f"0x{word:08x}" for word in words[index:index+8]) + ",")
        lines.append("};")
    lines.append("} // namespace vkfix::nativeprobe")
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool-dir", type=Path, required=True, help="NDK r27d shader-tools directory")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    output = generate(args.tool_dir)
    if args.check:
        if not DEST.exists() or DEST.read_text(encoding="utf-8") != output:
            parser.exit(1, "Driver trust shader fixtures are stale.\n")
    else:
        DEST.write_text(output, encoding="utf-8", newline="\n")
