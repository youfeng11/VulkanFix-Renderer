"""Check packaged native symbols and optional 16 KB ELF compatibility."""
import argparse
import glob
from pathlib import Path, PurePosixPath
import re
import subprocess
import tempfile
import zipfile


def check_apk(path, readelf, required_abis, check_16k=False, require_driver_probes=False):
    failures = []
    renderer_abis = set()
    payloads = {}
    libraries = 0
    print(f'{path}: {path.stat().st_size / 1024**2:.2f} MiB', flush=True)
    with zipfile.ZipFile(path) as apk, tempfile.TemporaryDirectory(prefix='vulkanfix-apk-') as temporary:
        for entry in apk.infolist():
            parts = PurePosixPath(entry.filename).parts
            if len(parts) != 3 or parts[0] != 'lib' or not parts[2].endswith('.so'):
                continue
            libraries += 1
            payloads.setdefault(parts[1], set()).add(parts[2])
            if parts[2] in ('libdriver_post_fault_driver.so', 'libdriver_trust_push_fixture.so'):
                failures.append(f'{entry.filename}: test-only forwarding driver must not be packaged')
            if parts[2] == 'libvulkanfix.so':
                renderer_abis.add(parts[1])
            if parts[2] == 'libvulkan_fix.so':
                failures.append(f'{entry.filename}: obsolete renderer library name')
            # Use a fixed temporary filename rather than extracting archive paths.
            library = Path(temporary) / 'library.so'
            library.write_bytes(apk.read(entry))
            result = subprocess.run([readelf, '--file-header', '--sections', '--program-headers', '--wide', str(library)],
                                    capture_output=True, text=True, check=False)
            if result.returncode:
                failures.append(f'{entry.filename}: readelf failed: {result.stderr.strip()}')
                continue
            sections = re.findall(r'^\s*\[\s*\d+\]\s+(\S+)', result.stdout, re.MULTILINE)
            if not sections:
                failures.append(f'{entry.filename}: no ELF sections found')
                continue
            debug = [s for s in sections if s.startswith(('.debug_', '.zdebug_')) or
                     s in ('.symtab', '.gnu_debugdata')]
            print(f'  {entry.filename}: {entry.file_size / 1024**2:.2f} MiB native, '
                  f'{entry.compress_size / 1024**2:.2f} MiB in APK', flush=True)
            if debug:
                failures.append(f'{entry.filename}: unstripped sections: {", ".join(debug)}')
            if parts[2] == 'libvulkanfix_probe.so':
                entry_point = re.search(r'Entry point address:\s*(0x[0-9a-fA-F]+)', result.stdout)
                if not entry_point or int(entry_point.group(1), 16) == 0 or not re.search(r'^\s*INTERP\s', result.stdout, re.MULTILINE):
                    failures.append(f'{entry.filename}: isolated worker is not an executable PIE with an interpreter')
            if check_16k and parts[1] in ('arm64-v8a', 'x86_64'):
                loads = 0
                for line in result.stdout.splitlines():
                    fields = line.split()
                    if fields and fields[0] == 'LOAD':
                        loads += 1
                        if int(fields[-1], 16) < 16384:
                            failures.append(f'{entry.filename}: LOAD alignment is below 16 KB')
                    if fields and fields[0] == 'GNU_RELRO':
                        if (int(fields[2], 16) + int(fields[5], 16)) % 16384:
                            failures.append(f'{entry.filename}: GNU_RELRO end is not 16 KB aligned')
                if not loads:
                    failures.append(f'{entry.filename}: no ELF LOAD segments found')
    if not libraries or not renderer_abis:
        failures.append('APK is missing libvulkanfix.so')
    missing = required_abis - renderer_abis
    if missing:
        failures.append(f'missing renderer ABIs: {", ".join(sorted(missing))}')
    if require_driver_probes:
        for abi in sorted(renderer_abis):
            missing = {'libvulkanfix_post.so', 'libvulkanfix_probe.so'} - payloads.get(abi, set())
            for name in sorted(missing):
                failures.append(f'lib/{abi}/{name}: required driver probe payload is missing')
    for failure in failures:
        print(f'FAIL: {path}: {failure}', flush=True)
    if not failures:
        print(f'PASS: {path}: native libraries are stripped', flush=True)
    return not failures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--readelf', default='llvm-readelf', help='NDK llvm-readelf executable')
    parser.add_argument('--require-abis', default='', help='Comma-separated renderer ABIs required in each APK')
    parser.add_argument('--check-16k', action='store_true', help='Check 64-bit LOAD and GNU_RELRO alignment')
    parser.add_argument('--require-driver-probes', action='store_true', help='Require POST and executable worker for every renderer ABI')
    parser.add_argument('apks', nargs='+', help='APK paths or glob patterns')
    args = parser.parse_args()
    required = {abi for abi in args.require_abis.split(',') if abi}
    paths = set()
    for pattern in args.apks:
        matches = glob.glob(pattern)
        if not matches:
            parser.error(f'no APK files matched: {pattern}')
        paths.update(Path(match) for match in matches)
    success = True
    for path in sorted(paths):
        try:
            success = check_apk(path, args.readelf, required, args.check_16k, args.require_driver_probes) and success
        except (OSError, ValueError, zipfile.BadZipFile) as error:
            print(f'FAIL: {path}: {error}', flush=True)
            success = False
    return 0 if success else 1


if __name__ == '__main__':
    raise SystemExit(main())
