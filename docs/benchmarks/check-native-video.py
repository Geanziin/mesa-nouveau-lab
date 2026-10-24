#!/usr/bin/env python3
"""Validate native VA-API H.264 pixels with Vulkan deliberately unavailable.

This is a correctness check, not a performance benchmark. Driver files are
selected per subprocess; nothing is installed or changed in /usr.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for data in iter(lambda: f.read(1024 * 1024), b''):
            h.update(data)
    return h.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--driver-dir', required=True, type=Path)
    parser.add_argument('--render-node', default='/dev/dri/renderD128')
    parser.add_argument('--keep-frames', action='store_true')
    args = parser.parse_args()
    driver = (args.driver_dir / 'nvgd_drv_video.so').resolve(strict=True)
    args.output.mkdir(parents=True, exist_ok=False)
    args.output = args.output.resolve()
    env = os.environ.copy()
    env.update(LIBVA_DRIVER_NAME='nvgd', LIBVA_DRIVERS_PATH=str(args.driver_dir.resolve()),
               VK_DRIVER_FILES=str(args.output / 'deliberately-missing-icd.json'),
               VK_ICD_FILENAMES=str(args.output / 'deliberately-missing-icd.json'),
               NOUVEAU_VK_VIDEO='0', MESA_LOG_LEVEL='debug')
    report = {
        'date_utc': datetime.now(timezone.utc).isoformat(),
        'driver': str(driver), 'driver_sha256': digest(driver),
        'environment': {k: env[k] for k in ('LIBVA_DRIVER_NAME', 'LIBVA_DRIVERS_PATH',
                                           'VK_DRIVER_FILES', 'VK_ICD_FILENAMES', 'NOUVEAU_VK_VIDEO',
                                           'MESA_LOG_LEVEL')},
        'cases': [],
        'limitations': ['Correctness, not a codec performance benchmark.',
                        'H.264 covers progressive 8-bit 4:2:0 decode; AV1 covers Profile0 '
                        '8/10-bit 4:2:0 without film grain; HEVC covers Main/Main10 '
                        'without RExt; VP9 covers Profile0 8-bit and Profile2 10-bit.',
                        'Encode checks validate stream structure, not visual quality or speed.',
                        'This does not certify other codecs or all GPUs.'],
    }
    root = Path(__file__).resolve().parents[2]
    native = root / 'src/gallium/drivers/nouveau'
    sources = [root / 'meson.build', root / 'src/compiler/meson.build',
               root / 'src/nouveau/meson.build', root / 'src/nouveau/headers/meson.build',
               root / 'src/gallium/winsys/nouveau/drm/nouveau.c',
               *[native / name for name in ('meson.build', 'nouveau_nvdec.c', 'nouveau_nvdec.h',
                  'nouveau_nvdec_h264.c', 'nouveau_nvdec_h264.h',
                  'nouveau_nvdec_av1.c', 'nouveau_nvdec_av1.h', 'nouveau_nvdec_av1_probs.c',
                  'nouveau_nvdec_hevc.c', 'nouveau_nvdec_hevc.h',
                  'nouveau_nvdec_vp9.c', 'nouveau_nvdec_vp9.h', 'nouveau_nvdec_vp9_tables.h',
                  'nouveau_nvenc.c',
                  'nouveau_nvenc_backend.c', 'nouveau_nvenc_backend.h', 'nouveau_screen.h',
                  'nouveau_vp3_video.c', 'nvc0/nvc0_video.c')]]
    report['source_sha256'] = {str(p.relative_to(root)): digest(p) for p in sources}

    def run(name, command, timeout=90):
        log = args.output / (name + '.log')
        with log.open('x') as f:
            result = subprocess.run(command, env=env, stdout=f, stderr=subprocess.STDOUT,
                                    timeout=timeout)
        return {'command': command, 'returncode': result.returncode, 'log': log.name}

    report['vainfo'] = run('vainfo', ['vainfo', '--display', 'drm', '--device', args.render_node])
    report['kernel'] = run('kernel', ['uname', '-a'])
    report['ffmpeg'] = run('ffmpeg-version', ['ffmpeg', '-version'])
    cases = [
        ('baseline', 'baseline', 128, 96, 24, 0, 1, 1, 'flat'),
        ('main', 'main', 128, 96, 24, 2, 4, 1, 'flat'),
        ('high', 'high', 128, 96, 24, 3, 4, 1, 'flat'),
        ('high-asymmetric', 'high', 128, 96, 24, 3, 4, 3, 'asymmetric'),
        ('main-1080p', 'main', 1920, 1080, 24, 3, 4, 4, 'flat'),
        ('high-cropped', 'high', 318, 178, 24, 3, 4, 3, 'jvt'),
        ('high-16refs', 'high', 128, 96, 48, 8, 16, 1, 'flat'),
        ('high-4k', 'high', 3840, 2160, 8, 2, 4, 4, 'flat'),
    ]
    ok = report['vainfo']['returncode'] == 0
    for name, profile, width, height, frames, bframes, refs, slices, matrix in cases:
        clip = args.output / (name + '.h264')
        cpu = args.output / (name + '-cpu.yuv')
        gpu = args.output / (name + '-vaapi.yuv')
        x264 = [f'slices={slices}']
        if matrix == 'asymmetric':
            for seed, (key, size) in enumerate((('cqm4iy', 16), ('cqm4ic', 16),
                                               ('cqm4py', 16), ('cqm4pc', 16),
                                               ('cqm8iy', 64), ('cqm8py', 64))):
                x264.append(key + '=' + ','.join(str(16 + (i * 7 + seed * 3) % 41)
                                                for i in range(size)))
        else:
            x264.append('cqm=' + matrix)
        case = {'name': name, 'profile': profile, 'width': width, 'height': height,
                'frames': frames, 'bframes': bframes, 'references': refs, 'slices': slices}
        report['cases'].append(case)
        case['generate'] = run(name + '-generate', [
            'ffmpeg', '-nostdin', '-v', 'error', '-f', 'lavfi', '-i',
            f'testsrc2=size={width}x{height}:rate=24', '-frames:v', str(frames),
            '-c:v', 'libx264', '-threads', '1', '-preset', 'fast', '-profile:v', profile,
            '-pix_fmt', 'yuv420p', '-g', '12', '-bf', str(bframes), '-refs', str(refs),
            '-x264-params', ':'.join(x264), '-crf', '18', '-f', 'h264', str(clip)])
        if case['generate']['returncode']:
            case['passed'] = False
            ok = False
            break
        case['input_sha256'] = digest(clip)
        common = ['ffmpeg', '-nostdin', '-v', 'verbose', '-xerror']
        output = ['-vf', 'format=yuv420p', '-c:v', 'rawvideo', '-threads', '1',
                  '-fps_mode', 'passthrough', '-f', 'rawvideo']
        case['cpu'] = run(name + '-cpu', common + ['-hwaccel', 'none', '-threads', '1',
                                                   '-i', str(clip)] + output + [str(cpu)])
        decode = common + ['-init_hw_device', 'vaapi=lab:' + args.render_node,
                           '-hwaccel', 'vaapi', '-hwaccel_device', 'lab',
                           '-hwaccel_output_format', 'vaapi', '-threads', '1', '-i', str(clip),
                           '-vf', 'hwdownload,format=nv12,format=yuv420p',
                           '-c:v', 'rawvideo', '-threads', '1', '-fps_mode', 'passthrough',
                           '-f', 'rawvideo', str(gpu)]
        if name == 'baseline':
            decode = ['strace', '-f', '-qq', '-e', 'trace=openat,ioctl', '-o',
                      str(args.output / 'native.trace')] + decode
        case['vaapi'] = run(name + '-vaapi', decode)
        expected = width * height * 3 // 2 * frames
        case['expected_bytes'] = expected
        case['passed'] = (not case['cpu']['returncode'] and not case['vaapi']['returncode']
                          and cpu.exists() and gpu.exists()
                          and cpu.stat().st_size == expected and gpu.stat().st_size == expected
                          and digest(cpu) == digest(gpu))
        for tag, path in [('cpu', cpu), ('vaapi', gpu)]:
            if path.exists():
                case[tag + '_sha256'] = digest(path)
                case[tag + '_bytes'] = path.stat().st_size
                if not args.keep_frames:
                    path.unlink()  # Only temporary outputs created by this test.
        print(name + ': ' + ('PASS' if case['passed'] else 'FAIL'), flush=True)
        ok &= case['passed']
        (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
        if not case['passed']:
            break
    av1_cases = [
        # (name, width, height, frames, gop, lag, tiles, bit-depth)
        ('av1-intra', 256, 256, 4, 1, 0, (0, 0), 8),
        ('av1-inter', 320, 180, 48, 24, 0, (0, 0), 8),
        ('av1-altref', 320, 180, 48, 24, 16, (0, 0), 8),
        ('av1-tiles', 640, 360, 24, 12, 0, (1, 1), 8),
        ('av1-main10', 320, 180, 48, 24, 16, (0, 0), 10),
    ]
    if ok and len(report['cases']) == len(cases):
        for name, width, height, frames, gop, lag, tiles, depth in av1_cases:
            clip = args.output / (name + '.ivf')
            cpu = args.output / (name + '-cpu.yuv')
            gpu = args.output / (name + '-vaapi.yuv')
            case = {'name': name, 'codec': 'AV1', 'width': width, 'height': height,
                    'frames': frames, 'gop': gop, 'lag': lag, 'tiles': list(tiles),
                    'bit_depth': depth}
            report.setdefault('av1', []).append(case)
            pix = f'yuv420p{"" if depth == 8 else "10le"}'
            case['generate'] = run(name + '-generate', [
                'ffmpeg', '-nostdin', '-v', 'error', '-f', 'lavfi', '-i',
                f'testsrc2=size={width}x{height}:rate=24', '-frames:v', str(frames),
                '-c:v', 'libaom-av1', '-threads', '4', '-row-mt', '1', '-cpu-used', '6',
                '-lag-in-frames', str(lag), '-auto-alt-ref', '1' if lag else '0',
                '-g', str(gop), '-crf', '30', '-tile-columns', str(tiles[0]),
                '-tile-rows', str(tiles[1]), '-pix_fmt', pix, '-f', 'ivf', str(clip)],
                timeout=180)
            if case['generate']['returncode']:
                case['passed'] = False
                ok = False
                break
            case['input_sha256'] = digest(clip)
            common = ['ffmpeg', '-nostdin', '-v', 'verbose', '-xerror']
            download = 'p010le' if depth == 10 else 'nv12'
            case['cpu'] = run(name + '-cpu', common + ['-hwaccel', 'none', '-c:v', 'libdav1d',
                                                        '-threads', '1', '-i', str(clip),
                                                        '-pix_fmt', pix, '-c:v', 'rawvideo',
                                                        '-threads', '1', '-fps_mode', 'passthrough',
                                                        '-f', 'rawvideo', str(cpu)])
            case['vaapi'] = run(name + '-vaapi', common + [
                '-init_hw_device', 'vaapi=lab:' + args.render_node,
                '-hwaccel', 'vaapi', '-hwaccel_device', 'lab',
                '-hwaccel_output_format', 'vaapi', '-c:v', 'av1', '-threads', '1',
                '-i', str(clip), '-vf', f'hwdownload,format={download},format={pix}',
                '-c:v', 'rawvideo', '-pix_fmt', pix, '-threads', '1',
                '-fps_mode', 'passthrough', '-f', 'rawvideo', str(gpu)])
            sample = 2 if depth == 10 else 1
            expected = width * height * 3 // 2 * sample * frames
            case['expected_bytes'] = expected
            case['passed'] = (not case['cpu']['returncode'] and not case['vaapi']['returncode']
                              and cpu.exists() and gpu.exists()
                              and cpu.stat().st_size == expected and gpu.stat().st_size == expected
                              and digest(cpu) == digest(gpu))
            for tag, path in [('cpu', cpu), ('vaapi', gpu)]:
                if path.exists():
                    case[tag + '_sha256'] = digest(path)
                    if not args.keep_frames:
                        path.unlink()
            print(name + ': ' + ('PASS' if case['passed'] else 'FAIL'), flush=True)
            ok &= case['passed']
            (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
            if not case['passed']:
                break
    # HEVC Main / Main10 (P010) and VP9 Profile0 / Profile2, both against
    # their software decoders. The HEVC tools reject RExt streams by design;
    # libx265 marks all-intra GOP-1 clips RExt, so the intra case uses g=2.
    hevc_cases = [
        ('hevc-main', 320, 180, 48, 8),
        ('hevc-main10', 320, 180, 48, 10),
        ('hevc-tiles', 640, 360, 24, 8),
        ('hevc-4k', 3840, 2160, 8, 8),
    ]
    if ok and len(report['cases']) == len(cases):
        for name, width, height, frames, depth in hevc_cases:
            clip = args.output / (name + '.hevc')
            cpu = args.output / (name + '-cpu.yuv')
            gpu = args.output / (name + '-vaapi.yuv')
            case = {'name': name, 'codec': 'HEVC', 'width': width, 'height': height,
                    'frames': frames, 'bit_depth': depth}
            report.setdefault('hevc', []).append(case)
            pix = f'yuv420p{"" if depth == 8 else "10le"}'
            download = 'p010le' if depth == 10 else 'nv12'
            case['generate'] = run(name + '-generate', [
                'ffmpeg', '-nostdin', '-v', 'error', '-f', 'lavfi', '-i',
                f'testsrc2=size={width}x{height}:rate=24', '-frames:v', str(frames),
                '-c:v', 'libx265', '-preset', 'fast', '-x265-params',
                f'keyint=24:bframes=4{":tile-columns=1:tile-rows=1" if name == "hevc-tiles" else ""}',
                '-crf', '28', '-pix_fmt', pix, '-f', 'hevc', str(clip)], timeout=300)
            if case['generate']['returncode']:
                case['passed'] = False
                ok = False
                break
            case['input_sha256'] = digest(clip)
            common = ['ffmpeg', '-nostdin', '-v', 'verbose', '-xerror']
            case['cpu'] = run(name + '-cpu', common + ['-hwaccel', 'none', '-c:v', 'hevc',
                                                        '-threads', '1', '-i', str(clip),
                                                        '-pix_fmt', pix, '-c:v', 'rawvideo',
                                                        '-threads', '1', '-fps_mode', 'passthrough',
                                                        '-f', 'rawvideo', str(cpu)], timeout=180)
            case['vaapi'] = run(name + '-vaapi', common + [
                '-init_hw_device', 'vaapi=lab:' + args.render_node,
                '-hwaccel', 'vaapi', '-hwaccel_device', 'lab',
                '-hwaccel_output_format', 'vaapi', '-c:v', 'hevc', '-threads', '1',
                '-i', str(clip), '-vf', f'hwdownload,format={download},format={pix}',
                '-c:v', 'rawvideo', '-pix_fmt', pix, '-threads', '1',
                '-fps_mode', 'passthrough', '-f', 'rawvideo', str(gpu)], timeout=180)
            sample = 2 if depth == 10 else 1
            expected = width * height * 3 // 2 * sample * frames
            case['expected_bytes'] = expected
            case['passed'] = (not case['cpu']['returncode'] and not case['vaapi']['returncode']
                              and cpu.exists() and gpu.exists()
                              and cpu.stat().st_size == expected and gpu.stat().st_size == expected
                              and digest(cpu) == digest(gpu))
            for tag, path in [('cpu', cpu), ('vaapi', gpu)]:
                if path.exists():
                    case[tag + '_sha256'] = digest(path)
                    if not args.keep_frames:
                        path.unlink()
            print(name + ': ' + ('PASS' if case['passed'] else 'FAIL'), flush=True)
            ok &= case['passed']
            (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
            if not case['passed']:
                break
    vp9_cases = [
        ('vp9-inter', 320, 180, 48, 8, (0, 0)),
        ('vp9-tiles', 640, 360, 24, 8, (2, 1)),
        ('vp9-profile2', 320, 180, 48, 10, (0, 0)),
    ]
    if ok and len(report['cases']) == len(cases):
        for name, width, height, frames, depth, tiles in vp9_cases:
            clip = args.output / (name + '.ivf')
            cpu = args.output / (name + '-cpu.yuv')
            gpu = args.output / (name + '-vaapi.yuv')
            case = {'name': name, 'codec': 'VP9', 'width': width, 'height': height,
                    'frames': frames, 'bit_depth': depth, 'tiles': list(tiles)}
            report.setdefault('vp9', []).append(case)
            pix = f'yuv420p{"" if depth == 8 else "10le"}'
            download = 'p010le' if depth == 10 else 'nv12'
            case['generate'] = run(name + '-generate', [
                'ffmpeg', '-nostdin', '-v', 'error', '-f', 'lavfi', '-i',
                f'testsrc2=size={width}x{height}:rate=24', '-frames:v', str(frames),
                '-c:v', 'libvpx-vp9', '-threads', '4', '-cpu-used', '4', '-crf', '30',
                '-g', '24', '-tile-columns', str(tiles[0]), '-tile-rows', str(tiles[1]),
                '-pix_fmt', pix, '-f', 'ivf', str(clip)], timeout=300)
            if case['generate']['returncode']:
                case['passed'] = False
                ok = False
                break
            case['input_sha256'] = digest(clip)
            common = ['ffmpeg', '-nostdin', '-v', 'verbose', '-xerror']
            case['cpu'] = run(name + '-cpu', common + ['-hwaccel', 'none', '-c:v', 'libvpx-vp9',
                                                        '-threads', '1', '-i', str(clip),
                                                        '-pix_fmt', pix, '-c:v', 'rawvideo',
                                                        '-threads', '1', '-fps_mode', 'passthrough',
                                                        '-f', 'rawvideo', str(cpu)], timeout=180)
            case['vaapi'] = run(name + '-vaapi', common + [
                '-init_hw_device', 'vaapi=lab:' + args.render_node,
                '-hwaccel', 'vaapi', '-hwaccel_device', 'lab',
                '-hwaccel_output_format', 'vaapi', '-c:v', 'vp9', '-threads', '1',
                '-i', str(clip), '-vf', f'hwdownload,format={download},format={pix}',
                '-c:v', 'rawvideo', '-pix_fmt', pix, '-threads', '1',
                '-fps_mode', 'passthrough', '-f', 'rawvideo', str(gpu)], timeout=180)
            sample = 2 if depth == 10 else 1
            expected = width * height * 3 // 2 * sample * frames
            case['expected_bytes'] = expected
            case['passed'] = (not case['cpu']['returncode'] and not case['vaapi']['returncode']
                              and cpu.exists() and gpu.exists()
                              and cpu.stat().st_size == expected and gpu.stat().st_size == expected
                              and digest(cpu) == digest(gpu))
            for tag, path in [('cpu', cpu), ('vaapi', gpu)]:
                if path.exists():
                    case[tag + '_sha256'] = digest(path)
                    if not args.keep_frames:
                        path.unlink()
            print(name + ': ' + ('PASS' if case['passed'] else 'FAIL'), flush=True)
            ok &= case['passed']
            (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
            if not case['passed']:
                break
    trace = args.output / 'native.trace'
    if trace.exists():
        text = trace.read_text()
        report['no_vulkan_icd_opened'] = 'libvulkan_nouveau.so' not in text
        report['drm_pushbuf_submissions'] = text.count('DRM_IOCTL_NOUVEAU_GEM_PUSHBUF')
        ok &= report['no_vulkan_icd_opened'] and report['drm_pushbuf_submissions'] > 0
    else:
        ok = False
    report['encode'] = []
    if ok and len(report['cases']) == len(cases):
        # Native Gallium planes can be separate GEM objects. Confirm that the
        # encoder consumes them directly instead of downloading the input.
        for profile, width, height in [('constrained_baseline', 192, 128), ('main', 192, 128),
                                       ('high', 1920, 1080)]:
            name = f'encode-{profile}-{width}x{height}'
            clip = args.output / (name + '.h264')
            item = {'profile': profile, 'width': width, 'height': height, 'frames': 24}
            report['encode'].append(item)
            item['submit'] = run(name, [
                'ffmpeg', '-nostdin', '-v', 'verbose', '-xerror',
                '-init_hw_device', 'vaapi=lab:' + args.render_node, '-filter_hw_device', 'lab',
                '-f', 'lavfi', '-i', f'testsrc2=size={width}x{height}:rate=24', '-frames:v', '24',
                '-vf', 'format=nv12,hwupload', '-c:v', 'h264_vaapi', '-profile:v', profile,
                '-qp', '24', '-f', 'h264', str(clip)])
            log = (args.output / item['submit']['log']).read_text()
            item['direct_surface_submissions'] = log.count('direct Gallium input:')
            item['verify'] = run(name + '-verify', [
                'ffmpeg', '-nostdin', '-v', 'error', '-xerror', '-threads', '1',
                '-i', str(clip), '-f', 'null', '-'])
            item['probe'] = run(name + '-probe', [
                'ffprobe', '-v', 'error', '-count_frames', '-select_streams', 'v:0',
                '-show_entries', 'stream=codec_name,profile,width,height,nb_read_frames',
                '-of', 'json', str(clip)])
            properties = {}
            if not item['probe']['returncode']:
                streams = json.loads((args.output / item['probe']['log']).read_text())['streams']
                if len(streams) == 1:
                    properties = streams[0]
            item['properties'] = properties
            item['passed'] = (not item['submit']['returncode'] and not item['verify']['returncode']
                              and item['direct_surface_submissions'] == 24
                              and properties.get('codec_name') == 'h264'
                              and properties.get('width') == width and properties.get('height') == height
                              and properties.get('nb_read_frames') == '24')
            if clip.exists():
                item['output_sha256'] = digest(clip)
            print(name + ': ' + ('PASS' if item['passed'] else 'FAIL'), flush=True)
            ok &= item['passed']
            if not item['passed']:
                break
    report['passed'] = bool(ok and len(report['cases']) == len(cases) and len(report['encode']) == 3
                            and len(report.get('av1', [])) == len(av1_cases)
                            and len(report.get('hevc', [])) == len(hevc_cases)
                            and len(report.get('vp9', [])) == len(vp9_cases))
    (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
