# Async spectrum upload job API (spec)

Status: **spec only** for `v1.2.2ff` (#PERF-4). Not implemented yet.
Live UI must never stream a full histogram upload inside an `esp_http_server` handler.

## Goals

- Allow future “прогрузка спектров” onto the board (or off the board) without blocking LIVE (`/api/spectrum*`, WS waterfall).
- Reuse the HEAVY lane (`http_io_gate`, concurrency=1) and/or a dedicated FreeRTOS worker (same pattern as `wf_offload` / `wf_fs_task`).

## Proposed endpoints

| Method | Path | Class | Body / response |
|---|---|---|---|
| `POST` | `/api/jobs/spectrum-upload` | HEAVY | multipart or raw bins + meta JSON → `{ "job_id": "…", "state": "queued" }` |
| `GET` | `/api/jobs/{id}` | LIVE (tiny) | `{ "state": "queued\|running\|done\|error", "progress": 0..1, "err": null }` |
| `DELETE` | `/api/jobs/{id}` | HEAVY | cancel if not finished |

## Rules

1. `POST` only enqueues; returns immediately (or 503 + `Retry-After` if HEAVY busy / queue full).
2. Worker writes LittleFS / NVS off the httpd task; never hold the HEAVY slot across Wi‑Fi waits longer than one chunk.
3. LIVE polls (`/api/spectrum`, `/api/spectrum/meta.json`, WS) must keep working during an upload job.
4. Autosave (`spectrum_autosave`) already skips while `http_io_gate_busy()` — upload jobs must take the same gate or an equivalent “flash busy” flag.
5. CSRF required on `POST`/`DELETE`.

## Client sketch

```js
async function startUpload(blob) {
  const r = await heavyFetch("/api/jobs/spectrum-upload", { method: "POST", body: blob, headers: { "X-CSRF-Token": csrf } });
  const { job_id } = await r.json();
  for (;;) {
    const s = await (await fetch("/api/jobs/" + job_id)).json();
    if (s.state === "done" || s.state === "error") return s;
    await new Promise(r => setTimeout(r, 500));
  }
}
```
