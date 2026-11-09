// OpenSSH KnownHostsCommand: trust only the fingerprint read at the VM console.
const crypto = require('node:crypto');
const fs = require('node:fs');
const [expected, destination, host, type, key] = process.argv.slice(2);
if (host !== '192.168.56.10' || type !== 'ssh-rsa' || !key || key === 'NONE') process.exit(0);
if (!/^[A-Za-z0-9+/=]+$/.test(key)) process.exit(1);
const blob = Buffer.from(key, 'base64');
const actual = expected.startsWith('SHA256:')
  ? 'SHA256:' + crypto.createHash('sha256').update(blob).digest('base64').replace(/=+$/, '')
  : 'MD5:' + crypto.createHash('md5').update(blob).digest('hex').match(/../g).join(':');
if (actual !== expected) { console.error('Console fingerprint mismatch.'); process.exit(1); }
const line = `${host} ${type} ${key}\n`;
if (fs.existsSync(destination)) {
  if (fs.readFileSync(destination, 'utf8') !== line) process.exit(1);
} else {
  fs.writeFileSync(destination, line, { flag: 'wx' });
}
process.stdout.write(line);