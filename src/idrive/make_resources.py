"""Build local RHMI text resources; never installs anything on the vehicle."""
from pathlib import Path
import sys
import zipfile
import xml.etree.ElementTree as ET

source = Path(__file__).resolve().parent
output = Path(sys.argv[1]).resolve()
output.mkdir(parents=True, exist_ok=True)
ET.parse(source / 'HelloNbt.xml')
ET.parse(source / 'HelloNbt.manifest.example')
languages = ['de', 'en-UK', 'en-US', 'es-US', 'es-ES', 'fr-FR', 'fr-CA', 'it', 'pt', 'ru']
with zipfile.ZipFile(output / 'HelloNbt_ALL_Texts.zip', 'w', zipfile.ZIP_DEFLATED) as archive:
    for language in languages:
        archive.writestr(language + '.txt',
            '100000=' + language + '\n1=Hello NBT\n2=Hello World from QNX x86!\n')
with zipfile.ZipFile(output / 'HelloNbt_ALL_Images.zip', 'w'):
    pass
print('Local prototype resources created:', output)
