# Handoff 2026-11-12: Android-Auto-Ton, Stabilität, Eingabeverzögerung

Stand nach den Sitzungen vom 11. und 12.11.2026. Alles hier Beschriebene ist auf dem Auto installiert, sofern nicht
anders vermerkt. Der Installer (`installer/payload`, exe) ist **nicht** auf diesem Stand.

## Zugriff (kurz)

- Auto: `.local\nbt_ssh_session.py` in einem Terminal-Tab starten, der Nutzer tippt das root-Passwort. Abrufe über
  `.local\nbt_request.py "cmd" [--input-file] [--output-file] [--timeout]` mit einem Python 3 (z. B. `NBT_PYTHON`).
- Die Verbindung zum Auto ist schlecht: alles Nötige in **einem** Abruf holen und offline auswerten.
- VM `qnx-build` (192.168.56.10): Falls nicht erreichbar, in der VM-Konsole
  `slay dhcp.client; ifconfig en0 192.168.56.10; /usr/sbin/sshd`. ssh mit Host-Alias `qnx-build`, `-F .local\ssh_config`,
  `-o HostKeyAlias=192.168.56.10`, Schlüssel `qnx_vm_rsa` (Pfad siehe Memory `nbt-vm-and-vehicle-access`).
- Bauen: Quellen nach `/tmp/rec/` kopieren, `sh /tmp/rec/b.sh` (aus `build-rec.sh`: `oneplus_native` gegen
  `~/.nbt-build/aa-session-20261106T014542Z-f41e2ba1`, dazu `native_service`). Einzelprogramme mit
  `qcc -V4.4.2,gcc_ntox86 -O2 -Wall -Wextra <datei>.c -o <name>`. Plugin siehe `scripts/build-qnx.sh` (HMI-Libs in
  `~/.nbt-build/idrive-hello-20261105`).
- Aufspielen: `scripts\Update-NbtAaPackage.ps1 -Python <codex python>` (enthält jetzt auch `nbt_a2i_play`), danach
  `sh $P/restart.sh`. Plugin-Änderungen wirken erst nach einem iDrive-Neustart.
- Nach **jedem** Dienst-Neustart muss das Handy aus- und eingesteckt werden (ByeBye). Den Nutzer vorher warnen.

Pfade am Auto: `$P=/var/opt/hmi/data/rw/NbtAndroidAutoRT`, `$R=/fs/ram3/nbt-aa-native`,
Plugin `/var/opt/hmi/data/rw/Plugins/NbtAndroidAuto`.

## Niemals

- `/dev/io-amp/routing`, `debug`, `diagnosis`, `processing` lesen (`cat`): direkt danach lief die Shell nicht mehr und das
  NBT startete neu. `ls /dev/io-amp` ist unkritisch.
- `NmeCmdLine -a0` oder `-n` am Auto benutzen (siehe Media-Ton).
- Stock `io-nme` oder `HmiMain3D` beenden, VM-Bibliotheken aufs Auto kopieren, Standalone-EGL-Test wiederholen.

## 1. Media-Tab ohne Ton (behoben, vom Nutzer als stabil bestätigt)

- Ursache: Unser Videoplayer lief mit `NmeCmdLine -a0` ("disable audio track"). Der Schalter bleibt in der **gemeinsamen**
  Media-Engine `io-nme` gesetzt. Schon offene Graphen behielten den Ton, jeder neu geöffnete bekam
  `OnSignalAudio(DEVICE_DETACHED)` und rief `A2I_Client_Open` gar nicht mehr auf. Radio und Telefon laufen nicht über
  `io-nme` und waren deshalb nicht betroffen.
- Fix: `-a0` aus `src/android_auto/nbt_decode_runner.c` entfernt.
- Nebenwirkung: Unser Videograph meldet jetzt `NmeDeviceA2I -> input_stereo_0` mit Audio-Renderer an. Bisher kein
  nachgewiesener Schaden. Falls das Video dadurch leidet: `-ad NmeDeviceNullAudio` wäre ein Kandidat (ungetestet).

## 2. Android-Auto-Musik über die Lautsprecher (funktioniert, vom Nutzer bestätigt: sauber)

Kette: Handy bietet Media-Senke Kanal 9 an (`NBT_SDR_DROP=2,4`) → PCM 48 kHz Stereo → Warteschlange + eigener
Schreib-Thread in `oneplus_native` (`audio_frame`, `audio_writer` in `aa_video_capture.cpp`) → `$R/audio.fifo` →
`nbt_a2i_play` (`src/android_auto/nbt_a2i_play.c`) → `liba2i_client.so` → `/dev/io-amp/input_multich_0`.

Erkenntnisse:
- Alle Stock-Quellen (DVD, Musiksammlung, Bluetooth) spielen in `input_multich_0`. `input_stereo_0` nimmt Daten an, ist
  aber nie hörbar.
- Das Gerät hat **8 Kanäle**. Mit 2 Kanälen geöffnet war der Ton völlig verzerrt; mit 8 (Stereo auf Kanal 0 und 1) sauber.
  Paketgröße = 256 Frames × Kanäle × 2 Byte, 16 Pakete, Priorität 21.
- Der Router bedient nur **einen** Client pro Eingang. Deshalb hält `nbt_a2i_play` vor dem Öffnen den Stock-Player an:
  `mmecli -c /dev/mme/multichannel stop`. Abschalten mit `$R/sound.nostop`.
- Im iDrive muss eine Media-Quelle aktiv sein (nicht Radio), sonst schaltet der Verstärker den Eingang nicht durch.
- Die FIFO ist kleiner als ein 8-KB-Tonpaket: nie aus dem Sitzungs-Thread schreiben oder warten (das hat das Video
  ausgebremst und Sitzungen beendet).
- Konfiguration: `$P/sound.args` = `input_multich_0 8` (auch der einkompilierte Standard). Log: `$R/sound.log`.
- Der Player lässt sich im laufenden Betrieb tauschen (alten `slay`, neuen in `$R` starten), ohne Handy-Replug.

Offen: Skip-Tasten (mmecli kennt `next`, `prev`, `play_file`), Stock-Quelle bleibt nach AA-Musik angehalten,
Navi-Ansagen (Kanal 8) laufen weiter über Bluetooth, Installer enthält `nbt_a2i_play` nicht.

## 3. Sitzungsabbrüche und Decoder

- Der Stock-Player beendet einen HTTP-Stream, der einige Sekunden keine Bytes liefert (`NmeHttp stream inactive`).
  `fifo_http` beendet sich, wenn der Player 2 s nicht liest.
- `native_service` startet in diesem Fall nur die Videokette neu (`restart_video_chain`), leert vorher die FIFO und
  schickt der Sitzung `SIGUSR1`. `oneplus_native` fordert per Video-Fokus NATIVE → PROJECTED ein Vollbild an
  (`AA_RESYNC`-Zeilen). Das Handy antwortet darauf nachweislich mit Config + IDR.
- Keep-alive: Nach 1 s ohne Bild schickt der Writer ein Füll-NAL.
- 12.11.: Live-Queue 384 KB war zu knapp (14 Überläufe, 42 Decoder-Neustarts in 20 min mit Navigation). Jetzt 1 MB, und
  das Füll-NAL geht auch während eines Überlauf-Resyncs raus (`reader_lost` trennt den Decoder-Neustart-Fall).
  **Installiert, noch nicht gefahren.**

## 4. iDrive-Plugin (Trägheit, Split-Screen, "Absturz")

- iDrive ignoriert Fokus-Event 52 (→ Zustand 20) für den Rest der Fahrt, sobald die Seite einmal aus einer mittleren
  Halteebene verlassen wurde. Event 54 (→ Zustand 13) funktioniert weiter.
- Neu: ein Fokus-Event je Halteebene (XML: 52, 55..60, 54 → Zustände 20..13). `Quick` prüft 0,7 s nach einem Event von
  der Einstiegsseite und fällt eine Ebene tiefer (`maxLevel`). Mindestabstand 400 ms, Retry 3 s, Schleifenschutz 40/min.
- Sperre: Solange Android Auto läuft, werden "Neu starten" und die Bildmodus-Knöpfe in den ersten 4 s nach Erscheinen
  der Einstiegsseite ignoriert (`accidental()`). Der "Absturz" vom 11.11. 20:44 war genau so ein Fehldrücker.
- Vom Nutzer nach iDrive-Neustart bestätigt: "besser".
- Build-XML = Quell-XML mit LF und **ohne BOM** (PS 5.1 `Set-Content -Encoding utf8` schreibt ein BOM: .NET
  `WriteAllText` benutzen).

## 5. Eingabeverzögerung

- Installiert: Access Unit Delimiter (NAL 9) hinter jedem Bild und drei 8-KB-Füllpakete im 30-ms-Abstand nach dem letzten
  Bild einer Änderung. Abschalten: `touch $P/aud.off` + Dienst-Neustart. Nutzer: "etwas schneller".
- **Hauptspur, offen:** Der Stock-Player hat nie mehr als ~23–25 Bilder/s dekodiert, das Handy schickt bei Bewegung 30.
  Vermutlich taktet er den zeitstempellosen Stream in eigenem Tempo, der Rückstand wächst bei bewegter Karte. Ideen: SPS
  VUI-Timing auf 60 fps umschreiben, bevor der Decoder es bekommt, oder eine Cinemo-Option für die Bildrate finden.
  `-s 1200` ließ NmeCmdLine nach einem Bild stoppen. Hardware-Decoding wurde am 10.11. verworfen.

## 6. Dauerprotokoll

- `$P/journal/<YYYYMMDD-HHMMSS>/`: ein Ordner je Dienstlauf, die neuesten 60 bleiben. `$P/logs` ist der laufende Satz,
  jede Minute und bei Zustandswechsel aktualisiert. `logs.prev` gibt es nicht mehr.
- `$P/sessions.log`: Grund jedes Sitzungs- und Decoder-Endes.
- Alles in einem Abruf: `scripts\car-journal-collect.sh` (optional `sh -s <Anzahl Läufe>`).
- Zuerst lesen: `AA_LIVE_QUEUE dropped=… drop_runs=…`, `AA_RESYNC`, `decoder ended`, im Plugin-Log
  `hold event ignored -> next level`.

## 7. Kurze Neustarts alle 1–5 Minuten (Abend 12.11., Ursache gefunden, Fix gebaut)

Befund aus `sessions.log` (Fahrt 14:54–16:52 und 16:53–17:13): Die Musik lief durch, aber die **Videokette** (fifo_http,
NmeCmdLine, nbt_hwvideo) wurde rund 50-mal neu gestartet; jedes Mal Video-Fokus NATIVE → PROJECTED (Bild weg, Pfeile im
Tacho neu). Bei laufender Navigation kam der Abbruch in einem **festen Takt von 64–66 s** (17:07:40, 17:08:46, 17:09:52,
17:10:56, 17:12:01; 16:24:02, 16:25:07, 16:26:12), sonst nach 3–14 min.

Ursache:
- Das SPS des Handys (Baseline, Level 3.1) hat eine VUI **ohne timing_info**, der Strom hat keine Zeitstempel. Der
  Stock-Player taktet ihn deshalb mit einer eigenen, angenommenen Bildrate (nie mehr als ~23–25 Bilder/s gesehen).
  Bei bewegter Karte schickt das Handy 30/s. Der Überschuss sammelt sich im Eingangspuffer des Players (das Bild läuft
  immer weiter hinterher, die bekannte Eingabeverzögerung). Ist der Puffer voll, liest der Player nicht mehr, fifo_http
  gibt nach 2 s auf, die Kette wird neu gestartet. Konstanter Überschuss + fester Puffer = fester Takt.
- `NmeHttp stream inactive` heißt nur "HTTP-Strom zu Ende", kein Zeitlimit (steht auch bei Verbindungsfehlern).
- Zweiter Fehler im `native_service`: Der Neustart-Zähler wurde nur zurückgesetzt, wenn eine Kette 120 s lief. Bei 65 s
  Takt nie, also beendete der fünfte Neustart die ganze Sitzung (16:52:39 nach 2 h, 17:13:08 nach 20 min), danach
  "Invalid channel" und Neu-Einstecken.

Fix (gebaut 12.11. abends, `build/aa-entry-20261106/oneplus_native` + `native_service`, alte Stände in `prev-20261112/`;
**installiert 12.11. 17:50 UTC, Dienst neu gestartet, noch nicht gefahren**):
- `src/android_auto/h264_sps_timing.hpp`: schreibt timing_info (num_units_in_tick 1, time_scale 120,
  fixed_frame_rate 1 = 60 fps) in das SPS der Codec-Config, Rest bitgenau gleich. Getestet in der VM mit
  `build/aa-phone-capture-02/phone.h264` (29 → 38 Byte, zweiter Durchlauf ändert nichts, Python-Parser liest 60 fps).
  Ob Cinemo die VUI-Bildrate nutzt, ist **am Auto noch nicht bestätigt**. `NBT_AA_SPS_FPS` (Standard 60, 0 = aus);
  Abschalten am Auto: `touch $P/sps.off` + Dienst-Neustart. Capture-Log: `AA_SPS timing info added`.
- `native_service`: Zähler-Reset schon nach 30 s Laufzeit; nur fünf Abbrüche in Folge mit je < 30 s beenden die Sitzung.
- Neue Diagnosezeilen im Capture-Log: `AA_LIVE_BLOCKED <ms>` (Player hat nicht gelesen) und `AA_LIVE_OVERFLOW`.
  Wenn nach dem Fix weiter Neustarts kommen und davor `AA_LIVE_BLOCKED` steht, liest der Player immer noch zu langsam.
  Nächste Stufe wäre dann, Video-Acks an den Handy-Takt zu koppeln (Ack erst nach dem Schreiben in die FIFO,
  begrenzt auf N/s; Infrastruktur dafür gibt es für Audio schon in `acks_`, `aa_session.cpp` in der VM).

### 7b. SPS-Patch war falsch, jetzt fps-Begrenzung über Video-Acks (12.11. ~18:00 UTC installiert)

- Mit 60 fps im SPS hielt der Player dauerhaft **3,9 MB** in seiner Video-Queue (`NmeGraph video 3.9 MB queue` in
  sloginfo) = ~10 s Verzögerung. Er nutzt die VUI-Bildrate also zum Takten mit großem Vorpuffer. SPS-Patch ist jetzt
  standardmäßig AUS (`NBT_AA_SPS_FPS` Standard 0), `$P/sps.off` liegt zusätzlich auf dem Auto.
- Neu: **gepacte Video-Acks** in der Session-Bibliothek (`aa_session.cpp/.hpp`, Kopie und Original in
  `patches/session-video-pacing-20261112/`, in der VM `reference/src/*.orig-20261112`,
  `libnbt_aa_core.a.orig-20261112`). Kanal-6-Bilder werden höchstens `NBT_AA_VIDEO_FPS` pro Sekunde quittiert, die
  Setup-Antwort erlaubt nur `NBT_AA_VIDEO_WINDOW` (Standard 3) unquittierte Bilder. Das Handy kann damit nicht schneller
  senden, als der Player zeigt. `native_service` setzt 20 fps; Datei `$P/video.fps` mit anderer Zahl (0 = keine Grenze).
  Capture-Log: `[AASession] video paced: at most 20 frames/s acked, window 3`.
- Gebaut mit `/tmp/rec/pace/build-pace.sh` (Bibliothek neu, dann `b.sh`). Protokoll-Simulator besteht, deckt aber kein
  Video ab. **Am Auto noch nicht bestätigt.** Vorherige Programme: `build/aa-entry-20261106/prev-20261112-sps/`
  (mit SPS-Patch) und `prev-20261112/` (Stand vor heute Abend).
- Prüfen nach der Fahrt: sloginfo `NmeGraph video ... queue` (soll klein bleiben), `decoder ended` in sessions.log,
  `AA_LIVE_BLOCKED`.

### 7c. Ergebnis mit fps-Grenze (Fahrt 12.11. 18:05–18:36 UTC) und Übergabe

Nutzer: "ist ok". Aus den Logs:
- Sitzung lief 1881 s und endete regulär (Handy getrennt, "Phone closed socket").
- Nur **3 Neustarts der Videokette** in 31 min: 18:05:58 (34 s nach dem Start), 18:27:58, 18:29:02. Zwischen 18:06 und
  18:27 lief das Bild **22 min ohne Unterbrechung**. Vorher waren es ~13 in 20 min, mit Navigation alle 65 s.
- Kein `NmeGraph video ... queue` mit großer Füllung mehr (die 3,9 MB vom SPS-Versuch sind weg).
- Vor **jedem** Abbruch: `AA_LIVE_BLOCKED 2002 ms, then the reader went away`. Der Player hat 2 s nicht gelesen,
  fifo_http hat nach seinen 2 s aufgegeben (`send_all` in `nbt_fifo_http.c`), danach `NmeHttp stream inactive`.
  18:27:58 → 18:29:02 sind wieder genau 64 s: In manchen Phasen läuft der Player also auch mit 20 fps noch voll.
- Die Zeile `[AASession] video paced: ...` ist in den gezogenen Logs nicht zu sehen: Das Capture-Log liegt in
  `/fs/ram3` und war nach dem Abschalten weg. Dass die Grenze aktiv war, folgt nur aus dem Code (native_service setzt
  `NBT_AA_VIDEO_FPS=20`, keine `video.fps`-Datei). Beim nächsten Mal das Capture-Log während der Fahrt sichern.

Offene Ansätze, in dieser Reihenfolge:
1. **fifo_http weniger ungeduldig**: Mit begrenzter Rate kann sich kein Dauerrückstand mehr aufbauen, ein kurzer
   Lese-Stopp des Players erholt sich vermutlich. Der 2-s-Abbruch in `send_all` erzwingt den Neustart. Kandidat: 6–8 s
   (Konstante in `nbt_fifo_http.c`). Nebenwirkung: Bei einem wirklich toten Player dauert die Erkennung länger.
2. Grenze feinjustieren: `$P/video.fps` z. B. 15 oder 18 und/oder `NBT_AA_VIDEO_WINDOW` 2. Die Grenze zählt Bilder,
   nicht Bytes: Große Bilder bei schneller Kartenbewegung können den Software-Decoder trotzdem überholen.
3. Messen statt raten: Rate und Bytes pro Sekunde, die wirklich in die FIFO gehen, periodisch loggen
   (`live_writer`), dazu `sloginfo | grep 'NmeGraph video'` während der Fahrt.
4. Der Abbruch 34 s nach dem Start tritt öfter auf (auch früher, z. B. 16:53:26 nach 9 s): Anlauf-Phase eigens
   ansehen.

**Log-Sicherung reparieren (vor der nächsten Testfahrt, Nutzerwunsch):** Die Rohlogs liegen in `/fs/ram3` (RAM, weg
nach dem Abschalten); `scripts/nbt-aa-boot.sh` (Schleife Zeile ~95–131) sichert jede Minute einen Auszug nach
`$P/logs`, beim nächsten Start ins Journal. Drei Lücken haben heute die Auswertung verhindert:
1. Zeile ~118: nur das **neueste** `capture-*.log` wird gesichert. Nach einer langen Sitzung überschreiben die kurzen
   Fehlversuche danach (`capture-2..4`) den Auszug der Fahrt (so ging `[AASession] video paced` verloren).
   → Jedes `capture-N.log` des Laufs als eigenes `$L/capture-N.log` sichern (gleicher grep-Filter), nur wenn es sich
   geändert hat (`cmp`).
2. Zeile ~120: nur `tail -800`. → Deutlich mehr (z. B. 5000 Zeilen), die Filter halten es klein.
3. Zeile ~125: `NmeGraph video` wird herausgefiltert, und sloginfo wird nur bei Zustandswechsel gesichert. Der
   Füllstand der Player-Queue (wichtigster Messwert für Verzögerung und Neustarts) wird so nie dauerhaft gespeichert.
   → Einmal pro Minute `sloginfo | grep 'NmeGraph video' | tail -1` mit Uhrzeit an `$L/nme-queue.txt` anhängen.
Flash-Last: wenige KB pro Minute. Aufspielen: `boot.sh` per `scripts/Update-NbtAaPackage.ps1` (lädt auch
`boot.sh`/`restart.sh`), wirkt ab dem nächsten Dienststart; ohne verbundenes Handy kein Neu-Einstecken nötig.
`scripts/car-journal-collect.sh` muss die neuen Dateien (`capture-*.log`, `nme-queue.txt`) mit ausgeben.

**Logs dieses Abends lokal** (nicht im Git, `.local/` ist ignoriert): `.local/car-logs/20261112-evening/`
- `state-after-pacing.txt`: Zustand nach der Fahrt mit fps-Grenze, sessions.log-Ende, io-nme-Log
- `journal.txt`: Journal der letzten 8 Dienstläufe (`scripts/car-journal-collect.sh`)
- `sessions-full.log`: `sessions.log.1` + `sessions.log` komplett (alle Abbrüche des Tages)
- `journal-1738-before-fix.txt`: Journal vor allen Änderungen des Abends (65-s-Takt)
- `sps60-10s-delay-1758.txt`: Beleg für den SPS-Fehlschlag (`NmeGraph video 3.9 MB queue`)
- `snapshot-restart-12171433.txt`, `snapshot-restart-12180132-sps60-session.txt`: Restart-Snapshots
- `sps.py` (SPS-Parser), `test_sps.cpp` (VM-Test des SPS-Patches)

**Stand der Quellen**: `src/android_auto/aa_video_capture.cpp`, `nbt_native_service.c`, neu `h264_sps_timing.hpp`;
Session-Bibliothek in `patches/session-video-pacing-20261112/` (geändert + `.orig`). Nichts davon ist committet.
Installer-Payload und exe enthalten keine der Änderungen dieses Abends.

## 8. Flusskontrolle, geduldiger fifo_http, Titelinfo und Medientasten (12.11. spät, gebaut, NICHT installiert)

Auswertung der Logs und der Cinemo-Bibliotheken (`.local/aa-decoder-runtime/`), ohne Auto:

- **Jeder** Neustart der Videokette begann mit dem 2-s-Abbruch in `fifo_http` (`send_all`). Der Player lief dabei weiter
  und arbeitete seine Queue ab (Snapshot 17:13:01–08: 0,6 MB, fallend). Er wäre also weitergelaufen.
- Die Einstellungen der Stock-Konfiguration `/opt/mm/etc/io-media-cinemo.cfg` gelten **auch für unseren Player**
  (Optionen sind in `io-nme` global). Beleg: `InitConnection(0) -> CinemoErrorTimeout` genau 5,0 s nach dem Verbinden,
  `Delay retry for 3000 ms`. Wichtig: `http-waiting-for-data-timeout = 5000` (5 s ohne Daten → Player trennt und
  verbindet neu), `http-retry-delay = 3000`, `http-low-level-retries = 3`, `http-cache-size = 1024`. Der alte
  `fifo_http` nahm nur **eine** Verbindung an, der Reconnect endete mit `Couldn't connect to server`.
- Standardwerte der Engine (aus `NmeOptions::ResetOptions` in `libNmeBaseClasses.so` gelesen): `General/Buffer/Stream`
  4096 KB (= die 3,9 MB vom SPS-Versuch), `StreamPrefetch` 1024 KB, `FileCache` 1024 KB, `Video/FrameDropThreshold`
  50 ms, `Video/MaxRenderedFPS` 0 (keine Grenze, falls die Stock-Konfiguration nichts anderes setzt: der hintere Teil
  der cfg liegt lokal nicht vor). Optionen ließen sich über `cinemo_options.xml` setzen, wirken dann aber auch auf
  den Stock-Player (wie damals `-a0`): nur nach Rückfrage.
- Mit 60 fps im SPS blieb die Queue voll, obwohl der Player dann schneller hätte takten müssen: Die ~25 Bilder/s sind
  eine Durchsatzgrenze (Dekodieren + Anzeigen), kein Takt. Die fps-Grenze (20) bleibt deshalb.
- `io-nme` lässt sich in der VM nur als root starten (`resmgr_attach`). Für Versuche mit der echten Engine muss der
  Nutzer in der VM-Konsole einen Befehl als root starten (Vorbild `/tmp/nbtcinemo`).

Geändert (Quellen in `src/android_auto/`, Bibliothek in `patches/session-video-pacing-20261112/`, dort auch
`build-flow.sh` für die VM; Programme in `build/aa-entry-20261106/`, vorheriger Stand in `prev-20261112-pacing/`):

- **Flusskontrolle** (`AASession::setVideoAckGate`, `postVideoConsumed`): Ein Bild wird dem Handy erst quittiert, wenn
  der Writer es in die FIFO geschrieben (oder verworfen) hat, zusätzlich zur fps-Grenze. Das Handy ist nie mehr als 3
  Bilder voraus; ein Player, der kurz nicht liest, hält das Handy-Video an, statt die Queue zu füllen und ein
  Schlüsselbild zu erzwingen. Sicherheitsnetz: nach 15 s ohne Rückmeldung wird trotzdem quittiert. Aus:
  `touch $P/video.gate.off`. Capture-Log: `[AASession] video gated: ...`.
- **`fifo_http` neu**: wartet 10 s auf einen Player, der nicht liest (`$P/http.stall` in ms), bleibt für Reconnects
  offen (neue Verbindung beginnt erst mit der Config, Datei `http.resync` → Dienst schickt `SIGUSR1`), `TCP_NODELAY`
  (ein Bild geht in mehreren Stücken unter Segmentgröße raus; Nagle + verzögertes ACK ist ein Kandidat für die
  restlichen ~0,5 s), eigenes Log `$R/http.log`. VM-Test `src/android_auto/test_fifo_http.c`: neu besteht, der alte
  `fifo_http` fällt bei 4 s Lesepause und beim Reconnect durch.
- **Titelinfo**: Kanal 12 → `$R/media.state` (state, position, duration, source, title, artist, album, track, cover)
  und `$R/media.cover` (Bilddatei vom Handy). Capture-Log: `[MEDIA] ...`. Noch niemand zeigt es an.
- **Medientasten**: 85/87/88/126/127 werden dem Handy angemeldet (`NBT_SDR_MEDIA_KEYS=1`, aus mit
  `$P/mediakeys.off`) und über `input.fifo` angenommen. Test am Auto: `echo 'K 87 1' > $R/input.fifo; echo 'K 87 0' > $R/input.fifo`.
- **Lenkrad**: `nbt_knob` protokolliert jede Änderung in FBlock 0x15 (`[Wheel] 15/02 fkt=... :`), gesichert als
  `wheel.txt` im Journal. Zuordnung ohne Neubau über `$P/mfl.map` (`<fkt hex> <Wert hex> <Taste>`, z. B. `514 01 87`).
  Bekannt bisher nur fkt 401 (periodisch) und 513 (MODE).
- **Log-Sicherung** (`scripts/nbt-aa-boot.sh`): jedes `capture-N.log` einzeln (5000 Zeilen), `http.log`, `wheel.txt`,
  einmal pro Minute der Player-Queue-Stand in `nme-queue.txt`. `scripts/car-journal-collect.sh` gibt sie aus.
- `scripts/Update-NbtAaPackage.ps1` spielt jetzt auch `fifo_http` auf. Neu: `scripts/car-media-probe.sh` (nur lesen:
  vollständige Cinemo-Konfiguration, mme/mcd, mmecli-Hilfe, Lenkradtasten, `http.log`).

**Installiert am 13.11.2026 21:46 UTC** (`native_service`, `oneplus_native`, `fifo_http`, `nbt_knob`, `boot.sh`; Dienst
ohne verbundenes Handy neu gestartet, läuft; vorherige Dateien am Auto in `$P/prev-20261112/`). Abrufe dieses Tages in
`.local/car-logs/20261113/`: `journal.txt` (seit dem 12.11. abends keine Sitzung) und `media-probe.txt`. Daraus:
- Vollständige Cinemo-Konfiguration: `video-max-rendered-fps = 0`, `buffer-stream-kb = 4096`,
  `buffer-stream-prefetch-kb = 1024`, `thread-prio-playback = 10`, `thread-prio-video-device = 14`. Es gibt also keine
  konfigurierte Bildraten-Grenze; die ~25 Bilder/s sind Durchsatz.
- Medienverwaltung: `mme-becker -c /opt/mm/etc/mme.conf`, `mcd -L /etc/mcd.cfg`, `qdb` (Datenbank `mme`),
  `io-media-cinemo`, `io-media-generic`. Das iPhone hängt als `io-fs-media -dipod,...,transport=ser:...dev=/dev/IAP1,
  mount=/fs/bti...` (iAP über ein serielles Gerät). `mmecli` ohne `-c` findet kein `/dev/mme/default`; Kontexte sind
  `multichannel`, `zone1..4`, `zoneVN`. Noch nicht gelesen: `/etc/mcd.cfg`, `/opt/mm/etc/mme.conf`.
- Lenkrad-Mitschnitt läuft (`[Wheel] 15/02 fkt=401`), Skip-Tasten wurden noch nicht gedrückt.

**Mit Handy im Stand bestätigt (13.11. 21:49 UTC):** Sitzung startet mit Flusskontrolle + fps-Grenze
(`video paced` und `video gated` im Capture-Log), `[SDR] ... media keys 85 87 88 126 127 declared` wird vom Handy
angenommen, `media.state`/`media.cover` kommen (Spotify, Titel, Interpret, Album, Dauer, Cover 50–84 KB). Tasten über
`input.fifo`: 85 schaltet Pause/Wiedergabe, 87 springt weiter, 88 zweimal springt zurück (einmal = Titelanfang).
`http.log` zeigt nur `player connected`. Noch offen: Fahrt mit Navigation (Neustarts, Verzögerung), Lenkradtasten.

**Skip-Tasten gefunden (13.11. 21:52 UTC):** Skip-Taste am Radio-Bedienteil = `15/0C fkt=C07` (02 gedrückt, 00 los;
01 vermutlich die andere Richtung, noch nicht gesehen), Rändelrad am Lenkrad = `15/02 fkt=C30` (80 in Ruhe, 81 bzw.
7c–7e beim Drehen). Beide gehen an die Stock-Quelle: Die Musiksammlung öffnet ihren nächsten Titel und hängt dann 10 s
an `input_multich_0` (`A2I_Client_GetNotification() TimeOut`), weil `nbt_a2i_play` den Eingang hält.
`nbt_knob` kann jetzt Vergleiche (`>80`, `<80`), löst einmal pro Betätigung aus, nimmt Skip nur, wenn Android Auto
spielt (`media.state` state=2), hält 1 s danach die Stock-Quelle wieder an (`mmecli ... stop`) und liest `mfl.map`
bei Änderung neu. Zuordnung `config/mfl.map` liegt auf dem Auto (`C07 02 87`, `C07 01 88`, `C30 >80 87`, `C30 <80 88`).
`nbt_knob` wurde im laufenden Betrieb getauscht (von Hand gestartet, `boot.sh` räumt ihn beim nächsten Start weg).
**Nutzer 13.11. ~21:57 UTC:** Die Tasten am Radio funktionieren. Das Rändelrad am Lenkrad darf **nicht** abgefangen
werden: Es gibt am Lenkrad nur dieses eine Rad, es blättert die Titelliste im Kombi, und geskippt wird darüber, sobald
das System den Titel mit Titelinfo kennt. Die beiden `C30`-Zeilen sind aus `mfl.map` entfernt (Auto und
`config/mfl.map`). Skip am Lenkrad geht also nur über den Weg "Android Auto als echte Medienquelle mit Titelinfo".

Nicht getestet: alles mit Handy und Player (Flusskontrolle, Reconnect am echten Cinemo, Titelinfo, Medientasten). Die
Protokoll-Simulation besteht, deckt aber kein Video ab.

Nächste Schritte am Auto: aufspielen, `sh $P/restart.sh` (Handy neu einstecken), fahren mit Navigation und Musik, dabei
Skip-Tasten am Lenkrad drücken; danach `car-journal-collect.sh` und `car-media-probe.sh` in je einem Abruf. Lesen:
`http.log` (Lesepausen in ms, Reconnects), `nme-queue.txt`, `decoder ended`, `[MEDIA]`, `[Wheel]`.
Für "Android Auto als echte Medienquelle" (Titel im Kombi wie beim iPhone, Cover, Skip über die Stock-Quelle) fehlt
noch die Untersuchung von mme/mcd am Auto; Ideen: virtueller Datenträger mit drei Titeln (vor/aktuell/nächster) über
einen eigenen Resource-Manager, oder Kombi-Texte direkt über `nbt_cluster`.

## 9. Android Auto als Stock-Medienquelle (13.11. spät, erster Nachweis)

Wunsch des Nutzers (höchste Priorität): Titel mit Titelinfo im Kombi, Skip über das eine Rändelrad am Lenkrad (über die
Titelliste im Kombi), möglichst mit Cover, "wie ein iPhone am USB".

Befunde (`.local/car-logs/20261113/mme-probe.txt`, `qdb-probe.txt`):
- `/etc/mcd.cfg` meldet jeden **Mount** unter `/fs/usb*` als eingesteckten Datenträger (`PATH_MEDIA_PROCMGR`). Ein
  procmgr-Symlink (`ln -sP`) reicht nicht.
- Datenbank `mme` (`qdbc -d mme "<sql>"`): Slots 1–10 = "USB" unter `/fs/usb0`–`/fs/usb9`; Tabellen `mediastores`,
  `library`, `folders`, `trksessions` usw. `wav` und `flac` sind in `mme.conf` als Audio eingetragen.
- `/opt/share/bin/io-fs-media` lässt sich aus der root-Shell weder starten noch kopieren (Permission denied).

Nachweis 22:07 UTC: `src/android_auto/nbt_aa_usb.c` (eigener Resource-Manager, spiegelt ein Quellverzeichnis nur
lesend) unter `/fs/usb0` eingehängt → mcd erkennt es, `mme-becker` legt Mediastore 46 in Slot 1 an ("USB", MSS `devb`),
`NBTMediaMainApp` öffnet ihn. Noch nicht synchronisiert und vom Nutzer im iDrive noch nicht bestätigt. Testdateien am
Auto (RAM, nach Zündung aus weg): `/fs/ram3/aausb/AndroidAuto/01-Testton-FLAC.flac`, `02-Testton-WAV.wav`, Programm in
`/fs/ram3/aatool/`. Beenden: `slay -s TERM nbt_aa_usb`.

Weiter am selben Abend (22:10–22:14 UTC, Abrufe `usb-check*.txt`, `usb-play*.txt`, `mmecli-strings.txt`):
- Nutzer: im iDrive ist kein USB-Gerät zu sehen. mme synchronisiert nicht von selbst, die HMI listet den Datenträger
  nicht (kein echtes USB-Gerät dahinter). `nbt_aa_usb` beantwortet jetzt `DCMD_FSYS_MOUNTED_AT/ON/BY`, `OPTIONS`,
  `LABEL` ("Android Auto" wird der Name des Mediastores) und `DCMD_CAM_DEV_SERIAL_NUMBER`.
- Mit `mmecli -c /dev/mme/multichannel` geht alles Nötige von Hand: `resync_mediastore <msid> 7` liest die Dateien in
  `library` ein (FLAC: Titel, Interpret, Dauer aus den Tags; WAV: nur Dauer), `newtrksession l "<select fid ...>"`,
  `settrksession <id>`, `play` → der Stock-Player spielt FLAC und WAV von `/fs/usb0` lückenlos in den Verstärker
  (vorher `nbt_a2i_play` beenden, er hält den Eingang). Weitere Befehle im Programm: `metadata_set`,
  `metadata_set_xml`, `sync_file`, `trksession_append_fids`, `play_file`, `next`, `prev`, `start_device_detection`.
- Nicht wiederholen: `settrksession` ohne Argument setzt Sitzung 0 (hat die laufende Sitzung der HMI gelöscht).
- Offen: Hat der Nutzer die Töne gehört, und was zeigten iDrive und Kombi, während mme unsere Sitzung spielte?

22:17–22:28 UTC: Der Nutzer hat beide Testtöne gehört und Titel/Interpret im iDrive gesehen (in der Musiksammlung),
während mme unsere Sitzung spielte. Danach Live-Version gebaut und von Hand gestartet (nur RAM):
- `src/android_auto/nbt_aa_usb.c` v2: `/fs/usb0/01.flac` "Vorheriger Titel", `02.flac` aktuelles Lied (Tags und Cover
  aus `media.state`/`media.cover`), `03.flac` "Naechster Titel"; alle drei liefern den Live-Ton aus `audio.fifo` als
  endloses FLAC aus unkomprimierten Frames; `$R/usb.state` für das Skript.
  Start: `slay nbt_a2i_play`, dann `nbt_aa_usb --mount /fs/usb0 --dir $R --log <datei>`.
- `scripts/nbt-aa-usb-ctl.sh` (am Auto `/fs/ram3/aatool/ctl.sh`, läuft solange `ctl.run` existiert, Log `ctl.log`):
  Resync → neue Track-Session → `play <fid von 02>`; wird 01/03 länger als 250 KB am Stück gelesen, geht die Taste
  Zurück/Weiter ans Handy; neue Tags (Liedwechsel) → von vorn.
- Erster Lauf 22:27: Medium 48, Session 3719, der Stock-Player spielt `/fs/usb0/02.flac` "Promi Party" live in
  Echtzeit, 1 s Vorlauf bis zum Ton. Vom Nutzer noch nicht bestätigt: Klang, Titel/Cover in iDrive und Kombi, Skip über
  die Liste, Verhalten beim Liedwechsel.
- Noch nicht eingebunden: `native_service` startet weiter `nbt_a2i_play`; nichts davon liegt in `$P`.

Plan (ursprünglich): Titeldateien, deren Inhalt der Live-Ton von Android Auto ist (spielt der Stock-Player, damit entfallen
`nbt_a2i_play` und der `mmecli stop`-Trick), Tags und Cover aus `media.state`/`media.cover`, ein rollender Satz von
Dateien, bei dem "nächste/vorige Datei geöffnet" zu MEDIA_NEXT/MEDIA_PREVIOUS fürs Handy wird. Offen: Spielt der
Stock-Player WAV und das selbst erzeugte FLAC, nimmt die HMI Tags/Cover aus der Datei oder nur aus der Datenbank, wie
wird der Titel beim Liedwechsel aktualisiert. Vor Live-Dateien: eigenes Attribut je Datei (jetzt teilen sich alle zwei
Attribute, ein blockierendes Lesen würde alles andere aufhalten).

## Sicherungen

- `build/aa-entry-20261106/prev-20261112-pacing/` (Stand mit fps-Grenze, vor der Flusskontrolle; der alte `fifo_http`
  liegt in `installer/payload`)
- `build/aa-entry-20261106/prev-20261111/` (Programme vor dem 11.11., Plugin .so + .xml vor der Umstellung)
- `build/aa-entry-20261106/prev-before-aud/` (vor der Bild-Ende-Markierung)

## Nächste Schritte

0. **Zuerst Abschnitt 7–7c lesen** (Neustarts der Videokette, fps-Grenze, offene Ansätze, Log-Ablage).
1. Journal nach der nächsten Fahrt mit Navigation lesen: Überläufe und Decoder-Neustarts zählen.
2. Bildrate des Players (Abschnitt 5).
3. Skip-Tasten und Wiederanlauf der Stock-Quelle.
4. Installer-Payload und exe auf diesen Stand bringen (`nbt_a2i_play`, `sound.args`, Journal in "Logs speichern").
5. `_nbt-dev-parked` auf dem NBT erst nach Rückfrage löschen.
