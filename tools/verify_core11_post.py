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
    devices = sum(row.get("name") == "device" for row in rows)
    if not devices:
        errors.append("missing physical-device identity")
    for name in ("Vulkan 1.1.0 Core", "core11.requiredCapabilities", "core11.queryChain"):
        gates = [row for row in rows if row.get("name") == name]
        if len(gates) != devices or not gates:
            errors.append(f"{name}: expected one result per device, got {len(gates)}")
        elif any(row.get("status") != "PASS" for row in gates):
            errors.append(f"{name}: mandatory gate did not pass")
    summaries = [row for row in rows if row.get("name") == "core11.summary"]
    subgroup = [row for row in rows if row.get("name") == "core11.subgroupBasic"]
    if not subgroup or len(subgroup) != devices:
        errors.append("core11.subgroupBasic: expected one execution result per device")
    elif any(row.get("evidence") != "functional" or
             (row.get("status") != "PASS" and not
              (row.get("status") == "SKIP" and row.get("detail") == "no compute queue"))
             for row in subgroup):
        errors.append("core11.subgroupBasic: mandatory compute execution did not pass")
    if len(summaries) != 1 or summaries[0].get("status") != "PASS":
        errors.append("missing or unsuccessful core11.summary")
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
            print(f"  {'FAIL' if require_pass else 'native-defect'} "
                  f"{row.get('name')}: {row.get('detail')}")
        if require_pass:
            errors = report_errors(code, rows)
            for error in errors:
                print(f"  verification: {error}")
            failed |= bool(errors)
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
