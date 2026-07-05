#include "atomspectra.h"
#include "spectrogram.h"
#include "web_waterfall.h"
#include "web_util.h"
#include "wf_offload.h"   // #REC-11-A2: конфиг/статус автономной выгрузки
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <unistd.h>      // close() для web_waterfall_on_close()
#include <dirent.h>      // #REC-11-A1: листинг /storage/wf для /segments
#include <sys/stat.h>    // #REC-11-A1: размер файла сегмента (stat)

static const char *TAG = "wf_web";

#define WF_WS_MAX        4
#define WS_INFLIGHT_MAX  8   // P2-3: лимит несброшенных кадров на клиента

static httpd_handle_t s_server;
static int            s_ws_fds[WF_WS_MAX];
// P2-3/P3-3: s_ws_fds трогают httpd-таск (ws_add/ws_del) и таск-продьюсер
// (wf_broadcast/ws_async_send). Мьютекс закрывает гонку реестра; s_ws_inflight
// считает кадры, поставленные в очередь httpd_queue_work но ещё не отданные, —
// при переполнении кадр дропается (back-pressure вместо роста кучи).
static int               s_ws_inflight[WF_WS_MAX];
static SemaphoreHandle_t s_ws_mutex = NULL;
#define WS_LOCK()   do { if (s_ws_mutex) xSemaphoreTake(s_ws_mutex, portMAX_DELAY); } while (0)
#define WS_UNLOCK() do { if (s_ws_mutex) xSemaphoreGive(s_ws_mutex); } while (0)

/* ---- WS client registry + async broadcast ---- */

static void ws_del_locked(int fd)
{
    for (int i = 0; i < WF_WS_MAX; i++)
        if (s_ws_fds[i] == fd) { s_ws_fds[i] = 0; s_ws_inflight[i] = 0; }
}

static void ws_add(int fd)
{
    // #UI-15 P0a: при отсутствии свободного слота — выталкиваем слот 0 (LRU),
    // иначе зомби после F5 копились и водопад «глох» на новых клиентах.
    WS_LOCK();
    for (int i = 0; i < WF_WS_MAX; i++) if (s_ws_fds[i] == fd) { WS_UNLOCK(); return; }
    int slot = -1;
    for (int i = 0; i < WF_WS_MAX; i++) if (s_ws_fds[i] <= 0) { slot = i; break; }
    if (slot < 0) { ESP_LOGW(TAG, "ws_add: evict fd=%d for fd=%d", s_ws_fds[0], fd); slot = 0; }
    s_ws_fds[slot] = fd; s_ws_inflight[slot] = 0;
    WS_UNLOCK();
}

static void ws_del(int fd)
{
    WS_LOCK();
    ws_del_locked(fd);
    WS_UNLOCK();
}

/* #UI-15 P0: httpd close_fn — единственный надёжный сигнал «сокет закрыт».
   Без него s_ws_fds[] копит зомби fd (RST/FIN при F5 не доходят как
   HTTPD_WS_TYPE_CLOSE) и водопад глохнет после 4 F5. */
void web_waterfall_on_close(httpd_handle_t hd, int sockfd)
{
    (void)hd;
    ws_del(sockfd);
    close(sockfd);   // обязательно: при заданном close_fn httpd сам не закрывает
}

typedef struct { int fd; size_t len; uint8_t buf[]; } ws_send_t;

static void ws_async_send(void *arg)
{
    ws_send_t *a = arg;
    httpd_ws_frame_t fr = { 0 };
    fr.type    = HTTPD_WS_TYPE_BINARY;
    fr.payload = a->buf;
    fr.len     = a->len;
    esp_err_t r = httpd_ws_send_frame_async(s_server, a->fd, &fr);
    WS_LOCK();
    for (int i = 0; i < WF_WS_MAX; i++)
        if (s_ws_fds[i] == a->fd) { if (s_ws_inflight[i] > 0) s_ws_inflight[i]--; break; }
    if (r != ESP_OK) ws_del_locked(a->fd);
    WS_UNLOCK();
    free(a);
}

static void wf_broadcast(const uint16_t *row, size_t bytes, uint32_t idx)
{
    (void)idx;
    WS_LOCK();
    for (int i = 0; i < WF_WS_MAX; i++) {
        int fd = s_ws_fds[i];
        if (fd <= 0) continue;
        if (s_ws_inflight[i] >= WS_INFLIGHT_MAX) continue;   // back-pressure: дроп кадра
        ws_send_t *a = heap_caps_malloc(sizeof(ws_send_t) + bytes, MALLOC_CAP_SPIRAM);
        if (!a) continue;
        a->fd  = fd;
        a->len = bytes;
        memcpy(a->buf, row, bytes);
        if (httpd_queue_work(s_server, ws_async_send, a) != ESP_OK) free(a);
        else s_ws_inflight[i]++;
    }
    WS_UNLOCK();
}

static int append_calib_json(char *buf, int off, int cap)
{
    spectrum_data_t *sp = malloc(sizeof(*sp));
    if (!sp) return off;
    spectrum_get_snapshot(sp);
    if (sp->serial_number[0])
        off += snprintf(buf + off, cap - off, ",\"serial\":\"%s\"", sp->serial_number);
    if (sp->calib_valid) {
        off += snprintf(buf + off, cap - off, ",\"calibration\":[");
        for (int i = 0; i <= sp->calib_order; i++)
            off += snprintf(buf + off, cap - off, "%s%.15g", i ? "," : "", sp->calibration[i]);
        off += snprintf(buf + off, cap - off, "]");
    }
    free(sp);
    return off;
}

static esp_err_t h_status(httpd_req_t *req)
{
    wf_status_t s;
    spectrogram_get_status(&s);
    char buf[448];
    int n = snprintf(buf, sizeof(buf),
        "{\"recording\":%s,\"persist\":%s,\"flash_full\":%s,\"ready\":%s,"
        "\"interval_sec\":%" PRIu32 ",\"ring_capacity\":%" PRIu32 ",\"ring_count\":%" PRIu32 ","
        "\"total_rows\":%" PRIu32 ",\"flash_rows\":%" PRIu32 ","
        "\"seg_count\":%" PRIu32 ",\"seg_dropped\":%" PRIu32 ","
        "\"started_at\":%ld,\"elapsed_sec\":%" PRIu32 ",\"channels\":%d}",
        s.recording ? "true" : "false", s.persist ? "true" : "false",
        s.flash_full ? "true" : "false", s.ready ? "true" : "false",
        s.interval_sec, s.ring_capacity, s.ring_count,
        s.total_rows, s.flash_rows,
        s.seg_count, s.seg_dropped,
        (long)s.started_at, s.elapsed_sec, WF_CHANNELS);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

static esp_err_t h_start(httpd_req_t *req)
{
    if (!web_csrf_check(req)) return ESP_FAIL;
    int r = spectrogram_start();
    /* Водопад должен работать НЕЗАВИСИМО от набора спектра: по Старту сами
       запускаем набор MCA на приборе (-sta — прибор стримит спектр раз в
       секунду). Без этого гистограмма не обновляется и строки водопада
       нулевые (см. PROTOCOL.md, раздел «MCA — управление набором спектра»). */
    if (r == 0) {
        int tx = usb_host_send_text_command("-sta");
        ESP_LOGI(TAG, "waterfall start -> -sta sent to analyzer (rc=%d)", tx);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, r == 0 ? "{\"ok\":true}" : "{\"ok\":false}");
    return ESP_OK;
}

static esp_err_t h_stop(httpd_req_t *req)
{
    if (!web_csrf_check(req)) return ESP_FAIL;
    spectrogram_stop();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t h_clear(httpd_req_t *req)
{
    if (!web_csrf_check(req)) return ESP_FAIL;
    int r = spectrogram_clear();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, r == 0 ? "{\"ok\":true}" : "{\"ok\":false,\"err\":\"recording\"}");
    return ESP_OK;
}

static esp_err_t h_config(httpd_req_t *req)
{
    if (!web_csrf_check(req)) return ESP_FAIL;
    char body[256] = { 0 };
    int rl = httpd_req_recv(req, body, sizeof(body) - 1);
    if (rl <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty"); return ESP_FAIL; }
    body[rl] = 0;
    cJSON *root = cJSON_Parse(body);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json"); return ESP_FAIL; }
    cJSON *iv = cJSON_GetObjectItem(root, "interval");
    if (cJSON_IsNumber(iv)) spectrogram_set_interval((uint32_t)iv->valuedouble);
    cJSON *ps = cJSON_GetObjectItem(root, "persist");
    if (cJSON_IsBool(ps)) spectrogram_set_persist(cJSON_IsTrue(ps));
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* GET /api/waterfall/window -> binary:
   "ASWW"(4) channels(u32 LE) rows(u32) first_index(u32) interval(u32) payload */
/* emit-колбэк: отдаёт одну строку окна chunked-кусками <= 8 КБ. */
static bool wf_window_emit(void *ctx, const uint16_t *row, size_t bytes)
{
    httpd_req_t *req = (httpd_req_t *)ctx;
    const char *p = (const char *)row;
    size_t off = 0;
    while (off < bytes) {
        size_t c = bytes - off; if (c > 8192) c = 8192;
        if (httpd_resp_send_chunk(req, p + off, c) != ESP_OK) return false;
        off += c;
    }
    return true;
}

static esp_err_t h_window(httpd_req_t *req)
{
    /* Потоковая отдача всего кольца (до 256 строк) через единственный 16-КБ
       bounce-буфер: НЕ держим второй 4-МБ буфер в PSRAM рядом с ring → нет
       OOM/HTTP 500. rows берём снимком ring_count; ring_count монотонно растёт
       до заполнения, поэтому стрим отдаст ровно столько строк, сколько в шапке. */
    wf_status_t s;
    spectrogram_get_status(&s);
    uint16_t *bounce = heap_caps_malloc(WF_ROW_BYTES, MALLOC_CAP_SPIRAM);
    if (!bounce) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }

    uint32_t rows = s.ring_count;
    uint32_t first = (rows <= s.total_rows) ? (s.total_rows - rows) : 0;
    uint8_t pre[20];
    memcpy(pre, "ASWW", 4);
    uint32_t ch = WF_CHANNELS, iv = s.interval_sec;
    memcpy(pre + 4,  &ch,    4);
    memcpy(pre + 8,  &rows,  4);
    memcpy(pre + 12, &first, 4);
    memcpy(pre + 16, &iv,    4);

    httpd_resp_set_type(req, "application/octet-stream");
    if (httpd_resp_send_chunk(req, (char *)pre, 20) != ESP_OK) {
        heap_caps_free(bounce); return ESP_FAIL;
    }
    spectrogram_stream_window(bounce, rows, NULL, wf_window_emit, req);
    httpd_resp_send_chunk(req, NULL, 0);
    heap_caps_free(bounce);
    return ESP_OK;
}

/* Контекст отдачи одной строки кольца как <RadMeasurement>. */
typedef struct {
    httpd_req_t *req;
    char        *acc;
    uint32_t     interval_sec;
    time_t       started_at;
    bool         have_cal;
    uint32_t     r;
    /* #FW-5: реальные длительности срезов окна (сек). durs[] выровнен с потоком
       строк stream_window; локальный индекс = r - r0. row_start — накопительное
       реальное начало текущего среза (StartDateTime). */
    const uint16_t *durs;
    uint32_t     r0;
    time_t       row_start;
} n42_ctx_t;

static bool n42_row_emit(void *vctx, const uint16_t *row, size_t bytes)
{
    n42_ctx_t *c = (n42_ctx_t *)vctx;
    (void)bytes;
    char *acc = c->acc, tbuf[40];
    struct tm tmv;
    uint32_t r = c->r++;
    /* #FW-5: реальная длительность среза (дельта живого времени прибора) вместо
       номинального interval_sec. dur==0 (или durs нет) → подставляем номинал, чтобы
       CPS = counts/dur не делился на ноль и таймлайн не застывал. */
    uint32_t local = r - c->r0;
    uint32_t dur = c->durs ? c->durs[local] : 0;
    if (dur == 0) dur = c->interval_sec;
    time_t ts = c->row_start;
    gmtime_r(&ts, &tmv);
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%SZ", &tmv);
    int n = snprintf(acc, 8192,
        "  <RadMeasurement id=\"m-%" PRIu32 "\">\n    <MeasurementClassCode>Foreground</MeasurementClassCode>\n    <StartDateTime>%s</StartDateTime>\n    <RealTimeDuration>PT%" PRIu32 "S</RealTimeDuration>\n    <Spectrum id=\"m-%" PRIu32 "-s-1\" radDetectorInformationReference=\"det-1\"%s>\n      <LiveTimeDuration>PT%" PRIu32 "S</LiveTimeDuration>\n      <ChannelData compressionCode=\"CountedZeroes\">",
        r, tbuf, dur, r, c->have_cal ? " energyCalibrationReference=\"ecal-1\"" : "", dur);
    c->row_start += dur;   /* #FW-5: следующий срез стартует после этого */
    if (httpd_resp_send_chunk(c->req, acc, n) != ESP_OK) return false;
    int off = 0; bool first = true; uint32_t i = 0;
    while (i < WF_CHANNELS) {
        int wrote;
        if (row[i] == 0) {
            uint32_t z = 0;
            while (i < WF_CHANNELS && row[i] == 0) { z++; i++; }
            wrote = snprintf(acc + off, 8192 - off, "%s0 %" PRIu32, first ? "" : " ", z);
        } else {
            wrote = snprintf(acc + off, 8192 - off, "%s%u", first ? "" : " ", (unsigned)row[i]); i++;
        }
        off += wrote; first = false;
        if (off > 8192 - 32) { if (httpd_resp_send_chunk(c->req, acc, off) != ESP_OK) return false; off = 0; }
    }
    if (off > 0) { if (httpd_resp_send_chunk(c->req, acc, off) != ESP_OK) return false; }
    n = snprintf(acc, 8192, "</ChannelData>\n    </Spectrum>\n  </RadMeasurement>\n");
    if (httpd_resp_send_chunk(c->req, acc, n) != ESP_OK) return false;
    return true;
}


/* GET /api/waterfall/export.n42 -> ANSI N42.42-2011 XML.
   Каждая строка водопада = отдельный <RadMeasurement> со спектром-дельтой
   за интервал; ChannelData сжата CountedZeroes. Калибровка (если валидна) —
   в <EnergyCalibration>. Источник — кольцо PSRAM (как /api/waterfall/window),
   поэтому экспорт работает и при выключенном persist (Flash). */
static esp_err_t h_export_n42(httpd_req_t *req)
{
    wf_status_t s;
    spectrogram_get_status(&s);
    if (s.ring_count == 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no waterfall data");
        return ESP_FAIL;
    }

    spectrum_data_t *sp = malloc(sizeof(*sp));
    if (sp) spectrum_get_snapshot(sp);
    bool have_cal = sp && sp->calib_valid;

    uint16_t *row = heap_caps_malloc(WF_ROW_BYTES, MALLOC_CAP_SPIRAM);
    char *acc = malloc(8192);
    if (!row || !acc) {
        if (row) heap_caps_free(row);
        if (acc) free(acc);
        if (sp) free(sp);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"waterfall.n42\"");

    int n;
    n = snprintf(acc, 8192,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<RadInstrumentData xmlns=\"http://physics.nist.gov/N42/2011/N42\">\n"
        "  <RadInstrumentInformation id=\"inst-1\">\n"
        "    <RadInstrumentManufacturerName>KB Radar</RadInstrumentManufacturerName>\n"
        "    <RadInstrumentModelName>Atom Spectra</RadInstrumentModelName>\n");
    httpd_resp_send_chunk(req, acc, n);
    if (sp && sp->serial_number[0]) {
        char esc[512];
        web_xml_escape(sp->serial_number, esc, sizeof(esc));
        n = snprintf(acc, 8192,
            "    <RadInstrumentIdentifier>%s</RadInstrumentIdentifier>\n", esc);
        httpd_resp_send_chunk(req, acc, n);
    }
    n = snprintf(acc, 8192,
        "    <RadInstrumentClassCode>Spectroscopic Personal Radiation Detector</RadInstrumentClassCode>\n"
        "  </RadInstrumentInformation>\n"
        "  <RadDetectorInformation id=\"det-1\">\n"
        "    <RadDetectorCategoryCode>Gamma</RadDetectorCategoryCode>\n"
        "    <RadDetectorKindCode>CsI</RadDetectorKindCode>\n"
        "  </RadDetectorInformation>\n");
    httpd_resp_send_chunk(req, acc, n);

    if (have_cal) {
        n = snprintf(acc, 8192,
            "  <EnergyCalibration id=\"ecal-1\">\n    <CoefficientValues>");
        httpd_resp_send_chunk(req, acc, n);
        for (int i = 0; i <= sp->calib_order; i++) {
            n = snprintf(acc, 8192, "%s%.9g", i ? " " : "", sp->calibration[i]);
            httpd_resp_send_chunk(req, acc, n);
        }
        n = snprintf(acc, 8192, "</CoefficientValues>\n  </EnergyCalibration>\n");
        httpd_resp_send_chunk(req, acc, n);
    }

    /* #FW-5: реальные длительности строк окна (сек). calloc → незаполненные
       элементы = 0 → n42_row_emit подставит номинал. NULL (oom) тоже безопасен. */
    uint16_t *durs = calloc(s.ring_count, sizeof(uint16_t));
    if (durs) spectrogram_copy_window_durations(durs, s.ring_count);

    /* Источник — кольцо PSRAM: row служит bounce-буфером, каждая строка
       отдаётся колбэком n42_row_emit() ВНЕ лока рекордера. r0 — глобальный индекс
       первой строки окна; строки до окна уже выпали из кольца, поэтому их суммарное
       время приближаем номиналом (r0*interval), а внутри окна время честное. */
    uint32_t r0 = (s.ring_count <= s.total_rows) ? (s.total_rows - s.ring_count) : 0;
    n42_ctx_t ctx = {
        .req = req, .acc = acc,
        .interval_sec = s.interval_sec,
        .started_at = s.started_at,
        .have_cal = have_cal,
        .r = r0,
        .durs = durs,
        .r0 = r0,
        .row_start = s.started_at + (time_t)r0 * (time_t)s.interval_sec,
    };
    spectrogram_stream_window(row, s.ring_count, NULL, n42_row_emit, &ctx);

    n = snprintf(acc, 8192, "</RadInstrumentData>\n");
    httpd_resp_send_chunk(req, acc, n);
    httpd_resp_send_chunk(req, NULL, 0);

    heap_caps_free(row);
    free(acc);
    if (durs) free(durs);   /* #FW-5 */
    if (sp) free(sp);
    return ESP_OK;
}

/* ---- #REC-11-A1: листинг и отдача сегментов /storage/wf (СТРОГО read-only) ---- */

/* Строгая валидация имени сегмента — anti-traversal для /segment?name=.
   Допускается РОВНО "seg_" + ≥1 десятичная цифра + ".aswf". Любые '/', '\\',
   '.' (кроме расширения), пробелы — отвергаются. Длина 10..24
   ("seg_0.aswf" = 10; запас под многозначный монотонный индекс). */
static bool wf_seg_name_ok(const char *name)
{
    size_t len = name ? strlen(name) : 0;
    if (len < 10 || len > 24) return false;
    if (strncmp(name, "seg_", 4) != 0) return false;
    const char *suf = name + len - 5;              // позиция ".aswf"
    if (strcmp(suf, ".aswf") != 0) return false;
    if (name + 4 == suf) return false;             // должна быть ≥1 цифра
    for (const char *p = name + 4; p < suf; p++)
        if (*p < '0' || *p > '9') return false;    // между seg_ и .aswf — только цифры
    return true;
}

/* GET /api/waterfall/segments -> JSON-массив завершённых/открытых сегментов:
   [{"name","idx","bytes","rows","finalized"}]. rows вычисляется из размера
   файла (payload / WF_ROW_BYTES) — шапки несут saved_rows=0 (#FW-14,
   derive-from-size). finalized=false у СЕЙЧАС открытого сегмента (по индексу,
   не по шапке) — браузеру забирать его не нужно. */
static esp_err_t h_segments(httpd_req_t *req)
{
    const char *dir = spectrogram_seg_dir();
    uint32_t open_idx = spectrogram_seg_open_index();
    httpd_resp_set_type(req, "application/json");

    DIR *d = opendir(dir);
    if (!d) { httpd_resp_sendstr(req, "[]"); return ESP_OK; }   // каталога ещё нет

    httpd_resp_send_chunk(req, "[", 1);
    struct dirent *e;
    char path[128], line[176];
    bool first = true;
    while ((e = readdir(d)) != NULL) {
        if (!wf_seg_name_ok(e->d_name)) continue;
        // %.90s/%.30s: dir и имя короткие, но d_name по типу до 255 — даём
        // доказуемую границу для -Wformat-truncation (90+1+30+1=122 <= 128).
        snprintf(path, sizeof(path), "%.90s/%.30s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        long bytes = (long)st.st_size;
        long rows  = (bytes > WF_SEG_HEADER) ? (bytes - WF_SEG_HEADER) / WF_ROW_BYTES : 0;
        uint32_t idx = (uint32_t)strtoul(e->d_name + 4, NULL, 10);
        bool finalized = (idx != open_idx);
        int n = snprintf(line, sizeof(line),
            "%s{\"name\":\"%s\",\"idx\":%" PRIu32 ",\"bytes\":%ld,\"rows\":%ld,\"finalized\":%s}",
            first ? "" : ",", e->d_name, idx, bytes, rows, finalized ? "true" : "false");
        if (httpd_resp_send_chunk(req, line, n) != ESP_OK) { closedir(d); return ESP_FAIL; }
        first = false;
    }
    closedir(d);
    httpd_resp_send_chunk(req, "]", 1);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* GET /api/waterfall/segment?name=seg_NNNNN.aswf -> сырой файл сегмента.
   ИНВАРИАНТ #REC-11-A1: СТРОГО read-only — fopen("rb"), НИКОГДА не удаляем файл
   при отдаче (удаление сегментов — только кольцо keep-last или фаза A2-аплоадер). */
static esp_err_t h_segment(httpd_req_t *req)
{
    char query[96], name[40];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "name required");
        return ESP_FAIL;
    }
    if (!wf_seg_name_ok(name)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
        return ESP_FAIL;
    }
    char path[128];
    // %.90s/%.30s: доказуемая граница для -Wformat-truncation (name[40] валидно ≤24).
    snprintf(path, sizeof(path), "%.90s/%.30s", spectrogram_seg_dir(), name);
    FILE *f = fopen(path, "rb");      // read-only: отдаём, не трогая файл
    if (!f) { httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found"); return ESP_FAIL; }

    char *bufp = malloc(4096);
    if (!bufp) { fclose(f); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }

    char disp[80];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", name);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);

    size_t rd;
    while ((rd = fread(bufp, 1, 4096, f)) > 0) {
        if (httpd_resp_send_chunk(req, bufp, rd) != ESP_OK) { free(bufp); fclose(f); return ESP_FAIL; }
    }
    free(bufp);
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* #REC-11 pull: POST /api/waterfall/segment/delete?name=seg_NNNNN.aswf
   PC-клиент подтверждает приём сегмента → удаляем его с Flash. CSRF-protected
   (мутирующий). Имя валидируется wf_seg_name_ok (anti-traversal); удаляется ТОЛЬКО
   завершённый сегмент (не открытый, не pinned). Pull-модель: ПК сам инициирует
   соединение и забирает сегмент через GET /segment, затем этим POST освобождает
   Flash — нулевая входящая поверхность на рабочем ПК (никаких открытых портов). */
static esp_err_t h_segment_delete(httpd_req_t *req)
{
    if (!web_csrf_check(req)) return ESP_FAIL;
    char query[96], name[40];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "name required");
        return ESP_FAIL;
    }
    if (!wf_seg_name_ok(name)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
        return ESP_FAIL;
    }
    uint32_t idx = (uint32_t)strtoul(name + 4, NULL, 10);
    bool ok = spectrogram_seg_delete(idx);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, ok ? "{\"ok\":true}" : "{\"ok\":false,\"err\":\"not-deletable\"}");
    return ESP_OK;
}

static esp_err_t h_page(httpd_req_t *req)
{
    extern const uint8_t waterfall_html_start[] asm("_binary_waterfall_html_start");
    extern const uint8_t waterfall_html_end[]   asm("_binary_waterfall_html_end");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)waterfall_html_start,
                    waterfall_html_end - waterfall_html_start);
    return ESP_OK;
}

static esp_err_t h_ws(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        int fd = httpd_req_to_sockfd(req);
        ws_add(fd);
        wf_status_t s;
        spectrogram_get_status(&s);
        char hdr[512];
        int hn = snprintf(hdr, sizeof(hdr),
            "{\"type\":\"header\",\"channels\":%d,\"interval_sec\":%" PRIu32 ",\"total_rows\":%" PRIu32,
            WF_CHANNELS, s.interval_sec, s.total_rows);
        hn = append_calib_json(hdr, hn, sizeof(hdr));
        hn += snprintf(hdr + hn, sizeof(hdr) - hn, "}");
        httpd_ws_frame_t fr = { 0 };
        fr.type    = HTTPD_WS_TYPE_TEXT;
        fr.payload = (uint8_t *)hdr;
        fr.len     = hn;
        httpd_ws_send_frame(req, &fr);
        ESP_LOGI(TAG, "ws client %d connected", fd);
        return ESP_OK;
    }
    httpd_ws_frame_t fr = { 0 };
    esp_err_t r = httpd_ws_recv_frame(req, &fr, 0);
    if (r != ESP_OK) return r;
    if (fr.type == HTTPD_WS_TYPE_CLOSE) {
        ws_del(httpd_req_to_sockfd(req));
    } else if (fr.len) {
        // P3-6: вычитать кадр ЦЕЛИКОМ. httpd_ws_recv_frame с max_len < длины
        // кадра вернёт ESP_ERR_INVALID_SIZE и НЕ извлечёт payload — остаток
        // повредил бы разбор следующего кадра. UI шлёт только мелкие
        // контрол-кадры; аномально большой — повод закрыть соединение.
        if (fr.len > 512) {
            ws_del(httpd_req_to_sockfd(req));
            return ESP_OK;
        }
        uint8_t tmp[512];
        fr.payload = tmp;
        httpd_ws_recv_frame(req, &fr, fr.len);
    }
    return ESP_OK;
}

/* #REC-11-A2: GET /api/waterfall/offload — конфиг + рантайм-статистика выгрузки.
   Пароль НИКОГДА не отдаётся наружу — только флаг has_pass. */
static esp_err_t h_offload_get(httpd_req_t *req)
{
    wf_offload_cfg_t  c; wf_offload_get_cfg(&c);
    wf_offload_stat_t s; wf_offload_get_stat(&s);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject  (root, "enabled",     c.enabled);
    cJSON_AddStringToObject(root, "url",         c.url);
    cJSON_AddStringToObject(root, "user",        c.user);
    cJSON_AddBoolToObject  (root, "has_pass",    c.pass[0] != 0);
    cJSON_AddNumberToObject(root, "sent_ok",     s.sent_ok);
    cJSON_AddNumberToObject(root, "failed",      s.failed);
    cJSON_AddNumberToObject(root, "last_status", s.last_status);
    cJSON_AddNumberToObject(root, "last_ok_at",  (double)s.last_ok_at);
    cJSON_AddBoolToObject  (root, "busy",        s.busy);
    char *out = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out ? out : "{}");
    if (out) free(out);
    cJSON_Delete(root);
    return ESP_OK;
}

/* #REC-11-A2: POST /api/waterfall/offload — задать конфиг выгрузки.
   Стартуем от текущего конфига: ключ "pass" отсутствует → прежний пароль
   сохраняется (UI не пересылает пароль обратно). */
static esp_err_t h_offload_set(httpd_req_t *req)
{
    if (!web_csrf_check(req)) return ESP_FAIL;
    char body[512] = { 0 };
    int rl = httpd_req_recv(req, body, sizeof(body) - 1);
    if (rl <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty"); return ESP_FAIL; }
    body[rl] = 0;
    cJSON *root = cJSON_Parse(body);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json"); return ESP_FAIL; }

    wf_offload_cfg_t c;
    wf_offload_get_cfg(&c);                              // сохранить pass при отсутствии ключа
    cJSON *en   = cJSON_GetObjectItem(root, "enabled");
    if (cJSON_IsBool(en))     c.enabled = cJSON_IsTrue(en);
    cJSON *url  = cJSON_GetObjectItem(root, "url");
    if (cJSON_IsString(url))  snprintf(c.url,  sizeof(c.url),  "%s", url->valuestring);
    cJSON *user = cJSON_GetObjectItem(root, "user");
    if (cJSON_IsString(user)) snprintf(c.user, sizeof(c.user), "%s", user->valuestring);
    cJSON *pass = cJSON_GetObjectItem(root, "pass");
    if (cJSON_IsString(pass)) snprintf(c.pass, sizeof(c.pass), "%s", pass->valuestring);
    cJSON_Delete(root);

    int r = wf_offload_set_cfg(&c);
    httpd_resp_set_type(req, "application/json");
    if (r == WF_OFFLOAD_OK) { httpd_resp_sendstr(req, "{\"ok\":true}"); return ESP_OK; }
    const char *e = (r == WF_OFFLOAD_ERR_BLOCKED) ? "narodmon-blocked"
                  : (r == WF_OFFLOAD_ERR_INVALID) ? "invalid-url"
                  : (r == WF_OFFLOAD_ERR_NVS)     ? "nvs" : "error";
    char msg[64];
    snprintf(msg, sizeof(msg), "{\"ok\":false,\"err\":\"%s\"}", e);
    httpd_resp_sendstr(req, msg);
    return ESP_OK;
}

/* v3: GET /api/waterfall/dose_k */
static esp_err_t h_dose_k_get(httpd_req_t *req)
{
    float k = spectrogram_get_dose_k();
    char resp[48];
    snprintf(resp, sizeof(resp), "{\"dose_k\":%.8g}", (double)k);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

/* v3: POST /api/waterfall/dose_k  body: {"dose_k": <float>} */
static esp_err_t h_dose_k_set(httpd_req_t *req)
{
    if (!web_csrf_check(req)) return ESP_FAIL;
    char body[64] = {0};
    int recv_len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (recv_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[recv_len] = '\0';
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");
        return ESP_FAIL;
    }
    cJSON *it = cJSON_GetObjectItem(root, "dose_k");
    if (it && cJSON_IsNumber(it))
        spectrogram_set_dose_k((float)cJSON_GetNumberValue(it));
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static void reg(httpd_handle_t srv, const char *uri, httpd_method_t m,
                esp_err_t (*h)(httpd_req_t *))
{
    httpd_uri_t u = { .uri = uri, .method = m, .handler = h, .user_ctx = NULL };
    httpd_register_uri_handler(srv, &u);
}

void web_waterfall_register(httpd_handle_t server)
{
    s_server = server;
    if (!s_ws_mutex) s_ws_mutex = xSemaphoreCreateMutex();
    for (int i = 0; i < WF_WS_MAX; i++) { s_ws_fds[i] = 0; s_ws_inflight[i] = 0; }

    reg(server, "/waterfall",            HTTP_GET,  h_page);
    reg(server, "/api/waterfall/status", HTTP_GET,  h_status);
    reg(server, "/api/waterfall/start",  HTTP_POST, h_start);
    reg(server, "/api/waterfall/stop",   HTTP_POST, h_stop);
    reg(server, "/api/waterfall/clear",  HTTP_POST, h_clear);
    reg(server, "/api/waterfall/config", HTTP_POST, h_config);
    reg(server, "/api/waterfall/window", HTTP_GET,  h_window);
    reg(server, "/api/waterfall/export.n42", HTTP_GET, h_export_n42);
    // #REC-11-A1: листинг и отдача сегментов (СТРОГО read-only).
    reg(server, "/api/waterfall/segments", HTTP_GET, h_segments);
    reg(server, "/api/waterfall/segment",  HTTP_GET, h_segment);
    // #REC-11 pull: удаление сегмента по ack от PC-клиента (CSRF, только завершённый).
    reg(server, "/api/waterfall/segment/delete", HTTP_POST, h_segment_delete);
    // #REC-11-A2: конфиг/статус автономной выгрузки сегментов.
    reg(server, "/api/waterfall/offload",  HTTP_GET,  h_offload_get);
    reg(server, "/api/waterfall/offload",  HTTP_POST, h_offload_set);
    // v3: дозовый коэффициент
    reg(server, "/api/waterfall/dose_k",  HTTP_GET,  h_dose_k_get);
    reg(server, "/api/waterfall/dose_k",  HTTP_POST, h_dose_k_set);

    httpd_uri_t ws = {
        .uri = "/ws/waterfall", .method = HTTP_GET,
        .handler = h_ws, .user_ctx = NULL, .is_websocket = true,
    };
    httpd_register_uri_handler(server, &ws);

    spectrogram_set_row_cb(wf_broadcast);
    ESP_LOGI(TAG, "waterfall endpoints registered");
}
