# Vorbereitung einer spaeteren NBT-Uebertragung

Status: NBT-Uebertragung und NBT-Ausfuehrung NICHT freigegeben, NICHT durchgefuehrt.

VM-Vorpruefung am 2026-11-05 erfolgreich: `20261105T172354Z-66cc61c1`.
Artefakt: `build/20261105T172354Z-66cc61c1/hello_nbt`.
ELF32/i386, Interpreter `/usr/lib/ldqnx.so.2`, benoetigte Bibliothek `libc.so.3`.
VM-Ausfuehrung als UID 100, Exitcode 0. SHA256 im zugehoerigen Manifest.
ELF-Pruefprotokoll liegt daneben als `elf-inspection.txt`.

Vor einer Uebertragung muss ein VM-Build mit passendem `manifest.json` vorliegen:
QNX 6.5.0, gcc_ntox86, ELF32/i386, Programmausgabe und Exitcode erfolgreich.
Zusaetzlich ELF-Interpreter und Abhaengigkeiten mit den QNX-Binutils pruefen
(readelf/objdump der installierten Toolchain). VM-Erfolg beweist noch keine
ABI-Kompatibilitaet mit der Fahrzeug-Laufzeit.

Nach Nutzerfreigabe zuerst NBT-Adresse, SSH-Benutzer, Hostschluessel,
freien Platz und ein vorhandenes beschreibbares temporaeres Verzeichnis
feststellen. Keine Adresse aus alten Fahrzeuglogs automatisch uebernehmen.
Fuer das NBT eine eigene SSH-Konfiguration und einen getrennten Host-Key-Eintrag
verwenden. Das VM-Skript bleibt fest auf 192.168.56.10 beschraenkt.

Nur `hello_nbt` in ein neues Unterverzeichnis des bestaetigten temporaeren
Zielpfads kopieren, danach Pruefsumme vergleichen. Keine Laufzeitbibliotheken
oder Systemdateien ersetzen. Noch nicht ausfuehren.

Ein anschliessender Konsolentest benoetigt ausdrueckliche Nutzerfreigabe fuer
die Ausfuehrung. Fahrzeug steht; ein einmaliger Programmstart ohne root-Rechte,
soweit moeglich. stdout/stderr und Exitcode erfassen. Keine Display-Uebernahme,
kein Autostart und kein Eingriff in HmiMain3D, WalServer oder DisplayManagerScreen.
Danach nur die selbst angelegten Testdateien gezielt entfernen.

Screen/EGL/OpenGL ES sowie ein moeglicher Android-Auto-Receiver sind separate
spaetere Arbeitspakete. Die mitgelieferten Bibliotheken ersetzen keine passenden
Header, Protokollimplementierung oder Tests auf dem Originalsystem.
