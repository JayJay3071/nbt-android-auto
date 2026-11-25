# Notices

## This project

Copyright (C) 2026 JayJay3071 and the NBT Android Auto contributors

The code written for this project is licensed under the GNU General Public License version 3 (see
[LICENSE-GPL-3.0.txt](LICENSE-GPL-3.0.txt), `SPDX-License-Identifier: GPL-3.0-only`), with the additional permission
below. That is every file of this repository that is not named in one of the sections [NBT-AAD](#nbt-aad),
[Files with code from valinet's fork under PolyForm Noncommercial](#files-with-code-from-valinets-fork-under-polyform-noncommercial),
[GPLv3 parts by Valentin Radu](#gplv3-parts-by-valentin-radu) or as third-party software further down. Until version
2026.11.24 the whole project was under the PolyForm Noncommercial License 1.0.0; the versions published then keep it.

    This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
    License as published by the Free Software Foundation, version 3.

    This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied
    warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
    details.

Additional permission under GNU GPL version 3 section 7: If you modify this program, or any covered work, by linking
or combining it with (a) NBT-AAD (<https://github.com/Sq00ky/Supra-iDrive6-Android-Auto>) or the files of this
repository that are under the PolyForm Noncommercial License 1.0.0, or (b) the libraries of the BMW NBT head unit (its
operating system, its HMI framework and its multimedia and window libraries, and the libraries they use), containing
parts covered by the terms of their own licenses, the licensors of this program grant you additional permission to
convey the resulting work. Corresponding Source for a non-source form of such a combination shall not include the
source code for the parts of the head unit's libraries. The parts under the PolyForm Noncommercial License keep that
license, so a work that contains them may only be used and conveyed for noncommercial purposes.

## Binary releases

The installer and the payload of a release contain programs built from all parts of this repository. They are
distributed under the PolyForm Noncommercial License 1.0.0 (see
[LICENSE-PolyForm-Noncommercial-1.0.0.md](LICENSE-PolyForm-Noncommercial-1.0.0.md)): free for private and other
noncommercial use, no commercial use. The NBT-AAD parts require that; this project's own code is included by its
copyright holder; Valentin Radu's programs are included under his permission, granted on 2026-11-24
(<https://github.com/JayJay3071/nbt-android-auto/issues/8#issuecomment-7147670516>), reproduced here in full and unmodified, and also kept as
[PERMISSION-valinet-binary-releases.txt](PERMISSION-valinet-binary-releases.txt):

```
Additional Permission for Binary Releases

I, Valentin Radu <valentin.radu@valinet.ro>, copyright holder 
of the files listed below (the "Contributions"), licensed under 
GPL-3.0-only, grant JayJay3071 the following additional permission:

1. You may compile the Contributions together with [nbt-android-auto](https://github.com/JayJay3071/nbt-android-auto) 
   and distribute the resulting binary releases of
   nbt-android-auto under the PolyForm Noncommercial License 1.0.0,
   without the GPL-3.0 conditions applying to those binaries.

2. Recipients of those binary releases receive a license to them
   from me under the PolyForm Noncommercial License 1.0.0.

3. Conditions. Every binary release must include:
   a) the Required Notice below;
   b) the full text of this permission;
   c) a link to [https://github.com/JayJay3071/nbt-android-auto], where the Contributions are
      available in source form under GPL-3.0-only.
   If the software has an About screen, credits screen, or
   similar user-visible information, items a) through c) must be
   reproduced there in full, unmodified.

4. This permission does not allow:
   a) distributing the Contributions in source form under any
      license other than GPL-3.0-only;
   b) granting commercial licenses to, or relicensing, the
      Contributions under any other terms;
   c) transferring or sublicensing this permission.

Required Notice: Copyright (c) 2026 Valentin Radu <valentin.radu@valinet.ro>

Covered files: everything file labeled GPL-3.0-only in the repository.
```

The source of every release is this repository, <https://github.com/JayJay3071/nbt-android-auto>, at the tag of the
release; `scripts/build_all.py` builds the programs from it.

Required Notice: Copyright (c) 2026 Valentin Radu <valentin.radu@valinet.ro>

Required Notice: Copyright (c) 2026 nanaisu - NBT-AAD (https://github.com/Sq00ky/Supra-iDrive6-Android-Auto)

Required Notice: Copyright (c) 2026 JayJay3071 and the NBT Android Auto contributors

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

The license text is in [LICENSE-PolyForm-Noncommercial-1.0.0.md](LICENSE-PolyForm-Noncommercial-1.0.0.md) and also
kept next to the code in [src/android_auto/LICENSE.NBT-AAD.md](src/android_auto/LICENSE.NBT-AAD.md).

## Files with code from valinet's fork under PolyForm Noncommercial

These files of this project contain parts taken from their versions in valinet's fork
(<https://github.com/valinet/nbt-android-auto>), which he published under the PolyForm Noncommercial License 1.0.0
(decoder mode and CarPlay mode of the service, the page with its buttons and its pool of focus events, the start and
build steps for his programs). They are under the PolyForm Noncommercial License 1.0.0:

- `src/android_auto/nbt_native_service.c`
- `src/idrive/aa_entry_plugin.cpp`, `src/idrive/NbtAndroidAuto.xml`, `src/idrive/make_aa_entry_resources.py`,
  `src/idrive/reboot/README.md`
- `scripts/nbt-aa-boot.sh`, `scripts/build-qnx.sh`, `scripts/build_all.py`

Required Notice: Copyright (c) 2026 JayJay3071 and Valentin Radu

## GPLv3 parts by Valentin Radu

Three programs come from valinet's fork of this project (<https://github.com/valinet/nbt-android-auto>),
Copyright (C) 2026 Valentin Radu <valentin.radu@valinet.ro>. They are licensed under the GNU General Public License
version 3 (see [LICENSE-GPL-3.0.txt](LICENSE-GPL-3.0.txt), `SPDX-License-Identifier: GPL-3.0-only`), with the two
additional permissions of their author: the one for the head unit's libraries at the end of this section, and the one
for this project's binary releases under [Binary releases](#binary-releases). Their complete source is in this
repository, and `scripts/build_all.py` builds the binaries of a release from it.

- `nbt_decoder` (the 1280-wide picture, with decoding by the car's own H.264 decoder):
  `src/android_auto/decoder/nbt_decoder.c`, `src/android_auto/decoder/nbt_hwdec.h`,
  `src/android_auto/decoder/ffmpeg/objects.txt` and `sources.txt`, and the FFmpeg build scripts
  `scripts/ffmpeg_qnx.py` and `scripts/build-ffmpeg-qnx.sh`
- `nbt_carplay` (wired CarPlay): everything in `src/carplay/`, `docs/CARPLAY.md`, `scripts/carplay_deps.py` and
  `scripts/mbedtls_empty_name.py`
- `nbt_reset` (restart of the head unit): `src/idrive/reboot/nbt_reset.c`

Changes made in this project (2026-11-24, by JayJay3071, under the same license, GPL-3.0-only):
`nbt_decoder.c`: placeholder stream (`--dummy`), the marker file `decoder.picture`, the trip computer box
(`--panel`, `nbt_panel_font.h`) removed; `src/carplay/`: 16-bit NCM format for iOS 27 (`cp_usb.c`), the sound into
this project's sound FIFO (`cp_audio.c`), titles, cover and route guidance from iAP2 (`cp_iap2.c`). The list is also
in [docs/CARPLAY.md](docs/CARPLAY.md) and [docs/DECODER.md](docs/DECODER.md).

The other files in `src/android_auto/decoder/ffmpeg/` (`config.h`, `config.asm`, `avconfig.h`, `ffversion.h`) are
generated by FFmpeg's configure and stay under FFmpeg's license (LGPL 2.1 or later); `vdpau/vdpau.h` and
`vdpau/vdpau_x11.h` stay under libvdpau's MIT license (see [FFmpeg](#ffmpeg)). What `nbt_decoder` and `nbt_carplay`
are linked against is listed under [FFmpeg](#ffmpeg), [lwIP and Monocypher](#lwip-and-monocypher) and
[Mbed TLS](#mbed-tls).

    Copyright (C) 2026 Valentin Radu

    This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
    License as published by the Free Software Foundation, version 3.

    This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied
    warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
    details.

Additional permission under GNU GPL version 3 section 7: If you modify this program, or any covered work, by linking
or combining it with the libraries of the BMW NBT head unit (its operating system and its NVIDIA multimedia and
window libraries, such as `libnmsdk.so`, `libKD.so` and `libwal_nvidia.so`, and the libraries they use), containing
parts covered by the terms of their own licenses, the licensors of this program grant you additional permission to
convey the resulting work. Corresponding Source for a non-source form of such a combination shall not include the
source code for the parts of those libraries.

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

## lwIP and Monocypher

`nbt_carplay` (wired CarPlay) is statically linked against lwIP 2.2.1 (<https://savannah.nongnu.org/projects/lwip>,
tag `STABLE-2_2_1_RELEASE`) and Monocypher 4.0.2 (<https://monocypher.org>, tag `4.0.2`). Neither is included in this
repository; the build scripts fetch those tags (`scripts/carplay_deps.py`).

lwIP: Copyright (c) 2001, 2002 Swedish Institute of Computer Science. All rights reserved. Redistribution and use in
source and binary forms, with or without modification, are permitted provided that the following conditions are met:
1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following
disclaimer. 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and
the following disclaimer in the documentation and/or other materials provided with the distribution. 3. The name of
the author may not be used to endorse or promote products derived from this software without specific prior written
permission. THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
SUCH DAMAGE.

Monocypher: dual-licensed, used here under CC0-1.0 (public domain) or, as a fallback, the 2-clause BSD license:
Copyright (c) 2017-2023, Loup Vaillant; Copyright (c) 2017-2019, Michael Savage; Copyright (c) 2017-2023, Fabio
Scotoni. All rights reserved. Redistribution and use in source and binary forms, with or without modification, are
permitted provided that the source code and binary redistributions retain this notice and its disclaimer (THE SOFTWARE
IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND).

## xcertplay

The CarPlay protocol knowledge in `src/carplay/` (USB, iAP2, AirPlay messages, keys, HID descriptors, audio format
bits) follows xcertplay (<https://github.com/shilapi/xcertplay>, GPL-3.0), which was read as the reference; no Kotlin
code is included.

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
