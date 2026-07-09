#include "atomspectra.h"
#include "spectrogram.h"
#include "boot_config.h"
#include "wf_offload.h"   // #REC-11-A2: автономная выгрузка сегментов водопада
#include "monitor.h"      // #MON-1: серия CPS-мониторинга на плате
#include "esp_log.h"
#include "esp_sntp.h"
#include <inttypes.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"   // #FW-13 фикс №2: ожидание коммита свипа перед autosave

static const char *TAG = "main";

// #FW-23: usb_host_cdc_init() (boot-автозапуск водопада) стартует раньше SNTP —
// started_at мог зафиксироваться near-epoch. На первый успешный sync — пересчитать.
static void time_sync_cb(struct timeval *tv)
{
    (void)tv;
    spectrogram_time_synced();
}

static void init_sntp(void)
{
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(time_sync_cb);
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
    wf_offload_init();   // #REC-11-A2: поднять задачу-аплоадер (конфиг из NVS, по умолчанию выкл.)
    monitor_init();      // #MON-1: кольцо серии CPS (6 ч в PSRAM) + задача-подписчик коммитов
    tcp_bridge_init();
    init_sntp();

    ESP_LOGI(TAG, "All subsystems initialized");

    // #FW-13 фикс №2: autosave (32.8 КБ LittleFS = freeze кэша обоих ядер) фазово
    // привязывается к коммиту свипа — запись уходит в тихое USB-окно (~0.5 с) сразу
    // после конца burst, а не в случайную фазу, где рвала приём FTDI.
    SemaphoreHandle_t autosave_sig = xSemaphoreCreateBinary();
    if (autosave_sig) spectrum_add_commit_listener(autosave_sig);

    int info_tick = 0, autosave_tick = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        const spectrum_data_t *sp = spectrum_get_current();
        ESP_LOGI(TAG, "USB:%s WiFi:%s TCP:%s counts:%" PRIu32 " cpu:%u%%",
            usb_host_cdc_is_connected() ? "OK" : "--",
            wifi_is_connected() ? "OK" : "--",
            tcp_bridge_client_connected() ? "OK" : "--",
            sp->total_counts, (unsigned)sp->cpu_load);

        // #FW-13: период -inf 30 с → 30 мин (оператор 2026-07-03). Единственное
        // динамичное поле ответа — температура; каждый -inf вклинивает 404-байтный
        // текстовый ответ в поток свипов, чаще незачем.
        if (usb_host_cdc_is_connected() && ++info_tick >= 180) {
            info_tick = 0;
            usb_host_send_text_command("-inf");
        }
        if (++autosave_tick >= 6) {
            autosave_tick = 0;
            if (autosave_sig) {
                xSemaphoreTake(autosave_sig, 0);                    // сброс протухшего сигнала
                xSemaphoreTake(autosave_sig, pdMS_TO_TICKS(1500));  // ждём свежий коммит
            }
            spectrum_autosave();
        }
        // #WF-1: отложенная запись калибровки (s_calib_dirty). Внутри сама берёт
        // SPEC_LOCK только на снапшот; flash-запись — вне лока и вне CDC/httpd.
        spectrum_save_calibration();
    }
}