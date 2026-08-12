#include "atomspectra.h"
#include "spectrogram.h"
#include "boot_config.h"
#include "wf_offload.h"   // #REC-11-A2: автономная выгрузка сегментов водопада
#include "monitor.h"      // #MON-1: серия CPS-мониторинга на плате
#include "net_time.h"     // #FIELD-5: источник времени (SNTP/браузер/ручной)
#include "http_io_gate.h" // #PERF-2: skip autosave while HEAVY I/O busy
#include "debug_log_ring.h"
#include "hist_drop_diag.h"
#include "flash_quiet.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include <inttypes.h>
#include <sys/time.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"   // #FW-13 фикс №2: ожидание коммита свипа перед autosave

static const char *TAG = "main";

// #FW-23: usb_host_cdc_init() (boot-автозапуск водопада) стартует раньше SNTP —
// started_at мог зафиксироваться near-epoch. На первый успешный sync — пересчитать.
static void time_sync_cb(struct timeval *tv)
{
    (void)tv;
    net_time_mark_sntp();      // #FIELD-5 (A2): зафиксировать факт реальной SNTP-синхронизации
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

    // #FW-2/#FW-3: настройки поведения при старте платы (NVS, по умолчанию всё OFF).
    // #FW-44: весь USB-стек (boot_config → spectrum → spectrogram → usb_host_cdc)
    // поднят ДО AP-проверки. Раньше свежая плата (wifi не настроен) уходила в
    // немой while(1){vTaskDelay} ещё до usb_host_cdc_init() → USB Host не стартовал,
    // консоль давала лишь 3 boot-строки и молчала навсегда. Теперь USB работает
    // независимо от WiFi — спектрометр определяется и диагностируется в AP-режиме.
    boot_config_t bc;
    boot_config_load(&bc);
    ESP_LOGI(TAG, "boot-config: as_spec=%d as_wf=%d clr_spec=%d clr_wf=%d",
             bc.autostart_spectrum, bc.autostart_waterfall, bc.clear_spectrum, bc.clear_waterfall);

    flash_quiet_init();
    spectrum_init();
    spectrum_restore_autosave();
    spectrum_load_calibration();
    // #FW-3: очистка накопленного спектра при старте — после restore, до того как
    // спектрограмма снимет baseline. -rst прибору пошлётся на первом USB-коннекте.
    if (bc.clear_spectrum) {
        spectrum_reset();
        ESP_LOGW(TAG, "FW-3: accumulated spectrum cleared on boot");
    }
    // #FW-50: PSRAM log ring — after spectrum_init, before spectrogram (reserve before WF).
    debug_log_ring_boot();
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

    // #FW-44/#FIELD-1: setup-портал (свежая плата без wifi-конфига) — диагностический
    // цикл, сеть-зависимые подсистемы не поднимаем (setup-httpd держится внутри
    // wifi_manager). USB Host уже поднят выше → логируем реальную диагностику вместо
    // прежней немоты. Выход — только перезагрузка после сохранения wifi/выбора режима.
    net_run_mode_t net_mode = wifi_manager_mode();
    if (net_mode == NET_MODE_SETUP) {
        ESP_LOGI(TAG, "Setup captive portal active, waiting for WiFi config");
        while (1) {
            const spectrum_data_t *sp = spectrum_get_current();
            ESP_LOGI(TAG, "SETUP: connect SSID=AtomSpectra-Setup -> http://192.168.4.1 | USB:%s spectrometer:%s counts:%" PRIu32,
                usb_host_cdc_is_connected() ? "OK" : "--",
                usb_host_cdc_spectrometer_dead() ? "DEAD"
                    : (usb_host_cdc_is_connected() ? "streaming" : "--"),
                sp->total_counts);
            vTaskDelay(pdMS_TO_TICKS(10000));
        }
    }

    // #FIELD-1: рабочий сетевой стек — общий для Indoor (STA) и Outdoor (полевой AP).
    web_server_init();
    monitor_init();      // #MON-1: кольцо серии CPS (6 ч в PSRAM) + задача-подписчик коммитов
    tcp_bridge_init();
    if (net_mode == NET_MODE_STA) {
        wf_offload_init();   // #REC-11-A2: аплоадер сегментов — только Indoor (есть сеть/приёмник)
        init_sntp();          // SNTP-время — только Indoor (есть интернет)
    } else {
        // #FIELD-1: Outdoor (полевой AP) — интернета/приёмника нет: SNTP и offload
        // не поднимаем. Время платы приходит от браузера телефона (#FIELD-5,
        // POST /api/time); данные водопада копятся на flash, забираются дома.
        ESP_LOGI(TAG, "FIELD-1: Outdoor AP — SNTP & offload disabled (time via browser)");
    }

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
        if (++autosave_tick >= HIST_DROP_E3_AUTOSAVE_TICKS) {
            autosave_tick = 0;
            // #PERF-2: не писать LittleFS, пока HEAVY download/export держит слот
            if (!http_io_gate_busy() && http_io_gate_try_enter()) {
#if HIST_DROP_I3_SLICED
                /* #FW-8 F1a: sliced write in post-commit quiet windows.
                 * Offline / silent USB → one-shot full write (no 1 Hz burst).
                 * After fail_streak≥5 → force one-shot (one hist-drop) instead of
                 * unbounded deferral. */
                if (!usb_host_cdc_is_connected() || spectrum_autosave_fail_streak() >= 5) {
                    if (spectrum_autosave_in_progress())
                        spectrum_autosave_abort();
                    hist_drop_diag_autosave_begin(true);
                    int64_t t0 = esp_timer_get_time();
                    spectrum_autosave();
                    int64_t dt_us = esp_timer_get_time() - t0;
                    hist_drop_diag_autosave_end();
                    ESP_LOGI(TAG, "LittleFS autosave took %lld us (%s)",
                             (long long)dt_us,
                             usb_host_cdc_is_connected() ? "force one-shot" : "offline one-shot");
                } else {
                    /* Begin must land in quiet (fopen ~100–160 ms). Retry a few
                     * commits if headroom was already gone. Resume after yield. */
                    int pumps = 0;
                    int wait_fail = 0;
                    int begin_tries = 0;
                    while (begin_tries < 4) {
                        begin_tries++;
                        if (autosave_sig && !spectrum_autosave_in_progress()) {
                            if (xSemaphoreTake(autosave_sig, pdMS_TO_TICKS(1500)) != pdTRUE) {
                                if (++wait_fail >= 3) break;
                                continue;
                            }
                        }
                        wait_fail = 0;
                        if (spectrum_autosave_begin()) break;
                        if (spectrum_autosave_in_progress()) break; /* yielded, snap held */
                    }
                    if (!spectrum_autosave_in_progress()) {
                        spectrum_autosave_note_fail();
                        ESP_LOGI(TAG, "LittleFS autosave begin deferred (no quiet headroom)");
                    }
                    while (spectrum_autosave_in_progress()) {
                        bool wait_timed_out = false;
#if HIST_DROP_DIAG && HIST_DROP_E4_NO_WAIT_COMMIT
                        wait_timed_out = true;
#else
                        if (autosave_sig) {
                            /* Do NOT Take(0)-clear here: a Give may have arrived
                             * during the previous pump's flash work. Clearing it
                             * forces a 1.5 s wait that times out if sweeps were
                             * dropped by that same freeze → abort mid-file. */
                            wait_timed_out =
                                (xSemaphoreTake(autosave_sig, pdMS_TO_TICKS(1500)) != pdTRUE);
                        }
#endif
                        if (wait_timed_out) {
                            if (++wait_fail >= 3) {
                                /* Soft yield: keep tmp+offset for next cycle. */
                                spectrum_autosave_yield();
                                spectrum_autosave_note_fail();
                                ESP_LOGI(TAG,
                                         "LittleFS autosave yielded (commit wait_timeout x3, USB live)");
                                break;
                            }
                            ESP_LOGW(TAG, "autosave wait_timeout (retry %d)", wait_fail);
                            continue;
                        }
                        wait_fail = 0;
                        if (!spectrum_autosave_begin()) {
                            /* Could not reopen after yield — try next tick. */
                            spectrum_autosave_note_fail();
                            break;
                        }
                        hist_drop_diag_autosave_begin(false);
                        spectrum_autosave_pump();
                        hist_drop_diag_autosave_end();
                        pumps++;
                    }
                    if (pumps > 0 && !spectrum_autosave_in_progress())
                        ESP_LOGI(TAG, "LittleFS autosave sliced pumps=%d", pumps);
                }
#else
                bool wait_timed_out = false;
#if HIST_DROP_DIAG && HIST_DROP_E4_NO_WAIT_COMMIT
                wait_timed_out = true;
#else
                if (autosave_sig) {
                    xSemaphoreTake(autosave_sig, 0);
                    wait_timed_out = (xSemaphoreTake(autosave_sig, pdMS_TO_TICKS(1500)) != pdTRUE);
                }
#endif
                if (wait_timed_out && usb_host_cdc_is_connected()) {
                    spectrum_autosave_note_fail();
                    ESP_LOGI(TAG, "LittleFS autosave skipped (commit wait_timeout, USB live)");
                } else {
                    hist_drop_diag_autosave_begin(wait_timed_out);
                    int64_t t0 = esp_timer_get_time();
                    spectrum_autosave();
                    int64_t dt_us = esp_timer_get_time() - t0;
                    hist_drop_diag_autosave_end();
                    ESP_LOGI(TAG, "LittleFS autosave took %lld us%s",
                             (long long)dt_us,
                             wait_timed_out ? " (wait_timeout)" : "");
                }
#endif
                http_io_gate_leave();
            }
        }
        // #WF-1: отложенная запись калибровки (s_calib_dirty). Внутри сама берёт
        // SPEC_LOCK только на снапшот; flash-запись — вне лока и вне CDC/httpd.
        spectrum_save_calibration();
    }
}