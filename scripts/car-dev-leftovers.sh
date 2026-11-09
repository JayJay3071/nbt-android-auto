#!/bin/sh
# Clean-state test for the installer: list (default) or park the development leftovers on the NBT, so the head unit
# looks like a fresh user's NBT (only SSH access remains). Nothing is deleted: "park" MOVES the items into
# /var/opt/hmi/data/rw/_nbt-dev-parked (outside Plugins/, so the HMI no longer loads them); "restore" moves them back.
# Send with:  python tools\nbt_request.py "tr -d '\015' | sh -s list"   --input-file scripts\car-dev-leftovers.sh
#             python tools\nbt_request.py "tr -d '\015' | sh -s park"   --input-file scripts\car-dev-leftovers.sh
# Afterwards restart the NBT, then run the installer as a new user would.
RW=/var/opt/hmi/data/rw
PARK=$RW/_nbt-dev-parked
MODE=${1:-list}
# our own items: everything named Nbt*/Hello* in rw/ and rw/Plugins/
items=''
for d in $RW/Nbt* $RW/Hello* $RW/Plugins/*; do
  test -e "$d" && items="$items $d"
done
echo "== mode $MODE"
case $MODE in
  list)
    for d in $items; do echo "$d"; done
    echo "== parked:"; ls $PARK 2>/dev/null
    ;;
  audit)
    # every file changed in the last 21 days on the writable persistent areas (project start: 2026-11-05), apart from
    # the parked items: shows anything the project wrote that is not part of the installer. Stock iDrive data
    # (persistence, logs) also shows up and is expected.
    for top in /var/opt /etc /root /home; do
      test -d $top && find $top -type f -mtime -21 2>/dev/null | grep -v "^$PARK" | grep -v '^/var/opt/hmi/data/rw/NbtAndroidAutoRT/logs'
    done
    ;;
  park)
    mkdir -p $PARK $PARK/Plugins || exit 2
    for d in $items; do
      case $d in
        $RW/Plugins/*) mv "$d" $PARK/Plugins/ && echo "parked $d" ;;
        *)             mv "$d" $PARK/ && echo "parked $d" ;;
      esac
    done
    ;;
  restore)
    # an item the installer has meanwhile re-created (NbtAndroidAutoRT, Plugins/NbtAndroidAuto) stays parked
    for d in $PARK/Plugins/*; do
      test -e "$d" || continue
      if test -e $RW/Plugins/`basename $d`; then echo "kept parked (exists): $d"; else mv "$d" $RW/Plugins/ && echo "restored $d"; fi
    done
    for d in $PARK/*; do
      case $d in $PARK/Plugins) continue ;; esac
      test -e "$d" || continue
      if test -e $RW/`basename $d`; then echo "kept parked (exists): $d"; else mv "$d" $RW/ && echo "restored $d"; fi
    done
    ;;
  *) echo 'usage: list | audit | park | restore'; exit 1 ;;
esac
