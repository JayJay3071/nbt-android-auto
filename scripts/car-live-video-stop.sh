#!/bin/sh
# Stop the native live-video mode on the car: feeder (if any) and native_service (which ends nbt_live_file, so the real
# file on the USB stick is visible again). SIGTERM only, pids taken from pidin and checked by name. RAM files stay.
# Send with: python tools\nbt_request.py "tr -d '\015' | sh -s" --input-file scripts\car-live-video-stop.sh --timeout 40
echo '=== before'
pidin ar | grep -e nbt_h264_feed -e native_service -e nbt_live_file -e accessory_window -e oneplus_native | grep -v grep
line=`pidin ar | grep nbt_h264_feed | grep -v grep | sed 1q`
set -- $line
if test -n "$1"; then kill $1; echo "SIGTERM nbt_h264_feed $1"; fi
line=`pidin ar | grep ' ./native_service' | grep -v grep | sed 1q`
set -- $line
if test -n "$1"; then kill $1; echo "SIGTERM native_service $1"; else echo 'native_service not running'; fi
sleep 8
echo '=== after'
pidin ar | grep -e nbt_h264_feed -e native_service -e nbt_live_file -e accessory_window -e oneplus_native | grep -v grep
echo '=== USB file visible again?'
ls -l /fs/usb0/ 2>&1 | sed 10q
cat /fs/ram3/nbt-aa-native/status.txt 2>&1
echo '=== END'
