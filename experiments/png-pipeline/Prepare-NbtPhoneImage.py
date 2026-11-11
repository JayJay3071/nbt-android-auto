"""Prepare a clearly labelled static phone screenshot for NbtVideoTest."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET
import zipfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--image', type=Path, required=True)
p.add_argument('--base', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--ffmpeg', type=Path, required=True)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=False)
image = a.output / 'phone-frame.png'
subprocess.run([str(a.ffmpeg), '-nostdin', '-v', 'error', '-i', str(a.image),
                '-frames:v', '1', '-vf', 'scale=480:288', str(image)], check=True, timeout=30)
tree = ET.parse(a.base / 'NbtVideoTest.xml')
app = tree.getroot().find('pluginApp')
title = 'Android Auto Bildtest'
label = 'OnePlus Aufnahme - Standbild'
app.set('text', title)
state = app.find('hmiStates/hmiState')
state.set('text', title)
state.find('components/label').set('text', label)
tree.write(a.output / 'NbtVideoTest.xml', encoding='utf-8', xml_declaration=True)
with zipfile.ZipFile(a.output / 'NbtVideoTest_ALL_Images.zip', 'w', zipfile.ZIP_DEFLATED) as z:
    z.write(image, '1.png')
with zipfile.ZipFile(a.output / 'NbtVideoTest_ALL_Texts.zip', 'w', zipfile.ZIP_DEFLATED) as z:
    for lang in ['de', 'en-UK', 'en-US', 'es-US', 'es-ES', 'fr-FR', 'fr-CA', 'it', 'pt', 'ru']:
        z.writestr(lang + '.txt', f'100000={lang}\n1={title}\n2={label}\n')
(a.output / 'provenance.json').write_text(json.dumps({
    'source': str(a.image.resolve()),
    'source_sha256': hashlib.sha256(a.image.read_bytes()).hexdigest(),
    'static_image': True, 'vehicle_display_confirmed': False,
}, indent=2))
