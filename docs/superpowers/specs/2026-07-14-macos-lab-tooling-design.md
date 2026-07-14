# macOS Lab Tooling for AtomSpectra ESP32-S3

Date: 2026-07-14  
Status: approved design  
Target release: `firmware-v1.1.2`

## Context

The immediate target is board #2, a dual-USB-C ESP32-S3 development board with an
`S3-N16R8` module. A live read-only probe on macOS confirmed:

- ESP32-S3 revision 0.2;
- 16 MB flash;
- 8 MB embedded PSRAM;
- quad flash at 3.3 V;
- native USB on the connector marked `USB`;
- a separate connector marked `COM` for flashing and serial monitoring.

This matches the hardware profile required by AtomSpectra Waterfall firmware
`v1.1.2`. Board #1 (`ESP32-S3-USB-OTG_V3.1` with an N8 module and display) is not
part of this iteration because it has only 8 MB flash and no PSRAM.

## Goals

1. Safely install the official `firmware-v1.1.2` factory image from macOS.
2. Preserve a restorable copy of the board's current 16 MB flash before writing.
3. Produce auditable evidence for the image, board, flash operation, and runtime
   acceptance checks.
4. Establish a reproducible local ESP-IDF 5.4 build, flash, monitor, and test loop
   for future firmware changes.
5. Keep firmware development tooling in this repository rather than creating a
   third companion repository.

## Non-goals

- Porting the firmware to board #1 or adding display support.
- Modifying the consumer GUI flasher repository.
- Enabling secure boot, flash encryption, NVS encryption, or changing eFuses.
- Automatically resetting or clearing data on the connected Atom Spectra.
- Automatically restoring a backup after a failed flash.
- Tracking raw flash dumps, downloaded binaries, credentials, or device logs in Git.

## Safety invariants

The tooling must enforce these rules:

1. No write operation is allowed until a successful probe identifies an ESP32-S3
   with 16 MB flash and 8 MB PSRAM.
2. The first write is blocked until a complete 16 MB backup exists and its size
   and SHA-256 have been verified.
3. Release flashing is pinned to `firmware-v1.1.2`; `latest` and an unqualified
   `main` build are not accepted.
4. Every downloaded or built image is hashed before use. Its origin, version,
   size, and hash are recorded in a manifest.
5. The initial flash uses the `COM` connector. The native `USB` connector is
   reserved for USB Host operation with Atom Spectra.
6. The tool never invokes eFuse, secure-boot, flash-encryption, or irreversible
   security commands.
7. Failure does not trigger an automatic erase, retry with a different image, or
   restore. The tool stops and preserves evidence.
8. The Atom Spectra is connected only after standalone board acceptance. No
   `-rst` command is sent during acceptance.

## Repository layout

The implementation will add:

```text
tools/lab/
  README.md                  operator workflow and recovery instructions
  atomspectra_lab.py         CLI entry point
  lab/                       focused Python modules
    artifacts.py             paths, manifests, hashing, atomic metadata writes
    board.py                 port discovery and hardware compatibility checks
    commands.py              subprocess argument construction
    release.py               pinned GitHub release retrieval and verification
    serial_log.py            timestamped serial capture
    verify.py                runtime and HTTP acceptance checks
  requirements.txt           pinned Python dependencies
  tests/                     host-only unit and integration tests

.lab/                        private generated artifacts, excluded from Git
  boards/<board-id>/
    backups/<timestamp>/
    flashes/<timestamp>/
    logs/<timestamp>/
  releases/firmware-v1.1.2/
```

The repository `.gitignore` will exclude `.lab/`. Board identifiers in filenames
will be stable local identifiers; full hardware identifiers remain only in private
manifest data.

## CLI interface

The internal tool is a Python CLI, not a GUI. It exposes these commands:

- `doctor`: verify macOS prerequisites, Python environment, esptool, ESP-IDF,
  serial ports, and expected USB connections.
- `probe`: read chip, revision, flash, PSRAM, flash mode, and security state
  without writing.
- `backup`: read the complete 16 MB flash and create a manifest with hashes and
  an explicit restoration command.
- `release`: retrieve the factory image for an exact release tag and record its
  source and SHA-256.
- `flash`: flash only after the compatibility, backup, and image gates pass.
- `monitor`: capture a timestamped serial log without mutating device state.
- `verify`: evaluate boot, memory, partitions, Wi-Fi setup, Web UI, HTTP API, and
  later the Atom Spectra USB connection.
- `restore`: validate the selected backup and print or execute its exact restore
  operation only after explicit operator confirmation.

The CLI invokes esptool and ESP-IDF as subprocesses with argument arrays. It does
not depend on unstable private Python APIs from either project. Commands and exit
codes are logged, while Wi-Fi passwords and unrelated environment secrets are not.

## Environment strategy

- Lab CLI dependencies live in a repository-local `.venv`.
- esptool is pinned in `tools/lab/requirements.txt`.
- Firmware development uses a local ESP-IDF 5.4 installation, matching the
  version declared by the firmware documentation and CI.
- The implementation must not modify the system Python or install packages into
  a PEP 668-managed interpreter.
- Docker is not the primary workflow because it is not currently installed and
  makes serial-device iteration on macOS less direct.

## First-flash flow

1. Connect the Mac to the board's `COM` connector with a data cable. Leave Atom
   Spectra disconnected.
2. Run `doctor` and `probe` and save their results.
3. Run `backup`; require a 16 MB output, compute SHA-256, and store a manifest.
4. Retrieve the factory image specifically for `firmware-v1.1.2` and hash it.
5. Inspect the image metadata and refuse incompatible chip or flash settings.
6. Flash the pinned image through `COM`.
7. Capture the first boot log and verify stable startup without reset loops,
   panics, PSRAM errors, partition errors, or filesystem mount failures.
8. Confirm the `AtomSpectra-Setup` access point, captive setup flow, and Web UI
   while the spectrometer remains disconnected.
9. Power the board off. Close the board's `USB-OTG` solder jumper using solder or
   a 0-ohm resistor. Inspect the bridge for shorts before power is restored.
10. Save any important spectrum from the Atom Spectra using its normal software.
11. Connect Atom Spectra to the board's native `USB` connector with an OTG-capable
    data cable while `COM` remains available for logs.
12. Verify FT232R enumeration, instrument information, live spectrum acquisition,
    and read-only HTTP endpoints. Do not issue a spectrum reset command.
13. Generate an acceptance report containing versions, hashes, commands, logs,
    outcomes, and the restoration command.

## Runtime acceptance criteria

The baseline is accepted only when all of the following pass:

- the device reports ESP32-S3, 16 MB flash, and 8 MB PSRAM;
- `firmware-v1.1.2` boots without panic or reset loop;
- the partition table and LittleFS mount successfully;
- the setup AP and Web UI are reachable;
- the USB Host enumerates the Atom Spectra FT232R device;
- instrument information can be read;
- live spectrum data updates in the Web UI;
- no reset/clear command is sent to the instrument during acceptance;
- the complete backup, image hash, serial log, and acceptance report exist in
  `.lab/`.

## Error handling and recovery

- Ambiguous or missing serial ports stop the operation and list candidates.
- A chip, flash, PSRAM, image, size, or hash mismatch stops before writing.
- A backup read or hash failure invalidates that backup and blocks flashing.
- A flash interruption preserves the log and proposes retrying the same verified
  image after re-entering the bootloader; it does not silently erase first.
- Boot panics, watchdog resets, filesystem errors, and USB enumeration failures
  are reported as distinct acceptance failures.
- Restore is never inferred from failure. The operator selects a validated backup
  and explicitly approves restoration.

## Testing strategy

Host-only automated tests cover:

- parsing representative esptool and ESP-IDF output;
- port selection and ambiguity handling;
- N16R8 compatibility gates;
- manifest validation and atomic writes;
- SHA-256 and artifact-size validation;
- pinned release selection;
- command construction without shell interpolation;
- refusal paths for missing backup, wrong hardware, wrong version, and bad hash;
- redaction of sensitive environment data from logs.

Hardware tests are explicit smoke tests, not automatic unit tests. They cover
probe, backup, first flash, boot monitoring, Wi-Fi/Web UI, USB enumeration, live
spectrum acquisition, and restoration only when restoration is deliberately
requested.

## Future development loop

After baseline acceptance, the same CLI will wrap the supported source workflow:

1. activate ESP-IDF 5.4;
2. build from a clean, identified Git revision;
3. run host tests;
4. flash through `COM`;
5. capture serial logs;
6. run board and HTTP smoke tests;
7. compare results against the accepted `v1.1.2` baseline.

Support for board #1, its display, reduced-memory operation, or alternative
hardware profiles requires a separate design and is not an extension of this
baseline by assumption.
