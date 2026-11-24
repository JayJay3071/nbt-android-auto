#!/bin/sh
# Control loop for Android Auto as an album in the music collection (ring version, 2026-11-14).
# nbt_aa_usb is mounted at /mnt/quota/mm/EntServer/AndroidAuto and offers 01.flac .. 40.flac, all with the phone's live
# sound. The HMI plays and lists them itself (its own track session); this script only
#   - writes each new song (title, artist, album, cover) into the NEXT entry of mme's database and then ends the entry
#     that is playing, so the stock player walks on to a fresh entry by itself (new entry = fresh title and cover),
#   - turns "the driver picked a later / earlier entry" into MEDIA_NEXT / MEDIA_PREVIOUS for the phone.
# Entries that were played keep their real titles (history); unused ones are "<" (before the start) and ">".
# Never "mmecli play <fid>" here (it shrinks the HMI's list to one title) and never a resync after the first one (new
# entry numbers would not be in the HMI's list).
# Runs while $T/ctl.run exists. Log: $T/ctl.log. State of the entries: $T/ent/<number> (the title written there).
R=/fs/ram3/nbt-aa-native
T=/fs/ram3/aatool
M="/opt/mm/bin/mmecli -c /dev/mme/multichannel"
MS=3
LAST=40
export LD_LIBRARY_PATH=/opt/mm/lib:/opt/sys/lib:/lib:/usr/lib
say() { echo "`date +%T` $*" >> $T/ctl.log; }
val() { sed -n "s/^$1=//p" $R/usb.state 2>/dev/null | sed 1q; }
med() { sed -n "s/^$1=//p" $R/media.state 2>/dev/null | sed 1q; }
num() { sed -n 's/^[0-9]*:.|\([0-9]*\)|.*/\1/p' | sed 1q; }
key() { echo "K $1 1" > $R/input.fifo; echo "K $1 0" > $R/input.fifo; say "key $1 to the phone"; }
name() { if test $1 -lt 10; then echo "0$1.flac"; else echo "$1.flac"; fi; }
fidof() { qdbc -d mme "select fid from library where msid=$MS and filename='`name $1`';" | num; }

# the phone's current song into entry $1: database row, cover, and our own note of what is there
settags() {
  f=`fidof $1`
  if test -z "$f"; then say "entry $1 is not in the library"; return 1; fi
  ti=`med title | sed "s/'/''/g"`; ar=`med artist | sed "s/'/''/g"`; al=`med album | sed "s/'/''/g"`
  qdbc -d mme "insert into library_artists(artist) select '$ar' where not exists (select 1 from library_artists where artist='$ar');" > /dev/null 2>&1
  qdbc -d mme "insert into library_albums(album) select '$al' where not exists (select 1 from library_albums where album='$al');" > /dev/null 2>&1
  qdbc -d mme "update library set title='$ti', artist_id=(select artist_id from library_artists where artist='$ar' limit 1), album_id=(select album_id from library_albums where album='$al' limit 1) where fid=$f;" > /dev/null 2>&1
  if test -s $R/media.cover; then
    cp $R/media.cover $T/cover.img
    last=`$M md_cs 2>&1 | sed -n 's/.*Created metadata session \([0-9]*\).*/\1/p'`
    if test -n "$last"; then
      n=$((last+1))
      { echo "md_cs"; for p in 0 1 2 3 4 5; do echo "md_li $n $f $p $T/cover.img"; done; echo "md_fs $n"; } > $T/md.script
      $M -u $T/md.script > $T/md.out 2>&1
    fi
  fi
  med title > $T/ent/$1
  say "entry $1 (fid $f): `med title` / `med artist`, cover sizes `grep -c 'URL=' $T/md.out 2>/dev/null`"
}

mkdir -p $T/ent
say "start"
if test -z "`fidof 6`"; then
  $M resync_mediastore $MS 7 > /dev/null 2>&1
  i=0; while test $i -lt 10 -a -z "`fidof 6`"; do sleep 1; i=$((i+1)); done
  # wait until mme has finished reading the tags, otherwise it overwrites what is written into its database below
  i=0; while test $i -lt 15; do sleep 1; i=$((i+1)); $M sync_get_status $MS 2>/dev/null | grep 'Current: 0; Pending: 0' > /dev/null && break; done
  sleep 2
  say "first read of the folder: entry 6 is fid `fidof 6`"
fi
cur=6        # the entry the stock player should be on
lastgen=0
guard=0      # rounds in which a different entry being read is not taken as a choice
expect=0     # rounds left in which a step to cur+1 is our own (after we ended the entry)
while test -e $T/ctl.run; do
  gen=`val gen`; st=`val aa_state`; rd=`val reading`; rb=`val reading_bytes`
  if test -z "$gen" -o -z "$st"; then sleep 1; continue; fi
  if test $guard -gt 0; then guard=$((guard-1)); fi
  if test $expect -gt 0; then expect=$((expect-1)); fi

  # the stock player is on another entry than we think
  if test -e $T/single.mode; then
    # which entry the HMI is on comes from mme itself (the stock player may be stopped and read nothing)
    info=`$M trksession_get_info 2>&1 | sed 1q`
    ct=`echo "$info" | sed -n 's/.*Current Track: \([0-9]*\), Total Tracks: 40\..*/\1/p'`
    if test -n "$ct" -a "${ct:-0}" -ge 1 -a "$ct" != "$cur"; then
      cur=$ct; lastgen=0; say "the HMI is on entry $cur"
    fi
  elif test "${rd:-0}" -gt 0 -a "${rd:-0}" != "$cur" -a "${rb:-0}" -gt 150000 -a $guard = 0; then
    if test "$rd" = $((cur+1)) -a $expect -gt 0; then
      say "player walked on to entry $rd"
    elif test "$rd" -gt "$cur"; then
      key 87
    else
      # "previous" restarts the song first when it has played for more than a few seconds
      pos=`med position`
      key 88
      if test "${pos:-0}" -gt 3 -a "`cat $T/ent/$rd 2>/dev/null`" != "`med title`"; then sleep 1; key 88; fi
    fi
    cur=$rd; expect=0; guard=2
  fi

  # the phone reports a new song
  if test "$st" = 2 -a "$gen" != 0 -a "$gen" != "$lastgen"; then
    lastgen=$gen
    if test -e $T/single.mode; then
      # One entry only (user idea 2026-11-14): the song is written into the entry that is on screen and the stock
      # player is left alone, so it does not start another entry and does not reach for the amplifier input again.
      # Skipping then goes through the radio's skip keys (mfl.map) or Android Auto itself.
      settags $cur
      say "refresh: `$M trksessionview_update 2>&1 | sed 1q`"
    elif test "`cat $T/ent/$cur 2>/dev/null`" = "`med title`"; then
      say "entry $cur already shows this song"
    elif test ! -s $T/ent/$cur && test "${rd:-0}" = 0; then
      # nothing is playing from our album yet: the song goes into the entry itself
      settags $cur
    elif test $cur -ge $LAST; then
      say "end of the ring: the song stays in entry $cur"
      settags $cur
    else
      if settags $((cur+1)); then
        echo $cur > $R/usb.eof
        expect=6
      fi
    fi
  fi
  sleep 1
done
say "end"
