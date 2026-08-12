# Histogram sweep drops after LittleFS autosave / WF segment flash (#FW-8 residual)

**Status:** root-caused (lab 2026-08-12). Fix not yet landed.  
**Related:** #FW-8 staging (intentional drop of torn sweeps), #FW-13 quiet-window autosave (incomplete vs write duration), #BRIDGE-1 RX ring (orthogonal).

## Symptom

Firmware logs:

```text
W (…) spectrum: histogram sweep dropped (gap in chunks), drops=N
```

Steady rate ≈ **1/min** on a long soak with spectrum + waterfall recording, plus short bursts at waterfall segment finalize/open.

## Mechanism

AtomSpectra sends a full 8192-channel histogram once per second as a burst of SHPROTO `CMD_HISTOGRAM` chunks. The gateway assembles them in PSRAM staging and publishes only contiguous sweeps (#FW-8). A missing chunk (USB RX starved while flash cache is frozen by LittleFS) marks the sweep bad; at 8192 channels the sweep is discarded and `hist_drop` increments.

Primary driver: **periodic `spectrum_autosave` (~60 s, ~0.45 s fwrite)** even when phased after a commit “quiet” window — write duration consumes the quiet budget and the next 1 Hz burst loses FIFO bytes (FTDI 256 B ≈ 4.3 ms @ 600 kbaud). Secondary: waterfall segment open/finalize flash work.

Lab discriminator: with waterfall **stopped**, drop rate remains ≈ **70/h** (autosave-only). With waterfall on, ≈ **80/h**. `rx_ring_drops` stays 0 — loss is below the host RX ring.

## Non-causes (for this symptom)

- Core0 WiFi/HTTP RAM streaming (commit rate stays ~1/s under `/api/spectrum` load).
- Torn live bins (staging already prevents publishing partial sweeps).

## Suggested fix direction

Defer or skip autosave (and heavy WF flash) when the write cannot finish with margin before the next sweep burst; never write after a failed commit-wait timeout. Optional: lengthen autosave period.

## Telemetry

`/api/spectrum.json` fields `hist_ok` / `hist_drop`; optional diag build adds gap `exp/got` and HB `hok`/`hd`/`rxe`/`rrd`.
