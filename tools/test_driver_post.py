"""Check POST inventory and exercise its failure paths without a GPU workload.

Usage: python -B tools/test_driver_post.py [--binary /path/to/host/probe]
Device execution is deliberately not hidden in these CPU tests.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import unittest
import xml.etree.ElementTree as ET

from generate_driver_post_commands import ROOT, OUTPUT, generate

BINARY = None
ADB = None
SERIAL = None
REPORTS = []
FAULT_DRIVER = None
BASE_DRIVER = None


def execute(arguments, environment=None):
    command = [BINARY, *arguments]
    if ADB:
        prefix = ["env", *[f"{key}={value}" for key, value in (environment or {}).items()]]
        command = [ADB, "-s", SERIAL, "shell", "timeout 15 " + shlex.join([*prefix, *command])]
    return subprocess.run(command, capture_output=True, text=True, timeout=20)


class InventoryTest(unittest.TestCase):
    def test_generated_matches_pinned_registry(self):
        self.assertEqual(OUTPUT.read_text(encoding="utf-8"), generate())

    def test_exact_core_command_membership(self):
        # Independently compare sets; a version row must not merely contain a
        # hand-picked subset of commonly used calls.
        registry = ET.parse(ROOT / "Vulkan-Headers/registry/vk.xml").getroot()
        text = OUTPUT.read_text(encoding="utf-8")
        for version in ("1.0", "1.1", "1.2"):
            features = [f for f in registry.findall("feature") if
                        f.get("number") == version and "vulkan" in f.get("api", "").split(",")]
            expected = {c.get("name") for f in features for req in f.findall("require")
                        if "vulkan" in req.get("api", "vulkan").split(",")
                        for c in req.findall("command")}
            pattern = rf'POST_COMMAND\(VK_API_VERSION_{version.replace(".", "_")}, \w+, "([^"]+)"\)'
            found = re.findall(pattern, text)
            self.assertEqual(set(found), expected)
            self.assertEqual(len(found), len(expected), "duplicate command")
            self.assertGreater(len(found), 0)

    def test_dispatch_scopes(self):
        text = OUTPUT.read_text(encoding="utf-8")
        for version, scope, name in (("1_0", "Global", "vkCreateInstance"),
                                     ("1_0", "Instance", "vkCreateDevice"),
                                     ("1_0", "Device", "vkQueueSubmit"),
                                     ("1_1", "Global", "vkEnumerateInstanceVersion"),
                                     ("1_2", "Device", "vkCmdDrawIndirectCount")):
            self.assertIn(f'POST_COMMAND(VK_API_VERSION_{version}, {scope}, "{name}")', text)

    def test_shader_fixture_source_digest(self):
        directory = ROOT / "layer/src/main/cpp/diagnostics"
        text = (directory / "driver_post_shaders.h").read_text(encoding="utf-8")
        for source in (directory / "shaders").glob("*"):
            digest = hashlib.sha256(source.read_text(encoding="utf-8").encode("utf-8")).hexdigest()
            self.assertIn(f"// {source.name} SHA256 {digest}", text)


class FailurePathTest(unittest.TestCase):
    def setUp(self):
        if BINARY is None:
            self.skipTest("supply --binary for host executable failure tests")

    def test_missing_loader_is_json_error(self):
        result = execute(['missing-driver-"quoted".dll'])
        self.assertEqual(result.returncode, 1)
        rows = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["status"], "FAIL")
        self.assertEqual(rows[0]["evidence"], "error")
        self.assertNotEqual(rows[0]["detail"], "")

    def test_invalid_arguments_do_not_touch_driver(self):
        result = execute(["one", "two"])
        self.assertEqual(result.returncode, 2)
        self.assertEqual(result.stdout, "")
        self.assertIn("Usage:", result.stderr)


class ReportTest(unittest.TestCase):
    def test_required_rows_and_evidence(self):
        if not REPORTS:
            self.skipTest("supply --report to validate a completed device report")
        expected = {
            "VK_KHR_dynamic_rendering", "VK_KHR_push_descriptor", "VK_KHR_synchronization2",
            "VK_EXT_vertex_attribute_divisor", "VK_KHR_swapchain", "multiDrawIndirect",
            "fillModeNonSolid", "drawIndirectFirstInstance", "samplerAnisotropy",
            "shaderDrawParameters", "timelineSemaphore", "hostQueryReset", "synchronization2",
            "dynamicRendering", "vertexAttributeInstanceRateDivisor", "vertexPipelineStoresAndAtomics",
            "Vulkan 1.0.0 Core", "Vulkan 1.1.0 Core", "Vulkan 1.2.0 Core",
            "bufferTransfer", "timelineSemaphore.hostSignalWait", "hostQueryReset.reset",
            "synchronization2.transfer", "dynamicRendering.clear", "pushDescriptor.compute",
            "graphics.draw", "multiDrawIndirect.draw", "drawIndirectFirstInstance.draw",
            "vertexAttributeInstanceRateDivisor.draw", "fillModeNonSolid.line", "shaderDrawParameters.draw",
            "vertexPipelineStoresAndAtomics.draw", "samplerAnisotropy.sample",
            "VK_KHR_swapchain.present",
        }
        for path in REPORTS:
            with self.subTest(report=str(path)):
                rows = [json.loads(line) for line in path.read_text(encoding="utf-8-sig").splitlines() if line.strip()]
                self.assertEqual(rows[-1]["evidence"], "completion", "probe failed or was truncated")
                devices = []
                for row in rows:
                    self.assertIn(row["status"], {"PASS", "FAIL", "SKIP", "WARN", "INFO"})
                    self.assertIn(row["evidence"], {"query", "identity", "functional", "completion"})
                    if row["name"] == "device":
                        devices.append([])
                    elif devices:
                        devices[-1].append(row)
                self.assertGreater(len(devices), 0)
                for device in devices:
                    for name in expected:
                        matches = [r for r in device if r["name"] == name]
                        self.assertEqual(len(matches), 1, name)
                        self.assertEqual(matches[0]["evidence"], "functional" if name in {
                            "bufferTransfer", "timelineSemaphore.hostSignalWait", "hostQueryReset.reset",
                            "synchronization2.transfer", "dynamicRendering.clear", "pushDescriptor.compute",
                            "graphics.draw", "multiDrawIndirect.draw", "drawIndirectFirstInstance.draw",
                            "vertexAttributeInstanceRateDivisor.draw", "fillModeNonSolid.line", "shaderDrawParameters.draw",
                            "vertexPipelineStoresAndAtomics.draw", "samplerAnisotropy.sample", "VK_KHR_swapchain.present"
                        } else "query")


class ReadbackNegativeTest(unittest.TestCase):
    def test_vulkan10_extension_dispatch(self):
        if not (ADB and BINARY and FAULT_DRIVER):
            self.skipTest("supply --adb, --binary and --fault-driver")
        result = execute([FAULT_DRIVER], {"VKFIX_POST_TEST_DRIVER": "libvulkan.so", "VKFIX_POST_TEST_DROP": "api10"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        rows = [json.loads(line) for line in result.stdout.splitlines() if line.strip()]
        self.assertEqual(rows[-1]["evidence"], "completion")
        by_name = {r["name"]: r for r in rows}
        self.assertEqual(by_name["Vulkan 1.0.0 Core"]["status"], "PASS")
        self.assertEqual(by_name["Vulkan 1.1.0 Core"]["status"], "FAIL")
        # Extension-provided features can remain usable even on a 1.0 API.
        for feature, probe in (("timelineSemaphore", "timelineSemaphore.hostSignalWait"),
                               ("hostQueryReset", "hostQueryReset.reset"),
                               ("shaderDrawParameters", "shaderDrawParameters.draw")):
            if by_name[feature]["status"] == "PASS":
                self.assertEqual(by_name[probe]["status"], "PASS", feature)

    def test_dropped_gpu_work_is_detected(self):
        if not (ADB and BINARY and FAULT_DRIVER and BASE_DRIVER):
            self.skipTest("supply --adb, --binary, --fault-driver and --base-driver")
        cases = (("fill", "bufferTransfer", "GPU fill readback mismatch"),
                 ("reset", "hostQueryReset.reset", "reset queries did not return unavailable/NOT_READY"),
                 ("copy", "synchronization2.transfer", "synchronization2 transfer readback mismatch"),
                 ("render", "dynamicRendering.clear", "dynamic rendering clear/readback mismatch"),
                 ("push", "pushDescriptor.compute", "push descriptor compute readback mismatch"),
                 ("indirect", "multiDrawIndirect.draw", "pixel readback mismatch"),
                 ("indirect", "drawIndirectFirstInstance.draw", "pixel readback mismatch"),
                 ("indirect", "shaderDrawParameters.draw", "pixel readback mismatch"),
                 ("divisor", "vertexAttributeInstanceRateDivisor.draw", "pixel readback mismatch"),
                 ("draw", "vertexPipelineStoresAndAtomics.draw", "vertex storage/atomic or pixel readback mismatch"),
                 ("draw", "samplerAnisotropy.sample", "pixel readback mismatch"))
        for mode, name, message in cases:
            with self.subTest(mode=mode):
                result = execute([FAULT_DRIVER], {"VKFIX_POST_TEST_DRIVER": BASE_DRIVER,
                                                 "VKFIX_POST_TEST_DROP": mode})
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                rows = [json.loads(line) for line in result.stdout.splitlines() if line]
                finding = next(r for r in rows if r["name"] == name)
                self.assertEqual(finding["status"], "FAIL")
                self.assertEqual(finding["evidence"], "functional")
                self.assertIn(message, finding["detail"])
                self.assertEqual(rows[-1]["evidence"], "completion")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary")
    parser.add_argument("--adb", type=Path)
    parser.add_argument("--serial")
    parser.add_argument("--report", type=Path, action="append", default=[])
    parser.add_argument("--fault-driver", help="remote test-only forwarding library")
    parser.add_argument("--base-driver", help="remote backend for the fault driver")
    args, remaining = parser.parse_known_args()
    REPORTS = args.report
    FAULT_DRIVER = args.fault_driver
    BASE_DRIVER = args.base_driver
    if args.binary:
        BINARY = args.binary if args.adb else str(Path(args.binary).resolve(strict=True))
    if args.adb:
        if not args.serial or not args.binary:
            parser.error("--adb requires --serial and the remote --binary path")
        ADB = str(args.adb.resolve(strict=True))
        SERIAL = args.serial
    unittest.main(argv=[__file__, *remaining])
