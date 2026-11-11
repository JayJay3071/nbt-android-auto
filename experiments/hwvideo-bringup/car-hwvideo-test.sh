#!/bin/sh
# First hardware-video test (2026-11-07): test clip through the STOCK video output (Cinemo output 1, windows
# CinemoVideo1_TaskA/B), then the HMI's own MOST sequence to VideoApp (FBlock D7 VideoConnectionMaster, D8 VideoSink,
# captured in build/video-trace-20261107/) to put it on the CID full screen, 15 s later the stop sequence.
# Send with: python tools\nbt_request.py "tr -d '\015' | sh -s" --input-file scripts\car-hwvideo-test.sh --timeout 90
T=/fs/ram3/nbt-video-test
cd $T || exit 2
ID='?video-output-id=1&video-output-aspect-x=1&video-output-aspect-y=1&video-window-class=CinemoVideo1_TaskA,CinemoVideo1_TaskB&video-subpicture-window-class=CinemoSubpicture1_TaskA,CinemoSubpicture1_TaskB'
rm -f hw-run.log hw-start.log hw-stop.log
(LD_LIBRARY_PATH=/opt/mm/lib/cinemo:/opt/mm/lib:/lib:/usr/lib ./video_runner 40 ./NmeCmdLine -a0 -wd 0 -wi 0 -wm 0 -vd NmeDeviceScreen -vi "$ID" -r -i $T/pattern-800x480.h264 > hw-run.log 2>&1 &)
sleep 4
K="./nbt_knob --trace --filter D7,D8 --fifo $T/none.fifo"
# start: connection source 0x16 -> CID, current source, source viewport 800x480, StartVideo, screen viewport 0,0,1280,480
LD_LIBRARY_PATH=/opt/sys/lib:/lib:/usr/lib $K --seconds 4 \
  --send 2008,D7,01,060,6,0,1,0,FFFF,01011601 \
  --send 2007,D8,01,C05,0,0,1,0,FFFF,1600 \
  --send 2007,D8,01,C03,0,0,1,0,FFFF,00000000032001e0 \
  --send 2007,D8,01,C01,6,0,1,0,FFFF,0101 \
  --send 2007,D8,01,C04,0,0,1,0,FFFF,00000000050001e0 > hw-start.log 2>&1
echo '=== start replies'
grep -e 'TX ' -e '2007->2' -e '2008->2' hw-start.log | sed 30q
sleep 15
LD_LIBRARY_PATH=/opt/sys/lib:/lib:/usr/lib $K --seconds 3 \
  --send 2007,D8,01,C02,6,0,1,0,FFFF,0102 \
  --send 2008,D7,01,061,6,0,1,0,FFFF,01021601 \
  --send 2007,D8,01,C05,0,0,1,0,FFFF,0000 > hw-stop.log 2>&1
echo '=== stop replies'
grep -e 'TX ' -e '2007->2' -e '2008->2' hw-stop.log | sed 30q
echo '=== system log'
sloginfo | grep -e DisplayManager -e nvideocapture | tail -30
echo '=== decoder'
sed 12q hw-run.log
echo '=== END'
