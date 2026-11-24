#!/bin/sh
# Records, once a second for a few minutes, what decides whether the Android Auto sound is audible on the Bluetooth
# entry (car 2026-11-23: sound stops after about 20 s when the Bluetooth tab is left, comes back when Bluetooth is
# selected again). Only reads. Runs in the background and writes into RAM, so it keeps going when SSH drops.
#   sh car-bt-sound-watch.sh [seconds, default 240]      -> /fs/ram3/aatool/btwatch.log
T=/fs/ram3/aatool
R=/fs/ram3/nbt-aa-native
S=${1:-240}
mkdir -p $T
L=$T/btwatch.log
: > $L
i=0
while test $i -lt $S; do
  {
    echo "=== `date +%T`"
    tail -1 $R/sound.log 2>/dev/null
    grep -e '^state=' -e '^title=' $R/media.state 2>/dev/null | tr '\n' ' '; echo
    for p in `pidin ar | grep -e io-media -e io-nme -e a2i | grep -v grep | sed 's/^ *//' | cut -d' ' -f1`; do
      pidin -p $p fds 2>/dev/null | grep io-amp | sed "s/^/  holder $p /" | cut -c1-80
    done
  } >> $L 2>&1
  sleep 1; i=$((i+1))
done
echo "=== end `date +%T`" >> $L
# the head unit's own view of the same minutes
sloginfo | grep -e 'MARKER HU_BT' -e 'MARKER HU_' -e 'DCMD_MEDIA_PLAYBACK_STATUS playstate' -e 'DCMD_MEDIA_PAUSE' \
  -e 'DCMD_MEDIA_PLAY' -e 'DCMD_MEDIA_RESUME' -e 'nbt_a2i_play' -e NmeAVRCP | grep -v -e print_parameter -e 'received bytes' \
  | tail -400 > $T/btwatch-slog.log 2>&1
