# Development guide

This page explains how the pieces are built and how to work on the car safely. The build is **not yet a single
command**: parts were built by hand in the QNX VM during development. Turning this into one reproducible build script
is one of the most useful contributions right now.

## What you need

- **QNX Software Development Platform 6.5.0 (x86)** in a virtual machine (VirtualBox works). It can be downloaded
  from QNX; it is not part of this repository. The compiler is `qcc -V4.4.2,gcc_ntox86` (C) / `gcc_ntox86_cpp` (C++).
  See [Setting up QNX 6.5.0](#setting-up-qnx-650) below.
- A Windows, macOS or Linux machine with Python 3.10+ for the installer and the helper scripts.
- An NBT with SSH root access for testing. A few proprietary libraries have to be copied **from your own NBT** for
  linking (see below). Never commit them.

Private files belong in `.local/` and build output in `build/`; both are ignored by git.

## Setting up QNX 6.5.0

1. Download **QNX 6.5.0** from QNX.
2. Install it in a VM. This video shows everything you need, step by step:
   [QNX 6.5.0 setup on YouTube](https://www.youtube.com/watch?v=3yjVaIwzPsc&t=182s).
3. **Tip: adjust the BIOS date of the VM** (set it back to an earlier date). In VirtualBox you can do this with an
   offset in milliseconds while the VM is off, for example one year back:

   ```sh
   VBoxManage modifyvm "QNX" --biossystemtimeoffset -31536000000
   ```

## QNX VM

The development VM used a host-only network (`192.168.56.10`, host `192.168.56.1`) and an unprivileged build user
`nbtbuild` with key login. After each VM start, as root on the VM console:

```sh
slay dhcp.client; ifconfig en0 192.168.56.10; /usr/sbin/sshd
```

Helper scripts for a Windows host:

- `config/ssh_config.example` → copy to `.local/ssh_config` and fill in user and key path.
- `scripts/Initialize-QnxTrust.ps1 -VerifiedFingerprint 'MD5:...'` pins the VM host key after you compared it on the
  VM console (`ssh-keygen -lf /etc/ssh/ssh_host_rsa_key.pub`).
- `scripts/Connect-QnxBootstrap.ps1` installs your public key for the build user (password typed by you, not stored).
- `scripts/Invoke-QnxBuild.ps1 -Action Probe` checks the toolchain; `build.cmd` builds and runs `src/hello_nbt.c` as a
  smoke test.

The old QNX OpenSSH 5.2 needs `HostKeyAlgorithms +ssh-rsa` and `MACs +hmac-sha1` (only for this host).

Environment for every build shell in the VM:

```sh
export QNX_HOST=/usr/qnx650/host/qnx6/x86
export QNX_TARGET=/usr/qnx650/target/qnx6
export PATH=$QNX_HOST/usr/bin:$PATH
```

## Components and how they are built

| Binary (on the car) | Source | Notes |
|---|---|---|
| `oneplus_native` | `src/android_auto/aa_video_capture.cpp`, `tls_usb_probe.cpp`, `oneplus_aoa.c` + NBT-AAD core | see "Android Auto core" |
| `native_service` | `src/android_auto/nbt_native_service.c` | supervisor; starts and watches the other processes |
| `fifo_http` | `src/android_auto/nbt_fifo_http.c` | serves the H.264 FIFO to Cinemo over local HTTP |
| `decode_runner` | `src/android_auto/nbt_decode_runner.c` | starts the stock `NmeCmdLine` with the hardware video device |
| `accessory_window` | `src/android_auto/accessory_window.c` | keeps the stock USB stack away from the phone |
| `nbt_knob`, `nbt_hwvideo`, `nbt_cluster` | `src/android_auto/knob/` | C++, use the MOST/DSI client in `knob/ref/` |
| `libNbtAndroidAuto.so` | `src/idrive/aa_entry_plugin.cpp`, `NbtAndroidAuto.xml`, `make_aa_entry_resources.py` | HMI plugin |
| `boot.sh`, `restart.sh` | `scripts/nbt-aa-boot.sh`, `scripts/nbt-aa-restart.sh` | autostart and service restart |

### Android Auto core

The protocol core comes from NBT-AAD ([Sq00ky/Supra-iDrive6-Android-Auto](https://github.com/Sq00ky/Supra-iDrive6-Android-Auto),
`milestone-7/`, commit `41975f2`). Check it out into `.local/reference-nbt-aad` and keep that checkout clean; the
scripts copy the files they need into a build archive.

1. `python scripts/Test-QnxTls.py` downloads Mbed TLS 3.6.7 (SHA-256 checked), builds the static libraries in the VM
   and runs the TLS tests.
2. `python scripts/Test-AndroidAutoCore.py` and `python scripts/Test-AndroidAutoSession.py` build the core
   (`aa_session`, `aa_cryptor`, `audio_acks`, `sdr_builder`, `crash_handler`) into `libnbt_aa_core.a`, run the
   session tests and link the `oneplus_capture` program (= `oneplus_native`) with `-DNBT_AA_TLS_HANDSHAKE -DNBT_AA_CAPTURE`.
3. The current car build also contains [patches/nbt-aad-sdr_builder-pixel-aspect.patch](../patches/nbt-aad-sdr_builder-pixel-aspect.patch)
   (pixel aspect ratio for the wide picture mode). Apply it to the source copy that is compiled, not to the clean
   reference checkout.

### HMI plugin

The plugin links against two libraries from the NBT itself. Copy them from your car (read only) into a VM folder,
for example `~/.nbt-build/hmi-libs/`:

- `libAppFramework.A.so`, `libCoreFramework.A.so` (from the HMI's library directory on the NBT)

Then in the VM:

```sh
qcc -V4.4.2,gcc_ntox86_cpp-ne -O2 -Wall -Wextra -shared -fPIC -fno-exceptions -fno-rtti -Wl,-z,defs -I. \
  aa_entry_plugin.cpp -o libNbtAndroidAuto.so -L$HOME/.nbt-build/hmi-libs -lAppFramework.A -lCoreFramework.A
```

Texts and images are packed by `src/idrive/make_aa_entry_resources.py`.

## Installer and release

1. Put the tested binaries into `build/aa-entry-20261106/` (service, `oneplus_native`, knob, hwvideo, cluster, decoder,
   `NbtAndroidAuto/`) and `build/native-prototype-20261106/package/` (`accessory_window`, `fifo_http`, PEM files).
2. `python installer/make_payload.py <version>` collects everything into `installer/payload/` and writes
   `manifest.json` with SHA-256 per file. It refuses a plugin library older than its source.
3. `installer/build_windows.ps1` builds the Windows .exe, `installer/build_mac.sh` the macOS app (on a Mac).
   paramiko must stay below 4 (the NBT only offers `ssh-rsa`).
4. Test against the VM first: the installer has hidden options `--key <ssh key>` and `--root <test dir>` to install
   below a test folder instead of the system.

Release assets: the Windows .exe, the zipped macOS app, `payload.zip` for the Python way, and SHA-256 sums.

## Working on the car

`tools/nbt_ssh_session.py` keeps one SSH session open (you type the root password in your own terminal; it is never
stored) and `tools/nbt_request.py` sends single commands or scripts through it:

```sh
python tools/nbt_ssh_session.py
python tools/nbt_request.py "tr -d '\015' | sh -s list" --input-file scripts/car-dev-leftovers.sh
```

Pin the NBT host key in `.local/nbt_known_hosts` first. The NBT shell lacks many tools (`head`, `printf`, `env`,
`which`); use `sed Nq`, `echo`, `ls -R`. Logs: `sloginfo`, `/fs/ram3/nbt-aa-boot.log`, the package `logs/` folder.

### Safety rules (learned the hard way)

- **Never stop or restart stock processes** such as `io-nme` or `HmiMain3D`. Stop only your own processes, with SIGTERM.
- **Do not revive the WAL sprite overlay** (`nbt_wal_*`, see `docs/history/`): unpaced sprite uploads deadlocked the
  `WalServer` and crashed the NVIDIA driver, which restarted the head unit.
- **No standalone EGL experiments** on the car; one crashed it. Never copy VM-only libraries to the car.
- Write files as `.tmp`, compare, then rename. Do not blindly repeat a command whose result is unknown.
- Test new things in RAM (`/fs/ram3/...`) first; it is gone after a restart.
- If the head unit hangs: hold the power button for about 30 s to restart the iDrive.

## Open topics

See [CONTRIBUTING.md](../CONTRIBUTING.md#where-help-is-needed).
