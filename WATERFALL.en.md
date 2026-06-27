# Waterfall (spectrogram) — accumulate, stream to PC, export

[🇷🇺 Русская версия](WATERFALL.md) · [← README](README.en.md)

The gateway can accumulate a **waterfall** (spectrogram): a sequence of spectra
taken at fixed intervals. Each waterfall row is a **delta** of the cumulative
spectrum over one interval — i.e. a finished "counts-per-period" spectrum,
8192 channels, `uint16` per channel (**16 KB per row**).

You can:

- view it right in the board's browser — `http://<board-ip>/waterfall`;
- **stream it to a PC live** over WebSocket;
- **export it with the "⬇ Export .n42" button** right from the Web UI — the board
  builds **ANSI N42.42** from the PSRAM ring (works even without flash persistence);
- convert to **ANSI N42.42** with the scripts shipped in this repo;
- open it as a 2D waterfall in the offline viewer shipped in this repo.

## What it looks like

Built-in waterfall Web UI (`http://<board-ip>/waterfall`): the spectrogram on top
(time flows downward, X axis is channels/energy), with a slice of the current
spectrum under the hovered row. Colour encodes intensity per the chosen palette and
scale (log/lin).

![Web UI — "Waterfall" tab (spectrogram + spectrum slice)](images/web-ui-waterfall.png)

Buttons **Start / Stop / Clear / Export .n42**, an **Interval** field, a
**keep in Flash** checkbox, X-axis selector (channels/keV), brightness (log/lin),
palette, contrast slider and channel zoom. The counter shows total rows · in ring
(`149/256`) · in Flash.

### Spectrogram palettes

Row colour is set by the chosen **palette** (the selector button above the
spectrogram). **14** palettes are available; the choice is saved in the browser
(`localStorage`, key `aswf-pal`) and applied to the whole waterfall on the fly (the
palette is expanded into a 256-level LUT). Default is **Inferno**.

| Palette | Type | Note |
|---|---|---|
| **Inferno** | perceptual, warm | default, high contrast on dark background |
| **Magma** | perceptual, warm | softer than Inferno, purple-pink |
| **Plasma** | perceptual, warm | purple→yellow, no black |
| **Viridis** | perceptual, cool | colour-blind friendly |
| **Cividis** | perceptual, cool | optimised for colour-blindness |
| **Parula** | perceptual | MATLAB default palette |
| **Cubehelix** | monotone luminance | survives black-and-white printing |
| **Turbo** | rainbow | Jet replacement with even luminance |
| **Jet** | rainbow | MATLAB classic |
| **Spectral** | diverging | blue↔yellow↔red |
| **Hot** | thermal | black→red→yellow→white |
| **Ocean** | cool | black→blue→white, calm |
| **Cool** | vivid | cyan→magenta |
| **Grayscale** | mono | for printing |

> For quantitative reading prefer the perceptually-uniform palettes
> (Inferno/Magma/Plasma/Viridis/Cividis/Parula/Cubehelix/Turbo) — they don't create
> false intensity "bands". Jet and the other rainbow maps are for eye-candy only.

## How it works

| Parameter | Value | Where in code |
|---|---|---|
| Channels | 8192 (`WF_CHANNELS`) | `main/spectrogram.h` |
| Row size | 16 KB (`WF_ROW_BYTES`) | `main/spectrogram.h` |
| PSRAM ring | 256 rows × 16 KB = **4 MB** (`WF_RING_ROWS_DEFAULT`) | `main/spectrogram.h` |
| Default interval | 5 s (`WF_INTERVAL_DEFAULT`), range **5…60** (`WF_INTERVAL_MIN`/`WF_INTERVAL_MAX`) | `main/spectrogram.h` |
| Data type | `uint16` little-endian | `main/web_waterfall.c` |

While `recording`, rows accumulate into a **PSRAM ring buffer** (the latest 256 rows
are always available for the window/stream). If `persist` is on, rows are also written
to **flash** (LittleFS) for later file export. When flash runs out of space `flash_full`
becomes `true`, flash writing stops, while the ring and the WS stream keep running.

> **Calibration.** The instrument's real energy calibration is a 5-coefficient
> polynomial (`E(ch) = c₀ + c₁·ch + c₂·ch² + c₃·ch³ + c₄·ch⁴`). It is delivered in the
> **WebSocket text header** (`/ws/waterfall`) and in the `.aswf` file header — **not**
> in the `t1/t2/t3` fields of `/api/spectrum.json`. The tools in `scripts/` read the
> calibration from the WS header, so the axis comes out in **keV**.

## Waterfall Web API

| Endpoint | Method | Purpose |
|---|---|---|
| `/waterfall` | GET | Built-in waterfall Web UI (heatmap on the board itself) |
| `/api/waterfall/status` | GET | Waterfall status (JSON, see below) |
| `/api/waterfall/start` | POST | Start recording |
| `/api/waterfall/stop` | POST | Stop recording |
| `/api/waterfall/clear` | POST | Clear ring + flash (only while stopped) |
| `/api/waterfall/config` | POST | `{"interval":N,"persist":bool}` — interval (s) and flash persistence |
| `/api/waterfall/window` | GET | Ring snapshot (**ASWW** binary, up to 256 rows) |
| `/api/waterfall/export.n42` | GET | **Export to ANSI N42.42** from the PSRAM ring (one `<RadMeasurement>` per row, `CountedZeroes`, calibration in `<EnergyCalibration>`). The "⬇ Export .n42" button in the Web UI. Does not require flash persistence |
| `/ws/waterfall` | WS | Text header on connect, then one binary frame (16384 B) per new row |

> All POST endpoints require the **`X-CSRF-Token`** header (from `GET /api/csrf-token`),
> same as the rest of the gateway API.

`GET /api/waterfall/status` → JSON:

```json
{
  "recording": false, "persist": false, "flash_full": false, "ready": true,
  "interval_sec": 5, "ring_capacity": 256, "ring_count": 0,
  "total_rows": 0, "flash_rows": 0, "started_at": 0, "channels": 8192
}
```

- `ready` — PSRAM ring allocated;
- `ring_count` — valid rows in the ring (≤ `ring_capacity`);
- `total_rows` — rows recorded since `start` (monotonic);
- `flash_rows` — rows in the flash file.

## File formats

### ASWW — window snapshot (`/api/waterfall/window`)

Compact header-less binary, streamed out (single 16 KB bounce buffer, no second
4 MB buffer → no OOM):

```
"ASWW" (4 bytes)
channels     u32 LE   (= 8192)
rows         u32 LE   (rows in the window)
first_index  u32 LE   (total index of the first row)
interval     u32 LE   (seconds between rows)
payload      = rows × channels × uint16 LE   (chronological, oldest first)
```

### ASWF — self-describing file (`scripts/waterfall_client.py`)

Binary with a JSON header — everything needed to interpret it standalone. The `.aswf`
file is written only by the PC script `waterfall_client.py` from the WS stream
(`/ws/waterfall`); there is no on-board dump endpoint:

```
"ASWF" (4 bytes)
header_len   u32 LE
header       = JSON (utf-8), header_len bytes
payload      = rows × channels × uint16 LE
```

JSON header:

```json
{
  "format": "atomspectra-waterfall", "version": 1,
  "channels": 8192, "dtype": "uint16", "byte_order": "little",
  "rows": 1234, "interval_sec": 5, "started_at": 1750000000,
  "serial": "...", "calibration": [c0, c1, c2, c3, c4]
}
```

`serial` and `calibration` are present only if the instrument reported them.

### WebSocket header (`/ws/waterfall`)

The first frame after connect is a **text** JSON:

```json
{ "type": "header", "channels": 8192, "interval_sec": 5, "total_rows": 187,
  "serial": "...", "calibration": [c0, c1, c2, c3, c4] }
```

Then one **binary** frame per new row (16384 bytes = 8192 × uint16 LE). Up to
4 WS clients are supported at once.

## Streaming to a PC and N42 export

`scripts/` ships three tools (require `pip install requests websocket-client`):

### `waterfall_n42.py` — export to ANSI N42.42-2012

ANSI N42.42 (IEC 62755) is the XML gamma-spectrometry interchange format understood by
InterSpec, PeakEasy, Cambio. Each waterfall row becomes one `<RadMeasurement>` with
`RealTimeDuration = PT{interval}S`; sparse delta rows are compressed with `CountedZeroes`.
The calibration is written to `<EnergyCalibration>` (polynomial from the WS header).

```bash
# board ring snapshot → snapshot.n42
python waterfall_n42.py window  <board-ip> -o snapshot.n42

# live stream to N42, auto-stop after 360 s (recording must be ON on the board)
python waterfall_n42.py stream  <board-ip> -o live.n42 --seconds 360

# convert a previously captured .aswf (pull calibration from the board via --host)
python waterfall_n42.py convert capture.aswf -o capture.n42 --host <board-ip>
```

Options: `--detector CsI|NaI|LaBr3|...` (default `CsI`), `-o/--out`.

### `waterfall_viewer.html` — offline waterfall viewer

A standalone HTML page (no server, no dependencies): open it in a browser and
**drag a `.n42` file** onto it — it renders a heatmap (time ↓ × energy →, viridis
palette). Controls: channel range, detail, log/contrast, hover tooltip with energy in
keV (when calibration is present). You can also pass a file via `?src=name.n42` when
serving over `http://`.

![Offline viewer waterfall_viewer.html — waterfall heatmap from .n42](images/waterfall-viewer.png)

A ready-made sample [`example-waterfall.n42`](scripts/example-waterfall.n42) (export of
a real run) ships with the repo — drag it into the viewer to see the result right away
without connecting to a board.

### `waterfall_client.py` — capture to `.aswf`

Writes an unbounded `.aswf` from the WS stream (not limited by the board's 256-row
ring). Stop with Ctrl+C — the header with the final row count is written on exit.
The `.aswf` can then be converted to N42 via `waterfall_n42.py convert`.

## What opens N42

| Tool | By | Note |
|---|---|---|
| [InterSpec](https://sandialabs.github.io/InterSpec/) | Sandia | Best choice; shows the measurement time-history |
| `waterfall_viewer.html` | this repo | 2D waterfall (heatmap), offline |
| PeakEasy | LANL | Spectrum viewer |
| Cambio | Sandia | Converter/viewer |