#!/bin/sh
# Persistent Android Auto package for the automatic start by the "Android Auto" plugin (2026-11-07).
# Copies the current RAM test binaries (/fs/ram3/nbt-video-test, uploaded by scripts\Upload-NbtAaTest.ps1) and the
# unchanged helpers/certificates of the older persistent package into a NEW directory; nothing else is touched.
# Send with: python tools\nbt_request.py "tr -d '\015' | sh -s" --input-file scripts\car-aa-autostart-install.sh
# The boot script itself is uploaded separately as /fs/ram3/nbt-aa-boot.sh.
# Remove: rm -r /var/opt/hmi/data/rw/NbtAndroidAutoRT   (disable only: touch .../NbtAndroidAutoRT/disabled)
T=/fs/ram3/nbt-video-test
O=/var/opt/hmi/data/rw/NbtNativePrototype
P=/var/opt/hmi/data/rw/NbtAndroidAutoRT
for f in $T/native_service_live $T/oneplus_native $T/nbt_knob $T/nbt_hwvideo $T/decode_runner \
         $O/accessory_window $O/fifo_http $O/cert.pem $O/key.pem $O/ca.pem /fs/ram3/nbt-aa-boot.sh; do
  if test ! -s $f; then echo "MISSING $f - nothing installed"; exit 2; fi
done
mkdir -p $P && chmod 700 $P || exit 3
cp $T/native_service_live $P/native_service.tmp && mv $P/native_service.tmp $P/native_service
for f in oneplus_native nbt_knob nbt_hwvideo decode_runner; do cp $T/$f $P/$f.tmp && mv $P/$f.tmp $P/$f; done
for f in accessory_window fifo_http cert.pem key.pem ca.pem; do cp $O/$f $P/$f.tmp && mv $P/$f.tmp $P/$f; done
cp /fs/ram3/nbt-aa-boot.sh $P/boot.sh.tmp && mv $P/boot.sh.tmp $P/boot.sh
chmod 700 $P/native_service $P/oneplus_native $P/nbt_knob $P/nbt_hwvideo $P/decode_runner $P/accessory_window $P/fifo_http $P/boot.sh
chmod 600 $P/*.pem
cmp -s $T/native_service_live $P/native_service && cmp -s $T/oneplus_native $P/oneplus_native && cmp -s $T/nbt_knob $P/nbt_knob \
  && cmp -s $T/nbt_hwvideo $P/nbt_hwvideo && cmp -s $T/decode_runner $P/decode_runner && echo READBACK_OK
ls -l $P
