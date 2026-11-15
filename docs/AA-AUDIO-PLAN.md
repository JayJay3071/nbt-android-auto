# Android-Auto-Ton ohne Bluetooth – Stand 7.11.2026

## Befunde
- **Pieptest über A2I** (`nbt_a2i_beep`, Quelle und Skripte in `experiments/audio/`):
  - In `input_stereo_1` bis `_5` wurde geschrieben, aber nichts war hörbar.
  - `input_stereo_0` blieb beim Öffnen hängen, weil er belegt ist (BT/Media).
  - Ergebnis: Ein Eingang ist nur hörbar, wenn die zugehörige Quelle beim Audio-Management aktiv ist.
- **Cinemo `NmeCmdLine`** spielt keine WAV-Dateien ab (0 Samples, auch mit `-n`).
- **Audio-Architektur des NBT:**
  - Der Verstärker / ConnectionMaster ist extern: FBlock **03** unter Adresse **0x100E**.
  - Die HMI baut dort Verbindungen auf (`CPxyConnectionMaster::BuildHighSyncConnection`, `RemoveHighSyncConnection`).
  - Die Antworten an die HMI sind fkt **053** op=D (ResultAck `00 02`, `00 03`, …) und Status fkt **C03** (Verbindungstabelle,
    z. B. `00 00 00 01 00`, `00 00 00 be 00`).
  - Die Anfrage selbst ist im Mitschnitt nicht zu sehen, weil sie an ein externes Gerät geht.
  - Belege: `build/audio-trace-20261107/atrace.log` (Quellwechsel per MODE-Taste: FBlock 15/02 fkt 513) und
    `build/video-trace-20261107/trace.log`.
- **Browser-Audio wie in der Referenz (NBT Evo):**
  - `/opt/sys/bin/dev-libasound -x input_stereo_3` (Connectivity.cfg) und der Treiber `/opt/mm/lib/libasound/libasound.so` sind
    vorhanden, ebenso das Paket `CONN_BROWSER_AUDIO`.
  - `audioconn` fehlt auf diesem NBT.
  - HMI-Strings: `CPxyBrowser::CSourceActivity`, `EBrowserAudioSourceType`, `getConnIDForAudioSource`.
- **Telefonie** läuft über das TCB (`devu-cdc_tcm-io_amp`, `play_dev=/input_mono_3`). **Bluetooth-A2DP** (Media → Bluetooth)
  funktioniert grundsätzlich, beim OnePlus aber unzuverlässig.

## Plan
1. **Offline:**
   - Den Serialisierer von `CPxyConnectionMaster::CBuildHighSyncConnection` in HmiMain3D lesen: Funktions-ID (vermutlich 053),
     Nutzdaten (Handle, Verbindungs-ID, Senke).
   - Die Verbindungs-ID der Browser-Quelle bestimmen (Referenz: 0xb8; die Tabelle `EAudioConnectionIDs` in HmiMain3D).
2. **AA-Seite:**
   - `NBT_SDR_DROP` ohne Kanal 9 (Media-Senke), Kanal 8 (Ansagen) behalten.
   - PCM aus `setAudioFrameCallback` in einen Ringpuffer.
3. **Ausgabe:**
   - PCM über `libasound.so` (`snd_pcm_open_preferred`, 16 Bit Stereo, 44,1 kHz laut Referenz; Resampler 48 → 44,1 kHz aus der
     Referenz) in `input_stereo_3`.
4. **Route:**
   - Beim ersten Ton `BuildHighSyncConnection(Browser-ID)` an 0x100E, nach Stille `Remove`. Das läuft über die Sendefunktion
     von nbt_knob/nbt_hwvideo (`Client::send`).
5. **Am Auto testen** mit dem Pieptest über libasound und gebauter Verbindung, danach mit AA-Musik. Jeder Sendetest nur mit
   Freigabe.

## Offline-Analyse HmiMain3D (15.11.2026, nichts gesendet)

Quelle: `HmiMain3D` aus dem eigenen NBT-Firmwarestand (nicht im Repository). Adressen unten sind VAs in dieser Datei.

- **Proxy ConnectionMaster (FBlock 0x03)**, Konstruktor `0x8b838fc`, Fkt-Registrierung `0x8b8361c`:
  `0x000, 0x001, 0x002, 0x053 (+0x6cc, CBuildHighSyncConnection), 0x054 (+0x764, Remove), 0x402, 0xC03 (Tabelle),
  0xC05, 0xC20`.
- **BuildHighSyncConnection-Anfrage** (`0x8b832e6`): FBlock 0x03, Fkt **0x053**, OpType **0x06 StartResultAck**,
  Nutzdaten **big-endian** `u16 SenderHandle, u16 ConnectionID` → 4 Bytes, z. B. `00 01 00 B8`.
  **RemoveHighSyncConnection** (`0x8b83258`): Fkt **0x054**, gleiches Format. Antwort: Fkt 0x053/0x054 OpType 0x0D
  ResultAck `u16 SenderHandle` (passt zu den `00 02`, `00 03` im Mitschnitt vom 7.11.); Fehler OpType 0x0F/0x09.
  Big-endian belegt durch den Stream-Writer `0x89b043e` (High-Byte zuerst).
- Die HMI selbst ruft Build mit festen IDs auf (`0x36`, `0x1A0`, `0x1AC`, `0x08/0x09/0x0A` je Senke) und über
  `CHmiAudioSink` mit der ID aus der Tabelle; SenderHandle teils `0xB1xx`, teils Zähler, teils `0xFFFF`.
- **Tabelle EAudioSource → ConnectionID** bei VA `0x9879224`, 0x88 Zeilen à `{src, conn_zone0, conn_zone1, conn_zone2,
  Kategorie}`. `CHmiAudioSink::getConnIDForAudioSource` (`0x899c068`) liefert `conn[1 + sinkIdx]`.
  Zeile `src 0x53 → 0xB8 / 0x149 / 0x14A`, `0x54 → 0xB9`, `0x55 → 0xBA` (Kategorie 0xB). Dieselbe ID 0xB8 ist auf
  NBT Evo `eSRC_BROWSER → BrowserFront` (NBT-AAD `audioconn -b 0xb8`). Damit ist **0xB8 sehr wahrscheinlich die
  Browser-Front-Verbindung auch auf dem originalen HU_NBT** (am Auto bestätigt, siehe unten).
- Online Entertainment Download (`oeapp`) und Streaming (`wrapp`) liefern je Zone `0x50/0x51/0x52` (Conn `0x182`, `0xC5`,
  `0xC6`), nicht 0x53.
- **Browser-Proxy** `CPxyBrowser`: FBlock **0xBE**; Fkts `CSourceActivity 0x103`, `CBrowserAudioRequired 0x20E`,
  `CBrowserAudioRequired2 0xD76`. Ein Eintrag `be` tauchte im C03-Status vom 7.11. auf. Welcher Prozess FBlock 0xBE
  bedient, ist offline nicht sichtbar (Extrakt enthält nur `boot/bin`).
- Instanz und MOST-Adresse von FBlock 03 im Betrieb aus `kAttrRegistry` lesen, nicht fest eintragen.

Die Verbindung `0xB8` wurde am Auto bestätigt (siehe unten); ob die HMI sie als Quellenwechsel anzeigt oder nur der
Verstärker schaltet, wurde nicht untersucht.

## Implementation and results (2026-11-15)

Android Auto media, navigation voice and system sound play over the USB connection on the original HU_NBT, without Bluetooth.

How it works
- **Output device.** The original NBT has no `libasound` / `dev-libasound` (it runs `dev-ioamp-router`, whose clients use the
  A2I library). `src/android_auto/audio_a2i/asound_device.*` replaces NBT-AAD's `AsoundDevice` (copied over it by
  `scripts/build_all.py`): the browser-audio mixer writes through `/opt/sys/lib/liba2i_client.so` into the A2I endpoint
  `input_stereo_3` (48 kHz stereo, 4096-byte packages x2, fallback 1024 x2, legacy cookie 0x05555555).
- **Route.** `audioconn` (`src/android_auto/knob/nbt_audioconn.cpp`, installed as `audioconn`) replaces NBT Evo's stock
  `audioconn`: `-b 0xb8` / `-r 0xb8` send Build / Remove to the ConnectionMaster (FBlock 0x03, address and instance from
  the MOST registry; Fkt 0x053 / 0x054, StartResultAck, big-endian `{u16 handle, u16 connId}`), `-l` reads the connection
  table. Only 0xb8..0xba are accepted. Frame builder: `ref/most/connmaster.hpp`, test `knob/tests/test_connmaster.cpp`.
- **Policy** (`native_service`, evaluated before every session): Android Auto sound with connection 0xb8, except
  `bt-audio` in the package folder (user switch), `audio-route.failed` (the last build was refused or not answered, kept
  in RAM until the next reboot) or a missing `audioconn` / `liba2i_client.so`, which means Bluetooth as before. After every
  session `native_service` removes a route that is still built (`audio-route.built`), because 0xb8 interrupts the radio.
  `aa-audio` (optional) holds another connection id; `0` plays unrouted.

Verified on a car (BMW F07 2014, original HU_NBT)
- `audioconn -b 0xb8` -> ConnectionMaster 03/00 (0x100E) **granted**; `-r 0xb8` **granted**.
- `experiments/audio/car-route-beep-test.sh`: with 0xb8 built, 3 beeps were heard on `input_stereo_3`; inputs 1, 2, 4 and 5
  were silent.
- Android Auto music through the A2I output plays (reported by the car owner as clean).
- CPU (`hogs`, Android Auto + music + scrolling, two cores): `oneplus_native` incl. mixer about 7 %, `dev-ioamp-router`
  about 5 %; the video decoder (`io-nme`) about 34 %.

Not verified / open
- Other cars, NBT software levels and phones; long drives.
- Navigation prompts ducking the music, and how fast the radio comes back after the route is removed.
- Calls and the microphone still use Bluetooth (channels 2 and 4 stay dropped); no microphone uplink yet.
- `knob/tests/test_connmaster.cpp` was compile-checked with the QNX GCC only, not executed on a host.

Rollback: `touch /var/opt/hmi/data/rw/NbtAndroidAutoRT/bt-audio`, then restart the Android Auto service. If the radio stays
muted: `/var/opt/hmi/data/rw/NbtAndroidAutoRT/audioconn -r 0xb8`. Logs: `capture-*.log` (`[AudioRoute]`, `[Browser]`),
`audioconn-cleanup.log`, `audio-route.*` in `/fs/ram3/nbt-aa-native`.
