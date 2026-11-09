"""Hold one SSH session to the NBT open and execute queued requests from tools/nbt_request.py.

You type the NBT root password yourself; it is never stored. The host key is pinned: put the NBT's key into
.local/nbt_known_hosts first (OpenSSH known_hosts format, e.g. from `ssh-keyscan -t rsa 169.254.199.99` after you
have compared the fingerprint). Requests and results live in .local/ssh-session/ (ignored by git).

Requires: pip install "paramiko>=3.4,<4"   (paramiko 4+ no longer accepts the NBT's ssh-rsa host key)
Usage:    python tools/nbt_ssh_session.py [--host 169.254.199.99]
"""
import argparse
import base64
import getpass
import json
import sys
import time
import traceback
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LOCAL = ROOT / '.local'
sys.path.insert(0, str(LOCAL / 'python-packages'))  # optional private package folder
import paramiko  # noqa: E402

QUEUE = LOCAL / 'ssh-session'
DEFAULT_HOST = '169.254.199.99'


def write_json(path, data):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(data, indent=2), encoding='utf-8')
    temporary.replace(path)


def status(state, **extra):
    write_json(QUEUE / 'status.json', {'state': state, **extra})


def execute(client, request):
    channel = client.get_transport().open_session(timeout=10)
    channel.settimeout(15)
    channel.exec_command(request['command'])
    payload = base64.b64decode(request.get('stdin_base64', ''), validate=True)
    if payload:
        channel.sendall(payload)
    channel.shutdown_write()
    out, err = bytearray(), bytearray()
    deadline = time.monotonic() + min(request.get('timeout', 30), 120)
    try:
        while True:
            while channel.recv_ready():
                out.extend(channel.recv(32768))
            while channel.recv_stderr_ready():
                err.extend(channel.recv_stderr(32768))
            if len(out) + len(err) > 1024 * 1024:
                raise RuntimeError('Output limit exceeded; command was not retried.')
            if channel.exit_status_ready() and not channel.recv_ready() and not channel.recv_stderr_ready():
                break
            if time.monotonic() > deadline:
                raise TimeoutError('Command timeout; remote outcome may be unknown. Do not retry automatically.')
            time.sleep(0.05)
        return {'exit_code': channel.recv_exit_status(),
                'stdout': out.decode('utf-8', errors='replace'),
                'stdout_base64': base64.b64encode(out).decode('ascii'),
                'stderr': err.decode('utf-8', errors='replace')}
    finally:
        channel.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--host', default=DEFAULT_HOST)
    parser.add_argument('--known-hosts', default=str(LOCAL / 'nbt_known_hosts'))
    args = parser.parse_args()
    QUEUE.mkdir(parents=True, exist_ok=True)
    client = paramiko.SSHClient()
    client.load_host_keys(args.known_hosts)
    if args.host != DEFAULT_HOST:
        # Permit an address change only with the pinned key of the original address.
        original = client.get_host_keys().lookup(DEFAULT_HOST)
        if not original:
            raise RuntimeError('Pinned NBT host key for %s is missing' % DEFAULT_HOST)
        for key_type, key in original.items():
            client.get_host_keys().add(args.host, key_type, key)
    client.set_missing_host_key_policy(paramiko.RejectPolicy())
    status('waiting_for_password')
    print('NBT SSH session: root@' + args.host)
    print('Password input is invisible. Leave this window open after login.')
    password = getpass.getpass('NBT root password: ')
    try:
        client.connect(args.host, username='root', password=password, look_for_keys=False, allow_agent=False,
                       timeout=60, banner_timeout=60, auth_timeout=60)
    finally:
        del password
    client.get_transport().set_keepalive(15)
    status('ready')
    print('Connected. tools/nbt_request.py can now use this session without more password prompts.')
    print('Close this window to disconnect. The session ends after one idle hour.')
    last_activity = time.monotonic()
    try:
        while time.monotonic() - last_activity < 3600:
            if not client.get_transport().is_active():
                raise ConnectionError('SSH disconnected; a new login is required.')
            for request_path in sorted(QUEUE.glob('*.request.json')):
                result_path = request_path.with_name(request_path.name.replace('.request.json', '.result.json'))
                if result_path.exists():
                    continue
                request = json.loads(request_path.read_text(encoding='utf-8-sig'))
                last_activity = time.monotonic()
                status('busy', request=request_path.name)
                try:
                    result = execute(client, request)
                except Exception as exc:
                    result = {'error': str(exc), 'traceback': traceback.format_exc()}
                write_json(result_path, result)
                status('ready')
            time.sleep(0.2)
    finally:
        client.close()
        status('closed')


if __name__ == '__main__':
    try:
        main()
    except Exception as exc:
        QUEUE.mkdir(parents=True, exist_ok=True)
        status('failed', error=str(exc))
        print(type(exc).__name__ + ': ' + str(exc))
        input('Press Enter to close')
