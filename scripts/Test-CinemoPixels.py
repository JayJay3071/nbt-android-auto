"""Compare Cinemo's MP4/Annex-B YUV420P dumps with an independent decoder.

No vehicle access and no proprietary runtime loading. All three inputs must
contain exactly the requested number of tightly packed YUV420P frames.
"""
import argparse
import hashlib
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--annexb', type=Path, required=True)
    parser.add_argument('--mp4', type=Path, required=True)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--width', type=int, required=True)
    parser.add_argument('--height', type=int, required=True)
    parser.add_argument('--frames', type=int, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if min(args.width, args.height, args.frames) <= 0 or args.width % 2 or args.height % 2:
        parser.error('Positive frame count and positive even YUV420P dimensions required')
    frame_size = args.width * args.height * 3 // 2
    paths = [args.annexb, args.mp4, args.reference]
    expected = frame_size * args.frames
    for path in paths:
        if path.stat().st_size != expected:
            parser.error(f'{path}: expected exactly {expected} bytes')
    digests = [hashlib.sha256() for _ in paths]
    mismatches = []
    reference_frames = []
    with paths[0].open('rb') as a, paths[1].open('rb') as b, paths[2].open('rb') as r:
        for index in range(args.frames):
            chunks = [stream.read(frame_size) for stream in (a, b, r)]
            for digest, data in zip(digests, chunks):
                digest.update(data)
            if any(len(data) != frame_size for data in chunks) or not chunks[0] == chunks[1] == chunks[2]:
                mismatches.append(index)
            reference_frames.append(hashlib.sha256(chunks[2]).hexdigest())
        if a.read(1) or b.read(1) or r.read(1):
            parser.error('Input size changed while reading')
    result = {
        'pixel_format': 'yuv420p', 'width': args.width, 'height': args.height,
        'frames': args.frames, 'bytes_per_output': expected,
        'all_pixels_identical': not mismatches, 'mismatched_frames_zero_based': mismatches,
        'inputs': [{'path': str(path), 'sha256': digest.hexdigest()}
                   for path, digest in zip(paths, digests)],
        'reference_frame_sha256': reference_frames,
    }
    with args.output.open('x', encoding='utf-8') as stream:
        json.dump(result, stream, indent=2)
        stream.write('\n')
    print(f'CINEMO_PIXELS_{"PASS" if not mismatches else "FAIL"} frames={args.frames} mismatches={len(mismatches)}')
    return int(bool(mismatches))


if __name__ == '__main__':
    raise SystemExit(main())
