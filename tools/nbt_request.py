"""Submit one command to the SSH session held open by tools/nbt_ssh_session.py. Never replays a command.

Usage:
  python tools/nbt_request.py "uname -a"
  python tools/nbt_request.py "tr -d '\\015' | sh -s" --input-file scripts/car-dev-leftovers.sh --timeout 60
  python tools/nbt_request.py "cat /fs/ram3/nbt-aa-boot.log" --output-file boot.log
"""
import argparse
import base64
import json
import time
import uuid
from pathlib import Path

QUEUE = Path(__file__).resolve().parent.parent / '.local' / 'ssh-session'

p = argparse.ArgumentParser()
p.add_argument('command')
p.add_argument('--input-file')
p.add_argument('--output-file')
p.add_argument('--timeout', type=int, default=30, help='remote command timeout in seconds (session caps at 120)')
a = p.parse_args()
state = json.loads((QUEUE / 'status.json').read_text())
if state['state'] not in ('ready', 'busy'):
    raise SystemExit('SSH session is not ready: ' + str(state))
name = 'task-' + time.strftime('%Y%m%dT%H%M%S') + '-' + uuid.uuid4().hex[:10]
req = {'command': a.command, 'timeout': max(5, min(a.timeout, 120))}
if a.input_file:
    req['stdin_base64'] = base64.b64encode(Path(a.input_file).read_bytes()).decode()
temporary = QUEUE / (name + '.tmp')
temporary.write_text(json.dumps(req), encoding='utf-8')
temporary.replace(QUEUE / (name + '.request.json'))
result_path = QUEUE / (name + '.result.json')
deadline = time.monotonic() + req['timeout'] + 15
while not result_path.exists():
    if time.monotonic() > deadline:
        raise SystemExit('Result pending; do not retry. Inspect ' + str(result_path))
    time.sleep(.1)
r = json.loads(result_path.read_text())
if a.output_file and r.get('exit_code') == 0 and not r.get('error'):
    Path(a.output_file).write_bytes(base64.b64decode(r['stdout_base64']))
    print('Saved:', a.output_file)
else:
    print(r.get('stdout', ''))
print(r.get('stderr', ''))
if r.get('error'):
    print(r['error'])
print('Evidence:', result_path)
raise SystemExit(r.get('exit_code', 1))
