"""Package a static native-HMI image test from an actual Cinemo frame dump.

Creates local resources only; no installation or HMI restart. The image is a
synthetic video test frame, explicitly not a phone/Android Auto screenshot.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--frames', type=Path, required=True)
    parser.add_argument('--ffmpeg', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    # Require the independently validated original fixture capture.
    digest = hashlib.sha256(args.frames.read_bytes()).hexdigest()
    if digest != '0628a50df1567ef37065621b370401c628a79d6614e4f726b56091d2d900b7af':
        parser.error('Expected the validated 30-frame Cinemo fixture capture')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    image = out / 'decoded-test-frame.png'
    command = [str(args.ffmpeg.resolve()), '-nostdin', '-v', 'error', '-f', 'rawvideo',
               '-pixel_format', 'yuv420p', '-video_size', '800x480', '-i', str(args.frames.resolve()),
               '-frames:v', '1', '-vf', 'scale=480:288', '-update', '1', str(image)]
    subprocess.run(command, check=True, timeout=30)
    package = out / 'NbtVideoTest'
    package.mkdir()
    tree = ET.parse(ROOT / 'src/idrive/HelloNbt.xml')
    app = tree.getroot().find('pluginApp')
    app.set('text', 'NBT Bildtest')
    app.set('applicationWeight', '201')
    ET.SubElement(app.find('models'), 'imageIdModel', id='6', imageId='1')
    state = app.find('hmiStates/hmiState')
    state.set('text', 'NBT Bildtest')
    components = state.find('components')
    components.find('label').set('text', 'H.264-Testbild - kein Handybild')
    picture = ET.SubElement(components, 'image', id='12', model='6')
    properties = ET.SubElement(picture, 'properties')
    for ident, value in [('3', 'true'), ('9', '480'), ('10', '288'), ('20', '0'), ('21', '40')]:
        ET.SubElement(properties, 'property', id=ident, value=value)
    tree.write(package / 'NbtVideoTest.xml', encoding='utf-8', xml_declaration=True)
    manifest = (ROOT / 'src/idrive/HelloNbt.manifest.example').read_text().replace('HelloNbt', 'NbtVideoTest')
    ET.fromstring(manifest)
    (package / 'NbtVideoTest.manifest').write_text(manifest, encoding='utf-8')
    with zipfile.ZipFile(package / 'NbtVideoTest_ALL_Images.zip', 'w', zipfile.ZIP_DEFLATED) as archive:
        archive.write(image, '1.png')
    with zipfile.ZipFile(package / 'NbtVideoTest_ALL_Texts.zip', 'w', zipfile.ZIP_DEFLATED) as archive:
        for language in ['de', 'en-UK', 'en-US', 'es-US', 'es-ES', 'fr-FR', 'fr-CA', 'it', 'pt', 'ru']:
            archive.writestr(language + '.txt', f'100000={language}\n1=NBT Bildtest\n2=H.264-Testbild - kein Handybild\n')
    (out / 'image-provenance.json').write_text(json.dumps({
        'source': str(args.frames), 'source_sha256': digest,
        'image_sha256': hashlib.sha256(image.read_bytes()).hexdigest(),
        'conversion_command': command, 'is_android_auto_screen': False,
        'purpose': 'Static native HMI display of a real Cinemo-decoded test frame',
    }, indent=2), encoding='utf-8')
    print(package)


if __name__ == '__main__':
    main()
