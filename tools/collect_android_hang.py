"""Collect a live Android rendering hang without signals, ptrace, or GPU work.

Requires adb and root for private /proc entries. AArch64 stack reads are limited
to 16 KiB within the selected thread's stack mapping. Candidate return addresses
are evidence for symbolication, not a complete or atomic userspace backtrace.
"""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import shlex
import shutil
import struct
import subprocess


def parse_maps(text):
    mappings = []
    for line in text.splitlines():
        match = re.match(r'^([0-9a-f]+)-([0-9a-f]+) (\S+) ([0-9a-f]+) \S+ \d+\s*(.*)', line)
        if match:
            start, end = int(match[1], 16), int(match[2], 16)
            if start < end:
                mappings.append((start, end, match[3], int(match[4], 16), match[5]))
    return mappings


def stack_range(mappings, sp, tid):
    for start, end, permissions, _, name in mappings:
        if (start <= sp < end and permissions.startswith('r') and
                (name == '[stack]' or name == f'[anon:stack_and_tls:{tid}]')):
            base = max(start, sp & ~4095)
            size = min(16384, end - base) // 4096 * 4096
            if size >= 4096:
                return base, size
    raise ValueError('The sampled stack pointer is outside the selected thread stack mapping')


def frame_candidates(data, base, sp, mappings):
    stack_end = base + len(data)
    candidates = []
    for offset in range(0, len(data) - 15, 8):
        next_fp, lr = struct.unpack_from('<QQ', data, offset)
        address = base + offset
        if address < sp or not address < next_fp < stack_end or next_fp % 16:
            continue
        for start, end, permissions, file_offset, name in mappings:
            if start <= lr - 4 < end and 'x' in permissions:
                candidates.append({'slot': hex(address), 'next_fp': hex(next_fp),
                                   'return_pc': hex(lr), 'file': name,
                                   'caller_offset': hex(lr - 4 - start + file_offset)})
                break
    return candidates


def start_time(stat):
    # comm may contain spaces and parentheses; fields following its last ')' start at field 3.
    return stat.rsplit(')', 1)[1].split()[19]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--adb', default=shutil.which('adb') or 'D:/SDK/platform-tools/adb.exe')
    parser.add_argument('--device', required=True)
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--render-tid', type=int)
    parser.add_argument('--read-render-stack', action='store_true')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.pid <= 0 or (args.render_tid is not None and args.render_tid <= 0):
        parser.error('Process and thread identifiers must be positive')
    root = Path(__file__).resolve().parents[1]
    output = args.output or root / 'layer/build/reports/android-hang' / (
        str(args.pid) + '-' + datetime.now().strftime('%Y%m%d-%H%M%S'))
    output.mkdir(parents=True, exist_ok=True)
    metadata = {'pid': args.pid, 'device': args.device,
                'captured_at_utc': datetime.now(timezone.utc).isoformat(), 'errors': {}}

    def read(command, timeout=20):
        result = subprocess.run([args.adb, '-s', args.device, 'exec-out',
                                 'su -c ' + shlex.quote(command)],
                                capture_output=True, timeout=timeout)
        if result.returncode:
            raise RuntimeError(result.stderr.decode('utf-8', errors='replace').strip() or
                               f'Command exited with status {result.returncode}')
        return result.stdout

    def collect(name, command):
        try:
            data = read(command)
            (output / name).write_bytes(data)
            return data.decode('utf-8', errors='replace')
        except (RuntimeError, subprocess.TimeoutExpired) as error:
            metadata['errors'][name] = str(error)
            return ''

    pid = args.pid
    initial_stat = collect('process-stat.txt', f'cat /proc/{pid}/stat')
    if not initial_stat:
        (output / 'metadata.json').write_text(json.dumps(metadata, indent=2), encoding='utf-8')
        raise SystemExit('The selected process is unavailable; saved collection errors')
    identity = start_time(initial_stat)
    metadata['process_start_ticks'] = identity
    collect('process-status.txt', f'cat /proc/{pid}/status')
    maps = collect('maps.txt', f'cat /proc/{pid}/maps')
    collect('memory.txt', 'cat /proc/meminfo /proc/pressure/memory /proc/pressure/io')
    collect('threads.txt', f'ps -T -p {pid} -o PID,TID,COMM,WCHAN')
    collect('gpu-utilization.txt', 'cat /sys/kernel/ged/hal/gpu_utilization')
    kernel = collect('kernel.txt', 'dmesg')
    rendering = collect('rendering-log.txt',
                        f'logcat -d --pid={pid} -t 1000 -s VulkanFix:I renderdoc:I ' + shlex.quote('*:S'))
    matches = re.findall(r'\b' + str(pid) +
                         r'\s+(\d+)\s+I VulkanFix\s*:.*(?:push descriptor:|vertex stores:|synchronization2:)',
                         rendering)
    tid = args.render_tid or (int(matches[-1]) if matches else None)
    metadata['render_tid'] = tid
    if tid:
        syscall = collect('render-syscall.txt', f'cat /proc/{pid}/task/{tid}/syscall')
        collect('render-kernel-stack.txt', f'cat /proc/{pid}/task/{tid}/stack')
        if args.read_render_stack:
            try:
                if read('uname -m').decode().strip() != 'aarch64':
                    raise ValueError('Passive stack decoding currently supports AArch64 only')
                elf_header = read(f'od -An -t u1 -N5 /proc/{pid}/exe').decode().split()
                if elf_header != ['127', '69', '76', '70', '2']:
                    raise ValueError('The selected process is not a 64-bit ELF process')
                if start_time(read(f'cat /proc/{pid}/stat').decode()) != identity:
                    raise ValueError('Process identity changed during collection')
                words = syscall.split()
                if len(words) != 9 or words[0] == '-1':
                    raise ValueError('Thread is not stably waiting in a system call')
                sp = int(words[-2], 16)
                mappings = parse_maps(maps)
                base, size = stack_range(mappings, sp, tid)
                pages = (size + 4095) // 4096
                data = read(f'dd if=/proc/{pid}/mem bs=4096 skip={base // 4096} '
                            f'count={pages} status=none')
                if len(data) != size:
                    raise ValueError(f'Incomplete stack read: expected {size}, received {len(data)}')
                if read(f'cat /proc/{pid}/task/{tid}/syscall').decode().strip() != syscall.strip():
                    raise ValueError('Thread moved during stack collection; memory sample discarded')
                (output / 'render-stack.bin').write_bytes(data)
                candidates = frame_candidates(data, base, sp, mappings)
                (output / 'frame-candidates.json').write_text(json.dumps(candidates, indent=2), encoding='utf-8')
                metadata['stack_sample'] = {'base': hex(base), 'sp': hex(sp), 'bytes': size,
                                            'candidate_frames': len(candidates)}
            except (ValueError, IndexError, RuntimeError, subprocess.TimeoutExpired) as error:
                metadata['errors']['render-stack'] = str(error)
    elif args.read_render_stack:
        metadata['errors']['render-stack'] = 'Render thread was not found; pass --render-tid'
    gpu_faults = []
    context = False
    for line in kernel.splitlines():
        if 'Queue group to be terminated' in line:
            gpu_faults.append(line)
        if 'mali:' in line and 'Ctx ' in line:
            context = f'Ctx {pid}_' in line
            if context:
                gpu_faults.append(line)
        elif context and line.startswith(('CS_FAULT.', 'CS_FAULT_INFO.')):
            gpu_faults.append(line)
        else:
            context = False
    (output / 'gpu-faults.txt').write_text('\n'.join(gpu_faults) + '\n', encoding='utf-8')
    metadata['note'] = 'Passive, non-atomic snapshots; GPU OoM lines require their adjoining context for attribution'
    (output / 'metadata.json').write_text(json.dumps(metadata, indent=2), encoding='utf-8')
    print(f'Saved live hang evidence to {output.resolve()}')
    print(json.dumps(metadata, indent=2))


if __name__ == '__main__':
    main()
