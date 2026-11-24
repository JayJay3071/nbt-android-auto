#!/bin/sh
# Started by boot.sh in the background (2026-11-23): whenever the phone's Bluetooth audio entry exists (the stock
# Bluetooth audio driver is attached at /fs/avrcpN), nbt_bt_inject is put in front of it, so that the entry shows the
# Android Auto title / artist / length / position / cover and reports "playing" (see src/android_auto/nbt_bt_inject.c).
# nbt_bt_inject ends by itself when the phone disconnects; this loop starts it again on the next connection.
# Ends together with the Android Auto service (service.lock gone). Off: touch $P/btinject.off and restart the service.
P=/var/opt/hmi/data/rw/NbtAndroidAutoRT
R=/fs/ram3/nbt-aa-native
L=$R/btinject.log
echo "`date +%T` loop started" >> $L
sleep 5
while test -e $R/service.lock -a ! -e $P/btinject.off; do
  for m in `ls /proc/mount/fs 2>/dev/null | grep '^avrcp[0-9]$'`; do
    if ! pidin ar | grep "nbt_bt_inject --mount /fs/$m " | grep -v grep > /dev/null; then
      V=0; test -e $P/btinject.verbose && V=1
      echo "`date +%T` /fs/$m appeared, starting nbt_bt_inject" >> $L
      ($R/nbt_bt_inject --mount /fs/$m --run $R --log $L --verbose $V > /dev/null 2>&1 &)
    fi
  done
  sleep 3
done
echo "`date +%T` loop ended" >> $L
