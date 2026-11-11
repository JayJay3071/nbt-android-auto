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
