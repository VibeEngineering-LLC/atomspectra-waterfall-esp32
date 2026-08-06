# #FW-51 — USB Host `CDC_ACM_HOST_ERROR` → silent analyzer stall (no reconnect, no user alert)

**Status:** code + HW verify **PASS** 2026-08-05 · soak on **`v1.2.5`** before upstream close  
**Firmware (incident):** `v1.2.4` · **fix ship:** `v1.2.5`  
**Release notes:** [`docs/releases/v1.2.5-fw51-fw43-hotplug.md`](../releases/v1.2.5-fw51-fw43-hotplug.md)  
**Private lab evidence:** kept only in the operator’s private lab tree (not published).

---

## Fix landed (2026-08-05)

Implements §5–6 below in `main/usb_host_cdc.c` / `atomspectra.h` / `web_server.c`,
then `#FW-43` hotplug follow-on in `v1.2.5`:

| Item | Behavior |
|---|---|
| `cdc_teardown(reason)` | Mutex-claim handle → NULL + `cdc_open=false` → `cdc_acm_host_close` |
| `CDC_ACM_HOST_ERROR` | Teardown (`error`), same path as disconnect |
| RX watchdog | After open arm window: stale RX or `bus_devs_now==0` → teardown |
| `usb_host_cdc_is_connected()` | Handle + fresh RX after 5 s grace (no false-green) |
| `/api/usb-diag` | `cdc_error_count`, `rx_watchdog_trips`, `bus_empty_trips`, `reconnect_ok`, `last_fault_*` |
| Hotplug follow-on | deferred RX reset on worker, `-inf` retries, deferred `POST /api/usb/recover`, Retry UI |

`#FW-43` `spectrometer_dead()` unchanged in intent (fresh FTDI, no SHPROTO); when RX dies, `is_connected()` goes false first.

**HW verify done:** unplug/replug CDC; spectrometer USB ~10 s cycle; live recover without reboot.  
**Soak:** overnight on `v1.2.5` — never false-green with flat counts.

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
| T0 | Last USB RX timestamp frozen | `/api/usb-diag` `rx_last_ts_ms` |
| T0+~1 s | Log: `E (…) usb_cdc: CDC error` | board debug-log pull |
| After | Counts freeze | same log + `/api/status` |
| After | **No** `Device disconnected` line | log (only one `usb_cdc` line that day = the ERROR) |
| Hours later | `bus_devs_now=0`, `cdc_open=true`, `open_attempts=1` | `/api/usb-diag` |

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

Elevated `line_status_errors` over the session — FTDI line noise / power / cable class. The **software bug** is independent: any ERROR-without-DISCONNECT must recover or alarm.

---

## 4. Related prior work (do not confuse)

| ID | Relation |
|---|---|
| **#FW-13** | LittleFS autosave freezes both cores’ cache — causes WiFi jitter / spectrum drops under write load. **Different** bug. |
| **#FW-43** (v1.0.11) | Hotplug re-init — does not cover ERROR-without-DISCONNECT silent stall. |
| **#FW-22 / #FW-43 diag** | `/api/usb-diag` was essential to prove `bus_devs_now=0` while `cdc_open=true`. |
| **#FW-50** | Nightly web UI hang — separate open issue; same debug-log tooling. |

---

## 5. Acceptance criteria for a fix

Must all be true:

1. **Detect** “USB path dead” within ≤ N seconds (N=5…15) when any of:
   - `CDC_ACM_HOST_ERROR`
   - `bus_devs_now==0` while `cdc_open`
   - `rx_age` above threshold while UI claims connected
2. **Recover:** close CDC handle, clear `s_cdc_dev` / `cdc_open`, re-arm connect task / re-enum.
3. **Tell the user:** flip `analyzer_connected` / `usb_connected` to disconnected/fault immediately.
4. **Log:** structured reason + counters on `/api/usb-diag`.
5. **Soak:** ≥24 h recording — never false-green with flat counts.

---

## 6. Immediate operator recovery

On unfixed `v1.2.4` (pre-patch): physical USB reseat of the analyzer **or** reboot the gateway.

With the #FW-51 patch flashed: software should teardown + reconnect within ~8–15 s of ERROR / silent RX loss; if it does not, reseat / reboot and capture `/api/usb-diag` + serial.
