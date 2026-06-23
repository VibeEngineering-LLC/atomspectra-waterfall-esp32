# Waterfall PC tools / Инструменты водопада для ПК

PC-side helpers for the AtomSpectra waterfall (spectrogram). See [`../WATERFALL.md`](../WATERFALL.md)
([🇬🇧 EN](../WATERFALL.en.md)) for formats (ASWW / ASWF / N42), the Web API and calibration details.

Вспомогательные инструменты для водопада AtomSpectra. Форматы, Web API и калибровка —
[`../WATERFALL.md`](../WATERFALL.md).

| File | Purpose / Назначение |
|---|---|
| `waterfall_n42.py` | Waterfall → **ANSI N42.42** (`.n42`) for InterSpec / PeakEasy / Cambio |
| `waterfall_viewer.html` | Offline 2D `.n42` heatmap viewer (open in a browser, no server) |
| `waterfall_client.py` | Capture the live WS stream into an unbounded `.aswf` file |

```
pip install requests websocket-client    # waterfall_n42.py / waterfall_client.py
```

## waterfall_n42.py — export to ANSI N42.42

Three modes. Calibration (5-coefficient energy polynomial) is taken from the WS header
or the `.aswf` JSON header — **not** from `/api/spectrum.json` (those coefficients are zero).

```bash
# 1) window — pull the current on-board ring (ASWW) and write one .n42
python waterfall_n42.py window <board-ip> -o snapshot.n42

# 2) stream — record the live WS feed until Ctrl+C (or --seconds N), then write .n42
python waterfall_n42.py stream <board-ip> --seconds 360 -o live_soak.n42

# 3) convert — turn a previously captured .aswf into .n42
python waterfall_n42.py convert capture.aswf -o capture.n42 --host <board-ip>
```

Options: `--detector` (default `CsI`), `--seconds` (stream auto-stop), `--host`
(calibration source for `convert` when the `.aswf` header has none).

## waterfall_viewer.html — offline viewer

Open the file directly in a browser (double-click) and drag a `.n42` onto it, or use
**Открыть .n42**. Renders the waterfall as a viridis heatmap; hover shows energy in keV
(when the file carries a calibration) and the bin counts. No network, no server.

## waterfall_client.py — capture to .aswf

Streams `/ws/waterfall` and writes an unbounded `.aswf` (not limited by the on-board
PSRAM ring). Stop with Ctrl+C — the header with the final row count is written on exit.

```bash
python waterfall_client.py <board-ip> -o capture.aswf
```

> `.n42` and `.aswf` captures are git-ignored — they are generated artifacts, not source.
