# Android-Auto-Ton ohne Bluetooth – Stand 7.11.2026

## Befunde
- **Pieptest über A2I** (`nbt_a2i_beep`, Quelle in `src/android_auto/audio/`, Skripte in `experiments/audio/`):
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

## Update 2026-11-18 (fork, car-tested, English)

The audio path works on the original NBT (F30), tested with `nbt_key` and `nbt_a2i_beep`:

1. `nbt_key cm 053 6 00 18 00 b8`: ConnectionMaster builds connection `0xb8`.
2. `nbt_a2i_beep input_stereo_3 2`: the beeps are audible (A2I client, 48 kHz 16 bit stereo).
3. `nbt_key cm 054 6 00 19 00 b8`: the connection is removed, the main source plays again.

**ConnectionMaster methods** (FBlock 03 at 100E; HmiMain3D `CPxyConnectionMaster` method table, `0x8b818f1`):

| fkt | Method | Data |
| --- | --- | --- |
| `053` | BuildHighSyncConnection | handle (2 bytes), connection (2 bytes); 5 bytes = error 05 (length) |
| `054` | RemoveHighSyncConnection | handle (2 bytes), connection (2 bytes) |
| `C03` | HighSyncConnectionTable (status) | `00 00`, then per connection: id (2 bytes), state (1 byte) |
| `C05` | HighSyncConnectionControl | not used |

State in `C03`: `00` = playing, `04` = muted by an interrupting connection. Example while `b8` is built over AUX:
`00 00 00 bb 04 00 b8 00`.

**Connection IDs**: HmiMain3D has a table of 136 rows {EAudioSource, three connection IDs, category} at `0x9877344`
(read by `CHmiAudioSink::getConnIDForAudioSource`). Category `01` holds the main sources (`01` radio, `bb` AUX, `c7`
Bluetooth audio, ...), `b8` is category `0b`.

**`b8` (browser, NBT-AAD: `eSRC_BROWSER`, "interrupting entertainment")** is not a main source: while it is built the
amplifier mutes the main source, but iDrive and the source status still report the main source, so the steering wheel
keys still act on it (car 2026-11-18: rocker changed the selected source's tracks / stations). Good for navigation
prompts; for music a real main source (category `01`) with a writable input is needed.

NBT-AAD's browser audio mode (`src/audio/browser_audio.*`, mixer, resampler, libasound shim) is built for exactly this
connection; it builds / removes it through BMW's `audioconn` tool, which this NBT does not have. Here `053` / `054` over
MOST take its place.
