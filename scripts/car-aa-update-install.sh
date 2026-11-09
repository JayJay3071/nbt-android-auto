#!/bin/sh
# Install an uploaded Android Auto update (files in /fs/ram3/aaupd/, sent with pscp) into the autostart package and the
# HMI plugin. Each file: copy as .tmp, compare, rename. Run: plink ... "sh /fs/ram3/aaupd/install.sh"
U=/fs/ram3/aaupd
P=/var/opt/hmi/data/rw/NbtAndroidAutoRT
D=/var/opt/hmi/data/rw/Plugins/NbtAndroidAuto
if test ! -d $P -o ! -d $D; then echo "package or plugin directory missing"; exit 2; fi
ok=1
put() {  # put SOURCE TARGET MODE
  if test ! -s $U/$1; then echo "MISSING $1"; ok=0; return; fi
  cp $U/$1 $2.tmp && cmp -s $U/$1 $2.tmp && chmod $3 $2.tmp && mv $2.tmp $2 && cmp -s $U/$1 $2 && echo "ok $1" || { echo "FAILED $1"; ok=0; }
}
put native_service $P/native_service 700
put oneplus_native $P/oneplus_native 700
put nbt_knob $P/nbt_knob 700
put nbt_hwvideo $P/nbt_hwvideo 700
put decode_runner $P/decode_runner 700
put libNbtAndroidAuto.so $D/libNbtAndroidAuto.so 755
tr -d '\015' < $U/nbt-aa-boot.sh > $U/boot.sh && put boot.sh $P/boot.sh 700
if test -e $U/res720; then touch $P/res720; echo 'res720 ON (phone sends 1280x720)'; fi
if test -e $U/res480; then rm -f $P/res720; echo 'res720 OFF (800x480)'; fi
ls -l $P $D
rm -r $U
if test $ok = 1; then echo '=== UPDATE OK - active after the next iDrive start'; else echo '=== UPDATE INCOMPLETE'; fi
