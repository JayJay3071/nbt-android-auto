#!/bin/sh
# FFmpeg 2.8.22 (H.264 decoder only) as static libraries for nbt_decoder, in the QNX 6.5.0 VM (no configure: see
# scripts/ffmpeg_qnx.py). Started by build-qnx.sh in the folder build_all.py prepared:
#   ffmpeg/          the FFmpeg files from src/android_auto/decoder/ffmpeg/sources.txt
#   ffmpeg-config/   config.h, avconfig.h, ffversion.h, objects.txt, KEY
#   ffmpeg-asm/      FFmpeg's assembly as ELF objects (yasm, run by build_all.py)
# Output: ffmpeg-lib/libavcodec.a, ffmpeg-lib/libavutil.a, ffmpeg-lib/include/ (config.h with the probed values,
# libavutil/avconfig.h, libavutil/ffversion.h). The libraries are kept in $HOME/.nbt-build/ffmpeg-<KEY> and reused as
# long as nothing that goes into them changed (FFmpeg takes a few minutes to compile).
# For a test build elsewhere: FFCC (C compiler), FFWC (prefix of compiler-only options, empty for gcc), FFAR, FFCACHE.
set -e
W=`pwd`
FFCC=${FFCC:-"qcc -V4.4.2,gcc_ntox86"}
FFWC=${FFWC-"-Wc,"}
FFAR=${FFAR:-ntox86-ar}
KEY=`cat ffmpeg-config/KEY`
CACHE=${FFCACHE:-$HOME/.nbt-build}/ffmpeg-$KEY
OUT=$W/ffmpeg-lib
B=$W/ffmpeg-build
rm -rf $OUT $B
mkdir -p $OUT/include/libavutil $B
cp ffmpeg-config/config.h $OUT/include/config.h
cp ffmpeg-config/avconfig.h ffmpeg-config/ffversion.h $OUT/include/libavutil/

if test -f $CACHE/libavcodec.a -a -f $CACHE/libavutil.a -a -f $CACHE/config.h; then
  cp $CACHE/libavcodec.a $CACHE/libavutil.a $OUT/
  cp $CACHE/config.h $OUT/include/config.h
  echo "FFmpeg: libraries from an earlier build ($CACHE)"
  exit 0
fi

DEFS="-D_ISOC99_SOURCE -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -D_QNX_SOURCE -D_POSIX_C_SOURCE=200112 -D_XOPEN_SOURCE=600"
STD="${FFWC}-std=c99 ${FFWC}-Werror=implicit-function-declaration"

# what the QNX C library has (configure would find this out): compile and link a call, set the value in config.h
probe() {   # NAME "headers" "statements"
  {
    for h in $2; do echo "#include <$h>"; done
    echo "int main(void) { volatile double d = 0.5; volatile float f = 0.5f; char b[64]; (void)d; (void)f; (void)b;"
    echo "  $3; return 0; }"
  } > $B/probe.c
  if $FFCC $DEFS $STD $B/probe.c -o $B/probe -lm > $B/probe.log 2>&1; then v=1; else v=0; fi
  sed -e "s/^#define $1 [01]\$/#define $1 $v/" $OUT/include/config.h > $B/config.tmp
  mv $B/config.tmp $OUT/include/config.h
  PROBED="$PROBED $1=$v"
}
PROBED=""
probe HAVE_MALLOC_H "malloc.h" ""
probe HAVE_SYS_PARAM_H "sys/param.h" ""
probe HAVE_UNISTD_H "unistd.h" ""
probe HAVE_SYS_TIME_H "sys/time.h" ""
probe HAVE_SYS_MMAN_H "sys/mman.h" ""
probe HAVE_SYS_SELECT_H "sys/select.h" ""
probe HAVE_SYS_RESOURCE_H "sys/resource.h" ""
probe HAVE_DIRENT_H "dirent.h" ""
probe HAVE_POLL_H "poll.h" ""
probe HAVE_SYS_UN_H "sys/un.h" ""
probe HAVE_TERMIOS_H "termios.h" ""
probe HAVE_DLFCN_H "dlfcn.h" ""
probe HAVE_ACCESS "unistd.h" "access(b, 0)"
probe HAVE_ISATTY "unistd.h" "isatty(0)"
probe HAVE_USLEEP "unistd.h" "usleep(1)"
probe HAVE_SYSCONF "unistd.h" "sysconf(0)"
probe HAVE_FCNTL "fcntl.h" "fcntl(0, F_GETFL)"
probe HAVE_MKSTEMP "stdlib.h unistd.h" "mkstemp(b)"
probe HAVE_MMAP "sys/mman.h" "mmap(0, 0, PROT_READ, MAP_SHARED, 0, 0)"
probe HAVE_MPROTECT "sys/mman.h" "mprotect(b, 0, PROT_READ)"
probe HAVE_STRERROR_R "string.h" "strerror_r(1, b, sizeof(b))"
probe HAVE_CLOCK_GETTIME "time.h" "struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t)"
probe HAVE_NANOSLEEP "time.h" "struct timespec t = {0, 1}; nanosleep(&t, 0)"
probe HAVE_GETTIMEOFDAY "sys/time.h" "struct timeval t; gettimeofday(&t, 0)"
probe HAVE_GMTIME_R "time.h" "time_t t = 0; struct tm m; gmtime_r(&t, &m)"
probe HAVE_LOCALTIME_R "time.h" "time_t t = 0; struct tm m; localtime_r(&t, &m)"
probe HAVE_GETRUSAGE "sys/resource.h" "struct rusage u; getrusage(RUSAGE_SELF, &u)"
probe HAVE_PTHREAD_CANCEL "pthread.h" "pthread_cancel(pthread_self())"
probe HAVE_ATANF "math.h" "f = atanf(f)"
probe HAVE_ATAN2F "math.h" "f = atan2f(f, f)"
probe HAVE_CBRT "math.h" "d = cbrt(d)"
probe HAVE_CBRTF "math.h" "f = cbrtf(f)"
probe HAVE_COPYSIGN "math.h" "d = copysign(d, d)"
probe HAVE_COSF "math.h" "f = cosf(f)"
probe HAVE_ERF "math.h" "d = erf(d)"
probe HAVE_EXP2 "math.h" "d = exp2(d)"
probe HAVE_EXP2F "math.h" "f = exp2f(f)"
probe HAVE_EXPF "math.h" "f = expf(f)"
probe HAVE_ISINF "math.h" "d = isinf(d)"
probe HAVE_ISNAN "math.h" "d = isnan(d)"
probe HAVE_LDEXPF "math.h" "f = ldexpf(f, 1)"
probe HAVE_LLRINT "math.h" "d = llrint(d)"
probe HAVE_LLRINTF "math.h" "f = llrintf(f)"
probe HAVE_LOG2 "math.h" "d = log2(d)"
probe HAVE_LOG2F "math.h" "f = log2f(f)"
probe HAVE_LOG10F "math.h" "f = log10f(f)"
probe HAVE_LRINT "math.h" "d = lrint(d)"
probe HAVE_LRINTF "math.h" "f = lrintf(f)"
probe HAVE_POWF "math.h" "f = powf(f, f)"
probe HAVE_RINT "math.h" "d = rint(d)"
probe HAVE_ROUND "math.h" "d = round(d)"
probe HAVE_ROUNDF "math.h" "f = roundf(f)"
probe HAVE_SINF "math.h" "f = sinf(f)"
probe HAVE_TRUNC "math.h" "d = trunc(d)"
probe HAVE_TRUNCF "math.h" "f = truncf(f)"
# registers the inline assembly (CABAC) may use: not ebx with position-independent code, not ebp with a frame pointer
# (configure's checks, in a function compiled with FFmpeg's options)
probe_asm() {   # NAME assembly
  echo "void f(void); void f(void) { __asm__ volatile($2); } int main(void) { f(); return 0; }" > $B/probe.c
  if $FFCC $DEFS $STD ${FFWC}-O3 ${FFWC}-fomit-frame-pointer $B/probe.c -o $B/probe > $B/probe.log 2>&1; then v=1; else v=0; fi
  sed -e "s/^#define $1 [01]\$/#define $1 $v/" $OUT/include/config.h > $B/config.tmp
  mv $B/config.tmp $OUT/include/config.h
  PROBED="$PROBED $1=$v"
}
probe_asm HAVE_EBX_AVAILABLE '"" :: "b"(0)'
probe_asm HAVE_EBP_AVAILABLE '"xorl %%ebp, %%ebp" ::: "%ebp"'
echo "FFmpeg: C library probes:$PROBED"

CFLAGS="-I$OUT/include -I$W/ffmpeg $DEFS -DHAVE_AV_CONFIG_H -D__unix__ $STD ${FFWC}-march=i686 ${FFWC}-O3
 ${FFWC}-fomit-frame-pointer ${FFWC}-fno-math-errno ${FFWC}-fno-signed-zeros ${FFWC}-fno-tree-vectorize
 ${FFWC}-Werror=return-type"
echo "FFmpeg: compiling `grep -c '\.c$' ffmpeg-config/objects.txt` files (a few minutes)"
echo "  started `date`"
for f in `grep '\.c$' ffmpeg-config/objects.txt`; do
  o=$B/`echo $f | sed -e 's,/,_,g' -e 's,\.c$,.o,'`
  if ! $FFCC $CFLAGS -c ffmpeg/$f -o $o > $B/cc.log 2>&1; then
    echo "FAILED: $f"
    tail -40 $B/cc.log
    exit 1
  fi
done
cp ffmpeg-asm/*.o $B/   # the assembly, assembled with yasm by build_all.py
$FFAR rcs $OUT/libavcodec.a $B/libavcodec_*.o
$FFAR rcs $OUT/libavutil.a $B/libavutil_*.o
echo "  done `date`"

# keep them for the next build (only this version)
for d in `dirname $CACHE`/ffmpeg-*; do
  if test -d "$d" -a "$d" != "$CACHE"; then rm -rf "$d"; fi
done
mkdir -p $CACHE
cp $OUT/libavcodec.a $OUT/libavutil.a $OUT/include/config.h $CACHE/
