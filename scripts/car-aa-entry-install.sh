#!/bin/sh
# Install the "Android Auto" Multimedia entry (plugin NbtAndroidAuto) next to the vehicle-confirmed NbtLiveImage plugin.
# Preconditions: the five files of build/aa-entry-20261106/NbtAndroidAuto/ uploaded to /fs/ram3/nbt-aa-entry/ with
#   python tools\nbt_request.py "cat > /fs/ram3/nbt-aa-entry/<name>" --input-file <local file>
# Send with: python tools\nbt_request.py "tr -d '\015' | sh -s" --input-file scripts\car-aa-entry-install.sh --timeout 60
# Only files in the new plugin directory are written; nothing stock and none of the existing plugins is touched.
# The HMI loads the plugin after its next normal restart. Remove with scripts/car-aa-entry-remove.sh.
S=/fs/ram3/nbt-aa-entry
D=/var/opt/hmi/data/rw/Plugins/NbtAndroidAuto
FILES='libNbtAndroidAuto.so NbtAndroidAuto.xml NbtAndroidAuto_ALL_Texts.zip NbtAndroidAuto_ALL_Images.zip'
for f in $FILES NbtAndroidAuto.manifest; do
  if test ! -s $S/$f; then echo "MISSING $S/$f - nothing installed"; exit 2; fi
done
if test -e $D; then echo "ALREADY PRESENT: $D - nothing changed"; ls -l $D; exit 3; fi
mkdir -p $D || exit 4
# copy under temporary names, compare, then rename; the manifest comes last so the HMI never sees a half package
for f in $FILES NbtAndroidAuto.manifest; do
  cp $S/$f $D/$f.tmp || { echo "COPY FAILED $f"; exit 5; }
  if ! cmp -s $S/$f $D/$f.tmp; then echo "READBACK MISMATCH $f"; exit 6; fi
done
chmod 755 $D/libNbtAndroidAuto.so.tmp
chmod 644 $D/NbtAndroidAuto.xml.tmp $D/NbtAndroidAuto_ALL_Texts.zip.tmp $D/NbtAndroidAuto_ALL_Images.zip.tmp $D/NbtAndroidAuto.manifest.tmp
for f in $FILES NbtAndroidAuto.manifest; do mv $D/$f.tmp $D/$f || { echo "RENAME FAILED $f"; exit 7; }; done
for f in $FILES NbtAndroidAuto.manifest; do
  if cmp -s $S/$f $D/$f; then echo "ok $f"; else echo "FINAL MISMATCH $f"; fi
done
ls -l $D
echo '=== installed; active after the next normal iDrive restart (Multimedia -> Android Auto)'
