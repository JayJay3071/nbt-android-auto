# Native image updates and first live phone preview — vehicle confirmed

## Latest confirmed milestones

The focus-start revision works. The user confirmed Maps/Spotify appeared in
the already-open NBT Bildwechsel page, then saw it change to the captured AA
start logo and back without restarting. Evidence:
`build/image-frame-20261106T040858-d91d2779/`,
`build/image-frame-20261106T040921-4623215e/`, and
`build/image-frame-20261106T040932-18e668a4/`.

The next controlled USB test produced a **user-confirmed live preview** from
the currently connected OnePlus. `scripts/Bridge-NbtVideo.py` incrementally
reads the growing H.264 capture over SSH, decodes it with laptop FFmpeg, and
publishes the newest PNG through the native receiver. It does not wait for
capture completion. The user explicitly confirmed the display updated.

Results: 30-second capture limit, 393 media messages, one codec configuration,
278,060 bytes, invalid=0, capture exit=0. The bridge decoded 47 images and
published 18 over about 36 seconds. Receiver sequence advanced from 3 to 21,
frames=21, rejected=0. This is a low-rate laptop-dependent development preview,
not fluid or standalone projection. Audio and iDrive input are not implemented
by this pipeline. The test ended; the last displayed image remains static.

Artifacts: `build/aa-live-bridge-20261106/` and
`build/video-bridge-20261106T041616-1dd5659a/`. Capture and CSV were fetched twice
and byte-compared. The 90-second USB path lease expired normally, all six stock
paths were readable, and all eleven temporary USB test files were removed.
The three installed HMI plugins remain. The frame receiver expires after
3,000 polls, approximately ten minutes after first focus; reopening currently
does not rearm an expired receiver. Do not claim an ongoing AA session.

Capture now optionally accepts NBT_AA_CAPTURE_SECONDS=1..60 (default 15),
with up to seconds*30 messages when set, still bounded by 8 MiB. Outer USB
alarm is 75 seconds. The actual live test used 30 seconds to finish within
the already-running 90-second lease. Native protocol/capture regression
passed in a fresh VM test directory with its lab certificate fixtures.
Initial reruns failed because of existing O_EXCL captures and then missing
lab certificates in a fresh directory; neither was a vehicle protocol failure.

The bridge dry-run incrementally decoded the earlier real capture before EOF:
`build/video-bridge-20261106T041404-2a13f6b2/`. Its remote mode is bounded to
75 seconds and 8 MiB, keeps only the newest decoded frame, and uploads into
RAM with atomic rename. Next work: sustained receiver/session lifecycle,
reduce transport latency, then move decoding/feeding onto QNX to remove the
laptop dependency. The existing Cinemo VM experiments are relevant but not
yet integrated with this confirmed native image-update path.

The pending-test descriptions below are historical.

## First vehicle result and focus-start revision

The user confirmed NBT Bildwechsel opens with the test label. First packet
upload succeeded on 169.254.199.99, but receiver status remained `waiting`,
frames=0, rejected=0, sequence=0 even after a follow-up read. Thus no dynamic
update was confirmed. Evidence: `build/image-frame-20261106T040150-e45603a2/`.
Factory-time scheduling did not produce an observed callback; exact native
initialization behavior remains unproven.

The revised library starts polling from the page's focus notification using
`addHandler(10,1,ActionDelegate)` rather than posting during factory creation.
It records waiting_for_focus, focused, polling and poll counts to distinguish
registration/event/timer failures. Build and VM linkage passed. Installed by
staging and atomic rename (never overwriting the mapped library), with before,
staged and final readbacks. Artifacts `libNbtLiveImage-focus.*`, prior library
`libNbtLiveImage-before-focus.so`, and `focus-update.json` in the build directory.
This revision is not yet vehicle-validated. User has been asked to restart and
reopen the page; then inspect status before sending another packet.

Use 169.254.199.99 as requested. An alternate address had a matching SSH key,
but the user corrected that the car had only just come online and asked to
retain the original address. The worker supports --host with the original
host-key pin, but do not switch addresses against this preference.

The real OnePlus screenshot is vehicle-confirmed in NBT Bilddiagnose. The
next prototype, **NBT Bildwechsel**, is a separate plugin named NbtLiveImage.
It updates a `raImageModel` through the original NBT `PluginApp::updateModel`
overload for `bmw::Image`, scheduled by native `PluginApp::post(Runnable*, ms)`.
It does not receive Android Auto video yet and is labelled as a picture-change
test rather than live projection. Existing Hello NBT and NbtVideoTest are unchanged.

## Implementation and bounds

- `src/idrive/nbt_image_abi.hpp` declares the observed original x86 image ABI:
  BasicString at +0, 24-byte BufferStream at +4, loaded flag at +28, total 32.
  The first field is a string, not a vtable. Destruction releases the buffer
  and string through their native destructors.
- `dynamic_image_plugin.cpp` posts one receiver callback at a time, every
  200 ms, for at most 3,000 polls (about ten minutes after plugin construction).
  No worker threads, USB operations, or EGL/KD/driver calls are introduced.
- A per-instance token in `/fs/ram3/nbt-live-status.txt` prevents stale frames
  being accepted after loading. Only send frames after the page is confirmed
  open. The callback is queued at construction, but no model update happens
  until a producer supplies a matching token and valid packet.
- Input is `/fs/ram3/nbt-live-frame.bin`, atomically replaced by the producer.
  Header: 8 bytes `NBTFRM1\0`, then six little-endian uint32 values: instance,
  nonzero sequence, PNG length, PNG CRC32, reserved zero, reserved zero.
- At most 512 KiB per PNG; 8-bit RGB/RGBA, <=800x480, no interlace. Validate
  chunk bounds, every PNG chunk CRC, IHDR, IDAT, terminal IEND, total length,
  and packet CRC before submitting the native image update. Duplicate sequence
  numbers are skipped. One fixed input buffer bounds application allocations;
  native Image owns its copied bytes during submission.
- Factory destruction marks the receiver stopped, destroys the native app
  (which tears down its handler), then deletes the caller-owned Runnable.
  Lifecycle under actual HMI unloading is still a prototype limitation.

## Validation completed

Built in QNX 6.5 x86 with gcc_ntox86_cpp-ne, no exceptions/RTTI, -Wall -Wextra,
and shared-library unresolved symbols forbidden. Native libraries copied from
this vehicle were used in the VM, not the Evo reference ABI.

- `test_image_abi.cpp`: 2,000 native construct/copy/read/destroy cycles;
  mutation of input after construction does not change owned image bytes;
  stack guards remain intact.
- `test_dispatch_abi.cpp`: 1,000 native Handler post/dispatch/destruction cycles;
  callbacks execute once, Runnable remains caller-owned, pending delayed jobs
  are discarded on handler destruction. A local dummy IDispatcher drives this
  test; it does not exercise the real HMI lifecycle.
- `test_frame_packet.cpp`: real captured PNG accepted; corrupted CRC, truncation,
  trailing bytes, wrong instance, length mismatch and reserved fields rejected.
- VM and separate vehicle-process linkage checks passed; neither called the
  factory. Five installed files were read back byte-for-byte, manifest last.
  Temporary vehicle checker removed.

Artifacts: `build/idrive-dynamic-20261106/`; VM directory
`.nbt-build/idrive-dynamic-20261106`. Installed directory:
`/var/opt/hmi/data/rw/Plugins/NbtLiveImage`.

## Next visual test

User has been asked to normally restart iDrive and open NBT Bildwechsel.
Once confirmed, reconnect SSH and send one known real phone frame:

```
python scripts/Send-NbtImageFrame.py build/idrive-phone-image-20261106/phone-frame.png --page-open-confirmed
```

The script obtains the current token, builds a packet, uploads a uniquely named
RAM staging file, atomically renames it, then checks the receiver's sequence.
`FRAME_UPDATE_SUBMITTED` means the update call was issued, **not** that rendering
was visually confirmed. Ask the user to confirm it, then send a different captured
frame without restarting. Only after this works should continuous AA decoding
be connected to the producer. If status says expired, no further frames are sent.

Rollback removes only the five files in installed-sha256.json from this new
plugin directory after inspection, plus its owned RAM frame/status files. A
normal restart unloads the plugin. Do not alter the confirmed static plugins.
