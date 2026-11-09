# Übergabe: Veröffentlichung auf GitHub (Stand 8.11.2026)

Ziel: das Projekt der Community auf GitHub bereitstellen, mit Installer-App (Windows + macOS) als Release-Download.
Diese Notiz ist die Aufgabenbeschreibung für die nächste Sitzung. Sie enthält alles, was dafür aus dem bisherigen
Verlauf wichtig ist.

## Was funktioniert (am Auto bestätigt)

Getestet an einem BMW F07 (2014) mit originalem NBT und einem OnePlus 7 Pro.

- **Android Auto per USB:**
  - Kabelgebunden, ohne Zusatzhardware.
  - Vollbild 800x480 in Hardware-Video (Cinemo NmeDeviceScreen + VideoApp über MOST), ca. 23 fps, ca. 27 % CPU.
  - DPI 100 (`vid.dpi`).
- **iDrive-Controller:**
  - Drehen = Rotary.
  - Kippen = DPAD.
  - Drücken = OK, BACK = Zurück.
  - MENU / MEDIA / NAV / COM / OPTION verlassen AA, das im Hintergrund weiterläuft.
- **Menüeintrag Multimedia → Android Auto:**
  - Zeigt den Status.
  - Startet den Dienst automatisch, 15 s nach dem HMI-Start.
  - Holt ein laufendes AA sofort nach vorne.
- **Halte-Logik:** 8 verschachtelte Haltezustände, höchstens 6 Fokus-Events pro Minute. Damit ist der Fehler
  „verbunden, aber kein Bild bis zum Neustart“ behoben: 30 min Test ohne Ausfall.
- **Beliebige Android-Telefone:** keine telefonspezifische Konfiguration. TLS „chain“ akzeptiert jede Google-Automotive-Link-Kette.

## Bekannte Einschränkungen (gehören in die README)

- Kein Ton über AA. Musik läuft über Bluetooth, Anrufe gehen. Plan: docs/AA-AUDIO-PLAN.md.
- Auflösung 800x480: Die Hardware-Aufnahme (dev-nvideocapture) ist auf WVGA begrenzt. 720p wurde getestet: nicht
  schärfer, mehr Eingabeverzögerung.
- Split-Screen in iDrive besser ausschalten (OPTION → Split-Screen aus).
- Ein Menüpunkt „System neu starten“ wurde gebaut, tat am Auto aber nichts. Er ist wieder entfernt. Ursache ungeklärt,
  Kandidaten: Button-Übergang, Uptime-Schutz < 120 s, `shutdown` aus dem HMI-Prozess. Nicht wieder einbauen ohne Logs.
- Getestet nur mit einem Fahrzeug und einem Telefon. Andere NBT-Softwarestände (I-Stufen) und NBT Evo sind ungetestet.
  NBT Evo hat eine andere DSI-Bibliothek.
- Voraussetzung: SSH-Root-Zugang zum NBT. Der Installer richtet diesen nicht ein.

## Installer (neu, in `installer/`)

- **Programm:** `nbt_aa_installer.py`
  - Tk-Oberfläche (IP, Port, Benutzer, Passwort) und CLI.
  - paramiko, nur exec-Kanäle (kein SFTP nötig).
  - Funktionen: Installieren/Aktualisieren, Deinstallieren, Diagnose-Logs als ZIP.
  - Sprache automatisch Deutsch/Englisch.
  - Prüft vorab QNX, `/var/opt/hmi/data/rw`, NmeCmdLine, Cinemo-XML, DSI-Bibliothek, DisplayManager und ein
    eventuell installiertes NbtAad.
  - Jede Datei: `.tmp` → zurücklesen und vergleichen → chmod → mv. Das Plugin-Manifest kommt zuletzt.
- **Payload:** `make_payload.py` sammelt die Payload nach `installer/payload/` (+ manifest.json mit SHA-256).
  - Quellen: `build/aa-entry-20261106/` (Dienst, Knob, hwvideo, Decoder, Plugin) und
    `build/native-prototype-20261106/package/` (accessory_window, fifo_http, PEMs).
  - Bricht ab, wenn `libNbtAndroidAuto.so` älter als `aa_entry_plugin.cpp` ist.
- **Builds:**
  - `build_windows.ps1` → `installer/dist/NBT-AndroidAuto-Installer.exe` (Onefile, ca. 18 MB, Fenster startet, getestet).
  - `build_mac.sh` → .app, muss auf einem Mac gebaut werden, unsigniert. Alternativ `python3 nbt_aa_installer.py`.

**Stand 8.11.2026:**
- Das Plugin ist ohne Neustart-Code neu gebaut (18085 Byte, Logik wie im 30-min-Test).
- Die Payload ist Version 2026.11.08. installer/dist/NBT-AndroidAuto-Installer.exe ist mit dieser Payload und
  paramiko 3.5.1 gebaut.
- In der QNX-VM per --root-Testordner getestet: Installieren, Aktualisieren (15 s), Logs-ZIP und Deinstallieren
  laufen fehlerfrei, oot.sh besteht sh -n, es bleiben keine .tmp-Reste.
- Am echten Auto noch nicht gelaufen. Der Nutzer tippt das Passwort selbst ein.

**Wichtige Funde:**
- **paramiko muss < 4 sein:** Das NBT (wie die VM) hat OpenSSH 5.2 und bietet nur ssh-rsa-Hostschlüssel an.
  paramiko 4+ lehnt das ab („Incompatible ssh peer“). Festgelegt in installer/requirements.txt und den Build-Skripten.
- **Smart App Control blockiert die unsignierte .exe:** Windows 11 mit eingeschaltetem Smart App Control (auf diesem
  PC aktiv) meldet „Eine Anwendungssteuerungsrichtlinie hat diese Datei blockiert“.
  - Für das Release ist eine Codesignatur nötig (z. B. kostenlos über SignPath Foundation für Open Source, oder ein
    eigenes Zertifikat).
  - Sonst bleibt nur der Python-Weg. Im README erklären. Den Nutzern nicht empfehlen, Smart App Control abzuschalten.
- **macOS:** Die App ist unsigniert und nicht notarisiert. Gatekeeper verlangt Rechtsklick → Öffnen. Für eine
  saubere Verteilung braucht es einen Apple Developer Account.

Plugin-Build in der VM (192.168.56.10, Benutzer nbtbuild). Nach jedem VM-Start an der VM-Konsole als root:

```
slay dhcp.client; ifconfig en0 192.168.56.10; /usr/sbin/sshd
```

Dann:

```
cd ~/.nbt-build/aa-entry-20261106
qcc -V4.4.2,gcc_ntox86_cpp-ne -O2 -Wall -Wextra -shared -fPIC -fno-exceptions -fno-rtti -Wl,-z,defs -I. \
  aa_entry_plugin.cpp -o libNbtAndroidAuto.so -L$HOME/.nbt-build/idrive-hello-20261105 -lAppFramework.A -lCoreFramework.A
```

QNX-Umgebung: `QNX_HOST=/usr/qnx650/host/qnx6/x86`, `QNX_TARGET=/usr/qnx650/target/qnx6`, `PATH=$QNX_HOST/usr/bin:$PATH`.

## Blocker und Pflichtpunkte vor dem Veröffentlichen

1. **Lizenz:** Teile stammen aus NBT-AAD (nanaisu, https://github.com/Sq00ky/Supra-iDrive6-Android-Auto) unter
   PolyForm-Noncommercial-1.0.0:
   - `src/android_auto/knob/ref/`,
   - `aa_cryptor_mbedtls.cpp`, `accessory_window.c`, `oneplus_aoa.c`, `tls_usb_probe.cpp`, `aa_cryptor.hpp`.

   Folgen:
   - Das Gesamtprojekt kann damit nur nichtkommerziell veröffentlicht werden. Lizenztext und alle
     „Required Notice:“-Zeilen müssen mitgeliefert werden (`src/android_auto/LICENSE.NBT-AAD.md`).
   - Für die eigenen Teile eine kompatible Lizenz wählen, oder dasselbe PolyForm NC.
   - Für mbedTLS (Apache-2.0) den NOTICE-Hinweis ergänzen.
   - Der Nutzer entscheidet.
2. **Head-Unit-Identität:** `cert.pem` / `key.pem` / `ca.pem` sind die öffentliche Identität aus NBT-AAD und kommen
   mit in die Payload.
   - Herkunft und Lizenz prüfen.
   - Im README offenlegen.
   - Nicht im Git-Baum ablegen (`.gitignore` hat `*.pem`), nur im Release-Asset.
3. **Proprietäres nicht veröffentlichen:**
   - `.local/` enthält Dumps von BMW/NVIDIA/Cinemo-Bibliotheken (z. B. `aa-libEGL-root.so`, `aa-libwal_nvidia.so`,
     `aa-NmeCmdLine`), Fahrzeugmitschnitte, SSH-Konfigurationen, known_hosts und Schlüssel-Pfade. Komplett ausschließen.
   - `build/` ebenfalls ausschließen.
   - Das QNX SDP 6.5 ist lizenzpflichtig: nur beschreiben, nicht beilegen.
4. **Persönliche Daten:** Vorname des Entwicklers und lokale Pfade (z. B. Schlüsselpfade) in (erledigt 9.11.2026):
   - `HANDOFF.md`,
   - `scripts/Upload-NbtAaTest.ps1`,
   - `scripts/Update-NbtAaPackage.ps1`.

   Durch Parameter oder Platzhalter ersetzen. Fahrzeug-IP 169.254.199.99 ist der NBT-Standard und darf bleiben.
5. **Git:** Im Projektordner ist `git` auf diesem PC nicht im PATH.
   - Repo-Zustand prüfen.
   - Vor dem ersten Push die Historie auf versehentlich eingecheckte Binaries, Schlüssel und `.local`-Inhalte
     durchsehen. Notfalls frisches Repo mit sauberem Stand.
6. **Haftung/Sicherheit im README:** Eingriff ins Head-Unit, keine Gewähr, Bedienung während der Fahrt nur im Rahmen
   der StVO. Keine BMW-Marken oder -Logos als Projektlogo verwenden.

## Vorschlag für die Repo-Struktur beim Veröffentlichen

- `README.md`, neu für Endnutzer:
  - Was es ist und welche Voraussetzungen gelten.
  - Download des Installers, Bedienung, bekannte Einschränkungen, Deinstallation, Logs für Bug-Reports.
- `installer/`: Quelle des Installers. Die Payload wird nicht eingecheckt.
- `src/`, `scripts/`, `docs/`: technische Doku. Veraltete Experimente (WAL-Sprite-Pfad, PNG-Fallback, Hello-Plugins)
  in `docs/history/` verschieben oder streichen.
- GitHub Release: `NBT-AndroidAuto-Installer.exe`, macOS-.app als ZIP, `payload.zip` (für den Python-Weg),
  SHA-256-Summen.
- Optional später: GitHub Actions, das aus einem Release-Asset `payload.zip` die exe/app baut. QNX-Binaries kann CI
  nicht bauen, die kommen weiter aus der VM.

## Sicherheitsregeln aus dem bisherigen Projekt (weiter gültig)

- Das root-Passwort des Autos tippt nur der Nutzer, nie Claude.
- Am Auto nie io-nme oder HmiMain3D beenden oder neu starten. Keine reinen VM-Bibliotheken ins Auto kopieren.
- Den WAL-Sprite-Pfad nicht wieder aufnehmen: Er führte zu NVIDIA-Treiberabstürzen.
