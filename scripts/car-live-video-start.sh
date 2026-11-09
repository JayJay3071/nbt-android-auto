#!/bin/sh
# Start the native live-video mode on the car, RAM only (nothing persistent is changed).
# Send with: python tools\nbt_request.py "tr -d '\015' | sh -s" --input-file scripts\car-live-video-start.sh --timeout 60
# Preconditions (uploaded to /fs/ram3/nbt-video-test first, see docs/NATIVE-VIDEO-CAR-RUNBOOK.md):
#   native_service_live (built from src/android_auto/nbt_native_service.c), nbt_live_file (nbt_live_file.c)
# The USB stick with a video file of the SAME NAME as SHADOW must be plugged in: the stock HMI lists that file and
# nbt_live_file overlays its path with the live H.264 stream. The car has no head/printf/env/rmdir.
# Stick: SHADOW=/fs/usb0/nbt-test-video.mp4   Phone as MTP media device (no stick needed): the file copied to the phone's Movies folder.
SHADOW="/fs/pfs0/Interner gemeinsamer Speicher/Movies/nbt-test-video.mp4"
P=/var/opt/hmi/data/rw/NbtNativePrototype
R=/fs/ram3/nbt-aa-native
echo '=== persistent package'
ls -l $P 2>&1 | sed 20q
if test -e $R/service.lock; then echo 'SERVICE LOCK EXISTS - not starting'; ls -l $R; exit 2; fi
mkdir -p $R
chmod 700 $R
cp $P/native_frames $P/fifo_http $P/decode_runner $P/accessory_window $P/oneplus_native $P/cert.pem $P/key.pem $P/ca.pem $R/
cp /fs/ram3/nbt-video-test/native_service_live $R/native_service
cp /fs/ram3/nbt-video-test/nbt_live_file $R/nbt_live_file
chmod 700 $R/native_service $R/nbt_live_file $R/native_frames $R/fifo_http $R/decode_runner $R/accessory_window $R/oneplus_native
chmod 600 $R/*.pem
echo "$SHADOW" > $R/shadow.path
cd $R
LD_LIBRARY_PATH=/opt/mm/lib:/lib:/usr/lib ./native_service </dev/null >/dev/null 2>&1 &
echo "started native_service pid=$!"
sleep 5
echo '=== status'
cat status.txt
echo '=== live_file.log'
sed 10q live_file.log
echo '=== shadow visible (size 2147483647 means the overlay is attached)'
ls -l "$SHADOW"
echo '=== processes'
pidin ar | grep -e native_service -e nbt_live_file -e accessory_window -e oneplus_native | grep -v grep
echo '=== END'
