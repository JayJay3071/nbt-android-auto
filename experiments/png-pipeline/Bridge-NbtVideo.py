"""Bounded prototype: growing NBT H.264 -> laptop FFmpeg -> native NBT PNG updates.

This is a low-rate development bridge, not standalone Android Auto projection.
Use --local-replay with --dry-run to validate incremental decoding without SSH.
"""
import argparse,binascii,json,os,struct,subprocess,sys,threading,time,uuid
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--capture',help='Existing growing /fs/ram3/nbt-aa-*.h264')
p.add_argument('--local-replay',type=Path)
p.add_argument('--dry-run',action='store_true')
p.add_argument('--seconds',type=int,default=65)
p.add_argument('--page-open-confirmed',action='store_true')
a=p.parse_args()
if not 1<=a.seconds<=75:p.error('seconds must be 1..75')
if bool(a.capture)==bool(a.local_replay):p.error('Specify capture OR local-replay')
if a.capture and (not a.capture.startswith('/fs/ram3/nbt-aa-') or not a.capture.endswith('.h264') or any(c not in '/.-_0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ' for c in a.capture)):p.error('Invalid capture path')
if not a.dry_run and not a.page_open_confirmed:p.error('Confirm the dynamic page is open')
out=ROOT/'build'/('video-bridge-'+time.strftime('%Y%m%dT%H%M%S')+'-'+uuid.uuid4().hex[:8]);out.mkdir()
def remote(cmd,*args):
 subprocess.run([sys.executable,str(ROOT/'tools/nbt_request.py'),cmd,*args],cwd=ROOT,check=True,stdout=subprocess.DEVNULL)
instance=sequence=0
if not a.dry_run:
 remote('cat /fs/ram3/nbt-live-status.txt; sleep 1','--output-file',str(out/'before.txt'))
 status=dict(x.split('=',1) for x in (out/'before.txt').read_text().splitlines() if '=' in x)
 if status['state'] not in ('focused','polling','updated','rejected'):p.error('Receiver is not active: '+status['state'])
 instance=int(status['instance']);sequence=int(status['sequence'])
ffmpeg=ROOT/'.local/video-tools/imageio_ffmpeg/binaries/ffmpeg-win-x86_64-v7.1.exe'
log=(out/'ffmpeg.log').open('wb')
proc=subprocess.Popen([str(ffmpeg),'-nostdin','-v','error','-probesize','32','-analyzeduration','0','-flags','low_delay','-f','h264','-i','pipe:0','-an','-vf','fps=3,scale=480:288','-c:v','png','-f','image2pipe','-flush_packets','1','pipe:1'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=log)
latest=[0,None];guard=threading.Lock();events=[];start=time.monotonic();eof_time=None
def exact(n):
 parts=bytearray()
 while len(parts)<n:
  b=proc.stdout.read(n-len(parts))
  if not b:return None
  parts.extend(b)
 return bytes(parts)
def decode():
 while True:
  sig=exact(8)
  if sig is None:return
  if sig!=b'\x89PNG\r\n\x1a\n':return
  image=bytearray(sig)
  while True:
   h=exact(8)
   if h is None:return
   size=struct.unpack('>I',h[:4])[0]
   if size>512*1024 or len(image)+size+12>512*1024:return
   data=exact(size+4)
   if data is None:return
   image.extend(h);image.extend(data)
   if h[4:]==b'IEND':break
  with guard:
   latest[0]+=1;latest[1]=bytes(image)
   events.append({'decoded':latest[0],'seconds':time.monotonic()-start})
thread=threading.Thread(target=decode,daemon=True);thread.start()
source=a.local_replay.open('rb') if a.local_replay else None
offset=sent=last_decoded=0
def publish_latest():
 global sent,last_decoded,sequence
 with guard: decoded,png=latest
 if png is None or decoded==last_decoded:return
 (out/'latest.png').write_bytes(png)
 if not a.dry_run:
  sequence=(sequence+1)&0xffffffff or 1
  packet=struct.pack('<8s6I',b'NBTFRM1\0',instance,sequence,len(png),binascii.crc32(png)&0xffffffff,0,0)+png
  (out/'packet.bin').write_bytes(packet)
  temp='/fs/ram3/nbt-live-bridge-'+uuid.uuid4().hex[:8]+'.tmp'
  remote('umask 077; set -C; cat > '+temp+' && mv '+temp+' /fs/ram3/nbt-live-frame.bin','--input-file',str(out/'packet.bin'))
 sent+=1;last_decoded=decoded
 print('FRAME',sent,'decoded',decoded,'source_bytes',offset,flush=True)
try:
 while time.monotonic()-start<a.seconds:
  if source:
   chunk=source.read(4096)
   if not chunk:break
   time.sleep(.03)
  else:
   block=offset//4096
   remote('dd if='+a.capture+' bs=4096 skip='+str(block)+' count=64 2>/dev/null; sleep 1','--output-file',str(out/'chunk.bin'))
   chunk=(out/'chunk.bin').read_bytes()[offset%4096:]
  if chunk:
   if offset+len(chunk)>8*1024*1024:raise RuntimeError('Capture byte bound exceeded')
   proc.stdin.write(chunk);proc.stdin.flush();offset+=len(chunk)
  publish_latest()
  if not chunk:time.sleep(.1)
 # Allow available decoded images to arrive before EOF; record whether streaming worked.
 time.sleep(.5);publish_latest();eof_time=time.monotonic()-start
 proc.stdin.close();proc.wait(timeout=10);thread.join(timeout=2);publish_latest()
finally:
 if source:source.close()
 if proc.poll() is None:proc.kill();proc.wait()
 log.close()
result={'bytes':offset,'decoded':latest[0],'published':sent,'eof_seconds':eof_time,'decoded_before_eof':bool(events and eof_time is not None and events[0]['seconds']<eof_time),'decoder_exit':proc.returncode,'events':events,'dry_run':a.dry_run,'capture':a.capture,'local_replay':str(a.local_replay) if a.local_replay else None}
if not a.dry_run:
 remote('cat /fs/ram3/nbt-live-status.txt; sleep 1','--output-file',str(out/'after.txt'))
(out/'result.json').write_text(json.dumps(result,indent=2));print('RESULT',json.dumps({k:v for k,v in result.items() if k!='events'}),out)
sys.exit(0 if latest[0] and result['decoded_before_eof'] and proc.returncode==0 else 2)
