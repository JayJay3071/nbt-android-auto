#!/bin/sh
# Start the Android Auto session with the WAL overlay presenter (decoded picture in front of the HMI), RAM only.
# Send with: python tools\nbt_request.py "tr -d '\015' | sh -s" --input-file scripts\car-wal-start.sh --timeout 60
# Preconditions: native_service_live and nbt_wal_present uploaded to /fs/ram3/nbt-video-test (see HANDOFF.md).
# WARNING (2026-11-06 incident): unpaced sprite uploads deadlocked the WalServer and restarted the car. nbt_wal_present
# paces itself; keep the defaults (10 fps, 40 ms settle) for the first runs and watch the WAL statistics.
P=/var/opt/hmi/data/rw/NbtNativePrototype
R=/fs/ram3/nbt-aa-native
T=/fs/ram3/nbt-video-test
if test -e $R/service.lock; then echo 'SERVICE LOCK EXISTS - not starting'; ls -l $R; exit 2; fi
mkdir -p $R
chmod 700 $R
cp $P/native_frames $P/fifo_http $P/decode_runner $P/accessory_window $P/oneplus_native $P/cert.pem $P/key.pem $P/ca.pem $R/
cp $T/native_service_live $R/native_service
cp $T/nbt_wal_present $R/nbt_wal_present
# newer builds (2026-11-06 night): oneplus_native with controller FIFO and phone list, nbt_knob (iDrive controller)
if test -f $T/oneplus_native; then cp $T/oneplus_native $R/oneplus_native; echo 'using oneplus_native from RAM test dir'; fi
if test -f $T/nbt_knob; then cp $T/nbt_knob $R/nbt_knob; chmod 700 $R/nbt_knob; echo 'nbt_knob present: controller forwarding on'; fi
# any Android phone works without a list (TLS: Google Automotive Link chain); peer.pin would restore a fixed pin
if test -f $T/peer.pin; then cp $T/peer.pin $R/peer.pin; fi
cp /opt/mm/bin/NmeCmdLine /opt/mm/lib/cinemo/cinemo_classes.xml /opt/mm/lib/cinemo/cinemo_links.xml $R/
chmod 700 $R/native_service $R/nbt_wal_present $R/native_frames $R/fifo_http $R/decode_runner $R/accessory_window $R/oneplus_native $R/NmeCmdLine
chmod 600 $R/*.pem
rm -f $R/shadow.path
# --idle 600: an Android Auto screen that does not change sends no new frames, keep the last picture for 10 minutes;
# the end of the session stops the presenter anyway (the service ends it with SIGTERM, it hides and removes its sprites).
# Full screen: the phone draws into 800x300 (NBT_VID_INSETS 0,180,0,0, set below and as default in native_service), the
# presenter takes rows 0..299 and the GPU stretches them to 1280x480 (texture 960 KB, smaller than the old 1.5 MB).
echo '--mode full --rows 300 --max-fps 10 --settle-ms 40 --scale 100 --idle 600' > $R/wal.args
# override for tests: /fs/ram3/nbt-video-test/wal.args (car 2026-11-07: 800x300 at ~9 fps made devg-NVRmlite use 65 % CPU
# and ended in an NVRM "Fifo: Parse Error" with a system restart; lower load: --scale 80 --max-fps 6)
if test -f $T/wal.args; then cp $T/wal.args $R/wal.args; echo 'wal.args from RAM test dir'; fi
# HMI gate: when the "Android Auto" plugin page is loaded (it creates /fs/ram3/nbt-aa-hidden at HMI start), the service
# touches the phone only after the page was opened once (/fs/ram3/nbt-aa-request). Without the plugin: old behaviour.
rm -f $R/gate.mode
if test -e /fs/ram3/nbt-aa-hidden -o -e /fs/ram3/nbt-aa-request; then touch $R/gate.mode; echo 'gate.mode: waiting for the Android Auto page'; fi
cd $R
# Phone UI density: reference default 140 (made for a native 1280x480 canvas). Our 800x300 canvas is stretched 1.6x, so at
# 140 everything is 1.6x larger than in the reference and the side bar gets crowded; 88 gives the reference layout exactly
# (softer text). For tests: echo 110 > /fs/ram3/nbt-video-test/vid.dpi  (no file = 140)
DPI=140
if test -f $T/vid.dpi; then DPI=`sed 1q $T/vid.dpi`; fi
echo "phone UI density NBT_VID_DPI=$DPI"
NBT_VID_DPI=$DPI NBT_VID_INSETS=0,180,0,0 LD_LIBRARY_PATH=/opt/mm/lib:/opt/mm/lib/cinemo:/lib:/usr/lib ./native_service </dev/null >/dev/null 2>&1 &
echo "started native_service pid=$!"
sleep 5
echo '=== status'
cat status.txt
echo '=== wal.args'
cat wal.args
echo '=== processes'
pidin ar | grep -e native_service -e accessory_window -e oneplus_native -e nbt_wal_present | grep -v grep
echo '=== WAL statistics must keep ticking'
sloginfo 2>/dev/null | grep delta_t | tail -2
echo '=== END'
