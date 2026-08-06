# HedgeyOS Mini

HedgeyOS Mini is a tiny, touch-first environment for the original M5Paper. It boots directly to a home screen and currently includes:

- **Read** — a fast, SD-backed EPUB reader based on atomic14's DIY ESP32 ePub Reader.
- **Write** — a persistent ruled notepad with a large touch QWERTY keyboard adapted from M5Stack's official M5Paper FactoryTest keyboard.

The home screen uses the supplied Hedgey artwork, rendered in 16-level grayscale for the e-ink panel. The reader retains rocker navigation while adding touch book/chapter selection, denser two-column book lists, page and chapter navigation, resume positions, and a consistent top bar.

## Hardware and storage

This build targets the **first-generation M5Paper**. EPUB files remain on the SD card; flashing HedgeyOS Mini changes only the ESP32's internal flash. Notes are saved to `/fs/.hedgeyos-mini-note.txt` on the SD card when leaving Write or entering sleep.

## Build

Install PlatformIO, clone recursively, then run:

```bash
pio run -e m5_paper
```

The app uses a standalone factory partition and boots without M5Launcher. To create the single-file release image:

```bash
./scripts/build_release.sh
```

This produces `dist/HedgeyOS-Mini-M5Paper.bin`.

## Flash

The release image replaces the internal-flash contents, including M5Launcher. It does not write to the SD card.

```bash
esptool.py --chip esp32 --port /dev/ttyACM0 erase_flash
esptool.py --chip esp32 --port /dev/ttyACM0 --baud 921600 \
  write_flash --flash_size 4MB 0x0 dist/HedgeyOS-Mini-M5Paper.bin
```

## Credits

HedgeyOS Mini is MIT-licensed and builds on the work of Chris Greening/atomic14. The keyboard mappings and portrait layout are adapted from M5Stack's MIT-licensed M5Paper FactoryTest. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for details.
