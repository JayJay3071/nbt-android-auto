#!/bin/sh
# Cached enumeration only: no -D descriptor requests, device opens or mode switch.
set -u
if [ "$(uname -s)" != QNX ] || [ "$(uname -p)" != x86 ]; then
    echo 'Expected QNX x86.' >&2
    exit 2
fi
echo 'NBT_USB_SNAPSHOT_BEGIN'
# The vehicle's noninteractive SSH environment omits its library directories.
LD_LIBRARY_PATH=/lib:/usr/lib:/opt/sys/lib /usr/bin/usb -t
usb_status=$?
echo "USB_QUERY_STATUS=$usb_status"
echo 'ACCESSORY_NODE_NAMES'
for node in /dev/android* /dev/oap*; do
    if [ -e "$node" ]; then ls -ld "$node"; fi
done
echo 'ACCESSORY_PROCESS_ARGUMENTS'
pidin ar | grep -E '/(aoap-switch|devc-serusb-aoap)( |$)' || :
echo 'NBT_USB_SNAPSHOT_END'
exit "$usb_status"
