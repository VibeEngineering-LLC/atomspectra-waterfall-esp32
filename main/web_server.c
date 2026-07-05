#include "atomspectra.h"
#include "shproto.h"
#include "web_waterfall.h"
#include "web_util.h"
#include "boot_config.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include <time.h>
#include <stddef.h>
#include <math.h>
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_littlefs.h"
#include "esp_random.h"
#include <dirent.h>

static const char *TAG = "web";

// CSRF-токен: генерируется при старте, выдаётся по GET /api/csrf-token,
// требуется в заголовке X-CSRF-Token на всех мутирующих POST. Защищает
// открытый-в-LAN Web UI от drive-by cross-origin POST: сторонняя страница
// не может прочитать токен (same-origin policy) → не может подделать запрос.
static char s_csrf[33];

static void csrf_generate(void)
{
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++)
        s_csrf[i] = hx[esp_random() & 0x0F];
    s_csrf[32] = '\0';
}

// P3-9: сравнение токена за постоянное время — не утекает позиция
// первого несовпавшего байта по времени ответа.
static bool csrf_ct_eq(const char *a, const char *b, size_t n)
{
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= (uint8_t)a[i] ^ (uint8_t)b[i];
    return d == 0;
}

// Проверяет X-CSRF-Token. При несовпадении сам шлёт 403 и возвращает false.
static bool csrf_check(httpd_req_t *req)
{
    char hdr[40] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-CSRF-Token", hdr, sizeof(hdr)) == ESP_OK
        && s_csrf[0] && strlen(hdr) == 32 && csrf_ct_eq(hdr, s_csrf, 32))
        return true;
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Bad or missing CSRF token");
    return false;
}

// Экспорт CSRF-проверки для web_waterfall.c (csrf_check здесь static).
bool web_csrf_check(httpd_req_t *req)
{
    return csrf_check(req);
}

static esp_err_t handle_csrf_token(httpd_req_t *req)
{
    char buf[48];
    int n = snprintf(buf, sizeof(buf), "{\"token\":\"%s\"}", s_csrf);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

static int parse_saved_index(const char *uri)
{
    const char *p = strstr(uri, "/api/saved/");
    if (!p) return -1;
    return atoi(p + 11);
}

static esp_err_t handle_status(httpd_req_t *req)
{
    const spectrum_data_t *sp = spectrum_get_current();
    const device_info_t   *di = spectrum_get_device_info();

    cJSON *root = cJSON_CreateObject();
    if (!root) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
    cJSON_AddBoolToObject(root, "analyzer_connected", usb_host_cdc_is_connected());
    cJSON_AddBoolToObject(root, "wifi_connected", wifi_is_connected());
    cJSON_AddNumberToObject(root, "total_counts", sp->total_counts);
    cJSON_AddNumberToObject(root, "cpu_load", sp->cpu_load);
    cJSON_AddBoolToObject(root, "tcp_client", tcp_bridge_client_connected());

    if (di->valid) {
        cJSON_AddNumberToObject(root, "dev", di->dev);
        cJSON_AddNumberToObject(root, "version", di->version);
        cJSON_AddNumberToObject(root, "mode", di->mode);
        cJSON_AddNumberToObject(root, "freq", di->freq);
        cJSON_AddNumberToObject(root, "t1", di->t1);
        cJSON_AddNumberToObject(root, "t2", di->t2);
        cJSON_AddNumberToObject(root, "t3", di->t3);
        cJSON_AddNumberToObject(root, "time", di->time_sec);
        cJSON_AddNumberToObject(root, "noise", di->noise);
        cJSON_AddNumberToObject(root, "max", di->max_integral);
    }

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t handle_spectrum(httpd_req_t *req)
{
    spectrum_data_t *sp = malloc(sizeof(*sp));
    if (!sp) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    if (!spectrum_get_snapshot(sp)) {
        free(sp);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No spectrum data yet");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline");
    httpd_resp_send(req, (const char *)sp->bins, SPECTRUM_CHANNELS * sizeof(uint32_t));
    free(sp);
    return ESP_OK;
}

// #DT-4: мёртвое время берём ИЗ ДЕТЕКТОРА. STAT-кадр (cmd 0x04) несёт pulse_width
// [offset 14, u32] = суммарная ширина импульсов (отсчёты АЦП) — реально измеренное прибором
// занятое время (учитывает наложения). dead = pulse_width/F; live = total - dead.
// Фоллбэк (#DT-1, модель total_counts*(RISE+FALL+Srise+Sfall)/F) — для прошивок без pulse_width
// (kADR<18 байт → pulse_width==0) или пока -inf не прочитан / F<=0.
static float compute_live_time(const spectrum_data_t *sp)
{
    const device_info_t *di = spectrum_get_device_info();
    float total = (float)sp->total_time_sec;
    if (!di->valid || di->freq <= 0.0f) return total;
    // #DT-4: метод BecqMoni (эталон Am6er, Utils/LiveTime.cs + AtomSpectraVCPDeviceForm.cs).
    // Мёртвое время на импульс τ = (RISE+FALL+1)/F (RISE/FALL/F из -inf, т.е. из прибора).
    // Мёртвое время за набор = (valid+invalid импульсы)·τ; TotalPulseCount = sum + InvalidPulses.
    float tau = ((float)di->rise + (float)di->fall + 1.0f) / di->freq;
    float dead = (float)(sp->total_counts + sp->lost_impulses) * tau;
    if (dead < 0.0f) dead = 0.0f;
    if (dead > total) dead = total;
    return total - dead;
}

static esp_err_t render_spectrum_json(httpd_req_t *req, const spectrum_data_t *sp)
{
    char *buf = malloc(4096);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{\"bins\":[");
    int pos = 0;
    for (int i = 0; i < SPECTRUM_CHANNELS; i++) {
        pos += snprintf(buf + pos, 4096 - pos, "%s%" PRIu32, (i > 0) ? "," : "", sp->bins[i]);
        if (pos > 3800) {
            httpd_resp_send_chunk(req, buf, pos);
            pos = 0;
        }
    }
    if (pos > 0) httpd_resp_send_chunk(req, buf, pos);
    uint32_t hist_ok = 0, hist_drop = 0;
    spectrum_get_hist_stats(&hist_ok, &hist_drop);
    int n = snprintf(buf, 4096,
        "],\"total\":%" PRIu32 ",\"cpu\":%u,\"cps\":%" PRIu32 ",\"lost\":%" PRIu32 ",\"time\":%" PRIu32 ",\"live\":%.1f,"
        "\"bridge_drop\":%" PRIu32 ",\"usb_rx_err\":%" PRIu32 ","
        "\"hist_ok\":%" PRIu32 ",\"hist_drop\":%" PRIu32 ","
        "\"t1\":%.1f,\"t2\":%.1f,\"t3\":%.1f,\"serial\":\"%s\"",
        sp->total_counts, sp->cpu_load, sp->cps, sp->lost_impulses,
        sp->total_time_sec, compute_live_time(sp),
        tcp_bridge_dropped_bytes(), usb_host_cdc_rx_errors(),
        hist_ok, hist_drop,
        sp->temperature[0], sp->temperature[1], sp->temperature[2],
        sp->serial_number[0] ? sp->serial_number : "");
    httpd_resp_send_chunk(req, buf, n);
    if (sp->calib_valid) {
        int p = snprintf(buf, 4096, ",\"calib\":[");
        for (int i = 0; i <= sp->calib_order; i++)
            p += snprintf(buf + p, 4096 - p, "%s%.15g", i ? "," : "", sp->calibration[i]);
        snprintf(buf + p, 4096 - p, "]");
        httpd_resp_sendstr_chunk(req, buf);
    }
    httpd_resp_sendstr_chunk(req, "}");
    httpd_resp_send_chunk(req, NULL, 0);
    free(buf);
    return ESP_OK;
}

static esp_err_t handle_spectrum_json(httpd_req_t *req)
{
    spectrum_data_t *sp = malloc(sizeof(*sp));
    if (!sp) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    if (!spectrum_get_snapshot(sp)) {
        free(sp);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No spectrum data yet");
        return ESP_FAIL;
    }
    esp_err_t ret = render_spectrum_json(req, sp);
    free(sp);
    return ret;
}

static esp_err_t handle_command(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;
    char body[256] = {0};
    int recv_len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (recv_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[recv_len] = '\0';
    uint8_t pkt_buf[512];
    shproto_struct pkt;
    shproto_init(&pkt, pkt_buf, sizeof(pkt_buf));
    shproto_packet_start(&pkt, CMD_TEXT);
    for (int i = 0; i <= recv_len; i++)
        shproto_packet_add_data(&pkt, body[i]);
    shproto_packet_complete(&pkt);
    int ret = usb_host_cdc_send(pkt.data, pkt.len);
    httpd_resp_sendstr(req, ret == 0 ? "{\"ok\":true}" : "{\"ok\":false}");
    return ESP_OK;
}

// #UI-1: лог текстовых ответов прибора для веб-UI. GET /api/devlog?since=N
// Read-only (CSRF не нужен). Отдаёт {"lines":[{"seq":N,"text":"..."}],"next":M}.
static esp_err_t handle_devlog(httpd_req_t *req)
{
    uint32_t since = 0;
    char q[48], v[16];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "since", v, sizeof(v)) == ESP_OK)
        since = strtoul(v, NULL, 10);
    char *buf = malloc(9000);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    usb_host_cdc_devlog_json(since, buf, 9000);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    free(buf);
    return ESP_OK;
}

static esp_err_t handle_reset(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;
    uint8_t pkt_buf[64];
    shproto_struct pkt;
    shproto_init(&pkt, pkt_buf, sizeof(pkt_buf));
    shproto_packet_start(&pkt, CMD_TEXT);
    const char *cmd = "-rst";
    for (int i = 0; cmd[i]; i++) shproto_packet_add_data(&pkt, cmd[i]);
    shproto_packet_add_data(&pkt, '\0');
    shproto_packet_complete(&pkt);
    usb_host_cdc_send(pkt.data, pkt.len);
    spectrum_reset();
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// #FW-2/#FW-3: GET текущих настроек «Поведение при старте платы» (NVS).
static esp_err_t handle_boot_config_get(httpd_req_t *req)
{
    boot_config_t bc;
    boot_config_load(&bc);
    char resp[200];
    snprintf(resp, sizeof(resp),
        "{\"autostart_spectrum\":%s,\"autostart_waterfall\":%s,"
        "\"clear_spectrum\":%s,\"clear_waterfall\":%s}",
        bc.autostart_spectrum  ? "true" : "false",
        bc.autostart_waterfall ? "true" : "false",
        bc.clear_spectrum      ? "true" : "false",
        bc.clear_waterfall     ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

// #FW-2/#FW-3: POST настроек. Отсутствующие в теле ключи сохраняют текущее значение.
static esp_err_t handle_boot_config_set(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;
    char body[256] = {0};
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
    boot_config_t bc;
    boot_config_load(&bc);   // стартуем от текущего, перекрываем переданными ключами
    cJSON *it;
    if ((it = cJSON_GetObjectItem(root, "autostart_spectrum")))  bc.autostart_spectrum  = cJSON_IsTrue(it);
    if ((it = cJSON_GetObjectItem(root, "autostart_waterfall"))) bc.autostart_waterfall = cJSON_IsTrue(it);
    if ((it = cJSON_GetObjectItem(root, "clear_spectrum")))      bc.clear_spectrum      = cJSON_IsTrue(it);
    if ((it = cJSON_GetObjectItem(root, "clear_waterfall")))     bc.clear_waterfall     = cJSON_IsTrue(it);
    cJSON_Delete(root);
    int rc = boot_config_save(&bc);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, rc == 0 ? "{\"ok\":true}" : "{\"ok\":false}");
    return ESP_OK;
}

static esp_err_t handle_save(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;
    int idx = spectrum_save_to_flash();
    char resp[64];
    if (idx >= 0)
        snprintf(resp, sizeof(resp), "{\"ok\":true,\"index\":%d}", idx);
    else
        snprintf(resp, sizeof(resp), "{\"ok\":false}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t handle_list(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{\"spectra\":[");
    char path[80], item[160];
    int count = 0;
    // Перечисляем реальные файлы каталога, а не пробуем индексы подряд:
    // save() занимает первую дырку, поэтому после удалений индексы не
    // обязательно идут без разрывов — старый "стоп после 20 пропусков" терял
    // спектры с большими индексами. readdir видит все файлы.
    DIR *dir = opendir(STORAGE_PATH);
    struct dirent *de;
    while (dir && (de = readdir(dir)) != NULL) {
        int i = -1;
        if (sscanf(de->d_name, "spec_%d.bin", &i) != 1 || i < 0) continue;
        // Путь восстанавливаем из разобранного индекса (%d ограничен), а не из
        // d_name (%s до 255 байт) — иначе -Werror=format-truncation.
        snprintf(path, sizeof(path), "%s/spec_%d.bin", STORAGE_PATH, i);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        uint32_t counts = 0, time_sec = 0;
        time_t saved_at = 0;
        fseek(f, offsetof(spectrum_data_t, total_counts), SEEK_SET);
        fread(&counts, 4, 1, f);
        fread(&time_sec, 4, 1, f);
        fseek(f, offsetof(spectrum_data_t, saved_at), SEEK_SET);
        fread(&saved_at, sizeof(time_t), 1, f);
        fclose(f);
        int n = snprintf(item, sizeof(item), "%s{\"index\":%d,\"counts\":%" PRIu32 ",\"time\":%" PRIu32 ",\"saved_at\":%ld}",
            count > 0 ? "," : "", i, counts, time_sec, (long)saved_at);
        httpd_resp_send_chunk(req, item, n);
        count++;
    }
    if (dir) closedir(dir);
    char tail[32];
    int n = snprintf(tail, sizeof(tail), "],\"count\":%d}", count);
    httpd_resp_send_chunk(req, tail, n);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

#define EMBED_HTML_HANDLER(fn,sym) static esp_err_t fn(httpd_req_t *req){ \
    extern const uint8_t sym##_start[] asm("_binary_" #sym "_start"); \
    extern const uint8_t sym##_end[]   asm("_binary_" #sym "_end"); \
    httpd_resp_set_type(req,"text/html"); \
    httpd_resp_send(req,(const char *)sym##_start, sym##_end - sym##_start); \
    return ESP_OK; }
EMBED_HTML_HANDLER(handle_index,        index_html)
EMBED_HTML_HANDLER(handle_saved_page,   saved_html)
EMBED_HTML_HANDLER(handle_system_page,  system_html)
EMBED_HTML_HANDLER(handle_service_page, service_html)

static esp_err_t render_spectrum_xml(httpd_req_t *req, const spectrum_data_t *sp, const char *filename)
{
    char *buf = malloc(4096);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/xml");
    char disp[80];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", filename);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);

    httpd_resp_sendstr_chunk(req,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
        "<ResultDataFile xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\""
        " xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">\r\n"
        "  <FormatVersion>120920</FormatVersion>\r\n"
        "  <ResultDataList>\r\n"
        "    <ResultData>\r\n");

    time_t end_time = (sp->saved_at > 0) ? sp->saved_at : time(NULL);
    struct tm ts, te;
    time_t t_start = end_time - sp->total_time_sec;
    localtime_r(&t_start, &ts);
    localtime_r(&end_time, &te);

    char serial_esc[160];
    web_xml_escape(sp->serial_number[0] ? sp->serial_number : "AtomSpectra",
               serial_esc, sizeof(serial_esc));
    int n = snprintf(buf, 4096,
        "      <SampleInfo>\r\n"
        "        <Name>%s</Name>\r\n"
        "        <Location />\r\n"
        "        <Time>%04d-%02d-%02dT%02d:%02d:%02d</Time>\r\n"
        "        <Weight>1</Weight>\r\n"
        "        <Volume>1</Volume>\r\n"
        "        <Note />\r\n"
        "      </SampleInfo>\r\n"
        "      <DeviceConfigReference>\r\n"
        "        <Name>Atom Spectra</Name>\r\n"
        "        <Guid>00000000-0000-0000-0000-000000000000</Guid>\r\n"
        "      </DeviceConfigReference>\r\n"
        "      <StartTime>%04d-%02d-%02dT%02d:%02d:%02d</StartTime>\r\n"
        "      <EndTime>%04d-%02d-%02dT%02d:%02d:%02d</EndTime>\r\n"
        "      <PresetTime>0</PresetTime>\r\n",
        serial_esc,
        ts.tm_year+1900, ts.tm_mon+1, ts.tm_mday, ts.tm_hour, ts.tm_min, ts.tm_sec,
        ts.tm_year+1900, ts.tm_mon+1, ts.tm_mday, ts.tm_hour, ts.tm_min, ts.tm_sec,
        te.tm_year+1900, te.tm_mon+1, te.tm_mday, te.tm_hour, te.tm_min, te.tm_sec);
    httpd_resp_send_chunk(req, buf, n);

    n = snprintf(buf, 4096,
        "      <EnergySpectrum>\r\n"
        "        <NumberOfChannels>%d</NumberOfChannels>\r\n"
        "        <ChannelPitch>1</ChannelPitch>\r\n",
        SPECTRUM_CHANNELS);
    httpd_resp_send_chunk(req, buf, n);

    if (sp->calib_valid) {
        n = snprintf(buf, 4096,
            "        <EnergyCalibration>\r\n"
            "          <PolynomialOrder>%d</PolynomialOrder>\r\n"
            "          <Coefficients>\r\n",
            sp->calib_order);
        httpd_resp_send_chunk(req, buf, n);
        for (int i = 0; i <= sp->calib_order; i++) {
            n = snprintf(buf, 4096,
                "            <Coefficient>%.15G</Coefficient>\r\n",
                sp->calibration[i]);
            httpd_resp_send_chunk(req, buf, n);
        }
        httpd_resp_sendstr_chunk(req,
            "          </Coefficients>\r\n"
            "        </EnergyCalibration>\r\n");
    }

    float live_time = compute_live_time(sp);

    n = snprintf(buf, 4096,
        "        <ValidPulseCount>%" PRIu32 "</ValidPulseCount>\r\n"
        "        <TotalPulseCount>%" PRIu32 "</TotalPulseCount>\r\n"
        "        <MeasurementTime>%" PRIu32 "</MeasurementTime>\r\n"
        "        <LiveTime>%.1f</LiveTime>\r\n"
        "        <NumberOfSamples>1</NumberOfSamples>\r\n"
        "        <Spectrum>\r\n",
        sp->total_counts,
        sp->total_counts + sp->lost_impulses,
        sp->total_time_sec, live_time);
    httpd_resp_send_chunk(req, buf, n);

    for (int i = 0; i < SPECTRUM_CHANNELS; ) {
        int pos = 0;
        for (int j = 0; j < 80 && i < SPECTRUM_CHANNELS; j++, i++) {
            pos += snprintf(buf + pos, 4096 - pos,
                "          <DataPoint>%" PRIu32 "</DataPoint>\r\n", sp->bins[i]);
        }
        httpd_resp_send_chunk(req, buf, pos);
    }

    httpd_resp_sendstr_chunk(req,
        "        </Spectrum>\r\n"
        "      </EnergySpectrum>\r\n"
        "      <PulseCollection>\r\n"
        "        <Format>Base64 encoded binary</Format>\r\n"
        "        <Pulses />\r\n"
        "      </PulseCollection>\r\n"
        "    </ResultData>\r\n"
        "  </ResultDataList>\r\n"
        "</ResultDataFile>\r\n");

    httpd_resp_send_chunk(req, NULL, 0);
    free(buf);
    return ESP_OK;
}

static esp_err_t render_spectrum_csv(httpd_req_t *req, const spectrum_data_t *sp, const char *filename)
{
    char *buf = malloc(4096);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/csv");
    char disp[80];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", filename);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);

    // #CSV-1 (2026-06-28): нативный формат Atom Spectra. Эталон оператора —
    // "spectrum (10).csv": одна строка-заголовок "Channel,Counts (TotalTime=<realtime>.0s)",
    // далее "канал,счёт" с канала 0 (не 1), без пробела после запятой, окончания CRLF,
    // 8192 канала. TotalTime = реальное (измерительное) время total_time_sec, формат %.1f.
    // Перекрывает прежний InterSpec/SpecUtils-заголовок #PR-2 по явному указанию оператора.
    int n = snprintf(buf, 4096,
        "Channel,Counts (TotalTime=%.1fs)\r\n", (double)sp->total_time_sec);
    httpd_resp_send_chunk(req, buf, n);

    for (int i = 0; i < SPECTRUM_CHANNELS; ) {
        int pos = 0;
        for (int j = 0; j < 200 && i < SPECTRUM_CHANNELS; j++, i++) {
            pos += snprintf(buf + pos, 4096 - pos, "%d,%" PRIu32 "\r\n", i, sp->bins[i]);
        }
        httpd_resp_send_chunk(req, buf, pos);
    }

    httpd_resp_send_chunk(req, NULL, 0);
    free(buf);
    return ESP_OK;
}

// #EXP-1 (2026-06-28): нативный N42-2011 экспорт. Эталон оператора — "spectrum (10).N42"
// (родная выгрузка Atom Spectra через BecqMoni). Байт-точно: UTF-8 BOM, CRLF, БЕЗ финального
// перевода строки, отступы 2 пробела, блок RadInstrumentInformation дословно, CoefficientValues
// и ChannelData с замыкающим пробелом. Значения — живые из снимка спектра.
static esp_err_t render_spectrum_n42(httpd_req_t *req, const spectrum_data_t *sp, const char *filename)
{
    char *buf = malloc(4096);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/octet-stream");
    char disp[80];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", filename);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    uint32_t r0 = esp_random(), r1 = esp_random(), r2 = esp_random(), r3 = esp_random();
    char uuid[40];
    snprintf(uuid, sizeof(uuid), "%08lx-%04lx-%04lx-%04lx-%04lx%08lx",
             (unsigned long)r0,
             (unsigned long)(r1 >> 16), (unsigned long)(r1 & 0xffff),
             (unsigned long)(r2 >> 16), (unsigned long)(r2 & 0xffff),
             (unsigned long)r3);
    int n = snprintf(buf, 4096,
        "\xEF\xBB\xBF"
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
        "<RadInstrumentData xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\""
        " xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\""
        " n42DocUUID=\"%s\" xmlns=\"http://physics.nist.gov/N42/2011/N42\">\r\n",
        uuid);
    httpd_resp_send_chunk(req, buf, n);
    httpd_resp_sendstr_chunk(req,
        "  <RadInstrumentInformation id=\"RadInstrument\">\r\n"
        "    <RadInstrumentManufacturerName>KB Radar</RadInstrumentManufacturerName>\r\n"
        "    <RadInstrumentModelName>ATOM Spectra</RadInstrumentModelName>\r\n"
        "    <RadInstrumentClassCode>Radionuclide Identifier</RadInstrumentClassCode>\r\n"
        "    <RadInstrumentVersion>\r\n"
        "      <RadInstrumentComponentName>Hardware</RadInstrumentComponentName>\r\n"
        "      <RadInstrumentComponentVersion />\r\n"
        "    </RadInstrumentVersion>\r\n"
        "    <RadInstrumentVersion>\r\n"
        "      <RadInstrumentComponentName>SoftwareName</RadInstrumentComponentName>\r\n"
        "      <RadInstrumentComponentVersion>BecqMoni</RadInstrumentComponentVersion>\r\n"
        "    </RadInstrumentVersion>\r\n"
        "    <RadInstrumentVersion>\r\n"
        "      <RadInstrumentComponentName>Software</RadInstrumentComponentName>\r\n"
        "      <RadInstrumentComponentVersion>2026.6.15.1</RadInstrumentComponentVersion>\r\n"
        "    </RadInstrumentVersion>\r\n"
        "  </RadInstrumentInformation>\r\n");
    if (sp->calib_valid) {
        httpd_resp_sendstr_chunk(req,
            "  <EnergyCalibration id=\"SpectrumCalibration-0\">\r\n"
            "    <CoefficientValues>");
        int cpos = 0;
        for (int i = 0; i <= sp->calib_order; i++)
            cpos += snprintf(buf + cpos, 4096 - cpos, "%.15G ", sp->calibration[i]);
        httpd_resp_send_chunk(req, buf, cpos);
        httpd_resp_sendstr_chunk(req,
            "</CoefficientValues>\r\n"
            "  </EnergyCalibration>\r\n");
    }
    time_t end_time = (sp->saved_at > 0) ? sp->saved_at : time(NULL);
    struct tm ts;
    time_t t_start = end_time - sp->total_time_sec;
    localtime_r(&t_start, &ts);
    n = snprintf(buf, 4096,
        "  <RadMeasurement id=\"SpectrumMeasurement-0\">\r\n"
        "    <MeasurementClassCode>Foreground</MeasurementClassCode>\r\n"
        "    <StartDateTime>%02d.%02d.%04d %02d:%02d:%02d</StartDateTime>\r\n"
        "    <RealTimeDuration>PT%" PRIu32 "S</RealTimeDuration>\r\n",
        ts.tm_mday, ts.tm_mon + 1, ts.tm_year + 1900, ts.tm_hour, ts.tm_min, ts.tm_sec,
        sp->total_time_sec);
    httpd_resp_send_chunk(req, buf, n);
    if (sp->calib_valid) {
        httpd_resp_sendstr_chunk(req,
            "    <Spectrum id=\"SpectrumData\" radDetectorInformationReference=\"Detector\""
            " energyCalibrationReference=\"SpectrumCalibration-0\">\r\n");
    } else {
        httpd_resp_sendstr_chunk(req,
            "    <Spectrum id=\"SpectrumData\" radDetectorInformationReference=\"Detector\">\r\n");
    }
    n = snprintf(buf, 4096,
        "      <LiveTimeDuration>PT%.1fS</LiveTimeDuration>\r\n"
        "      <ChannelData compressionCode=\"None\">",
        compute_live_time(sp));
    httpd_resp_send_chunk(req, buf, n);
    for (int i = 0; i < SPECTRUM_CHANNELS; ) {
        int pos = 0;
        for (int j = 0; j < 80 && i < SPECTRUM_CHANNELS; j++, i++)
            pos += snprintf(buf + pos, 4096 - pos, "%" PRIu32 " ", sp->bins[i]);
        httpd_resp_send_chunk(req, buf, pos);
    }
    n = snprintf(buf, 4096,
        "</ChannelData>\r\n"
        "    </Spectrum>\r\n"
        "    <GrossCounts id=\"GrossForeground\" radDetectorInformationReference=\"Detector\">\r\n"
        "      <TotalCounts>%" PRIu32 "</TotalCounts>\r\n"
        "    </GrossCounts>\r\n"
        "  </RadMeasurement>\r\n"
        "</RadInstrumentData>",
        sp->total_counts + sp->lost_impulses);
    httpd_resp_send_chunk(req, buf, n);
    httpd_resp_send_chunk(req, NULL, 0);
    free(buf);
    return ESP_OK;
}

// #EXP-2 (2026-06-28): экспорт ЛСРМ SpectraLine *.spe (бинарный вариант), как у SpectraVibe
// (gamma.io.lsrm_spe.write_lsrm_spe): CP-1251 заголовок KEY=VALUE\r\n, маркер "SPECTR=", затем
// бинарный блок счётов uint32 little-endian (по каналу, без разделителей). Значения ASCII.
static esp_err_t render_spectrum_spe(httpd_req_t *req, const spectrum_data_t *sp, const char *filename)
{
    char *buf = malloc(4096);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/octet-stream");
    char disp[80];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", filename);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    time_t end_time = (sp->saved_at > 0) ? sp->saved_at : time(NULL);
    struct tm ts;
    time_t t_start = end_time - sp->total_time_sec;
    localtime_r(&t_start, &ts);
    int pos = snprintf(buf, 4096,
        "SHIFR=%s\r\n"
        "CONFIGNAME=Atom Spectra\r\n"
        "MEASBEGIN=%02d-%02d-%02d %02d:%02d:%02d.00\r\n"
        "TLIVE=%.2f\r\n"
        "TREAL=%.2f\r\n"
        "DETECTOR=Atom Spectra\r\n",
        sp->serial_number[0] ? sp->serial_number : "AtomSpectra",
        ts.tm_mday, ts.tm_mon + 1, (ts.tm_year + 1900) % 100,
        ts.tm_hour, ts.tm_min, ts.tm_sec,
        compute_live_time(sp), (double)sp->total_time_sec);
    if (sp->calib_valid) {
        double emax = 0.0;
        for (int i = sp->calib_order; i >= 0; i--)
            emax = emax * (SPECTRUM_CHANNELS - 1) + sp->calibration[i];
        if (emax < 0.0) emax = 0.0;
        pos += snprintf(buf + pos, 4096 - pos, "ENBOUNDS=0,%d\r\n", (int)(emax + 0.5));
        pos += snprintf(buf + pos, 4096 - pos, "ENERGY=%d", sp->calib_order);
        for (int i = 0; i < 7; i++) {
            double c = (i <= sp->calib_order) ? sp->calibration[i] : 0.0;
            pos += snprintf(buf + pos, 4096 - pos, ",%.10g", c);
        }
        pos += snprintf(buf + pos, 4096 - pos, "\r\n");
    }
    pos += snprintf(buf + pos, 4096 - pos, "SPECTRSIZE=%d\r\nSPECTR=", SPECTRUM_CHANNELS);
    httpd_resp_send_chunk(req, buf, pos);
    for (int i = 0; i < SPECTRUM_CHANNELS; ) {
        int cnt = SPECTRUM_CHANNELS - i;
        if (cnt > 1024) cnt = 1024;
        httpd_resp_send_chunk(req, (const char *)&sp->bins[i], cnt * 4);
        i += cnt;
    }
    httpd_resp_send_chunk(req, NULL, 0);
    free(buf);
    return ESP_OK;
}

static esp_err_t handle_export_xml(httpd_req_t *req)
{
    spectrum_data_t *sp = malloc(sizeof(*sp));
    if (!sp) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    if (!spectrum_get_snapshot(sp)) {
        free(sp);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No spectrum data");
        return ESP_FAIL;
    }
    esp_err_t ret = render_spectrum_xml(req, sp, "spectrum.xml");
    free(sp);
    return ret;
}

static esp_err_t handle_export_spe(httpd_req_t *req)
{
    spectrum_data_t *sp = malloc(sizeof(*sp));
    if (!sp) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    if (!spectrum_get_snapshot(sp)) {
        free(sp);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No spectrum data");
        return ESP_FAIL;
    }
    esp_err_t ret = render_spectrum_spe(req, sp, "spectrum.spe");
    free(sp);
    return ret;
}

static esp_err_t handle_export_n42(httpd_req_t *req)
{
    spectrum_data_t *sp = malloc(sizeof(*sp));
    if (!sp) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    if (!spectrum_get_snapshot(sp)) {
        free(sp);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No spectrum data");
        return ESP_FAIL;
    }
    esp_err_t ret = render_spectrum_n42(req, sp, "spectrum.n42");
    free(sp);
    return ret;
}

static esp_err_t handle_export_csv(httpd_req_t *req)
{
    spectrum_data_t *sp = malloc(sizeof(*sp));
    if (!sp) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    if (!spectrum_get_snapshot(sp)) {
        free(sp);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No spectrum data");
        return ESP_FAIL;
    }
    esp_err_t ret = render_spectrum_csv(req, sp, "spectrum.csv");
    free(sp);
    return ret;
}

static esp_err_t handle_saved_export_xml(httpd_req_t *req);
static esp_err_t handle_saved_export_csv(httpd_req_t *req);
static esp_err_t handle_saved_json(httpd_req_t *req);

static esp_err_t handle_saved_get(httpd_req_t *req)
{
    const char *p = req->uri + 11;
    while (*p >= '0' && *p <= '9') p++;
    if (strcmp(p, "/export.xml") == 0) return handle_saved_export_xml(req);
    if (strcmp(p, "/export.csv") == 0) return handle_saved_export_csv(req);
    if (strcmp(p, "/spectrum.json") == 0) return handle_saved_json(req);
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Unknown saved action");
    return ESP_FAIL;
}

static esp_err_t handle_saved_export_xml(httpd_req_t *req)
{
    int idx = parse_saved_index(req->uri);
    if (idx < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad index");
        return ESP_FAIL;
    }
    spectrum_data_t *sp = malloc(sizeof(spectrum_data_t));
    if (!sp) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    if (spectrum_load_from_flash(idx, sp) != 0) {
        free(sp);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Spectrum not found");
        return ESP_FAIL;
    }
    char fn[32];
    snprintf(fn, sizeof(fn), "spectrum_%04d.xml", idx);
    esp_err_t ret = render_spectrum_xml(req, sp, fn);
    free(sp);
    return ret;
}

static esp_err_t handle_saved_export_csv(httpd_req_t *req)
{
    int idx = parse_saved_index(req->uri);
    if (idx < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad index");
        return ESP_FAIL;
    }
    spectrum_data_t *sp = malloc(sizeof(spectrum_data_t));
    if (!sp) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    if (spectrum_load_from_flash(idx, sp) != 0) {
        free(sp);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Spectrum not found");
        return ESP_FAIL;
    }
    char fn[32];
    snprintf(fn, sizeof(fn), "spectrum_%04d.csv", idx);
    esp_err_t ret = render_spectrum_csv(req, sp, fn);
    free(sp);
    return ret;
}

static esp_err_t handle_saved_json(httpd_req_t *req)
{
    int idx = parse_saved_index(req->uri);
    if (idx < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad index");
        return ESP_FAIL;
    }
    spectrum_data_t *sp = malloc(sizeof(spectrum_data_t));
    if (!sp) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    if (spectrum_load_from_flash(idx, sp) != 0) {
        free(sp);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Spectrum not found");
        return ESP_FAIL;
    }
    esp_err_t ret = render_spectrum_json(req, sp);
    free(sp);
    return ret;
}

static esp_err_t handle_saved_delete(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;
    int idx = parse_saved_index(req->uri);
    if (idx < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad index");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    if (spectrum_delete_from_flash(idx) == 0)
        httpd_resp_sendstr(req, "{\"ok\":true}");
    else
        httpd_resp_sendstr(req, "{\"ok\":false}");
    return ESP_OK;
}

static esp_err_t handle_device(httpd_req_t *req)
{
    const device_info_t *di = spectrum_get_device_info();
    // Снимок под локом: serial/calibration пишутся CDC-задачей конкурентно.
    spectrum_data_t *sp = malloc(sizeof(*sp));
    bool have_sp = sp && spectrum_get_snapshot(sp);
    cJSON *root = cJSON_CreateObject();
    if (!root) {  // P2-7: при OOM cJSON_Add* разыменует NULL
        free(sp);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    cJSON_AddBoolToObject(root, "valid", di->valid);
    if (di->valid) {
        cJSON_AddNumberToObject(root, "dev", di->dev);
        cJSON_AddNumberToObject(root, "version", di->version);
        cJSON_AddNumberToObject(root, "rise", di->rise);
        cJSON_AddNumberToObject(root, "fall", di->fall);
        cJSON_AddNumberToObject(root, "noise", di->noise);
        cJSON_AddNumberToObject(root, "freq", di->freq);
        cJSON_AddNumberToObject(root, "max_integral", di->max_integral);
        cJSON_AddNumberToObject(root, "hyst", di->hyst);
        cJSON_AddNumberToObject(root, "mode", di->mode);
        cJSON_AddNumberToObject(root, "step", di->step);
        cJSON_AddNumberToObject(root, "time", di->time_sec);
        cJSON_AddNumberToObject(root, "pot", di->pot);
        cJSON_AddNumberToObject(root, "t1", di->t1);
        cJSON_AddNumberToObject(root, "t2", di->t2);
        cJSON_AddNumberToObject(root, "t3", di->t3);
        cJSON_AddBoolToObject(root, "tc_on", di->tc_on);
        cJSON_AddNumberToObject(root, "tp", di->tp);
    }
    if (have_sp && sp->serial_number[0])
        cJSON_AddStringToObject(root, "serial", sp->serial_number);
    if (have_sp && sp->calib_valid) {
        cJSON *cal = cJSON_CreateArray();
        for (int i = 0; i <= sp->calib_order; i++)
            cJSON_AddItemToArray(cal, cJSON_CreateNumber(sp->calibration[i]));
        cJSON_AddItemToObject(root, "calibration", cal);
        cJSON_AddNumberToObject(root, "calib_order", sp->calib_order);
    }
    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(root);
    free(sp);
    return ESP_OK;
}

static esp_err_t handle_reboot_device(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;
    uint8_t pkt_buf[64];
    shproto_struct pkt;
    shproto_init(&pkt, pkt_buf, sizeof(pkt_buf));
    shproto_packet_start(&pkt, CMD_REBOOT);
    shproto_packet_complete(&pkt);
    int ret = usb_host_cdc_send(pkt.data, pkt.len);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, ret == 0 ? "{\"ok\":true}" : "{\"ok\":false}");
    return ESP_OK;
}

static esp_err_t handle_system(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {  // P2-7: при OOM cJSON_Add* разыменует NULL
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "min_free_heap", esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(root, "uptime_sec", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddBoolToObject(root, "usb_connected", usb_host_cdc_is_connected());
    cJSON_AddBoolToObject(root, "wifi_connected", wifi_is_connected());
    cJSON_AddBoolToObject(root, "tcp_client", tcp_bridge_client_connected());
    size_t total = 0, used = 0;
    esp_littlefs_info("storage", &total, &used);
    cJSON_AddNumberToObject(root, "flash_total", (double)total);
    cJSON_AddNumberToObject(root, "flash_used", (double)used);
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        cJSON_AddNumberToObject(root, "rssi", ap.rssi);
        cJSON_AddStringToObject(root, "ssid", (char *)ap.ssid);
    }
    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t handle_set_calibration(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;
    char body[512] = {0};
    int recv_len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (recv_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[recv_len] = '\0';
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    cJSON *arr = cJSON_GetObjectItem(root, "coeffs");
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing coeffs");
        return ESP_FAIL;
    }
    int n = cJSON_GetArraySize(arr);
    if (n < 1) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty coeffs");
        return ESP_FAIL;
    }
    if (n > CALIB_COEFFS) n = CALIB_COEFFS;
    double coeffs[CALIB_COEFFS] = {0};
    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        // P2-5: cJSON парсит "NaN"/"Infinity" в valuedouble как есть. NaN/Inf
        // в коэффициентах калибровки потом дают мусорные энергии и могут
        // распространиться в N42/CSV экспорт — отбраковываем на входе.
        if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble)) {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad coeff");
            return ESP_FAIL;
        }
        coeffs[i] = item->valuedouble;
    }
    spectrum_set_calibration(coeffs, n - 1);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// ---- #DEV-6: бэкап/восстановление настроек прибора (вкладка «Сервис») ----
//
// Формат — сырой текст ответов -inf + -tc_pot?, байт-в-байт совместимый с
// автосейвом MCA.exe (references/atomspectra_settings_backup_2026-07-01.md).
// Ниже — общий key/value-парсер этого текста для восстановления.

static inline bool is_kv_boundary(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// Ищет "KEY" на границе слова (не внутри другого токена — напр. не путает
// "POT" с "POT2", "TC" с "TCpot") и возвращает указатель на начало значения
// (сразу после разделяющего пробела), либо NULL.
// Граница слова — пробел/таб/CR/LF (не только пробел): бэкап — две строки
// через "\r\n" ("Tcpot [...]" начинается сразу после перевода строки), якорь
// только на ' ' пропускал бы этот ключ (#DEV-7, 2026-07-01).
static const char *find_kv(const char *text, const char *key)
{
    size_t klen = strlen(key);
    const char *p = text;
    while ((p = strstr(p, key)) != NULL) {
        bool left_ok  = (p == text) || is_kv_boundary(p[-1]);
        bool right_ok = (p[klen] == '\0') || is_kv_boundary(p[klen]);
        if (left_ok && right_ok)
            return p + klen + (is_kv_boundary(p[klen]) && p[klen] != '\0' ? 1 : 0);
        p += 1;
    }
    return NULL;
}

static bool kv_get_int(const char *text, const char *key, long *out)
{
    const char *v = find_kv(text, key);
    if (!v) return false;
    char *end;
    long val = strtol(v, &end, 10);
    if (end == v) return false;
    *out = val;
    return true;
}

static bool kv_get_double(const char *text, const char *key, double *out)
{
    const char *v = find_kv(text, key);
    if (!v) return false;
    char *end;
    double val = strtod(v, &end);
    if (end == v) return false;
    *out = val;
    return true;
}

// Копирует значение-токен до пробела (для "ON"/"OFF" и т.п.)
static bool kv_get_token(const char *text, const char *key, char *buf, size_t bufsz)
{
    const char *v = find_kv(text, key);
    if (!v) return false;
    size_t n = 0;
    while (v[n] && v[n] != ' ' && n < bufsz - 1) { buf[n] = v[n]; n++; }
    buf[n] = '\0';
    return n > 0;
}

// Парсит "KEY [n1 n2 ...]" -> массив long (до max элементов). Возвращает
// число элементов (0 для "[]"), либо -1 если ключ/скобка не найдены.
static int kv_get_array(const char *text, const char *key, long *out, int max)
{
    const char *v = find_kv(text, key);
    if (!v) return -1;
    while (*v == ' ') v++;
    if (*v != '[') return -1;
    v++;
    int n = 0;
    while (*v && *v != ']') {
        char *end;
        long val = strtol(v, &end, 10);
        if (end == v) break;
        if (n < max) out[n++] = val;
        v = end;
        while (*v == ' ') v++;
    }
    return n;
}

// GET /api/settings/backup — read-only относительно физического состояния
// прибора: шлёт -inf/-tc_pot? и отдаёт сырые ответы (см. spectrum_get_info_raw/
// spectrum_get_tcpot_raw, atomspectra.h). CSRF не требуется (не мутирует прибор).
static esp_err_t handle_settings_backup(httpd_req_t *req)
{
    if (!usb_host_cdc_is_connected()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Device not connected");
        return ESP_FAIL;
    }

    char line[700];
    uint32_t seq_before = 0, seq_after = 0;

    spectrum_get_info_raw(line, sizeof(line), &seq_before);
    usb_host_send_text_command("-inf");
    for (int waited = 0; waited < 2000; waited += 50) {
        vTaskDelay(pdMS_TO_TICKS(50));
        spectrum_get_info_raw(line, sizeof(line), &seq_after);
        if (seq_after != seq_before) break;
    }
    if (seq_after == seq_before) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No response to -inf (device busy/offline?)");
        return ESP_FAIL;
    }
    // #FW-17: static + 2048Б — под полную -inf с 99-элементным PileUp[] (см.
    // s_info_raw в spectrum.c). НЕ на стеке: httpd-воркер сериализует запросы
    // (async off), реентранси нет; 2КБ на стеке 8192 — лишний риск.
    static char info_line[2048];
    spectrum_get_info_raw(info_line, sizeof(info_line), NULL);

    spectrum_get_tcpot_raw(line, sizeof(line), &seq_before);
    usb_host_send_text_command("-tc_pot?");
    seq_after = seq_before;
    for (int waited = 0; waited < 2000; waited += 50) {
        vTaskDelay(pdMS_TO_TICKS(50));
        spectrum_get_tcpot_raw(line, sizeof(line), &seq_after);
        if (seq_after != seq_before) break;
    }
    if (seq_after == seq_before) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No response to -tc_pot? (older firmware?)");
        return ESP_FAIL;
    }
    char tcpot_line[700];
    spectrum_get_tcpot_raw(tcpot_line, sizeof(tcpot_line), NULL);

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"atomspectra_backup.txt\"");
    httpd_resp_sendstr_chunk(req, info_line);
    httpd_resp_sendstr_chunk(req, "\r\n");
    httpd_resp_sendstr_chunk(req, tcpot_line);
    httpd_resp_sendstr_chunk(req, "\r\n");
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static void send_cmd_delayed(const char *cmd)
{
    usb_host_send_text_command(cmd);
    // Пауза между командами настройки — не заваливать вход прибора пачкой
    // текстовых команд подряд (см. -t/-t_pot до 20 точек каждая).
    vTaskDelay(pdMS_TO_TICKS(30));
}

// POST /api/settings/restore — body = текст бэкапа (тот же формат, что
// отдаёт handle_settings_backup). Разбирает поля и шлёт прибору
// последовательность команд настройки по PROTOCOL.md.
// ⚠ МЕНЯЕТ физическую конфигурацию прибора (RISE/FALL/NOISE/MAX/HYST/F/POT/
// POT2/термокомпенсацию/pile-up). CSRF обязателен. Первый реальный тест на
// железе — только по явному отдельному «да» оператора (см. CLAUDE.md #DEV-6,
// прецедент #DEV-5 — оператор ранее сказал «не сейчас» именно для -frq).
static esp_err_t handle_settings_restore(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;
    if (!usb_host_cdc_is_connected()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Device not connected");
        return ESP_FAIL;
    }

    char *body = malloc(1600);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    int total = 0, r;
    while (total < 1599 && (r = httpd_req_recv(req, body + total, 1599 - total)) > 0)
        total += r;
    if (total <= 0) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[total] = '\0';

    if (!strstr(body, "VERSION ") || !strstr(body, "RISE ")) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Not an AtomSpectra backup (VERSION/RISE missing)");
        return ESP_FAIL;
    }

    char cmd[160];
    long v;
    double d;
    char tok[16];
    long arr[40];
    int n;

    if (kv_get_int(body, "RISE", &v))  { snprintf(cmd, sizeof(cmd), "-ris %ld", v);  send_cmd_delayed(cmd); }
    if (kv_get_int(body, "FALL", &v))  { snprintf(cmd, sizeof(cmd), "-fall %ld", v); send_cmd_delayed(cmd); }
    if (kv_get_int(body, "NOISE", &v)) { snprintf(cmd, sizeof(cmd), "-nos %ld", v);  send_cmd_delayed(cmd); }
    if (kv_get_int(body, "HYST", &v))  { snprintf(cmd, sizeof(cmd), "-hyst %ld", v); send_cmd_delayed(cmd); }
    if (kv_get_int(body, "MAX", &v))   { snprintf(cmd, sizeof(cmd), "-max %ld", v);  send_cmd_delayed(cmd); }
    if (kv_get_int(body, "STEP", &v))  { snprintf(cmd, sizeof(cmd), "-step %ld", v); send_cmd_delayed(cmd); }
    if (kv_get_double(body, "F", &d) && d > 0) {
        snprintf(cmd, sizeof(cmd), "-frq %ld", (long)(d + 0.5));
        send_cmd_delayed(cmd);
    }
    // POT/POT2 = значения U (HV) и V (baseline) соответственно (см.
    // atomspectra_protocol_official_spec.txt:141). Синтаксис без пробела.
    if (kv_get_int(body, "POT", &v))  { snprintf(cmd, sizeof(cmd), "-U%ld", v); send_cmd_delayed(cmd); }
    if (kv_get_int(body, "POT2", &v)) { snprintf(cmd, sizeof(cmd), "-V%ld", v); send_cmd_delayed(cmd); }

    if (kv_get_int(body, "Prise", &v)) { snprintf(cmd, sizeof(cmd), "-prise %ld", v); send_cmd_delayed(cmd); }
    if (kv_get_int(body, "Srise", &v)) { snprintf(cmd, sizeof(cmd), "-srise %ld", v); send_cmd_delayed(cmd); }
    if (kv_get_int(body, "Pfall", &v)) { snprintf(cmd, sizeof(cmd), "-pfall %ld", v); send_cmd_delayed(cmd); }
    if (kv_get_int(body, "Sfall", &v)) { snprintf(cmd, sizeof(cmd), "-sfall %ld", v); send_cmd_delayed(cmd); }

    if (kv_get_int(body, "PileUpThr", &v)) { snprintf(cmd, sizeof(cmd), "-pthr %ld", v); send_cmd_delayed(cmd); }
    n = kv_get_array(body, "PileUp", arr, 100);
    if (n > 0) {
        int off = snprintf(cmd, sizeof(cmd), "-pileup");
        for (int i = 0; i < n && off < (int)sizeof(cmd) - 8; i++)
            off += snprintf(cmd + off, sizeof(cmd) - off, " %ld", arr[i]);
        send_cmd_delayed(cmd);
    }

    // Tco[] — до 20 точек (temp, max_integral) термокомпенсации макс. интеграла.
    n = kv_get_array(body, "Tco", arr, 40);
    if (n >= 2) {
        send_cmd_delayed("-tclear");
        int npoints = n / 2;
        if (npoints > 20) npoints = 20;
        for (int i = 0; i < npoints; i++) {
            snprintf(cmd, sizeof(cmd), "-t %d %ld %ld", i + 1, arr[2 * i], arr[2 * i + 1]);
            send_cmd_delayed(cmd);
        }
    }
    if (kv_get_int(body, "TP", &v)) { snprintf(cmd, sizeof(cmd), "-tp %ld", v); send_cmd_delayed(cmd); }
    if (kv_get_token(body, "TC", tok, sizeof(tok))) {
        snprintf(cmd, sizeof(cmd), "-tc %s", strcasecmp(tok, "ON") == 0 ? "on" : "off");
        send_cmd_delayed(cmd);
    }

    // Вторая строка бэкапа: "Tcpot [...]" (регистр отличается от "TCpot" выше,
    // strstr — регистрозависим, поэтому find_kv не путает две строки) —
    // до 20 точек компенсации baseline (#DOC-3/BUG-AS-08).
    n = kv_get_array(body, "Tcpot", arr, 40);
    if (n >= 2) {
        int npoints = n / 2;
        if (npoints > 20) npoints = 20;
        for (int i = 0; i < npoints; i++) {
            snprintf(cmd, sizeof(cmd), "-t_pot %d %ld %ld", i + 1, arr[2 * i], arr[2 * i + 1]);
            send_cmd_delayed(cmd);
        }
    }
    if (kv_get_token(body, "TCpot", tok, sizeof(tok))) {
        snprintf(cmd, sizeof(cmd), "-tc_pot %s", strcasecmp(tok, "ON") == 0 ? "on" : "off");
        send_cmd_delayed(cmd);
    }

    free(body);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t handle_reboot_esp(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t handle_healthcheck(httpd_req_t *req)
{
    bool usb = usb_host_cdc_is_connected();
    bool wifi = wifi_is_connected();
    int64_t uptime_us = esp_timer_get_time();
    const spectrum_data_t *sp = spectrum_get_current();

    httpd_resp_set_type(req, "application/json");
    if (usb && wifi) {
        httpd_resp_set_status(req, "200 OK");
    } else {
        httpd_resp_set_status(req, "503 Service Unavailable");
    }

    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"status\":\"%s\",\"analyzer_connected\":%s,\"wifi_connected\":%s,"
        "\"uptime_sec\":%.1f,\"spectrum_valid\":%s}",
        (usb && wifi) ? "ok" : "degraded",
        usb ? "true" : "false",
        wifi ? "true" : "false",
        (double)uptime_us / 1000000.0,
        sp->valid ? "true" : "false");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t handle_wifi_reset(httpd_req_t *req)
{
    if (!csrf_check(req)) return ESP_FAIL;
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_all(nvs);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    httpd_resp_sendstr(req, "{\"ok\":true}");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

void web_server_init(void)
{
    csrf_generate();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    // #WF-1: httpd на core 1 к остальной прикладной сети (#TCP-2). Дефолт
    // tskNO_AFFINITY позволял httpd (prio 5) исполняться на core 0 рядом с
    // USB-приёмом — уводим целиком.
    config.core_id = 1;
    config.max_uri_handlers = 45;        // 30 базовых (27 + /api/settings/backup,/restore,/service #DEV-6) + 13 waterfall (11 A1 вкл. websocket + 2× /api/waterfall/offload A2) = 43, +2 запас
    config.stack_size = 8192;
    config.max_open_sockets = 11;        // из 16 LWIP-сокетов; запас для tcp_bridge + sntp
    config.lru_purge_enable = true;      // при исчерпании пула закрыть LRU-соединение, не отказывать (errno 23)
    config.uri_match_fn = httpd_uri_match_wildcard;
    // #UI-15 P0: чистим WS-реестр при ЛЮБОМ закрытии сокета (RST/FIN/LRU/F5);
    // без callback зомбирующиеся fd ломают broadcast после нескольких F5.
    config.close_fn = web_waterfall_on_close;
    // #UI-15 P2: сжимаем default recv/send (~5 c) — освобождаем сокеты быстрее
    // под давлением F5+poll, иначе пул держит «полудохлые» соединения долго.
    config.recv_wait_timeout = 3;
    config.send_wait_timeout = 3;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    const httpd_uri_t uris[] = {
        {"/api/csrf-token",              HTTP_GET,  handle_csrf_token,       NULL},
        {"/api/status",                  HTTP_GET,  handle_status,           NULL},
        {"/api/spectrum",                HTTP_GET,  handle_spectrum,         NULL},
        {"/api/spectrum.json",           HTTP_GET,  handle_spectrum_json,    NULL},
        {"/api/command",                 HTTP_POST, handle_command,          NULL},
        {"/api/devlog",                  HTTP_GET,  handle_devlog,           NULL},
        {"/api/reset",                   HTTP_POST, handle_reset,            NULL},
        {"/api/boot-config",             HTTP_GET,  handle_boot_config_get,  NULL},
        {"/api/boot-config",             HTTP_POST, handle_boot_config_set,  NULL},
        {"/api/save",                    HTTP_POST, handle_save,             NULL},
        {"/api/list",                    HTTP_GET,  handle_list,             NULL},
        {"/api/export.xml",              HTTP_GET,  handle_export_xml,       NULL},
        {"/api/export.csv",              HTTP_GET,  handle_export_csv,       NULL},
        {"/api/export.n42",              HTTP_GET,  handle_export_n42,       NULL},
        {"/api/export.spe",              HTTP_GET,  handle_export_spe,       NULL},
        {"/api/saved/*",                 HTTP_GET,  handle_saved_get,        NULL},
        {"/api/saved/*",                 HTTP_POST, handle_saved_delete,     NULL},
        {"/api/device",                  HTTP_GET,  handle_device,           NULL},
        {"/api/system",                  HTTP_GET,  handle_system,           NULL},
        {"/api/reboot-device",           HTTP_POST, handle_reboot_device,    NULL},
        {"/api/wifi/reset",              HTTP_POST, handle_wifi_reset,       NULL},
        {"/api/reboot-esp",              HTTP_POST, handle_reboot_esp,       NULL},
        {"/api/calibration",             HTTP_POST, handle_set_calibration,  NULL},
        {"/api/settings/backup",         HTTP_GET,  handle_settings_backup,  NULL},
        {"/api/settings/restore",        HTTP_POST, handle_settings_restore, NULL},
        {"/healthcheck",                 HTTP_GET,  handle_healthcheck,      NULL},
        {"/saved",                       HTTP_GET,  handle_saved_page,       NULL},
        {"/system",                      HTTP_GET,  handle_system_page,      NULL},
        {"/service",                     HTTP_GET,  handle_service_page,     NULL},
        {"/",                            HTTP_GET,  handle_index,            NULL},
    };

    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++)
        httpd_register_uri_handler(server, &uris[i]);

    web_waterfall_register(server);      // /waterfall, /api/waterfall/*, /ws/waterfall

    ESP_LOGI(TAG, "Web server started on port %d", config.server_port);
}