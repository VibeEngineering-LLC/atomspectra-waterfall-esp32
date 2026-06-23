# Водопад (спектрограмма) — накопление, стрим на ПК, экспорт

[🇬🇧 English](WATERFALL.en.md) · [← README](README.md)

Шлюз умеет копить **водопад** (спектрограмму): последовательность спектров,
снятых через равные интервалы. Каждая строка водопада — это **дельта**
накопительного спектра за интервал, т.е. законченный спектр «импульсы за период»,
8192 канала, `uint16` на канал (**16 КБ на строку**).

Водопад можно:

- смотреть прямо в браузере платы — `http://<IP-платы>/waterfall`;
- **стримить на ПК в реальном времени** по WebSocket;
- выгрузить накопленное во flash одним файлом;
- конвертировать в **ANSI N42.42** — индустриальный формат гамма-спектрометрии;
- открыть как 2D-водопад в офлайн-просмотрщике из этого репозитория.

## Как это устроено

| Параметр | Значение | Где в коде |
|---|---|---|
| Каналов | 8192 (`WF_CHANNELS`) | `main/spectrogram.h` |
| Размер строки | 16 КБ (`WF_ROW_BYTES`) | `main/spectrogram.h` |
| Кольцо в PSRAM | 256 строк × 16 КБ = **4 МБ** (`WF_RING_ROWS_DEFAULT`) | `main/spectrogram.h` |
| Интервал по умолчанию | 5 с (`WF_INTERVAL_DEFAULT`), диапазон 1…3600 | `main/spectrogram.h` |
| Тип данных | `uint16` little-endian | `main/web_waterfall.c` |

Запись (`recording`) копит строки в **кольцевой буфер PSRAM** (последние 256 строк
всегда под рукой для окна/стрима). Если включён `persist`, строки пишутся ещё и во
**flash** (LittleFS) — для последующего экспорта файлом. Когда место на flash
кончается, `flash_full` становится `true`, запись во flash останавливается, а кольцо
и WS-стрим продолжают работать.

> **Калибровка.** Реальная энергетическая калибровка прибора — это полином из
> 5 коэффициентов (`E(ch) = c₀ + c₁·ch + c₂·ch² + c₃·ch³ + c₄·ch⁴`). Она приходит в
> **текстовом заголовке WebSocket** (`/ws/waterfall`) и в заголовке файла `.aswf`,
> а **не** в полях `t1/t2/t3` из `/api/spectrum.json`. Инструменты в `scripts/`
> берут калибровку именно из WS-заголовка, поэтому ось получается в **кэВ**.

## Web API водопада

| Эндпоинт | Метод | Что делает |
|---|---|---|
| `/waterfall` | GET | Встроенный Web UI водопада (heatmap прямо на плате) |
| `/api/waterfall/status` | GET | Статус водопада (JSON, см. ниже) |
| `/api/waterfall/start` | POST | Начать запись |
| `/api/waterfall/stop` | POST | Остановить запись |
| `/api/waterfall/clear` | POST | Очистить кольцо + flash (только когда запись остановлена) |
| `/api/waterfall/config` | POST | `{"interval":N,"persist":bool}` — интервал (с) и запись во flash |
| `/api/waterfall/window` | GET | Снимок кольца (бинарь **ASWW**, до 256 строк) |
| `/api/waterfall/export` | GET | Накопленное во flash одним файлом (бинарь **ASWF**) |
| `/ws/waterfall` | WS | Текстовый заголовок при подключении, далее по одному бинарному кадру (16384 Б) на каждую новую строку |

> Все POST требуют заголовок **`X-CSRF-Token`** (получить из `GET /api/csrf-token`),
> как и остальной API шлюза.

`GET /api/waterfall/status` → JSON:

```json
{
  "recording": false, "persist": false, "flash_full": false, "ready": true,
  "interval_sec": 5, "ring_capacity": 256, "ring_count": 0,
  "total_rows": 0, "flash_rows": 0, "started_at": 0, "channels": 8192
}
```

- `ready` — PSRAM-кольцо выделено;
- `ring_count` — валидных строк в кольце (≤ `ring_capacity`);
- `total_rows` — строк записано с момента `start` (монотонно);
- `flash_rows` — строк в файле во flash.

## Форматы файлов

### ASWW — снимок окна (`/api/waterfall/window`)

Компактный бинарь без заголовка-JSON, отдаётся потоково (один 16-КБ bounce-буфер,
без второго 4-МБ буфера → нет OOM):

```
"ASWW" (4 байта)
channels     u32 LE   (= 8192)
rows         u32 LE   (строк в окне)
first_index  u32 LE   (total-индекс первой строки)
interval     u32 LE   (секунд между строками)
payload      = rows × channels × uint16 LE   (хронологически, старейшая первой)
```

### ASWF — самодостаточный файл (`/api/waterfall/export`, `scripts/waterfall_client.py`)

Бинарь с JSON-заголовком — содержит всё для автономной интерпретации:

```
"ASWF" (4 байта)
header_len   u32 LE
header       = JSON (utf-8), header_len байт
payload      = rows × channels × uint16 LE
```

JSON-заголовок:

```json
{
  "format": "atomspectra-waterfall", "version": 1,
  "channels": 8192, "dtype": "uint16", "byte_order": "little",
  "rows": 1234, "interval_sec": 5, "started_at": 1750000000,
  "serial": "...", "calibration": [c0, c1, c2, c3, c4]
}
```

Поля `serial` и `calibration` присутствуют только если прибор их сообщил.

### Заголовок WebSocket (`/ws/waterfall`)

Первый кадр после подключения — **текстовый** JSON:

```json
{ "type": "header", "channels": 8192, "interval_sec": 5, "total_rows": 187,
  "serial": "...", "calibration": [c0, c1, c2, c3, c4] }
```

Дальше — по одному **бинарному** кадру на каждую новую строку (16384 байта =
8192 × uint16 LE). Одновременно поддерживается до 4 WS-клиентов.

## Стрим на ПК и экспорт в N42

В `scripts/` — три инструмента (нужны `pip install requests websocket-client`):

### `waterfall_n42.py` — экспорт в ANSI N42.42-2012

ANSI N42.42 (IEC 62755) — XML-формат обмена гамма-спектрометрией, который понимают
InterSpec, PeakEasy, Cambio. Каждая строка водопада → один `<RadMeasurement>` с
`RealTimeDuration = PT{interval}S`, разреженные дельта-строки сжаты `CountedZeroes`.
Калибровка пишется в `<EnergyCalibration>` (полином из WS-заголовка).

```bash
# снимок кольца платы → snapshot.n42
python waterfall_n42.py window  <board-ip> -o snapshot.n42

# живой стрим в N42, авто-стоп через 360 с (запись на плате должна быть включена)
python waterfall_n42.py stream  <board-ip> -o live.n42 --seconds 360

# конвертировать ранее снятый .aswf (калибровку добрать с платы по --host)
python waterfall_n42.py convert capture.aswf -o capture.n42 --host <board-ip>
```

Опции: `--detector CsI|NaI|LaBr3|...` (по умолчанию `CsI`), `-o/--out`.

### `waterfall_viewer.html` — офлайн-просмотрщик водопада

Автономная HTML-страница (без сервера, без зависимостей): открой в браузере и
**перетащи `.n42`** внутрь — рисуется heatmap (время ↓ × энергия →, палитра viridis).
Управление: диапазон каналов, детализация, log/контраст, hover-подсказка с
энергией в кэВ (если есть калибровка). Можно отдать файл и через `?src=имя.n42`
при открытии через `http://`.

### `waterfall_client.py` — захват в `.aswf`

Пишет неограниченный по длине `.aswf` из WS-стрима (не упирается в 256-строчное
кольцо платы). Останов по Ctrl+C — заголовок с финальным числом строк дописывается
на выходе. Затем `.aswf` можно сконвертировать в N42 через `waterfall_n42.py convert`.

## Чем открывать N42

| Инструмент | Кто | Заметка |
|---|---|---|
| [InterSpec](https://sandialabs.github.io/InterSpec/) | Sandia | Лучший выбор; показывает time-history измерений |
| `waterfall_viewer.html` | этот репозиторий | 2D-водопад (heatmap) офлайн |
| PeakEasy | LANL | Просмотр спектров |
| Cambio | Sandia | Конвертер/просмотрщик |