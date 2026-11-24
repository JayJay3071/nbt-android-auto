#!/bin/sh
# One car test of the Bluetooth-entry injection (2026-11-23), so that little time is spent in the car.
#   sh car-bt-inject-test.sh start   check, start the sound watch and nbt_bt_inject (verbose), print the first answers
#   sh car-bt-inject-test.sh stop    end nbt_bt_inject (the stock driver takes over again) and pack all logs
# Files expected in /fs/ram3/aatool: nbt_bt_inject, car-bt-sound-watch.sh
T=/fs/ram3/aatool
R=/fs/ram3/nbt-aa-native
M=/fs/avrcp0
case "$1" in
start)
  echo "--- mount table entry of $M (needed by the program to see the stock driver)"
  ls /proc/mount$M 2>&1
  ls $M/.FS_info. 2>&1 || { echo "no $M: phone not connected by Bluetooth"; exit 1; }
  pidin ar | grep nbt_bt_inject | grep -v grep && { echo "already running"; exit 1; }
  (sh $T/car-bt-sound-watch.sh 300 > /dev/null 2>&1 &)
  : > $T/inject.log
  ($T/nbt_bt_inject --mount $M --run $R --log $T/inject.log --verbose 1 > /dev/null 2>&1 &)
  sleep 4
  cat $T/inject.log
  echo "--- through the program now:"
  $T/probe $M/.FS_info./control 40400e19 64 2>&1 | sed -n '1p;3p'
  $T/probe $M/.FS_info./control 40010e64 256 2>&1 | sed -n '1,2p'
  $T/probe $M/.FS_info./control 40010e66 256 2>&1 | sed -n '1,2p'
  ;;
stop)
  slay -f -Q nbt_bt_inject
  sleep 2
  ls $M/.FS_info. > /dev/null 2>&1 && echo "stock driver serves $M again"
  D=$T/inject-result; rm -rf $D; mkdir -p $D
  cp $T/inject.log $T/btwatch.log $T/btwatch-slog.log $D/ 2>/dev/null
  sloginfo | grep -e 'AVRCP:' -e NmeAVRCP -e 'MARKER HU_BT' -e nbt_a2i_play | grep -v -e 'received bytes' -e print_parameter \
    | tail -600 > $D/slog-bt.log
  tail -60 $R/sound.log > $D/sound.log 2>/dev/null
  ls -l $D
  ;;
*) echo "usage: sh car-bt-inject-test.sh start | stop"; exit 2 ;;
esac
