#!/bin/sh
# iPod source without re-seating (trial 2026-11-18).
#
# The head unit lists an iPod in iDrive only after its USB launcher has announced one, and that happens only when a
# real Apple device registers on the USB. With such a device left in the snap-in adapter this happens by itself at
# every start: the launcher starts the STOCK iPod driver and the media application detects the iPod.
# This script then lets our driver (nbt_aa_ipod --takeover 1) step in front of the stock driver at the same place -
# the place never disappears, so the head unit sees no removal - and ends the stock driver's process. From then on
# every question of the head unit about "the iPod" is answered with the Android Auto data.
#
# Only one process is ended here: the launcher's own driver for the USB iPod, recognised by its arguments
# ("io-fs-media -dipod,transport=usb..."). The Bluetooth iPod driver and the RAM disk driver have other arguments and
# are never touched.
# Usage: sh nbt-aa-ipod-takeover.sh [seconds to wait for the stock driver, default 120]
T=/fs/ram3/aatool
R=/fs/ram3/nbt-aa-native
W=${1:-120}
log() { echo "`date +%T` $*" >> $T/takeover.log; echo "$*"; }
i=0
L=
while test $i -lt $W; do
  if pidin ar | grep 'nbt_aa_ipod --mount' | grep -v grep > /dev/null; then log "our driver already runs, nothing to do"; exit 0; fi
  L=`pidin ar | grep 'io-fs-media -dipod,transport=usb' | grep -v grep | sed 1q`
  if test -n "$L"; then break; fi
  sleep 2; i=$((i+2))
done
if test -z "$L"; then log "no stock USB iPod driver within $W s (no Apple device in the adapter?)"; exit 1; fi
D=`echo "$L" | sed 's/^ *//' | cut -d' ' -f1`
M=`echo "$L" | sed -n 's/.*[,:]mount=\([^, ]*\).*/\1/p'`
case "$M" in /fs/ipod[0-9]) ;; *) log "unexpected mount '$M'"; exit 1 ;; esac
log "stock driver pid $D at $M"
# let the head unit finish its own detection with the stock driver first
i=0
while test ! -s $M/.FS_info./info.xml -a $i -lt 30; do sleep 1; i=$((i+1)); done
sleep 8
V=1; test -e $T/quiet && V=0
($T/nbt_aa_ipod --mount $M --run $R --takeover 1 --log $T/ipod.log --verbose $V > /dev/null 2>&1 &)
i=0
while test $i -lt 10; do
  if pidin ar | grep "nbt_aa_ipod --mount $M" | grep -v grep > /dev/null; then break; fi
  sleep 1; i=$((i+1))
done
sleep 1
if ! pidin ar | grep "nbt_aa_ipod --mount $M" | grep -v grep > /dev/null; then log "our driver did not start (see ipod.log)"; exit 1; fi
# still the same process as found above?
if pidin -p $D ar 2>/dev/null | grep 'io-fs-media -dipod,transport=usb' > /dev/null; then
  kill -9 $D
  log "our driver is in front, stock driver $D ended"
else
  log "stock driver $D is gone already"
fi
exit 0
