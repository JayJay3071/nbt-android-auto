#!/bin/sh
# Which io-amp input is audible right now? (2026-11-07, preparation for Android Auto audio without Bluetooth)
# Plays beepK.wav (K beeps) into candidate input K through Cinemo's A2I audio device, one after the other.
# The listener counts the beeps. Nothing stock is changed; NmeCmdLine is only a client of the shared io-nme.
# Needs /fs/ram3/nbt-video-test/beep1..6.wav and NmeCmdLine there.
# Send with: python tools\nbt_request.py "tr -d '\015' | sh -s" --input-file scripts\car-audio-probe.sh --timeout 90
T=/fs/ram3/nbt-video-test
cd $T || exit 2
k=1
for dev in input_multich_0 input_stereo_0 input_stereo_1 input_stereo_2 input_stereo_3 input_stereo_4; do
  echo "=== $k beep(s) -> /dev/io-amp/$dev"
  LD_LIBRARY_PATH=/opt/mm/lib/cinemo:/opt/mm/lib:/lib:/usr/lib ./video_runner 8 ./NmeCmdLine -v0 -ad NmeDeviceA2I -ai /dev/io-amp/$dev -i $T/beep$k.wav 2>&1 | grep -e 'Audio decoded' -e RUNNER -e rror | sed 3q
  sleep 1
  k=$((k + 1))
done
sloginfo | grep -i -e a2i -e io-amp -e NmeDeviceA2I | tail -12
echo '=== END'
