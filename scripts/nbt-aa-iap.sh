#!/bin/sh
# Android Auto as "iPod over Bluetooth" in iDrive (car-proven 2026-11-24: entry "Android Auto" with sound, title,
# artist, cover and length; only the OnePlus, connected by Bluetooth + USB, no Apple device).
# Started by boot.sh in the background after native_service when the mode is on ($P/nbt_aa_wms, $P/nbt_ipod_tap,
# $P/nbt_aa_ipod present, no $P/iap.off). Runs as long as the service (service.lock).
#   - The sound: boot.sh gives native_service our A2DP sound device (nbt_aa_wms, /dev/wms/nbtaa: SBC from the
#     session's audio.fifo) as its media sound player instead of nbt_a2i_play.
#   - nbt_ipod_tap in front of the Bluetooth audio driver /fs/avrcp0: the phone says "iPod over Bluetooth capable"
#     (iap supported), the sound URL points at /dev/wms/nbtaa, play state / title events follow Android Auto.
#   - nbt_aa_ipod at /fs/btipod (the Bluetooth iPod place) while /fs/avrcp0 exists: title, artist, cover, length,
#     skip keys from Android Auto.
# Logs in $R (small); $P/iap.verbose = full request logs (grow fast, only for diagnosis).
# The head unit reads "iap supported" only when the phone CONNECTS by Bluetooth, and nbt_ipod_tap says it only while a
# USB session runs (USB first, then Bluetooth; else Bluetooth off / on on the phone once). A phone connected by
# Bluetooth alone stays plain Bluetooth audio (2026-11-26).
P=/var/opt/hmi/data/rw/NbtAndroidAutoRT
R=/fs/ram3/nbt-aa-native
L=$R/iap.log
pids() { pidin ar | grep "$1" | grep -v grep | sed 's/^ *//' | cut -d' ' -f1; }
end_all() { for p in `pids "$1"`; do kill -9 $p 2>/dev/null; done; }
note() { echo "`date +%T` $*" >> $L; }
: > $L
end_all "nbt_ipod_tap --mount /fs/avrcp0"
end_all "nbt_aa_ipod --mount /fs/btipod"
for f in nbt_ipod_tap nbt_aa_ipod; do
  cp $P/$f $R/$f && chmod 700 $R/$f || { note "copy of $f failed"; exit 1; }
done
V=0; TAPLOG=/dev/null
if test -e $P/iap.verbose; then V=1; TAPLOG=$R/iap-tap.log; fi
(cd $R; ./nbt_ipod_tap --mount /fs/avrcp0 --iap 1 --url wms:/net/hu-intel.nbt//dev/wms/nbtaa --run $R --seconds 2000000000 --log $TAPLOG </dev/null >/dev/null 2>&1 &)
note "Bluetooth tap started"
sleep 1
OFF=20
while test -e $R/service.lock; do
  if test -z "`pids "nbt_ipod_tap --mount /fs/avrcp0"`"; then
    note "tap ended"
    break
  fi
  # iap.claimed (written by nbt_ipod_tap): this Bluetooth connection came while a USB session ran and was made our
  # iPod. Without it the phone is plain Bluetooth audio and the stock drivers are left alone.
  # and a phone session (status.txt of native_service); the place stays 20 s after a session so a short drop keeps it
  if grep -e state=running -e state=starting_decoder -e state=connecting $R/status.txt > /dev/null 2>&1; then OFF=0; else OFF=$(($OFF + 1)); fi
  if ls /proc/mount/fs/avrcp0 > /dev/null 2>&1 && test -e $R/iap.claimed && test $OFF -lt 20; then
    if test -z "`pids "nbt_aa_ipod --mount /fs/btipod"`"; then
      sleep 3
      note "Bluetooth audio there: iPod place started"
      if test $V = 1; then
        (cd $R; ./nbt_aa_ipod --mount /fs/btipod --run $R --remount 2000 --verbose 1 --log $R/btipod.log </dev/null >/dev/null 2>&1 &)
      else
        (cd $R; ./nbt_aa_ipod --mount /fs/btipod --run $R --remount 2000 --log $R/btipod.log </dev/null >/dev/null 2>&1 &)
      fi
    fi
  elif test -n "`pids "nbt_aa_ipod --mount /fs/btipod"`"; then
    note "Bluetooth audio gone or not ours: iPod place ended"
    end_all "nbt_aa_ipod --mount /fs/btipod"
  fi
  sleep 1
done
end_all "nbt_aa_ipod --mount /fs/btipod"
end_all "nbt_ipod_tap --mount /fs/avrcp0"
note "ended"
