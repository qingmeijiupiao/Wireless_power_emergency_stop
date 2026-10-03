# Wireless Power Emergency Stop v0.4.0

## APP update

Writes only the application partition at `0x10000`. It preserves NVS pairing,
calibration and business settings. Use it only when the bootloader and partition
layout have not changed.

## Merged firmware

Writes a complete image at `0x0`, including bootloader, partition table and APP.
Use it for first-time installation, recovery, or partition/bootloader changes.
The image blanks NVS at `0x9000-0xEFFF`, so pairing and calibration data are lost.

Neither image provides OTA. Both are flashed manually over USB/serial.
