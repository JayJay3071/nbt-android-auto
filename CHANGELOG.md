# Changelog

Versions are named after the build date of the installer payload.

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
