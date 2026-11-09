T=/fs/ram3/nbt-video-test
cd $T || exit 2
k=1
for e in input_stereo_0 input_stereo_1 input_stereo_2 input_stereo_3 input_stereo_4 input_stereo_5; do
  echo "=== $k beep(s) -> $e"
  LD_LIBRARY_PATH=/opt/sys/lib:/lib:/usr/lib ./nbt_a2i_beep $e $k || LD_LIBRARY_PATH=/opt/sys/lib:/lib:/usr/lib ./nbt_a2i_beep /$e $k
  sleep 2
  k=$((k + 1))
done
echo '=== END'