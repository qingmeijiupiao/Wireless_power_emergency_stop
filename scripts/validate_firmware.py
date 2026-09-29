"""Check the actual generated APP and merged firmware layout for CI."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
app = (root / 'build/Wireless_power_emergency_stop.bin').read_bytes()
merged = (root / 'Wireless_power_emergency_stop_merged.bin').read_bytes()
if not app or len(app) > 0x100000:
    raise SystemExit(f'Invalid APP size: {len(app)}')
if merged[0x10000:0x10000 + len(app)] != app:
    raise SystemExit('APP does not match merged image at 0x10000')
if any(value != 0xff for value in merged[0x9000:0xf000]):
    raise SystemExit('Merged NVS range is not blank')
print(f'Firmware layout OK: APP={len(app)} bytes, merged={len(merged)} bytes')
