#!/bin/sh
# READ-ONLY follow-up (2026-11-08): format names in the capture driver, APIX links, guidance viewer (junction view).
str() { f=$1; shift; tr -c '[:print:]' '\n' < "$f" 2>/dev/null | grep "$@" | sed 50q; }
echo '=== capture formats'
str /opt/sys/bin/dev-nvideocapture -e CAPTURE_FORMAT -e WVGA -e WXGA -e 'FORMAT_' -e eCaptureFormat
echo '=== apix devices'
ls -l /dev 2>/dev/null | grep -i -e apix -e ssi -e kombi
ls /dev/apix* /dev/ssi* 2>/dev/null
echo '=== dev-ssi-hbfpga strings'
B=`ls /opt/sys/bin/dev-ssi-hbfpga /opt/sys/bin/zipped/dev-ssi-hbfpga 2>/dev/null | sed 1q`; echo "binary $B"
test -n "$B" && str $B -i -e apix -e kombi -e display -e 1280 -e width -e resol
echo '=== guidance viewer binary'
G=`ls /fs/sda0/opt/nav/*/bin/zipped/NavigationGuidanceViewer_NV /fs/sda0/opt/nav/*/bin/NavigationGuidanceViewer_NV /opt/nav/*/bin/NavigationGuidanceViewer_NV 2>/dev/null | sed 1q`
echo "binary $G"
test -n "$G" && str $G -i -e kombi -e cluster -e apix -e 'screen' -e 'window' -e 'junction' -e 'crossing' -e 'gv_zone' -e 'zoom' -e 'wal::'
echo '=== guidance viewer / kombi in the system log'
sloginfo 2>/dev/null | grep -i -e GuidanceViewer -e kombi -e apix -e 'KI_' -e cluster | sed 40q
echo '=== WAL screens/windows in the log'
sloginfo 2>/dev/null | grep -i WalServer | grep -i -e screen -e display -e window | sed 30q
echo '=== WalServer strings'
str /opt/sys/bin/WalServer -i -e kombi -e cluster -e apix -e 'screen' -e 'display'
echo done
