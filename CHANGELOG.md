# Changelog

Versions are named after the build date of the installer payload.

## 2026.11.26 (pre-release)

Driven in the developer's car on 2026-11-26 with the iPhone (CarPlay and plain Bluetooth sound) and with the Android
phone (Android Auto). The change to the first pairing of an iPhone (below) was built after the drive and has not run
on a car.

- **Fix: the whole head unit restarted when a CarPlay session ended.** `nbt_carplay` gave every USB transfer to the
  head unit's USB stack (`io-usb`) with a time limit and ended without finishing the transfers that were still
  running. The stack then at times kept the timer of a transfer whose memory it had already released, crashed on its
  next timer tick (crash dump `/var/dump/io-usb.core.gz`), and the head unit restarted. Now no transfer has a time
  limit inside the stack, the waiting is done in `nbt_carplay`, and everything still running is ended before the
  program disconnects (`src/carplay/cp_usb.c`).
- **Sound: the order of Bluetooth and USB no longer matters.** The head unit asks only once per Bluetooth connection
  where the sound comes from. That place is now always this project's sound device: while no phone session runs it
  hands on the phone's own Bluetooth sound (and the phone's own titles are shown), with a session it delivers the
  session's sound. So a phone that is only connected by Bluetooth plays as usual, the CarPlay sound comes by itself
  when the cable is plugged in, and the Bluetooth sound comes back after unplugging. Switching Bluetooth off and on
  is no longer needed (`nbt_aa_wms`, `nbt_ipod_tap`, `nbt-aa-iap.sh`).
- **Fix: no sound for the rest of a Bluetooth connection.** The head unit's player reads the sound device once right
  after opening it and never again if that read fails; the device answered "nothing there" after one second while the
  phone was not playing yet. A read now waits until sound is there.
- **CarPlay, first pairing of an iPhone (not car-tested yet):** the two RSA-2048 keys of the pair record are made
  once and stored at once, and so are the two identifiers the head unit shows the iPhone. Until now they were made
  again at every attempt (6 to 56 s per key on the head unit) and only stored after "Trust". An iPhone that dropped
  off USB during those seconds never got to the trust question
  ([#21](https://github.com/JayJay3071/nbt-android-auto/issues/21): more than 70 attempts). This is probably also why
  a first connection took 2 to 3 minutes. An iPhone that is already paired is not affected
  (`src/carplay/cp_lockdown.c`).
- **CarPlay, instrument cluster:** roundabouts are drawn with their exit (from the exit angle the iPhone sends, other
  exits as stubs), the lane guidance of Apple Maps is shown, and "proceed to the route" has its own state.
- **CarPlay sound:** lost sound packets are counted in the log.
- **Diagnostics:** the USB side of each session start and end (bus errors, unplug) is written to
  `logs/usb-events.txt` and saved with the logs.
- **Good to know: iPhone hotspot.** With the personal hotspot on, the iPhone in the developer's car kept dropping off
  USB every minute or so (the head unit's USB controller reports transfer errors on all endpoints, then the iPhone
  re-connects). With the hotspot off and low power mode on it ran a whole drive without a drop. The suspected reason,
  not measured: the phone then needs more power than the head unit's USB port delivers.
- **Licenses:** the eight files with parts from valinet's fork (the service, the page, the start and build scripts)
  are now GPL-3.0-only too; Valentin Radu agreed on 2026-11-25. In 2026.11.24.2 they were still under the PolyForm
  Noncommercial License. See [NOTICE.md](NOTICE.md).
- **CarPlay, first connection:** the README and docs/CARPLAY.md now say that the first connection of an iPhone takes
  2 to 3 minutes and ends with the iPhone's trust question; later connections take about 10 to 20 seconds.
- **Correction:** the "about 98 % CPU with CarPlay" in the README and in docs/CARPLAY.md of 2026.11.24.2 was one short
  sample taken during testing and does not hold. The load has not been measured properly yet; see the README's known
  limitations.

## 2026.11.24.2 (pre-release)

Wired CarPlay, a picture that uses every pixel of the display, and restart buttons. Most of what is new is the work
of **Valentin Radu** ([valinet's fork](https://github.com/valinet/nbt-android-auto)): the decoder `nbt_decoder` with
hardware decoding and the sharp full-width picture, the CarPlay program `nbt_carplay` with Siri, calls and the car's
microphone, the reset tool `nbt_reset`, and the way the service and the page use them. He also allowed this project
to ship them in its releases. Thank you, Valentin!

Everything in this release was tested in the developer's car (BMW F07 2014, OnePlus 7 Pro, iPhone 17 Air with
iOS 27), except three small things that were added afterwards and the installer:

- the lock against changing between an Android phone and an iPhone in one start of the head unit, with its text on
  the page;
- `nbt_decoder` rebuilt without the fork's trip computer box (decoding checked in the QNX VM);
- the frame rates as built-in defaults (the same values ran on the car, there set by two files).

The installer of this version was tested in the QNX VM (install, update, logs, uninstall in a test folder), not yet on
a car; its gateway search only against a stand-in.

**Update of 25 November:** this version was then installed with its installer in the developer's car and driven,
Android Auto and CarPlay for about an hour each, without problems. The three additions above, the lock between
Android phone and iPhone among them, were tested with it.

- **Wired CarPlay** ([docs/CARPLAY.md](docs/CARPLAY.md)): plug in an iPhone and choose
  *Multimedia → Android Auto / CarPlay*. Picture, iDrive controller, sound, Siri and calls with the car microphone,
  Apple Maps turn hints in the instrument cluster. No adapter: the NBT's own USB port and Apple chip are used.
  - **Sound like Android Auto's:** the CarPlay sound plays through the same Apple entry under Multimedia, with title,
    artist, album, cover and progress, also in the instrument cluster; skip works from iDrive, the steering wheel and
    the radio panel keys. For this the iPhone is paired by Bluetooth with **only "Audio"** enabled (Telephone 1 / 2
    off) and CarPlay is picked as the audio output on the iPhone.
  - Changes for this car and iOS 27: 16-bit NCM network format, sound into this project's sound path, titles and
    cover from iAP2, route guidance for the cluster.
- **Sharp picture, 1280 pixels wide** ([docs/DECODER.md](docs/DECODER.md)): `nbt_decoder` hands the phone's H.264 to the
  head unit's video interface directly. Every display pixel is a phone pixel (before: 800 pixels stretched to 1280),
  the phone's own 30 pictures a second, and the controller reacts faster. The picture always uses the whole display;
  the two picture modes are gone from the page.
  - **Decoder button** on the page: hardware (the car's own H.264 decoder, default after a first install) or software
    (CPU). Applies after a restart of the head unit.
  - The stock player path is still there: `touch decoder.off` in the package folder.
  - The car stream runs at 60 so that repeated pictures do not block the hand-over (was a `decoder.args` setting on
    the test car, now the default).
- **Restart NBT** button on the page (`nbt_reset`): restarts the whole head unit with a diagnostic reset through the
  car's gateway, after a confirmation. The installer looks up the gateway address and stores it (not yet tested on a
  car; without it the page says "Gateway not reachable").
- **One kind of phone per start of the head unit** (not yet tested on a car): changing between an Android phone and an
  iPhone in the same start ended the decoder and then the head unit on the test car. The service now refuses the
  second kind and the page says "Restart the NBT first".
- **USB pauses:** a session's USB reads wait 8 s instead of 2 s. Before, a read that gave up after 2 s ended the
  session before the service's own rule (5 s of silence) could apply. Failed transfers are logged
  (`AA_USB_IN_FAILED` / `AA_USB_OUT_FAILED`). Same idea as in valinet's fork; suggested by a user by mail.
- **Sound delay fixed:** the sound device now delivers 44.1 kHz. The head unit's player treated the 48 kHz stream as
  44.1 kHz, played 8 % too slow and fell behind by seconds.
- **Menu entry:** renamed to "Android Auto / CarPlay". iDrive performs each focus event only about ten times per
  start, which very likely is why the entry stopped opening Android Auto after a while on long drives; the page now
  has a pool of 400 events. Every id in the page file is unique (a doubled id made a button dead).
- **Installer:** ships `nbt_decoder`, `nbt_carplay` and `nbt_reset`, switches hardware decoding on with the first
  install, stores the gateway address, and the diagnostic logs include the decoder and CarPlay logs.
- Removed from the fork's decoder: the trip computer box over the picture (not yet tested on a car without it).

- **Licenses:** the code written for this project is now under the GNU GPL version 3 (`GPL-3.0-only`, was PolyForm
  Noncommercial), with a permission to combine it with NBT-AAD and the head unit's libraries. The NBT-AAD parts stay
  PolyForm Noncommercial, and so do the binary releases: no commercial use. Valentin Radu's programs are GPL-3.0-only
  in source and part of the binary releases under his permission of 2026-11-24. Everything is listed in
  [NOTICE.md](NOTICE.md) and [LICENSE.md](LICENSE.md).

Known limits: see [README.md](README.md#known-limitations).

## 2026.11.24 (pre-release)

The programs run in the developer's car (BMW F07 2014, OnePlus 7 Pro); built from this repository they are identical
to the ones on the car (checked 2026-11-24 with `scripts/build_all.py`). The installer itself was tested in the QNX VM
only, not yet on a car.

- **Installer:** installs the sound as
  "iPod over Bluetooth" (`nbt_aa_wms`, `nbt_ipod_tap`, `nbt_aa_ipod`, `iap.sh`) and the skip key map `mfl.map`;
  every install / update switches the sound mode on (removes `iap.off`); uninstall also stops the sound helpers;
  diagnostic logs include `iap.log`, `btipod.log`, `sound.log`. Plugin XML is written with LF line ends (as the HMI
  needs).

- **Android Auto sound through the car speakers, without Bluetooth audio from the phone:**
  - `nbt_a2i_play` plays the Android Auto media sound into the head unit's amplifier (A2I); 8 channels, the stock
    music player is paused while it plays (`sound.args`, `sound.nostop`).
  - **"iPod over Bluetooth" mode** (2026-11-24): the phone appears in iDrive as an Apple device "Android Auto" with
    title, artist, album, cover and length, also in the instrument cluster; skip works from iDrive and the steering
    wheel. Parts: `nbt_ipod_tap` (filter in front of the Bluetooth audio driver), `nbt_aa_ipod` (Apple driver stand-in),
    `nbt_aa_wms` (own SBC sound device), started by `scripts/nbt-aa-iap.sh`. The phone has to connect by Bluetooth
    while the service is already running (otherwise switch Bluetooth off and on once). Off: `touch $P/iap.off`.
  - Fixed: the music collection / media tab lost its sound because our video player ran with `-a0` in the shared
    media engine.
- **Fewer picture stalls and restarts:** video flow control and a frame-rate limit (NBT-AAD patch 0003): the phone never
  sends faster than the stock player shows; `fifo_http` waits longer for a slow player; a stalled player only restarts
  the video chain, not the session.
- **Keys:** radio panel skip keys control Android Auto media (`config/mfl.map`); media keys declared to the phone.
- **Menu entry:** more reliable focus handling (one hold event per level, fallback to the next level), accidental
  presses right after the page appears are ignored.
- **Diagnostics:** every service run keeps its logs in `$P/journal/` (newest 60), `$P/sessions.log` records why each
  session ended; `scripts/car-journal-collect.sh` fetches them in one go.
- Build: NBT-AAD patches numbered `patches/000N-*.patch`; `scripts/build_all.py` builds the new programs too.
- Past trials (iPod in the snap-in over USB, probes) moved to `experiments/ipod-usb-trials/`.

## 2026.11.10.2

Tested on the car (BMW F07 2014, OnePlus 7 Pro), including install with the installer.

- **Fewer drop-outs:** picking the "exit" icon in the Android Auto rail no longer ends the session ("connected but
  nothing works"); the projection is handed back to the phone after half a second.
- **Clean session end:** before stopping, the service now says goodbye to the phone (ByeBye). After a service restart
  the phone no longer stays stuck until Android Auto is force-stopped.
- **No false "replug the phone":** a session that ran for at least a minute no longer counts as a failed attempt.
- **More reliable menu entry:** the plugin sends at most one hold command per second (two commands 50 ms apart in the
  middle of an iDrive page animation were ignored).
- **Cluster:** smoother road shields and digits (anti-aliased), distances like "5.4" show correctly (the cluster font
  has no comma), the distance bar appears only below 1 km.
- **Diagnostics:** the restart snapshot now includes the session, decoder and video logs of the current boot.
- Fix: updating `nbt_cluster` while it was running failed ("Resource busy").
- Investigated and documented, not shipped: hardware decoding (`-wd 1`, more delay) and a map picture in the cluster
  (the 6WB cluster shows bitmaps only in the top line), see `docs/CLUSTER-HUDTERMINAL.md`.

## 2026.11.09

First release candidate: wired Android Auto with hardware video, iDrive controller, menu entry with autostart, cluster
turn-by-turn guidance, installer for Windows and Python.
