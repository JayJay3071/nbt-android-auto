#!/bin/sh
# Copies the program and configuration areas of the head unit to a USB stick, for reading them on the PC without the
# car (2026-11-14: looking up how the stock media software handles an iPod). Only reads on the head unit.
#
# NOT copied: navigation maps, music collection, Gracenote data, anything under /mnt, /fs/sda*, /var - those are big
# and hold personal data (contacts, destinations, paired phones). What is copied still contains vehicle-specific
# files; the copy stays on the user's PC and never goes into the public repository.
#
# Usage on the car (stick plugged in, FAT or exFAT, about 2 GB free):
#   sh car-system-dump.sh sizes            only show how big the areas are and where the stick is
#   sh car-system-dump.sh copy /fs/usb0    copy to <stick>/nbt-dump
# The car has no tar on every software level: tar is used when there is one, otherwise plain cp -R (links followed).
AREAS="/proc/boot /etc /bin /sbin /lib /usr /opt"
case "$1" in
sizes)
  echo "--- stick candidates"; ls -d /fs/usb* 2>/dev/null; df -k 2>/dev/null | grep -e usb -e umass
  echo "--- sizes in KB"
  for a in $AREAS; do du -sk $a 2>/dev/null; done
  echo "--- tools"; ls /bin/tar /usr/bin/tar /bin/pax /usr/bin/pax 2>/dev/null
  ;;
copy)
  T=$2/nbt-dump
  test -d "$2" || { echo "no such folder: $2"; exit 1; }
  mkdir -p $T || exit 1
  date > $T/dump.log
  for a in $AREAS; do
    n=`echo $a | sed -e 's|^/||' -e 's|/|_|g'`
    echo "copy $a" >> $T/dump.log
    if test -x /bin/tar -o -x /usr/bin/tar; then
      tar -cf $T/$n.tar $a >> $T/dump.log 2>&1
    else
      mkdir -p $T/$n && cp -R -L $a/. $T/$n/ >> $T/dump.log 2>&1
    fi
    echo "done $a rc=$?" >> $T/dump.log
  done
  date >> $T/dump.log
  echo DUMP_DONE; tail -12 $T/dump.log
  ;;
*)
  echo "usage: sh car-system-dump.sh sizes | copy <stick folder>"; exit 2 ;;
esac
