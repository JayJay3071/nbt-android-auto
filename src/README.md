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
 │   └─ per session:
 │       ├─ oneplus_native --switch-aa      phone ▶ Android Open Accessory mode
 │       ├─ fifo_http                       annexb.fifo ▶ http://127.0.0.1:18765
 │       ├─ decode_runner ▶ NmeCmdLine      stock Cinemo player draws into the stock video window
 │       ├─ nbt_hwvideo                     VideoApp (MOST) shows that window in front of iDrive
 │       └─ oneplus_native --capture-video  the Android Auto session:
 │             USB ▶ TLS/protocol (NBT-AAD core) ▶ H.264 ▶ annexb.fifo
 │             input.fifo ▶ key events ▶ phone
 │             navigation status ▶ nav.state
 └─ nbt_cluster ............... nav.state ▶ instrument cluster (MOST HUDTerminal): arrow, distance, street, shields
libNbtAndroidAuto.so (HMI plugin) ▶ menu entry, status page (reads /fs/ram3/nbt-aa-state), picture mode, restart button
```

Runtime directory: `/fs/ram3/nbt-aa-native` (RAM). Installed package: `/var/opt/hmi/data/rw/NbtAndroidAutoRT` and
`/var/opt/hmi/data/rw/Plugins/NbtAndroidAuto`.

## Programs and their sources

| Program | Sources | Notes |
|---|---|---|
| `native_service` | `android_auto/nbt_native_service.c` | Start here: the file header explains the whole session flow and the mode files. |
| `oneplus_native` | `android_auto/oneplus_aoa.c` (main, USB/AOA), `android_auto/tls_usb_probe.cpp` (TLS over USB), `android_auto/aa_video_capture.cpp` (session glue: video queue, controller input, navigation), `android_auto/aa_cryptor_mbedtls.cpp` + the NBT-AAD core | Built with `-DNBT_AA_TLS_HANDSHAKE -DNBT_AA_CAPTURE`. |
| `fifo_http` | `android_auto/nbt_fifo_http.c` | |
| `decode_runner` | `android_auto/nbt_decode_runner.c` | Explains the NmeCmdLine options and why decoding stays in software. |
| `accessory_window` | `android_auto/accessory_window.c` | Adapted from NBT-AAD. |
| `nbt_knob` | `android_auto/knob/nbt_knob.cpp` + `knob/ref/` | |
| `nbt_hwvideo` | `android_auto/knob/nbt_hwvideo.cpp` + `knob/ref/` | Background: [docs/HW-VIDEO-PATH.md](../docs/HW-VIDEO-PATH.md). |
| `nbt_cluster` | `android_auto/knob/nbt_cluster.cpp` + `knob/ref/` | Background: [docs/CLUSTER-HUDTERMINAL.md](../docs/CLUSTER-HUDTERMINAL.md). |
| `libNbtAndroidAuto.so` | `idrive/aa_entry_plugin.cpp`, `idrive/nbt_image_abi.hpp`, `idrive/NbtAndroidAuto.xml`, `idrive/NbtAndroidAuto.manifest`, `idrive/make_aa_entry_resources.py` | Links against two libraries from the head unit (see DEVELOPMENT.md). |

`android_auto/knob/ref/` is the MOST/DSI client and iDrive controller decoder from NBT-AAD (PolyForm Noncommercial,
notices kept in every file), adapted to this NBT.

## The Android Auto core (not in this folder)

The protocol itself (version exchange, TLS, service discovery, channels) is **NBT-AAD's `AASession`**
([Sq00ky/Supra-iDrive6-Android-Auto](https://github.com/Sq00ky/Supra-iDrive6-Android-Auto), `milestone-7/src`, commit
`41975f2`). It is compiled from that upstream checkout with these changes:

- `android_auto/aa_cryptor_mbedtls.cpp` replaces upstream `aa_cryptor.cpp` (Mbed TLS 3.6.7 instead of OpenSSL).
- `scripts/aa_session_overlay.py` applies checked edits to upstream `aa_session.cpp` (stricter phase checks, TLS errors).
- [patches/nbt-aad-sdr_builder-pixel-aspect.patch](../patches/) adds the pixel aspect ratio for the wide picture mode.
- `scripts/mbedtls_legacy_time.py` patches Mbed TLS to accept the old UTCTime format of some phone certificates.

## Tests and diagnostics (VM)

- `android_auto/test_*.c(pp)`: offline tests run in the QNX VM by `scripts/Test-QnxTls.py`,
  `scripts/Test-AndroidAutoCore.py` and `scripts/Test-AndroidAutoSession.py` (TLS in memory, a simulated phone,
  certificate formats, H.264 splitting, decoder supervision).
- `android_auto/usb_inventory.c`, `connection_inventory.c`, `tls_capabilities.c`: read-only diagnostics used while
  exploring the head unit.
- `idrive/check_plugin.c`: loads a plugin library in the VM to check its symbols, without starting it.
- `hello_nbt.c`: toolchain smoke test (`build.cmd`).
