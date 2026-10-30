"""Generate the POST entry-point inventory from the pinned Khronos registry."""
import argparse
from pathlib import Path
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "layer/src/main/cpp/diagnostics/driver_post_commands.inc"


def generate():
    registry = ET.parse(ROOT / "Vulkan-Headers/registry/vk.xml").getroot()
    commands = {c.findtext("proto/name") or c.get("name"): c
                for c in registry.findall("commands/command")}
    lines = ["// Generated from Vulkan-Headers/registry/vk.xml. Do not edit.",
             "// Khronos registry: SPDX-License-Identifier: Apache-2.0 OR MIT"]
    for feature in registry.findall("feature"):
        if feature.get("number") not in ("1.0", "1.1", "1.2"):
            continue
        if "vulkan" not in feature.get("api", "").split(","):
            continue
        version = feature.get("number").replace(".", "_")
        seen = set()
        for require in feature.findall("require"):
            if "vulkan" not in require.get("api", "vulkan").split(","):
                continue
            for ref in require.findall("command"):
                name = ref.get("name")
                if name in seen:
                    continue
                seen.add(name)
                command = commands[name]
                while command.get("alias"):
                    command = commands[command.get("alias")]
                first = command.findtext("param/type")
                scope = "Device" if first in ("VkDevice", "VkQueue", "VkCommandBuffer") else "Instance"
                if name in ("vkCreateInstance", "vkEnumerateInstanceVersion",
                            "vkEnumerateInstanceExtensionProperties", "vkEnumerateInstanceLayerProperties",
                            "vkGetInstanceProcAddr"):
                    scope = "Global"
                lines.append(f'POST_COMMAND(VK_API_VERSION_{version}, {scope}, "{name}")')
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    content = generate()
    if args.check:
        if not OUTPUT.exists() or OUTPUT.read_text(encoding="utf-8") != content:
            parser.exit(1, "Driver POST inventory is stale; regenerate it.\n")
    else:
        OUTPUT.write_text(content, encoding="utf-8", newline="\n")
