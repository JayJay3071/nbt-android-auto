#!/bin/sh
# Record the Bluetooth A2DP sound device /dev/wms/player1 while the phone plays music over Bluetooth (2026-11-23), to
# rebuild it later with the Android Auto sound.
#   sh car-wms-tap.sh start   then: USB unplugged (no Android Auto), music on the phone, Bluetooth entry selected
#   sh car-wms-tap.sh stop    packs /fs/ram3/aatool/wms-result
T=/fs/ram3/aatool
D=$T/wms-result
case "$1" in
start)
  slay -f -Q nbt_ipod_tap 2>/dev/null
  : > $T/wms-tap.log
  ls -l /dev/wms; ls /proc/mount/dev/wms/player1
  ($T/nbt_ipod_tap --mount /dev/wms/player1 --seconds 600 --log $T/wms-tap.log </dev/null >/dev/null 2>&1 &)
  sleep 1; cat $T/wms-tap.log
  ;;
stop)
  slay -f -Q nbt_ipod_tap
  sleep 1
  rm -rf $D; mkdir -p $D
  cp $T/wms-tap.log $D/
  pidin ar | grep -e 192598 -e bt -e BT -e wms | grep -v grep > $D/procs.txt
  sloginfo 2>&1 | grep -e io-nme -e AVRCP -e wms -e A2DP | tail -3000 > $D/slog.txt
  ls -l $D
  ;;
esac
