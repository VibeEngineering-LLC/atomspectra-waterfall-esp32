# Known Issues

[🇷🇺 Русская версия](KNOWN_ISSUES.md) · **🇬🇧 English**

A list of known bugs, limitations, and fixed issues for the AtomSpectra ESP32 Gateway.

## Open

### #FW-50: overnight web UI hang (waterfall + monitoring)

**Status:** open · diagnostics in `v1.2.3` (PSRAM debug-log ring + Mac pull).

**Observation (2026-07-27):** a board on the LAN stopped answering overnight with
waterfall + monitoring enabled; later the dhcp lease from ap expired. AtomSpectra
USB was not power-cycled — instrument spectrum preserved. Other clients on the same
network stayed up → not an AP failure. Do **not** confuse with closed **#FW-13**
(LittleFS autosave freeze / UART CDC blocking — already fixed) or closed
**#FW-8 residual** (histogram drops from autosave — sliced quiet write, 2026-08-12).

**Tooling:** Service → Debug log (NVS `dbglog`); 384 KiB PSRAM ring; the dump is pulled by an
external collector (ours is a launchd job on a Mac every 5 min). Default **off**.

**Flash cost** (ESP-IDF 5.4.2, `esp32s3`, clean `sdkconfig` regenerated from
`sdkconfig.defaults`, `atomspectra_gw.bin` measured):

| Build | Size | Δ vs base |
|---|---|---|
| base (`v1.2.2`) | 1,486,112 B | — |
| ring without `CONFIG_LOG_MAXIMUM_LEVEL_DEBUG` | 1,494,128 B | **+8,016 B** (≈7.8 KiB) |
| ring as shipped (`v1.2.3`) | 1,530,848 B | **+44,736 B** (≈43.7 KiB) |

So the ring code itself costs ≈8 KiB; the other ≈36 KiB are the `ESP_LOGD` strings
across ESP-IDF that the compiler stops stripping once
`CONFIG_LOG_MAXIMUM_LEVEL_DEBUG=y`. Without that flag the ring still builds, but it
would never capture a DEBUG line — and those are exactly what the #FW-50 hypotheses
need. The app partition is 3 MiB; 51% stays free after the change. The 384 KiB ring
itself lives in PSRAM and does not touch flash.

**Ring endpoint contract:**

| Endpoint | CSRF | Why |
|---|---|---|
| `GET /api/debug/log/meta` | no | ring counters/settings only (`enabled`, `level`, `next_seq`, `dropped`, `lost_busy`, `gen`, `fill_pct`) — no `fw_version`/uptime/heap; hygiene, not auth (`/api/system` nearby is still open) |
| `GET /api/debug/log?since=N` | **yes** | the dump exposes SSID, IP and offload URL |
| `POST /api/debug/log/flush` | **yes** | mutating request |
| `GET/POST /api/debug/log/config` | POST — yes | same as other settings |

Requiring the header on a `GET` is deliberate: a third-party page open in the same browser
cannot read the token (same-origin policy), so it cannot pull the log from the user's
address either. Any external collector should do:

```sh
TOKEN=$(curl -s http://<board>/api/csrf-token | sed 's/.*"token":"\([^"]*\)".*/\1/')
curl -s -H "X-CSRF-Token: $TOKEN" "http://<board>/api/debug/log?since=0"
```

**Levels (`level` setting) — a ladder over tag scope, not just depth:**

| Level | What reaches the ring |
|---|---|
| `standard` | `*` = WARN + own tags (`main`, `wf`, `web`, `http_io`, …) at INFO |
| `detailed` | same + system networking tags at INFO (`httpd*`, `wifi*`, `dhcpc`/`dhcps`, `lwip`) |
| `debug` | same + DEBUG for the hottest own tags (`usb_cdc`, `spectrum`, `wf_ofl`) |

**What the ring will NOT catch.** The buffer lives in PSRAM and does not survive
a reboot: after a panic, a WDT reset or power loss the dump is empty and `gen`
restarts. The tool targets the soft-lock hypothesis specifically — the board is
alive and answers over HTTP while the UI is dead; for a "panicked and rebooted"
scenario you need a coredump, not this ring. Lines dropped because the ring
mutex was busy are counted separately from lines evicted by wraparound and show
up as `busy=` next to `drop=` (Service → Debug log) and as `lost_busy` in
`/api/debug/log/meta`: "there were no logs" and "logs were lost at the
interesting moment" are different outcomes and must not be conflated when
analysing a hang.

---

### BUG-AS-08: ⚠ The gateway does not back up the instrument's factory DSP tuning

**Status:** limitation by design + warning (not a gateway-firmware bug).

The "Atom Spectra" instrument stores its pulse-processing tuning (DSP tuning) **inside
itself**: pulse-shape thresholds `RISE` / `FALL` / `NOISE`, `MAX` / `HYST` / `MODE` /
`STEP`, the sampling frequency `F`, the hardware HV/gain potentiometers `POT` / `POT2`,
the pile-up table and the thermal compensation.

Most of these values are visible in the instrument's reply to the `-inf` command —
including the pile-up table (`PileUp […]` / `PileUpThr`) and the **MAX** thermal-
compensation table (`Tco […]`). **However, the baseline thermal-compensation table**
(`POT2` / `V` versus temperature, the `-t_pot` points) **is NOT returned by `-inf`** —
only the `TCpot ON/OFF` flag is present there. The full baseline thermal-compensation
snapshot is given by the separate **`-tc_pot?`** command.

**What was observed.** A case was recorded where this entire tuning **reset to zero**:
`-inf` started returning `RISE 0 FALL 0 NOISE 0 F 1.00 MAX 0 HYST 0 ... POT 0 POT2 0`,
while the instrument itself stayed online and still answered `VERSION`. With zero
thresholds the instrument physically cannot discriminate pulses — counting stops
(`total_counts = 0`, `cps = 0`) even though the USB/CDC link is fine. **Re-plugging USB
does NOT restore the tuning.**

**Impact:** until the DSP tuning is restored the instrument acquires no spectrum (zero
counts). The gateway, WiFi, Web UI, TCP bridge, and the **energy calibration** (the
`E(ch)` polynomial, stored on the gateway in `calib.bin`) are **not affected** — this is
a state of the instrument itself, not of the gateway firmware.

**⚠ Warning — do NOT restore "blind" with tuning commands.**
Although the instrument protocol has configuration commands (`-ris`, `-fall`, `-nos`,
`-max`, `-hyst`, `-U`, `-V`, `-step`, etc.), **you must not write them at random**:
- `POT` / `POT2` are hardware HV/gain potentiometers, **specific to each individual**
  detector. Values from another unit (including examples in documentation) can set the
  wrong bias/gain and damage the instrument.
- Only the factory calibration procedure for that specific unit knows the correct
  thresholds and HV.

**Workaround / recovery:** contact the **instrument's manufacturer (KB Radar)** —
restoring the factory detector profile is done by the manufacturer. Do not try to write
the tuning parameters yourself "blind" (see the warning above).

**Recommendation going forward:** the gateway can **read** `-inf` (it shows the fields in
the Web UI) but does not back them up. While the instrument is correctly tuned it is worth
saving **two** of its replies once, as a reference "snapshot" of the DSP configuration:

1. the reply to **`-inf`** — the main thresholds, `POT` / `POT2`, the MAX thermal-
   compensation table (`Tco […]`) and pile-up;
2. the reply to **`-tc_pot?`** — the baseline thermal-compensation table (`POT2` / `V`
   versus temperature); **it is not present in `-inf`** (see above).

Then a future reset can be detected and both snapshots handed to the manufacturer
(KB Radar) as a reference of the factory tuning.

### BUG-AS-03: Serial number is not read

**Status:** open

The instrument serial number (`serial_number`) stays empty after connection.

**Cause:** in response to the `-inf` command the instrument returns fewer than 40 text lines. The code expects the serial number on line 39 (`process_info_response`), but the actual response is shorter. The calibration (lines 0–10) is read correctly; the serial number is not.

**Impact:** in the Web UI and the XML / CSV export the serial number field is empty. Spectrometer functionality is not affected.

**Workaround:** the serial number can be set manually via the Web UI (calibration panel).

---

## Fixed

### #FW-63: an open segment was lost ENTIRELY on a sudden reset — FIXED (v1.2.17)

Closes the gap left by **P-016** below: that fix protected only routine reboot paths, not
emergency ones (power loss, an external re-flash, a panic).

Header metadata for the open segment was flushed to storage on the condition "every N rows
**and** only while the instrument is not connected over USB." The second half of that
condition nullifies the first in real operation: the instrument is connected the entire time
a measurement runs. The assumption was that a regular buffer flush (`fflush`) was enough to
update the file's size on disk — that's wrong; `fflush` does not require the filesystem to
update the on-disk size record, `fsync` does. On a sudden reset such a segment was read back
as a zero-size file, even though its header had been written, and it was lost outright.

A segment closes by age (10 min), not by row count: at the production interval of 180 s that
window fits only 3–4 rows out of 64 possible. So a sudden reset could cost **up to ten
minutes of measurements** at a time.

Fix: header metadata is now flushed to storage at least once a minute, regardless of whether
the instrument is connected. Verified on a live board, red-green: before the fix, a sudden
reset with 1 row in the open segment wiped it entirely (the loss counter went up); after the
fix, the same reset with 3 rows in the segment — the segment survived (3 rows, 86,102 B), the
counter did not change. The fix's cost was measured too (10 min of idle, identical
conditions before/after): sweep loss 0.00 % in both cases, no metadata flush ever took
longer than 100 ms.

### #MON-2: CPS monitoring showed an empty chart until "Start" was pressed — FIXED (v1.2.16)

The board (`#MON-1`) collects one-second samples into a PSRAM ring autonomously and
continuously, regardless of whether the "Monitoring" tab is open. The "Start" button on the
page, however, began the view from the **current** tail of the ring — everything
accumulated before the click never made it into view, and with no active session the page
displayed nothing at all. Opening the tab, a user saw an empty chart on top of samples the
board had already collected, and concluded that collection only happened while the window
was open — even though the firmware was working correctly.

Fix: opening the page with no active or frozen session now shows the entire history
available in the ring. "Start" still means "begin a new session from this moment" — that
behavior was not changed.

### P-014: an open Web UI degrades measurement completeness — FIXED (v1.2.12)

Serving HTTP competed with instrument data reception for CPU/bus/flash: while the board
answered a browser, USB frames could be lost and one-second spectrum sweeps got rejected by
CRC — **silently**, with no errors in the UI. Measured before the fixes (live board): idle —
0 % loss, an open "Waterfall" tab — 9.4 % of measurement seconds lost, "System" tab — 25.8 %,
six parallel clients — 95.6 %.

`v1.2.11` fixed the two main offenders (LittleFS used-space cache, metadata without spectrum
copy): "System" 2.8 %, "Waterfall" 1.1 % — but flash operations still froze both CPU cores'
cache while running, stalling USB reception.

`v1.2.12` removes the root cause entirely: firmware code and constants now execute from
PSRAM (`CONFIG_SPIRAM_XIP_FROM_PSRAM`), so flash operations no longer freeze the cores'
cache. Loss in all three scenarios — **0.0 %**. As a side effect, the UI got faster (median
response time −29 %, throughput +31 %). Cost: 1.5 MB of free PSRAM taken by the code.

### P-016: an open segment was lost on a routine reboot — FIXED (v1.2.12)

On reboot via the web button, a WiFi change, or an access-point switch, the last open
waterfall segment could be lost entirely, even though the data was already physically on
flash: LittleFS does not update a file's size in its inode without an explicit
`fsync`/`fclose`, and the integrity check on the next boot discarded such a file as empty.
In the worst case (production interval 180 s) this meant up to 3.2 hours of measurements
lost per reboot.

Fix: before every routine reboot path, the firmware now force-flushes and closes the open
segment. Emergency paths (power loss, panic, watchdog) are not covered by this fix — a
segment can still be lost there.

> **Update (v1.2.17):** the emergency-path gap is closed — see **#FW-63** below.

### #DATA-7: silent data loss in the collector on segment name reuse — FIXED (v1.2.9 + wf-recorder v0.3.0)

A segment file name (`seg_NNNNN.aswf`) is unique only within the current directory contents:
after `clear`, a reboot on an empty directory, or an NVS erase the numbering restarts and a
new segment gets the name of an already-collected one. The `wf-recorder` client ≤ v0.2
identified segments by the (name, size) pair — with a fixed interval all full segments weigh
the same, so a new segment was mistaken for an already-stitched one: the delete
acknowledgement was sent **without writing the data**. A real user lost 66 segments
(~13 h of measurements) this way — analysis in
[wf-recorder#1](https://github.com/VibeEngineering-LLC/wf-recorder/issues/1).

Fixed on both sides: firmware `v1.2.9` exposes `seg_seq` and `started_at` from the file
header in the listing; client `v0.3.0+` identifies segments by the SHA-256 of the body and
does not acknowledge deletion until the write is confirmed. Verified by four independent
reproductions, including an unplanned one (a name got reused during normal operation — the
client correctly downloaded it anew). Details: `docs/bugs/2026-08-15-fw8-unreachable-and-503.md`.

### v1.2.9–v1.2.11 batch: stack overflow, debug-log wipe, USB observability — FIXED

- **httpd stack overflow while parsing `PileUp[]`** (`v1.2.9`): `kv_get_array` accepted a
  limit of 100 elements into a 40-element array — writing past the handler's stack. Found
  by external audit, fixed via `sizeof`, covered by a host test with stack canaries.
- **`POST /api/debug/log/flush` wiped the whole log ring on a malformed request body**
  (`v1.2.10`): a read/parse failure still reached the flush with `upto=0, gen=0` (bypassing
  the optimistic lock) while replying `{"ok":true}`. Now a malformed body → `400 Bad
  Request` with no flush; the no-body path (legitimate full reset) is unchanged.
- **USB link observability** (`v1.2.10`–`v1.2.11`, #FW-53): a counter of frames dropped by
  CRC16/framing (`pkt_bad` — the signal existed in the parser but was never read), counters
  of assembled/dropped sweeps (`sweep_commits`/`sweep_drops` — not to be confused with
  `pkt_hist`, which counts chunks, ~128 per spectrum). All in `/api/usb-diag` and on the
  Service page → "USB link" panel; the serial log prints
  `usb_cdc: shproto pkts: N good, M bad` every 10 s.

### #FW-8 residual: `histogram sweep dropped` ≈ 1/min (LittleFS autosave) — FIXED

**Status:** fixed 2026-08-12 · shipped in **`v1.2.6`** · F1a sliced quiet-window autosave + F2 WF baseline/fsync.
Write-up: [`docs/bugs/2026-08-12-histogram-sweep-drops-autosave.md`](docs/bugs/2026-08-12-histogram-sweep-drops-autosave.md).
Release notes: [`docs/releases/v1.2.6-fw8-hist-drop.md`](docs/releases/v1.2.6-fw8-hist-drop.md).

**Was:** WARN about once per minute (~70/h with WF OFF) because a single
`fwrite(~33 KiB)` in the #FW-13 quiet window consumed the budget before the next
1 Hz burst (FTDI FIFO overrun, `rx_ring_drops=0`).

**Now:** write `current.bin.tmp` in 4 KiB slices only while
`FLASH_QUIET_BUDGET_MS` (+ ~180 ms start guard) remains after a hist commit
(`HIST_DROP_I3_SLICED=1` path); offline / `fail_streak≥5` → one-shot; skip live-USB
**batch** row `fsync`, but **`seg_finalize` always syncs**; yield (keep tmp) on
commit-wait×3; `make_room` breaks on deferred unlink; writer-lock 500 ms. Lab:
class A ≈0; rollovers without drop packs.

Follow-up (AUD-ASW126, 2026-08-13): Reset/UAF on sliced autosave, zombie-pin after
failed offload unlink, staging via the host-tested helper, static row pack buffer,
quiet-sig Taken only by `wf_fs_task`, `http_io_gate` only around I/O. Shipped in
**`v1.2.7`**. Write-up:
[`docs/bugs/2026-08-13-asw126-audit-followup.md`](docs/bugs/2026-08-13-asw126-audit-followup.md).
Release notes: [`docs/releases/v1.2.7-asw126-followup.md`](docs/releases/v1.2.7-asw126-followup.md).

### #FW-51: `CDC_ACM_HOST_ERROR` → silent analyzer stall (no reconnect / no alert)

**Status:** code + HW verify — **PASS** 2026-08-05; soak on `v1.2.5`. Release notes:
[`docs/releases/v1.2.5-fw51-fw43-hotplug.md`](docs/releases/v1.2.5-fw51-fw43-hotplug.md).
Write-up: [`docs/bugs/2026-08-05-cdc-host-error-silent-stall.md`](docs/bugs/2026-08-05-cdc-host-error-silent-stall.md).

**Was:** after `CDC error` without `Device disconnected`, handle stayed non-NULL →
false-green `analyzer_connected` / `usb_connected`, frozen counts, no reconnect.
`#FW-43` `spectrometer_dead` returned false when `rx_age≥4s` by design.

**Fix (`main/usb_host_cdc.c` + diag JSON):**
1. `cdc_teardown(reason)` — unified close+null+`cdc_open=false` (mutex claim).
2. `CDC_ACM_HOST_ERROR` → teardown (`error`), same as disconnect.
3. RX watchdog / bus-empty in `usb_connect_task` (arm window, then stale RX or
   `bus_devs_now==0`) → same teardown. ARM (10 s) must be **&lt;** WD (15 s).
4. `usb_host_cdc_is_connected()` requires fresh RX after 5 s grace (not handle alone).
5. `/api/usb-diag`: `cdc_error_count`, `rx_watchdog_trips`, `bus_empty_trips`,
   `reconnect_ok`, `last_fault_reason` / `last_fault_ts_ms`.

**HW verify:** unplug → `last_fault=disconnect`, `reconnect_ok≥1`, live again.

**Follow-on (hotplug UX / #FW-43 soft-lock, `v1.2.5`):** after unplug/replug the
“power-cycle the board” banner + inert Start was a soft false-lock: RX SHPROTO
not reset, single `-inf`, `POST /api/usb/recover` from httpd → reboot. Fix:
RX reset on worker, `-inf` retries under `s_tx_mutex`, deferred teardown on
`usb_conn`, banner + “Retry link”, `last_shproto_ts_ms`. Soft recover cannot
invent a spectrometer MCU VBUS edge.

### #FW-52: post-RESET boot-loop — `sys_evt` stack overflow (dbglog + GOT_IP)

**Status:** fixed; app-flash + serial verify — **PASS** 2026-08-05.
Evidence: [`docs/bugs/2026-08-05-sys-evt-boot-loop-fw52/`](docs/bugs/2026-08-05-sys-evt-boot-loop-fw52/).

**Cause:** `sys_evt` stack 2304 + `#FW-50` dbglog 512-byte buffer on caller stack at
GOT_IP. Fix: stack 4096 + dbglog pass-through on `sys_evt`/`wifi`.

### #UI-43: X-axis scale toggle (s/m/h) on "Monitoring" had no visible effect

**Status:** fixed, firmware [`firmware-v1.0.11`](https://github.com/VibeEngineering-LLC/atomspectra-waterfall-esp32/releases/tag/firmware-v1.0.11).

On the "Monitoring" chart the time-axis unit switch (seconds / minutes / hours) did not
change the visible label step — all three positions produced the same grid.

**Cause:** the old `xTickStep` (`web/monitor.html`) computed the target step as
`target = spanMs/6`, ignoring the selected unit — on a real span (e.g. 199 min) all units
rounded to the same "nice" step (3,600,000 ms), producing an identical grid (`all_equal`).

**Fix:** new density-based `xTickStep` — target tick density is set per unit
(`k = 15 / 8 / 4` for s / m / h) over a single nice-list of steps; the rounding guard-wall
was removed. Now s / m / h yield three different steps on any span.

**Verified (2026-07-11):** on the live board (`/api/system` fw=v1.0.11) over spans
1 min…12 h the units give `distinct=3` different steps; on a 199-min span s=900 s /
m=1800 s / h=3600 s → 13 / 7 / 3 ticks (was: all identical).

### #UI-42 + #FW-49: responsive-width Web UI and prefix field moved to export tabs

**Status:** fixed, firmware `firmware-v1.0.11`.

- **#UI-42:** all 6 Web UI pages (Spectrum / Waterfall / Saved / System / Service /
  Monitoring) did not stretch on wide screens — content was capped by a fixed `max-width`
  on the `.wrap` container. `max-width` removed, grids switched to `auto-fit` — the UI now
  uses the full window width.
- **#FW-49:** the "filename prefix" field lived on the "System" page, away from the export
  itself. Moved onto each export tab (Spectrum / Waterfall / Monitoring) right after the
  export buttons; "System" panels aligned.

### #FW-41: detector temperature added to the waterfall format (ASWF v5)

**Status:** implemented, firmware `firmware-v1.0.11` (internal format bump on bench build
v1.0.7).

The segmented waterfall format `.aswf` gained a detector-temperature field (`t1` from the
device `-inf` reply). Format version bumped **v4 → v5**: header `version:5`,
`row_stride:16406`, new `temperature` field at offset `16402` (float32, °C). When no valid
temperature is available (`di->valid = false`) a NaN-guard (`0x7FC00000`) is written.

**Compatibility:** ASWF v4 readers keep working (the field is appended to the row tail);
v5 readers get per-frame temperature.

### #FW-42 / #FW-43 / #FW-44 / #FW-46 / #FW-48: firmware-v1.0.11 batch

**Status:** fixed, firmware `firmware-v1.0.11`.

- **#FW-42:** the configurable filename prefix now applies to all exports (Spectrum /
  Waterfall / Monitoring / n42 / CSV).
- **#FW-43:** re-initialization of the USB spectrometer on hot re-plug (hotplug) failed —
  fixed.
- **#FW-44:** a fresh board with no configured WiFi produced no console diagnostics —
  diagnostic output on a no-network boot added.
- **#FW-46:** `sdkconfig` — USB-FIFO balance biased toward IN + hostname `atomspectra-gw`.
- **#FW-48:** the experimental DTR/RTS line "kick" was removed (revert) — it had no effect.

### #MON-1: monitoring moved from the browser to the board

**Status:** fixed, firmware [`firmware-v1.0.6`](https://github.com/VibeEngineering-LLC/atomspectra-waterfall-esp32/releases/tag/firmware-v1.0.6).

Previously "Monitoring" recording (interval-averaged CPS) ran in the browser — with the tab
closed or minimized no data accumulated (gaps), and it duplicated the waterfall recording.

**Fix:** monitoring collection moved onto the board (board-side) — accumulation runs
independently of the browser-tab state.

### #FW-39: stitch artifact at waterfall segment boundaries

**Status:** fixed, firmware `firmware-v1.0.6`.

When stitching `.aswf` segments into a single waterfall, stripes / time unevenness appeared
at the boundaries.

**Cause:** the segment header wrote `row_stride` as `16406` instead of the actual `16402` —
a row-step desync during stitching (`main/web_waterfall.c`).

**Fix:** header `row_stride` corrected to the actual `16402`. Files captured by the old
firmware are repaired by recomputing the step (PC-side reconciliation utility).

### #UI-39 / #UI-40 / #UI-41: waterfall crosshair and quick nuclide-line toggles

**Status:** implemented, firmware `firmware-v1.0.6`.

- **#UI-39:** the main waterfall screen gained a crosshair overlay — channel / energy /
  counts + row number and time "from now" (`#N · −Mm Ss`, `live → LIVE`).
- **#UI-40 / #UI-41:** a quick on/off button for nuclide lines on the "Waterfall" and
  "Spectrum" pages (the nuclide-set selection itself is not reset; state survives F5).

### #BRIDGE-3: `-inf`/`-cal` reply race in bridge mode corrupted serial and temperature

**Status:** fixed, firmware [`firmware-v1.0.5`](https://github.com/VibeEngineering-LLC/atomspectra-waterfall-esp32/releases/tag/firmware-v1.0.5).

In TCP-bridge mode (port 8234), polling the device concurrently with `-inf` and `-cal`
mixed the replies: the serial number degenerated to `FFFFFFFF`, temperature read as `0`,
and fields from different replies fused together.

**Fix:** separation and serialization of the `-inf` and `-cal` reply streams in the bridge
path.

**Verified (#TEST-2 Block A, 2026-07-09):** 40 iterations of interleaved `-inf`/`-cal` over
TCP :8234 — 0 corrupted `serial` / `t1` / `version`.

### #BRIDGE-1 + #DATA-1: USB-receive decoupling and end-to-end integrity control (format v4)

**Status:** implemented, firmware [`firmware-v1.0.4`](https://github.com/VibeEngineering-LLC/atomspectra-waterfall-esp32/releases/tag/firmware-v1.0.4).

- **#BRIDGE-1:** under TCP-bridge load, receive from the spectrometer is decoupled through
  an internal RX ring (`data_cb → RX ring → usb_rxw`); an `rx_ring_drops` metric was added
  to the status JSON.
- **#DATA-1 (a / b / c):** end-to-end stream-integrity control — per-row **CRC32** in the
  `.aswf` row + a global segment **seq** (NVS-persisted, gap detection) + a reconciliation
  counter "device-total vs written". The PC client verifies CRC/seq during assembly.

**Verified (#TEST-2 Block A):** `rx_ring_drops` delta = 0; per-row CRC 64/64; n42 ↔ aswf
192/192.

### #UI-38: time X-axis on "Monitoring" + timezone

**Status:** implemented, firmware `firmware-v1.0.4`.

The "Monitoring" chart gained a time X-axis (seconds / minutes / hours + real time); the
timezone is set on the "System" page.

### #UI-36 / #UI-37: AtomSpectra-style spectrum scaling and update-check button

**Status:** implemented, firmware `firmware-v1.0.4` (bench build v1.0.3).

- **#UI-36:** the spectrum display was brought in line with AtomSpectra — a logarithmic Y
  axis with auto-range and a light fill under the line.
- **#UI-37:** the "Check for update" button was sized to its text and pushed to the right.

### #FW-23: n42 export — `StartDateTime` 1970 on every row when auto-starting after reboot

**Status:** fixed in [`79eff24`](https://github.com/VibeEngineering-LLC/atomspectra-waterfall-esp32/commit/79eff24), in `main`. Firmware `firmware-v1.0.1`.

When waterfall recording auto-started after a reboot/power-up, the n42 export showed
`<StartDateTime>1970-01-01T...Z</StartDateTime>` on every segment — the start time was not
synchronized with the real clock.

**Cause:** initialization order in `app_main()` — `usb_host_cdc_init()` (waterfall
boot-autostart) runs **before** `init_sntp()`. `spectrogram_start()` latches
`started_at = time(NULL)` while the RTC is still at epoch 0 (no SNTP reply yet), so
`started_at` is pinned near 1970. Reconnecting USB/WiFi does not fix the value.

**Fix:** added an SNTP callback `spectrogram_time_synced()` (`main/spectrogram.c:831`).
On the first SNTP reply, if recording is active and `started_at < WF_SANE_EPOCH`,
`started_at` is recomputed backwards from elapsed time: `started_at = time(NULL) − elapsed`.
The real recording start is restored retroactively without losing already-written segments.

**Verification (2026-07-08):** a field n42 export `waterfall (30).n42` pulled from the board
(firmware `firmware-v1.0.1`, flashed with the flasher) — 60 segments, **zero `1970`**, all
dated `2026-07-08`; the boot-autostart segment is dated correctly (`12:24:51Z`), and
StartDateTime increases monotonically. Fixed on hardware.

### #WF-2: `/ws/waterfall` returns 404 → endless reconnect loop

**Status:** fixed in [`6c81c37`](https://github.com/VibeEngineering-LLC/atomspectra-waterfall-esp32/commit/6c81c37), in `main`.

The waterfall WebSocket endpoint `/ws/waterfall` intermittently returned `404 Not Found`
and the browser fell into a reconnect loop — the waterfall stream never came up.

**Cause:** the ESP-IDF httpd URI-handler registry overflowed. `CONFIG_HTTPD_MAX_URI_HANDLERS`
was 45, while the total number of registered handlers (pages, assets, REST API, segment
endpoints) grew past 45. Registration of `/ws/waterfall` failed with
`ESP_ERR_HTTPD_HANDLERS_FULL` → the server had no route → `404`.

**Fix:** `CONFIG_HTTPD_MAX_URI_HANDLERS` raised `45 → 60` in `sdkconfig.defaults` — headroom
for current and future endpoints.

### #WF-1: The device crash-looped while waterfall recording was active

**Status:** fixed in [`33fb4a4`](https://github.com/VibeEngineering-LLC/atomspectra-waterfall-esp32/commit/33fb4a4) (+ [`2b36737`](https://github.com/VibeEngineering-LLC/atomspectra-waterfall-esp32/commit/2b36737)), in `main`.

Under load from active waterfall recording (writing `.aswf` segments + simultaneous USB
intake from the detector + WiFi polling), the device crash-looped — rebooting roughly
every ~15 minutes.

**Cause:** `CONFIG_SPI_FLASH_AUTO_SUSPEND` was enabled on a flash chip not on the
ESP-IDF whitelist for that option — under concurrent load the SPI flash entered
auto-suspend at an inopportune moment during a write, causing a crash.

**Fix:** `CONFIG_SPI_FLASH_AUTO_SUSPEND` was disabled.

**Verification (#STAB-2, 2026-07-04):** a 9.40 h continuous board-path waterfall
recording retest (not an emulation) — **0 reboots, 0 dropped flash segments**
(`seg_dropped`) over the whole run, 52 clean `SEG_ROLLOVER` events. Before the fix — a
reboot every ~15 min under the same load. Full report with telemetry and charts:
[`docs/stab2_report.md`](docs/stab2_report.md) ([web version with charts](docs/stab2_report.html)).

### BUG-AS-07: Build fails on a clean clone — undefined `spectrogram_is_recording`

**Status:** fixed in [`1b21d61`](https://github.com/VibeEngineering-LLC/atomspectra-waterfall-esp32/commit/1b21d61)

The GitHub Actions `build` job (ESP-IDF v5.4) failed on a clean clone of the repository
within ~2 minutes. Local builds passed — the bug only showed up on a fresh clone (CI, a
different machine).

**Cause:** an **unfinished fragment** of the USB reconnect logic leaked into the public
repository. In `main/usb_host_cdc.c` (after the CDC link to the device was restored)
there was a call to `spectrogram_is_recording()` — to resume spectrum acquisition with
the `-sta` command if waterfall recording was active. That function is defined in the
waterfall recording subsystem files, which were **not committed** at the time of that
commit (the feature was unfinished local work). On the author's machine everything
compiled because the definition was in the working copy; on a clean clone the compiler
saw only the call with no declaration → `implicit declaration of function
'spectrogram_is_recording'`. ESP-IDF builds with `-Werror=all`, so an implicit
declaration is a hard build error, not a warning.

**Fix:** the unfinished recording-resume hook was removed from `usb_host_cdc.c` in the
public repository — the build again depends only on committed symbols. The
auto-resume-on-reconnect feature itself stays in an unpublished local branch and will
land as a single coherent commit together with the whole stream-to-disk recording
subsystem (not as a lone dangling call).

**Update:** the promised single commit has landed — the autonomous segment-recording
subsystem (`spectrogram.[ch]`, the `/segments` and `/segment` endpoints, the reconnect
hook in `usb_host_cdc.c`, auto-resume via `spectrogram_restore()`) is published as a
whole in the `rec-11-autonomous-recording` branch. A clean-clone build is self-contained
again — `spectrogram_is_recording()` is now defined within the same commit as its call.

**Verification:** the CI `build` job on commit `1b21d61` is green (the prior `b0b8856`
and `958ac19` failed with the same `implicit declaration`). Prevention going forward:
before pushing, verify the branch "as a clean clone" — build from committed files only,
not from a working copy that has uncommitted changes.

### BUG-AS-06: Waterfall stream-to-disk recording stops on its own

**Status:** fixed (see `web/waterfall.html`, write serializer `dskEnqueue`)

While recording the waterfall to a file ("💾 Stream to disk", `.aswf` format via the
browser File System Access API) the recording would stop on its own after a while —
the recording indicator went off and the file stopped growing, even though the
WebSocket stream kept arriving. It reproduced more reliably the more frequently rows
arrived (smaller waterfall interval); at larger intervals it happened less often but
still systematically.

**Cause:** incoming WS rows were written to the `FileSystemWritableFileStream` with
`dskWritable.write(...)` **without `await`**, while every 30 seconds the
`flushDiskHeader` timer rewrote the file header (several more `write()` calls).
`FileSystemWritableFileStream.write()` is asynchronous: starting a new write while a
previous one is still pending throws **synchronously** (the stream is locked,
`The stream is locked`). At the "data-row write ↔ header rewrite" boundary the
exception occurred systematically (roughly every 30 seconds); it was caught by the
write error handler, which called `stopDiskStream()` — so the recording disabled
itself.

**Fix:** all writes to the stream are serialized through a single Promise queue
(`dskEnqueue`) — the next write starts only after the previous one completes, never
two concurrent `write()` calls. Write positions use explicit byte offsets
(`{type:"write",position:…}`) instead of the implicit cursor: the header (positions 0
and 8) and the data rows (position ≥ `8 + header reserve`) no longer overlap.
`stopDiskStream()` drains the queue (`await dskQueue`) before the final header write
and `close()`; a reentrancy guard `dskStopping` was added.

**Verification:** firmware build (ESP-IDF v5.4) with no errors; `node --check` of the
embedded JS — no syntax errors; the change is mirrored into the demo
(`demo/waterfall.html`). On-device functional test: waterfall recording at a 5 s
interval (rows arrive more often than the 30 s header flush fires) — the recording
keeps running without self-disabling.

### BUG-AS-05: Web UI breaks on repeated Ctrl+F5 (HTTP 431)

**Status:** fixed (see `sdkconfig.defaults`, key `CONFIG_HTTPD_MAX_REQ_HDR_LEN`)

On repeated hard reload (Ctrl+F5) of the waterfall page the UI could break — some
assets failed to load, charts/stream stopped updating.

**Cause:** on a hard reload the browser bypasses the cache and sends a request with
the full set of HTTP headers (`Authorization: Basic` from web-auth + `Sec-Fetch-*` +
`Accept*` + `Cache-Control: no-cache`). The combined header block exceeded the
ESP-IDF httpd limit `CONFIG_HTTPD_MAX_REQ_HDR_LEN` (default **512 B**), so the server
replied `431 Request Header Fields Too Large` instead of the page. The firmware stayed
fully stable throughout (heap/CPU unchanged, no reboots) — only the browser UI broke.

**Fix:** httpd limits raised in `sdkconfig.defaults`:
`CONFIG_HTTPD_MAX_REQ_HDR_LEN=1024` and `CONFIG_HTTPD_MAX_URI_LEN=1024` (512 → 1024 B).

**Verification:** Ctrl+F5 stress test with UART capture — after the fix the `431`
responses are gone (was: on every F5 → now: 0), no firmware reboots, WS reconnects
succeed. Method and raw before/after logs: [`tests/stress/`](tests/stress/README.md).

### BUG-AS-04: Calibration missing from XML after reboot

**Status:** fixed in [`3ab490f`](https://github.com/VibeEngineering-LLC/atomspectra-esp32/commit/3ab490f)

After an ESP32 reboot, calibration could be missing from the JSON API and XML export despite a saved `calib.bin`.

**Cause:** in `main.c` the call to `spectrum_load_calibration()` came **before** `spectrum_restore_autosave()`. The autosave restored the full `s_spectrum` structure via `fread()`, overwriting the calibration loaded from `calib.bin`.

**Fix:** the call order in `app_main()` was changed — first `spectrum_restore_autosave()`, then `spectrum_load_calibration()`.

### BUG-AS-02: keV cursor shows the wrong energy

**Status:** fixed in [`a9dab5e`](https://github.com/VibeEngineering-LLC/atomspectra-esp32/commit/a9dab5e)

When hovering the cursor over the spectrum, the keV value did not match the real channel energy.

**Cause:** in the Web UI the energy calculation applied the polynomial coefficients incorrectly — the coefficient order was inverted (the BecqMoni UI shows coefficients in descending order: a=ch⁴, b=ch³, …, e=offset, while the internal format is ascending: c₀=offset, c₁=ch¹, …).

**Fix:** the `channelToKeV()` function in `index.html` recomputes using the ascending polynomial `E(ch) = c₀ + c₁·ch + c₂·ch² + c₃·ch³ + c₄·ch⁴`.

### BUG-AS-01: Web UI — labels and logarithmic scale

**Status:** fixed in [`a9dab5e`](https://github.com/VibeEngineering-LLC/atomspectra-esp32/commit/a9dab5e)

- The logarithmic Y scale rendered incorrectly (zero channels produced -Infinity)
- The device name was displayed as "Atom Spectra" instead of the real one from the instrument

**Fix:** a full Web UI rework — protected `Math.log10()` against zeros, correct device name.

### calibEditing: Calibration fields overwritten while editing

**Status:** fixed in [`7cf0beb`](https://github.com/VibeEngineering-LLC/atomspectra-esp32/commit/7cf0beb)

While manually entering calibration coefficients via the Web UI, the `update()` loop (once per second) overwrote the field contents with the current values from the API.

**Fix:** a `calibEditing` flag was added — while the user edits the form, auto-refresh of the calibration fields is paused.

---

## Limitations

### Flash memory: wear from autosave

Autosave of the current spectrum (`current.bin`, ~33 KB) runs every 60 seconds. That is ~1440 writes per day.

| Flash type | Endurance | Estimated lifetime |
|---|---|---|
| Winbond (typical, 100K cycles) | 100,000 erases/sector | **15–30 years** |
| Cheap no-name (10K cycles) | 10,000 erases/sector | **1.5–5 years** |

LittleFS uses copy-on-write and wear leveling across the whole partition (12.9 MB), which spreads the load. With original ESP32-S3-DevKitC-1 boards using Winbond flash, no issues are expected.

**Possible optimizations** (not implemented, not needed at current lifetimes):
- Increase the autosave interval (e.g. 5 minutes)
- Save only when the spectrum changes (delta check)
- Keep in PSRAM, write to flash only on a clean shutdown

### Single TCP client

The TCP bridge (port 8234) supports **one** simultaneous connection. A second connection attempt is rejected. BecqMoni or AtomSpectra on a PC works until a second instance opens.

### ESP32 supports WiFi 2.4 GHz only

5 GHz networks are not supported in hardware. Make sure the router broadcasts on 2.4 GHz.

### #PERF-5: WiFi STA — modem sleep disabled (`WIFI_PS_NONE`)

ESP-IDF 5.4 STA defaults to **MIN_MODEM** power save, which produces ICMP/HTTP RTT
spread of **tens to hundreds of ms** with near-zero packet loss on an idle board
(observed on board #1, 2026-07-26). After `esp_wifi_start()` in STA mode,
`wifi_manager.c` sets `WIFI_PS_NONE` and RTT flattens out.

**Trade-off:** higher STA power draw; acceptable on USB power, but for a
battery-powered scenario the mode should be reverted to `MIN_MODEM`.

The call is not boot-critical: if `esp_wifi_set_ps()` fails, a warning is logged
and the board keeps running with the default power-save mode.

### Web UI perf (`v1.2.2`, #PERF-1…4)

Under load (browser + AtomSpectra on a PC) an ICMP soak against the board showed
p50 ≈ 7.7 ms / **p95 ≈ 86.8 ms** / p99 ≈ 179 ms at 0% loss — 9000 packets,
30 minutes, firmware `v1.1.2f`. With `v1.2.2`: p50 ≈ 5.1 ms / **p95 ≈ 60.3 ms** /
p99 ≈ 92.1 ms, RFC 3550 jitter 39.6 → 8.8 ms, loss still 0%.
The single worst sample did grow (1.03 → 2.15 s, one sample out of 9000) — full
data and analysis in [`docs/perf1_report.md`](docs/perf1_report.md).

Both runs were taken on builds that already have modem sleep disabled
(`WIFI_PS_NONE`, #PERF-5); `v1.1.2f` is a private "base + #PERF-5" build and has
no tag in the repository. #PERF-5 ships as a separate change, so on this branch
**in isolation** modem sleep stays enabled by default and the absolute numbers
above are not reproducible on it. Treat the delta as reproducible, not the numbers.

- **#PERF-1** — 2 s spectrum snapshot cache; index hot path = `/api/spectrum` + `/api/spectrum/meta.json`.
- **#PERF-2** — HEAVY lane (`http_io_gate`, concurrency=1): 503 + `Retry-After`; `heavyFetch()`; autosave skips while busy.
- **#PERF-3** — waterfall `scheduleDraw()` / rAF coalesce (HiDPI unchanged).
- **#PERF-4** — async upload job API spec only: `docs/spectrum-upload-job-api.md`.

### Maximum channels — 8192

The Atom Spectra instrument transmits 8192 channels. This is a hardware limitation of the spectrometer, not the firmware.

### Autonomous recording: keep-last ring on flash-full has not had a long soak

**Context:** the feature + the #WF-1 fix (`33fb4a4`) are already in `main`; the
`rec-11-autonomous-recording` branch as a whole is not merged yet (extra unrelated commits on top).

Autonomous segment recording (`seg_NNNNN.aswf`) and segment rotation when
`WF_SEG_MAX_ROWS` (64 rows) is reached are **confirmed** on the device: `seg_00000.aswf`
finalizes when full and `seg_00001.aswf` is created automatically.

The **keep-last ring** mode (on flash-full the oldest not-yet-offloaded segment is
overwritten; counters `seg_count` / `seg_dropped`) is implemented but **has not been
verified by a long soak** up to actual partition exhaustion (≈763 rows, ≈5 days at a
10-min interval). Until that test passes, the "flash full → overwrite oldest" boundary is
considered untested — that specific scenario (real partition exhaustion) hasn't been run yet,
though the recording code itself is already in `main`.

**Update 2026-07-04 (#STAB-2):** a 9.40 h board-path soak test confirmed the reliability
of the segment writes themselves — **0 reboots, 0 `seg_dropped`**, 52 clean
`SEG_ROLLOVER` events (see the #WF-1 fix above). This is not the same test as a soak to
actual exhaustion of the 763-row partition (the segment keep-last ring still has not been
run to real overflow) — but a **separate** export limitation was found along the way: the
`ring_capacity` field in `/api/waterfall/status` is fixed at **256 rows** (~4.25 h at a
~60 s cadence) — smaller than the ~763-row partition-capacity estimate above. n42 exports
of recordings longer than ~4.25 h come out truncated (see #FW-19 below). Full writeup:
[`docs/stab2_report.md`](docs/stab2_report.md) §6.

### #FW-19: n42 export truncated by the flash-ring capacity (256 rows ≈ 4.25 h)

**Status:** open (task #32).

When exporting to n42 a recording that ran longer than ~4.25 h, the file contains only
the **last 256 rows** — regardless of how many rows were actually recorded
(`total_rows`). Cause: `ring_capacity=256` (`/api/waterfall/status`) — the ring
physically holds only the last 256 rows; older ones get overwritten by new ones before
the export is pulled.

This is **not a #WF-1 regression** — the `seg_dropped` counter (write losses) stays at 0,
i.e. the write path itself loses no data; the limitation is in the exported ring's
storage size, not write reliability. The `RING_OVERFLOW` event does not detect the
overwrite itself (its condition is the write-lag `total_rows − flash_rows ≥
ring_capacity`, not the wraparound fact).

**Workaround:** for recordings longer than ~4.25 h, periodically pull segments via
`/api/waterfall/segment` (see [`WATERFALL.en.md`](WATERFALL.en.md#autonomous-segment-recording-rec-11-a1))
instead of waiting until the end of the recording for a single export. Details and the
telemetry from the test that found this: [`docs/stab2_report.md`](docs/stab2_report.md) §6.

### Pull polling (`wf_pull_client.py`, #REC-12): no pin against the keep-last ring

The keep-last ring (`make_room()`, `main/spectrogram.c:304-321`) deletes the oldest
FINALIZED segment when Flash space is short — except the currently open segment and any
**pinned** one (`s_seg_pinned`). Only the push offload path (#REC-11-A2, `wf_offload.c`)
pins a segment; the pull path (`GET /api/waterfall/segment`, `main/web_waterfall.c:468`)
does **not** pin the segment while it downloads it.

If the PC client polls SLOWER than the board fills Flash with unread segments, the ring
can delete a segment before `wf_pull_client.py` gets a chance to fetch it. The next
`GET .../segment?name=...` then fails (`error:get:...` in the client log), no delete/ack
is sent, but the segment is already physically gone — the data is lost for good, leaving
a hole in the stitched `.aswf`. This is NOT detected as a "board pause" (the `gap` check
in `wf_pull_client.py:161-169` only catches recording pauses, not segments the ring ate).

**Workaround:** keep the client's `--interval` well below the time it takes the ring to
eat an unread segment at the current recording rate. There is currently no automatic
protection (a pin, like push has) for the pull download path — not implemented.

**Confirmed in practice (2026-08-16/17, joint testing):** the risk actually fires when the
polling interval is close to the segment period (polling every 330 s with a 320 s period —
one segment lost) and when polling pauses exceed the ring capacity. Rule of thumb: **poll at
least twice as often as the segment close period**. Client `v0.4.0+` distinguishes "ring ate
it before pull" from "open segment cut by a reboot" better than before, but the loss class
itself is only prevented by interval discipline.

---

## Compatibility

| Component | Version |
|---|---|
| ESP-IDF | v5.1+ (tested on v5.4) |
| Target chip | ESP32-S3 (USB OTG Host required) |
| Spectrometer | KB Radar "Atom Spectra" (FTDI FT232R, 600000 baud) |
| BecqMoni XML | FormatVersion 120920 |
| Web UI | Modern browsers (Chrome, Firefox, Safari, Edge) |