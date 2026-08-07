# HedgeyOS Mini

HedgeyOS Mini is a tiny, touch-first environment for the original M5Paper. It boots directly to a home screen and currently includes:

- **Read** — a fast, SD-backed EPUB reader based on atomic14's DIY ESP32 ePub Reader.
- **Write** — a persistent ruled notepad with a large touch QWERTY keyboard adapted from M5Stack's official M5Paper FactoryTest keyboard, plus optional BLE keyboard input.

The home screen uses the supplied Hedgey artwork, rendered in 16-level grayscale for the e-ink panel. The reader retains rocker navigation while adding touch book/chapter selection, denser two-column book lists, page and chapter navigation, resume positions, and a consistent top bar.

## Hardware and storage

This build targets the **first-generation M5Paper**. EPUB files remain on the SD card; flashing HedgeyOS Mini changes only the ESP32's internal flash. Notes are saved to `/fs/.hedgeyos-mini-note.txt` on the SD card when leaving Write or entering sleep.

## Bluetooth keyboards

Opening Write makes one quiet, five-second search for a BLE HID keyboard. A tiny line below the soft keyboard shows the result; the on-screen keyboard remains available whether or not a hardware keyboard is found. HedgeyOS Mini does not retry in the background. Close Write and open it again to make another attempt.

The first successful pairing may display a six-digit code to type on the keyboard followed by Enter. Bonding data and an ordered list of up to five keyboards are retained in internal NVS, so subsequent sessions reconnect without repeating the code. When several saved keyboards are nearby, the earliest saved reachable keyboard is selected.

## Build

Install PlatformIO, clone recursively, then run:

```bash
pio run -e m5_paper
```

The app uses a standalone factory partition and boots without M5Launcher. To create the single-file release image:

```bash
./scripts/build_release.sh
```

This produces a complete fresh-install image at `dist/HedgeyOS-Mini-M5Paper.bin` and an NVS-preserving update image at `dist/HedgeyOS-Mini-M5Paper-app.bin`.

## Flash

The complete release image replaces the internal-flash contents, including M5Launcher. It does not write to the SD card. This fresh-install procedure also clears saved Bluetooth pairings:

```bash
esptool.py --chip esp32 --port /dev/ttyACM0 erase_flash
esptool.py --chip esp32 --port /dev/ttyACM0 --baud 921600 \
  write_flash --flash_size 4MB 0x0 dist/HedgeyOS-Mini-M5Paper.bin
```

For later HedgeyOS Mini updates, preserve notes on the SD card and Bluetooth pairings in NVS by writing only the partition table and application (do not erase flash):

```bash
esptool.py --chip esp32 --port /dev/ttyACM0 --baud 921600 \
  write_flash --flash_size 4MB \
  0x8000 .pio/build/m5_paper/partitions.bin \
  0x10000 dist/HedgeyOS-Mini-M5Paper-app.bin
```

## Credits

HedgeyOS Mini is MIT-licensed and builds on the work of Chris Greening/atomic14. The keyboard mappings and portrait layout are adapted from M5Stack's MIT-licensed M5Paper FactoryTest. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for details.
