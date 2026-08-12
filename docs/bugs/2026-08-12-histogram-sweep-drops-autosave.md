# Histogram sweep drops after LittleFS autosave / WF segment flash (#FW-8 residual)

**Status:** fixed (lab 2026-08-12) — sliced quiet-window persist (F1a + F2); P0 follow-up (hang / durability / bounded skip) 2026-08-12.  
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

1. **F1a (sliced path, default `HIST_DROP_I3_SLICED=1`):** snapshot once, write `current.bin.tmp` in **4 KiB slices** only while post-commit quiet budget remains (`FLASH_QUIET_BUDGET_MS`=300, start-slice guard 180 ms); `fclose`+`rename` without live per-row `fsync`; continue on subsequent commits. Do not `Take(0)`-clear the commit semaphore mid-cycle. Gate `fopen` of the tmp file on the same quiet headroom. Offline USB → one-shot full write.
2. **F2:** waterfall baseline/rows quiet-sliced; skip **batch** row `fsync` while USB is live; **`seg_finalize` always `fsync`s** (once per ≤64 rows / age rollover — durability, not the class-B metronome); after segment `fclose`, wait two quiet windows before `fopen` of the next segment; **`unlink` of completed segments** waits for quiet budget and reports success to callers (`make_room` breaks on defer — no spin under FSLOCK).
3. **Completeness / fail-path (P0 follow-up):**
   - Offline / no USB → quiet budget treated as full (writers always complete).
   - Row `fwrite_quiet_slices`: after 4 idle quiet rounds force one slice; hard outer-round cap (no hang when commits stop).
   - Writer-lock wait **500 ms** (above erase cliffs 150–340 ms); lock contention on row `fflush` does **not** drop the open segment.
   - Autosave: soft **yield** on commit-wait×3 (tmp+offset kept); `fail_streak≥5` → force one-shot (one hist-drop) instead of unbounded deferral; `/api/spectrum.json` exports `autosave_age_sec` / `autosave_fail_streak`.
4. Shared helper: `flash_quiet.*` + `flash_quiet_math.h` (+ writer mutex; atomic `s_commit_us` load/store).
5. Host unit tests: staging gap (`tests/host/test_hist_stage.c`); quiet-budget math (`tests/host/test_flash_quiet.c`).

## Regression (lab)

- WF OFF ≥10 min: Δ`hist_drop` = **0** (class A metronome gone; was ≈70/h).
- WF ON ≥12 min with persist + offload: rollover/offload no longer produce drop packs; rare single orphans (&lt;2/event per 15 min) only.
- `/api/spectrum` smoke does not stall commit rate ≈1/s.

## Telemetry

`/api/spectrum.json` fields `hist_ok` / `hist_drop` / `autosave_age_sec` / `autosave_fail_streak`; optional diag build (`-DHIST_DROP_DIAG=1`) adds gap `exp/got`, slice timings, and HB `hok`/`hd`.
