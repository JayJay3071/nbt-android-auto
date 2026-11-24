#!/usr/bin/env python3
"""NBT Android Auto installer.

Writes the Android Auto / CarPlay package (service + iDrive "Android Auto / CarPlay" entry under Multimedia) to a BMW
NBT head unit over SSH. The user only enters the head unit's IP address and the root password (and, if the search does
not find it, the address of the car's gateway); nothing is stored on the computer.

What it changes on the head unit (and nothing else):
  /var/opt/hmi/data/rw/NbtAndroidAutoRT/        service binaries, certificates, boot.sh, restart.sh, iap.sh (sound as
                                                "iPod over Bluetooth"), mfl.map (skip keys), vid.dpi, decoder.hw
                                                (hardware decoding, first install only), gateway (the car's gateway
                                                address for "Restart NBT", if found), logs/, journal/, snapshots/
                                                (written by the "restart" button), carplay/ (iPhone pairing)
  /var/opt/hmi/data/rw/Plugins/NbtAndroidAuto/  HMI plugin (menu entry)
Every file is written as <name>.tmp, read back and compared byte by byte, and only then renamed into place, so a
running HMI never sees a half-written file. Uninstall removes exactly these two directories.

GUI:  python3 nbt_aa_installer.py
CLI:  python3 nbt_aa_installer.py --cli install|uninstall|logs [--host 169.254.199.99] [--gateway 169.254.x.x]
Needs: Python 3.8+ with tkinter (GUI) and paramiko (pip install paramiko). The release builds bundle both.
"""
import argparse
import getpass
import hashlib
import io
import ipaddress
import json
import locale
import os
import queue
import socket
import struct
import sys
import threading
import time
import zipfile
from pathlib import Path

try:
    import paramiko
except ImportError:  # shown nicely by the GUI / CLI below
    paramiko = None

APP_NAME = 'NBT Android Auto Installer'
DEFAULT_HOST = '169.254.199.99'
RT_DIR = '/var/opt/hmi/data/rw/NbtAndroidAutoRT'
PLUGIN_DIR = '/var/opt/hmi/data/rw/Plugins/NbtAndroidAuto'
PAYLOAD_OVERRIDE = None     # development: --payload <dir>


def payload_dir():
    if PAYLOAD_OVERRIDE:
        return Path(PAYLOAD_OVERRIDE)
    base = Path(getattr(sys, '_MEIPASS', Path(__file__).resolve().parent))
    return base / 'payload'


# --------------------------------------------------------------------------------------------------------------------
# texts
def _is_german():
    names = [os.environ.get(k, '') for k in ('LC_ALL', 'LC_MESSAGES', 'LANG')]
    try:
        names.append(locale.getlocale()[0] or '')
    except ValueError:
        pass
    if sys.platform == 'win32':
        try:
            import ctypes
            if ctypes.windll.kernel32.GetUserDefaultUILanguage() & 0x3ff == 0x07:
                return True
        except Exception:
            pass
    return any(n.lower().startswith(('de', 'german')) for n in names)


GERMAN = _is_german()
T = {
    'host': ('IP-Adresse des NBT', 'NBT IP address'),
    'port': ('Port', 'Port'),
    'user': ('Benutzer', 'User'),
    'password': ('Passwort', 'Password'),
    'install': ('Installieren / Aktualisieren', 'Install / Update'),
    'uninstall': ('Deinstallieren', 'Uninstall'),
    'logs': ('Diagnose-Logs speichern', 'Save diagnostic logs'),
    'intro': ('Laptop per Ethernet (ENET) mit dem Auto verbinden, Zuendung an, IP und root-Passwort eingeben.',
              'Connect the laptop to the car via Ethernet (ENET), ignition on, enter IP and root password.'),
    'confirm_uninstall': ('Android Auto wirklich vom NBT entfernen?', 'Really remove Android Auto from the NBT?'),
    'done_install': ('Fertig. Jetzt das iDrive neu starten. Danach: Multimedia -> Android Auto / CarPlay.',
                     'Done. Now restart iDrive. Then: Multimedia -> Android Auto / CarPlay.'),
    'done_uninstall': ('Entfernt. Nach dem naechsten iDrive-Neustart ist der Menuepunkt weg.',
                       'Removed. The menu entry is gone after the next iDrive restart.'),
    'busy': ('Bitte warten ...', 'Please wait ...'),
    'sound_hint': ('Ton: Android Auto erscheint unter Multimedia als Apple-Geraet "Android Auto" (Titel, Cover, Skip). '
                   'Dafuer das Telefon per USB anstecken und danach per Bluetooth verbinden. War Bluetooth schon vorher '
                   'verbunden: Bluetooth am Telefon einmal aus- und wieder einschalten.',
                   'Sound: Android Auto shows up under Multimedia as Apple device "Android Auto" (title, cover, skip). '
                   'Plug the phone in by USB, then connect it by Bluetooth. If Bluetooth was connected before: switch '
                   'Bluetooth on the phone off and on once.'),
    'carplay_hint': ('CarPlay: iPhone per USB anstecken ("Vertrauen" bestaetigen). Fuer den Ton das iPhone zusaetzlich per '
                     'Bluetooth koppeln und im Auto nur "Audio" aktivieren (Telefon 1 / Telefon 2 aus); der Ton laeuft '
                     'dann ueber denselben Apple-Eintrag "Android Auto".',
                     'CarPlay: plug the iPhone in by USB (confirm "Trust"). For sound, also pair the iPhone by Bluetooth '
                     'and enable only "Audio" for it in the car (Telephone 1 / Telephone 2 off); the sound then plays '
                     'through the same Apple entry "Android Auto".'),
    'decoder_hint': ('Bild: 1280 Pixel breit ueber den Hardware-Decoder des Autos. Umschalten auf Software: Knopf '
                     '"Decoder" auf der Seite, gilt nach einem NBT-Neustart.',
                     'Picture: 1280 pixels wide through the hardware decoder of the car. Switch to software: "Decoder" '
                     'button on the page, applies after an NBT restart.'),
    'gateway_found': ('Gateway des Autos gefunden und gespeichert (fuer "NBT neu starten"): %s',
                      'Found and stored the gateway of the car (for "Restart NBT"): %s'),
    'gateway_missing': ('Gateway des Autos nicht gefunden: "NBT neu starten" auf der Seite geht erst, wenn seine '
                        'IP-Adresse eingetragen ist. Dazu die Adresse oben ins Feld "Gateway" schreiben (zeigen z. B. '
                        'ISTA oder der SSH-Patcher an) und noch einmal installieren.',
                        'The gateway of the car was not found: "Restart NBT" on the page only works once its IP '
                        'address is set. Enter it in the "Gateway" field above (shown e.g. by ISTA or the SSH '
                        'patcher) and install again.'),
    'gateway': ('Gateway (fuer "NBT neu starten")', 'Gateway (for "Restart NBT")'),
    'gateway_search': ('Suchen', 'Search'),
    'gateway_searching': ('Suche das Gateway des Autos ...', 'Searching for the gateway of the car ...'),
    'gateway_search_found': ('Gateway gefunden: %s', 'Gateway found: %s'),
    'gateway_search_none': ('Kein Gateway gefunden. Ist das Kabel am Auto und die Zuendung an? Die Adresse kann auch '
                            'von Hand eingetragen werden.',
                            'No gateway found. Is the cable in the car and the ignition on? The address can also be '
                            'entered by hand.'),
    'gateway_bad': ('Die Gateway-Adresse ist keine IPv4-Adresse: %s', 'The gateway address is not an IPv4 address: %s'),
    'gateway_stored': ('Gateway gespeichert (fuer "NBT neu starten"): %s', 'Stored the gateway (for "Restart NBT"): %s'),
    'no_paramiko': ('Das Python-Paket "paramiko" fehlt: pip install paramiko',
                    'The Python package "paramiko" is missing: pip install paramiko'),
    'error': ('Fehler', 'Error'),
}


def tr(key):
    return T[key][0 if GERMAN else 1]


class InstallError(Exception):
    pass


# --------------------------------------------------------------------------------------------------------------------
# SSH access (exec channels only: the NBT's sshd has no reliable SFTP)
class Nbt:
    def __init__(self, host, port, user, password=None, key_file=None, log=print):
        self.log = log
        self.client = paramiko.SSHClient()
        # Lay users cannot verify host keys; the link is a direct cable to the car. The fingerprint is logged.
        self.client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
        try:
            pkey = paramiko.RSAKey.from_private_key_file(key_file) if key_file else None
            self.client.connect(host, port=port, username=user, password=password, pkey=pkey,
                                disabled_algorithms={'pubkeys': ['rsa-sha2-512', 'rsa-sha2-256']} if pkey else None,
                                look_for_keys=False, allow_agent=False, timeout=20, banner_timeout=60,
                                auth_timeout=60)
        except paramiko.AuthenticationException:
            raise InstallError('Login failed: wrong user or password.' if not GERMAN else
                               'Anmeldung fehlgeschlagen: Benutzer oder Passwort falsch.')
        except paramiko.SSHException as exc:
            if 'host key' in str(exc).lower():
                raise InstallError('The NBT only offers ssh-rsa host keys, which paramiko %s no longer supports. '
                                   'Use paramiko 3.x: pip install "paramiko>=3.4,<4"' % paramiko.__version__)
            raise InstallError('SSH error: %s' % exc)
        except (socket.timeout, OSError) as exc:
            raise InstallError(('No connection to %s:%d (%s). Cable, IP address and ignition?' if not GERMAN else
                                'Keine Verbindung zu %s:%d (%s). Kabel, IP-Adresse und Zuendung pruefen.')
                               % (host, port, exc))
        key = self.client.get_transport().get_remote_server_key()
        self.log('SSH %s@%s:%d  host key %s %s' % (user, host, port, key.get_name(),
                                                   hashlib.sha256(key.asbytes()).hexdigest()[:32]))
        self.client.get_transport().set_keepalive(15)

    def close(self):
        self.client.close()

    def run(self, command, stdin=b'', timeout=120, check=True):
        channel = self.client.get_transport().open_session(timeout=20)
        channel.settimeout(timeout)
        channel.exec_command(command)
        if stdin:
            view = memoryview(stdin)
            for offset in range(0, len(view), 32768):
                channel.sendall(view[offset:offset + 32768])
        channel.shutdown_write()
        out, err = bytearray(), bytearray()
        deadline = time.monotonic() + timeout
        while True:
            while channel.recv_ready():
                out.extend(channel.recv(65536))
            while channel.recv_stderr_ready():
                err.extend(channel.recv_stderr(65536))
            if channel.exit_status_ready() and not channel.recv_ready() and not channel.recv_stderr_ready():
                break
            if time.monotonic() > deadline:
                channel.close()
                raise InstallError('Timeout: ' + command)
            time.sleep(0.02)
        code = channel.recv_exit_status()
        channel.close()
        if check and code != 0:
            raise InstallError('Command failed (%d): %s\n%s' % (code, command, (err or out).decode('utf-8', 'replace')))
        return code, bytes(out), bytes(err)

    def put(self, data, target, mode):
        """Write data to target atomically: target.tmp, read back, compare, chmod, rename."""
        tmp = target + '.tmp'
        self.run("cat > '%s'" % tmp, stdin=data)
        _, back, _ = self.run("cat '%s'" % tmp)
        if back != data:
            self.run("rm -f '%s'" % tmp, check=False)
            raise InstallError('Read-back mismatch: ' + target)
        self.run("chmod %s '%s' && mv '%s' '%s'" % (mode, tmp, tmp, target))


# --------------------------------------------------------------------------------------------------------------------
def load_manifest():
    directory = payload_dir()
    manifest_path = directory / 'manifest.json'
    if not manifest_path.exists():
        raise InstallError('Payload missing: ' + str(manifest_path))
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    for entry in manifest['files']:
        data = (directory / entry['source']).read_bytes()
        if hashlib.sha256(data).hexdigest() != entry['sha256']:
            raise InstallError('Payload damaged: ' + entry['source'])
        entry['data'] = data
    return manifest


def preflight(nbt, log, root):
    code, out, _ = nbt.run('uname -s; uname -m; uname -r', check=False)
    info = out.decode('utf-8', 'replace').split()
    log('System: ' + ' '.join(info))
    if not info or info[0] != 'QNX':
        raise InstallError('This is not a QNX head unit (uname: %s).' % ' '.join(info))
    checks = [
        ("test -d '%s/var/opt/hmi/data/rw'" % root, 'HMI data directory /var/opt/hmi/data/rw', True),
        ("test -x '%s/opt/mm/bin/NmeCmdLine'" % root, 'Cinemo NmeCmdLine (video decoder)', True),
        ("test -f '%s/opt/mm/lib/cinemo/cinemo_classes.xml'" % root, 'Cinemo configuration', True),
        ("test -f '%s/opt/sys/lib/libsys_dsi_colibry.so.3'" % root, 'DSI library (iDrive controller)', False),
        ("test -e '%s/dev/DisplayManager:0'" % root, 'DisplayManager', False),
    ]
    for command, what, required in checks:
        ok = nbt.run(command, check=False)[0] == 0
        log(('  ok      ' if ok else ('  MISSING ' if required else '  warning ')) + what)
        if required and not ok:
            raise InstallError('Unsupported head unit: %s not found. Only the original NBT is supported.' % what)
    if nbt.run('test -d /opt/hmi/ID5/data/ro/Plugins/NbtAad -o -d /var/opt/hmi/data/rw/Plugins/NbtAad',
               check=False)[0] == 0:
        log('  warning another Android Auto solution (NbtAad) is installed - do not run both at once')
    probe = root + RT_DIR + '/.write-test'
    nbt.run("mkdir -p '%s' '%s'" % (root + RT_DIR, root + PLUGIN_DIR))
    if nbt.run("touch '%s' && rm -f '%s'" % (probe, probe), check=False)[0] != 0:
        raise InstallError('The HMI data directory is read-only.')


def find_gateway(nbt_host, timeout=3.0):
    """IP address of the car's gateway (ZGW), or None: HSFZ vehicle identification, a UDP broadcast to port 6811 on
    the ENET link. The page's "Restart NBT" sends its reset through the gateway, and the head unit cannot find it
    itself (it has no route to the broadcast address), so the installer asks from the laptop."""
    try:
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        probe.connect((nbt_host, 9))            # no packet is sent: only picks the interface towards the car
        local = probe.getsockname()[0]
        probe.close()
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    except OSError:
        return None
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        sock.bind((local, 0))
        sock.sendto(struct.pack('>IH', 0, 0x0011),
                    ('169.254.255.255' if local.startswith('169.254.') else '255.255.255.255', 6811))
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            sock.settimeout(max(0.1, deadline - time.monotonic()))
            data, sender = sock.recvfrom(512)
            # the answer: u32 length, u16 control, then "DIAGADR..BMWMAC..BMWVIN.."
            if len(data) >= 6 and b'DIAGADR' in data and sender[0] != nbt_host:
                return sender[0]
    except OSError:                             # timeout, no route, blocked by a firewall
        pass
    finally:
        sock.close()
    return None


def install(nbt, log, root='', gateway=''):
    manifest = load_manifest()
    log('Package %s, %d files' % (manifest.get('version', '?'), len(manifest['files'])))
    preflight(nbt, log, root)
    nbt.run("chmod 700 '%s'" % (root + RT_DIR))
    had_decoder = nbt.run("test -f '%s/nbt_decoder'" % (root + RT_DIR), check=False)[0] == 0
    # the manifest of the plugin goes last: the HMI must never see a half package
    files = sorted(manifest['files'], key=lambda e: e['target'].endswith('.manifest'))
    for entry in files:
        target = root + entry['target']
        log('  write %-46s %8d bytes' % (entry['target'].split('/')[-1], len(entry['data'])))
        nbt.put(entry['data'], target, entry['mode'])
    dpi = root + RT_DIR + '/vid.dpi'
    nbt.run("test -f '%s' || echo 100 > '%s'" % (dpi, dpi))
    nbt.run("rm -f '%s/disabled'" % (root + RT_DIR))
    if any(e['target'].endswith('/iap.sh') for e in files):
        # Android Auto sound as "iPod over Bluetooth": on with every install / update (off: touch iap.off on the NBT)
        nbt.run("rm -f '%s/iap.off'" % (root + RT_DIR))
        log(tr('sound_hint'))
    if any(e['target'].endswith('/nbt_decoder') for e in files):
        if not had_decoder:
            # The first install of the 1280-wide picture path decodes with the car's own H.264 decoder (fluid on the
            # developer's car; the CPU path loads the head unit fully). Later installs keep what the "Decoder" button
            # on the page chose. nbt_decoder falls back to the CPU by itself if the car refuses the decoder.
            nbt.run("touch '%s/decoder.hw'" % (root + RT_DIR))
        log(tr('decoder_hint'))
    if any(e['target'].endswith('/nbt_carplay') for e in files):
        log(tr('carplay_hint'))
    if any(e['target'].endswith('/nbt_reset') for e in files) and not root:
        # an address the user gave wins (it also replaces a stored one); else a stored one stays; else the search
        gateway_file = RT_DIR + '/gateway'
        if gateway:
            nbt.put((gateway + '\n').encode('ascii'), gateway_file, '644')
            log(tr('gateway_stored') % gateway)
        elif nbt.run("test -s '%s'" % gateway_file, check=False)[0] != 0:
            gateway = find_gateway(nbt.client.get_transport().getpeername()[0])
            if gateway:
                nbt.put((gateway + '\n').encode('ascii'), gateway_file, '644')
                log(tr('gateway_found') % gateway)
            else:
                log(tr('gateway_missing'))
    nbt.run("echo '%s' > '%s/VERSION'" % (manifest.get('version', '?'), root + RT_DIR))
    _, out, _ = nbt.run("ls -l '%s' '%s'" % (root + RT_DIR, root + PLUGIN_DIR), check=False)
    log(out.decode('utf-8', 'replace').rstrip())
    log(tr('done_install'))


def uninstall(nbt, log, root=''):
    preflight_root = root + '/var/opt/hmi/data/rw'
    if nbt.run("test -d '%s'" % preflight_root, check=False)[0] != 0:
        raise InstallError('HMI data directory not found.')
    nbt.run("test -d '%s' && touch '%s/disabled'" % (root + RT_DIR, root + RT_DIR), check=False)
    if not root:
        log('  stopping the Android Auto service')
        nbt.run('slay -f -Q -s TERM native_service', check=False)
        nbt.run('slay -f -Q -s TERM nbt_cluster', check=False)     # hides its cluster graphics on exit
        time.sleep(3)
        # iPod-over-Bluetooth helpers (iap.sh ends them itself when the service is gone; this is the safety net)
        for name in ('nbt_aa_ipod', 'nbt_ipod_tap', 'nbt_a2i_play'):
            nbt.run('slay -f -Q -s KILL %s' % name, check=False)
    for directory in (PLUGIN_DIR, RT_DIR):
        nbt.run("rm -r '%s'" % (root + directory), check=False)
        still = nbt.run("test -e '%s'" % (root + directory), check=False)[0] == 0
        log('  %s %s' % ('NOT removed' if still else 'removed', directory))
        if still:
            raise InstallError('Could not remove ' + directory)
    log(tr('done_uninstall'))


def save_logs(nbt, log, target_zip, root=''):
    sources = []
    for directory in (root + RT_DIR + '/logs', root + RT_DIR + '/logs.prev', root + RT_DIR + '/snapshots'):
        code, out, _ = nbt.run("ls '%s'" % directory, check=False)
        if code == 0:
            sources += [directory + '/' + name for name in out.decode('utf-8', 'replace').split()]
    sources += ['/fs/ram3/nbt-aa-plugin.log', '/fs/ram3/nbt-aa-boot.log', '/fs/ram3/nbt-aa-state', '/fs/ram3/nbt-cluster.log',
                '/fs/ram3/nbt-aa-native/iap.log', '/fs/ram3/nbt-aa-native/btipod.log',
                '/fs/ram3/nbt-aa-native/sound.log', root + RT_DIR + '/iap.off',
                '/fs/ram3/nbt-aa-native/decoder.notes', '/fs/ram3/nbt-reboot.log', root + RT_DIR + '/gateway',
                root + RT_DIR + '/VERSION', root + RT_DIR + '/vid.dpi',
                root + RT_DIR + '/sessions.log', root + RT_DIR + '/sessions.log.1']
    count = 0
    with zipfile.ZipFile(target_zip, 'w', zipfile.ZIP_DEFLATED) as archive:
        for path in sources:
            code, data, _ = nbt.run("cat '%s'" % path, check=False)
            if code == 0:
                archive.writestr(path.lstrip('/'), data)
                count += 1
        _, data, _ = nbt.run('uname -a; ls -l %s %s; pidin 2>&1 | grep -e native_service -e nbt_ -e NmeCmdLine'
                             % (root + RT_DIR, root + PLUGIN_DIR), check=False)
        archive.writestr('overview.txt', data)
    log('%d files saved: %s' % (count, target_zip))


def checked_gateway(text):
    """The gateway field as an IPv4 address, '' if empty; InstallError if it is something else (it goes into a file
    the head unit's page passes to a program)."""
    text = text.strip()
    if not text:
        return ''
    try:
        return str(ipaddress.IPv4Address(text))
    except ValueError:
        raise InstallError(tr('gateway_bad') % text)


# --------------------------------------------------------------------------------------------------------------------
def run_cli(args):
    if paramiko is None:
        sys.exit(tr('no_paramiko'))
    password = None if args.key else getpass.getpass('%s %s@%s: ' % (tr('password'), args.user, args.host))
    nbt = Nbt(args.host, args.port, args.user, password=password, key_file=args.key)
    try:
        if args.cli == 'logs':
            save_logs(nbt, print, args.output or 'nbt-aa-logs-%s.zip' % time.strftime('%Y%m%d-%H%M%S'),
                      args.root)
        elif args.cli == 'install':
            install(nbt, print, args.root, checked_gateway(args.gateway))
        else:
            uninstall(nbt, print, args.root)
    finally:
        nbt.close()


def run_gui(args):
    import tkinter as tk
    from tkinter import filedialog, messagebox, scrolledtext, ttk

    root = tk.Tk()
    root.title(APP_NAME)
    root.minsize(640, 460)
    frame = ttk.Frame(root, padding=12)
    frame.pack(fill='both', expand=True)
    ttk.Label(frame, text=tr('intro'), wraplength=600).grid(row=0, column=0, columnspan=4, sticky='w', pady=(0, 10))
    fields = {}
    for row, (key, default, show) in enumerate([('host', args.host, ''), ('port', str(args.port), ''),
                                                 ('user', args.user, ''), ('password', '', '*')], start=1):
        ttk.Label(frame, text=tr(key)).grid(row=row, column=0, sticky='w', pady=2)
        var = tk.StringVar(value=default)
        ttk.Entry(frame, textvariable=var, show=show, width=32).grid(row=row, column=1, sticky='w', pady=2)
        fields[key] = var
    # the car's gateway for the page's "Restart NBT": empty = the installer searches while installing
    ttk.Label(frame, text=tr('gateway')).grid(row=5, column=0, sticky='w', pady=2)
    fields['gateway'] = tk.StringVar(value=args.gateway)
    ttk.Entry(frame, textvariable=fields['gateway'], width=32).grid(row=5, column=1, sticky='w', pady=2)
    buttons = ttk.Frame(frame)
    buttons.grid(row=6, column=0, columnspan=4, sticky='w', pady=10)
    output = scrolledtext.ScrolledText(frame, height=16, state='disabled', font=('Courier', 9))
    output.grid(row=7, column=0, columnspan=4, sticky='nsew')
    frame.rowconfigure(7, weight=1)
    frame.columnconfigure(3, weight=1)
    messages = queue.Queue()
    widgets = []

    def log(text):
        messages.put(str(text))

    def pump():
        while not messages.empty():
            item = messages.get()
            if item == '\0done':
                for widget in widgets:
                    widget.configure(state='normal')
                continue
            if item.startswith('\0gateway '):
                fields['gateway'].set(item.split(' ', 1)[1])
                continue
            output.configure(state='normal')
            output.insert('end', item + '\n')
            output.see('end')
            output.configure(state='disabled')
        root.after(100, pump)

    def start(action, extra=None):
        if paramiko is None:
            messagebox.showerror(tr('error'), tr('no_paramiko'))
            return
        try:
            port = int(fields['port'].get())
        except ValueError:
            messagebox.showerror(tr('error'), 'Port?')
            return
        host, user, password = fields['host'].get().strip(), fields['user'].get().strip(), fields['password'].get()
        try:
            gateway = checked_gateway(fields['gateway'].get())
        except InstallError as exc:
            messagebox.showerror(tr('error'), str(exc))
            return
        for widget in widgets:
            widget.configure(state='disabled')
        log('--- ' + time.strftime('%H:%M:%S') + ' ' + tr('busy'))

        def work():
            nbt = None
            try:
                nbt = Nbt(host, port, user, password=password or None, key_file=args.key, log=log)
                if action == 'logs':
                    save_logs(nbt, log, extra, args.root)
                elif action == 'install':
                    install(nbt, log, args.root, gateway)
                else:
                    uninstall(nbt, log, args.root)
            except InstallError as exc:
                log('%s: %s' % (tr('error'), exc))
            except Exception as exc:  # unexpected: show it instead of dying silently
                log('%s: %s: %s' % (tr('error'), type(exc).__name__, exc))
            finally:
                if nbt:
                    nbt.close()
                messages.put('\0done')

        threading.Thread(target=work, daemon=True).start()

    def do_search():
        host = fields['host'].get().strip()
        for widget in widgets:
            widget.configure(state='disabled')
        log(tr('gateway_searching'))

        def work():
            found = find_gateway(host)
            if found:
                messages.put('\0gateway ' + found)
                log(tr('gateway_search_found') % found)
            else:
                log(tr('gateway_search_none'))
            messages.put('\0done')

        threading.Thread(target=work, daemon=True).start()

    search = ttk.Button(frame, text=tr('gateway_search'), command=do_search)
    search.grid(row=5, column=2, sticky='w', padx=(8, 0), pady=2)
    widgets.append(search)

    def do_uninstall():
        if messagebox.askyesno(APP_NAME, tr('confirm_uninstall')):
            start('uninstall')

    def do_logs():
        target = filedialog.asksaveasfilename(defaultextension='.zip',
                                              initialfile='nbt-aa-logs-%s.zip' % time.strftime('%Y%m%d-%H%M%S'))
        if target:
            start('logs', target)

    for column, (key, command) in enumerate([('install', lambda: start('install')), ('uninstall', do_uninstall),
                                             ('logs', do_logs)]):
        button = ttk.Button(buttons, text=tr(key), command=command)
        button.grid(row=0, column=column, padx=(0, 8))
        widgets.append(button)
    try:
        manifest = json.loads((payload_dir() / 'manifest.json').read_text(encoding='utf-8'))
        log('Package: %s (%d files)' % (manifest.get('version', '?'), len(manifest['files'])))
    except Exception as exc:
        log('%s: payload: %s' % (tr('error'), exc))
    if paramiko is None:
        log(tr('no_paramiko'))
    pump()
    root.mainloop()


def main():
    parser = argparse.ArgumentParser(description=APP_NAME)
    parser.add_argument('--cli', choices=['install', 'uninstall', 'logs'], help='run without a window')
    parser.add_argument('--host', default=DEFAULT_HOST)
    parser.add_argument('--port', type=int, default=22)
    parser.add_argument('--user', default='root')
    parser.add_argument('--output', help='logs: target zip file')
    parser.add_argument('--gateway', default='', help='install: IP address of the car\'s gateway for "Restart NBT" '
                                                      '(default: searched)')
    parser.add_argument('--payload', help=argparse.SUPPRESS)
    # development only: key login and installing below a test directory (e.g. in a QNX build VM)
    parser.add_argument('--key', help=argparse.SUPPRESS)
    parser.add_argument('--root', default='', help=argparse.SUPPRESS)
    args = parser.parse_args()
    global PAYLOAD_OVERRIDE
    PAYLOAD_OVERRIDE = args.payload
    if args.cli:
        try:
            run_cli(args)
        except InstallError as exc:
            sys.exit('%s: %s' % (tr('error'), exc))
    else:
        run_gui(args)


if __name__ == '__main__':
    main()
