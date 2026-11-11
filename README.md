# NBT Android Auto

Wired Android Auto for the **original BMW NBT head unit** (CIC successor, 2013–2016, QNX 6.5 on x86), with no extra
hardware. The phone's picture runs through the NBT's own hardware video path in full screen, you control it with the
iDrive controller, and Google Maps turn-by-turn guidance shows up in the instrument cluster.

> **Status: community prototype.** It works daily in the developer's car (BMW F07, 2014, OnePlus 7 Pro). It has not
> been tested on other cars, other NBT software levels (I-Stufen) or NBT Evo. Read the [disclaimer](#disclaimer) first.

🎬 **Sneak peek:** [see it running in the car (Instagram video)](https://www.instagram.com/reel/DfkW1wYsB__/)

🇩🇪 **Deutsch:** Kurzfassung [weiter unten](#deutsch), Installationsanleitung in [installer/README.md](installer/README.md).

## Features

- **Android Auto over USB**: any Android phone, no phone-specific configuration.
- **Hardware video**: 800x480 in full screen through the stock Cinemo/VideoApp path, about 23 fps at about 27 % CPU.
  Two picture modes: **wide** (full width) and **16:9** (sharper, iDrive split screen stays visible on the right).
- **iDrive controller**: turn = rotate, tilt = D-pad, press = OK, BACK = back. MENU / MEDIA / NAV / COM / OPTION leave
  Android Auto, which keeps running in the background.
- **Menu entry** *Multimedia → Android Auto*: shows the status, starts the service 15 s after the HMI boots and brings a
  running session to the front.
- **Navigation in the instrument cluster** (6WB cluster) while Google Maps guides a route: arrow, distance bar, street
  name, motorway/road shields and lanes (lanes not yet confirmed with a real phone). BMW navigation takes priority when it guides itself.
- **Restart Android Auto** button on the Android Auto page restarts only the service (not the NBT) and saves a
  diagnostic snapshot first.

## Known limitations

- **No Android Auto audio yet.** Music runs over Bluetooth as before, calls work. Plan: [docs/AA-AUDIO-PLAN.md](docs/AA-AUDIO-PLAN.md).
- **800x480 only.** The hardware capture (`dev-nvideocapture`) is limited to WVGA. 720p was tested: not sharper, more
  input lag.
- The controller has a noticeable delay because of the video buffer.
- No map picture in the instrument cluster: the 6WB cluster shows pictures only in its top line (turn arrow, shields).
- In wide mode, switch iDrive split screen off (OPTION → split screen off), otherwise tilting right twice opens it.
- Tested with one car and one phone only.

What changed between versions: [CHANGELOG.md](CHANGELOG.md).

## Requirements

- BMW with the **original NBT** (not NBT Evo, not CIC). NBT Evo uses a different DSI library and is not supported.
- **SSH root access to the NBT** and its root password. The installer does not set this up for you, see
  [Getting SSH access](#getting-ssh-access).
- A laptop connected to the car with an **ENET cable** (the NBT usually answers at `169.254.199.99`).
- A USB cable for the phone.

## Getting SSH access

The stock NBT does not accept SSH logins, so SSH has to be unlocked (patched) once before you can install anything.

- **How it was done on the developer's car:** with **FeatureInstaller**. It can take **several attempts**. The last
  step sometimes reports **errors even though SSH access was enabled**, so try to connect (for example
  `ssh root@169.254.199.99`) before you assume it failed.
- **Root password:** it can be looked up in this forum thread:
  [SSH access guide (supramkv.com)](https://www.supramkv.com/threads/ssh-access-guide-supra-nbtevo-headunit.24190/).
  The thread is about the NBT Evo in the Toyota Supra; the root password is given there.

Unlocking SSH changes the head unit's configuration. As with everything here, you do it at your own risk.

## Install

1. Download the installer from [Releases](../../releases): `NBT-AndroidAuto-Installer.exe` (Windows) or the macOS app.
   Alternatively run the Python version: `pip install "paramiko>=3.4,<4"`, then `python installer/nbt_aa_installer.py`
   with the `payload/` folder from the release next to it.
2. Connect the ENET cable, ignition on, enter IP address and root password, click **Install / Update**.
3. Restart the iDrive.
4. Plug in the phone and choose **Multimedia → Android Auto**.

The installer writes only two folders (`/var/opt/hmi/data/rw/NbtAndroidAutoRT/` and
`/var/opt/hmi/data/rw/Plugins/NbtAndroidAuto/`), verifies every file byte by byte and can uninstall them again. It
never stores your password. Details: [installer/README.md](installer/README.md).

**Unsigned binaries:** the Windows .exe is not code-signed yet, so Windows 11 *Smart App Control* may block it. Use the
Python version in that case; please do not switch off Smart App Control for this. The macOS app is not notarized: open
it with right-click → Open.

## Reporting problems

Use **Save diagnostic logs** in the installer and attach the ZIP to a
[bug report](../../issues/new?template=bug_report.md). Please include car model, build year, NBT software level and
phone model.

## How it works (short)

```
Phone ──USB/AOA──▶ oneplus_native (Android Auto protocol, TLS, H.264)
                     │ annexb.fifo
                     ▼
                   fifo_http ──HTTP──▶ decode_runner ──▶ io-nme / Cinemo NmeDeviceScreen (stock hardware video)
                                                         ▲
nbt_hwvideo ── MOST/DSI (VideoApp D7/D8) ────────────────┘   switches the stock video layer to our stream
nbt_knob    ── watches iDrive controller events (MOST, CCS) ──▶ input.fifo ──▶ Android Auto keys
nbt_cluster ── navigation state from the phone ──▶ instrument cluster (HUDTerminal)
NbtAndroidAuto (HMI plugin) ── menu entry, status page, autostart via boot.sh
```

More: [docs/HW-VIDEO-PATH.md](docs/HW-VIDEO-PATH.md), [docs/CLUSTER-HUDTERMINAL.md](docs/CLUSTER-HUDTERMINAL.md),
[docs/ANDROID-AUTO.md](docs/ANDROID-AUTO.md). Older experiments (PNG pipeline, WAL sprites, first hello-world plugin)
are kept in [docs/history/](docs/history/) for reference.

## Repository layout

| Path | Content |
|---|---|
| `src/android_auto/` | Android Auto side: USB/AOA, TLS, video capture, service, decoder runner, test programs |
| `src/android_auto/knob/` | iDrive controller (`nbt_knob`), hardware video switch (`nbt_hwvideo`), cluster guidance (`nbt_cluster`) |
| `src/idrive/` | HMI plugin (menu entry and Android Auto page) and its resources |
| `installer/` | Installer app (Python/Tk, Windows/macOS/Linux) and payload builder |
| `scripts/` | Build, test and diagnostic scripts (VM and car) |
| `tools/` | SSH session helper for development on the car |
| `patches/` | Small patches to the upstream NBT-AAD core |
| `docs/` | Technical documentation; `docs/history/` = development log |
| `experiments/` | Past experiments, not part of the product (some crashed the head unit), kept for reference |

**Where to start reading the code:** [src/README.md](src/README.md) maps every program on the head unit to its source
files and shows how they work together.

## Contributing

Help is very welcome, especially for **Android Auto audio** and other NBT software levels and cars. Everything can be
built from this repository with one command in a QNX 6.5.0 VM (`scripts/build_all.py`). Start with
[CONTRIBUTING.md](CONTRIBUTING.md) and [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).

## Support and contact

If this project is useful to you, donations are very welcome via **PayPal** to **androidauto.nbt@proton.me**.
The same address is also the place to reach me with questions or suggestions.

## Credits and license

Parts of the Android Auto core and the controller code come from **NBT-AAD** by nanaisu
([Sq00ky/Supra-iDrive6-Android-Auto](https://github.com/Sq00ky/Supra-iDrive6-Android-Auto)). Thank you!

This project is licensed under the [PolyForm Noncommercial License 1.0.0](LICENSE): free for private and other
noncommercial use, **no commercial use** (for example selling it pre-installed). Third-party notices are listed in
[NOTICE.md](NOTICE.md).

Required Notice: Copyright (c) 2026 nanaisu - NBT-AAD (https://github.com/Sq00ky/Supra-iDrive6-Android-Auto)

## Disclaimer

This is an unofficial hobby project. It is not affiliated with, endorsed by or connected to BMW AG, Google LLC or
Harman. BMW, iDrive and Android Auto are trademarks of their respective owners.

You modify your car's head unit **at your own risk**. There is no warranty of any kind; the authors are not liable for
damage to your car, head unit, phone or anything else, or for warranty claims you may lose. During development, wrong
experiments crashed and restarted the head unit; the published package avoids those paths, but it has been tested on
one car only. Do not operate the phone or the system while driving in a way that distracts you; follow your local
traffic laws.

---

## Deutsch

**NBT Android Auto** bringt kabelgebundenes Android Auto auf das **originale BMW NBT** (QNX, x86), ohne Zusatzhardware:
Vollbild über den Hardware-Videoweg, Bedienung mit dem iDrive-Controller, Google-Maps-Abbiegehinweise im Tacho.
So sieht es im Auto aus: [Video auf Instagram](https://www.instagram.com/reel/DfkW1wYsB__/).

- **Voraussetzungen:** originales NBT (kein NBT Evo), SSH-Root-Zugang mit Passwort, ENET-Kabel.
- **SSH freischalten:** SSH muss zuerst einmal gepatcht werden. Beim Entwickler ging das mit dem **FeatureInstaller**.
  Das braucht teils mehrere Anläufe, und der letzte Schritt meldet teils Fehler, obwohl SSH geklappt hat. Also einfach
  die Verbindung testen. Das root-Passwort steht in diesem
  [Forum-Thread](https://www.supramkv.com/threads/ssh-access-guide-supra-nbtevo-headunit.24190/).
- **Installation:** Installer aus den [Releases](../../releases) laden und der Anleitung in
  [installer/README.md](installer/README.md) folgen.
- **Einschränkungen:** noch kein Ton über Android Auto (Musik weiter per Bluetooth), 800x480, getestet mit einem Auto
  (F07, 2014) und einem Telefon.
- **Nutzung auf eigene Gefahr**, keine Gewähr. Lizenz: PolyForm Noncommercial 1.0.0, also keine kommerzielle Nutzung.
- Mitmachen ist ausdrücklich erwünscht: [CONTRIBUTING.md](CONTRIBUTING.md). Issues und Pull Requests gern auf Deutsch
  oder Englisch.
- **Spenden und Kontakt:** Wer das Projekt unterstützen möchte, kann gern per **PayPal** an
  **androidauto.nbt@proton.me** spenden. Über dieselbe Adresse erreichst du mich auch bei Fragen oder Vorschlägen.
