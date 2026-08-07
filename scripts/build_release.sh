#!/usr/bin/env bash
set -euo pipefail

pio run -e m5_paper
mkdir -p dist
platformio_home="${PLATFORMIO_CORE_DIR:-${HOME}/.platformio}"
esptool="${ESPTOOL:-${platformio_home}/packages/tool-esptoolpy/esptool.py}"
python3 "${esptool}" --chip esp32 merge_bin \
  --flash_mode dio --flash_freq 80m --flash_size 4MB \
  -o dist/HedgeyOS-Mini-M5Paper.bin \
  0x1000 .pio/build/m5_paper/bootloader.bin \
  0x8000 .pio/build/m5_paper/partitions.bin \
  0x10000 .pio/build/m5_paper/firmware.bin

cp .pio/build/m5_paper/firmware.bin dist/HedgeyOS-Mini-M5Paper-app.bin

sha256sum dist/HedgeyOS-Mini-M5Paper.bin dist/HedgeyOS-Mini-M5Paper-app.bin
