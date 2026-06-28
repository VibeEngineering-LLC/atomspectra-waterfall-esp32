# Known Issues

[🇷🇺 Русская версия](KNOWN_ISSUES.md) · **🇬🇧 English**

A list of known bugs, limitations, and fixed issues for the AtomSpectra ESP32 Gateway.

## Open

### BUG-AS-03: Serial number is not read

**Status:** open

The instrument serial number (`serial_number`) stays empty after connection.

**Cause:** in response to the `-inf` command the instrument returns fewer than 40 text lines. The code expects the serial number on line 39 (`process_info_response`), but the actual response is shorter. The calibration (lines 0–10) is read correctly; the serial number is not.

**Impact:** in the Web UI and the XML / CSV export the serial number field is empty. Spectrometer functionality is not affected.

**Workaround:** the serial number can be set manually via the Web UI (calibration panel).

---

## Fixed

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

### Maximum channels — 8192

The Atom Spectra instrument transmits 8192 channels. This is a hardware limitation of the spectrometer, not the firmware.

---

## Compatibility

| Component | Version |
|---|---|
| ESP-IDF | v5.1+ (tested on v5.4) |
| Target chip | ESP32-S3 (USB OTG Host required) |
| Spectrometer | KB Radar "Atom Spectra" (FTDI FT232R, 600000 baud) |
| BecqMoni XML | FormatVersion 120920 |
| Web UI | Modern browsers (Chrome, Firefox, Safari, Edge) |