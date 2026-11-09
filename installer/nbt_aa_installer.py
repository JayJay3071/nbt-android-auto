#!/usr/bin/env python3
"""NBT Android Auto installer.

Writes the Android Auto package (service + iDrive "Android Auto" entry under Multimedia) to a BMW NBT head unit over
SSH. The user only enters the head unit's IP address and the root password; nothing is stored.

What it changes on the head unit (and nothing else):
  /var/opt/hmi/data/rw/NbtAndroidAutoRT/        service binaries, certificates, boot.sh, restart.sh, vid.dpi, logs/,
                                                snapshots/ (written by the "restart" button)
  /var/opt/hmi/data/rw/Plugins/NbtAndroidAuto/  HMI plugin (menu entry)
Every file is written as <name>.tmp, read back and compared byte by byte, and only then renamed into place, so a
running HMI never sees a half-written file. Uninstall removes exactly these two directories.

GUI:  python3 nbt_aa_installer.py
CLI:  python3 nbt_aa_installer.py --cli install|uninstall|logs [--host 169.254.199.99]
Needs: Python 3.8+ with tkinter (GUI) and paramiko (pip install paramiko). The release builds bundle both.
"""
import argparse
import getpass
import hashlib
import io
import json
import locale
import os
import queue
import socket
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


def payload_dir():
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
    'done_install': ('Fertig. Jetzt das iDrive neu starten (Ein/Aus-Knopf ca. 30 s halten). Danach: Multimedia -> '
                     'Android Auto.',
                     'Done. Now restart iDrive (hold the power button for about 30 s). Then: Multimedia -> Android '
                     'Auto.'),
    'done_uninstall': ('Entfernt. Nach dem naechsten iDrive-Neustart ist der Menuepunkt weg.',
                       'Removed. The menu entry is gone after the next iDrive restart.'),
    'busy': ('Bitte warten ...', 'Please wait ...'),
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


def install(nbt, log, root=''):
    manifest = load_manifest()
    log('Package %s, %d files' % (manifest.get('version', '?'), len(manifest['files'])))
    preflight(nbt, log, root)
    nbt.run("chmod 700 '%s'" % (root + RT_DIR))
    # the manifest of the plugin goes last: the HMI must never see a half package
    files = sorted(manifest['files'], key=lambda e: e['target'].endswith('.manifest'))
    for entry in files:
        target = root + entry['target']
        log('  write %-46s %8d bytes' % (entry['target'].split('/')[-1], len(entry['data'])))
        nbt.put(entry['data'], target, entry['mode'])
    dpi = root + RT_DIR + '/vid.dpi'
    nbt.run("test -f '%s' || echo 100 > '%s'" % (dpi, dpi))
    nbt.run("rm -f '%s/disabled'" % (root + RT_DIR))
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
                root + RT_DIR + '/VERSION', root + RT_DIR + '/vid.dpi']
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


ACTIONS = {'install': install, 'uninstall': uninstall}


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
        else:
            ACTIONS[args.cli](nbt, print, args.root)
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
    buttons = ttk.Frame(frame)
    buttons.grid(row=5, column=0, columnspan=4, sticky='w', pady=10)
    output = scrolledtext.ScrolledText(frame, height=16, state='disabled', font=('Courier', 9))
    output.grid(row=6, column=0, columnspan=4, sticky='nsew')
    frame.rowconfigure(6, weight=1)
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
        for widget in widgets:
            widget.configure(state='disabled')
        log('--- ' + time.strftime('%H:%M:%S') + ' ' + tr('busy'))

        def work():
            nbt = None
            try:
                nbt = Nbt(host, port, user, password=password or None, key_file=args.key, log=log)
                if action == 'logs':
                    save_logs(nbt, log, extra, args.root)
                else:
                    ACTIONS[action](nbt, log, args.root)
            except InstallError as exc:
                log('%s: %s' % (tr('error'), exc))
            except Exception as exc:  # unexpected: show it instead of dying silently
                log('%s: %s: %s' % (tr('error'), type(exc).__name__, exc))
            finally:
                if nbt:
                    nbt.close()
                messages.put('\0done')

        threading.Thread(target=work, daemon=True).start()

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
    # development only: key login and installing below a test directory (e.g. in a QNX build VM)
    parser.add_argument('--key', help=argparse.SUPPRESS)
    parser.add_argument('--root', default='', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.cli:
        try:
            run_cli(args)
        except InstallError as exc:
            sys.exit('%s: %s' % (tr('error'), exc))
    else:
        run_gui(args)


if __name__ == '__main__':
    main()
