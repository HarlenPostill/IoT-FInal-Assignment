#!/usr/bin/env python3
"""Configure the local Docker stack without printing credentials."""
import argparse
import ast
from datetime import datetime
import os
from pathlib import Path
import re
import secrets

ROOT = Path(__file__).resolve().parents[1]


def read_lab_settings():
    text = (ROOT / "include/secrets.h").read_text()
    return {
        key: ast.literal_eval(literal)
        for key, literal in re.findall(r'^\s*#define\s+(LAB_\w+)\s+("(?:[^"\\]|\\.)*")', text, re.M)
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--local', action='store_true', help='Use local InfluxDB (the default).')
    parser.parse_args()
    path = ROOT / 'cloud/.env'
    previous = {}
    if path.exists():
        previous = dict(line.split('=', 1) for line in path.read_text().splitlines()
                        if line.strip() and not line.lstrip().startswith('#'))
        previous = {key: value.strip().strip("'") for key, value in previous.items()}
        if previous.get('INFLUX_URL') == 'http://localhost:8086' and previous.get('INFLUX_ADMIN_PASSWORD'):
            print('Local cloud/.env already configured; kept existing credentials.')
            return
    lab = read_lab_settings() if (ROOT / 'include/secrets.h').exists() else {}
    group = previous.get('LAB_GROUP_ID') or lab.get('LAB_GROUP_ID')
    if not group or 'YOUR_' in group:
        raise SystemExit('Set LAB_GROUP_ID in include/secrets.h or cloud/.env first.')
    token = secrets.token_hex(32)
    values = {
        'LAB_GROUP_ID': group,
        'INFLUX_URL': 'http://localhost:8086',
        'INFLUX_ORG': 'iot-local',
        'INFLUX_BUCKET': 'environment_lab',
        'INFLUX_TOKEN': token,
        'INFLUX_READ_TOKEN': token,
        'INFLUX_ADMIN_USER': 'admin',
        'INFLUX_ADMIN_PASSWORD': secrets.token_urlsafe(24),
        'GRAFANA_ADMIN_PASSWORD': previous.get('GRAFANA_ADMIN_PASSWORD') or secrets.token_urlsafe(24),
    }
    if any("'" in value or '\n' in value or '\r' in value for value in values.values()):
        raise SystemExit('A setting contains unsupported dotenv characters; configure .env manually.')
    if path.exists():
        backup = path.with_name('.env.' + datetime.now().strftime('%Y%m%d%H%M%S%f') + '.backup')
        fd = os.open(backup, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, 'w') as out:
            out.write(path.read_text())
        print('Preserved previous configuration in ignored ' + str(backup.relative_to(ROOT)))
    temp = path.with_name('.env.local.backup')
    fd = os.open(temp, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'w') as out:
        for key, value in values.items():
            out.write(f"{key}='{value}'\n")
    os.replace(temp, path)
    print('Configured local InfluxDB in ignored cloud/.env (mode 0600).')
    print('InfluxDB and Grafana passwords are in that file. Firmware secrets are unchanged.')


if __name__ == '__main__':
    main()
