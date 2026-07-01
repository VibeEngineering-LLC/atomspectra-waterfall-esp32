#include "atomspectra.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "mdns.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/sockets.h"
#include <string.h>

static const char *TAG = "wifi";

static EventGroupHandle_t s_wifi_events;
#define WIFI_CONNECTED_BIT BIT0

static int s_retry_count = 0;
#define MAX_RETRY 10

static bool s_ap_mode = false;

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

/* ---- Captive portal HTTP ---- */

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
    s_ap_mode = true;

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

    ESP_LOGI(TAG, "AP started: SSID=AtomSpectra-Setup, IP=192.168.4.1");

    xTaskCreate(dns_captive_task, "dns_captive", 4096, NULL, 5, NULL);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.uri_match_fn = httpd_uri_match_wildcard;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        const httpd_uri_t uris[] = {
            {"/",            HTTP_GET,  handle_setup_root,     NULL},
            {"/api/scan",    HTTP_GET,  handle_setup_scan,     NULL},
            {"/api/connect", HTTP_POST, handle_setup_connect,  NULL},
            {"/*",           HTTP_GET,  handle_setup_redirect, NULL},
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

/* ---- STA mode ---- */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        if (s_retry_count < MAX_RETRY) {
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGW(TAG, "Reconnecting (%d/%d)...", s_retry_count, MAX_RETRY);
        } else {
            ESP_LOGE(TAG, "WiFi failed after %d retries", MAX_RETRY);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
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

    wifi_config_t wifi_config = {0};
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(wifi_config.sta.ssid);
        nvs_get_str(nvs, "ssid", (char *)wifi_config.sta.ssid, &len);
        len = sizeof(wifi_config.sta.password);
        nvs_get_str(nvs, "pass", (char *)wifi_config.sta.password, &len);
        nvs_close(nvs);
        ESP_LOGI(TAG, "WiFi from NVS: SSID=%s", wifi_config.sta.ssid);
    }

    if (wifi_config.sta.ssid[0] == 0) {
        ESP_LOGW(TAG, "No WiFi config, starting captive portal");
        start_captive_portal();
        return;
    }

    esp_event_handler_instance_t inst_any, inst_got_ip;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
        &wifi_event_handler, NULL, &inst_any);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
        &wifi_event_handler, NULL, &inst_got_ip);

    esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    start_mdns();

    ESP_LOGI(TAG, "WiFi STA starting, SSID=%s", wifi_config.sta.ssid);
}

bool wifi_is_connected(void)
{
    return (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) != 0;
}

bool wifi_manager_is_ap_mode(void)
{
    return s_ap_mode;
}