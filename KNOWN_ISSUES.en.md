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