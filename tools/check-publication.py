#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Fail closed on unexpected public files, personal details and history payloads."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

CONTACT = 'royakemartinsson@gmail.com'
ALLOWED_EMAILS = {CONTACT, 'noreply@github.com'}
EMAIL = re.compile(rb'[A-Za-z0-9_.+%-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}')
RULES = {
    'personal-home-path': rb'(?:/home/|/Users/)[A-Za-z0-9_.-]+|[A-Za-z]:[\\/]Users[\\/][A-Za-z0-9_.-]+',
    'private-ip': rb'\b(?:192\.168\.[0-9]{1,3}\.[0-9]{1,3}|10\.[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}|172\.(?:1[6-9]|2[0-9]|3[01])\.[0-9]{1,3}\.[0-9]{1,3})\b',
    'private-key': rb'-----BEGIN (?:RSA |EC |OPENSSH |DSA )?PRIVATE KEY-----',
    'access-token': rb'\b(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{40,}|AKIA[A-Z0-9]{16}|xox[baprs]-[A-Za-z0-9-]{20,})\b',
    'credential-url': rb'https?://[^\s/@:]+:[^\s/@]{4,}@',
}
FORBIDDEN = re.compile(r'(^|/)(private|plans|evidence|research|\.ssh|\.env)(/|$)|\.(o|ko|a|so|wav|mkv|mp4|deb|ddeb|pyc)$')

def git(root, *args):
    return subprocess.check_output(['git', '-C', str(root), *args], stderr=subprocess.DEVNULL)

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--root', type=Path, default=Path.cwd())
    ap.add_argument('--history', action='store_true')
    ap.add_argument('--private-markers', type=Path)
    args = ap.parse_args()
    root = args.root.resolve()
    errors = []
    allowed = set((root / '.publication-files').read_text().splitlines())
    if any(not x or x.startswith('/') or '..' in Path(x).parts for x in allowed):
        raise SystemExit('Invalid publication manifest')
    images = json.loads((root / '.publication-images.json').read_text())
    markers = json.loads(args.private_markers.read_text()) if args.private_markers else []
    def scan(label, data):
        # Diagnostics identify a file/rule, never print matched sensitive values.
        for name, pattern in RULES.items():
            if re.search(pattern, data): errors.append(f'{label}: {name}')
        for address in EMAIL.findall(data):
            if address.decode(errors='replace') not in ALLOWED_EMAILS:
                errors.append(f'{label}: unapproved email')
        if any(m.lower().encode() in data.lower() for m in markers):
            errors.append(f'{label}: private marker')
    try:
        files = set(git(root, 'ls-files', '-z').decode().rstrip('\0').split('\0'))
    except subprocess.CalledProcessError:
        files = {str(p.relative_to(root)) for p in root.rglob('*') if p.is_file()}
    if files != allowed:
        errors.append('File allowlist mismatch: ' + repr(sorted(files ^ allowed)))
    for name in sorted(files):
        path = root / name
        if FORBIDDEN.search(name): errors.append(f'{name}: forbidden path')
        if path.is_symlink():
            errors.append(f'{name}: symlink not allowed'); continue
        if not path.is_file():
            errors.append(f'{name}: missing file'); continue
        data = path.read_bytes()
        if name in images:
            if hashlib.sha256(data).hexdigest() != images[name]:
                errors.append(f'{name}: image requires visual review and new approved hash')
        elif b'\0' in data:
            errors.append(f'{name}: unexpected binary')
        else: scan(name, data)
    if args.history:
        scan('commit metadata/messages', git(root, 'log', '--all', '--format=%an <%ae>%n%cn <%ce>%n%B'))
        inventory = git(root, 'rev-list', '--all', '--objects').decode().splitlines()
        proc = subprocess.Popen(['git', '-C', str(root), 'cat-file', '--batch'], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        for line in inventory:
            oid, _, name = line.partition(' ')
            proc.stdin.write((oid+'\n').encode()); proc.stdin.flush()
            hdr = proc.stdout.readline().decode().split()
            data = proc.stdout.read(int(hdr[2])); proc.stdout.read(1)
            if hdr[1] != 'blob': continue
            if FORBIDDEN.search(name): errors.append(f'history {name}: forbidden path')
            if b'\0' in data:
                if hashlib.sha256(data).hexdigest() not in images.values():
                    errors.append(f'history {name}: unapproved binary')
            else: scan('history '+name, data)
        proc.stdin.close(); proc.wait()
    if errors:
        print('\n'.join(sorted(set(errors))), file=sys.stderr)
        return 1
    print(f'Publication check passed: {len(files)} allowed files' + ('; reachable history checked' if args.history else ''))
    return 0

if __name__ == '__main__':
    sys.exit(main())
