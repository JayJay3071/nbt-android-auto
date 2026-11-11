#!/bin/sh
# Read-only probe for Android Auto audio routing on the original NBT (2026-11-11, docs/AA-AUDIO-PLAN.md).
# Question: how can a program outside the HMI make the car play the browser audio source (ConnectionMaster connection
# 0xb8 on NBT Evo, input_stereo_3)? NBT-AAD does it with the stock `audioconn` tool, which this NBT seems to lack.
# This script only reads: it lists files, greps binaries for interface names and prints the system log. It starts no
# audio, sends nothing on MOST/DSI and writes no file (output goes to stdout only). Heavy greps run under nice.
# Send with: python tools\nbt_request.py "tr -d '\015' | sh" --input-file scripts\car-audio-route-probe.sh --timeout 600 --output-file <file>
N='nice -n 10'
DIRS='/opt /bin /sbin /usr/bin /usr/sbin /fs/sda0/opt /var/opt/hmi/data/rw'
KEYS='BuildHighSyncConnection|RemoveHighSyncConnection|requestFocus|releaseFocus|EAudioConnectionIDs|eSRC_BROWSER|BrowserFront|DMMAudioSource|getConnIDForAudioSource|CPxyConnectionMaster|AudioManagement|preAllocate|audioconn'

echo "=== `date` uptime"; uptime 2>/dev/null; uname -a

echo '=== 1. audioconn (any name or place)'
for d in $DIRS; do
  test -d $d && $N find $d -name '*audioconn*' 2>/dev/null
done
echo '--- candidates NBT-AAD looks for'
ls -l /fs/sda0/opt/mm/bin/audioconn /opt/mm/bin/audioconn 2>&1
echo '--- other audio test tools'
for d in /opt/mm/bin /opt/sys/bin /opt/conn/bin /usr/bin; do
  test -d $d && ls $d 2>/dev/null | grep -i -e audio -e conn -e amp -e a2i -e focus | sed "s|^|$d/|"
done

echo '=== 2. browser audio front end (where NBT-AAD plays)'
ls -l /opt/mm/lib/libasound /opt/mm/lib/libasound/* 2>&1
ls -l /opt/sys/bin/dev-libasound /dev/io-amp /dev/snd 2>&1
echo '--- running audio processes'
pidin arg 2>/dev/null | grep -e io-audio -e io-amp -e dev-libasound -e Online -e Connectivity -e AudioMan -e audiomgr \
  -e ConnectionMaster | grep -v grep
echo '--- configuration lines naming input_stereo / libasound'
for f in `$N find /opt /etc -name '*.cfg' 2>/dev/null`; do
  grep -n -e input_stereo -e dev-libasound -e BROWSER_AUDIO $f 2>/dev/null | sed "s|^|$f:|"
done

echo '=== 3. which binaries carry the routing interfaces'
HMI=`$N find /opt -name 'HmiMain3D*' 2>/dev/null | sed 1q`
echo "HmiMain3D: $HMI"; test -n "$HMI" && ls -l $HMI
for d in /opt/hmi /opt/mm/lib /opt/mm/bin /opt/sys/lib /opt/sys/bin /opt/conn/bin /opt/conn/lib; do
  test -d $d || continue
  for f in `$N grep -l -E "$KEYS" $d/* 2>/dev/null`; do ls -l $f; done
done

echo '=== 4. interface strings in HmiMain3D (names, ids, enum tables)'
if test -n "$HMI"; then
  if type strings >/dev/null 2>&1; then
    $N strings $HMI | grep -E "$KEYS|ConnID|ConnectionID|eSRC_|eSNK_" | sort -u | sed 200q
  else
    echo 'no strings tool: copy the file to the laptop for analysis (scp root@<nbt>:'$HMI' .)'
  fi
fi

echo '=== 5. same strings in the audio / connectivity libraries'
if type strings >/dev/null 2>&1; then
  for d in /opt/mm/lib /opt/sys/lib /opt/conn/lib /opt/conn/bin; do
    test -d $d || continue
    for f in `$N grep -l -E "$KEYS" $d/* 2>/dev/null`; do
      echo "--- $f"
      $N strings $f | grep -E "$KEYS|eSRC_|eSNK_" | sort -u | sed 60q
    done
  done
fi

echo '=== 6. system log: audio connections the HMI built since boot'
sloginfo 2>/dev/null | grep -i -e preAllocate -e ConnectionMaster -e HighSync -e requestFocus -e Browser -e AudioMan \
  | tail -60

echo '=== end'
