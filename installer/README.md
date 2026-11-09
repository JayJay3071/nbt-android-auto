# NBT Android Auto Installer

Installiert Android Auto auf einem originalen BMW NBT (QNX). Danach gibt es unter **Multimedia** den Eintrag
**Android Auto**. Der Installer braucht nur SSH-Zugang zum NBT: IP-Adresse und root-Passwort.
Das Passwort wird nirgends gespeichert.

## Benutzung

1. Laptop per ENET-Kabel mit dem Auto verbinden, Zündung an. Die übliche NBT-Adresse ist `169.254.199.99`.
2. Installer starten:
   - Windows: `NBT-AndroidAuto-Installer.exe`
   - macOS: `NBT-AndroidAuto-Installer.app`. Beim ersten Start mit Rechtsklick → Öffnen, weil die App nicht signiert ist.
3. IP-Adresse und Passwort eingeben, dann **Installieren / Aktualisieren** klicken.
4. Das iDrive neu starten: Ein/Aus-Knopf ca. 30 s halten.
5. Telefon per USB anstecken und **Multimedia → Android Auto** wählen.

## Was Android Auto auf dem NBT kann

- **Bildmodus** auf der Android-Auto-Seite:
  - **Breitbild** (Standard) nutzt die ganze Breite.
  - **16:9** ist schärfer, rechts daneben bleibt der iDrive-Split-Screen sichtbar.

  Der Wechsel gilt ab der nächsten Verbindung des Telefons.
- **Bedienung** mit dem iDrive-Controller (drehen, drücken, kippen, BACK).
- **Navigation im Tacho** (6WB-Kombiinstrument), solange Google Maps eine Route führt:
  - Pfeil, Entfernung mit Balken und Straße,
  - Autobahn- und Bundesstraßenschild,
  - Fahrspuren.

  Sobald die BMW-Navigation selbst führt, hat sie Vorrang.
- **Android Auto neu starten** (Knopf auf der Android-Auto-Seite) startet nur den Android-Auto-Dienst neu, nicht das
  NBT. Vorher wird ein Diagnose-Schnappschuss gespeichert. Den holt **Diagnose-Logs speichern** mit ab.

Im **Breitbild**-Modus empfiehlt es sich, in iDrive den Split-Screen auszuschalten (OPTION → Split-Screen aus). Sonst
öffnet zweimal schnell nach rechts kippen das Seitenfenster.

**Deinstallieren** entfernt genau die beiden installierten Ordner. **Diagnose-Logs speichern** lädt die Protokolle als
ZIP herunter, z. B. für Fehlermeldungen.

## Was auf das NBT geschrieben wird

| Ort | Inhalt |
|---|---|
| `/var/opt/hmi/data/rw/NbtAndroidAutoRT/` | Dienst, Tacho-Navigation, Zertifikate, `boot.sh`, `restart.sh`, `vid.dpi`, `logs/`, `snapshots/` |
| `/var/opt/hmi/data/rw/Plugins/NbtAndroidAuto/` | iDrive-Plugin (Menüeintrag) |

Jede Datei wird erst als `.tmp` geschrieben, zurückgelesen und Byte für Byte verglichen und erst dann umbenannt. Weitere
Systemdateien werden nicht verändert.

## Ohne fertige App (Windows, macOS, Linux)

```
pip3 install "paramiko>=3.4,<4"   # paramiko 4+ kann das alte ssh-rsa des NBT nicht mehr
python3 nbt_aa_installer.py                  # Fenster
python3 nbt_aa_installer.py --cli install    # Terminal
```

Der Ordner `payload/` muss neben dem Skript liegen.

## Für Entwickler

- `python installer/make_payload.py [version]` sammelt die am Auto getesteten Build-Dateien in `installer/payload/`.
  Die Datei `manifest.json` enthält SHA-256 je Datei. Der Ordner wird nicht eingecheckt.
- `installer/build_windows.ps1` baut die Windows-.exe. `installer/build_mac.sh` baut die App auf einem Mac.
- Für Tests in einer QNX-VM gibt es versteckte Optionen: `--key <ssh-key>` und `--root <testverzeichnis>`. Damit wird
  unterhalb eines Testordners installiert statt im System.
