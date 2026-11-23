# Wired CarPlay (exploration)

<!-- SPDX-License-Identifier: GPL-3.0-only -->

Started 2026-11-23. Nothing of this runs in the package; `nbt_carplay_probe` is a test program.

## How wired CarPlay works

The car proves to the iPhone that it is an approved accessory with Apple's authentication chip, over iAP2 (iPod
Accessory Protocol 2); then the iPhone sends its screen (H.264) and sound to the car over AirPlay, through a network
link over the USB cable. Only the car is checked: the iPhone signs nothing.

A car normally switches the USB port to device mode for this (the iPhone becomes the host). The NBT's port is host
only, so the route is the one LIVI and xcertplay (GPL-3.0, <https://github.com/shilapi/xcertplay>) use for Android
head units with the same limitation:

1. A vendor request (0x52, IN, value 0, index 4) makes the iPhone re-connect with an extra USB configuration: the
   Apple USB multiplexor (`ff/fe/02`) plus a CDC NCM network function (`02/0d`).
2. Over the multiplexor: lockdown (the iPhone asks "Trust this computer?" the first time), TLS, then the service
   `com.apple.carkit.service`.
3. iAP2 over that service: identification as a CarPlay car, then authentication with the chip.
4. A network link over NCM (IPv6 link-local), Bonjour, then AirPlay: pairing and encryption, the screen as H.264
   (into `nbt_decoder` and the car's hardware decoder), sound streams, and input (CarPlay supports a rotary knob).

## The car's authentication chip

`dev-i2c` (`/opt/sys/etc/i2c-conf_x86.cfg`) has it on I2C bus 0 as slave `acp`, address 0x20, node
`/dev/i2c/i2c_0/acp`. A plain read returns the registers from 0 (car 2026-11-23):

```
03 01 02 00 00 00 02 00
```

Device version 0x03 = **authentication chip 2.0B** (0x05 = 2.0C, 0x07 = 3.0), firmware 1, protocol 2.0 (RSA-1024 with
SHA-1), device id 0x00000200. CarPlay-era head units and the DIY CarPlay boxes use 2.0C or 3.0; whether an iPhone
accepts a 2.0B chip for CarPlay is not known. The iPhone's answer to step 3 above decides it.

`/dev/ipod-io-media` is the stock iPod stack (io-media), which uses the chip when an iPhone is plugged in.

## nbt_carplay_probe

Source `src/carplay/nbt_carplay_probe.c`, built with the rest (`build.cmd`), output `programs/nbt_carplay_probe`.
Copy it into RAM:

```
%CAR% "cat > /fs/ram3/nbt_carplay_probe && chmod 755 /fs/ram3/nbt_carplay_probe" < build\qnx-build-<newest>\programs\nbt_carplay_probe
```

Over SSH the library path lacks `/lib` (`libusbdi.so.2`): run it with `LD_LIBRARY_PATH=/opt/sys/lib:/lib:/usr/lib`, e.g.

```
%CAR% "LD_LIBRARY_PATH=/opt/sys/lib:/lib:/usr/lib /fs/ram3/nbt_carplay_probe acp"
```

- `nbt_carplay_probe acp` (no iPhone plugged in): the chip's version, its certificate (written to
  `/fs/ram3/acp-cert.der`) and one test signature of a fixed 20-byte challenge.
- `nbt_carplay_probe usb`: waits up to 60 s for an iPhone and prints every USB configuration, interface and endpoint
  it reports. Changes nothing. If every interface is held by another driver, the stock iPod stack has it.
- `nbt_carplay_probe usb carplay`: the same, then sends the vendor request of step 1 and prints what the iPhone
  offers after re-connecting. Unplugging the iPhone brings it back to normal.
- `nbt_carplay_probe lockdown`: the CarPlay configuration (asked for if needed), the USB multiplexor in it, a TCP
  connection to lockdown and QueryType / GetValue (DeviceName, ProductVersion, ProductType). No pairing, no prompt.

## Car results (2026-11-23)

- `acp`: the chip answers when its register number is written first and the read follows after a pause (an immediate
  read gives Input/output error). Certificate 908 bytes (PKCS#7 around an X.509 certificate); a test signature took
  about 1.5 s, 128 bytes (RSA-1024).
- `usb`: the iPhone 15 Pro Max (05ac:12a8, USB 2.10) offers 4 configurations: PTP; "iPod USB Interface" (audio + HID);
  PTP + multiplexor; PTP + multiplexor + Apple USB Ethernet. Nothing else held it (the attach worked).
- `usb carplay`: after the vendor request it re-connects within 1 s with 6: also "PTP + Apple Mobile Device + NCM" (5)
  and "PTP + Apple Mobile Device + Apple USB Ethernet + NCM" (6): the CarPlay configurations, as on LIVI.
- Certificate: issuer "Apple iPod Accessories Certification Authority", subject `IPA_1212AA131029AA04AA0314AA474334`
  (OU Apple iPod Accessories), RSA-1024, valid 2013-10-30 to 2021-10-30 (expired; whether iOS checks the validity of
  accessory certificates is not known). The test signature verifies against its key: PKCS#1 v1.5 with a SHA-1
  DigestInfo around the 20-byte challenge.
- `lockdown`, first try: the configuration was selected, but transfers on the multiplexor's pipes failed with ENOENT
  (opened through interface 0's attachment). The iPhone asked "Trust this computer?" afterwards (accepted). Now the
  pipes go through the multiplexor interface's own attachment.
- Next: `nbt_carplay_probe lockdown`: select the CarPlay configuration, open the multiplexor (bulk 04 / 85), version 2
  handshake, a TCP connection to lockdown (62078) and a few questions that need no pairing.

## Plan

1. `nbt_carplay_probe`: chip I/O and signing, the iPhone's USB configurations, the CarPlay configuration (now).
2. USB multiplexor, lockdown pairing and TLS (Mbed TLS, already in the build), `com.apple.carkit.service`.
3. iAP2 link, identification as CarPlay and authentication with the 2.0B chip: the iPhone accepts or rejects it.
4. Only if 3 is accepted: NCM networking, AirPlay, picture, sound, input.
