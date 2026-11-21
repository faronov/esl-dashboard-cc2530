# L8 dashboard display

The firmware drives a Waveshare 4.26" 800×480 black/white e-paper panel
(SSD1677) wired to the LG ESL29 board's display FPC and shows a Home
Assistant dashboard. Pins, the command sequence and the timing are in
[`include/display.h`](../include/display.h). `make firmware PANEL=579` builds
the same firmware for the Waveshare 5.79" 792×272 panel instead
([L5](#l5-the-579-panel)).

## Driver and schedule

- [`src/display.c`](../src/display.c) is a state machine advanced from every
  READY poll (`zcl_sensor_tick`): power and reset, the boot sequence, the
  planes streamed `EPD_ROWS_PER_STEP` gate rows per poll, refresh (`20`),
  deep sleep (`10 01`), then the panel power is cut
  (`board_early_off`). Every wait is bounded; a timeout or an SPI stall powers
  the panel off and counts a failure.
- The device sleeps in PM2 between polls. While rows stream it stays awake;
  while the panel refreshes on its own (about 3.3 s) the PM2 sleep is bounded
  by the expected rest of the refresh (`zcl_sensor_quiet`).
- The driver runs only while the network is READY. A refresh interrupted by a
  rejoin resumes when READY returns and normally fails on its 30 s limit;
  the next one starts as usual.
- Refreshes use the panel's OTP waveforms. The first refresh after boot or
  after a failure, and every `EPD_PARTIALS + 1`-th (13th) refresh, is full
  (border `3C 01`, `22 F7`); the others are partial (border `3C 80`,
  `22 FF`).
- The panel loses power between refreshes, so a partial refresh streams both
  planes: the previously shown view into `26` (old) and the new one into `24`.
  The driver keeps both views (`epd.old`, `epd.next`, texts included), so the
  old plane is exactly what the panel shows.

## Layout

[`src/dashboard.c`](../src/dashboard.c) draws an 800×480 landscape frame,
rendered row by row (100 octets per row, no frame buffer).

- status bar: battery gauge (0..36 columns for 2.0 .. 3.0 V) and voltage,
  Zigbee icon, the RSSI of the last received frame and the parent's LQI
  of this device (`LQI --` until known), local clock
  (`Пн 21.04 14:32`, `-- --.-- --:--` until a time is written);
- left half `ДОМА`: the device's temperature and humidity from the SHT3x
  (7-segment, `--` without a sensor);
- right half `УЛИЦА`: outdoor temperature and humidity and a 96×96 weather
  icon (a Material Design Icons weather icon, 48×48 drawn twice the size),
  all pushed by Home Assistant;
- temperatures and humidities use fixed-width digit cells: three for a
  temperature (`21.4`, `-3.2`, and `5.3` behind a blank cell; at -10 °C and
  below `-12` without a point) and two for a humidity (a blank cell in front
  of a single digit), so the point and the unit stay in place;
- four slots: a label (scale 1) and a value (7-segment for digits, `-`,
  `.`, `,` and `:`, glyphs otherwise; a narrow `1`, so long values fit);
- a message line (scale 2).

Text uses the t0-16b-uni 8×16 font (ASCII, Cyrillic, `Ёё` and `°`); other
characters show `?`. [`tools/epd/l8_art.py`](../tools/epd/l8_art.py) is the
reference design and writes [`include/display_art.h`](../include/display_art.h)
and [`tests/display_golden.h`](../tests/display_golden.h);
`tests/test_display.c` compares the C renderer with the goldens:

```sh
make art FONTS=<u8g2>/tools/font/bdf   # or: python3 tools/epd/l8_art.py --fonts ... [--check] [--png out.png]
```

The weather icons are a table in `l8_art.py`;
[`tools/epd/l8_icons.py`](../tools/epd/l8_icons.py) documents and repeats
it from the pinned `@mdi/svg` SVGs (needs cairosvg and Pillow):

```sh
python3 tools/epd/l8_icons.py [--svg DIR]
```

## L5: the 5.79" panel

With `EPD_PANEL_579` (`make firmware PANEL=579`: `tools/stage_panel.py`
stages the tracked tree in `build/app-579` with a manifest that adds the
define and leaves out `CC2530_APP_MGMT_LQI`) the firmware drives the
Waveshare 5.79" 792×272 panel: two cascaded SSD1683, the master for the left
half and the slave (commands `| 0x80`) for the right one, with the same pins.

- Every refresh streams four passes of 272 rows × 50 octets: the previous
  plane (`26`, `A6`; all zero for a full refresh) and the new one (`24`, `A4`),
  master then slave; the halves share octet 49 of the 99-octet row.
- A load-temperature update hangs the slave, so the driver writes the SHT3x
  temperature (`1A`, `9A`; 20 °C when unknown) and refreshes with `22 D7`
  (full, about 3.4 s) or `3C 80`, `BC 80`, `22 DF` (partial, about 0.45 s).
  Partial refreshes leave light ghosts that the full one every 13th time
  clears.
- [`src/dashboard_l5.c`](../src/dashboard_l5.c) renders the L5 layout row by
  row and runs in bank 6 with its tables: a black header (battery, voltage,
  clock, signal bars and `-63 dBm` in white), `ДОМА` and `УЛИЦА` with
  7-segment temperatures and humidities (the D3 digits: tapered bars,
  pointed verticals, the `1` in the middle of its cell), a 48×48 weather
  icon, four slots and the message. Text is proportional DejaVu Sans,
  22 px (header, units, message) and 13 px bold (labels), rasterised once
  without anti-aliasing. The store, the cluster and the views are those of
  L8; the header shows no LQI.

[`tools/epd/l5_art.py`](../tools/epd/l5_art.py) is the L5 reference and writes
[`include/display_art_l5.h`](../include/display_art_l5.h) and
[`tests/display_golden_l5.h`](../tests/display_golden_l5.h);
`tests/test_display_579.c` checks the renderer and the two-controller
sequence:

```sh
make art FONTS=<u8g2>/tools/font/bdf DEJAVU=/usr/share/fonts/truetype/dejavu
python3 tools/epd/l5_art.py --dejavu DIR [--check] [--png out.png]
```

The generated header records the SHA-256 of the font files it was made from
(DejaVu 2.37).

## Cluster 0xFC10

A cluster in the manufacturer-specific range on the application endpoint
(server). Frames with or without a manufacturer code are accepted. All
attributes are readable; all but ClusterRevision are writable.

| Id | Type | Attribute | Range / unknown |
|---|---|---|---|
| 0x0000 | uint32 | local time, seconds since 2000-01-01 00:00 | 0 unknown |
| 0x0001 | uint16 | refresh interval, s | 60..28800, default 300 |
| 0x0010 | int16 | outdoor temperature, 0.01 °C | 0x8000 unknown |
| 0x0011 | uint16 | outdoor humidity, 0.01 %RH | ≤ 10000, 0xFFFF unknown |
| 0x0012 | enum8 | weather: 0 none, 1 sunny, 2 clear night, 3 partly cloudy, 4 cloudy, 5 rainy, 6 snowy, 7 fog, 8 lightning, 9 windy | 0..9 |
| 0x0020 + 2k | charStr | slot k label (k 0..3) | 22 glyphs |
| 0x0021 + 2k | charStr | slot k value | 12 glyphs |
| 0x0030 | charStr | message | 48 glyphs |
| 0xFFFD | uint16 | ClusterRevision 1 | read-only (0x88) |

Out-of-range values return 0x87. Strings are UTF-8 and cut to the field;
reads return the stored text as UTF-8.

The clock counts on from the last time write. A write that changes what is
shown (a time write only when the time was unknown) redraws the panel once
the last write is 10 s old and the last refresh started at least 30 s
before, so a batch of writes gives one refresh. Otherwise the panel
refreshes every interval when the view changed (indoor values, battery,
RSSI, clock). Refreshes are partial, with a periodic full one (see "Driver and schedule").

## Home Assistant and Zigbee2MQTT

[`tools/ha/`](../tools/ha) holds the integrations for the device
`cc2530-zigbee` / `LG-ESL29`. After flashing, re-pair (or re-interview) the
device so the coordinator sees cluster 0xFC10.

- ZHA: copy [`zha_quirk_lg_esl29_l8.py`](../tools/ha/zha_quirk_lg_esl29_l8.py)
  to the custom quirks directory (`zha: custom_quirks_path:`, for example
  `/config/custom_zha_quirks/`) and reload ZHA. It adds the cluster as
  `l8_display` and a refresh interval number entity.
- HA package: copy [`lg_esl29_l8.yaml`](../tools/ha/lg_esl29_l8.yaml) to
  `/config/packages/` (`homeassistant: packages: !include_dir_named packages`)
  and edit the IEEE address, weather entity and slots in `script.l8_push`.
  Its automation writes the clock, outdoor weather, slot labels, values and
  interval at start and every 5 minutes (the device keeps them in RAM only,
  so a reset shows dashes until the next push);
  `script.l8_message` writes the message line.
- Zigbee2MQTT: copy [`lg_esl29_l8.mjs`](../tools/ha/lg_esl29_l8.mjs) to
  `external_converters/` and restart Z2M. It exposes every attribute as a
  settable entity, writes the clock on device announce and adds a
  `sync_time` action.

The device is a sleepy end device: writes are delivered on its next poll.

## Font notice

The text uses t0-16b-uni of the u8g2 collection:

```
Copyright (C) 2012-2025 Uwe Waldmann

Permission is hereby granted, free of charge, to any person obtaining
a copy of this font software and associated files (the "Software"),
to deal in the Software without restriction, including without
limitation the rights to use, copy, modify, merge, publish,
distribute, embed, sublicense, and/or sell copies of the Software,
and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

(1) The above copyright notice, this permission notice, and the
    disclaimer below shall be included in all copies or substantial
    portions of the Software.

(2) If the design of any glyphs in the fonts that are contained in the
    Software or generated during the installation process is modified
    or if additional glyphs are added to the fonts, the fonts
    must be renamed. The new names may not contain the word "UW",
    irrespective of capitalisation; the new names may contain the word
    "ttyp0", irrespective of capitalisation, only if preceded by a
    foundry name different from "UW".

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

The L5 text uses DejaVu Sans and DejaVu Sans Bold, rasterised to bitmaps:

```
Copyright (c) 2003 by Bitstream, Inc. All Rights Reserved. Bitstream Vera
is a trademark of Bitstream, Inc. DejaVu changes are in public domain.

Permission is hereby granted, free of charge, to any person obtaining a copy
of the fonts accompanying this license ("Fonts") and associated
documentation files (the "Font Software"), to reproduce and distribute the
Font Software, including without limitation the rights to use, copy, merge,
publish, distribute, and/or sell copies of the Font Software, and to permit
persons to whom the Font Software is furnished to do so, subject to the
following conditions:

The above copyright and trademark notices and this permission notice shall
be included in all copies of one or more of the Font Software typefaces.

The Font Software may be modified, altered, or added to, and in particular
the designs of glyphs or characters in the Fonts may be modified and
additional glyphs or characters may be added to the Fonts, only if the fonts
are renamed to names not containing either the words "Bitstream" or the word
"Vera".

This License becomes null and void to the extent applicable to Fonts or Font
Software that has been modified and is distributed under the "Bitstream
Vera" names.

The Font Software may be sold as part of a larger software package but no
copy of one or more of the Font Software typefaces may be sold by itself.

THE FONT SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO ANY WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF COPYRIGHT, PATENT,
TRADEMARK, OR OTHER RIGHT. IN NO EVENT SHALL BITSTREAM OR THE GNOME
FOUNDATION BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, INCLUDING
ANY GENERAL, SPECIAL, INDIRECT, INCIDENTAL, OR CONSEQUENTIAL DAMAGES,
WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
THE USE OR INABILITY TO USE THE FONT SOFTWARE OR FROM OTHER DEALINGS IN THE
FONT SOFTWARE.

Except as contained in this notice, the names of Gnome, the Gnome
Foundation, and Bitstream Inc., shall not be used in advertising or
otherwise to promote the sale, use or other dealings in this Font Software
without prior written authorization from the Gnome Foundation or Bitstream
Inc., respectively. For further information, contact: fonts at gnome dot
org.
```

## Icon notice

The L8 and L5 weather icons are rasterised from Material Design Icons
(`@mdi/svg` 7.4.47) by [Pictogrammers](https://pictogrammers.com/),
licensed under the Apache License, Version 2.0
(<https://www.apache.org/licenses/LICENSE-2.0>). They are converted to
48×48 bitmaps (modified). The other icons are drawn by the generator.
