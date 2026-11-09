"""Build the local NbtAndroidAuto plugin package (XML, manifest, text/image zips). Never installs anything.
Usage: python make_aa_entry_resources.py <output-dir>   (libNbtAndroidAuto.so is built separately in the QNX VM)
Text ids 2..9 are the status lines the plugin switches between (aa_entry_plugin.cpp kTxt*)."""
from pathlib import Path
import shutil
import sys
import zipfile
import xml.etree.ElementTree as ET

source = Path(__file__).resolve().parent
output = Path(sys.argv[1]).resolve()
output.mkdir(parents=True, exist_ok=True)
for name in ('NbtAndroidAuto.xml', 'NbtAndroidAuto.manifest'):
    ET.parse(source / name)
    shutil.copyfile(source / name, output / name)
# keep every line short: the page shows about 25 characters per line (car 2026-11-08)
GERMAN = {
    1: 'Android Auto',
    2: 'Telefon per USB verbinden',
    3: 'Kein Telefon verbunden',
    4: 'Android Auto startet ...',
    5: 'Android Auto verbunden',
    6: 'Telefon neu anstecken',
    7: 'Geraet ohne Android Auto',
    8: 'Dienst laeuft nicht',
    9: 'Nur das Telefon anstecken',
    10: 'Bildmodus',
    11: '16:9 - schaerfer',
    12: 'Breitbild - ganze Breite',
    13: 'Tipp: 16:9 + Split-Screen',
    14: 'Breitbild: etwas weicher',
    15: 'Wechsel: Telefon abziehen',
    16: '16:9 - schaerfer (aktiv)',
    17: 'Breitbild (aktiv)',
    18: 'Bildmodus gespeichert',
    19: 'Android Auto neu starten',
    20: 'Android Auto startet neu',
}
ENGLISH = {
    1: 'Android Auto',
    2: 'Connect phone via USB',
    3: 'No phone connected',
    4: 'Starting Android Auto ...',
    5: 'Android Auto connected',
    6: 'Reconnect the phone',
    7: 'No Android Auto device',
    8: 'Service not running',
    9: 'Connect only the phone',
    10: 'Picture mode',
    11: '16:9 - sharper',
    12: 'Widescreen - full width',
    13: 'Tip: 16:9 + split screen',
    14: 'Widescreen: a bit softer',
    15: 'To switch: unplug phone',
    16: '16:9 - sharper (active)',
    17: 'Widescreen (active)',
    18: 'Picture mode saved',
    19: 'Restart Android Auto',
    20: 'Android Auto restarting',
}
languages = ['de', 'en-UK', 'en-US', 'es-US', 'es-ES', 'fr-FR', 'fr-CA', 'it', 'pt', 'ru']
with zipfile.ZipFile(output / 'NbtAndroidAuto_ALL_Texts.zip', 'w', zipfile.ZIP_DEFLATED) as archive:
    for language in languages:
        texts = GERMAN if language == 'de' else ENGLISH
        body = '100000=' + language + '\n' + ''.join('%d=%s\n' % (k, v) for k, v in sorted(texts.items()))
        archive.writestr(language + '.txt', body)
with zipfile.ZipFile(output / 'NbtAndroidAuto_ALL_Images.zip', 'w'):
    pass
print('Local package resources created:', output)
