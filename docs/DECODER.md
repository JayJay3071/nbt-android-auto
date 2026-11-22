# Decoder mode

The default picture path: `nbt_decoder` decodes the phone's H.264 on the CPU with FFmpeg and hands every picture to
the car's NVIDIA video library, which shows it in iDrive's EARLYVIDEO window. It replaces `fifo_http` + Cinemo
(`decode_runner`) + `nbt_hwvideo` for the session. Source: `src/android_auto/decoder/nbt_decoder.c` (its header
has the details below in short form).

Why: Cinemo's software decoder needs about 45 % CPU for about 28 pictures/s on this head unit. With the USB sound
running as well it fell behind, its queue grew and the picture lagged several seconds. FFmpeg with its SIMD assembly
decodes an 800x480 picture in about 7 ms on the car.

## Settings

Used whenever `nbt_decoder` is installed, at 60 fps; no file is needed. Optional files in
`/var/opt/hmi/data/rw/NbtAndroidAutoRT`, read at boot:

| File | Effect |
|---|---|
| `decoder.off` | Do not use the decoder mode: the old picture path (Cinemo). Without it the decoder mode is used whenever `nbt_decoder` is installed (the installer ships it when the build has it) |
| `decoder.30fps` | Ask the phone for 30 pictures/s instead of 60. Default: 60 (`NBT_VID_FPS`, core patch `nbt-aad-sdr_builder-video-fps.patch`, and the car's stream set to 60, `--fps 60`) |
| `split800` | The 800x480 picture 1:1 at the left edge (`--size 800x480`), iDrive's split screen stays visible; otherwise stretched to 1280x480 |
| `decoder.stale` | Milliseconds without a new picture before the window is hidden (default 5000, `0` = never) |
| `decoder.720` | Full screen sharp (test): the phone draws 1280x720 with its interface in the top 480 rows, shown 1:1 (`--rows 480`) instead of 800x480 stretched; 2.4x the pixels to decode on the CPU |
| `decoder.args` | One line of extra arguments for `nbt_decoder` |
| `decoder.delay` | Seconds after iDrive's `MARKER HMIReady` before the first session at boot (default 2) |
| `decoder.showdelay` | Seconds of uptime before Android Auto is brought to the front (start at boot, NAV, AUX; default 40) |
| `decoder.timeout` | `NV_NMSDK_TASK_TIMEOUT_MS` for the video calls (default 3000) |
| `decoder.repeat` | Milliseconds without a new picture before the last one is handed to the car again (default 50, `0` = never) |

Reboot after changing them.

## How it runs

`native_service` starts `nbt_decoder --input annexb.fifo --ready http.ready [--size 800x480] [--fps 60]` once and
keeps it for all following sessions: each session opens the FIFO as writer and closes it at the end. Starting and
killing the program per session froze the whole iDrive display when the phone was plugged in again (car
2026-11-18/19, once "NO SIGNAL" and an iDrive restart).

Inside `nbt_decoder`:

- **Main thread** (inside libKD's `NvKdLaunch`, which the video calls need): reads the FIFO, cuts the Annex B stream
  into access units (a new one starts with AUD, SPS, PPS, SEI or types 14-18 after a slice, or with a slice whose
  `first_mb_in_slice` is 0), decodes them with FFmpeg (one thread, low delay), converts I420 to NV12 and puts the
  picture into a three-picture mailbox; a picture not yet taken is replaced by the newer one. It also runs the window
  policy at least every 100 ms.
- **Display thread:** takes the newest picture and, while the window is shown, uploads it (`VideoFrame`) and presents
  it (`VideoRenderFrame`). With nothing new for 50 ms (`decoder.repeat`) it hands the car the last picture again: the car
  fades the window out when no picture comes for a short while. On still screens the phone sends few pictures
  (YouTube Music ~6/s against ~30/s in Waze), and the picture and iDrive's page alternated about twice a second
  (car 2026-11-19).
- **Window policy:** shown while the Android Auto page is in front (no `/fs/ram3/nbt-aa-hidden`), the session runs
  (`/fs/ram3/nbt-aa-state` says `state=running` and is at most 5 s old; a stop counts after 1.5 s), the session has
  sent its first picture and pictures keep coming (`decoder.stale`). Hiding waits 400 ms so short page changes do not
  blink; position, size, z-order and visibility are set again every 2 s.
- `SIGTERM` hides the window and ends the program with `_exit()`; returning through `NvKdLaunch` crashed in the
  libraries' cleanup.

`nbt_decoder --bench FILE` decodes a recorded stream as fast as possible and prints the rate, without the display.

## The car's video interface

From `/lib/libnmsdk.so`, `/lib/libKD.so` and `/opt/sys/lib/libwal_nvidia.so` (2014). libnmsdk exports
`CNvVideoBase` with full C++ signatures, so the virtual method table and the argument types are readable directly:

| Slot | Method |
|---|---|
| 0 | `Init()` |
| 2 | `VideoOpen(void** handle, ICallback*, INvAvSync*, CConfDecoder*)` |
| 4 | `VideoConfigure(handle, const CConfVideo&)` |
| 5 | `VideoFrame(handle, EVideoFormat, const uint8_t** planes, uint32_t* pitches, int64_t pts, EPictureFormat, void*)` |
| 12, 13 | `SetVideoSource`, `SetVideoDestination(handle, const CRect&)` |
| 14 | `VideoBufferCreate(void** buffer, width, height, float, bool)` |
| 16 | `VideoRenderFrame(buffer, handle, CVideoRenderFrameParams*)` |
| 17 | `VideoStreamCreate(handle, void** buffers, count)` |

Every call is queued to the library's worker thread and waits for it, at most `NV_NMSDK_TASK_TIMEOUT_MS`.

- **`CConfVideo`** (13 words, `OnVideoConfigure`): width, height, two floats, the second being the frame rate,
  the decoder type in word 5 (0 = none, pictures come with `VideoFrame`; 1-2 MPEG-2, 3 H.264, 4-6 VC-1, 7 capture), a
  64-bit value in words 9-10, the surface count (1-20) in word 11 and the stream id in word 12, which becomes stream
  property `0x1789` of `kdCreateStreamNV`. The library allocates that many NV24 surfaces.
- **Frame rate:** `OnVideoRenderFrame` holds each picture for two display refreshes when the configured rate is below
  32, for one otherwise. Configured with 30, the stream can show at most 30 pictures/s whatever the phone sends (car
  2026-11-19 with 60 fps from the phone: every upload took ~31 ms and pictures were dropped); with 60 it shows them all.
- **`VideoFrame`:** `EVideoFormat` 1 = NV12 (2/3 = YV12 with U/V in either order, 4/5 = packed 4:2:2). The library
  takes a free surface and converts the picture into it on the CPU (`NVVAVideoSurfacePutBits`, NV12 to NV24): this is
  the ~13-15 ms "upload". It waits for a free surface, and surfaces come back only once the stream is composited on
  screen, so the program does not upload while its window is hidden.
- **`VideoRenderFrame`:** parameter word 0 = picture structure (3 = frame); surface 0 = the one `VideoFrame` just
  filled. Byte 0x14 is set to 1 (its meaning is not traced).
- **Windows** (`wal::LayerId` in libwal_nvidia): 1024 `EARLYVIDEO`, 1052 `VIDEO`. The stream with id -28 lands in
  EARLYVIDEO (1052 - 28), which iDrive leaves alone while Android Auto is in front. libwal_nvidia's
  `WalHmiInterface` (`setWindowPositionAndSize`, `setZOrder`, `setVisible`) places, raises and shows it.

## Boot

Shown at boot before about 34-40 s, the picture stalls for good: every `VideoFrame` waits the full timeout
(`0x800f0053`), because the window is not composited yet and no surface comes back. Hence:

- with the phone plugged in at boot, the session waits for iDrive's `MARKER HMIReady` in the system log (+`decoder.delay`);
- "start at boot" brings Android Auto to the front only from 40 s of uptime (`decoder.showdelay`); the session itself
  starts as early as before. Since 2026-11-20 every show request (NAV, AUX, after parking) waits for that too; the HMI
  plugin keeps the request and takes it at 40 s.

Plugged in later, nothing waits.

## FFmpeg build

FFmpeg 2.8.22 with only the H.264 decoder, LGPL 2.1 (no GPL or non-free parts). The QNX VM has no bash, perl or yasm,
so FFmpeg's `configure` and assembler cannot run there (details in `scripts/ffmpeg_qnx.py`):

- `configure` ran once on Linux for a 32-bit x86 QNX target; its `config.h` / `config.asm` were adjusted for QNX 6.5
  and GCC 4.4.2 and are kept in `src/android_auto/decoder/ffmpeg/` with the list of needed files and their SHA-256.
- `build_all.py` clones FFmpeg `n2.8.22` once into `.local/`, checks every file, assembles the 22 assembly files with
  yasm 1.3.0 (on Windows the official `yasm-1.3.0-win64.exe`, downloaded once into `.local/`, SHA-256 checked) and
  sends sources and objects to the VM.
- `scripts/build-ffmpeg-qnx.sh` probes the QNX C library for the values configure would have checked, compiles the
  C files with qcc and puts everything into `libavcodec.a` / `libavutil.a`. The libraries are kept in
  `~/.nbt-build/ffmpeg-<key>` in the VM and reused until an input changes (the first build takes a few minutes).

Without the assembly the car decoded a picture in ~10 ms, with it ~7 ms.

## Car results (2026-11-19, 800x480)

| | 30 fps | 60 fps |
|---|---|---|
| Decode | ~7 ms (~10 ms while driving with the map moving) | ~7 ms |
| Upload (`VideoFrame`) | ~13 ms | ~14-15 ms |
| Replaced before upload | 43 of 133,410 pictures in 75 min | 18 of 1,920 in a minute of scrolling |
| CPU (`aps show -v`, 10 s) | | total ~58 %, partition `interaction` ~39 % (budget 33 %) |

## Log

`/fs/ram3/nbt-aa-native/nbt_decoder-<session>.log` (RAM, the session it was started in). Every 5 s, for that interval:

```
stats 5.0 s: 150 decoded (30.0/s), 149 shown (0 repeats), 1 replaced, 0 while hidden, 0 errors; decode 6.8 ms (max 14.1), upload 12.9 ms (max 21.0)
```

```
%CAR% "grep stats /fs/ram3/nbt-aa-native/nbt_decoder-*.log"
```

```
%CAR% "grep -e video: -e window: -e input: -e decoder: /fs/ram3/nbt-aa-native/nbt_decoder-*.log"
```
