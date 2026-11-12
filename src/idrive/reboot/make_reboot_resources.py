"""Build the NbtReboot plugin resources (XML, manifest, text/image zips) into <output-dir>. Never installs anything.
Usage: python make_reboot_resources.py <output-dir>   (libNbtReboot.so and nbt_reset are built in the QNX VM)"""
from pathlib import Path
import shutil
import sys
import zipfile
import xml.etree.ElementTree as ET

source = Path(__file__).resolve().parent
output = Path(sys.argv[1]).resolve()
output.mkdir(parents=True, exist_ok=True)
for name in ('NbtReboot.xml', 'NbtReboot.manifest'):
    ET.parse(source / name)
    shutil.copyfile(source / name, output / name)
# keep every line short: the page shows about 25 characters per line
GERMAN = {
    1: 'iDrive neu starten',
    2: 'Head Unit jetzt neu starten?',
    3: 'Jetzt neu starten',
    4: 'Startet neu ...',
    5: 'Erst 30 s nach dem Start',
    6: 'Neustart laeuft bereits',
    7: 'Eintrag erneut oeffnen',
    8: 'Gateway nicht erreichbar',
    9: 'Neustart abgelehnt',
    10: 'Keine Antwort vom Gateway',
}
ENGLISH = {
    1: 'Restart iDrive',
    2: 'Restart the head unit now?',
    3: 'Restart now',
    4: 'Restarting ...',
    5: 'Not yet: 30 s after boot',
    6: 'Restart already running',
    7: 'Open the entry again',
    8: 'Gateway not reachable',
    9: 'Reset refused by the unit',
    10: 'No answer from gateway',
}
languages = ['de', 'en-UK', 'en-US', 'es-US', 'es-ES', 'fr-FR', 'fr-CA', 'it', 'pt', 'ru']
with zipfile.ZipFile(output / 'NbtReboot_ALL_Texts.zip', 'w', zipfile.ZIP_DEFLATED) as archive:
    for language in languages:
        texts = GERMAN if language == 'de' else ENGLISH
        body = '100000=' + language + '\n' + ''.join('%d=%s\n' % (k, v) for k, v in sorted(texts.items()))
        archive.writestr(language + '.txt', body)
# empty, as in NbtAndroidAuto (history: image resources once made an entry disappear)
with zipfile.ZipFile(output / 'NbtReboot_ALL_Images.zip', 'w'):
    pass
print('NbtReboot resources created:', output)
