# Notices

## This project

NBT Android Auto is licensed under the PolyForm Noncommercial License 1.0.0 (see [LICENSE](LICENSE),
`SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0`), except the files listed under
[GPLv3 parts](#gplv3-parts), which are licensed under the GNU General Public License version 3.

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

## FFmpeg

`nbt_decoder` is statically linked against FFmpeg 2.8.22 (<https://ffmpeg.org/>, tag `n2.8.22`), built with only the
H.264 decoder and no GPL or non-free parts, licensed under the GNU Lesser General Public License 2.1 or later
(<https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html>). FFmpeg is not included in this repository; the build
scripts download that tag, and [scripts/ffmpeg_qnx.py](scripts/ffmpeg_qnx.py),
[scripts/build-ffmpeg-qnx.sh](scripts/build-ffmpeg-qnx.sh) and `src/android_auto/decoder/ffmpeg/` (configuration,
list of files with their SHA-256) rebuild the same libraries, so `nbt_decoder` can be relinked against a modified
FFmpeg. The assembly is built with yasm 1.3.0 (BSD), which is not part of the binaries.

FFmpeg's VDPAU interface is built in (for the car's hardware decoder, `--hw`); it needs libvdpau's header
`vdpau/vdpau.h`, kept in `src/android_auto/decoder/ffmpeg/vdpau/` under its MIT licence (Copyright (c) 2008-2015
NVIDIA Corporation, notice in the file). libvdpau itself is not used.

## DejaVu fonts

`src/android_auto/decoder/nbt_panel_font.h` holds glyph bitmaps rendered from DejaVu Sans
(<https://dejavu-fonts.github.io/>). Fonts are (c) Bitstream (see below); DejaVu changes are in public domain.
Bitstream Vera Fonts Copyright (c) 2003 by Bitstream, Inc. All Rights Reserved. Bitstream Vera is a trademark of
Bitstream, Inc. Permission is hereby granted, free of charge, to any person obtaining a copy of the fonts accompanying
this license ("Fonts") and associated documentation files, to reproduce and distribute the Font Software, including
without limitation the rights to use, copy, merge, publish, distribute, and/or sell copies of the Font Software, and
to permit persons to whom the Font Software is furnished to do so, subject to the conditions of the Bitstream Vera
Fonts license (<https://dejavu-fonts.github.io/License.html>), whose copyright and trademark notices are reproduced
here.

## Installer

The installer app bundles Python, Tcl/Tk and paramiko (LGPL-2.1, <https://www.paramiko.org/>) with its dependencies
(cryptography, bcrypt, PyNaCl, cffi; Apache-2.0/BSD/MIT), packed with PyInstaller (GPL-2.0 with bootloader
exception). Their licenses ship inside the packaged app; the Python version uses them as installed by `pip`.

## Not included

No BMW, Harman, NVIDIA, Cinemo or QNX software is included in this repository or its releases. The programs use the
libraries that are already installed on the head unit at run time. Building requires the QNX Software Development
Platform 6.5, which you have to obtain under its own license.

## GPLv3 parts

These files are licensed under the GNU General Public License version 3 (see
[LICENSE-GPL-3.0.txt](LICENSE-GPL-3.0.txt), `SPDX-License-Identifier: GPL-3.0-only`), with the additional
permission below. They contain no code from NBT-AAD or the rest of this project:

- the decoder `nbt_decoder`: `src/android_auto/decoder/nbt_decoder.c`, `src/android_auto/decoder/ffmpeg/objects.txt`
  and `src/android_auto/decoder/ffmpeg/sources.txt`, and the FFmpeg build scripts `scripts/ffmpeg_qnx.py` and
  `scripts/build-ffmpeg-qnx.sh`
- `src/idrive/reboot/nbt_reset.c`
- `docs/DECODER.md`, `docs/FORK-CHANGES.md`, `docs/WORKFLOW.md`, `docs/SSH-ACCESS-SERIAL.md`

The other files in `src/android_auto/decoder/ffmpeg/` (`config.h`, `config.asm`, `avconfig.h`, `ffversion.h`) are
generated by FFmpeg's configure and stay under FFmpeg's license (LGPL 2.1 or later).

Copyright (C) 2026 Valentin Radu

This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
License as published by the Free Software Foundation, version 3.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied
warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

Additional permission under GNU GPL version 3 section 7: If you modify this program, or any covered work, by linking
or combining it with the libraries of the BMW NBT head unit (its operating system and its NVIDIA multimedia and
window libraries, such as `libnmsdk.so`, `libKD.so` and `libwal_nvidia.so`, and the libraries they use), containing
parts covered by the terms of their own licenses, the licensors of this program grant you additional permission to
convey the resulting work. Corresponding Source for a non-source form of such a combination shall not include the
source code for the parts of those libraries.

## Trademarks

BMW and iDrive are trademarks of BMW AG. Android and Android Auto are trademarks of Google LLC. This project is not
affiliated with or endorsed by them.
