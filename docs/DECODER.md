# The 1280-wide picture: nbt_decoder

Since 2026.11.24.2 the phone's picture no longer goes through the stock player and the 800x480 video capture. The program
`nbt_decoder` from [valinet's fork](https://github.com/valinet/nbt-android-auto) (GPLv3, `src/android_auto/decoder/`)
hands the phone's H.264 to the head unit's NVIDIA video interface directly, so every pixel of the 1280x480 display is a
pixel from the phone. Android Auto and CarPlay use the same decoder and the same window.

The path before it (stock Cinemo player, [HW-VIDEO-PATH.md](HW-VIDEO-PATH.md)) is still in the package and takes over
with one file, see the table below.

## What happens

- **Android Auto:** the phone draws a 1280x720 canvas with its interface in the top 480 rows (inset `0,240,0,0`) at its
  own 30 pictures a second. `nbt_decoder --rows 480` shows those rows 1:1.
- **CarPlay:** the iPhone sends 1280x480 (H.264 High profile); `nbt_carplay` asks it for 30 pictures a second.
- `native_service` starts `nbt_decoder` with the first session and keeps it: across sessions and across restarts of
  the service. A second decoder in the same start of the head unit gets no picture (the window server keeps the first
  stream's window), so the decoder is never ended on purpose.
- **Placeholder stream:** on the developer's car (F07, no rear camera) the first video stream of a boot becomes the
  window server's "driving stream" and never shows. `nbt_decoder` therefore starts a second process (`--dummy`) that
  creates a small hidden stream first.
- **Car stream at 60:** the decoder tells the car 60 pictures a second. At 30, new pictures plus repeated ones came to
  more than 30 hand-overs a second, the hand-over blocked and the picture fell behind the controller.
- **Hardware or software:** with `decoder.hw` the car's own H.264 decoder does the work (hand-over under 1 ms per
  picture on the developer's car). Without it FFmpeg decodes on the CPU (about 25 ms decoding plus 25 ms upload per
  picture, the head unit's CPU is fully loaded). If the car refuses its decoder, `nbt_decoder` falls back to the CPU by
  itself. The installer switches hardware decoding on with the first install; the **Decoder** button on the page
  changes it. The change applies after the next start of the head unit.
- **One kind of phone per start of the head unit:** an iPhone after an Android phone (or the other way round) in the
  same start ended the car's decoder and then the head unit on the developer's car. `native_service` therefore
  remembers which kind had the picture and refuses the other one until the head unit was restarted; the page then shows
  "Restart the NBT first", and **Restart NBT** is right below it.

## Files

In the package folder `/var/opt/hmi/data/rw/NbtAndroidAutoRT` (read by `boot.sh` at the start of the service):

| File | Effect |
|---|---|
| `decoder.off` | The stock player path as before 2026.11.24.2 (800x480, picture modes). CarPlay is off then, too |
| `decoder.hw` | The car's own H.264 decoder instead of the CPU (the **Decoder** button creates and deletes this file) |
| `decoder.args` | One line of extra arguments for `nbt_decoder` |
| `dummy.off` | No placeholder stream |
| `carplay.off` | Apple devices are left alone, see [CARPLAY.md](CARPLAY.md) |

In the runtime folder `/fs/ram3/nbt-aa-native` (RAM): `nbt_decoder.log` (statistics every 5 s), `decoder.notes` (what
the service did with the decoder), `decoder.kind` (the kind of phone that has the picture in this start),
`decoder.hwfail` (the car's decoder gave up, CPU until the next start). The last lines of both logs are copied to the
persistent `logs/` and `journal/` folders once a minute and come with the installer's **Save diagnostic logs**.

## Building

`scripts/build_all.py` builds `nbt_decoder` with the other programs. It needs an FFmpeg checkout at tag `n2.8.22`
(cloned into `.local/` by `scripts/ffmpeg_qnx.py`, H.264 decoder only) and yasm 1.3.0 for the assembly files; without
them `nbt_decoder` is skipped and the rest is built. Details are in the header of `scripts/ffmpeg_qnx.py`.

Test without a car, in the QNX VM: `nbt_decoder --bench FILE.h264` decodes a recorded stream as fast as possible and
prints pictures per second and errors.

## Not taken from the fork

The fork's trip computer box over the picture (`--panel`) is not part of this project's copy of `nbt_decoder`.
