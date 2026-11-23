#!/bin/sh
# Build every program of the installed package inside the QNX 6.5.0 VM.
#
# Normally started by scripts/build_all.py, which prepares the current directory:
#   src/                 this repository's src/ folder
#   reference/           NBT-AAD milestone-7 src/ and include/ with our overlay and patches applied
#   mbedtls-3.6.7/       Mbed TLS release with the legacy-UTCTime patch and the QNX config
#   ffmpeg/, ffmpeg-config/, ffmpeg-asm/, build-ffmpeg-qnx.sh   FFmpeg for nbt_decoder (scripts/ffmpeg_qnx.py)
#   lwip/, monocypher/   lwIP and Monocypher sources for nbt_carplay (scripts/carplay_deps.py)
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
rm -f libnbt_aa_core.a
ntox86-ar rcs libnbt_aa_core.a core/aa_session.o core/aa_cryptor.o core/audio_acks.o core/sdr_builder.o core/crash_handler.o

echo "== oneplus_native (USB/AOA + TLS + Android Auto session)"
cd $A
$CC -Wall -Wextra -DNBT_AA_TLS_HANDSHAKE -DNBT_AA_CAPTURE -c oneplus_aoa.c -o $W/oneplus_aoa.o
$CXX $AAINC aa_video_capture.cpp aa_media_out.cpp aa_mic.cpp tls_usb_probe.cpp $W/oneplus_aoa.o $W/core/aa_cryptor.o $W/libnbt_aa_core.a $TLSLIBS \
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
$CXX -Iref nbt_key.cpp ref/dsi_client.cpp ref/most/client.cpp -o $OUT/nbt_key

echo "== nbt_a2i_beep (test: beeps into an io-amp input)"
$CC -Wall $A/audio/nbt_a2i_beep.c -o $OUT/nbt_a2i_beep -lm
echo "== nbt_decoder (decoder mode: FFmpeg 2.8.22 H.264 decoder + the car's video interface)"
cd $W
if test ! -f ffmpeg-config/KEY; then
  echo "SKIPPED: no FFmpeg sources (build_all.py could not get them)"
elif sh build-ffmpeg-qnx.sh > ffmpeg.log 2>&1; then
  cat ffmpeg.log
  if ! $CC -O2 -Wall -Wextra -Wc,-std=gnu99 -I$W/ffmpeg-lib/include -I$W/ffmpeg $A/decoder/nbt_decoder.c \
      $W/ffmpeg-lib/libavcodec.a $W/ffmpeg-lib/libavutil.a -lm -o $OUT/nbt_decoder; then
    echo "SKIPPED: nbt_decoder did not compile (see above)"
  fi
else
  cat ffmpeg.log
  echo "SKIPPED: FFmpeg build failed (see above)"
fi

echo "== HMI plugin libNbtAndroidAuto.so"
cd $W/src/idrive
if test -f $HMI_LIBS/libAppFramework.A.so -a -f $HMI_LIBS/libCoreFramework.A.so; then
  qcc -V4.4.2,gcc_ntox86_cpp-ne -O2 -Wall -Wextra -shared -fPIC -fno-exceptions -fno-rtti -Wl,-z,defs \
    aa_entry_plugin.cpp -o $OUT/libNbtAndroidAuto.so -L$HMI_LIBS -lAppFramework.A -lCoreFramework.A
else
  echo "SKIPPED: no libAppFramework.A.so / libCoreFramework.A.so in $HMI_LIBS (copy them from your NBT)"
fi

echo "== nbt_reset (Restart system on the Android Auto page)"
cd $W/src/idrive/reboot
$CC -O2 -Wall -Wextra nbt_reset.c -lsocket -o $OUT/nbt_reset

echo "== nbt_carplay_probe (test: CarPlay first steps, docs/CARPLAY.md)"
cd $W/src/carplay
if ! $CC -O2 -Wall -Wextra -Wc,-std=gnu99 $TLSINC nbt_carplay_probe.c $TLSLIBS -lusbdi -o $OUT/nbt_carplay_probe; then
  echo "SKIPPED: nbt_carplay_probe did not compile (see above)"
fi

echo "== nbt_carplay (wired CarPlay: lwIP, Monocypher, Mbed TLS; docs/CARPLAY.md)"
if test ! -d $W/lwip/src -o ! -d $W/monocypher/src; then
  echo "SKIPPED: no lwIP / Monocypher sources (build_all.py could not get them)"
else
  mkdir -p $W/carplay-obj
  LW=$W/lwip/src
  CPINC="-I$W/src/carplay -I$W/src/carplay/lwip_port -I$LW/include -I$W/monocypher/src -I$W/monocypher/src/optional $TLSINC"
  ok=1
  # third-party code without warnings; ours with
  for c in $LW/core/init.c $LW/core/def.c $LW/core/inet_chksum.c $LW/core/ip.c $LW/core/mem.c $LW/core/memp.c \
           $LW/core/netif.c $LW/core/pbuf.c $LW/core/sys.c $LW/core/tcp.c $LW/core/tcp_in.c $LW/core/tcp_out.c \
           $LW/core/timeouts.c $LW/core/udp.c $LW/core/ipv6/ethip6.c $LW/core/ipv6/icmp6.c $LW/core/ipv6/inet6.c \
           $LW/core/ipv6/ip6.c $LW/core/ipv6/ip6_addr.c $LW/core/ipv6/ip6_frag.c $LW/core/ipv6/mld6.c \
           $LW/core/ipv6/nd6.c $LW/api/api_lib.c $LW/api/api_msg.c $LW/api/err.c $LW/api/netbuf.c $LW/api/sockets.c \
           $LW/api/tcpip.c $LW/netif/ethernet.c $W/monocypher/src/monocypher.c \
           $W/monocypher/src/optional/monocypher-ed25519.c; do
    $CC -O2 -w -Wc,-std=gnu99 $CPINC -c $c -o $W/carplay-obj/`basename $c .c`.o || ok=0
  done
  cd $W/src/carplay
  for c in nbt_carplay cp_util cp_acp cp_usb cp_lockdown cp_iap2 cp_ncm cp_net_lwip cp_bplist cp_crypto cp_airplay \
           cp_screen cp_audio cp_mic cp_input lwip_port/sys_arch; do
    $CC -O2 -Wall -Wextra -Wc,-std=gnu99 $CPINC -c $c.c -o $W/carplay-obj/cp-`basename $c`.o || ok=0
  done
  if test $ok = 1 && $CC $W/carplay-obj/*.o $TLSLIBS -lusbdi -o $OUT/nbt_carplay; then
    :
  else
    echo "SKIPPED: nbt_carplay did not compile (see above)"
  fi
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
