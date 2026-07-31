# Асинхронная загрузка спектров: API задания (спецификация)

Статус: **только спецификация** к `v1.2.2` (#PERF-4), кода пока нет.
Смысл: живой UI не должен ждать, пока полная гистограмма перельётся внутри
обработчика `esp_http_server`.

## Зачем

- Дать «прогрузку спектров» на плату (и с платы) так, чтобы не блокировался
  LIVE — `/api/spectrum*` и водопад по WS.
- Переиспользовать HEAVY-полосу (`http_io_gate`, конкурентность 1) и/или
  отдельную задачу FreeRTOS — тем же приёмом, что `wf_offload` / `wf_fs_task`.

## Предлагаемые эндпоинты

| Метод | Путь | Класс | Тело / ответ |
|---|---|---|---|
| `POST` | `/api/jobs/spectrum-upload` | HEAVY | multipart либо сырые bins + meta JSON → `{ "job_id": "…", "state": "queued" }` |
| `GET` | `/api/jobs/{id}` | LIVE (короткий) | `{ "state": "queued\|running\|done\|error", "progress": 0..1, "err": null }` |
| `DELETE` | `/api/jobs/{id}` | HEAVY | отмена, если задание ещё не завершено |

## Правила

1. `POST` только ставит в очередь и возвращается сразу — либо `503` +
   `Retry-After`, если HEAVY занята или очередь полна.
2. Воркер пишет LittleFS / NVS вне задачи httpd; слот HEAVY не удерживается
   через ожидание Wi-Fi дольше одного чанка.
3. LIVE-опрос (`/api/spectrum`, `/api/spectrum/meta.json`, WS) обязан
   продолжать работать во время загрузки.
4. Автосохранение (`spectrum_autosave`) берёт слот HEAVY через
   `http_io_gate_try_enter()` и держит его на время записи. Задание загрузки
   обязано брать тот же слот — иначе запись в LittleFS снова пойдёт
   параллельно с выгрузкой.
5. На `POST` и `DELETE` — CSRF.

## Набросок клиента

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
