#!/bin/sh
# Frees a phone the stock USB stack grabbed at boot, so the Android Auto service can claim it.
# At boot the stock launcher starts aoap-switch and an MTP io-fs-media (-dpfs) for the phone
# before native_service runs. Once accessory_window is up, the stock launcher can no longer
# start them again, so killing the running copies leaves the phone to Android Auto.
# native_service may already have tried the phone (EBUSY) and ignores it until a replug, so
# after a kill the service is restarted once. restart.sh reruns boot.sh, which starts this
# script again: it then finds nothing to kill, and the done marker allows one restart per boot.
# Started from boot.sh (installed as claim-phone.sh in the package). Car 2026-11-11: with the phone left plugged in,
# iDrive took it at every boot and Android Auto never saw it; with this script it connects.
P=/var/opt/hmi/data/rw/NbtAndroidAutoRT
LOG=/fs/ram3/nbt-aa-claim.log
KILLED=/fs/ram3/nbt-aa-claim.killed
DONE=/fs/ram3/nbt-aa-claim.done
exec >> $LOG 2>&1
echo "=== claim-phone `date`"
n=0
while ! pidin ar | grep 'accessory_windo[w]' > /dev/null; do
  n=$((n+1))
  if test $n -gt 60; then echo 'no accessory_window after 60 s, giving up'; exit 0; fi
  sleep 1
done
sleep 2
rm -f $KILLED
pidin ar | grep '/opt/mm/bin/aoap-switc[h]' | while read p rest; do echo "kill aoap-switch $p"; kill -9 $p; touch $KILLED; done
pidin ar | grep 'io-fs-media -dpf[s]' | while read p rest; do echo "kill io-fs-media pfs $p"; kill -9 $p; touch $KILLED; done
if test -e $KILLED -a ! -e $DONE; then
  rm -f $KILLED
  touch $DONE
  sleep 2
  echo 'restarting the Android Auto service once'
  sh $P/restart.sh
  exit 0
fi
rm -f $KILLED
echo 'done'
