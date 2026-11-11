"""Create a short synthetic H.264 MP4 and equivalent Annex-B stream locally.

Verifies both decode to the same 30 frames using laptop FFmpeg. This is fixture
validation, not evidence that Cinemo or the vehicle decoder works.
"""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / '.local/video-tools'))
import imageio_ffmpeg


def main():
    ffmpeg = imageio_ffmpeg.get_ffmpeg_exe()
    out = ROOT / 'build' / ('video-fixture-' + time.strftime('%Y%m%dT%H%M%SZ', time.gmtime()) + '-' + uuid.uuid4().hex[:8])
    out.mkdir(parents=True)
    commands = []

    def run(args):
        command = [ffmpeg, '-nostdin', '-hide_banner', *args]
        commands.append(command)
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30, check=True)
        return result.stdout, result.stderr

    mp4, raw = out / 'pattern-800x480.mp4', out / 'pattern-800x480.h264'
    _, log = run(['-f', 'lavfi', '-i', 'testsrc2=size=800x480:rate=30', '-frames:v', '30',
                  '-an', '-c:v', 'libx264', '-threads', '1', '-profile:v', 'baseline',
                  '-level:v', '3.1', '-pix_fmt', 'yuv420p', '-g', '30', '-bf', '0',
                  '-crf', '23', '-movflags', '+faststart', str(mp4)])
    (out / 'encode.log').write_bytes(log)
    run(['-i', str(mp4), '-map', '0:v:0', '-c:v', 'copy', '-bsf:v', 'h264_mp4toannexb', '-f', 'h264', str(raw)])
    hashes = []
    for path in [mp4, raw]:
        stdout, stderr = run(['-v', 'error', '-i', str(path), '-map', '0:v:0', '-an',
                              '-pix_fmt', 'yuv420p', '-f', 'framemd5', '-'])
        (out / (path.suffix[1:] + '-framemd5.txt')).write_bytes(stdout)
        if stderr:
            raise RuntimeError('Decoder emitted errors: ' + stderr.decode(errors='replace'))
        frames = [line.rsplit(',', 1)[1].strip() for line in stdout.decode().splitlines()
                  if line and not line.startswith('#')]
        if len(frames) != 30:
            raise RuntimeError('Expected exactly 30 decoded frames')
        hashes.append(frames)
    if hashes[0] != hashes[1]:
        raise RuntimeError('MP4 and Annex-B decoded frames differ')
    report = {'fixture_validated': True, 'frames': 30, 'width': 800, 'height': 480,
              'fps': 30, 'codec': 'H.264 constrained baseline', 'level': '3.1',
              'audio': False, 'mp4_annexb_decoded_frames_equal': True,
              'validation_decoder': 'laptop FFmpeg', 'cinemo_tested': False,
              'vehicle_tested': False, 'ffmpeg_version': imageio_ffmpeg.get_ffmpeg_version(),
              'ffmpeg_sha256': hashlib.sha256(Path(ffmpeg).read_bytes()).hexdigest(),
              'files_sha256': {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in [mp4, raw]},
              'commands': commands}
    (out / 'results.json').write_text(json.dumps(report, indent=2))
    print('FIXTURE_OK: 30 identical decoded frames from MP4 and Annex-B; no Cinemo/vehicle execution')
    print('Evidence:', out)


if __name__ == '__main__':
    main()
