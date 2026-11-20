# Changes in this fork

This fork ([valinet/nbt-android-auto](https://github.com/valinet/nbt-android-auto)) builds on upstream
[JayJay3071/nbt-android-auto](https://github.com/JayJay3071/nbt-android-auto) `main` at `12c8411` (2026-11-11). It was
developed and tested on a BMW F30 with NBT. Reverted experiments are not listed; only what the code does now.

**Setup:** a dedicated phone that always stays in the car, connected via USB. Its sound comes over USB into the car
(source `f6`, see Audio below); the 3.5 mm cable to AUX is optional (flag file `media-jack` = the old cable setup).

**Split screen:** I primarily develop and test this with iDrive's split screen on, showing the trip computer. Other
split screen contents, or split screen off, get less testing.

Most behaviour has an off switch: an environment variable read by `nbt_knob`, or a flag file in
`/var/opt/hmi/data/rw/NbtAndroidAutoRT`. Defaults are as described. Commands for building and debugging:
[WORKFLOW.md](WORKFLOW.md).

## Android Auto page (HMI plugin)

The plugin starts with iDrive, can bring Android Auto to the front from anywhere, and holds the picture on one screen.

| Change | What it does |
| --- | --- |
| Menu | Picture mode buttons renamed "Split screen" / "Full width", hint lines removed; mode change works right after boot |
| Restart system | On the Android Auto page: reboots the head unit with `nbt_reset` (UDS 11 01 through the gateway); the separate NbtReboot plugin is gone. From a laptop: `scripts/nbt_reset.py` |
| Start at boot | On/off button (file `start-at-boot.off`); once per boot, brings Android Auto to the front when the first session runs |
| Service start | The Android Auto service starts at once, no 15 s delay; no HMI gate (opt-in file `gate-on`) |
| Menu entry | Leads straight to the picture while a session runs |
| Early start | The plugin calls `PluginApp::start()` itself when iDrive loads it, so its timers run before the page is first opened |
| Show request | Any program creates `/fs/ram3/nbt-aa-show`; within 250 ms the plugin brings Android Auto to the front |
| Parking assistant | Reads iDrive car data `sensors.PDCStatus` (1 = on, 2 = off). While it is on, Android Auto's window is hidden and the hold page does not take the screen back. When it closes and Android Auto was on screen before, `nbt_knob` sends the controller's NAV key (`/fs/ram3/nbt-aa-hmi-nav`), which brings Android Auto back. The controller's keys go from 3002 to the HMI's address (2021 in this car); before the driver has pressed a controller key, `nbt_knob` takes that address from the controller's periodic `E5/0F C06`. iDrive drops the first controller message after its start and any with counter `ff`, so while the controller is silent one `ff` message goes first. |
| BACK fallback | If iDrive's BACK still leaves the page, one focus event brings Android Auto back (1 per s, 10 per minute) |
| Hold screen | One inert hold screen instead of 8 nested ones |
| Focus event limit | iDrive performs each focus event of an app at most 11 times per HMI start (HmiMain3D `CFocusEvent::perform`, "FocusEvent counter > 10"), which made it ignore the hold events until a reboot. The page XML has a pool of 500 focus events (200–699, all to the hold screen, about 5000 refocuses per boot); the plugin uses each 10 times, then the next |
| Night mode | This NBT has no day/night car data, so the headlights stand in: iDrive car data `controls.headlights` goes to `/fs/ram3/nbt-aa-headlights`; the session sends headlights on = night to the phone as Android Auto's night mode, so maps go dark at night |

## Controller and steering wheel (nbt_knob)

`nbt_knob` only listens on MOST, so iDrive always sees every key; the fixes steer iDrive with injected controller
messages instead of blocking keys. The controller tricks act only while a phone session runs and the picture is on
screen.

| Input | Behaviour now |
| --- | --- |
| Tilt right | Goes to the phone; nbt_knob then sends iDrive a BACK, so iDrive's focus returns from the split screen |
| BACK | Goes to the phone; on the press nbt_knob sends iDrive a tilt right, so iDrive's BACK only returns the focus (`NBT_BACK_GUARD`) |
| Tilt left | The same tilt right on the press, plus a BACK on the release (`NBT_TILT_GUARD`) |
| NAV | Brings Android Auto to the front (`NBT_NAV_OPENS_AA`) |
| NAV in Android Auto | Android Auto audio: source not AUX → switch to AUX and start playback if the phone is not playing; source AUX → play / pause (`NBT_NAV_AUDIO`) |
| MENU, COM, MEDIA, OPTION | Hide the picture at once and stay with iDrive |
| Wheel roller | Up = NEXT, down = PREVIOUS, push = PLAY_PAUSE, only while the source is AUX (`NBT_WHEEL_AUX_ONLY`) |
| Roller at start | The first status after start (`ff`) is ignored; only `80` / `7e` count as steps (fixed a one-time song skip at boot) |
| Phone button | Roller and push stay with iDrive's contact list until 4 s without a turn (`NBT_LIST_HOLD_S`) |

## Audio source (MOST ConnectionMaster)

`nbt_knob` reads and switches the car's audio source directly, through the source master at MOST address 100E
(FBlock 03, the ConnectionMaster).

**Android Auto sound over USB (car-tested 2026-11-18):** the phone's media, navigation prompts and system sounds come
over the Android Auto connection and are written into io-amp input `input_stereo_1` (`aa_media_out`, A2I client,
48 kHz stereo, media turned down under prompts, acks once played). That input plays as main source **`f6`**, which the
amplifier builds without a medium; iDrive keeps it, shows a globe with a music note as the source and leaves the steering wheel to us. `f6` is the
Android Auto source: play or NAV in Android Auto switches to it, leaving it pauses the phone, the rocker goes to the
phone only on `f6`. iDrive's AUX stays factory. The file `media-jack` in `/var/opt/hmi/data/rw/NbtAndroidAutoRT` brings
back the cable setup (no media channel, the Android Auto source is AUX, choosing AUX opens Android Auto). Prompts
while another source plays (radio, Bluetooth, AUX, none) go into `input_stereo_3` instead, and `nbt_knob` builds
connection **`b8`** (the browser, interrupting) for them: the main source is muted while the prompt plays, then `b8` is
removed 0.8 s after it (like traffic news). Flag file `prompts-off`: prompts only on Android Auto's source.
Both inputs are written in 4096-byte packages (21 ms, three queued) by writer threads at priority 21 (`NBT_A2I_PRIO`); the writer sleeps until a package is due instead of
polling every millisecond (1024-byte packages are the fallback; env `NBT_A2I_BUF`, `NBT_A2I_PACKAGES`).

**Car microphone (2026-11-20, not yet car-tested here):** when the phone opens the microphone (Google Assistant,
voice search; channel 14, already offered by the NBT-AAD service discovery), `aa_mic` captures from the car's audio
manager through the A2I client: endpoint `/output_multich_0`, 22050 Hz, 4 channels, 16-bit. Channels 0 and 1 are
averaged, interpolated to 16 kHz mono, amplified x4 (`NBT_MIC_GAIN` 1..16) and sent in 16 ms chunks until the phone
closes the microphone. No echo cancellation (the phone turns its own sound down while the Assistant listens).
`NBT_MIC=0` turns it off; `NBT_MIC_ENDPOINT` / `NBT_MIC_RATE` / `NBT_MIC_CHANNELS` select another endpoint. Each
capture ends with an `AA_MIC ... closed` line in the session log with the level (peak and RMS), so silence can be told
from speech. A long press (0.75 s or more) of the steering-wheel voice button starts the Assistant while Android Auto
is on screen (`nbt_knob`, NBT-AAD's code, sends the phone key 84 on release; `native_service` sets
`NBT_PTT_ASSISTANT=1`, `=0` leaves the button to iDrive). A short press stays with BMW's voice control.
From the phone's voice session start to its end (the session marks it with `/fs/ram3/nbt-aa-voice`, patch
`nbt-aad-aa_session-voice-session`), while another source plays (radio, Bluetooth, ...), `nbt_knob` builds connection
`b8` as for a prompt: that source is muted while the Assistant listens, thinks and answers, however long that takes.
A short press (BMW's voice control) while Android Auto's source plays and the phone plays pauses the phone; it plays
again when Android Auto's source, muted by the dialog (state `04` in the 100E table), plays again, or after 4 s if the
dialog never muted it. Leaving Android Auto's source in between drops the resume.
A call made or answered in iDrive (any interrupting connection other than our `b8` that mutes Android Auto's source
while the phone plays: also traffic news or BMW navigation) pauses the phone the same way; it plays again when the
source plays again (`NBT_INTERRUPT_PAUSE=0` off).

| Function | How |
| --- | --- |
| Read the source | Notification registration at 100E (`03/00 fkt 001`); 100E answers at once and on every switch with `fkt C03`: `00 00`, then {id (2 bytes), state} per connection. The source is the first main-source entry (`f6` Android Auto, `bb` AUX, `c7` Bluetooth, `01` radio, ...); an empty table (CD/DVD without a disc) = none. A plain Get is refused |
| Switch the source | `03/00 fkt 053` (BuildHighSyncConnection) to 100E with {request number, connection}; `fkt 054` removes one |
| Leaving Android Auto's source | Sends PLAY_PAUSE (85) to the phone if it is playing (`NBT_AUX_PAUSE`); keys 126 / 127 are not used, they dropped the session |
| Back on it | Sends PLAY_PAUSE again if the pause was ours (`NBT_AUX_RESUME`) |
| Phone starts playing | Switches to Android Auto's source if it is not active (`NBT_PLAY_AUX`) |
| NAV in Android Auto | Switches to Android Auto's source and plays, or play / pause on it (`NBT_NAV_AUDIO`) |
| AUX chosen in iDrive | Cable setup only (`media-jack`): brings Android Auto to the front (`NBT_AUX_OPENS_AA`) |

`NBT_AUX_SOURCE` is Android Auto's source: `f6` by default, `bb` (AUX) with `media-jack`.

The phone's playing / paused state comes from the session: a small NBT-AAD core patch
(`patches/nbt-aad-aa_session-media-state.patch`) writes it to `/fs/ram3/nbt-aa-media` (2 = playing, 3 = paused).

## Connection, USB, service and decoder

- **Decoder audio bug:** `NmeCmdLine -a0` set `Audio/Enable=0` in io-nme's one global option set, so every source
  iDrive started afterwards (Bluetooth, AUX) had no audio until an HMI reboot. The decoder no longer passes `-a0`, and
  saves and restores the options it changes (`nme-options.saved`, cleared at boot).
- **Reconnect:** after each failed attempt the service resets the phone's USB connection; "Reconnect" is asked for
  only after 3 resets.
- **Bluetooth audio while Android Auto runs:** `accessory_window` covers the MTP module `iofs-pfs.so` instead of
  `io-fs-media`, which Bluetooth audio needs.
- **USB reads:** a session's USB read waits 8 s instead of 2 s (the session's own 5 s silence rule decides), and
  failed transfers are logged as `AA_USB_IN/OUT_FAILED`.
- **Phone keys:** the session lets the media keys 85, 87 and 88 through to the phone.
- **Hardware decoding (opt-in):** the flag file `/var/opt/hmi/data/rw/NbtAndroidAutoRT/hwdecode` makes the decoder use
  `NmeCmdLine -wd 1` instead of software decoding, from the next session on. Off by default: on 2026-11-10 it gave
  seconds of delay.

- **Video flow control:** the phone's video frames are acknowledged only once they are written to the decoder FIFO or
  dropped, so the phone stays at most its window of 10 frames ahead of the decoder instead of filling the queue
  (`patches/nbt-aad-aa_session-video-acks.patch`; a frame not taken within `NBT_AA_VIDEO_ACK_FORCE_MS`, default 1500, is
  acked anyway; `NBT_AA_VIDEO_ACKS=0` = ack on arrival as before).
- **Decoder mode (default):** `nbt_decoder` decodes the picture with FFmpeg 2.8 (H.264 only, with its SIMD assembly)
  and shows it through the car's own video library in iDrive's EARLYVIDEO window, instead of fifo_http + Cinemo +
  nbt_hwvideo (Cinemo needs ~45 % CPU for ~28 fps; with the USB sound it fell behind and the picture lagged). Used
  whenever `nbt_decoder` is installed; `decoder.off` = the old path. Full width, or with `split800` 1:1 in an 800x480
  window. 60 fps by default: asked from the phone (`NBT_VID_FPS`, core patch `nbt-aad-sdr_builder-video-fps.patch`)
  and set for the car's stream; `decoder.30fps` = 30. Car: ~7 ms decode, ~13 ms upload per picture. Details, settings, the car's video interface and the FFmpeg build:
  [DECODER.md](DECODER.md).

## Build, tools

- `build_all.py` also builds `nbt_reset` and the test tool `nbt_key`, applies the new core patch, and deletes the VM
  build folder after fetching. The payload ships `nbt_reset`; the installer creates every target folder first.
- `nbt_key`: `--watch`, `back`, `source`, `cm <fkt> <op> <bytes>` (see [WORKFLOW.md](WORKFLOW.md)).
- `scripts/nbt_reset.py`: reboot the head unit from a laptop over ENET.

## Open issues

- Occasional drops mid-session: the phone goes silent for 5 s and the session reconnects. Seen while running on
  battery; cause not yet found.
- A OnePlus 7 Pro on Android 11 never answers the first protocol message; needs the phone's own log.
