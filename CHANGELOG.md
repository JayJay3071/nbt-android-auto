# Changelog

Versions are named after the build date of the installer payload.

## Unreleased

Not built in the VM or tested on the car yet.

- **Experimental Android Auto audio (off by default):** with the flag file `NbtAndroidAutoRT/aa-audio`, the phone
  sends media, guidance and system sounds over Android Auto, and `oneplus_native` plays them through the car's browser
  audio front end (NBT-AAD browser mode). The car still has to be told to play that source; that part is open, see
  `docs/AA-AUDIO-PLAN.md`. Without the flag nothing changes.
- New read-only diagnostic `scripts/car-audio-route-probe.sh` (how the car's audio routing can be reached).
- **Video queue:** a key frame after a drop run now discards all older queued pictures instead of only as many as
  needed to fit, so the decoder does not first play seconds of old video. New `AA_LIVE_QUEUE_STATS` line every 10 s
  in the session log (peak frames / KB / wait). Optional cap `NBT_AA_QUEUE_FRAMES` (off by default): the byte budget
  alone allows about 384 padded frames (13-17 s).
- **nbt_hwvideo:** follows VideoApp to new MOST addresses when the registry reports a change, and logs when VideoApp
  stops answering the video source query (logged only, no automatic rebuild).
- Fix: `scripts/Test-AndroidAutoSession.py` now applies `patches/*.patch` (the VM test had not compiled since the
  ByeBye change).

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
