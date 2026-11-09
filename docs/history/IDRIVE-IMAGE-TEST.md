# Native HMI phone image — confirmed on vehicle

## Confirmed first real Android Auto image on iDrive

The user opened **Multimedia → NBT Bilddiagnose** after restarting and confirmed
**Maps and Spotify visible**. The confirmed resources are in
`build/idrive-minimal-phone-image-20261106/` and installed in NbtVideoTest.
This establishes custom PNG archive loading and native image display using
the minimal layout, image ID 55002 and preinst=false. Hello NBT is unchanged.

The image originates from the OnePlus USB AA capture, decoded on the laptop
and installed as a static PNG. This is **not live projection**: continuous
capture, decoding and frame updates are not yet connected to this display.
Controller input and audio are also not established by this test.

Preserve this confirmed minimal layout as the baseline. The earlier richer
layout failed; the exact offending property/change has not been isolated.
The previously pending deployment notes below are historical.

Latest deployment: the minimal phone-image test described below is now
installed and verified. SSH reconnected successfully; all three baseline
resources matched the user-confirmed built-in-image version before staging.
All staged and installed bytes matched. Artifacts:
`build/idrive-minimal-phone-image-20261106/`. Menu title stays **NBT Bilddiagnose**,
label is **OnePlus Aufnahme - Standbild**, image is the 480x288 actual capture
PNG, custom ID 55002 with preinst=false. No image properties were added.
Hello NBT is untouched. User has been asked to restart and inspect the page;
custom-image display remains pending.

## Confirmed: native image component works

The user confirmed **NBT Bilddiagnose appears and displays both text and the
preinstalled symbol**. This validates the minimal image component/layout on
the actual vehicle. It does not yet validate our custom PNG archive or the
optional sizing/position properties used in the earlier failed pages.

Next prepared test: `.local/install_minimal_phone_image.py` preserves the
confirmed minimal layout and menu title, replaces the preinstalled image with
custom image 55002 (preinst=false), and labels it as a OnePlus still capture.
It verifies the current known baseline before writing and all transferred
bytes afterwards. **Not installed yet:** vehicle networking is currently
unreachable after the user's restart. Hello NBT remains untouched.

## Current diagnostic: preinstalled image, no custom PNG

The XML declaration-only test failed: the user still saw only Hello NBT.
The declaration hypothesis is rejected. Read-only offline analysis of the
vehicle HmiMain3D confirms CAbstractImage / CImageIDDataModel support and a
resource-size check against 0xfa000 (1,024,000 bytes), above our archive size.
System slog did not expose the actual plugin resource rejection.

The separate NbtVideoTest now contains **NBT Bilddiagnose**: the working Hello
XML structure plus one imageIdModel referencing native TerminalUI image 56202,
preinst=true, and an image component without optional properties. The image
archive is empty. Thus this test removes custom PNG decoding and explicit
image properties from the path. Hello NBT remains restored and untouched.
Resources are staged/readback verified. Evidence and preceding version backups:
`build/idrive-builtin-image-probe-20261106/`. Visual result is pending.

The private offline HMI binary and analysis are in `.local/hmi-analysis/`;
do not publish these vehicle binaries. The binary was read, not executed.

User subsequently confirmed restored Hello NBT is visible again. A controlled
test is now installed in NbtVideoTest only: replace the ElementTree-generated
XML declaration (single quotes, lowercase utf-8) with the exact declaration
style of the working original (double quotes, uppercase UTF-8). The parsed
XML tree is identical; no other resource or Hello NBT file changed. Staged and
installed bytes were checked. This is a compatibility hypothesis, **not a
confirmed cause**. User has been asked to restart and inspect Android Auto
Bildtest. Evidence: `build/idrive-phone-image-20261106/xml-header-test.*`;
previous XML: `xml-before-header-test.xml`.

Update: SSH reconnected after increasing the connection timeouts to 60 seconds.
The exact three original Hello NBT resources have now been restored, with
staged and final byte-for-byte readback checks. Evidence: the `.restored` files
in `build/idrive-phone-in-hello-20261106/`. Restoration on disk is complete;
visibility after the next normal HMI restart is still unconfirmed.

## Latest result: image resources prevent menu registration

The user reports that Hello NBT also disappeared after restarting with the
phone-image resources. This narrows the failure to the resource changes rather
than only the identity/registration of the second plugin. No image has yet
been displayed on the vehicle. The specific XML/image incompatibility is not
identified; successful file transfer and dlopen checks did not validate it.

`.local/restore_hello_resources.py` is prepared to restore all three exact
original resources from `build/idrive-phone-in-hello-20261106/*.before` with
staged and final readback verification. **Restoration has not run yet:** two
fresh SSH connections timed out and the vehicle IP could not be reached via
Ethernet. Do not mistake this pending rollback for completed restoration.

## Missing menu diagnostic and Hello NBT fallback

After the user's restart, both libHelloNbt.so and libNbtVideoTest.so were loaded
in HmiMain3D (PID 135214), but the new entry was absent. The user confirmed
Hello NBT still opened its original text page. Slog did not identify a plugin
registration error; the cause of the missing new entry remains unresolved.

For the next bounded visual test, Hello NBT's three resources were backed up
and updated to show the captured phone still. Its existing menu title, weight,
actions, state IDs, native library and manifest were retained. The image uses
custom image ID 55002 with explicit preinst=false. All staged and installed
resources were read back byte-for-byte. Evidence and original backups:
`build/idrive-phone-in-hello-20261106/`. The user must open **Hello NBT** after
a normal restart to check this version. Display is not yet confirmed.

## Real phone still image update (2026-11-06)

The three image/XML/text resources have now been replaced with a static frame
from the successfully captured OnePlus Android Auto dashboard. The menu title
is `Android Auto Bildtest`, with the explicit label `OnePlus Aufnahme - Standbild`.
The original library and manifest remain unchanged. This is not live projection.

`scripts/Prepare-NbtPhoneImage.py` creates these resources. Evidence and local
backups of the previous resources are in `build/idrive-phone-image-20261106/`.
Every old resource matched its recorded installed hash before replacement;
every staged and final resource was read back and compared byte-for-byte.
The running HMI still had only HelloNbt loaded before this update. The user has
been asked to perform a normal iDrive restart and inspect the new menu entry;
no remote restart or graphics probe was performed. Visual confirmation is pending.

The following describes the earlier synthetic image version.

The user requested a faster route to the first visible Android Auto picture.
On 2026-11-06 a separate `NBT Bildtest` Multimedia plugin was installed under
`/var/opt/hmi/data/rw/Plugins/NbtVideoTest`. Hello NBT remains unchanged.
The user reported the car is on but they cannot currently inspect the display.
No HMI restart was triggered. Discovery/rendering after the next normal HMI
restart must still be checked visually.

The page contains the explicit label `H.264-Testbild - kein Handybild` and a
480x288 PNG derived from the first frame of the pixel-validated Cinemo YUV420P
capture. It is a static synthetic color pattern, **not an Android Auto screen**.
This tests the existing HMI image component before adding live frame updates.
It does not establish sufficient frame rate or latency for projection.

The native factory reuses the vehicle-tested Hello NBT base PluginApp ABI,
with only the exported plugin name changed to `NbtVideoTest`. It contains no
EGL/KD calls, direct display manipulation, service launch or runtime hooks.
Image resources use `imageIdModel` and the native `image` component; the
schema is documented in the [RHMI analysis](https://bimmergestalt.github.io/BMWConnectedAnalysis/rhmi-resources/).

`scripts/Prepare-NbtImageTest.py` creates resources from the exact validated
capture, checks its hash, converts the first frame with FFmpeg and records
provenance. Build output is `build/idrive-image-test-20261106/`.
`src/idrive/video_test_plugin.cpp` selects the new plugin name while preserving
the default Hello NBT name. `check_plugin.c` now accepts an optional expected
name while retaining its previous default.

Validation completed:

- QNX x86 no-exceptions shared library built with unresolved symbols forbidden.
- VM and separate vehicle-process loader checks passed. Neither called the
  plugin factory or HMI services.
- All five installed files were read back and compared byte-for-byte;
  `installed-sha256.json` records hashes. The manifest was installed last.
- Temporary vehicle loader checker was removed. Existing plugins, firmware,
  boot settings and system services were not altered.

When the user is next at the car, look under Multimedia for `NBT Bildtest`
after normal HMI discovery/restart, open it and report whether the colored
pattern and its label appear. Absence before a restart is not evidence of a
rendering failure. Avoid repeating the earlier standalone EGL probe.

Rollback scope is exactly the five files listed in `installed-sha256.json`
inside the new plugin directory. A loaded plugin may require a subsequent
normal HMI restart to unload; do not remove or modify Hello NBT.
