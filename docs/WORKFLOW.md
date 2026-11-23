# Everyday workflow (Windows)

The commands used to build, install and debug this fork from a Windows laptop on the ENET cable. Run them in `cmd` in
the repository folder.

## SSH shortcut

Set this once per `cmd` window; every car command below starts with `%CAR%`:

```
set CAR=ssh -oHostKeyAlgorithms=+ssh-rsa -oPubkeyAcceptedAlgorithms=+ssh-rsa -oMACs=+hmac-sha1 -oPreferredAuthentications=password,keyboard-interactive -oPubkeyAuthentication=no -oIdentitiesOnly=yes root@169.254.199.99
```

The default root password is `ts&SK412`. SSH has to be enabled once first: see
[Getting SSH access](../README.md#getting-ssh-access) or the free serial method in
[SSH-ACCESS-SERIAL.md](SSH-ACCESS-SERIAL.md).

The head unit's shell has no `head`: use `sed 3q` instead (`tail` exists). Its `grep` has no `-A` / `-B`.

## Apply a patch, build, install

1. Apply a patch (made with `git format-patch`):
```
git am "%USERPROFILE%\Downloads\<patch file>.patch"
```

2. Build (QNX build VM):
```
python scripts\build_all.py
```

3. Package, with the newest build folder name in place of `<newest>`:
```
python installer\make_payload.py --built build\qnx-build-<newest> --pem-dir .local\pem
```

4. Install:
```
python installer\nbt_aa_installer.py --cli install
```
Files already on the head unit with the same content are not written again (`same` in the output). To write
everything anyway, add `--force`.

5. Reboot the head unit: Multimedia → CarPlay / Android Auto → **Restart system**, or from the laptop:
```
python scripts\nbt_reset.py
```

Undo the last patch before it is pushed: `git reset --hard HEAD~1`. Undo a pushed commit: `git revert <commit>`.

## Logs on the car

The logs live in RAM (`/fs/ram3`): a reboot clears them, so read them before rebooting.

| What | Command |
| --- | --- |
| Service state and session number | `%CAR% "cat /fs/ram3/nbt-aa-state"` |
| HMI plugin (page focus, show requests, parking assistant) | `%CAR% "tail -30 /fs/ram3/nbt-aa-plugin.log"` |
| Controller, steering wheel, audio source | `%CAR% "tail -30 /fs/ram3/nbt-aa-native/knob.log"` |
| Session logs, newest first | `%CAR% "ls -lt /fs/ram3/nbt-aa-native/capture-*.log"` |
| How sessions ended | `%CAR% "grep -h -e AA_USB -e 'No data' -e 'closed socket' /fs/ram3/nbt-aa-native/capture-*.log"` |
| System log | `%CAR% "sloginfo \| tail -40"` |

## CPU load

CPU use per adaptive partition (QNX APS) over the last 0.1 s, 1 s and 10 s:

```
%CAR% "aps show -v"
```

It groups processes, it does not name them: the HMI and everything this project starts (spawned from the HMI plugin) run
in `interaction`, system services such as `io-nme` (the decoder) most likely in `System`. Compare the **Total** of the
10 s column, taken in the same situation (same screen, video running, phone connected for a minute), for example with the
hardware decoder on and off.

## MOST trace

60 s of all MOST messages the head unit receives, then copied to the laptop:

```
%CAR% "cd /fs/ram3/nbt-aa-native && LD_LIBRARY_PATH=/opt/sys/lib:/lib:/usr/lib ./nbt_knob --trace --seconds 60 --fifo /fs/ram3/nbt-trace-dummy.fifo > /fs/ram3/trace.txt 2>&1"
```
```
%CAR% "cat /fs/ram3/trace.txt" > trace.txt
```

## Hardware decoder

Off by default (software decoding). Applies from the next session: replug the phone or use **Restart service**.

| What | Command |
| --- | --- |
| Hardware decoder on | `%CAR% "touch /var/opt/hmi/data/rw/NbtAndroidAutoRT/hwdecode"` |
| Hardware decoder off | `%CAR% "rm -f /var/opt/hmi/data/rw/NbtAndroidAutoRT/hwdecode"` |
| Which one runs | `%CAR% "grep -h 'decoder:' /fs/ram3/nbt-aa-native/decoder-*.log"` |

## Android Auto sound: USB or cable

By default the phone's sound comes over USB into the car (source `f6`). To use the 3.5 mm cable on AUX instead:

| What | Command |
| --- | --- |
| Cable (AUX) | `%CAR% "touch /var/opt/hmi/data/rw/NbtAndroidAutoRT/media-jack"` |
| USB (default) | `%CAR% "rm -f /var/opt/hmi/data/rw/NbtAndroidAutoRT/media-jack"` |
| What the session does with the sound | `%CAR% "grep -h AA_MEDIA_OUT /fs/ram3/nbt-aa-native/capture-*.log"` |

Applies after **Restart service** (the service reads the file when it starts).

Navigation prompts while another source plays (radio, ...) cut in over connection `b8`. To turn that off (prompts only
on Android Auto's own source): `%CAR% "touch /var/opt/hmi/data/rw/NbtAndroidAutoRT/prompts-off"`. What happened:
`%CAR% "grep -e Prompt -e 'build handle' -e 'remove handle' /fs/ram3/nbt-aa-native/knob.log"` and
`%CAR% "grep -h -e prompt -e b8 /fs/ram3/nbt-aa-native/capture-*.log"`.

## Test tool nbt_key

Copy it to the car after a build (it is not part of the installer payload):

```
%CAR% "cat > /fs/ram3/nbt_key && chmod 755 /fs/ram3/nbt_key" < build\qnx-build-<newest>\programs\nbt_key
```

Read the current audio source:

```
%CAR% "LD_LIBRARY_PATH=/opt/sys/lib:/lib:/usr/lib /fs/ram3/nbt_key source 5"
```

Beep test (`nbt_a2i_beep`, also only in the build folder): K short beeps into one io-amp input, audible only while
the amplifier has a connection to that input:

```
%CAR% "cat > /fs/ram3/nbt_a2i_beep && chmod 755 /fs/ram3/nbt_a2i_beep" < build\qnx-build-<newest>\programs\nbt_a2i_beep
```
```
%CAR% "LD_LIBRARY_PATH=/opt/sys/lib:/lib:/usr/lib /fs/ram3/nbt_a2i_beep input_stereo_3 2"
```

Other modes: `--watch [s]` (print controller messages), `back [s]` (BACK after a tilt right),
`cm <fkt> <op> <bytes>` (send one ConnectionMaster message to 100E): `cm 053 6 00 10 00 bb` switches to AUX,
`cm 053 6 00 11 00 b8` builds the browser connection (main source muted, `input_stereo_3` audible), `cm 054 6 00 12 00 b8`
removes it again. The second pair of bytes is a request number of your choice; change it each time.
