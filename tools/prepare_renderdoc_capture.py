"""Stage an optional RenderDoc capture APK from the user's installed official SDK."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import shutil
import subprocess
import zipfile
from android_build_config import configured_ndk_version


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderdoc-dir', required=True, type=Path)
    parser.add_argument('--sdk-root', required=True, type=Path)
    parser.add_argument('--ndk-version', default=configured_ndk_version())
    args = parser.parse_args()
    version = subprocess.check_output([str(args.renderdoc_dir / 'renderdoccmd.exe'), 'version'], text=True).strip()
    if 'v1.46 ' not in version:
        raise RuntimeError('This layer interface has been verified with the official RenderDoc 1.46 SDK')
    root = Path(__file__).resolve().parents[1]
    build = root / 'pluginApp/build'
    host = {'Windows': 'windows-x86_64', 'Linux': 'linux-x86_64', 'Darwin': 'darwin-x86_64'}[platform.system()]
    toolchain = args.sdk_root / 'ndk' / args.ndk_version / 'toolchains/llvm/prebuilt' / host
    compiler = toolchain / 'bin' / ('clang++.exe' if platform.system() == 'Windows' else 'clang++')
    source = root / 'layer/src/main/cpp/diagnostics/renderdoc_driver.cpp'
    source_hash = digest(source)
    snapshot = build / 'renderdoc-inputs' / (source_hash + '.cpp')
    snapshot.parent.mkdir(parents=True, exist_ok=True)
    snapshot.write_bytes(source.read_bytes())
    records = []
    for abi, package, target in [('arm64-v8a', 'arm64', 'aarch64-linux-android26'),
                                  ('armeabi-v7a', 'arm32', 'armv7a-linux-androideabi26')]:
        apk = args.renderdoc_dir / 'plugins/android' / f'org.renderdoc.renderdoccmd.{package}.apk'
        directory = build / 'renderdoc-jniLibs' / abi
        directory.mkdir(parents=True, exist_ok=True)
        layer = directory / 'libVkLayer_GLES_RenderDoc.so'
        with zipfile.ZipFile(apk) as archive:
            layer.write_bytes(archive.read(f'lib/{abi}/{layer.name}'))
        bridge = directory / 'libvulkan_fix_renderdoc_driver.so'
        subprocess.run([str(compiler), f'--target={target}', f'--sysroot={toolchain / "sysroot"}',
                        '-I', str(root / 'Vulkan-Headers/include'), '-shared', '-fPIC', '-std=c++17',
                        '-O2', '-g', '-fvisibility=hidden', '-Wl,-Bsymbolic-functions',
                        '-Wl,-z,max-page-size=16384', '-Wl,-z,common-page-size=16384',
                        '-Wl,-soname,libvulkan_fix_renderdoc_driver.so', '-static-libstdc++',
                        str(snapshot), '-ldl', '-llog', '-o', str(bridge)], check=True)
        records.append({'abi': abi, 'sdk_apk_sha256': digest(apk), 'layer_sha256': digest(layer),
                        'bridge_sha256': digest(bridge)})
    if digest(source) != source_hash:
        raise RuntimeError('Source changed during staging; run preparation again')
    assets = build / 'renderdoc-assets'
    assets.mkdir(parents=True, exist_ok=True)
    # Preserve the SDK's complete license notices, including third-party notices.
    shutil.copyfile(args.renderdoc_dir / 'LICENSE.rtf', assets / 'RenderDoc-LICENSE.rtf')
    (assets / 'VulkanFix-renderdoc-build.json').write_text(json.dumps(
        {'renderdoc': version, 'bridge_source_sha256': source_hash, 'libraries': records}, indent=2) + '\n', encoding='utf-8')
    print('Staged RenderDoc 1.46 capture libraries for arm64-v8a and armeabi-v7a')


if __name__ == '__main__':
    main()
