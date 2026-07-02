#include "atomspectra.h"
#include "shproto.h"
#include "spectrogram.h"
#include "esp_log.h"
#include <inttypes.h>
#include "usb/cdc_acm_host.h"
#include "usb/usb_host.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "usb_cdc";

#define FTDI_SIO_RESET          0
#define FTDI_SIO_SET_MODEM_CTRL 1
#define FTDI_SIO_SET_FLOW_CTRL  2
#define FTDI_SIO_SET_BAUDRATE   3
#define FTDI_SIO_SET_DATA       4
#define FTDI_REQTYPE_OUT        0x40
#define FTDI_FS_PACKET_SIZE     64

static cdc_acm_dev_hdl_t s_cdc_dev = NULL;
static SemaphoreHandle_t s_tx_mutex = NULL;

static uint8_t s_rx_buf[4096];
static shproto_struct s_rx_packet;

static uint8_t s_tx_buf[256];
static shproto_struct s_tx_packet;

static usb_raw_rx_cb_t s_raw_rx_cb = NULL;
static int s_rx_cb_count = 0;
static volatile uint32_t s_usb_rx_err = 0;  // #TCP-4: FTDI line-status RX errors (OE/PE/FE/FIFO)

static char s_text_accum[4096];
static int  s_text_accum_len = 0;

// #UI-1: кольцо последних текстовых ответов прибора для веб-лога команд (/api/devlog).
// devlog_push() вызывается из cdc-acm data_cb (handle_rx_packet, CMD_TEXT); чтение —
// из httpd-таска (usb_host_cdc_devlog_json). Синхронизация — отдельный мьютекс.
#define DEVLOG_RING   8
#define DEVLOG_TEXTSZ 480
static char     s_devlog_text[DEVLOG_RING][DEVLOG_TEXTSZ];
static uint16_t s_devlog_len [DEVLOG_RING];
static uint32_t s_devlog_seq [DEVLOG_RING];   // seq записи в слоте (0 = пусто)
static uint32_t s_devlog_next = 1;            // seq для следующей записи (монотонный)
static SemaphoreHandle_t s_devlog_mutex = NULL;

static void devlog_push(const uint8_t *txt, int len)
{
    if (len <= 0 || !s_devlog_mutex) return;
    if (len > DEVLOG_TEXTSZ - 1) len = DEVLOG_TEXTSZ - 1;
    xSemaphoreTake(s_devlog_mutex, portMAX_DELAY);
    uint32_t seq = s_devlog_next++;
    int slot = seq % DEVLOG_RING;
    memcpy(s_devlog_text[slot], txt, len);
    s_devlog_text[slot][len] = '\0';
    s_devlog_len[slot] = (uint16_t)len;
    s_devlog_seq[slot] = seq;
    xSemaphoreGive(s_devlog_mutex);
}

// #FW-2/#FW-3: однократные действия при ПЕРВОМ USB-коннекте после ребута.
// Значения задаёт usb_host_cdc_set_autostart() из main.c (читая boot_config из NVS).
static bool s_boot_autostart_spec = false;
static bool s_boot_autostart_wf   = false;
static bool s_boot_clear_spectrum = false;
static bool s_boot_once_done      = false;

// #CMD-1: распознать завершённый ответ на -cal (дамп 40 регистров).
// Формат подтверждён на реальном приборе (Text(400), один CDC-пакет):
// строки ровно по 8 hex, разделённые \r\n; первые 12 — калибровка+CRC, 39-я — серийник.
// Первая строка = 8 hex + перевод строки, и накоплено >=40 строк → дамп целиком.
static bool is_complete_cal(const char *s)
{
    int h = 0;
    while (h < 8) {
        char c = s[h];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')))
            return false;
        h++;
    }
    if (s[8] != '\n' && s[8] != '\r') return false;
    int nl = 0;
    for (const char *q = s; *q; q++) if (*q == '\n') nl++;
    return nl >= 39;
}

static void handle_rx_packet(void)
{
    switch (s_rx_packet.cmd) {
    case CMD_HISTOGRAM:
        spectrum_process_histogram_chunk(s_rx_packet.data, s_rx_packet.len);
        break;
    case CMD_TEXT:
        if (s_rx_packet.len > 0) {
            s_rx_packet.data[s_rx_packet.len] = '\0';
            ESP_LOGI(TAG, "Text(%u): %.80s%s", (unsigned)s_rx_packet.len,
                     (const char*)s_rx_packet.data, s_rx_packet.len>80?"...":"");
            devlog_push(s_rx_packet.data, (int)s_rx_packet.len);  // #UI-1: в веб-лог команд
            int sp = sizeof(s_text_accum)-s_text_accum_len-1;
            int cp = (int)s_rx_packet.len < sp ? (int)s_rx_packet.len : sp;
            memcpy(s_text_accum+s_text_accum_len, s_rx_packet.data, cp);
            s_text_accum_len += cp; s_text_accum[s_text_accum_len] = '\0';
            // #CMD-1: два РАЗНЫХ ответа прибора, каждый завершает накопление и СБРАСЫВАЕТ
            // аккумулятор немедленно, чтобы они не склеивались (доказано на дампе: -cal,
            // не сброшенный, прилипал к следующему -inf → 48 строк вместо 40, CRC mismatch).
            //  • -inf  → один Text(404) с параметрами (VERSION..PileUpThr). Калибровки НЕ
            //            содержит (прежний #PR-1 комментарий про «калибровку в -inf» был
            //            НЕВЕРЕН — опровергнуто реальным дампом). Триггер: "PileUpThr "+"VERSION ".
            //  • -cal  → один Text(400) с дампом 40 регистров (по 8 hex, \r\n). Калибровка
            //            (L0..L9 = 5 double), CRC L10, серийник L39. Триггер: is_complete_cal().
            if (is_complete_cal(s_text_accum) ||
                (strstr(s_text_accum,"PileUpThr ") && strstr(s_text_accum,"VERSION "))) {
                spectrum_process_info_response(s_text_accum);
                s_text_accum_len = 0;
            } else if (s_text_accum_len >= 6 && s_text_accum[s_text_accum_len-1] == ']' &&
                       strstr(s_text_accum,"Tcpot ") != NULL) {
                // #DEV-6: ответ на -tc_pot? — один Text-пакет "Tcpot [...]", завершение
                // маркируется закрывающей скобкой (у -inf/-cal свои триггеры выше).
                // Живой прибор шлёт ведущий пробел перед "Tcpot" (подтверждено devlog
                // seq 18, 2026-07-01: " Tcpot [...]") — ищем подстроку, как у -inf,
                // а не якорим strncmp на позицию 0 (иначе триггер не срабатывал никогда).
                spectrum_process_tcpot_response(strstr(s_text_accum,"Tcpot "));
                s_text_accum_len = 0;
            } else if (s_text_accum_len >= (int)sizeof(s_text_accum) - 128) {
                ESP_LOGW(TAG, "text accum overflow without trigger, reset");
                s_text_accum_len = 0;
            }
        }
        break;
    case CMD_STAT:
        spectrum_process_stat_packet(s_rx_packet.data, s_rx_packet.len);
        break;
    case CMD_OSCILLOSCOPE:
        break;
    default:
        ESP_LOGW(TAG, "Unknown cmd 0x%02x len=%u", (unsigned)s_rx_packet.cmd, (unsigned)s_rx_packet.len);
        break;
    }
}

static void feed_shproto(const uint8_t *d, size_t n) {
    for (size_t i = 0; i < n; i++) {
        shproto_byte_received(&s_rx_packet, d[i]);
        if (s_rx_packet.ready) { s_rx_packet.ready = false; handle_rx_packet(); }
    }
}
static bool handle_rx(const uint8_t *data, size_t data_len, void *arg) {
    if (++s_rx_cb_count <= 10) {
        char h[80]; size_t p=0, show = data_len<16?data_len:16;
        for (size_t i=0;i<show;i++) p+=snprintf(h+p,sizeof(h)-p,"%02x ",data[i]);
        ESP_LOGI(TAG,"RX#%d len=%u: %s",s_rx_cb_count,(unsigned)data_len,h);
    }
    for (size_t off=0; off<data_len; ) {
        size_t ch=data_len-off; if(ch>64)ch=64; if(ch<=2)break;
        if (data[off+1] & 0x8E) s_usb_rx_err++;  // #TCP-4: FTDI line-status byte = OE|PE|FE|FIFO
        if(s_raw_rx_cb) s_raw_rx_cb(data+off+2,ch-2);
        feed_shproto(data+off+2,ch-2);
        off+=ch;
    }
    return true;
}

static void handle_event(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
    switch (event->type) {
    case CDC_ACM_HOST_ERROR:
        ESP_LOGE(TAG, "CDC error");
        break;
    case CDC_ACM_HOST_DEVICE_DISCONNECTED:
        ESP_LOGW(TAG, "Device disconnected");
        if (s_cdc_dev) {
            cdc_acm_host_close(s_cdc_dev);
            s_cdc_dev = NULL;
        }
        break;
    default:
        break;
    }
}

static bool enum_filter_cb(const usb_device_desc_t *dev_desc, uint8_t *bConfigurationValue)
{
    ESP_LOGI(TAG, "USB device enumerated: VID=%04x PID=%04x class=%d subclass=%d",
             dev_desc->idVendor, dev_desc->idProduct,
             dev_desc->bDeviceClass, dev_desc->bDeviceSubClass);
    *bConfigurationValue = 1;
    return true;
}

static void usb_host_lib_task(void *arg)
{
    while (1) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

static void try_open_device(void)
{
    if (s_cdc_dev) return;

    const cdc_acm_host_device_config_t dev_config = {
        .connection_timeout_ms = 5000,
        .out_buffer_size = 1024,
        .in_buffer_size = 1024,  /* #TCP-5 ОТКАТ: 1024*36 → ESP_ERR_NO_MEM на нашей S3 (DMA-RAM < 36КБ, мост мёртв). Вернул проверенный 1024; #TCP-5 переоткрыт — нужен подбор размера/освобождение DMA-RAM */
        .event_cb = handle_event,
        .data_cb = handle_rx,
        .user_arg = NULL,
    };

    static int s_attempt = 0;
    s_attempt++;

    int num_devs = 0;
    uint8_t dev_addrs[8];
    usb_host_device_addr_list_fill(sizeof(dev_addrs)/sizeof(dev_addrs[0]), dev_addrs, &num_devs);
    if (num_devs > 0 && (s_attempt <= 3 || (s_attempt % 15) == 0)) {
        ESP_LOGI(TAG, "USB bus: %d device(s) enumerated (addrs:", num_devs);
        for (int i = 0; i < num_devs; i++) ESP_LOGI(TAG, "  addr=%d", dev_addrs[i]);
    }

    esp_err_t err = cdc_acm_host_open_vendor_specific(ANALYZER_VID, ANALYZER_PID, 0, &dev_config, &s_cdc_dev);
    if (err != ESP_OK) {
        if (s_attempt <= 3 || (s_attempt % 15) == 0) {
            ESP_LOGW(TAG, "open failed: %s (attempt %d, devs_on_bus=%d)", esp_err_to_name(err), s_attempt, num_devs);
        }
        return;
    }

    cdc_acm_host_send_custom_request(s_cdc_dev,FTDI_REQTYPE_OUT,FTDI_SIO_RESET,0,0,0,NULL);
    cdc_acm_host_send_custom_request(s_cdc_dev,FTDI_REQTYPE_OUT,FTDI_SIO_SET_BAUDRATE,5,0,0,NULL);
    cdc_acm_host_send_custom_request(s_cdc_dev,FTDI_REQTYPE_OUT,FTDI_SIO_SET_DATA,0x0008,0,0,NULL);
    cdc_acm_host_send_custom_request(s_cdc_dev,FTDI_REQTYPE_OUT,FTDI_SIO_SET_FLOW_CTRL,0,0,0,NULL);
    cdc_acm_host_send_custom_request(s_cdc_dev,FTDI_REQTYPE_OUT,FTDI_SIO_SET_MODEM_CTRL,0x0303,0,0,NULL);
    cdc_acm_host_send_custom_request(s_cdc_dev,FTDI_REQTYPE_OUT,FTDI_SIO_RESET,1,0,0,NULL);
    cdc_acm_host_send_custom_request(s_cdc_dev,FTDI_REQTYPE_OUT,FTDI_SIO_RESET,2,0,0,NULL);
    ESP_LOGI(TAG, "FTDI configured baud=%u 8N1", ANALYZER_BAUD);
    ESP_LOGI(TAG, "Analyzer connected (VID=%04x PID=%04x)", ANALYZER_VID, ANALYZER_PID);
    s_rx_cb_count = 0;
    vTaskDelay(pdMS_TO_TICKS(100));

    shproto_init(&s_tx_packet, s_tx_buf, sizeof(s_tx_buf));
    shproto_packet_start(&s_tx_packet, CMD_TEXT);
    const char *cmd = "-inf";
    while (*cmd) shproto_packet_add_data(&s_tx_packet, *cmd++);
    shproto_packet_add_data(&s_tx_packet, '\0');
    shproto_packet_complete(&s_tx_packet);
    esp_err_t txerr = cdc_acm_host_data_tx_blocking(s_cdc_dev, s_tx_packet.data, s_tx_packet.len, 1000);
    ESP_LOGI(TAG, "Sent -inf (%u bytes) rc=%s", (unsigned)s_tx_packet.len, esp_err_to_name(txerr));

    // Однократно отметить, что первый коннект после ребута состоялся: автозапуск
    // (#FW-2) и очистка прибора (#FW-3) применяются ровно один раз за boot, не на
    // каждый реконнект.
    bool first_connect = !s_boot_once_done;
    s_boot_once_done = true;

    if (spectrogram_is_recording()) {
        // #REC-6: если на момент (ре)коннекта водопад пишется — возобновить набор
        // спектра на приборе (-sta). Защищает от случайной остановки анализатора и
        // от ситуации «ESP ребутнулся, восстановил запись, но прибор уже не набирает».
        // Запись уже идёт → автозапуск #FW-2 не нужен (намеренно пропускаем).
        vTaskDelay(pdMS_TO_TICKS(100));
        usb_host_send_text_command("-sta");
        ESP_LOGW(TAG, "recording active — resent -sta to resume acquisition");
    } else if (first_connect) {
        // #FW-3: очистка спектра при старте — сбросить гистограмму прибора тем же
        // путём, что кнопка «Сброс» (-rst). Локальный spectrum_reset() уже сделан в
        // main.c на boot; -rst синхронизирует прибор, чтобы первый пакет не «поднял»
        // обнулённый спектр обратно.
        if (s_boot_clear_spectrum) {
            vTaskDelay(pdMS_TO_TICKS(100));
            usb_host_send_text_command("-rst");
            ESP_LOGW(TAG, "FW-3: boot clear spectrum — sent -rst to device");
        }
        // #FW-2: автозапуск при старте платы (по настройкам NVS, по умолчанию OFF).
        if (s_boot_autostart_wf) {
            vTaskDelay(pdMS_TO_TICKS(100));
            spectrogram_start();              // начать запись водопада (сегменты)
            usb_host_send_text_command("-sta");
            ESP_LOGW(TAG, "FW-2: boot autostart — waterfall recording started");
        } else if (s_boot_autostart_spec) {
            vTaskDelay(pdMS_TO_TICKS(100));
            usb_host_send_text_command("-sta");
            ESP_LOGW(TAG, "FW-2: boot autostart — spectrum acquisition started");
        }
    }
}

void usb_host_cdc_set_autostart(bool autostart_spectrum, bool autostart_waterfall, bool clear_spectrum)
{
    s_boot_autostart_spec = autostart_spectrum;
    s_boot_autostart_wf   = autostart_waterfall;
    s_boot_clear_spectrum = clear_spectrum;
}

static void usb_connect_task(void *arg)
{
    while (1) {
        try_open_device();
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void usb_host_cdc_init(void)
{
    shproto_init(&s_rx_packet, s_rx_buf, sizeof(s_rx_buf));
    shproto_init(&s_tx_packet, s_tx_buf, sizeof(s_tx_buf));
    s_tx_mutex = xSemaphoreCreateMutex();
    s_devlog_mutex = xSemaphoreCreateMutex();  // #UI-1

    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
        .enum_filter_cb = enum_filter_cb,
    };
    esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "USB Host library installed");

    // #FW-8: usb_lib и CDC driver ВЫШЕ httpd (prio 5, tskNO_AFFINITY). Иначе при
    // скачивании сегмента httpd-таск секундами крутит flash-read+send на core 0 и
    // голодом душит USB-приём: FTDI FIFO переполняется, chunk-и свипа теряются
    // все ~35-40 c окна выгрузки → 7 строк dur=0 подряд (наблюдение 2026-07-02).
    // WiFi/LWIP (prio 18+) всё равно выше — их не задеваем.
    xTaskCreatePinnedToCore(usb_host_lib_task, "usb_lib", 4096, NULL, 7, NULL, 0);

    const cdc_acm_host_driver_config_t driver_config = {
        // 8192: парсинг info-response (spectrum_process_info_response) идёт в
        // контексте этого таска через data_cb→handle_rx_packet и раньше клал
        // ~3.5 КБ локальных буферов на стек (P1-1). Буфер вынесен в static,
        // плюс расширенный стек убирает остаточный риск переполнения.
        .driver_task_stack_size = 8192,
        .driver_task_priority = 8,   // #FW-8: выше httpd(5) и usb_lib(7)
        .xCoreID = 0,
    };
    err = cdc_acm_host_install(&driver_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cdc_acm_host_install failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "CDC-ACM driver installed");

    xTaskCreatePinnedToCore(usb_connect_task, "usb_conn", 4096, NULL, 2, NULL, 0);
    ESP_LOGI(TAG, "USB Host CDC initialized, waiting for analyzer...");
}

bool usb_host_cdc_is_connected(void)
{
    return s_cdc_dev != NULL;
}

int usb_host_cdc_send(const uint8_t *data, size_t len)
{
    if (!s_cdc_dev) return -1;
    if (xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return -1;
    esp_err_t err = cdc_acm_host_data_tx_blocking(s_cdc_dev, data, len, 1000);
    xSemaphoreGive(s_tx_mutex);
    return (err == ESP_OK) ? 0 : -1;
}

void usb_host_cdc_set_raw_rx_cb(usb_raw_rx_cb_t cb)
{
    s_raw_rx_cb = cb;
}

uint32_t usb_host_cdc_rx_errors(void)
{
    return s_usb_rx_err;
}

// #UI-1: JSON-массив текстовых ответов прибора с seq>since:
//   {"lines":[{"seq":N,"text":"..."},...],"next":M}
// Вызывается из httpd-таска (handle_devlog). Текст эскейпится для JSON: " \ → \" \\,
// \n → \\n, \r/\t и непечатные/не-ASCII символы отбрасываются. out должен быть ≥ ~9 КБ.
void usb_host_cdc_devlog_json(uint32_t since, char *out, size_t outsz)
{
    if (!out || outsz < 32) { if (out && outsz) out[0] = '\0'; return; }
    uint32_t next, start;
    if (s_devlog_mutex) xSemaphoreTake(s_devlog_mutex, portMAX_DELAY);
    next = s_devlog_next - 1;                 // последний присвоенный seq (0 если пусто)
    if (s_devlog_mutex) xSemaphoreGive(s_devlog_mutex);

    size_t pos = 0;
    pos += snprintf(out + pos, outsz - pos, "{\"lines\":[");
    // окно: только то, что ещё в кольце (последние DEVLOG_RING записей)
    start = since + 1;
    if (next >= DEVLOG_RING && start < next - DEVLOG_RING + 1) start = next - DEVLOG_RING + 1;

    char tmp[DEVLOG_TEXTSZ];
    int emitted = 0;
    for (uint32_t s = start; s <= next && next != 0; s++) {
        int tlen = -1;
        if (s_devlog_mutex) xSemaphoreTake(s_devlog_mutex, portMAX_DELAY);
        int slot = s % DEVLOG_RING;
        if (s_devlog_seq[slot] == s) {        // ещё не вытеснен
            tlen = s_devlog_len[slot];
            if (tlen > DEVLOG_TEXTSZ - 1) tlen = DEVLOG_TEXTSZ - 1;
            memcpy(tmp, s_devlog_text[slot], tlen);
        }
        if (s_devlog_mutex) xSemaphoreGive(s_devlog_mutex);
        if (tlen < 0) continue;

        // запас под объект; если мало места — прекращаем (next всё равно отдадим)
        if (pos + (size_t)tlen * 2 + 48 >= outsz) break;
        pos += snprintf(out + pos, outsz - pos, "%s{\"seq\":%" PRIu32 ",\"text\":\"",
                        emitted ? "," : "", s);
        for (int i = 0; i < tlen && pos + 8 < outsz; i++) {
            unsigned char c = (unsigned char)tmp[i];
            if (c == '"' || c == '\\')      { out[pos++] = '\\'; out[pos++] = c; }
            else if (c == '\n')             { out[pos++] = '\\'; out[pos++] = 'n'; }
            else if (c >= 0x20 && c < 0x7f) { out[pos++] = c; }
            // \r, \t, прочие управляющие/не-ASCII — пропускаем
        }
        pos += snprintf(out + pos, outsz - pos, "\"}");
        emitted++;
    }
    snprintf(out + pos, outsz - pos, "],\"next\":%" PRIu32 "}", next);
}

int usb_host_send_text_command(const char *cmd)
{
    uint8_t pkt_buf[256];
    shproto_struct pkt;
    shproto_init(&pkt, pkt_buf, sizeof(pkt_buf));
    shproto_packet_start(&pkt, CMD_TEXT);
    while (*cmd) shproto_packet_add_data(&pkt, *cmd++);
    shproto_packet_add_data(&pkt, '\0');
    shproto_packet_complete(&pkt);
    return usb_host_cdc_send(pkt.data, pkt.len);
}
