"""Validate a bounded AA video capture and decode its first image locally.

Does not contact the vehicle or change installed HMI resources. A successful
PNG proves a decodable captured image, not vehicle display or live projection.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--capture', type=Path, required=True)
    parser.add_argument('--index', type=Path, required=True)
    parser.add_argument('--ffmpeg', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not 0 < args.capture.stat().st_size <= 8 * 1024 * 1024:
        parser.error('Capture must be nonempty and at most 8 MiB')
    if args.index.stat().st_size > 1024 * 1024:
        parser.error('Index exceeds bound')
    data = args.capture.read_bytes()
    offset = frames = configs = 0
    timestamps = []
    with args.index.open(newline='') as stream:
        for number, row in enumerate(csv.reader(stream)):
            if number >= 4096 or len(row) != 5:
                parser.error('Invalid or oversized capture index')
            kind, count, pts, start, size = row
            count, pts, start, size = map(int, (count, pts, start, size))
            if (kind not in ('config', 'frame') or count != frames or start != offset
                    or not 0 <= pts < 2**64 or not 4 <= size <= 2 * 1024 * 1024
                    or start + size > len(data)):
                parser.error('Capture boundary/counter mismatch')
            payload = data[start:start + size]
            if not payload.startswith((b'\0\0\1', b'\0\0\0\1')):
                parser.error('Payload does not start with Annex-B data')
            if kind == 'frame':
                frames += 1
                timestamps.append(pts)
            else:
                configs += 1
            offset += size
    if offset != len(data) or not frames or not configs or frames > 300:
        parser.error('Incomplete, empty or oversized indexed capture')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    image = out / 'first-frame.png'
    command = [str(args.ffmpeg.resolve()), '-nostdin', '-v', 'warning', '-f', 'h264',
               '-i', str(args.capture.resolve()), '-frames:v', '1', '-vf', 'scale=480:-2',
               '-update', '1', str(image)]
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
    (out / 'decode.log').write_bytes(result.stderr)
    if result.returncode or not image.exists() or not image.read_bytes().startswith(b'\x89PNG\r\n\x1a\n'):
        raise RuntimeError('No decoded first image; inspect decode.log')
    report = {'capture_sha256': hashlib.sha256(data).hexdigest(),
              'indexed_media_messages': frames, 'codec_config_messages': configs,
              'pts': timestamps, 'decoded_first_image': True, 'vehicle_display_tested': False,
              'decoder_warnings': bool(result.stderr), 'command': command}
    (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print('AA_FIRST_IMAGE_DECODED ' + str(image))


if __name__ == '__main__':
    main()
