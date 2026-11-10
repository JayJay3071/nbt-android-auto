# Handoff 9.11.2026: Review (Auflösung, Zuverlässigkeit, Karte im Tacho)

**Stand:** Alles ist gebaut und abgelegt, **nichts ist installiert oder am Auto getestet**. Die SSH-Verbindung zum Auto
war weg. Zugang, VM und Regeln stehen in den Memory-Notizen (`nbt-vm-and-vehicle-access`, `pending-car-tests-…`).

## Was abgelegt ist

| Datei | Inhalt |
|---|---|
| `build/aa-entry-20261106/staged-20261109/libNbtAndroidAuto.so` | Plugin: höchstens ein Sprung-Befehl pro Sekunde, loggt „ms since the last focus change“ |
| `build/aa-entry-20261106/staged-20261109/native_service` | Fehlversuch-Zähler wird nach einer Sitzung ≥ 60 s zurückgesetzt |
| `build/aa-entry-20261106/staged-20261109/decode_runner` | Schalter `NBT_DEC_HW=1` für Hardware-Dekodierung (Standard bleibt Software) |
| `build/aa-entry-20261106/center-test/nbt_cluster_bench` | Messlauf `--bench`: Bitmap-Tempo zum Tacho |
| `scripts/car-review-collect.sh` | Logs und Schnappschüsse einsammeln, nur lesend |
| `scripts/car-hwdec-ram-test.sh` | RAM-Test Hardware-Dekodierung (`sh -s 1` an, `sh -s 0` aus) |

Die Quellen dazu sind schon geändert: `aa_entry_plugin.cpp`, `nbt_native_service.c`, `nbt_decode_runner.c`,
`nbt_cluster.cpp`.

## Reihenfolge am Auto

Der Nutzer loggt sich per SSH ein. Auto geparkt, keine Route aktiv.

1. **Logs lesen:** `car-review-collect.sh` über `nbt_request.py` schicken (`"tr -d '\015' | sh"`, `--input-file`,
   `--output-file`).
   - Im Plugin-Log nach `focus event`, `verify: no hold state focused` und `hold skipped` suchen.
   - Frage: Hat der Ausweich-Befehl 54 geholfen? Wurde ein Befehl ignoriert, der kurz nach einem Fokuswechsel kam?
   - Schnappschüsse unter `snapshots/` lesen (Media-Ton weg, „verbunden, aber kein Bild“).
2. **Messlauf Tacho:** `nbt_cluster_bench` nach `/fs/ram3/nbt-center-test` hochladen, dann
   `LD_LIBRARY_PATH=/opt/sys/lib:/lib:/usr/lib ./nbt_cluster_bench --bench` (ca. 1 Minute, Zeilen `[bench]`).
   - Zeigt: Bytes pro Sekunde auf dem Steuerkanal (tx 0), ob der Tacho Bilder auf dem schnellen Kanal (tx 1) annimmt und
     ob die Flächen 0x21/0x1F (221×158) überhaupt Bitmaps akzeptieren.
   - Entscheidung: Nur bei mehreren Bildern pro Sekunde lohnt der Umbau für eine Karte (eigener AA-Videostrom für den
     Tacho, zweiter Decoder). Sonst dem Nutzer klar absagen. Erwartung aus dem Fahrt-Mitschnitt: 1 Bild alle 1–4 s.
3. **Korrekturen installieren:** Plugin nach `build/aa-entry-20261106/NbtAndroidAuto/`, `native_service` und
   `decode_runner` nach `build/aa-entry-20261106/` kopieren, dann `scripts/Update-NbtAaPackage.ps1`. Aktiv nach einem
   iDrive-Neustart. Danach Installer neu bauen: `installer/make_payload.py`, `installer/build_windows.ps1`.
4. **Hardware-Dekodierung nur mit OK des Nutzers:** `decode_runner` nach `/fs/ram3/nbt-center-test`, dann
   `car-hwdec-ram-test.sh` mit `sh -s 1`, Handy verbunden.
   - Prüfen: Bild da, flüssiger, `decoder-N.log` („Video decoded fps“).
   - Zurück: `sh -s 0` oder NBT-Neustart. Risiko: Im schlimmsten Fall startet das NBT einmal neu.
   - Bei Erfolg `NBT_DEC_HW=1` in die Startzeile von `scripts/nbt-aa-boot.sh` aufnehmen.

## Erkenntnisse

- **Auflösung:** 800×480 ist eine Hardware-Grenze der internen Videoleitung. 16:9 ist schon pixelgenau.
- **Dekodierung:** Bisher Software (`-wd 0`, ca. 23 Bilder/s, 27 % CPU). `-wd 1` ist ungetestet.
- **Sprung-Befehl:** Verdacht, nicht bewiesen: Ein Befehl mitten in der Seitenanimation von iDrive wird ignoriert,
  danach alle weiteren. Im Log vom 9.11. war der erste ignorierte Befehl ein Doppel im Abstand von 50 ms.
- **MOST:** Übertragungstyp 0 = Steuerkanal, 1 = asynchroner Kanal. BMWs eigenes Schild (305×44) braucht 0,4–1,1 s.
- **Latenz-Risiko, nicht geändert:** Die Video-Warteschlange darf 3 MB groß werden. Fällt der Decoder zurück, sind
  mehrere Sekunden Verzögerung möglich.

## Weiter offen

Media-Ton (Schnappschuss nötig), Fahrspuren vom echten Handy, Tempolimit-Quelle (pausiert), sauberer Installer-Test,
Code-Signatur für die `.exe`.
