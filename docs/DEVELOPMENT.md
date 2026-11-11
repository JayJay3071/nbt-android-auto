# Development guide

This page explains how the pieces are built and how to work on the car safely. **One command builds every program of
the installed package** in the QNX VM: `python scripts/build_all.py` (see [Building everything](#building-everything)).
Built from this repository, the programs are byte-identical to the ones in installer 2026.11.10.2 (details below).

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

## Building everything

Which program is built from which files: [src/README.md](../src/README.md).

### Once

1. **NBT-AAD** (the Android Auto protocol core) at the exact commit the patches are made for:

   ```sh
   git clone https://github.com/Sq00ky/Supra-iDrive6-Android-Auto .local/reference-nbt-aad
   git -C .local/reference-nbt-aad checkout 41975f269b065dbdbc77af9f568387cd22b9ed27
   ```

   Keep this checkout unmodified; the build applies our changes to a copy.
2. **Two HMI libraries from your own NBT** for the plugin: copy `libAppFramework.A.so` and `libCoreFramework.A.so` from
   the HMI's library directory on the head unit (read only) into a folder in the VM, e.g. `~/.nbt-build/hmi-libs/`.
   They are BMW software: never commit or publish them. Without them everything except the plugin is built.
3. SSH from your PC to the VM works with a key (see [QNX VM](#qnx-vm)); the build runs as the normal VM user, not root.
   Mbed TLS 3.6.7 is downloaded automatically on the first run (SHA-256 checked).

### Every build

```sh
python scripts/build_all.py --host qnx-build --hmi-libs '$HOME/.nbt-build/hmi-libs' --compare installer/payload
```

`--identity <key>` if your SSH config does not name the VM key; `--compare` is optional (an installer payload, for
example unpacked from a release ZIP). The run takes about 5 minutes (most of it Mbed TLS) and leaves:

- `build/qnx-build-<time>/programs/` - all programs for the head unit, plus `boot.sh` / `restart.sh`
- `build/qnx-build-<time>/plugin-resources/` - plugin XML, manifest and the text/image ZIPs
- `build/qnx-build-<time>/build.log` - the full VM log with the comparison table

What `build_all.py` puts together (on your PC) and `scripts/build-qnx.sh` compiles (in the VM):

| Step | What |
|---|---|
| Mbed TLS 3.6.7 | release archive + `scripts/mbedtls_legacy_time.py` (old phone certificate dates) + QNX config (no AES-NI/ASM/PadLock), static libraries |
| NBT-AAD core `libnbt_aa_core.a` | upstream `milestone-7/src` + `scripts/aa_session_overlay.py` + `patches/*.patch` + our `aa_cryptor_mbedtls.cpp` (Mbed TLS instead of OpenSSL) |
| `oneplus_native` | `oneplus_aoa.c` (`-DNBT_AA_TLS_HANDSHAKE -DNBT_AA_CAPTURE`), `aa_video_capture.cpp`, `tls_usb_probe.cpp` + core + Mbed TLS, `-lsocket -lm -lusbdi` |
| `native_service`, `decode_runner` | C, `-O2` |
| `fifo_http`, `accessory_window` | C, no optimisation (as shipped) |
| `nbt_knob`, `nbt_hwvideo`, `nbt_cluster` | C++ `-O1`, with the MOST/DSI client from `knob/ref/` |
| `libNbtAndroidAuto.so` | C++ shared library without exceptions/RTTI, linked against the two HMI libraries |

**Verified on 2026-11-11** against installer 2026.11.10.2: `native_service`, `decode_runner`, `fifo_http`,
`accessory_window`, `nbt_knob`, `nbt_cluster` and `libNbtAndroidAuto.so` are byte-identical, `oneplus_native` is
identical apart from the symbol table (file names). `nbt_hwvideo` differs on purpose: the source has one more
read-only diagnostic option (`--registry-all`, 2026-11-08) than the shipped program.

## Installer and release

1. `python installer/make_payload.py <version> --built build/qnx-build-<time> --pem-dir <folder with cert.pem, key.pem, ca.pem>`
   collects everything into `installer/payload/` and writes `manifest.json` with SHA-256 per file. The PEM files are
   the public NBT-AAD head-unit identity; take them from `rt/` of a release payload. (Without `--built` the script
   uses the original developer's private build folders.)
2. `installer/build_windows.ps1` builds the Windows .exe, `installer/build_mac.sh` the macOS app (on a Mac).
   paramiko must stay below 4 (the NBT only offers `ssh-rsa`).
3. Test against the VM first: the installer has hidden options `--key <ssh key>` and `--root <test dir>` to install
   below a test folder instead of the system. Then test on a car.

Release assets: the Windows .exe, the Python package ZIP (installer + payload), and SHA-256 sums.

The older test scripts `scripts/Test-QnxTls.py`, `Test-AndroidAutoCore.py` and `Test-AndroidAutoSession.py` still run
the offline TLS and protocol tests (simulated phone) in the VM.

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
- **Do not revive the WAL sprite overlay** (`experiments/wal-overlay/`): unpaced sprite uploads deadlocked the
  `WalServer` and crashed the NVIDIA driver, which restarted the head unit.
- **No standalone EGL experiments** on the car; one crashed it. Never copy VM-only libraries to the car.
- Write files as `.tmp`, compare, then rename. Do not blindly repeat a command whose result is unknown.
- Test new things in RAM (`/fs/ram3/...`) first; it is gone after a restart.
- If the head unit hangs: hold the power button for about 30 s to restart the iDrive.

## Open topics

See [CONTRIBUTING.md](../CONTRIBUTING.md#where-help-is-needed).
