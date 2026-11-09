"""Send one PNG to the already opened NBT Bildwechsel prototype.

Uses the existing authenticated SSH worker. Does not restart HMI or retry writes.
Call only after confirming the new page is registered/open on the vehicle.
"""
import argparse
import binascii
import json
from pathlib import Path
import struct
import subprocess
import sys
import time
import uuid

ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('png',type=Path)
p.add_argument('--page-open-confirmed',action='store_true',required=True)
a=p.parse_args()
data=a.png.read_bytes()
if len(data)>512*1024 or data[:8]!=b'\x89PNG\r\n\x1a\n':p.error('Expected PNG <=512KiB')
width,height=struct.unpack('>II',data[16:24])
if not(0<width<=800 and 0<height<=480):p.error('Image exceeds 800x480')
run=ROOT/'build'/('image-frame-'+time.strftime('%Y%m%dT%H%M%S')+'-'+uuid.uuid4().hex[:8])
run.mkdir(parents=True)
def remote(command,*args):
 subprocess.run([sys.executable,str(ROOT/'tools/nbt_request.py'),command,*args],cwd=ROOT,check=True)
remote('cat /fs/ram3/nbt-live-status.txt; sleep 1','--output-file',str(run/'before.txt'))
status=dict(line.split('=',1) for line in (run/'before.txt').read_text().splitlines() if '=' in line)
if status.get('state') in ('stopped','expired'):p.error('Receiver stopped/expired; no frame sent')
instance=int(status['instance']);sequence=(int(status['sequence'])+1)&0xffffffff
if not sequence:sequence=1
header=struct.pack('<8s6I',b'NBTFRM1\0',instance,sequence,len(data),binascii.crc32(data)&0xffffffff,0,0)
packet=run/'frame.bin';packet.write_bytes(header+data)
staged='/fs/ram3/nbt-live-'+uuid.uuid4().hex[:12]+'.tmp'
remote('umask 077; set -C; cat > '+staged,'--input-file',str(packet))
remote('mv '+staged+' /fs/ram3/nbt-live-frame.bin')
remote('sleep 1; cat /fs/ram3/nbt-live-status.txt; sleep 1','--output-file',str(run/'after.txt'))
after=dict(line.split('=',1) for line in (run/'after.txt').read_text().splitlines() if '=' in line)
ok=int(after.get('instance','0'))==instance and int(after.get('sequence','0'))==sequence
(run/'result.json').write_text(json.dumps({'accepted':ok,'instance':instance,'sequence':sequence,'source':str(a.png.resolve()),'visual_confirmation':False},indent=2))
print('FRAME_UPDATE_SUBMITTED' if ok else 'FRAME_NOT_ACKNOWLEDGED',run)
sys.exit(0 if ok else 2)
