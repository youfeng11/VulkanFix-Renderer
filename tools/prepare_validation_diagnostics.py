"""Stage an explicitly requested Android VVL diagnostic provider; never install it."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import shutil
import subprocess
import urllib.request
import zipfile
from android_build_config import configured_ndk_version

VERSION = "1.4.363.0"
SHA256 = "9a3b8e32cb3330406dd4f3651594201635baee4528da8e2a5300e4f55e1a9cc7"
URL = f"https://github.com/KhronosGroup/Vulkan-ValidationLayers/releases/download/vulkan-sdk-{VERSION}/android-binaries-{VERSION}.zip"
TARGETS = {
    "arm64-v8a": "aarch64-linux-android26",
    "armeabi-v7a": "armv7a-linux-androideabi26",
    "x86": "i686-linux-android26",
    "x86_64": "x86_64-linux-android26",
}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-root", type=Path, required=True)
    parser.add_argument("--ndk-version", default=configured_ndk_version())
    parser.add_argument("--archive", type=Path, help="Existing official archive; SHA256 is always checked")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    build = root / "pluginApp/build"
    archive = args.archive or build / f"validation-downloads/android-binaries-{VERSION}.zip"
    if not archive.is_file():
        archive.parent.mkdir(parents=True, exist_ok=True)
        urllib.request.urlretrieve(URL, archive)
    if digest(archive) != SHA256:
        raise RuntimeError("Official validation-layer archive SHA256 mismatch")
    host = {"Windows": "windows-x86_64", "Linux": "linux-x86_64", "Darwin": "darwin-x86_64"}[platform.system()]
    toolchain = args.sdk_root / "ndk" / args.ndk_version / "toolchains/llvm/prebuilt" / host
    clang = toolchain / "bin" / ("clang++.exe" if platform.system() == "Windows" else "clang++")
    if not clang.is_file():
        raise RuntimeError(f"NDK compiler missing: {clang}")
    source = root / "layer/src/main/cpp/diagnostics/validation_driver.cpp"
    source_hash = digest(source)
    frozen_source = build / "validation-inputs" / f"{source_hash}.cpp"
    frozen_source.parent.mkdir(parents=True, exist_ok=True)
    if not frozen_source.is_file():
        shutil.copyfile(source, frozen_source)
    if digest(frozen_source) != source_hash:
        raise RuntimeError("Validation-driver source snapshot is inconsistent")
    payload = build / "validation-jniLibs"
    records = []
    with zipfile.ZipFile(archive) as contents:
        for abi, target in TARGETS.items():
            directory = payload / abi
            directory.mkdir(parents=True, exist_ok=True)
            validation = directory / "libVkLayer_khronos_validation.so"
            # Copy known members only; never extract arbitrary archive paths.
            validation.write_bytes(contents.read(f"android-binaries-{VERSION}/{abi}/{validation.name}"))
            bridge = directory / "libvulkan_fix_validation_driver.so"
            subprocess.run([
                str(clang), f"--target={target}", f"--sysroot={toolchain / 'sysroot'}",
                "-I", str(root / "Vulkan-Headers/include"), "-shared", "-fPIC", "-std=c++17",
                "-O2", "-g", "-fvisibility=hidden", "-Wl,-Bsymbolic-functions",
                "-Wl,-z,max-page-size=16384", "-Wl,-z,common-page-size=16384",
                "-Wl,-soname,libvulkan_fix_validation_driver.so", "-static-libstdc++",
                str(frozen_source), "-ldl", "-llog", "-o", str(bridge),
            ], check=True)
            records.append({"abi": abi, "validation_sha256": digest(validation),
                            "bridge_sha256": digest(bridge)})
    assets = build / "validation-assets"
    assets.mkdir(parents=True, exist_ok=True)
    license_file = root / "tools/validation/VVL-LICENSE.txt"
    if not license_file.is_file():
        raise RuntimeError("Pinned VVL license text is missing")
    shutil.copyfile(license_file, assets / "VVL-LICENSE.txt")
    if digest(source) != source_hash:
        raise RuntimeError("Validation-driver source changed during build; stage it again")
    record = {"sdk": VERSION, "archive_url": URL, "archive_sha256": SHA256,
              "bridge_source_sha256": source_hash, "libraries": records}
    (assets / "VulkanFix-validation-build.json").write_text(json.dumps(record, indent=2)+"\n", encoding="utf-8")
    print(f"Staged {len(records)} ABIs in {payload}")


if __name__ == "__main__":
    main()
