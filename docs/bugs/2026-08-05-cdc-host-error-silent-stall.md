# #FW-51 — USB Host `CDC_ACM_HOST_ERROR` → silent analyzer stall (no reconnect, no user alert)

**Status:** code fixed 2026-08-05 · awaits app-flash + hardware verify / soak on `.183`  
**Board:** `192.168.20.183` / lab `ae71a8c38527` (board-2)  
**Firmware (incident):** `v1.2.4`  
**Evidence (frozen):**  
`atomspectra-waterfall-esp32-macos-lab/.lab/incidents/20260805T103100Z-cdc-stall-board183/`  
(gitignored `.lab/`; tar before flash/erase — see incident `README.md`)

---

## Fix landed (2026-08-05)

Implements §5–6 below in `main/usb_host_cdc.c` / `atomspectra.h` / `web_server.c`:

| Item | Behavior |
|---|---|
| `cdc_teardown(reason)` | Mutex-claim handle → NULL + `cdc_open=false` → `cdc_acm_host_close` |
| `CDC_ACM_HOST_ERROR` | Teardown (`error`), same path as disconnect |
| RX watchdog | After open ≥10 s: `rx_age≥8s` or `bus_devs_now==0` → teardown |
| `usb_host_cdc_is_connected()` | Handle + fresh RX after 5 s grace (no false-green) |
| `/api/usb-diag` | `cdc_error_count`, `rx_watchdog_trips`, `bus_empty_trips`, `reconnect_ok`, `last_fault_*` |

`#FW-43` `spectrometer_dead()` unchanged in intent (fresh FTDI, no SHPROTO); when RX dies, `is_connected()` goes false first.

**Still required for full close:** explicit flash «да» on `.183` with AtomSpectra on USB-host; unplug + ERROR path; ≥24 h soak without false-green.

---

## 1. User-visible bug

After ~115 h continuous uptime with AtomSpectra on USB-host:

1. Spectrum counts stop increasing.
2. UI / API still report **connected** (`analyzer_connected=true`, `usb_connected=true`, heartbeat `usb=1`).
3. **No** reconnect, **no** banner / toast / status bit that something is wrong.
4. Operator only notices hours later (or via external debug-log review).

This is a product bug: loss of measurement + false health.

---

## 2. What happened (fact chain)

| Step | Fact | Source |
|---|---|---|
| T0 | Last USB RX at uptime `415056727` ms | `/api/usb-diag` `rx_last_ts_ms` |
| T0+~1 s | Log: `E (…) usb_cdc: CDC error` | `board183-20260805.log` |
| Wall | Between pulls `2026-08-05T06:00:16Z` and `06:00:46Z` ≈ **09:00 MSK** | launchd pull headers |
| After | Counts freeze at `58490181` | same log + `/api/status` |
| After | **No** `Device disconnected` line | log (only one `usb_cdc` line that day = the ERROR) |
| Hours later | `bus_devs_now=0`, `cdc_open=true`, `open_attempts=1`, `enum_cb_count=1` | `/api/usb-diag` snapshot in incident `api/` |
| Hours later | `drv_task_alive_ts_ms` also frozen at last RX | same |

Immediate pre-context: normal waterfall segment finalize/open/offload + LittleFS autosave. No WDT, brownout, or reboot.

---

## 3. Root cause (code)

### 3.1 `CDC_ACM_HOST_ERROR` is a no-op

`main/usb_host_cdc.c` `handle_event()`:

```c
case CDC_ACM_HOST_ERROR:
    ESP_LOGE(TAG, "CDC error");
    break;
case CDC_ACM_HOST_DEVICE_DISCONNECTED:
    // close + s_cdc_dev = NULL + diag cdc_open=false
    ...
```

On this incident the stack delivered **ERROR only**, never **DISCONNECTED**. Handle stays non-NULL → `usb_host_cdc_is_connected()` stays true forever.

### 3.2 Health APIs lie

Anything keyed off `s_cdc_dev != NULL` / `cdc_open` reports healthy while the bus has zero devices and RX timestamp is hours stale.

### 3.3 `#FW-43` dead-spectrometer detector does not cover this case

`usb_host_cdc_spectrometer_dead()` explicitly returns false when FTDI frames are stale (`rx_age >= 4000`), assuming “ordinary disconnect” will clear the handle. That assumption failed here.

### 3.4 Related signal (not proven causal)

`line_status_errors=51118` over the session — FTDI line noise / power / cable class. Board has user-authorized VBUS diode bypass; worth noting for soak/QA, but the **software bug** is independent: any ERROR-without-DISCONNECT must recover or alarm.

---

## 4. Related prior work (do not confuse)

| ID | Relation |
|---|---|
| **#FW-13** | LittleFS autosave freezes both cores’ cache — causes WiFi jitter / spectrum drops under write load. **Different** bug; confirmed 2026-08-01 WF ON vs OFF ping A/B (evidence in incident `ping/`). |
| **#FW-43** (v1.0.11) | Hotplug re-init — does not cover ERROR-without-DISCONNECT silent stall. |
| **#FW-22 / #FW-43 diag** | `/api/usb-diag` was essential to prove `bus_devs_now=0` while `cdc_open=true`. Keep / extend for the fix. |
| **#FW-50** | Nightly web UI hang — separate open issue; same debug-log tooling. |

---

## 5. Acceptance criteria for a fix (plan inputs)

Must all be true:

1. **Detect** “USB path dead” within ≤ N seconds (propose N=5…15) when any of:
   - `CDC_ACM_HOST_ERROR`
   - `bus_devs_now==0` while `cdc_open`
   - `rx_age` above threshold while UI claims connected
2. **Recover:** close CDC handle, clear `s_cdc_dev` / `cdc_open`, re-arm connect task / re-enum (same path as real DISCONNECT).
3. **Tell the user:** flip `analyzer_connected` / `usb_connected` (and Web UI) to disconnected/fault **immediately** on detect; optional sticky “USB fault / reconnecting” until open succeeds.
4. **Log:** structured reason (`error` vs `disconnect` vs `rx_watchdog`) + counters exposed on `/api/usb-diag`.
5. **Soak test:** ≥24 h recording + forced ERROR injection (or physical yank that only yields ERROR) must reconnect or stay visibly failed — never false-green with flat counts.

Non-goals for the first patch: fixing LittleFS/WiFi freeze (#FW-13 class residual), changing offload, or blaming WebUI.

---

## 6. Suggested refactoring sketch (for plan author)

Order of work (cheap → structural):

1. **Treat `CDC_ACM_HOST_ERROR` like disconnect** (close + null + wake connect). Minimal patch; verify with physical unplug and with long soak.
2. **RX watchdog** independent of event type: if `cdc_open && rx_age > T` → same teardown path. Covers silent bus loss without any callback.
3. **Single source of truth for “analyzer live”:** combine `cdc_open` + fresh `rx_last_ts` (+ optional SHPROTO freshness). Drive `/api/status`, HB `usb=`, and UI from that, not from handle alone.
4. **User signal:** Web status / Settings / optional WS event on transition connected→fault.
5. **Diag counters:** `cdc_error_count`, `rx_watchdog_trips`, `reconnect_ok`, last fault reason — for field soaks.
6. **QA matrix:** unplug; power-cycle analyzer only; WF recording on/off; overnight soak; confirm no false-green.

Code touch points (starting set):

- `main/usb_host_cdc.c` — `handle_event`, `usb_host_cdc_is_connected`, `usb_host_cdc_spectrometer_dead`, connect task
- callers of `usb_host_cdc_is_connected` / status JSON in `main/web_server.c` (and any UI status mapping)
- heartbeat / main status string that prints `USB:OK`

---

## 7. Immediate operator recovery

On unfixed `v1.2.4` (pre-patch): physical USB reseat of the analyzer **or** reboot the gateway.

With the #FW-51 patch flashed: software should teardown + reconnect within ~8–12 s of ERROR / silent RX loss; if it does not, reseat / reboot and capture `/api/usb-diag` + serial.

---

## 8. Evidence checklist

| Artifact | Path |
|---|---|
| Incident README / timeline | `.lab/incidents/20260805T103100Z-cdc-stall-board183/{README,TIMELINE}.md` |
| Day logs (frozen) | `…/logs/board183-20260804.log`, `…/20260805.log` |
| ±200 lines around ERROR | `…/extracts/cdc-error-context-pm200.txt` |
| API snaps | `…/api/*.json` |
| Code excerpts at archive time | `…/code-refs/*.txt` |
| WF ON/OFF ping A/B (Phase A) | `…/ping/ping183-root-*-300s.txt` |
