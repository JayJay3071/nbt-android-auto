#!/bin/sh
# Control loop for "Android Auto as a USB medium" (see src/android_auto/nbt_aa_usb.c). Runs on the head unit next to
# nbt_aa_usb and tells the stock media management (mme) what to do:
#   - the phone plays and the three files have new tags      -> read the files, new track session, play 02.flac
#   - the stock player reads 03.flac / 01.flac continuously  -> the driver chose "next" / "previous": send the media
#                                                                key to the phone (its new song then gives new tags)
# Stops when the file $T/ctl.run is removed. Log: $T/ctl.log.
# Start: touch $T/ctl.run; sh nbt-aa-usb-ctl.sh &
R=/fs/ram3/nbt-aa-native
T=/fs/ram3/aatool
M="/opt/mm/bin/mmecli -c /dev/mme/multichannel"
export LD_LIBRARY_PATH=/opt/mm/lib:/opt/sys/lib:/lib:/usr/lib
say() { echo "`date +%T` $*" >> $T/ctl.log; }
val() { sed -n "s/^$1=//p" $R/usb.state 2>/dev/null | sed 1q; }
num() { sed -n 's/^[0-9]*:.|\([0-9]*\)|.*/\1/p' | sed 1q; }
key() { echo "K $1 1" > $R/input.fifo; echo "K $1 0" > $R/input.fifo; say "key $1 to the phone"; }

# The phone's cover for the middle file. mme does not find the picture embedded in the FLAC file, but it takes an image
# file for a library entry (metadata_link_image) and converts it to its own sizes (profiles 0..5 in mme.conf, cached in
# /mnt/quota/mm/PVCache/<msid>). A metadata session only lives inside one mmecli call, so the commands go into a small
# script; its session number is the one after a session created just before.
cover() {
  test -s $R/media.cover || return 0
  cp $R/media.cover $T/cover.img
  $M metadata_image_cache_clear $1 > /dev/null 2>&1
  last=`$M md_cs 2>&1 | sed -n 's/.*Created metadata session \([0-9]*\).*/\1/p'`
  test -n "$last" || return 0
  n=$((last+1))
  { echo "md_cs"; for p in 0 1 2 3 4 5; do echo "md_li $n $2 $p $T/cover.img"; done; echo "md_fs $n"; } > $T/md.script
  $M -u $T/md.script > $T/md.out 2>&1
  say "cover: `grep -c 'URL=' $T/md.out` size(s) linked to fid $2"
}

sid=0
recenter() {
  # hdd.mode: the files live in a folder of the music collection (media store 3); the HMI keeps its own track session
  # and only the middle title is started again. Otherwise: the USB slot with a track session of our own.
  if test -e $T/hdd.mode; then ms=3; else ms=`qdbc -d mme "select msid from slots where slotid=1;" | num`; fi
  if test -z "$ms" -o "$ms" = 0; then say "no medium in the USB slot"; return 1; fi
  fid=''
  if test -e $T/hdd.mode; then fid=`qdbc -d mme "select fid from library where msid=$ms and filename='02.flac';" | num`; fi
  if test -z "$fid"; then
    $M resync_mediastore $ms 7 > /dev/null 2>&1
    i=0
    while test $i -lt 6; do
      sleep 1
      i=$((i+1))
      $M sync_get_status $ms 2>/dev/null | grep 'Current: 0; Pending: 0' > /dev/null && break
    done
    fid=`qdbc -d mme "select fid from library where msid=$ms and filename='02.flac';" | num`
  elif test -e $T/hdd.mode; then
    # Music collection: the library entry must keep its number (the HMI's own track session refers to it; a resync
    # after changed tags gave new numbers: "Could not find fid in tracksession"). So the new title, artist and album
    # go straight into mme's database.
    q() { sed -n "s/^$1=//p" $R/media.state | sed 1q | sed "s/'/''/g"; }
    ti=`q title`; ar=`q artist`; al=`q album`
    qdbc -d mme "insert into library_artists(artist) select '$ar' where not exists (select 1 from library_artists where artist='$ar');" > /dev/null 2>&1
    qdbc -d mme "insert into library_albums(album) select '$al' where not exists (select 1 from library_albums where album='$al');" > /dev/null 2>&1
    qdbc -d mme "update library set title='$ti', artist_id=(select artist_id from library_artists where artist='$ar' limit 1), album_id=(select album_id from library_albums where album='$al' limit 1) where fid=$fid;" >> $T/ctl.log 2>&1
  fi
  if test -z "$fid"; then say "02.flac is not in the library of medium $ms"; return 1; fi
  cover $ms $fid
  if test -e $T/hdd.mode; then
    # nothing to start: the HMI plays its own list ("play <fid>" would shrink that list to this one title)
    new=hmi
  else
    new=`$M newtrksession l "select fid from library where msid=$ms and ftype=1 order by filename" 2>&1 | sed -n 's/.*new trksessionid=\([0-9]*\).*/\1/p'`
    if test -z "$new"; then say "no track session"; return 1; fi
    $M settrksession $new > /dev/null 2>&1
    $M play $fid > /dev/null 2>&1
    if test "$sid" != 0; then $M rmtrksession $sid > /dev/null 2>&1; fi
    sid=$new
  fi
  say "medium $ms: session $new, playing fid $fid (`qdbc -d mme "select title from library where fid=$fid;" | sed -n 's/^[0-9]*:.|\(.*\)|$/\1/p' | sed 1q`)"
  return 0
}

say "start"
lastgen=0
laststate=0
arm1=1
arm3=1
pend=''
guard=0     # rounds in which the reads of 01/03 are ignored (mme reads them itself after a change)
waitkey=0   # rounds left to wait for the phone's new song after a key
while test -e $T/ctl.run; do
  gen=`val gen`; st=`val aa_state`
  b1=`val t1_bytes`; a1=`val t1_age`; a2=`val t2_age`; b3=`val t3_bytes`; a3=`val t3_age`
  if test -z "$gen" -o -z "$st"; then sleep 1; continue; fi
  again=0
  if test "$st" = 2 -a "$gen" != 0; then
    if test "$gen" != "$lastgen"; then again=1; fi
    # the phone started to play again while the stock player is not on our files
    if test "$laststate" != 2 -a "${a2:-0}" -gt 3000; then again=1; fi
    # a key was sent but the song did not change (e.g. "previous" only restarted it): back to the middle file
    if test $waitkey = 1; then again=1; fi
  fi
  if test $waitkey -gt 0; then waitkey=$((waitkey-1)); fi
  if test $again = 1; then
    if recenter; then
      lastgen=$gen; guard=3; waitkey=0
      # Music collection: only now, with the new title and cover in place, go back to the middle entry (the HMI reads
      # title and cover at that moment; going back before the phone had reported the new song showed the old one).
      # A song change without a choice: ask for a refresh of the list view.
      if test -e $T/hdd.mode; then
        if test -n "$pend"; then $M $pend > /dev/null 2>&1; say "back to the middle entry ($pend)"; pend=''
        else say "refresh: `$M trksessionview_update 2>&1 | sed 1q`"; fi
      fi
    fi
  elif test $guard -gt 0; then
    guard=$((guard-1))
  elif test "$st" = 2; then
    # One key per choice: a side file must have been quiet (armed) before it counts again, and right after the key
    # the player is sent back to the middle title, so the side file is not played on and the key is not repeated.
    if test "${b3:-0}" -lt 50000; then arm3=1; fi
    if test "${b1:-0}" -lt 50000; then arm1=1; fi
    if test "$arm3" = 1 -a "${b3:-0}" -gt 150000 -a "${a3:-9999}" -lt 1200 -a "${a2:-0}" -gt 800; then
      key 87; arm3=0; guard=2; waitkey=5
      if test -e $T/hdd.mode; then pend=prev; else test -n "$fid" && $M play $fid > /dev/null 2>&1; fi
    elif test "$arm1" = 1 -a "${b1:-0}" -gt 150000 -a "${a1:-9999}" -lt 1200 -a "${a2:-0}" -gt 800; then
      key 88; arm1=0; guard=2; waitkey=5
      if test -e $T/hdd.mode; then pend=next; else test -n "$fid" && $M play $fid > /dev/null 2>&1; fi
    fi
  fi
  laststate=$st
  sleep 1
done
test -e $T/hdd.mode || $M stop > /dev/null 2>&1
if test "$sid" != 0; then $M rmtrksession $sid > /dev/null 2>&1; fi
say "end"
