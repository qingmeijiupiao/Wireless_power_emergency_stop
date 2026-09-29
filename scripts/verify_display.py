"""COM3 bring-up 页传输检查；不代表物理屏幕外观已经通过验收。"""
import argparse
from pathlib import Path
import time
import serial
import re

parser = argparse.ArgumentParser()
parser.add_argument('--port', default='COM3')
parser.add_argument('--rounds', type=int, default=1)
args = parser.parse_args()
if args.rounds < 1:
    parser.error('--rounds must be positive')
log = []
baseline = None
with serial.Serial(args.port, 115200, timeout=0.2) as device:
    # 只切换显示页，不发送无线控制命令。
    device.write(b'p')
    for command, page in ([(str(n).encode(), n) for n in range(10)] + [(b'm', 10), (b'l', 11), (b't', 12)]) * args.rounds:
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
out = Path(__file__).resolve().parents[1]/'docs/serial-verification.log'
out.write_text('\n'.join(log), encoding='utf-8')
print(f'All {13 * args.rounds} frames transmitted; failures baseline/final={baseline}/{failures}; meter page selected. Log: {out}')
