#include "atomspectra.h"
#include "spectrogram.h"   // #FW-55 (P-016): spectrogram_prepare_reboot()
#include "wifi_reconnect_plan.h"  // AWF-2a (#1): расписание пауз реконнекта
#include "wifi_return_plan.h"     // AWF-2a (#2): решение о возврате из Field AP
#include "boot_config.h"          // AWF-2a доработка: настройка ap_fallback
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_timer.h"       // #FIELD-2: fallback-таймер STA→AP
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "mdns.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/sockets.h"
#include <string.h>

static const char *TAG = "wifi_mgr";

static EventGroupHandle_t s_wifi_events;
#define WIFI_CONNECTED_BIT BIT0

// AWF-2a (#1): счётчик попыток (индекс в расписании пауз) и таймер,
// планирующий следующий esp_wifi_connect() без блокировки обработчика событий.
// P1 (независимое ревью f091b01): счётчик неудачных попыток возврата в STA
// из fallback Field AP — растёт, если после esp_restart() в STA плата снова
// свалилась в fallback (SSID виден, но не держится: пароль/DHCP). Сброс —
// только по IP_EVENT_STA_GOT_IP (см. wifi_return_plan.h).
static uint32_t s_return_fail_count = 0;
// AWF-2a доработка: STA хоть раз получила IP в ЭТОЙ загрузке (сбрасывается
// ребутом) — вход в решение wifi_reconnect_should_fallback().
static bool s_got_ip_this_boot = false;
// Диагностика застревания в Field AP (живой тест 25.09): причина последнего
// блока возврата, для /api/system. Лог — только при смене причины.
static const char *s_return_block_reason = "none";

static int s_retry_count = 0;
static int64_t s_disconnect_started_us = 0;   // 0 = сейчас не в серии реконнектов
static esp_timer_handle_t s_reconnect_timer = NULL;

// #FIELD-2a (ревью-2): единый источник порога — WIFI_RECONNECT_FALLBACK_S
// (wifi_reconnect_plan.h, 300с), а не своя константа. Было 90с — короче, чем
// расписание реконнекта (WIFI_RECONNECT_STEPS суммарно ~108с и растёт), из-за
// чего startup-таймер уводил плату в Field AP раньше событийного пути:
// "моргнул свет", роутер поднимается 1-2 мин — 90с не хватало.

// #FIELD-1: текущий сетевой режим. Дефолт STA, устанавливается в развилке init.
static net_run_mode_t s_mode = NET_MODE_STA;

// #FIELD-1/#SEC-2: параметры полевого AP (дефолты — из ТЗ разд. 9).
#define AP_SSID_DEFAULT "AtomSpectra-Outdoor"
#define AP_PASS_DEFAULT "atomspectra"
static char s_ap_ssid[WIFI_SSID_MAX] = AP_SSID_DEFAULT;
static char s_ap_pass[WIFI_PASS_MAX] = AP_PASS_DEFAULT;
static bool s_ap_pass_default = true;   // #SEC-2: пароль AP не менялся
static bool s_ap_forced = false;        // #FIELD-6: field_ap липкий (ap_mode=1) vs fallback

static esp_timer_handle_t s_fallback_timer = NULL;

/* ---- DNS captive portal ---- */

static void dns_captive_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket failed");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = INADDR_ANY
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    uint8_t rx[512], tx[512];
    while (1) {
        struct sockaddr_in client;
        socklen_t clen = sizeof(client);
        int len = recvfrom(sock, rx, sizeof(rx), 0,
                           (struct sockaddr *)&client, &clen);
        if (len < 12) continue;
        // P2-2: ниже к ответу дописываются 16 байт A-записи (off = len + 16).
        // Без этой проверки запрос длиной > sizeof(tx)-16 переполнил бы tx[].
        if (len > (int)sizeof(tx) - 16) continue;

        memcpy(tx, rx, len);
        tx[2] = 0x80 | (rx[2] & 0x79);
        tx[3] = 0x80;
        tx[6] = 0; tx[7] = 1;

        int off = len;
        tx[off++] = 0xC0; tx[off++] = 0x0C;
        tx[off++] = 0x00; tx[off++] = 0x01;
        tx[off++] = 0x00; tx[off++] = 0x01;
        tx[off++] = 0x00; tx[off++] = 0x00;
        tx[off++] = 0x00; tx[off++] = 0x3C;
        tx[off++] = 0x00; tx[off++] = 0x04;
        tx[off++] = 192;  tx[off++] = 168;
        tx[off++] = 4;    tx[off++] = 1;

        sendto(sock, tx, off, 0,
               (struct sockaddr *)&client, clen);
    }
}

/* ---- Captive setup portal HTTP (свежая плата, NET_MODE_SETUP) ---- */

static esp_err_t handle_setup_root(httpd_req_t *req)
{
    extern const uint8_t setup_html_start[] asm("_binary_setup_html_start");
    extern const uint8_t setup_html_end[]   asm("_binary_setup_html_end");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)setup_html_start,
                    setup_html_end - setup_html_start);
    return ESP_OK;
}

static esp_err_t handle_setup_scan(httpd_req_t *req)
{
    wifi_scan_config_t scan_cfg = { .show_hidden = false };
    esp_wifi_scan_start(&scan_cfg, true);

    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num > 20) num = 20;

    wifi_ap_record_t *records = calloc(num, sizeof(wifi_ap_record_t));
    if (!records) {
        httpd_resp_sendstr(req, "[]");
        return ESP_OK;
    }
    esp_wifi_scan_get_ap_records(&num, records);

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < num; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", (char *)records[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", records[i].rssi);
        cJSON_AddNumberToObject(item, "auth", records[i].authmode);
        cJSON_AddItemToArray(arr, item);
    }
    free(records);

    char *json = cJSON_PrintUnformatted(arr);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(arr);
    return ESP_OK;
}

static esp_err_t handle_setup_connect(httpd_req_t *req)
{
    char body[256] = {0};
    int recv_len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (recv_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty");
        return ESP_FAIL;
    }
    body[recv_len] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");
        return ESP_FAIL;
    }

    const char *ssid = cJSON_GetStringValue(
        cJSON_GetObjectItem(root, "ssid"));
    const char *pass = cJSON_GetStringValue(
        cJSON_GetObjectItem(root, "pass"));

    if (!ssid || strlen(ssid) == 0) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No SSID");
        return ESP_FAIL;
    }

    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, "ssid", ssid);
        // P2-6 / #SEC-1: пароль WiFi пишется в NVS открытым текстом. Любой с
        // физическим доступом к плате может вычитать flash
        // (esptool read_flash 0x9000 0x6000 nvs.bin) и достать пароль строкой —
        // шифрования нет. Это осознанный компромисс для модели "доверенный
        // домашний LAN". Единственная корректная защита — включить шифрование
        // flash (NVS-encryption работает поверх него): заготовка
        // CONFIG_SECURE_FLASH_ENC_ENABLED + CONFIG_NVS_ENCRYPTION лежит
        // закомментированной в sdkconfig.defaults. ⚠ Необратимо (прожигает eFuse),
        // поэтому по умолчанию выкл — это gate оператора, здесь не трогаем.
        // Подробности и последствия — INSTALL.md, раздел "9. Безопасность".
        nvs_set_str(nvs, "pass", pass ? pass : "");
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "WiFi config saved: SSID=%s", ssid);
    }

    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");

    spectrogram_prepare_reboot();   // #FW-55 (P-016): не терять открытый сегмент
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

// #FIELD-2c: из setup-портала — «Работать без роутера (полевой режим)».
// Ставит forced-флаг Outdoor и ребутит: свежая плата сразу в поле без домашней сети.
static esp_err_t handle_setup_field(httpd_req_t *req)
{
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, "ap_mode", 1);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    ESP_LOGI(TAG, "FIELD-2c: field mode requested from setup -> reboot");
    spectrogram_prepare_reboot();   // #FW-55 (P-016): не терять открытый сегмент
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

static esp_err_t handle_setup_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_sendstr(req, "");
    return ESP_OK;
}

static void start_captive_portal(void)
{
    s_mode = NET_MODE_SETUP;

    esp_netif_create_default_wifi_ap();

    wifi_config_t ap_config = {
        .ap = {
            .ssid = "AtomSpectra-Setup",
            .channel = 1,
            .authmode = WIFI_AUTH_OPEN,
            .max_connection = 4,
        }
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Setup AP: SSID=AtomSpectra-Setup, IP=192.168.4.1");

    xTaskCreate(dns_captive_task, "dns_captive", 4096, NULL, 5, NULL);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.uri_match_fn = httpd_uri_match_wildcard;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        const httpd_uri_t uris[] = {
            {"/",              HTTP_GET,  handle_setup_root,     NULL},
            {"/api/scan",      HTTP_GET,  handle_setup_scan,     NULL},
            {"/api/connect",   HTTP_POST, handle_setup_connect,  NULL},
            {"/api/field-mode",HTTP_POST, handle_setup_field,    NULL},  // #FIELD-2c
            {"/*",             HTTP_GET,  handle_setup_redirect, NULL},
        };
        for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++)
            httpd_register_uri_handler(server, &uris[i]);
        ESP_LOGI(TAG, "Captive portal started");
    }
}

/* ---- mDNS (atomspectra.local) ---- */

static void start_mdns(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return;
    }
    mdns_hostname_set("atomspectra");
    mdns_instance_name_set("AtomSpectra Gateway");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS started: http://atomspectra.local/");
}

/* ---- Полевой AP (Outdoor, NET_MODE_FIELD_AP) ---- */

// #FIELD-1/#SEC-2: подтянуть SSID/пароль AP из NVS (иначе — документированные дефолты).
static void load_ap_config(void)
{
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READONLY, &nvs) != ESP_OK) return;
    size_t len = sizeof(s_ap_ssid);
    nvs_get_str(nvs, "ap_ssid", s_ap_ssid, &len);   // not-found → дефолт цел
    len = sizeof(s_ap_pass);
    if (nvs_get_str(nvs, "ap_pass", s_ap_pass, &len) == ESP_OK)
        s_ap_pass_default = false;                   // пароль был задан оператором
    nvs_close(nvs);
}

// #FIELD-1: полевой AP-режим. AP-only (не APSTA), полный Web UI поднимает main.c.
// DNS-hijack + mDNS здесь; captive-пробы и REST — в web_server (#FIELD-4).
static void start_field_ap(void)
{
    s_mode = NET_MODE_FIELD_AP;
    load_ap_config();

    esp_netif_create_default_wifi_ap();

    wifi_config_t ap = {
        .ap = {
            .channel        = 1,   // #FIELD-1/A10: мягкое требование (AP-only гарантирует)
            .max_connection = 4,
            .authmode = (strlen(s_ap_pass) >= 8) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN,
        }
    };
    strlcpy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(s_ap_ssid);
    strlcpy((char *)ap.ap.password, s_ap_pass, sizeof(ap.ap.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "FIELD-1: Outdoor AP up: SSID=%s auth=%s IP=192.168.4.1",
             s_ap_ssid, (ap.ap.authmode == WIFI_AUTH_OPEN) ? "OPEN" : "WPA2");

    xTaskCreate(dns_captive_task, "dns_captive", 4096, NULL, 5, NULL);
    start_mdns();
}

/* ---- STA fallback → полевой AP (FIELD-2a, способ A4: ребут+одноразовый флаг) ---- */

static void set_fb_flag_and_reboot(void)
{
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, "ap_fb_once", 1);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    ESP_LOGW(TAG, "FIELD-2a: STA no IP -> reboot into field AP");
    spectrogram_prepare_reboot();   // #FW-55 (P-016): не терять открытый сегмент
    esp_restart();
}

static void fallback_timer_cb(void *arg)
{
    (void)arg;
    if (!wifi_is_connected())
        set_fb_flag_and_reboot();
}

// AWF-2a финал (решение оператора 25.09): настройка «Переходить в Field AP при
// потере Wi-Fi» из /api/boot-config — по умолчанию ВЫКЛ (отсутствующий ключ
// в NVS = false, прямая семантика, см. boot_config.h). Страховка (STA без IP
// 300с → Field AP) не зависит от этого флага, читается отдельно.
static bool ap_fallback_enabled(void)
{
    boot_config_t bc;
    boot_config_load(&bc);
    return bc.field_ap_fallback_enabled;
}

const char *wifi_manager_return_block_reason(void)
{
    return s_return_block_reason;
}
// Лог ТОЛЬКО при смене причины (живой тест 25.09: 11 мин застревания не
// оставили следа — молчаливые ранние выходы). Не чаще раза на причину.
static void note_return_block(const char *reason)
{
    if (s_return_block_reason != reason) {
        ESP_LOGW(TAG, "FIELD return blocked: %s", reason);
        s_return_block_reason = reason;
    }
}

// AWF-2a (#1): очередная попытка по расписанию пауз — не блокирует обработчик
// событий (эта функция сама вызывается из таска esp_timer, не из handler'а).
static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    if (!wifi_is_connected())
        esp_wifi_connect();
}

/* ---- STA mode ---- */

// Таймер создаётся лениво (esp_wifi_connect() ещё не звали до первого
// STA_DISCONNECTED) — обычный esp-idf идиом, ошибку создания просто логируем.
static void ensure_reconnect_timer(void)
{
    if (s_reconnect_timer) return;
    const esp_timer_create_args_t targs = {
        .callback = reconnect_timer_cb,
        .name = "wifi_reconnect",
    };
    if (esp_timer_create(&targs, &s_reconnect_timer) != ESP_OK)
        ESP_LOGE(TAG, "reconnect timer create failed");
}

// P1: STA реально держит IP — закрыть серию неудачных попыток возврата.
static void wifi_return_note_connected(void)
{
    s_return_fail_count = 0;
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_set_u8(nvs, "ret_fail", 0);
    nvs_erase_key(nvs, "ret_pend");   // ENOENT молча проглатывается esp-idf
    nvs_commit(nvs);
    nvs_close(nvs);
}

// P1: вызывать РОВНО когда плата входит в fallback Field AP (ap_fb_once).
// ret_pend от прошлой попытки возврата означает, что STA не удержалась —
// бампим счётчик задержки; иначе (обычный первый уход в поле) счётчик не
// трогаем, только подгружаем его текущее значение в RAM.
static void wifi_return_note_fallback_entered(void)
{
    uint8_t ret_pend = 0, ret_fail = 0;
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READWRITE, &nvs) != ESP_OK) { s_return_fail_count = 0; return; }
    nvs_get_u8(nvs, "ret_pend", &ret_pend);
    nvs_get_u8(nvs, "ret_fail", &ret_fail);
    if (ret_pend) {
        ret_fail = (uint8_t)wifi_return_fail_count_bump(ret_fail);
        nvs_set_u8(nvs, "ret_fail", ret_fail);
        nvs_erase_key(nvs, "ret_pend");
    }
    nvs_commit(nvs);
    nvs_close(nvs);
    s_return_fail_count = ret_fail;
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)data;
        uint8_t reason = disc ? disc->reason : 0;
        ESP_LOGW(TAG, "STA disconnected reason=%u", (unsigned)reason);
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        // AWF-2a (#1): нарастающие паузы (esp_timer, не блокируя handler) вместо
        // немедленных попыток подряд — короткая перезагрузка роутера не должна
        // уводить плату в поле раньше, чем роутер успевает подняться.
        if (s_disconnect_started_us == 0) s_disconnect_started_us = esp_timer_get_time();
        uint32_t elapsed_s = (uint32_t)((esp_timer_get_time() - s_disconnect_started_us) / 1000000);
        if (wifi_reconnect_should_fallback(ap_fallback_enabled(), s_got_ip_this_boot, elapsed_s)) {
            ESP_LOGE(TAG, "WiFi down %us (reason=%u) -> field AP", (unsigned)elapsed_s, (unsigned)reason);
            set_fb_flag_and_reboot();
        } else {
            uint32_t delay_s = wifi_reconnect_delay_s(s_retry_count);
            s_retry_count++;
            ensure_reconnect_timer();
            if (s_reconnect_timer) {
                esp_timer_stop(s_reconnect_timer);   // на случай уже взведённого (idempotent)
                esp_timer_start_once(s_reconnect_timer, (uint64_t)delay_s * 1000000);
            }
            ESP_LOGW(TAG, "Reconnect in %us (attempt %d, down %us, reason=%u)",
                     (unsigned)delay_s, s_retry_count, (unsigned)elapsed_s, (unsigned)reason);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_got_ip_this_boot = true;   // AWF-2a доработка
        s_retry_count = 0;
        s_disconnect_started_us = 0;   // AWF-2a (#1): серия реконнектов закрыта
        if (s_reconnect_timer) esp_timer_stop(s_reconnect_timer);
        wifi_return_note_connected();  // P1: единственная точка сброса ret_fail/ret_pend
        // #FIELD-2a: STA поднялась — отменить fallback-таймер
        if (s_fallback_timer) esp_timer_stop(s_fallback_timer);
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

void wifi_manager_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // #FIELD-2: режимные флаги NVS
    uint8_t ap_mode = 0, ap_fb_once = 0;
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, "ap_mode", &ap_mode);
        nvs_get_u8(nvs, "ap_fb_once", &ap_fb_once);
        nvs_close(nvs);
    }

    // #FIELD-2a: одноразовый fallback после неудачной STA — войти в поле, сбросить флаг.
    // Не липкий: следующий ребут снова пробует STA (FIELD-3).
    if (ap_fb_once) {
        wifi_return_note_fallback_entered();   // P1: детектирует неудачную попытку возврата
        if (nvs_open("wifi", NVS_READWRITE, &nvs) == ESP_OK) {
            nvs_erase_key(nvs, "ap_fb_once");
            nvs_commit(nvs);
            nvs_close(nvs);
        }
        ESP_LOGW(TAG, "FIELD-2a: entering field AP (one-shot fallback, ret_fail=%u)",
                 (unsigned)s_return_fail_count);
        s_ap_forced = false;   // #FIELD-6: это fallback, не липкий forced
        start_field_ap();
        return;
    }

    // #FIELD-2b: forced Outdoor (липкий; снимается переключателем Indoor/Outdoor в UI).
    if (ap_mode) {
        ESP_LOGI(TAG, "FIELD-2b: forced Outdoor (ap_mode=1)");
        s_ap_forced = true;    // #FIELD-6: липкий forced (снимается переключателем)
        start_field_ap();
        return;
    }

    // STA-конфиг из NVS
    wifi_config_t wifi_config = {0};
    if (nvs_open("wifi", NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(wifi_config.sta.ssid);
        nvs_get_str(nvs, "ssid", (char *)wifi_config.sta.ssid, &len);
        len = sizeof(wifi_config.sta.password);
        nvs_get_str(nvs, "pass", (char *)wifi_config.sta.password, &len);
        nvs_close(nvs);
        ESP_LOGI(TAG, "WiFi from NVS: SSID=%s", wifi_config.sta.ssid);
    }

    if (wifi_config.sta.ssid[0] == 0) {
        ESP_LOGW(TAG, "No WiFi config, starting setup captive portal");
        start_captive_portal();
        return;
    }

    // #FIELD-1: Indoor (STA) + fallback-таймер WIFI_RECONNECT_FALLBACK_S (FIELD-2a).
    esp_event_handler_instance_t inst_any, inst_got_ip;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
        &wifi_event_handler, NULL, &inst_any);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
        &wifi_event_handler, NULL, &inst_got_ip);

    esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* #PERF-5: дефолтный для STA MIN_MODEM power save даёт джиттер ICMP/HTTP RTT
     * на idle-плате почти без потерь. Trade-off: выше потребление в STA.
     * Не критично для загрузки — при ошибке продолжаем на дефолтном PS. */
    esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ps_err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_ps(NONE) failed: %s, keeping default PS",
                 esp_err_to_name(ps_err));
    } else {
        wifi_ps_type_t ps = WIFI_PS_NONE;
        if (esp_wifi_get_ps(&ps) == ESP_OK) {
            ESP_LOGI(TAG, "WiFi PS mode=%d (0=NONE)", (int)ps);
        }
    }

    start_mdns();
    s_mode = NET_MODE_STA;

    // fallback-таймер: нет IP за WIFI_RECONNECT_FALLBACK_S -> ребут в полевой AP
    // (тот же порог, что и событийный путь STA_DISCONNECTED — единый источник).
    const esp_timer_create_args_t targs = {
        .callback = fallback_timer_cb,
        .name = "wifi_fb",
    };
    if (esp_timer_create(&targs, &s_fallback_timer) == ESP_OK)
        esp_timer_start_once(s_fallback_timer, (uint64_t)WIFI_RECONNECT_FALLBACK_S * 1000000);

    ESP_LOGI(TAG, "WiFi STA starting, SSID=%s (fallback %us)",
             wifi_config.sta.ssid, (unsigned)WIFI_RECONNECT_FALLBACK_S);
}

bool wifi_is_connected(void)
{
    return (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) != 0;
}

net_run_mode_t wifi_manager_mode(void)
{
    return s_mode;
}

bool wifi_manager_is_ap_mode(void)
{
    return s_mode != NET_MODE_STA;   // FIELD_AP или SETUP
}

int wifi_manager_ap_clients(void)
{
    if (s_mode == NET_MODE_STA) return -1;
    wifi_sta_list_t list;
    if (esp_wifi_ap_get_sta_list(&list) == ESP_OK) return list.num;
    return 0;
}

const char *wifi_manager_ap_ssid(void)
{
    if (s_mode == NET_MODE_FIELD_AP) return s_ap_ssid;
    if (s_mode == NET_MODE_SETUP)    return "AtomSpectra-Setup";
    return "";
}

bool wifi_manager_ap_forced(void)
{
    return s_ap_forced;
}

bool wifi_manager_ap_pass_is_default(void)
{
    return s_ap_pass_default;
}

// AWF-2a (#2): сохранённый SSID из NVS, куда его пишет handle_setup_connect().
// Пусто, если плата ни разу не настраивалась на роутер (свежая/только-forced-AP).
static bool load_saved_sta_ssid(char *out, size_t out_sz)
{
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READONLY, &nvs) != ESP_OK) return false;
    size_t len = out_sz;
    esp_err_t err = nvs_get_str(nvs, "ssid", out, &len);
    nvs_close(nvs);
    return err == ESP_OK && out[0] != '\0';
}

// Временно расширяет режим до APSTA (клиентов на AP уже нет — проверено
// вызывающим), сканирует эфир на конкретный SSID и возвращает AP-only режим.
// AP на время скана может уйти на другой канал (esp-idf так и работает) —
// поэтому вызывающий обязан гарантировать отсутствие клиентов заранее.
static bool scan_for_saved_ssid(const char *ssid)
{
    if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK) return false;
    wifi_scan_config_t scan_cfg = { .ssid = (uint8_t *)ssid, .show_hidden = true };
    bool found = false;
    if (esp_wifi_scan_start(&scan_cfg, true) == ESP_OK) {
        uint16_t num = 0;
        esp_wifi_scan_get_ap_num(&num);
        found = (num > 0);
    }
    esp_wifi_set_mode(WIFI_MODE_AP);
    return found;
}

// Сканирует и решает, перепроверив активность ПОСЛЕ скана (F8, итоговое
// ревью 25.09: раньше переиспользовался снимок activity ДО скана — скан
// блокирующий (esp_wifi_scan_start(...,true)), активность во время него не
// видна, комментарий "перепроверка" был декоративным. Теперь свежий снимок.
static void wifi_return_finish(bool entered_by_fallback, const char *ssid)
{
    bool found = scan_for_saved_ssid(ssid);
    bool quiet_now = web_server_http_activity_quiet(WIFI_RETURN_ACTIVITY_QUIET_MS);
    if (!wifi_return_should_reboot_to_sta(entered_by_fallback, quiet_now, found)) {
        note_return_block(found ? "activity" : "ssid_not_found");
        return;
    }

    ESP_LOGW(TAG, "FIELD-2a: saved SSID '%s' visible again, HTTP quiet -> reboot to STA", ssid);
    // P1: помечаем ПОПЫТКУ до ребута — если STA не удержится и плата опять
    // свалится в fallback, wifi_return_note_fallback_entered() увидит флаг и
    // взведёт бэкофф. Успешный коннект чистит флаг сам (wifi_return_note_connected).
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, "ret_pend", 1);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    spectrogram_prepare_reboot();
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

// AWF-2a (#2): вызывать периодически (раз в 2-3 мин, main.c tick) только в
// Field AP. Возврат — исключительно когда SSID РЕАЛЬНО увиден сканированием,
// клиентов нет и вход был по fallback (не forced Outdoor, тот липкий).
void wifi_manager_try_return_to_sta(void)
{
    if (s_mode != NET_MODE_FIELD_AP) return;
    bool entered_by_fallback = !s_ap_forced;
    if (!entered_by_fallback) { note_return_block("mode"); return; }

    // P1: бэкофф от начала ЭТОЙ Field-AP сессии (esp_timer сбрасывается на
    // каждом ребуте, ret_fail персистентен в NVS) — не сканировать/не решать
    // о возврате, пока не прошёл нужный интервал после N неудачных попыток.
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (!wifi_return_backoff_elapsed(s_return_fail_count, now_ms, 0)) {
        note_return_block("backoff");
        return;
    }
    // F1 (итоговое ревью 25.09): блокирует АКТИВНОСТЬ на уровне СОКЕТА (см.
    // main/http_activity_plan.h) — открытый keep-alive/WS держит активность
    // БЕЗУСЛОВНО, не по метке момента открытия. Не сам факт подключения
    // клиента к Field AP (живой тест 25.09 — простаивающий телефон блокировал
    // возврат 11 минут при прежней, по-соединению, модели).
    bool quiet = web_server_http_activity_quiet(WIFI_RETURN_ACTIVITY_QUIET_MS);
    if (!wifi_return_scan_allowed(entered_by_fallback, quiet)) {
        note_return_block("activity");
        return;
    }

    char ssid[WIFI_SSID_MAX] = {0};
    if (!load_saved_sta_ssid(ssid, sizeof(ssid))) {
        note_return_block("no_saved_ssid");
        return;
    }
    wifi_return_finish(entered_by_fallback, ssid);
}
