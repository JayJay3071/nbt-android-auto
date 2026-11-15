#!/bin/sh
# Parked test (2026-11-15): does the HU_NBT's ConnectionMaster grant browser connection 0xb8, and which A2I input is
# then audible? Run by hand over SSH, car parked, volume moderate:
#   sh /var/opt/hmi/data/rw/NbtAndroidAutoRT/car-route-beep-test.sh
# Needs nbt_a2i_beep (build experiments/audio/nbt_a2i_beep.c for QNX) and audioconn (nbt_audioconn, part of the
# package) in the same folder.
# Steps: connection table -> build 0xb8 -> beeps into input_stereo_1..5 (N beeps = input N) -> remove 0xb8 -> table.
# Output: /var/opt/hmi/data/rw/NbtAndroidAutoRT/logs/route-beep-test.txt (also shown on screen).
# If the radio stays muted afterwards: /var/opt/hmi/data/rw/NbtAndroidAutoRT/audioconn -r 0xb8
P=/var/opt/hmi/data/rw/NbtAndroidAutoRT
T=$P
OUT=$P/logs/route-beep-test.txt
mkdir -p $P/logs
export LD_LIBRARY_PATH=/opt/sys/lib:/lib:/usr/lib
{
  echo "=== route beep test `date`"
  echo '--- connection table before'
  $P/audioconn -l; echo "rc=$?"
  echo '--- build 0xb8'
  $P/audioconn -b 0xb8; B=$?; echo "rc=$B"
  echo '--- connection table after build'
  $P/audioconn -l; echo "rc=$?"
  sleep 1
  for n in 3 1 2 4 5; do
    echo "--- $n beep(s) -> input_stereo_$n"
    $T/nbt_a2i_beep input_stereo_$n $n || $T/nbt_a2i_beep /input_stereo_$n $n
    sleep 2
  done
  echo '--- remove 0xb8'
  $P/audioconn -r 0xb8; echo "rc=$?"
  echo '--- connection table at the end'
  $P/audioconn -l; echo "rc=$?"
  echo '--- system log (audio)'
  sloginfo | grep -i -e a2i -e io-amp -e ioamp -e amp -e audio | tail -40
  echo "=== done (build rc=$B: 0 granted, 1 refused, 3 no answer, 4 no ConnectionMaster)"
} > $OUT 2>&1
cat $OUT
