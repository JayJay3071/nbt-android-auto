#!/bin/sh
# Started by nbt_driver_stub when the USB launcher wants the iPod driver (iPod trial, 2026-11-14). $1 = the driver's
# argument string. Mounts our iPod stand-in at the path the launcher waits for: "mount=<path>" in the arguments,
# otherwise the first free one of /fs/ipod0, /fs/ipod1.
#
# Two modes:
#   $T/forward exists: listening proxy in front of the stock Bluetooth iPod driver (/fs/btipod). Everything the HMI and
#     mme ask is passed on to that driver and logged with its answer (research: the whole iPod conversation). Only
#     info.xml is our own (name "Android Auto", own serial number, our mount point).
#   otherwise: logging mirror of the copied layout in $S; $T/ans holds recorded answers of the real driver.
T=/fs/ram3/aatool
S=/fs/ram3/aaipod
# $T/off: the trial is switched off, the iPhone is simply not handled (as while the driver path was blocked)
if test -e $T/off; then echo "`date +%T` iPod driver asked, trial is off" >> $T/stub.log; exit 1; fi
M=`echo "$1" | sed -n 's/.*[,:]mount=\([^,]*\).*/\1/p'`
if test -z "$M"; then if test -e /fs/ipod0; then M=/fs/ipod1; else M=/fs/ipod0; fi; fi
# the iPhone's place on the USB ("busno=0:devno=4"): nbt_aa_ipod makes it register again when the HMI asks for that
U=`echo "$1" | sed -n 's/.*busno=\([0-9]*\):devno=\([0-9]*\).*/\1:\2/p'`
echo "`date +%T` iPod driver asked for: mount $M usb $U" >> $T/stub.log
if test -e $T/forward -a -e /fs/btipod/.FS_info./info.xml; then
  mkdir -p $T/ovr/.FS_info.
  sed -e "s|<mountpoint>[^<]*</mountpoint>|<mountpoint>$M</mountpoint>|" \
      -e "s|<name>[^<]*</name>|<name>Android Auto</name>|" \
      -e "s|<serial>[^<]*</serial>|<serial>NBTAA0001</serial>|" /fs/btipod/.FS_info./info.xml > $T/ovr/.FS_info./info.xml
  exec $T/nbt_dir_mirror --mount $M --source /fs/btipod --forward 1 --override $T/ovr --log $T/mirror.log
fi
# the real thing: our own iPod driver, fed by the running Android Auto session
if test -x $T/nbt_aa_ipod -a ! -e $T/mirror.mode; then
  exec $T/nbt_aa_ipod --mount $M --run /fs/ram3/nbt-aa-native --usb $U --log $T/ipod.log --verbose `test -e $T/quiet && echo 0 || echo 1`
fi
sed -e "s|<mountpoint>[^<]*</mountpoint>|<mountpoint>$M</mountpoint>|" $S/.FS_info./info.xml > $S/.FS_info./info.xml.new && mv $S/.FS_info./info.xml.new $S/.FS_info./info.xml
exec $T/nbt_dir_mirror --mount $M --source $S --answers $T/ans --log $T/mirror.log
