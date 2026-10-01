"""Diagnostic page transfer check; physical STOP must be released during this test."""
import argparse
from pathlib import Path
import time
import serial
import re

parser = argparse.ArgumentParser()
parser.add_argument('--port', default='COM17')
parser.add_argument('--rounds', type=int, default=1)
args = parser.parse_args()
if args.rounds < 1:
    parser.error('--rounds must be positive')
log = []
baseline = None
device = serial.Serial()
device.port = args.port
device.baudrate = 115200
device.timeout = 0.2
device.dtr = False
device.rts = False
with device:
    # 只切换显示页，不发送无线控制命令。
    for command, page in ([(str(n).encode(), 200 + n) for n in range(10)] + [(b't', 212)]) * args.rounds:
        device.reset_input_buffer()
        device.write(command)
        received = ''
        deadline = time.monotonic() + 4
        while time.monotonic() < deadline:
            received += device.read(4096).decode('utf-8', errors='replace')
            if f'FRAME_OK page={page} ' in received:
                break
        log.append(received)
        if f'FRAME_OK page={page} ' not in received or 'FRAME_FAILED' in received:
            raise RuntimeError(f'Page {page} failed: {received}')
        failures = int(re.search(r'failures=(\d+)', received)[1])
        if baseline is None:
            baseline = failures
        if failures != baseline:
            raise RuntimeError(f'Failure count increased: {received}')
        print(f'PASS page={page}')
    device.write(b'm')
out = Path(__file__).resolve().parents[1]/'tmp/serial-verification.log'
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text('\n'.join(log), encoding='utf-8')
print(f'All {11 * args.rounds} frames transmitted; failures baseline/final={baseline}/{failures}; live page selected. Log: {out}')
