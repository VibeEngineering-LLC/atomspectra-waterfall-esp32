# Histogram sweep drops after LittleFS autosave / WF segment flash (#FW-8 residual)

**Status:** fixed (lab 2026-08-12) — sliced quiet-window persist (F1a + F2).  
**Related:** #FW-8 staging (intentional drop of torn sweeps), #FW-13 quiet-window autosave (incomplete vs write duration — now sliced), #BRIDGE-1 RX ring (orthogonal).

## Symptom (before fix)

Firmware logs:

```text
W (…) spectrum: histogram sweep dropped (gap in chunks), drops=N
```

Steady rate ≈ **1/min** on a long soak with spectrum + waterfall recording, plus short bursts at waterfall segment finalize/open.

## Mechanism

AtomSpectra sends a full 8192-channel histogram once per second as a burst of SHPROTO `CMD_HISTOGRAM` chunks. The gateway assembles them in PSRAM staging and publishes only contiguous sweeps (#FW-8). A missing chunk (USB RX starved while flash cache is frozen by LittleFS) marks the sweep bad; at 8192 channels the sweep is discarded and `hist_drop` increments.

Primary driver was **periodic `spectrum_autosave` (~60 s, ~0.45 s fwrite of ~33 KiB)** even when phased after a commit “quiet” window — write duration consumed the quiet budget and the next 1 Hz burst lost FIFO bytes (FTDI 256 B ≈ 4.3 ms @ 600 kbaud). Secondary: waterfall segment open/finalize flash work.

Lab discriminator: with waterfall **stopped**, drop rate remained ≈ **70/h** (autosave-only). With waterfall on, ≈ **80/h**. `rx_ring_drops` stayed 0 — loss is below the host RX ring.

## Fix

1. **F0 / H3:** never run LittleFS autosave on commit-wait timeout while USB analyzer is live.
2. **F1a:** snapshot once, write `current.bin.tmp` in **4 KiB slices** only while post-commit quiet budget remains (`FLASH_QUIET_BUDGET_MS`=300, start-slice guard 180 ms); `fclose`+`rename` without live `fsync`; continue on subsequent commits. Do not `Take(0)`-clear the commit semaphore mid-cycle. Gate `fopen` of the tmp file on the same quiet headroom.
3. **F2:** waterfall baseline/rows quiet-sliced; skip row/`finalize` `fsync` while USB is live; after segment `fclose`, wait two quiet windows before `fopen` of the next segment; **`unlink` of completed segments** (offload / ring `make_room` / pull-delete) also waits for the quiet budget (~1 MiB erase otherwise freezes RX).
4. Shared helper: `flash_quiet.*` (+ writer mutex so autosave and WF do not contend in one window).
5. Host unit test: staging gap `exp=256 got=448` → not OK; contiguous 8192 → OK (`tests/host/test_hist_stage.c`).

## Regression (lab)

- WF OFF ≥10 min: Δ`hist_drop` = **0** (class A metronome gone; was ≈70/h).
- WF ON ≥12 min with persist + offload: rollover/offload no longer produce drop packs; rare single orphans (&lt;2/event per 15 min) only.
- `/api/spectrum` smoke does not stall commit rate ≈1/s.

## Telemetry

`/api/spectrum.json` fields `hist_ok` / `hist_drop`; optional diag build (`-DHIST_DROP_DIAG=1`) adds gap `exp/got`, slice timings, and HB `hok`/`hd`.
