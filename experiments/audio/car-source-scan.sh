#!/bin/sh
# car-source-scan.sh - which main audio sources (category 01 of HmiMain3D's connection table at 0x9877344) does the
# amplifier (ConnectionMaster 100E) build without a medium, and is one of the free io-amp inputs audible on them?
# 2026-11-18, step towards Android Auto music as a real source. Needs /fs/ram3/nbt_key and /fs/ram3/nbt_a2i_beep.
#
# For every connection: nbt_key cm 053 (build). Refused (op 9, e.g. 42 = not available) -> next. Accepted -> beeps into
# the free inputs, the number of beeps = the input:
#   1 beep  input_stereo_1      2 beeps input_stereo_2      3 beeps input_multich_0
#   4 beeps input_stereo_4      5 beeps input_stereo_5
# then back to AUX (bb). Listen and note which connection gave which beeps. Everything is also written to
# /fs/ram3/source-scan.txt. Takes a few minutes; run with the phone connected and AUX selected.
export LD_LIBRARY_PATH=/opt/sys/lib:/lib:/usr/lib
K=/fs/ram3/nbt_key
B=/fs/ram3/nbt_a2i_beep
L=/fs/ram3/source-scan.txt
: > $L
log() { echo "$*"; echo "$*" >> $L; }

beep() {   # input, beeps
  log "     $2 beep(s) -> $1"
  $B $1 $2 > /dev/null 2>&1
  sleep 2
}

scan() {   # label, id bytes, build handle, restore handle, iDrive source number
  out=`$K cm 053 6 $3 $2 2>&1`
  if echo "$out" | grep "fkt=053 op=D" > /dev/null; then
    log "== $1 (iDrive source $5) ACCEPTED"
    echo "$out" | grep "fkt=C03 op=C" | sed 1q | sed 's/^/     /' >> $L
    sleep 2
    beep input_stereo_1 1
    beep input_stereo_2 2
    beep input_multich_0 3
    beep input_stereo_4 4
    beep input_stereo_5 5
    $K cm 053 6 $4 00 bb > /dev/null 2>&1
    log "     back to AUX"
    sleep 2
  else
    err=`echo "$out" | grep "fkt=053 op=9" | sed 's/.*: //'`
    log "== $1 (iDrive source $5) refused: $err"
  fi
}

[ -x $K ] && [ -x $B ] || { echo "copy nbt_key and nbt_a2i_beep to /fs/ram3 first"; exit 2; }
scan 0001 "00 01" "02 00" "02 01" 1
scan 0068 "00 68" "02 02" "02 03" 2
scan 0062 "00 62" "02 04" "02 05" 3
scan 0018 "00 18" "02 06" "02 07" 4
scan 0020 "00 20" "02 08" "02 09" 5
scan 0024 "00 24" "02 0a" "02 0b" 6
scan 0027 "00 27" "02 0c" "02 0d" 7
scan 0028 "00 28" "02 0e" "02 0f" 8
scan 0086 "00 86" "02 10" "02 11" 9
scan 001b "00 1b" "02 12" "02 13" 10
scan 0050 "00 50" "02 14" "02 15" 11
scan 0053 "00 53" "02 16" "02 17" 12
scan 004d "00 4d" "02 18" "02 19" 13
scan 0080 "00 80" "02 1a" "02 1b" 14
scan 007d "00 7d" "02 1c" "02 1d" 15
scan 0077 "00 77" "02 1e" "02 1f" 16
scan 009a "00 9a" "02 20" "02 21" 17
scan 009d "00 9d" "02 22" "02 23" 18
scan 00a0 "00 a0" "02 24" "02 25" 19
scan 00b4 "00 b4" "02 26" "02 27" 20
scan 0123 "01 23" "02 28" "02 29" 23
scan 00c1 "00 c1" "02 2a" "02 2b" 24
scan 0122 "01 22" "02 2c" "02 2d" 25
scan 01b0 "01 b0" "02 2e" "02 2f" 26
scan 01b3 "01 b3" "02 30" "02 31" 27
scan 0088 "00 88" "02 32" "02 33" 28
scan 00c7 "00 c7" "02 34" "02 35" 30
scan 0150 "01 50" "02 36" "02 37" 31
scan 0153 "01 53" "02 38" "02 39" 32
scan 0156 "01 56" "02 3a" "02 3b" 33
scan 0159 "01 59" "02 3c" "02 3d" 34
scan 00ea "00 ea" "02 3e" "02 3f" 35
scan 00fc "00 fc" "02 40" "02 41" 36
scan 00ed "00 ed" "02 42" "02 43" 37
scan 00fd "00 fd" "02 44" "02 45" 38
scan 00f0 "00 f0" "02 46" "02 47" 39
scan 00fe "00 fe" "02 48" "02 49" 40
scan 00f3 "00 f3" "02 4a" "02 4b" 41
scan 00f6 "00 f6" "02 4c" "02 4d" 42
scan 00f9 "00 f9" "02 4e" "02 4f" 43
scan 01d0 "01 d0" "02 50" "02 51" 44
scan 01d3 "01 d3" "02 52" "02 53" 45
scan 01d6 "01 d6" "02 54" "02 55" 46
scan 00df "00 df" "02 56" "02 57" 47
scan 0100 "01 00" "02 58" "02 59" 48
scan 0109 "01 09" "02 5a" "02 5b" 49
scan 0110 "01 10" "02 5c" "02 5d" 50
scan 0119 "01 19" "02 5e" "02 5f" 51
scan 0124 "01 24" "02 60" "02 61" 52
scan 0127 "01 27" "02 62" "02 63" 53
scan 012a "01 2a" "02 64" "02 65" 54
scan 012d "01 2d" "02 66" "02 67" 55
scan 0200 "02 00" "02 68" "02 69" 56
scan 0209 "02 09" "02 6a" "02 6b" 57
scan 020f "02 0f" "02 6c" "02 6d" 58
scan 0212 "02 12" "02 6e" "02 6f" 59
scan 010c "01 0c" "02 70" "02 71" 60
scan 011c "01 1c" "02 72" "02 73" 61
scan 020c "02 0c" "02 74" "02 75" 62
scan 02a0 "02 a0" "02 76" "02 77" 63
scan 02a3 "02 a3" "02 78" "02 79" 64
scan 0166 "01 66" "02 7a" "02 7b" 65
scan 0169 "01 69" "02 7c" "02 7d" 66
scan 016c "01 6c" "02 7e" "02 7f" 67
scan 016f "01 6f" "02 80" "02 81" 68
scan 0071 "00 71" "02 82" "02 83" 69
scan 0220 "02 20" "02 84" "02 85" 120
scan 0221 "02 21" "02 86" "02 87" 121
$K cm 053 6 7f ff 00 bb > /dev/null 2>&1
log "== END (back on AUX)"
