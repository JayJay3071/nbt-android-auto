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
# optional: Android Auto media sound over USB (without the program the phone plays over Bluetooth)
# a leftover player would share the sound FIFO with the new one (and a running binary cannot be overwritten)
slay -f -Q nbt_a2i_play 2>/dev/null
# a controller observer that was started by hand (swapped while a session ran) is not a child of the old service
slay -f -Q nbt_knob 2>/dev/null
# Android Auto as "iPod over Bluetooth" (car 2026-11-24, see iap.sh): native_service gets our A2DP sound device
# (nbt_aa_wms) as its media sound player under the name nbt_a2i_play; the old player is not used then.
# Off: touch $P/iap.off (then the old player and the Bluetooth entry as before).
IAP=''
if test -x $P/nbt_aa_wms -a -x $P/nbt_ipod_tap -a -x $P/nbt_aa_ipod -a -f $P/iap.sh -a ! -e $P/iap.off; then IAP=1; fi
for p in `pidin ar | grep -e 'nbt_ipod_tap --mount' -e 'nbt_aa_ipod --mount' -e 'nbt_aa_wms' | grep -v grep | sed 's/^ *//' | cut -d' ' -f1`; do
  kill -9 $p 2>/dev/null
done
if test -n "$IAP"; then
  cp $P/nbt_aa_wms $R/nbt_a2i_play && chmod 700 $R/nbt_a2i_play
  echo 'ipod over bluetooth mode on'
elif test -x $P/nbt_a2i_play; then cp $P/nbt_a2i_play $R/nbt_a2i_play && chmod 700 $R/nbt_a2i_play; fi
if test -e $P/sound.args; then cp $P/sound.args $R/sound.args; fi
# Sound on the Bluetooth audio source (car 2026-11-23): with sound.args "input_stereo_0 2" the Android Auto media
# sound goes to the amplifier input the head unit routes for its Bluetooth / iPod sources, so it is heard on the
# phone's Bluetooth audio entry in iDrive (phone connected by Bluetooth as usual) and not in the music collection.
# sound.nostop: the player does not stop the stock music-collection player first (not needed on that input).
# The iPod-in-the-snap-in trial (2026-11-14 .. 11-23, $P/ipod) is removed: its stand-in driver is no longer installed
# and accessory_window keeps the stock iPod driver path blocked as before.
rm -f $R/sound.nostop /fs/ram3/aatool/stub
if test -e $P/sound.nostop; then touch $R/sound.nostop; fi
if test -e $P/sound.args; then echo "sound on `sed 1q $P/sound.args`"; fi
# Android Auto title / artist / length / cover in the phone's Bluetooth audio entry (2026-11-23, nbt_bt_inject in front
# of the stock Bluetooth audio driver while the phone is connected by Bluetooth). Off: touch $P/btinject.off
slay -f -Q nbt_bt_inject 2>/dev/null
if test -z "$IAP" -a -x $P/nbt_bt_inject -a -f $P/bt-inject.sh -a ! -e $P/btinject.off; then
  cp $P/nbt_bt_inject $R/nbt_bt_inject && chmod 700 $R/nbt_bt_inject
  BTINJECT=1
fi
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
# Decoder picture path (2026-11-24): nbt_decoder (valinet's decoder, GPLv3) shows the phone's 1280x720 picture sharp on the
# whole 1280x480 display at 30 pictures/s, instead of the stock player's 800 pixels stretched. Used when it is installed.
#   touch $P/decoder.off   the old picture path again (from the next start of the head unit)
#   touch $P/decoder.hw    the car's own H.264 decoder instead of FFmpeg on the CPU (valinet's test path)
#   $P/decoder.args        one line of extra arguments for nbt_decoder
# The picture modes (16:9 / widescreen) do not apply to it: always the whole display.
# A running nbt_decoder outlives service restarts (its car stream works once per boot, see native_service): its
# program file stays then, a new one in the package runs after the next start of the head unit.
rm -f $R/decoder.mode $R/decoder.hw $R/decoder.args
test -e $R/decoder.pid || rm -f $R/nbt_decoder
if test -f $P/nbt_decoder -a ! -e $P/decoder.off; then
  test -e $R/decoder.pid || { cp $P/nbt_decoder $R/nbt_decoder && chmod 700 $R/nbt_decoder; }
  test -x $R/nbt_decoder && touch $R/decoder.mode
  test -e $P/decoder.hw && touch $R/decoder.hw
  test -f $P/decoder.args && cp $P/decoder.args $R/decoder.args
  echo "picture: nbt_decoder (1280x720 sharp`test -e $R/decoder.hw && echo ', hardware decoding'`)"
fi
# CarPlay (valinet's nbt_carplay): with it installed, an iPhone gets CarPlay through the same decoder and window.
# touch $P/carplay.off = Apple devices are left alone as before; $P/carplay.args = one line of extra arguments.
rm -f $R/nbt_carplay $R/carplay.args
if test -f $P/nbt_carplay -a ! -e $P/carplay.off -a -e $R/decoder.mode; then
  cp $P/nbt_carplay $R/nbt_carplay && chmod 700 $R/nbt_carplay && echo 'CarPlay: nbt_carplay'
  test -f $P/carplay.args && cp $P/carplay.args $R/carplay.args
fi
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
if test -n "$BTINJECT"; then (sh $P/bt-inject.sh </dev/null >/dev/null 2>&1 &); echo 'bluetooth entry injection on'; fi
if test -n "$IAP"; then (sh $P/iap.sh </dev/null >/dev/null 2>&1 &); echo 'ipod over bluetooth started'; fi

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
# Since 2026-11-11 the set of every service run is kept in $P/journal/<start time>/ (the newest 60 runs, about a week of
# driving; each set is a few hundred KB), so the logs of a long drive can be fetched afterwards
# (scripts/car-journal-collect.sh). While the service runs the set is refreshed once a minute, so switching the car off
# loses at most the last minute.
L=$P/logs
J=$P/journal
mkdir -p $J
rm -r $L.prev 2>/dev/null
if test -d $L; then mv $L $J/`date +%Y%m%d-%H%M%S`; fi
n=0
for d in `ls -t $J`; do n=$((n+1)); if test $n -gt 60; then rm -r $J/$d; fi; done
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
  n=0
  while test -e $R/service.lock -o -z "$last"; do
    s=`cat /fs/ram3/nbt-aa-state 2>/dev/null | tr '\n' ' '`
    n=$((n+1))
    changed=0
    if test "$s" != "$last"; then changed=1; fi
    # on every state change, and once a minute
    if test $changed = 1 -o $n -ge 12; then
      n=0
      if test $changed = 1; then
        last="$s"
        echo "`date +%T` $s" >> $L/states.txt
      fi
      # the plugin log (what iDrive did with the controller) only when it has grown
      cmp -s /fs/ram3/nbt-aa-plugin.log $L/nbt-aa-plugin.log || cp /fs/ram3/nbt-aa-plugin.log $L/ 2>/dev/null
      cp /fs/ram3/nbt-aa-boot.log $R/sound.log $L/ 2>/dev/null
      for k in decoder hwvideo; do
        f=`ls -t $R/$k-*.log 2>/dev/null | sed 1q`
        if test -n "$f"; then cp $f $L/$k-last.log; fi
      done
      # Every session log of this run without the per-packet lines, each in its own file and only when it has changed
      # (2026-11-12: only the newest log was kept, so the short failed sessions after a long drive overwrote the drive).
      for f in `ls $R/capture-*.log 2>/dev/null`; do
        grep -v -e 'Frame RX' -e ' Data: ' -e 'Streaming Media' -e 'TLS record' -e 'audio media' $f | tail -5000 > /fs/ram3/nbt-aa-cap.tmp
        b=${f##*/}
        cmp -s /fs/ram3/nbt-aa-cap.tmp $L/$b || cp /fs/ram3/nbt-aa-cap.tmp $L/$b
      done
      rm -f /fs/ram3/nbt-aa-cap.tmp
      # fifo_http: connections, stalls of the player, why the chain ended
      cmp -s $R/http.log $L/http.log || cp $R/http.log $L/ 2>/dev/null
      # fill level of the player's queue once a minute (delay and restarts show here first)
      q=`sloginfo 2>/dev/null | grep 'NmeGraph video' | tail -1`
      if test -n "$q"; then echo "`date +%T` $q" >> $L/nme-queue.txt; fi
      tail -300 $R/knob.log > $L/knob-tail.log 2>/dev/null
      # decoder picture path: what nbt_decoder logged last (statistics every 5 s) and what the service did with it
      for f in `ls $R/carplay-*.log 2>/dev/null`; do tail -400 $f > $L/${f##*/}; done
      if test -e $R/nbt_decoder.log; then
        tail -200 $R/nbt_decoder.log > $L/nbt_decoder-tail.log
        cp $R/decoder.notes $L/ 2>/dev/null
      fi
      # steering wheel keys seen by nbt_knob (to find the skip keys)
      grep -e '^.Wheel' $R/knob.log 2>/dev/null | tail -200 > /fs/ram3/nbt-aa-cap.tmp
      cmp -s /fs/ram3/nbt-aa-cap.tmp $L/wheel.txt || cp /fs/ram3/nbt-aa-cap.tmp $L/wheel.txt
      rm -f /fs/ram3/nbt-aa-cap.tmp
      tail -100 /fs/ram3/nbt-cluster.log > $L/cluster-tail.log 2>/dev/null
      if test $changed = 1; then
        # the USB side of a session's start or end (bus errors, unplug, the car's own iPod handlers): to tell a bad
        # cable from a device that was reset (2026-11-25: connections ended every minute, the reason was not logged)
        { date +%T; sloginfo 2>/dev/null | grep -e EHCI -e OHCI -e CLASS_ -e 'Removal Callback' -e 'ipod audio' | tail -40; } >> $L/usb-events.txt
        tail -400 $L/usb-events.txt > /fs/ram3/nbt-aa-cap.tmp && cp /fs/ram3/nbt-aa-cap.tmp $L/usb-events.txt; rm -f /fs/ram3/nbt-aa-cap.tmp
        sloginfo 2>/dev/null | grep -e DisplayManagerScreen -e nvideocapture -e 'WindowVisible' -e 'zorder' -e 'NVRM' -e watchdog -e nme -e Nme -e NME -e udio -e a2i | grep -v -e 'NmeGraph video' -e AVRCP | tail -120 > $L/slog-tail.txt
      fi
    fi
    sleep 5
  done
  echo "`date +%T` service ended" >> $L/states.txt
) </dev/null >/dev/null 2>&1 &
