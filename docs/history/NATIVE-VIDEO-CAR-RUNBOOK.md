# Fahrzeug-Termin: native Videoausgabe über den Stock-Cinemo-Pfad
Stand 6. November 2026. Vorbereitet ohne Fahrzeug. Nichts hiervon wurde bisher auf dem NBT ausgeführt.

## Was offline belegt ist
- `NmeCmdLine` (Cinemo 1.24.13, Plattform `qnx_gf650`, Harman-NBT-Branch) hat eingebaute Optionen für die Videoausgabe:
  `-vd <Gerät>`, `-vi <Geräte-ID>`, `-vl <GF-Videolayer>`, `-vs <GF-Untertitellayer>`, `-x/-y/-xx/-yy` (Fenstergröße/Position),
  `-va <1 erhalten|2 füllen|3 strecken>`, `-f` (Vollbild). `-n` setzt NUR Null-Renderer (das nutzt der PNG-Pfad).
- `libNmeVideoDevice.so` enthält die Geräteklassen `NmeDeviceScreen` (libscreen), `NmeDeviceGF` (libgf), `NmeDeviceIow` (libiow),
  `NmeDeviceCallbackVideo`, `NmeDeviceNullVideo`, `NmeDeviceConsole`.
- Die Geräte lesen ihre Einstellungen aus der Geräte-ID als URL-Parameter-String: `irgendwas?schlüssel=wert&schlüssel2=wert2`
  (Parser `NmeString::get_url_parameter` in libNmeBaseClasses: erst `?`, dann Trennung an `&`, Vergleich `schlüssel=`).
  Bekannte Schlüssel: `hmi-window-class`, `video-window-class`, `video-subpicture-window-class`, `hmi-layer-id`, `video-layer-id`,
  `video-subpicture-layer-id`, `video-output-id` (GF), `display`, `windowpos`, `windowsize`, `handle-events`, `video-subpicture-sw-scale`.
  Welche Werte die Serie benutzt, steht NICHT offline fest. Das liefert die Inventur.
- Stock-Prozesse (alte Prozessliste): `io-nme -vv` (Cinemo-Dienst, /dev/io-nme), `io-media-cinemo -c /opt/mm/etc/io-media-cinemo.cfg -D`,
  `io-media-generic -c /opt/mm/etc/io-media-generic.cfg -D`, `mme-becker -c /opt/mm/etc/mme.conf -v -S`, `io-msvdx -v`,
  `/bin/screen`, `DisplayManagerScreen`, `WalServer 72`, `nvkdserver`, `HmiMain3D`. Hardware: Intel Poulsbo (`pci-poulsbo`, /dev/msvdx) plus NVIDIA-GPU.
- Hypothese (hohe, aber nicht bewiesene Wahrscheinlichkeit): `NmeCmdLine` ist nur Client des geteilten `io-nme`; das Videogerät wird im
  Dienst erzeugt. Dazu passt der Kommentar in `test_decoder_deadline.c` und der SIGPIPE-Leseguard der Roh-FIFO. Die Inventur
  (`libs`: geladene Bibliotheken und offene Dateien von io-nme) entscheidet das.

## Vorbereitete Werkzeuge
| Werkzeug | Zweck |
|---|---|
| `scripts/Inspect-NbtVideoRoute.sh` | nur lesende Inventur, abschnittsweise, jeder Abschnitt begrenzt (in der VM trocken geprüft) |
| `scripts/Collect-NbtVideoRoute.ps1` | führt alle Abschnitte über die offene SSH-Sitzung aus, Ergebnisse nach `build/native-video-route-20261106/vehicle-<zeit>/` |
| `src/android_auto/nbt_video_runner.c` | begrenzter Starter für einen eigenen `NmeCmdLine` mit frei wählbaren Argumenten (SIGTERM, dann SIGKILL, keine Core-Dumps) |
| `scripts/Test-NbtNativeVideo.ps1` | `Stage` (Binärdatei und Clip ins RAM, Rücklesen per SHA-256), `Run`, `Status`, `Cleanup` |
| `build/native-video-test-20261106/` | `video_runner` (in der VM gebaut, getestet), `pattern-800x480.mp4/.h264`, `manifest.json` |

In der VM geprüft: Kompilierung, Fehlerfälle (rc 2), Zeitlimit (`forced=1`, rc 124), normales Ende (rc 0). Stage, Rücklesen, Run, Status und
Cleanup wurden mit einem Test-Worker gegen die VM durch das echte `nbt_request.py`-Protokoll geprobt (Dateien byte-identisch zurückgelesen).
Nicht geprüft: echtes Rendering. Das geht nur im Fahrzeug.

## Ablauf im Auto
0. Kabel und Zündung. `Ethernet` muss „Connected“ sein. In einem eigenen Terminal `python .local\nbt_ssh_session.py` starten und das Root-Passwort selbst eingeben. Fenster offen lassen.
1. Inventur (nur lesend, ca. 1–2 Minuten):
   `.\scripts\Collect-NbtVideoRoute.ps1`
   Danach sind Ergebnisse da für: Prozesse, geladene Bibliotheken und offene Dateien von io-nme/io-media-cinemo/mme-becker/Screen/Wal/HMI,
   `/opt/mm/etc/io-media-cinemo.cfg`, gefilterte `mme.conf`, Gerätedateien, Grafikkonfiguration, `use`-Texte, gefilterte slog-Zeilen.
2. Auswertung (hier, offline): Aus `io-media-cinemo.cfg` und den geladenen Bibliotheken von io-nme werden Gerät (`NmeDeviceScreen|Iow|GF`),
   Geräte-ID und ggf. Layer der Serienwiedergabe abgelesen. Erst danach gibt es Testparameter. Nichts raten.
3. Klartest mit bekanntem Clip (RAM, rückstandsfrei):
   `.\scripts\Test-NbtNativeVideo.ps1 -Action Stage`
   `.\scripts\Test-NbtNativeVideo.ps1 -Action Run -Device <aus Inventur> -DeviceId '<aus Inventur>' -Layer <aus Inventur> -Seconds 8`
   Nutzer beobachtet das Display. Ein Lauf ist auf höchstens 20 s begrenzt. Danach `-Action Cleanup`.
4. Abbruchkriterien: HMI flackert/hängt, iDrive startet neu, `io-nme` verschwindet, Audio der Serienfunktionen bricht weg → sofort stoppen, Befund
   dokumentieren, den Versuch NICHT wiederholen. Ein früherer EGL-Versuch endete mit Absturz und Neustart.
5. Wenn der Clip sichtbar und sauber endet: Live-Anbindung (siehe unten). PNG-Pfad bleibt als Rückfall unverändert.

## Live-Anbindung nach bestandenem Clip-Test (noch nicht umgesetzt)
`nbt_native_service.c` startet heute `fifo_http`, `native_frames` (PNG) und `decode_runner` (`-n ... -dv raw.fifo`). Für die native Ausgabe wird
im selben Dienst, abhängig von einer optionalen Datei `native-video.args` im Laufzeitordner, stattdessen `video_runner 0 ./NmeCmdLine <Argumente>
http://127.0.0.1:18765/live.h264` gestartet und `native_frames` ausgelassen. Ohne die Datei bleibt das Verhalten unverändert. Das wird erst
umgesetzt, wenn die Parameter aus Schritt 2/3 bekannt sind, damit der funktionierende Stand nicht ohne Belegbarkeit verändert wird.

## Ehrliche Einordnung
Fertig im Sinne von „native Videoausgabe belegt“ ist heute realistisch, wenn die Inventur eindeutige Serienparameter liefert und der Clip-Test sauber läuft.
Nicht Teil davon und weiter offen: Bildrate und Latenz im Livebetrieb (20–30 fps sind ein Ziel, kein Nachweis), Vollbild-Policy mit der HMI,
Audio, Controller-Eingabe, Boot-Autostart, Langzeitstabilität.
