#!/bin/sh
# READ-ONLY inventory (2026-11-08): (1) can the video capture run wider than 800x480 (CAPTURE_FORMAT_WVGA)?
# (2) how does the NBT feed the 6WB instrument cluster (turn arrows, junction view / "Kreuzungszoom")?
# Writes nothing on the head unit. Send with:
#   python tools\nbt_request.py "tr -d '\015' | sh -s" --input-file scripts\car-inspect-res-cluster.sh --timeout 110
str() { # printable strings of a binary that match a pattern (no 'strings' needed)
  tr -c '[:print:]' '\n' < "$1" 2>/dev/null | grep -e "$2" | sort -u | sed 60q
}
echo '=== 1 capture: process and options'
pidin arg 2>/dev/null | grep -i -e nvideocapture -e DisplayManager -e VideoApp -e WalServer -e devg- -e io-display | grep -v grep
B=`ls /opt/sys/bin/dev-nvideocapture /sbin/dev-nvideocapture /usr/sbin/dev-nvideocapture /bin/dev-nvideocapture 2>/dev/null | sed 1q`
echo "binary: $B"
test -n "$B" && use $B 2>&1 | sed 40q
echo '=== 1 capture: format names in the driver'
test -n "$B" && str $B 'CAPTURE_FORMAT\|FORMAT_\|WVGA\|WXGA\|1280\|720P\|eCaptureFormat'
echo '=== 1 capture: configuration files'
ls -l /opt/sys/etc/ 2>/dev/null | grep -i -e capture -e display -e video -e kombi -e apix
for f in /opt/sys/etc/videocapture*; do test -f "$f" && { echo "--- $f"; sed 80q "$f"; }; done
echo '=== 1 Intel display side (DisplayManager screens and modes)'
for f in /etc/system/config/display.conf /opt/sys/etc/DisplayManager*.cfg /opt/sys/etc/displaymanager*; do test -f "$f" && { echo "--- $f"; sed 60q "$f"; }; done
sloginfo 2>/dev/null | grep -i -e 'DisplayManagerScreen' -e 'mode' -e 'resolution' -e 'timing' | grep -i -e screen -e display | sed 30q
echo '=== 2 cluster: processes'
pidin arg 2>/dev/null | grep -i -e kombi -e cluster -e apix -e ihu -e hud -e nav | grep -v grep | sed 30q
echo '=== 2 cluster: files'
ls /opt/sys/etc /opt/hmi/etc /opt/nav 2>/dev/null | grep -i -e kombi -e cluster -e apix -e hud -e junction -e crossing | sed 40q
echo '=== 2 cluster: second screen / video task B in the log'
sloginfo 2>/dev/null | grep -i -e kombi -e cluster -e apix -e 'task 1' -e 'TaskB' -e 'screen 1' -e 'display 1' -e junction -e crossing -e zoom | sed 40q
echo '=== 2 cluster: names in WAL / HMI libraries'
for f in /opt/sys/lib/libwal_nvidia.so /opt/sys/bin/WalServer; do
  test -f $f && { echo "--- $f"; str $f 'Kombi\|KOMBI\|Cluster\|CLUSTER\|Screen\|screen[0-9]\|Display[0-9]\|APIX'; }
done
for f in `ls /opt/hmi/lib/*.so 2>/dev/null | grep -i -e kombi -e nav -e map | sed 15q`; do echo "lib: $f"; done
echo '=== 2 cluster: MOST FBlocks seen (Kombi = FBlock 0x?? in the registry, see nbt_hwvideo --registry)'
echo 'done'
