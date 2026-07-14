# macOS Lab Tooling Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Safely back up, flash, and validate AtomSpectra firmware on board #2 (ESP32-S3 N16R8) from macOS, then establish a repeatable ESP-IDF 5.4 development loop.

**Architecture:** A small Python CLI in `tools/lab` owns discovery, immutable release verification, backup/restore guards, flashing, serial capture, and read-only acceptance checks. Hardware artifacts and evidence live under ignored `.lab/`; tracked code contains no machine-specific ports, MAC addresses, Wi-Fi credentials, or binaries.

**Tech Stack:** Python 3.11, `unittest`, esptool 4.8.1, pyserial 3.5, ESP-IDF 5.4, existing host-test Makefile, macOS shell utilities.

## Global constraints

- Work only with board #2. Board #1 is explicitly out of scope.
- Use the `COM` USB-C connector for flashing and serial monitoring. The native `USB` connector is reserved for the analyzer host path after the USB-OTG jumper is soldered.
- Never write eFuses, security bits, NVS credentials, or partition-specific data outside the approved factory image operation.
- Never send AtomSpectra `-rst`; acceptance is read-only.
- A flash requires all three: a compatible live probe, a verified full-chip backup, and the pinned verified release image.
- Restore is never automatic and requires the operator to type the full backup SHA-256.
- Agents must not commit or push. Every task ends with exact manual commit commands for the user.
- Release baseline is `firmware-v1.1.2`, factory image size `1534368`, SHA-256 `a7d39da9998293cff53f8804439b25e05a9702626d052e6ace1cbc8b841ebd4c`, flashed at offset `0x0` as DIO/80 MHz/16 MB.

## File map

```text
.gitignore
INSTALL.md
README.md
tools/lab/
  README.md
  requirements.txt
  atomspectra_lab.py
  atomspectra_lab/
    __init__.py
    artifacts.py
    board.py
    commands.py
    operations.py
    release.py
    serial_log.py
    verify.py
  tests/
    fixtures/
      flash_id_n16r8.txt
      boot_ok.txt
      boot_failure.txt
    test_artifacts.py
    test_board.py
    test_operations.py
    test_release.py
    test_serial_log.py
    test_verify.py
    test_cli.py
docs/superpowers/plans/2026-07-14-macos-lab-tooling.md
```

---

## Task 1: Private artifact store and deterministic environment

**Files:**

- Modify: `.gitignore`
- Create: `tools/lab/requirements.txt`
- Create: `tools/lab/atomspectra_lab/__init__.py`
- Create: `tools/lab/atomspectra_lab/artifacts.py`
- Create: `tools/lab/tests/test_artifacts.py`

- [ ] **Step 1: Write failing artifact tests**

Cover SHA-256 calculation, size validation, atomic JSON output, and refusal to place an artifact outside the configured root.

```python
from pathlib import Path
from tempfile import TemporaryDirectory
import json
import unittest

from atomspectra_lab.artifacts import ArtifactStore, sha256_file


class ArtifactStoreTests(unittest.TestCase):
    def test_hash_and_atomic_manifest(self):
        with TemporaryDirectory() as tmp:
            store = ArtifactStore(Path(tmp))
            image = store.path("backups/board.bin")
            image.parent.mkdir(parents=True)
            image.write_bytes(b"abc")
            self.assertEqual(
                sha256_file(image),
                "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            )
            store.write_json("backups/board.json", {"size": 3})
            self.assertEqual(json.loads(store.path("backups/board.json").read_text()), {"size": 3})

    def test_rejects_path_escape(self):
        with TemporaryDirectory() as tmp:
            with self.assertRaises(ValueError):
                ArtifactStore(Path(tmp)).path("../escape.bin")
```

- [ ] **Step 2: Run the test and confirm the expected import failure**

```bash
cd /Users/vadimkz/Projects/atomspectra-waterfall-esp32
PYTHONPATH=tools/lab /opt/homebrew/bin/python3.11 -m unittest tools/lab/tests/test_artifacts.py -v
```

Expected: `ModuleNotFoundError` or missing `artifacts` module.

- [ ] **Step 3: Implement the artifact boundary**

Implement:

```python
@dataclass(frozen=True)
class ArtifactStore:
    root: Path

    def path(self, relative: str) -> Path:
        root = self.root.resolve()
        candidate = (root / relative).resolve()
        if candidate != root and root not in candidate.parents:
            raise ValueError("artifact path escapes store")
        return candidate

    def write_json(self, relative: str, value: Mapping[str, object]) -> Path:
        target = self.path(relative)
        target.parent.mkdir(parents=True, exist_ok=True)
        temporary = target.with_suffix(target.suffix + ".tmp")
        temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")
        temporary.replace(target)
        return target


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate_file(path: Path, expected_size: int, expected_sha256: str) -> None:
    if path.stat().st_size != expected_size:
        raise ValueError(f"unexpected size for {path}")
    if sha256_file(path) != expected_sha256:
        raise ValueError(f"unexpected SHA-256 for {path}")
```

- [ ] **Step 4: Pin the Python dependencies and private paths**

`tools/lab/requirements.txt`:

```text
esptool==4.8.1
pyserial==3.5
```

Append to `.gitignore`:

```gitignore
# Local AtomSpectra lab state
.lab/
tools/lab/.venv/
tools/lab/**/__pycache__/
```

- [ ] **Step 5: Run tests and prove ignore behavior**

```bash
PYTHONPATH=tools/lab /opt/homebrew/bin/python3.11 -m unittest tools/lab/tests/test_artifacts.py -v
mkdir -p .lab/probe
git check-ignore -v .lab/probe tools/lab/.venv
```

Expected: tests pass and both paths are ignored.

- [ ] **Step 6: Hand the user the manual commit**

```bash
cd /Users/vadimkz/Projects/atomspectra-waterfall-esp32
git status --short
git add .gitignore tools/lab/requirements.txt tools/lab/atomspectra_lab/__init__.py tools/lab/atomspectra_lab/artifacts.py tools/lab/tests/test_artifacts.py
git commit -m "build: add private macOS lab artifact store"
```

---

## Task 2: Command boundary, serial-port selection, and N16R8 probe

**Files:**

- Create: `tools/lab/atomspectra_lab/commands.py`
- Create: `tools/lab/atomspectra_lab/board.py`
- Create: `tools/lab/tests/fixtures/flash_id_n16r8.txt`
- Create: `tools/lab/tests/test_board.py`

- [ ] **Step 1: Capture the known live probe output as a fixture**

The fixture must include the confirmed properties from board #2: ESP32-S3 rev 0.2, 16 MB flash, 8 MB embedded PSRAM, quad flash mode, and 3.3 V.

- [ ] **Step 2: Write failing parser and selection tests**

Test these cases:

- exactly one USB serial candidate is accepted;
- zero or multiple candidates fail closed and list candidates;
- live output parses to `chip=ESP32-S3`, `flash_size=16MB`, `psram_size=8MB`;
- 8 MB/no-PSRAM output is incompatible;
- command failure preserves stdout, stderr, and exit code.

Core API:

```python
@dataclass(frozen=True)
class CommandResult:
    argv: tuple[str, ...]
    returncode: int
    stdout: str
    stderr: str


@dataclass(frozen=True)
class PortCandidate:
    device: str
    description: str
    vid: int | None
    pid: int | None


@dataclass(frozen=True)
class BoardInfo:
    port: str
    chip: str
    revision: str
    flash_size_mb: int
    psram_size_mb: int
    flash_voltage: str

    @property
    def compatible_n16r8(self) -> bool:
        return self.chip == "ESP32-S3" and self.flash_size_mb == 16 and self.psram_size_mb == 8
```

- [ ] **Step 3: Run tests and confirm failure**

```bash
PYTHONPATH=tools/lab /opt/homebrew/bin/python3.11 -m unittest tools/lab/tests/test_board.py -v
```

- [ ] **Step 4: Implement subprocess and esptool boundaries**

`run_command(argv, timeout)` must use an argv list, never `shell=True`. `esptool_prefix()` must resolve the active Python interpreter and return:

```python
(sys.executable, "-m", "esptool")
```

`probe_board(port)` runs `chip_id` and `flash_id`, stores raw output under `.lab/probe/`, parses the result, and raises before any write when compatibility is false.

- [ ] **Step 5: Implement deterministic port discovery**

Use `serial.tools.list_ports.comports()`. Prefer callout devices matching `/dev/cu.usb*`; never select Bluetooth or a port solely because it is first. When more than one candidate remains, require `--port`.

- [ ] **Step 6: Run unit tests and a read-only live probe**

```bash
PYTHONPATH=tools/lab /opt/homebrew/bin/python3.11 -m unittest tools/lab/tests/test_board.py -v
PYTHONPATH=tools/lab tools/lab/.venv/bin/python -m atomspectra_lab.board --port /dev/cu.usbmodem101
```

Before the live command, rediscover the actual `COM` port; `/dev/cu.usbmodem101` is an example from the native connector and must not be copied blindly.

- [ ] **Step 7: Hand the user the manual commit**

```bash
cd /Users/vadimkz/Projects/atomspectra-waterfall-esp32
git status --short
git add tools/lab/atomspectra_lab/commands.py tools/lab/atomspectra_lab/board.py tools/lab/tests/fixtures/flash_id_n16r8.txt tools/lab/tests/test_board.py
git commit -m "feat: add safe ESP32-S3 N16R8 probing"
```

---

## Task 3: Pinned release resolver and immutable download

**Files:**

- Create: `tools/lab/atomspectra_lab/release.py`
- Create: `tools/lab/tests/test_release.py`

- [ ] **Step 1: Write failing release tests**

Use a temporary directory and mocked `urllib.request.urlopen`. Cover:

- API response must have exact tag `firmware-v1.1.2`;
- asset name must be exactly `firmware.factory.bin`;
- download must be exactly `1534368` bytes;
- SHA mismatch deletes the temporary file and fails;
- successful resolution writes a manifest with tag, source URL, size, SHA-256, and retrieval timestamp.

- [ ] **Step 2: Run the expected failure**

```bash
PYTHONPATH=tools/lab /opt/homebrew/bin/python3.11 -m unittest tools/lab/tests/test_release.py -v
```

- [ ] **Step 3: Implement the pinned specification**

```python
@dataclass(frozen=True)
class ReleaseSpec:
    repository: str = "VibeEngineering-LLC/atomspectra-waterfall-esp32"
    tag: str = "firmware-v1.1.2"
    asset: str = "firmware.factory.bin"
    size: int = 1_534_368
    sha256: str = "a7d39da9998293cff53f8804439b25e05a9702626d052e6ace1cbc8b841ebd4c"
    offset: int = 0
    flash_mode: str = "dio"
    flash_freq: str = "80m"
    flash_size: str = "16MB"
```

Resolve release metadata through GitHub's public release API, but trust only the compiled-in specification. Stream the asset to `.part`, validate it, atomically rename it, then write `.lab/releases/firmware-v1.1.2/manifest.json`.

- [ ] **Step 4: Test both offline fixtures and live release metadata**

```bash
PYTHONPATH=tools/lab /opt/homebrew/bin/python3.11 -m unittest tools/lab/tests/test_release.py -v
PYTHONPATH=tools/lab tools/lab/.venv/bin/python -m atomspectra_lab.release --store .lab
shasum -a 256 .lab/releases/firmware-v1.1.2/firmware.factory.bin
```

Expected digest: `a7d39da9998293cff53f8804439b25e05a9702626d052e6ace1cbc8b841ebd4c`.

- [ ] **Step 5: Hand the user the manual commit**

```bash
cd /Users/vadimkz/Projects/atomspectra-waterfall-esp32
git status --short
git add tools/lab/atomspectra_lab/release.py tools/lab/tests/test_release.py
git commit -m "feat: pin and verify AtomSpectra release firmware"
```

---

## Task 4: Full-chip backup and guarded restore

**Files:**

- Create: `tools/lab/atomspectra_lab/operations.py`
- Create: `tools/lab/tests/test_operations.py`

- [ ] **Step 1: Write failing backup tests**

Inject a fake command runner and cover:

- backup command is exactly `read_flash 0x0 0x1000000 <path>`;
- backup is rejected unless output size is exactly 16,777,216 bytes;
- manifest contains board probe, port, timestamp, image size, image SHA-256, and exact esptool argv;
- an existing backup is never overwritten;
- failed reads remove partial files and do not create a valid manifest.

- [ ] **Step 2: Write failing restore-guard tests**

Restore must reject:

- no `--execute` flag;
- incompatible live board;
- missing or invalid backup manifest;
- file size or digest mismatch;
- typed confirmation not equal to the complete 64-character backup SHA-256.

- [ ] **Step 3: Run the expected failures**

```bash
PYTHONPATH=tools/lab /opt/homebrew/bin/python3.11 -m unittest tools/lab/tests/test_operations.py -v
```

- [ ] **Step 4: Implement backup**

Public boundary:

```python
FULL_FLASH_SIZE = 0x1000000


def backup_flash(
    *,
    port: str,
    board: BoardInfo,
    store: ArtifactStore,
    runner: CommandRunner = run_command,
) -> BackupManifest:
    if not board.compatible_n16r8:
        raise SafetyError("full backup is limited to the confirmed N16R8 board")
    # timestamped destination, read_flash, exact size/hash validation, atomic manifest
```

Use a UTC timestamp in the path, for example `.lab/backups/<board-id>/<timestamp>/flash-16mb.bin`. Derive `<board-id>` from a one-way SHA-256 of the chip MAC; expose only the first 12 hex characters in console output while keeping the full MAC only in private `.lab` metadata.

- [ ] **Step 5: Implement restore as an explicit break-glass operation**

Only after every check passes, generate and execute:

```text
python -m esptool --chip esp32s3 --port <actual-port> write_flash 0x0 <verified-backup>
```

Do not erase first. Do not infer a backup. Print the selected manifest and typed-hash requirement before the final prompt.

- [ ] **Step 6: Run tests and perform the live backup before any flash**

```bash
PYTHONPATH=tools/lab /opt/homebrew/bin/python3.11 -m unittest tools/lab/tests/test_operations.py -v
PYTHONPATH=tools/lab tools/lab/.venv/bin/python tools/lab/atomspectra_lab.py backup --port "$COM_PORT"
shasum -a 256 .lab/backups/*/*/flash-16mb.bin
```

The operator must set `COM_PORT` to the currently discovered `COM` connector. Stop if the tool reports anything other than 16 MB flash and 8 MB PSRAM.

- [ ] **Step 7: Hand the user the manual commit**

```bash
cd /Users/vadimkz/Projects/atomspectra-waterfall-esp32
git status --short
git add tools/lab/atomspectra_lab/operations.py tools/lab/tests/test_operations.py
git commit -m "feat: add full-flash backup and guarded restore"
```

---

## Task 5: Factory-image flash safety gate

**Files:**

- Modify: `tools/lab/atomspectra_lab/operations.py`
- Modify: `tools/lab/tests/test_operations.py`

- [ ] **Step 1: Add failing flash-gate tests**

Assert that the runner is never called unless all conditions are true:

1. live probe is compatible N16R8;
2. a current full-backup manifest points to a valid 16 MB image;
3. release manifest matches the compiled-in specification;
4. factory image size and digest match;
5. typed confirmation is exactly `FLASH firmware-v1.1.2`;
6. `--execute` is present.

Also assert the final esptool argv contains:

```text
--chip esp32s3 --port <port> --baud 460800 write_flash
--flash_mode dio --flash_freq 80m --flash_size 16MB 0x0 <factory-image>
```

- [ ] **Step 2: Run the expected failing tests**

```bash
PYTHONPATH=tools/lab /opt/homebrew/bin/python3.11 -m unittest tools/lab/tests/test_operations.py -v
```

- [ ] **Step 3: Implement `flash_factory`**

```python
def flash_factory(
    *,
    port: str,
    board: BoardInfo,
    backup_manifest: Path,
    release_manifest: Path,
    confirmation: str,
    execute: bool,
    runner: CommandRunner = run_command,
) -> FlashManifest:
    # validate every input before constructing the write command
```

The flash record under `.lab/flashes/<timestamp>/manifest.json` must include the preflight results, backup digest, release digest, full argv, exit status, and timestamps. It must never contain Wi-Fi credentials.

- [ ] **Step 4: Prove dry-run behavior**

```bash
PYTHONPATH=tools/lab tools/lab/.venv/bin/python tools/lab/atomspectra_lab.py flash \
  --port "$COM_PORT" \
  --backup-manifest "$BACKUP_MANIFEST" \
  --release-manifest .lab/releases/firmware-v1.1.2/manifest.json
```

Expected: validation summary plus refusal to write because `--execute` and the confirmation string are absent.

- [ ] **Step 5: Execute the one approved factory flash**

```bash
PYTHONPATH=tools/lab tools/lab/.venv/bin/python tools/lab/atomspectra_lab.py flash \
  --port "$COM_PORT" \
  --backup-manifest "$BACKUP_MANIFEST" \
  --release-manifest .lab/releases/firmware-v1.1.2/manifest.json \
  --execute \
  --confirm "FLASH firmware-v1.1.2"
```

No separate `erase_flash` step is permitted.

- [ ] **Step 6: Hand the user the manual commit**

```bash
cd /Users/vadimkz/Projects/atomspectra-waterfall-esp32
git status --short
git add tools/lab/atomspectra_lab/operations.py tools/lab/tests/test_operations.py
git commit -m "feat: guard factory firmware flashing"
```

---

## Task 6: Serial capture and read-only acceptance checks

**Files:**

- Create: `tools/lab/atomspectra_lab/serial_log.py`
- Create: `tools/lab/atomspectra_lab/verify.py`
- Create: `tools/lab/tests/fixtures/boot_ok.txt`
- Create: `tools/lab/tests/fixtures/boot_failure.txt`
- Create: `tools/lab/tests/test_serial_log.py`
- Create: `tools/lab/tests/test_verify.py`

- [ ] **Step 1: Write failing boot-log tests**

Successful boot evidence requires all applicable markers:

- `AtomSpectra Gateway starting...`
- `LittleFS: total=`
- `waterfall ready: ring=` and `(4096 KB PSRAM)`
- either setup portal activation or normal Wi-Fi startup.

Fatal patterns include panic, Guru Meditation, repeated reset, mount failure, heap allocation failure, and watchdog reset. A fatal pattern always overrides success markers.

- [ ] **Step 2: Write failing HTTP-verification tests**

Inject an HTTP client and verify that only GET requests are issued:

- setup mode: `/` and `/api/scan` respond successfully;
- normal mode: `/api/status`, `/api/usb-diag`, and `/api/spectrum.json` return valid JSON;
- status schema contains `analyzer_connected` and `wifi_connected`;
- spectrum schema contains a non-empty bins collection;
- malformed JSON or a missing key fails the report.

- [ ] **Step 3: Write failing analyzer-log tests**

Require:

- VID `0403` and PID `6001` enumeration;
- `FTDI configured baud=600000 8N1`;
- `Analyzer connected (VID=0403 PID=6001)`;
- absence of evidence that `-rst` was sent.

- [ ] **Step 4: Implement bounded serial capture**

```python
def capture_serial(port: str, baud: int, duration_seconds: int, output: Path) -> SerialSummary:
    # bounded timeout, binary-safe decode with replacement, fsync output, analyze markers
```

Default to 115200 baud and 30 seconds. Never leave an unbounded monitor running from the CLI.

- [ ] **Step 5: Implement read-only verification**

```python
class ReadOnlyHttpClient:
    def get_json(self, url: str, timeout: float = 5.0) -> object:
        request = urllib.request.Request(url, method="GET")
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return json.load(response)
```

No POST helper belongs in this module. Store raw responses and a redacted summary under `.lab/acceptance/<timestamp>/`.

- [ ] **Step 6: Run all verification tests**

```bash
PYTHONPATH=tools/lab /opt/homebrew/bin/python3.11 -m unittest \
  tools/lab/tests/test_serial_log.py tools/lab/tests/test_verify.py -v
```

- [ ] **Step 7: Hand the user the manual commit**

```bash
cd /Users/vadimkz/Projects/atomspectra-waterfall-esp32
git status --short
git add tools/lab/atomspectra_lab/serial_log.py tools/lab/atomspectra_lab/verify.py tools/lab/tests/fixtures/boot_ok.txt tools/lab/tests/fixtures/boot_failure.txt tools/lab/tests/test_serial_log.py tools/lab/tests/test_verify.py
git commit -m "feat: add boot and read-only acceptance checks"
```

---

## Task 7: Operator CLI and macOS runbook

**Files:**

- Create: `tools/lab/atomspectra_lab.py`
- Create: `tools/lab/tests/test_cli.py`
- Create: `tools/lab/README.md`

- [ ] **Step 1: Write failing CLI tests**

Patch operation functions and assert argument routing for:

```text
doctor
probe
backup
release
flash
monitor
verify
restore
```

Test that write commands default to dry-run, accept only explicit safety flags, and return non-zero on a failed safety check.

- [ ] **Step 2: Implement `argparse` dispatch**

Keep `main(argv: Sequence[str] | None = None) -> int` testable. Catch known lab errors, print one actionable line to stderr, and return 2. Unexpected exceptions retain a traceback for debugging.

`doctor` must report:

- macOS and architecture;
- exact Python version, requiring 3.11;
- esptool 4.8.1;
- pyserial 3.5;
- candidate serial ports;
- whether ESP-IDF is installed, without requiring it for release flashing;
- whether `.lab/` is ignored.

- [ ] **Step 3: Create the environment and run the full CLI test suite**

```bash
cd /Users/vadimkz/Projects/atomspectra-waterfall-esp32
/opt/homebrew/bin/python3.11 -m venv tools/lab/.venv
tools/lab/.venv/bin/python -m pip install -r tools/lab/requirements.txt
PYTHONPATH=tools/lab tools/lab/.venv/bin/python -m unittest discover -s tools/lab/tests -v
```

- [ ] **Step 4: Document exact connector and safety semantics**

`tools/lab/README.md` must include:

- a labeled board #2 connector map (`COM` for flash/log, `USB` for AtomSpectra);
- why the USB-OTG jumper is soldered only after the first successful boot;
- discovery commands that produce `COM_PORT` and `BACKUP_MANIFEST` values;
- the backup-before-flash invariant;
- release digest and offset;
- recovery procedure using `restore`;
- a warning that board #1 is incompatible with this N16R8 image.

- [ ] **Step 5: Run doctor and ensure help is complete**

```bash
PYTHONPATH=tools/lab tools/lab/.venv/bin/python tools/lab/atomspectra_lab.py --help
PYTHONPATH=tools/lab tools/lab/.venv/bin/python tools/lab/atomspectra_lab.py doctor
```

- [ ] **Step 6: Hand the user the manual commit**

```bash
cd /Users/vadimkz/Projects/atomspectra-waterfall-esp32
git status --short
git add tools/lab/atomspectra_lab.py tools/lab/tests/test_cli.py tools/lab/README.md
git commit -m "feat: add macOS AtomSpectra lab CLI"
```

---

## Task 8: Establish the board #2 hardware baseline

**Files:**

- No tracked files; all evidence goes to ignored `.lab/`

- [ ] **Step 1: Move the data cable to the `COM` connector and rediscover the port**

```bash
ls -1 /dev/cu.usb* 2>/dev/null
PYTHONPATH=tools/lab tools/lab/.venv/bin/python tools/lab/atomspectra_lab.py doctor
```

Do not reuse the old `/dev/cu.usbmodem101` value without rediscovery.

- [ ] **Step 2: Run a read-only compatibility probe**

```bash
PYTHONPATH=tools/lab tools/lab/.venv/bin/python tools/lab/atomspectra_lab.py probe --port "$COM_PORT"
```

Acceptance: ESP32-S3 rev 0.2, 16 MB flash, 8 MB PSRAM. Any mismatch stops the run.

- [ ] **Step 3: Make and independently verify the full-chip backup**

```bash
PYTHONPATH=tools/lab tools/lab/.venv/bin/python tools/lab/atomspectra_lab.py backup --port "$COM_PORT"
ls -l "$BACKUP_IMAGE"
shasum -a 256 "$BACKUP_IMAGE"
```

Acceptance: exact size 16,777,216 bytes and the independent digest equals the manifest.

- [ ] **Step 4: Download and independently verify the pinned factory image**

```bash
PYTHONPATH=tools/lab tools/lab/.venv/bin/python tools/lab/atomspectra_lab.py release
ls -l .lab/releases/firmware-v1.1.2/firmware.factory.bin
shasum -a 256 .lab/releases/firmware-v1.1.2/firmware.factory.bin
```

Acceptance: size `1534368`, digest `a7d39da9998293cff53f8804439b25e05a9702626d052e6ace1cbc8b841ebd4c`.

- [ ] **Step 5: Execute the guarded factory flash**

Run the Task 5 command with the actual `COM_PORT` and `BACKUP_MANIFEST`. Keep the board connected to `COM` until boot validation finishes.

- [ ] **Step 6: Capture and classify the first boot**

```bash
PYTHONPATH=tools/lab tools/lab/.venv/bin/python tools/lab/atomspectra_lab.py monitor \
  --port "$COM_PORT" --duration 45
```

Acceptance: all required boot markers and no fatal marker. If setup mode appears, verify `/` and `/api/scan` only; do not submit Wi-Fi credentials through automation.

- [ ] **Step 7: Power off before soldering the USB-OTG jumper**

Disconnect both USB cables and any analyzer. The user performs and visually checks the solder bridge. The agent does not infer success from a photo alone; electrical behavior must confirm it.

- [ ] **Step 8: Connect AtomSpectra through the native `USB` port**

Keep `COM` attached for logs, attach the analyzer to the board's host-side native USB connector, and preserve a known non-empty spectrum source.

- [ ] **Step 9: Run read-only hardware acceptance**

```bash
PYTHONPATH=tools/lab tools/lab/.venv/bin/python tools/lab/atomspectra_lab.py monitor \
  --port "$COM_PORT" --duration 45
PYTHONPATH=tools/lab tools/lab/.venv/bin/python tools/lab/atomspectra_lab.py verify \
  --base-url "http://$DEVICE_ADDRESS"
```

Acceptance:

- FTDI VID/PID `0403:6001` is detected;
- 600000 baud, 8N1 configuration is logged;
- analyzer connection is true;
- spectrum bins are non-empty;
- no `-rst` transmission is observed;
- report is saved under `.lab/acceptance/`.

- [ ] **Step 10: Record the result without a Git commit**

```bash
git status --short --ignored
```

Expected: hardware evidence appears only as ignored `.lab/`; there are no tracked changes from this task.

---

## Task 9: ESP-IDF 5.4 source-build loop

**Files:**

- Modify: `tools/lab/atomspectra_lab/operations.py`
- Modify: `tools/lab/atomspectra_lab.py`
- Modify: `tools/lab/tests/test_operations.py`
- Modify: `tools/lab/tests/test_cli.py`
- Modify: `tools/lab/README.md`

- [ ] **Step 1: Write failing build-command tests**

Cover:

- exact IDF major/minor must be 5.4;
- a missing IDF environment gives an actionable failure;
- source build runs `idf.py build` only, never `flash`;
- build manifest records Git commit, dirty status, IDF version, command, and hashes of generated binaries;
- a dirty worktree requires explicit `--allow-dirty`.

- [ ] **Step 2: Implement `build` as an isolated operation**

The CLI may call a small generated shell wrapper only to source `export.sh`; all paths must be argv-quoted. Output goes to the normal ignored `build/` directory, while immutable build metadata goes to `.lab/builds/<timestamp>/manifest.json`.

- [ ] **Step 3: Install ESP-IDF 5.4 locally if absent**

Use the official ESP-IDF 5.4 installer under a user-local path such as `/Users/vadimkz/esp/esp-idf-v5.4`. This is a networked write outside the repository and therefore requires explicit runtime approval before execution.

- [ ] **Step 4: Run the existing host tests first**

```bash
cd /Users/vadimkz/Projects/atomspectra-waterfall-esp32/tests/host
make test
make asan
```

- [ ] **Step 5: Build the firmware from source**

```bash
cd /Users/vadimkz/Projects/atomspectra-waterfall-esp32
PYTHONPATH=tools/lab tools/lab/.venv/bin/python tools/lab/atomspectra_lab.py build \
  --idf-path /Users/vadimkz/esp/esp-idf-v5.4
```

Acceptance: `idf.py --version` reports 5.4.x, build succeeds, and the private manifest hashes bootloader, partition table, application image, and factory image where generated.

- [ ] **Step 6: Keep source flashing behind the same safety gates**

Do not add an automatic build-and-flash command. A later source image must pass a separate explicit flash operation with a full backup and image manifest. The release-flash path remains pinned to the official release.

- [ ] **Step 7: Run regression tests**

```bash
PYTHONPATH=tools/lab tools/lab/.venv/bin/python -m unittest discover -s tools/lab/tests -v
```

- [ ] **Step 8: Hand the user the manual commit**

```bash
cd /Users/vadimkz/Projects/atomspectra-waterfall-esp32
git status --short
git add tools/lab/atomspectra_lab/operations.py tools/lab/atomspectra_lab.py tools/lab/tests/test_operations.py tools/lab/tests/test_cli.py tools/lab/README.md
git commit -m "feat: add ESP-IDF 5.4 source-build workflow"
```

---

## Task 10: Integrate documentation and run final verification

**Files:**

- Modify: `INSTALL.md`
- Modify: `README.md`
- Modify: `tools/lab/README.md`

- [ ] **Step 1: Add the macOS path to the top-level documentation**

Document two clearly separate workflows:

1. official release recovery/installation through `tools/lab`;
2. source development through ESP-IDF 5.4.

Link to `tools/lab/README.md`; do not duplicate long commands in three places.

- [ ] **Step 2: Document the hardware compatibility matrix**

```text
Board #2: ESP32-S3 N16R8, 16 MB flash + 8 MB PSRAM — supported.
Board #1: ESP32-S3-USB-OTG V3.1, 8 MB flash + no PSRAM — unsupported by v1.1.2 N16R8 image.
```

- [ ] **Step 3: Run every automated check**

```bash
cd /Users/vadimkz/Projects/atomspectra-waterfall-esp32
PYTHONPATH=tools/lab tools/lab/.venv/bin/python -m unittest discover -s tools/lab/tests -v
cd tests/host
make test
make asan
```

- [ ] **Step 4: Run static repository checks**

```bash
cd /Users/vadimkz/Projects/atomspectra-waterfall-esp32
git diff --check
git status --short --ignored
git ls-files .lab tools/lab/.venv
```

Acceptance: `git diff --check` is clean; `.lab/` and the venv are ignored; `git ls-files` emits nothing for private paths.

- [ ] **Step 5: Repeat the non-writing smoke checks**

```bash
PYTHONPATH=tools/lab tools/lab/.venv/bin/python tools/lab/atomspectra_lab.py doctor
PYTHONPATH=tools/lab tools/lab/.venv/bin/python tools/lab/atomspectra_lab.py probe --port "$COM_PORT"
PYTHONPATH=tools/lab tools/lab/.venv/bin/python tools/lab/atomspectra_lab.py verify --base-url "http://$DEVICE_ADDRESS"
```

- [ ] **Step 6: Hand the user the final documentation commit**

```bash
cd /Users/vadimkz/Projects/atomspectra-waterfall-esp32
git status --short
git add INSTALL.md README.md tools/lab/README.md
git commit -m "docs: document macOS flashing and development workflow"
```

---

## Completion criteria

- Board #2 has an independently verified, restorable 16 MB backup.
- The official `firmware-v1.1.2` factory image was verified by size and SHA-256 before flashing.
- The first boot passed serial validation without panic or reset loop.
- The soldered USB-OTG path enumerated the AtomSpectra FTDI interface at `0403:6001` and 600000 baud.
- Read-only HTTP checks reported a connected analyzer and non-empty spectrum bins.
- No `-rst`, eFuse write, credential mutation, automatic restore, or push occurred.
- The Python lab suite, host tests, ASan tests, and ESP-IDF 5.4 build all pass.
- Private hardware artifacts remain ignored under `.lab/`.
