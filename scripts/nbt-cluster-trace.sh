#!/bin/sh
# READ-ONLY observer for the instrument-cluster investigation (2026-11-08): how does the junction view ("Kreuzungszoom")
# reach the 6WB cluster? Records, every 10 s, the NEW system-log lines about compositor windows (WalServer: window
# created/moved/visible/z-order, desktop geometry), the navigation guidance viewer and anything cluster/APIX related,
# plus the process list of the navigation. Writes only $P/logs/cluster-trace.txt (capped at 2 MB). Nothing else.
# Started by boot.sh when $P/trace-cluster exists (remove that file to stop it at the next start), or by hand:
#   sh /var/opt/hmi/data/rw/NbtAndroidAutoRT/cluster-trace.sh [seconds, default 3600]
P=/var/opt/hmi/data/rw/NbtAndroidAutoRT
OUT=$P/logs/cluster-trace.txt
T=/fs/ram3/nbt-cluster-trace
SECONDS_MAX=${1:-3600}
mkdir -p $T $P/logs
if test -e $T/running; then exit 0; fi     # one instance per boot (boot.sh may run again after a picture-mode change)
touch $T/running
filter() {
  grep -i -e 'NEW WINDOW' -e 'Window .* created' -e 'destroy' -e 'zorder' -e 'visible' -e 'Desktop' -e 'display width' \
          -e 'GuidanceViewer' -e 'GuidingView' -e 'junction' -e 'kombi' -e 'cluster' -e 'apix' -e 'KD_' -e 'viewport' \
          -e 'moveWindow' -e 'setWindow' -e 'MapViewer' -e 'Splitscreen'
}
echo "=== cluster trace start `date`, $SECONDS_MAX s" >> $OUT
pidin arg 2>/dev/null | grep -i -e Navigation -e WalServer -e apix -e GuidanceViewer | grep -v grep >> $OUT
sloginfo 2>/dev/null | filter > $T/old
cat $T/old >> $OUT
i=0
while test $i -lt $SECONDS_MAX; do
  sleep 10
  i=$((i+10))
  size=`ls -l $OUT | sed 's/  */ /g' | sed 's/^[^ ]* [^ ]* [^ ]* [^ ]* \([0-9]*\).*/\1/'`
  if test "$size" -gt 2000000; then echo "=== size cap reached" >> $OUT; break; fi
  sloginfo 2>/dev/null | filter > $T/new
  if ! cmp -s $T/new $T/old; then
    echo "--- t=$i `date +%T`" >> $OUT
    grep -v -x -F -f $T/old $T/new >> $OUT
    mv $T/new $T/old
  fi
done
echo "=== cluster trace end `date`" >> $OUT
rm -f $T/running
