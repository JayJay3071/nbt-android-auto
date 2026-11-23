# NBT CarPlay / Android Auto installer

Installs CarPlay and Android Auto on an original BMW NBT (QNX). Afterwards **Multimedia** has the entry
**CarPlay / Android Auto**. The installer only needs SSH access to the NBT: IP address and root password. The password
is not stored anywhere. Its window is in German or English, following the computer's language.

## First: SSH access

The stock NBT does not accept SSH logins, so SSH has to be unlocked once:

- with **FeatureInstaller**. It can take several attempts, and the last step sometimes reports errors although SSH
  works: test the connection afterwards, e.g. `ssh root@169.254.199.99`;
- or yourself through the debug serial port: [../docs/SSH-ACCESS-SERIAL.md](../docs/SSH-ACCESS-SERIAL.md).

The root password is given in this forum thread (about the NBT Evo in the Toyota Supra):
[SSH access guide (supramkv.com)](https://www.supramkv.com/threads/ssh-access-guide-supra-nbtevo-headunit.24190/).

Unlocking SSH changes the head unit's configuration. Like everything here, you do it at your own risk.

## Use

1. Connect the laptop to the car with the ENET cable, ignition on. The NBT usually answers at `169.254.199.99`.
2. Start the installer:
   - Windows: `NBT-AndroidAuto-Installer.exe`
   - macOS: `NBT-AndroidAuto-Installer.app`; the first time open it with right-click → Open (the app is not signed).
3. Enter the IP address and the password, click **Install / Update**.
4. Restart iDrive.
5. Plug in the phone and open **Multimedia → CarPlay / Android Auto**.

**Update:** run the installer of the new release the same way; settings are kept. **Uninstall** removes exactly the
two installed folders. **Save diagnostic logs** downloads the logs as a zip, e.g. for a bug report.

## What is written to the NBT

| Folder | Content |
|---|---|
| `/var/opt/hmi/data/rw/NbtAndroidAutoRT/` | The service and its programs, certificates, `boot.sh`, `restart.sh`, settings files, CarPlay pairing (`carplay/`), `logs/`, `snapshots/` |
| `/var/opt/hmi/data/rw/Plugins/NbtAndroidAuto/` | The iDrive plugin (menu entry and page) |

Every file is written as `.tmp` first, read back, compared byte by byte and only then renamed. No other system file
is changed.

## Without the app (Windows, macOS, Linux)

```
pip3 install "paramiko>=3.4,<4"
python3 nbt_aa_installer.py
python3 nbt_aa_installer.py --cli install
```

The first line installs paramiko (version 4 no longer supports the NBT's old `ssh-rsa`), the second opens the window,
the third installs from the terminal. The `payload/` folder must be next to the script.

## For developers

- `python installer/make_payload.py [version]` collects the build's files into `installer/payload/`, with
  `manifest.json` (SHA-256 per file). The folder is not checked in.
- `installer/build_windows.ps1` builds the Windows .exe, `installer/build_mac.sh` the app on a Mac.
- For tests in a QNX VM there are hidden options `--key <ssh-key>` and `--root <test folder>`, which install below a
  test folder instead of the system.
