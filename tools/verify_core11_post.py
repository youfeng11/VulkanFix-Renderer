"""Run the Vulkan 1.1 POST on a device and verify the wrapper repairs defects.

The native run is allowed to report failures: those are the driver defects the
wrapper is expected to repair. The wrapper and forced-emulation runs must both
reach a clean core11.summary.
"""
import argparse
import json
import subprocess


def run(adb, directory, binary, library=None, env=None):
    command = f"cd {directory} && "
    for key, value in (env or {}).items():
        command += f"{key}={value} "
    command += f"timeout 120 ./{binary}"
    if library:
        command += f" ./{library}"
    result = subprocess.run([adb, "shell", command], capture_output=True, text=True)
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
                if row.get("name", "").startswith("core11.")
                and row.get("status") == "FAIL"]
    return result.returncode, rows, summary, failures


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
        if require_pass and (
                code != 0 or not summary or summary.get("status") != "PASS" or failures):
            failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
