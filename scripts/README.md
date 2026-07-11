# Waterfall PC tools / Инструменты водопада для ПК

PC-side helpers for the AtomSpectra waterfall (spectrogram). See [`../WATERFALL.md`](../WATERFALL.md)
([🇬🇧 EN](../WATERFALL.en.md)) for formats (ASWW / ASWF / N42), the Web API and calibration details.

Вспомогательные инструменты для водопада AtomSpectra. Форматы, Web API и калибровка —
[`../WATERFALL.md`](../WATERFALL.md).

| File | Purpose / Назначение |
|---|---|
| `waterfall_n42.py` | Waterfall → **ANSI N42.42** (`.n42`) for InterSpec / PeakEasy / Cambio |
| `waterfall_viewer.html` | Offline 2D `.n42`/`.aswf` heatmap viewer (open in a browser, no server) |
| `waterfall_client.py` | Capture the live WS stream into an unbounded `.aswf` file |
| `wf_pull_client.py` | CLI: pull finalized segments from the board and stitch them into one `.aswf` on the fly (idempotent, resumable) |
| `wf_recorder` (GUI-рекордер) | **Перенесён** → [VibeEngineering-LLC/wf-recorder](https://github.com/VibeEngineering-LLC/wf-recorder) (#REC-12/14) |
| `example-waterfall.n42` | Sample export of a real run — drag it into the viewer to try it / образец реального прогона |

> **Продвинутый вьюер / advanced viewer.** For a full native desktop viewer (3D waterfall
> render, 2D map, a slice/section/sample panel) see
> **[VibeEngineering-LLC/waterfall-viewer](https://github.com/VibeEngineering-LLC/waterfall-viewer)**
> — a separate standalone application, more capable than the single-file HTML viewer here.

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

A ready-made [`example-waterfall.n42`](example-waterfall.n42) ships next to the viewer —
drag it in to see the result immediately, no board required.

![waterfall_viewer.html — offline heatmap of example-waterfall.n42](../images/waterfall-viewer.png)

## waterfall_client.py — capture to .aswf

Streams `/ws/waterfall` and writes an unbounded `.aswf` (not limited by the on-board
PSRAM ring). Stop with Ctrl+C — the header with the final row count is written on exit.

```bash
python waterfall_client.py <board-ip> -o capture.aswf
```

## wf_pull_client.py — segment pull + on-the-fly stitching (#REC-12)

Pull-based alternative to `waterfall_client.py`: instead of holding a live WS connection,
it lists **finalized** segments on the board (`/api/waterfall/segments`), fetches each one
once, appends its rows into a single growing `.aswf`, and only then deletes the segment on
the board (delete happens after the local file is fsynced to disk — a crash mid-transfer
never loses a segment). Safe against restarts: progress (which segments are already
ingested, running row/duration totals) is tracked in a sidecar `<file>.state.json`, so
stopping and re-running the same command resumes without re-ingesting or duplicating rows.

```bash
python wf_pull_client.py <board-ip> --stitch capture.aswf --interval 60
```

## wf_recorder — перенесён в отдельный репозиторий

Настольный GUI-рекордер водопада (`wf_recorder_app.py` + `wf_recorder.bat`, #REC-12/#REC-14)
вынесен в собственный репозиторий, чтобы его релизы не затеняли релизы прошивки в
`releases/latest` этого репо:

**→ <https://github.com/VibeEngineering-LLC/wf-recorder>**

Там — исходники, сборка PyInstaller и готовый `wf_recorder.exe` в
[Releases](https://github.com/VibeEngineering-LLC/wf-recorder/releases) (v0.2.1 — Latest).

Клиент забора/шва (`wf_pull_client.py`) **остался здесь** — его используют тесты
`test_rec13_integrity.py` / `test_rec14_rotation.py` (в новом репо лежит его копия).

> `.n42` and `.aswf` captures are git-ignored — they are generated artifacts, not source.
> The only exception is the bundled `example-waterfall.n42` sample, kept for the viewer.
