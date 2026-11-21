"""Run the Vulkan 1.1 POST on a device and verify the wrapper repairs defects.

The native run supplies a baseline. Failures require normative investigation;
they are not automatically driver defects. Wrapper runs require mandatory
capability/query gates and no functional failures, including inherited Core 1.0
features. These probes are necessary checks, not a conformance certification.
"""
import argparse
import json
import shlex
import subprocess


# These are behavioral obligations exercised by the POST, not optional feature
# inventory. Queue-specific probes may skip only when that queue is absent.
REQUIRED_EXECUTIONS = {
    **{f"core11.{name}": () for name in (
        "queryChain", "externalProperties", "sparseMemoryRequirements2",
        "bufferMemory2", "imageMemory2", "descriptorUpdateTemplate",
        "descriptorSetLayoutSupport", "formatTransferFeatures", "imageTypeCopy",
    )},
    "deviceQueue2.sameQueue": (),
    "core11.volumeAttachment": ("no graphics queue", "no compute queue"),
    **{f"core11.{name}": ("no compute queue",) for name in (
        "subgroupBasic", "dispatchBase", "dispatchBaseNonZero",
        "dispatchBaseNonZero2", "dispatchBaseYZ", "dispatchBaseImage",
        "dispatchBaseAtomic", "spirv13Downgrade", "relaxedBlockLayout",
    )},
    **{f"core11.{name}": ("no graphics queue",) for name in (
        "negativeViewport", "dynamicNegativeViewport",
        "dynamicNegativeViewportIndirect", "dynamicNegativeViewportIndexed",
        "dynamicNegativeViewportIndexedIndirect", "dynamicPositiveViewport",
        "staticViewportPositionRead", "dynamicViewportPositionRead", "inputAttachmentAspects", "volumeInputAttachment",
    )},
    # A compute-only implementation may omit multiview. For graphics devices,
    # requiredCapabilities independently rejects an unavailable feature.
    "core11.multiviewRenderPass": ("feature unavailable", "no graphics queue"),
    "core11.tessellationDomainOriginUpper": ("optional shader stage unavailable", "no graphics queue"),
    "core11.tessellationDomainOriginLower": ("optional shader stage unavailable", "no graphics queue"),
}


def run(adb, directory, binary, library=None, env=None):
    arguments = ["env", *[f"{key}={value}" for key, value in (env or {}).items()],
                 "timeout", "120", f"./{binary}"]
    if library:
        arguments.append(f"./{library}")
    command = f"cd {shlex.quote(directory)} && {shlex.join(arguments)}"
    result = subprocess.run([adb, "shell", command], capture_output=True, text=True, timeout=135)
    rows = []
    for line in result.stdout.splitlines():
        line = line.strip()
        if line.startswith("{"):
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError:
                pass
    summary = next((row for row in rows if row.get("name") == "core11.summary"), None)
    failures = [row for row in rows
        if row.get("status") == "FAIL" and
                (row.get("name", "").startswith("core11.") or row.get("evidence") == "functional")]
    return result.returncode, rows, summary, failures


def report_errors(code, rows):
    """Necessary report gates; a passing report still is not full conformance."""
    errors = []
    if code:
        errors.append(f"POST exited with status {code}")
    devices = []
    for row in rows:
        if row.get("name") == "device":
            devices.append([])
        elif devices:
            devices[-1].append(row)
    if not devices:
        errors.append("missing physical-device identity")
    for index, device in enumerate(devices):
        for name, evidence in (("Vulkan 1.1.0 Core", "query"),
                               ("core11.requiredCapabilities", "query")):
            gates = [row for row in device if row.get("name") == name]
            if len(gates) != 1 or gates[0].get("status") != "PASS" or gates[0].get("evidence") != evidence:
                errors.append(f"device {index} {name}: mandatory query gate missing, duplicated or unsuccessful")
        for name, skips in REQUIRED_EXECUTIONS.items():
            results = [row for row in device if row.get("name") == name]
            if len(results) != 1:
                errors.append(f"device {index} {name}: expected one execution result, got {len(results)}")
                continue
            result = results[0]
            allowed = result.get("status") == "PASS" or (
                result.get("status") == "SKIP" and result.get("detail") in skips)
            if result.get("evidence") != "functional" or not allowed:
                errors.append(f"device {index} {name}: required execution did not pass")
    summaries = [row for row in rows if row.get("name") == "core11.summary"]
    if len(summaries) != 1 or summaries[0].get("status") != "PASS" or summaries[0].get("evidence") != "functional":
        errors.append("missing or unsuccessful core11.summary")
    if not rows or rows[-1].get("name") != "complete" or rows[-1].get("status") != "INFO" or rows[-1].get("evidence") != "completion":
        errors.append("missing final POST completion")
    if any(row.get("evidence") == "error" for row in rows):
        errors.append("POST reported an execution error")
    if any(row.get("name", "").startswith("core11.") and row.get("status") == "FAIL" for row in rows):
        errors.append("one or more Core 1.1 probes failed")
    outside_core11 = [row.get("name", "unnamed") for row in rows
                      if row.get("evidence") == "functional" and row.get("status") == "FAIL"
                      and not row.get("name", "").startswith("core11.")]
    if outside_core11:
        errors.append("other functional probes failed: " + ", ".join(outside_core11))
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--adb", default="adb")
    parser.add_argument("--dir", default="/data/local/tmp/vkfix-core11-peer")
    parser.add_argument("--binary", default="vulkanfix_driver_post")
    parser.add_argument("--library", default="libvulkanfix.so")
    args = parser.parse_args()

    cases = [
        ("native", None, {}, False),
        ("wrapper", args.library, {}, True),
        ("wrapper-forced", args.library, {
            "VULKAN_FIX_EMULATE_NEGATIVE_VIEWPORT": "1",
            "VULKAN_FIX_EMULATE_DISPATCH_BASE": "1",
        }, True),
    ]
    failed = False
    for name, library, env, require_pass in cases:
        code, rows, summary, failures = run(
            args.adb, args.dir, args.binary, library, env)
        detail = summary.get("detail") if summary else "missing core11.summary"
        print(f"{name}: {detail}")
        for row in failures:
            print(f"  {'FAIL' if require_pass else 'native-failure'} "
                  f"{row.get('name')}: {row.get('detail')}")
        if require_pass:
            errors = report_errors(code, rows)
            for error in errors:
                print(f"  verification: {error}")
            failed |= bool(errors)
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
