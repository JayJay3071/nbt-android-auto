# NBT Android Auto

Wired Android Auto for the **original BMW NBT head unit** (CIC successor, 2013–2016, QNX 6.5 on x86), with no extra
hardware. The phone's picture runs through the NBT's own hardware video path in full screen, you control it with the
iDrive controller, and Google Maps turn-by-turn guidance shows up in the instrument cluster.

> **Status: community prototype.** It works daily in the developer's car (BMW F07, 2014, OnePlus 7 Pro). It has not
> been tested on other cars, other NBT software levels (I-Stufen) or NBT Evo. Read the [disclaimer](#disclaimer) first.

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
- In wide mode, switch iDrive split screen off (OPTION → split screen off), otherwise tilting right twice opens it.
- Tested with one car and one phone only.

## Requirements

- BMW with the **original NBT** (not NBT Evo, not CIC). NBT Evo uses a different DSI library and is not supported.
- **SSH root access to the NBT** and its root password. The installer does not set this up for you.
- A laptop connected to the car with an **ENET cable** (the NBT usually answers at `169.254.199.99`).
- A USB cable for the phone.

## Install

1. Download the installer from [Releases](../../releases): `NBT-AndroidAuto-Installer.exe` (Windows) or the macOS app.
   Alternatively run the Python version: `pip install "paramiko>=3.4,<4"`, then `python installer/nbt_aa_installer.py`
   with the `payload/` folder from the release next to it.
2. Connect the ENET cable, ignition on, enter IP address and root password, click **Install / Update**.
3. Restart the iDrive (hold the power button for about 30 s).
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

## Contributing

Help is very welcome, especially for **Android Auto audio**, other NBT software levels and cars, and a reproducible
build. Start with [CONTRIBUTING.md](CONTRIBUTING.md) and [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).

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

- **Voraussetzungen:** originales NBT (kein NBT Evo), SSH-Root-Zugang mit Passwort, ENET-Kabel.
- **Installation:** Installer aus den [Releases](../../releases) laden und der Anleitung in
  [installer/README.md](installer/README.md) folgen.
- **Einschränkungen:** noch kein Ton über Android Auto (Musik weiter per Bluetooth), 800x480, getestet mit einem Auto
  (F07, 2014) und einem Telefon.
- **Nutzung auf eigene Gefahr**, keine Gewähr. Lizenz: PolyForm Noncommercial 1.0.0, also keine kommerzielle Nutzung.
- Mitmachen ist ausdrücklich erwünscht: [CONTRIBUTING.md](CONTRIBUTING.md). Issues und Pull Requests gern auf Deutsch
  oder Englisch.
