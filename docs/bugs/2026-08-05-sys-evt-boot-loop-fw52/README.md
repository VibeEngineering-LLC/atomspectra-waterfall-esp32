# #FW-52 evidence — `sys_evt` stack overflow boot-loop (`v1.2.4`)

Upstream-oriented evidence pack for the post-RESET DHCP flap / green-LED blink
loop observed on lab board-2 after the #FW-51 CDC silent stall.

**Fixed in:** commit `583fb20` (`fix(fw): #FW-52 raise sys_evt stack and skip dbglog format on event/wifi tasks`).  
**This directory** freezes the **pre-fix** factory app image + serial proof.

## Privacy / what is NOT here

- **No** full 16 MiB flash dump (contains NVS WiFi credentials + LittleFS user data).
- **No** `nvs.bin` / `phy_init.bin`.
- Full-flash SHA-256 is recorded below so a lab operator can match a private backup
  without publishing the blob.

Private lab tree (gitignored macos-lab `.lab/`):  
`incidents/20260805T103100Z-cdc-stall-board183/` + followups `…-p1-console-only` /
`…-p1b-flash-settings-backup`.

## Board / build under test

| Field | Value |
|---|---|
| Lab board | board-2 / STA `192.168.20.183` / MAC `44:1B:F6:8D:5B:90` |
| Chip | ESP32-S3 N16R8, rev 0.2 |
| Firmware | `v1.2.4` (compile Jul 31 2026 13:22:36) |
| ELF SHA256 prefix | `f7b67db2d…` (matches crash log) |
| Factory app SHA256 | `3ce540003d93722a9dcc7e0431c066000abe6740e81a26f9f52143c486ce4850` |
| Full flash SHA256 (private) | `54fa52b63fc716483713737bcec06f00457f24a6587a5f11dc8288e04dbd05a3` |
| dbglog at fail | NVS ON, level=DEBUG (`debug log ring ON level=2`) |

## Artifacts

| Path | Description |
|---|---|
| `artifacts/factory-app-v1.2.4-board183-pre-fw52.bin` | Exact `factory` partition payload from the frozen full-flash backup (byte-identical to the `v1.2.4` build `atomspectra_gw.bin`) |
| `artifacts/serial-60s-boot-loop.log` | 60 s CDC console capture (~20 reboot cycles) |
| `artifacts/serial-60s-boot-loop.sha256` | Hash of the serial log |
| `artifacts/addr2line-both.txt` | Symbolized backtraces against matching ELF |
| `artifacts/r00t-dhcp-flap-sample.txt` | MikroTik DHCP assign/deassign sample during the loop |

## Timeline (MSK / UTC)

| When | Event |
|---|---|
| 2026-08-01 | Separate #FW-13 class: WF recording ON → R00T ICMP loss; WF OFF → 0% loss (not this bug). |
| 2026-08-01 → 05 | Healthy soak on `.183`, `v1.2.4`, waterfall recording, dbglog puller; **no reboot** (`boots=0`). |
| 2026-08-05 ≈09:00 MSK / 06:00Z | **#FW-51**: `usb_cdc: CDC error`, counts freeze `58490181`, false-green connected (~115 h uptime). |
| ~13:35–13:50 MSK | Operator RESET → DHCP storm; power-cycle no help. Board-1 `.185` takes over logging. |
| ~17:15 MSK / 14:15Z | P1: console-only serial (AtomSpectra off USB-host). Green LED blink = reboot loop. |
| ~17:18 MSK | 60 s capture: **19×** `stack overflow in task sys_evt` after GOT_IP; **1×** WPA/AES `LoadProhibited` (misleading `spec_cache:` TAG). |
| ~17:28–17:35 MSK | P1b: full flash + NVS/phy backups frozen privately. |
| Same day | Root cause: `sys_evt` stack 2304 + `#FW-50` `hooked_vprintf` (512 B format) with dbglog ON at GOT_IP. |
| Fix commit | Raise `CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE` to 4096; skip dbglog format/ring on `sys_evt`/`wifi` tasks. |

## Root cause (short)

1. IDF default `CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=2304`.
2. `#FW-50` debug-log ring installs `esp_log_set_vprintf(hooked_vprintf)`, which
   allocates `char buf[512]` on the **calling** task stack before mirroring to the ring.
3. By `IP_EVENT_STA_GOT_IP`, httpd / mDNS / SNTP / USB are already up; `esp_netif_handlers`
   + `wifi_mgr` INFO both run on `sys_evt` → overflow → `rst:0xc` → DHCP flap.

**Not** a corrupt LittleFS / `spectrum_http_cache` object (cache is RAM/PSRAM only).
The rare Guru line tagged `spec_cache:` is ESP-IDF’s last-log-TAG attribution;
symbols resolve to WPA EAPOL → AES → `esp_intr_alloc`.

## Reproduce (lab)

1. Flash / run `v1.2.4` with dbglog enabled at DEBUG (or DETAILED + net tags).
2. Ensure STA credentials in NVS; boot with subsystems that start before GOT_IP
   (web server, mDNS, optional SNTP) — stock `app_main` order.
3. Observe serial: `Connected, IP: …` then `stack overflow in task sys_evt`.

## Fix verification checklist

After flashing a build that includes `#FW-52`:

- [ ] 60 s serial: **zero** `stack overflow in task sys_evt`
- [ ] STA holds lease; HTTP `/api/system` responds
- [ ] dbglog still ON; reboot twice; still stable
- [ ] `.185` / LaunchAgent unchanged during the test

## Related

- `#FW-51` — CDC ERROR silent stall (separate product bug; morning of same day)
- `#FW-50` — debug-log ring (necessary context for the vprintf hook)
- `#FW-13` — LittleFS/WiFi jitter (different; WF ON/OFF ping A/B)
