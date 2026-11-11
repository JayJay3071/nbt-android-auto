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
- the Android Auto protocol core (`aa_session`, `sdr_builder`, `audio_acks`, `crash_handler`, and the browser audio
  mode `audio/browser_audio` with its helpers `asound_device`, `audio_route`, `mixer`, `resampler`, `drift`,
  `config`, `stream_rate`, `pcm_ring`, `playout`), which is built from the
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

## speexdsp

The `oneplus_native` binary contains the speexdsp 1.2.1 resampler (`resample.c`, unmodified), vendored in NBT-AAD under
`milestone-7/third_party/speexdsp/` and used by its browser audio mode. It is not included in this repository; the
build takes it from the NBT-AAD checkout. Its license:

```
Copyright 2002-2008 	Xiph.org Foundation
Copyright 2002-2008 	Jean-Marc Valin
Copyright 2005-2007	Analog Devices Inc.
Copyright 2005-2008	Commonwealth Scientific and Industrial Research
                        Organisation (CSIRO)
Copyright 1993, 2002, 2006 David Rowe
Copyright 2003 		EpicGames
Copyright 1992-1994	Jutta Degener, Carsten Bormann

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

- Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.

- Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.

- Neither the name of the Xiph.org Foundation nor the names of its
contributors may be used to endorse or promote products derived from
this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

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
