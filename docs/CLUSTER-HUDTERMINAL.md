# Tacho (6WB) – Navi-Grafik über MOST „HUDTerminal“ (Stand 8.11.2026)

## Ergebnis
- Der 6WB-Kombi ist **kein Display des NBT**. `/etc/system/config/nvkddisplay.conf` kennt nur das LVDS-Panel 1280x480 (CID),
  und auf der Intel-Seite gibt es nur `DisplayManager:0`.
- Die Navi-Grafik im Tacho zeichnet der **Kombi selbst**. Das NBT steuert ihn über ein Grafik-Terminal am MOST-Bus fern.
  Gerät **0x0101**, FBlock **0x82**, Instanz 00 (es gibt auch 82/02, vermutlich HUD).
- Wer sendet: `NavNBTHighController` (CGuidanceViewerIcHud / CIcHudComm, Logik-Gerät `CHUDTerminalLogDev`).
  Lokaler Client 0x2033 empfängt die Quittungen.
- Die Kreuzungsansicht im 6WB (FPK, „TopFrame 11–13“) besteht aus Polygonen, Linien, Kreisen und Bitmaps
  (NBT_RLE32 Bosch / NBT_RLE16 Conti). Dazu kommt der „Carsor“, der fahrende Positionspfeil. Gezeichnet wird
  doppelt gepuffert: Es wird in eine Instanz gezeichnet, während die andere sichtbar ist, dann wird umgeschaltet.

## Funktionen von 82/00
FktIDs-Antwort, 12-Bit-Paare Start/Ende:

```
000-003, 200-202, 203-206, 20E-20F, 210-212, 215-216, 218-21A, 21B-221, 226-228, 229-22A, 22B-22C,
247-248, 300-301, C01-C0A
```

Namen im Controller (HUDTerminal.*), Zuordnung zu Nummern noch nicht bewiesen:
- CreateInstance, ReleaseInstance, GetInstanceId, TerminalInterface
- ImageAddBitmap (segmentiert), ImageGetSize, ImageSetTextareaFormat, FrameAddItem
- PotentiometerSetRange/Value/Type
- ImageRemoveAllItems, ImageRemoveItem
- ImageAddPolygon, ImageSetFillMode/FillColor/LineWidth/LineColor
- ImageSetPositionRotation, ImageSetRotation, ImageAddCircle(Diameter)
- TextareaSetText, KOMBITextareaSetFormat
- SwitchVisibility, IsVisible, SetVisibility
- AnimationSetGuidingStep, CurrentGuidingStep

## Quittungen aus der Fahrt
`.local/gv-analysis/hudterminal-results-drive1.txt`, 1079 Zeilen. ResultAck op=D, tx=4. Die Nutzdaten wiederholen
die Parameter:

| fkt | Anzahl | Nutzdaten (Beispiel) | Deutung (Hypothese) |
|---|---|---|---|
| 21D | 400 | `inst16 item16 type8(02 Linie/03 Fläche) n8 n×(x16,y16)` | ImageAddPolygon |
| C06 | 188 | `inst16 x16 y16 0 0 0x0064 0` (x/y ändern sich laufend) | ImageSetPositionRotation (Carsor) |
| 227 | 123 | `inst16 0000 00xx 02` (xx fällt: fc, ec, e5, de …) | PotentiometerSetValue (Entfernungsbalken) |
| 210 | 82 | `inst16 00` | SetVisibility o. ä. |
| 220 | 67 | `inst16` | ImageRemoveAllItems o. ä. |
| 204 | 65 | `inst16 00/01` | Sichtbarkeit |
| 20E | 55 | `instA16 instB16` | SwitchVisibility (Puffer tauschen) |
| 219 | 41 | `inst16 u8` | Farbe / Linienstil |
| 21B | 27 | `inst16 u8` (04 / 0a) | LineWidth / FillMode |
| C09 | 16 | `inst16 item16 02 x16 y16 r16 …` | ImageAddCircle |
| C04/C05 | 15 | `inst16 …` | Eigenschaften (Guiding Step?) |

## Aktiv bestätigt am Auto (8.11.2026, im Stand, Route aktiv)
- Senden über `nbt_knob --send 0101,82,00,<fkt>,6,4,1,0,FFFF,<hex>`: OpType 6 (StartResultAck), tx=4 (tx=0 geht auch),
  Nutzdaten = Instanz (u16) + Parameter wie in den Quittungen. Die Quittungen gehen an den NBT-Client 0x2033.
- `204 [inst16][00|01]` blendet aus bzw. ein: Instanz 0x17 schaltet die gesamte Navi-Grafik im Tacho aus und wieder an.
  Bestätigt.
- `21D [inst][item16][03=Fläche|02=Linie][n][n×(x16,y16)]` in Instanz 0x1F/0x20 (Kreuzungsansicht) zeichnet über die
  Navi-Grafik: Quadrat 0x30..0x90 plus X 0x10..0xB0. Bestätigt.
- `C06` (Fahrpfeil 0x25) mit alter Position wird quittiert.
- Instanzen beim Routenstart: 0x17 (Gesamtcontainer), 0x18/0x19 (C04/C05), 0x1A–0x1C (210/204), Paare 0x1D/0x1E,
  0x1F/0x20, 0x21/0x22, 0x23/0x24 (Doppelpuffer: zeichnen, `20E [alt][neu]`, `220 [alt]` leeren), 0x25 Fahrpfeil,
  0x35 Entfernungsbalken (`227`).

## Farben, Flächen, Original-Design (8.11.2026 abends)
- `21B [inst][c]` = Farbe/Stil der folgenden Elemente. Werte 0–19 gültig (ab 20 wird das Element verworfen). Palette:
  Weiß, Creme, Grautöne, Schwarz, Rot, teils durchsichtig. **Kein Blau/Gelb.** Die Farben folgen vermutlich dem
  Kombi-Design (Komfort/Sport/Eco).
- `219 [inst][c]` ändert die Füllfarbe nicht, vermutlich Linienbreite (NBT nutzt 0x0A/0x0E/0x16).
- Instanz 0x1F/0x20 ist **perspektivisch** (Straßenebene, oben schmaler). Der obere Bereich (y < ~70) liegt hinter dem
  Horizont und ist unsichtbar.
- Polygone in 0x1D/0x21/0x23 waren nicht zu sehen. Vermutlich Bitmap-/Textflächen bzw. andere Größe.
- Original (Foto des Nutzers): oben eine flache Zeile mit nächstem Manöver-Symbol, Straßenschild (gelb „45“ =
  Bundesstraße) und Straßenname/Ausfahrt (Text), unten der große Pfeil als **Umriss**. Schilder = Bitmap (RoadIcon,
  `ImageAddBitmap`, NBT_RLE32), Text = `TextareaSetText` (CommingRoad). Beide werden vermutlich nicht quittiert und sind
  deshalb in den Mitschnitten nicht aufgetaucht.
- Mitschnitt ab NBT-Start eingerichtet: Flag `$P/trace-most` lässt boot.sh 4 min `nbt_knob --trace --filter 82` nach
  `$P/logs/most-boot-82.txt` schreiben. Ziel: CreateInstance-Quittungen mit Typ, Position und Größe aller Flächen.
- Achtung: `nbt_knob --trace` leitet Controller-Tasten weiter in den Standard-FIFO weiter. Bei Mitschnitten parallel zu AA
  `--fifo /fs/ram3/<dummy>` angeben.
- nbt_cluster v1 (RAM-Test): Pfeil plus Balken funktionieren. Fehler: Er hält verspätete eigene Quittungen für
  BMW-Aktivität, zieht sich zurück und räumt nicht auf. Fix: gesendete Befehle merken und Quittungen zuordnen.

## Übergabe für die nächste Sitzung (8.11.2026, Credits knapp)
- **Startmitschnitt:** Der Start-Mitschnitt (Flag `$P/trace-most`) lief nach einem NBT-Neustart 4 min, trotz Route aber
  **ohne einen einzigen 82-Eintrag** (`.local/gv-analysis/most-boot-82-full.txt`). CreateInstance passiert also früher als
  boot.sh (HMI +15 s), oder die Route war in dem Fenster nicht aktiv. Das Flag ist noch gesetzt, Wert gering.
- **Gewünschte Logik** (Nutzer):
  - Ist das nächste Manöver > 1 km entfernt: unten großer Geradeaus-Pfeil, oben in der flachen Zeile das kommende
    Manöver mit Symbol, Entfernung und Straße.
  - Unter 1 km: das Manöver unten groß mit Entfernung, die obere Zeile leer.
  - Danach wieder von vorn.
  - Pfeil als **Umriss** wie im Original, nicht gefüllt.
  - **Fahrspuren** unten (AA liefert `lanes`: shape + highlighted).
  - **Autobahn-/Bundesstraßenschild** (blau/gelb) bei Auf- und Abfahrten.
- **Offen technisch:**
  - Textbefehl und Bitmapbefehl (Nummern und Format) sowie die flachen Instanzen für die obere Zeile und die
    Fahrspurzeile.
  - Weg A: Serialisierer im Controller (`ctl.dis`, CIcHudComm / HUDTerminal-Klassen) auswerten.
  - Weg B: unbenutzte Funktionsnummern von 82/00 mit tx=0 anfragen und die Fehlercodes (op F) auswerten:
    04 = OpType, 05/06 = Länge/Parameter.
- **nbt_cluster v1:** Code in `src/android_auto/knob/nbt_cluster.cpp`. Lief nur im RAM, ist nach dem Neustart weg und
  nicht im Paket. Nav-Export in `aa_video_capture.cpp` (`nav.state`) ist gebaut (`center-test/oneplus_native`), aber
  ebenfalls nicht im Paket. Zu beheben: falsche BMW-Erkennung (gesendete Befehle merken), Aufräumen beim Ende,
  1-km-Logik, Umriss-Pfeil.

## Funktionsnummern und Parameter aus dem Controller (Parameter-Records, ctl.dis 0x8792f8e–0x879f366)
Pro Funktion legt der Controller einen Record an (`movw $fkt,0x12(%ebx)`) und davor Parameter-Objekte
(`CHBParaNumber` mit Breite, `CHBParaStream(max)`, `CHBParaString(max)`, `CHBParaBool`). Hinten steht immer ein
generisches `u8, Stream(10)`.

```
C06 u16,i16,i16,i16,i16,i16,u16        Carsor Position (bestätigt)
C09 u16,u16,u8,i16,i16,u16,i32,i32     ImageAddCircle (21E gleich aufgebaut)
C07 u16,u16,u8                         ?
C05 u16,u16,u16,u8,u8,u8,u8,Stream(60000)   ImageAddBitmap (Quittungen Inst 0x18/0x19)
C04 u16,u8,Stream,u16,u16              ImageGetSize (0x19 -> 0x31 x 0x2c = 49x44)
C03 u16,u8,u8,u8,u8,u16,u8             ?
22B u16,u16,u8,u16,u8                  FrameAddItem?
220 u16                                ImageRemoveAllItems (bestätigt)
21F u16,u16,u16                        ?
21D u16,u16,u8,u8,Stream(400)          ImageAddPolygon (bestätigt)
21C/21B/219/218 u16,u8                 Stil-Setter (21B = Farbe, bestätigt)
215 u16,i16,i16,i16,u16                ImageSetPositionRotation?
211 u16,u8 x6                          Textarea-Format?
20E u16,u16                            SwitchVisibility (bestätigt)
203 u16,u8                             ?
226 u16,u8,bool                        ?
205 u16,u8,Stream,bool                 IsVisible?
204 u16,bool                           SetVisibility (bestätigt)
C08 bool,u8,Stream,u8                  ?
300 Stream(10200)                      Bitmap-Segment über tx 3?
C01 u16                                ?
247 u8,u8,u8                           ?
C00 u8,u8,String(18)x3                 TerminalInterface (Versionen)
210 u16,String(255)                    TextareaSetText (NBT leert 0x1A/0x1B/0x1C beim Routenstart)
201 u16,String(255),...,u16            ?
200 u16,u8,u8,String(255),...,u16      CreateInstance?
229 u16,u8,i32,i8,i32,i8,i32,i8        PotentiometerSetRange
227 u16,i32,i8                         PotentiometerSetValue (bestätigt)
```
Instanzen: 0x1A–0x1C Textfelder, 0x18/0x19 Bildflächen (Schild/Symbol), 0x1D–0x24 Zeichenflächen
(0x1F/0x20 perspektivisch), 0x25 Carsor, 0x35 Balken, 0x17 Container.
MOST-String: erstes Byte Kodierung (00 UTF-16, 01 ISO-8859-15, 02 UTF-8 laut MOST-Spezifikation, am Auto noch zu
prüfen), dann Text, Null-Ende.

## Nächste Schritte
1. Die Anfrage-Form klären. Vermutlich gleiche Parameter wie in der Quittung, aber welcher OpType (Start oder StartResultAck),
   welches tx (4 = „Packetizer“?) und welche Sender-Handles? Quelle: Serialisierer im Controller (`ctl.dis`).
2. Erster aktiver Test im Stand bei laufender Route: einen eigenen Kreis bzw. ein Polygon in die gerade sichtbare
   Instanz zeichnen, oder den Carsor verschieben.
3. Ziel: AA-Abbiegepfeil (Polygon oder RLE-Bitmap), Entfernung (Balken/Text) und Straßenname im Tacho.

Eine Live-Karte ist nicht realistisch. Möglich ist nur Vektorgrafik bzw. eine kleine Bitmap über die MOST-Steuerleitung.

## Bitmaps, Flächengrößen und Spurzeile (Auto 2026-11-08)

**Flächengrößen** per `C04` ImageGetSize (`[inst][01]`, tx 4). Die Antwort ist `[inst][01][w u16][h u16]`:

| Instanz | Größe | Lage im Tacho (Rahmentest) |
|---|---|---|
| 0x17 | 309 × 330 | ganzer Navi-Bereich bis knapp über die Infozeile (Zeit/Reichweite) |
| 0x18/0x19 | 305 × 44 | obere Zeile (RoadIcon, Bitmap) |
| 0x1D/0x1E | 145 × 44 | flacher Streifen unter der oberen Zeile, Mitte |
| 0x1F/0x20, 0x21/0x22, 0x25 | 221 × 158 | perspektivische Ebene in der Mitte |
| 0x23/0x24 | 283 × 40 | flacher Streifen direkt über der Infozeile (Spurzeile) |
| 0x35 | 28 × 113 | Entfernungsbalken |

Linien direkt auf dem Rand einer Fläche werden abgeschnitten. Die Farbe 0x0E (grau) ist im Tacho nicht zu sehen.

**ImageAddBitmap `C05`** muss auf **Übertragungstyp 0** gesendet werden. Auf tx 4 kommt keine Antwort.

```
[inst u16][x u16][y u16][Dateityp 01][01][00][Bitmap-Nr. u8][RLE32-Daten]
```

- Die Antwort ist `ResultAck [inst][Bitmap-Nr.][01]`.
- Ein falsches 4. Byte (Dateityp) ergibt `ErrorAck 06 04` (Parameter 4 außerhalb des Bereichs).
- Bei Dateityp ≤ 0x7F muss das Bild **genau** die Größe der Fläche haben, also 305 × 44 für 0x18/0x19. Ein 49 × 44 großes Bild kam als gestreiftes Band an.

**NBT_RLE32** (aus `CGVRLE32Compressor`, NavigationGuidanceViewer_NV 0x80f0972):

- Pakete:
  - `[0x80|n][A B G R]` heißt n-mal dieses Pixel (n = 2..127).
  - `[n][n × A B G R]` sind n einzelne Pixel (n = 1..127).
- Zeile für Zeile ohne Trennung.
- Am Ende mit Nullen auf ein Vielfaches von 8 Bytes aufgefüllt, mindestens 1 Byte.
- Byte-Reihenfolge **A B G R**: Mit A R G B wurde Blau zu Braun.
- A = 0 ist durchsichtig.

**Bitmap-Tempo** (Messlauf am Auto 10.11.2026, `nbt_cluster --bench` / `--bench2`, Auto geparkt):

| Kanal | Bild | Größe | Zeit bis zur Antwort |
|---|---|---|---|
| tx 0 (Steuerkanal) | Schild 305×44 | 1,4 KB | 250–420 ms |
| tx 0 | Streifen 305×44 | 5,7 KB | 830 ms (~7 KB/s) |
| tx 0 | Karte 221×158 | 7,2 KB | **ErrorAck `0C 01`** (Segmentierungsfehler), ab ~6 KB |
| tx 1 (asynchron) | Karte 221×158 | 7 KB | 360–490 ms (~15–19 KB/s) |
| tx 1 | Streifen 221×158 | 11 KB | 570 ms |

- Die perspektivische Fläche 0x21 nimmt Bitmaps an.
- Drei Kartenbilder hintereinander auf tx 1 ergaben **1,5 Bilder/s**, und zwar mit einer stark vereinfachten Testgrafik:
  Route, sechs Straßen, wenige Flächen.
- Ein echtes Google-Maps-Bild hat Verläufe und Kantenglättung. Mit RLE wird es ein Vielfaches größer, geschätzt
  50–120 KB, also mehrere Sekunden pro Bild.
- Eine flüssige Karte im Tacho geht über diesen Weg nicht. Höchstens eine stark vereinfachte Grafik, die etwa einmal pro
  Sekunde wechselt, wäre machbar.
- **Sichttest (`--mapdemo`, 10.11.2026):** In den Zeichenflächen 0x21, 0x1F, 0x1D und 0x23 werden Bitmaps zwar mit
  ResultAck bestätigt, aber **nicht angezeigt**. Polygone in denselben Flächen sind sichtbar. Bilder zeigt der Tacho
  nur in den RoadIcon-Flächen 0x18/0x19 (obere Zeile, 305×44). Eine Karte ist damit ausgeschlossen.

**Text:** Die große Entfernungsschrift (0x1B) hat kein Komma: „5,4“ erscheint als „5 4“, „5.4“ mit Punkt geht.
`nbt_cluster` wandelt das Komma deshalb in einen Punkt um.

**Schilder:** Seit 10.11.2026 werden Ziffern und Kanten kantengeglättet gezeichnet (Strichschrift, Randpixel über Schwarz
gemischt und deckend). Damit werden die Bilder größer (ca. 5,5 KB mit Schild und 50er-Schild), sie gehen deshalb über
tx 1. Der Entfernungsbalken erscheint erst unter 1 km.

**Umsetzung in `nbt_cluster`:**

- **Schilder:** „A 66“ wird zu einem blauen Schild mit weißer Zahl, „B 40“ zu einem gelben mit schwarzer Zahl. Beide liegen links in der oberen Zeile (0x18 und 0x19). Die Nummer wird dann aus dem Straßentext entfernt.
- **Spuren:** Die Spurzeile liegt in 0x23. Spuren, die man nehmen soll, werden gefüllt, die anderen nur als Umriss gezeichnet.
- **Testmodi:**
  - `--demo` zeigt feste Testdaten.
  - `--frames` zeichnet Testrahmen in 0x21, 0x23 und 0x1D.
  - `--bmp VARIANTE INST STRASSE` dient zum Testen der C04/C05-Varianten.
