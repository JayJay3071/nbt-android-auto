#!/bin/sh
# Car test "iPhone protocol over Bluetooth, faked for the OnePlus" (2026-11-23).
#   sh car-iap-fake.sh start [url|block]   then switch the OnePlus' Bluetooth off and on (USB / Android Auto stays)
#     url: sound as HTTP WAV stream for the head unit's own player (nbt_aa_wav_http instead of nbt_a2i_play) - FAILED,
#          the player opens only wms: devices
#     wms [44100]: own A2DP device /dev/wms/nbtaa with SBC (nbt_aa_wms instead of nbt_a2i_play), default 48 kHz
#   sh car-iap-fake.sh status
#   sh car-iap-fake.sh stop    ends everything, packs the logs into /fs/ram3/aatool/iap-result
# 1. nbt_ipod_tap --iap 1 stands in front of the Bluetooth audio driver at /fs/avrcp0 (attached within 15 ms of the
#    mount) and changes only <iap status="not_supported"/> to "supported" in the device description - the head unit then
#    takes the phone as "iPod over Bluetooth capable" (a real iPhone says supported).
# 2. When /fs/avrcp0 is there, our iPod driver is started at /fs/btipod 3 s later (real iPhone: the Apple mount comes
#    a few seconds after the audio mount) and answers message 0x80 like the stock driver (away for 2 s, then back).
#    When /fs/avrcp0 goes, our driver is ended.
# Files expected in /fs/ram3/aatool: nbt_ipod_tap, nbt_aa_ipod.bt
T=/fs/ram3/aatool
R=/fs/ram3/nbt-aa-native
D=$T/iap-result
running() { pidin ar | grep "$1" | grep -v grep > /dev/null; }
# by pid: slay by name missed programs started with a full path (car 2026-11-24: an old nbt_aa_wav_http kept reading)
end_all() { for p in `pidin ar | grep "$1" | grep -v grep | sed 's/^ *//' | cut -d' ' -f1`; do kill -9 $p 2>/dev/null; done; }
case "$1" in
start)
  if ls /proc/mount/fs/btipod > /dev/null 2>&1; then echo "/fs/btipod in use (iPhone connected?): iPhone Bluetooth OFF first"; exit 1; fi
  slay -f -Q nbt_ipod_tap 2>/dev/null; slay -f -Q nbt_aa_ipod.bt 2>/dev/null
  rm -f $T/iap.stop
  : > $T/avrcp-tap.log; : > $T/btfake.log; : > $T/iap-loop.log
  # "start block": the head unit's player io-nme may not open /fs/avrcp0 (its silent A2DP stream would hold our input)
  BLOCK=''
  if test "$2" = block; then
    NME=`pidin ar | grep '/opt/mm/bin/io-nme' | grep -v grep | sed 's/^ *//' | cut -d' ' -f1 | sed 1q`
    if test -n "$NME"; then BLOCK="--block-pid $NME"; echo "io-nme pid $NME kept away from /fs/avrcp0"; fi
  fi
  # "start url": the head unit's player gets the Android Auto sound as an HTTP WAV stream (nbt_aa_wav_http reads the
  # session's audio.fifo instead of nbt_a2i_play) and the playback status follows Android Auto
  URL=''
  if test "$2" = url; then
    end_all nbt_a2i_play; end_all nbt_aa_wav_http
    : > $T/wav.log
    (cd $R; LD_LIBRARY_PATH=/proc/boot:/lib:/usr/lib:/opt/mm/lib $T/nbt_aa_wav_http $R/audio.fifo 18766 </dev/null >> $T/wav.log 2>&1 &)
    URL="--url http://127.0.0.1:18766/aa.wav --run $R"
    sleep 1; cat $T/wav.log
  fi
  # "start wms" (2026-11-24): our own A2DP sound device /dev/wms/nbtaa (nbt_aa_wms: SBC made from the session's
  # audio.fifo, instead of nbt_a2i_play); the URL request is answered with it, so the head unit's own player plays it
  if test "$2" = wms; then
    end_all nbt_a2i_play; end_all nbt_aa_wms; end_all nbt_aa_wav_http
    : > $T/wms.log
    RATE=''
    if test -n "$3"; then RATE="--rate $3"; fi
    (cd $R; LD_LIBRARY_PATH=/proc/boot:/lib:/usr/lib $T/nbt_aa_wms --fifo $R/audio.fifo $RATE --log $T/wms.log </dev/null >/dev/null 2>&1 &)
    URL="--url wms:/net/hu-intel.nbt//dev/wms/nbtaa --run $R"
    sleep 1; cat $T/wms.log; ls -l /dev/wms
  fi
  ($T/nbt_ipod_tap --mount /fs/avrcp0 --iap 1 $BLOCK $URL --seconds 1800 --log $T/avrcp-tap.log </dev/null >/dev/null 2>&1 &)
  (
    while ! test -e $T/iap.stop && running 'nbt_ipod_tap --mount /fs/avrcp0'; do
      if ls /proc/mount/fs/avrcp0 > /dev/null 2>&1; then
        if ! running 'nbt_aa_ipod.bt --mount'; then
          sleep 3
          echo "`date +%T` avrcp0 there: starting our iPod driver at /fs/btipod" >> $T/iap-loop.log
          (cd $R; $T/nbt_aa_ipod.bt --mount /fs/btipod --run $R --remount 2000 --verbose 1 --log $T/btfake.log </dev/null >/dev/null 2>&1 &)
        fi
      elif running 'nbt_aa_ipod.bt --mount'; then
        echo "`date +%T` avrcp0 gone: ending our iPod driver" >> $T/iap-loop.log
        slay -f -Q nbt_aa_ipod.bt
      fi
      sleep 1
    done
    slay -f -Q nbt_aa_ipod.bt 2>/dev/null
    echo "`date +%T` loop ended" >> $T/iap-loop.log
  ) </dev/null >/dev/null 2>&1 &
  sleep 1
  cat $T/avrcp-tap.log
  echo "ON. Now: OnePlus Bluetooth OFF, 5 s, ON again (USB stays). Then look for 'Android Auto' in iDrive."
  ;;
status)
  echo "--- mounts"; ls /proc/mount/fs
  echo "--- loop"; cat $T/iap-loop.log
  echo "--- tap"; grep -e attached -e 'self test' -e 'iap ' -e 'url ' -e left -e 'mount ' $T/avrcp-tap.log | tail -10 | cut -c1-180
  echo "--- wav stream"; tail -6 $T/wav.log 2>/dev/null | cut -c1-160
  echo "--- our A2DP device"; tail -8 $T/wms.log 2>/dev/null | cut -c1-160
  echo "--- player"; sloginfo | grep -e 'cinemo Z' | grep -v -e IOM_EV_RESOURCE -e 'state [01]:0:0' | tail -12 | cut -c8-180
  echo "--- our driver"; grep -v -e 00000318 -e 'pid 184390' $T/btfake.log | grep -e 'msg' -e remount -e 'open .FS_info./control' -e attached | tail -12 | cut -c1-160
  echo "--- stores"; qdbc -d mme 'select msid,slotid,available,mssname,name,mountpath from mediastores where slotid in (15,18,21);'
  echo "--- markers"; sloginfo | grep -e 'MARKER HU_' -e 'BT-ServiceConnect' | tail -8 | cut -c8-160
  ;;
stop)
  touch $T/iap.stop
  slay -f -Q nbt_ipod_tap; slay -f -Q nbt_aa_ipod.bt
  # back to the normal sound player (it is not restarted by the service by itself)
  if pidin ar | grep -e nbt_aa_wav_http -e nbt_aa_wms | grep -v grep > /dev/null; then
    end_all nbt_aa_wav_http; end_all nbt_aa_wms
    (cd $R; LD_LIBRARY_PATH=/opt/mm/lib:/opt/mm/lib/cinemo:/lib:/usr/lib ./nbt_a2i_play audio.fifo `sed 1q $R/sound.args 2>/dev/null` </dev/null >> $R/sound.log 2>&1 &)
    echo "nbt_a2i_play started again"
  fi
  sleep 2
  rm -rf $D; mkdir -p $D
  cp $T/avrcp-tap.log $T/iap-loop.log $D/
  cp $T/wav.log $T/wms.log $D/ 2>/dev/null
  grep -v 00000318 $T/btfake.log > $D/btfake.log
  sloginfo 2>&1 | tail -20000 > $D/slog-full.txt
  qdbc -d mme 'select msid,slotid,available,mssname,name,mountpath from mediastores where slotid in (15,18,21);' > $D/stores.txt 2>&1
  ls -l $D
  ;;
*) echo "usage: sh car-iap-fake.sh start | status | stop"; exit 2 ;;
esac
