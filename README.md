# NBT CarPlay / Android Auto

Wired **Android Auto** and **Apple CarPlay** for the **original BMW NBT** head unit (F-series iDrive, 2013–2016,
QNX 6.5 on x86). No adapter and no extra hardware: the phone plugs into the NBT's own USB port, the picture runs
through the car's video path, the sound plays through the car, and the iDrive controller does the rest.

**Not** for NBT Evo, CIC or later iDrive generations.

> **Status:** works daily in the developer's car (BMW F30 with NBT). Other cars, NBT software levels and phones have
> had little or no testing. Read the [disclaimer](#disclaimer) first.

A fork of [JayJay3071/nbt-android-auto](https://github.com/JayJay3071/nbt-android-auto). What it changes:
[docs/FORK-CHANGES.md](docs/FORK-CHANGES.md); per version: [CHANGELOG.md](CHANGELOG.md).

## Features

| | |
|---|---|
| **Android Auto** | Any Android phone over USB, no phone-specific setup |
| **CarPlay** | Any iPhone over USB, authenticated with the car's own Apple chip ([docs/CARPLAY.md](docs/CARPLAY.md)) |
| **Picture** | Full screen and sharp: the phone draws for the 1280×480 display, shown 1:1, up to 60 fps. Decoded on the CPU or by the car's hardware decoder (iDrive setting). Optional split screen mode: 800×480 next to iDrive's split screen ([docs/DECODER.md](docs/DECODER.md)) |
| **iDrive controller** | Turn, tilt, push and BACK control the phone. MENU, MEDIA, COM and OPTION go to iDrive while the phone keeps running; NAV brings it back |
| **Sound over USB** | Music, navigation and system sounds play through the car as their own audio source. Playback on the phone switches to it; choosing another source pauses the phone |
| **Over the radio** | Navigation prompts and voice assistant answers play over the radio (or any other source), which carries on afterwards |
| **Steering wheel** | Roller = next / previous track, push = play / pause on the phone's source. Long press on the voice button = Google Assistant / Siri, short press = BMW voice control |
| **Car microphone** | The voice assistant hears the car's microphones |
| **Calls** | A call in iDrive pauses the phone's music and resumes it afterwards |
| **Night mode** | Android Auto: maps go dark with the headlights on |
| **Trip computer widget** | A small box over the picture with trip and car data; long NAV switches pages or hides it |
| **Parking assistant** | The phone's screen steps aside for park distance control and comes back when it closes |
| **iDrive page** | *Multimedia → CarPlay / Android Auto*: status, start at boot, picture and decoder settings, widget size, **Restart service**, **Restart system** |
| **Recovers by itself** | Unplug and replug, ignition off and on, service restarts: the session comes back without a reboot |

## Known limitations

- Wired only: no wireless CarPlay or Android Auto.
- Phone calls still go over Bluetooth, as before.
- After changing the picture mode or the decoder, the picture comes back only after a system restart (the iDrive page
  asks for it).

## Requirements

- BMW with the **original NBT**.
- **SSH root access** to the NBT. The stock NBT does not allow it; it has to be unlocked once, for example with
  FeatureInstaller, or yourself through the debug serial port ([docs/SSH-ACCESS-SERIAL.md](docs/SSH-ACCESS-SERIAL.md)).
- A laptop connected with an **ENET cable** (the NBT usually answers at `169.254.199.99`).
- A USB cable for the phone.

## Install

1. Get the installer from [Releases](../../releases) (Windows .exe, macOS app or the Python version).
2. Connect the ENET cable, ignition on, enter the IP address and root password, click **Install / Update**.
3. Restart iDrive.
4. Plug in the phone and open **Multimedia → CarPlay / Android Auto**.

The installer writes only its own two folders, checks every file it copies, never stores the password and can remove
everything again ([installer/README.md](installer/README.md)).

## Reporting problems

Click **Save diagnostic logs** in the installer and attach the zip to a
[bug report](../../issues/new?template=bug_report.md), with car model, build year, NBT software level and phone model.

## Documentation

| Document | Content |
|---|---|
| [docs/CARPLAY.md](docs/CARPLAY.md) | Wired CarPlay on the NBT: USB, authentication chip, iAP2, AirPlay |
| [docs/DECODER.md](docs/DECODER.md) | Picture path, settings files, hardware decoding, trip computer widget |
| [docs/FORK-CHANGES.md](docs/FORK-CHANGES.md) | Controller, steering wheel, audio sources, microphone and iDrive page in detail |
| [docs/ANDROID-AUTO.md](docs/ANDROID-AUTO.md) | Development log of the Android Auto USB and TLS bring-up |
| [src/README.md](src/README.md) | Which program on the head unit comes from which source files |
| [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) | Setting up the QNX 6.5 build VM, working on the car |
| [docs/WORKFLOW.md](docs/WORKFLOW.md) | Everyday build, install and debug commands |
| [CONTRIBUTING.md](CONTRIBUTING.md) | How to contribute |

Everything builds from this repository with one command in a QNX 6.5.0 VM: `python scripts/build_all.py`.

## Credits and license

- Android Auto protocol core and parts of the controller code: **NBT-AAD** by nanaisu
  ([Sq00ky/Supra-iDrive6-Android-Auto](https://github.com/Sq00ky/Supra-iDrive6-Android-Auto)).
- The project this fork is based on: [JayJay3071/nbt-android-auto](https://github.com/JayJay3071/nbt-android-auto).
- The CarPlay USB route follows [xcertplay](https://github.com/shilapi/xcertplay).

Licensed under the [PolyForm Noncommercial License 1.0.0](LICENSE): free for private and other noncommercial use,
**no commercial use**. CarPlay (`nbt_carplay`), the decoder (`nbt_decoder`) and the other files listed in
[NOTICE.md](NOTICE.md#gplv3-parts) are licensed under the [GNU GPL version 3](LICENSE-GPL-3.0.txt). Third-party
notices (Mbed TLS, FFmpeg, lwIP, Monocypher): [NOTICE.md](NOTICE.md).

Required Notice: Copyright (c) 2026 nanaisu - NBT-AAD (https://github.com/Sq00ky/Supra-iDrive6-Android-Auto)

## Disclaimer

An unofficial hobby project, not affiliated with or endorsed by BMW AG, Apple Inc., Google LLC or Harman. BMW, iDrive,
CarPlay and Android Auto are trademarks of their respective owners.

You modify your head unit **at your own risk**, without any warranty. Install and test while parked, and do not let
the phone distract you while driving.
