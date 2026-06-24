#include "atomspectra.h"
#include "spectrogram.h"
#include "web_waterfall.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

static const char *TAG = "wf_web";

#define WF_WS_MAX  4

static httpd_handle_t s_server;
static int            s_ws_fds[WF_WS_MAX];

/* ---- WS client registry + async broadcast ---- */

static void ws_add(int fd)
{
    for (int i = 0; i < WF_WS_MAX; i++) if (s_ws_fds[i] == fd) return;
    for (int i = 0; i < WF_WS_MAX; i++) if (s_ws_fds[i] <= 0) { s_ws_fds[i] = fd; return; }
}

static void ws_del(int fd)
{
    for (int i = 0; i < WF_WS_MAX; i++) if (s_ws_fds[i] == fd) s_ws_fds[i] = 0;
}

typedef struct { int fd; size_t len; uint8_t buf[]; } ws_send_t;

static void ws_async_send(void *arg)
{
    ws_send_t *a = arg;
    httpd_ws_frame_t fr = { 0 };
    fr.type    = HTTPD_WS_TYPE_BINARY;
    fr.payload = a->buf;
    fr.len     = a->len;
    if (httpd_ws_send_frame_async(s_server, a->fd, &fr) != ESP_OK) ws_del(a->fd);
    free(a);
}

static void wf_broadcast(const uint16_t *row, size_t bytes, uint32_t idx)
{
    (void)idx;
    for (int i = 0; i < WF_WS_MAX; i++) {
        int fd = s_ws_fds[i];
        if (fd <= 0) continue;
        ws_send_t *a = malloc(sizeof(ws_send_t) + bytes);
        if (!a) continue;
        a->fd  = fd;
        a->len = bytes;
        memcpy(a->buf, row, bytes);
        if (httpd_queue_work(s_server, ws_async_send, a) != ESP_OK) free(a);
    }
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
    char buf[384];
    int n = snprintf(buf, sizeof(buf),
        "{\"recording\":%s,\"persist\":%s,\"flash_full\":%s,\"ready\":%s,"
        "\"interval_sec\":%" PRIu32 ",\"ring_capacity\":%" PRIu32 ",\"ring_count\":%" PRIu32 ","
        "\"total_rows\":%" PRIu32 ",\"flash_rows\":%" PRIu32 ",\"started_at\":%ld,\"channels\":%d}",
        s.recording ? "true" : "false", s.persist ? "true" : "false",
        s.flash_full ? "true" : "false", s.ready ? "true" : "false",
        s.interval_sec, s.ring_capacity, s.ring_count,
        s.total_rows, s.flash_rows, (long)s.started_at, WF_CHANNELS);
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

/* GET /api/waterfall/export -> self-describing file:
   "ASWF"(4) header_len(u32 LE) JSON-header payload */
static esp_err_t h_export(httpd_req_t *req)
{
    wf_status_t s;
    spectrogram_get_status(&s);
    uint32_t rows = s.flash_rows;
    FILE *f = fopen(spectrogram_data_path(), "rb");
    if (!f || rows == 0) {
        if (f) fclose(f);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no waterfall data");
        return ESP_FAIL;
    }
    char hdr[512];
    int hn = snprintf(hdr, sizeof(hdr),
        "{\"format\":\"atomspectra-waterfall\",\"version\":1,\"channels\":%d,"
        "\"dtype\":\"uint16\",\"byte_order\":\"little\",\"rows\":%" PRIu32 ","
        "\"interval_sec\":%" PRIu32 ",\"started_at\":%ld",
        WF_CHANNELS, rows, s.interval_sec, (long)s.started_at);
    hn = append_calib_json(hdr, hn, sizeof(hdr));
    hn += snprintf(hdr + hn, sizeof(hdr) - hn, "}");

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"waterfall.aswf\"");
    uint8_t pre[8];
    memcpy(pre, "ASWF", 4);
    uint32_t hl = (uint32_t)hn;
    memcpy(pre + 4, &hl, 4);
    httpd_resp_send_chunk(req, (char *)pre, 8);
    httpd_resp_send_chunk(req, hdr, hn);

    char *buf = malloc(8192);
    if (buf) {
        size_t remain = (size_t)rows * WF_ROW_BYTES;
        while (remain > 0) {
            size_t c = remain > 8192 ? 8192 : remain;
            size_t rd = fread(buf, 1, c, f);
            if (rd == 0) break;
            if (httpd_resp_send_chunk(req, buf, rd) != ESP_OK) break;
            remain -= rd;
        }
        free(buf);
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
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
} n42_ctx_t;

static bool n42_row_emit(void *vctx, const uint16_t *row, size_t bytes)
{
    n42_ctx_t *c = (n42_ctx_t *)vctx;
    (void)bytes;
    char *acc = c->acc, tbuf[40];
    struct tm tmv;
    uint32_t r = c->r++;
    time_t ts = c->started_at + (time_t)r * (time_t)c->interval_sec;
    gmtime_r(&ts, &tmv);
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%SZ", &tmv);
    int n = snprintf(acc, 8192,
        "  <RadMeasurement id=\"m-%" PRIu32 "\">\n    <MeasurementClassCode>Foreground</MeasurementClassCode>\n    <StartDateTime>%s</StartDateTime>\n    <RealTimeDuration>PT%" PRIu32 "S</RealTimeDuration>\n    <Spectrum id=\"m-%" PRIu32 "-s-1\" radDetectorInformationReference=\"det-1\"%s>\n      <LiveTimeDuration>PT%" PRIu32 "S</LiveTimeDuration>\n      <ChannelData compressionCode=\"CountedZeroes\">",
        r, tbuf, c->interval_sec, r, c->have_cal ? " energyCalibrationReference=\"ecal-1\"" : "", c->interval_sec);
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
        n = snprintf(acc, 8192,
            "    <RadInstrumentIdentifier>%s</RadInstrumentIdentifier>\n",
            sp->serial_number);
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

    /* Источник — кольцо PSRAM: row служит bounce-буфером, каждая строка
       отдаётся колбэком n42_row_emit() ВНЕ лока рекордера. */
    n42_ctx_t ctx = {
        .req = req, .acc = acc,
        .interval_sec = s.interval_sec,
        .started_at = s.started_at,
        .have_cal = have_cal,
        .r = (s.ring_count <= s.total_rows) ? (s.total_rows - s.ring_count) : 0,
    };
    spectrogram_stream_window(row, s.ring_count, NULL, n42_row_emit, &ctx);

    n = snprintf(acc, 8192, "</RadInstrumentData>\n");
    httpd_resp_send_chunk(req, acc, n);
    httpd_resp_send_chunk(req, NULL, 0);

    heap_caps_free(row);
    free(acc);
    if (sp) free(sp);
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
        uint8_t tmp[64];
        fr.payload = tmp;
        fr.len = fr.len > sizeof(tmp) ? sizeof(tmp) : fr.len;
        httpd_ws_recv_frame(req, &fr, fr.len);
    }
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
    for (int i = 0; i < WF_WS_MAX; i++) s_ws_fds[i] = 0;

    reg(server, "/waterfall",            HTTP_GET,  h_page);
    reg(server, "/api/waterfall/status", HTTP_GET,  h_status);
    reg(server, "/api/waterfall/start",  HTTP_POST, h_start);
    reg(server, "/api/waterfall/stop",   HTTP_POST, h_stop);
    reg(server, "/api/waterfall/clear",  HTTP_POST, h_clear);
    reg(server, "/api/waterfall/config", HTTP_POST, h_config);
    reg(server, "/api/waterfall/window", HTTP_GET,  h_window);
    reg(server, "/api/waterfall/export", HTTP_GET,  h_export);
    reg(server, "/api/waterfall/export.n42", HTTP_GET, h_export_n42);

    httpd_uri_t ws = {
        .uri = "/ws/waterfall", .method = HTTP_GET,
        .handler = h_ws, .user_ctx = NULL, .is_websocket = true,
    };
    httpd_register_uri_handler(server, &ws);

    spectrogram_set_row_cb(wf_broadcast);
    ESP_LOGI(TAG, "waterfall endpoints registered");
}
