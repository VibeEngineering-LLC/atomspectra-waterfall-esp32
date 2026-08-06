# #FW-52 evidence — `sys_evt` stack overflow boot-loop (`v1.2.4`)

Upstream-oriented evidence pack for the post-RESET DHCP flap / green-LED blink
loop observed on a lab board after the #FW-51 CDC silent stall.

**Fixed in:** `#FW-52` (`sdkconfig.defaults` stack 4096 + dbglog pass-through on
`sys_evt`/`wifi`).  
**This directory** freezes the **pre-fix** factory app image + serial proof.

## Privacy / what is NOT here

- **No** full 16 MiB flash dump (contains NVS WiFi credentials + LittleFS user data).
- **No** `nvs.bin` / `phy_init.bin`.
- **No** LAN-IP / STA MAC / lab board-id / absolute host paths (sanitized for public PR).
- Full private lab backups stay in the operator’s private lab tree only.

## Build under test

| Field | Value |
|---|---|
| Chip | ESP32-S3 N16R8, rev 0.2 |
| Firmware | `v1.2.4` |
| ELF SHA256 prefix | `f7b67db2d…` (matches crash log) |
| Factory app SHA256 | `3ce540003d93722a9dcc7e0431c066000abe6740e81a26f9f52143c486ce4850` |
| Serial log SHA256 | `271fa32fad027227e7d3316ac2b358b164a630e36462d5294da01a3fa808ce0d` |
| dbglog at fail | NVS ON, level=DEBUG (`debug log ring ON level=2`) |

## Artifacts

| Path | Description |
|---|---|
| `artifacts/factory-app-v1.2.4-pre-fw52.bin` | Exact `factory` partition payload from the frozen full-flash backup (byte-identical to the `v1.2.4` build `atomspectra_gw.bin`) |
| `artifacts/serial-60s-boot-loop.sha256` | SHA256 of the 60 s CDC console capture (`<hash>  -`) |
| `artifacts/addr2line-both.txt` | Symbolized backtraces (relative `components/…` paths only) |
| `artifacts/r00t-dhcp-flap-sample.txt` | DHCP assign/deassign sample during the loop (placeholders for MAC/IP/hostname) |

## Timeline (lab day)

| When | Event |
|---|---|
| Days before | Healthy soak on `v1.2.4`, waterfall recording, dbglog puller; **no reboot**. |
| Morning | **#FW-51**: `usb_cdc: CDC error`, counts freeze, false-green connected (~115 h uptime). |
| Afternoon | Operator RESET → DHCP storm; power-cycle no help. |
| Same afternoon | Console-only serial (spectrometer off USB-host). Green LED blink = reboot loop. |
| Capture | 60 s: many `stack overflow in task sys_evt` after GOT_IP; rare WPA/AES `LoadProhibited` (misleading `spec_cache:` TAG). |
| Same day | Root cause: `sys_evt` stack 2304 + `#FW-50` `hooked_vprintf` (512 B format) with dbglog ON at GOT_IP. |
| Fix | Raise `CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE` to 4096; skip dbglog format/ring on `sys_evt`/`wifi` tasks. |

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

## Related

- `#FW-51` — CDC ERROR silent stall (separate product bug; same lab day)
- `#FW-50` — debug-log ring (necessary context for the vprintf hook)
- `#FW-13` — LittleFS/WiFi jitter (different; WF ON/OFF ping A/B)
