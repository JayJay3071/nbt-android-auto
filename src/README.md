# Source map

What runs on the head unit, which source files it is built from, and how the parts talk to each other. Build commands
and the QNX setup are in [docs/DEVELOPMENT.md](../docs/DEVELOPMENT.md). Old experiments are in
[experiments/](../experiments/), not here.

## The processes on the NBT

```
boot.sh (scripts/nbt-aa-boot.sh, started by the HMI plugin 15 s after the HMI)
 ├─ native_service ............ supervisor: finds the phone, starts one session after the other
 │   ├─ accessory_window ...... keeps the stock USB stack away from the phone (whole service life)
 │   ├─ nbt_knob .............. iDrive controller (MOST, observe-only) ──▶ input.fifo
 │   ├─ nbt_decoder ........... annexb.fifo ▶ the head unit's video interface, 1280x480 (started once, kept)
 │   └─ per session:
 │       ├─ oneplus_native --switch-aa      phone ▶ Android Open Accessory mode
 │       ├─ nbt_carplay                     an iPhone instead: the whole CarPlay session, same FIFOs and state files
 │       ├─ (with decoder.off: fifo_http, decode_runner ▶ NmeCmdLine and nbt_hwvideo, the stock player path)
 │       └─ oneplus_native --capture-video  the Android Auto session:
 │             USB ▶ TLS/protocol (NBT-AAD core) ▶ H.264 ▶ annexb.fifo
 │             input.fifo ▶ key events ▶ phone
 │             navigation status ▶ nav.state, media status ▶ media.state
 │             media sound (PCM) ▶ audio.fifo
 │   └─ nbt_a2i_play .......... audio.fifo ▶ car amplifier (A2I); in iPod mode this is nbt_aa_wms (SBC sound device)
 ├─ iap.sh (iPod-over-Bluetooth mode, scripts/nbt-aa-iap.sh)
 │   ├─ nbt_ipod_tap .......... in front of the Bluetooth audio driver: phone counts as Apple device, sound = nbt_aa_wms
 │   └─ nbt_aa_ipod ........... Apple driver stand-in: titles, cover, length, skip keys (from media.state)
 └─ nbt_cluster ............... nav.state ▶ instrument cluster (MOST HUDTerminal): arrow, distance, street, shields
libNbtAndroidAuto.so (HMI plugin) ▶ menu entry, status page (reads /fs/ram3/nbt-aa-state), decoder and restart buttons
 └─ nbt_reset ................. "Restart NBT": diagnostic reset of the head unit through the car's gateway
```

Runtime directory: `/fs/ram3/nbt-aa-native` (RAM). Installed package: `/var/opt/hmi/data/rw/NbtAndroidAutoRT` and
`/var/opt/hmi/data/rw/Plugins/NbtAndroidAuto`.

## Programs and their sources

| Program | Sources | Notes |
|---|---|---|
| `native_service` | `android_auto/nbt_native_service.c` | Start here: the file header explains the whole session flow and the mode files. |
| `oneplus_native` | `android_auto/oneplus_aoa.c` (main, USB/AOA), `android_auto/tls_usb_probe.cpp` (TLS over USB), `android_auto/aa_video_capture.cpp` (session glue: video queue, controller input, navigation), `android_auto/aa_cryptor_mbedtls.cpp` + the NBT-AAD core | Built with `-DNBT_AA_TLS_HANDSHAKE -DNBT_AA_CAPTURE`. |
| `nbt_decoder` | `android_auto/decoder/nbt_decoder.c`, `decoder/nbt_hwdec.h`, FFmpeg 2.8.22 | GPLv3, from valinet's fork. Background: [docs/DECODER.md](../docs/DECODER.md). |
| `nbt_carplay` | `carplay/` | GPLv3, from valinet's fork. Background: [docs/CARPLAY.md](../docs/CARPLAY.md). |
| `nbt_reset` | `idrive/reboot/nbt_reset.c` | GPLv3, from valinet's fork. [idrive/reboot/README.md](idrive/reboot/README.md). |
| `fifo_http` | `android_auto/nbt_fifo_http.c` | Stock player path only (`decoder.off`). |
| `decode_runner` | `android_auto/nbt_decode_runner.c` | Stock player path only. Explains the NmeCmdLine options and why decoding stays in software there. |
| `accessory_window` | `android_auto/accessory_window.c` | Adapted from NBT-AAD. |
| `nbt_knob` | `android_auto/knob/nbt_knob.cpp` + `knob/ref/` | Radio panel keys for media: `config/mfl.map`. |
| `nbt_a2i_play` | `android_auto/nbt_a2i_play.c` | Media sound into the amplifier (`sound.args`, `sound.nostop`). |
| `nbt_aa_wms` | `android_auto/nbt_aa_wms.c` | SBC encoder + sound device `/dev/wms/nbtaa` for the iPod mode. |
| `nbt_ipod_tap` | `android_auto/nbt_ipod_tap.c` | Filter in front of `/fs/avrcp0` (iPod mode). |
| `nbt_aa_ipod` | `android_auto/nbt_aa_ipod.c` | Takes `/fs/btipod`: track info and skip keys (iPod mode). |
| `nbt_bt_inject` | `android_auto/nbt_bt_inject.c` | Older alternative: titles on the phone's normal Bluetooth entry (when the iPod mode is off). |
| `nbt_hwvideo` | `android_auto/knob/nbt_hwvideo.cpp` + `knob/ref/` | Stock player path only. Background: [docs/HW-VIDEO-PATH.md](../docs/HW-VIDEO-PATH.md). |
| `nbt_cluster` | `android_auto/knob/nbt_cluster.cpp` + `knob/ref/` | Background: [docs/CLUSTER-HUDTERMINAL.md](../docs/CLUSTER-HUDTERMINAL.md). |
| `libNbtAndroidAuto.so` | `idrive/aa_entry_plugin.cpp`, `idrive/nbt_image_abi.hpp`, `idrive/NbtAndroidAuto.xml`, `idrive/NbtAndroidAuto.manifest`, `idrive/make_aa_entry_resources.py` | Links against two libraries from the head unit (see DEVELOPMENT.md). |

How the sound and the iPod mode were found out: [docs/history/HANDOFF-2026-11-12-AUDIO-STABILITY.md](../docs/history/HANDOFF-2026-11-12-AUDIO-STABILITY.md)
and [docs/history/HANDOFF-2026-11-24-IPOD-OVER-BLUETOOTH.md](../docs/history/HANDOFF-2026-11-24-IPOD-OVER-BLUETOOTH.md).

`android_auto/knob/ref/` is the MOST/DSI client and iDrive controller decoder from NBT-AAD (PolyForm Noncommercial,
notices kept in every file), adapted to this NBT.

## The Android Auto core (not in this folder)

The protocol itself (version exchange, TLS, service discovery, channels) is **NBT-AAD's `AASession`**
([Sq00ky/Supra-iDrive6-Android-Auto](https://github.com/Sq00ky/Supra-iDrive6-Android-Auto), `milestone-7/src`, commit
`41975f2`). It is compiled from that upstream checkout with these changes:

- `android_auto/aa_cryptor_mbedtls.cpp` replaces upstream `aa_cryptor.cpp` (Mbed TLS 3.6.7 instead of OpenSSL).
- `scripts/aa_session_overlay.py` applies checked edits to upstream `aa_session.cpp` (stricter phase checks, TLS errors).
- [patches/0001-nbt-aad-aa_session-byebye.patch](../patches/) adds the clean session end (`postByeBye`, ByeByeRequest /
  ByeByeResponse) that `aa_video_capture.cpp` uses.
- [patches/0002-nbt-aad-sdr_builder-pixel-aspect.patch](../patches/) adds the pixel aspect ratio for the wide picture mode.
- [patches/0003-nbt-aad-flow-control-media.patch](../patches/) adds video flow control and the frame-rate limit (the
  phone never runs ahead of the stock player), the media status channel (track info, cover) and the media keys.

Every change to the NBT-AAD core goes into a new numbered patch here; never change the core only in the build VM.

`scripts/build_all.py` applies all of this automatically and builds every program (see DEVELOPMENT.md).
- `scripts/mbedtls_legacy_time.py` patches Mbed TLS to accept the old UTCTime format of some phone certificates.

## Tests and diagnostics (VM)

- `android_auto/test_*.c(pp)`: offline tests run in the QNX VM by `scripts/Test-QnxTls.py`,
  `scripts/Test-AndroidAutoCore.py` and `scripts/Test-AndroidAutoSession.py` (TLS in memory, a simulated phone,
  certificate formats, H.264 splitting, decoder supervision).
- `android_auto/usb_inventory.c`, `connection_inventory.c`, `tls_capabilities.c`: read-only diagnostics used while
  exploring the head unit.
- `idrive/check_plugin.c`: loads a plugin library in the VM to check its symbols, without starting it.
- `hello_nbt.c`: toolchain smoke test (`build.cmd`).
