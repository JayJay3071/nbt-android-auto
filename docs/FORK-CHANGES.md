# Changes in this fork

This fork ([valinet/nbt-android-auto](https://github.com/valinet/nbt-android-auto)) builds on upstream
[JayJay3071/nbt-android-auto](https://github.com/JayJay3071/nbt-android-auto) `main` at `12c8411` (2026-11-11). It was
developed and tested on a BMW F30 with NBT. Reverted experiments are not listed; only what the code does now.

**Setup:** in my setup, the phone is connected via USB, and its audio goes from the phone's 3.5 mm jack to the car's AUX
port. The entire solution is built around this; change it according to your needs (the `NBT_*` switches below).

**Android Auto audio:** not fixed in this fork either; Android Auto still sends no audio to the car. I use a dedicated
phone that always stays in the car, always connected to it, so its 3.5 mm jack on AUX is all I need.

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
| Parking assistant | Reads iDrive car data `sensors.PDCStatus` (1 = on, 2 = off); Android Auto comes back when it closes, if it was on screen before |
| BACK fallback | If iDrive's BACK still leaves the page, one focus event brings Android Auto back (1 per s, 10 per minute) |
| Hold screen | One inert hold screen instead of 8 nested ones |
| Focus event limit | iDrive performs each focus event of an app at most 11 times per HMI start (HmiMain3D `CFocusEvent::perform`, "FocusEvent counter > 10"), which made it ignore the hold events until a reboot. The page XML has a pool of 100 focus events (200–299, all to the hold screen); the plugin uses each 10 times, then the next |
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

**Android Auto sound over USB (2026-11-18, being tested):** the phone's media, navigation prompts and system sounds come
over the Android Auto connection and are written into io-amp input `input_stereo_1` (`aa_media_out`, A2I client,
48 kHz stereo, media turned down under prompts, acks once played). That input plays as main source **`f6`**, which the
amplifier builds without a medium; iDrive keeps it, shows no player and leaves the steering wheel to us. `f6` is the
Android Auto source: play or NAV in Android Auto switches to it, leaving it pauses the phone, the rocker goes to the
phone only on `f6`. iDrive's AUX stays factory. The file `media-jack` in `/var/opt/hmi/data/rw/NbtAndroidAutoRT` brings
back the cable setup (no media channel, the Android Auto source is AUX, choosing AUX opens Android Auto). Prompts
while another source plays (radio) are not audible yet (next step: connection `b8` + `input_stereo_3`).

| Function | How |
| --- | --- |
| Read the source | Notification registration at 100E (`03/00 fkt 001`); 100E answers at once and on every switch with `fkt C03`, byte 3 = source (`bb` AUX, `c7` Bluetooth, `01` radio). A plain Get is refused |
| Switch the source | `03/00 fkt 053` StartResultAck to 100E with {request number, `00`, source}; `00 xx 00 bb` switches to AUX |
| Leaving AUX | Sends PLAY_PAUSE (85) to the phone if it is playing (`NBT_AUX_PAUSE`); keys 126 / 127 are not used, they dropped the session |
| Back to AUX | Sends PLAY_PAUSE again if the pause was ours (`NBT_AUX_RESUME`) |
| Phone starts playing | Switches the source to AUX if it is not AUX (`NBT_PLAY_AUX`) |
| AUX chosen in iDrive | Brings Android Auto to the front (`NBT_AUX_OPENS_AA`) |

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

## Build, tools

- `build_all.py` also builds `nbt_reset` and the test tool `nbt_key`, applies the new core patch, and deletes the VM
  build folder after fetching. The payload ships `nbt_reset`; the installer creates every target folder first.
- `nbt_key`: `--watch`, `back`, `source`, `cm <fkt> <op> <bytes>` (see [WORKFLOW.md](WORKFLOW.md)).
- `scripts/nbt_reset.py`: reboot the head unit from a laptop over ENET.

## Open issues

- Occasional drops mid-session: the phone goes silent for 5 s and the session reconnects. Seen while running on
  battery; cause not yet found.
- A OnePlus 7 Pro on Android 11 never answers the first protocol message; needs the phone's own log.
