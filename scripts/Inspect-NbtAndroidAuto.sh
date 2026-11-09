#!/bin/sh
# Read-only inventory. Send on SSH stdin to sh; no installation required.
# Do not open device nodes, switch USB mode, signal drivers or start services.
set -u
if [ "$(uname -s)" != QNX ] || [ "$(uname -p)" != x86 ]; then
    echo 'Expected QNX x86; inventory refused.' >&2
    exit 2
fi
echo 'NBT_AA_INVENTORY_BEGIN'
uname -a

echo 'USB_AND_GRAPHICS_DEVICE_NAMES'
for dir in /dev /dev/io-usb; do
    if [ -d "$dir" ]; then
        echo "$dir"
        ls -1 "$dir" | grep -E 'android|oap|usb|screen|display|video|audio|pcm|snd' || :
    fi
done

echo 'RELEVANT_PROCESS_ARGUMENTS'
if command -v pidin >/dev/null 2>&1; then
    pidin ar | grep -E 'io-usb|aoap|medialaunch|mm-renderer|io-audio|ioamp|WalServer|DisplayManager|HmiMain3D' || :
fi

echo 'LIBRARY_CANDIDATES_METADATA_ONLY'
for dir in /lib /usr/lib /boot/lib /opt/mm/lib /opt/conn/lib /opt/graphics/lib /opt/sys/lib /opt/share/lib; do
    for file in "$dir"/libusbdi.so* "$dir"/libssl.so* "$dir"/libcrypto.so* \
        "$dir"/libscreen.so* "$dir"/libKD.so* "$dir"/libEGL.so* \
        "$dir"/libGLESv2.so* "$dir"/liba2i_client.so* "$dir"/libasound.so* \
        "$dir"/libmmrenderclient.so* "$dir"/libcpp.so* "$dir"/libstdc++.so*; do
        if [ -e "$file" ]; then ls -l "$file"; fi
    done
done

echo 'USB_TOOLS_METADATA_ONLY'
for file in /opt/mm/bin/aoap-switch /opt/mm/bin/devc-serusb-aoap \
    /opt/sys/bin/aoap-switch /opt/sys/bin/devc-serusb-aoap \
    /opt/conn/bin/carplay/mm-renderer /usr/bin/usb /bin/usb /usr/sbin/usb; do
    if [ -e "$file" ]; then ls -l "$file"; fi
done

echo 'USB_ENUMERATION_CONFIG'
for file in /opt/sys/etc/umass-enum.cfg /opt/mm/etc/umass-enum.cfg /etc/umass-enum.cfg; do
    if [ -f "$file" ] && [ -r "$file" ]; then
        echo "$file"
        grep -n -E 'aoap|android|oap|usb|18[dD]1|2[dD]0' "$file" || :
    fi
done
echo 'NBT_AA_INVENTORY_END'
