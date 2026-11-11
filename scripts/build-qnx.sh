#!/bin/sh
# Build every program of the installed package inside the QNX 6.5.0 VM.
#
# Normally started by scripts/build_all.py, which prepares the current directory:
#   src/                 this repository's src/ folder
#   reference/           NBT-AAD milestone-7 src/ and include/ with our overlay and patches applied
#   mbedtls-3.6.7/       Mbed TLS release with the legacy-UTCTime patch and the QNX config
#   expected/            optional: binaries of an installer payload to compare against
# Environment: HMI_LIBS = folder with libAppFramework.A.so and libCoreFramework.A.so copied from your own NBT
#              (needed only for the HMI plugin; without it the plugin is skipped).
# Output: out/ (the programs as they go into installer/payload/rt and plugin/).
set -e
QNX_HOST=/usr/qnx650/host/qnx6/x86
QNX_TARGET=/usr/qnx650/target/qnx6
PATH=$QNX_HOST/usr/bin:$PATH
export QNX_HOST QNX_TARGET PATH

W=`pwd`
OUT=$W/out
A=$W/src/android_auto
HMI_LIBS=${HMI_LIBS:-$HOME/.nbt-build/hmi-libs}
CC="qcc -V4.4.2,gcc_ntox86"
CXX="qcc -V4.4.2,gcc_ntox86_cpp -lang-c++ -O1 -Wall -D_QNX_SOURCE -Wc,-std=gnu++0x"
TLSINC="-I$W/mbedtls-3.6.7/include"
TLSLIBS="$W/mbedtls-3.6.7/library/libmbedtls.a $W/mbedtls-3.6.7/library/libmbedx509.a $W/mbedtls-3.6.7/library/libmbedcrypto.a"
AAINC="$TLSINC -I$W/reference/src -I$W/reference/include"
mkdir -p $OUT

echo "== Mbed TLS 3.6.7 static libraries (a few minutes)"
if ! (cd mbedtls-3.6.7 && make lib CC="$CC" AR=ntox86-ar CFLAGS='-O2 -D_QNX_SOURCE -Wc,-std=gnu99' >$W/mbedtls-build.log 2>&1); then
  tail -20 $W/mbedtls-build.log
  exit 1
fi

echo "== NBT-AAD core: libnbt_aa_core.a"
mkdir -p core
for c in aa_session aa_cryptor audio/audio_acks sdr_builder crash_handler; do
  $CXX $AAINC -c reference/src/$c.cpp -o core/`basename $c`.o
done
# browser audio mode (Android Auto sound over the AA connection, off unless the aa-audio mode file exists)
for c in browser_audio asound_device audio_route mixer resampler drift config stream_rate pcm_ring playout; do
  $CXX $AAINC -c reference/src/audio/$c.cpp -o core/audio_$c.o
done
$CC -O1 -I$W/reference/src -c reference/src/audio/speex.c -o core/audio_speex.o   # vendored speexdsp resampler
rm -f libnbt_aa_core.a
ntox86-ar rcs libnbt_aa_core.a core/aa_session.o core/aa_cryptor.o core/audio_acks.o core/sdr_builder.o core/crash_handler.o \
  core/audio_*.o

echo "== oneplus_native (USB/AOA + TLS + Android Auto session)"
cd $A
$CC -Wall -Wextra -DNBT_AA_TLS_HANDSHAKE -DNBT_AA_CAPTURE -c oneplus_aoa.c -o $W/oneplus_aoa.o
$CXX $AAINC aa_video_capture.cpp aa_audio.cpp tls_usb_probe.cpp $W/oneplus_aoa.o $W/core/aa_cryptor.o $W/libnbt_aa_core.a $TLSLIBS \
  -lsocket -lm -lusbdi -o $OUT/oneplus_native

echo "== native_service, decode_runner, fifo_http, accessory_window"
$CC -O2 -Wall -Wextra nbt_native_service.c -o $OUT/native_service
$CC -O2 -Wall -Wextra nbt_decode_runner.c -o $OUT/decode_runner
$CC -Wall -Wextra nbt_fifo_http.c -lsocket -o $OUT/fifo_http   # no -O: as in the shipped package
$CC -Wall -Wextra accessory_window.c -o $OUT/accessory_window

echo "== nbt_knob, nbt_hwvideo, nbt_cluster (MOST/DSI)"
cd $A/knob
$CXX -Iref nbt_knob.cpp ref/dsi_client.cpp ref/most/client.cpp ref/car/ccs_input.cpp -o $OUT/nbt_knob
$CXX -Iref nbt_hwvideo.cpp ref/dsi_client.cpp ref/most/client.cpp -o $OUT/nbt_hwvideo
$CXX -Iref nbt_cluster.cpp ref/dsi_client.cpp ref/most/client.cpp -o $OUT/nbt_cluster -lm

echo "== HMI plugin libNbtAndroidAuto.so"
cd $W/src/idrive
if test -f $HMI_LIBS/libAppFramework.A.so -a -f $HMI_LIBS/libCoreFramework.A.so; then
  qcc -V4.4.2,gcc_ntox86_cpp-ne -O2 -Wall -Wextra -shared -fPIC -fno-exceptions -fno-rtti -Wl,-z,defs \
    aa_entry_plugin.cpp -o $OUT/libNbtAndroidAuto.so -L$HMI_LIBS -lAppFramework.A -lCoreFramework.A
else
  echo "SKIPPED: no libAppFramework.A.so / libCoreFramework.A.so in $HMI_LIBS (copy them from your NBT)"
fi

cd $W
echo "== built"
ls -l $OUT

if test -d expected; then
  echo "== compare with expected/ (SAME = byte-identical, SAME-STRIPPED = identical without symbol tables)"
  mkdir -p cmp
  for f in `ls $OUT`; do
    if test ! -f expected/$f; then echo "NO-REFERENCE  $f"; continue; fi
    if cmp -s $OUT/$f expected/$f; then echo "SAME          $f"; continue; fi
    cp $OUT/$f cmp/a; cp expected/$f cmp/b; ntox86-strip cmp/a cmp/b
    if cmp -s cmp/a cmp/b; then echo "SAME-STRIPPED $f"; else echo "DIFFERENT     $f"; fi
  done
fi
echo "BUILD_QNX_DONE"
