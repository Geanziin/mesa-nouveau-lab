#!/usr/bin/env python3
"""Repeat a fixed vkmark workload on NVK or NVIDIA without changing drivers."""
import argparse
import csv
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('driver', choices=('nvgd', 'nvidia'))
parser.add_argument('output', type=Path)
parser.add_argument('--runs', type=int, default=3)
parser.add_argument('--reference', type=Path, help='Check benchmark/data hashes against a prior environment.json')
args = parser.parse_args()
if args.runs < 1:
    parser.error('--runs must be positive')
icd = Path('/usr/share/vulkan/icd.d') / (
    'nouveau_icd.x86_64.json' if args.driver == 'nvgd' else 'nvidia_icd.json')
module = Path('/sys/module') / ('nvgd' if args.driver == 'nvgd' else 'nvidia')
if not module.exists() or not icd.is_file():
    parser.error('Selected driver must already be loaded and its ICD installed')
if not os.environ.get('WAYLAND_DISPLAY'):
    parser.error('Run in a Wayland desktop session for both drivers')
binary = shutil.which('vkmark')
if not binary:
    parser.error('Install vkmark first')
args.output.mkdir(parents=True, exist_ok=False)
command = [binary, '--winsys', 'wayland', '--present-mode', 'immediate',
           '--size', '800x600', '--pixel-format', 'B8G8R8A8Unorm']
env = os.environ.copy()
for key in ('VK_ICD_FILENAMES', 'VK_INSTANCE_LAYERS', 'VK_LAYER_ENABLES',
            'VK_LOADER_DEBUG', 'VK_LOADER_LAYERS_ENABLE'):
    env.pop(key, None)
env.update(VK_DRIVER_FILES=str(icd), VK_LOADER_LAYERS_DISABLE='~all~')


def query(command):
    try:
        result = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=30, env=env)
        return {'command': command, 'returncode': result.returncode, 'output': result.stdout}
    except (OSError, subprocess.TimeoutExpired) as error:
        return {'command': command, 'error': str(error)}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read(path):
    try:
        return path.read_text().strip()
    except OSError:
        return None


device = Path('/sys/bus/pci/devices/0000:03:00.0')
hwmon = next(iter(sorted((device / 'hwmon').glob('hwmon*'))), None)
fields = {name: hwmon / file if hwmon else None for name, file in {
    'gpu_hz': 'freq1_input', 'memory_hz': 'freq2_input',
    'power_uw': 'power1_average', 'temperature_mc': 'temp1_input',
}.items()}
fields.update(gpu_busy_percent=device / 'gpu_busy_percent',
              pcie_speed=device / 'current_link_speed')
metadata = {
    'started_utc': datetime.now(timezone.utc).isoformat(),
    'driver': args.driver, 'command': command, 'runs': args.runs,
    'warmup': 'cube:duration=3 before each measured suite',
    'suite': 'vkmark default scenes, default 10 seconds per scene',
    'environment': {k: env[k] for k in ('VK_DRIVER_FILES', 'VK_LOADER_LAYERS_DISABLE')},
    'driver_overrides': {k: v for k, v in env.items()
                         if k.startswith(('NVGD_', 'NVK_', 'NAK_', 'MESA_', '__GL_', '__VK_'))},
    'kernel': query(['uname', '-r']),
    'packages': query(['pacman', '-Q', 'vkmark', 'assimp', 'mesa-git', 'lib32-mesa-git',
                       'nvgd-dkms', 'nvidia-open', 'nvidia-utils', 'gnome-shell', 'mutter']),
    'vulkan': query(['vulkaninfo', '--summary']),
    'status_before': query(['nvgd-status'] if args.driver == 'nvgd' else ['nvidia-smi']),
    'cpu_governor': read(Path('/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor')),
    'cpu_model': next((s.split(':', 1)[1].strip() for s in Path('/proc/cpuinfo').read_text().splitlines()
                       if s.startswith('model name')), None),
    'load_before': read(Path('/proc/loadavg')),
    'input_sha256': {str(p): digest(p) for p in
                     [Path(binary), *sorted(Path('/usr/lib/vkmark').glob('*.so')),
                      *sorted(p for p in Path('/usr/share/vkmark').rglob('*') if p.is_file())]},
    'limitations': ['Normal desktop session, not an isolated workstation.',
                    'FrameTime in vkmark is reciprocal FPS, not input latency or p99.',
                    'No visual correctness certification is implied by a score.'],
}
(args.output / 'environment.json').write_text(json.dumps(metadata, indent=2) + '\n')
if args.reference:
    reference = json.loads(args.reference.read_text())
    if reference['input_sha256'] != metadata['input_sha256']:
        raise SystemExit('Benchmark binary/plugins/data changed; do not compare these runs')
scores = []
for run in range(1, args.runs + 1):
    with (args.output / f'warmup-{run}.log').open('x') as output:
        subprocess.run(command + ['-b', 'cube:duration=3'], env=env,
                       stdout=output, stderr=subprocess.STDOUT, check=True, timeout=45)
    start = time.monotonic()
    log = args.output / f'run-{run}.log'
    with log.open('x') as output, (args.output / f'run-{run}-telemetry.csv').open('x') as telemetry:
        writer = csv.DictWriter(telemetry, fieldnames=['seconds', *fields])
        writer.writeheader()
        child = subprocess.Popen(command, env=env, stdout=output, stderr=subprocess.STDOUT)
        try:
            while child.poll() is None:
                writer.writerow({'seconds': round(time.monotonic() - start, 3),
                                 **{k: read(p) if p else None for k, p in fields.items()}})
                telemetry.flush()
                if time.monotonic() - start > 600:
                    child.kill()
                    raise RuntimeError('Benchmark exceeded 600 seconds')
                time.sleep(0.5)
        finally:
            if child.poll() is None:
                child.terminate()
            try:
                child.wait(timeout=10)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
    text = log.read_text()
    score = re.findall(r'vkmark Score:\s*(\d+)', text)
    names = re.findall(r'Device Name:\s*(.+)', text)
    scenes = re.findall(r'^\[[^\]]+\] .*: FPS: \d+', text, re.M)
    if child.returncode or not score or len(scenes) != 13 or len(names) != 1 or '3050' not in names[0]:
        raise RuntimeError(f'Run {run}: failed benchmark or wrong device; inspect {log}')
    if ('NVK' in names[0]) != (args.driver == 'nvgd'):
        raise RuntimeError('Unexpected Vulkan driver selected')
    scores.append(int(score[-1]))
    print(f'run {run}: score={scores[-1]}, elapsed={time.monotonic()-start:.1f}s', flush=True)
summary = {
    'driver': args.driver, 'scores': scores, 'median_score': statistics.median(scores),
    'min_score': min(scores), 'max_score': max(scores),
    'status_after': query(['nvgd-status'] if args.driver == 'nvgd' else ['nvidia-smi']),
    'command': command, 'input_sha256': metadata['input_sha256'],
}
(args.output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
print(json.dumps({k: summary[k] for k in ('driver', 'scores', 'median_score', 'min_score', 'max_score')}, indent=2))
