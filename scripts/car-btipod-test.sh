#!/bin/sh
# Car test "iPhone over Bluetooth" (2026-11-23): learn the step after message 0x80 from a real iPhone, then try our
# own driver at the Bluetooth iPod place. Everything runs from /fs/ram3/aatool (gone after an NBT restart).
#   sh car-btipod-test.sh tap        iPhone Bluetooth OFF first; starts the recorder, then switch the iPhone's Bluetooth on
#   sh car-btipod-test.sh status     short look while it runs
#   sh car-btipod-test.sh tapstop    ends the recorder, packs everything into /fs/ram3/aatool/btipod-result
#   sh car-btipod-test.sh fake       iPhone Bluetooth OFF, OnePlus by Bluetooth + USB with AA: our driver at /fs/btipod,
#                                    answers 0x80 by leaving the place and coming back after 3 s
#   sh car-btipod-test.sh fakestop   ends our driver, packs its log
# Files expected in /fs/ram3/aatool: nbt_ipod_tap, nbt_aa_ipod.bt
T=/fs/ram3/aatool
R=/fs/ram3/nbt-aa-native
M=/fs/btipod
D=$T/btipod-result
markers() {
  sloginfo | grep -e 'MARKER HU_' -e btipod -e IAPBT -e 'BT-IPOD' -e '\[DD\]' -e 'iPod' -e 'ipod' -e ReIdentify \
    | grep -v -e WindowVisible -e 'received bytes' | tail -$1 | cut -c8-220
}
case "$1" in
tap)
  if ls /proc/mount$M > /dev/null 2>&1; then
    echo "$M is mounted already (iPhone connected by Bluetooth?): switch the iPhone's Bluetooth OFF, wait 10 s, run again"
    ls /proc/mount$M
    exit 1
  fi
  pidin ar | grep nbt_ipod_tap | grep -v grep && { echo "recorder already running"; exit 1; }
  : > $T/tap.log
  ($T/nbt_ipod_tap --mount $M --log $T/tap.log --seconds 900 </dev/null >/dev/null 2>&1 &)
  sleep 1
  cat $T/tap.log
  echo "RECORDER ON (15 min). Now: iPhone Bluetooth ON, wait for the iPhone in iDrive, select it, play, skip once."
  ;;
status)
  echo "--- mount"; ls /proc/mount$M 2>&1
  echo "--- recorder"; pidin ar | grep -e nbt_ipod_tap -e 'nbt_aa_ipod --mount /fs/btipod' | grep -v grep | cut -c1-100
  echo "--- tap log: `grep -c . $T/tap.log 2>/dev/null` lines"; grep -e attached -e 'self test' -e mount -e ' sub ' -e left $T/tap.log 2>/dev/null | tail -12 | cut -c1-200
  echo "--- stores"; qdbc -d mme 'select msid,slotid,available,mssname,name,mountpath from mediastores where slotid in (15,18,21);'
  echo "--- markers"; markers 10
  ;;
tapstop)
  slay -f -Q nbt_ipod_tap
  sleep 2
  rm -rf $D; mkdir -p $D
  cp $T/tap.log $D/
  {
    echo "=== processes"; pidin ar | grep -e io-fs-media -e IAP -e ipod -e iap -e io-media -e mme | grep -v grep
    echo "=== mount"; ls /proc/mount$M 2>&1; ls -la $M $M/.FS_info. 2>&1
    echo "=== info.xml"; cat $M/.FS_info./info.xml 2>&1
    echo "=== stores"; qdbc -d mme 'select msid,slotid,available,active,mssname,name,identifier,mountpath,capabilities from mediastores where available=1;'
    echo "=== nowplaying"; qdbc -d mme 'select * from nowplaying where ccid in (1,4);'
  } > $D/state.txt 2>&1
  markers 400 > $D/markers.txt
  sloginfo 2>&1 | tail -20000 > $D/slog-full.txt
  ls -l $D
  ;;
fake)
  if ls /proc/mount$M > /dev/null 2>&1; then
    echo "$M is in use (iPhone still connected by Bluetooth?): switch the iPhone's Bluetooth OFF first"; exit 1
  fi
  ls /fs/avrcp0/.FS_info. > /dev/null 2>&1 || echo "note: no /fs/avrcp0 - OnePlus not connected by Bluetooth (the head unit needs it)"
  slay -f -Q nbt_ipod_tap 2>/dev/null
  : > $T/btfake.log
  (cd $R; $T/nbt_aa_ipod.bt --mount $M --run $R --remount 2000 --verbose 1 --log $T/btfake.log </dev/null >/dev/null 2>&1 &)
  sleep 2
  cat $T/btfake.log | cut -c1-160
  echo "OUR DRIVER ON at $M. Now: look for 'Android Auto' in iDrive (Multimedia / external devices)."
  ;;
fakestop)
  slay -f -Q nbt_aa_ipod.bt
  sleep 1
  mkdir -p $D
  grep -v 00000318 $T/btfake.log > $D/btfake.log
  markers 200 > $D/fake-markers.txt
  qdbc -d mme 'select msid,slotid,available,mssname,name,mountpath from mediastores where slotid in (15,18,21);' > $D/fake-stores.txt 2>&1
  ls -l $D
  ;;
*) echo "usage: sh car-btipod-test.sh tap | status | tapstop | fake | fakestop"; exit 2 ;;
esac
