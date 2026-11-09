# Validierung vom 2026-11-05

## Tatsaechlich durchgefuehrte Pruefungen

- Windows erreicht QNX ueber Host-only: 192.168.56.1 -> 192.168.56.10.
  Zwei Pings erfolgreich, jeweils unter 1 ms; MAC 08:00:27:a6:31:e7 stimmt.
- SSH-Server: OpenSSH_5.2 QNX_Secure_Shell-20090621-hpn13v6.
- RSA-3072-Hostschluessel anhand der lokalen VM-Konsole verifiziert.
  SHA256: LrZmBXlAHJWOqKwQmPCQTLIVaEY3wbUAEjnMEDUsOew.
- Hostschluesselpruefung bleibt strikt. Optionen fuer alte Algorithmen gelten
  ausschliesslich fuer den VM-Alias. Kein KEX-Downgrade erforderlich.
- Test des Pinning-Helfers: falscher Fingerprint wird ohne Vertrauensdatei
  abgewiesen; passender Fingerprint angenommen. Integration mit OpenSSH und
  Dateipfaden mit Leerzeichen erfolgreich.
- Passwortlose SSH-Schluesselanmeldung als nbtbuild erfolgreich:
  uid=100(nbtbuild), gid=100(nbtbuild). Gruppeneintrag ist vorhanden.
- Probe-Skript bestaetigt QNX 6.5.0/x86 und qcc 4.4.2,gcc_ntox86.
- Build-Skript kompiliert den C-Quelltext in der VM, startet ihn dort und holt
  das Artefakt mit Legacy-SCP ueber die verifizierte SSH-Verbindung zurueck.
- Ausgabe: Hello World from QNX x86!
- Exitcode: 0. Kein Root-Build, kein Zugriff auf Display oder Fahrzeugdienste.
- ELF-Pruefung: ELF32, little-endian, Intel 80386, dynamischer Interpreter
  /usr/lib/ldqnx.so.2, einzige NEEDED-Bibliothek libc.so.3.
- PowerShell-Syntaxpruefungen und git diff --check erfolgreich.

## Artefakt

Lauf-ID: 20261105T172354Z-66cc61c1

Windows: build/20261105T172354Z-66cc61c1/hello_nbt (5239 Bytes).

VM: /home/nbtbuild/.nbt-build/20261105T172354Z-66cc61c1/hello_nbt.

SHA256:
9B774C03687721B313FA00DF3D83D633193027FEEF34E078E45E7D679E16C84C

Quelltext-SHA256:
B9C18C82D312A223E36B1CBD84442F2D65CCCC5BE732957242EEFEE112685344

manifest.json, vm-test.txt und elf-inspection.txt liegen neben dem Artefakt.
Build-Ausgaben und lokale SSH-Konfiguration sind von Git ausgeschlossen.

## Grenzen des Nachweises

Kein SSH-Zugriff, keine Uebertragung und keine Ausfuehrung auf dem BMW NBT.
VM-Erfolg belegt noch keine Laufzeitkompatibilitaet mit der Headunit.
Der Transferplan steht in NBT-TRANSFER.md; ein Fahrzeugtest braucht weiterhin
separate Nutzerfreigabe.

Die QNX-IP wurde zur Laufzeit gesetzt und sshd manuell gestartet. Es wurden
keine Bootprofile geaendert. Nach einem VM-Neustart koennen beide Schritte
an der Konsole erneut erforderlich sein.
## Windows-PowerShell-Launcher

Nachpruefung: 20261105T173241Z-214504aa.
`build.cmd` startet das Skript mit prozesslokalem RemoteSigned. Der echte Build,
VM-Test und Artefaktdownload liefen mit Exitcode 0 durch. Die .NET-SHA256-Werte
im Manifest wurden unabhaengig mit Get-FileHash in PowerShell 7 verglichen.
Nach dem Test war die Windows-PowerShell-Standardrichtlinie weiterhin Restricted,
und alle gespeicherten Richtlinien-Scope-Werte waren unveraendert Undefined.