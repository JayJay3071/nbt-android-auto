#!/bin/sh
# Read-only collection for the reliability review (2026-11-09): plugin logs (hold events and whether iDrive honoured
# them), restart snapshots, capture/decoder statistics, cluster log.
# Send with: python tools\nbt_request.py "tr -d '\015' | sh" --input-file scripts\car-review-collect.sh --timeout 90 --output-file <file>
P=/var/opt/hmi/data/rw/NbtAndroidAutoRT
R=/fs/ram3/nbt-aa-native
echo "=== `date` uptime"; uptime 2>/dev/null
echo '=== live state'; cat /fs/ram3/nbt-aa-state 2>/dev/null
ls -l $P $P/logs $P/logs.prev $P/snapshots 2>&1
for f in /fs/ram3/nbt-aa-plugin.log $P/logs/nbt-aa-plugin.log $P/logs.prev/nbt-aa-plugin.log; do
  echo "=== PLUGIN $f"; cat $f 2>/dev/null
done
for d in $P/logs $P/logs.prev; do
  echo "=== STATES $d"; cat $d/states.txt 2>/dev/null
  echo "=== BOOT $d"; cat $d/nbt-aa-boot.log 2>/dev/null
  echo "=== CAPTURE tail $d"; tail -40 $d/capture-last.log 2>/dev/null
  echo "=== DECODER $d"; tail -30 $d/decoder-last.log 2>/dev/null
  echo "=== HWVIDEO tail $d"; tail -25 $d/hwvideo-last.log 2>/dev/null
  echo "=== CLUSTER $d"; cat $d/cluster-tail.log 2>/dev/null
  echo "=== SLOG $d"; cat $d/slog-tail.txt 2>/dev/null
done
echo '=== live capture / decoder logs'
for f in `ls -t $R/capture-*.log 2>/dev/null | sed 2q` `ls -t $R/decoder-*.log 2>/dev/null | sed 2q`; do echo "--- $f"; tail -25 $f; done
echo '=== live cluster log'; tail -60 /fs/ram3/nbt-cluster.log 2>/dev/null
for f in `ls -t $P/snapshots/restart-*.txt 2>/dev/null`; do echo "=== SNAPSHOT $f"; cat $f; done
echo '=== processes'; pidin arg 2>/dev/null | grep -e native_service -e oneplus -e decode_runner -e NmeCmdLine -e nbt_ -e fifo_http -e accessory
echo '=== end'
