# NBT Android Auto Installer

Installiert Android Auto und CarPlay auf einem originalen BMW NBT (QNX). Danach gibt es unter **Multimedia** den
Eintrag **Android Auto / CarPlay**. Der Installer braucht nur SSH-Zugang zum NBT: IP-Adresse und root-Passwort.
Das Passwort wird nirgends gespeichert.

## Vorher: SSH-Zugang freischalten

Ab Werk nimmt das NBT keine SSH-Anmeldung an. SSH muss deshalb zuerst einmal gepatcht werden.

- **So hat es der Entwickler gemacht:** mit dem **FeatureInstaller**.
  - Das braucht teils **mehrere Anläufe**.
  - Der letzte Schritt meldet teils **Fehler, obwohl SSH geklappt hat**. Deshalb danach einfach die Verbindung testen,
    z. B. `ssh root@169.254.199.99`.
- **Root-Passwort:** steht in diesem Forum-Thread:
  [SSH access guide (supramkv.com)](https://www.supramkv.com/threads/ssh-access-guide-supra-nbtevo-headunit.24190/).
  Der Thread dreht sich um das NBT Evo im Toyota Supra, das root-Passwort steht dort.

Das Freischalten verändert die Konfiguration des Geräts. Wie alles hier geschieht es auf eigene Gefahr.

## Benutzung

1. Laptop per ENET-Kabel mit dem Auto verbinden, Zündung an. Die übliche NBT-Adresse ist `169.254.199.99`.
2. Installer starten:
   - Windows: `NBT-AndroidAuto-Installer.exe`
   - macOS: `NBT-AndroidAuto-Installer.app`. Beim ersten Start mit Rechtsklick → Öffnen, weil die App nicht signiert ist.
3. IP-Adresse und Passwort eingeben, dann **Installieren / Aktualisieren** klicken.
4. Das iDrive neu starten.
5. Telefon per USB anstecken und **Multimedia → Android Auto / CarPlay** wählen. Ein iPhone fragt beim ersten Mal
   „Diesem Computer vertrauen?“.
6. Für den Ton das Telefon **danach** per Bluetooth mit dem Auto verbinden (siehe unten).

## Was das Paket auf dem NBT kann

- **Android Auto** mit jedem Android-Telefon und **CarPlay** mit dem iPhone, beides über das USB-Kabel.
  Pro Start des NBT geht nur eine Art Telefon: Nach einem Android-Telefon läuft ein iPhone erst nach einem Neustart
  des NBT und umgekehrt. Die Seite zeigt dann „Erst NBT neu starten“.
- **Scharfes Bild:** 1280x480, jeder Bildpunkt des Displays kommt vom Telefon. Der Knopf **Decoder** auf der Seite
  schaltet zwischen Hardware (Decoder des Autos, Standard nach der ersten Installation) und Software um. Der Wechsel
  gilt nach einem Neustart des NBT.
  - Bleibt das Bild auf deinem Auto aus: auf dem NBT die Datei `/var/opt/hmi/data/rw/NbtAndroidAutoRT/decoder.off`
    anlegen und das NBT neu starten. Dann läuft der Bildweg der früheren Versionen (800x480). Bitte melden.
- **Ton über die Lautsprecher als „iPod über Bluetooth“:** Unter Multimedia erscheint ein Apple-Gerät
  **„Android Auto“** mit Titel, Interpret, Album, Cover und Länge, auch im Tacho. Weiter und Zurück gehen über iDrive
  und das Lenkrad. Das gilt für Android Auto und für CarPlay.
  - **iPhone (CarPlay):** das iPhone per Bluetooth mit dem Auto koppeln. Dann unter Einstellungen → Verbindungen →
    Bluetooth das iPhone wählen, OPTION drücken, **Gerät einrichten**, und **nur „Audio“** aktivieren (Telefon 1 und
    Telefon 2 aus). Am iPhone als Tonausgabe **CarPlay** wählen. Anrufe und Siri laufen dann über CarPlay und das
    Mikrofon des Autos. Bilder dazu stehen im [README](../README.md#sound).
  - Das Telefon muss sich per Bluetooth verbinden, **während Android Auto schon läuft**: erst USB anstecken, dann
    Bluetooth verbinden.
  - War Bluetooth schon vorher verbunden (z. B. beim Einsteigen), gibt es zunächst nur den normalen Bluetooth-Eintrag
    ohne Android-Auto-Ton. Dann Bluetooth am Telefon einmal aus- und wieder einschalten.
  - Ohne Bluetooth-Verbindung gibt es keinen Android-Auto-Ton.
  - Bisher nur mit einem Auto, einem Android-Telefon (OnePlus 7 Pro) und einem iPhone (iPhone 17 Air) getestet.
  - Abschalten: auf dem NBT die Datei `/var/opt/hmi/data/rw/NbtAndroidAutoRT/iap.off` anlegen. Ein erneutes
    „Installieren / Aktualisieren“ schaltet den Ton wieder ein.
- **Bedienung** mit dem iDrive-Controller (drehen, drücken, kippen, BACK). Die Skip-Tasten am Radio steuern die
  Android-Auto-Musik.
- **Navigation im Tacho** (6WB-Kombiinstrument), solange Google Maps oder Apple Karten eine Route führt:
  - Pfeil, Entfernung mit Balken und Straße,
  - bei Android Auto zusätzlich Autobahn- und Bundesstraßenschild und Fahrspuren.

  Sobald die BMW-Navigation selbst führt, hat sie Vorrang.
- **Android Auto neu starten** (Knopf auf der Seite) startet nur den Dienst neu, nicht das NBT. Vorher wird ein
  Diagnose-Schnappschuss gespeichert. Den holt **Diagnose-Logs speichern** mit ab.
- **NBT neu starten** (Knopf auf der Seite) startet nach einer Rückfrage das ganze NBT neu. Dafür braucht die Seite
  die Adresse des Gateways im Auto. Im Installer gibt es dafür das Feld **Gateway** mit dem Knopf **Suchen**: Der
  Knopf sucht das Gateway im Auto und trägt die gefundene Adresse ein. Bleibt das Feld leer, sucht der Installer
  beim Installieren selbst. Findet er nichts, steht das im Protokollfenster, und die Seite meldet „Gateway nicht
  erreichbar“. Dann die IP-Adresse des Gateways (zeigen z. B. ISTA, E-Sys oder der SSH-Patcher an) von Hand ins
  Feld schreiben und noch einmal installieren. Eine eingetragene Adresse ersetzt die im Auto gespeicherte.

Es empfiehlt sich, in iDrive den Split-Screen auszuschalten (OPTION → Split-Screen aus): Das Bild nutzt das ganze
Display.

**Deinstallieren** entfernt genau die beiden installierten Ordner. **Diagnose-Logs speichern** lädt die Protokolle als
ZIP herunter, z. B. für Fehlermeldungen.

## Was auf das NBT geschrieben wird

| Ort | Inhalt |
|---|---|
| `/var/opt/hmi/data/rw/NbtAndroidAutoRT/` | Dienst, Bild (`nbt_decoder`), CarPlay (`nbt_carplay`, Kopplung in `carplay/`), Neustart (`nbt_reset`, `gateway`), Tacho-Navigation, Ton (`nbt_aa_wms`, `nbt_ipod_tap`, `nbt_aa_ipod`, `iap.sh`), Tastenbelegung `mfl.map`, Zertifikate, `boot.sh`, `restart.sh`, `vid.dpi`, `decoder.hw`, `logs/`, `journal/`, `snapshots/` |
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

## Credits und Lizenzen

Der Knopf **Credits / Lizenzen** im Installer zeigt, von wem welcher Teil stammt:

- Scharfes Bild mit Hardware-Decoder (`nbt_decoder`), CarPlay (`nbt_carplay`) und NBT-Neustart (`nbt_reset`) stammen
  von **Valentin Radu** ([valinet](https://github.com/valinet/nbt-android-auto)). Er hat diesem Projekt erlaubt, seine
  Programme in den Releases mitzuliefern. Der volle Text seiner Erlaubnis liegt als
  `PERMISSION-valinet-binary-releases.txt` im Paket und wird mit auf das NBT geschrieben.
- Der Android-Auto-Kern und der Controller-Code stammen aus **NBT-AAD** von nanaisu.

Die fertigen Dateien eines Release stehen unter der PolyForm Noncommercial License 1.0.0: keine kommerzielle Nutzung.
Der Quelltext liegt auf <https://github.com/JayJay3071/nbt-android-auto>, Einzelheiten in `NOTICE.md`.
