# PicoCalc Meshtastic

A Meshtastic client for the **PicoCalc** with a **Raspberry Pi Pico 2 W**. It runs as a UF2 app launched from the SD card by the uf2loader bootloader, and talks to a Meshtastic radio (for example a Heltec V3 or Wireless Tracker) over **Bluetooth LE**, in the same way the Android app does.

Tested on a PicoCalc (Pico 2 W, uf2loader) with a Heltec V3 running Meshtastic firmware 2.7.

## Install

1. Download `picocalc_meshtastic.uf2` from the latest [GitHub release](../../releases/latest), or build it yourself. Copy it to the `pico2-apps/` folder on the PicoCalc SD card.
2. Start the PicoCalc and choose **picocalc_meshtastic** in the loader.
3. Press **F5 (Conn)** and then **S** to scan. Select your radio and press **Enter**.
4. When the PIN prompt appears, type the PIN shown on the radio's screen. If the radio uses a fixed PIN, the default is `123456`. Press **Enter**.

Once paired, the bond is stored on the SD card, and the app reconnects automatically on later launches unless you turn auto-connect off with **A**.

The app keeps its data in `/meshtastic/` on the SD card: `settings.bin`, `nodes.bin`, `messages.bin`, `bt_keys.bin` and the map tile cache in `tiles/`. Delete that folder to reset the app. The app never writes to the Pico's flash.

## Tabs and keys

The tabs follow the Android app:

| Key | Tab | What it does |
|-----|-----|--------------|
| F1 | Chat | Lists conversations (channels and DMs) with unread counts. Enter opens one; type and press Enter to send. Up/Down/PgUp/PgDn scroll and Esc goes back. Each message shows its delivery status (Sent / Acknowledged / Delivered / Failed). |
| F2 | Node | Node list (you, then favourites, then most recently heard). Type to filter. Enter shows details and actions: DM, traceroute, request position, exchange user info, favourite, remove. |
| F3 | Map | Plots nodes on a Web Mercator map, over downloaded map tiles when enabled (see [Wi-Fi and map tiles](#wi-fi-and-map-tiles)). Arrows pan, `+`/`-` zoom, Tab selects a node, `c` centres, `f` fits all nodes, Enter opens the node. |
| F4 | Chan | Channels and LoRa summary. `U` shows the channel share URL (also printed on USB serial). |
| F5 | Conn | Bluetooth: `S` starts/stops a scan, Enter connects, `D` disconnects, `A` toggles auto-connect, `O` shows non-Meshtastic devices, `X` forgets all pairings. |
| F6 | Set | Radio settings (owner name, role, region, modem preset, hop limit, reboot), app settings (sound, units, backlights, timezone, clear history) and Wi-Fi & map settings. |
| F7 | Stat | Your node's telemetry: battery, channel utilisation, air time, uptime. |
| F8 | Log | Debug log. Up/Down/PgUp/PgDn/Home/End scroll it. |
| F9 | Help | Key reference. |
| F10 | Info | About the app and the connected radio. |

**Ctrl+Left** and **Ctrl+Right** move to the previous or next tab.

## Wi-Fi and map tiles

The Pico 2 W's Wi-Fi is used only to download map tiles; the radio link is always Bluetooth.

1. Press **F6 (Set)** and scroll to **Wi-Fi & map**.
2. Turn **Wi-Fi** on, then select **Network**. The app scans; pick your network (or **Other...** for a hidden one) and type the password.
3. **Status** shows the connection and signal; press Enter on it to reconnect. The status bar shows a Wi-Fi icon while Wi-Fi is on.
4. **Map tiles** chooses the background for the Map tab:

| Source | Notes |
|--------|-------|
| OpenStreetMap | Default. |
| OpenStreetMap Dark | OpenStreetMap tiles recoloured on the device for a dark theme. Shares the OpenStreetMap cache. |
| OpenTopoMap | Terrain and contours, up to zoom 17. |
| Humanitarian | OSM France's Humanitarian style. |
| Off (plain map) | No tiles, as in earlier versions. |

Tiles are fetched over HTTPS and cached on the SD card in `/meshtastic/tiles/`, so areas you have viewed still work without Wi-Fi. Areas not in the cache show "Map tiles need Wi-Fi". **Clear map tile cache** deletes the cache.

Notes:

- Please respect the tile servers' usage policies (for example the [OSM tile usage policy](https://operations.osmfoundation.org/policies/tiles/)). The app sends an identifying User-Agent, downloads only what's on screen, and caches everything it fetches. Don't use it for bulk downloading.
- The Wi-Fi password is stored unencrypted in `/meshtastic/settings.bin` on the SD card.
- TLS server certificates are not verified (the app has no CA store). Tiles are public data, but a network attacker could substitute images.
- To free RAM for Wi-Fi and TLS, the message history kept in memory is limited to the latest 200 messages.

## Building

You need macOS or Linux with `git`, `curl`, `cmake` and `python3`.

```sh
./setup.sh        # fetches toolchain, pico-sdk 2.2.0, nanopb, protobufs into .deps/
export PICO_TOOLCHAIN_PATH=$PWD/.deps/arm-gnu-toolchain-14.2.rel1-<host>-arm-none-eabi
export PATH=$PICO_TOOLCHAIN_PATH/bin:$PATH
cmake -B build -DCMAKE_BUILD_TYPE=Release
make -C build -j8
```

The build produces `build/picocalc_meshtastic.uf2`.

### Releases

Every push to `main` triggers [.github/workflows/release.yml](.github/workflows/release.yml), which builds the app and publishes a GitHub release with the `.uf2` attached:

- Each release increments the patch number of the last `vX.Y.Z` tag, e.g. v1.0.4 → v1.0.5. The version is compiled into the app and shown on the About tab.
- To bump the major or minor version, raise the base `APP_VERSION` in `CMakeLists.txt` (e.g. to `1.1.0`). The next release uses it, and later releases continue from there.
- Pushes that only change Markdown files or `.gitignore` don't create a release.
- A release can also be triggered by hand from the repository's **Actions** tab.

### Layout

- `src/hw/`: PicoCalc drivers (ILI9488 LCD, I2C keyboard, SD over SPI, PWM sound).
- `src/mesh/ble_link.c`: BTstack BLE central. It handles scanning, pairing, GATT discovery and the ToRadio/FromRadio/FromNum pump.
- `src/mesh/mesh.c`: Meshtastic protocol handling, node DB, messages and admin.
- `src/ui/`: framebuffer graphics, widgets and the screens.
- `src/bt_glue.c`: CYW43 + BTstack setup, with the pairing database on the SD card instead of flash.
- `src/net/`: Wi-Fi connection manager, lwIP/mbedTLS configuration and a small HTTP(S) client.
- `src/map/`: map tile sources, SD cache, the on-screen tile renderer and a streaming PNG decoder.
- `src/proto/`: generated nanopb sources (`tools/gen_protos.sh`).

## Debugging over USB

While the app runs, the Pico's USB port is a serial console (115200 baud). It prints the app log, which is replayed when a terminal connects. Single-character commands:

| Key | Action |
| --- | --- |
| `B` | Reboot into BOOTSEL safely (between keyboard transfers) |
| `K` | Keyboard I2C statistics |
| `I` | Keyboard controller probe (FIFO and battery read) |
| `Wrv` | Raw keyboard-controller register write: register byte `r`, value byte `v` |
| `!x` | Inject the key `x` (`\r` = Enter, raw bytes for F-keys, e.g. `0x81` = F1) |
| `P` | Dump the framebuffer as hex (`FB-BEGIN` ... `FB-END`) |
| `R` | Force a full screen refresh |
| `T` | LCD write/readback stress test |
| `M` | Heap usage |

To flash a new build without the SD card, send `B`, then run `picotool load -x build/picocalc_meshtastic.uf2`. This writes into the loader's app slot and starts the app. Avoid `picotool -f`, which resets the Pico at an arbitrary moment. If it interrupts a keyboard transfer, the PicoCalc's keyboard controller can hang until the next power cycle.

Notes:

- While USB is plugged in, the Pico stays powered when the PicoCalc is switched off. To get back to the loader menu, unplug USB, then switch on while holding F1, Up or F5.
- If the keyboard stops responding, unplug USB and switch the PicoCalc off and on again.

## Updating the PicoCalc keyboard firmware

The keyboard controller is an STM32F103, flashed through the PicoCalc's own USB-C port. The official firmware is `Bin/PicoCalc_BIOS_v1.7.bin` from [clockworkpi/PicoCalc](https://github.com/clockworkpi/PicoCalc).

1. Install stm32flash: `brew install stm32flash`.
2. On the back of the mainboard, set **DIP 1 ON** (boot mode, and routes the serial chip to the STM32) and **DIP 2 OFF** (routes USB-C to the CH340 serial chip).
3. Remove a battery for a few seconds so the STM32 resets, then connect USB-C and power on. The screen stays blank. The CH340 is powered from the PicoCalc's 3.3V rail, so it only enumerates while the PicoCalc is on. It appears as `/dev/cu.usbserial-*`.
4. Back up the current firmware, then write and verify the new one:
   ```sh
   stm32flash -r kbd_backup.bin -S 0x08000000:65536 /dev/cu.usbserial-10
   stm32flash -w PicoCalc_BIOS_v1.7.bin -v -S 0x08000000 /dev/cu.usbserial-10
   ```
5. Set DIP 1 OFF and DIP 2 back to its previous position, then remove a battery briefly to restart the STM32.

## Limitations

- Map tiles need Wi-Fi to download (cached tiles work offline). Interlaced PNGs and tiles wider than 256 px are not supported.
- Keyboard input is ASCII only, but received UTF-8 is shown using a Latin-1 font.
- One radio connection at a time. The radio is reached over Bluetooth LE only (no Wi-Fi/TCP or serial connection to the radio).
- The PicoCalc's factory keyboard firmware has no keyboard-backlight control and doesn't report the battery level. The app detects this: the keyboard-backlight setting shows "n/a" and the battery indicator is hidden. Screen brightness works on all firmware versions. To get both features, update the keyboard firmware (see below).
- Some advanced config sections (modules, MQTT, security keys) aren't editable.
