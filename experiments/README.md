# Experiments (not part of the product)

Everything in this folder is a **past experiment**. None of it is installed by the installer, and some of it is known
to be dangerous on the car. It is kept because the findings are useful and the code shows how the head unit was
explored. The development log in [docs/history/](../docs/history/) tells the full story.

Paths inside these files still point to their old places (`src/...`, `scripts/...`) and to private folders; expect to
adapt them before building anything here.

| Folder | What it was | Outcome |
|---|---|---|
| `wal-overlay/` | Android Auto picture as a WAL sprite on top of the iDrive HMI (`libwal_nvidia`) | **Do not use.** Worked for seconds, then deadlocked the WalServer / crashed the NVIDIA driver and restarted the head unit. |
| `png-pipeline/` | First live picture: H.264 decoded to PNG frames shown by an HMI image plugin (max. 3 fps), plus test tools | Worked, too slow. Replaced by the hardware video path. |
| `usb-file-overlay/` | A QNX resource manager that pretends to be a video file on the USB stick so the stock player plays the live stream | Worked with a stick, but the car has only one USB port (phone or stick). Abandoned. |
| `hwvideo-bringup/` | Test scripts used to find the stock hardware video path (Cinemo `NmeDeviceScreen`, VideoApp over MOST) | Led to the current solution (`src/android_auto/knob/nbt_hwvideo.cpp`). |
| `hello-plugin/` | The first "Hello NBT" menu entry, used to learn the HMI plugin format | Basis of the current plugin (`src/idrive/aa_entry_plugin.cpp`). |
| `audio/` | Probes for playing sound through the head unit's audio inputs | **Open topic.** Starting point for Android Auto audio, see [docs/AA-AUDIO-PLAN.md](../docs/AA-AUDIO-PLAN.md). |
