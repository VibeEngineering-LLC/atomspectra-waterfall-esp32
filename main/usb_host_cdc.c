#include "atomspectra.h"
#include "shproto.h"
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

static char s_text_accum[4096];
static int  s_text_accum_len = 0;
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
            int sp = sizeof(s_text_accum)-s_text_accum_len-1;
            int cp = (int)s_rx_packet.len < sp ? (int)s_rx_packet.len : sp;
            memcpy(s_text_accum+s_text_accum_len, s_rx_packet.data, cp);
            s_text_accum_len += cp; s_text_accum[s_text_accum_len] = '\0';
            // #PR-1: ждём ПОЗДНИЙ маркер "PileUpThr " — он приходит в конце -inf ответа,
            // когда весь блок (калибровка L0..L10, серийник L39) уже накоплен из нескольких
            // CDC-пакетов. Раньше триггер по "DEV "+"VERSION " срабатывал слишком рано
            // (эти токены ранние) → spectrum_process_info_response видел неполный текст и
            // калибровка не разбиралась. "VERSION " оставлен как sanity-guard.
            if (strstr(s_text_accum,"PileUpThr ") && strstr(s_text_accum,"VERSION ")) {
                spectrum_process_info_response(s_text_accum);
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
        .in_buffer_size = 1024,
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

    xTaskCreatePinnedToCore(usb_host_lib_task, "usb_lib", 4096, NULL, 2, NULL, 0);

    const cdc_acm_host_driver_config_t driver_config = {
        // 8192: парсинг info-response (spectrum_process_info_response) идёт в
        // контексте этого таска через data_cb→handle_rx_packet и раньше клал
        // ~3.5 КБ локальных буферов на стек (P1-1). Буфер вынесен в static,
        // плюс расширенный стек убирает остаточный риск переполнения.
        .driver_task_stack_size = 8192,
        .driver_task_priority = 3,
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
