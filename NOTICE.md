# Notices

## This project

NBT Android Auto is licensed under the PolyForm Noncommercial License 1.0.0 (see [LICENSE](LICENSE),
`SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0`).

Required Notice: Copyright (c) 2026 the NBT Android Auto contributors

## NBT-AAD

Parts of this project are taken from or based on NBT-AAD by nanaisu,
<https://github.com/Sq00ky/Supra-iDrive6-Android-Auto>, licensed under the PolyForm Noncommercial License 1.0.0:

- `src/android_auto/knob/ref/` (MOST/DSI client, CCS controller decoding)
- `src/android_auto/aa_cryptor.hpp`, `src/android_auto/aa_cryptor_mbedtls.cpp`
- `src/android_auto/accessory_window.c`, `src/android_auto/oneplus_aoa.c`, `src/android_auto/tls_usb_probe.cpp`
- the Android Auto protocol core (`aa_session`, `sdr_builder`, `audio_acks`, `crash_handler`), which is built from the
  upstream repository at commit `41975f2` plus `scripts/aa_session_overlay.py` and [patches/](patches/) (both are
  modifications of NBT-AAD under the same license) and is contained in the `oneplus_native` binary
- the head-unit TLS identity `cert.pem` / `key.pem` / `ca.pem` shipped in the installer payload (the well-known
  Android Auto head-unit test identity, as embedded in upstream `aa_cryptor.cpp`; it is public and not a secret)

Required Notice: Copyright (c) 2026 nanaisu - NBT-AAD (https://github.com/Sq00ky/Supra-iDrive6-Android-Auto)

The license text is also kept next to the code in [src/android_auto/LICENSE.NBT-AAD.md](src/android_auto/LICENSE.NBT-AAD.md).

## Mbed TLS

The QNX binaries are statically linked against Mbed TLS 3.6.7 (<https://github.com/Mbed-TLS/mbedtls>),
Copyright The Mbed TLS Contributors, licensed under the Apache License 2.0
(<https://www.apache.org/licenses/LICENSE-2.0>). Mbed TLS is not included in this repository; the build scripts
download the release archive.

## Installer

The installer app bundles Python, Tcl/Tk and paramiko (LGPL-2.1, <https://www.paramiko.org/>) with its dependencies
(cryptography, bcrypt, PyNaCl, cffi; Apache-2.0/BSD/MIT), packed with PyInstaller (GPL-2.0 with bootloader
exception). Their licenses ship inside the packaged app; the Python version uses them as installed by `pip`.

## Not included

No BMW, Harman, NVIDIA, Cinemo or QNX software is included in this repository or its releases. The programs use the
libraries that are already installed on the head unit at run time. Building requires the QNX Software Development
Platform 6.5, which you have to obtain under its own license.

## Trademarks

BMW and iDrive are trademarks of BMW AG. Android and Android Auto are trademarks of Google LLC. This project is not
affiliated with or endorsed by them.
