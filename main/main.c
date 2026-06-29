#include "atomspectra.h"
#include "spectrogram.h"
#include "boot_config.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

static void init_sntp(void)
{
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
}

void app_main(void)
{
    ESP_LOGI(TAG, "AtomSpectra Gateway starting...");

    wifi_manager_init();

    if (wifi_manager_is_ap_mode()) {
        ESP_LOGI(TAG, "Captive portal active, waiting for WiFi config");
        while (1) { vTaskDelay(pdMS_TO_TICKS(10000)); }
    }

    // #FW-2/#FW-3: настройки поведения при старте платы (NVS, по умолчанию всё OFF).
    boot_config_t bc;
    boot_config_load(&bc);
    ESP_LOGI(TAG, "boot-config: as_spec=%d as_wf=%d clr_spec=%d clr_wf=%d",
             bc.autostart_spectrum, bc.autostart_waterfall, bc.clear_spectrum, bc.clear_waterfall);

    spectrum_init();
    spectrum_restore_autosave();
    spectrum_load_calibration();
    // #FW-3: очистка накопленного спектра при старте — после restore, до того как
    // спектрограмма снимет baseline. -rst прибору пошлётся на первом USB-коннекте.
    if (bc.clear_spectrum) {
        spectrum_reset();
        ESP_LOGW(TAG, "FW-3: accumulated spectrum cleared on boot");
    }
    spectrogram_init();
    // #FW-3: очистка водопада при старте — ДО spectrogram_restore(), иначе restore
    // возобновит прежнюю запись из сохранённого состояния.
    if (bc.clear_waterfall) {
        spectrogram_clear();
        ESP_LOGW(TAG, "FW-3: waterfall cleared on boot");
    }
    spectrogram_restore();   // #REC-6: возобновить запись после ребута/сбоя питания
    // #FW-2: передать флаги автозапуска в USB-модуль ДО его инициализации.
    usb_host_cdc_set_autostart(bc.autostart_spectrum, bc.autostart_waterfall, bc.clear_spectrum);
    usb_host_cdc_init();
    web_server_init();
    tcp_bridge_init();
    init_sntp();

    ESP_LOGI(TAG, "All subsystems initialized");

    int info_tick = 0, autosave_tick = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        const spectrum_data_t *sp = spectrum_get_current();
        ESP_LOGI(TAG, "USB:%s WiFi:%s TCP:%s counts:%" PRIu32 " cpu:%u%%",
            usb_host_cdc_is_connected() ? "OK" : "--",
            wifi_is_connected() ? "OK" : "--",
            tcp_bridge_client_connected() ? "OK" : "--",
            sp->total_counts, (unsigned)sp->cpu_load);

        if (usb_host_cdc_is_connected() && ++info_tick >= 3) {
            info_tick = 0;
            usb_host_send_text_command("-inf");
        }
        if (++autosave_tick >= 6) {
            autosave_tick = 0;
            spectrum_autosave();
        }
    }
}