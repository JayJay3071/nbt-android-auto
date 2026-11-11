#!/bin/sh
# Read-only inventory of the stock NBT video route (v3, 2026-11-06).
# Usage: sh -s -- <section>     (script supplied on stdin)
# Sections: procs libs devs graphics cfg-cinemo cfg-mme cfg-generic cfg-search use slog all
# Does NOT open a display, start playback, create windows, signal a process or write any file.
# The car has NO head, printf or env (PATH=/usr/bin:/bin:/usr/sbin:/sbin): only echo, sed, grep, tail,
# ls, cat, pidin, use, sloginfo are used. head N is replaced by sed Nq.
# Every section is bounded so it finishes well inside the 40 s used by Collect-NbtVideoRoute.ps1.
LD_LIBRARY_PATH=/lib:/usr/lib:/opt/sys/lib:/opt/mm/lib:/opt/mm/lib/cinemo
export LD_LIBRARY_PATH

section="$1"
test -n "$section" || section=all

bounded_cat() { # file maxlines
  if test -r "$1"; then ls -l "$1"; sed "$2q" "$1"; else echo "MISSING_OR_UNREADABLE $1"; fi
}

proc_libs() { # label grep-pattern
  line=`pidin ar 2>/dev/null | grep -e "$2" | grep -v grep | sed 1q`
  if test -z "$line"; then echo "NOT_RUNNING $1"; return; fi
  set -- $line
  pid=$1
  echo "--- $pid $line"
  pidin -p $pid mem 2>/dev/null | grep -e '\.so' -e 'libs' | sed 80q
  echo "--- open files of $pid (pidin fd)"
  pidin -p $pid fd 2>/dev/null | sed 60q
}

do_procs() {
  echo 'VIDEO_ROUTE_PROCESSES'
  pidin ar 2>/dev/null | grep -e DisplayManager -e WalServer -e io-nme -e io-media -e mme -e '/bin/screen' \
    -e msvdx -e nvkdserver -e videocapture -e VideoApp -e HmiMain -e NBTMedia -e mmlauncher -e NmeCmdLine \
    -e native_service -e fifo_http | grep -v grep
}

do_libs() {
  echo 'VIDEO_ROUTE_LOADED_LIBRARIES (pidin -p PID mem, .so lines only)'
  proc_libs io-nme '/opt/mm/bin/io-nme'
  proc_libs io-media-cinemo 'io-media-cinemo'
  proc_libs mme-becker 'mme-becker'
  proc_libs io-msvdx 'io-msvdx'
  proc_libs DisplayManagerScreen 'DisplayManagerScreen'
  proc_libs WalServer 'WalServer'
  proc_libs screen ' /bin/screen'
  proc_libs HmiMain3D 'HmiMain3D'
  proc_libs NBTMediaMainApp 'NBTMediaMainApp'
}

do_devs() {
  echo 'VIDEO_ROUTE_DEVICE_NODES'
  ls -l /dev 2>/dev/null | grep -i -e io-nme -e iow -e screen -e msvdx -e wal -e nv -e video -e gf -e display -e mme -e media -e io-amp | sed 60q
  echo 'VIDEO_ROUTE_LIBRARY_LOCATIONS'
  for dir in /lib /usr/lib /lib/graphics /usr/lib/graphics /opt/graphics/lib /opt/sys/lib /opt/mm/lib /opt/mm/lib/cinemo /opt/hmi/lib; do
    for name in libscreen.so.1 libgf.so.1 libiow.so.1 libWFD.so.1 libKD.so.1 libKD.so libEGL.so libEGL.so.1 libwal_nvidia.so libwal.so; do
      if test -e "$dir/$name"; then ls -l "$dir/$name"; fi
    done
  done
}

do_graphics() {
  echo 'VIDEO_ROUTE_GRAPHICS_CONFIGURATION_CANDIDATES'
  ls -l /opt/graphics/etc /opt/sys/etc /opt/mm/etc /etc /usr/lib/graphics /lib/graphics 2>/dev/null \
    | grep -i -e video -e screen -e display -e cinemo -e window -e wal -e iow -e kd -e graphics -e layer -e nv | sed 80q
  echo 'VIDEO_ROUTE_ENVIRONMENT (set, filtered)'
  set 2>/dev/null | grep -i -e GRAPHICS -e SCREEN -e DISPLAY -e NME -e CINEMO | sed 20q
}

do_cfg_cinemo() {
  echo 'VIDEO_ROUTE_IO_MEDIA_CINEMO_CFG'
  bounded_cat /opt/mm/etc/io-media-cinemo.cfg 250
  echo 'VIDEO_ROUTE_CINEMO_DIRECTORY'
  ls -l /opt/mm/lib/cinemo 2>/dev/null | sed 80q
}

do_cfg_mme() {
  echo 'VIDEO_ROUTE_MME_CONF_FILTERED'
  ls -l /opt/mm/etc/mme.conf 2>/dev/null
  grep -n -i -e video -e cinemo -e layer -e window -e display -e iow -e screen -e nme /opt/mm/etc/mme.conf 2>/dev/null | sed 120q
  echo 'VIDEO_ROUTE_MMLAUNCHER_CFG_FILTERED'
  grep -n -i -e video -e cinemo -e io-nme -e msvdx -e screen /opt/mm/etc/mmlauncher_B069.cfg 2>/dev/null | sed 60q
}

do_cfg_generic() {
  echo 'VIDEO_ROUTE_IO_MEDIA_GENERIC_CFG'
  bounded_cat /opt/mm/etc/io-media-generic.cfg 120
}

do_cfg_search() {
  echo 'VIDEO_ROUTE_CONFIG_SEARCH (text configs only, binary libraries not scanned)'
  grep -l -a -i -e 'video-layer-id' -e 'video-window-class' -e 'NmeDevice' -e 'T_VIDEO_' -e 'VIDEO_LAYER' \
    /opt/mm/etc/* /opt/sys/etc/* /etc/* /opt/mm/lib/cinemo/*.xml /opt/hmi/data/ro/*.xml 2>/dev/null | sed 40q
  echo 'VIDEO_ROUTE_NME_OPTION_FILES'
  ls -l /opt/mm/etc /var/opt/mm /tmp/mm /fs/ram3 2>/dev/null | grep -i -e nme -e option -e cinemo | sed 20q
}

do_use() {
  echo 'VIDEO_ROUTE_USAGE_TEXT (use reads the embedded usage note; does not execute)'
  for b in /opt/mm/bin/io-media-cinemo /opt/mm/bin/io-nme /opt/mm/bin/io-msvdx /opt/sys/bin/DisplayManagerScreen /opt/sys/bin/WalServer /bin/screen; do
    echo "--- use $b"
    use $b 2>&1 | sed 40q
  done
}

do_slog() {
  echo 'VIDEO_ROUTE_SLOG_FILTERED (tail, wal frame statistics excluded)'
  sloginfo 2>/dev/null | grep -v delta_t | grep -i -e nme -e cinemo -e iow -e video -e layer -e msvdx -e screen -e wal | tail -80
}

case "$section" in
  procs) do_procs ;;
  libs) do_libs ;;
  devs) do_devs ;;
  graphics) do_graphics ;;
  cfg-cinemo) do_cfg_cinemo ;;
  cfg-mme) do_cfg_mme ;;
  cfg-generic) do_cfg_generic ;;
  cfg-search) do_cfg_search ;;
  use) do_use ;;
  slog) do_slog ;;
  all) do_procs; do_libs; do_devs; do_graphics; do_cfg_cinemo; do_cfg_mme; do_cfg_generic; do_cfg_search; do_use; do_slog ;;
  *) echo "unknown section: $section"; exit 2 ;;
esac
exit 0
