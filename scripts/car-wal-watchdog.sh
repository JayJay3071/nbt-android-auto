#!/bin/sh
# Local watchdog for the WAL overlay experiments (runs ON the car, independent of SSH).
# Every second it appends the thread states of WalServer and of our presenter to a small file on PERSISTENT storage,
# so the evidence survives a restart of the car. If the WalServer shows a thread in state MUTEX in two consecutive
# samples (the deadlock seen on 2026-11-06), it immediately sends SIGKILL to the presenter and SIGTERM to native_service.
# Usage (started in the background by the caller):  sh car-wal-watchdog.sh [seconds, default 120, max 600]
# Output: /var/opt/hmi/data/rw/NbtNativePrototype/wd.log (delete it after reading; it is small and bounded)
# The car has no head/printf/env/rmdir/expr; only sed, grep, sleep, pidin, kill, date and $((..)) are used.
WD=/var/opt/hmi/data/rw/NbtNativePrototype/wd.log
LIMIT=${1:-120}
test "$LIMIT" -gt 600 && LIMIT=600
srv=`pidin ar | grep WalServer | grep -v grep | sed 1q | sed 's/^ *\([0-9]*\).*/\1/'`
echo "=== watchdog start `date` limit=$LIMIT s WalServer pid=$srv" >> $WD
i=0
stuck=0
while test $i -lt $LIMIT; do
  st=`pidin -p $srv 2>/dev/null | sed 8q`
  echo "--- t=$i `date +%T`" >> $WD
  echo "$st" >> $WD
  pres=`pidin ar | grep nbt_wal_present | grep -v grep | sed 1q | sed 's/^ *\([0-9]*\).*/\1/'`
  if test -n "$pres"; then pidin -p $pres 2>/dev/null | sed 6q >> $WD; fi
  if echo "$st" | grep MUTEX > /dev/null; then stuck=$((stuck + 1)); else stuck=0; fi
  if test $stuck -ge 2; then
    echo "!!! WalServer thread in MUTEX twice in a row at `date +%T`: killing presenter and service" >> $WD
    test -n "$pres" && kill -9 $pres
    svc=`pidin ar | grep ' ./native_service' | grep -v grep | sed 1q | sed 's/^ *\([0-9]*\).*/\1/'`
    test -n "$svc" && kill $svc
    pidin -p $srv 2>/dev/null | sed 8q >> $WD
    stuck=0
  fi
  sleep 1
  i=$((i + 1))
done
echo "=== watchdog end `date`" >> $WD
