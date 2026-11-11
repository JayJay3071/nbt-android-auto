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

## Stand 11.11.2026: AA-Seite gebaut, Route offen

- **Gebaut (nicht am Auto getestet):** `oneplus_native` enthält jetzt den Browser-Audio-Modus von NBT-AAD
  (`audio/browser_audio.*` mit Mixer, Resampler, Drift-Regelung, verzögerten Audio-Acks, Ausgabe über
  `/opt/mm/lib/libasound/libasound.so`). Die Verdrahtung steht in `src/android_auto/aa_audio.cpp`.
  - Die Upstream-Audiotests (Mixer, Resampler, Acks, Route-Policy, SDR) laufen auf dem Host grün.
  - Der QNX-Build ist noch nicht in der VM geprüft.
- **Schalter:** `touch /var/opt/hmi/data/rw/NbtAndroidAutoRT/aa-audio`, dann iDrive neu starten (oder Restart-Knopf).
  - `native_service` setzt dann `NBT_AUDIO_MODE=browser`, `NBT_SDR_DROP=2,4` (Kanal 9 bleibt drin) und
    `NBT_BROWSER_CONN=0` (ohne Route abspielen).
  - Ohne die Datei ändert sich nichts: Ton weiter über Bluetooth.
  - Zurück: `rm .../aa-audio`, neu starten.
- **Route:** BrowserAudio ruft für den Verbindungsaufbau ein `audioconn`-kompatibles Programm auf
  (`audioconn -b 0x<id>` / `-r 0x<id>`, Exit 0 = gewährt; Pfad `NBT_AUDIOCONN`).
  - Erste Zeile von `aa-audio` = Verbindungs-ID (z. B. `0xb8`), wirkt nur mit einem Programm unter
    `NbtAndroidAutoRT/audioconn`.
  - Ein eigener Ersatz (MOST `BuildHighSyncConnection` an 0x100E oder DSI AudioManagement `requestFocus`) fehlt noch.
- **Nächster Schritt am Auto (nur lesend):** `scripts/car-audio-route-probe.sh`.
  - Prüft, ob es `audioconn` doch gibt, ob der libasound-Shim und `dev-libasound` laufen.
  - Sucht, welche Binärdateien die Routing-Schnittstellen tragen (Strings aus `HmiMain3D` und den Audio-Bibliotheken).
  - Gibt das Systemlog zu Audio-Verbindungen aus.
- **Erster Hörtest danach (mit Freigabe):** `aa-audio` an, Musik in AA starten.
  - Hörbar, ohne dass eine Route gebaut wurde: Bluetooth-/Radioquelle hört dann ggf. mit.
  - Sonst im Log `capture-*.log` nach `[Browser] PCM open`, `device setup` und den 10-s-Statistiken schauen.
  - Erwartung laut Pieptest vom 7.11.: still, bis die Route gebaut wird.
