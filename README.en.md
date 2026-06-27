# AtomSpectra → ESP32-S3 → Web UI

[🇷🇺 Русская версия](README.md) · **🇬🇧 English**

A WiFi gateway for the **KB Radar "Atom Spectra"** gamma spectrometer, built on the ESP32-S3 with USB OTG Host.

It connects to the spectrometer over **USB** (not BLE!), receives the 8192-channel spectrum
in real time, and displays it in your browser — with axes, a logarithmic scale,
energy calibration in keV, and export to the **BecqMoni** and **InterSpec** formats.

![ESP32-S3](https://img.shields.io/badge/ESP32--S3-N16R8-blue) ![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.4-green) ![USB](https://img.shields.io/badge/USB-OTG%20Host-orange) ![Channels](https://img.shields.io/badge/Spectrum-8192%20ch-purple)

## What it solves

The desktop **AtomSpectra** application is excellent, but it requires the spectrometer to be
plugged directly into a computer over USB. This gateway turns an ESP32-S3 into a **WiFi bridge**:

- the spectrometer connects to the ESP over USB — a tiny board sits right next to the instrument;
- the spectrum is available **in any browser** over WiFi — no cables to a PC;
- **BecqMoni XML** downloads in one click — opens directly in [BecqMoni](https://github.com/Am6er/BecqMoni);
- **InterSpec CSV** — for [InterSpec](https://sandialabs.github.io/InterSpec/) by Sandia;
- a **TCP bridge** (port 8234) — BecqMoni / AtomSpectra on a PC can connect over WiFi instead of a COM port;
- spectra are **stored on flash** (up to ~400) — accumulate them and export later.

No cloud. No accounts. Everything runs on your local network.

## Architecture

```
                          USB-C OTG cable
┌──────────────────┐    (host → device)    ┌──────────────────┐
│  KB Radar        │ ◄──────────────────── │  ESP32-S3        │
│  Atom Spectra    │   shproto @ 600 kBd   │  (USB OTG Host)  │
│  (gamma          │   8192 ch × 32 bit    │                  │
│   spectrometer,  │   + stat + calibr.    │  WiFi 2.4 GHz    │
│   FTDI FT232R)   │                       │  ┌────────────┐  │
└──────────────────┘                       │  │ Web UI     │  │──► Browser
                                           │  │ REST API   │  │──► BecqMoni (TCP:8234)
                                           │  │ LittleFS   │  │──► InterSpec (CSV)
                                           │  │ 12.9 MB    │  │
                                           │  └────────────┘  │
                                           └──────────────────┘
```

## What you see in the Web UI

![Board Web UI — "Waterfall" tab: spectrogram (time ↓, energy →) with a spectrum slice](images/web-ui-waterfall.png)

The Web UI opens in a browser at `http://<board-IP>/`:

**Spectrum (canvas 1200×400)**
- Live 8192-channel spectrum, refreshed once per second
- **Axes** with grid, tick labels, and a frame
- **Log / Lin** — toggle logarithmic / linear Y scale
- **CPS / Counts** — instantaneous count rate or accumulated pulses
- **Ch / keV** — channels or energy (requires calibration from the instrument)
- **Cursor** — hover over the spectrum → channel, energy, counts, CPS

**Instrument control**
- ▶ Start / ■ Stop / ↻ Reset — start/stop/reset acquisition on the instrument
- Arbitrary text command (`-inf`, `-nos 5`, etc.)
- Reboot the instrument (CMD 0xF3) and reset WiFi

**Big display**
- Acquisition time (hours:minutes:seconds)
- CPS (counts per second)
- Total counts

**Spectra**
- **Save** the current spectrum to flash
- **Load** a saved one — overlaid on the live spectrum for comparison (overlay)
- **Export XML** — download a BecqMoni-compatible file
- **Export CSV** — download an InterSpec-compatible file
- Delete saved spectra

## Export

### BecqMoni XML (`/api/export.xml`)

The full 8192-channel spectrum in the `ResultDataFile` format (FormatVersion 120920):
- `EnergyCalibration` — polynomial coefficients from the instrument
- `ValidPulseCount` / `TotalPulseCount` / `MeasurementTime` / `LiveTime`
- 8192 `<DataPoint>` elements
- Compatible with BecqMoni: File → Open → select the downloaded `.xml`

### InterSpec CSV (`/api/export.csv`)

Headers with calibration coefficients, serial number, and timing:
- `calibcoeff` — calibration polynomial
- `livetime` / `realtime` — time adjusted for CPU load
- 8192 lines of `channel, count` (1-based)
- Compatible with InterSpec: File → Open → select the `.csv`

## What you need

**Hardware:**
- **ESP32-S3-DevKitC-1 N16R8** (16 MB Flash, 8 MB PSRAM) — it must be the S3 with USB OTG ([buy on Ozon](https://ozon.ru/t/BYG7CO2))
- **USB-C OTG cable** — from the ESP32-S3 (host) to the spectrometer (device)
- A **KB Radar "Atom Spectra"** spectrometer (with a USB port, FTDI FT232R inside)
- A USB cable for flashing the ESP (via the UART port, not OTG)

**Software:**
- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/) **v5.4** (the tested version; builds in CI) **or** Docker (`espressif/idf:v5.4`)
- CH343 driver (if the board uses a CH343 USB-UART: [WCH driver](https://www.wch-ic.com/downloads/CH343SER_ZIP.html))

> Detailed install from scratch — see [`INSTALL.en.md`](INSTALL.en.md).
> Known issues and limitations — see [`KNOWN_ISSUES.en.md`](KNOWN_ISSUES.en.md).

## Quick start (5 minutes)

```bash
# 1. Clone
git clone https://github.com/VibeEngineering-LLC/atomspectra-esp32.git
cd atomspectra-esp32

# 2. Build (option A: local ESP-IDF)
idf.py set-target esp32s3
idf.py build

# 2. Build (option B: Docker, no ESP-IDF install)
docker run --rm -v "$(pwd):/project" -w /project espressif/idf:v5.4 \
  bash -c ". /opt/esp/idf/export.sh && idf.py build"

# 3. Flash (substitute your own COM port)
idf.py -p COM14 flash

# 4. Connect to WiFi
#    The board raises an AP "AtomSpectra-Setup" → captive portal → enter SSID and password

# 5. Open in a browser
#    http://<board-IP>/

# 6. Connect the spectrometer with a USB-C OTG cable to the ESP32-S3 USB port
#    The spectrum appears automatically
```

## Web API

| Endpoint | Method | What it does |
|---|---|---|
| `/` | GET | Web UI |
| `/api/csrf-token` | GET | Issue a CSRF token (required in the `X-CSRF-Token` header on all POSTs) |
| `/api/status` | GET | Device status (JSON) |
| `/api/spectrum.json` | GET | Live spectrum + statistics + calibration |
| `/api/spectrum` | GET | Raw binary spectrum (32768 bytes) |
| `/api/export.xml` | GET | BecqMoni XML (live spectrum) |
| `/api/export.csv` | GET | InterSpec CSV (live spectrum) |
| `/api/command` | POST | Send a text command to the instrument |
| `/api/reset` | POST | Reset the spectrum counters |
| `/api/save` | POST | Save the spectrum to flash |
| `/api/list` | GET | List saved spectra (JSON) |
| `/api/saved/<N>/export.xml` | GET | Export a saved spectrum (XML) |
| `/api/saved/<N>/export.csv` | GET | Export a saved spectrum (CSV) |
| `/api/saved/<N>/spectrum.json` | GET | Saved spectrum (JSON) |
| `/api/saved/<N>/delete` | POST | Delete a saved spectrum |
| `/api/device` | GET | Instrument info (settings, calibration, serial) |
| `/api/system` | GET | ESP32 health (heap, uptime, RSSI, flash) |
| `/api/calibration` | POST | Set calibration coefficients manually |
| `/api/reboot-device` | POST | Reboot the spectrometer (CMD 0xF3) |
| `/api/reboot-esp` | POST | Reboot the ESP32 |
| `/api/wifi/reset` | POST | Reset WiFi, reboot into setup mode |
| `/waterfall` · `/api/waterfall/*` · `/ws/waterfall` | GET/POST/WS | **Waterfall** (spectrogram): record, snapshot, stream — see [`WATERFALL.en.md`](WATERFALL.en.md) |

> **All POST endpoints require the `X-CSRF-Token` header** with the value obtained
> from `GET /api/csrf-token`. The Web UI does this automatically. The CSRF token is generated
> at board startup and protects against request forgery from a third-party page in the browser.

## Security and trust model

The gateway is designed for a **trusted local network** (home Wi-Fi) and has **no user
authentication** — anyone on the same network can open the Web UI, read the spectrum,
and control the instrument. This is a deliberate choice for a home device with no cloud
and no accounts; do not expose the board directly to the internet.

What is protected:
- A **CSRF token** on all mutating POSTs (`/api/command`, `/api/reset`, `/api/save`,
  `/api/reboot-*`, `/api/wifi/reset`, `/api/calibration`, spectrum deletion). A third-party
  tab in the operator's browser cannot read the token (same-origin policy), so it cannot
  blindly send, for example, a Wi-Fi reset or a reboot.
- The **TCP bridge** (port 8234) — one client at a time.

What is absent (by design): TLS, login/password, access control. If you need external
access, set it up over a trusted channel (VPN / reverse proxy with authentication),
not via port forwarding.

## TCP bridge (port 8234)

A transparent serial-over-WiFi bridge. BecqMoni or AtomSpectra on a PC connect to
`<board-IP>:8234` instead of a COM port — and work as usual.

- One client at a time
- The Web UI works in parallel with the TCP bridge
- `TCP_NODELAY` for minimal latency

## Waterfall (spectrogram)

![Offline viewer waterfall_viewer.html — waterfall heatmap from a .n42 file](images/waterfall-viewer.png)

Besides the live spectrum, the gateway can accumulate a **waterfall** — a sequence of
spectra at equal intervals (5…60 s; each row = the accumulation delta over one period, 8192
channels, `uint16`). The waterfall can be viewed in the browser
(`http://<board-IP>/waterfall`), **streamed to a PC** over WebSocket in real time, and
**exported with the "⬇ Export .n42" button** right from the Web UI to the
industry-standard **ANSI N42.42** (InterSpec / PeakEasy / Cambio). The spectrogram
colour is any of **14 palettes** (Inferno by default; the choice is saved in the
browser).

> 🔴 **Live demo on real data:**
> **<https://vibeengineering-llc.github.io/atomspectra-waterfall-esp32/demo/>**
> — the very same spectrogram from the firmware's Web UI, rendered from a real `.aswf`
> capture of an "Atom Spectra" instrument (480 rows × 8192 channels, real calibration).
> No board required: hover the image and a per-row spectrum slice with a keV energy
> axis appears below.

`scripts/` holds the PC-side tools: N42 export (`waterfall_n42.py`), an offline 2D
waterfall viewer (`waterfall_viewer.html`), and `.aswf` capture (`waterfall_client.py`).

📖 Formats (ASWW / ASWF / N42), the full waterfall Web API, calibration, and how to use
the scripts — [`WATERFALL.en.md`](WATERFALL.en.md).

## Protocol

Atom Spectra communicates over the binary **shproto** protocol via USB serial (600000 baud):

| Parameter | Value |
|---|---|
| Start byte | `0xFE` |
| Escape byte | `0xFD` (next byte = `~byte & 0xFF`) |
| Finish byte | `0xA5` |
| CRC | CRC-16 Modbus (init `0xFFFF`, poly `0xA001`) |
| Commands | `0x01` histogram, `0x03` text, `0x04` statistics, `0xF3` reboot |

**Calibration**: the instrument returns 5 polynomial coefficients in response to the `-inf`
command (10 lines of hex-encoded doubles + CRC32). Polynomial: `E(ch) = c₀ + c₁·ch + c₂·ch² + c₃·ch³ + c₄·ch⁴`.

📖 Full reference of all device commands and packet formats — [`PROTOCOL.en.md`](PROTOCOL.en.md).

## Project layout

```
atomspectra-esp32/
├── components/shproto/       shproto protocol (CRC-16 Modbus, escaping)
│   ├── shproto.c
│   └── include/shproto.h
├── main/
│   ├── atomspectra.h          project header, data types
│   ├── main.c                 entry point, SNTP
│   ├── usb_host_cdc.c         USB Host CDC-ACM + FTDI vendor init
│   ├── wifi_manager.c         STA + AP captive portal
│   ├── web_server.c           HTTP API + BecqMoni XML + InterSpec CSV
│   ├── tcp_bridge.c           transparent serial-over-WiFi bridge
│   ├── spectrum.c             spectrum processing + LittleFS storage
│   ├── spectrogram.c          waterfall: PSRAM ring + flash recording
│   └── web_waterfall.c        waterfall: HTTP/WS API (ASWW/ASWF)
├── web/
│   ├── index.html             main Web UI (spectrum, buttons, export)
│   ├── waterfall.html         waterfall Web UI (heatmap)
│   └── setup.html             captive portal (WiFi setup)
├── scripts/                   PC tools: N42 export, viewer, capture
│   ├── waterfall_n42.py       waterfall → ANSI N42.42
│   ├── waterfall_viewer.html  offline 2D .n42 viewer
│   └── waterfall_client.py    capture WS stream to .aswf
├── partitions.csv             partition table (3 MB app + 12.9 MB LittleFS)
├── sdkconfig.defaults         ESP32-S3 USB OTG config
├── CMakeLists.txt
├── INSTALL.en.md              detailed install guide
├── KNOWN_ISSUES.en.md         known issues and limitations
├── LICENSE                    MIT
└── README.en.md               this file
```

## License

MIT — see [`LICENSE`](LICENSE).

## Credits

- **KB Radar** ([kbradar.org](https://kbradar.org/)) — maker of the Atom Spectra spectrometer.
- **Am6er/BecqMoni** ([github](https://github.com/Am6er/BecqMoni)) — reference UI implementation for AtomSpectra, the XML format.
- **InterSpec** ([Sandia Labs](https://sandialabs.github.io/InterSpec/)) — gamma spectrum analysis.
- **Espressif** — ESP-IDF and the USB Host stack.