# AUD-ASW126 follow-up (post v1.2.6)

Audit `AUD-ASW126-2026-08-12.md` against tag `firmware-v1.2.6`. P0 items from that
release stay closed. This follow-up lands findings 1–13 before the next tag.

## Heap / pin (P1)

1. HTTP Reset no longer `fclose`/`free`s sliced-autosave state. Abort is a flag;
   the main task consumes it (`spectrum_autosave_consume_abort`). Also unlinks
   `current.bin` so Reset survives reboot.
2. Successful offload always unpins. Failed quiet-unlink marks a single-flight
   zombie so the same file is not claimed again; the keep-last ring may still
   delete it.

## Reliability / tests (P2)

- Histogram staging continuity is the host-tested helper (`UINT32_MAX` idle).
- Waterfall row pack is a static PSRAM buffer (`WF_ROW_STRIDE`), not per-row malloc.
- `s_fs_quiet_sig` is Taken only by `wf_fs_task` (httpd/offload use delay fallback).
- `http_io_gate` wraps fopen/fwrite/rename only, not commit-wait loops.

## Hygiene (P3)

- `s_wf_epoch` after `make_room` (do not use `!recording` — that would break stop drain).
- `seg_write_row` is `void`; `COMMIT_LISTENERS_MAX` is 6.
- Quiet budget SOT remains `FLASH_QUIET_BUDGET_MS` in `flash_quiet_math.h`.
- CI `build.yml` uploads `atomspectra_gw.bin` + SHA256 (including `firmware-v*` tags).
  Release Docker binaries are still flashed to hardware per the upstream-PR skill.
