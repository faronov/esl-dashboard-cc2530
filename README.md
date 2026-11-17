# esl-dashboard-cc2530

Open firmware that turns an LG ESL29 electronic shelf label board (CC2530)
with a Waveshare 4.26" 800×480 e-paper panel (SSD1677) into a Home Assistant
dashboard. It is a LEAN, PM2 sleepy Zigbee 3.0 end device built on the
[cc2530-zigbee](https://github.com/faronov/cc2530-zigbee) stack with the
LLVM MCS-51 compiler, and it does not use Z-Stack.

The panel shows the battery, the link RSSI and the parent's LQI, a local
clock, the indoor temperature and humidity from an SHT30/SHT31 sensor, the
outdoor weather pushed by Home Assistant, four labelled
value slots and a message line. Home Assistant writes them through the
manufacturer-specific cluster 0xFC10 on endpoint 1; see
[docs/DISPLAY.md](docs/DISPLAY.md).

The device identity is manufacturer `cc2530-zigbee`, model `LG-ESL29`, so the
ZHA quirk and the Zigbee2MQTT converter in [`tools/ha/`](tools/ha) apply.

**Status:**

- Works on hardware: joins ZHA through a router parent, reaches READY, and
  redraws the dashboard on writes from Home Assistant.
- Partial refresh with a full refresh every 13th time, at boot and after a
  failure; not yet verified on hardware.
- Not yet qualified: sleep current.

## SHT3x sensor

Temperature Measurement 0x0402 and Relative Humidity 0x0405 come from an
SHT30/SHT31 (address 0x44, ADDR low) on the board's TX/RX pads: TX is P1.6
SDA, RX is P1.7 SCL. The stock firmware maps USART1 there (alternative 2);
the pads have not been continuity tested. The bus needs pull-ups: a breakout
board's 4.7–10 kΩ to 3 V, or the weak internal port 1 pull-ups. The sensor is
read every 10 s, each value one period late; without a sensor both read
invalid and the panel shows `--`. The CC2530 die temperature is not used.

## Layout

| Path | Contents |
|---|---|
| `cc2530-zigbee/` | the stack (git submodule) |
| `cc2530-app.json` | [external application](https://github.com/faronov/cc2530-zigbee/blob/main/docs/EXTERNAL_APP.md) manifest: stack options and replaced units |
| `include/app_config.h` | board identity |
| `include/app_zcl.h`, `src/zcl_dashboard.c` | ZCL application server, simple descriptor |
| `include/sht3x.h`, `src/sht3x.c` | SHT3x temperature and humidity on bit-banged I2C |
| `include/display.h`, `src/display.c` | SSD1677 driver and refresh state machine |
| `src/dashboard.c`, `include/display_art.h` | dashboard renderer, cluster 0xFC10, generated glyphs and icons |
| `boards/lg_esl29.c` | early pin parking, panel power |
| `tests/` | host tests: renderer against goldens, ZCL server |
| `tools/epd/` | glyph, icon and golden generator |
| `tools/ha/` | ZHA quirk, Home Assistant package, Zigbee2MQTT converter |

## Build

```sh
git clone --recursive https://github.com/faronov/esl-dashboard-cc2530.git
cd esl-dashboard-cc2530
make test                                   # host tests (cc, ASan/UBSan)
make firmware LLVM_TOOLS=<toolchain>/bin LLVM_SRC=<llvm-project>
```

`make firmware` needs Python 3 with `pyelftools` and the LLVM MCS-51
compiler at the revision pinned in `cc2530-zigbee/tools/llvm/toolchain.json`:
the prebuilt `mcs51-llvm-<version>-<revision>` toolchain from the
[faronov/llvm-project releases](https://github.com/faronov/llvm-project/releases)
(or a build of that revision, see the stack's
[LLVM_DEVELOPMENT.md](https://github.com/faronov/cc2530-zigbee/blob/main/docs/LLVM_DEVELOPMENT.md)).
`LLVM_SRC` is a checkout of the same revision; only `llvm/lib/Target/MCS51`
(runtime, startup, linker script) is used, so a sparse checkout is enough.
CI uses the release and builds LLVM only when the pinned revision has none.
The image, its audit and its sealed receipt are written to `build/firmware/`
(`firmware.hex`).

CI builds the compiler and the firmware on every push and uploads the HEX as an
artifact.

## Flash

```sh
cc-tool -e -w build/firmware/firmware.hex
```

Then permit joining in your coordinator. The device scans all channels and can
join through a router. Install the integration from [`tools/ha/`](tools/ha)
([docs/DISPLAY.md](docs/DISPLAY.md#home-assistant-and-zigbee2mqtt)).

## License

BSD-3-Clause, see [LICENSE](LICENSE). The generated font and icon bitmaps
carry their own notices in [docs/DISPLAY.md](docs/DISPLAY.md).
