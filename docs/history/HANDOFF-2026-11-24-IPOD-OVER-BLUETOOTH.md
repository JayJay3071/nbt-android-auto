# Handoff 2026-11-24: Android Auto als „iPod über Bluetooth“ in iDrive

## Was jetzt geht (im Auto bestätigt am 2026-11-24)

Unter Multimedia erscheint ein eigener Eintrag **„Android Auto“** als Apple-Gerät, und zwar nur mit dem OnePlus. Es braucht kein iPhone und nichts im Snap-in.

- **Ton:** Die Android-Auto-Musik kommt über USB und wird vom eigenen Player des NBT abgespielt, ohne Aussetzer.
- **Anzeige:** Titel, Interpret, Album, Cover und Länge werden angezeigt und stimmen mit dem laufenden Song überein.
- **Bedienung:** Weiter und Zurück funktionieren über iDrive bzw. das Lenkrad.
- **Kombiinstrument:** Die Titelanzeige im Tacho funktioniert ebenfalls (bestätigt am 2026-11-24).
- Bild, Navigation und Bedienung von Android Auto laufen unverändert über USB.

## So verbindet man es

Das NBT nimmt den Apple-Eintrag nur an, wenn das Telefon sich per Bluetooth **verbindet, während unser Dienst schon läuft**. Dann liest der Vorschalter beim Verbinden die Geräteinfos mit.

**Weg 1, empfohlen:** Erst das OnePlus per USB anstecken, Android Auto startet. Danach verbindet sich das OnePlus per Bluetooth. Kurz warten, dann in iDrive „Android Auto“ wählen.

**Weg 2, Bluetooth war schon vorher verbunden:** Zum Beispiel weil das Telefon sich beim Einsteigen sofort verbunden hat. Dann ist zunächst nur der normale Bluetooth-Eintrag „OnePlus 7 Pro“ da, ohne Ton. In dem Fall bei laufendem Android Auto die Bluetooth-Verbindung **einmal trennen und neu verbinden**, zum Beispiel Bluetooth am OnePlus aus und wieder an. Danach erscheint „Android Auto“.

Ist das OnePlus nicht per Bluetooth verbunden, gibt es keinen Ton. Ein Rückfall auf den alten Player ist bewusst nicht eingebaut, das war der Wunsch des Nutzers.

## Wie es funktioniert

| Teil | Datei (Quelle) | Auf dem NBT | Aufgabe |
|---|---|---|---|
| Vorschalter Bluetooth-Audio | `src/android_auto/nbt_ipod_tap.c` | `$P/nbt_ipod_tap` | Sitzt vor `/fs/avrcp0` (Bluetooth-Audio-Treiber des Telefons) und reicht alles durch, außer: Geräteinfo `iap status="supported"` (Telefon gilt als iPhone), Ton-URL → `wms:/net/hu-intel.nbt//dev/wms/nbtaa`, Wiedergabestatus „spielt“, Titel-Ereignisse und Titeltexte aus Android Auto |
| Apple-Treiber | `src/android_auto/nbt_aa_ipod.c` | `$P/nbt_aa_ipod` | Belegt `/fs/btipod` (Platz für iPod über Bluetooth), liefert Titel, Interpret, Cover und Länge, nimmt Skip-Befehle an, beantwortet die Begrüßung 0x80 wie der echte Treiber (2 s abmelden, dann wieder da), meldet Änderungen alle 500 ms |
| Eigene Bluetooth-Tonquelle | `src/android_auto/nbt_aa_wms.c` | `$P/nbt_aa_wms` (vom Dienst als `nbt_a2i_play` gestartet) | `/dev/wms/nbtaa`: macht aus der Android-Auto-Musik (audio.fifo) SBC-Pakete wie eine echte A2DP-Quelle, mit 48 kHz, Qualitätsstufe 53, 200 ms Puffer und Tempoangleich |
| Startskript | `scripts/nbt-aa-iap.sh` | `$P/iap.sh` | Startet den Vorschalter und den Apple-Treiber, solange der Dienst läuft |
| Autostart | `scripts/nbt-aa-boot.sh` | `$P/boot.sh` (alt: `boot.sh.pre-iap`) | Im Apple-Modus bekommt der Dienst `nbt_aa_wms` statt des alten Players und startet `iap.sh` |

`$P` = `/var/opt/hmi/data/rw/NbtAndroidAutoRT`. Laufzeitordner: `/fs/ram3/nbt-aa-native`, mit den Logs `iap.log`, `btipod.log` und `sound.log`.

**Schalter:**
- `touch $P/iap.off`: Apple-Modus aus, beim nächsten Start gilt wieder der alte Weg (Ton auf dem Bluetooth-Eintrag, ohne Titel).
- `touch $P/iap.verbose`: ausführliche Logs (`iap-tap.log`, `btipod.log`). Nur zur Fehlersuche, die Logs wachsen schnell.

**Installieren bzw. aktualisieren:** `powershell -ExecutionPolicy Bypass -File scripts\Start-NbtBtIpodTest.ps1 iapinstall` lädt die drei Programme, `iap.sh` und `boot.sh` hoch. Aktiv wird es beim nächsten Dienst- bzw. NBT-Start.

## Was dafür herausgefunden wurde (wichtig für später)

1. **Begrüßung 0x80:** Aufgenommen mit einem echten iPhone über Bluetooth. Der echte Treiber meldet sich ab, antwortet und ist nach 2 s wieder da. Das ist auch die Erklärung für das „doppelte Einstecken“ beim USB-Weg.
2. **Apple-Platz nur für Apple-Geräte:** Die Media-App nimmt `/fs/btipod` nur an, wenn die Geräteinfo des Bluetooth-Audio-Treibers (devctl 0x0e8b) `<iap status="supported"/>` enthält. Beim OnePlus steht dort `not_supported`, der Vorschalter ändert genau diese Zeile.
3. **Ton:** Der Player des NBT (io-nme) spielt für diesen Eintrag nur „wms:“-Geräte ab, keine HTTP-Adressen. Das Format von `/dev/wms/player1` ist aufgenommen: Statusabfrage devctl 0x40200520 mit 32 Byte, `read()` liefert 1 Byte Kopf plus 8 SBC-Rahmen. Den SBC-Encoder habe ich selbst geschrieben und offline geprüft: Filterbank 64 dB, Prüfsumme wie bei echten Rahmen, Rauschabstand 65 dB.
4. **Titel:** Sie kommen über den Apple-Treiber. Der Player braucht dafür Benachrichtigungen auf `.FS_info./control` (alle 500 ms bzw. bei jeder Änderung). Die Anzeige nimmt einen Listeneintrag relativ zur Statusnummer. Deshalb liefert jeder Eintrag der Titelliste den laufenden Song.
5. **Ereignisformat des Bluetooth-Audio-Treibers (0x0e37):** aufgenommen und im Vorschalter nachgebaut, steht im Quelltext. Für die Anzeige war es am Ende nicht nötig.

Aufnahmen: `.local/car-logs/btipod-20261123-2311` (echtes iPhone), `wms-20261123` (AAC), `wms-sbc-20261123` (SBC), `btipod-2026112*` (unsere Läufe).

## Offen

- **Weg 2** (Bluetooth trennen und neu verbinden) ist nötig, wenn sich das Telefon vor dem Start des Dienstes verbindet. Abhilfe wäre, dass der Dienst früher startet oder die Bluetooth-Verbindung selbst neu anstößt. Beides ist nicht gebaut.
- `scripts/Restore-NbtAaInstall.ps1` kennt den Apple-Modus noch nicht. Nach einem Wipe also zusätzlich `iapinstall` ausführen.
- Nichts davon ist in git committed.
