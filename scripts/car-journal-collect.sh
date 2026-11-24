#!/bin/sh
# Read-only: prints the Android Auto journal of the last drives in one go (for a poor connection: one request).
#   $P/journal/<start time>/   one set per service run (kept: the newest 60), see nbt-aa-boot.sh
#   $P/logs/                   the running set
#   $P/sessions.log            why every session / decoder ended
# Send with: python .local\nbt_request.py "tr -d '\015' | sh" --input-file scripts\car-journal-collect.sh --timeout 110 --output-file <file>
# Optional first argument: number of newest runs to print (default 12).
P=/var/opt/hmi/data/rw/NbtAndroidAutoRT
N=${1:-12}
echo "##### `date`"; cat /fs/ram3/nbt-aa-state 2>/dev/null
echo '##### runs'; ls -lt $P/journal
echo '##### sessions.log'; tail -400 $P/sessions.log
for d in $P/logs `ls -t $P/journal | sed ${N}q | sed "s|^|$P/journal/|"`; do
  for f in states.txt nbt-aa-boot.log nbt-aa-plugin.log sound.log http.log nme-queue.txt wheel.txt capture-last.log decoder-last.log knob-tail.log slog-tail.txt; do
    if test -f $d/$f; then echo "##### $d/$f"; cat $d/$f; fi
  done
  # one file per session of that run (since 2026-11-12; capture-last.log is the older single file)
  for f in `ls $d/capture-[0-9]*.log 2>/dev/null`; do
    echo "##### $f"; cat $f
  done
done
echo '##### end'
