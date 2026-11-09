#!/bin/sh
# Remove the "Android Auto" entry again (only the files the install script created) and its RAM control files.
# Send with: python tools\nbt_request.py "tr -d '\015' | sh -s" --input-file scripts\car-aa-entry-remove.sh --timeout 30
# The library may still be mapped by the running HMI: removing the directory entry is safe, the entry disappears after the
# next normal restart. The manifest is removed first so the HMI never finds a manifest without its library.
D=/var/opt/hmi/data/rw/Plugins/NbtAndroidAuto
if test ! -d $D; then echo "not installed: $D"; else
  rm -f $D/NbtAndroidAuto.manifest
  rm -f $D/libNbtAndroidAuto.so $D/NbtAndroidAuto.xml $D/NbtAndroidAuto_ALL_Texts.zip $D/NbtAndroidAuto_ALL_Images.zip
  rm -f $D/*.tmp
  # no rmdir on the car: only remove the directory when it is empty (rm -r would also delete unknown files)
  if test -z "`ls -A $D`"; then rm -r $D; else echo "directory not empty, left in place:"; ls -l $D; fi
fi
rm -f /fs/ram3/nbt-aa-hidden /fs/ram3/nbt-aa-request
ls -l /var/opt/hmi/data/rw/Plugins
echo '=== removed; gone from the menu after the next normal iDrive restart'
