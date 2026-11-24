# Wireless CarPlay and Android Auto

<!-- SPDX-License-Identifier: GPL-3.0-only -->

What was found out on 2026-11-24 in the developer's car (BMW F30, NBT) about running CarPlay and Android Auto without
a cable.

| | State |
|---|---|
| Wireless Android Auto with a wired-to-wireless adapter | works (tested: E-Boda AA30) |
| Wireless CarPlay with a wired-to-wireless adapter | does not work: the adapter needs a USB role switch the NBT cannot do |
| Wireless CarPlay and Android Auto with a CarPlay receiver dongle for Android head units (Carlinkit CPC200-CCPA) | not done yet: the most realistic route, needs a program for the dongle's USB protocol |
| The NBT's own Wi-Fi / Bluetooth | not attempted: needs a 5 GHz access point and our own Bluetooth service in BMW's stack |

## Wired-to-wireless adapters

These adapters (E-Boda, Carlinkit "wired to wireless", Ottocast and similar) plug into the car's USB port and pretend
to be the phone; towards the phone they are a wireless head unit (Bluetooth, own 5 GHz Wi-Fi access point). They do
not pass the session through unchanged: each side has its own encryption.

### Android Auto: works

Tested with the **E-Boda AA30** (2-in-1, about 18 euro). On USB it appears as `18d1:4ee1` "CarPlayBox"; our switch to
accessory mode works and it comes back as `18d1:2d00`, like a phone. Points found on the way:

- **Expired certificate.** The adapter uses two Android Auto certificates, both signed by Google Automotive Link:
  `O=CarService, OU=53`, expired 2022-08-24, for its own screen, and `O=CarService`, valid until 2027-02-17, for the
  phone behind it. Our TLS check refused the expired one ("Certificate verification failed", flags 0x1), the session
  ended and the adapter dropped back. Now the car accepts any phone certificate: the certificate is still logged
  (`[TLS] depth ...` in `capture-N.log`), a failed check only adds "not verified (flags ...), accepted"
  (`src/android_auto/aa_cryptor_mbedtls.cpp`).
- **Its own screen.** Without a phone connected the adapter runs an Android Auto session of its own: a pink screen
  with instructions on how to connect. When the phone connects (or the wireless link to it drops), the adapter leaves
  USB and comes back, then shows the phone (or its own screen again).
- **No USB resets for it.** After a failed session the service resets the phone's USB port (three times, then
  "Reconnect the phone"). That only got in the adapter's way while it connected the phone. Now the reset is skipped
  when the session's device left by itself: gone, back in normal mode or at another address
  (`[native] the device reconnected by itself after session N: no USB reset` in `native.log`). A phone that still
  sits in accessory mode after a failed session is reset as before.
- **Link drops.** Sometimes all data from the adapter stops after a few seconds, the phone's ping messages included
  ("No data from phone for 15 s"): the wireless link between phone and adapter dropped, the adapter shows its own
  screen and reconnects. That is on the phone / adapter side; usual remedies: no battery optimisation for Android
  Auto on the phone, the phone not hopping to another Wi-Fi, the adapter on 5 GHz and up to date.
- From replugging to the picture it is almost as fast as with a cable when the phone connects at once.

The adapter was only borrowed for these tests: it is not used daily and no longer in the developer's possession, so
no further testing with it is possible here. Patches that make its operation smoother are welcome.

### CarPlay: does not work

The same adapter presents itself to the car as an iPhone (`05ac:12a8`, the four USB configurations of a real iPhone).
It accepts our CarPlay request (vendor request 0x52) and reconnects, but with the same four configurations: no
multiplexor + NCM configuration, so `nbt_carplay` stops at "no CarPlay configuration after the request".

Such adapters are built for factory head units, which use the USB role switch: the car asks the iPhone (or the
adapter) to become the USB host and turns its own port into a USB device. The NBT cannot do that (as far as could be
found out):

- `io-usb` runs only the EHCI and OHCI host controllers of the ST ConneXt companion chip (STA2X11, `104a:cc00` and
  `104a:cc01`); the front USB port is root port 1 of the EHCI controller, next to the internal telematics box.
- The chip also has a USB OTG controller (`104a:cc02`, `pci -v` bus 3 function 3), but nothing uses it (its interrupts
  are off), its port most likely has pins of its own, and there is no public documentation and no QNX driver for it.

So a role switch would mean board rework plus a device-mode driver written blind. Our wired CarPlay instead uses the
route where the iPhone stays the USB device ([CARPLAY.md](CARPLAY.md)); these adapters do not offer it.

The adapters contain an Apple authentication chip of their own: towards the iPhone they are the accessory that is
checked. Towards the car they are the "iPhone", which checks nothing.

## CarPlay receiver dongles for Android head units

Dongles such as the Carlinkit / Autokit **CPC200-CCPA** are complete CarPlay and Android Auto receivers (own Apple
chip, Bluetooth, 5 GHz Wi-Fi, wired and wireless) for head units whose USB port is host only, as the NBT's. To the
head unit they are a USB device with their own simple protocol for the picture (H.264), the sound and the input; LIVI
used them in its earlier versions, its code implements the protocol. On the NBT a program for that protocol would feed
`nbt_decoder` and the existing sound, microphone and controller paths. Not started yet (dongle ordered).

## The NBT's own Wi-Fi and Bluetooth

Not attempted. Wireless CarPlay and Android Auto both start over Bluetooth (CarPlay: iAP2 with the authentication
chip; Android Auto: its own channel for the Wi-Fi details), then the phone joins the head unit's 5 GHz access point
(CarPlay also needs Bonjour / mDNS). That would need our own Bluetooth service inside BMW's closed Bluetooth stack next
to its phone and audio profiles, and an access point on the NBT's Wi-Fi chip and driver; whether either is possible
was not checked. xcertplay and LIVI do it this way on Android and Linux boards, where Bluetooth and Wi-Fi are open.

A plain USB Wi-Fi stick through a hub would not help: QNX 6.5 has no driver that runs a 5 GHz access point. A small
Linux board in USB device mode (for example a Radxa Zero 3W) could do the Wi-Fi and Bluetooth part and forward it to
the NBT over a USB network link (CDC NCM, as `nbt_carplay` already uses); the car's own Apple chip would still do the
authentication.

## Logs

- `/fs/ram3/nbt-aa-native/native.log`: the service (sessions, USB resets, "reconnected by itself"), since 2026-11-24;
  also in the installer's diagnostic zip.
- `/fs/ram3/nbt-aa-native/capture-N.log`: each Android Auto session, with the phone's certificate (`[TLS] depth 0`).
- `/fs/ram3/nbt-aa-native/switch.log`: the last switch to accessory mode (`AOA_PROTOCOL_OK identity=...`).
- `/fs/ram3/nbt-aa-native/carplay-N.log`: each CarPlay attempt, with the USB configurations seen.
