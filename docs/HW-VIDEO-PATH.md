# Hardware-Videoweg ohne Stick (Stand 7.11.2026)

## Warum
Der WAL-Sprite-Weg (Bild als Textur hochladen) ist für Dauerbetrieb nicht tragfähig. Belege:
- 7.11., drei Läufe:
  - Lauf 1: ca. 60 s, dann Sitzungsende.
  - Lauf 2: 800x300 bei ~9 fps, nach ~30 s Neustart.
  - Lauf 3: 640x240 bei ≤6 fps, nach ~40 s Neustart.
- Lauf 2 und 3 endeten jeweils mit `NVRM: PUSHER RESERVED CMD ERROR / Fifo: Parse Error` im NVIDIA-Treiber, direkt nach
  `create sprite 31281`. Danach SIGSTOP an alle GPU-Prozesse, Watchdog, Neustart.
  Belege: `build/aa-entry-20261106/dump-run2/`, `dump-run3/`.
- In Lauf 3 tauchte der Treiber in der CPU-Messung nicht auf. Es liegt also nicht an der Last: Das wiederholte Ersetzen einer großen
  Textur löst den Fehler früher oder später aus.
- Zusätzlich kosten die Rohbild-Pipes ~28 % CPU (`pipe`), der Software-Decoder (io-nme) ~24 %.

## Wie die HMI ein Video einschaltet (MOST-Mitschnitt 7.11., Handy-MTP-Video)
Mitschnitt: `build/video-trace-20261107/trace.log`, aufgezeichnet mit `nbt_knob --trace` (nur zuhören).
Zeitpunkte passen zum Systemprotokoll (`slog-video.txt`: nvideocapture-Start 00:13:01 = t≈781, Stopp 00:13:33 = t≈813).

Teilnehmer:

| Adresse | Rolle |
|---|---|
| 2021 | HMI |
| 2008 | `VideoApp` mit FBlock **D7** (VideoConnectionMaster) |
| 2007 | `VideoApp` mit FBlock **D8** (VideoSink CID) |
| 2006 | FBlock 07/11, wird von VideoApp benachrichtigt |

Start (zweimal identisch beobachtet, t=738 und t=781):
```
2021->2008 D7/01 fkt=060 op=6 (StartResult)  03 aa 16 01     BuildVideoConnection(handle 0x03aa, Quelle 0x16, Senke 0x01)
2008->2006 07/11 fkt=D62 op=0  06 00 00 00 00 00 00 nn        (VideoApp intern)
2008->alle D7/01 fkt=062 op=C  00 00 16 00 00 00 00           Verbindungstabelle: Senke CID <- Quelle 0x16
2008->2021 D7/01 fkt=060 op=D (ResultAck)  03 aa 16 01
2021->2007 D8/01 fkt=C05 op=0 (Set)  16 00                    CurrentVideoSource = 0x16
2021->2007 D8/01 fkt=C03 op=0 (Set)  00 00 00 00 07 80 04 38  VideoViewport (Quellbereich) 0,0,1920,1080 -> DisplayManager Src-size
2021->2007 D8/01 fkt=C01 op=6 (StartResult)  03 aa           StartVideo -> dev-nvideocapture startet (800x480, 60 Hz)
2007->2021 D8/01 fkt=301..304 op=C                            Helligkeit/Kontrast/Farbe/Tint
2007->2021 D8/01 fkt=C01 op=D  03 aa                          ResultAck
2021->2007 D8/01 fkt=C04 op=0 (Set)  01 0e 00 8c 01 40 00 b4  ScreenViewPort 270,140,320,180 (Vorschau)
2021->2007 D8/01 fkt=C04 op=0 (Set)  00 d5 00 00 03 55 01 e0  ScreenViewPort 213,0,853,480 (Vollbild 16:9)
```
Stopp (t=762 und t=813):
```
2021->2007 D8/01 fkt=C02 op=6  35 1d          StopVideo
2021->2008 D7/01 fkt=061 op=6  35 1d 16 01    RemoveVideoConnection(Quelle 0x16, Senke 0x01)
2021->2007 D8/01 fkt=C05 op=0  00 00          CurrentVideoSource = keine
```
Quelle 0x16 ist der Multimedia-Player (Cinemo Ausgang 1, Fenster `CinemoVideo1_TaskA/B`). VideoApp setzt daraufhin selbst
Src-pos/Src-size/Visibility beim DisplayManager, startet die NVIDIA-Aufnahme und stellt die WAL-Videoebene ein
(`CPVideoSinkWALController`, `WalHmiInterface::setViewport`).

## Plan für Android Auto
1. Unser Decoder läuft weiter über io-nme, aber mit der Stock-Ausgabe: `NmeCmdLine -vd NmeDeviceScreen -vi
   "?video-output-id=1&...&video-window-class=CinemoVideo1_TaskA,CinemoVideo1_TaskB"`. Das wurde schon gestern getestet (21 fps
   dekodiert, Fenster angelegt). Damit entfallen YUV-Pipe, RGBA-Wandlung und Sprite-Uploads.
2. Ein neues Werkzeug `nbt_video_route` (DSI-Client wie nbt_knob) sendet die Startfolge oben. Quellbereich 0,0,800,300 (unser
   AA-Inhalt), ScreenViewPort 0,0,1280,480 (Vollbild, Faktor 1,6 in beiden Richtungen). Zum Beenden sendet es die Stoppfolge.
3. Liegt die HMI-Ebene über der Videoebene, die Reihenfolge per `WalHmiInterface::setZOrder` umdrehen, oder die AA-Seite
   (Halteseite 13) mit transparentem Inhalt gestalten.

## Offene Punkte vor dem ersten Sendetest
- Sendeformat von DSI-Anfrage 0x2 (`processMessage`): `u32 clientId` + MessageType. Unklar sind Übertragungsart und Variante
  (empfangen: tx=0, var=2/0) und die Absenderadresse (eigene deviceId 0x206x oder die der HMI). Das wird aus HmiMain3D bzw.
  libimp_sys_most_base_nbt.so.11 abgeleitet.
- Reaktion der HMI auf eine fremde Verbindung mit Quelle 0x16: Der Multimedia-Player hält die Quelle eventuell für seine eigene.
  Möglicherweise wäre eine andere, ungenutzte Quellen-ID besser.
- Risiko: Falsche Nachrichten an VideoApp können die Videokette stören. Ein Neustart ist möglich, aber kein Datenverlust.
  Jeder Sendetest nur mit Freigabe.
