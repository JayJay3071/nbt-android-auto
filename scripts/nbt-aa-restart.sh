#!/bin/sh
# Restart of the Android Auto service only (not the head unit). Started by the HMI plugin from the Android Auto page:
# "Android Auto neu starten" (no argument) or a picture mode change ($1 = new mode flag: split800 | anamorph).
# First a diagnostic snapshot (car 2026-11-09: rarely "connected" but the picture does not come; sometimes the media tab
# has no sound after switching between media and radio) into $P/snapshots, the newest 5 are kept.
P=/var/opt/hmi/data/rw/NbtAndroidAutoRT
R=/fs/ram3/nbt-aa-native
cd $P || exit 1
D=$P/snapshots
mkdir -p $D
S=$D/restart-`date +%d%H%M%S`.txt
{
  echo "=== `date` restart ${1:-service}"
  echo '--- aa state'; cat /fs/ram3/nbt-aa-state
  ls -l /fs/ram3/nbt-aa-hidden /fs/ram3/nbt-aa-request /fs/ram3/nbt-aa-leave
  echo '--- plugin log'; tail -80 /fs/ram3/nbt-aa-plugin.log
  echo '--- boot log'; tail -40 /fs/ram3/nbt-aa-boot.log
  echo '--- states'; tail -30 $P/logs/states.txt
  # the session logs of this boot: boot.sh rotates logs/ right after this snapshot (car 2026-11-10: the reason for a
  # drop-out before the restart was lost that way)
  for f in `ls -t $R/capture-*.log 2>/dev/null | sed 3q`; do echo "--- $f"; tail -60 $f; done
  for f in `ls -t $R/decoder-*.log 2>/dev/null | sed 2q` `ls -t $R/hwvideo-*.log 2>/dev/null | sed 1q` $R/switch.log; do echo "--- $f"; tail -20 $f; done
  echo '--- processes'; pidin arg | grep -i -e nme -e media -e audio -e amp -e nbt -e oneplus -e native -e decode -e hwvideo -e mm
  echo '--- system log'; sloginfo | tail -400
} > $S 2>&1
n=0
for f in `ls -t $D/restart-*.txt`; do n=$((n+1)); if test $n -gt 5; then rm -f $f; fi; done
if test -n "$1"; then rm -f anamorph center800 split800 classic; touch $1; fi
slay -f -Q -s TERM native_service
i=0
while test -e $R/service.lock -a $i -lt 20; do sleep 1; i=$((i+1)); done
slay -f -Q -s KILL native_service
rm -f $R/service.lock
sh $P/boot.sh
