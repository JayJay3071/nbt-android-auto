# Umzug auf den Windows-11-Laptop

Dieses Paket enthaelt den Arbeitsstand vom 2026-11-05. Die QNX-VM wurde
im ausgeschalteten Zustand kopiert. Ihre Festplatte ist bytegleich zur Quelle.
Der Laptop selbst wurde noch nicht eingerichtet oder getestet.

## 1. Paket pruefen und lokal kopieren

- Auf dem USB-Stick `verify-package.cmd` starten. Erwartet: Package integrity OK.
- Den Ordner `NBT-Project` einschliesslich der versteckten `.git`- und `.local`-
  Ordner nach `C:\NBT-Project` auf dem Laptop kopieren. Kein vorhandenes Projekt
  ueberschreiben; im Zweifelsfall einen neuen Zielordner verwenden.
- Den kompletten Ordner `QNX-VM` auf die interne Laptop-SSD kopieren, z.B. nach
  `C:\Users\<Laptop-Benutzer>\VirtualBox VMs\QNX`.
- Originalmaterial und Installation ebenfalls aufbewahren. Die VM nicht direkt
  vom USB-Stick betreiben. Als Reserve das unveraenderte USB-Paket behalten.
- Die Paketpruefung vor dem Einrichten ausfuehren: spaetere Builds, neue
  Schluessel und VirtualBox aendern absichtlich die Arbeitskopie.

## 2. VirtualBox und VM

Dieser Weg setzt einen Intel-/AMD-x64-Laptop voraus. Bei einem Snapdragon-/ARM-
Laptop zuerst die Architektur klaeren; diese x86-VM kann dort nicht einfach
mit VirtualBox uebernommen werden.

Im Ordner Installation liegt der bereits vorhandene, Oracle-signierte Installer
VirtualBox-7.2.22-175245-Win.exe. Die Authenticode-Signatur wurde auf dem Quell-PC
als gueltig geprueft. Installation bei Bedarf auf dem Laptop selbst starten.
Die alte 5.2.44-Datei liegt nur zur Reproduzierbarkeit bei; sie ist keine pauschale
Empfehlung fuer eine neue Windows-11-Installation. Der bisherige VM-Build wurde
unter 5.2.44 getestet, der Gaststart unter 7.2 auf dem Laptop steht noch aus.
Das Extension Pack und Guest Additions sind fuer diesen SSH-Workflow nicht noetig.

In VirtualBox die kopierte Datei `QNX.vbox` ueber Hinzufuegen/Oeffnen registrieren.
Nicht die Datei QNX-original.vbox.reference.txt verwenden.

Die Kopie bewahrt die bisherige virtuelle Hardware:
- 1 CPU, 1024 MB RAM, BIOS, 32-Bit-Gast, IDE-Festplatte.
- Netzwerkkarte PCnet-FAST III / Am79C973, MAC 08:00:27:a6:31:e7.
- Bestehende weitere Hardwareeinstellungen beibehalten.
- Der bisherige VM-Uhrenoffset ist in der Kopie erhalten; nicht beilaufig aendern.

Nur ein veralteter, nicht eingelegter ISO-Verweis auf den alten Windows-
Benutzernamen wurde aus der kopierten .vbox entfernt. Die Originalkonfiguration
liegt daneben als Referenz; die Original-VM auf dem Desktop blieb unveraendert.

## 3. Host-only-Netz vor dem VM-Start

Auf dem Laptop in VirtualBox einen Host-only-Adapter anlegen/auswaehlen:
- Windows-Adapter: 192.168.56.1, Netzmaske 255.255.255.0.
- QNX-Netzwerkkarte 1 an diesen Adapter binden, Kabel verbunden.
- Keine NAT- oder Bridged-Karte hinzufuegen. Weitere VM-Karten bleiben aus.
- Kein Internet-Sharing und keine Netzwerkbruecke zum Fahrzeugadapter einrichten.
- Ein vorhandener DHCP-Pool darf 192.168.56.10 nicht vergeben.

Der Adaptername auf dem Laptop kann anders lauten; deshalb die Zuordnung in
der VM vor dem ersten Start pruefen. Ein kopierter Name erzeugt keinen Adapter.

## 4. QNX starten

Die bestehenden Benutzer `root` und `nbtbuild` und ihre Passwoerter bleiben in
der VM erhalten. Es werden keine Passwoerter in dieses Dokument geschrieben.

An der QNX-Konsole als root anmelden und eingeben:

    ifconfig en0 192.168.56.10
    /usr/sbin/sshd

Die Meldung zum fehlenden DSA-Hostschluessel trat bereits am Desktop auf; der
RSA-Server funktionierte trotzdem. Keinen neuen RSA-Hostschluessel erzeugen.
Wenn SSH bereits lauscht, muss kein zweiter Server gestartet werden.
IP und sshd wurden bisher nicht in Bootdateien eingetragen.

Windows-Test:

    ping 192.168.56.10

## 5. Neuer SSH-Clientschluessel auf dem Laptop

In `C:\NBT-Project` die Datei `setup-laptop.cmd` starten.
Das Skript prueft den mitgenommenen oeffentlichen VM-Hostschluessel, erzeugt einen
neuen RSA-3072-Clientschluessel unter `%LOCALAPPDATA%\NBT-Project\ssh`, begrenzt
den Ordnerzugriff auf den angemeldeten Windows-Benutzer und erstellt die lokale
SSH-Konfiguration mit dem richtigen Laptop-Benutzerpfad.
Der dedizierte Automationsschluessel hat keine Passphrase; deshalb sind die
Dateirechte wichtig. Private Clientschluessel werden nicht per USB uebertragen.

Einmal das Passwort von `nbtbuild` in der lokalen Windows-Abfrage eingeben.
Das Skript speichert kein Passwort. Es ergaenzt nur den neuen oeffentlichen
Schluessel in der VM und testet die Anmeldung. Root-SSH ist dafuer nicht noetig.
Ein bereits vorhandener Clientschluessel wird nicht ueberschrieben.

Die .local-Datei im Paket enthaelt ausschliesslich den geprueften oeffentlichen
Hostschluessel. Erwarteter Fingerprint:
SHA256:LrZmBXlAHJWOqKwQmPCQTLIVaEY3wbUAEjnMEDUsOew

Falls der erste Verbindungsversuch nach der Schluesselerzeugung scheitert, die
Ursache beheben und dann die Einrichtung wiederaufnehmen mit:

    powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File C:\NBT-Project\scripts\Connect-QnxBootstrap.ps1

OpenSSH Client muss als Windows-Feature vorhanden sein. Node.js ist fuer diesen
Umzug mit bereits verifiziertem Hostschluessel nicht erforderlich. Git ist nur
fuer die weitere Versionsverwaltung noetig; die komplette Historie liegt bereits
im .git-Ordner. Es wird kein altes Benutzerprofil kopiert.

## 6. Echten Build erneut testen

    C:\NBT-Project\build.cmd

Erwartet: Hello World from QNX x86!, NBT_VM_TEST_OK, Exitcode 0 und ein neues
Verzeichnis unter build mit hello_nbt und manifest.json. Erst dieser Test
bestaetigt den kompletten Workflow auf dem Laptop.

## 7. Stand am Fahrzeug

Noch kein Test, keine Uebertragung und keine Aenderung auf dem BMW NBT.
Das Ziel ist das originale HU_NBT (F07, 2014), NICHT NBT EVO.
QNX Neutrino 6.5.0, Intel x86 32 Bit.
Keine Firmware- oder Bootaenderungen und keine Fahrzeugdienste beenden.
Eine rein lesende NBT-Pruefung wurde als naechster Schritt besprochen; aktuelle
NBT-IP und Benutzer sind noch offen. Uebertragung/Ausfuehrung brauchen weiterhin
eine ausdrueckliche Freigabe. Die VM-IP ist nicht die NBT-IP.

## Inhalt und Umgang

NBT-Project: Git-Historie, Quelltext, Build-Skripte und erfolgreiche Build-Artefakte.
QNX-VM: installierte Entwicklungs-VM mit qcc, Benutzerkonten und SSH-Serveridentitaet.
Originalmaterial: unveraendertes BMW-NBT-Gesamtpaket.zip einschliesslich Referenzdateien.
Installation: vorhandene QNX-ISO und signierte VirtualBox-Installer.

Das Paket ist privat: die VM enthaelt installierte Software, Passwort-Hashes
und ihren privaten SSH-Server-Hostschluessel. Das Original-ZIP enthaelt auch
proprietaere Fahrzeugbibliotheken und Diagnoseprotokolle. Nicht oeffentlich
hochladen. Plaintext-Passwoerter und der Windows-Clientprivatschluessel wurden
nicht hinzugefuegt. Die alten Client-Public-Keys bleiben in der VM erhalten.

Quellen:
- https://docs.oracle.com/en/virtualization/virtualbox/7.2/user/installation.html
- https://docs.oracle.com/en/virtualization/virtualbox/7.2/user/Introduction.html
- https://www.oracle.com/virtualization/technologies/vm/downloads/virtualbox-downloads.html
- https://www.qnx.com/developers/docs/6.5.0SP1/neutrino/utilities/s/shutdown.html