#!/bin/sh
# RAM-only test of the 1:1 picture (800x480 in the middle, black bars): swaps the RUNNING copies in /fs/ram3/nbt-aa-native
# for the new native_service / decode_runner / nbt_hwvideo (uploaded to /fs/ram3/nbt-center-test) and restarts the
# service with center800. The persistent package is not touched: an NBT restart brings the old state back.
# Usage: sh -s [on|ana|off] [dpi]  on (default) = center800, ana = anamorphic full screen, off = old 800x300 picture
# Send with: python tools\nbt_request.py "tr -d '\015' | sh -s on 140" --input-file scripts\car-center-ram-test.sh --timeout 60
R=/fs/ram3/nbt-aa-native
T=/fs/ram3/nbt-center-test
MODE=${1:-on}
DPI=${2:-140}
for f in native_service decode_runner nbt_hwvideo; do
  if test ! -s $T/$f; then echo "MISSING $T/$f - nothing changed"; exit 2; fi
done
test -d $R || { echo "no runtime directory $R (service never started?)"; exit 3; }
echo '== stopping the service (clean stop: video connection released)'
slay -f -Q -s TERM native_service
i=0
while test -e $R/service.lock -a $i -lt 20; do sleep 1; i=$((i+1)); done
if test -e $R/service.lock; then echo 'service did not stop within 20 s - nothing changed'; exit 4; fi
for f in native_service decode_runner nbt_hwvideo; do
  cp $T/$f $R/$f.tmp && chmod 700 $R/$f.tmp && mv $R/$f.tmp $R/$f && cmp -s $T/$f $R/$f && echo "ok $f" || { echo "COPY FAILED $f"; exit 5; }
done
rm -f $R/center800 $R/anamorph $R/split800 $R/res720
if test "$MODE" = on; then touch $R/center800; fi
if test "$MODE" = ana; then touch $R/anamorph; fi
if test "$MODE" = split; then touch $R/split800; fi
# optional new phone-side binary (anamorph needs the pixel aspect ratio in the video configuration)
if test -s $T/oneplus_native; then
  cp $T/oneplus_native $R/oneplus_native.tmp && chmod 700 $R/oneplus_native.tmp && mv $R/oneplus_native.tmp $R/oneplus_native && cmp -s $T/oneplus_native $R/oneplus_native && echo "ok oneplus_native"
fi
cd $R
NBT_VID_DPI=$DPI NBT_VID_INSETS=0,180,0,0 LD_LIBRARY_PATH=/opt/mm/lib:/opt/mm/lib/cinemo:/lib:/usr/lib ./native_service </dev/null >/dev/null 2>&1 &
echo "== native_service started pid=$! mode=$MODE dpi=$DPI"
sleep 3
cat /fs/ram3/nbt-aa-state
