#!/usr/bin/env python3
"""Local controls for a synthetic microphone, publishing the actuator's real source topic."""
import json
import math
import os
from pathlib import Path
import re
import secrets
import socket
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from check_pipeline import text, packet, receive

ROOT = Path(__file__).resolve().parents[2]
CONFIG = Path(os.environ.get('MOCK_CONFIG_DIR', ROOT / 'include'))


def constant(filename, name):
    content = (CONFIG / filename).read_text()
    match = re.search(r'\b' + name + r'\[\]\s*=\s*"([^"]+)"', content)
    if not match:
        raise ValueError('Missing configuration: ' + name)
    return match[1]


class Simulator:
    def __init__(self):
        self.group = os.environ['LAB_GROUP_ID']
        self.device = constant('cabinet_config.h', 'SOURCE_DEVICE')
        self.host = constant('noise_config.h', 'MQTT_HOST')
        content = (CONFIG / 'noise_config.h').read_text()
        self.port = int(re.search(r'MQTT_PORT\s*=\s*(\d+)', content)[1])
        self.user = constant('noise_config.h', 'MQTT_USER')
        self.password = constant('noise_config.h', 'MQTT_PASSWORD')
        self.topic = f'iot2026/{self.group}/noise/{self.device}/telemetry'
        self.lock = threading.Lock()
        self.boot = secrets.token_hex(8)
        self.started = time.monotonic()
        self.enabled = False
        self.level = 65.0
        self.fault = False
        self.sequence = 0
        self.sent = 0
        self.last_sent = None
        self.error = ''
        self.listening = False
        self.listener_error = ''
        self.cabinet = None
        self.cabinet_at = None
        self.last_payload = None

    def connect(self, role):
        sock = socket.create_connection((self.host, self.port), timeout=3)
        sock.settimeout(3)
        try:
            body = text('MQTT') + bytes([4, 0xc2]) + struct.pack('!H', 15)
            body += text(f'mock-{role}-{self.boot}') + text(self.user) + text(self.password)
            sock.sendall(packet(0x10, body))
            if receive(sock) != (0x20, b'\x00\x00'):
                raise ConnectionError('MQTT connection rejected')
            return sock
        except Exception:
            sock.close()
            raise

    def payload(self):
        self.sequence += 1
        state = 'sensor_fault' if self.fault else ('high' if self.level >= 95 else 'warning' if self.level >= 80 else 'safe')
        result = dict(group=self.group, device=self.device, location='building_exterior',
                      firmware='mock-interface-1.0', simulated=True, boot_id=self.boot,
                      sensor_ok=not self.fault, state=state,
                      state_code={'safe': 0, 'warning': 1, 'high': 2, 'sensor_fault': 3}[state],
                      clipped=False, calibrated=False, warning_db=80.0, limit_db=95.0,
                      offset_db=0.0, hysteresis_db=2.0, sequence=self.sequence,
                      sample_age_ms=0, uptime_ms=int((time.monotonic()-self.started)*1000),
                      window_ms=500, sample_rate_hz=16000, rssi_dbm=-50)
        if not self.fault:
            result.update(estimated_spl_db=self.level, dbfs=self.level-120.0)
        return result

    def publisher(self):
        sock = None
        while True:
            started = time.monotonic()
            try:
                # Controls wait for an in-flight publish before reporting stopped.
                with self.lock:
                    if self.enabled:
                        if sock is None:
                            sock = self.connect('publisher')
                        payload = self.payload()
                        ident = self.sequence % 65535 or 65535
                        body = text(self.topic) + struct.pack('!H', ident) + json.dumps(payload).encode()
                        sock.sendall(packet(0x32, body))  # QoS 1, never retained.
                        if receive(sock) != (0x40, struct.pack('!H', ident)):
                            raise ConnectionError('Missing publish acknowledgement')
                        self.sent += 1
                        self.last_sent = time.time()
                        self.last_payload = payload
                        self.error = ''
                    elif sock is not None:
                        sock.sendall(b'\xe0\x00')
                        sock.close()
                        sock = None
            except (OSError, ValueError, ConnectionError) as exc:
                with self.lock:
                    self.error = str(exc)
                if sock:
                    sock.close()
                    sock = None
            time.sleep(max(0.05, 1.0-(time.monotonic()-started)))

    def subscriber(self):
        topic = f'iot2026/{self.group}/cabinet/+/telemetry'
        while True:
            try:
                with self.connect('observer') as sock:
                    sock.sendall(packet(0x82, b'\x00\x01' + text(topic) + b'\x00'))
                    if receive(sock) != (0x90, b'\x00\x01\x00'):
                        raise ConnectionError('MQTT subscription rejected')
                    with self.lock:
                        self.listening = True
                        self.listener_error = ''
                    ping_at = time.monotonic()
                    while True:
                        if time.monotonic()-ping_at >= 5:
                            sock.sendall(b'\xc0\x00')
                            ping_at = time.monotonic()
                        try:
                            header, body = receive(sock)
                        except socket.timeout:
                            continue
                        if header >> 4 != 3:
                            continue
                        length = struct.unpack('!H', body[:2])[0]
                        data = json.loads(body[2+length:])
                        if data.get('group') == self.group and data.get('source_device') == self.device:
                            with self.lock:
                                self.cabinet = data
                                self.cabinet_at = time.time()
            except (OSError, ValueError, ConnectionError, struct.error) as exc:
                with self.lock:
                    self.listening = False
                    self.listener_error = str(exc)
                time.sleep(2)

    def status(self):
        with self.lock:
            return dict(enabled=self.enabled, level=self.level, fault=self.fault,
                        sent=self.sent, last_sent=self.last_sent, error=self.error,
                        listening=self.listening, listener_error=self.listener_error,
                        group=self.group, device=self.device, broker=f'{self.host}:{self.port}',
                        topic=self.topic, cabinet=self.cabinet, cabinet_at=self.cabinet_at,
                        last_payload=self.last_payload)

    def control(self, data):
        if not isinstance(data, dict) or set(data)-{'enabled', 'level', 'fault'}:
            raise ValueError('Expected enabled, level and/or fault')
        if 'level' in data and (isinstance(data['level'], bool) or not isinstance(data['level'], (int, float))
                                or not math.isfinite(data['level']) or not 30 <= data['level'] <= 120):
            raise ValueError('Level must be between 30 and 120 dB')
        for name in ('enabled', 'fault'):
            if name in data and not isinstance(data[name], bool):
                raise ValueError(name + ' must be true or false')
        with self.lock:
            if 'level' in data:
                self.level = float(data['level'])
            for name in ('enabled', 'fault'):
                if name in data:
                    setattr(self, name, data[name])
        return self.status()


class Handler(BaseHTTPRequestHandler):
    def reply(self, status, body, content_type='application/json'):
        self.send_response(status)
        self.send_header('Content-Type', content_type)
        self.send_header('Cache-Control', 'no-store')
        self.send_header('X-Content-Type-Options', 'nosniff')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == '/':
            self.reply(200, Path(__file__).with_name('index.html').read_bytes(), 'text/html; charset=utf-8')
        elif self.path == '/api/status':
            self.reply(200, json.dumps(self.server.simulator.status()).encode())
        else:
            self.reply(404, b'{"error":"Not found"}')

    def do_POST(self):
        if self.path != '/api/control':
            self.reply(404, b'{"error":"Not found"}')
            return
        # Only same-origin browser requests can operate the publisher.
        if self.headers.get('Origin') not in (None, 'http://' + self.headers.get('Host', '')):
            self.reply(403, b'{"error":"Origin rejected"}')
            return
        try:
            size = int(self.headers.get('Content-Length', '0'))
            if not 0 < size <= 1024:
                raise ValueError('Invalid request size')
            data = json.loads(self.rfile.read(size))
            result = self.server.simulator.control(data)
        except (ValueError, TypeError) as exc:
            self.reply(400, json.dumps({'error': str(exc)}).encode())
            return
        self.reply(200, json.dumps(result).encode())

    def log_message(self, *_):
        pass


def main():
    sim = Simulator()
    for target in (sim.publisher, sim.subscriber):
        threading.Thread(target=target, daemon=True).start()
    server = ThreadingHTTPServer(('0.0.0.0', 8765), Handler)
    server.simulator = sim
    print('Mock microphone interface on port 8765; publishing STOPPED until Start is pressed.', flush=True)
    server.serve_forever()


if __name__ == '__main__':
    main()
