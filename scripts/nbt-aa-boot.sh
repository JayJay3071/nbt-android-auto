#!/bin/sh
# Android Auto on the original NBT: start the service from the PERSISTENT package (installed by car-aa-autostart-install.sh).
# Started by the "Android Auto" HMI plugin (libNbtAndroidAuto.so) ~15 s after the HMI loaded it, and again when the page
# is opened while the service is not running. Also usable by hand: sh /var/opt/hmi/data/rw/NbtAndroidAutoRT/boot.sh
# Hardware video path (VideoApp, car-proven 2026-11-07), iDrive controller (nbt_knob), RAM only at runtime.
# Disable the automatic start: touch /var/opt/hmi/data/rw/NbtAndroidAutoRT/disabled
P=/var/opt/hmi/data/rw/NbtAndroidAutoRT
R=/fs/ram3/nbt-aa-native
LOG=/fs/ram3/nbt-aa-boot.log
exec >> $LOG 2>&1
echo "=== boot.sh `date`"
if test -e $P/disabled; then echo 'disabled'; exit 0; fi
if test -e $R/service.lock; then echo 'service already running (lock)'; exit 0; fi
mkdir -p $R
chmod 700 $R
for f in native_service oneplus_native nbt_knob nbt_hwvideo decode_runner accessory_window fifo_http; do
  cp $P/$f $R/$f && chmod 700 $R/$f
done
cp $P/cert.pem $P/key.pem $P/ca.pem $R/ && chmod 600 $R/*.pem
cp /opt/mm/bin/NmeCmdLine /opt/mm/lib/cinemo/cinemo_classes.xml /opt/mm/lib/cinemo/cinemo_links.xml $R/
chmod 700 $R/NmeCmdLine
touch $R/hw.mode
# optional sharper picture: phone sends 1280x720 (touch $P/res720 to enable, rm to go back to 800x480)
rm -f $R/res720
if test -e $P/res720; then touch $R/res720; echo 'res720 on'; fi
rm -f $R/wal.args $R/shadow.path
# the plugin page exists: the phone is only switched after the page was opened once
if test -e /fs/ram3/nbt-aa-hidden -o -e /fs/ram3/nbt-aa-request; then touch $R/gate.mode; fi
DPI=140
if test -f $P/vid.dpi; then DPI=`sed 1q $P/vid.dpi`; fi
# picture mode (car-tested 2026-11-08), one flag file in $P, none = classic 800x300 stretched to the whole display:
#   anamorph   whole 800x480 canvas, phone told pixels are 1.6x wide, stretched to 1280 horizontally only
#   center800  whole 800x480 canvas 1:1 in the middle, black bars left/right
#   split800   whole 800x480 canvas 1:1 at the left edge, iDrive split screen visible on the right
# These modes use the full 800x480 canvas: DPI 150 by default (the 800x300 DPI of 100 would make everything tiny),
# own value in $P/mode.dpi.
# Default without any flag: anamorph ("Breitbild" in the Android Auto page, which writes split800 or anamorph);
# $P/classic restores the old 800x300 picture.
rm -f $R/center800 $R/anamorph $R/split800
MODE=anamorph
for m in split800 center800 anamorph classic; do
  if test -e $P/$m; then MODE=$m; break; fi
done
if test $MODE != classic; then
  touch $R/$MODE; DPI=150                    # 140 -> 150 (user 2026-11-09: everything a bit bigger)
  if test -f $P/mode.dpi; then DPI=`sed 1q $P/mode.dpi`; fi
fi
echo "picture mode $MODE, dpi $DPI"
cd $R
NBT_VID_DPI=$DPI NBT_VID_INSETS=0,180,0,0 LD_LIBRARY_PATH=/opt/mm/lib:/opt/mm/lib/cinemo:/lib:/usr/lib ./native_service </dev/null >/dev/null 2>&1 &
echo "native_service started pid=$!"

# Android Auto turn-by-turn in the instrument cluster (arrow, distance, street, road shield, lanes; nbt_cluster draws only
# while the phone guides and yields to the iDrive navigation). Disable: touch $P/cluster-off
if test -f $P/nbt_cluster -a ! -e $P/cluster-off; then
  # stop a running copy first: a running binary cannot be overwritten (car 2026-11-10: "cp: Resource busy")
  slay nbt_cluster 2>/dev/null
  sleep 1
  cp $P/nbt_cluster $R/nbt_cluster && chmod 700 $R/nbt_cluster
  (cd $R; LD_LIBRARY_PATH=/opt/sys/lib:/lib:/usr/lib ./nbt_cluster </dev/null > /fs/ram3/nbt-cluster.log 2>&1 &)
  echo 'cluster guidance started'
fi

# Diagnostic logs on PERSISTENT storage (car 2026-11-07: after an Android Auto drop-out SSH only worked again after a
# restart, and the RAM logs were gone). Written only when the public state changes (connecting, running, draining, ...),
# so the flash sees a few small writes per session. The previous boot's set is kept as logs.prev.
L=$P/logs
rm -r $L.prev 2>/dev/null
if test -d $L; then mv $L $L.prev; fi
mkdir -p $L
# optional passive MOST trace of the cluster graphics terminal (FBlock 0x82) right after the start, when the NBT creates
# its drawing instances (touch $P/trace-most; writes $L/most-boot-82.txt, 4 minutes, observe only)
if test -e $P/trace-most -a -x $R/nbt_knob; then
  (cd $R; LD_LIBRARY_PATH=/opt/sys/lib:/lib:/usr/lib ./nbt_knob --trace --filter 82 --seconds 240 --fifo /fs/ram3/nbt-trace-dummy.fifo > $L/most-boot-82.txt 2>&1 &)
  echo 'most boot trace started'
fi
# optional read-only observer for the cluster investigation (touch $P/trace-cluster; writes $L/cluster-trace.txt)
if test -e $P/trace-cluster -a -f $P/cluster-trace.sh; then
  sh $P/cluster-trace.sh 3600 </dev/null >/dev/null 2>&1 &
  echo "cluster trace started pid=$!"
fi
(
  last=''
  while test -e $R/service.lock -o -z "$last"; do
    s=`cat /fs/ram3/nbt-aa-state 2>/dev/null | tr '\n' ' '`
    if test "$s" != "$last"; then
      last="$s"
      echo "`date +%T` $s" >> $L/states.txt
      cp /fs/ram3/nbt-aa-plugin.log /fs/ram3/nbt-aa-boot.log $L/ 2>/dev/null
      for k in capture decoder hwvideo; do
        f=`ls -t $R/$k-*.log 2>/dev/null | sed 1q`
        if test -n "$f"; then cp $f $L/$k-last.log; fi
      done
      tail -200 $R/knob.log > $L/knob-tail.log 2>/dev/null
      tail -100 /fs/ram3/nbt-cluster.log > $L/cluster-tail.log 2>/dev/null
      sloginfo 2>/dev/null | grep -e DisplayManagerScreen -e nvideocapture -e 'WindowVisible' -e 'zorder' -e 'NVRM' -e watchdog -e nme -e Nme -e NME -e udio | tail -60 > $L/slog-tail.txt
    fi
    sleep 5
  done
  echo "`date +%T` service ended" >> $L/states.txt
) </dev/null >/dev/null 2>&1 &
