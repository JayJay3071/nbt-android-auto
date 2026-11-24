#!/bin/sh
# Read-only: collects what is needed to plan "Android Auto as a real media source" (title in the instrument cluster, skip
# from the steering wheel, cover) and the open Cinemo questions of 2026-11-12. Prints text only, changes nothing.
# NEVER add /dev/io-amp/routing|debug|diagnosis|processing here (reading them restarted the head unit).
# Send with: python .local\nbt_request.py "tr -d '\015' | sh" --input-file scripts\car-media-probe.sh --timeout 110 --output-file <file>
P=/var/opt/hmi/data/rw/NbtAndroidAutoRT
R=/fs/ram3/nbt-aa-native
echo "##### `date`"
echo '##### io-media-cinemo.cfg (options only; the global Cinemo settings also apply to our player)'
grep -v -e '^ *#' -e '^ *$' /opt/mm/etc/io-media-cinemo.cfg
echo '##### /opt/mm/etc'; ls -l /opt/mm/etc
echo '##### /opt/mm/bin'; ls -l /opt/mm/bin
echo '##### /dev/mme'; ls -l /dev/mme 2>&1
echo '##### mme / mcd / qdb processes'; pidin ar 2>/dev/null | grep -i -e mme -e mcd -e qdb -e io-media -e ipod -e iap -e NBTMedia
echo '##### mcd rules'; for f in /opt/mm/etc/mcd*.conf /etc/mcd.conf /opt/mm/etc/mcd.conf; do if test -f $f; then echo "--- $f"; grep -v -e '^ *#' -e '^ *$' $f; fi; done
echo '##### mme config files'; for f in /opt/mm/etc/mme*.cfg /opt/mm/etc/mme*.conf /opt/mm/etc/*.sql; do if test -f $f; then echo "--- $f"; ls -l $f; sed 120q $f; fi; done
echo '##### mmecli usage'; /opt/mm/bin/mmecli 2>&1 | sed 80q
echo '##### mmecli_ext usage'; /opt/mm/bin/mmecli_ext 2>&1 | sed 80q
echo '##### qdb databases'; ls -l /dev/qdb 2>&1; ls -l /pps 2>&1 | sed 40q
echo '##### pps media objects'; ls -lR /pps 2>/dev/null | grep -i -e media -e mme -e track -e ipod -e player | sed 60q
echo '##### mounted file systems'; df -k 2>/dev/null | sed 40q; ls -l /fs
echo '##### media state of the last session'; cat $R/media.state 2>&1; ls -l $R/media.cover 2>&1
echo '##### steering wheel keys seen'; grep -e '^.Wheel' $R/knob.log 2>/dev/null | tail -60
echo '##### http.log'; tail -60 $R/http.log 2>&1
echo '##### player queue'; sloginfo 2>/dev/null | grep 'NmeGraph video' | tail -5
echo '##### end'
