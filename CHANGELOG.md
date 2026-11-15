# Changelog

Versions are named after the build date of the installer payload.

## Unreleased

Built with the QNX 6.5 GCC 4.4.2 toolchain; the audio route and the A2I output were tested on one car (BMW F07 2014,
original HU_NBT). Not part of a released installer payload yet.

- **Android Auto sound over USB, no Bluetooth needed** for music, navigation voice and system sounds. The original
  NBT has no `libasound`, so the NBT-AAD browser-audio mixer now writes to the A2I endpoint `input_stereo_3`
  (`src/android_auto/audio_a2i`). The new `audioconn` (`nbt_audioconn`) asks the audio ConnectionMaster
  (FBlock 0x03) for connection 0xb8, the request the iDrive HMI itself sends (decoded from `HmiMain3D`), and removes
  it again after silence and after every session. On by default; falls back to Bluetooth if the route is refused or
  `liba2i_client.so` is missing; `touch .../NbtAndroidAutoRT/bt-audio` forces Bluetooth.
  Details, findings and rollback: `docs/AA-AUDIO-PLAN.md`.

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
